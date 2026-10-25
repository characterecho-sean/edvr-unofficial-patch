#!/usr/bin/env python3
"""Masked whole-function signatures for the game addresses EDVR binds (issue 124).

Every hook binds fixed RVAs of one game build and stands down on any other.
The plan in issue 124: on an unknown build, find each address at load by
matching its whole function against the known build, with the bytes that a
rebuild moves left out of the comparison.  This tool is step 1: the offline
generator, a copy of the runtime matcher in Python, and a check that measures
whether the matcher would have found every address on builds we hold.

    python tools/hook_signatures.py generate --exe analysis/EliteDangerous64-334925.exe ^
        --rvas .agents/build-334925/map_all.json --out build/hook_signatures.json [--dry-run]
    python tools/hook_signatures.py resolve --table build/hook_signatures.json --exe <exe>
    python tools/hook_signatures.py check --table build/hook_signatures.json --exe <exe> ^
        --expect <build_diff.py map --json output> [--invert] [--src src]
    python tools/hook_signatures.py survey --exe <known> --other <exe> --cache <map cache>

The unit.  The same code units tools/build_diff.py uses: each .pdata
RUNTIME_FUNCTION range (chained fragments and funclets included), plus the
leaf stubs in the gaps between them, split on int3 padding.  A code target is
(unit, offset within the unit).

The mask.  The unit is decoded with capstone and these byte ranges are left
out of the comparison: the rel32 of every relative call, jmp and jcc; every
RIP-relative disp32; every byte under a base relocation; the RVA disp32 of a
jump-table access and the table's entries (see decode_unit).  Everything else
is exact: struct displacements off other registers, immediates, rel8
branches.  A change there means the function changed.  The signature is the
FNV-1a-64 of the unit with the masked bytes zeroed, plus the unit's length.

Resolution.  The matcher reads the .pdata units of the unknown exe, takes the
ones of the same length, hashes each with the target's mask, and accepts the
target only when exactly one unit matches ("hash").  Other targets need a
second witness:

  * a function whose masked bytes are shared by clones (tiny getters and
    thunks): the rel32 or RIP-relative operand of a caller that is itself a
    unique unit names it, and the named unit must be one of the clones ("ref");
    so does the unit at a fixed distance, in address order, from a unique
    neighbour ("nbr"), but neighbours alone are refused (layout-only) unless
    --allow-layout, because a family of identical thunks that gained or lost
    a member is mis-named by position;
  * a data address, by any of these, all of which must agree when several
    apply: a RIP-relative operand in a unique unit names it, as unit start +
    next-instruction offset + the disp32 read from the new bytes ("ref"); it
    is a function-pointer cell, found as the one cell of the image that holds
    the (uniquely matched) function's address ("ptr"); it is a string, found
    as the one NUL-delimited string with that length and hash ("content").
    A string or a pointer is also checked against its shape before the
    address is accepted.

A reference or a search that finds no single answer leaves the target
unresolved; a disagreement between the answers does too.  The table holds
lengths, offsets, mask ranges, hashes and string hashes.  It never holds code
bytes or strings, because it is meant for a public repository and the game's
code must not be redistributed (the self-test asserts it).

generate needs capstone (the one place that decodes); resolve and check use
the standard library only, as the runtime matcher in C++ will.
"""
import argparse
import bisect
import contextlib
import io
import json
import random
import re
import struct
import sys
import tempfile
import time
from collections import defaultdict
from pathlib import Path

try:
    from openxr_pe import Image, PEError
    import build_diff as bd
except ImportError:  # Allows a repository-root import in unit tests.
    from tools.openxr_pe import Image, PEError
    from tools import build_diff as bd

FORMAT = 1
HASH_NAME = "fnv1a64-masked"
MAX_REFS = 3              # reference units recorded per target (cross-checked)
PREFERRED_REF_LEN = 24    # a reference unit shorter than this is a last resort
MAX_REF_CANDIDATES = 60   # reference units examined per target
NBR_SPAN = 6              # units searched either side for a unique neighbour
TAILS = (0, 1, 2, 4)      # immediate bytes that can follow a RIP-relative disp32
MAPPED = ("IDENTICAL", "RELOC-ONLY", "XREF-EXACT", "XREF-BRACKET", "CONTENT-SEARCH")
SRC_SUFFIXES = {".cpp", ".h", ".hpp", ".c", ".cc", ".inl", ".inc", ".py", ".hlsl"}


# --------------------------------------------------------------------------
# Shared by the generator and the matcher.
# --------------------------------------------------------------------------

def masked_bytes(body, flat):
    """body with the flat [off, len, off, len, ...] ranges zeroed."""
    if not flat:
        return bytes(body)
    out = bytearray(body)
    for k in range(0, len(flat), 2):
        off, ln = flat[k], flat[k + 1]
        out[off:off + ln] = bytes(len(out[off:off + ln]))
    return bytes(out)


def signature_hash(body, flat):
    return bd.fnv1a64(masked_bytes(body, flat))


def merge_ranges(pairs):
    """Sorted, overlap-merged [off, len] pairs flattened to one list."""
    flat = []
    for off, ln in sorted(pairs):
        if flat and off <= flat[-2] + flat[-1]:
            flat[-1] = max(flat[-1], off + ln - flat[-2])
        else:
            flat += [off, ln]
    return flat


def exec_sections(im):
    return [(rva, min(span, raw_size), raw)
            for rva, span, raw, raw_size, flags in im.sections
            if flags & bd.SCN_EXEC]


def section_name_at(im, rva):
    """Section name holding rva, or None."""
    for row in bd.section_table(im):
        if row["rva"] <= rva < row["rva"] + row["span"]:
            return row["name"]
    return None


def image_base(im):
    nt = int.from_bytes(im.data[0x3C:0x40], "little")
    return int.from_bytes(im.data[nt + 24 + 24:nt + 24 + 32], "little")


def read_relocs(im):
    """Sorted (rva, size) of every typed base relocation: the pointer-sized
    fields the loader rewrites when the image does not load at its base."""
    rva, size = im.directory(5)
    if not rva:
        return []
    off, _ = im.mapping(rva, size)
    end, out = off + size, []
    while off + 8 <= end:
        page, block = struct.unpack_from("<II", im.data, off)
        if block < 8:
            break
        for k in range((block - 8) // 2):
            entry = struct.unpack_from("<H", im.data, off + 8 + 2 * k)[0]
            width = {3: 4, 10: 8, 1: 2, 2: 2}.get(entry >> 12)
            if width:
                out.append((page + (entry & 0xFFF), width))
        off += block
    return sorted(out)


# --------------------------------------------------------------------------
# Content guards for data targets.
# --------------------------------------------------------------------------

_PRINTABLE = re.compile(rb"[\x20-\x7e]{4,63}\x00")
_PRINTABLE_RUN = re.compile(rb"[\x20-\x7e]+")


def data_guard(im, U, rva):
    """What a data target looks like, checked again at resolve time.

    A NUL-terminated printable string is guarded by its length and hash; an
    8-byte absolute pointer into code by its shape; anything else has none."""
    head = bd._read_rva(im, rva, 64)
    if head is None:
        return None
    m = _PRINTABLE.match(head)
    if m:
        text = m.group(0)[:-1]
        return {"kind": "str", "len": len(text), "hash": "%016X" % bd.fnv1a64(text)}
    ptr = struct.unpack_from("<Q", head, 0)[0] - image_base(im)
    if 0 < ptr < 0x100000000 and U.at(ptr) >= 0:
        return {"kind": "code-ptr"}
    return None


def guard_ok(guard, im, U, new):
    """True when the bytes at new look like the guard says."""
    if not guard:
        return True
    if guard["kind"] == "str":
        head = bd._read_rva(im, new, guard["len"] + 1)
        return bool(head) and head[-1] == 0 and len(head) == guard["len"] + 1 \
            and "%016X" % bd.fnv1a64(head[:-1]) == guard["hash"]
    head = bd._read_rva(im, new, 8)
    return bool(head) and U.at(struct.unpack("<Q", head)[0] - image_base(im)) >= 0


def data_sections(im):
    """(rva, bytes) of every non-executable section's raw data."""
    return [(rva, im.data[raw:raw + min(span, raw_size)])
            for rva, span, raw, raw_size, flags in im.sections
            if not flags & bd.SCN_EXEC]


def pointer_cells(im, value):
    """RVAs of the 8-byte aligned cells in the data sections that hold the
    absolute address `value` (a function-pointer table or a vtable slot)."""
    pattern, hits = struct.pack("<Q", value), []
    for rva, buf in data_sections(im):
        at = buf.find(pattern)
        while at >= 0:
            if (rva + at) % 8 == 0:
                hits.append(rva + at)
            at = buf.find(pattern, at + 1)
    return hits


def string_hits(im, guard):
    """Every start of a NUL-delimited printable string of the guard's length
    and hash in the data sections."""
    hits, length = [], guard["len"]
    for rva, buf in data_sections(im):
        pos = 0
        for token in buf.split(b"\0"):
            if len(token) == length and _PRINTABLE_RUN.fullmatch(token) and \
                    "%016X" % bd.fnv1a64(token) == guard["hash"]:
                hits.append(rva + pos)
            pos += len(token) + 1
    return hits


# --------------------------------------------------------------------------
# Decoding (generate only).
# --------------------------------------------------------------------------

def _capstone():
    try:
        import capstone
        from capstone import x86
    except ImportError as exc:
        raise ValueError("generate needs capstone (pip install -r tools/requirements.txt)") from exc
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    return md, capstone, x86


def decode_unit(md, cs, x86, body, start, data_spans=(), code_spans=()):
    """(mask pairs, refs, undecoded bytes, suspect fields) of one unit.

    refs are (field offset, next-instruction offset, absolute target RVA) of
    every rel32 branch and RIP-relative operand.  Bytes capstone cannot decode
    are skipped one at a time and counted; they stay exact in the comparison.
    A RIP-relative operand always has a disp32 (capstone 5.0.7 reports
    disp_size 2 for a 66-prefixed SSE instruction, so the size is not read from
    it); each field is checked against the value capstone decoded for it, and
    one that disagrees is counted as suspect.

    A jump table is reached as [base + index*scale + disp32] with the image
    base in a register, so that disp32 is an RVA and moves with the table.
    The compiler leaves such tables inside the function, and their 4-byte
    entries are RVAs of the case labels, which move too.  A disp32 on an
    indexed operand is masked when its base register holds the image base
    (loaded by a RIP-relative lea of RVA 0 in this unit) or when it lands
    inside this unit or in a data section; the entries of an inline 4-byte
    table are masked with it.  A table the instruction of another fragment
    reaches ends its own fragment: a run of two or more 4-byte values that all
    look like code RVAs near the unit, up to the unit's end, is masked too.
    """
    pairs, refs, bad, suspect, pos, n = [], [], 0, 0, 0, len(body)
    end, base_regs = start + n, set()
    in_code = lambda v: any(lo <= v < hi for lo, hi in code_spans)
    while pos < n:
        last = pos
        for ins in md.disasm(body[pos:], start + pos):
            off = ins.address - start
            last = off + ins.size
            if ins.group(cs.CS_GRP_BRANCH_RELATIVE):
                if ins.imm_size == 4:
                    at = ins.imm_offset + off
                    rel = struct.unpack_from("<i", body, at)[0]
                    suspect += ins.operands[0].imm != start + last + rel
                    pairs.append((at, 4))
                    refs.append((at, last, start + last + rel))
                continue
            for op in ins.operands:
                if op.type != cs.CS_OP_MEM:
                    continue
                if op.mem.base == x86.X86_REG_RIP:
                    at = ins.disp_offset + off
                    disp = struct.unpack_from("<i", body, at)[0]
                    suspect += disp != op.mem.disp or at + 4 > last
                    pairs.append((at, 4))
                    refs.append((at, last, start + last + disp))
                    if ins.id == x86.X86_INS_LEA and start + last + disp == 0:
                        base_regs.add(ins.operands[0].reg)  # __ImageBase
                elif op.mem.index and ins.disp_size == 4:
                    table = op.mem.disp
                    inline = start <= table < end
                    if op.mem.base in base_regs or inline or \
                            any(lo <= table < hi for lo, hi in data_spans):
                        at = ins.disp_offset + off
                        suspect += at + 4 > last or \
                            struct.unpack_from("<i", body, at)[0] != table
                        pairs.append((at, 4))
                        if inline and op.mem.scale == 4:
                            first, count = table - start, 0
                            while first + 4 * count + 4 <= n and in_code(
                                    struct.unpack_from("<I", body, first + 4 * count)[0]):
                                count += 1
                            if count:
                                pairs.append((first, 4 * count))
                break
        if last == pos:
            bad += 1
            pos += 1
        else:
            pos = last
    tail = n
    while tail >= 4 and in_code(struct.unpack_from("<I", body, tail - 4)[0]) and \
            abs(struct.unpack_from("<I", body, tail - 4)[0] - start) < 0x100000:
        tail -= 4
    if n - tail >= 8:
        pairs.append((tail, n - tail))
    return pairs, refs, bad, suspect


# --------------------------------------------------------------------------
# Target lists.
# --------------------------------------------------------------------------

def _int(value):
    return value if isinstance(value, int) else int(str(value), 0)


def read_targets(path, rva_field="new_rva"):
    """[(rva, kind)] from a build_diff map JSON (rva_field of each row) or a
    plain list: one RVA per line, optional `code`/`data`, # comments."""
    text = Path(path).read_text(encoding="utf-8-sig")
    out, seen = [], set()
    if text.lstrip().startswith("["):
        for row in json.loads(text):
            if row.get(rva_field) is None:
                continue
            rva = _int(row[rva_field])
            if rva not in seen:
                seen.add(rva)
                out.append((rva, row.get("kind", "code")))
        return out
    for line in text.splitlines():
        words = line.split("#", 1)[0].split()
        if words:
            rva = int(words[0], 0)
            kind = words[1] if len(words) > 1 and words[1] in ("code", "data") else "code"
            if rva not in seen:
                seen.add(rva)
                out.append((rva, kind))
    return out


# --------------------------------------------------------------------------
# generate
# --------------------------------------------------------------------------

_RIP_MODRM = re.compile(rb"[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]")
_REL32_E8E9 = re.compile(rb"[\xe8\xe9]")
_REL32_JCC = re.compile(rb"\x0f[\x80-\x8f]")


def scan_refs(im, wanted):
    """{target: [field rva]}: byte positions that look like a rel32 branch or
    a RIP-relative disp32 pointing at a wanted RVA.  A RIP-relative operand is
    a ModRM byte with mod 00 and rm 101, then the disp32, then 0, 1, 2 or 4
    immediate bytes.  These are candidates; a real decode verifies them."""
    wanted = set(wanted)
    hits = defaultdict(list)
    for rva, n, raw in exec_sections(im):
        buf = im.data[raw:raw + n]
        for pattern, tails in ((_RIP_MODRM, TAILS), (_REL32_E8E9, (0,)),
                               (_REL32_JCC, (0,))):
            for m in pattern.finditer(buf):
                p = m.end()
                if p + 4 > n:
                    continue
                base = rva + p + 4 + struct.unpack_from("<i", buf, p)[0]
                for tail in tails:
                    if base + tail in wanted:
                        hits[base + tail].append(rva + p)
    return hits


def generate(exe_path, targets, use_relocs=True, log=lambda s: None):
    """Build the signature table of exe_path for [(rva, kind)] targets.

    Returns (table, report); the report lists what is not usable: targets in
    no unit, non-unique functions, data without a reference."""
    md, cs, x86 = _capstone()
    im = Image(Path(exe_path).read_bytes())
    stamp, image_size = bd.read_identity(im)
    U = bd._Units(im)
    reloc_width = dict(read_relocs(im)) if use_relocs else {}
    reloc_rvas = sorted(reloc_width)
    by_len = defaultdict(list)
    for i in range(len(U)):
        by_len[U.en[i] - U.st[i]].append(i)

    units, uid_of, unit_refs = [], {}, {}
    data_spans = [(rva, rva + len(buf)) for rva, buf in data_sections(im)]
    code_spans = [(rva, rva + n) for rva, n, _ in exec_sections(im)]

    def unit_id(ui):
        if ui in uid_of:
            return uid_of[ui]
        body, start = U.body(ui), U.st[ui]
        pairs, refs, bad, suspect = decode_unit(md, cs, x86, body, start,
                                                data_spans, code_spans)
        for k in range(bisect.bisect_left(reloc_rvas, start), len(reloc_rvas)):
            r = reloc_rvas[k]
            if r >= start + len(body):
                break
            pairs.append((r - start, min(reloc_width[r], len(body) - (r - start))))
        flat = merge_ranges(pairs)
        want = masked_bytes(body, flat)
        same = [uj for uj in by_len[len(body)]
                if uj != ui and masked_bytes(U.body(uj), flat) == want]
        units.append({"len": len(body), "hash": "%016X" % bd.fnv1a64(want),
                      "mask": flat, "leaf": int(U.leaf[ui]), "rva": start,
                      "same": 1 + len(same), "undecoded": bad, "suspect": suspect})
        uid_of[ui] = len(units) - 1
        unit_refs[uid_of[ui]] = refs
        return uid_of[ui]

    report = {"no_unit": [], "non_unique": [], "leaf": [], "undecoded": [],
              "code_no_ref": [], "code_orphan": [], "data_no_ref": [],
              "data_weak_ref": [], "data_orphan": []}
    out_targets, pending = [], []
    for rva, kind in targets:
        if kind != "code":
            continue
        ui = U.at(rva)
        if ui < 0:
            report["no_unit"].append(rva)
            continue
        uid = unit_id(ui)
        entry = {"rva": rva, "kind": "code", "unit": uid, "off": rva - U.st[ui]}
        out_targets.append(entry)
        if units[uid]["same"] > 1:
            report["non_unique"].append(rva)
            pending.append((entry, ui))
        if units[uid]["leaf"]:
            report["leaf"].append(rva)
        if units[uid]["undecoded"] or units[uid]["suspect"]:
            report["undecoded"].append(rva)
    code_units = set(uid_of.values())

    data = [rva for rva, kind in targets if kind == "data"]
    want = {U.st[ui] for _, ui in pending} | set(data)
    hits = scan_refs(im, want) if want else {}
    log("reference scan: %d candidate fields for %d wanted addresses"
        % (sum(len(v) for v in hits.values()), len(want)))

    def choose(address, own=-1):
        """Up to MAX_REFS references to address, each from a unit that is
        unique in this exe: units already in the table first, then units long
        enough to be distinctive, shortest first."""
        by_unit = {}
        for p in hits.get(address, ()):
            ui = U.at(p)
            if ui >= 0 and ui != own:
                by_unit.setdefault(ui, []).append(p)
        order = sorted(by_unit, key=lambda ui: (
            ui not in uid_of, U.en[ui] - U.st[ui] < PREFERRED_REF_LEN,
            U.en[ui] - U.st[ui]))
        refs = []
        for ui in order[:MAX_REF_CANDIDATES]:
            uid = unit_id(ui)
            if units[uid]["same"] != 1:
                continue
            for at, nxt, target in unit_refs[uid]:
                # The scan guessed the immediate length; the decode knows it.
                if target == address and U.st[ui] + at in by_unit[ui]:
                    refs.append({"unit": uid, "at": at, "next": nxt})
                    break
            if len(refs) >= MAX_REFS:
                break
        return refs

    def neighbours(ui):
        """The nearest unique unit on each side within NBR_SPAN units, as
        (unit, delta): the target is `delta` units from it in address order."""
        out = []
        for side in (-1, 1):
            for step in range(1, NBR_SPAN + 1):
                j = ui + side * step
                if not 0 <= j < len(U):
                    break
                uid = unit_id(j)
                if units[uid]["same"] == 1:
                    out.append({"unit": uid, "delta": -side * step})
                    break
        return out

    for entry, ui in pending:
        refs = choose(U.st[ui], own=ui)
        if refs:
            entry["refs"] = refs
        nbr = neighbours(ui)
        if nbr:
            entry["nbr"] = nbr
        if not refs and not nbr:
            report["code_orphan"].append(entry["rva"])
        elif not refs:
            report["code_no_ref"].append(entry["rva"])
    base = image_base(im)
    entry_index = {t["rva"]: i for i, t in enumerate(out_targets) if t["off"] == 0}
    for rva in data:
        refs = choose(rva)
        guard = data_guard(im, U, rva)
        entry = {"rva": rva, "kind": "data", "sec": section_name_at(im, rva),
                 "guard": guard, "refs": refs}
        if guard and guard["kind"] == "code-ptr":
            # The cell holds a function's address: finding that function is a
            # second way to find the cell.  A function that is itself a target
            # is found by its own entry; any other must be unique.
            ptr = struct.unpack("<Q", bd._read_rva(im, rva, 8))[0] - base
            pui = U.at(ptr)
            if ptr in entry_index:
                entry["ptee"] = {"target": entry_index[ptr]}
            elif pui >= 0 and units[unit_id(pui)]["same"] == 1:
                entry["ptee"] = {"unit": uid_of[pui], "off": ptr - U.st[pui]}
        if not refs:
            report["data_no_ref"].append(rva)
            if "ptee" not in entry and not (guard and guard["kind"] == "str"):
                report["data_orphan"].append(rva)
        elif not any(r["unit"] in code_units for r in refs):
            report["data_weak_ref"].append(rva)
        out_targets.append(entry)

    table = {"format": FORMAT, "hash": HASH_NAME,
             "known": {"timestamp": "0x%X" % stamp, "image_size": "0x%X" % image_size},
             "units": units, "targets": out_targets}
    return table, report


def table_text(table):
    return json.dumps(table, separators=(",", ":")) + "\n"


# --------------------------------------------------------------------------
# resolve (the runtime matcher; standard library only)
# --------------------------------------------------------------------------

def resolve(table, exe_path, allow_layout=False):
    """Find every table target in exe_path.

    Returns (results, stats).  results has one dict per target: rva (known
    build), kind, new (RVA or None), via (hash; ref, ptr, content, nbr or a
    combination), reason (None, no-candidate, ambiguous, layout-only,
    data-ref-failed) and detail.  Only a unique answer, confirmed by every
    mechanism that has one, is accepted.  A clone told apart only by its
    neighbours (nbr) rests on the layout alone, which the survey found wrong
    for a family of identical thunks; it is refused (layout-only) unless
    allow_layout."""
    t0 = time.perf_counter()
    im = Image(Path(exe_path).read_bytes())
    t_load = time.perf_counter()
    U = bd._Units(im)
    t_units = time.perf_counter()
    by_len = defaultdict(list)
    for i in range(len(U)):
        by_len[U.en[i] - U.st[i]].append(i)
    t_group = time.perf_counter()
    stats = {"units": len(U), "leaf": sum(U.leaf), "candidates": 0, "bytes": 0,
             "pages": set(), "table_units": len(table["units"])}
    unit_state = {}

    def match(uid):
        """(state, matching unit indexes) of table unit uid in this exe."""
        if uid not in unit_state:
            u = table["units"][uid]
            want, found = int(u["hash"], 16), []
            for ui in by_len.get(u["len"], ()):
                body = U.body(ui)
                stats["candidates"] += 1
                stats["bytes"] += len(body)
                stats["pages"].update(range(U.off[ui] >> 12,
                                            (U.off[ui] + len(body) - 1 >> 12) + 1))
                if signature_hash(body, u["mask"]) == want:
                    found.append(ui)
            unit_state[uid] = ("ok" if len(found) == 1 else
                               "ambiguous" if found else "no-candidate", found)
        return unit_state[uid]

    def ref_values(refs):
        """(addresses the references name, why the others did not)."""
        values, why = set(), []
        for ref in refs:
            state, found = match(ref["unit"])
            if state != "ok":
                why.append(state)
                continue
            rel = struct.unpack_from("<i", U.body(found[0]), ref["at"])[0]
            values.add(U.st[found[0]] + ref["next"] + rel)
        return values, why

    def neighbour_starts(nbrs):
        """(starts the neighbours name, why the others did not): the unit
        `delta` units from a uniquely matched neighbour."""
        starts, why = set(), []
        for nb in nbrs:
            state, found = match(nb["unit"])
            j = found[0] + nb["delta"] if state == "ok" else -1
            if 0 <= j < len(U):
                starts.add(U.st[j])
            else:
                why.append("neighbour " + state)
        return starts, why

    results = []
    for tg in table["targets"]:
        row = {"rva": tg["rva"], "kind": tg["kind"], "new": None, "via": None,
               "reason": None, "detail": ""}
        refs = tg.get("refs", ())
        if tg["kind"] == "code":
            state, found = match(tg["unit"])
            if state == "ok":
                row["new"], row["via"] = U.st[found[0]] + tg["off"], "hash"
            elif state == "ambiguous" and (refs or tg.get("nbr")):
                # The clones are identical, so a second witness picks one;
                # every witness that answers must name one of the clones.
                clone_starts = {U.st[ui] for ui in found}
                answers, why, conflict = {}, [], False
                for name, (values, w) in (("ref", ref_values(refs)),
                                          ("nbr", neighbour_starts(tg.get("nbr", ())))):
                    why += w
                    if len(values) == 1 and next(iter(values)) in clone_starts:
                        answers[name] = next(iter(values))
                    elif values:
                        conflict = True
                        why.append(name + " misses the clones or disagrees")
                if answers and not conflict and len(set(answers.values())) == 1 \
                        and set(answers) == {"nbr"} and not allow_layout:
                    row["reason"] = "layout-only"
                    row["detail"] = "%d identical clones, named only by their neighbours" \
                        % len(found)
                elif answers and not conflict and len(set(answers.values())) == 1:
                    row["new"] = next(iter(answers.values())) + tg["off"]
                    row["via"] = "+".join(sorted(answers))
                else:
                    if len(set(answers.values())) > 1:
                        why.append("answers disagree")
                    row["reason"] = "ambiguous"
                    row["detail"] = "%d matches; %s" % (
                        len(found), "; ".join(why) or "no witness answered")
            else:
                row["reason"] = state
                row["detail"] = "%d matches" % len(found) if found else ""
        else:
            answers, why, conflict = {}, [], False
            if refs:
                values, w = ref_values(refs)
                why += w
                if len(values) == 1:
                    answers["ref"] = next(iter(values))
                elif values:
                    conflict = True
                    why.append("references disagree")
            ptee = tg.get("ptee")
            if ptee:
                pointee = None
                if "target" in ptee:  # A target of the table: already resolved.
                    done = results[ptee["target"]]
                    pointee = done["new"]
                    if pointee is None:
                        why.append("pointee " + str(done["reason"]))
                else:
                    state, found = match(ptee["unit"])
                    if state == "ok":
                        pointee = U.st[found[0]] + ptee["off"]
                    else:
                        why.append("pointee " + state)
                if pointee is not None:
                    holders = pointer_cells(im, image_base(im) + pointee)
                    if len(holders) == 1:
                        answers["ptr"] = holders[0]
                    else:
                        why.append("%d cells hold the pointer" % len(holders))
            if (tg.get("guard") or {}).get("kind") == "str":
                hits = string_hits(im, tg["guard"])
                if len(hits) == 1:
                    answers["content"] = hits[0]
                else:
                    why.append("%d strings match" % len(hits))
            new = next(iter(answers.values())) if answers else None
            if not answers or conflict or len(set(answers.values())) != 1:
                row["reason"] = "data-ref-failed"
                if len(set(answers.values())) > 1:
                    why.append("answers disagree: " + ", ".join(
                        "%s 0x%X" % kv for kv in sorted(answers.items())))
                row["detail"] = "; ".join(why) or "no reference in the table"
            elif section_name_at(im, new) != tg.get("sec"):
                row["reason"], row["detail"] = "data-ref-failed", \
                    "0x%X is outside %s" % (new, tg.get("sec"))
            elif not guard_ok(tg.get("guard"), im, U, new):
                row["reason"], row["detail"] = "data-ref-failed", \
                    "0x%X fails the %s guard" % (new, tg["guard"]["kind"])
            else:
                row["new"], row["via"] = new, "+".join(sorted(answers))
        results.append(row)
    t_end = time.perf_counter()
    stats.update(pages=len(stats["pages"]), load_s=t_load - t0,
                 units_s=t_units - t_load, group_s=t_group - t_units,
                 match_s=t_end - t_group, total_s=t_end - t0)
    return results, stats


# --------------------------------------------------------------------------
# check
# --------------------------------------------------------------------------

def read_expect(path, invert=False):
    """{known-build rva: (expected rva or None, status)} from a build_diff map
    --json file.  Rows map rva -> new_rva; with invert the rows run the other
    way (e.g. .agents/build-334925/map_all.json, 334473 -> 334925)."""
    out = {}
    for row in json.loads(Path(path).read_text(encoding="utf-8-sig")):
        a, b = row.get("rva"), row.get("new_rva")
        if invert:
            a, b = b, a
        if a is None:
            continue
        mapped = row.get("status") in MAPPED and b is not None
        out[_int(a)] = (_int(b) if mapped else None, row.get("status"))
    return out


def classify(results, expect):
    """Per-target class: EQUAL, WRONG, UNRESOLVED or GT-CHANGED (the ground
    truth has no confident new address).  Returns rows with a `cls` field."""
    rows = []
    for r in results:
        want, status = expect.get(r["rva"], (None, "no-ground-truth"))
        row = dict(r, want=want, gt_status=status)
        if want is None:
            row["cls"] = "GT-CHANGED"
        elif r["new"] == want:
            row["cls"] = "EQUAL"
        elif r["new"] is None:
            row["cls"] = "UNRESOLVED"
        else:
            row["cls"] = "WRONG"
        rows.append(row)
    return rows


def hex_tokens(text):
    for m in re.finditer(r"0[xX][0-9A-Fa-f']+", text):
        try:
            yield int(m.group(0).replace("'", ""), 16)
        except ValueError:
            continue


def source_files(src_dir, rvas):
    """{rva: sorted relative paths under src_dir that spell it (as an RVA or
    as a VA at the preferred image base)}."""
    wanted, out = set(rvas), defaultdict(set)
    root = Path(src_dir)
    for path in sorted(root.rglob("*")):
        if path.suffix.lower() not in SRC_SUFFIXES or not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for value in hex_tokens(text):
            for cand in (value, value - bd.IMAGE_BASE):
                if cand in wanted:
                    out[cand].add(path.relative_to(root).as_posix())
    return {k: sorted(v) for k, v in out.items()}


def check_report(rows, files, stats):
    """Lines describing a classified check run."""
    lines = []
    count = defaultdict(int)
    for r in rows:
        count[r["cls"]] += 1
    reasons = defaultdict(int)
    for r in rows:
        if r["cls"] == "UNRESOLVED":
            reasons[r["reason"]] += 1
    lines.append("targets %d: EQUAL %d, WRONG %d, UNRESOLVED %d%s, ground truth CHANGED/unmapped %d"
                 % (len(rows), count["EQUAL"], count["WRONG"], count["UNRESOLVED"],
                    " (" + ", ".join("%s %d" % kv for kv in sorted(reasons.items())) + ")"
                    if reasons else "", count["GT-CHANGED"]))
    for kind in ("code", "data"):
        sub = [r for r in rows if r["kind"] == kind]
        lines.append("  %-4s %3d: EQUAL %d, WRONG %d, UNRESOLVED %d, GT-CHANGED %d" % (
            kind, len(sub), *(sum(1 for r in sub if r["cls"] == c)
                              for c in ("EQUAL", "WRONG", "UNRESOLVED", "GT-CHANGED"))))
    via = defaultdict(int)
    for r in rows:
        if r["cls"] == "EQUAL":
            via[(r["kind"], r["via"])] += 1
    lines.append("  EQUAL by how: " + ", ".join(
        "%s/%s %d" % (k, v, n) for (k, v), n in sorted(via.items())))
    for r in rows:
        if r["cls"] == "WRONG":
            lines.append("WRONG 0x%X (%s, via %s): resolved 0x%X, ground truth 0x%X (%s)"
                         % (r["rva"], r["kind"], r["via"], r["new"], r["want"], r["gt_status"]))
    for r in rows:
        if r["cls"] == "GT-CHANGED":
            lines.append("GT-CHANGED 0x%X (%s): ground truth %s; matcher %s" % (
                r["rva"], r["kind"], r["gt_status"],
                "resolved 0x%X via %s" % (r["new"], r["via"]) if r["new"] is not None
                else r["reason"]))
    for r in rows:
        if r["cls"] == "UNRESOLVED":
            lines.append("UNRESOLVED 0x%X (%s): %s%s" % (
                r["rva"], r["kind"], r["reason"],
                " (%s)" % r["detail"] if r["detail"] else ""))
    if files is not None:
        lines.append("per source file (an address spelled in several files counts in each):")
        by_file = defaultdict(list)
        orphan = []
        for r in rows:
            names = files.get(r["rva"])
            if names:
                for name in names:
                    by_file[name].append(r)
            else:
                orphan.append(r)
        whole = 0
        for name in sorted(by_file):
            group = by_file[name]
            eq = sum(1 for r in group if r["cls"] == "EQUAL")
            bad = [r for r in group if r["cls"] == "WRONG"]
            lines.append("  %-44s %3d/%-3d EQUAL%s%s" % (
                name, eq, len(group), "  WRONG %d" % len(bad) if bad else "",
                "  [whole]" if eq == len(group) else ""))
            whole += eq == len(group)
        lines.append("  files whole: %d of %d" % (whole, len(by_file)))
        if orphan:
            lines.append("  not spelled under src (%d): %s" % (
                len(orphan), " ".join("0x%X:%s" % (r["rva"], r["cls"][0]) for r in orphan)))
    lines.append("resolve: %d units (%d leaf), %d candidates hashed over %d bytes in %d pages; "
                 "load %.2fs, units %.2fs, group %.2fs, match %.2fs, total %.2fs"
                 % (stats["units"], stats["leaf"], stats["candidates"], stats["bytes"],
                    stats["pages"], stats["load_s"], stats["units_s"], stats["group_s"],
                    stats["match_s"], stats["total_s"]))
    return lines


# --------------------------------------------------------------------------
# survey: the matcher on units other than the hook addresses.
# --------------------------------------------------------------------------

MIN_SURVEY_LEN = 5


def survey(known, other, cache, n_random, per_status, seed, allow_layout=False):
    """Run the matcher on a sample of the known exe's units and compare with
    tools/build_diff.py's map to `other`.

    The hook addresses are functions that stayed put across the builds we
    hold, so check cannot show what the matcher does with a function that
    changed.  The sample is n_random units plus up to per_status of each
    status the map assigns (IDENTICAL, RELOC-ONLY, CHANGED, AMBIGUOUS,
    NOT-FOUND).  Returns (counts, details, text lines): counts maps
    (map status, outcome, via or reason) to a number; details lists the
    WRONG rows and the rows where the map says CHANGED."""
    bm = bd.build_map(known, other, cache)
    O, rng = bm.O, random.Random(seed)
    usable = [i for i in range(len(O)) if O.en[i] - O.st[i] >= MIN_SURVEY_LEN]
    pick = set(rng.sample(usable, min(n_random, len(usable))))
    by_status = defaultdict(list)
    for i in usable:
        by_status[bm.status[i]].append(i)
    for rows in by_status.values():
        pick.update(rng.sample(rows, min(per_status, len(rows))))
    table, report = generate(known, [(O.st[i], "code") for i in sorted(pick)])
    results, stats = resolve(table, other, allow_layout)
    counts, wrong, changed = defaultdict(int), [], []
    for r in results:
        gt = bm.lookup(r["rva"])
        if gt["status"] in ("IDENTICAL", "RELOC-ONLY"):
            if r["new"] == gt["new_rva"]:
                key = ("mapped", "EQUAL", r["via"])
            elif r["new"] is None:
                key = ("mapped", "UNRESOLVED", r["reason"])
            else:
                key = ("mapped", "WRONG", r["via"])
                wrong.append((r["rva"], gt["new_rva"], r["new"], r["via"]))
        elif r["new"] is None:
            key = (gt["status"], "refused", r["reason"])
        else:
            key = (gt["status"], "resolved", r["via"])
            guess = gt.get("new_rva") if gt.get("new_rva") is not None else gt.get("placed_rva")
            if gt["status"] == "CHANGED" and guess != r["new"]:
                changed.append((r["rva"], guess, r["new"], r["via"]))
        counts[key] += 1
    lines = ["sample %d units of %d (%d non-unique in the known exe); candidates hashed %d"
             % (len(pick), len(usable), len(report["non_unique"]), stats["candidates"])]
    lines += ["  %-10s %-10s %-12s %d" % (*k, v) for k, v in sorted(counts.items(), key=str)]
    mapped = sum(v for k, v in counts.items() if k[0] == "mapped")
    lines.append("map says the function is in the new build (IDENTICAL/RELOC-ONLY): %d; "
                 "WRONG %d" % (mapped, len(wrong)))
    for row in wrong:
        lines.append("  WRONG 0x%X: map 0x%X, matcher 0x%X (via %s)" % row)
    lines.append("map says CHANGED and the matcher names another place: %d" % len(changed))
    return counts, {"wrong": wrong, "changed": changed}, lines


# --------------------------------------------------------------------------
# Self-test: a synthetic exe pair.
# --------------------------------------------------------------------------

class _Asm:
    """Just enough x86-64 to build functions with the fields the mask covers."""

    def __init__(self, base):
        self.base, self.buf, self.relocs = base, bytearray(), []

    def here(self):
        return self.base + len(self.buf)

    def raw(self, *chunks):
        for c in chunks:
            self.buf += bytes.fromhex(c) if isinstance(c, str) else c
        return self

    def rip(self, prefix, target):
        """prefix bytes, then a disp32 that reaches target from the next byte."""
        head = bytes.fromhex(prefix)
        end = self.here() + len(head) + 4
        self.buf += head + (target - end).to_bytes(4, "little", signed=True)
        return self

    def call(self, target):
        return self.rip("E8", target)

    def jcc(self, target):
        return self.rip("0F84", target)

    def lea_rax(self, target):
        return self.rip("488D05", target)

    def mov_rcx(self, target):
        return self.rip("488B0D", target)

    def movdqa_store(self, target):
        # 66-prefixed SSE: capstone 5.0.7 reports disp_size 2 for this form.
        return self.rip("660F7F05", target)

    def mov_rbx_field(self, disp):
        self.buf += bytes.fromhex("8B83") + disp.to_bytes(4, "little")
        return self

    def mov_rcx_abs(self, value):
        self.relocs.append(self.here() + 2)
        self.buf += bytes.fromhex("48B9") + value.to_bytes(8, "little")
        return self


_FX_STRING = b"name_literal"
_FX_ORPHAN = b"orphan_name_literal"


def _reloc_block(page, rvas):
    entries = [0xA000 | (r - page) for r in rvas]  # DIR64
    entries += [0] * (len(entries) % 2)
    return struct.pack("<II", page, 8 + 2 * len(entries)) \
        + b"".join(struct.pack("<H", e) for e in entries)


def _fixture(new, break_data=False):
    """Known build (new False) or unknown build (new True): (exe bytes, names).

    Known text: G F1 F2 C1 C2 LEAF H P1 P2 Q1 Q2 S1 S2 W1.  Unknown text: X G
    F1 F2 C1 C2 LEAF H P1 P2 Q1 Q2 S1 S2 W1e W1c, everything shifted, data
    moved.  F1 changes only in fields the mask covers (EQUAL); F2's struct
    displacement changes (UNRESOLVED); C1 and C2 are identical clones that P1
    and P2 call, and with break_data they swap places, so the caller and the
    neighbours name different clones; LEAF has no .pdata entry; W1 is
    edited in the unknown build and an exact copy of the old W1 sits beside
    it, so the matcher resolves W1 to the wrong function.  Data: D1 and D2
    are read by F1 and H; D3 nothing refers to; D4 is a cell holding P1's
    address that nothing refers to; D5 a string read by S1; D6 a cell holding
    G's address read by S2; D7 a string nothing refers to; D8 a cell holding
    C1's address that nothing refers to.  Q1 and Q2 are clones that nothing
    calls.  With break_data the unknown build's strings differ and its cells
    are null."""
    text_rva, data_rva = 0x1000, 0x3200 if new else 0x3000
    pdata_rva, reloc_rva = 0x4200 if new else 0x4000, 0x5000
    S = {"D1": data_rva + 0x10, "D2": data_rva + 0x40, "D3": data_rva + 0x80,
         "D4": data_rva + 0x50, "D5": data_rva + 0x100, "D6": data_rva + 0x120,
         "D7": data_rva + 0x140, "D8": data_rva + 0x160}
    tail = ["LEAF", "H", "P1", "P2", "Q1", "Q2", "S1", "S2", "J", "T"]
    order = (["X"] if new else []) + ["G", "F1", "F2"] \
        + (["C2", "C1"] if break_data else ["C1", "C2"]) + tail \
        + (["W1e", "W1c"] if new else ["W1"])
    sizes = {"X": 0x20, "G": 0x30, "F1": 0x60, "F2": 0x30, "C1": 0x30, "C2": 0x30,
             "LEAF": 0x10, "H": 0x40, "P1": 0x20, "P2": 0x20, "Q1": 0x20, "Q2": 0x20,
             "S1": 0x20, "S2": 0x20, "J": 0x30, "T": 0x20, "W1": 0x30, "W1e": 0x30,
             "W1c": 0x30}
    cursor = text_rva
    for name in order:
        S[name] = cursor
        cursor = (cursor + sizes[name] + 15) & ~15
    text_size = cursor - text_rva
    a = {name: _Asm(S[name]) for name in order}
    a["G"].raw("4883EC28", "B907000000", "01C8", "4883C428", "C3")
    if "X" in a:
        a["X"].raw("4883EC28", "BA55000000", "31C0", "4883C428", "C3")
    f1 = a["F1"]
    f1.raw("4883EC28").call(S["G"]).lea_rax(S["D1"]).movdqa_store(S["D1"] + 0x20)
    f1.mov_rbx_field(0x30)
    f1.mov_rcx_abs(bd.IMAGE_BASE + S["D1"]).jcc(f1.here() + 8).raw("31C0", "4883C428", "C3")
    S["F1+lea"] = S["F1"] + 9
    a["F2"].raw("4883EC28").mov_rbx_field(0x38 if new else 0x30).raw("4883C428", "C3")
    for name in ("C1", "C2"):
        a[name].raw("4883EC28", "B905000000", "31C0", "4883C428", "C3")
    a["LEAF"].raw("B867452301", "6BC003", "C3")
    a["H"].mov_rcx(S["D2"]).raw("BA99000000", "C3")
    a["P1"].raw("4883EC28").call(S["C1"]).raw("4883C428", "C3")
    a["P2"].raw("4883EC28", "B811000000").call(S["C2"]).raw("4883C428", "C3")
    for name in ("Q1", "Q2"):  # Clones that nothing calls.
        a[name].raw("4883EC28", "B933000000", "31C0", "4883C428", "C3")
    a["S1"].raw("4883EC28").rip("488D15", S["D5"]).raw("B877000000", "4883C428", "C3")
    a["S2"].rip("488B05", S["D6"]).raw("BA78000000", "C3")
    # A switch: the image base in rdx, a 4-byte table of case RVAs inside the
    # function, reached as [rdx + rax*4 + RVA of the table].
    j = S["J"]
    a["J"].rip("488D15", 0).raw("8B8C82", (j + 32).to_bytes(4, "little"), "4803CA", "FFE1",
                                "B801000000C3", "B802000000C3", "90",
                                (j + 19).to_bytes(4, "little") + (j + 25).to_bytes(4, "little"))
    # A fragment that is only the tail of a switch: the table is the last thing
    # in the unit and nothing in the unit reaches it.
    t = S["T"]
    a["T"].raw("C3", "909090", "4883C428", "C3", "90909090",
               (t + 4).to_bytes(4, "little") + (t + 8).to_bytes(4, "little")
               + (t + 12).to_bytes(4, "little"))
    wbody = lambda x: ["4883EC28", "BA%02X000000" % x]
    if new:
        a["W1e"].raw(*wbody(0xAC)).call(S["G"]).raw("4883C428", "C3")
        a["W1c"].raw(*wbody(0xAB)).call(S["G"]).raw("4883C428", "C3")
    else:
        a["W1"].raw(*wbody(0xAB)).call(S["G"]).raw("4883C428", "C3")
    funcs = [n for n in order if n != "LEAF"]
    raw_text, raw_data = 0x400, 0x400 + text_size
    raw_pdata, raw_reloc = raw_data + 0x200, raw_data + 0x200 + 12 * len(funcs)
    exe = bytearray(bd._fixture_pe(
        text_size=text_size, extra_sections=[
            (".rdata", data_rva, 0x200, raw_data, 0x40000040),
            (".pdata", pdata_rva, 12 * len(funcs), raw_pdata, 0x40000040),
            (".reloc", reloc_rva, 0x40, raw_reloc, 0x42000040)],
        exception_dir=(pdata_rva, 12 * len(funcs))))
    exe[raw_text:raw_text + text_size] = b"\xcc" * text_size
    for name in order:
        off = raw_text + S[name] - text_rva
        exe[off:off + len(a[name].buf)] = a[name].buf
        assert len(a[name].buf) <= sizes[name], name
    for k, name in enumerate(funcs):
        exe[raw_pdata + 12 * k:raw_pdata + 12 * k + 12] = struct.pack(
            "<III", S[name], S[name] + len(a[name].buf), pdata_rva)
    # Base relocations: the absolute pointer in F1 and the two code-pointer cells.
    cells = {"D4": "P1", "D6": "G", "D8": "C1"}
    blocks = _reloc_block(S["F1"] & ~0xFFF, a["F1"].relocs) \
        + _reloc_block(data_rva & ~0xFFF, [S[c] for c in cells])
    exe[raw_reloc:raw_reloc + len(blocks)] = blocks
    struct.pack_into("<II", exe, 0x80 + 24 + 112 + 5 * 8, reloc_rva, len(blocks))
    struct.pack_into("<Q", exe, 0x80 + 24 + 24, bd.IMAGE_BASE)
    for cell, func in cells.items():
        at = raw_data + S[cell] - data_rva
        exe[at:at + 8] = bytes(8) if break_data \
            else struct.pack("<Q", bd.IMAGE_BASE + S[func])
    for cell, text in (("D5", _FX_STRING), ("D7", _FX_ORPHAN)):
        at = raw_data + S[cell] - data_rva
        exe[at:at + len(text) + 1] = text + b"\0"
        if break_data:
            exe[at] ^= 0x20
    return bytes(exe), S


def self_test():
    failures = []

    def check(condition, message=""):
        if not condition:
            failures.append(message or "check %d" % (len(failures) + 1))
            print("FAIL: " + failures[-1])

    def quiet(argv):
        with contextlib.redirect_stdout(io.StringIO()):
            return main(argv)

    known, K = _fixture(False)
    unknown, N = _fixture(True)
    broken, _ = _fixture(True, break_data=True)
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        kp, np_, bp = td / "known.exe", td / "unknown.exe", td / "broken.exe"
        kp.write_bytes(known)
        np_.write_bytes(unknown)
        bp.write_bytes(broken)
        targets = [(K["F1"], "code"), (K["F1+lea"], "code"), (K["F2"], "code"),
                   (K["C1"], "code"), (K["LEAF"], "code"), (K["G"], "code"),
                   (K["W1"], "code"), (K["D1"], "data"), (K["D2"], "data"),
                   (K["D3"], "data"), (K["D4"], "data"), (K["D5"], "data"),
                   (K["D6"], "data"), (K["D7"], "data"), (K["D8"], "data"),
                   (K["Q1"], "code"), (K["J"], "code"), (K["T"], "code"),
                   (K["D1"] + 0x1000, "code")]
        table, report = generate(kp, targets)
        by_rva = {t["rva"]: t for t in table["targets"]}
        u = table["units"]
        check(K["D1"] + 0x1000 in report["no_unit"], "target outside any unit reported")
        check(K["C1"] in report["non_unique"], "identical clones reported non-unique")
        check(u[by_rva[K["F1"]]["unit"]]["same"] == 1, "F1 unique")
        check(K["LEAF"] in report["leaf"] and u[by_rva[K["LEAF"]]["unit"]]["leaf"],
              "leaf unit flagged")
        check(by_rva[K["F1+lea"]]["unit"] == by_rva[K["F1"]]["unit"]
              and by_rva[K["F1+lea"]]["off"] == 9, "two sites share one unit")
        f1 = u[by_rva[K["F1"]]["unit"]]
        pairs = list(zip(f1["mask"][::2], f1["mask"][1::2]))
        check((5, 4) in pairs and (12, 4) in pairs and (42, 4) in pairs,
              "call, lea and jcc rel32 masked")
        check((20, 4) in pairs, "66-prefixed SSE RIP disp masked in full")
        check((32, 8) in pairs, "relocated imm64 masked")
        check(len(pairs) == 5 and not any(o <= 26 < o + ln for o, ln in pairs),
              "struct displacement stays exact")
        check(f1["suspect"] == 0 and f1["undecoded"] == 0, "F1 decoded cleanly")
        jt = u[by_rva[K["J"]]["unit"]]
        check(jt["mask"] == [3, 4, 10, 4, 32, 8], "jump table: disp32 and entries masked: %s"
              % jt["mask"])
        check(u[by_rva[K["T"]]["unit"]]["mask"] == [13, 12], "a unit's trailing RVA table masked")
        check(len(by_rva[K["C1"]].get("refs", ())) == 1
              and K["C1"] not in report["code_no_ref"], "clone target gets its caller")
        check(K["Q1"] in report["non_unique"] and K["Q1"] in report["code_no_ref"]
              and not report["code_orphan"] and "refs" not in by_rva[K["Q1"]]
              and len(by_rva[K["Q1"]]["nbr"]) == 2, "uncalled clone gets two neighbours")
        check(report["data_orphan"] == [K["D3"]], "only D3 has no way to be found")
        check({K["D3"], K["D4"], K["D7"], K["D8"]} == set(report["data_no_ref"]),
              "data without a reference reported")
        check(by_rva[K["D1"]]["refs"] and by_rva[K["D1"]]["refs"][0]["unit"]
              == by_rva[K["F1"]]["unit"], "data ref prefers a code target's unit")
        check(K["D2"] in report["data_weak_ref"] and by_rva[K["D2"]]["refs"],
              "data ref in a non-target unit")
        check(by_rva[K["D5"]]["guard"]["kind"] == "str"
              and by_rva[K["D6"]]["guard"] == {"kind": "code-ptr"}, "guards recorded")
        check("unit" in by_rva[K["D4"]]["ptee"] and "ptee" not in by_rva[K["D5"]],
              "pointer cells record their function")
        check("target" in by_rva[K["D6"]]["ptee"] and "target" in by_rva[K["D8"]]["ptee"],
              "a cell holding a target's address names the target")
        text = table_text(table)
        check(not re.search(r"[0-9A-Fa-f]{17,}", text), "table holds no raw bytes")
        check(all(s.decode() not in text and s.hex() not in text.lower()
                  for s in (_FX_STRING, _FX_ORPHAN)), "table holds no string content")
        kunits = bd._Units(Image(known))
        check(all(kunits.body(i).hex()[:16] not in text.lower()
                  for i in range(len(kunits)) if len(kunits.body(i)) >= 8),
              "table leaks code bytes")

        # Resolve against the unknown build.
        res, stats = resolve(table, np_)
        got = {r["rva"]: r for r in res}
        check(got[K["F1"]]["new"] == N["F1"], "F1 moved with changed rel32/RIP/reloc fields")
        check(got[K["F1+lea"]]["new"] == N["F1"] + 9, "offset inside the unit kept")
        check(got[K["G"]]["new"] == N["G"] and got[K["G"]]["via"] == "hash",
              "plain move resolves")
        check(got[K["LEAF"]]["new"] == N["LEAF"], "leaf unit resolves")
        check(got[K["J"]]["new"] == N["J"], "function with an inline jump table resolves")
        check(got[K["F2"]]["reason"] == "no-candidate", "changed struct offset is unresolved")
        check(got[K["C1"]]["new"] == N["C1"] and got[K["C1"]]["via"] == "nbr+ref",
              "clones told apart by their caller and neighbours")
        check(got[K["T"]]["new"] == N["T"], "function ending in a table of RVAs resolves")
        check(got[K["Q1"]]["new"] is None and got[K["Q1"]]["reason"] == "layout-only",
              "uncalled clone named only by its neighbours is refused")
        lenient = {r["rva"]: r for r in resolve(table, np_, allow_layout=True)[0]}
        check(lenient[K["Q1"]]["new"] == N["Q1"] and lenient[K["Q1"]]["via"] == "nbr",
              "uncalled clone told apart by its neighbours when layout is allowed")
        check(got[K["D8"]]["new"] == N["D8"] and got[K["D8"]]["via"] == "ptr",
              "cell found through a resolved target")
        check(got[K["W1"]]["new"] == N["W1c"], "removed function resolves to its clone")
        check(got[K["D1"]]["new"] == N["D1"], "data through a target unit")
        check(got[K["D2"]]["new"] == N["D2"], "data through a non-target unit")
        check(got[K["D3"]]["reason"] == "data-ref-failed", "data without a way in fails")
        check(got[K["D4"]]["new"] == N["D4"] and got[K["D4"]]["via"] == "ptr",
              "pointer cell found through its function")
        check(got[K["D7"]]["new"] == N["D7"] and got[K["D7"]]["via"] == "content",
              "string found by content")
        check(got[K["D5"]]["new"] == N["D5"] and got[K["D5"]]["via"] == "content+ref"
              and got[K["D6"]]["new"] == N["D6"] and got[K["D6"]]["via"] == "ptr+ref",
              "two mechanisms that agree")
        check(stats["leaf"] == 1 and stats["candidates"] > 0, "stats counted")
        res_k, _ = resolve(table, kp)
        check(all(r["new"] == r["rva"] for r in res_k if r["reason"] is None),
              "known build resolves to itself")
        got_b = {r["rva"]: r for r in resolve(table, bp)[0]}
        check(all(got_b[K[d]]["reason"] == "data-ref-failed" for d in ("D4", "D5", "D6", "D7", "D8")),
              "changed strings and null cells are refused")
        check("str guard" in got_b[K["D5"]]["detail"] and "code-ptr guard" in got_b[K["D6"]]["detail"],
              "guards reject changed content")
        check(got_b[K["C1"]]["reason"] == "ambiguous" and "disagree" in got_b[K["C1"]]["detail"],
              "witnesses that name different clones are refused")

        # Without the relocation mask the absolute pointer breaks F1.
        table_nr, _ = generate(kp, targets, use_relocs=False)
        res_nr, _ = resolve(table_nr, np_)
        check({r["rva"]: r for r in res_nr}[K["F1"]]["reason"] == "no-candidate",
              "relocation mask is what keeps F1 matching")

        # check: classes against a ground truth that says W1 went elsewhere.
        truth = [{"rva": "0x%X" % K[n], "new_rva": "0x%X" % N[m], "status": s, "kind": k}
                 for n, m, s, k in (
                     ("F1", "F1", "RELOC-ONLY", "code"), ("G", "G", "RELOC-ONLY", "code"),
                     ("F2", "F2", "CHANGED", "code"), ("C1", "C1", "RELOC-ONLY", "code"),
                     ("LEAF", "LEAF", "IDENTICAL", "code"), ("W1", "W1e", "RELOC-ONLY", "code"),
                     ("D1", "D1", "XREF-EXACT", "data"), ("D2", "D2", "XREF-EXACT", "data"),
                     ("D3", "D3", "XREF-EXACT", "data"), ("D4", "D4", "XREF-EXACT", "data"))]
        truth[2]["new_rva"] = None
        (td / "truth.json").write_text(json.dumps(truth), encoding="utf-8")
        rows = classify(res, read_expect(td / "truth.json"))
        cls = {r["rva"]: r["cls"] for r in rows}
        check(cls[K["F1"]] == "EQUAL" and cls[K["G"]] == "EQUAL" and cls[K["LEAF"]] == "EQUAL"
              and cls[K["C1"]] == "EQUAL" and cls[K["D4"]] == "EQUAL", "check counts EQUAL")
        check(cls[K["W1"]] == "WRONG", "check lists the dangerous class")
        check(cls[K["D3"]] == "UNRESOLVED", "check counts UNRESOLVED")
        check(cls[K["F2"]] == "GT-CHANGED", "check separates ground truth changed")
        inv = [{"rva": r["new_rva"], "new_rva": r["rva"], "status": r["status"]}
               for r in truth if r["new_rva"]]
        (td / "inv.json").write_text(json.dumps(inv), encoding="utf-8")
        check(read_expect(td / "inv.json", invert=True)[K["F1"]][0] == N["F1"], "--invert")
        files = {K["F1"]: ["a.cpp"], K["G"]: ["a.cpp", "b.cpp"]}
        lines = check_report(rows, files, stats)
        check(any(l.startswith("WRONG") for l in lines), "report prints WRONG rows")
        check(any("a.cpp" in l for l in lines) and any("b.cpp" in l for l in lines),
              "report breaks down per source file")

        # Source scan: RVA and VA spellings, digit separators.
        src = td / "src"
        src.mkdir()
        (src / "x.cpp").write_text("const a = 0x%X; const b = 0x%X; // 0x1'0\n"
                                   % (K["F1"], bd.IMAGE_BASE + K["G"]), encoding="utf-8")
        check(source_files(src, [K["F1"], K["G"]]) == {K["F1"]: ["x.cpp"], K["G"]: ["x.cpp"]},
              "source scan")

        # Plain-list targets.
        (td / "list.txt").write_text("0x1000 data # a\n0x2000\n", encoding="utf-8")
        check(read_targets(td / "list.txt") == [(0x1000, "data"), (0x2000, "code")],
              "plain target list")

        # generate --dry-run writes nothing at all; without it, the table.
        (td / "rvas.txt").write_text("\n".join("0x%X %s" % (r, k) for r, k in targets),
                                     encoding="utf-8")
        before = sorted(p.name for p in td.rglob("*"))
        out = td / "out" / "table.json"
        argv = ["generate", "--exe", str(kp), "--rvas", str(td / "rvas.txt"),
                "--out", str(out)]
        check(quiet(argv + ["--dry-run"]) == 0, "dry run succeeds")
        check(not out.exists() and not out.parent.exists()
              and sorted(p.name for p in td.rglob("*")) == before,
              "--dry-run wrote something")
        check(quiet(argv) == 0 and out.is_file(), "generate writes the table")
        check(json.loads(out.read_text(encoding="utf-8"))["units"] == table["units"],
              "written table equals the in-memory table")

        # survey on build_diff's own fixture pair: A B C G and the leaf stub move
        # and must come out EQUAL, the edited D must be refused, nothing is WRONG.
        old_exe, _ = bd._map_fixture(False)
        new_exe, _ = bd._map_fixture(True)
        (td / "old.exe").write_bytes(old_exe)
        (td / "new.exe").write_bytes(new_exe)
        counts, details, _ = survey(td / "old.exe", td / "new.exe", None, 50, 50, 1)
        check(not details["wrong"], "survey found a wrong answer on the fixture")
        check(sum(v for k, v in counts.items() if k[:2] == ("mapped", "EQUAL")) >= 5,
              "survey: the moved functions are found")
        check(not any(k[1] == "resolved" and k[0] == "CHANGED" for k in counts),
              "survey: an edited function is not resolved")

    print("hook_signatures self-test: %s" % ("FAILED (%d)" % len(failures) if failures else "ok"))
    return 1 if failures else 0


# --------------------------------------------------------------------------
# Command line.
# --------------------------------------------------------------------------

def _load_table(path):
    table = json.loads(Path(path).read_text(encoding="utf-8"))
    if table.get("format") != FORMAT or table.get("hash") != HASH_NAME:
        raise ValueError("%s is not a format-%d %s table" % (path, FORMAT, HASH_NAME))
    return table


def _summary(table, report, text):
    units, targets = table["units"], table["targets"]
    code = [t for t in targets if t["kind"] == "code"]
    data = [t for t in targets if t["kind"] == "data"]
    lines = ["%d targets: %d code in %d units, %d data" % (
        len(targets), len(code), len({t["unit"] for t in code}), len(data)),
        "table: %d units, %d mask ranges, %d bytes (unit lengths %d..%d)" % (
            len(units), sum(len(u["mask"]) // 2 for u in units), len(text.encode("utf-8")),
            min(u["len"] for u in units), max(u["len"] for u in units)),
        "code targets: leaf %d, in a non-unique function %d (%d of those have no caller "
        "reference, %d no neighbour either), undecodable or suspect %d, in no unit %d" % (
            len(report["leaf"]), len(report["non_unique"]),
            len(report["code_no_ref"]) + len(report["code_orphan"]),
            len(report["code_orphan"]), len(report["undecoded"]), len(report["no_unit"])),
        "data targets: no usable reference %d (of those, no other way to find %d), "
        "references only outside the code targets' units %d" % (
            len(report["data_no_ref"]), len(report["data_orphan"]),
            len(report["data_weak_ref"]))]
    for key, label in (("no_unit", "in no unit"), ("non_unique", "non-unique"),
                       ("code_no_ref", "non-unique, no caller"),
                       ("code_orphan", "non-unique, no caller, no neighbour"),
                       ("data_no_ref", "data, no reference"),
                       ("data_orphan", "data, no way in")):
        if report[key]:
            lines.append("  %s: %s" % (label, " ".join("0x%X" % r for r in report[key])))
    return lines


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command")
    layout = "accept a clone named only by its neighbours (refused by default)"
    gen = sub.add_parser("generate", help="build the signature table of the known exe")
    gen.add_argument("--exe", required=True, help="the known build's executable")
    gen.add_argument("--rvas", required=True,
                     help="build_diff map JSON, or a list: RVA [code|data] per line")
    gen.add_argument("--rva-field", default="new_rva",
                     help="JSON field holding the known build's RVA (default new_rva)")
    gen.add_argument("--out", required=True, help="table JSON to write")
    gen.add_argument("--dry-run", action="store_true", help="write nothing")
    res = sub.add_parser("resolve", help="find the table's targets in an exe")
    res.add_argument("--table", required=True)
    res.add_argument("--exe", required=True)
    res.add_argument("--allow-layout", action="store_true", help=layout)
    chk = sub.add_parser("check", help="resolve and compare with a ground truth")
    chk.add_argument("--table", required=True)
    chk.add_argument("--exe", required=True)
    chk.add_argument("--allow-layout", action="store_true", help=layout)
    chk.add_argument("--expect", required=True,
                     help="build_diff map --json output (known build -> this exe)")
    chk.add_argument("--invert", action="store_true",
                     help="the expect rows run exe -> known (map_all.json style)")
    chk.add_argument("--src", default=str(Path(__file__).resolve().parent.parent / "src"),
                     help="source tree searched for each address (per-file breakdown)")
    sv = sub.add_parser("survey", help="run the matcher on a sample of ALL units and "
                                       "compare with build_diff's map")
    sv.add_argument("--exe", required=True, help="the known build's executable")
    sv.add_argument("--other", required=True, help="the build to look in")
    sv.add_argument("--cache", default=None,
                    help="build_diff map cache (known -> other); written when absent")
    sv.add_argument("--random", type=int, default=1500, help="random units (default 1500)")
    sv.add_argument("--per-status", type=int, default=400,
                    help="units drawn from each map status (default 400)")
    sv.add_argument("--seed", type=int, default=7)
    sv.add_argument("--allow-layout", action="store_true", help=layout)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.command:
        ap.error("a subcommand is required (generate, resolve, check, survey, or --self-test)")
    try:
        if args.command == "survey":
            _, details, lines = survey(args.exe, args.other, args.cache, args.random,
                                       args.per_status, args.seed, args.allow_layout)
            print("\n".join(lines))
            return 1 if details["wrong"] else 0
        if args.command == "generate":
            targets = read_targets(args.rvas, args.rva_field)
            t0 = time.perf_counter()
            table, report = generate(args.exe, targets,
                                     log=lambda s: print(s, flush=True))
            text = table_text(table)
            for line in _summary(table, report, text):
                print(line)
            print("generated in %.1fs" % (time.perf_counter() - t0))
            if args.dry_run:
                print("dry run: would write %d bytes to %s; nothing written"
                      % (len(text.encode("utf-8")), args.out))
            else:
                out = Path(args.out)
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_text(text, encoding="utf-8")
                print("wrote " + str(out))
            return 0
        table = _load_table(args.table)
        results, stats = resolve(table, args.exe, args.allow_layout)
        if args.command == "resolve":
            for r in results:
                print("0x%-8X %-4s %s" % (r["rva"], r["kind"],
                      "0x%X (%s)" % (r["new"], r["via"]) if r["new"] is not None
                      else "%s%s" % (r["reason"], " (%s)" % r["detail"] if r["detail"] else "")))
            print("%d of %d resolved in %.2fs" % (
                sum(1 for r in results if r["new"] is not None), len(results),
                stats["total_s"]))
            return 0
        rows = classify(results, read_expect(args.expect, args.invert))
        files = source_files(args.src, [r["rva"] for r in rows]) \
            if Path(args.src).is_dir() else None
        for line in check_report(rows, files, stats):
            print(line)
        return 1 if any(r["cls"] == "WRONG" for r in rows) else 0
    except (OSError, PEError, ValueError, KeyError) as exc:
        print("hook_signatures: " + str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

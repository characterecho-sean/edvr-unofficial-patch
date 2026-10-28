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
    python tools/hook_signatures.py fixture --out-dir <dir> [--dry-run]

generate --emit-cpp <path> also writes the table as a C++ include
(src/common/game_signatures.inc, the runtime resolver's table; see
src/common/game_build_core.h); check --emit-expect <path> writes what the
runtime rig compares with (tools/game_build_test/expect_<build>.txt: the
ground truth and this matcher's answer per target); fixture writes the
self-test's synthetic exes with their table and answers for that rig.

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
    the clone's own fingerprints pick it out ("fp"): what its masked fields
    point at in the known build -- for a rel32 or RIP-relative operand that
    lands in another code unit, that unit's length, masked hash and the
    offset landed on; for a RIP-relative operand into a read-only data
    section, the FNV-1a-64 of up to 16 bytes there (relocated bytes and the
    import address table left out); and, for a unit whose last instruction
    does not end the flow, the length and masked hash of the unit it falls
    into -- and a candidate is accepted only when it is the one clone that
    shows every fingerprint; the unit at a fixed distance, in address order,
    from a unique neighbour ("nbr") names one too, but neighbours alone are
    refused (layout-only) unless --allow-layout, because a family of
    identical thunks that gained or lost a member is mis-named by position;
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

FORMAT = 2                # 2: clone fingerprints ("fp")
CPP_FORMAT = 1            # the layout of the emitted C++ table (game_build_core.h)
HASH_NAME = "fnv1a64-masked"
SCN_WRITE = 0x80000000    # IMAGE_SCN_MEM_WRITE
MAX_FPS = 8               # fingerprints recorded per clone target
RODATA_FP_LEN = 16        # bytes hashed for a read-only data fingerprint
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


def iat_range(im):
    """[start, end) RVAs of the import address table: the loader writes the
    imported functions' addresses there, so its bytes differ in memory."""
    rva, size = im.directory(12)
    return (rva, rva + size) if rva else (0, 0)


def rodata_bytes(im, rva, n):
    """The n bytes at rva when [rva, rva + n) lies in one section that is
    neither executable nor writable and outside the import address table
    (zero-filled past the raw data, as the loader maps it), else None."""
    if n <= 0:
        return None
    lo, hi = iat_range(im)
    if lo < hi and rva < hi and lo < rva + n:
        return None
    for start, span, _raw, _raw_size, flags in im.sections:
        if start <= rva < start + span:
            if rva + n > start + span or flags & (bd.SCN_EXEC | SCN_WRITE):
                return None
            return bd._read_rva(im, rva, n)
    return None


def unit_shows(U, ui, units, uid):
    """True when unit ui has table unit uid's length and masked hash."""
    u = units[uid]
    return U.en[ui] - U.st[ui] == u["len"] and \
        signature_hash(U.body(ui), u["mask"]) == int(u["hash"], 16)


def fp_ok(fp, im, U, ui, units):
    """True when unit ui of im shows the clone fingerprint fp.

    fall: the unit that starts where ui ends is table unit fp["unit"].  code:
    the rel32 or RIP-relative field at fp["at"] lands fp["off"] bytes into
    another unit that is table unit fp["unit"].  rodata: it lands in a
    read-only data section on fp["len"] bytes whose hash is fp["hash"]."""
    if fp["kind"] == "fall":
        k = ui + 1
        return k < len(U) and U.st[k] == U.en[ui] and unit_shows(U, k, units, fp["unit"])
    body = U.body(ui)
    if fp["at"] + 4 > len(body):
        return False
    target = U.st[ui] + fp["next"] + struct.unpack_from("<i", body, fp["at"])[0]
    if fp["kind"] == "code":
        k = U.at(target)
        return k >= 0 and k != ui and target - U.st[k] == fp["off"] \
            and unit_shows(U, k, units, fp["unit"])
    head = rodata_bytes(im, target, fp["len"])
    return head is not None and "%016X" % bd.fnv1a64(head) == fp["hash"]


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


def falls_through(md, x86, body, start):
    """True when the unit's bytes decode to the end and the last instruction
    does not end the flow (no ret, jmp, int3, ud2, hlt or int): execution
    runs on into whatever unit starts at the unit's end, which is a code
    relation and not a layout one."""
    stops = {x86.X86_INS_JMP, x86.X86_INS_RET, x86.X86_INS_RETF, x86.X86_INS_INT3,
             x86.X86_INS_UD2, x86.X86_INS_HLT, x86.X86_INS_INT, x86.X86_INS_IRET,
             x86.X86_INS_IRETQ}
    pos, n, last = 0, len(body), None
    while pos < n:
        end = pos
        for ins in md.disasm(body[pos:], start + pos):
            end = ins.address - start + ins.size
            last = ins
        if end == pos:
            return False  # an undecodable byte: no claim about the flow
        pos = end
    return last is not None and pos == n and last.id not in stops


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


def generate(exe_path, targets, use_relocs=True, log=lambda s: None, build=None):
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

    units, uid_of, unit_refs, clones_of = [], {}, {}, {}
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
        clones_of[uid_of[ui]] = sorted([ui] + same)
        return uid_of[ui]

    lo_iat, hi_iat = iat_range(im)

    def fingerprints(ui):
        """The clone fingerprints of unit ui (see fp_ok), those that tell it
        from at least one of its known clones first, at most MAX_FPS, or []
        when together they do not single it out among the known clones."""
        uid, start = unit_id(ui), U.st[ui]
        found = []
        for at, nxt, target in unit_refs[uid]:
            k = U.at(target)
            if k >= 0:
                if k != ui:
                    found.append({"kind": "code", "at": at, "next": nxt,
                                  "unit": unit_id(k), "off": target - U.st[k]})
                continue
            n = RODATA_FP_LEN
            for start_s, span, _raw, _rs, _fl in im.sections:
                if start_s <= target < start_s + span:
                    n = min(n, start_s + span - target)
                    break
            k = bisect.bisect_left(reloc_rvas, target - 7)
            while k < len(reloc_rvas) and reloc_rvas[k] < target + n:
                r = reloc_rvas[k]
                if r + reloc_width[r] > target:
                    n = r - target  # stop before the first relocated byte
                    break
                k += 1
            if n < 4 or (lo_iat < hi_iat and target < hi_iat and lo_iat < target + n):
                continue
            head = rodata_bytes(im, target, n)
            if head is not None:
                found.append({"kind": "rodata", "at": at, "next": nxt, "len": n,
                              "hash": "%016X" % bd.fnv1a64(head)})
        if ui + 1 < len(U) and U.st[ui + 1] == U.en[ui] and \
                falls_through(md, x86, U.body(ui), start):
            found.append({"kind": "fall", "unit": unit_id(ui + 1)})
        others = [c for c in clones_of[uid] if c != ui]
        telling = [fp for fp in found if any(not fp_ok(fp, im, U, c, units) for c in others)]
        chosen = (telling + [fp for fp in found if fp not in telling])[:MAX_FPS]
        if not telling or any(all(fp_ok(fp, im, U, c, units) for fp in chosen) for c in others):
            return []
        return chosen

    report = {"no_unit": [], "non_unique": [], "leaf": [], "undecoded": [],
              "code_no_ref": [], "code_orphan": [], "data_no_ref": [],
              "data_weak_ref": [], "data_orphan": [], "code_fp": [],
              "code_layout_only": []}
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
        fps = fingerprints(ui)
        if fps:
            entry["fp"] = fps
            report["code_fp"].append(entry["rva"])
        if not refs and not nbr and not fps:
            report["code_orphan"].append(entry["rva"])
        elif not refs:
            report["code_no_ref"].append(entry["rva"])
            if not fps:
                report["code_layout_only"].append(entry["rva"])
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

    if build is None:
        digits = re.findall(r"\d{5,}", Path(exe_path).stem)
        build = digits[-1] if digits else ""
    table = {"format": FORMAT, "hash": HASH_NAME,
             "known": {"timestamp": "0x%X" % stamp, "image_size": "0x%X" % image_size,
                       "build": build},
             "units": units, "targets": out_targets}
    return table, report


def table_text(table):
    return json.dumps(table, separators=(",", ":")) + "\n"


def table_hash(table):
    """FNV-1a-64 of the table's JSON text: names one table in the C++ table
    and in the rig's expectation files."""
    return bd.fnv1a64(table_text(table).encode("utf-8"))


FP_KINDS = {"code": 1, "rodata": 2, "fall": 3}      # game_build_core.h kFpCode...
GUARD_KINDS = {"str": 1, "code-ptr": 2}              # kGuardStr, kGuardCodePtr
REASONS = ("no-candidate", "ambiguous", "layout-only", "data-ref-failed")
CPP_TABLE_PATH = "src/common/game_signatures.inc"


def emit_cpp(table, generated_by):
    """The table as a C++ include for src/common/game_build_core.h's Table.

    Lengths, offsets, mask ranges, hashes and the known build's identity;
    never code bytes or strings (section names are the PE's own).  Every
    array has at least one element, the counts say how many are real."""
    known = table["known"]
    mask, unit_rows = [], []
    for u in table["units"]:
        unit_rows.append("    {%du, %du, %du, 0x%sull}," % (
            u["len"], len(mask) // 2, len(u["mask"]) // 2, u["hash"]))
        mask += u["mask"]
    sections, refs, nbrs, fps, rows = [], [], [], [], []

    def sec_index(name):
        if name is None:
            return 0xFF
        if name not in sections:
            sections.append(name)
        return sections.index(name)

    for t in table["targets"]:
        r0, n0, f0 = len(refs), len(nbrs), len(fps)
        refs += ["    {%du, %du, %du}," % (r["unit"], r["at"], r["next"]) for r in t.get("refs", ())]
        nbrs += ["    {%du, %d}," % (nb["unit"], nb["delta"]) for nb in t.get("nbr", ())]
        for f in t.get("fp", ()):
            fps.append("    {%d, %du, %du, %du, %du, %du, 0x%sull}," % (
                FP_KINDS[f["kind"]], f.get("at", 0), f.get("next", 0), f.get("unit", 0),
                f.get("off", 0), f.get("len", 0), f.get("hash", "0" * 16)))
        guard, ptee = t.get("guard") or {}, t.get("ptee") or {}
        if t["kind"] == "code":
            kind, sec, unit, off, pk, pi, po = 0, 0xFF, t["unit"], t["off"], 0, 0, 0
        else:
            kind, sec, unit, off = 1, sec_index(t.get("sec")), 0, 0
            pk, pi, po = (1, ptee["target"], 0) if "target" in ptee else \
                (2, ptee["unit"], ptee["off"]) if "unit" in ptee else (0, 0, 0)
        rows.append("    {0x%Xu, %d, %d, %d, %d, %du, %du, %du, %du, %du, %du, %du, %du, %du, %du, "
                    "%du, 0x%sull}," % (
                        t["rva"], kind, sec, GUARD_KINDS.get(guard.get("kind"), 0), pk, unit, off,
                        r0, len(refs) - r0, n0, len(nbrs) - n0, f0, len(fps) - f0, pi, po,
                        guard.get("len", 0), guard.get("hash", "0" * 16)))

    def array(ctype, name, items, empty):
        return ["constexpr %s %s[] = {" % (ctype, name)] + (items or ["    %s," % empty]) + ["};"]

    mask_rows = ["    " + " ".join("%du," % v for v in mask[k:k + 16])
                 for k in range(0, len(mask), 16)]
    out = [
        "// GENERATED by tools/hook_signatures.py; do not edit. The signature table of the game build EDVR",
        "// binds (issue 124, docs/design-game-update-signatures-2026-10-25.md): build %s, PE stamp %s,"
        % (known.get("build") or "unnamed", known["timestamp"]),
        "// SizeOfImage %s. Lengths, offsets, mask ranges and hashes only: no code bytes, no strings."
        % known["image_size"],
        "// Regenerate after every retarget, from the new known executable:",
        "//   " + generated_by,
        "// Included inside a namespace that sees src/common/game_build_core.h's types.",
        "constexpr uint32_t kFormat = %du;" % CPP_FORMAT,
        "constexpr uint32_t kKnownStamp = %su;" % known["timestamp"],
        "constexpr uint32_t kKnownImageSize = %su;" % known["image_size"],
        "constexpr char kKnownBuild[] = \"%s\";" % (known.get("build") or ""),
        "constexpr uint64_t kTableHash = 0x%016Xull;" % table_hash(table),
        "// {length, first mask pair, mask pairs, masked FNV-1a-64}",
    ] + array("Unit", "kUnits", unit_rows, "{0u, 0u, 0u, 0ull}") + [
        "// Masked ranges inside the units: offset, length.",
    ] + array("uint32_t", "kMask", mask_rows, "0u") + [
        "// {known RVA, kind, section, guard, pointee kind, unit, offset, refs first/count, neighbours",
        "//  first/count, fingerprints first/count, pointee index, pointee offset, string length, string hash}",
    ] + array("Target", "kTargets", rows, "{}") + [
        "// {unit, field offset, next-instruction offset}",
    ] + array("Ref", "kRefs", refs, "{0u, 0u, 0u}") + [
        "// {unit, units away}",
    ] + array("Nbr", "kNbrs", nbrs, "{0u, 0}") + [
        "// {kind, field offset, next-instruction offset, unit, offset in it, bytes, hash}",
    ] + array("Fp", "kFps", fps, "{0, 0u, 0u, 0u, 0u, 0u, 0ull}") + \
        array("Section", "kSections", ["    {\"%s\"}," % s for s in sections], "{\"\"}") + [
        "constexpr Table kTable = {kFormat, kKnownStamp, kKnownImageSize, kKnownBuild, kTableHash,",
        "    kUnits, %du, kMask, %du, kTargets, %du, kRefs, %du, kNbrs, %du, kFps, %du, kSections, %du};"
        % (len(unit_rows), len(mask), len(rows), len(refs), len(nbrs), len(fps), len(sections)),
    ]
    return "\n".join(out) + "\n"


def exe_identity(path):
    im = Image(Path(path).read_bytes())
    return bd.read_identity(im)


def expect_text(table, identity, rows, label):
    """The rig's expectation file for one exe: the known build and table it
    was made with, the exe's identity, then per target the ground truth (or
    -) and this matcher's answer, via and reason (or -), then per unit probe
    (unit_probes) the address unit_rva gives it (or -)."""
    known = table["known"]
    out = ["# hook_signatures expectations (%s): generated by tools/hook_signatures.py; do not edit." % label,
           "known %s %s" % (known["timestamp"], known["image_size"]),
           "exe 0x%X 0x%X" % identity,
           "table %016X" % table_hash(table)]
    for r in rows:
        want = r.get("want")
        out.append("r 0x%X %s %s %s %s %s" % (
            r["rva"], r["kind"], "0x%X" % want if want is not None else "-",
            "0x%X" % r["new"] if r["new"] is not None else "-", r["via"] or "-",
            r["reason"] or "-"))
    for p in unit_probes(table):
        new = unit_rva(table, rows, p)
        out.append("u 0x%X %s" % (p, "0x%X" % new if new is not None else "-"))
    return "\n".join(out) + "\n"


def write_outputs(outputs, dry_run, say=print):
    """Write {path: text}; with dry_run write nothing at all and say so."""
    for path, data in outputs.items():
        size = len(data) if isinstance(data, bytes) else len(data.encode("utf-8"))
        if dry_run:
            say("dry run: would write %d bytes to %s; nothing written" % (size, path))
            continue
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(data, bytes):
            path.write_bytes(data)
        else:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(data)
        say("wrote %s" % path)


def fixture_outputs(out_dir):
    """{path: bytes or text} for the C++ rig: the self-test's three exes,
    their table as C++, Python's answers for each and the fixture's names."""
    out_dir = Path(out_dir)
    known, K = _fixture(False)
    unknown, N = _fixture(True)
    broken, B = _fixture(True, break_data=True)
    outputs = {out_dir / "known.exe": known, out_dir / "unknown.exe": unknown,
               out_dir / "broken.exe": broken}
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        paths = {}
        for name, data in (("known", known), ("unknown", unknown), ("broken", broken)):
            paths[name] = td / (name + ".exe")
            paths[name].write_bytes(data)
        table, _ = generate(paths["known"], _fixture_targets(K), build="fixture")
        outputs[out_dir / "fixture_signatures.inc"] = emit_cpp(
            table, "python tools/hook_signatures.py fixture --out-dir <dir> (the self-test's fixture)")
        for name in ("known", "unknown", "broken"):
            results, _ = resolve(table, paths[name])
            text = expect_text(table, exe_identity(paths[name]),
                               [dict(r, want=None) for r in results], "fixture " + name)
            outputs[out_dir / ("expect_%s.txt" % name)] = text.replace(
                "\nexe ", "\nfile %s.exe\nexe " % name, 1)
    names = ["# fixture names: name, RVA in the known, unknown and broken builds (- where it has none)"]
    for name in sorted(set(K) | set(N) | set(B)):
        names.append("name %s %s" % (name, " ".join(
            "0x%X" % S[name] if name in S else "-" for S in (K, N, B))))
    outputs[out_dir / "fixture_names.txt"] = "\n".join(names) + "\n"
    return outputs


# --------------------------------------------------------------------------
# resolve (the runtime matcher; standard library only)
# --------------------------------------------------------------------------

def resolve(table, exe_path, allow_layout=False):
    """Find every table target in exe_path.

    Returns (results, stats).  results has one dict per target: rva (known
    build), kind, new (RVA or None), via (hash; ref, fp, ptr, content, nbr or
    a combination), reason (None, no-candidate, ambiguous, layout-only,
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

    def fp_values(fps, found):
        """({the start of the one clone that shows every fingerprint}, why):
        none, or several, name nothing (and are not a disagreement)."""
        if not fps:
            return set(), []
        hits = [ui for ui in found
                if all(fp_ok(fp, im, U, ui, table["units"]) for fp in fps)]
        if len(hits) == 1:
            return {U.st[hits[0]]}, []
        return set(), ["%d of %d clones show the fingerprints" % (len(hits), len(found))]

    results = []
    for tg in table["targets"]:
        row = {"rva": tg["rva"], "kind": tg["kind"], "new": None, "via": None,
               "reason": None, "detail": ""}
        refs = tg.get("refs", ())
        if tg["kind"] == "code":
            state, found = match(tg["unit"])
            if state == "ok":
                row["new"], row["via"] = U.st[found[0]] + tg["off"], "hash"
            elif state == "ambiguous" and (refs or tg.get("nbr") or tg.get("fp")):
                # The clones are identical, so a second witness picks one;
                # every witness that answers must name one of the clones.
                clone_starts = {U.st[ui] for ui in found}
                answers, why, conflict = {}, [], False
                for name, (values, w) in (("ref", ref_values(refs)),
                                          ("nbr", neighbour_starts(tg.get("nbr", ()))),
                                          ("fp", fp_values(tg.get("fp"), found))):
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
# Addresses inside a matched unit (game_build_core.h unitRva, coverRange)
# --------------------------------------------------------------------------

def unit_at(table, results, rva):
    """(target index, known start, new start, length) of the found code unit
    that holds the known-build rva; None when no found unit holds it or two
    found targets of that unit disagree on where it is now."""
    have = None
    for i, (t, r) in enumerate(zip(table["targets"], results)):
        if t["kind"] != "code" or r["new"] is None or r["reason"]:
            continue
        start, now = t["rva"] - t["off"], r["new"] - t["off"]
        n = table["units"][t["unit"]]["len"]
        if not start <= rva < start + n:
            continue
        if have is not None and (have[1], have[2]) != (start, now):
            return None
        have = (i, start, now, n)
    return have


def unit_rva(table, results, rva):
    """The known-build rva in the build results were resolved for: a table
    target's own answer; otherwise, inside a found code target's unit, the
    same offset in the unit it was found as (a whole-unit match keeps every
    instruction's offset); None when neither."""
    for t, r in zip(table["targets"], results):
        if t["rva"] == rva:
            return r["new"]
    hit = unit_at(table, results, rva)
    return None if hit is None else hit[2] + rva - hit[1]


def pin_cover(table, results, rva, n):
    """The pieces [(target index, known rva, length)] of found code units
    that hold [rva, rva + n) whole, contiguous in the known build and moved
    together in this one; None when a byte is in no found unit or the units
    moved apart."""
    if n <= 0:
        return None
    pieces, at, shift = [], rva, None
    while at < rva + n:
        hit = unit_at(table, results, at)
        if hit is None or (shift is not None and hit[2] - hit[1] != shift):
            return None
        shift = hit[2] - hit[1]
        end = min(rva + n, hit[1] + hit[3])
        pieces.append((hit[0], at, end - at))
        at = end
    return pieces


def unit_probes(table):
    """Known-build RVAs that exercise unit_rva off the table: each code
    target's unit start, the byte after it, its last byte and its end (one
    past it, so only the next unit can answer), and the byte after each
    target; table RVAs are left out so only the in-unit path answers."""
    rvas, probes = {t["rva"] for t in table["targets"]}, set()
    for t in table["targets"]:
        if t["kind"] != "code":
            continue
        start = t["rva"] - t["off"]
        n = table["units"][t["unit"]]["len"]
        probes.update((start, start + 1, start + n - 1, start + n, t["rva"] + 1))
    return sorted(p for p in probes if p not in rvas)


# --------------------------------------------------------------------------
# check
# --------------------------------------------------------------------------

def read_expect(path, invert=False):
    """{known-build rva: (expected rva or None, status)} from a build_diff map
    --json file.  Rows map rva -> new_rva; with invert the rows run the other
    way (e.g. .agents/build-334925/map_all.json, 334473 -> 334925).  An
    expectation file (check --emit-expect) is read too: its ground truth
    column, so the file can be regenerated without the map."""
    out = {}
    text = Path(path).read_text(encoding="utf-8-sig")
    if text.startswith("# hook_signatures expectations"):
        for line in text.splitlines():
            words = line.split()
            if len(words) == 7 and words[0] == "r":
                out[int(words[1], 16)] = (int(words[3], 16) if words[3] != "-" else None,
                                          "expect-file" if words[3] != "-" else "expect-file-none")
        return out
    for row in json.loads(text):
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
        if path.name == Path(CPP_TABLE_PATH).name:
            continue  # The generated table spells every address; it is not a user of one.
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
    for rva, guess, new, via in changed:
        lines.append("  CHANGED 0x%X: map %s, matcher 0x%X (via %s)" % (
            rva, "0x%X" % guess if guess is not None else "none", new, via))
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

    Known text: G F1 F2 C1 C2 LEAF H P1 P2 Q1 Q2 S1 S2 J T R1 R2 K1 K2 Z1 Z2
    W1.  Unknown text: X G F1 F2 C1 C2 LEAF H P1 P2 Q1 Q2 S1 S2 J T R1 R2 K1
    K2 Z1 Z2 W1e W1c, everything shifted, data moved.  F1 changes only in
    fields the mask covers (EQUAL); F2's struct displacement changes
    (UNRESOLVED); C1 and C2 are identical clones that P1 and P2 call, and with
    break_data they swap places, so the caller and the neighbours name
    different clones; LEAF has no .pdata entry; W1 is edited in the unknown
    build and an exact copy of the old W1 sits beside it, so the matcher
    resolves W1 to the wrong function.  Data: D1 and D2 are read by F1 and
    H; D3 nothing refers to; D4 is a cell holding P1's address that nothing
    refers to; D5 a string read by S1; D6 a cell holding G's address read by
    S2; D7 a string nothing refers to; D8 a cell holding C1's address that
    nothing refers to; D9 and D10 are 16-byte constants K1 and K2 load.  Q1
    and Q2 are clones that nothing calls and nothing tells apart.  Clones
    that nothing calls but their fingerprints tell apart: R1 calls G and R2
    calls H; K1 loads D9 and K2 D10; Z1 and Z2 are nine-byte .pdata
    fragments that fall into different second fragments (Z1b, Z2b).  With
    break_data R1 and R2 swap places too (the neighbours then name R2), the
    unknown build's strings differ and its cells are null."""
    text_rva, data_rva = 0x1000, 0x3200 if new else 0x3000
    pdata_rva, reloc_rva = 0x4200 if new else 0x4000, 0x5000
    S = {"D1": data_rva + 0x10, "D2": data_rva + 0x40, "D3": data_rva + 0x80,
         "D4": data_rva + 0x50, "D5": data_rva + 0x100, "D6": data_rva + 0x120,
         "D7": data_rva + 0x140, "D8": data_rva + 0x160, "D9": data_rva + 0x180,
         "D10": data_rva + 0x190}
    tail = ["LEAF", "H", "P1", "P2", "Q1", "Q2", "S1", "S2", "J", "T"] \
        + (["R2", "R1"] if break_data else ["R1", "R2"]) + ["K1", "K2", "Z1", "Z2"]
    order = (["X"] if new else []) + ["G", "F1", "F2"] \
        + (["C2", "C1"] if break_data else ["C1", "C2"]) + tail \
        + (["W1e", "W1c"] if new else ["W1"])
    sizes = {"X": 0x20, "G": 0x30, "F1": 0x60, "F2": 0x30, "C1": 0x30, "C2": 0x30,
             "LEAF": 0x10, "H": 0x40, "P1": 0x20, "P2": 0x20, "Q1": 0x20, "Q2": 0x20,
             "S1": 0x20, "S2": 0x20, "J": 0x30, "T": 0x20, "W1": 0x30, "W1e": 0x30,
             "W1c": 0x30, "R1": 0x20, "R2": 0x20, "K1": 0x10, "K2": 0x10, "Z1": 0x20,
             "Z2": 0x20}
    splits = {"Z1": 9, "Z2": 9}  # Units of two .pdata fragments: the first ends at this offset.
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
    # Fingerprinted clones: the same bytes once masked, told apart by what
    # the masked field reaches (R, K) or by the fragment they fall into (Z).
    a["R1"].raw("4883EC28", "B944000000").call(S["G"]).raw("4883C428", "C3")
    a["R2"].raw("4883EC28", "B944000000").call(S["H"]).raw("4883C428", "C3")
    a["K1"].rip("0F2805", S["D9"]).raw("0F1102", "C3")
    a["K2"].rip("0F2805", S["D10"]).raw("0F1102", "C3")
    for name, imm in (("Z1", 1), ("Z2", 2)):
        a[name].raw("4053", "4883EC20", "4885D2", "B8%02X000000" % imm, "4883C420", "5B", "C3")
    funcs = [n for n in order if n != "LEAF"]
    entries = []
    for name in funcs:
        cut = splits.get(name)
        ends = [cut, len(a[name].buf)] if cut else [len(a[name].buf)]
        begin = S[name]
        for end in ends:
            entries.append((begin, S[name] + end))
            begin = S[name] + end
    S["Z1b"], S["Z2b"] = S["Z1"] + splits["Z1"], S["Z2"] + splits["Z2"]
    raw_text, raw_data = 0x400, 0x400 + text_size
    raw_pdata, raw_reloc = raw_data + 0x200, raw_data + 0x200 + 12 * len(entries)
    exe = bytearray(bd._fixture_pe(
        text_size=text_size, extra_sections=[
            (".rdata", data_rva, 0x200, raw_data, 0x40000040),
            (".pdata", pdata_rva, 12 * len(entries), raw_pdata, 0x40000040),
            (".reloc", reloc_rva, 0x40, raw_reloc, 0x42000040)],
        exception_dir=(pdata_rva, 12 * len(entries))))
    exe[raw_text:raw_text + text_size] = b"\xcc" * text_size
    for name in order:
        off = raw_text + S[name] - text_rva
        exe[off:off + len(a[name].buf)] = a[name].buf
        assert len(a[name].buf) <= sizes[name], name
    for k, (begin, end) in enumerate(entries):
        exe[raw_pdata + 12 * k:raw_pdata + 12 * k + 12] = struct.pack(
            "<III", begin, end, pdata_rva)
    for cell, fill in (("D9", 0x01), ("D10", 0x11)):
        at = raw_data + S[cell] - data_rva
        exe[at:at + 16] = bytes(range(fill, fill + 16))
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


def _fixture_targets(K):
    """The self-test's targets in the known fixture: [(rva, kind)]."""
    return [(K["F1"], "code"), (K["F1+lea"], "code"), (K["F2"], "code"),
            (K["C1"], "code"), (K["LEAF"], "code"), (K["G"], "code"),
            (K["W1"], "code"), (K["D1"], "data"), (K["D2"], "data"),
            (K["D3"], "data"), (K["D4"], "data"), (K["D5"], "data"),
            (K["D6"], "data"), (K["D7"], "data"), (K["D8"], "data"),
            (K["Q1"], "code"), (K["J"], "code"), (K["T"], "code"),
            (K["R1"], "code"), (K["K1"], "code"), (K["Z1"], "code"),
            (K["D1"] + 0x1000, "code")]


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
    broken, B = _fixture(True, break_data=True)
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        kp, np_, bp = td / "known.exe", td / "unknown.exe", td / "broken.exe"
        kp.write_bytes(known)
        np_.write_bytes(unknown)
        bp.write_bytes(broken)
        targets = _fixture_targets(K)
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
        check("fp" not in by_rva[K["Q1"]] and report["code_layout_only"] == [K["Q1"]],
              "clones nothing tells apart get no fingerprint")
        fps = {n: by_rva[K[n]].get("fp", []) for n in ("R1", "K1", "Z1")}
        check([f["kind"] for f in fps["R1"]] == ["code"] and fps["R1"][0]["off"] == 0
              and fps["R1"][0]["at"] == 10, "a clone's callee is its fingerprint: %s" % fps["R1"])
        check([f["kind"] for f in fps["K1"]] == ["rodata"] and fps["K1"][0]["len"] == 16,
              "a clone's read-only constant is its fingerprint: %s" % fps["K1"])
        check([f["kind"] for f in fps["Z1"]] == ["fall"]
              and u[fps["Z1"][0]["unit"]]["rva"] == K["Z1b"],
              "a fragment that falls into the next is fingerprinted by it: %s" % fps["Z1"])
        check(set(report["code_fp"]) == {K["R1"], K["K1"], K["Z1"]}, "fingerprinted clones reported")
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
        for name in ("R1", "K1", "Z1"):
            check(got[K[name]]["new"] == N[name] and got[K[name]]["via"] == "fp+nbr",
                  "uncalled clone %s told apart by its fingerprints: %s" % (name, got[K[name]]))
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
        # Addresses inside a matched unit, off the table: the same offset in the unit found.
        f1_len = f1["len"]
        check(unit_rva(table, res, K["F1"] + 3) == N["F1"] + 3, "an RVA inside a found unit keeps its offset")
        check(unit_rva(table, res, K["F1"] + 9) == N["F1"] + 9, "a table RVA inside the unit gives its own answer")
        check(unit_rva(table, res, K["F2"] + 1) is None, "an RVA inside a unit not found has no answer")
        check(unit_rva(table, res, 0x10) is None, "an RVA in no unit has no answer")
        check([p[1:] for p in pin_cover(table, res, K["F1"], f1_len) or ()] == [(K["F1"], f1_len)],
              "a pin in one unit is one piece")
        check(pin_cover(table, res, K["F2"], 4) is None, "a pin in a unit not found is not covered")
        check("u 0x%X 0x%X" % (K["F1"] + 1, N["F1"] + 1) in
              expect_text(table, (0, 0), [dict(r, want=None) for r in res], "t").splitlines(),
              "the expectation file lists the unit probes with their answers")
        syn = {"units": [{"len": 16}, {"len": 8}],
               "targets": [{"rva": 0x1000, "kind": "code", "unit": 0, "off": 0},
                           {"rva": 0x1010, "kind": "code", "unit": 1, "off": 0},
                           {"rva": 0x1004, "kind": "code", "unit": 0, "off": 4}]}
        together = [{"new": 0x2000, "reason": None}, {"new": 0x2010, "reason": None}, {"new": 0x2004, "reason": None}]
        apart = [{"new": 0x2000, "reason": None}, {"new": 0x3000, "reason": None}, {"new": 0x2004, "reason": None}]
        split = [{"new": 0x2000, "reason": None}, {"new": 0x2010, "reason": None}, {"new": 0x5004, "reason": None}]
        check([p[1:] for p in pin_cover(syn, together, 0x1008, 0x10) or ()] == [(0x1008, 8), (0x1010, 8)],
              "a range over two fragments that moved together is two pieces")
        check(pin_cover(syn, apart, 0x1008, 0x10) is None, "fragments that moved apart do not cover a range")
        check(pin_cover(syn, together, 0x1008, 0x11) is None, "a range past the last unit is not covered")
        check(unit_rva(syn, together, 0x1014) == 0x2014 and unit_rva(syn, apart, 0x1014) == 0x3004,
              "the second fragment's RVAs follow it")
        check(unit_rva(syn, split, 0x1002) is None, "targets of one unit that disagree give no answer")
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
        check(got_b[K["R1"]]["reason"] == "ambiguous" and got_b[K["K1"]]["new"] == B["K1"],
              "fingerprints that disagree with the neighbours are refused")

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

        # The C++ table: what the runtime reads, and nothing the game's code is.
        cpp = emit_cpp(table, "test")
        check("kTable = {" in cpp and "0x%016Xull" % table_hash(table) in cpp
              and "constexpr Fp kFps[] = {" in cpp, "C++ table emitted")
        check(not re.search(r"[0-9A-Fa-f]{17,}", cpp), "C++ table holds no raw bytes")
        check(all(s.decode() not in cpp for s in (_FX_STRING, _FX_ORPHAN)),
              "C++ table holds no string content")
        check(all(kunits.body(i).hex()[:16] not in cpp.lower()
                  for i in range(len(kunits)) if len(kunits.body(i)) >= 8),
              "C++ table leaks code bytes")
        check(cpp.count("\n    {0x") == len(table["targets"]), "one C++ row per target")
        inc = td / "inc" / "game_signatures.inc"
        before = sorted(p.name for p in td.rglob("*"))
        check(quiet(argv + ["--emit-cpp", str(inc), "--dry-run"]) == 0
              and not inc.parent.exists() and sorted(p.name for p in td.rglob("*")) == before,
              "generate --emit-cpp --dry-run wrote something")
        check(quiet(argv + ["--emit-cpp", str(inc)]) == 0 and inc.is_file()
              and "kTable" in inc.read_text(encoding="utf-8"), "generate --emit-cpp writes the include")

        # The rig's inputs: fixture and check --emit-expect, both dry-run clean.
        fx_dir = td / "fx"
        before = sorted(p.name for p in td.rglob("*"))
        check(quiet(["fixture", "--out-dir", str(fx_dir), "--dry-run"]) == 0
              and not fx_dir.exists() and sorted(p.name for p in td.rglob("*")) == before,
              "fixture --dry-run wrote something")
        check(quiet(["fixture", "--out-dir", str(fx_dir)]) == 0
              and all((fx_dir / n).is_file() for n in (
                  "known.exe", "unknown.exe", "broken.exe", "fixture_signatures.inc",
                  "expect_known.txt", "expect_unknown.txt", "expect_broken.txt",
                  "fixture_names.txt")), "fixture writes the rig's files")
        fx_unknown = (fx_dir / "expect_unknown.txt").read_text(encoding="utf-8")
        check("r 0x%X code - 0x%X fp+nbr -" % (K["R1"], N["R1"]) in fx_unknown
              and "file unknown.exe" in fx_unknown, "fixture expectations carry the answers")
        exp = td / "exp" / "expect.txt"
        chk_argv = ["check", "--table", str(out), "--exe", str(np_), "--expect",
                    str(td / "truth.json"), "--src", str(td / "nosrc"), "--emit-expect", str(exp)]
        before = sorted(p.name for p in td.rglob("*"))
        quiet(chk_argv + ["--dry-run"])
        check(not exp.parent.exists() and sorted(p.name for p in td.rglob("*")) == before,
              "check --emit-expect --dry-run wrote something")
        quiet(chk_argv)
        back = read_expect(exp)
        check(exp.is_file() and back[K["F1"]] == (N["F1"], "expect-file")
              and back[K["F2"]][0] is None, "an expectation file is read back as a ground truth")

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

    # audit: synthetic sources against a two-target table.
    with tempfile.TemporaryDirectory() as td:
        s = Path(td)
        (s / "d3d11").mkdir()
        tab = {"stamp": 0x6ADA8048, "size": 0x640C000,
               "targets": [(0x4314C60, "code", 0x4314C60, 0x4314C9B), (0x5189070, "data", 0x5189070, 0x5189071)]}

        def audited(name, text):
            (s / "d3d11" / name).write_text(text, encoding="utf-8")
            try:
                return audit(s, tab)[0]
            finally:
                (s / "d3d11" / name).unlink()

        good = ('constexpr uintptr_t kStoreRva = 0x4314C60u;  // base + 0x4314C60 in a comment\n'
                'const char* kText = "base + 0x4314C60";\n'
                'struct E { const char* name; uintptr_t rva; };\n'
                'E g_entry{"store", 0x4314C60u};\n'
                'constexpr uintptr_t kVtables[2] = {0x5189070u, 0x4314C64u};\n'
                'void f(uintptr_t base, E& e, uintptr_t v) {\n'
                '    if (!gameRvasFound(kVtables, 2, nullptr)) return;\n'
                '    const uintptr_t at = gameRva(static_cast<uint32_t>(kStoreRva));\n'
                '    use(base + at, base + gameRva(0x4314C65) + 5, base + gameRva(uint32_t(e.rva)));\n'
                '    if (v == base + gameRva(static_cast<uint32_t>(kVtables[0]))) Log::get().note("%x", kStoreRva);\n'
                '    static_assert(sizeof(kVtables) == 16, "two");\n'
                '}\n')
        check(audited("good.cpp", good) == [], "audit: resolved uses pass: %s" % audited("good.cpp", good))
        for name, text, rule, what in (
                ("b1.cpp", "void f(uintptr_t b){ use(b + 0x4314C60); }", "routed", "base + a literal"),
                ("b2.cpp", "constexpr uintptr_t kRva = 0x4314C60;\nvoid f(uintptr_t b){ use(b + kRva); }",
                 "routed", "base + a constant"),
                ("b3.cpp", "struct E{uintptr_t rva;}; void f(uintptr_t b, E& e){ gameRva(0x4314C60); use(b + e.rva); }",
                 "routed", "base + entry.rva"),
                ("b4.cpp", "constexpr uintptr_t kTable[] = {0x4314C60, 0x4314C61};\n"
                           "bool f(uintptr_t r){ for (uintptr_t k : kTable) if (r == k) return true; return false; }",
                 "routed", "a table of addresses walked raw"),
                ("b5.cpp", "bool f(uint32_t r){ switch(r){ case 0x4314C60: return true; } return false; }",
                 "routed", "a case label"),
                ("b6.cpp", "constexpr uintptr_t kRva = 0x4315000;", "known", "an address the table does not hold"),
                ("b7.cpp", "constexpr uintptr_t kRva = 0x144314C60ull;", "known", "a VA"),
                ("b8.cpp", "constexpr uint32_t kStamp = 1792704584u;", "identity", "the stamp"),
                ("b9.cpp", "bool f(uint32_t s){ return s == 0x640C000u; }", "identity", "the image size")):
            got = audited(name, text)
            check(any((": %s: " % rule) in p for p in got), "audit: %s fails as %s: %s" % (what, rule, got))
        check(audited("ok.cpp", "constexpr uint32_t kMask = 0xFFFFFFu; int n = 60000000;") == [],
              "audit: masks and decimal counts are not addresses")
        (s / "d3d11" / "native_on_foot_core.h").write_text(
            "namespace ecm {\nconstexpr uint32_t kExpectedTimestamp = 0x6ADA8048u;\n"
            "constexpr uintptr_t kFooRva = 0x4314C60u;\n}\n"
            "inline uintptr_t foo(uintptr_t b) { return b + kFooRva; }\n", encoding="utf-8")
        check(audit(s, tab)[0] == [], "audit: an exact-only file may key on the build and add the base")
        check(any(": identity: " in p for p in audited("c1.cpp", "bool f(uint32_t s){ return s == ecm::kExpectedTimestamp; }")),
              "audit: a name for the identity fails outside the exact-only files")
        check(any(": routed: " in p for p in audited("c2.cpp", "uintptr_t f(uintptr_t b){ return b + ecm::kFooRva; }")),
              "audit: an exact-only file's address used raw elsewhere fails")
        check(audited("c3.cpp", "uintptr_t f(uintptr_t b){ if (!gameRva(ecm::kFooRva)) return 0; "
                                "return b + gameRva(ecm::kFooRva); }") == [],
              "audit: an exact-only file's address resolved elsewhere passes")
        for name in ("native_on_foot.cpp", "native_on_foot_hooks.cpp", "avatar_hooks_core.h", "avatar_skeleton_core.h",
                     "native_on_foot_f87.h", "native_on_foot_f111.h", "native_on_foot_f114b.h", "native_on_foot_f116.h",
                     "native_on_foot_f154.h"):
            check(audited(name, "bool f(uint32_t stamp){ return stamp == 0x6ADA8048u; }\n"
                                "uintptr_t g(uintptr_t b){ return b + 0x4315000u; }") == [],
                  "audit: the exact-build native file %s retains its identity and raw addresses" % name)
        for name in ("native_on_foot_future.h", "native_on_foot_f999.h", "avatar_hooks_future.h", "explorer_cam_future.h"):
            check(any(": routed: " in p for p in audited(name, "uintptr_t f(uintptr_t b){ return b + 0x4314C60u; }")),
                  "audit: a new similarly named file %s is not exact-only" % name)
        check(audited("draw_record_tag.h", "constexpr uint32_t kSalt = 0x00A5C35Au;\n"
                                             "uint32_t f(uint32_t s){ return s ^ kSalt; }") == [],
              "audit: the draw-record sentinel salt is not an address")
        check(any(": known: " in p for p in audited("draw_record_tag.h", "constexpr uint32_t kOther = 0x00A5C35Bu;")),
              "audit: the salt exception does not exempt another value in the same header")
        check(any(": known: " in p for p in audited("draw_record_other.h", "constexpr uint32_t kSalt = 0x00A5C35Au;")),
              "audit: the salt exception does not exempt its value in another file")
        # The review's blind spots (2026-10-25 round 2), each a shape that passed the gate before: every one fails now.
        gmh = "reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))"
        for name, text, rule, what in (
                ("r_a.cpp", "#include <cstdint>\nstruct Site { uint32_t call; uint8_t b[5]; };\n"
                            "constexpr Site kSites[] = {{0x4314C60u,{0}}};\n"
                            "uintptr_t f(uintptr_t base){ return base + kSites[0].call; }\n",
                 "routed", "(a) a struct member not named *rva holding an address"),
                ("r_b.cpp", "constexpr UINT64 kHook = 0x4314C60;\nuintptr_t g(uintptr_t b){ return b + kHook; }\n",
                 "routed", "(b) a constant of a type the old list did not name"),
                ("r_b2.cpp", "#include <array>\nconstexpr std::array<uint32_t,2> kA = {0x4314C60u, 0x4314C61u};\n"
                             "uintptr_t j(uintptr_t b){ return b + kA[0]; }\n",
                 "routed", "(b) a std::array of addresses"),
                ("r_c.cpp", "uintptr_t i(){ const uintptr_t b = %s; return b + 0x8A3C0; }\n" % gmh,
                 "base", "(c) an RVA below 0x100000 added to the module base"),
                ("r_c2.cpp", "constexpr uint32_t kLow = 0x8A3C0u;\nuintptr_t i2(uintptr_t b){ return b + kLow; }\n"
                             "uintptr_t i3(){ return i2(%s); }\n" % gmh,
                 "base", "(c) a low RVA added to a parameter its caller passes the module base"),
                ("r_d.cpp", "uintptr_t h(uintptr_t b){ return b + 70339680; }\n",
                 "known", "(d) a game address written in decimal"),
                ("r_d2.cpp", "uintptr_t h2(){ const uintptr_t b = %s; return b + 70340000; }\n" % gmh,
                 "base", "(d) a decimal offset added to the module base"),
                ("r_e.cpp", "uintptr_t gameRva(uint32_t);\nvoid patch(uintptr_t);\n"
                            "void k(uintptr_t base){ const uintptr_t at = gameRva(0x4314C60u); patch(base + at); }\n",
                 "zero", "(e) a gameRva result used in arithmetic with no zero test"),
                ("r_e2.cpp", "void k2(){ const uintptr_t b = %s; patch(b + gameRva(0x4314C60u)); }\n" % gmh,
                 "zero", "(e) a gameRva call added to the module base with no guard"),
                ("r_e3.cpp", "uintptr_t k3(bool known){ const uintptr_t at = known ? gameRva(0x4314C60u) : 0; "
                             "return at + 5; }\n",
                 "zero", "(e) a conditional gameRva result used with no zero test"),
                ("r_f.cpp", "struct G { uintptr_t gameBase; } g;\n"
                            "void n(){ g.gameBase = %s; }\nuintptr_t o(uint32_t off){ return g.gameBase + off; }\n" % gmh,
                 "base", "a member holding the module base, plus a value the audit cannot place"),
                ("r_g.cpp", "constexpr uint32_t kHookRva = 0x4314C60u;\nuintptr_t m(uintptr_t b){ return b + kHookRva; }\n",
                 "routed", "the control: a named constant added raw")):
            got = audited(name, text)
            check(any((": %s: " % rule) in p for p in got), "audit: %s fails as %s: %s" % (what, rule, got))
        good2 = ("struct E2 { uint32_t call; uint8_t b[5]; };\n"
                 "constexpr E2 kE[] = {{0x4314C60u, {0}}};\n"
                 "uintptr_t p(bool known) {\n"
                 "    const uintptr_t base = %s;\n"
                 "    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);\n"
                 "    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);\n"
                 "    const uintptr_t at = known ? gameRva(0x4314C60u) : 0;\n"
                 "    if (at == 0) return 0;\n"
                 "    const uintptr_t site = gameRva(kE[0].call);\n"
                 "    if (!site) return 0;\n"
                 "    if (base + gameRva(0x4314C61u) == base + at) return 1;\n"
                 "    Log::get().note(\"%%x\", kE[0].call);\n"
                 "    return use(base + at + 5, base + site, nt, base + nt->OptionalHeader.SizeOfImage);\n"
                 "}\n"
                 "uintptr_t q(uintptr_t addr) {\n"
                 "    static const uint32_t kAll[1] = {0x4314C60u};\n"
                 "    if (!gameRvasFound(kAll, 1, nullptr)) return 0;\n"
                 "    const uintptr_t base = %s;\n"
                 "    return addr < base + 0x1000 ? 0 : base + gameRva(0x4314C60u);\n"
                 "}\n") % (gmh, gmh)
        check(audited("good3.cpp", "#include <array>\nconstexpr std::array<uint32_t,2> kA2 = {0x4314C60u, 0x4314C61u};\n") == [],
              "audit: a std::array of addresses may be declared")
        check(audited("good2.cpp", good2) == [],
              "audit: PE fields, a tested conditional result, a member resolved, a comparison and a bound pass: %s"
              % audited("good2.cpp", good2))
        check("inside the table unit" in where_text(tab, 0x4314C65) and "NOT PLACED" in where_text(tab, 0x4315000)
              and "lies in 1 table unit" in where_text(tab, 0x4314C60, 16)
              and "NOT covered" in where_text(tab, 0x4314C60, 0x40), "where: placement and cover")
        check(table_from_cpp(Path(__file__).resolve().parent.parent.joinpath(CPP_TABLE_PATH).read_text(
            encoding="utf-8"))["targets"], "audit: the committed C++ table parses")

    print("hook_signatures self-test: %s" % ("FAILED (%d)" % len(failures) if failures else "ok"))
    return 1 if failures else 0


# --------------------------------------------------------------------------
# audit (a build.bat gate): every game address in src goes through the
# resolver (src/common/game_build.h)
# --------------------------------------------------------------------------

RESOLVER_CALLS = {"gameRva", "gameRvasFound", "gamePinMatches", "gamePinProvenByMatch"}
# The resolver and its generated table spell the addresses and the identity on purpose.
AUDIT_RESOLVER = ("common/game_build.h", "common/game_build.cpp", "common/game_build_core.h",
                  "common/game_signatures.inc")
# Exact-build only, by the maintainer's decision (docs/design-game-update-signatures-2026-10-25.md,
# step 3). They may key on the identity and add the module base to their own constants.
AUDIT_EXACT_ONLY = (
    ("common/elite_oculus_profile.h", "the Oculus route profile stays exact-build only"),
    ("d3d11/oculus_route.cpp", "the Oculus route, which reads that profile"),
    # Explorer Cam was deleted; its avatar hooks now belong to native on foot. These existing files keep their own identity gates and unwitnessed struct layouts,
    # so resolving their addresses alone would not make them safe on an unknown build. New files in this family remain audited unless reviewed individually.
    ("d3d11/native_on_foot.cpp", "native on foot's exact-build hook installers and struct reads"),
    ("d3d11/native_on_foot_hooks.cpp", "the identity-gated avatar hooks extracted from Explorer Cam"),
    ("d3d11/native_on_foot_core.h", "native on foot's exact-build addresses and identity"),
    ("d3d11/avatar_hooks_core.h", "the extracted avatar hooks' exact-build addresses and identity"),
    ("d3d11/avatar_skeleton_core.h", "the extracted avatar skeleton's exact-build vtables and slots"),
    ("d3d11/native_on_foot_f87.h", "the exact-build interaction prompt probe's addresses and pins"),
    ("d3d11/native_on_foot_f111.h", "the exact-build panel pointer probe's functions and slots"),
    ("d3d11/native_on_foot_f114b.h", "the exact-build panel pointer's owner transform and click path"),
    ("d3d11/native_on_foot_f116.h", "the exact-build HUD submit gate's addresses and pins"),
    ("d3d11/native_on_foot_f154.h", "the exact-build screen-space owner's vtable slot and the stolen size of its hook"),
    ("d3d11/mem_tick.cpp", "a diagnostic whose addresses are not in the table"),
    ("d3d11/terrain_cull_cover*", "terrain cull cover, which finds its site by its own scan on an unknown build"),
)
# Literals in the image's range that are not game addresses: (path glob, values or None for all, why).
AUDIT_NOT_ADDRESSES = (
    ("*", (0x100000, 0x1FFFFF, 0x7FFFFE, 0x7FFFFF, 0x800000, 0xFFFFFF, 0x1000193),
     "bit masks, a 1 MiB bound and the FNV-1a-32 prime"),
    ("d3d11/dxbc_*.h", None, "DXBC shader tokens"),
    ("d3d11/*_shader*.h", None, "shader tokens"),
    ("d3d11/fsr/*", None, "the FidelityFX headers"),
    ("d3d11/orbital_width_patch.h", None, "DXBC shader tokens"),
    ("d3d11/ui_holo_remap.h", None, "DXBC shader tokens"),
    ("installer/logbundle.cpp", (0x4034B50, 0x2014B50, 0x6054B50), "ZIP record signatures"),
    ("d3d11/perf_monitor.cpp", (0x150E828,), "an NVAPI function id"),
    ("d3d11/kinematic_eval_probe.cpp", (0x4301234, 0x4305678), "the probe self-test's made-up vtable RVAs"),
    ("d3d11/draw_record_tag.h", (0x00A5C35A,), "the draw-record sentinel's XOR salt"),
    ("d3d11/flat_stationary_camera_capture.cpp", (0x6000000,),
     "the known build's bound on the RVAs the capture logs (an unknown build uses its SizeOfImage)"),
    ("d3d11/scheduler_stack_probe.h", (0x4DB6000,),
     "the known build's end of .text, a stack-scan bound (an unknown build reads its own from the PE header)"),
)
_INT_TYPES = {"uintptr_t", "intptr_t", "uint64_t", "int64_t", "uint32_t", "int32_t", "size_t",
              "unsigned", "int", "long", "DWORD", "DWORD64", "ULONG_PTR", "UINT_PTR", "auto"}
_CASTS = {"static_cast", "reinterpret_cast", "const_cast"}
_WHOLE_CALLS = {"sizeof", "alignof", "decltype", "static_assert", "_countof", "size", "extent"}
_LOG_CALLS = {"note", "printf", "fprintf", "snprintf", "sprintf_s", "_snprintf_s", "vsnprintf"}
_AUDIT_LEX = re.compile(r"""
    (?P<pp>^[ \t]*\#(?:[^\n\\]|\\.)*)
  | (?P<com>//[^\n]*|/\*.*?\*/)
  | (?P<str>R"(?P<d>[^(\s\\"]{0,16})\(.*?\)(?P=d)"|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*')
  | (?P<num>\.?\d(?:[eEpP][+-]|[\w'.])*)
  | (?P<id>[A-Za-z_]\w*)
  | (?P<op>->|::|<<=|>>=|<=|>=|==|!=|&&|\|\||[-+*/%&|^]=|<<|>>|\+\+|--|\S)
  | (?P<ws>\s+)
""", re.S | re.M | re.X)


def _num_value(text):
    t = text.replace("'", "")
    m = re.fullmatch(r"(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*", t)
    if not m:
        return None
    return int(m.group(1), 16 if m.group(1)[:2].lower() == "0x" else 10)


def _lex(text):
    """[(kind, text, line)] with comments, whitespace and preprocessor lines left out."""
    toks, line = [], 1
    for m in _AUDIT_LEX.finditer(text):
        kind = m.lastgroup if m.lastgroup != "d" else "str"
        if kind not in ("ws", "com", "pp"):
            toks.append((kind, m.group(0), line))
        line += m.group(0).count("\n")
    return toks


def _brackets(toks):
    """(match, parent): the matching bracket of each bracket token, and the
    innermost open bracket enclosing each token (-1 at top level)."""
    match, parent, stack = {}, [], []
    pairs = {")": "(", "]": "[", "}": "{"}
    for i, (_, t, _) in enumerate(toks):
        if t in pairs:
            while stack and toks[stack[-1]][1] != pairs[t]:
                stack.pop()
            if stack:
                j = stack.pop()
                match[i], match[j] = j, i
        parent.append(stack[-1] if stack else -1)
        if t in ("(", "[", "{"):
            stack.append(i)
    return match, parent


def table_from_cpp(text):
    """The parts of the emitted C++ table the audit needs: {stamp, size,
    targets: [(rva, kind, unit start, unit end)]}."""
    stamp = int(re.search(r"kKnownStamp = (0x[0-9A-Fa-f]+)u", text).group(1), 16)
    size = int(re.search(r"kKnownImageSize = (0x[0-9A-Fa-f]+)u", text).group(1), 16)
    units_text = text.split("kUnits[] = {", 1)[1].split("};", 1)[0]
    lens = [int(m.group(1)) for m in re.finditer(r"\{(\d+)u, \d+u, \d+u, 0x[0-9A-F]+ull\}", units_text)]
    targets_text = text.split("kTargets[] = {", 1)[1].split("};", 1)[0]
    targets = []
    for m in re.finditer(r"\{0x([0-9A-F]+)u, (\d), \d+, \d, \d, (\d+)u, (\d+)u", targets_text):
        rva, kind, unit, off = int(m.group(1), 16), int(m.group(2)), int(m.group(3)), int(m.group(4))
        if kind == 0:
            targets.append((rva, "code", rva - off, rva - off + lens[unit]))
        else:
            targets.append((rva, "data", rva, rva + 1))
    return {"stamp": stamp, "size": size, "targets": targets}


def table_for_audit(table):
    """The same parts from a table JSON (generate --out)."""
    out = []
    for t in table["targets"]:
        if t["kind"] == "code":
            start = t["rva"] - t["off"]
            out.append((t["rva"], "code", start, start + table["units"][t["unit"]]["len"]))
        else:
            out.append((t["rva"], "data", t["rva"], t["rva"] + 1))
    known = table["known"]
    return {"stamp": _int(known["timestamp"]), "size": _int(known["image_size"]), "targets": out}


_OPERATORS = {"+", "-", "*", "/", "%", "<", ">", "&", "|", "^", "?", ":", "!", "~", "==", "!=", "<=",
              ">=", "+=", "-=", "<<", ">>", "&&", "||", "++", "--", "case", "return"}


def _decl_name(toks, match, i):
    """For an initializer that starts at token i (an '=', or the '{' or '('
    of a direct initialization): (name, integral, is_decl), or None when the
    token before is not a name or is a member (x.rva = ..., whose member the
    audit tracks by its own name)."""
    j = i - 1
    while j >= 0 and toks[j][1] == "]" and j in match:
        j = match[j] - 1
    if j < 0 or toks[j][0] != "id" or (j >= 1 and toks[j - 1][1] in (".", "->")):
        return None
    k, type_toks = j - 1, []
    while k >= 0 and toks[k][1] not in (";", "{", "}", "(", ",", ")", "="):
        if toks[k][1] in (">", ">>"):   # a template argument list (std::array<uint32_t, 2>): its commas are the type's
            depth = 0
            while k >= 0 and toks[k][1] not in (";", "{", "}"):
                depth += {">": 1, ">>": 2, "<": -1}.get(toks[k][1], 0)
                type_toks.append(toks[k][1])
                k -= 1
                if depth <= 0:
                    break
            continue
        type_toks.append(toks[k][1])
        k -= 1
    is_decl = bool(type_toks) and type_toks[0] not in ("return", "else", "case", "::", "<<", ">>")
    return toks[j][1], any(t in _INT_TYPES for t in type_toks), is_decl, type_toks[::-1]


def _struct_members(toks, match, parent):
    """{struct or class name: [data member names in declaration order]} for
    every struct and class body in toks (member functions, nested types,
    static members and using/typedef lines left out)."""
    out = {}
    for a, (_, t, _) in enumerate(toks):
        if t != "{" or a not in match:
            continue
        k, head = a - 1, []
        while k >= 0 and toks[k][1] not in (";", "{", "}", ")"):
            head.append(toks[k][1])
            k -= 1
        head.reverse()
        if len(head) < 2 or head[0] not in ("struct", "class") or not re.fullmatch(r"[A-Za-z_]\w*", head[1]):
            continue
        members, cur = [], []

        def flush(stmt):
            words = [toks[j][1] for j in stmt]
            while len(words) >= 2 and words[1] == ":" and words[0] in ("public", "private", "protected"):
                words, stmt = words[2:], stmt[2:]
            if not words or words[0] in ("static", "using", "typedef", "friend", "enum", "struct", "class",
                                         "template", "union") or "(" in words or "operator" in words:
                return
            piece = []
            for j in stmt + [None]:
                if j is None or toks[j][1] == ",":
                    name = None
                    for q in piece:
                        if toks[q][1] in ("[", "=", "{", ":"):
                            break
                        if toks[q][0] == "id":
                            name = toks[q][1]
                    if name:
                        members.append(name)
                    piece = []
                else:
                    piece.append(j)

        for j in range(a + 1, match[a]):
            if parent[j] != a:
                continue
            if toks[j][1] == ";":
                flush(cur)
                cur = []
            elif toks[j][1] == "}" and (j + 1 >= len(toks) or toks[j + 1][1] not in (";", ",")):
                cur = []   # the end of a member function's body or a nested type: not a data member
            else:
                cur.append(j)
        out.setdefault(head[1], members)
    return out


def _at_namespace_scope(toks, match, parent, i):
    """True when every brace around token i opens a namespace, class, struct,
    union, enum or extern body (not a function body, a lambda or a block)."""
    a = parent[i]
    while a >= 0:
        if toks[a][1] == "{":
            k, head = a - 1, []
            while k >= 0 and toks[k][1] not in (";", "{", "}"):
                head.append(toks[k][1])
                k -= 1
            if not ({"namespace", "struct", "class", "union", "enum"} & set(head) or
                    (head and head[-1] == "extern")):
                return False
        a = parent[a]
    return True

def _span(toks, match, i):
    """The whole primary expression around token i: through qualification
    (a::b), a member chain (x[i].rva), subscripts, parentheses and integer
    casts (static_cast<uint32_t>(...), uintptr_t(...), (uint32_t)...)."""
    lo = hi = i
    while True:
        if lo >= 2 and toks[lo - 1][1] == "::" and toks[lo - 2][0] == "id":
            lo -= 2
        elif lo >= 2 and toks[lo - 1][1] in (".", "->"):
            b = lo - 2
            while b >= 0 and toks[b][1] in ("]", ")") and b in match:
                b = match[b] - 1
            lo = b if b >= 0 and toks[b][0] == "id" else b + 1
        else:
            break
    while hi + 1 < len(toks) and toks[hi + 1][1] == "[" and (hi + 1) in match:
        hi = match[hi + 1]
    return _widen(toks, match, lo, hi)


def _widen(toks, match, lo, hi):
    """[lo, hi] widened over the parentheses and integer or pointer casts
    around it (static_cast<T>(...), uintptr_t(...), (uint32_t)..., (...))."""
    while True:
        p, q = lo - 1, hi + 1
        if p >= 0 and q < len(toks) and toks[p][1] == "(" and toks[q][1] == ")" and match.get(p) == q:
            b = p - 1
            if b >= 0 and toks[b][1] == ">":
                depth, c = 0, b
                while c >= 0:
                    depth += {">": 1, "<": -1}.get(toks[c][1], 0)
                    if depth == 0:
                        break
                    c -= 1
                if c >= 1 and toks[c - 1][1] in _CASTS:
                    lo, hi = c - 1, q
                    continue
                break
            if b >= 0 and toks[b][0] == "id" and toks[b][1] in _INT_TYPES:
                lo, hi = b, q
                continue
            if b >= 0 and (toks[b][0] == "id" or toks[b][1] in (")", "]", ">")):
                break   # the argument of a call
            lo, hi = p, q
            continue
        if p >= 0 and toks[p][1] == ")" and p in match:
            inner = [t[1] for t in toks[match[p] + 1:p]]
            if inner and all(t in _INT_TYPES or t in ("const", "unsigned", "long", "::", "std") for t in inner):
                lo = match[p]
                continue
        break
    return lo, hi


def _context(toks, match, parent, i):
    """How the tracked token i is used: ('ok', how, declaration or None) or
    ('bad', why, None)."""
    lo, hi = _span(toks, match, i)
    prev = toks[lo - 1][1] if lo else ";"
    nxt = toks[hi + 1][1] if hi + 1 < len(toks) else ";"
    a = parent[lo]
    while a >= 0:
        if toks[a][1] == "(" and a >= 1 and toks[a - 1][1] in _WHOLE_CALLS:
            return "ok", "sizeof", None
        a = parent[a]
    if prev == "(" and lo >= 2 and toks[lo - 2][1] in RESOLVER_CALLS and nxt in (",", ")"):
        return "ok", "resolver", None
    if nxt == "=" and (prev not in _OPERATORS or prev in (">", ">>")):
        return "ok", "assigned", None   # (after '>': the name a template type declares, std::array<T, N> name = ...)
    if prev in ("(", ",") and nxt in (",", ")") and parent[lo] >= 1 and toks[parent[lo] - 1][1] in _LOG_CALLS:
        return "ok", "log", None
    if prev == "=" and nxt in (";", ","):
        d = _decl_name(toks, match, lo - 1)
        if d:
            return "ok", "init", d
    if prev in ("{", "(") and nxt in ("}", ")") and match.get(lo - 1) == hi + 1:
        d = _decl_name(toks, match, lo - 1)
        if d and d[2]:
            return "ok", "init", d
    if prev in ("{", ",") and nxt in (",", "}") and parent[lo] >= 0 and toks[parent[lo]][1] == "{":
        b = parent[lo]   # an element of a brace list: find what the outermost list initializes
        while parent[b] >= 0 and toks[parent[b]][1] == "{" and b >= 1 and toks[b - 1][1] in ("{", ","):
            b = parent[b]
        d = None
        if b >= 1 and toks[b - 1][1] == "=":
            d = _decl_name(toks, match, b - 1)
        elif b >= 1 and toks[b - 1][0] == "id":
            d = _decl_name(toks, match, b)
        if d and d[2]:
            return "ok", "table", d
    op = prev if prev in _OPERATORS else nxt if nxt in _OPERATORS else None
    what = " ".join(t[1] for t in toks[lo:hi + 1])
    if op:
        return "bad", "%s is an operand of '%s'" % (what, op), None
    return "bad", "%s is used as something other than a resolver argument, an initializer, a table " \
                  "entry or a log argument (after '%s')" % (what, prev), None


# The module base: what GetModuleHandle of the executable returns (nullptr, NULL or 0, or the game's
# own name), and every name, member, parameter and function the audit can follow it into.
_GMH_CALLS = {"GetModuleHandleW", "GetModuleHandleA", "GetModuleHandle"}
_GAME_MODULE_ARGS = {"nullptr", "NULL", "0"}
_RELATIONAL = {"<", "<=", ">", ">="}
_NOT_TYPES = {"return", "case", "else", "do", "throw", "new", "delete", "sizeof", "co_return", "goto"}
_KEYWORDS = _NOT_TYPES | {"if", "while", "for", "switch", "try", "catch", "const", "constexpr", "static", "operator"}
# Operands added to the module base that are not game addresses, each a part of the PE layout: (path glob,
# the operand's text, why). Anything else added to a module base must be a gameRva result.
AUDIT_BASE_OPERANDS = (
    ("d3d11/scheduler_stack_probe.cpp", "kRangeLo",
     "the start of .text: the low bound of the return addresses a stack scan accepts (a range, never read)"),
    ("d3d11/scheduler_stack_probe.cpp", "rangeHi_",
     "the end of .text (the known build's kRangeHi, an unknown build's from its PE header): the scan's high bound"),
)


def _brace_is_scope(toks, a):
    """True when the brace at a opens a namespace, class, struct, union,
    enum or extern body."""
    k, head = a - 1, []
    while k >= 0 and toks[k][1] not in (";", "{", "}"):
        head.append(toks[k][1])
        k -= 1
    return bool({"namespace", "struct", "class", "union", "enum"} & set(head)) or \
        bool(head and head[-1] == "extern")


def _primary(toks, match, s):
    """(end, core) of the primary expression that starts at token s: casts
    and parentheses taken off, then a name with its qualification, member
    chain, subscripts and calls, or a literal.  end is its last token, core
    the name or literal the casts wrap (for gameRva(...), the gameRva token;
    for x[0].call, the call token).  None when s starts none."""
    n = len(toks)
    if s >= n:
        return None
    kind, t, _ = toks[s]
    if t in _CASTS and s + 1 < n and toks[s + 1][1] == "<":
        depth, c = 0, s + 1
        while c < n:
            depth += {"<": 1, ">": -1}.get(toks[c][1], 0)
            if depth == 0:
                break
            c += 1
        if c + 1 < n and toks[c + 1][1] == "(" and (c + 1) in match:
            close = match[c + 1]
            inner = _primary(toks, match, c + 2)
            return close, inner[1] if inner and inner[0] == close - 1 else None
        return None
    if t == "(" and s in match:
        close = match[s]
        inner_toks = [x[1] for x in toks[s + 1:close]]
        if inner_toks and all(x in _INT_TYPES or x in ("const", "unsigned", "long", "::", "std", "*", "char",
                                                         "void", "uint8_t", "int8_t", "uint16_t", "int16_t")
                              for x in inner_toks):
            return _primary(toks, match, close + 1)   # a C-style cast
        inner = _primary(toks, match, s + 1)
        return close, inner[1] if inner and inner[0] == close - 1 else None
    if kind == "num":
        return s, s
    if kind != "id":
        return None
    if t in _INT_TYPES and s + 1 < n and toks[s + 1][1] == "(" and (s + 1) in match:
        close = match[s + 1]
        inner = _primary(toks, match, s + 2)
        return close, inner[1] if inner and inner[0] == close - 1 else None
    e = core = s
    while True:
        if e + 2 < n and toks[e + 1][1] == "::" and toks[e + 2][0] == "id":
            e = core = e + 2
        elif e + 1 < n and toks[e + 1][1] in ("(", "[") and (e + 1) in match:
            e = match[e + 1]
        elif e + 2 < n and toks[e + 1][1] in (".", "->") and toks[e + 2][0] == "id":
            e = core = e + 2
        else:
            return e, core


def _functions(toks, match, parent, is_scope):
    """{name: [(body brace, [parameter names])]} for the function bodies in
    toks (a brace that is not a scope's, after a parameter list)."""
    out = defaultdict(list)
    for a, (_, t, _) in enumerate(toks):
        if t != "{" or is_scope(a) or a not in match:
            continue
        p = parent[a]
        if p >= 0 and toks[p][1] != "{":
            continue
        if p >= 0 and not is_scope(p):
            continue   # a block or a lambda inside a function: its function is the outer one
        k = a - 1
        while k >= 0 and toks[k][1] in ("const", "noexcept", "override", "final", "volatile"):
            k -= 1
        if k < 1 or toks[k][1] != ")" or k not in match or toks[match[k] - 1][0] != "id":
            continue
        open_ = match[k]
        pieces, cur = [], []
        for j in range(open_ + 1, k):
            if parent[j] == open_ and toks[j][1] == ",":
                pieces.append(cur)
                cur = []
            else:
                cur.append(j)
        pieces.append(cur)
        names = []
        for piece in pieces:
            last = None   # the parameter's name: its last name before a default argument
            for j in piece:
                if parent[j] != open_:
                    continue
                if toks[j][1] == "=":
                    break
                if toks[j][0] == "id":
                    last = toks[j][1]
            names.append(last)
        out[toks[open_ - 1][1]].append((a, names))
    return out


def _base_problems(rel, toks, match, parent):
    """The module-base rule of audit(): (problems, counts) for one file."""
    n = len(toks)
    scope_cache = {}

    def is_scope(a):
        if a not in scope_cache:
            scope_cache[a] = _brace_is_scope(toks, a)
        return scope_cache[a]

    def body_of(i):
        a, body = parent[i], None
        while a >= 0:
            if toks[a][1] == "{" and not is_scope(a):
                body = a
            a = parent[a]
        return body

    def in_body(i, body):
        return body is not None and body < i < match.get(body, -1)

    funcs = _functions(toks, match, parent, is_scope)
    enclosing = {}

    def function_of(i):
        b = body_of(i)
        if b not in enclosing:
            enclosing[b] = next((name for name, defs in funcs.items() for body, _ in defs if body == b), None)
        return enclosing[b]

    def declared_in(name, body):
        for k in range(body + 1, match.get(body, body)):
            if toks[k][1] == name and toks[k][0] == "id" and k >= 1:
                pv = toks[k - 1]
                if (pv[0] == "id" and pv[1] not in _NOT_TYPES) or \
                        (pv[1] in ("*", "&", ">") and k >= 2 and (toks[k - 2][0] == "id" or toks[k - 2][1] == ">")):
                    if k + 1 < n and toks[k + 1][1] in ("=", ";", ",", "{", "[", ")"):
                        return True
        return False

    class Names:
        def __init__(self):
            self.local, self.glob, self.members, self.funcs = set(), set(), set(), set()

        def at(self, j):
            """True when token j starts a reference to a tracked name."""
            kind, t, _ = toks[j]
            if kind != "id" or (j + 1 < n and toks[j + 1][1] in (".", "->", "::")):
                return False   # dos->e_lfanew reads through the name; it is not the name's value
            if j >= 1 and toks[j - 1][1] in (".", "->"):
                return t in self.members
            if j + 1 < n and toks[j + 1][1] == "(" and t in self.funcs:
                return True
            return (t, body_of(j)) in self.local or t in self.glob

        def span(self, j):
            if j + 1 < n and toks[j + 1][1] == "(" and toks[j][1] in self.funcs and (j + 1) in match:
                return _widen(toks, match, j, match[j + 1])
            return _span(toks, match, j)

        def mark(self, lo, hi):
            """The value [lo, hi] is tracked: follow it into what it initializes."""
            prev = toks[lo - 1][1] if lo else ";"
            nxt = toks[hi + 1][1] if hi + 1 < n else ";"
            grew = False
            if prev in ("?", ":") or nxt in ("?", ":"):
                # A branch of a conditional (x = known ? gameRva(k) : 0): the whole conditional is the value.
                a = parent[lo]
                s = lo - 1
                while s >= 0 and parent[s] == a and toks[s][1] not in ("=", ";", "{", "}", ",", "(", "return"):
                    s -= 1
                e = hi + 1
                while e < n and parent[e] == a and toks[e][1] not in (";", ",", ")", "}"):
                    e += 1
                if s + 1 < lo or e - 1 > hi:
                    return self.mark(s + 1, e - 1)
                return False
            if prev in ("=", "{") and nxt in (";", ",", ")", "}"):
                j = lo - 2
                if prev == "{":
                    b = lo - 1   # an element of a brace list: what it initializes
                    if not (b in match and match[b] == hi + 1 or nxt in (",", "}")):
                        return False
                    j = b - 1
                    if j >= 0 and toks[j][1] == "=":
                        j -= 1
                while j >= 0 and toks[j][1] == "]" and j in match:
                    j = match[j] - 1
                if j < 0 or toks[j][0] != "id" or toks[j][1] in _KEYWORDS:
                    return False
                name = toks[j][1]
                if j >= 1 and toks[j - 1][1] in (".", "->"):
                    grew = name not in self.members
                    self.members.add(name)
                    return grew
                body = body_of(j)
                if body is not None and declared_in(name, body):
                    grew = (name, body) not in self.local
                    self.local.add((name, body))
                else:
                    grew = name not in self.glob
                    self.glob.add(name)
                return grew
            if prev == "return" and nxt == ";":
                f = function_of(lo)
                if f and f not in self.funcs:
                    self.funcs.add(f)
                    return True
                return False
            if prev in ("(", ",") and nxt in (",", ")"):
                a = parent[lo]
                if a < 1 or toks[a][1] != "(" or toks[a - 1][0] != "id" or toks[a - 1][1] not in funcs:
                    return False
                pos = sum(1 for k in range(a + 1, lo) if toks[k][1] == "," and parent[k] == a)
                for body, names in funcs[toks[a - 1][1]]:
                    if pos < len(names) and names[pos] and (names[pos], body) not in self.local:
                        self.local.add((names[pos], body))
                        grew = True
            return grew

        def follow(self, seeds):
            for lo, hi in seeds:
                self.mark(lo, hi)
            for _ in range(10):
                grew = False
                for j in range(n):
                    if self.at(j):
                        lo, hi = self.span(j)
                        grew = self.mark(lo, hi) or grew
                if not grew:
                    break

    # The module base, from GetModuleHandle of the executable.
    bases, seeds = Names(), []
    for i, (kind, t, _) in enumerate(toks):
        if kind == "id" and t in _GMH_CALLS and i + 1 < n and toks[i + 1][1] == "(" and (i + 1) in match:
            args = [x[1] for x in toks[i + 2:match[i + 1]]]
            if len(args) == 1 and (args[0] in _GAME_MODULE_ARGS or
                                   (args[0].startswith(('L"', '"')) and "elitedangerous64" in args[0].lower())):
                seeds.append(_widen(toks, match, i, match[i + 1]))
    bases.follow(seeds)
    # gameRva's results, and every name, member and array that holds one.
    resolved, seeds = Names(), []
    for i, (kind, t, _) in enumerate(toks):
        if kind == "id" and t == "gameRva" and i + 1 < n and toks[i + 1][1] == "(" and (i + 1) in match:
            seeds.append(_widen(toks, match, i, match[i + 1]))
    resolved.follow(seeds)

    def zero_tested(core, body):
        """Whether the name at token core is tested against zero in body (or,
        for a member or a name outside any function, anywhere in the file)."""
        name = toks[core][1]
        member = core >= 1 and toks[core - 1][1] in (".", "->")
        lo_b, hi_b = (body, match.get(body, n)) if body is not None and not member and \
            (name, body) in resolved.local else (-1, n)
        for k in range(max(lo_b, 0), hi_b):
            if toks[k][1] != name or toks[k][0] != "id":
                continue
            lo, hi = _span(toks, match, k)
            pv = toks[lo - 1][1] if lo else ";"
            nx = toks[hi + 1][1] if hi + 1 < n else ";"
            if pv == "!" or nx in ("&&", "||", "?") or pv in ("&&", "||"):
                return True
            if nx in ("==", "!=") and hi + 2 < n and toks[hi + 2][1] in ("0", "0u", "nullptr"):
                return True
            if pv in ("==", "!=") and lo >= 2 and toks[lo - 2][1] in ("0", "0u", "nullptr"):
                return True
            if pv == "(" and nx == ")" and lo >= 2 and toks[lo - 2][1] in ("if", "while"):
                return True
        return False

    def guarded(body, member_or_global):
        """A gameRvasFound call in the function (or, for a member or global, in the file)."""
        lo_b, hi_b = (body, match.get(body, n)) if body is not None and not member_or_global else (0, n)
        return any(toks[k][0] == "id" and toks[k][1] == "gameRvasFound" for k in range(lo_b, hi_b))

    def call_tested(core, body):
        """For base + gameRva(K): gameRva(K) itself tested against zero in the function."""
        if body is None:
            return False
        arg = " ".join(x[1] for x in toks[core + 2:match[core + 1]])
        for k in range(body + 1, match.get(body, n)):
            if k == core or toks[k][1] != "gameRva" or k + 1 >= n or toks[k + 1][1] != "(" or (k + 1) not in match:
                continue
            if " ".join(x[1] for x in toks[k + 2:match[k + 1]]) != arg:
                continue
            lo, hi = _widen(toks, match, k, match[k + 1])
            pv = toks[lo - 1][1] if lo else ";"
            nx = toks[hi + 1][1] if hi + 1 < n else ";"
            if pv == "!" or nx in ("&&", "||") or pv in ("&&", "||"):
                return True
            if nx in ("==", "!=") and hi + 2 < n and toks[hi + 2][1] in ("0", "0u", "nullptr"):
                return True
        return False

    problems, counts = [], defaultdict(int)
    allowed = {text for pat, text, _ in AUDIT_BASE_OPERANDS if pat == "*" or rel == pat or Path(rel).match(pat)}
    seen = set()
    for j in range(n):
        if not bases.at(j):
            continue
        lo, hi = bases.span(j)
        if (lo, hi) in seen:
            continue
        seen.add((lo, hi))
        if hi + 1 < n and toks[hi + 1][1] in ("+", "+="):
            s = hi + 2
            p = _primary(toks, match, s)
        elif lo >= 2 and toks[lo - 1][1] == "+" and toks[lo - 2][0] in ("id", "num") and \
                (lo < 3 or toks[lo - 3][1] not in (".", "->", "::", "]", ")")):
            s = lo - 2
            p = _primary(toks, match, s)
        else:
            continue
        where = "src/%s:%d" % (rel, toks[j][2])
        base_text = " ".join(x[1] for x in toks[lo:hi + 1])
        if not p:
            problems.append("%s: base: %s + ... adds something the audit cannot read to the module base" % (where, base_text))
            continue
        end, core = p
        operand = " ".join(x[1] for x in toks[s:end + 1])
        counts["module-base sums"] += 1
        after = toks[max(end, hi) + 1][1] if max(end, hi) + 1 < n else ";"
        before = toks[min(lo, s) - 1][1] if min(lo, s) else ";"
        if before in _RELATIONAL or after in _RELATIONAL or before in ("==", "!=") or after in ("==", "!="):
            counts["module-base comparisons"] += 1
            continue   # a range test or a comparison: nothing at that address is read or written
        ct = toks[core][1] if core is not None else None
        if (ct == "gameRva" and core + 1 < n and toks[core + 1][1] == "(") or (core is not None and resolved.at(core)):
            counts["module-base sums resolved"] += 1
            continue   # a gameRva result (its zero test is the rule below)
        if ct == "e_lfanew" or (core is not None and core >= 1 and toks[core - 1][1] in (".", "->") and ct[:1].isupper()):
            counts["module-base PE fields"] += 1
            continue   # a field of the PE headers (IMAGE_DOS_HEADER.e_lfanew, IMAGE_SECTION_HEADER.VirtualAddress, ...)
        if operand in allowed:
            counts["module-base listed"] += 1
            continue
        problems.append("%s: base: %s + %s adds something other than a gameRva result to the module base "
                        "(resolve it with gameRva, or list a PE-layout offset in AUDIT_BASE_OPERANDS)" % (
                            where, base_text, operand))

    def compared(lo, hi):
        """Whether the arithmetic around [lo, hi] is an operand of a comparison (nothing is addressed then)."""
        a = parent[lo]
        stops = ("=", ";", "{", "}", ",", "(", "[", "return", "&&", "||", "?", ":")
        s = lo - 1
        while s >= 0 and toks[s][1] not in stops:
            s = match[s] - 1 if toks[s][1] in (")", "]") and s in match else s - 1
        e = hi + 1
        while e < n and toks[e][1] not in stops + (")", "]"):
            e = match[e] + 1 if toks[e][1] in ("(", "[") and e in match else e + 1
        for k in range(s + 1, e):
            if parent[k] != a:
                continue
            t = toks[k][1]
            if t in ("==", "!=", "<=", ">="):
                return True
            if t == "<" and not (k >= 1 and toks[k - 1][1] in _CASTS) or \
                    t == ">" and not (k + 1 < n and toks[k + 1][1] == "("):
                return True
        return False

    # The zero rule: a gameRva result (a call, or a name, member or array that holds one) used in address
    # arithmetic needs a zero test of it, or a gameRvasFound guard, in the same function (for a member or a
    # name outside any function, in the file): on an unknown build 0 means "not found".
    seen = set()
    for j in range(n):
        is_call = toks[j][1] == "gameRva" and j + 1 < n and toks[j + 1][1] == "(" and (j + 1) in match
        if not is_call and not resolved.at(j):
            continue
        lo, hi = _widen(toks, match, j, match[j + 1]) if is_call else resolved.span(j)
        if (lo, hi) in seen:
            continue
        seen.add((lo, hi))
        prev = toks[lo - 1][1] if lo else ";"
        nxt = toks[hi + 1][1] if hi + 1 < n else ";"
        if prev not in ("+", "-", "+=", "-=") and nxt not in ("+", "-"):
            continue
        if compared(lo, hi):
            counts["gameRva results compared"] += 1
            continue
        body = body_of(j)
        where = "src/%s:%d" % (rel, toks[j][2])
        text = " ".join(x[1] for x in toks[lo:hi + 1])
        counts["gameRva results in arithmetic"] += 1
        if is_call:
            if not guarded(body, False) and not call_tested(j, body):
                problems.append("%s: zero: %s is used in address arithmetic without a zero test or a gameRvasFound "
                                "guard in the same function (0 means not found on an unknown build)" % (where, text))
            continue
        member = j >= 1 and toks[j - 1][1] in (".", "->")
        outer = member or (toks[j][1], body) not in resolved.local
        if not guarded(body, outer) and not zero_tested(j, body):
            problems.append("%s: zero: %s holds a gameRva result and is used in address arithmetic with no zero test or "
                            "gameRvasFound guard in %s" % (where, text, "the file" if outer else "the same function"))
    return problems, counts


def where_text(table, rva, n=1):
    """One line: how gameRva places rva on an unknown build (a table address,
    or inside the unit of one), and whether [rva, rva + n) lies in table units
    end to end (what gamePinMatches and gamePinProvenByMatch need; the units
    must also be found and have moved together on the build in hand)."""
    targets = table["targets"]
    units = sorted({(t[2], t[3]) for t in targets if t[1] == "code"})
    hit = [t for t in targets if t[0] == rva]
    holder = [u for u in units if u[0] <= rva < u[1]]
    if hit:
        how = "a table %s address" % hit[0][1]
    elif holder:
        how = "inside the table unit [0x%X, 0x%X) at +0x%X" % (holder[0][0], holder[0][1], rva - holder[0][0])
    else:
        how = "NOT PLACED: neither a table address nor inside a table unit"
    at, pieces = rva, []
    while at < rva + n:
        u = next((u for u in units if u[0] <= at < u[1]), None)
        if u is None:
            break
        pieces.append(u)
        at = u[1]
    if n <= 1:
        cover = ""
    elif at >= rva + n:
        cover = "; [0x%X, 0x%X) lies in %d table unit(s)" % (rva, rva + n, len(pieces))
    else:
        cover = "; [0x%X, 0x%X) is NOT covered: 0x%X is in no table unit" % (rva, rva + n, at)
    return "0x%X: %s%s" % (rva, how, cover)


def audit(src_dir, table):
    """Check every C/C++ file under src_dir; returns (problems, counts).

    A game address is a hex literal in the known image's range (an RVA is
    always written in hex here), or a name whose value is one.  Outside the resolver and the exact-only files
    (AUDIT_EXACT_ONLY):
      identity  the known build's PE stamp or image size, or a name defined
                from one, is not used: a hook does not key on the build;
      known     every game address is a table address or inside the code
                unit of one (an address gameRva places by its offset);
      routed    a game address, a name defined from one and a member named
                *rva are used only as the first argument of gameRva,
                gameRvasFound, gamePinMatches or gamePinProvenByMatch, as the
                whole initializer of a constant or of a table entry, inside
                sizeof or static_assert, as a log argument, or as an
                assignment's target.  Never as an operand: base + 0x4314C60
                and base + entry.rva fail, base + gameRva(0x4314C60) passes.
                A name is one whatever its type is spelled (UINT64, auto),
                an array or std::array whose list holds addresses is one,
                and a struct member that a table entry's list fills with an
                address is tracked by name like *rva (Callsite::call).
                A game address spelled in decimal fails as known.
      base      the module base (a name, member or parameter that holds
                GetModuleHandle of the executable, or a function that
                returns one, followed through assignments and calls in the
                file) plus anything other than a gameRva result fails,
                whatever the value: base + 0x8A3C0, base + 70340000 and
                base + off fail.  A field of the PE headers (e_lfanew,
                VirtualAddress, ...), a range test or comparison (nothing
                is addressed) and AUDIT_BASE_OPERANDS pass.
      zero      a gameRva result (the call, or a name, member or array that
                holds one) used in address arithmetic needs a zero test of
                it or a gameRvasFound guard in the same function (for a
                member or a name outside any function, in the file).
    Only a name defined at namespace or class scope in a header is shared by
    every file; any other is its own file's.  Not seen: a parameter whose
    caller is in another file (base + 0x8A3C0 on such a parameter passes)."""
    root = Path(src_dir)
    stamp, size = table["stamp"], table["size"]
    targets = table["targets"]
    rvas = {t[0] for t in targets}
    units = sorted((t[2], t[3]) for t in targets if t[1] == "code")
    starts = [u[0] for u in units]

    def known(v):
        k = bisect.bisect_right(starts, v)
        return v in rvas or any(units[j][0] <= v < units[j][1] for j in range(max(0, k - 16), k))

    files = {}
    for path in sorted(root.rglob("*")):
        if path.is_file() and path.suffix.lower() in {".h", ".hpp", ".cpp", ".cc", ".c", ".inl", ".inc"}:
            files[path.relative_to(root).as_posix()] = _lex(path.read_text(encoding="utf-8", errors="replace"))

    def matches(rel, pat):
        return pat == "*" or rel == pat or Path(rel).match(pat)

    def not_address(rel, v):
        return any((vals is None or v in vals) and matches(rel, pat) for pat, vals, _ in AUDIT_NOT_ADDRESSES)

    exact = lambda rel: any(matches(rel, p) for p, _ in AUDIT_EXACT_ONLY)
    in_range = lambda v: 0x100000 <= v < size or 0x140100000 <= v < 0x140000000 + size
    brackets = {rel: _brackets(toks) for rel, toks in files.items()}
    shared = {"addr": set(), "id": set(), "member": set()}
    own = {"addr": defaultdict(set), "id": defaultdict(set), "member": defaultdict(set)}
    # Every struct's data members in order, for a table entry whose address is not in a member named *rva.
    layouts, layout_shared = {}, set()
    for rel, toks in files.items():
        for name, members in _struct_members(toks, *brackets[rel]).items():
            layouts.setdefault((rel, name), members)
            if rel.endswith((".h", ".hpp", ".inl", ".inc")):
                layout_shared.add(name)
                layouts.setdefault(("*", name), members)

    def tracked(rel, toks):
        """[(index, kind)]: kind 'addr' (a literal), 'dec' (a game address
        written in decimal), 'id' (an identity literal), 'carrier',
        'idcarrier' or 'member'. Members count only in a file that holds a
        game address or a resolver call."""
        addr, ident = shared["addr"] | own["addr"][rel], shared["id"] | own["id"][rel]
        member_names = shared["member"] | own["member"][rel]
        out, game_file = [], False
        for i, (kind, text, _) in enumerate(toks):
            if kind == "num":
                v = _num_value(text)
                if v is None:
                    continue
                if v in (stamp, size):
                    out.append((i, "id"))
                elif text[:2] in ("0x", "0X") and in_range(v) and not not_address(rel, v):
                    out.append((i, "addr"))
                    game_file = True
                elif text[:2] not in ("0x", "0X") and in_range(v) and v < 0x140000000 and known(v) and \
                        not not_address(rel, v):
                    out.append((i, "dec"))
                    game_file = True
            elif kind == "id":
                if i and toks[i - 1][1] in (".", "->"):
                    if re.fullmatch(r"\w*(?:rva|Rva|RVA)", text) or text in member_names:
                        out.append((i, "member"))
                elif text in ident:
                    out.append((i, "idcarrier"))
                elif text in addr:
                    out.append((i, "carrier"))
                    game_file = True
                elif text in RESOLVER_CALLS:
                    game_file = True
        return out if game_file else [o for o in out if o[1] != "member"]

    # Phase 1: the names defined from a game address or an identity value, to a fixpoint.
    for _ in range(10):
        grew = False
        for rel, toks in files.items():
            if rel in AUDIT_RESOLVER:
                continue
            match, parent = brackets[rel]
            for i, kind in tracked(rel, toks):
                if kind == "member":
                    continue
                verdict, how, d = _context(toks, match, parent, i)
                if verdict != "ok" or not d:
                    continue
                name, integral, is_decl, type_toks = d
                header = rel.endswith((".h", ".hpp", ".inl", ".inc"))
                if how == "table" and is_decl and kind not in ("id", "idcarrier"):
                    # An element of a brace list. A flat list of a scalar array (int x[] = {...}, std::array<T, N>
                    # x = {...}) makes its name a carrier, whatever the element type is spelled; an entry of a
                    # struct (or of an array of structs) makes the struct's member at that position one.
                    lo, _ = _span(toks, match, i)
                    lst = parent[lo]
                    outer = lst
                    while parent[outer] >= 0 and toks[parent[outer]][1] == "{" and outer >= 1 and \
                            toks[outer - 1][1] in ("{", ","):
                        outer = parent[outer]
                    k = outer - 1
                    if k >= 0 and toks[k][1] == "=":
                        k -= 1
                    is_array = (k >= 0 and toks[k][1] == "]") or "array" in type_toks
                    if not (is_array and lst == outer):
                        if not integral or lst != outer:
                            struct = next((t for t in reversed(type_toks) if re.fullmatch(r"[A-Za-z_]\w*", t)
                                           and t not in ("const", "constexpr", "static", "inline", "volatile",
                                                         "std", "array", "extern", "thread_local")), None)
                            members = layouts.get((rel, struct)) or layouts.get(("*", struct))
                            pos = sum(1 for q in range(lst + 1, lo) if toks[q][1] == "," and parent[q] == lst)
                            if members and pos < len(members):
                                bucket = shared["member"] if struct in layout_shared else own["member"][rel]
                                if members[pos] not in bucket:
                                    bucket.add(members[pos])
                                    grew = True
                            continue   # a table entry or an object: its member is tracked by name
                elif how != "init" and is_decl and not integral:
                    continue   # a table entry or an object: its *rva member is tracked by name
                group = "id" if kind in ("id", "idcarrier") else "addr"
                bucket = shared[group] if header and is_decl and _at_namespace_scope(toks, match, parent, i) \
                    else own[group][rel]
                if name not in bucket:
                    bucket.add(name)
                    grew = True
        if not grew:
            break

    # Phase 2: every use outside the resolver and the exact-only files.
    problems, counts = [], defaultdict(int)
    for rel, toks in files.items():
        if rel in AUDIT_RESOLVER:
            continue
        counts["resolver calls"] += sum(1 for k, t, _ in toks if k == "id" and t in RESOLVER_CALLS)
        if exact(rel):
            continue
        match, parent = brackets[rel]
        base_problems, base_counts = _base_problems(rel, toks, match, parent)
        problems.extend(base_problems)
        for k, v in base_counts.items():
            counts[k] += v
        for i, kind in tracked(rel, toks):
            text, line = toks[i][1], toks[i][2]
            where = "src/%s:%d" % (rel, line)
            if kind == "dec":
                problems.append("%s: known: %s is the game address 0x%X written in decimal; write the RVA in hex "
                                "and pass it to gameRva" % (where, text, _num_value(text)))
                continue
            if kind in ("id", "idcarrier"):
                problems.append("%s: identity: %s is the known build's PE %s; a hook does not key on the "
                                "build (gameRva and gamePinMatches do)" % (
                                    where, text, "stamp or image size" if kind == "idcarrier" else
                                    "stamp" if _num_value(text) == stamp else "image size"))
                continue
            if kind == "addr":
                counts["game addresses"] += 1
                v = _num_value(text)
                if v >= 0x140000000:
                    problems.append("%s: known: %s is a VA at the preferred image base; write the RVA and "
                                    "pass it to gameRva" % (where, text))
                    continue
                if not known(v):
                    problems.append("%s: known: %s is in the game image's range but is neither a table "
                                    "address nor inside the unit of one (add it to the table, or to "
                                    "AUDIT_NOT_ADDRESSES if it is not an address)" % (where, text))
                    continue
            verdict, how, _ = _context(toks, match, parent, i)
            counts[how if verdict == "ok" else "routed problems"] += 1
            if verdict != "ok":
                problems.append("%s: routed: %s; resolve it with gameRva first" % (where, how))
    counts["files"] = len(files)
    return problems, counts


# --------------------------------------------------------------------------
# Command line.
# --------------------------------------------------------------------------

def _load_table(path):
    table = json.loads(Path(path).read_text(encoding="utf-8"))
    if table.get("format") == 1 and table.get("hash") == HASH_NAME:
        table["format"] = FORMAT  # Format 1 is format 2 without fingerprints.
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
            len(report["data_weak_ref"])),
        "clone targets with fingerprints %d; named only by neighbours (layout-only) %d" % (
            len(report["code_fp"]), len(report["code_layout_only"]))]
    for key, label in (("no_unit", "in no unit"), ("non_unique", "non-unique"),
                       ("code_no_ref", "non-unique, no caller"),
                       ("code_fp", "non-unique, fingerprinted"),
                       ("code_layout_only", "non-unique, layout-only"),
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
    gen.add_argument("--rvas", required=True, action="append",
                     help="build_diff map JSON, or a list: RVA [code|data] per line "
                          "(repeatable: the targets of every file, in order, each once)")
    gen.add_argument("--rva-field", default="new_rva",
                     help="JSON field holding the known build's RVA (default new_rva)")
    gen.add_argument("--out", required=True, help="table JSON to write")
    gen.add_argument("--emit-cpp", default=None,
                     help="also write the table as the runtime's C++ include (%s)" % CPP_TABLE_PATH)
    gen.add_argument("--build", default=None,
                     help="the known build's number for the log (default: digits in the exe's name)")
    gen.add_argument("--dry-run", action="store_true", help="write nothing")
    res = sub.add_parser("resolve", help="find the table's targets in an exe")
    res.add_argument("--table", required=True)
    res.add_argument("--exe", required=True)
    res.add_argument("--allow-layout", action="store_true", help=layout)
    chk = sub.add_parser("check", help="resolve and compare with a ground truth")
    chk.add_argument("--table", required=True)
    chk.add_argument("--exe", required=True)
    chk.add_argument("--allow-layout", action="store_true", help=layout)
    chk.add_argument("--expect", required=True, action="append",
                     help="build_diff map --json output (known build -> this exe), or an "
                          "expectation file (repeatable: a later file adds the RVAs the "
                          "earlier ones lack)")
    chk.add_argument("--invert", action="store_true",
                     help="the expect rows run exe -> known (map_all.json style)")
    chk.add_argument("--src", default=str(Path(__file__).resolve().parent.parent / "src"),
                     help="source tree searched for each address (per-file breakdown)")
    chk.add_argument("--emit-expect", default=None,
                     help="write the C++ rig's expectation file for this exe "
                          "(tools/game_build_test/expect_<build>.txt)")
    chk.add_argument("--dry-run", action="store_true", help="with --emit-expect: write nothing")
    fx = sub.add_parser("fixture", help="write the self-test's synthetic exes, their C++ table "
                                        "and this matcher's answers for tools/game_build_test")
    fx.add_argument("--out-dir", required=True)
    fx.add_argument("--dry-run", action="store_true", help="write nothing")
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
    au = sub.add_parser("audit", help="check that every game address in the source goes through "
                                      "the resolver (a build gate)")
    au.add_argument("--src", default=str(Path(__file__).resolve().parent.parent / "src"))
    au.add_argument("--table", default=None,
                    help="the table: generate's JSON, or the emitted C++ (default %s)" % CPP_TABLE_PATH)
    wh = sub.add_parser("where", help="say how gameRva places a known-build RVA, and whether a "
                                      "range lies in table units (what gamePinMatches needs)")
    wh.add_argument("--rva", required=True, action="append", help="RVA[:length] (repeatable)")
    wh.add_argument("--table", default=None, help="as for audit")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.command:
        ap.error("a subcommand is required (generate, resolve, check, survey, fixture, audit, "
                 "or --self-test)")
    try:
        if args.command in ("audit", "where"):
            path = Path(args.table) if args.table else \
                Path(__file__).resolve().parent.parent / CPP_TABLE_PATH
            text = path.read_text(encoding="utf-8")
            table = table_for_audit(json.loads(text)) if text.lstrip().startswith("{") \
                else table_from_cpp(text)
        if args.command == "where":
            for item in args.rva:
                rva_text, _, n_text = item.partition(":")
                rva, n = int(rva_text, 0), int(n_text, 0) if n_text else 1
                print(where_text(table, rva, n))
            return 0
        if args.command == "audit":
            problems, counts = audit(args.src, table)
            for p in problems:
                print(p)
            print("hook_signatures audit: %d files, %d game addresses, %d resolver calls, %d sums on the module base, "
                  "%d gameRva results in address arithmetic; %s" % (
                      counts["files"], counts["game addresses"], counts["resolver calls"], counts["module-base sums"],
                      counts["gameRva results in arithmetic"], "%d problem(s)" % len(problems) if problems else "ok"))
            return 1 if problems else 0
        if args.command == "survey":
            _, details, lines = survey(args.exe, args.other, args.cache, args.random,
                                       args.per_status, args.seed, args.allow_layout)
            print("\n".join(lines))
            return 1 if details["wrong"] else 0
        if args.command == "fixture":
            write_outputs(fixture_outputs(args.out_dir), args.dry_run)
            return 0
        if args.command == "generate":
            targets, seen = [], set()
            for path in args.rvas:
                for rva, kind in read_targets(path, args.rva_field):
                    if rva not in seen:
                        seen.add(rva)
                        targets.append((rva, kind))
            t0 = time.perf_counter()
            table, report = generate(args.exe, targets,
                                     log=lambda s: print(s, flush=True), build=args.build)
            text = table_text(table)
            for line in _summary(table, report, text):
                print(line)
            print("generated in %.1fs" % (time.perf_counter() - t0))
            outputs = {Path(args.out): text}
            if args.emit_cpp:
                command = ("python tools/hook_signatures.py generate --exe <known exe> --rvas "
                           "<map_all.json> --rvas <signature_extra.txt> --out build/hook_signatures.json "
                           "--emit-cpp %s" % CPP_TABLE_PATH)
                outputs[Path(args.emit_cpp)] = emit_cpp(table, command)
            write_outputs(outputs, args.dry_run)
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
        expect = {}
        for path in args.expect:
            for rva, value in read_expect(path, args.invert).items():
                expect.setdefault(rva, value)
        rows = classify(results, expect)
        files = source_files(args.src, [r["rva"] for r in rows]) \
            if Path(args.src).is_dir() else None
        for line in check_report(rows, files, stats):
            print(line)
        if args.emit_expect:
            write_outputs({Path(args.emit_expect): expect_text(
                table, exe_identity(args.exe), rows, Path(args.exe).name)}, args.dry_run)
        return 1 if any(r["cls"] == "WRONG" for r in rows) else 0
    except (OSError, PEError, ValueError, KeyError) as exc:
        print("hook_signatures: " + str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

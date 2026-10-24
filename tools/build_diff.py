#!/usr/bin/env python3
"""Offline "did the game's render-relevant code change between builds" detector.

    python tools/build_diff.py capture --exe analysis/EliteDangerous64-334925.exe ^
        --out tools/build_diff_targets.json
    python tools/build_diff.py diff --exe "C:\\path\\to\\EliteDangerous64.exe"

capture fingerprints the render-pipeline target list (see
docs/engine-render-pipeline.md) into a JSON baseline: the PE identity
(timestamp + image size, the same pair kinematic_eval_hook.cpp's
targetValid checks), the file SHA-256, and per-target FNV-1a-64 hashes,
prologue bytes and raw body bytes.  diff compares a candidate executable
against that baseline without the baseline file being present:

  * identical PE identity (timestamp + image size) -> "unchanged";
  * per-target MATCH / CHANGED (with first-diff offset and byte count) /
    UNRELIABLE (section table moved, so same-RVA comparison is
    meaningless) / GONE-UNMAPPABLE (RVA outside every section);
  * a .text block-diff map: FNV-1a-64 per 4 KiB block, clustered into
    contiguous changed regions, localizing change without disassembly.

Elite updates rebase every VA-bound target; the diff report is the cheap
pre-flight that says whether the next test flight is measuring the same
engine the analysis docs describe.  No executable is launched or
modified; only the --out file is written.

Historical map example (332841 to 334473), with explicit old RVAs; the
334473 to 334925 step ran the same way with --rvas-file and --seed
(.agents/build-334925/):

    python tools/build_diff.py map --old analysis/EliteDangerous64.exe ^
        --new analysis/EliteDangerous64-334473.exe --rva 0x430EFE0 ^
        --rva 0x4DDEFB0 [--rvas-file F] [--json OUT] ^
        [--all-functions OUT.csv] [--cache MAP.json]

--seed maps the current baseline's targets (--targets, default
tools/build_diff_targets.json, build 334925), so --old must be that build;
map refuses --seed when --old's timestamp and image size differ.  For the
next game update:

    python tools/build_diff.py map --old analysis/EliteDangerous64-334925.exe ^
        --new <next exe> --seed

diff cannot follow a target once sections move, which every update does.
map carries old-build RVAs (hook sites, data) onto the new build.  It
splits both images into code units (each .pdata function or fragment, plus
the leaf stubs between them) and pairs them: byte-identical units that are
unique in both images anchor an in-order walk, and between anchors a unit
pairs with the next same-length unit whose bytes differ only in short runs.
Each differing run must then be a 32-bit field (rel32 call/jmp, RIP-relative
disp32, or [image-base + RVA] disp32) whose old target maps onto its new
target: a code target onto the mapped counterpart, a data target by a delta
that the data references around it vote for.  Code RVAs report

  IDENTICAL   same bytes at a new address (new = unit start + offset);
  RELOC-ONLY  every difference verified as a displacement (same new RVA rule);
  CHANGED     the paired unit was edited: the best new unit, a similarity
              score, and a lower-confidence placement (~RVA) of the old
              offset (function entry maps to entry; the rest by common-chunk
              alignment, with the span it may fall in);
  AMBIGUOUS   look-alike units (thunks, initialisers) beside an edit: the
              pairing rests on order, candidates are listed;
  NOT-FOUND   no pairing (removed or rewritten; best guesses listed).

Data RVAs report XREF-EXACT (code references both targets), XREF-BRACKET
(the nearest referenced neighbours agree on a delta; the 32 bytes at the new
address are compared), CONTENT-SEARCH, UNCONFIRMED, or AMBIGUOUS; a table of
code pointers (vtable) also reports how many slots map and are found at the
new address.  --all-functions writes the whole old-to-new unit map as CSV
(new_start is the paired unit's start, only a guess for CHANGED/AMBIGUOUS
rows; units that exist only in the new build follow as ADDED).
--cache keeps the built map (about 25 s to build) keyed by both exes' SHA-256.
Known blind spots: look-alike families that the update shuffled are matched
by order and verified by data deltas, so they come out AMBIGUOUS or CHANGED
rather than wrong; code inside a function that was split or merged between
.pdata entries is NOT-FOUND.  Pure Python, standard library only.
"""
import argparse
import bisect
import hashlib
import json
import re
import struct
import sys
import tempfile
import time
from pathlib import Path

try:
    from openxr_pe import Image, PEError
except ImportError:  # Allows a repository-root import in unit tests.
    from tools.openxr_pe import Image, PEError

SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_TARGETS = SCRIPT_DIR / "build_diff_targets.json"

FNV_OFFSET = 0xCBF29CE484222325  # Same constants as tools/elite_oculus.py.
FNV_PRIME = 0x100000001B3
PROLOGUE_LEN = 14  # Same 14-byte prologue convention as kinematic_eval_hook.cpp.
BLOCK_SIZE = 4096

# Render-pipeline map target list (docs/engine-render-pipeline.md), seeded
# 2026-09-21 from the baseline at analysis/EliteDangerous64.exe; retargeted to
# build 334473 on 2026-10-20 from analysis/EliteDangerous64-334473.exe, and to
# build 334925 on 2026-10-23 (every target maps RELOC-ONLY or IDENTICAL).  Sizes are
# the Ghidra body sizes from analysis/decomp/decomp_*.txt headers; they may
# include jump-table bytes, which is fine for fingerprinting.
TARGET_SEED = [
    {"name": "kinematic-eval-fun_144311ca0", "stage": "kinematic-eval",
     "rva": 0x4311C00, "size": 1006},
    {"name": "lod-evaluator-fun_144334000", "stage": "cull-lod",
     "rva": 0x4333F60, "size": 64},
    {"name": "worker-entry-fun_144324640", "stage": "frame-scheduler",
     "rva": 0x43245A0, "size": 503},
    {"name": "worker-entry-fun_144323040", "stage": "frame-scheduler",
     "rva": 0x4322FA0, "size": 565},
    {"name": "bucket-builder-fun_1442b70e0", "stage": "bucket-workers",
     "rva": 0x42B7040, "size": 3222},
    {"name": "rig-dispatch-fun_14431dce0", "stage": "kinematic-eval",
     "rva": 0x431DC40, "size": 699},
    {"name": "bucket-traversal-fun_144314d00", "stage": "bucket-workers",
     "rva": 0x4314C60, "size": 1597},
    {"name": "scheduler-reset-fun_1436a3c50", "stage": "frame-scheduler",
     "rva": 0x36A3BB0, "size": 687},
    {"name": "merge-fun_144350f0e", "stage": "frame-scheduler",
     "rva": 0x4350E6E, "size": 129},
    {"name": "merge-fun_144350f8f", "stage": "frame-scheduler",
     "rva": 0x4350EEF, "size": 315},
    {"name": "enqueue-fun_144c81b50", "stage": "bucket-workers",
     "rva": 0x4C81A30, "size": 632},
    {"name": "accumulator-fun_144c86390", "stage": "bucket-workers",
     "rva": 0x4C86270, "size": 427},
    {"name": "drainer-fun_144350490", "stage": "bucket-workers",
     "rva": 0x43503F0, "size": 64},
    {"name": "rekey-wrapper-fun_1442b8330", "stage": "bucket-workers",
     "rva": 0x42B8290, "size": 64},
    {"name": "projection-wrapper-0x4e3050", "stage": "cull-lod",
     "rva": 0x4E2FF0, "size": 64},
    {"name": "projection-wrapper-0x4e2e30", "stage": "cull-lod",
     "rva": 0x4E2DD0, "size": 64},
]


def fnv1a64(data):
    value = FNV_OFFSET
    for byte in data:
        value ^= byte
        value = (value * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return value


def read_identity(im):
    """PE timestamp (NT header +8) and image size (NT header +0x50).

    This is exactly the pair targetValid() in kinematic_eval_hook.cpp
    memcmps before trusting a hook site.
    """
    nt = int.from_bytes(im.data[0x3C:0x40], "little")
    timestamp = int.from_bytes(im.data[nt + 8:nt + 12], "little")
    image_size = int.from_bytes(im.data[nt + 0x50:nt + 0x54], "little")
    return timestamp, image_size


def section_table(im):
    """Named (rva, span) pairs, for the layout-compared UNRELIABLE rule."""
    nt = int.from_bytes(im.data[0x3C:0x40], "little")
    opt_size = int.from_bytes(im.data[nt + 20:nt + 22], "little")
    count = int.from_bytes(im.data[nt + 6:nt + 8], "little")
    table = nt + 24 + opt_size
    rows = []
    for i in range(count):
        off = table + i * 40
        name = im.data[off:off + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        span = max(int.from_bytes(im.data[off + 8:off + 12], "little"),
                   int.from_bytes(im.data[off + 16:off + 20], "little"))
        rows.append({"name": name,
                     "rva": int.from_bytes(im.data[off + 12:off + 16], "little"),
                     "span": span})
    return rows


def find_text(im):
    """Return (rva, raw_size) of .text, falling back to the first
    executable section; None when there is no executable section."""
    nt = int.from_bytes(im.data[0x3C:0x40], "little")
    opt_size = int.from_bytes(im.data[nt + 20:nt + 22], "little")
    count = int.from_bytes(im.data[nt + 6:nt + 8], "little")
    table = nt + 24 + opt_size
    fallback = None
    for i in range(count):
        off = table + i * 40
        name = im.data[off:off + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        rva = int.from_bytes(im.data[off + 12:off + 16], "little")
        raw_size = int.from_bytes(im.data[off + 16:off + 20], "little")
        characteristics = int.from_bytes(im.data[off + 36:off + 40], "little")
        if name == ".text":
            return rva, raw_size
        if fallback is None and characteristics & 0x20000000 and raw_size:
            fallback = (rva, raw_size)
    return fallback


def text_block_hashes(im, rva, raw_size):
    """FNV-1a-64 of every 4 KiB block of the section's raw (file) bytes."""
    offset, _ = im.mapping(rva, 1)
    hashes = []
    for start in range(0, raw_size, BLOCK_SIZE):
        block = im.data[offset + start:offset + min(start + BLOCK_SIZE, raw_size)]
        hashes.append("%016X" % fnv1a64(block))
    return hashes


def _rva_bytes(im, rva, size):
    offset, _ = im.mapping(rva, size)
    return im.data[offset:offset + size]


def capture(exe_path, out_path, targets):
    """Fingerprint exe_path into out_path.  Writes only the --out file."""
    data = Path(exe_path).read_bytes()
    im = Image(data)
    timestamp, image_size = read_identity(im)
    text = find_text(im)
    baseline = {
        "exe": Path(exe_path).name,
        "sha256": hashlib.sha256(data).hexdigest(),
        "timestamp": timestamp,
        "image_size": image_size,
        "sections": section_table(im),
    }
    if text is not None:
        baseline["text_map"] = {"rva": text[0], "block_size": BLOCK_SIZE,
                                "hashes": text_block_hashes(im, text[0], text[1])}
    rows = []
    for target in targets:
        rva, size = target["rva"], target["size"]
        try:
            body = _rva_bytes(im, rva, size)
        except PEError as exc:
            rows.append({"name": target["name"], "stage": target["stage"],
                         "rva": rva, "size": size, "unmappable": str(exc)})
            continue
        rows.append({
            "name": target["name"],
            "stage": target["stage"],
            "rva": rva,
            "size": size,
            "fnv1a64": "%016X" % fnv1a64(body),
            "prologue_hex": body[:PROLOGUE_LEN].hex(),
            "bytes_hex": body.hex(),
        })
    document = {"baseline": baseline, "targets": rows}
    Path(out_path).write_text(json.dumps(document, indent=1, sort_keys=True) + "\n",
                              encoding="utf-8")
    return document


def _cluster_changed(changed):
    """Group sorted changed block indices into (first, last) runs."""
    clusters = []
    for index in changed:
        if clusters and index == clusters[-1][1] + 1:
            clusters[-1][1] = index
        else:
            clusters.append([index, index])
    return clusters


def compare(exe_path, baseline_doc):
    """Compare a candidate exe against a captured baseline document.

    Returns (lines, stats): the human report and a dict of per-target
    verdicts the self-test asserts against.
    """
    data = Path(exe_path).read_bytes()
    im = Image(data)
    candidate_sha = hashlib.sha256(data).hexdigest()
    timestamp, image_size = read_identity(im)
    base = baseline_doc["baseline"]
    lines = []
    stats = {"targets": [], "identity_match": False, "unreliable": 0,
             "changed": 0, "gone": 0, "match": 0,
             "block_changes": 0, "clusters": []}

    lines.append("baseline:  sha256 %s timestamp 0x%X image_size 0x%X"
                 % (base["sha256"], base["timestamp"], base["image_size"]))
    lines.append("candidate: sha256 %s timestamp 0x%X image_size 0x%X"
                 % (candidate_sha, timestamp, image_size))

    if (timestamp, image_size) == (base["timestamp"], base["image_size"]):
        stats["identity_match"] = True
        lines.append("")
        lines.append("exe unchanged (identity match)")
        return lines, stats

    candidate_sections = section_table(im)
    layout_same = candidate_sections == base["sections"]

    lines.append("")
    by_stage = {}
    for target in baseline_doc["targets"]:
        rva, size = target["rva"], target["size"]
        name = target["name"]
        stage = target["stage"]
        if target.get("unmappable"):
            verdict = "GONE/UNMAPPABLE"
            detail = ("unmappable in the baseline capture too (%s)"
                      % target["unmappable"])
            stats["gone"] += 1
            candidate_bytes = None
            mapped = True  # layout and byte checks do not apply
        else:
            base_bytes = bytes.fromhex(target["bytes_hex"])
            try:
                candidate_bytes = _rva_bytes(im, rva, size)
                mapped = True
            except PEError:
                candidate_bytes = None
                mapped = False
        if target.get("unmappable"):
            pass
        elif not mapped:
            verdict = "GONE/UNMAPPABLE"
            detail = "rva 0x%X is outside every section" % rva
            stats["gone"] += 1
        elif not layout_same:
            verdict = "UNRELIABLE"
            detail = ("section table differs from baseline; same-RVA "
                      "comparison is meaningless -- re-run capture")
            stats["unreliable"] += 1
        elif candidate_bytes == base_bytes:
            verdict = "MATCH"
            detail = "rva 0x%X (%d bytes)" % (rva, size)
            stats["match"] += 1
        else:
            diffs = [i for i in range(size) if candidate_bytes[i] != base_bytes[i]]
            first = diffs[0]
            prologue = candidate_bytes[:len(bytes.fromhex(target["prologue_hex"]))]
            prologue_note = ("prologue still matches"
                             if prologue.hex() == target["prologue_hex"]
                             else "prologue DIFFERS (hook sites likely broken)")
            verdict = "CHANGED"
            detail = ("rva 0x%X, first diff +0x%X, %d of %d bytes differ, %s"
                      % (rva, first, len(diffs), size, prologue_note))
            stats["changed"] += 1
        stats["targets"].append({"name": name, "stage": stage, "verdict": verdict})
        by_stage.setdefault(stage, []).append("  %-15s %-36s %s"
                                              % (verdict, name, detail))

    for stage in by_stage:
        lines.append("[%s]" % stage)
        lines.extend(by_stage[stage])
    if not by_stage:
        lines.append("(no targets captured in the baseline)")

    text = find_text(im)
    stored_map = base.get("text_map")
    lines.append("")
    if stored_map is None or text is None:
        lines.append(".text block map: unavailable (missing from baseline "
                     "or candidate)")
    else:
        candidate_hashes = text_block_hashes(im, text[0], text[1])
        base_hashes = stored_map["hashes"]
        changed = [i for i in range(max(len(base_hashes), len(candidate_hashes)))
                   if i >= len(base_hashes) or i >= len(candidate_hashes)
                   or base_hashes[i] != candidate_hashes[i]]
        stats["block_changes"] = len(changed)
        stats["clusters"] = _cluster_changed(changed)
        approximate = ""
        if text[0] != stored_map["rva"] or len(candidate_hashes) != len(base_hashes):
            approximate = (" (approximate: .text RVA range differs between "
                           "exes -- blocks compared by index)")
        lines.append(".text block map: %d baseline blocks, %d changed%s"
                     % (len(base_hashes), len(changed), approximate))
        if changed:
            clusters = []
            for first, last in stats["clusters"]:
                start = stored_map["rva"] + first * BLOCK_SIZE
                end = stored_map["rva"] + (last + 1) * BLOCK_SIZE
                clusters.append("0x%X-0x%X (%d block%s)"
                                % (start, end, last - first + 1,
                                   "s" if last > first else ""))
            lines.append("changed clusters: " + ", ".join(clusters))
        else:
            lines.append("changed clusters: none")

    lines.append("")
    if stats["unreliable"]:
        lines.append("render-relevant change: layout changed -- re-run "
                     "capture and re-verify")
    elif stats["changed"]:
        lines.append("render-relevant change: %d target%s changed"
                     % (stats["changed"], "s" if stats["changed"] != 1 else ""))
    elif stats["block_changes"]:
        lines.append("render-relevant change: none detected (%d .text "
                     "block%s changed outside the target list)"
                     % (stats["block_changes"],
                        "s" if stats["block_changes"] != 1 else ""))
    else:
        lines.append("render-relevant change: none detected")
    return lines, stats


# ---------------------------------------------------------------------------
# map: carry old-build RVAs (hook sites, data) across an update.
#
# Elite's code is recompiled from nearly the same source, so a function in
# the new build is byte-identical to its old self except for rel32 call/jmp
# displacements and RIP-relative disp32 fields that now point at moved
# targets.  The matcher exploits that: it pairs same-length functions whose
# differences are all short runs, then checks that every differing run is a
# 32-bit displacement landing on the *mapped counterpart* of its old target.

IMAGE_BASE = 0x140000000   # Preferred base; absolute pointers in .rdata/.data.
SCN_EXEC = 0x20000000      # IMAGE_SCN_MEM_EXECUTE.
MAX_RUN = 8                # Widest diff run a clean same-length pair may carry.
MAX_SKIP = 64              # Most units an edit may add and drop before resync.
RESYNC_COOLDOWN = 3        # Mismatches stepped over after a failed resync.
ANCHOR_SHARED = 16         # Bytes two units must share to anchor a resync.
ANCHOR_MIN = 12            # Shortest unit that can be a unique-bytes anchor.
LOOKAHEAD = 8              # Units after a resync candidate that must pair too,
LOOKAHEAD_NEED = 6         # and how many of them.
SHINGLE = 8                # Byte n-gram size for similarity and alignment.
DATA_BLOCK = 0x400         # Data address granularity of the delta votes.
CONTENT_GOOD = 28          # Of 32 data bytes that must survive a move to be trusted.
VTABLE_CAP = 256           # Most pointer slots read from one table.
REMATCH_RADIUS = 0x4000    # Address distance searched for a unit that failed to verify.
REMATCH_TRIES = 32         # Nearest same-length candidates tried per unit.
REL, RVA = 1, 2            # Field kinds: target = field end + disp, or = disp.

IDENTICAL = "IDENTICAL"
RELOC_ONLY = "RELOC-ONLY"
CHANGED = "CHANGED"
AMBIGUOUS = "AMBIGUOUS"
NOT_FOUND = "NOT-FOUND"
STATUSES = (IDENTICAL, RELOC_ONLY, CHANGED, AMBIGUOUS, NOT_FOUND)

H_SEQ, H_MOVED, H_SIM = 1, 2, 3
HOW_NAMES = {H_SEQ: "in-order", H_MOVED: "moved", H_SIM: "similarity"}

_NONZERO_RUN = re.compile(rb"[^\x00]+")
_CC_RUN = re.compile(rb"\xcc+")
_I32 = struct.Struct("<i").unpack_from


def _runs(a, b):
    """Maximal (start, end) runs of differing bytes in equal-length buffers."""
    x = (int.from_bytes(a, "little") ^ int.from_bytes(b, "little")).to_bytes(
        len(a), "little")
    return [m.span() for m in _NONZERO_RUN.finditer(x)]


def _clean(a, b, min_shared, max_run):
    """Diff runs when a and b differ only in short runs, else None."""
    if a == b:
        return []
    runs = _runs(a, b)
    for x, y in runs:
        if y - x > max_run:
            return None
    if len(a) - sum(y - x for x, y in runs) < min_shared:
        return None
    return runs


def _pdata_entries(im):
    """Sorted (begin, end) RVA pairs of the RUNTIME_FUNCTION table."""
    rva, size = im.directory(3)
    if not rva:
        return []
    off, _ = im.mapping(rva, size)
    count = size // 12
    vals = struct.unpack_from("<%dI" % (count * 3), im.data, off)
    return sorted(set((vals[k], vals[k + 1]) for k in range(0, count * 3, 3)
                      if vals[k + 1] > vals[k]))


class _Units:
    """The code units of one image, sorted by RVA.

    A unit is a .pdata RUNTIME_FUNCTION range (chained fragments and funclets
    included, each its own unit) or a leaf stub in the gap between two of
    them: leaf functions carry no unwind data, so the gaps are split on the
    int3 padding that aligns the next function.
    """

    def __init__(self, im):
        self.im = im
        self.data = im.data
        self.st, self.en, self.off, self.leaf = [], [], [], []
        entries = _pdata_entries(im)
        begins = [b for b, _ in entries]
        for rva, span, raw, raw_size, flags in im.sections:
            if not flags & SCN_EXEC:
                continue
            limit = rva + min(span, raw_size)
            cursor = rva
            for k in range(bisect.bisect_left(begins, rva), len(entries)):
                begin, end = entries[k]
                if begin >= limit:
                    break
                begin = max(begin, cursor)
                end = min(end, limit)
                if end <= begin:
                    continue
                self._gap(rva, raw, cursor, begin, False)
                self._add(raw + begin - rva, begin, end, False)
                cursor = end
            self._gap(rva, raw, cursor, limit, True)

    def _add(self, off, begin, end, leaf):
        self.st.append(begin)
        self.en.append(end)
        self.off.append(off)
        self.leaf.append(leaf)

    def _gap(self, sec_rva, sec_raw, a, b, section_tail):
        """Split the leaf code in [a, b) on int3 padding runs that end on a
        16-byte boundary (or at the gap's end)."""
        if b <= a:
            return
        blk = self.data[sec_raw + a - sec_rva:sec_raw + b - sec_rva]
        pos = 0
        for m in _CC_RUN.finditer(blk):
            if (a + m.end()) % 16 == 0 or m.end() == len(blk):
                if m.start() > pos:
                    self._add(sec_raw + a + pos - sec_rva, a + pos,
                              a + m.start(), True)
                pos = m.end()
        if pos < len(blk):
            seg = blk[pos:]
            if section_tail:  # Zero fill after the last function.
                seg = seg.rstrip(b"\0")
            if seg:
                self._add(sec_raw + a + pos - sec_rva, a + pos,
                          a + pos + len(seg), True)

    def __len__(self):
        return len(self.st)

    def body(self, i):
        return self.data[self.off[i]:self.off[i] + self.en[i] - self.st[i]]

    def at(self, rva):
        """Index of the unit holding rva, or -1 (padding, or not code)."""
        i = bisect.bisect_right(self.st, rva) - 1
        return i if i >= 0 and rva < self.en[i] else -1


def _unique_anchors(O, N, olen, nlen):
    """(old, new) pairs of byte-identical units that occur once in each image,
    reduced to the longest ordered chain: fixed points the walk cannot slip
    across."""
    seen = {}
    for side, (units, lens) in enumerate(((O, olen), (N, nlen))):
        for k in range(len(units)):
            if lens[k] >= ANCHOR_MIN:
                seen.setdefault(units.body(k), [[], []])[side].append(k)
    pairs = sorted((row[0][0], row[1][0]) for row in seen.values()
                   if len(row[0]) == 1 and len(row[1]) == 1)
    return _chain(pairs)


def _match_units(O, N):
    """Pair old units with same-length new units whose bytes differ only in
    short runs.

    Byte-identical units that are unique in both images anchor the match
    (the longest ordered chain of them).  Between two anchors the units are
    the same code in the same order, so a walk pairs old[i] with new[j]
    while they match and, at a mismatch, resyncs on the cheapest (units
    dropped from old, units added to new) after which the next units match
    again.  Units still unmatched at the end are paired with a clean unique
    twin anywhere (flagged "moved").
    Returns (match, how, runs_of): new index or -1 per old unit, H_* codes,
    and the diff runs of the pairs made in order.
    """
    n_old, n_new = len(O), len(N)
    olen = [O.en[k] - O.st[k] for k in range(n_old)]
    nlen = [N.en[k] - N.st[k] for k in range(n_new)]
    match = [-1] * n_old
    how = [0] * n_old
    taken = bytearray(n_new)
    runs_of = {}

    def same(i, j, strict=False):
        """Diff runs when old[i] and new[j] are clean twins, else None;
        strict (an anchor to resync on) also wants 16+ bytes in common, so
        look-alike getters and thunks cannot anchor."""
        size = olen[i]
        if size != nlen[j] or (strict and size < ANCHOR_SHARED):
            return None
        if strict:
            return _clean(O.body(i), N.body(j), ANCHOR_SHARED, 4)
        return _clean(O.body(i), N.body(j), min(2, size), MAX_RUN)

    def resync(i, i_end, j, j_end):
        """Cheapest (dropped, added) after which strict twins start a run of
        matches, or None."""
        by_len = {}
        for jj in range(j, min(j_end, j + MAX_SKIP + 1)):
            if nlen[jj] >= ANCHOR_SHARED:
                by_len.setdefault(nlen[jj], []).append(jj)
        cands = sorted((ii - i + jj - j, ii, jj)
                       for ii in range(i, min(i_end, i + MAX_SKIP + 1))
                       for jj in by_len.get(olen[ii], ())
                       if ii - i + jj - j <= MAX_SKIP)
        for _, ii, jj in cands:
            if taken[jj] or same(ii, jj, True) is None:
                continue
            ahead = [t for t in range(1, LOOKAHEAD + 1)
                     if ii + t < i_end and jj + t < j_end]
            if sum(same(ii + t, jj + t) is not None for t in ahead) \
                    >= min(len(ahead), LOOKAHEAD_NEED):
                return ii - i, jj - j
        return None

    def walk(i, i_end, j, j_end):
        floor = j
        shift = None
        cooling = 0
        while i < i_end:
            runs = same(i, j) if j < j_end else None
            if runs is not None:
                match[i], how[i], taken[j], runs_of[i] = j, H_SEQ, 1, runs
                shift = N.st[j] - O.st[i]
                cooling = 0
                i += 1
                j += 1
                continue
            hit = None
            if not cooling:
                hit = resync(i, i_end, j, j_end)
                if hit is None and shift is not None:
                    # Lost by more than a resync can reach: look where the
                    # running address shift says the unit should be.
                    near = bisect.bisect_left(N.st, O.st[i] + shift) - 8
                    if abs(near - j) > 2 and near >= floor:
                        hit = resync(i, i_end, near, j_end)
                        if hit is not None:
                            hit = (hit[0], hit[1] + near - j)
            if hit is None:  # Edited in place: step over both unpaired.
                cooling = cooling - 1 if cooling else RESYNC_COOLDOWN
                i += 1
                j += 1
                continue
            i += hit[0]
            j += hit[1]

    prev_i = prev_j = -1
    for ai, aj in _unique_anchors(O, N, olen, nlen) + [(n_old, n_new)]:
        walk(prev_i + 1, ai, prev_j + 1, aj)
        if ai < n_old:
            match[ai], how[ai], taken[aj], runs_of[ai] = aj, H_SEQ, 1, []
        prev_i, prev_j = ai, aj

    # Left-overs: pair the ones with exactly one clean twin among the
    # unmatched new units of the same length, if no other left-over claims it.
    free = {}
    for jj in range(n_new):
        if not taken[jj] and nlen[jj] >= 24:
            free.setdefault(nlen[jj], []).append(jj)
    claims = {}
    for ii in range(n_old):
        if match[ii] < 0 and olen[ii] >= 24:
            twins = [jj for jj in free.get(olen[ii], ())
                     if same(ii, jj, True) is not None]
            if len(twins) == 1:
                claims.setdefault(twins[0], []).append(ii)
    for jj, olds in claims.items():
        if len(olds) == 1:
            match[olds[0]], how[olds[0]], taken[jj] = jj, H_MOVED, 1
    return match, how, runs_of


def _ambiguous_runs(O, N, match):
    """Look-alike units next to an edit: {old index: [new index, ...]}.

    Where a unit was added or dropped, a run of look-alike units beside it
    (clean twins of the added/dropped one: thunks, initialisers, clones) may
    have slipped by one place: their pairing then rests on order alone.
    Candidates are the unit's own partner first, then the others it could
    equally be paired with.
    """
    def twin(a, b):
        return len(a) == len(b) and _clean(a, b, min(8, len(a)), 4) is not None

    owner = {j: i for i, j in enumerate(match) if j >= 0}
    amb = {}
    for x in range(len(N)):
        if x in owner:
            continue
        b = N.body(x)
        for step in (-1, 1):
            j = x + step
            while 0 <= j < len(N) and j in owner and twin(O.body(owner[j]), b):
                amb.setdefault(owner[j], [j]).append(x)
                j += step
    for y in range(len(O)):
        if match[y] >= 0:
            continue
        a = O.body(y)
        for step in (-1, 1):
            k = y + step
            while 0 <= k < len(O) and match[k] >= 0 and twin(a, N.body(match[k])):
                amb.setdefault(k, [match[k]])
                amb.setdefault(y, []).append(match[k])
                k += step
    return amb


def _shingles(body):
    return {body[k:k + SHINGLE] for k in range(len(body) - SHINGLE + 1)}


def _dice(a, b):
    if not a or not b:
        return 0.0
    return 2.0 * len(a & b) / (len(a) + len(b))


def _alike(x, y):
    """Similarity of two [body, shingles-or-None] units: the share of equal
    bytes when they are the same length (displacement fields spoil the
    shingles of short ones), else the shingle overlap."""
    if len(x[0]) == len(y[0]):
        diff = sum(b - a for a, b in _runs(x[0], y[0])) if x[0] != y[0] else 0
        return 1.0 - diff / float(max(1, len(x[0])))
    for unit in (x, y):
        if unit[1] is None:
            unit[1] = _shingles(unit[0])
    return _dice(x[1], y[1])


def _pair_changed(O, N, match, how):
    """Pair the units the exact matcher left over, bracket by bracket.

    Between two matched neighbours the unmatched old and new units are the
    same code in the same order, edited: a lone pair is taken as is (the
    score says how alike), several are assigned by an order-preserving
    best-score alignment.  Returns {old index: score}.
    """
    scores = {}
    anchors = [i for i in range(len(O)) if match[i] >= 0 and how[i] != H_MOVED]
    taken = {match[i] for i in range(len(O)) if match[i] >= 0}
    prev_i, prev_j = -1, -1
    for i1 in anchors + [len(O)]:
        j1 = match[i1] if i1 < len(O) else len(N)
        if j1 < prev_j:  # A jump back: not an ordered bracket.
            prev_i, prev_j = i1, max(prev_j, j1)
            continue
        ol = [i for i in range(prev_i + 1, i1) if match[i] < 0]
        nl = [j for j in range(prev_j + 1, j1) if j not in taken]
        if ol and nl:
            sa = [[O.body(i), None] for i in ol]
            sb = [[N.body(j), None] for j in nl]
            lone = len(ol) == 1 and len(nl) == 1
            for p, q, score in _align_units(sa, sb, 0.0 if lone else 0.25):
                match[ol[p]], how[ol[p]] = nl[q], H_SIM
                scores[ol[p]] = score
        prev_i, prev_j = i1, j1
    return scores


def _align_units(sa, sb, floor):
    """Order-preserving assignment maximising total similarity (an
    alignment DP over a band around the diagonal, wide enough to absorb the
    difference in counts)."""
    rows, cols = len(sa), len(sb)
    half = max(24, abs(cols - rows) + 8)
    lo = [max(0, int(p * cols / float(rows)) - half) for p in range(rows)]
    hi = [min(cols, int(p * cols / float(rows)) + half + 1) for p in range(rows)]
    best = [dict() for _ in range(rows + 1)]
    sim = {}

    def value(p, q):
        return best[p].get(q, 0.0)
    for p in range(rows - 1, -1, -1):
        for q in range(hi[p] - 1, lo[p] - 1, -1):
            s = _alike(sa[p], sb[q])
            sim[p, q] = s
            take = s + value(p + 1, q + 1) if s >= floor else -1.0
            best[p][q] = max(take, value(p + 1, q), value(p, q + 1))
    pairs = []
    p = q = 0
    while p < rows and q < cols:
        if q < lo[p]:
            q = lo[p]
        if q >= hi[p]:
            p += 1
            continue
        s = sim[p, q]
        take = s + value(p + 1, q + 1) if s >= floor else -1.0
        if take >= 0 and take >= value(p + 1, q) and take >= value(p, q + 1):
            pairs.append((p, q, s))
            p += 1
            q += 1
        elif value(p + 1, q) >= value(p, q + 1):
            p += 1
        else:
            q += 1
    return pairs


def _chain(anchors):
    """Longest strictly increasing run of the second coordinate."""
    tails, tail_at, prev = [], [], [-1] * len(anchors)
    for k, (_, nb) in enumerate(anchors):
        pos = bisect.bisect_left(tails, nb)
        if pos == len(tails):
            tails.append(nb)
            tail_at.append(k)
        else:
            tails[pos] = nb
            tail_at[pos] = k
        prev[k] = tail_at[pos - 1] if pos else -1
    out = []
    k = tail_at[-1] if tail_at else -1
    while k >= 0:
        out.append(anchors[k])
        k = prev[k]
    out.reverse()
    return out


def _blocks(a, b):
    """Common chunks of two byte strings as sorted (old, new, length).

    Anchors are SHINGLE-byte windows unique in both; the longest chain of
    them in order, merged where they sit on one diagonal and then grown
    outward while bytes still agree, gives the chunks.  Displacement bytes
    fall in the gaps between chunks.
    """
    first_a, first_b = {}, {}
    for table, buf in ((first_a, a), (first_b, b)):
        for k in range(len(buf) - SHINGLE + 1):
            g = buf[k:k + SHINGLE]
            table[g] = -1 if g in table else k
    anchors = sorted((ka, first_b[g]) for g, ka in first_a.items()
                     if ka >= 0 and first_b.get(g, -1) >= 0)
    blocks = []
    for ka, kb in _chain(anchors):
        if blocks:
            o, n, size = blocks[-1]
            if ka - kb == o - n and ka <= o + size:
                blocks[-1] = (o, n, ka + SHINGLE - o)
                continue
            if ka < o + size or kb < n + size:
                continue
        blocks.append((ka, kb, SHINGLE))
    grown = []
    for k, (o, n, size) in enumerate(blocks):
        lo_a = grown[-1][0] + grown[-1][2] if grown else 0
        lo_b = grown[-1][1] + grown[-1][2] if grown else 0
        hi_a = blocks[k + 1][0] if k + 1 < len(blocks) else len(a)
        hi_b = blocks[k + 1][1] if k + 1 < len(blocks) else len(b)
        while o > lo_a and n > lo_b and a[o - 1] == b[n - 1]:
            o, n, size = o - 1, n - 1, size + 1
        while o + size < hi_a and n + size < hi_b and a[o + size] == b[n + size]:
            size += 1
        grown.append((o, n, size))
    return grown


def _place(blocks, off, new_len):
    """Place old offset off in the new body.

    Returns (new offset, how, lo, hi): inside a chunk the offset is exact;
    in the gap between chunks it is carried over when the gap kept its
    size and interpolated when not, with [lo, hi) the span it may fall in.
    """
    starts = [o for o, _, _ in blocks]
    k = bisect.bisect_right(starts, off) - 1
    if k >= 0 and off < blocks[k][0] + blocks[k][2]:
        o, n, _ = blocks[k]
        return n + off - o, "chunk", n + off - o, n + off - o + 1
    end_a = blocks[k][0] + blocks[k][2] if k >= 0 else 0
    end_b = blocks[k][1] + blocks[k][2] if k >= 0 else 0
    if k + 1 < len(blocks):
        gap_a, next_b = blocks[k + 1][0] - end_a, blocks[k + 1][1]
    else:
        gap_a, next_b = None, new_len
    gap_b = next_b - end_b
    if gap_a == gap_b:
        return end_b + off - end_a, "gap-same-size", end_b, next_b
    if gap_b <= 0:
        return end_b, "gap-interpolated", end_b, end_b + 1
    return min(end_b + off - end_a, next_b - 1), "gap-interpolated", end_b, next_b


def _read_rva(im, rva, size):
    """Bytes at rva, zero-filled past the raw data; None outside every section."""
    for start, span, raw, raw_size, _ in im.sections:
        if start <= rva < start + span:
            size = min(size, start + span - rva)
            delta = rva - start
            avail = max(0, min(size, raw_size - delta))
            return im.data[raw + delta:raw + delta + avail] + bytes(size - avail)
    return None


# One-byte opcodes that carry a ModRM byte; two-byte (0F) and VEX/EVEX forms
# are recognised by their escape bytes in _modrm_follows.
_MODRM_OPS = frozenset(
    [b for lo in range(0x00, 0x40, 8) for b in range(lo, lo + 4)]
    + [0x62, 0x63, 0x69, 0x6B] + list(range(0x80, 0x90))
    + [0xC0, 0xC1, 0xC6, 0xC7, 0xD0, 0xD1, 0xD2, 0xD3, 0xF6, 0xF7, 0xFE, 0xFF]
    + list(range(0xD8, 0xE0)))


def _modrm_follows(a, m):
    """True when the byte at m can be the ModRM byte of an instruction (the
    opcode before it takes one)."""
    if m < 1:
        return False
    return (a[m - 1] in _MODRM_OPS
            or (m >= 2 and a[m - 2] == 0x0F)
            or (m >= 3 and a[m - 3] == 0x0F and a[m - 2] in (0x38, 0x3A))
            or (m >= 3 and a[m - 3] == 0xC5)
            or (m >= 4 and a[m - 4] == 0xC4)
            or (m >= 5 and a[m - 5] == 0x62))


def _field_kinds(a, f):
    """Field kinds the bytes before offset f make plausible for a disp32 there.

    E8/E9 and 0F 8x are rel32 branches; a ModRM byte of mod 00, rm 101 is
    RIP-relative; mod 10 (with or without a SIB byte) is [base+disp32],
    which the compiler uses with the image base held in a register, so the
    disp32 is an RVA there.
    """
    kinds = []
    p = a[f - 1] if f else 0
    if p in (0xE8, 0xE9) or (f >= 2 and a[f - 2] == 0x0F and 0x80 <= p <= 0x8F) \
            or ((p & 0xC7) == 0x05 and _modrm_follows(a, f - 1)):
        kinds.append(REL)
    elif (p & 0xC0) == 0x80 and (p & 7) != 4 and _modrm_follows(a, f - 1) \
            or (f >= 2 and (a[f - 2] & 0xC0) == 0x80 and (a[f - 2] & 7) == 4
                and _modrm_follows(a, f - 2)):
        kinds.append(RVA)
    return kinds


def _imm_tail(a, f):
    """Immediate bytes after a RIP-relative disp32 at f, from its opcode
    (best effort: the common ALU/MOV/TEST/shift immediates)."""
    if f < 2:
        return 0
    modrm, op = a[f - 1], a[f - 2]
    reg = (modrm >> 3) & 7
    wide = 2 if (f >= 3 and a[f - 3] == 0x66) \
        or (f >= 4 and a[f - 4] == 0x66 and 0x40 <= a[f - 3] <= 0x4F) else 4
    if f >= 3 and a[f - 3] == 0x0F:
        return 1 if op in (0x70, 0x71, 0x72, 0x73, 0xA4, 0xAC, 0xBA, 0xC2,
                           0xC4, 0xC5, 0xC6) else 0
    if f >= 4 and a[f - 4] == 0x0F and a[f - 3] == 0x3A:
        return 1
    if op in (0x80, 0x82, 0x83, 0xC0, 0xC1, 0x6B) or (op in (0xC6, 0xF6) and reg < 2):
        return 1
    if op in (0x81, 0x69) or (op in (0xC7, 0xF7) and reg < 2):
        return wide
    return 0


class BuildMap:
    """Old-build to new-build map of code units, built once per exe pair."""

    def __init__(self, old_data, new_data, state=None):
        self.old, self.new = Image(old_data), Image(new_data)
        self.O, self.N = _Units(self.old), _Units(self.new)
        self.seconds = {}
        self._sections()
        if state is not None:
            self._restore(state)
            return
        clock = time.time()

        def lap(name):
            nonlocal clock
            self.seconds[name] = round(time.time() - clock, 1)
            clock = time.time()
        self.match, self.how, self.runs_of = _match_units(self.O, self.N)
        lap("walk")
        self.score = _pair_changed(self.O, self.N, self.match, self.how)
        self.amb = _ambiguous_runs(self.O, self.N, self.match)
        lap("pair")
        self._neighbours()
        self._learn_deltas()
        lap("learn")
        self._verify()
        lap("verify")
        self.refined = self._rematch()
        lap("rematch")
        if self.refined:
            self._neighbours()
            self._verify()
            lap("reverify")

    # -- cache ---------------------------------------------------------------

    def state(self):
        """JSON-able result of the build, for --cache."""
        return {
            "match": self.match, "how": self.how, "refined": self.refined,
            "score": [[i, s] for i, s in self.score.items()],
            "amb": [[i, c] for i, c in self.amb.items()],
            "status": [STATUSES.index(s) for s in self.status],
            "bad": [[i, r] for i, r in self.bad.items()],
            "pairs": [[o, sorted(n)] for o, n in self.data_pairs.items()],
            "votes": [[[b, [[d, n] for d, n in row.items()]]
                       for b, row in sec.items()] for sec in self.votes],
            "seconds": self.seconds}

    def _restore(self, state):
        self.match, self.how = state["match"], state["how"]
        self.refined = state["refined"]
        self.score = {i: s for i, s in state["score"]}
        self.amb = {i: c for i, c in state["amb"]}
        self.status = [STATUSES[k] for k in state["status"]]
        self.bad = {i: [tuple(r) for r in runs] for i, runs in state["bad"]}
        self.data_pairs = {o: set(n) for o, n in state["pairs"]}
        self.data_keys = sorted(self.data_pairs)
        self.votes = [{b: {d: n for d, n in row} for b, row in sec}
                      for sec in state["votes"]]
        self.modes = [dict() for _ in self.sec_start]
        self.runs_of = {}
        self.seconds = dict(state["seconds"], cached=True)
        self._neighbours()

    # -- tables the verifier reads -------------------------------------------

    def _sections(self):
        """Old section spans, their new counterparts (by name), exec flags."""
        old_rows, new_rows = section_table(self.old), section_table(self.new)
        new_by_name = {row["name"]: n for n, row in enumerate(new_rows)}
        self.sec_start, self.sec_end, self.sec_exec = [], [], []
        self.sec_new, self.sec_name, self.sec_new_index = [], [], []
        for n, row in enumerate(old_rows):
            rva, span = self.old.sections[n][0], self.old.sections[n][1]
            self.sec_start.append(rva)
            self.sec_end.append(rva + span)
            self.sec_exec.append(bool(self.old.sections[n][4] & SCN_EXEC))
            self.sec_name.append(row["name"])
            m = new_by_name.get(row["name"])
            self.sec_new_index.append(m)
            self.sec_new.append(None if m is None else
                                (self.new.sections[m][0],
                                 self.new.sections[m][0] + self.new.sections[m][1]))

    def _neighbours(self):
        """Nearest ordered-matched unit at or before / after each old unit."""
        n = len(self.O)
        anchor = [self.match[i] >= 0 and self.how[i] != H_MOVED for i in range(n)]
        self.prev_hit, self.next_hit = [-1] * n, [-1] * n
        last = -1
        for i in range(n):
            if anchor[i]:
                last = i
            self.prev_hit[i] = last
        last = -1
        for i in range(n - 1, -1, -1):
            if anchor[i]:
                last = i
            self.next_hit[i] = last

    def _section_of(self, rva):
        k = bisect.bisect_right(self.sec_start, rva) - 1
        if k >= 0 and rva < self.sec_end[k]:
            return k
        return -1

    # -- field decoding ------------------------------------------------------

    def _candidates(self, a, x, lo, generic):
        """(f, kind) fields that could cover differing byte x, most plausible
        first: those the preceding bytes support, then (if generic) any
        alignment."""
        spans = [f for f in range(x, max(lo, x - 3, 0) - 1, -1)
                 if f + 4 <= len(a)]
        for f in spans:
            for kind in _field_kinds(a, f):
                yield f, kind
        if generic:
            for f in spans:
                if not _field_kinds(a, f):
                    yield f, REL
                    yield f, RVA

    def _learn_deltas(self):
        """Vote the new-minus-old displacement of every data reference.

        Data moves by piecewise-constant amounts (a section shift plus small
        insertions before the variable), so the commonest deltas among the
        references near an address say how far that address moved.  A run
        only votes when one field alignment is plausible for it.
        """
        self.votes = [dict() for _ in self.sec_start]
        self.modes = [dict() for _ in self.sec_start]
        O, N, match = self.O, self.N, self.match
        for i in range(len(O)):
            j = match[i]
            if j < 0 or O.en[i] - O.st[i] != N.en[j] - N.st[j]:
                continue
            a, b = O.body(i), N.body(j)
            if a == b:
                continue
            us, vs = O.st[i], N.st[j]
            for x, y in self._runs(i, a, b):
                cands = [(f, kind)
                         for f in range(max(0, y - 4), min(x, len(a) - 4) + 1)
                         for kind in _field_kinds(a, f)]
                if len(cands) != 1:
                    continue
                f, kind = cands[0]
                od, nd = _I32(a, f)[0], _I32(b, f)[0]
                if kind == REL:
                    ot, nt = us + f + 4 + od, vs + f + 4 + nd
                else:
                    ot, nt = od, nd
                k = self._section_of(ot)
                if k < 0 or self.sec_exec[k] or self.sec_new[k] is None:
                    continue
                row = self.votes[k].setdefault((ot - self.sec_start[k]) // DATA_BLOCK, {})
                row[nt - ot] = row.get(nt - ot, 0) + 1

    def _runs(self, i, a, b):
        runs = self.runs_of.get(i)
        return _runs(a, b) if runs is None else runs

    def _expected(self, k, ot):
        """Deltas a data reference to ot may carry: the commonest ones among
        the votes within a few blocks of it, widening until some repeat."""
        block = (ot - self.sec_start[k]) // DATA_BLOCK
        got = self.modes[k].get(block)
        if got is None:
            total = {}
            good = []
            for radius in (2, 16, 128):
                total = {}
                for near in range(block - radius, block + radius + 1):
                    for delta, n in self.votes[k].get(near, {}).items():
                        total[delta] = total.get(delta, 0) + n
                good = sorted(((n, d) for d, n in total.items() if n >= 2),
                              reverse=True)[:3]
                if good:
                    break
            got = frozenset(d for _, d in good) or frozenset(
                [self.sec_new[k][0] - self.sec_start[k]])
            self.modes[k][block] = got
        return got

    # -- verification --------------------------------------------------------

    def _code_target(self, ot, nt):
        """0 no, 1 exact counterpart, 2 inside the counterpart's span."""
        O, N = self.O, self.N
        u = O.at(ot)
        if u < 0:
            return 0
        v = self.match[u]
        if v >= 0:
            if ot == O.st[u] or O.en[u] - O.st[u] == N.en[v] - N.st[v]:
                return 1 if nt == N.st[v] + ot - O.st[u] else 0
            return 2 if N.st[v] <= nt < N.en[v] else 0
        p, q = self.prev_hit[u], self.next_hit[u]
        lo = N.en[self.match[p]] if p >= 0 else 0
        hi = N.st[self.match[q]] if q >= 0 else 1 << 32
        return 2 if lo <= nt < hi else 0

    def _target_ok(self, ot, nt):
        """(tag, section): tag 0 unverified, 1 code exact, 2 code in span,
        3 data, 4 image base."""
        if ot == nt == 0:
            return 4, None  # lea reg, [__ImageBase]
        k = self._section_of(ot)
        if k < 0:
            return 0, None
        if self.sec_exec[k]:
            return self._code_target(ot, nt), None
        new = self.sec_new[k]
        if new is None or not new[0] <= nt < new[1]:
            return 0, None
        if nt - ot in self._expected(k, ot):
            return 3, k
        return 0, None

    def _explain(self, i, j, a, b, runs, record=None, generic=True):
        """Explain each differing run as a displacement field.

        Returns (unexplained runs, counts) with counts = [code exact,
        code in span, data, image base].  record, when given, collects
        {old data target: {new data target}}.
        """
        us, vs = self.O.st[i], self.N.st[j]
        pos = 0
        bad = []
        counts = [0, 0, 0, 0]
        for x, y in runs:
            x = max(x, pos)
            while x < y:
                hit = None
                for f, kind in self._candidates(a, x, pos, generic):
                    od, nd = _I32(a, f)[0], _I32(b, f)[0]
                    if kind == RVA or (f and a[f - 1] in (0xE8, 0xE9)) \
                            or (f >= 2 and a[f - 2] == 0x0F):
                        tails = (0,)
                    else:
                        t = _imm_tail(a, f)
                        tails = (t,) + tuple(v for v in (0, 1, 2, 4) if v != t)
                    for tail in tails:
                        if kind == REL:
                            ot, nt = us + f + 4 + tail + od, vs + f + 4 + tail + nd
                        else:
                            ot, nt = od, nd
                        tag, _ = self._target_ok(ot, nt)
                        if tag:
                            hit = (f, tag, ot, nt)
                            break
                    if hit:
                        break
                if hit is None:
                    bad.append((x, y))
                    break
                f, tag, ot, nt = hit
                counts[tag - 1] += 1
                if tag == 3 and record is not None:
                    record.setdefault(ot, set()).add(nt)
                pos = f + 4
                x = pos
        return bad, counts

    def _rematch(self):
        """Re-pair units whose displacements do not add up.

        Look-alike units (static initialisers, thunks) pair up by order, but
        an update can shuffle them or slip them by one; their own call and
        data targets say which new unit each really is.  Every unit that
        failed to verify (or never paired) tries the nearby new units of its
        length and takes the one whose every difference verifies against an
        exact call target or the expected data delta.  A unit holding the
        candidate gives it up only if it failed too.  Returns the number of
        units re-paired.
        """
        O, N = self.O, self.N
        by_len = {}
        for j in range(len(N)):
            starts, idxs = by_len.setdefault(N.en[j] - N.st[j], ([], []))
            starts.append(N.st[j])
            idxs.append(j)
        owner = {j: i for i, j in enumerate(self.match) if j >= 0}
        todo = {i for i in range(len(O)) if O.en[i] - O.st[i] >= 12
                and (self.match[i] < 0
                     or (self.status[i] == CHANGED and i in self.bad))}
        fixed = set()
        moved = 0
        for sweep in range(2):
            changed = False
            for i in sorted(todo - fixed, reverse=bool(sweep & 1)):
                a = O.body(i)
                p = self.prev_hit[i]
                shift = (N.st[self.match[p]] - O.st[p]) if p >= 0 else 0
                exp = O.st[i] + shift
                starts, idxs = by_len.get(len(a), ((), ()))
                first = bisect.bisect_left(starts, exp - REMATCH_RADIUS)
                last = bisect.bisect_right(starts, exp + REMATCH_RADIUS)
                near = sorted(range(first, last),
                              key=lambda k: abs(starts[k] - exp))[:REMATCH_TRIES]
                found = []
                for k in near:
                    j = idxs[k]
                    holder = owner.get(j, -1)
                    if holder == i or (holder >= 0 and (holder not in todo
                                                        or holder in fixed)):
                        continue
                    b = N.body(j)
                    runs = _clean(a, b, min(2, len(a)), MAX_RUN)
                    if runs is None:
                        continue
                    left, counts = self._explain(i, j, a, b, runs, None, False)
                    if not left and counts[0] + counts[2]:
                        found.append((-(counts[0] + counts[2]),
                                      abs(starts[k] - exp), j))
                if not found:
                    continue
                found.sort()
                j = found[0][2]
                holder = owner.get(j, -1)
                if holder >= 0:
                    self.match[holder] = -1
                    self.runs_of.pop(holder, None)
                if self.match[i] >= 0:
                    owner.pop(self.match[i], None)
                self.match[i], self.how[i] = j, H_SEQ
                self.runs_of.pop(i, None)
                owner[j] = i
                fixed.add(i)
                changed = True
                moved += 1
                if len(found) > 1 and found[1][:1] == found[0][:1]:
                    self.amb[i] = [j] + [f[2] for f in found[1:]]
            if not changed:
                break
        return moved

    def _verify(self):
        """Classify every old unit and collect the data references that
        fully verified units show (their old/new targets)."""
        n = len(self.O)
        self.status = [NOT_FOUND] * n
        self.bad = {}
        self.data_pairs = {}
        for i in range(n):
            j = self.match[i]
            if j < 0:
                self.status[i] = AMBIGUOUS if i in self.amb else NOT_FOUND
                continue
            a, b = self.O.body(i), self.N.body(j)
            if len(a) != len(b):
                self.status[i] = CHANGED
                continue
            if a == b:
                self.status[i] = AMBIGUOUS if i in self.amb else IDENTICAL
                continue
            pairs = {}
            bad, counts = self._explain(i, j, a, b, self._runs(i, a, b), pairs)
            if bad:
                self.status[i] = CHANGED
                self.bad[i] = bad
            elif i in self.amb and not counts[0]:
                self.status[i] = AMBIGUOUS
            else:
                self.status[i] = RELOC_ONLY
                self.amb.pop(i, None)
                for target, news in pairs.items():
                    self.data_pairs.setdefault(target, set()).update(news)
        self.data_keys = sorted(self.data_pairs)

    # -- queries -------------------------------------------------------------

    def summary(self):
        counts = {name: 0 for name in STATUSES}
        for status in self.status:
            counts[status] += 1
        matched = {j for j in self.match if j >= 0}
        counts["new-only"] = len(self.N) - len(matched)
        return counts

    def lookup(self, rva):
        k = self._section_of(rva)
        if k < 0:
            return {"rva": rva, "kind": "none", "status": NOT_FOUND,
                    "new_rva": None, "detail": "rva is outside every section"}
        if self.sec_exec[k]:
            return self._lookup_code(rva)
        return self._lookup_data(rva, k)

    def _window(self, u, off, new_body, new_off):
        """Byte difference between the 24 bytes at T and at its placement."""
        a = self.O.body(u)[off:off + 24]
        b = new_body[new_off:new_off + 24]
        if len(a) != len(b) or not a:
            return "window n/a"
        return "24B at T: %d differ" % sum(1 for p, q in zip(a, b) if p != q)

    def _lookup_code(self, rva):
        O, N = self.O, self.N
        u = O.at(rva)
        res = {"rva": rva, "kind": "code", "new_rva": None, "placed_rva": None,
               "candidate": None, "score": None, "how": None, "detail": ""}
        if u < 0:
            res.update(status=NOT_FOUND,
                       detail="rva is in padding between code units")
            return res
        off = rva - O.st[u]
        size = O.en[u] - O.st[u]
        res["old_unit"] = (O.st[u], size, "leaf" if O.leaf[u] else "pdata")
        status = self.status[u]
        res["status"] = status
        j = self.match[u]
        if status == AMBIGUOUS:
            cands = [c for c in self.amb.get(u, []) if c >= 0]
            res["detail"] = "look-alike new units at " + ", ".join(
                "0x%X" % N.st[c] for c in cands[:6])
            res["candidates"] = [N.st[c] + off for c in cands[:6]]
            return res
        if j < 0:
            res["detail"] = self._best_guess(u)
            return res
        res["how"] = HOW_NAMES[self.how[u]]
        nsize = N.en[j] - N.st[j]
        res["candidate"] = (N.st[j], nsize)
        if status in (IDENTICAL, RELOC_ONLY):
            res["new_rva"] = N.st[j] + off
            if status == RELOC_ONLY:
                a, b = O.body(u), N.body(j)
                _, counts = self._explain(u, j, a, b, _runs(a, b))
                res["detail"] = ("%d code refs exact, %d code refs in span, "
                                 "%d data refs, %d image-base refs" % tuple(counts))
            return res
        new_body = N.body(j)
        if off == 0:
            placed, method = N.st[j], "entry"
            lo, hi = placed, placed + 1
            tail = self._window(u, 0, new_body, 0)
        elif size == nsize:
            placed, method = N.st[j] + off, "same-length"
            lo, hi = placed, placed + 1
            tail = self._window(u, off, new_body, off)
        else:
            blocks = _blocks(O.body(u), new_body)
            noff, method, lo, hi = _place(blocks, off, nsize)
            placed, lo, hi = N.st[j] + noff, N.st[j] + lo, N.st[j] + hi
            res["score"] = sum(n for _, _, n in blocks) / float(size)
            tail = self._window(u, off, new_body, noff)
        if u in self.score:
            res["score"] = max(res["score"] or 0.0, self.score[u])
        res["placed_rva"] = placed
        res["placement"] = method
        res["placement_span"] = (lo, hi)
        diffs = self.bad.get(u)
        note = ""
        if diffs:
            note = "; %d unexplained diff run(s) at +%s" % (
                len(diffs), ",".join("0x%X" % x for x, _ in diffs[:4]))
        res["detail"] = "%s; %s%s" % (method, tail, note)
        return res

    def _best_guess(self, u):
        """Most similar unmatched new unit between u's matched neighbours."""
        O, N = self.O, self.N
        p, q = self.prev_hit[u], self.next_hit[u]
        lo = self.match[p] + 1 if p >= 0 else 0
        hi = self.match[q] if q >= 0 else len(N)
        if hi - lo > 64:
            return "no unit paired; neighbours too far apart to guess"
        taken = {m for m in self.match if m >= 0}
        shingles = _shingles(O.body(u))
        guesses = sorted(((_dice(shingles, _shingles(N.body(c))), c)
                          for c in range(lo, hi) if c not in taken),
                         reverse=True)[:3]
        if not guesses:
            return "no unit paired; no unmatched new unit in its bracket"
        return "no unit paired; best guesses " + ", ".join(
            "0x%X (score %.2f)" % (N.st[c], s) for s, c in guesses)

    # -- data ----------------------------------------------------------------

    def _map_pointer(self, ptr):
        """New VA of the function an old code pointer names, or None."""
        u = self.O.at(ptr - IMAGE_BASE)
        if u < 0 or self.O.st[u] != ptr - IMAGE_BASE or self.match[u] < 0:
            return None
        return IMAGE_BASE + self.N.st[self.match[u]]

    def _vtable(self, rva, new_rva, k):
        """If the old data at rva starts with code pointers, map them and
        report how many the new data at new_rva (or a search) confirms."""
        raw = _read_rva(self.old, rva, 8 * VTABLE_CAP) or b""
        slots = []
        for p in range(0, len(raw) - 7, 8):
            ptr = int.from_bytes(raw[p:p + 8], "little")
            if ptr < IMAGE_BASE or self.O.at(ptr - IMAGE_BASE) < 0:
                break
            slots.append(ptr)
        if len(slots) < 2:
            return None
        mapped = [self._map_pointer(s) for s in slots]
        first = next((n for n, m in enumerate(mapped) if m is not None), None)
        info = {"slots": len(slots), "mapped": sum(m is not None for m in mapped),
                "at_new": None, "found": []}
        if first is None:
            return info

        def confirmed(base):
            got = _read_rva(self.new, base, 8 * len(slots))
            if got is None:
                return 0
            return sum(1 for n, m in enumerate(mapped) if m is not None
                       and int.from_bytes(got[8 * n:8 * n + 8], "little") == m)
        if new_rva is not None:
            info["at_new"] = confirmed(new_rva)
        if info["at_new"] != info["mapped"] and self.sec_new_index[k] is not None:
            sec = self.new.sections[self.sec_new_index[k]]
            blob = self.new.data[sec[2]:sec[2] + sec[3]]
            run = []
            for m in mapped[first:first + 4]:
                if m is None:
                    break
                run.append(m.to_bytes(8, "little"))
            pat = b"".join(run)
            pos = blob.find(pat)
            while pos >= 0 and len(info["found"]) < 4:
                info["found"].append(sec[0] + pos - 8 * first)
                pos = blob.find(pat, pos + 1)
        return info

    def _content_hits(self, k, window, lo, hi):
        """New RVAs in [lo, hi) of section k where the old bytes recur."""
        m = self.sec_new_index[k]
        sec = self.new.sections[m]
        blob = self.new.data[sec[2]:sec[2] + sec[3]]
        hits = []
        pos = blob.find(window, max(0, lo - sec[0]), max(0, hi - sec[0]))
        while pos >= 0 and len(hits) < 4:
            hits.append(sec[0] + pos)
            pos = blob.find(window, pos + 1, max(0, hi - sec[0]))
        return hits

    def _lookup_data(self, rva, k):
        res = {"rva": rva, "kind": "data", "new_rva": None, "status": NOT_FOUND,
               "section": self.sec_name[k], "detail": ""}
        new = self.sec_new[k]
        if new is None:
            res["detail"] = "section %s has no counterpart" % self.sec_name[k]
            return res
        keys = self.data_keys
        pos = bisect.bisect_left(keys, rva)
        exact = self.data_pairs.get(rva)

        def anchored(indices):
            """(old, delta) of the first single-valued pair in this section."""
            for n in indices:
                key = keys[n]
                if self._section_of(key) != k:
                    return None
                targets = self.data_pairs[key]
                if len(targets) == 1:
                    return key, next(iter(targets)) - key
            return None
        below = anchored(range(pos - 1, max(pos - 33, -1), -1))
        above = anchored(range(pos + (exact is not None), min(pos + 33, len(keys))))
        old_win = _read_rva(self.old, rva, 32) or b""
        zero = not any(old_win)
        plain = len(set(old_win)) < 6  # Too repetitive to search for.

        def equal(new_rva):
            win = _read_rva(self.new, new_rva, 32)
            if win is None:
                return 0
            return sum(1 for p, q in zip(old_win, win) if p == q)
        deltas = []
        agree = False
        if exact is not None and len(exact) == 1:
            res.update(status="XREF-EXACT", new_rva=next(iter(exact)),
                       detail="referenced from code in both builds")
        else:
            res["neighbours"] = {
                "below": below and {"old": below[0], "delta": below[1]},
                "above": above and {"old": above[0], "delta": above[1]}}
            deltas = sorted({pair[1] for pair in (below, above) if pair})
            if not deltas:
                deltas = [new[0] - self.sec_start[k]]
            if len(deltas) == 1:
                agree = bool(below and above)
                side = "both neighbours" if agree else (
                    "the neighbour" if below or above else "the section shift")
                res.update(status="XREF-BRACKET", new_rva=rva + deltas[0],
                           detail="delta 0x%X from %s" % (deltas[0], side))
            else:
                scores = [equal(rva + d) for d in deltas]
                best = [d for d, s in zip(deltas, scores) if s == max(scores)]
                if len(best) == 1 and max(scores) >= CONTENT_GOOD and not zero:
                    res.update(status="XREF-BRACKET", new_rva=rva + best[0],
                               detail="neighbours disagree (deltas %s); content "
                               "picks 0x%X" % (", ".join("0x%X" % d for d in deltas),
                                              best[0]))
                else:
                    res.update(status=AMBIGUOUS, candidates=[rva + d for d in deltas],
                               detail="neighbours disagree (deltas %s)"
                               % ", ".join("0x%X" % d for d in deltas))
        new_rva = res["new_rva"]
        if new_rva is not None:
            got = equal(new_rva)
            res["detail"] += "; " + ("content: zero fill (uninformative)" if zero
                                     else "content: %d/32 bytes equal at new" % got)
            if res["status"] == "XREF-BRACKET" and not agree \
                    and not zero and got < CONTENT_GOOD:
                res["status"] = "UNCONFIRMED"
        if res["status"] in (AMBIGUOUS, "UNCONFIRMED") and not zero and not plain \
                and len(old_win) == 32:
            near = [rva + d for d in deltas] or [rva + new[0] - self.sec_start[k]]
            hits = self._content_hits(k, old_win, min(near) - 0x800, max(near) + 0x800)
            if len(hits) == 1:
                res.update(status="CONTENT-SEARCH", new_rva=hits[0])
                res["detail"] += "; the 32 bytes recur once nearby, at 0x%X" % hits[0]
        vt = self._vtable(rva, res["new_rva"], k)
        if vt is not None:
            res["vtable"] = vt
            text = "vtable: %d slots%s, %d map to new code" % (
                vt["slots"], "+" if vt["slots"] == VTABLE_CAP else "", vt["mapped"])
            if vt["at_new"] is not None:
                text += ", %d confirmed at new" % vt["at_new"]
            if vt["found"]:
                text += ", pointer search found %s" % ",".join(
                    "0x%X" % h for h in vt["found"])
                if len(vt["found"]) == 1 and res["status"] in (NOT_FOUND, AMBIGUOUS):
                    res.update(status="VTABLE-SEARCH", new_rva=vt["found"][0])
            if res["status"] == "UNCONFIRMED" and vt["mapped"] >= 2 \
                    and vt["at_new"] == vt["mapped"]:
                res["status"] = "XREF-BRACKET"  # The pointers confirm the delta.
            res["detail"] += "; " + text
        return res


CACHE_VERSION = 1  # Bump when the matcher changes what it stores or decides.


def build_map(old_path, new_path, cache=None):
    """BuildMap for two exes, reusing/writing a --cache JSON when it matches
    both files' SHA-256 and CACHE_VERSION."""
    old_data = Path(old_path).read_bytes()
    new_data = Path(new_path).read_bytes()
    key = [hashlib.sha256(old_data).hexdigest(),
           hashlib.sha256(new_data).hexdigest(), CACHE_VERSION]
    if cache and Path(cache).is_file():
        try:
            doc = json.loads(Path(cache).read_text(encoding="utf-8"))
            if doc.get("key") == key:
                return BuildMap(old_data, new_data, doc["state"])
        except (ValueError, KeyError, TypeError):
            pass  # A stale or damaged cache is rebuilt, not trusted.
    bm = BuildMap(old_data, new_data)
    if cache:
        Path(cache).write_text(json.dumps({"key": key, "state": bm.state()}),
                               encoding="utf-8")
    return bm


def read_rvas(path):
    """RVAs from a file: one per line, optional trailing name, # comments."""
    rvas = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            rvas.append(int(line.split()[0], 0))
    return rvas


def _hex(value):
    return None if value is None else "0x%X" % value


def result_row(res):
    """One table line for a lookup result."""
    if res.get("new_rva") is not None:
        new = "0x%X" % res["new_rva"]
    elif res.get("placed_rva") is not None:
        new = "~0x%X" % res["placed_rva"]
    else:
        new = "-"
    parts = [res["detail"]] if res["detail"] else []
    if res.get("how") and res["status"] in (IDENTICAL, RELOC_ONLY, CHANGED):
        parts.insert(0, res["how"])
    if res.get("candidate") and res["status"] == CHANGED:
        parts.insert(0, "unit 0x%X (%d bytes)%s" % (
            res["candidate"][0], res["candidate"][1],
            " score %.2f" % res["score"] if res.get("score") is not None else ""))
    return "%-12s %-12s %-11s %s" % ("0x%X" % res["rva"], new, res["status"],
                                    "; ".join(parts))


def result_json(res):
    """JSON-ready copy of a lookup result: RVAs as hex strings."""
    out = {}
    for key, value in res.items():
        if key in ("rva", "new_rva", "placed_rva"):
            out[key] = _hex(value)
        elif key in ("old_unit", "candidate"):
            out[key] = None if value is None else {
                "start": _hex(value[0]), "length": value[1]}
        elif key == "placement_span":
            out[key] = [_hex(value[0]), _hex(value[1])]
        elif key == "candidates":
            out[key] = [_hex(v) for v in value]
        elif key == "neighbours":
            out[key] = {side: pair and {"old": _hex(pair["old"]),
                                        "delta": _hex(pair["delta"])}
                        for side, pair in value.items()}
        elif key == "vtable":
            out[key] = dict(value, at_new=value["at_new"],
                            found=[_hex(v) for v in value["found"]])
        else:
            out[key] = value
    return out


def write_function_map(bm, path):
    """CSV of every old code unit: where it went and how sure the map is."""
    O, N = bm.O, bm.N
    lines = ["old_start,old_len,new_start,status,new_len,kind,how,score"]
    for i in range(len(O)):
        j = bm.match[i]
        lines.append("0x%X,%d,%s,%s,%s,%s,%s,%s" % (
            O.st[i], O.en[i] - O.st[i],
            "0x%X" % N.st[j] if j >= 0 else "", bm.status[i],
            N.en[j] - N.st[j] if j >= 0 else "",
            "leaf" if O.leaf[i] else "pdata",
            HOW_NAMES.get(bm.how[i], "") if j >= 0 else "",
            "%.2f" % bm.score[i] if i in bm.score else ""))
    matched = {j for j in bm.match if j >= 0}
    for j in range(len(N)):
        if j not in matched:
            lines.append(",,0x%X,ADDED,%d,%s,," % (
                N.st[j], N.en[j] - N.st[j], "leaf" if N.leaf[j] else "pdata"))
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")


def _check_seed_baseline(old_path, targets_path):
    """Refuse --seed unless --old is the build the seed RVAs belong to.

    TARGET_SEED is the target list of the baseline in targets_path, so its
    RVAs are only old-build addresses when --old has that baseline's PE
    timestamp and image size (the pair diff compares).
    """
    base = json.loads(Path(targets_path).read_text(encoding="utf-8"))["baseline"]
    timestamp, image_size = read_identity(Image(Path(old_path).read_bytes()))
    if (timestamp, image_size) == (base["timestamp"], base["image_size"]):
        return
    name = base.get("exe")
    raise ValueError(
        "--seed refused: the seed RVAs belong to baseline %s (timestamp 0x%X, "
        "image size 0x%X), but --old %s has timestamp 0x%X, image size 0x%X; "
        "pass that build as --old, or give explicit --rva values for this build"
        % (name or "(unnamed)", base["timestamp"], base["image_size"],
           Path(old_path).name, timestamp, image_size))


def map_rvas(args):
    """The `map` subcommand body: build the map, print/write what was asked."""
    rvas = list(args.rva or [])
    if args.rvas_file:
        rvas += read_rvas(args.rvas_file)
    if args.seed:
        _check_seed_baseline(args.old, args.targets)
        rvas += [t["rva"] for t in TARGET_SEED]
    if not rvas and not args.all_functions:
        raise ValueError("map needs --rva, --rvas-file, --seed or --all-functions")
    bm = build_map(args.old, args.new, args.cache)
    print("old %s: %d code units; new %s: %d code units; map %s %.0fs"
          % (Path(args.old).name, len(bm.O), Path(args.new).name, len(bm.N),
             "loaded from cache (built in" if bm.seconds.get("cached")
             else "built in",
             sum(v for k, v in bm.seconds.items() if k != "cached"))
          + (")" if bm.seconds.get("cached") else ""))
    results = [bm.lookup(rva) for rva in rvas]
    if results:
        print("%-12s %-12s %-11s %s" % ("old RVA", "new RVA", "status", "detail"))
        for res in results:
            print(result_row(res))
    if args.json:
        Path(args.json).write_text(
            json.dumps([result_json(r) for r in results], indent=1) + "\n",
            encoding="utf-8")
    if args.all_functions:
        write_function_map(bm, args.all_functions)
    counts = bm.summary()
    print("units: " + ", ".join("%s %d" % (name, counts[name]) for name in STATUSES)
          + "; new-only %d" % counts["new-only"])
    return 0


def _fixture_pe(timestamp=0x11111111, image_size=0x100000, text_rva=0x1000,
                text_raw=0x400, text_size=0x4000, extra_sections=(),
                exception_dir=None):
    """Minimal PE32+ with one .text section openxr_pe.Image accepts.

    extra_sections rows are (name, rva, span, raw[, characteristics]);
    exception_dir is the (rva, size) of the .pdata table, if any."""
    size = text_raw + text_size
    sections = [(".text", text_rva, text_size, text_raw)] + list(extra_sections)
    for row in sections:
        size = max(size, row[3] + row[2])
    data = bytearray(size)

    def put(off, value, size_=4):
        data[off:off + size_] = int(value).to_bytes(size_, "little")

    data[:2] = b"MZ"
    put(60, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    put(0x84, 0x8664, 2)
    put(0x86, len(sections), 2)
    put(0x88, timestamp)
    put(0x94, 240, 2)
    put(0x98, 0x20B, 2)
    put(0x98 + 56, image_size)
    put(0x98 + 60, text_raw)
    put(0x98 + 108, 16)
    table = 0x98 + 240
    if exception_dir:
        put(0x98 + 112 + 3 * 8, exception_dir[0])
        put(0x98 + 112 + 3 * 8 + 4, exception_dir[1])
    for i, row in enumerate(sections):
        name, rva, span, raw = row[:4]
        off = table + i * 40
        data[off:off + 8] = name.encode("ascii")[:8].ljust(8, b"\0")
        put(off + 8, span)
        put(off + 12, rva)
        put(off + 16, span)
        put(off + 20, raw)
        put(off + 36, row[4] if len(row) > 4 else 0x60000020)
    for i in range(text_size):
        data[text_raw + i] = (i * 7 + 3) & 0xFF
    return bytes(data)


def _fixture_targets(text_rva=0x1000):
    return [
        {"name": "probe-a", "stage": "kinematic-eval",
         "rva": text_rva + 0x100, "size": 0x80},
        {"name": "probe-b", "stage": "bucket-workers",
         "rva": text_rva + 0x200, "size": 0x40},
        {"name": "probe-far", "stage": "frame-scheduler",
         "rva": text_rva + 0x1000 + 0x800000, "size": 0x40},
    ]


def _noise(seed, size):
    """Deterministic filler bytes for the map fixtures (no int3 runs)."""
    out = bytearray()
    x = seed
    for _ in range(size):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        b = (x >> 16) & 0xFF
        out.append(0x90 if b == 0xCC else b)
    return bytes(out)


def _map_fixture(new):
    """Old or new build for the map self-test: (exe bytes, {name: rva}).

    Old text: A B C D E1 E2 G L.  New text adds X before A, grows D by an
    inserted chunk and an edit, adds a third clone E3 after E2, and moves
    .rdata up by 0x200.  A, C, G, L are plain moves; B calls G, which moves
    by a different amount, and reads two data items (so it needs relocating);
    E1/E2/E3 are identical clones; L is a leaf stub with no .pdata entry.
    The vtable holds A, C and B.
    """
    text_rva, text_size = 0x1000, 0x500 if new else 0x400
    data_rva = 0x3200 if new else 0x3000
    pdata_rva = 0x4200 if new else 0x4000
    names = (["X"] if new else []) + ["A", "B", "C", "D", "E1", "E2"] \
        + (["E3"] if new else []) + ["G", "L"]
    sizes = {"X": 0x40, "A": 0x30, "B": 0x40, "C": 0x20, "D": 0x48 if new else 0x40,
             "E1": 0x20, "E2": 0x20, "E3": 0x20, "G": 0x30, "L": 6}
    where, cursor = {}, text_rva
    for name in names:
        where[name] = cursor
        cursor = (cursor + sizes[name] + 15) & ~15
    d1, d2, vtable = data_rva + 0x10, data_rva + 0x20, data_rva + 0x80
    old_d = _noise(4, 0x40)
    bodies = {"X": _noise(9, 0x40), "A": _noise(1, 0x30), "C": _noise(3, 0x20),
              "E1": _noise(5, 0x20), "E2": _noise(5, 0x20), "E3": _noise(5, 0x20),
              "G": _noise(6, 0x30), "L": bytes.fromhex("b82a000000c3"),
              "D": old_d[:0x20] + _noise(11, 8) + old_d[0x20:0x38]
              + bytes([old_d[0x38] ^ 0xFF]) + old_d[0x39:] if new else old_d}
    b = bytearray(_noise(2, 0x40))
    b[0x08] = 0xE8
    b[0x09:0x0D] = (where["G"] - (where["B"] + 0x0D)).to_bytes(4, "little", signed=True)
    b[0x10:0x13] = bytes([0x48, 0x8D, 0x05])
    b[0x13:0x17] = (d1 - (where["B"] + 0x17)).to_bytes(4, "little", signed=True)
    b[0x20:0x23] = bytes([0x48, 0x8B, 0x0D])
    b[0x23:0x27] = (d2 - (where["B"] + 0x27)).to_bytes(4, "little", signed=True)
    bodies["B"] = bytes(b)
    raw_text, raw_data, raw_pdata = 0x400, 0x400 + text_size, 0x400 + text_size + 0x200
    funcs = [n for n in names if n != "L"]
    exe = bytearray(_fixture_pe(
        text_size=text_size, extra_sections=[
            (".rdata", data_rva, 0x200, raw_data, 0x40000040),
            (".pdata", pdata_rva, 12 * len(funcs), raw_pdata, 0x40000040)],
        exception_dir=(pdata_rva, 12 * len(funcs))))
    exe[raw_text:raw_text + text_size] = b"\xcc" * text_size
    for name in names:
        off = raw_text + where[name] - text_rva
        exe[off:off + sizes[name]] = bodies[name]
    exe[raw_data:raw_data + 0x200] = _noise(10, 0x200)
    for slot, name in enumerate(("A", "C", "B")):
        exe[raw_data + 0x80 + 8 * slot:raw_data + 0x88 + 8 * slot] = \
            (IMAGE_BASE + where[name]).to_bytes(8, "little")
    for k, name in enumerate(funcs):
        row = struct.pack("<III", where[name], where[name] + sizes[name], pdata_rva)
        exe[raw_pdata + 12 * k:raw_pdata + 12 * k + 12] = row
    where.update(d1=d1, d2=d2, vtable=vtable, data=data_rva)
    return bytes(exe), where


def _self_test_map(check, td):
    old_data, old = _map_fixture(False)
    new_data, new = _map_fixture(True)
    bm = BuildMap(old_data, new_data)

    # Identical move: no differing bytes, found at its new address.
    res = bm.lookup(old["A"] + 5)
    check(res["status"] == IDENTICAL and res["new_rva"] == new["A"] + 5)
    check(bm.lookup(old["C"])["new_rva"] == new["C"])
    check(bm.lookup(old["G"])["status"] == IDENTICAL)
    # A leaf stub (no .pdata entry) moves too.
    res = bm.lookup(old["L"])
    check(res["status"] == IDENTICAL and res["new_rva"] == new["L"]
          and res["old_unit"][2] == "leaf")

    # Reloc-only move: a call and two RIP-relative reads, all verified.
    res = bm.lookup(old["B"] + 0x15)
    check(res["status"] == RELOC_ONLY and res["new_rva"] == new["B"] + 0x15)
    check("1 code refs exact" in res["detail"] and "2 data refs" in res["detail"])
    check(bm.old.sections[0][0] == 0x1000 and new["B"] - old["B"] == 0x40)

    # A changed function: entry maps to entry, bytes before the inserted
    # chunk and after it are placed by common-chunk alignment.
    res = bm.lookup(old["D"])
    check(res["status"] == CHANGED and res["new_rva"] is None
          and res["placed_rva"] == new["D"] and res["candidate"] == (new["D"], 0x48))
    check(bm.lookup(old["D"] + 0x08)["placed_rva"] == new["D"] + 0x08)
    res = bm.lookup(old["D"] + 0x30)
    check(res["placed_rva"] == new["D"] + 0x38 and res["placement"] == "chunk")
    check(res["score"] is not None and res["score"] > 0.5)

    # An ambiguous pair: two clones beside a third that appeared.
    res = bm.lookup(old["E2"])
    check(res["status"] == AMBIGUOUS and res["new_rva"] is None)
    check(new["E3"] in res["candidates"])
    check(bm.lookup(old["E1"])["status"] == AMBIGUOUS)
    counts = bm.summary()
    check(counts[IDENTICAL] == 4 and counts[RELOC_ONLY] == 1 and counts[CHANGED] == 1
          and counts[AMBIGUOUS] == 2 and counts[NOT_FOUND] == 0
          and counts["new-only"] == 2)
    check(bm.lookup(old["A"] - 1)["status"] == NOT_FOUND)  # Outside every unit.
    check(bm.lookup(0x7000)["kind"] == "none")

    # Data: an exact reference, a bracketed address, and the vtable.
    res = bm.lookup(old["d1"])
    check(res["status"] == "XREF-EXACT" and res["new_rva"] == new["d1"])
    res = bm.lookup(old["d1"] + 8)
    check(res["status"] == "XREF-BRACKET" and res["new_rva"] == new["d1"] + 8
          and "both neighbours" in res["detail"] and "32/32" in res["detail"])
    res = bm.lookup(old["vtable"])
    check(res["new_rva"] == new["vtable"] and res["status"] == "XREF-BRACKET")
    check(res["vtable"]["slots"] == 3 and res["vtable"]["mapped"] == 3
          and res["vtable"]["at_new"] == 3)

    # CSV, JSON, the table and the cache round-trip through the CLI.
    import contextlib
    import io
    old_path, new_path = td / "old_map.exe", td / "new_map.exe"
    old_path.write_bytes(old_data)
    new_path.write_bytes(new_data)
    rvas_file = td / "rvas.txt"
    rvas_file.write_text("# hook sites\n0x%X  probe-a\n%d\n"
                         % (old["A"], old["D"]), encoding="utf-8")
    check(read_rvas(rvas_file) == [old["A"], old["D"]])
    out = {}
    for run in (1, 2):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            code = main(["map", "--old", str(old_path), "--new", str(new_path),
                         "--rva", hex(old["B"]), "--rvas-file", str(rvas_file),
                         "--json", str(td / "map.json"),
                         "--all-functions", str(td / "map.csv"),
                         "--cache", str(td / "map_cache.json")])
        check(code == 0)
        out[run] = buf.getvalue()
    check("loaded from cache" in out[2] and "loaded from cache" not in out[1])
    first = [l for l in out[1].splitlines() if l.startswith("0x")]
    check(first == [l for l in out[2].splitlines() if l.startswith("0x")])
    check(first[0].split()[:3] == [hex(old["B"]).upper().replace("0X", "0x"),
                                   "0x%X" % new["B"], RELOC_ONLY])
    check(first[2].split()[2] == CHANGED and first[2].split()[1].startswith("~0x"))
    check(out[1].rstrip().splitlines()[-1].startswith("units: IDENTICAL 4,"))
    rows = (td / "map.csv").read_text(encoding="utf-8").splitlines()
    check(rows[0] == "old_start,old_len,new_start,status,new_len,kind,how,score")
    check(len(rows) == 1 + 8 + 2 and sum(r.split(",")[3] == "ADDED" for r in rows) == 2)
    check(rows[1].split(",")[:4] == ["0x%X" % old["A"], "48", "0x%X" % new["A"],
                                     IDENTICAL])
    doc = json.loads((td / "map.json").read_text(encoding="utf-8"))
    check(len(doc) == 3 and doc[0]["new_rva"] == "0x%X" % new["B"]
          and doc[2]["placed_rva"] == "0x%X" % new["D"])
    # A stale cache (different exe) is rebuilt rather than trusted.
    other = td / "other_map.exe"
    other.write_bytes(new_data)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        main(["map", "--old", str(other), "--new", str(new_path),
              "--rva", hex(new["A"]), "--cache", str(td / "map_cache.json")])
    check("loaded from cache" not in buf.getvalue())
    # Nothing to map is a usage error, reported and not a traceback.
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        check(main(["map", "--old", str(old_path), "--new", str(new_path)]) == 2)
    check("map needs --rva" in err.getvalue())

    # --seed belongs to the baseline in --targets: refused when --old has
    # another PE identity (same bytes, timestamp at NT header +8), accepted
    # when --old is that baseline.  Without --seed the guard does not run.
    restamped_path = td / "restamped_map.exe"
    restamped = bytearray(old_data)
    restamped[0x88:0x8C] = (0x22222222).to_bytes(4, "little")
    restamped_path.write_bytes(bytes(restamped))
    baseline_path = td / "seed_baseline.json"
    capture(old_path, baseline_path, _fixture_targets())
    seed_out, seed_err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(seed_out), contextlib.redirect_stderr(seed_err):
        code = main(["map", "--old", str(restamped_path), "--new", str(new_path),
                     "--seed", "--targets", str(baseline_path)])
    lines = seed_err.getvalue().splitlines()
    check(code == 2 and seed_out.getvalue() == "" and len(lines) == 1)
    check(lines[0].startswith("build_diff: --seed refused") and "old_map.exe" in lines[0]
          and "0x11111111" in lines[0] and "0x22222222" in lines[0])
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        code = main(["map", "--old", str(old_path), "--new", str(new_path),
                     "--seed", "--targets", str(baseline_path)])
    check(code == 0 and "units: " in buf.getvalue())
    with contextlib.redirect_stdout(io.StringIO()):
        code = main(["map", "--old", str(restamped_path), "--new", str(new_path),
                     "--rva", hex(old["B"]), "--targets", str(baseline_path)])
    check(code == 0)


def self_test():
    checks = 0

    def check(condition):
        nonlocal checks
        checks += 1
        if not condition:
            raise AssertionError("check %d failed" % checks)

    def verdict_of(stats, name):
        for row in stats["targets"]:
            if row["name"] == name:
                return row["verdict"]
        return None

    import tempfile
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        fixture_path = td / "EliteDangerous64.exe"
        fixture_path.write_bytes(_fixture_pe())
        targets = _fixture_targets()

        # capture -> diff roundtrip: same exe short-circuits on identity.
        doc_path = td / "baseline.json"
        capture(fixture_path, doc_path, targets)
        doc = json.loads(doc_path.read_text(encoding="utf-8"))
        check(doc["baseline"]["timestamp"] == 0x11111111)
        check(len(doc["targets"]) == 3)
        check(doc["targets"][0]["prologue_hex"] ==
              doc["targets"][0]["bytes_hex"][:28])
        lines, stats = compare(fixture_path, doc)
        check(stats["identity_match"])
        check(any("exe unchanged (identity match)" in line for line in lines))

        # capture writes ONLY the --out file: the fixture is untouched.
        check(fixture_path.read_bytes() == _fixture_pe())

        # Identical code under a new identity: all targets MATCH.
        restamped = td / "restamped.exe"
        restamped.write_bytes(_fixture_pe(timestamp=0x22222222))
        lines, stats = compare(restamped, doc)
        check(not stats["identity_match"])
        check(stats["match"] == 2 and stats["changed"] == 0
              and stats["gone"] == 1)
        check(verdict_of(stats, "probe-a") == "MATCH")
        check(verdict_of(stats, "probe-far") == "GONE/UNMAPPABLE")
        check(any(line == "render-relevant change: none detected"
                  for line in lines))

        # Changed bytes at a target RVA: CHANGED with the correct first diff.
        changed = td / "changed.exe"
        changed_data = bytearray(_fixture_pe(timestamp=0x22222222))
        file_off = 0x400 + 0x100 + 0x10
        changed_data[file_off] ^= 0xFF
        changed_data[file_off + 2] ^= 0xFF
        changed.write_bytes(bytes(changed_data))
        lines, stats = compare(changed, doc)
        check(verdict_of(stats, "probe-a") == "CHANGED")
        row = [t for t in stats["targets"] if t["name"] == "probe-a"][0]
        check(row["verdict"] == "CHANGED")
        detail = [line for line in lines
                  if "probe-a" in line and "CHANGED" in line][0]
        check("first diff +0x10" in detail and "2 of 128 bytes differ" in detail)
        check("prologue still matches" in detail)
        check(verdict_of(stats, "probe-b") == "MATCH")
        check(any(line == "render-relevant change: 1 target changed"
                  for line in lines))

        # A change inside the 14-byte prologue is flagged as such.
        prologue_hit = td / "prologue.exe"
        hit_data = bytearray(_fixture_pe(timestamp=0x22222222))
        hit_data[0x400 + 0x200 + 1] ^= 0xFF
        prologue_hit.write_bytes(bytes(hit_data))
        lines, stats = compare(prologue_hit, doc)
        detail = [line for line in lines
                  if "probe-b" in line and "CHANGED" in line][0]
        check("prologue DIFFERS" in detail)
        check("first diff +0x1" in detail)

        # Different section layout: same-RVA verdicts are UNRELIABLE, loudly.
        relaid = td / "relaid.exe"
        relaid.write_bytes(_fixture_pe(timestamp=0x22222222, text_size=0x8000))
        lines, stats = compare(relaid, doc)
        check(verdict_of(stats, "probe-a") == "UNRELIABLE")
        check(stats["unreliable"] == 2)
        detail = [line for line in lines if "probe-a" in line][0]
        check("section table differs" in detail)
        check(any(line == "render-relevant change: layout changed -- re-run "
                          "capture and re-verify" for line in lines))

        # Block-diff clustering: touches in blocks 1 and 3 -> two clusters.
        clustered = td / "clustered.exe"
        cl_data = bytearray(_fixture_pe(timestamp=0x22222222))
        cl_data[0x400 + 0x1000 + 0x10] ^= 0xFF   # block 1
        cl_data[0x400 + 0x3000 + 0x30] ^= 0xFF   # block 3 (isolated from 1)
        clustered.write_bytes(bytes(cl_data))
        lines, stats = compare(clustered, doc)
        check(stats["block_changes"] == 2)
        check(stats["clusters"] == [[1, 1], [3, 3]])
        cluster_line = [line for line in lines
                        if line.startswith("changed clusters:")][0]
        check("0x2000-0x3000 (1 block)" in cluster_line)
        check("0x4000-0x5000 (1 block)" in cluster_line)
        check(any("none detected (2 .text blocks changed outside the target "
                  "list)" in line for line in lines))

        # Refused inputs: truncated and non-PE bytes fail the shared parser.
        for bad in (b"", b"MZ" + bytes(62)):
            bad_path = td / "bad.exe"
            bad_path.write_bytes(bad)
            try:
                Image(bad)
            except PEError:
                check(True)
            else:
                check(False)

        # A corrupt baseline document is a usage error, not a comparison.
        try:
            compare(fixture_path, {"baseline": {}, "targets": []})
        except (KeyError, ValueError, TypeError):
            check(True)
        else:
            check(False)

        _self_test_map(check, td)

    print("self-test: ok")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command")

    cap = sub.add_parser("capture", help="fingerprint an exe into a JSON baseline")
    cap.add_argument("--exe", required=True, help="executable to fingerprint")
    cap.add_argument("--out", required=True, help="baseline JSON to write")
    cap.add_argument("--targets", default=None,
                     help="existing capture JSON whose target list is reused; "
                          "defaults to the built-in render-pipeline target list")

    dif = sub.add_parser("diff", help="compare an exe against a captured baseline")
    dif.add_argument("--exe", required=True, help="candidate executable")
    dif.add_argument("--targets", default=str(DEFAULT_TARGETS),
                     help="captured baseline JSON (default: %s)"
                          % DEFAULT_TARGETS)

    mp = sub.add_parser("map", help="map old-build RVAs to the new build")
    mp.add_argument("--old", required=True, help="old (analysed) executable")
    mp.add_argument("--new", required=True, help="new executable")
    mp.add_argument("--rva", action="append", type=lambda s: int(s, 0),
                    help="old RVA to map (repeatable)")
    mp.add_argument("--rvas-file", default=None,
                    help="file of old RVAs, one per line, # comments")
    mp.add_argument("--seed", action="store_true",
                    help="also map every built-in render-pipeline target; --old "
                         "must be the --targets baseline build (refused otherwise)")
    mp.add_argument("--targets", default=str(DEFAULT_TARGETS),
                    help="baseline JSON that --seed checks --old against "
                         "(default: %s)" % DEFAULT_TARGETS)
    mp.add_argument("--json", default=None, help="write the results as JSON")
    mp.add_argument("--all-functions", default=None, metavar="CSV",
                    help="write the full old->new unit map as CSV")
    mp.add_argument("--cache", default=None, metavar="JSON",
                    help="reuse/write the built map (keyed by both exes' SHA-256)")

    ap.add_argument("--self-test", action="store_true",
                    help="check this script against itself and exit")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.command:
        ap.error("a subcommand is required (capture, diff, map, or --self-test)")
    try:
        if args.command == "capture":
            if args.targets:
                seed = json.loads(Path(args.targets).read_text(encoding="utf-8"))
                targets = [{"name": t["name"], "stage": t["stage"],
                            "rva": t["rva"], "size": t["size"]}
                           for t in seed["targets"]]
            else:
                targets = TARGET_SEED
            document = capture(args.exe, args.out, targets)
            print("captured %d targets from %s -> %s"
                  % (len(document["targets"]), args.exe, args.out))
            return 0
        if args.command == "map":
            return map_rvas(args)
        doc = json.loads(Path(args.targets).read_text(encoding="utf-8"))
        lines, _stats = compare(args.exe, doc)
        for line in lines:
            print(line)
        return 0
    except (OSError, PEError, ValueError, KeyError) as exc:
        print("build_diff: " + str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

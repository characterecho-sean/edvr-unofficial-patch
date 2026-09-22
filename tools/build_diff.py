#!/usr/bin/env python3
"""Offline "did the game's render-relevant code change between builds" detector.

    python tools/build_diff.py capture --exe analysis/EliteDangerous64.exe ^
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
"""
import argparse
import hashlib
import json
import sys
import tempfile
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
# 2026-09-21 from the baseline at analysis/EliteDangerous64.exe.  Sizes are
# the Ghidra body sizes from analysis/decomp/decomp_*.txt headers; they may
# include jump-table bytes, which is fine for fingerprinting.
TARGET_SEED = [
    {"name": "kinematic-eval-fun_14430efe0", "stage": "kinematic-eval",
     "rva": 0x430EFE0, "size": 1006},
    {"name": "lod-evaluator-fun_144331300", "stage": "cull-lod",
     "rva": 0x4331300, "size": 64},
    {"name": "worker-entry-fun_144321940", "stage": "frame-scheduler",
     "rva": 0x4321940, "size": 503},
    {"name": "worker-entry-fun_144320340", "stage": "frame-scheduler",
     "rva": 0x4320340, "size": 565},
    {"name": "bucket-builder-fun_1442b4420", "stage": "bucket-workers",
     "rva": 0x42B4420, "size": 3222},
    {"name": "rig-dispatch-fun_14431afe0", "stage": "kinematic-eval",
     "rva": 0x431AFE0, "size": 699},
    {"name": "bucket-traversal-fun_144312040", "stage": "bucket-workers",
     "rva": 0x4312040, "size": 1597},
    {"name": "scheduler-reset-fun_1436a0f50", "stage": "frame-scheduler",
     "rva": 0x36A0F50, "size": 687},
    {"name": "merge-fun_14434e20e", "stage": "frame-scheduler",
     "rva": 0x434E20E, "size": 129},
    {"name": "merge-fun_14434e28f", "stage": "frame-scheduler",
     "rva": 0x434E28F, "size": 315},
    {"name": "enqueue-fun_144c7ef70", "stage": "bucket-workers",
     "rva": 0x4C7EF70, "size": 632},
    {"name": "accumulator-fun_144c837b0", "stage": "bucket-workers",
     "rva": 0x4C837B0, "size": 427},
    {"name": "drainer-fun_14434d790", "stage": "bucket-workers",
     "rva": 0x434D790, "size": 64},
    {"name": "rekey-wrapper-fun_1442b5670", "stage": "bucket-workers",
     "rva": 0x42B5670, "size": 64},
    {"name": "projection-wrapper-0x4e2f50", "stage": "cull-lod",
     "rva": 0x4E2F50, "size": 64},
    {"name": "projection-wrapper-0x4e2d30", "stage": "cull-lod",
     "rva": 0x4E2D30, "size": 64},
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


def _fixture_pe(timestamp=0x11111111, image_size=0x100000, text_rva=0x1000,
                text_raw=0x400, text_size=0x4000, extra_sections=()):
    """Minimal PE32+ with one .text section openxr_pe.Image accepts."""
    size = text_raw + text_size
    sections = [(".text", text_rva, text_size, text_raw)] + list(extra_sections)
    for _name, rva, span, raw in list(sections):
        size = max(size, raw + span)
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
    for i, (name, rva, span, raw) in enumerate(sections):
        off = table + i * 40
        data[off:off + 8] = name.encode("ascii")[:8].ljust(8, b"\0")
        put(off + 8, span)
        put(off + 12, rva)
        put(off + 16, span)
        put(off + 20, raw)
        put(off + 36, 0x60000020)
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

    ap.add_argument("--self-test", action="store_true",
                    help="check this script against itself and exit")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.command:
        ap.error("a subcommand is required (capture, diff, or --self-test)")
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

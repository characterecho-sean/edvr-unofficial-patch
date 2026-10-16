#!/usr/bin/env python3
"""Mutation proof for the offscreen fast path (src\\d3d11\\flat_offscreen_fast.h, tools\\flat_temporal_test\\flat_offscreen_tests.h).

A rig that passes proves little until it is seen to FAIL on a source that breaks a rule it pins. For each mutation below this tool copies the
headers the standalone driver (flat_offscreen_test.cpp) includes into a temp directory OUTSIDE the repo, applies one textual edit to
flat_offscreen_fast.h, builds the driver against the copy, runs it from the repository root (the corpus and the sources are read by relative
path), and requires it to fail on a check of the case that belongs to the rule: every check's text starts "offscreen O<n>:". Nothing is
written inside the repo.

  O1 the key's inputs   O2 the live conditions   O3 the draw conditions   O4 the memo   O6 the line

  python tools\\flat_temporal_test\\offscreen_mutants.py --self-test   text only; every anchor is found exactly once, every case is in the rig
  python tools\\flat_temporal_test\\offscreen_mutants.py --run [--only a,b] [--jobs N]
  python tools\\flat_temporal_test\\offscreen_mutants.py --list         the plan; writes nothing, starts nothing

--run needs the MSVC toolchain. build.bat gates on --self-test, which is what keeps an edit of the header from silently orphaning a mutation.
"""
import argparse
import concurrent.futures
import importlib.util
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
HEADER_REL = Path("src") / "d3d11" / "flat_offscreen_fast.h"
HEADER = ROOT / HEADER_REL
COUNTS_REL = Path("src") / "d3d11" / "flat_offscreen_counts.h"
COUNTS = ROOT / COUNTS_REL
TESTS = HERE / "flat_offscreen_tests.h"
DRIVER = HERE / "flat_offscreen_test.cpp"
CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS"]

KEY_INPUTS = [
    ("vs", "if (in(0) && vs != o.vs) return false;"), ("ps", "if (in(1) && ps != o.ps) return false;"),
    ("color", "if (in(2) && color != o.color) return false;"), ("depth", "if (in(3) && depth != o.depth) return false;"),
    ("rtv", "if (in(4) && rtv != o.rtv) return false;"), ("dsv", "if (in(5) && dsv != o.dsv) return false;"),
    ("b1", "if (in(6) && b1 != o.b1) return false;"), ("camera-hash", "if (in(7) && cameraHash != o.cameraHash) return false;"),
    ("write-epoch", "if (in(8) && writeEpoch != o.writeEpoch) return false;"), ("write-seq", "if (in(9) && writeSeq != o.writeSeq) return false;"),
    ("has-camera", "if (in(10) && hasCamera != o.hasCamera) return false;"),
    ("rtv-generation", "if (in(13) && rtvGen != o.rtvGen) return false;"), ("dsv-generation", "if (in(14) && dsvGen != o.dsvGen) return false;"),
]
# (name, anchor, replacement, case): the rig must fail on a check whose text starts "offscreen <case>:".
MUTATIONS = [("key-ignores-" + name, anchor, "", "O1") for name, anchor in KEY_INPUTS] + [
    ("key-ignores-extents", "if (in(11) && (width != o.width ||", "if (in(11) && false && (width != o.width ||", "O1"),
    ("key-ignores-viewport", "if (in(12) && (viewportCount != o.viewportCount ||", "if (in(12) && false && (viewportCount != o.viewportCount ||", "O1"),
    # The live conditions: each removed in turn.
    ("live-ignores-work", "if (!l.workFull) return FlatOffscreenNo::Work;", "", "O2"),
    ("live-ignores-gpu-span", "if (!l.gpuFrameOpened || !l.foregroundFrameReady) return FlatOffscreenNo::FrameState;",
     "if (!l.foregroundFrameReady) return FlatOffscreenNo::FrameState;", "O2"),
    ("live-ignores-foreground-reset", "if (!l.gpuFrameOpened || !l.foregroundFrameReady) return FlatOffscreenNo::FrameState;",
     "if (!l.gpuFrameOpened) return FlatOffscreenNo::FrameState;", "O2"),
    ("live-ignores-audit", "if (l.auditing) return FlatOffscreenNo::Audit;", "", "O2"),
    ("live-ignores-diagnostics", "if (l.diagnostics) return FlatOffscreenNo::Diagnostics;", "", "O2"),
    ("live-ignores-foreign-work", "if (l.foreignWork) return FlatOffscreenNo::Foreign;", "", "O2"),
    ("live-ignores-overlay", "if (l.overlayOpen) return FlatOffscreenNo::Overlay;", "", "O2"),
    ("live-ignores-projection-audit", "if (l.projectionAudit) return FlatOffscreenNo::ProjectionAudit;", "", "O2"),
    ("live-ignores-weapon-footprint", "if (l.weaponFootprint) return FlatOffscreenNo::WeaponFootprint;", "", "O2"),
    ("live-ignores-foreground", "if (l.foregroundTouchesDepth) return FlatOffscreenNo::Foreground;", "", "O2"),
    ("live-ignores-hdr-consumer", "if (l.hdrCouldConsume) return FlatOffscreenNo::HdrConsumer;", "", "O2"),
    # The draw conditions.
    ("shape-ignores-pool-family", "if (pair.supported ||", "if (", "O3"),
    ("shape-ignores-tone", "pair.toneSlot != ~0u ||", "", "O3"),
    ("shape-ignores-map-tone", "pair.mapToneSlot != ~0u ||", "", "O3"),
    ("shape-ignores-source-tone", "pair.sourceTone ||", "", "O3"),
    ("shape-ignores-copy-pair", "pair.copyPair ||", "", "O3"),
    ("shape-ignores-ui-family", "pair.uiMayAsk ||", "", "O3"),
    ("shape-ignores-copy-pixel-shader", "k.ps == flat_mono_detail::kCopyPs)", "false)", "O3"),
    ("shape-ignores-output", "if (k.color && (k.color == output || flatOffscreenActedOnFormat(k.format))) return FlatOffscreenNo::Format;",
     "if (k.color && flatOffscreenActedOnFormat(k.format)) return FlatOffscreenNo::Format;", "O3"),
    ("shape-ignores-format", "if (k.color && (k.color == output || flatOffscreenActedOnFormat(k.format))) return FlatOffscreenNo::Format;",
     "if (k.color && k.color == output) return FlatOffscreenNo::Format;", "O3"),
    ("shape-ignores-format-26", "return format == 9 || format == 23 || format == 26 || format == 27 || format == 60;",
     "return format == 9 || format == 23 || format == 27 || format == 60;", "O3"),
    ("shape-ignores-native-depth", "if (nativeDepth) return FlatOffscreenNo::Extent;", "", "O3"),
    ("shape-ignores-triangle", "if (flatOffscreenFullscreenTriangle(a)) return FlatOffscreenNo::Shape;", "", "O3"),
    ("triangle-needs-three-instances", "return a.count == 3 && a.instances == 1 && (a.kind == 'D' || a.kind == 'N');",
     "return a.count == 3 && a.instances == 3 && (a.kind == 'D' || a.kind == 'N');", "O3"),
    ("triangle-ignores-indirect-n", "(a.kind == 'D' || a.kind == 'N');", "(a.kind == 'D');", "O3"),
    # The memo.
    ("memo-ignores-the-frame", "if (frame != frame_) return FlatOffscreenNo::OtherFrame;", "", "O4"),
    ("memo-keeps-the-sources-camera-pointer", "draw_.key.camera = d.key.camera ? draw_.camera : nullptr;", "draw_.key.camera = d.key.camera;", "O4"),
    ("memo-reuse-keeps-the-old-instances", "draw_.instances = instances;", "(void)instances;", "O4"),
    ("memo-recopies-the-held-key", "if (valid_ && frame_ == frame && key_.same(key)) return;   // already held: nothing to copy", "", "O4"),
    ("memo-forget-keeps-it", "void forget() noexcept { valid_ = false; }", "void forget() noexcept {}", "O4"),
    ("memo-lookup-skips-the-key", "return key_.same(now) ? FlatOffscreenNo::None : FlatOffscreenNo::KeyDiffers;", "return FlatOffscreenNo::None;", "O4"),
]
COUNT_MUTATIONS = [
    ("line-drops-the-rest-count", 'if (n > 3) put(", and %u more", n - 3);', "", "O6"),
    ("line-shows-reasons-unsorted", "c.declined[order[j]] > c.declined[order[j - 1]]", "false", "O6"),
    ("line-divides-by-frames-wrongly", "const double f = frames > 0 ? frames : 1.0;", "const double f = 1.0;", "O6"),
]


def read(path):
    return path.read_text(encoding="utf-8")


def self_test():
    header, counts, tests = read(HEADER), read(COUNTS), read(TESTS)
    names = [m[0] for m in MUTATIONS + COUNT_MUTATIONS]
    assert len(set(names)) == len(names), "duplicate mutation name"
    for name, anchor, replacement, case in MUTATIONS:
        assert header.count(anchor) == 1, name + ": anchor found " + str(header.count(anchor)) + " times in the header, want 1"
        assert anchor != replacement, name + ": no edit"
        assert '"' + case + ':' in tests, name + ": the rig has no check '" + case + ":'"
    for name, anchor, replacement, case in COUNT_MUTATIONS:
        assert counts.count(anchor) == 1, name + ": anchor found " + str(counts.count(anchor)) + " times in the counts header, want 1"
        assert anchor != replacement, name + ": no edit"
        assert '"' + case + ':' in tests, name + ": the rig has no check '" + case + ":'"
    flat = read(ROOT / "tools" / "flat_temporal_test" / "flat_temporal_test.cpp")
    assert '#include "flat_offscreen_tests.h"' in flat and "flatOffscreenTests()" in flat, "flat_temporal_test does not run the offscreen tests"
    assert "flatOffscreenTests()" in read(DRIVER), "the standalone driver does not run them"
    build = read(ROOT / "build.bat")
    assert 'python "tools\\flat_temporal_test\\offscreen_mutants.py" --self-test' in build, "build.bat does not gate on this file"
    print("offscreen mutants: PASS " + str(len(names)) + " anchors, cases, driver, build gate")


def toolchain():
    spec = importlib.util.spec_from_file_location("panel_mutants_toolchain", ROOT / "tools/panel_curve_test/mutants.py")
    module = importlib.util.module_from_spec(spec)
    previous = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = previous
    return module.Toolchain()


def run(only, jobs):
    known = {m[0] for m in MUTATIONS + COUNT_MUTATIONS}
    if only - known:
        raise ValueError("unknown mutation: " + ", ".join(sorted(only - known)))
    selected = [(m, HEADER_REL) for m in MUTATIONS if not only or m[0] in only] + \
               [(m, COUNTS_REL) for m in COUNT_MUTATIONS if not only or m[0] in only]
    tc = toolchain()
    with tempfile.TemporaryDirectory(prefix="edvr-offscreen-mutants-") as directory:
        temp = Path(directory)

        def mirror(name):
            root = temp / name
            for source in (ROOT / "src").rglob("*.h"):
                target = root / source.relative_to(ROOT)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
            for source in list(HERE.glob("*.h")) + [DRIVER]:
                target = root / "tools" / "flat_temporal_test" / source.name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
            third = ROOT / "third_party"
            for source in third.rglob("*.h"):
                target = root / source.relative_to(ROOT)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
            return root

        def one(item):
            mutation, rel = item if item else (("baseline", None, None, None), None)
            name, anchor, replacement, case = mutation
            root = mirror(name)
            if anchor is not None:
                path = root / rel
                path.write_text(path.read_text(encoding="utf-8").replace(anchor, replacement, 1), encoding="utf-8")
            exe = root / "t.exe"
            done = subprocess.run([tc.cl] + CL_FLAGS + ["/Fo" + str(root) + "\\", "/Fe" + str(exe),
                                   str(root / "tools" / "flat_temporal_test" / DRIVER.name), "/link", "/INCREMENTAL:NO", "kernel32.lib"],
                                  capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(root))
            if done.returncode:
                raise RuntimeError(name + ": compile failed\n" + done.stdout + done.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, errors="replace", cwd=str(ROOT))
            return name, case, result

        name, case, result = one(None)
        if result.returncode:
            raise RuntimeError("baseline failed\n" + result.stdout + result.stderr)
        print("baseline: PASS")
        failures = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            for name, case, result in pool.map(one, selected):
                cases = re.findall(r"^FAIL: offscreen (O\d+):", result.stdout, re.M)
                if result.returncode != 1 or case not in cases:
                    failures.append(name + ": survived or failed the wrong check (exit " + str(result.returncode) + ", wanted " + case + ", got " +
                                    ",".join(sorted(set(cases))) + ")\n" + result.stdout[-600:])
                else:
                    print(name + ": killed by " + case)
        if failures:
            raise RuntimeError("\n".join(failures))
    print("offscreen mutants: PASS " + str(len(selected)) + " killed")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--dry-run", action="store_true", help="the plan; writes nothing, starts nothing")
    parser.add_argument("--only", default="")
    parser.add_argument("--jobs", type=int, default=6)
    args = parser.parse_args()
    if args.dry_run or args.list:
        for name, _, _, case in MUTATIONS + COUNT_MUTATIONS:
            print(name + ": " + case)
        return 0
    if args.self_test:
        self_test()
    if args.run:
        run(set(filter(None, args.only.split(","))), max(1, args.jobs))
    if not args.self_test and not args.run:
        parser.error("choose --self-test, --run, --list or --dry-run")
    return 0


if __name__ == "__main__":
    sys.dont_write_bytecode = True
    raise SystemExit(main())

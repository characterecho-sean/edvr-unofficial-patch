#!/usr/bin/env python3
"""Deterministic mutation proof for the platform-independent map bounce.

--self-test checks anchors/labels/build gate without writing. --run builds the
baseline then each mutant in a temporary directory. --dry-run writes nothing.
The two adapter-order/thread mutants use fake-driver callbacks compiled away
by if constexpr in production. An early real Unmap commits GPU bytes before
the bounced writeback; the GPU oracle detects this ordering error.
"""
import argparse
import importlib.util
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
HEADER = ROOT / "src/d3d11/flat_map_bounce.h"
RIG = HERE / "flat_map_bounce_test.cpp"
MUTATIONS = [
    ("skip-flush", "skipFlush", "C1"),
    ("width-minus-one", "shortFlush", "C1"),
    ("width-plus-one", "longFlush", "C7"),
    ("flush-after-real-unmap", "flushAfterUnmap", "C1"),
    ("bounce-no-overwrite", "acceptNonDiscard", "C2"),
    ("accept-failed-map", "acceptFailed", "C3"),
    ("no-remap-drop", "noDrop", "C6"),
    ("skip-off-owner-flush", "skipOffOwner", "C5"),
    ("arena-plus-eight", "unaligned", "C7"),
    ("reuse-open-slot", "reuseOpen", "C4"),
    ("invert-threshold", "inverted", "C8"),
    ("verify-disabled", "verifyOff", "C5"),
]


def self_test():
    header = HEADER.read_text(encoding="utf-8")
    rig = RIG.read_text(encoding="utf-8")
    build = (ROOT / "build.bat").read_text(encoding="utf-8")
    for name, flag, case in MUTATIONS:
        assert header.count(flag + "=false") == 1, name + ": anchor drift"
        assert ('"' + case + ".") in rig, name + ": missing labelled check"
    assert ":rig_flat_map_bounce_test" in build
    assert '"tools\\flat_map_bounce_test\\mutants.py" --self-test' in build
    assert '"tools\\flat_map_bounce_test\\flat_map_bounce_test.cpp"' in build
    assert len({name for name, _, _ in MUTATIONS}) == len(MUTATIONS)
    print("flat_map_bounce mutants: PASS anchors, C1-C8 labels, build gate")


def toolchain():
    # Same sanctioned MSVC bootstrap as the adjacent house rig; reuse rather
    # than duplicate Visual Studio discovery and native-command escaping.
    spec = importlib.util.spec_from_file_location("panel_mutants_toolchain", ROOT / "tools/panel_curve_test/mutants.py")
    module = importlib.util.module_from_spec(spec)
    previous = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = previous
    return module.Toolchain()


def run(only):
    tc = toolchain()
    header = HEADER.read_text(encoding="utf-8")
    selected = [m for m in MUTATIONS if not only or m[0] in only]
    if only - {m[0] for m in MUTATIONS}:
        raise ValueError("unknown mutation")
    with tempfile.TemporaryDirectory(prefix="edvr-flat-map-bounce-") as directory:
        temp = Path(directory)
        exe = temp / "flat_map_bounce_test.exe"
        for name, flag, case in [("baseline", None, None)] + selected:
            source = header if flag is None else header.replace(flag + "=false", flag + "=true", 1)
            (temp / "flat_map_bounce.h").write_text(source, encoding="utf-8")
            build = subprocess.run([tc.cl, "/nologo", "/std:c++17", "/EHsc", "/O2", "/MT", "/W4",
                                    "/I" + str(temp), "/Fe" + str(exe), "/Fo" + str(temp / "rig.obj"), str(RIG)],
                                   cwd=temp, env=tc.env, capture_output=True, text=True)
            if build.returncode:
                raise RuntimeError(name + ": compile failed\n" + build.stdout + build.stderr)
            result = subprocess.run([str(exe), "--self-test"], cwd=temp, capture_output=True, text=True)
            if flag is None:
                if result.returncode:
                    raise RuntimeError("baseline failed\n" + result.stdout + result.stderr)
                print("baseline: PASS")
                continue
            labels = re.findall(r"^FAIL (C\d+)\.", result.stdout, re.M)
            if result.returncode != 1 or case not in labels:
                raise RuntimeError(name + ": mutant survived or failed wrong check\n" + result.stdout + result.stderr)
            print(name + ": killed by " + case)
    print("flat_map_bounce mutants: PASS " + str(len(selected)) + " killed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--only", default="")
    args = parser.parse_args()
    if args.dry_run or args.list:
        for name, _, case in MUTATIONS:
            print(name + ": " + case)
        return 0
    if args.self_test:
        self_test()
    if args.run:
        run(set(filter(None, args.only.split(","))))
    if not args.self_test and not args.run:
        parser.error("choose --self-test, --run, or --dry-run")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

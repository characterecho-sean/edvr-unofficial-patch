#!/usr/bin/env python3
"""Mutation proof for the remember-cap line (src\\d3d11\\engine_velocity.cpp, tools\\engine_velocity_test\\remember_cap_tests.h).

A full remember table used to drop a keyed shader object without a word. It now writes one line per kind (vertex, pixel)
on the first drop. remember_cap_tests.h holds the rules (cases R1-R5; every check's text starts "cap R<n>"). A rig that
passes proves little until it is seen to FAIL on a source that breaks a rule it pins. For each mutation below this tool
copies engine_velocity.cpp into a temp directory OUTSIDE the repo, applies one textual edit, links the rig against that
copy, and requires the rig to fail on a check of the case that belongs to the rule. Nothing is written inside the repo.

  python tools\\engine_velocity_test\\mutants.py --self-test   text only; every anchor is found exactly once in the source as it is
                                                              now, every case is in the rig, build.bat gates on this file
  python tools\\engine_velocity_test\\mutants.py --run [--only a,b] [--jobs N]
  python tools\\engine_velocity_test\\mutants.py --list        the plan; writes nothing, starts nothing

--run needs the MSVC toolchain and build\\gen (build.bat makes it). --self-test runs in build.bat's rig and is what keeps an
edit of the source from silently orphaning a mutation.
"""
import argparse
import concurrent.futures
import importlib.util
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SOURCE = ROOT / "src" / "d3d11" / "engine_velocity.cpp"
RIG = HERE / "engine_velocity_test.cpp"
CASES = HERE / "remember_cap_tests.h"
GEN = ROOT / "build" / "gen"
# (name, anchor, replacement, case): the rig must fail on a check whose text starts with the case.
MUTATIONS = [
    ("vs-line-removed", "if (!g_vsDropSaid) {", "if (false) {", "cap R3"),
    ("ps-line-removed", "if (!g_psDropSaid) {", "if (false) {", "cap R3"),
    ("vs-line-every-drop", "g_vsDropSaid = true;", "(void)0;", "cap R4"),
    ("ps-line-every-drop", "g_psDropSaid = true;", "(void)0;", "cap R4"),
    ("vs-line-omits-hash", "vs_%s was dropped", "vs_%.0s was dropped", "cap R3"),
    ("ps-line-omits-hash", "ps_%s was dropped", "ps_%.0s was dropped", "cap R3"),
    ("vs-line-below-cap", "if (g_vs.size() >= kRememberCap) {", "if (g_vs.size() + 1 >= kRememberCap) {", "cap R1"),
    ("ps-line-below-cap", "if (g_ps.size() >= kRememberCap) {", "if (g_ps.size() + 1 >= kRememberCap) {", "cap R1"),
    ("vs-line-on-reoffer", "if (g_vs.count(shader)) return;\n    if (g_vs.size() >= kRememberCap) {",
     "if (g_vs.size() >= kRememberCap || g_vs.count(shader)) {", "cap R2"),
    ("ps-line-on-reoffer", "if (g_ps.count(shader)) return;\n    if (g_ps.size() >= kRememberCap) {",
     "if (g_ps.size() >= kRememberCap || g_ps.count(shader)) {", "cap R2"),
]
CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX",
            "/D_CRT_SECURE_NO_WARNINGS", "/DUNICODE", "/D_UNICODE", "/utf-8", "/DEDVR_ENGINE_VELOCITY_RIG",
            "/DEDVR_BINDING_SHADOW_EXTERNAL"]
LIBS = ["d3d11.lib", "d3dcompiler.lib", "dxguid.lib"]


def read(path):
    return path.read_text(encoding="utf-8")


def self_test():
    source, rig, cases, build = read(SOURCE), read(RIG), read(CASES), read(ROOT / "build.bat")
    names = [m[0] for m in MUTATIONS]
    assert len(set(names)) == len(names), "duplicate mutation name"
    for name, anchor, replacement, case in MUTATIONS:
        assert source.count(anchor) == 1, name + ": anchor found " + str(source.count(anchor)) + " times, want 1"
        assert anchor != replacement, name + ": no edit"
        assert case + ":" in cases, name + ": the rig has no check '" + case + ":'"
    # The two log fragments the rig looks for are the ones the source writes.
    for kind in ("vertex", "pixel"):
        fragment = kind + " shader remember cap reached"
        assert source.count(fragment) == 1 and cases.count(fragment) == 1, kind + ": fragment drifted"
    assert "remember_cap_tests.h" in rig, "the rig does not include the cases"
    # It runs last: every other suite's run( comes before it, because it fills both tables for good.
    last = rig.index("remember_cap_tests::run(")
    for other in re.finditer(r"\b(?!remember_cap_tests)\w+_tests::run\(", rig):
        assert other.start() < last, other.group(0) + " runs after the cap test"
    assert "EDVR_ENGINE_VELOCITY_RIG" in source and "engineVelocityRememberCapForRig" in source
    # build.bat compiles the rig the way this tool does, and gates on this file.
    assert '"tools\\engine_velocity_test\\engine_velocity_test.cpp" "src\\d3d11\\engine_velocity.cpp"' in build
    assert '"src\\d3d11\\gpu_timing.cpp" "src\\d3d11\\gpu_span_d3d11.cpp"' in build
    assert "/DEDVR_ENGINE_VELOCITY_RIG /DEDVR_BINDING_SHADOW_EXTERNAL" in build
    assert 'python "tools\\engine_velocity_test\\mutants.py" --self-test' in build, "build.bat does not gate on this file"
    print("engine_velocity mutants: PASS " + str(len(MUTATIONS)) + " anchors, cases, order, build gate")


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


def compile_obj(tc, source, temp, name, extra=()):
    obj = temp / (name + ".obj")
    cmd = [tc.cl] + CL_FLAGS + ["/c", "/I" + str(GEN), "/I" + str(ROOT / "src" / "d3d11")] + list(extra) + \
          ["/Fo" + str(obj), str(source)]
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(temp))
    if done.returncode:
        raise RuntimeError(name + ": compile failed\n" + done.stdout + done.stderr)
    return obj


def link_and_run(tc, objs, temp, name):
    exe = temp / (name + ".exe")
    done = subprocess.run([tc.cl, "/nologo"] + [str(o) for o in objs] + ["/Fe" + str(exe), "/link", "/INCREMENTAL:NO"] + LIBS,
                          capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(temp))
    if done.returncode:
        raise RuntimeError(name + ": link failed\n" + done.stdout + done.stderr)
    # From the repository root: the rig reads some sources by relative path (build.bat runs it there too).
    return subprocess.run([str(exe), "--self-test"], capture_output=True, text=True, errors="replace", cwd=str(ROOT))


def run(only, jobs):
    if not GEN.is_dir():
        raise RuntimeError("build\\gen is missing: run build.bat once so the generated shader headers exist")
    known = {m[0] for m in MUTATIONS}
    if only - known:
        raise ValueError("unknown mutation: " + ", ".join(sorted(only - known)))
    selected = [m for m in MUTATIONS if not only or m[0] in only]
    tc = toolchain()
    source = read(SOURCE)
    with tempfile.TemporaryDirectory(prefix="edvr-engine-velocity-mutants-") as directory:
        temp = Path(directory)
        shared = [compile_obj(tc, RIG, temp, "rig"),
                  compile_obj(tc, ROOT / "src" / "d3d11" / "gpu_timing.cpp", temp, "gpu_timing"),
                  compile_obj(tc, ROOT / "src" / "d3d11" / "gpu_span_d3d11.cpp", temp, "gpu_span")]

        def one(mutation):
            name, anchor, replacement, case = mutation if mutation else ("baseline", None, None, None)
            copy = temp / name
            copy.mkdir()
            text = source if anchor is None else source.replace(anchor, replacement, 1)
            (copy / "engine_velocity.cpp").write_text(text, encoding="utf-8")
            obj = compile_obj(tc, copy / "engine_velocity.cpp", copy, "engine_velocity")
            result = link_and_run(tc, shared + [obj], copy, name)
            return name, case, result

        for name, case, result in [one(None)]:
            if result.returncode:
                raise RuntimeError("baseline failed\n" + result.stdout + result.stderr)
            print("baseline: PASS")
        failures = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            for name, case, result in pool.map(one, selected):
                labels = re.findall(r"^FAIL: (cap R\d+):", result.stderr, re.M)
                if result.returncode != 1 or case not in labels:
                    failures.append(name + ": survived or failed the wrong check (exit " + str(result.returncode) + ", wanted " + case
                                    + ")\n" + result.stdout + result.stderr)
                else:
                    print(name + ": killed by " + case)
        if failures:
            raise RuntimeError("\n".join(failures))
    print("engine_velocity mutants: PASS " + str(len(selected)) + " killed")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--dry-run", action="store_true", help="the plan; writes nothing, starts nothing")
    parser.add_argument("--only", default="")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if args.dry_run or args.list:
        for name, _, _, case in MUTATIONS:
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

#!/usr/bin/env python3
"""Mutation proof for the remember-cap line (src\\d3d11\\engine_velocity.cpp, tools\\engine_velocity_test\\remember_cap_tests.h)
for the overlay guard's remembered verification (GuardVerified, cases G1-G3 in flat_lazy_tests.h: every such check's text starts
"guard G<n>") and for the claim check of the primary-copy clear, merge and pool copier (src\\d3d11\\engine_velocity_primary_copy.h, cases C1-C12 in
claim_check_tests.h: every such check's text starts "claim check C<n>"). The claim-check mutations edit that header, so the rig and
engine_velocity.cpp are compiled from a throwaway copy of the trees they include (still outside the repo), and the rig runs with
--claim-check, which runs only claim_check_tests, so the first failing check is the one that names the case.

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
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SOURCE = ROOT / "src" / "d3d11" / "engine_velocity.cpp"
RIG = HERE / "engine_velocity_test.cpp"
CASES = HERE / "remember_cap_tests.h"
GUARD_CASES = HERE / "flat_lazy_tests.h"
HEADER = ROOT / "src" / "d3d11" / "engine_velocity_primary_copy.h"
CLAIM_CASES = HERE / "claim_check_tests.h"
# Directories the rig includes by relative path; the claim-check mutations compile from a copy of these.
MIRROR = ["src", "third_party/dxbc_hash", "tools/engine_velocity_test", "tools/skin_clone_test", "tools/skin_join_test"]
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
    # The overlay guard's remembered verification (GuardVerified): each input of the key is removed in turn, and the remembering itself.
    # G1: the game rebinds t3 between two guarded draws and its next state must be what the restore writes back, t3 included.
    # G2: a verification is not reused once RTV0, DSV0 or the private SRV's identity moved, and is reused while none did (the shape of the
    #     D3D calls a guarded draw makes in the sequence). G3: the pixels of the sequence do not change (the control of the whole).
    ("guard-ignores-t3-generation", "g_guardVerified.srv3Gen == bindingGeneration(BindSlot::PsSrv3) &&",
     "true &&", "guard G1"),
    ("guard-ignores-rtv-generation", "g_guardVerified.rtvGen == bindingGeneration(BindSlot::Rtv0) &&", "true &&", "guard G2"),
    ("guard-ignores-dsv-generation", "g_guardVerified.dsvGen == bindingGeneration(BindSlot::Dsv0);", "true;", "guard G2"),
    ("guard-never-trusts", "return !g_guardTrustOff && g_guardVerified.valid", "return false && g_guardVerified.valid", "guard G2"),
    # The lazy guard (round 4): the private t3 stays bound across the group's draws. G1: a kept t3 must be the game's after a game set of t3 or a
    # non-guarded draw; G5: and it must cost what it saves (the second draw of a group binds nothing).
    ("lazy-guard-kept-ignores-t3-generation",
     "const bool keptBound = g_bound.guardSrv3.Get() == privateSrv && bindingGeneration(BindSlot::PsSrv3) == g_bound.srv3Gen;",
     "const bool keptBound = g_bound.guardSrv3.Get() == privateSrv;", "guard G1"),
    ("lazy-guard-nonguard-draw-keeps-t3", "if (!guardOverlay && g_bound.guardSrv3) guardRestoreT3(ctx);", "", "flat lazy"),
    ("lazy-guard-flushes-at-every-overlay-draw", " && !(e.overlayGroup && e.overlayBaseSrv)) {\n        ++g_draw.flatFlushBy",
     ") {\n        ++g_draw.flatFlushBy", "guard G5"),
    ("lazy-guard-ends-eager", "    if (!g_flatLazy) {\n        std::lock_guard<std::recursive_mutex> lock(g_mutex);",
     "    if (!g_flatLazy || g_bound.guardSrv3) {\n        std::lock_guard<std::recursive_mutex> lock(g_mutex);", "guard G5"),
]
CLEAR_CHECK = "inline bool clearClaimCheck(uintptr_t dictionary) noexcept {\n    if(!g_claimPrecheck)return false;\n    ClaimViewState view;\n    "
MERGE_CHECK = "inline bool mergeClaimCheck(uintptr_t destination,uintptr_t source) noexcept {\n    if(!g_claimPrecheck)return false;\n    ClaimViewState view;\n    "
GATE = "if(!readClaimView(view) || view.plans || view.count>kClaimViewMax)"
# The claim check (header mutations): each removes one early exit's reason to fall back, or one input of its decision.
HEADER_MUTATIONS = [
    ("claim-clear-ignores-plans", CLEAR_CHECK + GATE, CLEAR_CHECK + "if(!readClaimView(view) || view.count>kClaimViewMax)", "claim check C5"),
    ("claim-merge-ignores-plans", MERGE_CHECK + GATE, MERGE_CHECK + "if(!readClaimView(view) || view.count>kClaimViewMax)", "claim check C5"),
    ("claim-clear-ignores-the-limit", CLEAR_CHECK + GATE, CLEAR_CHECK + "if(!readClaimView(view) || view.plans)", "claim check C4"),
    ("claim-merge-ignores-the-limit", MERGE_CHECK + GATE, MERGE_CHECK + "if(!readClaimView(view) || view.plans)", "claim check C4"),
    ("claim-clear-ignores-hits", "hit=view.holds(node);return !hit;", "hit=false;return true;", "claim check C2"),
    ("claim-clear-accepts-a-refused-walk", "return false;   // a hit, or a refusal the locked walk acts on (clear)", "return !hit;", "claim check C2"),
    ("claim-merge-accepts-a-refused-list", "return false;   // a hit, or a refusal the locked walk acts on (merge)", "return !hit;", "claim check C3"),
    ("claim-merge-ignores-source-slots", "hit=view.holds(node) || (fill && view.holds(tail,tailCount,tailCount+fill));",
     "hit=(fill && view.holds(tail,tailCount,tailCount+fill));", "claim check C3"),
    ("claim-merge-ignores-tail-fill", "hit=view.holds(node) || (fill && view.holds(tail,tailCount,tailCount+fill));", "hit=view.holds(node);", "claim check C3"),
    ("claim-merge-tail-fill-off-by-one", "hit=view.holds(node) || (fill && view.holds(tail,tailCount,tailCount+fill));",
     "hit=view.holds(node) || (fill && view.holds(tail,tailCount+1,tailCount+fill));", "claim check C3"),
    ("claim-slot-window-misses-the-last-slot", "offset/kBytes>=first && offset/kBytes<last && offset/kBytes<8",
     "offset/kBytes>=first && offset/kBytes+1<last && offset/kBytes<8", "claim check C2"),
    ("claim-view-never-rewritten", "if(next==g_claimViewMirror)return;", "if(true)return;", "claim check C1"),
    ("claim-view-unsorted", "while(at && next.address[at-1]>e.first)", "while(at && next.address[at-1]<e.first)", "claim check C1"),
    ("claim-view-omits-plans", "ClaimViewState next;next.plans=!g_mergePlans.empty();", "ClaimViewState next;next.plans=false;", "claim check C6"),
    ("claim-view-read-not-rechecked", "return g_claimView.seq.load(std::memory_order_relaxed)==before;", "return true;", "claim check C7"),
    # The pool copier's claim check (round 6; cases C8-C12): each removes one reason to fall back, one input of the decision or one counter.
    # The rig runs C8, C10, C11, C9, C12 and stops at the first failing check, so a mutant is labelled with the first case that fails it; the four
    # labelled C8 (hits ignored, range one short, bound never raised, bound lost) are also killed by C9 alone (checked with C9 moved first).
    ("copier-claim-ignores-plans", "if(plans || claims>kClaimViewMax || leases>kMaxPools)return declined();",
     "if(claims>kClaimViewMax || leases>kMaxPools)return declined();", "claim check C10"),
    ("copier-claim-ignores-the-limit", "if(plans || claims>kClaimViewMax || leases>kMaxPools)return declined();",
     "if(plans || leases>kMaxPools)return declined();", "claim check C10"),
    ("copier-claim-ignores-hits", "if(claimed[i]>=source && offset%kBytes==0 && offset/kBytes<count)return false;", "if(false)return false;", "claim check C8"),
    ("copier-claim-range-one-short", "offset/kBytes<count)return false;", "offset/kBytes+1<count)return false;", "claim check C8"),
    ("copier-claim-ignores-ambiguity", "if(matches>1)return false;   // (a) ambiguous", "", "claim check C9"),
    ("copier-claim-ignores-fit", "if(stride!=kBytes || firstSlot>slots || count>slots-firstSlot)return false;   // (b) does not fit",
     "if(stride!=kBytes)return false;", "claim check C9"),
    ("copier-claim-ignores-stride", "if(stride!=kBytes || firstSlot>slots || count>slots-firstSlot)return false;   // (b) does not fit",
     "if(firstSlot>slots || count>slots-firstSlot)return false;", "claim check C9"),
    ("copier-claim-ignores-patches",
     "if(patchLo<=patchHi && uint64_t(patchLo)<uint64_t(firstSlot)+count && patchHi>=firstSlot)return false;   // (c) a patch to erase", "", "claim check C9"),
    ("copier-claim-patch-bound-high-one-short", "patchHi>=firstSlot)return false;   // (c)", "patchHi>firstSlot)return false;   // (c)", "claim check C9"),
    ("copier-claim-patch-bound-low-one-short", "uint64_t(patchLo)<uint64_t(firstSlot)+count", "uint64_t(patchLo)+1<uint64_t(firstSlot)+count", "claim check C9"),
    ("copier-claim-window-not-rechecked", "if(g_publishSeq.load(std::memory_order_relaxed)!=before)return declined();", "", "claim check C11"),
    ("publish-window-never-opens", "if(open)return;\n        open=true;", "return;\n        open=true;", "claim check C11"),
    ("copier-claim-counts-nothing", "g_copierPrechecked.fetch_add(1,std::memory_order_relaxed);\n    if(!matches)", "if(!matches)", "claim check C8"),
    ("copier-claim-no-lease-uncounted", "if(!matches)g_copierPrecheckedNoLease.fetch_add(1,std::memory_order_relaxed);", "", "claim check C8"),
    ("copier-claim-declines-uncounted", "g_copierPrecheckDeclined.fetch_add(1,std::memory_order_relaxed);return false;", "return false;", "claim check C10"),
    ("pool-patch-bound-never-raised", "notePatch(*pool,patch.slot);", "", "claim check C8"),
    ("pool-view-bound-lost", "lo[count]=held?(entry.second.patchLo<=entry.second.patchHi?entry.second.patchLo:0u):~0u;", "lo[count]=~0u;", "claim check C8"),
    ("pool-view-compare-ignores-bounds",
     "lo[i]==g_poolView.patchLo[i].load(std::memory_order_relaxed) && hi[i]==g_poolView.patchHi[i].load(std::memory_order_relaxed);", "true;", "claim check C9"),
]
CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX",
            "/D_CRT_SECURE_NO_WARNINGS", "/DUNICODE", "/D_UNICODE", "/utf-8", "/DEDVR_ENGINE_VELOCITY_RIG",
            "/DEDVR_BINDING_SHADOW_EXTERNAL"]
LIBS = ["d3d11.lib", "d3dcompiler.lib", "dxguid.lib"]


def read(path):
    return path.read_text(encoding="utf-8")


def self_test():
    source, rig, cases, build = read(SOURCE), read(RIG), read(CASES), read(ROOT / "build.bat")
    header, claim_cases = read(HEADER), read(CLAIM_CASES)
    names = [m[0] for m in MUTATIONS + HEADER_MUTATIONS]
    assert len(set(names)) == len(names), "duplicate mutation name"
    for name, anchor, replacement, case in MUTATIONS:
        assert source.count(anchor) == 1, name + ": anchor found " + str(source.count(anchor)) + " times, want 1"
        assert anchor != replacement, name + ": no edit"
        assert case + ":" in cases + read(GUARD_CASES), name + ": the rig has no check '" + case + ":'"
    for name, anchor, replacement, case in HEADER_MUTATIONS:
        assert header.count(anchor) == 1, name + ": anchor found " + str(header.count(anchor)) + " times in the header, want 1"
        assert anchor != replacement, name + ": no edit"
        assert case + ":" in claim_cases, name + ": the rig has no check '" + case + ":'"
    assert '#include "claim_check_tests.h"' in rig and "claim_check_tests::run(" in rig and 'L"--claim-check"' in rig, "the rig does not run the claim check"
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
    print("engine_velocity mutants: PASS " + str(len(MUTATIONS) + len(HEADER_MUTATIONS)) + " anchors, cases, order, build gate")


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


def compile_obj(tc, source, temp, name, extra=(), include=None):
    obj = temp / (name + ".obj")
    cmd = [tc.cl] + CL_FLAGS + ["/c", "/I" + str(GEN), "/I" + str(include or ROOT / "src" / "d3d11")] + list(extra) + \
          ["/Fo" + str(obj), str(source)]
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(temp))
    if done.returncode:
        raise RuntimeError(name + ": compile failed\n" + done.stdout + done.stderr)
    return obj


def link_and_run(tc, objs, temp, name, mode="--self-test"):
    exe = temp / (name + ".exe")
    done = subprocess.run([tc.cl, "/nologo"] + [str(o) for o in objs] + ["/Fe" + str(exe), "/link", "/INCREMENTAL:NO"] + LIBS,
                          capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(temp))
    if done.returncode:
        raise RuntimeError(name + ": link failed\n" + done.stdout + done.stderr)
    # From the repository root: the rig reads some sources by relative path (build.bat runs it there too).
    return subprocess.run([str(exe), mode], capture_output=True, text=True, errors="replace", cwd=str(ROOT))


def run(only, jobs):
    if not GEN.is_dir():
        raise RuntimeError("build\\gen is missing: run build.bat once so the generated shader headers exist")
    known = {m[0] for m in MUTATIONS + HEADER_MUTATIONS}
    if only - known:
        raise ValueError("unknown mutation: " + ", ".join(sorted(only - known)))
    selected = [m for m in MUTATIONS if not only or m[0] in only]
    selected_header = [m for m in HEADER_MUTATIONS if not only or m[0] in only]
    tc = toolchain()
    source = read(SOURCE)
    with tempfile.TemporaryDirectory(prefix="edvr-engine-velocity-mutants-") as directory:
        temp = Path(directory)
        shared = [compile_obj(tc, RIG, temp, "rig"),
                  compile_obj(tc, ROOT / "src" / "d3d11" / "skin_join_gpu.cpp", temp, "skin_join_gpu"),   # (build.bat links it; --run did not, since F2)
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
        header_text = read(HEADER)

        def one_header(mutation):
            name, anchor, replacement, case = mutation
            mirror = temp / name
            for relative in MIRROR:
                shutil.copytree(ROOT / relative, mirror / relative, dirs_exist_ok=True,
                                ignore=shutil.ignore_patterns("*.obj", "*.exe", "*.pdb"))
            (mirror / "src" / "d3d11" / HEADER.name).write_text(header_text.replace(anchor, replacement, 1), encoding="utf-8")
            include = mirror / "src" / "d3d11"
            rig = compile_obj(tc, mirror / "tools" / "engine_velocity_test" / RIG.name, mirror, "rig", include=include)
            core = compile_obj(tc, include / SOURCE.name, mirror, "engine_velocity", include=include)
            return name, case, link_and_run(tc, [rig, core] + shared[1:], mirror, name, "--claim-check")

        failures = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            pending = [pool.submit(one_header, m) for m in selected_header]
            for name, case, result in pool.map(one, selected):
                labels = re.findall(r"^FAIL: ((?:cap R|guard G|claim check C)\d+|flat lazy):", result.stderr, re.M)
                if result.returncode != 1 or case not in labels:
                    failures.append(name + ": survived or failed the wrong check (exit " + str(result.returncode) + ", wanted " + case
                                    + ")\n" + result.stdout + result.stderr)
                else:
                    print(name + ": killed by " + case)
            for future in pending:
                name, case, result = future.result()
                labels = re.findall(r"^FAIL: ((?:cap R|guard G|claim check C)\d+|flat lazy):", result.stderr, re.M)
                if result.returncode != 1 or case not in labels:
                    failures.append(name + ": survived or failed the wrong check (exit " + str(result.returncode) + ", wanted " + case
                                    + ")\n" + result.stdout + result.stderr)
                else:
                    print(name + ": killed by " + case)
        if failures:
            raise RuntimeError("\n".join(failures))
    print("engine_velocity mutants: PASS " + str(len(selected) + len(selected_header)) + " killed")


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
        for name, _, _, case in MUTATIONS + HEADER_MUTATIONS:
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

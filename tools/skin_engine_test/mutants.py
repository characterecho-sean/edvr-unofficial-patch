#!/usr/bin/env python3
"""The mutation proof for tools\\skin_engine_test: the rig fails when a rule of the second skin's engine side is broken.

The rig (skin_engine_test.cpp, with tools\\engine_velocity_test\\skin_compose_tests.h and skin_lifecycle_tests.h) holds the compose's half of the second
skin (C1 the arithmetic, C2 the production mv pass on real resources, C3 the derived blend state) and the engine's draw half end to end (L1..L9: the
linked engine_velocity.cpp and skin_join_gpu.cpp drawing a skinned character in both eyes through the real path). Every check carries a label
"C<case>.<what>" or "L<case>.<what>". A rig that passes proves little until it is seen to FAIL on a source that breaks the rule it pins: for each
mutation below the machinery (tools\\rig_mutants_lib.py) edits a copy of the source in a temp directory OUTSIDE the repo and requires a FAIL on a check
of the case that belongs to the rule. The shipped HLSL (temporal_shader_source.h) is read by the rig as text, so a mutation of it runs through the
unmutated rig against a temp root holding the edited copy; the C++ sources are rebuilt into a fresh rig.

  python tools\\skin_engine_test\\mutants.py --self-test       text only: every anchor is found exactly once, every case named is in the rig, and
                                                               build.bat compiles the rig the way the machinery does and runs this self-test
  python tools\\skin_engine_test\\mutants.py --run [--only a,b] [--jobs N] [--keep] [--verbose]     (needs a finished build: it reads build\\gen)
  python tools\\skin_engine_test\\mutants.py --list
  python tools\\skin_engine_test\\mutants.py --run --dry-run   the plan; writes nothing, starts nothing
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import rig_mutants_lib as lib  # noqa: E402

ROOT = HERE.parents[1]
D3D = ROOT / "src" / "d3d11"
FILES = {
    "hlsl": D3D / "temporal_shader_source.h",
    "state": D3D / "engine_velocity_state.h",
    "engine": D3D / "engine_velocity.cpp",
    "gpu": D3D / "skin_join_gpu.cpp",
    "join": D3D / "skin_join.h",
}
HEADER_KEYS = ("state", "engine", "gpu", "join")
PIN_KEYS = ("hlsl",)


def tree_files():
    """Everything else the rig compiles or includes, copied into the temp tree unmutated: the source headers, the test suites, the generated shaders."""
    skip = {FILES[k].resolve() for k in HEADER_KEYS}
    out = []
    for pattern in ("src/d3d11/*.h", "src/common/*.h", "src/d3d11/gpu_timing.cpp", "src/d3d11/gpu_span_d3d11.cpp", "tools/engine_velocity_test/*.h",
                    "tools/skin_join_test/skin_join_world.h", "tools/skin_clone_test/synthetic_skin.h", "tools/skin_engine_test/skin_engine_test.cpp",
                    "third_party/dxbc_hash/DxilHash.cpp", "build/gen/*.h", "build/gen/*.inc"):
        for p in sorted(ROOT.glob(pattern)):
            if p.resolve() in skip or not p.is_file():
                continue
            out.append((p.relative_to(ROOT).as_posix(), p))
    return out


UNITS = [("src/d3d11/engine_velocity.cpp", "engine"), ("src/d3d11/skin_join_gpu.cpp", "gpu"), ("src/d3d11/gpu_timing.cpp", None), ("src/d3d11/gpu_span_d3d11.cpp", None)]
CL_FLAGS = lib.DEFAULT_CL_FLAGS + ["/DUNICODE", "/D_UNICODE", "/utf-8", "/DEDVR_ENGINE_VELOCITY_RIG", "/DEDVR_BINDING_SHADOW_EXTERNAL"]
CONFIG = lib.Config(
    here=__file__, files=FILES, header_keys=HEADER_KEYS, pin_keys=PIN_KEYS, rig=HERE / "skin_engine_test.cpp",
    rig_label=":rig_skin_engine_test", rig_source_in_bat="tools\\skin_engine_test\\skin_engine_test.cpp", rig_exe_in_bat="skin_engine_test.exe",
    case_prefix="[CL]", include_dirs=("build/gen", "src/d3d11"), include_aliases={"build/gen": '/I"%GEN%"'}, cl_flags=CL_FLAGS, units=UNITS,
    link_args=["d3d11.lib", "d3dcompiler.lib", "dxguid.lib"], min_mutants=30, run_timeout=300.0, tree_extra=tree_files(), rig_in_tree=True)

M = lib.M
MUTANTS = [
    # ---- C1, C2: the compose (the shipped HLSL, read as text) ----
    M("skinned-taken-as-rigid", "C2", "hlsl", [("if ((uint(probe.w + 0.5) & 16384u) != 0u && r.data[0].x != 0u) {", "if (false) {")],
      "a skinned record goes down the rigid path: no marker, no E"),
    M("rigid-reads-target-7", "C2", "hlsl", [("if ((uint(probe.w + 0.5) & 16384u) != 0u && r.data[0].x != 0u) {", "if ((uint(probe.w + 0.5) & 16384u) != 0u) {")],
      "a rigid record reads target 7 too"),
    M("bit-16384-ignored", "C2", "hlsl", [("if ((uint(probe.w + 0.5) & 16384u) != 0u && r.data[0].x != 0u) {", "if (r.data[0].x != 0u) {")],
      "target 7 is read whether the pass says it exists or not"),
    M("valid-flag-ignored", "C2", "hlsl", [("if (!(sk.w > 0.5) || !all(isfinite(sk.xyz))) return 2u;", "if (!all(isfinite(sk.xyz))) return 2u;")],
      "a pixel with valid 0 takes its E anyway"),
    M("unreprojectable-not-masked", "C2", "hlsl", [("if (!engineReprojectSkinned(r, sk.xyz, skNdc, zr, skBefore)) return 2u;", "if (!engineReprojectSkinned(r, sk.xyz, skNdc, zr, skBefore)) return 0u;")],
      "a previous position behind last frame's camera keeps the camera term instead of no history"),
    M("skinned-count-wrong", "C2", "hlsl", [("        gSkinKind = 1u;\n        gSkinMagnitudeCm", "        gSkinKind = 2u;\n        gSkinMagnitudeCm")],
      "a joined skinned pixel is counted as masked"),
    M("masked-count-wrong", "C2", "hlsl", [("InterlockedAdd(gCount[55], 1u);", "InterlockedAdd(gCount[54], 1u);")], "a masked skinned pixel is counted as joined"),
    M("bins-wrong", "C2", "hlsl", [("4.0 * log10(max(gSkinMagnitudeCm, 1e-6) / 0.01)", "2.0 * log10(max(gSkinMagnitudeCm, 1e-6) / 0.01)")],
      "the |E| histogram's bins are twice as wide"),
    M("sign-flipped", ("C1", "C2"), "hlsl", [("prevWorld = world + (nCam - bCam) + skinMove;", "prevWorld = world + (nCam - bCam) - skinMove;")],
      "the surface goes forward in time"),
    M("camera-term-dropped", "C1", "hlsl", [("prevWorld = world + (nCam - bCam) + skinMove;", "prevWorld = world + skinMove;")],
      "the camera's own motion is not carried"),
    M("centimetres-as-decimetres", "C1", "hlsl", [("engineReprojectRowsE(r, true, skinCm * 0.01,", "engineReprojectRowsE(r, true, skinCm * 0.1,")], "E is read as decimetres"),
    M("skinned-flag-dropped", "C1", "hlsl", [("engineReprojectRowsE(r, true, skinCm * 0.01,", "engineReprojectRowsE(r, false, skinCm * 0.01,")],
      "the skinned reprojection reads the record's pose blocks"),
    M("skinned-branch-never-taken", "C1", "hlsl", [("    if (skinned) {\n        prevWorld", "    if (false) {\n        prevWorld")], "the skinned arithmetic is unreachable"),
    # ---- C3: the derived blend state ----
    M("mode-1-writes-target-7", ("C3", "L6"), "state", [("k.RenderTargetWriteMask = skinMode == 2 ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;", "k.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;")],
      "a draw that exports nothing to target 7 leaves undefined values there"),
    M("mode-2-writes-red-only", ("C3", "L6"), "state", [("k.RenderTargetWriteMask = skinMode == 2 ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;", "k.RenderTargetWriteMask = skinMode == 2 ? D3D11_COLOR_WRITE_ENABLE_RED : 0;")],
      "E is written to one channel"),
    M("mode-0-touches-target-7", "C3", "state", [("    if (skinMode != 0) {\n        D3D11_RENDER_TARGET_BLEND_DESC& k", "    if (true) {\n        D3D11_RENDER_TARGET_BLEND_DESC& k")],
      "the derived state overrides target 7 where the pass has no second skin"),
    # ---- L: the engine ----
    M("rigid-draws-leave-target-7-open", "L6", "engine", [("const int skinMode = e.skinBound ? ((exportsE || zeroes) ? 2 : 1) : 0;", "const int skinMode = e.skinBound && skinDraw ? ((exportsE || zeroes) ? 2 : 1) : 0;")],
      "a rigid family's draw writes undefined values into target 7"),
    M("rigid-draws-write-target-7", "L6", "engine", [("const int skinMode = e.skinBound ? ((exportsE || zeroes) ? 2 : 1) : 0;", "const int skinMode = e.skinBound ? 2 : 0;")],
      "a rigid family's draw writes target 7 through the derived blend state (the pixel shader has no output there)"),
    M("no-history-write-masked", ("L6", "L11"), "engine", [("const int skinMode = e.skinBound ? ((exportsE || zeroes) ? 2 : 1) : 0;", "const int skinMode = e.skinBound ? (exportsE ? 2 : 1) : 0;")],
      "a skinned family's pixel shader that exports no E is write-masked off target 7: its pixels keep the E an earlier draw wrote there"),
    M("no-history-variant-never-made", "L6", "engine", [("for (int mode = wantsExport ? 0 : skinFamily ? 1 : 2; mode <= 2 && !patched; ++mode) {", "for (int mode = wantsExport ? 0 : 2; mode <= 2 && !patched; ++mode) {")],
      "a skinned family's pixel shader that exports no E is made without the no-history write: its draws are not substituted"),
    M("skin-views-not-bound", ("L2", "L3", "L6"), "engine", [("        ctx->VSSetShaderResources(kSkinPrevPaletteSlot, 3, skinViews);\n        engineVelocityNoteStateCalls(1);\n        g_bound.skinSrvs = true;",
                                                               "        g_bound.skinSrvs = true;")],
      "the cloned vertex shader reads unbound views"),
    M("skin-views-kept-at-the-boundary", "L6", "engine", [("    if (ctx && g_bound.skinSrvs) {\n        ID3D11ShaderResourceView* none[3] = {};", "    if (false && ctx && g_bound.skinSrvs) {\n        ID3D11ShaderResourceView* none[3] = {};")],
      "the three skin views stay bound across the frame"),
    M("target-7-not-cleared", "L3", "engine", [("ctx->ClearRenderTargetView(e.skinRtv.Get(), zero);", "(void)zero;")],
      "target 7 keeps last frame's answers where nothing is drawn"),
    M("view-not-offered", "L2", "engine", [("if (exportsE) { e.skinWrittenFrame = frame; ++g_skinStats.draws; }", "if (exportsE) { ++g_skinStats.draws; }")],
      "the compose is never given target 7"),
    M("pose-table-not-built", ("L2", "L3"), "engine", [("            g_skin.buildPose(ctx, e.poolSrv.Get(), e.poolBytes / emit::kItemBytes, ended, refs);", "            (void)refs;")],
      "the previous pose table is never made: no character has history"),
    M("draws-not-listed", "L10", "engine", [("    c.ranges.push_back(startInstance);\n    c.ranges.push_back(instances);", "    (void)startInstance;")],
      "a skinned draw's instance window is never listed: no record is live and the stale second record kills its base"),
    M("skin-draws-never-noted", "L10", "engine", [("    cache.skin = skinDraw && e.skinBound && eye != kEngineVelocitySourceEye;", "    cache.skin = false;")],
      "the draw hook's second call never reaches the list"),
    M("ambiguous-stream-trusted", "L10", "engine", [("    bool ok = eights == 1;", "    bool ok = eights >= 1;")],
      "a draw that binds two vertex buffers of stride 8 is listed against the first: the list is called complete"),
    M("stream-offset-dropped", "L10", "engine", [("if (!c.stream) { c.stream = vbs[pick]; c.streamOffset = offsets[pick]; }", "if (!c.stream) { c.stream = vbs[pick]; c.streamOffset = 0; }")],
      "the stream's input-assembler offset is not carried: the draws' entries are read from the wrong place"),
    M("incomplete-list-called-complete", "L10", "engine", [("            refs.complete = listed ? c.complete : true;", "            refs.complete = true;")],
      "a frame whose list met a draw it could not take is built as if the list were complete"),
    M("games-target-7-replaced", "L6", "engine", [("if (!gameRt7) { rt[kSkinTarget] = e.skinRtv.Get(); e.skinBound = true; }\n        else ++g_skinStats.slotTaken;", "{ rt[kSkinTarget] = e.skinRtv.Get(); e.skinBound = true; }")],
      "a game that binds its own target 7 loses it"),
    M("chain-dispatch-not-fed", ("L2", "L3"), "engine", [("    if (g_skin.noteChain(ctx, frameNow(), groups)) ++g_motion.chainCalls;", "    (void)groups;")],
      "the palette chain's dispatch never reaches the join"),
    M("hook-gate-not-opened", "L1", "engine", [("    skinEntityHookSetGate(true);\n    if (state == SkinHookState::Armed)", "    if (state == SkinHookState::Armed)")],
      "the hook is armed and its gate stays shut"),
    M("second-skin-not-wanted", "L1", "engine", [("    g_skinWanted.store(true, std::memory_order_release);\n    Log::get().note(\"skin join: the second skin is live", "    Log::get().note(\"skin join: the second skin is live")],
      "configure says it is live and the chain dispatch hook is never told"),
    M("periodic-lines-dropped", "L8", "engine", [("    skinSummaryLocked(ctx);\n    g_emit.clear();", "    g_emit.clear();")], "the window ends without the second skin's lines"),
    # ---- L: the join's GPU half ----
    M("previous-rows-unknown", "L9", "gpu", [("plan.prevRows = uint32_t(std::min<uint64_t>(previousBytes / 48u, kMaxRows));", "plan.prevRows = kMaxRows;")],
      "the previous palette buffer's size is not told to the join"),
    M("rows-in-use-guessed-without-a-list", ("L2", "L3"), "gpu", [("snap.end : 0;", "snap.end : kMaxRows;")],
      "with no list the previous buffer is compared with the whole table and found small: no history whenever the hook is down"),
    M("previous-job-table-never-refreshed", "L5", "gpu", [("        ctx->CopySubresourceRegion(s.prevJobs.Get(), 0, 0, 0, 0, s.frameJobs.Get(), 0, &box);", "        if (chainFrames_ == 1) ctx->CopySubresourceRegion(s.prevJobs.Get(), 0, 0, 0, 0, s.frameJobs.Get(), 0, &box);")],
      "last frame's job table is the first frame's: a changed character never has history again"),
    M("hook-plan-has-no-previous-entities", "L7", "join", [("if (q.vtable == e.vtable && q.mesh == e.mesh && q.count == e.count) plan.prevIdx[i] = p->second;", "if (q.vtable == e.vtable && q.mesh == e.mesh && q.count == e.count) plan.prevIdx[i] = kNone;")],
      "the hook's list names no entity from one frame to the next: nothing joins by it"),
    M("tables-not-alternated", ("L3", "L4"), "gpu", [("    s.parity ^= 1u;\n    Plan& plan", "    Plan& plan")],
      "this frame's by-base and pose tables are last frame's"),
    M("counters-never-read-back", "L8", "gpu", [("        st.at = chainFrames_;\n", "        st.at = 0;\n")], "the periodic line never gets the GPU's counters"),
    M("window-halves-unaligned", "L8", "gpu", [("const JoinFeeder::Counters& f = s.cpuNewest.feeder;", "const JoinFeeder::Counters& f = s.feeder.counters();")],
      "the CPU half of the join line is the CPU's counters at the summary, not at the chain frame the GPU's counters are of"),
    M("cpu-counters-not-snapshotted", "L8", "gpu", [("        st.cpu.feeder = s.feeder.counters();\n", "")],
      "the CPU counters are not kept with the GPU's copy: the CPU half of the line counts nothing"),
    M("views-asks-not-counted", ("L2", "L3", "L8"), "engine", [("    ++g_motion.asked;\n", "")],
      "the entry fade's signal never sees the compose ask for the views: it is never armed, so nothing is ever waited for"),
    M("armed-without-a-consumer", "L8", "engine", [("    if (!g_motion.consumer) return r;", "    ;")],
      "the signal is armed although the temporal pass is not asking for the views (it is off): every entry would be held the full second"),
    M("run-needs-one-eye-only", "L8", "engine", [("g_motion.viewsGiven[0] != g_motion.viewsSeen[0] && g_motion.viewsGiven[1] != g_motion.viewsSeen[1];", "g_motion.viewsGiven[0] != g_motion.viewsSeen[0] || g_motion.viewsGiven[1] != g_motion.viewsSeen[1];")],
      "a run of live views builds with one eye given the views"),
    M("join-live-taken-from-jobs", "L2", "engine", [("g_motion.skinLive = g_motion.skinJobs && g_skin.lastJoinLive();", "g_motion.skinLive = g_motion.skinJobs;")],
      "the signal calls the join live whenever there are skinned jobs: the fade would not wait for a join that has no history"),
    # ---- L12: the palette chain dispatched twice in a frame; the join is one per present frame over the union ----
    M("join-per-dispatch", "L12", "gpu", [("    ++s.pendingDispatches;\n    return true;\n}", "    ++s.pendingDispatches;\n    runJoin(ctx);\n    return true;\n}")],
      "the join runs at each dispatch, as it did: the second dispatch is a history gap and a late join, the character in it has none"),
    M("second-table-overwrites-first", "L12", "gpu", [("ctx->CopySubresourceRegion(s.frameJobs.Get(), 0, s.pendingJobs * 16u, 0, 0, jobsBuffer.Get(), 0, &box);", "ctx->CopySubresourceRegion(s.frameJobs.Get(), 0, 0, 0, 0, jobsBuffer.Get(), 0, &box);")],
      "each dispatch's job table is copied to the start of the frame's table: the second overwrites the first"),
    M("table-rows-not-accumulated", "L12", "gpu", [("    s.pendingJobs += take;\n", "    s.pendingJobs = take;\n")],
      "the frame's table holds the last dispatch's row count: the join sees only its jobs"),
    M("late-dispatch-rejoined", "L12", "gpu", [("if (!s.pending && s.ranPresent == present) {", "if (false) {")],
      "a dispatch after the frame's join starts a second join of the same present frame: a gap and a flipped parity inside one frame"),
    M("late-dispatch-uncounted", "L12", "gpu", [("        ++s.chainLate;\n", "")],
      "a late dispatch is not said"),
    M("multi-dispatch-frame-uncounted", "L12", "gpu", [("    if (dispatches > 1) ++s.chainMulti;\n", "")],
      "a frame with two dispatches is not said"),
    M("mixed-palette-joined", "L12", "gpu", [("verdict == kHistoryOk && groups <= kMaxJobs && !s.pendingMixed;", "verdict == kHistoryOk && groups <= kMaxJobs;")],
      "a frame whose dispatches wrote two palette buffers is joined as if one buffer were last frame's"),
    M("boundary-does-not-join", "L12", "engine", [("    if (ctx) g_skin.flushPending(ctx);\n", "")],
      "a frame whose chain no skinned draw needed is not joined at the boundary: the entry fade reads the frame before's join as this frame's"),
    M("draw-does-not-join", ("L2", "L3", "L12"), "engine", [("        g_skin.flush(ctx, frame);   // the frame's chain dispatches", "        // the frame's chain dispatches")],
      "the first skinned draw does not run the frame's join: the draws bind the empty one, every valid flag is 0"),
    M("join-live-always", ("L2", "L12"), "gpu", [("    return views(impl_->joinPresent).live;", "    return true;")],
      "the last join is called live whatever its history"),
    M("pose-line-idle-and-unresolved-swapped", "L10", "join", [("d[kStatPoseIdle], d[kStatPoseUnresolved], unlisted", "d[kStatPoseUnresolved], d[kStatPoseIdle], unlisted")],
      "the witness line prints the idle and the unresolved bases the wrong way round"),
    M("chain-line-late-and-multi-swapped", "L12", "join", [("(unsigned long long)c.chainMulti, (unsigned long long)c.chainLate", "(unsigned long long)c.chainLate, (unsigned long long)c.chainMulti")],
      "the chain line prints the late dispatches where the frames with two belong"),
    M("chain-job-count-not-told-to-hook", "L7", "gpu", [("    skinEntityHookNoteChain(groups);", "    ;")],
      "the hook is never told its job table has jobs: an empty list is judged by nothing"),
]

if __name__ == "__main__":
    sys.exit(lib.main(CONFIG, MUTANTS))

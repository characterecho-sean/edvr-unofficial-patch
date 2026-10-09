#!/usr/bin/env python3
"""The mutation proof for tools\\skin_join_gpu_test: the rig fails when the GPU half of the second skin's join is broken.

The rig (skin_join_gpu_test.cpp) compiles the shipped HLSL (src\\d3d11\\skin_join_shader.h: JoinCS and the pose table's clear, scatter and verify
passes), runs it on WARP beside the CPU model (src\\d3d11\\skin_join.h) and requires word-for-word agreement: the numbers in the text are the header's
(G1), a steady world through hook and prefix (G2), twenty scripted changes (G3), two hundred random frames (G4), and the pose passes (G5). Every
check carries a label "G<case>.<what>". A rig that passes proves little until it is seen to FAIL on a source that breaks the rule it pins: for each
mutation below the machinery (tools\\rig_mutants_lib.py) copies the sources into a temp directory OUTSIDE the repo, applies the edit to the HLSL text
or the CPU twin, rebuilds the rig against the copy and requires a FAIL on a check of the case that belongs to the rule.

  python tools\\skin_join_gpu_test\\mutants.py --self-test       text only: every anchor is found exactly once, every case named is in the rig,
                                                                 and build.bat compiles the rig the way the machinery does and runs this self-test
  python tools\\skin_join_gpu_test\\mutants.py --run [--only a,b] [--jobs N] [--keep] [--verbose]
  python tools\\skin_join_gpu_test\\mutants.py --list
  python tools\\skin_join_gpu_test\\mutants.py --run --dry-run   the plan; writes nothing, starts nothing
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import rig_mutants_lib as lib  # noqa: E402

ROOT = HERE.parents[1]
FILES = {
    "shader": ROOT / "src" / "d3d11" / "skin_join_shader.h",
    "join": ROOT / "src" / "d3d11" / "skin_join.h",
}
CONFIG = lib.Config(
    here=__file__, files=FILES, header_keys=("shader", "join"), pin_keys=(), rig=HERE / "skin_join_gpu_test.cpp",
    rig_label=":rig_skin_join_gpu_test", rig_source_in_bat="tools\\skin_join_gpu_test\\skin_join_gpu_test.cpp", rig_exe_in_bat="skin_join_gpu_test.exe",
    case_prefix="G", include_dirs=("src/d3d11",), link_args=["d3d11.lib", "d3dcompiler.lib", "dxguid.lib"], min_mutants=30, run_timeout=240.0)

M = lib.M
ALL = ("G2", "G3", "G4")   # the scripted and random worlds both exercise the join; any of them may be the first to disagree
MUTANTS = [
    # ---- G1: the numbers the two sides share ----
    M("rows-differ", "G1", "shader", [("#define SJ_MAX_ROWS 65536u", "#define SJ_MAX_ROWS 32768u")], "the HLSL covers half the rows the header does"),
    M("plan-offset-differs", "G1", "shader", [("#define SJ_PLAN_COUNT 1033u", "#define SJ_PLAN_COUNT 1034u")], "the HLSL reads the entity counts one word late"),
    M("hook-flag-differs", "G1", "join", [("kPlanHook = 2,", "kPlanHook = 4,")], "the header offers the hook with another bit"),
    M("counter-index-differs", "G1", "shader", [("#define SJ_STAT_JOINED 7u", "#define SJ_STAT_JOINED 8u")], "the HLSL counts joins into another counter"),
    M("mismatch-bit-differs", "G1", "shader", [("#define SJ_MM_SUM 8u", "#define SJ_MM_SUM 64u")], "the HLSL names a disagreement with another bit"),
    # ---- G2, G3, G4: the join ----
    M("previous-verification-ignored", ALL, "shader", [("const bool useHook = history && hookOk && prevHookOk != 0u;", "const bool useHook = history && hookOk;")],
      "the hook is trusted on its first agreeing frame"),
    M("disagreement-ignored", ALL, "shader", [("const bool useHook = history && hookOk && prevHookOk != 0u;", "const bool useHook = history && offered && prevHookOk != 0u;")],
      "the hook is used on a frame the table disagreed with"),
    M("jobs-sum-ignored", ALL, "shader", [("if (gSum != endRow - PW(SJ_PLAN_RS)) gMismatch |= SJ_MM_SUM;", ";")], "jobs that do not add up to the end are not a disagreement"),
    M("head-number-ignored", ALL, "shader", [("if (gHeads != m) gMismatch |= SJ_MM_HEADS;", ";")], "an entry with no job at its base is not a disagreement"),
    M("entity-tiling-ignored", ALL, "shader", [("if (gEntSum[ee] != PW(SJ_PLAN_RS + ee + 1u) - PW(SJ_PLAN_RS + ee)) InterlockedOr(gMismatch, SJ_MM_ENTITY_SUM, o);", ";")],
      "an entity whose jobs do not tile its range is not a disagreement"),
    M("head-count-ignored", ALL, "shader", [("if (row.w != PW(SJ_PLAN_COUNT + ent)) InterlockedOr(gMismatch, SJ_MM_HEAD_COUNT, o);", ";")],
      "a primary job with another count than its entry's is not a disagreement"),
    M("job-outside-ranges-ignored", ALL, "shader", [("if (ent == SJ_NONE) { InterlockedOr(gMismatch, SJ_MM_NOT_IN_RANGE, o); continue; }", "if (ent == SJ_NONE) { continue; }")],
      "a job outside every entity's range is not a disagreement"),
    M("range-change-ignored", ALL, "shader", [("if (rs1 - rs0 != ps1 - ps0) { fRange += 1u; continue; }", ";")], "an entity whose range changed keeps its history"),
    M("layout-bind-ignored", ALL, "shader", [("pinfo.y != row.w || pinfo.x != row.z", "pinfo.y != row.w")], "a child replaced by another bind keeps its history"),
    M("layout-count-ignored", ALL, "shader", [("pinfo.y != row.w || pinfo.x != row.z", "pinfo.x != row.z")], "a child with another bone count keeps its history"),
    M("pose-record-unchecked", ALL, "shader", [("|| PrevPose[prevDst].a.x != prevDst) { fPose += 1u; continue; }", ") { fPose += 1u; continue; }")],
      "a base with no previous pose record is joined"),
    M("prefix-ignores-bind", ALL, "shader", [("if (a.z != b.z || a.w != b.w) { uint o; InterlockedMin(gPrefix, k, o); }", "if (a.w != b.w) { uint o; InterlockedMin(gPrefix, k, o); }")],
      "the prefix compares only the bone counts"),
    M("previous-rows-unguarded", ALL, "shader", [("if (prevDst < SJ_MAX_ROWS && prevDst + row.w > prevRows) { fPrevRows += 1u; continue; }", ";")],
      "a job whose previous rows run past the previous palette buffer keeps its history"),
    M("previous-rows-word-misread", ALL, "shader", [("endRow = PW(3u), prevRows = PW(7u);", "endRow = PW(3u), prevRows = PW(4u);")],
      "the previous buffer's capacity is read from the wrong plan word"),
    M("duplicates-not-counted", ALL, "shader", [("else dupFails += 1u;", "else dupFails += 0u;")], "a second job on a base is not counted"),
    M("cap-failures-not-counted", ALL, "shader", [("capFails += 1u; continue;", "continue;")], "a job past the tables is not counted"),
    M("join-table-not-cleared", ALL, "shader", [("  JoinOut[i] = 0u;\n", "")], "last frame's joins survive into this frame"),
    M("by-base-table-not-cleared", ALL, "shader", [("  Info.Store2(i * 8u, uint2(0u, 0u));\n", "")], "last frame's by-base entries survive into this frame"),
    M("verification-always-remembered", ALL, "shader", [("Stats.Store(SJ_STAT_PREV_HOOK_OK * 4u, (offered && gMismatch == 0u) ? 1u : 0u);", "Stats.Store(SJ_STAT_PREV_HOOK_OK * 4u, 1u);")],
      "the frame after a disagreement trusts the hook"),
    M("no-history-still-joins", ALL, "shader", [("if (history) {\n  [loop] for (uint jd = tid;", "if (true) {\n  [loop] for (uint jd = tid;")], "without certified history jobs are joined anyway"),
    M("no-history-uncounted", ALL, "shader", [("else Stats.Store(SJ_STAT_NO_HISTORY * 4u, Stats.Load(SJ_STAT_NO_HISTORY * 4u) + 1u);", ";")],
      "a frame without history is not counted"),
    # ---- G5: the pose passes with no reference list ----
    M("pose-record-misplaced", "G5", "shader", [(") == 0u) PoseOut[base] = p;", ") == 0u) PoseOut[base + 1u] = p;")], "a record lands one element late"),
    M("conflict-not-dropped", "G5", "shader", [(" PoseOut[id.x] = z;\n uint o; Stats.InterlockedAdd(SJ_STAT_POSE_DROPPED * 4u, 1u, o);", " uint o; Stats.InterlockedAdd(SJ_STAT_POSE_DROPPED * 4u, 1u, o);")],
      "two records of one base that disagree both stand: the base is counted dropped and its entry is kept"),
    M("word-seven-compared", "G5", "shader", [("t.b.y == p.b.y && t.b.z == p.b.z;", "t.b.y == p.b.y && t.b.z == p.b.z && t.b.w == p.b.w;")], "a difference in word 7 alone kills the base"),
    M("base-zero-written", "G5", "shader",
      [(" uint base = p.a.x;\n if (base == 0u || base >= rows) return;\n if ((BaseState.Load", " uint base = p.a.x;\n if (base >= rows) return;\n if ((BaseState.Load")],
      "a record with base 0 is written"),
    M("conflicts-uncounted", "G5", "shader", [("Stats.InterlockedAdd(SJ_STAT_POSE_CONFLICTS * 4u, 1u, o);", "Stats.InterlockedAdd(SJ_STAT_POSE_CONFLICTS * 4u, 0u, o);")],
      "a conflict is not counted"),
    M("records-miscounted", "G5", "shader", [("Stats.InterlockedAdd(SJ_STAT_POSE_RECORDS * 4u, 1u, o);", "Stats.InterlockedAdd(SJ_STAT_POSE_RECORDS * 4u, 2u, o);")],
      "a record is counted twice"),
    M("pose-clear-skipped", ("G2", "G3", "G4", "G5"), "shader", [("  PoseOut[id.x] = z;\n  BaseState.Store(id.x * 4u, 0u);", "  z.a.x = 0u;\n  BaseState.Store(id.x * 4u, 0u);")],
      "the pose table is not cleared before a frame's records are scattered"),
    M("base-state-not-cleared", ("G5", "G6", "G7"), "shader", [("  BaseState.Store(id.x * 4u, 0u);\n }", "  z.a.x = 0u;\n }")],
      "last frame's live and conflict marks survive into this frame's table"),
    M("bitmap-not-cleared", ("G6", "G7"), "shader", [(" if (id.x <= SJ_REF_WORDS) RefBits.Store(id.x * 4u, 0u);", " if (id.x < 0u) RefBits.Store(id.x * 4u, 0u);")],
      "the records the last frame's draws read stay read"),
    # ---- G6, G7: which record of a base is the live one ----
    M("idle-uncounted", "G6", "shader", [(" if (RefValid()) Stats.InterlockedAdd(((BaseState.Load(id.x * 4u) & SJ_POSE_LIVE) != 0u ? SJ_STAT_POSE_UNRESOLVED : SJ_STAT_POSE_IDLE) * 4u, 1u, o);", "")],
      "the dropped bases are not split into idle (no draw read them) and unresolved (records a draw read disagree)"),
    M("idle-and-unresolved-swapped", "G6", "shader", [("? SJ_STAT_POSE_UNRESOLVED : SJ_STAT_POSE_IDLE)", "? SJ_STAT_POSE_IDLE : SJ_STAT_POSE_UNRESOLVED)")],
      "a base no draw read is called unresolved and one whose read records disagree is called idle"),
    M("unlisted-called-idle", "G6", "shader", [(" if (RefValid()) Stats.InterlockedAdd(((BaseState", " Stats.InterlockedAdd(((BaseState")],
      "with no exact draw list a dropped base is called idle or unresolved: nobody's reading decided it"),
    M("reference-test-ignored", ("G6", "G7"), "shader", [(" if (RefValid() && Referenced(id.x)) {\n  PoseOut[base] = p;", " if (false) {\n  PoseOut[base] = p;")],
      "no record is ever live: every record decides, and the stale second set kills its bases"),
    M("rest-ignores-live", ("G6", "G7"), "shader", [("if ((BaseState.Load(base * 4u) & SJ_POSE_LIVE) == 0u) PoseOut[base] = p;", "PoseOut[base] = p;")],
      "the records no draw read overwrite the live one in the table"),
    M("unreferenced-counts-as-conflict", ("G6", "G7"), "shader", [(" const bool decisive = !hasLive || Referenced(id.x);", " const bool decisive = true;")],
      "a record no draw read still decides a base that has a live record: the stale second set kills the base"),
    M("live-disagreement-ignored", ("G6", "G7"), "shader", [(" const bool decisive = !hasLive || Referenced(id.x);", " const bool decisive = !hasLive;")],
      "two records that draws both read and that disagree are not a conflict"),
    M("no-live-disagreement-ignored", ("G6", "G7"), "shader", [(" const bool decisive = !hasLive || Referenced(id.x);", " const bool decisive = hasLive && Referenced(id.x);")],
      "two records that disagree and that no draw read are not a conflict"),
    M("incomplete-list-trusted", ("G6", "G7"), "shader", [("bool RefValid() { return (poseFlags & 1u) != 0u && RefBits.Load(SJ_REF_WORDS * 4u) == 0u; }", "bool RefValid() { return RefBits.Load(SJ_REF_WORDS * 4u) == 0u; }")],
      "a list the CPU did not call complete is used"),
    M("unreadable-list-trusted", ("G6", "G7"), "shader", [("bool RefValid() { return (poseFlags & 1u) != 0u && RefBits.Load(SJ_REF_WORDS * 4u) == 0u; }", "bool RefValid() { return (poseFlags & 1u) != 0u; }")],
      "a list with an entry the GPU could not read is used"),
    M("entry-span-unchecked", ("G6", "G7"), "shader", [("  if (entry >= instFirst && entry - instFirst < instEntries) {", "  if (true) {")],
      "an entry outside the copied span is read (as zero: record 0 is taken for read)"),
    M("record-bound-unchecked", ("G6", "G7"), "shader", [("   if (rec < records && rec < SJ_REF_WORDS * 32u) {", "   if (rec < SJ_REF_WORDS * 32u) {")],
      "an entry naming a record the pool does not hold is accepted"),
    M("range-limit-unchecked", "G6", "shader", [(" bool bad = r.y > SJ_MAX_RANGE_INSTANCES;", " bool bad = false;")], "a draw naming more instances than the limit is accepted"),
    M("lists-counted-backwards", ("G6", "G7"), "shader", [("(RefBits.Load(SJ_REF_WORDS * 4u) == 0u ? SJ_STAT_POSE_LISTS_EXACT : SJ_STAT_POSE_LISTS_BAD)", "(RefBits.Load(SJ_REF_WORDS * 4u) == 0u ? SJ_STAT_POSE_LISTS_BAD : SJ_STAT_POSE_LISTS_EXACT)")],
      "an exact list is counted unreadable and the other way round"),
    M("dropped-uncounted", ("G5", "G6", "G7"), "shader", [("uint o; Stats.InterlockedAdd(SJ_STAT_POSE_DROPPED * 4u, 1u, o);", "uint o; Stats.InterlockedAdd(SJ_STAT_POSE_DROPPED * 4u, 0u, o);")],
      "a dropped base is not counted"),
    M("resolved-uncounted", ("G6", "G7"), "shader", [("Stats.InterlockedAdd(SJ_STAT_POSE_RESOLVED * 4u, 1u, o);", "Stats.InterlockedAdd(SJ_STAT_POSE_RESOLVED * 4u, 0u, o);")],
      "an overruled record is not counted"),
    M("twin-first-writer", ("G6", "G7"), "join", [("if (valid && referenced(i)) { r.table[base] = pool[i]; state[base] |= 1; }", "if (valid && !(state[base] & 1)) { r.table[base] = pool[i]; state[base] |= 1; }")],
      "(the twin) the first record of a base is its live one, read or not"),
    M("twin-last-writer", ("G6", "G7"), "join", [("if (valid && referenced(i)) { r.table[base] = pool[i]; state[base] |= 1; }", "if (valid) { r.table[base] = pool[i]; state[base] |= 1; }")],
      "(the twin) the last record of a base is its live one, read or not"),
    M("twin-reference-test-ignored", ("G6", "G7"), "join", [("if (valid && referenced(i)) { r.table[base] = pool[i]; state[base] |= 1; }", "if (false) { r.table[base] = pool[i]; state[base] |= 1; }")],
      "(the twin) no record is ever live"),
]

if __name__ == "__main__":
    sys.exit(lib.main(CONFIG, MUTANTS))

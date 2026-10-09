#!/usr/bin/env python3
"""The mutation proof for tools\\skin_join_test: the rig fails when a rule of the second skin's identity join is flipped.

The rig (skin_join_test.cpp) holds the CPU half of the join (src\\d3d11\\skin_join.h: the snapshot checks J1, the continuity map J2, the palette
history's certificates J3, the CPU reference of JoinCS J4-J9 -- hook and prefix sources, the disagreement gate, the pose and layout checks, the
residuals -- and the periodic line J10) and the hook's reading of the game's list (src\\d3d11\\skin_entity_walk.h, J11). Every check carries a label
"J<case>.<what>". A rig that passes proves little until it is seen to FAIL on a source that breaks the rule it pins: for each mutation below the
machinery (tools\\rig_mutants_lib.py) copies the header into a temp directory OUTSIDE the repo, applies the edit, rebuilds the rig against the copy and
requires a FAIL on a check of the case that belongs to the rule.

  python tools\\skin_join_test\\mutants.py --self-test        text only: every anchor is found exactly once, every case named is in the rig, and
                                                              build.bat compiles the rig the way the machinery does and runs this self-test
  python tools\\skin_join_test\\mutants.py --run [--only a,b] [--jobs N] [--keep] [--verbose]
  python tools\\skin_join_test\\mutants.py --list
  python tools\\skin_join_test\\mutants.py --run --dry-run    the plan; writes nothing, starts nothing
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import rig_mutants_lib as lib  # noqa: E402

ROOT = HERE.parents[1]
FILES = {
    "join": ROOT / "src" / "d3d11" / "skin_join.h",
    "walk": ROOT / "src" / "d3d11" / "skin_entity_walk.h",
}
CONFIG = lib.Config(
    here=__file__, files=FILES, header_keys=("join", "walk"), pin_keys=(), rig=HERE / "skin_join_test.cpp",
    rig_label=":rig_skin_join_test", rig_source_in_bat="tools\\skin_join_test\\skin_join_test.cpp", rig_exe_in_bat="skin_join_test.exe",
    case_prefix="J", include_dirs=("src/d3d11",), min_mutants=40, run_timeout=120.0)

M = lib.M
MUTANTS = [
    # ---- J1: the snapshot checks ----
    M("snapshot-range-unchecked", "J1", "join", [('else if (e.dst > next || e.count > next - e.dst) { w = "range"; ok = false; }', 'else if (false) { w = "range"; ok = false; }')],
      "an entity whose primary job overruns its range is accepted"),
    M("snapshot-range-sum-wraps", "J1", "join", [('else if (e.dst > next || e.count > next - e.dst) { w = "range"; ok = false; }', 'else if (e.dst + e.count > next) { w = "range"; ok = false; }')],
      "the range test is the 32-bit sum again: a base near 2^32 wraps it back under the next base and the entry is accepted"),
    M("snapshot-range-off-by-one", "J1", "join", [('else if (e.dst > next || e.count > next - e.dst) { w = "range"; ok = false; }', 'else if (e.dst > next || e.count >= next - e.dst) { w = "range"; ok = false; }')],
      "a primary job that exactly fills its range is refused"),
    M("snapshot-implausible-accepted", "J1", "join", [("if (s.flags & (kSnapFault | kSnapOverflow | kSnapImplausible | kSnapNodeChanged))", "if (s.flags & (kSnapFault | kSnapOverflow | kSnapNodeChanged))")],
      "a list the walk flagged as holding a value it could not believe is accepted, though the flag's own comment says checkSnapshot refuses it"),
    M("snapshot-zero-bones-accepted", "J1", "join", [("if (!e.key || !e.mesh || e.count == 0 || e.count > kMaxBonesPerJob)", "if (!e.key || !e.mesh || e.count > kMaxBonesPerJob)")],
      "an entry with no bones is accepted"),
    M("snapshot-null-key-accepted", "J1", "join", [("if (!e.key || !e.mesh ||", "if (!e.mesh ||")], "an entry with no address is accepted"),
    M("snapshot-end-beyond-tables", "J1", "join", [('else if (s.end == 0 || s.end > kMaxRows) { w = "end row"; ok = false; }', 'else if (s.end == 0) { w = "end row"; ok = false; }')],
      "an end row beyond the tables is accepted"),
    M("snapshot-node-change-accepted", "J1", "join", [("if (s.flags & (kSnapFault | kSnapOverflow | kSnapImplausible | kSnapNodeChanged))", "if (s.flags & (kSnapFault | kSnapOverflow | kSnapImplausible))")],
      "a list from another node is accepted"),
    M("snapshot-empty-accepted", "J1", "join", [("else if (s.n == 0 || s.n > kMaxEntries) { w = \"entry count\"; ok = false; }", "else if (s.n > kMaxEntries) { w = \"entry count\"; ok = false; }")],
      "an empty list is accepted"),
    # ---- J2: continuity from one list to the next ----
    M("gap-not-declined", "J2", "join", [("else if (latest->seq != lastSeq_ + 1) decline = kDeclineGap;", "else if (false) decline = kDeclineGap;")],
      "a skipped call is offered as consecutive"),
    M("stale-not-declined", "J2", "join", [("else if (latest->seq == lastSeq_ && haveLast_) decline = kDeclineStale;", "else if (false) decline = kDeclineStale;")],
      "the same call twice is offered again"),
    M("node-change-not-declined", "J2", "join", [("else if (latest->node != last_.node) decline = kDeclineNodeChanged;", "else if (false) decline = kDeclineNodeChanged;")],
      "a list from another node is continuous with the last"),
    M("vtable-ignored", "J2", "join", [("if (q.vtable == e.vtable && q.mesh == e.mesh && q.count == e.count) plan.prevIdx[i] = p->second;", "if (q.mesh == e.mesh && q.count == e.count) plan.prevIdx[i] = p->second;")],
      "a key with another vtable is the same entity"),
    M("mesh-ignored", "J2", "join", [("if (q.vtable == e.vtable && q.mesh == e.mesh && q.count == e.count) plan.prevIdx[i] = p->second;", "if (q.vtable == e.vtable && q.count == e.count) plan.prevIdx[i] = p->second;")],
      "a key with another mesh is the same entity"),
    M("bone-count-ignored", "J2", "join", [("if (q.vtable == e.vtable && q.mesh == e.mesh && q.count == e.count) plan.prevIdx[i] = p->second;", "if (q.vtable == e.vtable && q.mesh == e.mesh) plan.prevIdx[i] = p->second;")],
      "an entity whose primary bone count changed is the same entity"),
    M("key-listed-twice-joins", "J2", "join", [("if (twice || p == previous.end() || duplicated[p->second]) continue;", "if (p == previous.end() || duplicated[p->second]) continue;")],
      "a key listed twice in this list joins"),
    M("previous-twice-joins", "J2", "join", [("if (twice || p == previous.end() || duplicated[p->second]) continue;", "if (twice || p == previous.end()) continue;")],
      "a key listed twice in the previous list joins"),
    M("previous-end-not-carried", "J2", "join", [("plan.prevRs[last_.n] = last_.end;", ";")], "the previous list's end row is not in the plan"),
    M("end-not-carried", "J2", "join", [("plan.rs[latest->n] = latest->end;", ";")], "this list's end row is not in the plan"),
    # ---- J3: the history certificates ----
    M("gap-in-frames-allowed", "J3", "join", [("else if (frame != lastFrame_ + 1) v = kHistoryGap;", "else if (false) v = kHistoryGap;")],
      "a missed frame still has history"),
    M("same-buffer-allowed", "J3", "join", [("else if (bufferId == lastBuffer_) v = kHistorySame;", "else if (false) v = kHistorySame;")],
      "the game did not swap its buffers and the previous palette is trusted"),
    M("shrunk-allowed", "J3", "join", [("else if (rowsInUse && lastBytes_ / 48 < rowsInUse) v = kHistoryShrunk;", "else if (false) v = kHistoryShrunk;")],
      "the previous buffer cannot hold the rows in use and is trusted"),
    M("size-of-new-buffer", "J3", "join", [("else if (rowsInUse && lastBytes_ / 48 < rowsInUse) v = kHistoryShrunk;", "else if (rowsInUse && byteWidth / 48 < rowsInUse) v = kHistoryShrunk;")],
      "the CURRENT buffer's size decides, not the previous one's"),
    M("no-pose-allowed", "J3", "join", [("else if (!poseBuiltLastFrame) v = kHistoryPose;", "else if (false) v = kHistoryPose;")],
      "a frame whose previous pose table was never built has history"),
    # ---- J4, J5, J6, J7, J8: JoinCS's CPU reference ----
    M("previous-verification-ignored", "J4", "join", [("const bool useHook = history && offered && hookOk && prevHookOk;", "const bool useHook = history && offered && hookOk;")],
      "the hook is trusted on its first agreeing frame"),
    M("disagreement-ignored", "J7", "join", [("const bool useHook = history && offered && hookOk && prevHookOk;", "const bool useHook = history && offered && prevHookOk;")],
      "the hook is used on a frame where the dispatch's own table disagreed"),
    M("verification-not-remembered", "J7", "join", [("r.prevHookOk = offered && hookOk ? 1 : 0;", "r.prevHookOk = offered ? 1 : 0;")],
      "the frame after a disagreement trusts the hook"),
    M("prefix-off-by-one", "J5", "join", [("{ prefix = k; break; }", "{ prefix = k + 1; break; }")], "the first differing job still joins"),
    M("prefix-ignores-bind", "J5", "join", [("if (jobs[k].bind != prevJobs[k].bind || jobs[k].count != prevJobs[k].count)", "if (jobs[k].count != prevJobs[k].count)")],
      "the prefix compares only the bone counts"),
    M("range-change-ignored", "J6", "join", [("if (plan.rs[i + 1] - plan.rs[i] != plan.prevRs[ip + 1] - plan.prevRs[ip]) { ++s[kStatFailRange]; continue; }", "if (false) { ++s[kStatFailRange]; continue; }")],
      "an entity whose range changed length keeps its history"),
    M("layout-bind-ignored", "J6", "join", [("prevDstInfo[prevDst].count != jb.count || prevDstInfo[prevDst].bind != jb.bind", "prevDstInfo[prevDst].count != jb.count")],
      "a child replaced by another bind keeps its history"),
    M("layout-count-ignored", "J6", "join", [("prevDstInfo[prevDst].count != jb.count || prevDstInfo[prevDst].bind != jb.bind", "prevDstInfo[prevDst].bind != jb.bind")],
      "a child with another bone count keeps its history"),
    M("child-offset-dropped", "J5", "join", [("prevDst = plan.prevRs[ip] + (jb.dst - plan.rs[i]);", "prevDst = plan.prevRs[ip];")],
      "every job of an entity joins to the entity's first row"),
    M("pose-record-unchecked", "J8", "join", [("if (prevDst == 0 || prevDst >= kMaxRows || prevPoseW0[prevDst] != prevDst) { ++s[kStatFailPose]; continue; }", "if (prevDst == 0 || prevDst >= kMaxRows) { ++s[kStatFailPose]; continue; }")],
      "a base with no previous pose record is joined"),
    M("duplicate-base-overwrites", "J8", "join", [("if (r.dstInfo[jb.dst].count != 0) { ++s[kStatDupBase]; continue; }", "if (r.dstInfo[jb.dst].count != 0) { ++s[kStatDupBase]; }")],
      "the second job on a base replaces the first"),
    M("zero-bone-job-valid", "J8", "join", [("if (jb.count == 0 || jb.dst >= kMaxRows || jb.count > kMaxRows - jb.dst) { ++s[kStatFailCap]; continue; }", "if (jb.dst >= kMaxRows || jb.count > kMaxRows - jb.dst) { ++s[kStatFailCap]; continue; }")],
      "a job with no bones is counted as valid"),
    M("previous-rows-unguarded", "J8", "join", [("if (prevDst < kMaxRows && uint64_t(prevDst) + jb.count > plan.prevRows) { ++s[kStatFailPrevRows]; continue; }", ";")],
      "a job whose previous rows run past the previous palette buffer keeps its history"),
    M("unknown-rows-called-shrunk", "J3", "join", [("else if (rowsInUse && lastBytes_ / 48 < rowsInUse) v = kHistoryShrunk;", "else if (lastBytes_ / 48 < (rowsInUse ? rowsInUse : kMaxRows)) v = kHistoryShrunk;")],
      "with no list the previous buffer is compared with nothing and found small"),
    M("prev-rows-word-misplaced", "J2", "join", [("out[5] = jobs; out[6] = prevJobs; out[7] = prevRows;", "out[5] = jobs; out[6] = prevJobs; out[7] = parity;")],
      "the plan carries the parity where JoinCS reads the previous buffer's rows"),
    M("no-history-still-joins", "J4", "join", [("if (!history) { s[kStatNoHistory] = 1; return r; }", "if (!history) { s[kStatNoHistory] = 1; }")],
      "without certified history jobs are joined anyway"),
    M("job-outside-ranges-ignored", "J7", "join", [("{ mismatch |= kMmNotInRange; continue; }", "{ continue; }")],
      "a job outside every entity's range is not a disagreement"),
    M("head-count-ignored", "J7", "join", [("if (jb.count != plan.count[i]) mismatch |= kMmHeadCount;", ";")],
      "a primary job whose count differs from its entry's is not a disagreement"),
    M("head-number-ignored", "J7", "join", [("if (heads != plan.m) mismatch |= kMmHeads;", ";")],
      "an entry with no job at its base is not a disagreement"),
    M("jobs-sum-ignored", "J7", "join", [("if (sum != uint64_t(plan.end) - plan.rs[0]) mismatch |= kMmSum;", ";")],
      "jobs that do not add up to the list's end are not a disagreement"),
    M("entity-tiling-ignored", "J7", "join", [("if (entitySum[i] != uint64_t(plan.rs[i + 1]) - plan.rs[i]) { mismatch |= kMmEntitySum; break; }", "if (false) { mismatch |= kMmEntitySum; break; }")],
      "an entity whose jobs do not tile its range is not a disagreement"),
    M("empty-plan-accepted", "J7", "join", [("if (plan.m == 0 || plan.m > kMaxEntries) mismatch |= kMmNoPlan;", ";")], "a plan with no entities is a plan"),
    # ---- J9: the residuals stay as documented (the rig pins them) ----
    M("hook-falls-back-to-position", "J9", "join",
      [("if (twice || p == previous.end() || duplicated[p->second]) continue;",
        "if (twice || p == previous.end() || duplicated[p->second]) { if (i < last_.n && last_.e[i].count == e.count) plan.prevIdx[i] = i; continue; }")],
      "an entity with no key match is joined by its position in the list (the prefix join by another name)"),
    # ---- J10: the periodic line ----
    M("line-hook-always-named", "J10", "join", [('hookArmed ? hookState : "off"', "hookState")], "a hook that never armed is not said to be off"),
    M("line-source-always-prefix", "J10", "join", [('d[kStatPrefixUsed] ? "prefix" : "none"', '"prefix"')], "a window with no frames still names a source"),
    # ---- J12: which record of a base is the live one ----
    M("pose-reference-test-ignored", "J12", "join", [("if (valid && referenced(i)) { r.table[base] = pool[i]; state[base] |= 1; }", "if (false) { r.table[base] = pool[i]; state[base] |= 1; }")],
      "no record is ever live: every record decides and the stale second set kills its bases"),
    M("pose-first-writer-is-live", "J12", "join", [("if (valid && referenced(i)) { r.table[base] = pool[i]; state[base] |= 1; }", "if (valid && !(state[base] & 1)) { r.table[base] = pool[i]; state[base] |= 1; }")],
      "the first record of a base is its live one, whether a draw read it or not"),
    M("pose-last-writer-is-live", "J12", "join", [("if (valid && referenced(i)) { r.table[base] = pool[i]; state[base] |= 1; }", "if (valid) { r.table[base] = pool[i]; state[base] |= 1; }")],
      "the last record of a base is its live one, whether a draw read it or not"),
    M("pose-both-read-resolved", "J12", "join", [("const bool decisive = !hasLive || referenced(i);", "const bool decisive = !hasLive;")],
      "two records that draws both read and that disagree are not a conflict"),
    M("pose-none-read-resolved", "J12", "join", [("const bool decisive = !hasLive || referenced(i);", "const bool decisive = hasLive && referenced(i);")],
      "two records that disagree and that no draw read are not a conflict"),
    M("pose-unreferenced-decides", "J12", "join", [("const bool decisive = !hasLive || referenced(i);", "const bool decisive = true;")],
      "a record no draw read still decides a base that has a live record"),
    M("pose-incomplete-list-trusted", "J12", "join", [("const bool valid = refs.complete && !bad;", "const bool valid = !bad;")],
      "a list the CPU did not call complete is used"),
    M("pose-outside-entry-ignored", "J12", "join", [("if (entry < refs.first || entry - refs.first >= entries) { bad = true; continue; }", "if (entry < refs.first || entry - refs.first >= entries) { continue; }")],
      "a draw naming an entry outside the copied span is taken for a draw of nothing"),
    M("pose-record-outside-pool-ignored", "J12", "join", [("if (rec >= records || rec >= kRefWords * 32u) { bad = true; continue; }", "if (rec >= records || rec >= kRefWords * 32u) { continue; }")],
      "an entry naming a record the pool does not hold is dropped quietly"),
    M("pose-range-limit-ignored", "J12", "join", [("if (count > kMaxRangeInstances) { bad = true; continue; }", "if (count > kMaxRangeInstances) { continue; }")],
      "a draw naming more instances than the limit is dropped quietly"),
    M("pose-dropped-base-keeps-pose", "J12", "join", [("r.table[b] = PoseWords{};", "r.table[b].w[0] = 0;")],
      "a dropped base keeps the rest of its pose in the table"),
    M("pose-resolved-uncounted", "J12", "join", [("        else ++r.resolved;", "        else { }")], "an overruled record is not counted"),
    M("pose-base-zero-written", "J12", "join", [("        if (base == 0 || base >= kMaxRows) continue;\n        ++r.records;", "        if (base >= kMaxRows) continue;\n        ++r.records;")],
      "a record with base 0 is counted and written"),
    M("pose-line-counts-swapped", "J12", "join", [("d[kStatPoseIdle], d[kStatPoseUnresolved], unlisted", "d[kStatPoseUnresolved], d[kStatPoseIdle], unlisted")],
      "the witness line prints the idle and the unresolved bases the wrong way round"),
    M("pose-idle-counted-unresolved", "J12", "join", [("if (valid) { if (state[b] & 1) ++r.unresolved; else ++r.idle; }", "if (valid) { ++r.unresolved; }")],
      "a base no draw read is called unresolved: the witness cannot tell a culled character from a conflict between records a draw read"),
    M("pose-unlisted-counted-idle", "J12", "join", [("if (valid) { if (state[b] & 1) ++r.unresolved; else ++r.idle; }", "{ if (state[b] & 1) ++r.unresolved; else ++r.idle; }")],
      "with no exact draw list a dropped base is called idle: nobody's reading decided it"),
    M("pose-line-hides-unlisted", "J12", "join", [("const uint32_t unlisted = d[kStatPoseDropped] > listed ? d[kStatPoseDropped] - listed : 0u;", "const uint32_t unlisted = 0u;")],
      "the witness line does not say how many bases were dropped without an exact draw list"),
    # ---- J13: the chain's dispatches over the joins ----
    M("chain-line-late-and-multi-swapped", "J13", "join", [("(unsigned long long)c.chainMulti, (unsigned long long)c.chainLate", "(unsigned long long)c.chainLate, (unsigned long long)c.chainMulti")],
      "the chain line prints the frames with two dispatches where the late dispatches belong"),
    M("chain-line-frames-from-jobs", "J13", "join", [("c.chainDispatches, d[kStatFrames],", "c.chainDispatches, d[kStatJobs],")],
      "the chain line divides the dispatches by the jobs, not by the joins"),
    # ---- J11: the hook's reading of the game's list ----
    M("walk-list-offset", "J11", "walk", [("constexpr uint32_t kOffNodeList = 0xA8, kOffNodeEnd = 0xC4;", "constexpr uint32_t kOffNodeList = 0xA0, kOffNodeEnd = 0xC4;")],
      "the list head is read from the wrong field"),
    M("walk-end-offset", "J11", "walk", [("constexpr uint32_t kOffNodeList = 0xA8, kOffNodeEnd = 0xC4;", "constexpr uint32_t kOffNodeList = 0xA8, kOffNodeEnd = 0xC0;")],
      "the end row is read from the wrong field"),
    M("walk-mesh-offset", "J11", "walk", [("kOffEntryMesh = 0x38,", "kOffEntryMesh = 0x30,")], "the mesh data pointer is read from the wrong field"),
    M("walk-base-offset", "J11", "walk", [("kOffEntryDst = 0xA8;", "kOffEntryDst = 0xAC;")], "the assigned base is read from the wrong field"),
    M("walk-alignment-unchecked", "J11", "walk", [("&& (p & 7u) == 0; }", "; }")], "an unaligned pointer is believed"),
    M("walk-mesh-unchecked", "J11", "walk", [("if (!userPointer(mesh)) { s.flags |= kSnapImplausible; break; }", "")], "a mesh pointer that cannot be an object is read through"),
    M("walk-cycle-unflagged", "J11", "walk", [("if (s.n >= kMaxEntries) { s.flags |= kSnapOverflow; break; }", "if (s.n >= kMaxEntries) { break; }")],
      "a list that points back into itself ends without saying so"),
    M("walk-mesh-fault-unflagged", "J11", "walk", [("if (!read(mesh, &count, 2)) { s.flags |= kSnapFault; break; }", "if (!read(mesh, &count, 2)) { break; }")],
      "a fault reading the bone count is not reported"),
    M("walk-node-unchecked", "J11", "walk", [("if (!userPointer(node) || !read(node + kOffNodeList, &entry, 8)) {", "if (!read(node + kOffNodeList, &entry, 8)) {")],
      "a node address that cannot be a user object is read"),
]

if __name__ == "__main__":
    sys.exit(lib.main(CONFIG, MUTANTS))

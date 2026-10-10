#!/usr/bin/env python3
"""The mutation proof for tools\\skin_entity_hook_test: the rig fails when a rule of the read-only hook on the skinning-job assembly is broken.

The rig (skin_entity_hook_test.cpp) compiles the production hook (src\\d3d11\\skin_entity_hook.cpp, with EDVR_SKIN_HOOK_TEST) with the real CodeHook and
runs its self-test on a synthetic game function that begins with the real 28-byte prologue: nothing exists before arming (H1), a wrong prologue stands
the hook down (H2), the right one arms it and says READ ONLY (H3), the original runs first and the list is read whole (H4), the first call is reported
(H5), a shut gate observes nothing (H6), an unmapped pointer is a fault (H7), a wild pointer is implausible (H8), a cycle ends at the capacity (H9),
recovery and the sequence (H10), another thread is reported (H11), a reader never gets a torn copy (H12), a second node stands the hook down (H13).
Every failure carries a label "H<case>.<what>". A rig that passes proves little until it is seen to FAIL on a source that breaks the rule it pins:
for each mutation below the machinery (tools\\rig_mutants_lib.py) copies the sources into a temp directory OUTSIDE the repo, applies the edit,
rebuilds the rig against the copy and requires a FAIL on a check of the case that belongs to the rule.

  python tools\\skin_entity_hook_test\\mutants.py --self-test    text only: every anchor is found exactly once, every case named is in the rig,
                                                                 and build.bat compiles the rig the way the machinery does and runs this self-test
  python tools\\skin_entity_hook_test\\mutants.py --run [--only a,b] [--jobs N] [--keep] [--verbose]
  python tools\\skin_entity_hook_test\\mutants.py --list
  python tools\\skin_entity_hook_test\\mutants.py --run --dry-run   the plan; writes nothing, starts nothing
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import rig_mutants_lib as lib  # noqa: E402

ROOT = HERE.parents[1]
FILES = {
    "hook": ROOT / "src" / "d3d11" / "skin_entity_hook.cpp",
    "walk": ROOT / "src" / "d3d11" / "skin_entity_walk.h",
}
# everything else the rig compiles or includes, copied into the temp tree unmutated
COMMON = ["code_hook.h", "code_hook.cpp", "guard.h", "guard.cpp", "log.h", "log.cpp", "config.h", "config.cpp", "ini_name.h", "runtime_profile.h", "proxy.h"]
TREE_EXTRA = [("src/common/" + name, ROOT / "src" / "common" / name) for name in COMMON] + [
    ("src/d3d11/explorer_cam_core.h", ROOT / "src" / "d3d11" / "explorer_cam_core.h"),
    ("src/d3d11/skin_entity_hook.h", ROOT / "src" / "d3d11" / "skin_entity_hook.h"),
    ("src/d3d11/skin_join.h", ROOT / "src" / "d3d11" / "skin_join.h"),
]
UNITS = [("src/d3d11/skin_entity_hook.cpp", "hook")] + [("src/common/" + n, None) for n in ("code_hook.cpp", "guard.cpp", "log.cpp", "config.cpp")]
CL_FLAGS = lib.DEFAULT_CL_FLAGS + ["/DUNICODE", "/D_UNICODE", "/DEDVR_SKIN_HOOK_TEST"]
CONFIG = lib.Config(
    here=__file__, files=FILES, header_keys=("hook", "walk"), pin_keys=(), rig=HERE / "skin_entity_hook_test.cpp",
    rig_label=":rig_skin_entity_hook_test", rig_source_in_bat="tools\\skin_entity_hook_test\\skin_entity_hook_test.cpp", rig_exe_in_bat="skin_entity_hook_test.exe",
    case_prefix="H", include_dirs=("src/d3d11",), cl_flags=CL_FLAGS, units=UNITS, link_args=["kernel32.lib", "user32.lib"], min_mutants=14,
    run_timeout=120.0, tree_extra=TREE_EXTRA)

M = lib.M
MUTANTS = [
    M("state-before-arming", "H1", "hook", [("std::atomic<State*> g_state{nullptr};", "std::atomic<State*> g_state{new State};")],
      "a state exists before anything armed the hook"),
    M("prologue-unchecked", "H2", "hook", [("if (!checkBytes(target, kPrologue, kPrologueBytes)) {", "if (false) {")],
      "a function whose prologue is not build 332841's is patched"),
    M("not-read-only-in-the-line", "H3", "hook", [('"READ ONLY: the original runs first', '"the original runs first')], "the arm line does not say the hook only reads"),
    M("observe-before-original", "H4", "hook",
      [("    forward(node, b, c, d);\n    if (s->gate.load(std::memory_order_relaxed)) observe(*s, node);", "    if (s->gate.load(std::memory_order_relaxed)) observe(*s, node);\n    forward(node, b, c, d);")],
      "the list is read before the game's function has assigned the bases"),
    M("sequence-skips", ("H4", "H10"), "hook", [("const uint64_t seq = s.calls.fetch_add(1, std::memory_order_relaxed) + 1;", "const uint64_t seq = s.calls.fetch_add(1, std::memory_order_relaxed) + 2;")],
      "the sequence numbers do not count the calls"),
    M("first-call-unreported", "H5", "hook", [('known == 0 ? "first call" : "call from another thread"', 'known == 0 ? "call" : "call from another thread"')],
      "the first call is not named as such"),
    M("gate-never-shut", "H6", "hook", [("s->gate.store(open ? 1 : 0, std::memory_order_seq_cst);", "s->gate.store(1, std::memory_order_seq_cst);")],
      "closing the gate leaves it open: the relay keeps calling the observer"),
    M("fault-swallowed", "H7", "hook",
      [("std::memcpy(out, reinterpret_cast<const void*>(address), bytes);\n        return true;\n    } __except (EXCEPTION_EXECUTE_HANDLER) {\n        return false;",
        "std::memcpy(out, reinterpret_cast<const void*>(address), bytes);\n        return true;\n    } __except (EXCEPTION_EXECUTE_HANDLER) {\n        return true;")],
      "a read that faults is taken for a read of zeros"),
    M("wild-pointer-followed", "H8", "walk", [("if (!userPointer(entry)) { s.flags |= kSnapImplausible; break; }", "if (!userPointer(entry)) { break; }")],
      "a next pointer that cannot be an object ends the walk without saying so"),
    M("cycle-runs-on", "H9", "walk", [("if (s.n >= kMaxEntries) { s.flags |= kSnapOverflow; break; }", "if (s.n >= kMaxEntries) { break; }")],
      "a list that points back into itself ends without saying so"),
    M("thread-never-new", "H11", "hook", [("seen = seen || s.tids[i].load(std::memory_order_relaxed) == tid;", "seen = true;")],
      "a call from another thread is not noticed"),
    M("overtaken-copy-handed-out", "H12", "hook",
      [("if (s->slotSeq[slot].load(std::memory_order_acquire) == seq && out.seq == seq) return true;", "return true;")],
      "a copy the writer overtook is handed out"),
    M("second-node-tolerated", "H13", "hook", [("    if (second) {\n        char line[300];", "    if (false) {\n        char line[300];")],
      "a second processor node does not stand the hook down"),
    M("stand-down-not-recorded", "H13", "hook", [("if (s.state.compare_exchange_strong(expected, int(SkinHookState::StoodDown))) {", "if (false) {")],
      "the state still says armed after the hook stood down"),
    M("empty-lists-judged", "H14", "hook", [("const bool empty = !good && emptyList(slot);", "const bool empty = false;")],
      "a list of no entries (a menu, a loading screen) is judged like any other: 120 of them stand the hook down before a character exists"),
    M("empty-end-row-ignored", "H15", "hook", [("bool emptyList(const Snapshot& s) noexcept { return s.n == 0 && s.flags == 0 && s.end <= 1; }", "bool emptyList(const Snapshot& s) noexcept { return s.n == 0 && s.flags == 0; }")],
      "a list of no entries but an end row past 1 is taken for a clean empty one and never judged"),
    M("empty-flags-ignored", "H15", "hook", [("bool emptyList(const Snapshot& s) noexcept { return s.n == 0 && s.flags == 0 && s.end <= 1; }", "bool emptyList(const Snapshot& s) noexcept { return s.n == 0 && s.end <= 1; }")],
      "a list the walk could not read (a fault, no entries) is taken for a clean empty one and never judged"),
    M("bound-removed", "H15", "hook", [("} else if (judged >= kStandDownAfter && s.usable.load(std::memory_order_relaxed) == 0) {", "} else if (false) {")],
      "120 unusable lists with something to read no longer stand the hook down"),
    M("idle-dispatch-judges", "H14", "hook", [("if (!jobs || s.state.load(std::memory_order_relaxed) != int(SkinHookState::Armed)) return;", "if (s.state.load(std::memory_order_relaxed) != int(SkinHookState::Armed)) return;")],
      "a chain dispatch with no jobs judges the empty list it finds"),
    M("jobs-never-judge-empty", "H16", "hook", [("const uint64_t n = s.emptyWithJobs.fetch_add(1, std::memory_order_relaxed) + 1;", "const uint64_t n = 0;")],
      "a dispatch with jobs that meets an empty list counts nothing: a hook with wrong offsets is never stood down"),
    M("usable-does-not-end-judging", "H17", "hook", [("if (n >= kStandDownAfter && s.usable.load(std::memory_order_relaxed) == 0) {", "if (n >= kStandDownAfter) {")],
      "the empty-with-jobs rule stands the hook down although a usable list was read"),
    M("list-offset-wrong", "H4", "walk", [("constexpr uint32_t kOffNodeList = 0xA8, kOffNodeEnd = 0xC4;", "constexpr uint32_t kOffNodeList = 0xA0, kOffNodeEnd = 0xC4;")],
      "the list head is read from the wrong field"),
    M("end-offset-wrong", "H4", "walk", [("constexpr uint32_t kOffNodeList = 0xA8, kOffNodeEnd = 0xC4;", "constexpr uint32_t kOffNodeList = 0xA8, kOffNodeEnd = 0xC0;")],
      "the end row is read from the wrong field"),
]

if __name__ == "__main__":
    sys.exit(lib.main(CONFIG, MUTANTS))

#!/usr/bin/env python3
"""The mutation proof for tools\\menu_edit_hold_test: the rig fails when a rule of the held numeric edits is broken.

The rig (menu_edit_hold_test.cpp) drives the coalescer of src\\d3d11\\menu_edit_hold.h the way the menu tick does -- a frame clock, the tracker's edge-and-repeat, the key
state of the last poll -- and holds it to: a tap writes once (H1), a long hold writes about every 250 ms (about 20 times in five seconds) and none of its steps waits more than that (H2), a key held with
no step coming (a row at its limit) is written by the bound, once (H3), a row or page switch writes first (H4, H5), a close and a shutdown write what is held
(H6), any other change goes behind it (H7), nothing is written twice (H8), a failed write puts the row back (H9), a burst is one change in the log (H10), the glue in
menu.cpp is wired (H11, read as text), five seconds of hold are 12 to 22 writes (H12), and the config refresh window line says what it counted (H13). Every failure
carries a label "H<case>.<what>". A rig that passes proves little until it is seen to FAIL on a source that breaks the rule it pins: for each mutation below the machinery
(tools\\rig_mutants_lib.py) copies the header (or the glue, which the rig reads as text) into a temp directory OUTSIDE the repo, applies the edit, rebuilds or reruns the rig
against the copy and requires a FAIL on a check of the case that belongs to the rule.

  python tools\\menu_edit_hold_test\\mutants.py --self-test        text only: every anchor is found exactly once, every case named is in the rig, and
                                                                   build.bat compiles the rig the way the machinery does and runs this self-test
  python tools\\menu_edit_hold_test\\mutants.py --run [--only a,b] [--jobs N] [--keep] [--verbose]
  python tools\\menu_edit_hold_test\\mutants.py --list
  python tools\\menu_edit_hold_test\\mutants.py --run --dry-run    the plan; writes nothing, starts nothing
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import rig_mutants_lib as lib  # noqa: E402

ROOT = HERE.parents[1]
FILES = {
    "hold": ROOT / "src" / "d3d11" / "menu_edit_hold.h",
    "line": ROOT / "src" / "d3d11" / "config_refresh_line.h",
    "menu": ROOT / "src" / "d3d11" / "menu.cpp",           # read by the rig as text (H11)
    "perf": ROOT / "src" / "d3d11" / "perf_monitor.cpp",   # ...and this one (H13.p)
}
TREE_EXTRA = [("src/d3d11/menu_keys.h", ROOT / "src" / "d3d11" / "menu_keys.h")]
CONFIG = lib.Config(
    here=__file__, files=FILES, header_keys=("hold", "line"), pin_keys=("menu", "perf"), rig=HERE / "menu_edit_hold_test.cpp",
    rig_label=":rig_menu_edit_hold_test", rig_source_in_bat="tools\\menu_edit_hold_test\\menu_edit_hold_test.cpp", rig_exe_in_bat="menu_edit_hold_test.exe",
    case_prefix="H", include_dirs=("src/d3d11",), min_mutants=30, run_timeout=120.0, tree_extra=TREE_EXTRA)

BOUND = "else if (now - m_firstMs >= kEditMaxWaitMs) why = EditFlush::MaxWait;"
SENTENCE = 'reloads == 0 && edits == 0 ? " No refresh and no menu edit in this window." : ""'
FORMAT_CALL = "formatConfigRefreshWindow(refreshLine, sizeof(refreshLine), s.frameNo, reloads, reloadMs, reloadMaxMs, edits);"

M = lib.M
MUTANTS = [
    # ---- the coalescer ----
    M("tap-waits-for-the-bound", "H1", "hold", [("else if (!held) why = EditFlush::Release;", "else if (false) why = EditFlush::Release;")],
      "a key coming up is not a reason to write: a tap waits out the bound, and a hold's last steps with it"),
    M("every-step-written", ("H2", "H12"), "hold", [("            m_merge(m_job, job);\n        }\n    }", "            m_merge(m_job, job);\n        }\n        flush(EditFlush::Other);\n    }")],
      "no coalescing: every step is written, about twelve a second, as before"),
    M("no-bound-on-a-hold", ("H2", "H3", "H12"), "hold", [(BOUND, "else if (false) why = EditFlush::MaxWait;")],
      "a long hold writes nothing until it ends, and a key held at a row's limit never: the live effect of the setting does not show while the key is down"),
    M("bound-restarted-by-every-step", ("H2", "H12"), "hold", [("            m_merge(m_job, job);\n", "            m_merge(m_job, job);\n            m_firstMs = now;\n")],
      "the bound is measured from the last step, which a hold renews every 83 ms: it never comes"),
    M("bound-back-to-two-seconds", ("H2", "H12"), "hold", [("constexpr uint64_t kEditMaxWaitMs = 250;", "constexpr uint64_t kEditMaxWaitMs = 2000;")],
      "the first choice, 2000 ms: a hold writes three times in five seconds and its live effect lags two seconds"),
    M("bound-doubled", ("H2", "H12"), "hold", [("constexpr uint64_t kEditMaxWaitMs = 250;", "constexpr uint64_t kEditMaxWaitMs = 500;")],
      "a hold writes twice a second, not four times: the preview moves in half-second steps"),    M("row-switch-by-highlight-ignored", "H4", "hold", [("if (m_def != highlightDef) why = EditFlush::RowSwitch;", "if (false) why = EditFlush::RowSwitch;")],
      "moving the highlight to another row with the key down leaves the first row's edit held"),
    M("row-switch-by-step-ignored", "H4", "hold", [("        if (m_pending && m_def != def) flush(EditFlush::RowSwitch);\n", "")],
      "a step of another row is folded into the first row's edit: the wrong row is written"),
    M("page-switch-ignored", "H5", "hold", [("else if (m_page != page) why = EditFlush::PageSwitch;", "else if (false) why = EditFlush::PageSwitch;")],
      "another page shown leaves the edit held"),
    M("flush-keeps-the-edit", ("H8", "H9"), "hold", [("        m_pending = false;   // before the sink: a sink that comes back here finds nothing to write twice\n", "")],
      "a written edit is still held: it is written again on the next tick"),
    M("close-writes-nothing", "H6", "hold", [("if (!m_pending) return false;\n        const Job job", "if (!m_pending || why == EditFlush::Close) return false;\n        const Job job")],
      "closing the menu with a held edit loses it"),
    M("shutdown-writes-nothing", "H6", "hold", [("if (!m_pending) return false;\n        const Job job", "if (!m_pending || why == EditFlush::Shutdown) return false;\n        const Job job")],
      "a shutdown with a held edit loses it"),
    M("other-change-passes-the-held-one", "H7", "hold", [("if (!m_pending) return false;\n        const Job job", "if (!m_pending || why == EditFlush::Other) return false;\n        const Job job")],
      "another change is queued ahead of the held edit: the file ends on the older value"),
    M("merge-bypassed", "H10", "hold", [("            m_merge(m_job, job);\n", "            m_job = job;\n")],
      "each step replaces the held job whole: the log says N-1 -> N for a burst of many"),
    # ---- the config refresh window line ----
    M("sentence-unconditional", "H13", "line", [(SENTENCE, '" No refresh and no menu edit in this window."')],
      "the closing sentence is printed after counts that are not zero (F16's line)"),
    M("sentence-never", "H13", "line", [(SENTENCE, '""')],
      "an empty window is not told from a build without the line"),
    M("sentence-when-either-is-zero", "H13", "line", [("reloads == 0 && edits == 0 ?", "reloads == 0 || edits == 0 ?")],
      "a window with menu edits and no refresh yet, or a refresh and no edit, is called empty"),
    M("mean-over-edits", "H13", "line", [("reloads ? reloadMs / reloads : 0.0", "edits ? reloadMs / edits : 0.0")],
      "the mean is the total over the edits, not over the refreshes"),
    # ---- the glue in menu.cpp and perf_monitor.cpp (read as text) ----
    M("number-step-not-held", "H11", "menu", [("applyChange(defIndex, formatNumber(v, d.precision), true);", "applyChange(defIndex, formatNumber(v, d.precision));")],
      "a stepped Number row writes on every step again"),
    M("width-step-not-held", "H11", "menu", [("applyResolutionChange(defIndex, v, next, true);", "applyResolutionChange(defIndex, v, next);")],
      "the per-headset render width row writes on every step again"),
    M("other-change-does-not-flush", "H11", "menu", [("if (!held) g_held.flush(EditFlush::Other);", "if (!held) {}")],
      "a toggle or a typed value is queued ahead of a held edit"),
    M("held-step-written-at-once", "H11", "menu", [("if (held) g_held.hold(job, job.def, s.page, s.tickMs);\n    else enqueueWrite(job);", "enqueueWrite(job);")],
      "a held step goes to the writer anyway"),
    M("tick-not-run-flat", "H11", "menu", [("            tickHeldEdit(now);   // a held numeric edit whose burst has ended is written now (menu_edit_hold.h)\n", "")],
      "the flat profile's menu never writes a held edit on release"),
    M("tick-not-run-vr", "H11", "menu", [("        tickHeldEdit(now);\n        // Writes the worker finished since last frame.", "        // Writes the worker finished since last frame.")],
      "the VR menu never writes a held edit on release"),
    M("tick-ignores-the-keys", "H11", "menu", [("g_held.tick(now, stepKeyHeld(), highlightedSettingDef(), g_s.page);", "g_held.tick(now, false, highlightedSettingDef(), g_s.page);")],
      "the tick is told no key is down: a hold writes on every tick"),
    M("close-does-not-flush", "H11", "menu",
      [("    g_held.flush(EditFlush::Close);   // a held edit is written now, open or not: a close is never the end of an edit\n", "")],
      "closing the menu leaves a held edit unwritten"),
    M("shutdown-does-not-flush", "H11", "menu",
      [("    g_held.flush(EditFlush::Shutdown);   // before the writer stops, which writes what was queued before it quit\n", "")],
      "a shutdown loses a held edit"),
    M("writer-drops-the-queue-at-quit", "H11", "menu", [("if (g_writer.queue.empty()) return;", "if (g_writer.quit) return;")],
      "the writer, told to quit, leaves what was queued before it unwritten"),
    M("reload-puts-the-held-row-back", "H11", "menu", [("        if (g_held.pending() && g_held.def() == i) continue;\n", "")],
      "the reload of an earlier write sets a held row back to the file's value: the hold loses its steps"),
    M("failed-write-not-rolled-back", "H11", "menu", [("            g_rows[w.job.def].value = rowValue(d);", "            (void)d;")],
      "a failed write leaves the row showing a value the file does not hold"),
    # ---- a failed write while a newer edit of the row exists (EditGeneration, the 0.19.0 review's "existing limitation") ----
    M("rollback-ignores-newer-edits", "H14", "hold", [("{ return jobGeneration == latest; }", "{ return true; }")],
      "a failed write puts the row back to the file's value whatever has been edited since: the review's case ends at 1, not 6"),
    M("rollback-never", "H14", "hold", [("{ return jobGeneration == latest; }", "{ return false; }")],
      "a failed write never puts the row back: the screen shows a value the file does not hold"),
    M("edit-number-not-advanced", "H14", "hold", [("uint32_t next() { return ++latest; }", "uint32_t next() { return latest; }")],
      "every edit of a row has the same number: every failed write is its row's latest"),
    M("edit-number-taken-before-counting", "H14", "hold", [("uint32_t next() { return ++latest; }", "uint32_t next() { return latest++; }")],
      "a job carries the number before the one the row now holds: no failed write is ever its row's latest"),
    M("job-not-stamped", "H11", "menu", [("    job.gen = g_rows[job.def].edits.next();\n", "")],
      "an edit does not take the row's next number: every job carries 0"),
    M("rollback-unconditional", "H11", "menu", [("if (g_rows[w.job.def].edits.rollbackDue(w.job.gen)) {", "if (true) {")],
      "the glue puts the row back whatever the row's edit counter says"),
    M("burst-forgets-its-first-value", "H11", "menu", [("    pending.before = before;\n", "")],
      "the log says N-1 -> N for a burst of many"),
    M("monitor-line-has-its-own-sentence", "H13", "perf", [(FORMAT_CALL, FORMAT_CALL + ' std::strcat(refreshLine, " All zero: no refresh in the window.");')],
      "the monitor adds the closing sentence to the formatter's line, after counts that are not zero"),
]

if __name__ == "__main__":
    sys.exit(lib.main(CONFIG, MUTANTS))

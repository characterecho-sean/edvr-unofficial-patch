#!/usr/bin/env python3
"""The mutation proof for tools\\explorer_cam_fade_test: the rig fails when a rule of the entry fade's wait for the engine's motion is broken.

The rig (explorer_cam_fade_test.cpp) drives the pure comfort timeline (src\\d3d11\\explorer_cam_fade_core.h) with the engine-motion facts the glue feeds it:
an entry that is otherwise ready fades in at once when the motion is live (M1), holds black until the views have run three frames in a row (M2), gives up
one second after it was otherwise ready and says which condition was missing (M3), does not wait for the second skin's join in a scene with no skinned jobs
(M4), waits for nothing when the engine's motion is not armed (M5), never delays an exit (M6), waits on a re-attach like an entry (M7), starts the hold over
when it stops being otherwise ready (M8), leaves the 3 s cap from the press as it was (M9), and the lines (M10); the clock it is fed: the QPC-to-microsecond
conversion that does not wrap and its two call sites in explorer_cam.cpp, read as text (M11), and a clock that steps back (M12). Every failure carries a label
"M<case>.<what>". A rig that passes proves little until it is seen to FAIL on a source that breaks the rule it pins: for each mutation below the machinery
(tools\\rig_mutants_lib.py) copies the header into a temp directory OUTSIDE the repo, applies the edit, rebuilds the rig against the copy and requires a FAIL
on a check of the case that belongs to the rule.

  python tools\\explorer_cam_fade_test\\mutants.py --self-test    text only: every anchor is found exactly once, every case named is in the rig, and
                                                                  build.bat compiles the rig the way the machinery does and runs this self-test
  python tools\\explorer_cam_fade_test\\mutants.py --run [--only a,b] [--jobs N] [--keep] [--verbose]
  python tools\\explorer_cam_fade_test\\mutants.py --list
  python tools\\explorer_cam_fade_test\\mutants.py --run --dry-run   the plan; writes nothing, starts nothing
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import rig_mutants_lib as lib  # noqa: E402

ROOT = HERE.parents[1]
FILES = {
    "fade": ROOT / "src" / "d3d11" / "explorer_cam_fade_core.h",
    "glue": ROOT / "src" / "d3d11" / "explorer_cam.cpp",   # read by the rig as text (M11.p): the two places that convert the counter
}
TREE_EXTRA = [("src/d3d11/explorer_cam_core.h", ROOT / "src" / "d3d11" / "explorer_cam_core.h")]
CONFIG = lib.Config(
    here=__file__, files=FILES, header_keys=("fade",), pin_keys=("glue",), rig=HERE / "explorer_cam_fade_test.cpp",
    rig_label=":rig_explorer_cam_fade_test", rig_source_in_bat="tools\\explorer_cam_fade_test\\explorer_cam_fade_test.cpp", rig_exe_in_bat="explorer_cam_fade_test.exe",
    case_prefix="M", include_dirs=("src/d3d11",), min_mutants=28, run_timeout=120.0, tree_extra=TREE_EXTRA)

KIND_GATE = "if (m_kind != ComfortKind::Exit && in.motionArmed) {"
FADE_IN_NOW = "if (motionUnmet == 0) {\n                beginIn(ComfortEv::FadeIn, ComfortWhy::None, 0, in, out);"
HOLD_END = "if (in.nowUs > m_motionHoldUs && in.nowUs - m_motionHoldUs >= static_cast<uint64_t>(kFadeMotionHoldMs) * 1000u)"
TIMED_OUT = "beginIn(ComfortEv::MotionTimedOut, ComfortWhy::None, motionUnmet, in, out);"
QPC_FN = "return (ticks / freq) * 1000000ull + (ticks % freq) * 1000000ull / freq;"
REBASE = "if (m_have && raw.nowUs + m_shiftUs < m_lastUs) m_shiftUs = m_lastUs - raw.nowUs;"
REALNOW = "return ecm::qpcTicksToUs(qpcNow(), freq);"
BOUNDARY = "in.nowUs = ecm::qpcTicksToUs(static_cast<uint64_t>(t.QuadPart), freq);"

M = lib.M
MUTANTS = [
    # ---- the wait itself ----
    M("motion-wait-removed", ("M2", "M3"), "fade", [(FADE_IN_NOW, FADE_IN_NOW.replace("if (motionUnmet == 0) {", "if (true) {"))],
      "an entry that is otherwise ready fades in at once whatever the engine's motion is doing: the F12 flight's half second of aliased NPC is back"),
    M("views-ignored", "M2", "fade", [("if (in.viewsRun < kFadeViewsLiveFrames) motionUnmet |= kComfortUnmetViews;", "if (false) motionUnmet |= kComfortUnmetViews;")],
      "the views are not waited for: only the second skin's join is"),
    M("views-one-frame-enough", "M2", "fade", [("if (in.viewsRun < kFadeViewsLiveFrames) motionUnmet |= kComfortUnmetViews;", "if (in.viewsRun < 1) motionUnmet |= kComfortUnmetViews;")],
      "a single frame of views is taken for live"),
    M("views-off-by-one", "M1", "fade", [("if (in.viewsRun < kFadeViewsLiveFrames) motionUnmet |= kComfortUnmetViews;", "if (in.viewsRun <= kFadeViewsLiveFrames) motionUnmet |= kComfortUnmetViews;")],
      "an entry whose views have run exactly the three frames asked for still waits for a fourth"),
    M("skin-ignored", "M4", "fade", [("if (in.skinJobs && !in.skinLive) motionUnmet |= kComfortUnmetSkin;", "if (false) motionUnmet |= kComfortUnmetSkin;")],
      "a frame with skinned jobs and no live join fades in on the views alone"),
    M("skin-waits-without-jobs", ("M1", "M4"), "fade", [("if (in.skinJobs && !in.skinLive) motionUnmet |= kComfortUnmetSkin;", "if (!in.skinLive) motionUnmet |= kComfortUnmetSkin;")],
      "a scene with no characters waits for a join that has nothing to join"),
    M("unarmed-waits", "M5", "fade", [(KIND_GATE, KIND_GATE.replace(" && in.motionArmed", ""))],
      "the engine's motion not running at all still holds the entry black"),
    M("exit-waits", "M6", "fade", [(KIND_GATE, KIND_GATE.replace("m_kind != ComfortKind::Exit && ", ""))],
      "an exit waits for the engine's motion like an entry"),
    M("reattach-exempt", "M7", "fade", [(KIND_GATE, KIND_GATE.replace("m_kind != ComfortKind::Exit", "m_kind == ComfortKind::Enter"))],
      "a re-attach does not wait for the engine's motion"),
    # ---- the cap ----
    M("cap-removed", "M3", "fade", [(HOLD_END, "if (false)")],
      "an engine motion that never comes holds the view black for ever (until the placement drops)"),
    M("cap-too-long", "M3", "fade", [(HOLD_END, HOLD_END.replace("* 1000u)", "* 2000u)"))],
      "the hold runs two seconds, not one"),
    M("cap-too-short", "M3", "fade", [(HOLD_END, HOLD_END.replace("* 1000u)", "* 500u)"))],
      "the hold gives up after half a second"),
    M("cap-from-the-press", "M9", "fade", [("if (m_motionHoldUs == 0) m_motionHoldUs = in.nowUs ? in.nowUs : 1;", "if (m_motionHoldUs == 0) m_motionHoldUs = m_startUs ? m_startUs : 1;")],
      "the second counts from the F5 press, so an entry that was ready late gets no hold at all"),
    M("hold-not-restarted", "M8", "fade", [("if (unmet != 0 || !m_released) m_motionHoldUs = 0;", "if (!m_released) m_motionHoldUs = 0;")],
      "the hold keeps its first start when the entry stops being otherwise ready and is ready again"),
    M("old-cap-removed", "M9", "fade", [("} else if (heldMs(in) >= kFadeMaxBlackMs) {", "} else if (false) {")],
      "an entry that is never otherwise ready no longer fades in at the 3 s cap"),
    # ---- the report ----
    M("timeout-says-fade-in", "M3", "fade", [(TIMED_OUT, TIMED_OUT.replace("ComfortEv::MotionTimedOut", "ComfortEv::FadeIn"))],
      "the cap fires as an ordinary fade in: nothing in the log says the motion was missing"),
    M("timeout-names-nothing", "M3", "fade", [(TIMED_OUT, TIMED_OUT.replace("motionUnmet, in, out", "0, in, out"))],
      "the cap's line does not name the conditions that were missing"),
    M("skin-never-named", "M3", "fade", [('item(kComfortUnmetSkin, "', 'item(0x10000u, "')],
      "the cap's line never names the second skin's join"),
    M("fade-in-hides-the-hold", "M2", "fade",
      [("l.motionMs = m_motionHoldUs && in.nowUs > m_motionHoldUs ? static_cast<uint32_t>((in.nowUs - m_motionHoldUs) / 1000) : 0;", "l.motionMs = 0;")],
      "a fade in that waited reports no wait"),
    M("fade-in-line-silent", "M2", "fade", [("if (l.motionMs)", "if (false)")],
      "the fade in line does not say how long it was held for the engine's motion"),
    M("unarmed-line-silent", "M5", "fade", [('if (!l.motionArmed) o.put(" [engine motion: not running, so nothing was waited for]");', "if (!l.motionArmed) {}")],
      "a fade in that waited for nothing because the engine's motion was not running says nothing: it cannot be told from one that was ready"),
    M("armed-line-silent", "M1", "fade", [('else o.put(" [engine motion: views live', 'else if (false) o.put(" [engine motion: views live')],
      "a fade in that was ready does not report the engine's motion as it stood"),
    M("start-line-silent", "M10", "fade", [("(then, if the engine's motion is not live yet, up to %u ms more)", "(then more)")],
      "the Start line does not say the hold exists or how long it can last"),
    # ---- the clock the timeline is fed ----
    M("qpc-multiply-first", "M11", "fade", [(QPC_FN, "return ticks * 1000000ull / freq;")],
      "the old conversion: the product wraps a uint64 after about 21 days of counter and the clock jumps back below every stored deadline"),
    M("qpc-remainder-dropped", "M11", "fade", [(QPC_FN, "return (ticks / freq) * 1000000ull;")],
      "the conversion keeps whole seconds only: the clock stands still between them"),
    M("realnow-call-site-old", "M11", "glue", [(REALNOW, "return qpcNow() * 1000000ull / freq;")],
      "realNowUs (the follow smoothing's clock) is left on the multiply-first arithmetic"),
    M("boundary-call-site-old", "M11", "glue", [(BOUNDARY, "in.nowUs = static_cast<uint64_t>(t.QuadPart) * 1000000ull / freq;")],
      "the frame boundary's input to the comfort fade is left on the multiply-first arithmetic: the review's black screen"),
    M("backwards-clock-not-rebased", "M12", "fade", [(REBASE, "(void)0;")],
      "a clock that steps back leaves the press and the hold in the future of now: the 3 s cap and the motion hold stop firing"),
]

if __name__ == "__main__":
    sys.exit(lib.main(CONFIG, MUTANTS))

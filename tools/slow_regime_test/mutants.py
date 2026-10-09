#!/usr/bin/env python3
"""The mutation proof for tools\\slow_regime_test: the rig fails when a rule of the runtime's vendor instruments is flipped.

The rig (slow_regime_test.cpp) holds the vendor event log (src\\openxr\\vendor_events.h), the long xrEndFrame episodes (end_frame_episodes.h), the slow regime
(slow_regime.h), the test trigger's schedule (src\\common\\slow_test.h), the hooks they are fed from (SessionState's observer in session_state.h, FrameBoundary's
end-of-call hook and the hold in frame_boundary.h) and, by source text, the glue (native_runtime_host.h, perf_monitor.cpp, frame_flag, build.bat, edvr.ini). Every
check carries a label "<case>.<what>", a letter and a number. A rig that passes proves little until it is seen to FAIL on a source that breaks the rule it pins. This
tool does that: for each mutation below it copies the source it edits into a temp directory OUTSIDE the repo, applies one textual edit (or a few that belong
together), and runs the rig against that copy, requiring it to fail on a check of the case that belongs to the rule (a FAIL label starting with one of the mutation's
case ids). A mutated HEADER is compiled into a fresh rig; a mutated GLUE source (the G1 pins) is run through the unmutated rig, which is handed a temp repository root
holding the edited copy. Nothing is written inside the repo; the temp directory is removed at the end.

  python tools\\slow_regime_test\\mutants.py --self-test       text only: every anchor is found exactly once in the file it edits as it is now,
                                                             every case named is in the rig, and build.bat compiles the rig the way this tool
                                                             does (add --build-bat PATH to check another copy)
  python tools\\slow_regime_test\\mutants.py --run [--only a,b] [--jobs N] [--keep]
  python tools\\slow_regime_test\\mutants.py --list
  python tools\\slow_regime_test\\mutants.py --run --dry-run   the plan; writes nothing, starts nothing

--run needs the MSVC toolchain (cl.exe on PATH, or Visual Studio found with vswhere as build.bat does). --self-test runs in build.bat's rig and
is what keeps an edit of a source from silently orphaning a mutation: if an anchor stops matching, the build fails and this file says which.
"""
import argparse
import concurrent.futures
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
FILES = {
    "events": ROOT / "src" / "openxr" / "vendor_events.h",
    "episodes": ROOT / "src" / "openxr" / "end_frame_episodes.h",
    "regime": ROOT / "src" / "openxr" / "slow_regime.h",
    "schedule": ROOT / "src" / "common" / "slow_test.h",
    "boundary": ROOT / "src" / "openxr" / "frame_boundary.h",
    "session": ROOT / "src" / "openxr" / "session_state.h",
    "host": ROOT / "src" / "openxr" / "native_runtime_host.h",
    "perf": ROOT / "src" / "d3d11" / "perf_monitor.cpp",
    "flagcpp": ROOT / "src" / "common" / "frame_flag.cpp",
    "flagh": ROOT / "src" / "common" / "frame_flag.h",
    "bat": ROOT / "build.bat",
    "ini": ROOT / "edvr.ini",
}
HEADER_KEYS = ("events", "episodes", "regime", "schedule", "boundary", "session")     # compiled into the rig: a mutation of one rebuilds the rig
PIN_KEYS = ("host", "perf", "flagcpp", "flagh", "bat", "ini")                         # read by the rig as text: a mutation of one is run through the unmutated rig
# Everything the rig's headers include, laid out as the headers expect to find each other (src\common beside src\openxr, the OpenVR compat header beside both).
TREE = [
    ("src/openxr/vendor_events.h", ROOT / "src" / "openxr" / "vendor_events.h"),
    ("src/openxr/end_frame_episodes.h", ROOT / "src" / "openxr" / "end_frame_episodes.h"),
    ("src/openxr/slow_regime.h", ROOT / "src" / "openxr" / "slow_regime.h"),
    ("src/openxr/frame_boundary.h", ROOT / "src" / "openxr" / "frame_boundary.h"),
    ("src/openxr/session_state.h", ROOT / "src" / "openxr" / "session_state.h"),
    ("src/openxr/frame_pacer.h", ROOT / "src" / "openxr" / "frame_pacer.h"),
    ("src/openxr/native_trace.h", ROOT / "src" / "openxr" / "native_trace.h"),
    ("src/openxr/submission_measurement.h", ROOT / "src" / "openxr" / "submission_measurement.h"),
    ("src/openvr/compat/openvr_v0_9_20.h", ROOT / "src" / "openvr" / "compat" / "openvr_v0_9_20.h"),
    ("src/common/slow_test.h", ROOT / "src" / "common" / "slow_test.h"),
    ("src/common/vram_watch.h", ROOT / "src" / "common" / "vram_watch.h"),
]
INCLUDE_DIRS = ("src/common", "src/openxr")        # the rig's /I directories under the header tree (build.bat's compile line has the same, and the OpenXR headers)
EXTERNAL_INCLUDES = (ROOT / "third_party" / "openxr" / "include",)
BAT_INCLUDES = ("src\\common", "src\\openxr", "third_party\\openxr\\include")
RIG = HERE / "slow_regime_test.cpp"
BUILD_BAT = ROOT / "build.bat"
RIG_LABEL = ":rig_slow_regime_test"
RIG_SOURCE_IN_BAT = "tools\\slow_regime_test\\slow_regime_test.cpp"
RIG_EXE_IN_BAT = "slow_regime_test.exe"
CASE_RE = r"[A-Z]\d+"
CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS"]
LINK_ARGS = []
MIN_MUTANTS = 100
RUN_TIMEOUT = 180.0


class Mutant:
    def __init__(self, name, caught, file, edits, why):
        self.name = name
        self.caught = tuple([caught] if isinstance(caught, str) else caught)   # case ids whose checks must report it
        self.file = file                                                        # a key of FILES
        self.edits = list(edits)                                                # (old, new) pairs, applied in order
        self.why = why


def M(name, caught, file, edits, why):
    return Mutant(name, caught, file, edits, why)


# ---- the anchors: text of the production sources, verbatim (the self-test finds each exactly once) ------------------------------
# vendor_events.h
EV_CAP = "constexpr unsigned kVendorEventLineCap = 200;\n"
EV_UNK = "constexpr unsigned kVendorUnknownTypesNamed = 24;\n"
EV_SYNC = "    case XR_SESSION_STATE_SYNCHRONIZED: return \"SYNCHRONIZED\";\n"
EV_LOCAL = "    case XR_REFERENCE_SPACE_TYPE_LOCAL: return \"LOCAL\";\n"
EV_WARN = "  if (l == XR_PERF_SETTINGS_NOTIF_LEVEL_WARNING_EXT) return \"WARNING\";\n"
EV_FROMTO = "xrSessionStateName(state_, a), xrSessionStateName(v.state, b),"
EV_STATE = "        state_ = v.state;\n"
EV_STAMP = "        stateNotedMs_ = nowMs;\n"
EV_LOST = "        counts_.lostEvents += v.lostEventCount;\n"
EV_LAG = "        if (std::isfinite(lagMs)) std::snprintf(lag, sizeof(lag), \"%.3f\", lagMs);\n"
EV_POSE = "v.poseValid ? 1u : 0u);"
EV_NAMED = "        if (named_[i] == static_cast<int32_t>(e.type)) return 0;   // this type has had its line\n"
EV_CAPCMP = "    if (counts_.logged - unknownLines() >= kVendorEventLineCap) {\n"
EV_CAPONCE = "      if (capNoted_) return 0;\n"
EV_UNKLINES = "  uint64_t unknownLines() const { return counts_.unknownTypes; }\n"
EV_SUMHEAD = "\"native_xr_events_summary,%s%s%sreceived=%llu"
EV_ARMED = "\"native_xr_events,armed=1,source=xrPollEvent,line_cap=%u,"
EV_AGE = "  double stateAgeMs(uint64_t nowMs) const { return everState_ && nowMs >= stateNotedMs_ ? static_cast<double>(nowMs - stateNotedMs_) : -1.0; }\n"
EV_FIRST = "type=unknown,number=%d,first_of_type=1\""
EV_REASON = "reason ? \"reason=\" : \"\", reason ? reason : \"\", reason ? \",\" : \"\",\n                      static_cast<unsigned long long>(counts_.received)"
# end_frame_episodes.h
EP_PERIODS = "constexpr double kEndFramePeriods = 3.0;\n"
EP_NORMAL = "constexpr unsigned kEndFrameEndAfterNormal = 8;\n"
EP_EVERY = "constexpr uint64_t kEndFrameStartLineEveryMs = 2000;\n"
EP_CAP = "constexpr unsigned kEndFrameStartLineCap = 100;\n"
EP_MINSLOW = "constexpr unsigned kEndFrameEndLineMinSlow = 20;\n"
EP_SLOW = "    const bool slow = c.periodMs > 0 && std::isfinite(c.ms) && c.ms >= kEndFramePeriods * c.periodMs;\n"
EP_SLOWCOUNT = "      ++summary_.slowCalls;\n"
EP_LONGEST = "      if (c.ms > summary_.longestMs) summary_.longestMs = c.ms;\n"
EP_RUNRESET = "      normalRun_ = 0;\n      add(c);\n"
EP_ENDRULE = "    if (++normalRun_ >= kEndFrameEndAfterNormal) close(\"normal_calls\", out);\n"
EP_INEP = "    const uint64_t inEpisode = calls_ >= normalRun_ ? calls_ - normalRun_ : calls_;\n"
EP_STARTED = "    startedMs_ = c.nowMs >= static_cast<uint64_t>(c.ms) ? c.nowMs - static_cast<uint64_t>(c.ms) : 0;\n"
EP_RANK = "    const uint64_t rank = (count_ + 1) / 2;\n"
EP_CENTER = "      if (seen >= rank) return (i + 0.5) * kBinMs;\n"
EP_BIN = "  static constexpr double kBinMs = 0.5;\n"
EP_MEAN = "count_ ? sumMs_ / static_cast<double>(count_) : 0.0"
EP_MAX = "    if (c.ms > maxMs_) maxMs_ = c.ms;\n"
EP_LIMIT = "(!everWritten_ || c.nowMs - lastStartLineMs_ >= kEndFrameStartLineEveryMs)"
EP_LINECAP = "summary_.startLines < kEndFrameStartLineCap && "
EP_WRITE = "    const bool write = startWritten_ || count_ >= kEndFrameEndLineMinSlow;\n"
EP_STARTFLAG = "static_cast<unsigned long long>(summary_.episodes), reason, startWritten_ ? 1u : 0u, static_cast<unsigned long long>(inEpisode),"
EP_PATH = "  static const char* pathName(const EndFrameCall& c) { return c.background ? \"loading\" : c.overlapped ? \"overlapped\" : \"synchronous\"; }\n"
EP_PACING = "c.turbo ? \"deferred\" : \"runtime\""
EP_STARTHEAD = "\"native_end_frame_episode,episode=%llu,sequence=%llu,ms=%.4f,periods=%.2f,"
EP_ENDHEAD = "\"native_end_frame_episode_end,episode=%llu,reason=%s,start_line=%u,calls=%llu,"
EP_FINISH = "    if (open_) close(\"session_close\", out);\n"
EP_OPEN = "    open_ = false;\n    const bool write = startWritten_"
EP_SUMHEAD = "\"native_end_frame_episodes_summary,%s%s%sthreshold_periods=%.1f,"
EP_ARMED = "\"native_end_frame_episodes,armed=1,threshold_periods=%.1f,period=last_real_predicted_display_period,"
EP_EPISODES = "    ++summary_.episodes;\n    count_ = 1;\n"
EP_ENDLINES = "    ++summary_.endLines;\n"
EP_LASTSEQ = "      lastSequence_ = c.sequence;\n      return;\n"
# slow_regime.h
SR_FRACTION = "constexpr double kSlowFraction = 0.40;\n"
SR_HOLD = "constexpr unsigned kSlowHoldSeconds = 5;\n"
SR_END = "constexpr unsigned kSlowEndSeconds = 2;\n"
SR_STILL = "constexpr unsigned kSlowStillSlowSeconds = 30;\n"
SR_SHARE = "constexpr double kSlowOwnerShare = 0.35;\n"
SR_BUCKET = "constexpr unsigned kSlowBucketMs = 1000;\n"
SR_CATCH = "constexpr unsigned kSlowMaxCatchUp = 120;\n"
SR_JUDGE = "    const bool slow = refHz_ > 0 && static_cast<double>(bucket_.frames) < kSlowFraction * refHz_;\n"
SR_PERIOD = "    else if (f.periodMs > 0) refHz_ = 1000.0 / f.periodMs;\n"
SR_REPORTED = "    if (f.refHz > 0) refHz_ = f.refHz;\n"
SR_LOOP = "        if (closed >= kSlowMaxCatchUp) {\n"
SR_ANCHOR = "          bucketStartMs_ += ((f.nowMs - bucketStartMs_) / kSlowBucketMs) * kSlowBucketMs;\n"
SR_REGSTART = "        if (++runBuckets_ == 1) regimeStartMs_ = bucketStartMs_;\n        lastSlowEndMs_ = bucketStartMs_ + kSlowBucketMs;\n"
SR_REGEND = "        lastSlowEndMs_ = bucketStartMs_ + kSlowBucketMs;\n        if (runBuckets_ >= kSlowHoldSeconds) {\n"
SR_WINRESET = ("        emit(slowReport(SlowEvent::StillSlow, windowSums_, refHz_, durationS(), regime_, nullptr));\n"
               "        windowSums_ = SlowSums{};\n        windowBuckets_ = 0;\n")
SR_FOLD = "        regimeSums_.merge(pending_);\n        windowSums_.merge(pending_);\n        windowBuckets_ += normalBuckets_;\n"
SR_ENDCMP = "      if (++normalBuckets_ >= kSlowEndSeconds) {\n"
SR_ENDSUMS = "        emit(slowReport(SlowEvent::End, regimeSums_, refHz_, durationS(), regime_, \"recovered\"));\n"
SR_REGNUM = "          ++regime_;\n"
SR_GAME = "  r.gameMs = std::max(0.0, r.frameMs - accounted);\n"
SR_SHARECMP = "  if (r.largestShare >= kSlowOwnerShare) {\n"
SR_BEST = "    if (it.ms > best->ms) best = &it;\n"
SR_VSHARE = "  r.vendorShare = (r.vendorEndMs + r.vendorWaitMs + r.vendorSwapMs) / r.frameMs;\n"
SR_NOFRAMES = "  if (!s.frames || !s.spanMs) {\n"
SR_FPS = "  r.fps = n * 1000.0 / static_cast<double>(s.spanMs);\n"
SR_FRACTION_CALC = "  r.fraction = refHz > 0 ? r.fps / refHz : 0;\n"
SR_LINEHEAD = "      \"native_slow_regime,event=%s,%sregime=%u,duration_s=%.1f,window_s=%.1f,"
SR_VRAMNONE = "std::snprintf(figures, sizeof(figures), \"vram=unavailable\");"
SR_VRAMSEP = "formatVramFigures(figures, sizeof(figures), *vram, ',', \"vram_\")"
SR_SUMOWNER = "\"held by %s: %.1f ms of every %.1f ms frame (%.0f%%)\""
SR_SUMNONE = "\"no single owner; the largest is %s at %.0f%% of a %.1f ms frame\""
SR_KEYEND = "    case SlowOwner::VendorEndFrame: return \"vendor_end_frame\";\n"
SR_TEXTEND = "    case SlowOwner::VendorEndFrame: return \"the vendor runtime's xrEndFrame\";\n"
SR_FINISHOPEN = "    emit(slowReport(SlowEvent::End, regimeSums_, refHz_, durationS(), regime_, \"session_close\"));\n    inRegime_ = false;\n"
SR_FINISHCLOSED = "    if (!inRegime_) return;\n    emit(slowReport(SlowEvent::End, regimeSums_, refHz_, durationS(), regime_, \"session_close\"));\n"
SR_ARMED = "      \"native_slow_regime,armed=1,threshold_fraction=%.2f,of=display_rate,hold_s=%u,"
SR_COMMA = "    if (*p == ',') *p = ';';\n"
SR_SUMHEAD = "\"native_slow_regime_summary,%s%s%sregimes=%u,open=%u,"
SR_FRAMESSEEN = "    ++framesSeen_;\n"
SR_SLOWSEC = "    if (slow) ++slowSeconds_;\n"
SR_ADD = "    bucket_.add(f);\n"
SR_BOUNDARY = "    if (f.nowMs >= bucketStartMs_ + kSlowBucketMs) {\n"
SR_PERIODSECS = "  r.windowS = static_cast<double>(s.spanMs) / 1000.0;\n"
SR_RECOVERED = "regime_, \"recovered\"));\n"
SR_CTXRULE = "  r.context = s.loading * 2 >= s.frames ? SlowContext::Loading : s.empty * 2 >= s.frames ? SlowContext::NoLayers : SlowContext::Scene;\n"
SR_CTXTALLY = "    if (f.background) ++loading;\n    else if (f.noLayers) ++empty;\n"
SR_CTXMERGE = "    loading += o.loading;\n    empty += o.empty;\n"
SR_CTXCOUNTS = "  r.loadingFrames = s.loading;\n  r.emptyFrames = s.empty;\n"
SR_CTXLINE = "      \"context=%s,loading_frames=%llu,empty_frames=%llu,\"\n"
SR_CTXARGS = "      r.frameMs, slowContextKey(r.context), static_cast<unsigned long long>(r.loadingFrames), static_cast<unsigned long long>(r.emptyFrames),\n"
SR_CTXKEY = "    case SlowContext::NoLayers: return \"no_layers\";\n"
# slow_test.h
ST_START = "constexpr uint64_t kSlowTestStartMs = 90000;\n"
ST_FOR = "constexpr uint64_t kSlowTestForMs = 40000;\n"
ST_MAX = "constexpr int kSlowTestMaxMs = 500;\n"
ST_BEGIN = "            if (elapsed < kSlowTestStartMs) return Step::None;\n"
ST_ENDCMP = "        if (elapsed >= kSlowTestStartMs + kSlowTestForMs) {\n"
ST_CLAMP = "        holdMs_ = holdMs < 0 ? 0 : holdMs > kSlowTestMaxMs ? kSlowTestMaxMs : holdMs;\n"
ST_NOW = "    uint32_t holdNowMs() const { return active_ ? static_cast<uint32_t>(holdMs_) : 0u; }\n"
ST_DONE = "            done_ = true;\n"
ST_ELAPSED = "        const uint64_t elapsed = nowMs >= armedMs_ ? nowMs - armedMs_ : 0;\n"
# frame_boundary.h
FB_HOLD = "      lastResult_ = session_.end(frame_, end, {geometryReady_, pixels});\n      sink_.holdEndFrame(); }\n    sink_.endFrameReturned("
FB_REPORT = "    sink_.endFrameReturned({sequence, endFrameMs_, lastPeriod_, shouldRender, pixels, background, turbo_, lastResult_});\n"
FB_SHOULD = "    const bool shouldRender = frame_.shouldRender; // end() below clears it\n    const uint64_t sequence = frame_.sequence;\n"
FB_EMPTYREPORT = "    sink_.endFrameReturned({sequence, ms, lastPeriod_, shouldRender, false, false, turbo_, r});\n"
FB_EMPTYSCOPE = "    { SubmissionWallScope measured(&ms); r = session_.end(frame, empty, {false, false}); }\n"
# session_state.h
SS_OBSERVE = "      if (observer_) observer_(buffer, observerContext_);\n"
SS_UNAVAIL = "      if (r == XR_EVENT_UNAVAILABLE) return lastResult_;\n"
SS_CLEAR = "    observer_ = nullptr; observerContext_ = nullptr;\n"
SS_STATECHANGED = "      if (buffer.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) { unhandled(buffer); continue; }\n"
# the host, perf_monitor.cpp, frame_flag, build.bat, edvr.ini (text pins)
H_OBSERVER = "    state.setEventObserver(vendorEvent,this);\n"
H_START = "    startInstruments();\n"
H_SLEEP = "    Sleep(ms);\n  }\n  void writeSlowLine("
H_EPISODES = "    endFrameEpisodes.observe(call,endFrameLines);\n"
H_REGIME = "    slowRegime.observe(frame,[&](const SlowReport& report){writeSlowLine(report);});\n"
H_ENDMS = "    frame.vendorEndMs=info.ms;\n"
H_OVERLAP = "    finishingOverlapped=true; // endFrameReturned names the path\n"
H_FINISH = "    if(tracing)finishInstruments();\n"
H_PERIODIC = "writeInstrumentSummaries(\"periodic\");}\n"
H_WALL = "      stereo.renderCaptured(frameViews,frameSpace,captured,layer,observer,&wall,framePlacement,fade);"
H_MEASURE = "    const bool measure=counterNow(&dispatchBegan);\n"
H_VRAM = "    {const char* why=nullptr;vramAdapter.Attach(vramAdapterOf(graphics.device(),&why));vramWhy=why?why:\"\";}\n"
H_COPY = "    const double copy=scene?transferWall.producerDispatch+submitSample.receiveMs:0.0;\n"
H_NOLAYERS = "    frame.noLayers=!info.layers&&!info.background; // an empty end (clear, drain): the context field tells a load from a slow scene\n"
H_WAITFIELD = "    frame.vendorWaitMs=boundary.waitBlockMs()+boundary.pacerBlockMs();\n"
H_LAG = "XR_SUCCEEDED(host.api.convertTime(host.instance,&counter,&now))"
P_SLOWTICK = "    freezeTestTick();\n    slowTestTick();\n"
P_END = "            requestEndFrameHold(0);\n"
P_KEY = "Config::get().getIntInRange(\"advanced.slow_test_ms\", 0, 0, 500)"
FF_VERSION = "constexpr uint32_t kFrameFlagVersion = 37;"
FF_CLAMP = "ms > 5000u ? 5000u : ms"
FF_FIELD = "    volatile LONG     endFrameHold;\n"
B_RIG = "\"tools\\slow_regime_test\\slow_regime_test.cpp\""
I_KEY = "#slow_test_ms = 0\n"

MUTANTS = [
    # ---- E1: names ---------------------------------------------------------------------------------------------------------------
    M("state-synchronized-renamed", "E1", "events", [(EV_SYNC, "    case XR_SESSION_STATE_SYNCHRONIZED: return \"SYNCED\";\n")], "SYNCHRONIZED is another word"),
    M("space-local-lowercase", "E1", "events", [(EV_LOCAL, "    case XR_REFERENCE_SPACE_TYPE_LOCAL: return \"local\";\n")], "LOCAL is lower case"),
    M("perf-warning-short", "E1", "events", [(EV_WARN, "  if (l == XR_PERF_SETTINGS_NOTIF_LEVEL_WARNING_EXT) return \"WARN\";\n")], "WARNING is WARN"),
    # ---- E2: the decode ----------------------------------------------------------------------------------------------------------
    M("from-and-to-swapped", ("E2", "E3"), "events", [(EV_FROMTO, "xrSessionStateName(v.state, a), xrSessionStateName(state_, b),")], "a state change says to where it says from"),
    M("lost-events-not-summed", ("E2", "E5"), "events", [(EV_LOST, "")], "the total of lost events is never added"),
    M("lag-always-unknown", "E2", "events", [(EV_LAG, "        if (false) std::snprintf(lag, sizeof(lag), \"%.3f\", lagMs);\n")], "a lag the host measured is never printed"),
    M("pose-always-valid", "E2", "events", [(EV_POSE, "1u);")], "a reference space change says its pose is valid whatever it says"),
    M("unknown-first-word", "E4", "events", [(EV_FIRST, "type=unknown,number=%d,first=1\"")], "the undecoded type's line says first, not first_of_type"),
    # ---- E3: the state ------------------------------------------------------------------------------------------------------------
    M("state-never-kept", "E3", "events", [(EV_STATE, "")], "the last state is never recorded: every change is from UNKNOWN"),
    M("state-age-from-boot", "E3", "events", [(EV_AGE, "  double stateAgeMs(uint64_t nowMs) const { return everState_ ? static_cast<double>(nowMs) : -1.0; }\n")], "the state's age is the clock, not the time since"),
    M("state-stamp-never-kept", "E3", "events", [(EV_STAMP, "")], "the time the state was read is never recorded"),
    # ---- E4: undecoded types ------------------------------------------------------------------------------------------------------
    M("unknown-types-twelve", ("E4", "E6"), "events", [(EV_UNK, "constexpr unsigned kVendorUnknownTypesNamed = 12;\n")], "twelve undecoded types are named, not 24"),
    M("unknown-named-every-time", "E4", "events", [(EV_NAMED, "")], "an undecoded type has a line every time"),
    # ---- E5: the bound ------------------------------------------------------------------------------------------------------------
    M("cap-one-hundred", ("E5", "E6"), "events", [(EV_CAP, "constexpr unsigned kVendorEventLineCap = 100;\n")], "the line cap is 100"),
    M("cap-off-by-one", "E5", "events", [(EV_CAPCMP, "    if (counts_.logged - unknownLines() > kVendorEventLineCap) {\n")], "201 lines before the notice"),
    M("cap-notice-repeats", "E5", "events", [(EV_CAPONCE, "")], "the cap notice is written for every event past it"),
    M("unknown-lines-use-the-cap", "E5", "events", [(EV_UNKLINES, "  uint64_t unknownLines() const { return 0; }\n")], "the undecoded types' lines count against the 200"),
    # ---- E6: the summary and the armed line -----------------------------------------------------------------------------------------
    M("summary-renamed", "E6", "events", [(EV_SUMHEAD, "\"native_xr_event_summary,%s%s%sreceived=%llu")], "the summary line has another name"),
    M("armed-without-armed", "E6", "events", [(EV_ARMED, "\"native_xr_events,source=xrPollEvent,line_cap=%u,")], "the armed line does not say armed=1"),
    M("summary-reason-dropped", "E6", "events", [(EV_REASON, "\"\", \"\", \"\",\n                      static_cast<unsigned long long>(counts_.received)")], "the summary never says why it was written"),
    # ---- E7: SessionState's observer ----------------------------------------------------------------------------------------------
    M("observer-never-called", "E7", "session", [(SS_OBSERVE, "")], "the observer is never called"),
    M("observer-misses-instance-loss", "E7", "session", [(SS_OBSERVE, ""), (SS_STATECHANGED, "      if (observer_) observer_(buffer, observerContext_);\n" + SS_STATECHANGED)],
      "the observer is called only for events that get past the instance-loss check"),
    M("observer-sees-the-unavailable", "E7", "session", [(SS_UNAVAIL, "      if (observer_) observer_(buffer, observerContext_);\n" + SS_UNAVAIL)], "the observer is called for a poll that found nothing"),
    M("observer-survives-reset", "E7", "session", [(SS_CLEAR, "")], "a reset leaves the observer bound"),
    # ---- P1: the threshold ----------------------------------------------------------------------------------------------------------
    M("threshold-four-periods", ("P1", "P2", "P3"), "episodes", [(EP_PERIODS, "constexpr double kEndFramePeriods = 4.0;\n")], "an episode is four periods, not three"),
    M("threshold-strict", "P1", "episodes", [(EP_SLOW, "    const bool slow = c.periodMs > 0 && std::isfinite(c.ms) && c.ms > kEndFramePeriods * c.periodMs;\n")], "exactly three periods is not an episode"),
    M("no-period-is-slow", "P1", "episodes", [(EP_SLOW, "    const bool slow = std::isfinite(c.ms) && c.ms >= kEndFramePeriods * c.periodMs;\n")], "with no period every call is slow"),
    # ---- P2: the start line ----------------------------------------------------------------------------------------------------------
    M("start-head-renamed", "P2", "episodes", [(EP_STARTHEAD, "\"native_end_frame_episode_start,episode=%llu,sequence=%llu,ms=%.4f,periods=%.2f,")], "the start line has another name"),
    M("path-words-swapped", "P2", "episodes", [(EP_PATH, "  static const char* pathName(const EndFrameCall& c) { return c.background ? \"loading\" : c.overlapped ? \"synchronous\" : \"overlapped\"; }\n")],
      "the overlapped path is called synchronous"),
    M("pacing-words-swapped", "P2", "episodes", [(EP_PACING, "c.turbo ? \"runtime\" : \"deferred\"")], "deferred pacing is called runtime"),
    # ---- P3: the episode's life ---------------------------------------------------------------------------------------------------------
    M("end-after-four-normal", "P3", "episodes", [(EP_NORMAL, "constexpr unsigned kEndFrameEndAfterNormal = 4;\n")], "four normal calls end it"),
    M("end-needs-one-more", "P3", "episodes", [(EP_ENDRULE, "    if (++normalRun_ > kEndFrameEndAfterNormal) close(\"normal_calls\", out);\n")], "nine normal calls are needed"),
    M("slow-call-keeps-the-run", "P3", "episodes", [(EP_RUNRESET, "      add(c);\n")], "a slow call does not restart the run of normal ones"),
    M("calls-include-the-trailing-run", "P3", "episodes", [(EP_INEP, "    const uint64_t inEpisode = calls_;\n")], "the eight normal calls that ended it are counted in it"),
    M("duration-from-the-call-end", "P3", "episodes", [(EP_STARTED, "    startedMs_ = c.nowMs;\n")], "the episode starts when its first call returned, not when it began"),
    M("end-head-renamed", "P3", "episodes", [(EP_ENDHEAD, "\"native_end_frame_episode_stop,episode=%llu,reason=%s,start_line=%u,calls=%llu,")], "the end line has another name"),
    M("last-sequence-not-kept", "P3", "episodes", [(EP_LASTSEQ, "      return;\n")], "the last slow call's sequence is not kept"),
    M("episode-never-numbered", ("P3", "P5"), "episodes", [(EP_EPISODES, "    count_ = 1;\n")], "the episodes are never counted"),
    # ---- P4: p50 and max ----------------------------------------------------------------------------------------------------------------
    M("p50-rank-low", "P4", "episodes", [(EP_RANK, "    const uint64_t rank = count_ / 2;\n")], "the median is one rank low"),
    M("p50-bin-edge", "P4", "episodes", [(EP_CENTER, "      if (seen >= rank) return i * kBinMs;\n")], "the median is the bin's start, not its middle"),
    M("bin-one-ms", "P4", "episodes", [(EP_BIN, "  static constexpr double kBinMs = 1.0;\n")], "the histogram's bins are a millisecond"),
    M("mean-over-calls", "P4", "episodes", [(EP_MEAN, "count_ ? sumMs_ / static_cast<double>(calls_) : 0.0")], "the mean is over every call, normal ones too"),
    M("max-never-kept", "P4", "episodes", [(EP_MAX, "")], "the max is never kept"),
    # ---- P5: the rate limit ---------------------------------------------------------------------------------------------------------------
    M("start-every-half-second", "P5", "episodes", [(EP_EVERY, "constexpr uint64_t kEndFrameStartLineEveryMs = 500;\n")], "a start line every half second"),
    M("start-cap-fifty", ("P1", "P5"), "episodes", [(EP_CAP, "constexpr unsigned kEndFrameStartLineCap = 50;\n")], "fifty start lines a session"),
    M("long-from-ten", "P5", "episodes", [(EP_MINSLOW, "constexpr unsigned kEndFrameEndLineMinSlow = 10;\n")], "ten slow calls are a long episode"),
    M("start-limiter-open", "P5", "episodes", [(EP_LIMIT, "(!everWritten_ || true)")], "every episode gets its start line"),
    M("start-cap-ignored", "P5", "episodes", [(EP_LINECAP, "")], "the cap on start lines is never applied"),
    M("long-episode-silent", "P5", "episodes", [(EP_WRITE, "    const bool write = startWritten_;\n")], "an episode whose start line was held back is silent however long"),
    M("start-line-always-claimed", "P5", "episodes", [(EP_STARTFLAG, "static_cast<unsigned long long>(summary_.episodes), reason, 1u, static_cast<unsigned long long>(inEpisode),")], "the end line always says its start line was written"),
    M("end-lines-uncounted", "P5", "episodes", [(EP_ENDLINES, "")], "the end lines written are never counted"),
    # ---- P6: close, summary, armed -----------------------------------------------------------------------------------------------------------
    M("close-does-not-end", "P6", "episodes", [(EP_FINISH, "")], "an open episode is not ended at session close"),
    M("close-leaves-it-open", "P3", "episodes", [(EP_OPEN, "    const bool write = startWritten_")], "an ended episode is still open"),
    M("slow-calls-uncounted", ("P1", "P6"), "episodes", [(EP_SLOWCOUNT, "")], "the slow calls are never counted"),
    M("longest-never-kept", "P6", "episodes", [(EP_LONGEST, "")], "the longest call of the session is never kept"),
    M("episodes-summary-renamed", "P6", "episodes", [(EP_SUMHEAD, "\"native_end_frame_episode_summary,%s%s%sthreshold_periods=%.1f,")], "the summary has another name"),
    M("episodes-armed-changed", "P6", "episodes", [(EP_ARMED, "\"native_end_frame_episodes,armed=1,threshold_periods=%.1f,period=predicted_period,")], "the armed line names another period"),
    # ---- P7, P8: FrameBoundary -----------------------------------------------------------------------------------------------------------------
    M("hold-after-the-timed-region", ("P8", "G1"), "boundary", [(FB_HOLD, "      lastResult_ = session_.end(frame_, end, {geometryReady_, pixels}); }\n    sink_.holdEndFrame();\n    sink_.endFrameReturned(")],
      "the hold runs after the timed region, so xr_end_frame does not see it"),
    M("finish-reports-nothing", "P7", "boundary", [(FB_REPORT, "")], "finish() does not report the call"),
    M("report-should-render-false", "P7", "boundary", [(FB_SHOULD, "    const bool shouldRender = false;\n    const uint64_t sequence = frame_.sequence;\n")], "the vendor's answer is read after end() cleared it"),
    M("report-no-sequence", "P7", "boundary", [(FB_SHOULD, "    const bool shouldRender = frame_.shouldRender; // end() below clears it\n    const uint64_t sequence = 0;\n")], "the report has no sequence"),
    M("report-always-layers", "P7", "boundary", [(FB_REPORT, "    sink_.endFrameReturned({sequence, endFrameMs_, lastPeriod_, shouldRender, true, background, turbo_, lastResult_});\n")], "every end says it had layers"),
    M("report-not-background", "P7", "boundary", [(FB_REPORT, "    sink_.endFrameReturned({sequence, endFrameMs_, lastPeriod_, shouldRender, pixels, false, turbo_, lastResult_});\n")], "a loading frame is not said to be one"),
    M("report-not-turbo", "P7", "boundary", [(FB_REPORT, "    sink_.endFrameReturned({sequence, endFrameMs_, lastPeriod_, shouldRender, pixels, background, false, lastResult_});\n")], "deferred pacing is not said"),
    M("report-no-period", "P7", "boundary", [(FB_REPORT, "    sink_.endFrameReturned({sequence, endFrameMs_, 0, shouldRender, pixels, background, turbo_, lastResult_});\n")], "the report has no period"),
    M("report-no-time", "P8", "boundary", [(FB_REPORT, "    sink_.endFrameReturned({sequence, 0.0, lastPeriod_, shouldRender, pixels, background, turbo_, lastResult_});\n")], "the report has no duration"),
    M("empty-ends-unreported", "P7", "boundary", [(FB_EMPTYREPORT, "")], "the empty ends clear() and drain() make are not reported"),
    M("empty-ends-held", "P8", "boundary", [(FB_EMPTYSCOPE, "    { SubmissionWallScope measured(&ms); r = session_.end(frame, empty, {false, false}); sink_.holdEndFrame(); }\n")], "the hold also runs in the empty ends"),
    # ---- S1, S2: the judgement --------------------------------------------------------------------------------------------------------------------
    M("fraction-fifty", ("S1", "S2"), "regime", [(SR_FRACTION, "constexpr double kSlowFraction = 0.50;\n")], "a second is slow under 50% of the display rate"),
    M("judge-inclusive", "S1", "regime", [(SR_JUDGE, "    const bool slow = refHz_ > 0 && static_cast<double>(bucket_.frames) <= kSlowFraction * refHz_;\n")], "exactly 40% is slow"),
    M("no-period-fallback", "S1", "regime", [(SR_PERIOD, "")], "with no reported rate the period is never used"),
    M("reported-rate-ignored", "S1", "regime", [(SR_REPORTED, "    if (false) refHz_ = f.refHz;\n")], "the vendor's reported rate is never used"),
    M("bucket-half-second", ("S1", "S3"), "regime", [(SR_BUCKET, "constexpr unsigned kSlowBucketMs = 500;\n")], "the buckets are half a second"),
    # ---- S3: begin ---------------------------------------------------------------------------------------------------------------------------------
    M("hold-three-seconds", "S3", "regime", [(SR_HOLD, "constexpr unsigned kSlowHoldSeconds = 3;\n")], "three slow seconds begin a regime"),
    M("regime-starts-a-second-late", "S3", "regime", [(SR_REGSTART, "        if (++runBuckets_ == 2) regimeStartMs_ = bucketStartMs_;\n        lastSlowEndMs_ = bucketStartMs_ + kSlowBucketMs;\n")],
      "the regime's start is the second slow second's"),
    M("regime-end-never-moves", "S3", "regime", [(SR_REGEND, "        if (runBuckets_ >= kSlowHoldSeconds) {\n")], "the regime's last slow second is never noted while it is being found"),
    M("regime-not-numbered", ("S3", "S5"), "regime", [(SR_REGNUM, "")], "the regimes are never numbered"),
    M("frames-never-added", "S3", "regime", [(SR_ADD, "")], "no frame is ever added to a second"),
    M("boundary-frame-stays", "S3", "regime", [(SR_BOUNDARY, "    if (f.nowMs > bucketStartMs_ + kSlowBucketMs) {\n")], "a frame exactly on the second's edge belongs to the second it ends"),
    M("fps-per-frame", "S3", "regime", [(SR_FPS, "  r.fps = n / static_cast<double>(s.spanMs);\n")], "the frame rate is not per second"),
    M("fraction-multiplied", "S3", "regime", [(SR_FRACTION_CALC, "  r.fraction = refHz > 0 ? r.fps * refHz : 0;\n")], "the fraction is the rate times the display's"),
    M("window-in-ms", "S3", "regime", [(SR_PERIODSECS, "  r.windowS = static_cast<double>(s.spanMs);\n")], "the window is in milliseconds"),
    # ---- S4: still slow ----------------------------------------------------------------------------------------------------------------------------
    M("still-slow-twenty", "S4", "regime", [(SR_STILL, "constexpr unsigned kSlowStillSlowSeconds = 20;\n")], "a still_slow line every 20 s"),
    M("still-slow-window-never-resets", "S4", "regime", [(SR_WINRESET, "        emit(slowReport(SlowEvent::StillSlow, windowSums_, refHz_, durationS(), regime_, nullptr));\n")],
      "the still_slow window is never cleared: the second one covers 60 s"),
    # ---- S5: end -----------------------------------------------------------------------------------------------------------------------------------
    M("end-after-one-second", "S5", "regime", [(SR_END, "constexpr unsigned kSlowEndSeconds = 1;\n")], "one normal second ends it"),
    M("end-needs-three", "S5", "regime", [(SR_ENDCMP, "      if (++normalBuckets_ > kSlowEndSeconds) {\n")], "three normal seconds are needed"),
    M("normal-second-not-folded", "S5", "regime", [(SR_FOLD, "")], "a normal second between slow ones is dropped from the regime"),
    M("end-covers-the-window", "S5", "regime", [(SR_ENDSUMS, "        emit(slowReport(SlowEvent::End, windowSums_, refHz_, durationS(), regime_, \"recovered\"));\n")], "the end report covers the last 30 s, not the regime"),
    M("recovered-renamed", ("S5", "S9"), "regime", [(SR_RECOVERED, "regime_, \"ended\"));\n")], "the end reason is another word"),
    # ---- S6: owners --------------------------------------------------------------------------------------------------------------------------------
    M("owner-share-half", "S6", "regime", [(SR_SHARE, "constexpr double kSlowOwnerShare = 0.50;\n")], "an owner needs half the frame to be named"),
    M("owner-share-strict", "S6", "regime", [(SR_SHARECMP, "  if (r.largestShare > kSlowOwnerShare) {\n")], "exactly 35% is not named"),
    M("owner-tie-goes-last", "S6", "regime", [(SR_BEST, "    if (it.ms >= best->ms) best = &it;\n")], "of equal owners the last is named"),
    M("game-goes-negative", "S6", "regime", [(SR_GAME, "  r.gameMs = r.frameMs - accounted;\n")], "owners that add to more than the frame leave the game a negative share"),
    M("vendor-share-without-swapchain", "S6", "regime", [(SR_VSHARE, "  r.vendorShare = (r.vendorEndMs + r.vendorWaitMs) / r.frameMs;\n")], "the vendor's share leaves out its swapchain calls"),
    M("no-frames-divides", "S6", "regime", [(SR_NOFRAMES, "  if (!s.spanMs) {\n")], "a window with no frames is divided by zero"),
    # ---- S7: gaps ------------------------------------------------------------------------------------------------------------------------------------
    M("catch-up-never", "S7", "regime", [(SR_LOOP, "        if (closed >= 0) {\n")], "no empty second is ever closed"),
    M("catch-up-240", "S7", "regime", [(SR_CATCH, "constexpr unsigned kSlowMaxCatchUp = 240;\n")], "a gap closes 240 seconds"),
    M("gap-not-anchored", "S7", "regime", [(SR_ANCHOR, "")], "after a gap the clock is not moved to the frame"),
    # ---- S8: close -----------------------------------------------------------------------------------------------------------------------------------
    M("finish-leaves-the-regime", "S8", "regime", [(SR_FINISHOPEN, "    emit(slowReport(SlowEvent::End, regimeSums_, refHz_, durationS(), regime_, \"session_close\"));\n")], "a regime that ended at close is still open"),
    M("finish-without-a-regime", "S8", "regime", [(SR_FINISHCLOSED, "    emit(slowReport(SlowEvent::End, regimeSums_, refHz_, durationS(), regime_, \"session_close\"));\n")], "an end line at close when no regime is open"),
    # ---- S9: the lines -------------------------------------------------------------------------------------------------------------------------------
    M("slow-line-head", "S9", "regime", [(SR_LINEHEAD, "      \"native_slow_regime,event=%s,%sregime=%u,window_s=%.1f,duration_s=%.1f,")], "the SLOW line gives the window before the duration"),
    M("vram-unavailable-worded", "S9", "regime", [(SR_VRAMNONE, "std::snprintf(figures, sizeof(figures), \"vram=none\");")], "no figures are written as vram=none"),
    M("vram-space-separated", "S9", "regime", [(SR_VRAMSEP, "formatVramFigures(figures, sizeof(figures), *vram, ' ', \"vram_\")")], "the figures are space-separated inside a comma-separated line"),
    M("summary-without-share", "S9", "regime", [(SR_SUMOWNER, "\"held by %s: %.1f ms of every %.1f ms frame\"")], "the words leave out the share"),
    M("none-worded-differently", "S9", "regime", [(SR_SUMNONE, "\"no owner; the largest is %s at %.0f%% of a %.1f ms frame\"")], "no single owner is said another way"),
    M("owner-key-shortened", "S9", "regime", [(SR_KEYEND, "    case SlowOwner::VendorEndFrame: return \"vendor_end\";\n")], "the owner key is vendor_end"),
    M("owner-text-shortened", "S9", "regime", [(SR_TEXTEND, "    case SlowOwner::VendorEndFrame: return \"xrEndFrame\";\n")], "the owner is named xrEndFrame, not the vendor's"),
    # ---- S10: armed, summary, counters -----------------------------------------------------------------------------------------------------------------
    M("armed-without-the-basis", "S10", "regime", [(SR_ARMED, "      \"native_slow_regime,armed=1,threshold_fraction=%.2f,hold_s=%u,")], "the armed line does not say the threshold is of the display rate"),
    M("armed-commas-kept", "S10", "regime", [(SR_COMMA, "    if (*p == ';') *p = ';';\n")], "a reason's commas split the armed line"),
    M("slow-summary-renamed", "S10", "regime", [(SR_SUMHEAD, "\"native_slow_regime_totals,%s%s%sregimes=%u,open=%u,")], "the summary has another name"),
    M("frames-not-counted", "S10", "regime", [(SR_FRAMESSEEN, "")], "the frames seen are never counted"),
    M("slow-seconds-not-counted", "S10", "regime", [(SR_SLOWSEC, "")], "the slow seconds are never counted"),
    # ---- S11: the context a game-owned regime is read by ---------------------------------------------------------------------------------------------
    M("context-strictly-more-than-half", "S11", "regime", [(SR_CTXRULE, "  r.context = s.loading * 2 > s.frames ? SlowContext::Loading : s.empty * 2 > s.frames ? SlowContext::NoLayers : SlowContext::Scene;\n")],
      "exactly half of the frames is not enough to call a window a load"),
    M("context-by-a-third", "S11", "regime", [(SR_CTXRULE, "  r.context = s.loading * 3 >= s.frames ? SlowContext::Loading : s.empty * 3 >= s.frames ? SlowContext::NoLayers : SlowContext::Scene;\n")],
      "a third of the frames is enough to call a window a load"),
    M("context-empty-first", "S11", "regime", [(SR_CTXRULE, "  r.context = s.empty * 2 >= s.frames ? SlowContext::NoLayers : s.loading * 2 >= s.frames ? SlowContext::Loading : SlowContext::Scene;\n")],
      "empty ends are checked before the runtime's loading frames"),
    M("context-never-loading", "S11", "regime", [(SR_CTXRULE, "  r.context = s.empty * 2 >= s.frames ? SlowContext::NoLayers : SlowContext::Scene;\n")], "a window of loading frames is not called one"),
    M("context-never-no-layers", "S11", "regime", [(SR_CTXRULE, "  r.context = s.loading * 2 >= s.frames ? SlowContext::Loading : SlowContext::Scene;\n")], "a window of ends with no layers is called scene"),
    M("context-always-scene", "S11", "regime", [(SR_CTXRULE, "  r.context = SlowContext::Scene;\n")], "every window is the game's scenes"),
    M("context-loading-also-empty", "S11", "regime", [(SR_CTXTALLY, "    if (f.background) ++loading;\n    if (f.noLayers) ++empty;\n")], "a loading frame is counted as an empty end too"),
    M("context-loading-not-counted", "S11", "regime", [(SR_CTXTALLY, "    if (f.noLayers) ++empty;\n")], "the runtime's loading frames are never counted"),
    M("context-empty-not-counted", "S11", "regime", [(SR_CTXTALLY, "    if (f.background) ++loading;\n")], "the ends with no layers are never counted"),
    M("context-counts-not-merged", "S11", "regime", [(SR_CTXMERGE, "")], "the regime's sums lose the loading and empty counts of every second folded into them"),
    M("context-counts-swapped", "S11", "regime", [(SR_CTXCOUNTS, "  r.loadingFrames = s.empty;\n  r.emptyFrames = s.loading;\n")], "the report's loading and empty counts are each other's"),
    M("context-counts-dropped", "S11", "regime", [(SR_CTXCOUNTS, "")], "the report carries no counts"),
    M("context-line-renamed", ("S9", "S11"), "regime", [(SR_CTXLINE, "      \"ctx=%s,loading_frames=%llu,empty_frames=%llu,\"\n")], "the line's field is not called context"),
    M("context-line-args-swapped", "S11", "regime", [(SR_CTXARGS, "      r.frameMs, slowContextKey(r.context), static_cast<unsigned long long>(r.emptyFrames), static_cast<unsigned long long>(r.loadingFrames),\n")],
      "the line writes the empty count where the loading count belongs"),
    M("context-key-renamed", "S11", "regime", [(SR_CTXKEY, "    case SlowContext::NoLayers: return \"empty\";\n")], "the no_layers context has another key"),
    # ---- T1: the schedule ------------------------------------------------------------------------------------------------------------------------------
    M("hold-from-sixty", "T1", "schedule", [(ST_START, "constexpr uint64_t kSlowTestStartMs = 60000;\n")], "the hold begins 60 s in"),
    M("hold-for-ten", "T1", "schedule", [(ST_FOR, "constexpr uint64_t kSlowTestForMs = 10000;\n")], "the hold lasts 10 s"),
    M("hold-max-a-second", "T1", "schedule", [(ST_MAX, "constexpr int kSlowTestMaxMs = 1000;\n")], "a hold may be a second a call"),
    M("begin-after-the-edge", "T1", "schedule", [(ST_BEGIN, "            if (elapsed <= kSlowTestStartMs) return Step::None;\n")], "exactly 90 s is too soon"),
    M("end-after-the-edge", "T1", "schedule", [(ST_ENDCMP, "        if (elapsed > kSlowTestStartMs + kSlowTestForMs) {\n")], "exactly 130 s is not the end"),
    M("clamp-lost", "T1", "schedule", [(ST_CLAMP, "        holdMs_ = holdMs < 0 ? 0 : holdMs;\n")], "a value over 500 is kept"),
    M("hold-before-the-start", "T1", "schedule", [(ST_NOW, "    uint32_t holdNowMs() const { return static_cast<uint32_t>(holdMs_); }\n")], "the hold is asked for before it begins"),
    M("hold-restarts", "T1", "schedule", [(ST_DONE, "")], "the hold begins again after it ended"),
    M("clock-behind-wraps", "T1", "schedule", [(ST_ELAPSED, "        const uint64_t elapsed = nowMs - armedMs_;\n")], "a clock behind the arming reading wraps to a huge elapsed time"),
    # ---- G1: the glue ------------------------------------------------------------------------------------------------------------------------------------
    M("glue-events-unobserved", "G1", "host", [(H_OBSERVER, "")], "the host never observes the vendor's events"),
    M("glue-no-armed-lines", "G1", "host", [(H_START, "")], "the instruments never write their armed lines"),
    M("glue-hold-never-sleeps", "G1", "host", [(H_SLEEP, "  }\n  void writeSlowLine(")], "the test hold never holds"),
    M("glue-episodes-unfed", "G1", "host", [(H_EPISODES, "")], "the end-frame episodes are never fed"),
    M("glue-regime-unfed", "G1", "host", [(H_REGIME, "")], "the slow regime is never fed"),
    M("glue-end-frame-unattributed", "G1", "host", [(H_ENDMS, "    frame.vendorEndMs=0;\n")], "the vendor's end-frame time is not given to the slow regime"),
    M("glue-wait-unattributed", "G1", "host", [(H_WAITFIELD, "    frame.vendorWaitMs=0;\n")], "the vendor's pose wait is not given to the slow regime"),
    M("glue-copy-double-counted", "G1", "host", [(H_COPY, "    const double copy=scene?transferWall.producerDispatch+transferWall.producerAcquire+transferWall.producerFlush+submitSample.receiveMs:0.0;\n")],
      "the copy adds nested phases to the phase that holds them"),
    M("glue-overlap-unnamed", "G1", "host", [(H_OVERLAP, "")], "the overlapped finish does not say so"),
    M("glue-no-layers-unfed", "G1", "host", [(H_NOLAYERS, "")], "the ends with no layers are never told to the slow regime"),
    M("glue-no-layers-any-call", "G1", "host", [(H_NOLAYERS, "    frame.noLayers=!info.layers||!info.background; // an empty end (clear, drain): the context field tells a load from a slow scene\n")],
      "every call without a loading frame's flags is an empty end"),
    M("glue-close-unfinished", "G1", "host", [(H_FINISH, "")], "an open episode and regime are not ended at close"),
    M("glue-no-periodic-summary", "G1", "host", [(H_PERIODIC, "}\n")], "the summaries are never written while the session runs"),
    M("glue-wall-only-while-open", "G1", "host", [(H_WALL, "      stereo.renderCaptured(frameViews,frameSpace,captured,layer,observer,submitStats.full()?nullptr:&wall,framePlacement,fade);")],
      "the swapchain calls are timed only while the submit window has room"),
    M("glue-treatments-only-while-open", "G1", "host", [(H_MEASURE, "    const bool measure=!submitStats.full()&&counterNow(&dispatchBegan);\n")], "the treatments are timed only while the submit window has room"),
    M("glue-no-vram", "G1", "host", [(H_VRAM, "")], "the SLOW line has no adapter to read"),
    M("glue-no-lag", "G1", "host", [(H_LAG, "false")], "a session state change has no lag"),
    M("glue-trigger-never-ends", "G1", "perf", [(P_END, "")], "the hold request is never withdrawn"),
    M("glue-trigger-not-ticked", "G1", "perf", [(P_SLOWTICK, "    freezeTestTick();\n")], "the test trigger is never ticked"),
    M("glue-trigger-wrong-key", "G1", "perf", [(P_KEY, "Config::get().getIntInRange(\"advanced.slow_hold_ms\", 0, 0, 500)")], "the key read is not the one documented"),
    M("glue-flag-version", "G1", "flagh", [(FF_VERSION, "constexpr uint32_t kFrameFlagVersion = 36;")], "the layout grew without a new version"),
    M("glue-flag-unclamped", "G1", "flagcpp", [(FF_CLAMP, "ms")], "the runtime may be asked to hold a call for any time"),
    M("glue-flag-no-field", "G1", "flagcpp", [(FF_FIELD, "")], "the shared block has no room for the hold"),
    M("glue-not-in-the-build", "G1", "bat", [(B_RIG, "\"tools\\slow_regime_test\\other.cpp\"")], "build.bat does not compile this rig"),
    M("glue-key-undocumented", "G1", "ini", [(I_KEY, "")], "the key is not documented in edvr.ini"),
]


# ---- applying an edit ----------------------------------------------------------------------------------------------------
def apply_edits(text, edits, name="?"):
    """The text with each (old, new) applied in order; each `old` must occur exactly once in the text as it stands then."""
    for old, new in edits:
        count = text.count(old)
        if count != 1:
            raise ValueError("mutation %s: an anchor occurs %d times (want 1): %r" % (name, count, old[:90]))
        if old == new:
            raise ValueError("mutation %s: an edit changes nothing: %r" % (name, old[:90]))
        text = text.replace(old, new)
    return text


def read_source(path):
    return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


def fail_labels(output):
    """Every check label the rig printed on a 'FAIL: <case>.<what>' line (the text up to ' -- ', ' [' or the end of the line); the rig's
    closing 'FAIL: slow regime: n of m checks failed' line is not a label."""
    labels = []
    for line in output.splitlines():
        if line.startswith("FAIL: "):
            label = re.split(r" -- | \[", line[len("FAIL: "):], 1)[0].strip()
            if re.match(CASE_RE + r"\.", label):
                labels.append(label)
    return labels


def rig_cases(rig_text):
    """The case ids the rig's checks are labelled with."""
    return set(re.findall(r'"(' + CASE_RE + r')[.:]', rig_text))


def label_block(bat_text, label):
    """The lines of one build.bat subroutine, continuation lines (a trailing ^) joined; None when the label is absent."""
    lines = bat_text.replace("\r\n", "\n").split("\n")
    start = next((i for i, l in enumerate(lines) if l.strip().lower() == label.lower() or l.lower().startswith(label.lower() + " ")), None)
    if start is None:
        return None
    out, joined = [], ""
    for line in lines[start + 1:]:
        if line.startswith(":") and not line.startswith("::"):
            break
        if line.rstrip().endswith("^"):
            joined += line.rstrip()[:-1] + " "
            continue
        out.append(joined + line)
        joined = ""
    return out


# ---- the toolchain -----------------------------------------------------------------------------------------------------------
class Toolchain:
    """cl.exe and link.exe by absolute path, with the environment they need: this one if cl is on PATH, else the one vcvars64.bat makes."""

    def __init__(self):
        found = shutil.which("cl.exe")
        if found:
            self.env = os.environ.copy()
        else:
            vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
            vs = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                 "-property", "installationPath"], capture_output=True, text=True).stdout.strip()
            bat = Path(vs) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
            if not vs or not bat.is_file():
                raise RuntimeError("cl.exe is not on PATH and no Visual Studio with the x64 C++ tools was found")
            dump = subprocess.run('cmd.exe /s /c ""%s" >nul && set"' % bat, capture_output=True, text=True).stdout
            self.env = {}
            for line in dump.splitlines():
                if "=" in line:
                    key, value = line.split("=", 1)
                    self.env[key] = value
            path = next((v for k, v in self.env.items() if k.upper() == "PATH"), "")
            found = shutil.which("cl.exe", path=path)
            if not found:
                raise RuntimeError("vcvars64.bat did not put cl.exe on PATH")
        self.cl = str(found)


def build_rig(tc, tree, exe):
    """Compile the rig against the header tree at `tree`; (exit code, output)."""
    cmd = ([tc.cl] + CL_FLAGS + ["/I" + str(tree / d) for d in INCLUDE_DIRS] + ["/I" + str(p) for p in EXTERNAL_INCLUDES] +
           ["/Fo" + str(exe.parent) + "\\", "/Fe" + str(exe), str(RIG), "/link", "/INCREMENTAL:NO"] + LINK_ARGS)
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(exe.parent))
    return done.returncode, done.stdout + done.stderr


def run_rig(exe, root, tc):
    """(outcome, labels, tail) of one run of the rig against the repository root `root`: 'pass', 'fail', 'crash', 'timeout'."""
    try:
        done = subprocess.run([str(exe), "--self-test", str(root)], capture_output=True, text=True, env=tc.env, errors="replace", timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        return "timeout", [], "no result within %d s" % RUN_TIMEOUT
    out = done.stdout + "\n" + done.stderr
    if done.returncode == 0:
        return "pass", [], ""
    labels = fail_labels(out)
    if not labels:
        return "crash", [], "exit 0x%08X" % (done.returncode & 0xFFFFFFFF)
    return "fail", labels, ""


def make_tree(dest, mutated=None):
    """The headers the rig includes, laid out under `dest`; `mutated` maps a FILES key to replacement text."""
    mutated = mutated or {}
    for rel, src in TREE:
        target = dest / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        key = next((k for k in HEADER_KEYS if FILES[k] == src), None)
        if key and key in mutated:
            target.write_text(mutated[key], encoding="utf-8", newline="\n")
        else:
            shutil.copyfile(src, target)


def make_root(dest, mutated):
    """A repository root holding the sources the rig reads as text, `mutated` replacing one of them. The rig reads eight: the glue's own (PIN_KEYS) and
    the two headers it also compiles (frame_boundary.h and session_state.h, whose hooks the G1 pins look for): a root without all eight fails G1.read
    on every mutation and proves nothing about any pin."""
    for key in PIN_KEYS + ("boundary", "session"):
        target = dest / FILES[key].relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(mutated.get(key, read_source(FILES[key])), encoding="utf-8", newline="\n")


# ---- the run -----------------------------------------------------------------------------------------------------------------------
def select(only):
    if not only:
        return MUTANTS
    wanted = [n for n in only.split(",") if n]
    unknown = [n for n in wanted if n not in {m.name for m in MUTANTS}]
    if unknown:
        raise ValueError("no such mutation: " + ", ".join(unknown))
    return [m for m in MUTANTS if m.name in wanted]


def plan_text(mutants):
    lines = ["%d mutation(s); each runs the rig against an edited copy of one source in a temp directory outside the repo and must fail on a check of the cases named:" % len(mutants)]
    for m in mutants:
        lines.append("  %-40s %-8s caught by %-9s %s" % (m.name, m.file, "/".join(m.caught), m.why))
    lines.append("cl.exe flags: " + " ".join(CL_FLAGS))
    return "\n".join(lines)


def run_all(only=None, jobs=None, keep=False, dry_run=False, verbose=False, out=sys.stdout):
    mutants = select(only)
    if dry_run:
        print(plan_text(mutants), file=out)
        print("dry-run: nothing compiled, started or written", file=out)
        return 0
    tc = Toolchain()
    sources = {k: read_source(p) for k, p in FILES.items()}
    work = Path(tempfile.mkdtemp(prefix="srm_"))
    workers = jobs or min(8, os.cpu_count() or 2)
    try:
        # The control: the rig built from the unmutated headers, run against the real repository, every case.
        control = work / "k"
        make_tree(control)
        exe = control / "rig.exe"
        code, text = build_rig(tc, control, exe)
        if code != 0:
            print("control: nocompile\n" + text.strip()[-1500:], file=out)
            return 1
        outcome, labels, tail = run_rig(exe, ROOT, tc)
        print("control (the unmutated sources, every case): %s %s" % (outcome, tail or " ".join(labels[:3])), file=out)
        if outcome != "pass":
            print("the rig does not pass on the unmutated sources when built this way; nothing below means anything", file=out)
            return 1

        def one(index, m):
            try:
                mutated = apply_edits(sources[m.file], m.edits, m.name)
            except ValueError as error:
                return m, "badedit", str(error)
            d = work / ("m%03d" % index)
            d.mkdir()
            if m.file in HEADER_KEYS:
                tree = d / "t"
                make_tree(tree, {m.file: mutated})
                mexe = d / "rig.exe"
                code, text = build_rig(tc, tree, mexe)
                if code != 0:
                    return m, "nocompile", (text.strip().splitlines() or [""])[-1]
                outcome, labels, tail = run_rig(mexe, ROOT, tc)
            else:
                root = d / "r"
                make_root(root, {m.file: mutated})
                outcome, labels, tail = run_rig(exe, root, tc)
            if outcome == "fail":
                if any(l.startswith(c + ".") for l in labels for c in m.caught):
                    if verbose:
                        return m, "caught", "%d failing label(s): %s" % (len(labels), ", ".join(labels))
                    return m, "caught", "%d failing label(s), first %s" % (len(labels), labels[0])
                return m, "OTHER", "failed on %s only" % ", ".join(sorted({l.split(".")[0] for l in labels}))
            return m, {"pass": "SURVIVED", "crash": "CRASH", "timeout": "TIMEOUT"}[outcome], tail

        results = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            for m, verdict, detail in pool.map(lambda im: one(*im), list(enumerate(mutants))):
                results.append((m, verdict, detail))
                print("%-9s %-40s %-9s %s" % (verdict, m.name, "/".join(m.caught), detail if verbose else detail[:150]), file=out)
        bad = [r for r in results if r[1] != "caught"]
        print("%d mutation(s): %d caught by their own case, %d not" % (len(results), len(results) - len(bad), len(bad)), file=out)
        for m, verdict, detail in bad:
            print("  NOT CAUGHT: %s (%s): wanted %s, got %s %s" % (m.name, m.why, "/".join(m.caught), verdict, detail[:120]), file=out)
        return 0 if not bad else 1
    finally:
        if keep:
            print("kept: %s" % work, file=out)
        else:
            shutil.rmtree(work, ignore_errors=True)


# ---- the self-test ------------------------------------------------------------------------------------------------------------------
def self_test(build_bat=BUILD_BAT):
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    # the tool's own pieces
    check(fail_labels("a\nFAIL: P4.flight -- x [y]\nFAIL: S2.sub\nFAIL: slow regime: 3 of 9\n") == ["P4.flight", "S2.sub"],
          "fail_labels reads the label of every check's FAIL: line, and not the closing summary")
    check(fail_labels("PASS: 12 slow regime checks\n") == [], "fail_labels reads nothing from a pass")
    check(apply_edits("a b c", [("b", "B")]) == "a B c", "apply_edits applies one edit")
    for bad, why in ((("a b b", [("b", "B")]), "a repeated anchor"), (("a b", [("x", "y")]), "a missing anchor"), (("a b", [("b", "b")]), "a no-op edit")):
        try:
            apply_edits(*bad)
            check(False, "apply_edits refuses " + why)
        except ValueError:
            pass
    joined = label_block(":rig_a\nx ^\n y\nexit /b 0\n:rig_b\nz\n", ":rig_a")
    check(joined is not None and [" ".join(l.split()) for l in joined] == ["x y", "exit /b 0"], "label_block joins continuations and stops at the next label")
    check(rig_cases('check(x, "E1.a: b"); check(y, "S10.c")') == {"E1", "S10"}, "rig_cases reads the case ids off the labels")

    # every mutation against the sources as they are now, and against the rig
    sources = {k: read_source(p) for k, p in FILES.items()}
    rig = read_source(RIG)
    cases = rig_cases(rig)
    names = [m.name for m in MUTANTS]
    check(len(names) == len(set(names)), "mutation names are unique")
    check(len(MUTANTS) >= MIN_MUTANTS, "the mutation list did not shrink below %d (%d)" % (MIN_MUTANTS, len(MUTANTS)))
    for m in MUTANTS:
        check(m.file in FILES, "%s edits %s, which this tool does not know" % (m.name, m.file))
        if m.file in FILES:
            try:
                mutated = apply_edits(sources[m.file], m.edits, m.name)
                check(mutated != sources[m.file], "%s changes the source" % m.name)
            except ValueError as error:
                failures.append(str(error))
        for c in m.caught:
            check(c in cases, "%s: the rig has no case %s" % (m.name, c))
    covered = {c for m in MUTANTS for c in m.caught}
    check(covered == cases, "every case of the rig has a mutation, and only cases of the rig: %s vs %s" % (sorted(covered), sorted(cases)))
    for rel, src in TREE:
        check(src.is_file(), "%s exists (the header tree the rig is built against)" % rel)

    # build.bat compiles the rig the way this tool does, and runs this tool's self-test
    bat = Path(build_bat).read_bytes().decode("utf-8", errors="replace")
    block = label_block(bat, RIG_LABEL)
    check(block is not None, "build.bat has %s" % RIG_LABEL)
    if block:
        text = "\n".join(block)
        cl = next((l for l in block if l.strip().lower().startswith("cl.exe")), "")
        for flag in CL_FLAGS:
            check(flag in cl, "build.bat's rig compile has %s" % flag)
        for d in BAT_INCLUDES:
            check('/I"%s"' % d in cl, "build.bat's rig finds the headers through /I %s (the mutated tree is found the same way)" % d)
        check(RIG_SOURCE_IN_BAT in cl, "build.bat's rig compile has the rig's source")
        check(RIG_EXE_IN_BAT + '" --dry-run' in text and RIG_EXE_IN_BAT + '" --self-test "%ROOT%"' in text,
              "build.bat runs the rig's --dry-run and --self-test with the repository root (the G1 pins read the glue's sources from it)")
        check('mutants.py" --self-test' in text, "build.bat runs this tool's self-test")

    # --dry-run starts nothing and writes nothing
    calls = []
    real_mkdtemp, real_run = tempfile.mkdtemp, subprocess.run
    tempfile.mkdtemp = lambda *a, **k: calls.append("mkdtemp") or real_mkdtemp(*a, **k)
    subprocess.run = lambda *a, **k: calls.append("subprocess") or real_run(*a, **k)
    try:
        sink = io.StringIO()
        code = run_all(dry_run=True, out=sink)
    finally:
        tempfile.mkdtemp, subprocess.run = real_mkdtemp, real_run
    check(code == 0 and not calls and "dry-run" in sink.getvalue(), "--dry-run starts no process and makes no directory (saw %s)" % calls)
    try:
        select("no-such-mutation")
        check(False, "--only names an unknown mutation")
    except ValueError:
        pass

    if failures:
        for f in failures:
            print("FAIL: " + f, file=sys.stderr)
        return 1
    print("PASS: slow_regime_test mutants.py self-test (%d mutations over %d cases, every anchor found once, build.bat wired)" % (len(MUTANTS), len(covered)))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--only", default="")
    parser.add_argument("--jobs", type=int, default=0)
    parser.add_argument("--keep", action="store_true", help="leave the temp directory (printed) for a look at a mutant")
    parser.add_argument("--verbose", action="store_true", help="print every label a mutant made the rig fail on, not just the first")
    parser.add_argument("--build-bat", default=str(BUILD_BAT), help="the build.bat the self-test reads the rig's label from")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test(args.build_bat)
    if args.list:
        print(plan_text(MUTANTS))
        return 0
    if args.run:
        try:
            return run_all(only=args.only, jobs=args.jobs or None, keep=args.keep, dry_run=args.dry_run, verbose=args.verbose)
        except (ValueError, RuntimeError) as error:
            print("error: %s" % error, file=sys.stderr)
            return 2
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())

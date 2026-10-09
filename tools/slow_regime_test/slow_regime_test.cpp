// The rig for the runtime's vendor instruments (docs/headset-lock-vdxr-2026-10-02.md, instruments 2, 3 and 4 and the
// test trigger): the vendor's events (src\openxr\vendor_events.h), long xrEndFrame episodes (end_frame_episodes.h), the
// slow regime (slow_regime.h), the test trigger's schedule (src\common\slow_test.h), the hooks in SessionState and
// FrameBoundary they are fed from, and by source text the glue in the host.
//
// What it holds, case by case. Every check's label starts with its case id (a letter and a number), which is how
// tools\slow_regime_test\mutants.py tells a mutation that tripped the right check from one that tripped something else:
//
//   E1  the names: session states, reference spaces, performance-settings domains and levels
//   E2  every decoded event, line by line: the exact text of each
//   E3  the last session state and its age, from -> to across events
//   E4  an event type nothing decodes: named once by its number, up to 24 types
//   E5  the bound: 200 lines of decoded events, one notice, the counts and the state going on past it
//   E6  the summary and the armed line
//   E7  SessionState's observer: it sees every event, handled or not, and changes nothing
//
//   P1  the threshold: 3 display periods or more starts an episode, nothing without a period
//   P2  the start line: the first call of the episode, field by field, and the words for the path and the pacing
//   P3  the episode's life: counted while it lasts, ended by 8 normal calls in a row, the duration and the calls
//   P4  p50 and max: the histogram, the clamp for a call over 512 ms, the mean
//   P5  the rate limit on start lines, the cap, the end line a long episode always gets
//   P6  session close, the summary and the armed line
//   P7  FrameBoundary's hook: every xrEndFrame the boundary makes is reported once, on every path
//   P8  the test hold: inside the timed region, and nowhere else
//
//   S1  a second is slow under 40% of the display rate, judged by the vendor's rate or else by its period
//   S2  the vendor's half-rate mode is not slow; 10 fps at 72 Hz is
//   S3  five slow seconds begin a regime: the SLOW report's figures
//   S4  every 30 s of a regime that goes on: the still_slow report covers those 30 s
//   S5  two normal seconds end it; one does not; the end report covers the whole regime
//   S6  who holds the frame: each owner in turn, the 35% rule, the game's share never negative, no frames
//   S7  a gap with no frames, and a gap too long to walk second by second
//   S8  session close
//   S9  the lines: SLOW, still_slow and end as the log carries them, the figures beside them, every one fitting
//   S10 the armed and summary lines and the counters
//
//   T1  the test trigger's schedule: off, armed, the hold 90 s in for 40 s, the key's clamp
//
//   G1  the glue, by source text: the host feeds the instruments from every end and every event, writes their armed
//       lines at startup and their summaries at close; the hold is inside the timed region; the test trigger's
//       request crosses frame_flag; the measurement the attribution needs is always on
//
//   slow_regime_test.exe --dry-run          touches nothing
//   slow_regime_test.exe --self-test [root]  root is the repository, for the G1 source pins (skipped without it)
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "frame_boundary.h"
#include "vendor_events.h"
#include "end_frame_episodes.h"
#include "slow_regime.h"
#include "slow_test.h"
#include "vram_watch.h"

using namespace edvr;
using namespace edvr::openxr;

namespace {
unsigned g_checks = 0, g_failures = 0;

void check(bool ok, const char* label) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("FAIL: %s\n", label);
  }
}

bool contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }
// A failing expectation must never be a crash: every read of a result that may not exist goes through these.
const std::string& lineAt(const std::vector<std::string>& v, size_t i) {
  static const std::string none;
  return i < v.size() ? v[i] : none;
}
const SlowReport& reportAt(const std::vector<SlowReport>& v, size_t i) {
  static const SlowReport none;
  return i < v.size() ? v[i] : none;
}
const EndFrameInfo& infoAt(const std::vector<EndFrameInfo>& v, size_t i) {
  static const EndFrameInfo none;
  return i < v.size() ? v[i] : none;
}
bool closeTo(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

// ---- events: builders ------------------------------------------------------------------------------------------
template <class T>
XrEventDataBuffer wrap(const T& event) {
  XrEventDataBuffer b{XR_TYPE_EVENT_DATA_BUFFER};
  static_assert(sizeof(T) <= sizeof(XrEventDataBuffer), "an event fits the buffer");
  std::memcpy(&b, &event, sizeof(event));
  return b;
}
XrEventDataBuffer stateChanged(XrSessionState s, XrTime t, XrSession session = reinterpret_cast<XrSession>(2)) {
  XrEventDataSessionStateChanged e{XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
  e.session = session;
  e.state = s;
  e.time = t;
  return wrap(e);
}
XrEventDataBuffer eventsLost(uint32_t n) {
  XrEventDataEventsLost e{XR_TYPE_EVENT_DATA_EVENTS_LOST};
  e.lostEventCount = n;
  return wrap(e);
}
XrEventDataBuffer instanceLoss(XrTime t) {
  XrEventDataInstanceLossPending e{XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING};
  e.lossTime = t;
  return wrap(e);
}
XrEventDataBuffer referenceSpace(XrReferenceSpaceType type, XrTime t, bool valid) {
  XrEventDataReferenceSpaceChangePending e{XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING};
  e.referenceSpaceType = type;
  e.changeTime = t;
  e.poseValid = valid ? XR_TRUE : XR_FALSE;
  return wrap(e);
}
XrEventDataBuffer interactionProfile() {
  XrEventDataInteractionProfileChanged e{XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED};
  return wrap(e);
}
XrEventDataBuffer visibilityMask(uint32_t view) {
  XrEventDataVisibilityMaskChangedKHR e{XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR};
  e.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  e.viewIndex = view;
  return wrap(e);
}
XrEventDataBuffer refreshRate(float from, float to) {
  XrEventDataDisplayRefreshRateChangedFB e{XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB};
  e.fromDisplayRefreshRate = from;
  e.toDisplayRefreshRate = to;
  return wrap(e);
}
XrEventDataBuffer perfSettings(XrPerfSettingsDomainEXT d, XrPerfSettingsSubDomainEXT sd, XrPerfSettingsNotificationLevelEXT from,
                               XrPerfSettingsNotificationLevelEXT to) {
  XrEventDataPerfSettingsEXT e{XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT};
  e.domain = d;
  e.subDomain = sd;
  e.fromLevel = from;
  e.toLevel = to;
  return wrap(e);
}
XrEventDataBuffer unknownEvent(int32_t type) {
  XrEventDataBuffer b{XR_TYPE_EVENT_DATA_BUFFER};
  b.type = static_cast<XrStructureType>(type);
  return b;
}

const double kNaN = std::numeric_limits<double>::quiet_NaN();

std::string note(VendorEventLog& log, const XrEventDataBuffer& e, double lag = kNaN, uint64_t nowMs = 1000) {
  char line[640];
  const size_t n = log.note(e, lag, nowMs, line, sizeof(line));
  return n ? std::string(line) : std::string();
}

// ---- E1 ----------------------------------------------------------------------------------------------------------
void nameCases() {
  char s[24];
  check(std::string(xrSessionStateName(XR_SESSION_STATE_UNKNOWN, s)) == "UNKNOWN" && std::string(xrSessionStateName(XR_SESSION_STATE_IDLE, s)) == "IDLE" &&
            std::string(xrSessionStateName(XR_SESSION_STATE_READY, s)) == "READY" && std::string(xrSessionStateName(XR_SESSION_STATE_SYNCHRONIZED, s)) == "SYNCHRONIZED" &&
            std::string(xrSessionStateName(XR_SESSION_STATE_VISIBLE, s)) == "VISIBLE" && std::string(xrSessionStateName(XR_SESSION_STATE_FOCUSED, s)) == "FOCUSED" &&
            std::string(xrSessionStateName(XR_SESSION_STATE_STOPPING, s)) == "STOPPING" && std::string(xrSessionStateName(XR_SESSION_STATE_LOSS_PENDING, s)) == "LOSS_PENDING" &&
            std::string(xrSessionStateName(XR_SESSION_STATE_EXITING, s)) == "EXITING",
        "E1.states: every session state has its name");
  check(std::string(xrSessionStateName(static_cast<XrSessionState>(77), s)) == "STATE_77", "E1.states: a number this build does not know prints as STATE_<n>");
  check(std::string(xrReferenceSpaceName(XR_REFERENCE_SPACE_TYPE_VIEW, s)) == "VIEW" && std::string(xrReferenceSpaceName(XR_REFERENCE_SPACE_TYPE_LOCAL, s)) == "LOCAL" &&
            std::string(xrReferenceSpaceName(XR_REFERENCE_SPACE_TYPE_STAGE, s)) == "STAGE" && std::string(xrReferenceSpaceName(static_cast<XrReferenceSpaceType>(9), s)) == "SPACE_9",
        "E1.spaces: VIEW, LOCAL, STAGE, and SPACE_<n> for the rest");
  check(std::string(xrPerfDomainName(XR_PERF_SETTINGS_DOMAIN_CPU_EXT, s)) == "CPU" && std::string(xrPerfDomainName(XR_PERF_SETTINGS_DOMAIN_GPU_EXT, s)) == "GPU" &&
            std::string(xrPerfSubDomainName(XR_PERF_SETTINGS_SUB_DOMAIN_COMPOSITING_EXT, s)) == "COMPOSITING" &&
            std::string(xrPerfSubDomainName(XR_PERF_SETTINGS_SUB_DOMAIN_RENDERING_EXT, s)) == "RENDERING" &&
            std::string(xrPerfSubDomainName(XR_PERF_SETTINGS_SUB_DOMAIN_THERMAL_EXT, s)) == "THERMAL" &&
            std::string(xrPerfLevelName(XR_PERF_SETTINGS_NOTIF_LEVEL_NORMAL_EXT, s)) == "NORMAL" && std::string(xrPerfLevelName(XR_PERF_SETTINGS_NOTIF_LEVEL_WARNING_EXT, s)) == "WARNING" &&
            std::string(xrPerfLevelName(XR_PERF_SETTINGS_NOTIF_LEVEL_IMPAIRED_EXT, s)) == "IMPAIRED" && std::string(xrPerfLevelName(static_cast<XrPerfSettingsNotificationLevelEXT>(40), s)) == "LEVEL_40",
        "E1.perf: the performance-settings domain, sub-domain and level names");
}

// ---- E2 ----------------------------------------------------------------------------------------------------------
void decodeCases() {
  VendorEventLog log;
  check(note(log, stateChanged(XR_SESSION_STATE_IDLE, 1000000), 2.5) ==
            "native_xr_event,n=1,type=session_state_changed,from=UNKNOWN,to=IDLE,event_time=1000000,lag_ms=2.500",
        "E2.state: a session state change says from, to, the event's own time and how long ago it was");
  check(note(log, eventsLost(3)) == "native_xr_event,n=2,type=events_lost,lost_count=3", "E2.lost: events lost says how many");
  check(note(log, instanceLoss(5000000000ll)) == "native_xr_event,n=3,type=instance_loss_pending,loss_time=5000000000", "E2.instance: instance loss pending says when");
  check(note(log, referenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, 123456789, true)) ==
            "native_xr_event,n=4,type=reference_space_change_pending,space=LOCAL,change_time=123456789,pose_valid=1",
        "E2.space: a reference space change names the space, when and whether the pose is valid");
  check(note(log, interactionProfile()) == "native_xr_event,n=5,type=interaction_profile_changed", "E2.profile: an interaction profile change");
  VendorEventLog space;
  check(note(space, referenceSpace(XR_REFERENCE_SPACE_TYPE_VIEW, 5, false)) == "native_xr_event,n=1,type=reference_space_change_pending,space=VIEW,change_time=5,pose_valid=0",
        "E2.space: a change whose pose is not valid says so (pose_valid=0), and names the VIEW space");
  check(note(log, visibilityMask(1)) == "native_xr_event,n=6,type=visibility_mask_changed,view_config=2,view=1", "E2.mask: the visibility mask event names its view");
  check(note(log, refreshRate(72.0f, 90.0f)) == "native_xr_event,n=7,type=display_refresh_rate_changed,from_hz=72.000,to_hz=90.000", "E2.rate: a display refresh rate change");
  check(note(log, perfSettings(XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_SUB_DOMAIN_COMPOSITING_EXT, XR_PERF_SETTINGS_NOTIF_LEVEL_NORMAL_EXT,
                               XR_PERF_SETTINGS_NOTIF_LEVEL_WARNING_EXT)) ==
            "native_xr_event,n=8,type=perf_settings,domain=CPU,sub_domain=COMPOSITING,from=NORMAL,to=WARNING",
        "E2.perf: a performance settings event names domain, sub-domain and both levels");
  VendorEventLog other;
  check(note(other, stateChanged(XR_SESSION_STATE_READY, 5)) == "native_xr_event,n=1,type=session_state_changed,from=UNKNOWN,to=READY,event_time=5,lag_ms=unknown",
        "E2.lag: a lag the host could not measure is said to be unknown");
  check(note(other, stateChanged(XR_SESSION_STATE_FOCUSED, 7), 0.0).find("lag_ms=0.000") != std::string::npos, "E2.lag: a lag of zero is zero, not unknown");
  const auto& c = log.counts();
  check(c.received == 8 && c.logged == 8 && c.sessionStateChanged == 1 && c.eventsLost == 1 && c.lostEvents == 3 && c.instanceLossPending == 1 &&
            c.referenceSpaceChangePending == 1 && c.interactionProfileChanged == 1 && c.visibilityMaskChanged == 1 && c.displayRefreshRateChanged == 1 && c.perfSettings == 1,
        "E2.counts: each kind is counted in its own field");
}

// ---- E3 ----------------------------------------------------------------------------------------------------------
void stateCases() {
  VendorEventLog log;
  check(!log.anyState() && log.lastState() == XR_SESSION_STATE_UNKNOWN && log.stateAgeMs(5000) < 0, "E3.start: before any state change the state is UNKNOWN and has no age");
  note(log, stateChanged(XR_SESSION_STATE_IDLE, 10), kNaN, 1000);
  const std::string second = note(log, stateChanged(XR_SESSION_STATE_READY, 20), kNaN, 2000);
  check(contains(second, ",from=IDLE,to=READY,"), "E3.chain: the second change is from the first's state");
  note(log, stateChanged(XR_SESSION_STATE_SYNCHRONIZED, 30), kNaN, 3000);
  const std::string fourth = note(log, stateChanged(XR_SESSION_STATE_FOCUSED, 40), kNaN, 4000);
  check(contains(fourth, ",from=SYNCHRONIZED,to=FOCUSED,") && log.lastState() == XR_SESSION_STATE_FOCUSED && log.anyState(),
        "E3.chain: from -> to follows the events, and the last state is the last event's");
  check(closeTo(log.stateAgeMs(4500), 500.0, 0.001) && closeTo(log.stateAgeMs(4000), 0.0, 0.001) && log.stateAgeMs(3999) < 0,
        "E3.age: the state's age is the time since the host read it, 0 at the moment, unknown for a clock behind it");
  note(log, eventsLost(1), kNaN, 9000);
  check(log.lastState() == XR_SESSION_STATE_FOCUSED && closeTo(log.stateAgeMs(9000), 5000.0, 0.001), "E3.age: another kind of event changes neither the state nor its age");
}

// ---- E4 ----------------------------------------------------------------------------------------------------------
void unknownCases() {
  VendorEventLog log;
  const std::string first = note(log, unknownEvent(1000999000));
  check(first == "native_xr_event,n=1,type=unknown,number=1000999000,first_of_type=1", "E4.once: an undecoded type is named by its number");
  check(note(log, unknownEvent(1000999000)).empty() && note(log, unknownEvent(1000999000)).empty(), "E4.once: and only the first time");
  check(contains(note(log, unknownEvent(1000999001)), "number=1000999001"), "E4.types: another type has its own line");
  for (int i = 0; i < 40; ++i) note(log, unknownEvent(2000000000 + i));
  const auto& c = log.counts();
  check(c.unknownTypes == kVendorUnknownTypesNamed && c.unknownEvents == 44, "E4.limit: no more than 24 types are named, every undecoded event is counted");
  check(note(log, unknownEvent(2000000099)).empty() && note(log, unknownEvent(1000999000)).empty(), "E4.limit: a 25th type gets no line, and an old one still none");
  check(contains(note(log, eventsLost(2)), "type=events_lost"), "E4.limit: a decoded event after the limit is written as ever");
}

// ---- E5 ----------------------------------------------------------------------------------------------------------
void capCases() {
  VendorEventLog log;
  unsigned written = 0, notices = 0;
  for (unsigned i = 0; i < 260; ++i) {
    const std::string line = note(log, eventsLost(1));
    if (line.empty()) continue;
    if (contains(line, "suppressed=1")) ++notices;
    else ++written;
  }
  check(written == kVendorEventLineCap && notices == 1, "E5.cap: 200 decoded events are written, then one notice, then nothing");
  check(log.counts().suppressed == 59 + 1 && log.counts().received == 260 && log.counts().eventsLost == 260 && log.counts().lostEvents == 260,
        "E5.counts: every event past the cap is counted, in its own kind's field and in suppressed (the notice's event too)");
  VendorEventLog withState;
  for (unsigned i = 0; i < 210; ++i) note(withState, eventsLost(1));
  note(withState, stateChanged(XR_SESSION_STATE_STOPPING, 99), kNaN, 5000);
  check(withState.lastState() == XR_SESSION_STATE_STOPPING && closeTo(withState.stateAgeMs(5000), 0.0, 0.001),
        "E5.state: a state change past the cap is not written and is not lost: the last state is still the last");
  check(contains(note(withState, unknownEvent(1000000123)), "first_of_type=1"), "E5.unknown: an undecoded type is still named after the cap, and does not count against it");
  VendorEventLog fresh;
  for (unsigned i = 0; i < 100; ++i) note(fresh, unknownEvent(1000000000 + static_cast<int32_t>(i % 24)));
  unsigned decoded = 0;
  for (unsigned i = 0; i < 205; ++i)
    if (!note(fresh, eventsLost(1)).empty()) ++decoded;
  check(decoded == kVendorEventLineCap + 1, "E5.cap: the 24 undecoded types' lines do not use up the 200: 200 decoded lines and the notice");
}

// ---- E6 ----------------------------------------------------------------------------------------------------------
void summaryCases() {
  VendorEventLog log;
  note(log, stateChanged(XR_SESSION_STATE_FOCUSED, 1), kNaN, 10);
  note(log, eventsLost(4));
  note(log, unknownEvent(1000999000));
  char line[800];
  log.formatSummary(line, sizeof(line), "session_close");
  check(std::string(line) == "native_xr_events_summary,reason=session_close,received=3,logged=3,suppressed=0,session_state_changed=1,events_lost=1,lost_events=4,"
                             "instance_loss_pending=0,reference_space_change_pending=0,interaction_profile_changed=0,visibility_mask_changed=0,"
                             "display_refresh_rate_changed=0,perf_settings=0,unknown_events=1,unknown_types=1,last_state=FOCUSED",
        "E6.summary: the counts of every kind, the last state, and the reason it was written");
  log.formatSummary(line, sizeof(line));
  check(std::string(line).rfind("native_xr_events_summary,received=3,", 0) == 0, "E6.summary: without a reason there is no reason field");
  VendorEventLog empty;
  empty.formatSummary(line, sizeof(line), "periodic");
  check(contains(line, "reason=periodic,received=0,logged=0,suppressed=0,session_state_changed=0") && contains(line, "last_state=UNKNOWN"),
        "E6.summary: a session that saw nothing says zeros, which is the proof the instrument ran");
  VendorEventLog::formatArmed(line, sizeof(line));
  check(std::string(line).rfind("native_xr_events,armed=1,source=xrPollEvent,line_cap=200,unknown_types_named=24,decoded=session_state_changed|events_lost|", 0) == 0 &&
            contains(line, "perf_settings,undecoded=named_once_by_number"),
        "E6.armed: the armed line names its source, its bounds and what it decodes");
  VendorEventLog widest;
  widest.note(stateChanged(static_cast<XrSessionState>(123456), 9223372036854775807ll), 1e15, 1, line, sizeof(line));
  char widestLine[640];
  const size_t n = widest.note(perfSettings(static_cast<XrPerfSettingsDomainEXT>(1234567), static_cast<XrPerfSettingsSubDomainEXT>(1234567),
                                            static_cast<XrPerfSettingsNotificationLevelEXT>(1234567), static_cast<XrPerfSettingsNotificationLevelEXT>(1234567)),
                               0, 1, widestLine, sizeof(widestLine));
  check(n > 0 && n < sizeof(widestLine) - 1 && contains(widestLine, "to=LEVEL_1234567"), "E6.fit: the widest event line fits the 640 bytes the host gives it");
}

// ---- E7 ----------------------------------------------------------------------------------------------------------
struct PollScript {
  std::vector<XrEventDataBuffer> queue;
  unsigned next = 0;
  static PollScript* cur;
  static XrResult poll(XrInstance, XrEventDataBuffer* out) {
    if (cur->next >= cur->queue.size()) {
      *out = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
      return XR_EVENT_UNAVAILABLE;
    }
    *out = cur->queue[cur->next++];
    return XR_SUCCESS;
  }
  static XrResult beginSession(XrSession, const XrSessionBeginInfo*) { return XR_SUCCESS; }
  static XrResult endSession(XrSession) { return XR_SUCCESS; }
  static XrResult wait(XrSession, const XrFrameWaitInfo*, XrFrameState* out) {
    *out = XrFrameState{XR_TYPE_FRAME_STATE};
    return XR_SUCCESS;
  }
  static XrResult beginFrame(XrSession, const XrFrameBeginInfo*) { return XR_SUCCESS; }
  static XrResult endFrame(XrSession, const XrFrameEndInfo*) { return XR_SUCCESS; }
  Dispatch dispatch() {
    cur = this;
    return {poll, beginSession, endSession, wait, beginFrame, endFrame};
  }
};
PollScript* PollScript::cur = nullptr;

struct Seen {
  std::vector<int32_t> types;
  static void observe(const XrEventDataBuffer& e, void* context) noexcept { static_cast<Seen*>(context)->types.push_back(static_cast<int32_t>(e.type)); }
};

void observerCases() {
  PollScript script;
  const XrSession mine = reinterpret_cast<XrSession>(2);
  script.queue = {interactionProfile(), stateChanged(XR_SESSION_STATE_IDLE, 1), eventsLost(2), stateChanged(XR_SESSION_STATE_FOCUSED, 5, reinterpret_cast<XrSession>(77)),
                  stateChanged(XR_SESSION_STATE_READY, 9, mine)};
  SessionState state;
  check(state.reset(script.dispatch(), reinterpret_cast<XrInstance>(1), mine, XR_ENVIRONMENT_BLEND_MODE_OPAQUE) == XR_SUCCESS, "E7.setup: the session binds");
  Seen seen;
  state.setEventObserver(Seen::observe, &seen);
  const XrResult polled = state.pollEvents();
  check(polled == XR_SUCCESS && seen.types.size() == 5 && seen.types[0] == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED &&
            seen.types[1] == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED && seen.types[2] == XR_TYPE_EVENT_DATA_EVENTS_LOST && seen.types[3] == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED &&
            seen.types[4] == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED,
        "E7.sees-all: the observer sees every event the vendor sent, the ones the policy handles, the ones it passes on, and another session's");
  check(state.lifecycle() == Lifecycle::Ready && state.startIfReady() == XR_SUCCESS && state.running(), "E7.unchanged: the policy handled the events exactly as before: READY, and the session starts");

  PollScript loss;
  loss.queue = {instanceLoss(77), eventsLost(1)};
  SessionState lossy;
  lossy.reset(loss.dispatch(), reinterpret_cast<XrInstance>(1), mine, XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
  Seen lossSeen;
  lossy.setEventObserver(Seen::observe, &lossSeen);
  lossy.pollEvents();
  check(lossSeen.types.size() == 1 && lossSeen.types[0] == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING && lossy.lifecycle() == Lifecycle::InstanceLossPending && lossy.terminal(),
        "E7.instance-loss: an instance loss pending is observed before the policy goes terminal on it");

  PollScript quiet;
  SessionState idle;
  idle.reset(quiet.dispatch(), reinterpret_cast<XrInstance>(1), mine, XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
  Seen none;
  idle.setEventObserver(Seen::observe, &none);
  idle.pollEvents();
  check(none.types.empty(), "E7.quiet: no event, no observation (XR_EVENT_UNAVAILABLE is not an event)");

  PollScript again;
  again.queue = {eventsLost(1)};
  SessionState rebound;
  rebound.reset(again.dispatch(), reinterpret_cast<XrInstance>(1), mine, XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
  Seen after;
  rebound.setEventObserver(Seen::observe, &after);
  rebound.reset(again.dispatch(), reinterpret_cast<XrInstance>(1), mine, XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
  rebound.pollEvents();
  check(after.types.empty(), "E7.reset: a reset clears the observer with the rest, like the unhandled-event sink");
}

// ---- P: episodes -------------------------------------------------------------------------------------------------
constexpr double kPeriod = 13.889;   // 72 Hz

EndFrameCall slowCall(uint64_t nowMs, double ms = 83.1, uint64_t sequence = 96731) {
  EndFrameCall c;
  c.sequence = sequence;
  c.ms = ms;
  c.periodMs = kPeriod;
  c.nowMs = nowMs;
  c.shouldRender = true;
  c.layers = true;
  c.sessionState = "FOCUSED";
  c.stateAgeMs = 12345.6;
  return c;
}

EndFrameCall normalCall(uint64_t nowMs, uint64_t sequence = 0) {
  EndFrameCall c = slowCall(nowMs, 1.2, sequence);
  return c;
}

struct EpisodeRig {
  EndFrameEpisodes episodes;
  EndFrameEpisodes::Lines lines;
  std::vector<std::string> starts, ends;
  void feed(const EndFrameCall& c) {
    episodes.observe(c, lines);
    if (lines.startLen) starts.push_back(lines.start);
    if (lines.endLen) ends.push_back(lines.end);
  }
  void finish() {
    episodes.finish(lines);
    if (lines.startLen) starts.push_back(lines.start);
    if (lines.endLen) ends.push_back(lines.end);
  }
};

void thresholdCases() {
  check(kEndFramePeriods == 3.0 && kEndFrameEndAfterNormal == 8 && kEndFrameStartLineEveryMs == 2000 && kEndFrameStartLineCap == 100 && kEndFrameEndLineMinSlow == 20,
        "P1.constants: 3 periods, 8 normal calls to end, a start line every 2 s up to 100, a long episode from 20 slow calls");
  EpisodeRig r;
  r.feed(slowCall(1000, 3.0 * kPeriod - 0.01));
  check(!r.episodes.open() && r.episodes.summary().slowCalls == 0, "P1.threshold: 2.99 periods is not an episode");
  r.feed(slowCall(2000, 3.0 * kPeriod));
  check(r.episodes.open() && r.episodes.summary().episodes == 1 && r.starts.size() == 1, "P1.threshold: exactly 3 periods is (3 or more)");
  EpisodeRig noPeriod;
  EndFrameCall c = slowCall(1000, 500.0);
  c.periodMs = 0;
  noPeriod.feed(c);
  check(!noPeriod.episodes.open() && noPeriod.starts.empty() && noPeriod.episodes.summary().calls == 1,
        "P1.period: with no period to compare with nothing is an episode, however long the call (and it is still counted)");
  EpisodeRig nan;
  nan.feed(slowCall(1000, kNaN));
  check(!nan.episodes.open(), "P1.finite: a call whose time is not a number starts nothing");
  EpisodeRig big;
  big.feed(slowCall(1000, 83.0));
  big.feed(slowCall(1100, 2.0 * kPeriod));
  check(big.episodes.summary().slowCalls == 1, "P1.slow: a call under the threshold is not counted slow even inside an episode");
}

void startLineCases() {
  EpisodeRig r;
  EndFrameCall c = slowCall(1000000, 83.1, 96731);
  c.acquireMs = 0.0027;
  c.waitMs = 0.0007;
  c.drawMs = 0.0794;
  c.releaseMs = 0.0012;
  c.copyMs = 0.33;
  r.feed(c);
  check(r.starts.size() == 1 &&
            lineAt(r.starts, 0) == "native_end_frame_episode,episode=1,sequence=96731,ms=83.1000,periods=5.98,period_ms=13.8890,should_render=1,layers=1,path=synchronous,"
                           "pacing=runtime,result=0,xr_acquire_ms=0.0027,xr_wait_ms=0.0007,xr_draw_ms=0.0794,xr_release_ms=0.0012,copy_ms=0.3300,"
                           "session_state=FOCUSED,state_age_ms=12345.6,units=wall_ms",
        "P2.line: the first call of the episode: sequence, ms, periods, the period, the vendor's answer, the frame's own swapchain and copy times, the session state");
  EpisodeRig o;
  EndFrameCall overlapped = slowCall(1000000);
  overlapped.overlapped = true;
  overlapped.turbo = true;
  o.feed(overlapped);
  check(contains(lineAt(o.starts, 0), ",path=overlapped,pacing=deferred,"), "P2.path: the overlapped path and deferred pacing are named");
  EpisodeRig l;
  EndFrameCall loading = slowCall(1000000);
  loading.background = true;
  loading.overlapped = true;   // a loading frame is the loading path whatever else is set
  loading.shouldRender = false;
  loading.layers = false;
  l.feed(loading);
  check(contains(lineAt(l.starts, 0), ",should_render=0,layers=0,path=loading,"), "P2.path: a loading frame is the loading path, and the vendor's no-render answer is shown");
  EpisodeRig u;
  EndFrameCall unnamed = slowCall(1000000);
  unnamed.sessionState = nullptr;
  unnamed.stateAgeMs = -1;
  u.feed(unnamed);
  check(contains(lineAt(u.starts, 0), ",session_state=UNKNOWN,state_age_ms=-1.0,"), "P2.state: a session state nobody has told us of is UNKNOWN with an age of -1");
  EndFrameCall failed = slowCall(1000000);
  failed.result = -2;
  EpisodeRig f;
  f.feed(failed);
  check(contains(lineAt(f.starts, 0), ",result=-2,"), "P2.result: the call's XrResult is on the line");
}

void lifeCases() {
  EpisodeRig r;
  for (unsigned i = 0; i < 10; ++i) r.feed(slowCall(10000 + 100ull * (i + 1), 83.0, 500 + i));
  check(r.episodes.open() && r.ends.empty(), "P3.open: while the calls stay slow the episode stays open");
  for (unsigned i = 0; i < 7; ++i) r.feed(normalCall(11000 + 14ull * i));
  check(r.episodes.open() && r.ends.empty(), "P3.run: seven normal calls in a row do not end it");
  r.feed(slowCall(11200, 90.0, 600));
  for (unsigned i = 0; i < 7; ++i) r.feed(normalCall(11300 + 14ull * i));
  check(r.episodes.open(), "P3.run: a slow call among them restarts the count");
  r.feed(normalCall(11500));
  check(!r.episodes.open() && r.ends.size() == 1, "P3.end: the eighth normal call in a row ends it");
  check(contains(lineAt(r.ends, 0), "native_end_frame_episode_end,episode=1,reason=normal_calls,start_line=1,calls=18,slow_calls=11,") &&
            contains(lineAt(r.ends, 0), ",first_sequence=500,last_sequence=600,units=wall_ms"),
        "P3.counts: calls is the episode's (the ones ending it are not), slow_calls the slow ones, and the sequences span it");
  check(contains(lineAt(r.ends, 0), "duration_ms=1183.0,"), "P3.duration: from the first slow call's start (10100 - 83) to the last slow call's end (11200)");
  r.feed(slowCall(20000, 100.0, 700));
  check(r.episodes.open() && r.episodes.summary().episodes == 2, "P3.again: the next slow call is a new episode");
  check(r.starts.size() == 2 && contains(lineAt(r.starts, 1), "episode=2,"), "P3.again: with its own number");
  EpisodeRig sparse;
  sparse.feed(slowCall(1000));
  for (unsigned i = 0; i < 3; ++i) {
    sparse.feed(normalCall(1100 + 14ull * i));
    sparse.feed(slowCall(1200 + 100ull * i));
  }
  check(sparse.episodes.open() && sparse.ends.empty(), "P3.sparse: a vendor that is slow every other call is one episode, not many");
}

void percentileCases() {
  EpisodeRig r;
  const double v[] = {50, 60, 70, 80, 90};
  for (unsigned i = 0; i < 5; ++i) r.feed(slowCall(1000 + 100ull * i, v[i]));
  r.finish();
  check(r.ends.size() == 1 && contains(lineAt(r.ends, 0), "p50_ms=70.25,max_ms=90.0000,mean_ms=70.0000,"), "P4.p50: the median of five is the third, to the histogram's half millisecond; the max and the mean are exact");
  EpisodeRig even;
  const double w[] = {50, 60, 70, 80};
  for (unsigned i = 0; i < 4; ++i) even.feed(slowCall(1000 + 100ull * i, w[i]));
  even.finish();
  check(contains(lineAt(even.ends, 0), "p50_ms=60.25,"), "P4.p50: of four, the lower middle (rank (n+1)/2)");
  EpisodeRig huge;
  huge.feed(slowCall(1000, 100.0));
  huge.feed(slowCall(2000, 100.0));
  huge.feed(slowCall(3000, 600.0));
  huge.finish();
  check(contains(lineAt(huge.ends, 0), "p50_ms=100.25,max_ms=600.0000,"), "P4.clamp: a call over 512 ms is in the last bin and does not move the median; the max is its own");
  EpisodeRig all;
  for (unsigned i = 0; i < 3; ++i) all.feed(slowCall(1000 + 100ull * i, 700.0));
  all.finish();
  check(contains(lineAt(all.ends, 0), "p50_ms=511.75,max_ms=700.0000,"), "P4.clamp: when every call is past the last bin the p50 is that bin's middle and the max is still exact");
  EpisodeRig mixed;
  mixed.feed(slowCall(1000, 50.0));
  for (unsigned k = 0; k < 3; ++k) mixed.feed(normalCall(1100 + k));
  mixed.feed(slowCall(1200, 70.0));
  mixed.finish();
  check(contains(lineAt(mixed.ends, 0), ",calls=5,slow_calls=2,") && contains(lineAt(mixed.ends, 0), "mean_ms=60.0000,"),
        "P4.mean: the mean is over the slow calls (50 and 70 is 60) however many normal calls were among them");
  EpisodeRig one;
  one.feed(slowCall(1000, 83.1));
  one.finish();
  check(contains(lineAt(one.ends, 0), "slow_calls=1,") && contains(lineAt(one.ends, 0), "p50_ms=83.25,max_ms=83.1000,mean_ms=83.1000,period_ms=13.8890,max_periods=5.98,"), "P4.single: one call is its own p50, max and mean");
}

void limiterCases() {
  EpisodeRig r;
  for (unsigned i = 0; i < 10; ++i) {
    r.feed(slowCall(1000 + 500ull * i));
    for (unsigned k = 0; k < 8; ++k) r.feed(normalCall(1000 + 500ull * i + 100 + k));
  }
  check(r.episodes.summary().episodes == 10 && r.starts.size() == 3 && r.ends.size() == 3,
        "P5.rate: ten one-call episodes half a second apart: a start line at most every 2 s, so one in four (three), and an end line only for those");
  check(r.episodes.summary().startLines == 3 && r.episodes.summary().endLines == 3, "P5.rate: and the summary counts the lines written");
  EpisodeRig cap;
  for (unsigned i = 0; i < 150; ++i) {
    cap.feed(slowCall(10000 + 3000ull * i));
    for (unsigned k = 0; k < 8; ++k) cap.feed(normalCall(10000 + 3000ull * i + 100 + k));
  }
  check(cap.episodes.summary().episodes == 150 && cap.starts.size() == 100 && cap.ends.size() == 100,
        "P5.cap: a hundred start lines a session, and the episodes past it are counted");
  EpisodeRig quiet;
  quiet.feed(slowCall(1000));
  for (unsigned k = 0; k < 8; ++k) quiet.feed(normalCall(1100 + k));
  quiet.feed(slowCall(1500));   // 500 ms after the last start line: held back
  for (unsigned i = 0; i < 24; ++i) quiet.feed(slowCall(1600 + 100ull * i));
  for (unsigned k = 0; k < 8; ++k) quiet.feed(normalCall(4100 + k));
  check(quiet.starts.size() == 1 && quiet.ends.size() == 2 && contains(lineAt(quiet.ends, 1), "start_line=0,") && contains(lineAt(quiet.ends, 1), "slow_calls=25,"),
        "P5.long: an episode whose start line was held back still gets its end line when it was long (20 slow calls or more), saying it had no start line");
  EpisodeRig brief;
  brief.feed(slowCall(1000));
  for (unsigned k = 0; k < 8; ++k) brief.feed(normalCall(1100 + k));
  brief.feed(slowCall(1500));
  for (unsigned i = 0; i < 18; ++i) brief.feed(slowCall(1600 + 100ull * i));
  for (unsigned k = 0; k < 8; ++k) brief.feed(normalCall(3600 + k));
  check(brief.ends.size() == 1, "P5.long: and 19 slow calls with no start line are not (the line is for the long ones)");
}

void closeCases() {
  EpisodeRig r;
  r.finish();
  check(r.starts.empty() && r.ends.empty(), "P6.close: nothing open, nothing written");
  r.feed(slowCall(1000));
  r.feed(slowCall(1100));
  for (unsigned k = 0; k < 3; ++k) r.feed(normalCall(1200 + k));
  r.finish();
  check(r.ends.size() == 1 && contains(lineAt(r.ends, 0), "reason=session_close,") && contains(lineAt(r.ends, 0), "calls=2,slow_calls=2,") && !r.episodes.open(),
        "P6.close: an episode still open at close ends there, without the normal calls after its last slow one");
  char line[800];
  r.episodes.formatSummary(line, sizeof(line), "session_close");
  check(std::string(line) == "native_end_frame_episodes_summary,reason=session_close,threshold_periods=3.0,episodes=1,start_lines=1,end_lines=1,calls=5,slow_calls=2,"
                             "longest_ms=83.1000,open=0,units=wall_ms",
        "P6.summary: the episodes, lines and calls, the longest call, whether one is open");
  r.episodes.formatSummary(line, sizeof(line), nullptr);
  check(std::string(line).rfind("native_end_frame_episodes_summary,threshold_periods=3.0,", 0) == 0, "P6.summary: without a reason there is no reason field");
  EndFrameEpisodes::formatArmed(line, sizeof(line));
  check(std::string(line) == "native_end_frame_episodes,armed=1,threshold_periods=3.0,period=last_real_predicted_display_period,end_after_normal_calls=8,"
                             "start_lines=1_per_2000_ms_max_100,long_episode_end_line_from_slow_calls=20,paths=synchronous|overlapped|loading,units=wall_ms",
        "P6.armed: the armed line says the threshold, the end rule, the rate limit and the paths covered");
  EpisodeRig widest;
  EndFrameCall w = slowCall(18446744073709551615ull, 123456789.1234, 18446744073709551615ull);
  w.acquireMs = w.waitMs = w.drawMs = w.releaseMs = w.copyMs = 123456789.1234;
  w.stateAgeMs = 123456789012.3;
  w.result = -2147483647;
  widest.feed(w);
  check(widest.lines.startLen > 0 && widest.lines.startLen < sizeof(widest.lines.start) - 1 && contains(lineAt(widest.starts, 0), "units=wall_ms"),
        "P6.fit: the widest start line fits its buffer, tail intact");
  widest.finish();
  check(widest.lines.endLen > 0 && widest.lines.endLen < sizeof(widest.lines.end) - 1 && contains(lineAt(widest.ends, 0), "units=wall_ms"), "P6.fit: and the widest end line");
}

// ---- P7, P8: FrameBoundary ---------------------------------------------------------------------------------------
struct Runtime {
  static Runtime* cur;
  std::atomic<unsigned> waits{0}, begins{0}, ends{0};
  // The vendor's wait can be made to stay inside the call (the pacer thread's), so a deferred frame is synthesized for certain.
  std::atomic<bool> block{false}, entered{false}, released{false};
  XrTime time = 1000;
  XrDuration period = 13888889;
  bool shouldRender = true;
  bool ready = true;
  static XrResult poll(XrInstance, XrEventDataBuffer* out) {
    if (!cur->ready) {
      *out = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
      return XR_EVENT_UNAVAILABLE;
    }
    cur->ready = false;
    XrEventDataSessionStateChanged e{XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
    e.session = reinterpret_cast<XrSession>(2);
    e.state = XR_SESSION_STATE_READY;
    std::memcpy(out, &e, sizeof(e));
    return XR_SUCCESS;
  }
  static XrResult beginSession(XrSession, const XrSessionBeginInfo*) { return XR_SUCCESS; }
  static XrResult endSession(XrSession) { return XR_SUCCESS; }
  static XrResult wait(XrSession, const XrFrameWaitInfo*, XrFrameState* out) {
    ++cur->waits;
    if (cur->block.load()) {
      cur->entered = true;
      for (int i = 0; i < 2000 && !cur->released.load(); ++i) Sleep(1);
    }
    *out = XrFrameState{XR_TYPE_FRAME_STATE};
    out->predictedDisplayTime = cur->time;
    cur->time += cur->period;
    out->predictedDisplayPeriod = cur->period;
    out->shouldRender = cur->shouldRender ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
  }
  static XrResult beginFrame(XrSession, const XrFrameBeginInfo*) {
    ++cur->begins;
    return XR_SUCCESS;
  }
  static XrResult endFrame(XrSession, const XrFrameEndInfo*) {
    ++cur->ends;
    return XR_SUCCESS;
  }
  Dispatch dispatch() {
    cur = this;
    return {poll, beginSession, endSession, wait, beginFrame, endFrame};
  }
};
Runtime* Runtime::cur = nullptr;

struct HookSink : FrameSink {
  unsigned holds = 0;
  DWORD holdMs = 0;
  std::vector<EndFrameInfo> reported;
  vr::EVRCompositorError capture(vr::EVREye, const vr::Texture_t*, const vr::VRTextureBounds_t*, vr::EVRSubmitFlags, bool) override { return vr::VRCompositorError_None; }
  XrResult compose(XrCompositionLayerProjection& layer) override {
    layer.space = reinterpret_cast<XrSpace>(3);
    layer.viewCount = 2;
    static XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    layer.views = views;
    return XR_SUCCESS;
  }
  XrResult composeBackground(XrCompositionLayerProjection& layer) override { return compose(layer); }
  void holdEndFrame() override {
    ++holds;
    if (holdMs) Sleep(holdMs);
  }
  void endFrameReturned(const EndFrameInfo& info) override { reported.push_back(info); }
};

bool startSession(SessionState& session, Runtime& runtime) {
  if (session.reset(runtime.dispatch(), reinterpret_cast<XrInstance>(1), reinterpret_cast<XrSession>(2), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) != XR_SUCCESS) return false;
  return session.pollEvents() == XR_SUCCESS && session.startIfReady() == XR_SUCCESS;
}

void pair(FrameBoundary& boundary) {
  vr::Texture_t texture{};
  boundary.setGeometryReady(true);
  boundary.submit(vr::Eye_Right, &texture);
  boundary.submit(vr::Eye_Left, &texture);
}

void hookCases() {
  Runtime runtime;
  SessionState session;
  check(startSession(session, runtime), "P7.setup: the session starts");
  HookSink sink;
  FrameBoundary boundary(session, sink);
  check(boundary.waitAndBegin() == XR_SUCCESS, "P7.setup: a frame begins");
  pair(boundary);
  check(sink.reported.size() == 1 && runtime.ends == 1, "P7.once: a pair that ends the frame reports its xrEndFrame once");
  const EndFrameInfo first = sink.reported.empty() ? EndFrameInfo{} : infoAt(sink.reported, 0);
  check(first.layers && first.shouldRender && !first.background && !first.turbo && first.sequence == 1 && first.result == XR_SUCCESS && first.periodNs == 13888889 && first.ms >= 0.0,
        "P7.fields: the report carries the frame's sequence, the vendor's answer, the layers, the pacing, the period and the result");

  runtime.shouldRender = false;
  boundary.waitAndBegin();
  pair(boundary);
  check(sink.reported.size() == 2 && runtime.ends == 2 && !infoAt(sink.reported, 1).layers && !infoAt(sink.reported, 1).shouldRender, "P7.no-render: a frame the vendor said not to render ends empty, and is reported as such");

  runtime.shouldRender = true;
  boundary.waitAndBegin();
  vr::Texture_t texture{};
  boundary.setGeometryReady(true);
  boundary.submit(vr::Eye_Left, &texture);
  boundary.waitAndBegin();   // closes the half-submitted frame empty first
  check(sink.reported.size() == 3 && runtime.ends == 3 && !infoAt(sink.reported, 2).layers && infoAt(sink.reported, 2).shouldRender && infoAt(sink.reported, 2).sequence == 3,
        "P7.clear: the empty end of a frame the game never finished is reported, with the frame's own sequence");

  pair(boundary);
  boundary.waitAndBegin();
  boundary.setGeometryReady(true);
  const XrResult loaded = boundary.background();
  check(loaded == XR_SUCCESS && sink.reported.size() == 5 && runtime.ends == 5 && infoAt(sink.reported, sink.reported.size() ? sink.reported.size() - 1 : 0).background && infoAt(sink.reported, sink.reported.size() ? sink.reported.size() - 1 : 0).layers,
        "P7.loading: a loading frame's end is reported, as one");

  boundary.waitAndBegin();
  boundary.clear();
  check(sink.reported.size() == runtime.ends, "P7.every: every xrEndFrame the runtime saw was reported once (clear included)");

  // The hold: inside the timed region, and only there.
  HookSink holdSink;
  holdSink.holdMs = 30;
  Runtime r2;
  SessionState s2;
  startSession(s2, r2);
  FrameBoundary b2(s2, holdSink);
  b2.waitAndBegin();
  pair(b2);
  check(holdSink.holds == 1 && b2.endFrameMs() >= 25.0 && holdSink.reported.size() == 1 && infoAt(holdSink.reported, 0).ms >= 25.0 && closeTo(infoAt(holdSink.reported, 0).ms, b2.endFrameMs(), 0.001),
        "P8.inside: the hold is inside the timed region: xr_end_frame and the report both include it, and say the same");
  b2.waitAndBegin();
  vr::Texture_t tex{};
  b2.submit(vr::Eye_Left, &tex);
  b2.clear();
  check(holdSink.holds == 1 && holdSink.reported.size() == 2, "P8.only: the empty end of clear() is not held (the hold ran once, for the first pair)");
  b2.waitAndBegin();
  b2.setGeometryReady(true);
  b2.background();
  check(holdSink.holds == 2 && holdSink.reported.size() == 3 && infoAt(holdSink.reported, holdSink.reported.size() ? holdSink.reported.size() - 1 : 0).ms >= 25.0, "P8.loading: a loading frame's end is held too (finish() is one path)");

  // Deferred (turbo) pacing: the synthesized frame and the pacer's block.
  Runtime r3;
  SessionState s3;
  startSession(s3, r3);
  HookSink turboSink;
  FramePacer pacer;
  pacer.priorityHigh = false;
  pacer.bind(Runtime::wait, reinterpret_cast<XrSession>(2));
  FrameBoundary b3(s3, turboSink, &pacer);
  b3.waitAndBegin(FramePacing::Deferred);   // no real prediction yet: the blocking wait
  r3.block = true;                           // the pacer's next wait stays inside the vendor's call
  pair(b3);                                  // finish() kicks it
  for (int i = 0; i < 2000 && !r3.entered.load(); ++i) Sleep(1);
  b3.waitAndBegin(FramePacing::Deferred);   // a synthesized frame: the real wait is on the pacer thread
  const bool synthesized = b3.deferred();
  r3.block = false;
  r3.released = true;
  pair(b3);                                  // finish() takes the pacer's result, begins, ends
  check(synthesized && turboSink.reported.size() == 2 && infoAt(turboSink.reported, 0).turbo && infoAt(turboSink.reported, 1).turbo && infoAt(turboSink.reported, 1).layers && r3.ends == 2,
        "P7.turbo: both frames of a deferred-pacing session are reported with the pacing, the synthesized one included");
  const unsigned before = r3.ends;
  b3.drain();   // the pacer's last kicked wait is begun and ended empty
  check(r3.ends == before + 1 && turboSink.reported.size() == before + 1 && !infoAt(turboSink.reported, turboSink.reported.size() ? turboSink.reported.size() - 1 : 0).layers, "P7.drain: the empty end drain() makes is reported");
  pacer.stop();
}

// ---- S: the slow regime ------------------------------------------------------------------------------------------
struct SlowRig {
  SlowRegime regime;
  std::vector<SlowReport> reports;
  uint64_t now = 1000000;
  // Marks a frame as the runtime's loading frame or an empty end, by its index since the rig began (null: every frame is a scene frame).
  std::function<void(SlowFrame&, uint64_t)> tag;
  uint64_t index = 0;
  void frame(double hz, double end, double wait = 0, double swap = 0, double copy = 0, double edvr = 0, double period = 13.889) {
    SlowFrame f;
    f.nowMs = now;
    f.refHz = hz;
    f.periodMs = period;
    f.vendorEndMs = end;
    f.vendorWaitMs = wait;
    f.vendorSwapMs = swap;
    f.copyMs = copy;
    f.edvrMs = edvr;
    if (tag) tag(f, index);
    ++index;
    regime.observe(f, [&](const SlowReport& r) { reports.push_back(r); });
  }
  // `seconds` of frames at `fps` (evenly spaced), the same owner figures in each.
  void run(unsigned seconds, unsigned fps, double hz, double end, double wait = 0, double swap = 0, double copy = 0, double edvr = 0, double period = 13.889) {
    const double step = 1000.0 / fps;
    const uint64_t origin = now;
    for (unsigned s = 0; s < seconds; ++s)
      for (unsigned k = 0; k < fps; ++k) {
        now = origin + static_cast<uint64_t>(1000.0 * s + step * k + 0.5);
        frame(hz, end, wait, swap, copy, edvr, period);
      }
    now = origin + 1000ull * seconds;
  }
  unsigned count(SlowEvent e) const {
    unsigned n = 0;
    for (const SlowReport& r : reports)
      if (r.event == e) ++n;
    return n;
  }
};

void judgeCases() {
  {
    SlowRig r;
    r.run(8, 28, 72.0, 5.0);   // 28 frames a second against 28.8: under 40%
    r.frame(72.0, 5.0);
    check(r.count(SlowEvent::Slow) == 1, "S1.under: 28 frames in a second at 72 Hz is under 40% (28.8)");
  }
  {
    SlowRig r;
    r.run(8, 29, 72.0, 5.0);
    r.frame(72.0, 5.0);
    check(r.count(SlowEvent::Slow) == 0 && !r.regime.inRegime(), "S1.over: 29 frames a second is not");
  }
  {
    SlowRig r;
    r.run(8, 36, 90.0, 5.0);   // exactly 40% of 90 Hz
    r.frame(90.0, 5.0);
    check(r.count(SlowEvent::Slow) == 0 && !r.regime.inRegime() && r.regime.slowSeconds() == 0, "S1.edge: exactly 40% of the display rate (36 of 90) is not under it");
    SlowRig under;
    under.run(8, 35, 90.0, 5.0);
    under.frame(90.0, 5.0);
    check(under.count(SlowEvent::Slow) == 1, "S1.edge: 35 of 90 is");
  }
  {
    SlowRig r;
    r.run(8, 10, 0.0, 83.0, 0, 0, 0, 0, 13.889);   // the vendor reported no rate: judged by the period (72 Hz)
    r.frame(0.0, 83.0);
    check(r.count(SlowEvent::Slow) == 1 && closeTo(reportAt(r.reports, 0).refHz, 1000.0 / 13.889, 0.001), "S1.period: with no reported rate the display's is taken from the predicted period");
  }
  {
    SlowRig r;
    SlowFrame f;
    f.nowMs = 1000;
    for (unsigned i = 0; i < 80; ++i) {
      f.nowMs = 1000 + 1000ull * i;
      r.regime.observe(f, [&](const SlowReport& rep) { r.reports.push_back(rep); });
    }
    check(r.reports.empty() && !r.regime.inRegime(), "S1.unknown: with neither a rate nor a period there is nothing to judge by, so nothing is slow");
  }
  {
    SlowRig r;
    r.run(8, 10, 90.0, 5.0, 0, 0, 0, 0, 100.0);   // a vendor rate of 90 beats a 100 ms period
    r.frame(90.0, 5.0, 0, 0, 0, 0, 100.0);
    check(r.count(SlowEvent::Slow) == 1 && closeTo(reportAt(r.reports, 0).refHz, 90.0, 0.001), "S1.reported: the vendor's reported rate is preferred to the period");
  }
}

void halfRateCases() {
  SlowRig half;
  half.run(60, 36, 72.0, 5.0);   // the vendor's reprojection mode: half the rate
  half.frame(72.0, 5.0);
  check(half.reports.empty() && !half.regime.inRegime() && half.regime.slowSeconds() == 0, "S2.half-rate: 36 fps at 72 Hz for a minute is the vendor's reprojection mode, not a slow regime");
  SlowRig slow;
  slow.run(60, 10, 72.0, 83.1);
  slow.frame(72.0, 83.1);
  check(slow.count(SlowEvent::Slow) == 1 && slow.regime.inRegime(), "S2.slow: 10 fps at 72 Hz for a minute is");
  SlowRig predicted;
  predicted.run(60, 36, 0.0, 5.0, 0, 0, 0, 0, 27.78);   // no reported rate and the period itself halved: 36 fps of 36 Hz
  predicted.frame(0.0, 5.0, 0, 0, 0, 0, 27.78);
  check(predicted.reports.empty(), "S2.predicted: with no reported rate and the predicted period itself at half, the display is taken to be at 36 and 36 fps is all of it");
}

void beginCases() {
  SlowRig r;
  r.run(4, 10, 72.0, 83.1, 0.0, 0.04, 0.4, 0.3);
  r.frame(72.0, 83.1, 0.0, 0.04, 0.4, 0.3);
  check(r.reports.empty() && !r.regime.inRegime(), "S3.hold: four slow seconds are not yet a regime");
  SlowRig s;
  s.run(5, 10, 72.0, 83.1, 0.0, 0.04, 0.4, 0.3);
  s.frame(72.0, 83.1, 0.0, 0.04, 0.4, 0.3);   // the frame that closes the fifth second
  check(s.reports.size() == 1 && reportAt(s.reports, 0).event == SlowEvent::Slow && s.regime.inRegime() && s.regime.regimes() == 1, "S3.begin: the fifth slow second begins the regime, with one report");
  const SlowReport& rep = reportAt(s.reports, 0);
  check(rep.regime == 1 && closeTo(rep.durationS, 5.0, 1e-9) && closeTo(rep.windowS, 5.0, 1e-9) && rep.frames == 50 && closeTo(rep.fps, 10.0, 1e-9) && closeTo(rep.refHz, 72.0, 1e-9) &&
            closeTo(rep.fraction, 10.0 / 72.0, 1e-9) && closeTo(rep.frameMs, 100.0, 1e-9),
        "S3.report: five seconds, 50 frames, 10 fps of 72 Hz, a 100 ms frame");
  check(closeTo(rep.vendorEndMs, 83.1, 1e-9) && closeTo(rep.vendorWaitMs, 0.0, 1e-9) && closeTo(rep.vendorSwapMs, 0.04, 1e-9) && closeTo(rep.copyMs, 0.4, 1e-9) && closeTo(rep.edvrMs, 0.3, 1e-9) &&
            closeTo(rep.gameMs, 100.0 - 83.1 - 0.04 - 0.4 - 0.3, 1e-9) && closeTo(rep.endMaxMs, 83.1, 1e-9),
        "S3.report: the means by owner, and the game's the rest of the frame");
  check(rep.owner == SlowOwner::VendorEndFrame && closeTo(rep.ownerShare, 0.831, 1e-9) && closeTo(rep.vendorShare, 0.8314, 1e-9), "S3.owner: held by the vendor's xrEndFrame at 83% of the frame");
  s.frame(72.0, 83.1);
  check(s.reports.size() == 1, "S3.once: the same second does not report again");
}

void stillSlowCases() {
  SlowRig r;
  r.run(5, 10, 72.0, 83.1);
  r.run(34, 10, 72.0, 40.0, 0, 0, 0, 0);    // the next 34 s: the vendor's end frame is shorter (a different window)
  r.frame(72.0, 40.0);
  check(r.count(SlowEvent::StillSlow) == 1 && r.count(SlowEvent::Slow) == 1, "S4.cadence: 30 s after the SLOW report, one still_slow report");
  const SlowReport* still = nullptr;
  for (const SlowReport& rep : r.reports)
    if (rep.event == SlowEvent::StillSlow) still = &rep;
  check(still && closeTo(still->windowS, 30.0, 1e-9) && still->frames == 300 && closeTo(still->vendorEndMs, 40.0, 1e-9) && closeTo(still->durationS, 35.0, 1e-9) && still->regime == 1,
        "S4.window: it covers the last 30 s only (not the first five), and says how long the regime has lasted");
  r.run(30, 10, 72.0, 40.0);
  r.frame(72.0, 40.0);
  check(r.count(SlowEvent::StillSlow) == 2, "S4.again: and another 30 s after that");
}

void endCases() {
  SlowRig r;
  r.run(40, 10, 72.0, 83.1);
  r.run(1, 72, 72.0, 5.0);   // one normal second
  r.run(10, 10, 72.0, 83.1);   // slow again
  r.frame(72.0, 83.1);
  check(r.count(SlowEvent::End) == 0 && r.regime.inRegime(), "S5.one: one normal second does not end the regime");
  SlowRig e;
  e.run(40, 10, 72.0, 83.1, 0, 0.04, 0.4, 0.3);
  e.run(3, 72, 72.0, 5.0);
  check(e.count(SlowEvent::End) == 1 && !e.regime.inRegime(), "S5.end: two normal seconds in a row end it, once");
  const SlowReport* end = nullptr;
  for (const SlowReport& rep : e.reports)
    if (rep.event == SlowEvent::End) end = &rep;
  check(end && std::string(end->reason) == "recovered" && closeTo(end->durationS, 40.0, 1e-9) && end->frames == 400 && closeTo(end->windowS, 40.0, 1e-9) && closeTo(end->fps, 10.0, 1e-9) &&
            closeTo(end->vendorEndMs, 83.1, 1e-9) && end->owner == SlowOwner::VendorEndFrame && end->regime == 1,
        "S5.report: the end report covers the whole regime (400 frames in 40 s, the owner of all of it), and its duration stops at the last slow second");
  e.run(10, 10, 72.0, 83.1);
  e.frame(72.0, 83.1);
  check(e.count(SlowEvent::Slow) == 2 && e.regime.regimes() == 2, "S5.again: the next slow stretch is regime 2");
  SlowRig mixed;
  mixed.run(5, 10, 72.0, 83.1);
  mixed.run(1, 72, 72.0, 5.0);
  mixed.run(5, 10, 72.0, 83.1);
  mixed.run(2, 72, 72.0, 5.0);
  mixed.frame(72.0, 5.0);   // the frame that closes the second normal second
  const SlowReport* last = mixed.reports.empty() ? nullptr : &reportAt(mixed.reports, mixed.reports.size() ? mixed.reports.size() - 1 : 0);
  check(last && last->event == SlowEvent::End && last->frames == 5 * 10 + 72 + 5 * 10 && closeTo(last->durationS, 5 + 1 + 5, 1e-9),
        "S5.fold: a normal second between slow ones belongs to the regime: its frames are in the end report and its second in the duration");
}

void ownerCases() {
  auto holder = [](double end, double wait, double swap, double copy, double edvr) {
    SlowRig r;
    r.run(5, 10, 72.0, end, wait, swap, copy, edvr);
    r.frame(72.0, end, wait, swap, copy, edvr);
    return r.reports.empty() ? SlowReport() : reportAt(r.reports, 0);
  };
  check(holder(60, 0, 0, 0, 0).owner == SlowOwner::VendorEndFrame, "S6.end: xrEndFrame at 60 ms of 100");
  check(holder(0, 60, 0, 0, 0).owner == SlowOwner::VendorWaitFrame, "S6.wait: the pose wait at 60 ms of 100");
  check(holder(0, 0, 70, 0, 0).owner == SlowOwner::VendorSwapchain, "S6.swapchain: the swapchain calls at 70 ms of 100");
  check(holder(0, 0, 0, 80, 0).owner == SlowOwner::EdvrCopy, "S6.copy: EDVR's copy at 80 ms of 100");
  check(holder(0, 0, 0, 0, 90).owner == SlowOwner::EdvrWork, "S6.edvr: EDVR's own work at 90 ms of 100");
  const SlowReport game = holder(5, 1, 0, 1, 1);
  check(game.owner == SlowOwner::Game && closeTo(game.gameMs, 92.0, 1e-9) && closeTo(game.ownerShare, 0.92, 1e-9), "S6.game: what no owner accounts for is the game's frame");
  const SlowReport none = holder(30, 30, 0, 30, 0);
  check(none.owner == SlowOwner::None && none.largest == SlowOwner::VendorEndFrame && closeTo(none.largestShare, 0.30, 1e-9), "S6.share: under 35% no owner is named; the largest is kept for the line (the first of equals)");
  const SlowReport edge = holder(35, 30, 0, 30, 0);
  check(edge.owner == SlowOwner::VendorEndFrame && closeTo(edge.ownerShare, 0.35, 1e-9) && holder(34.9, 30, 0, 30, 0).owner == SlowOwner::None, "S6.share: 35% is named, just under it is not");
  const SlowReport overlap = holder(90, 0, 0, 40, 0);
  check(closeTo(overlap.gameMs, 0.0, 1e-9) && overlap.owner == SlowOwner::VendorEndFrame && closeTo(overlap.vendorShare, 0.9, 1e-9),
        "S6.overlap: owners that add to more than the frame (the game and the end-frame side by side) leave the game's share at zero, never below");
  const SlowReport vendors = holder(30, 30, 30, 0, 0);
  check(closeTo(vendors.vendorShare, 0.9, 1e-9) && vendors.owner == SlowOwner::None, "S6.vendor: three vendor calls of 30 ms each: the vendor holds 90% in all, and no one call is named");
  SlowSums empty;
  empty.spanMs = 5000;
  const SlowReport none2 = slowReport(SlowEvent::Slow, empty, 72.0, 5.0, 1, nullptr);
  check(none2.owner == SlowOwner::NoFrames && none2.frames == 0 && closeTo(none2.fps, 0.0, 1e-9), "S6.no-frames: a window in which no frame reached xrEndFrame has no owner to name, and says so");
}

void gapCases() {
  SlowRig r;
  r.run(3, 10, 72.0, 83.1);
  r.now += 40000;   // 40 s with nothing at all
  r.frame(72.0, 83.1);
  check(r.reports.size() == 2 && reportAt(r.reports, 0).event == SlowEvent::Slow && reportAt(r.reports, 1).event == SlowEvent::StillSlow, "S7.gap: 40 s without a frame is slow seconds, reported when the next frame arrives, in order");
  check(reportAt(r.reports, 0).frames == 30 && reportAt(r.reports, 1).owner == SlowOwner::NoFrames && reportAt(r.reports, 1).frames == 0, "S7.gap: the SLOW report holds the 30 frames there were, the still_slow report none");
  SlowRig huge;
  huge.run(2, 10, 72.0, 83.1);
  huge.now += 36000ull * 1000ull;   // ten hours
  huge.frame(72.0, 83.1);
  check(huge.regime.secondsClosed() == 1 + kSlowMaxCatchUp, "S7.cap: a gap of ten hours closes at most 120 seconds (the one second before it closed as the frames went), not 36000");
  check(huge.count(SlowEvent::Slow) == 1 && huge.count(SlowEvent::StillSlow) == 3, "S7.cap: and writes the lines of those seconds, no more: the SLOW line and a still_slow line every 30 s of them");
  const size_t before = huge.reports.size();
  huge.run(3, 72, 72.0, 5.0);
  huge.frame(72.0, 5.0);
  check(huge.reports.size() == before + 1 && reportAt(huge.reports, huge.reports.size() ? huge.reports.size() - 1 : 0).event == SlowEvent::End, "S7.anchor: after the jump the clock is anchored at the frame: two normal seconds end the regime");
}

void finishCases() {
  SlowRig r;
  int calls = 0;
  r.regime.finish([&](const SlowReport&) { ++calls; });
  check(calls == 0, "S8.closed: nothing open, nothing written at close");
  r.run(12, 10, 72.0, 83.1);
  r.frame(72.0, 83.1);
  SlowReport closing;
  r.regime.finish([&](const SlowReport& rep) {
    ++calls;
    closing = rep;
  });
  check(calls == 1 && closing.event == SlowEvent::End && std::string(closing.reason) == "session_close" && closing.owner == SlowOwner::VendorEndFrame && closeTo(closing.durationS, 12.0, 1e-9) &&
            !r.regime.inRegime(),
        "S8.open: a regime open at close ends there, with its owner and how long it had lasted");
  r.regime.finish([&](const SlowReport&) { ++calls; });
  check(calls == 1, "S8.once: and only once");
}

// ---- S11: what the frames were (the context a game-owned regime is read by) ------------------------------------------------
void contextCases() {
  // Five slow seconds at 10 fps (50 frames, held by no one but the game), frames tagged by `tag`: the SLOW report's context.
  auto slowWindow = [](std::function<void(SlowFrame&, uint64_t)> tag) {
    SlowRig r;
    r.tag = tag;
    r.run(5, 10, 72.0, 5.0);
    r.frame(72.0, 5.0);
    return r;
  };
  const SlowRig scene = slowWindow(nullptr);
  const SlowReport& s0 = reportAt(scene.reports, 0);
  check(scene.reports.size() == 1 && s0.owner == SlowOwner::Game && s0.context == SlowContext::Scene && s0.loadingFrames == 0 && s0.emptyFrames == 0,
        "S11.scene: frames the game submitted are the scene context, with no loading or empty frame counted");
  const SlowRig loading = slowWindow([](SlowFrame& f, uint64_t) { f.background = true; });
  const SlowReport& l0 = reportAt(loading.reports, 0);
  check(l0.context == SlowContext::Loading && l0.loadingFrames == 50 && l0.emptyFrames == 0 && l0.owner == SlowOwner::Game,
        "S11.loading: a window of the runtime's own loading frames is the loading context, still held by the game's own frame");
  const SlowRig empty = slowWindow([](SlowFrame& f, uint64_t) { f.noLayers = true; });
  const SlowReport& e0 = reportAt(empty.reports, 0);
  check(e0.context == SlowContext::NoLayers && e0.emptyFrames == 50 && e0.loadingFrames == 0, "S11.no-layers: a window of ends with no layers is the no_layers context");
  const SlowRig both = slowWindow([](SlowFrame& f, uint64_t) { f.background = f.noLayers = true; });
  const SlowReport& b0 = reportAt(both.reports, 0);
  check(b0.loadingFrames == 50 && b0.emptyFrames == 0 && b0.context == SlowContext::Loading, "S11.once: a loading frame is counted as one, never also as an empty end");
  const SlowRig half = slowWindow([](SlowFrame& f, uint64_t i) { f.background = i < 50 && i % 2 == 0; });
  const SlowRig under = slowWindow([](SlowFrame& f, uint64_t i) { f.background = i < 48 && i % 2 == 0; });
  check(reportAt(half.reports, 0).loadingFrames == 25 && reportAt(half.reports, 0).context == SlowContext::Loading && reportAt(under.reports, 0).loadingFrames == 24 &&
            reportAt(under.reports, 0).context == SlowContext::Scene,
        "S11.half: half the window's frames (25 of 50) make it a load, one fewer does not");
  const SlowRig emptyHalf = slowWindow([](SlowFrame& f, uint64_t i) { f.noLayers = i < 25; });
  const SlowRig emptyUnder = slowWindow([](SlowFrame& f, uint64_t i) { f.noLayers = i < 24; });
  check(reportAt(emptyHalf.reports, 0).context == SlowContext::NoLayers && reportAt(emptyUnder.reports, 0).context == SlowContext::Scene,
        "S11.empty-half: the same at half for the ends with no layers (25 of 50, not 24)");
  const SlowRig mixed = slowWindow([](SlowFrame& f, uint64_t i) {
    f.background = i < 25;
    f.noLayers = i >= 25 && i < 50;
  });
  check(reportAt(mixed.reports, 0).context == SlowContext::Loading && reportAt(mixed.reports, 0).loadingFrames == 25 && reportAt(mixed.reports, 0).emptyFrames == 25,
        "S11.first: when both reach half, loading is what it is called");
  SlowRig regime;
  regime.tag = [](SlowFrame& f, uint64_t i) { f.background = i < 50; };   // the first five seconds are loading frames, the next thirty-five the game's scenes
  regime.run(40, 10, 72.0, 5.0);
  regime.run(3, 72, 72.0, 5.0);
  const SlowReport* end = nullptr;
  for (const SlowReport& rep : regime.reports)
    if (rep.event == SlowEvent::End) end = &rep;
  check(reportAt(regime.reports, 0).event == SlowEvent::Slow && reportAt(regime.reports, 0).context == SlowContext::Loading && end && end->frames == 400 && end->loadingFrames == 50 &&
            end->emptyFrames == 0 && end->context == SlowContext::Scene,
        "S11.regime: the SLOW report reads the first five seconds (a load), the end report the whole regime (50 loading frames of 400: scenes)");
  SlowRig folded;
  folded.tag = [](SlowFrame& f, uint64_t i) { f.noLayers = i >= 50 && i < 122; };   // the one normal second between two slow stretches is all empty ends
  folded.run(5, 10, 72.0, 5.0);
  folded.run(1, 72, 72.0, 5.0);
  folded.run(5, 10, 72.0, 5.0);
  folded.run(2, 72, 72.0, 5.0);
  folded.frame(72.0, 5.0);
  const SlowReport& f0 = reportAt(folded.reports, folded.reports.size() ? folded.reports.size() - 1 : 0);
  check(f0.event == SlowEvent::End && f0.frames == 5 * 10 + 72 + 5 * 10 && f0.emptyFrames == 72 && f0.context == SlowContext::Scene,
        "S11.fold: a normal second folded into the regime brings its empty frames with it (72 of 172: the regime's scenes still outweigh them)");
  SlowSums nobody;
  nobody.spanMs = 5000;
  const SlowReport n0 = slowReport(SlowEvent::Slow, nobody, 72.0, 5.0, 1, nullptr);
  check(n0.context == SlowContext::None && n0.loadingFrames == 0 && n0.emptyFrames == 0, "S11.none: a window with no frame has no context");
  char line[1200];
  formatSlowLine(line, sizeof(line), reportAt(loading.reports, 0), nullptr);
  check(contains(line, ",frame_ms=100.00,context=loading,loading_frames=50,empty_frames=0,held_by=game,"), "S11.line: the line carries the context and its counts right after the frame time");
  formatSlowLine(line, sizeof(line), n0, nullptr);
  check(contains(line, ",context=none,loading_frames=0,empty_frames=0,held_by=no_frames,"), "S11.line: a window with no frame says context=none");
  check(std::string(slowContextKey(SlowContext::Scene)) == "scene" && std::string(slowContextKey(SlowContext::Loading)) == "loading" &&
            std::string(slowContextKey(SlowContext::NoLayers)) == "no_layers" && std::string(slowContextKey(SlowContext::None)) == "none",
        "S11.keys: the four contexts' keys");
}

VramFigures sampleFigures() {
  VramFigures f;
  f.local.valid = f.nonLocal.valid = true;
  f.local.usage = 7421ull << 20;
  f.local.budget = 10863ull << 20;
  f.nonLocal.usage = 316ull << 20;
  f.nonLocal.budget = 16311ull << 20;
  return f;
}

void lineCases() {
  SlowRig r;
  r.run(5, 10, 72.0, 83.1, 0.0, 0.04, 0.4, 0.3);
  r.frame(72.0, 83.1, 0.0, 0.04, 0.4, 0.3);
  char line[1100];
  const VramFigures figures = sampleFigures();
  formatSlowLine(line, sizeof(line), reportAt(r.reports, 0), &figures);
  check(std::string(line) ==
            "native_slow_regime,event=SLOW,regime=1,duration_s=5.0,window_s=5.0,frames=50,fps=10.00,display_hz=72.00,fraction=0.139,threshold=0.40,frame_ms=100.00,"
            "context=scene,loading_frames=0,empty_frames=0,"
            "held_by=vendor_end_frame,held_share=0.831,vendor_share=0.831,vendor_end_frame_ms=83.10,vendor_wait_frame_ms=0.00,vendor_swapchain_ms=0.04,edvr_copy_ms=0.40,"
            "edvr_work_ms=0.30,game_ms=16.16,end_frame_max_ms=83.10,vram_local_used_mb=7421,vram_local_budget_mb=10863,vram_local_pct=68.3,vram_nonlocal_used_mb=316,"
            "vram_nonlocal_budget_mb=16311,vram_nonlocal_pct=1.9,units=wall_ms,summary=held by the vendor runtime's xrEndFrame: 83.1 ms of every 100.0 ms frame (83%)",
        "S9.slow: the SLOW line: the regime, the window, the rate against the display's, the means by owner, who holds it, the VRAM figures, and the words");
  formatSlowLine(line, sizeof(line), reportAt(r.reports, 0), nullptr);
  check(contains(line, ",end_frame_max_ms=83.10,vram=unavailable,units=wall_ms,"), "S9.vram: without figures the line says so, not zeros");
  VramFigures partial = figures;
  partial.nonLocal = VramSegment{};
  formatSlowLine(line, sizeof(line), reportAt(r.reports, 0), &partial);
  check(contains(line, "vram_local_used_mb=7421,") && contains(line, "vram_nonlocal_used_mb=-,vram_nonlocal_budget_mb=-,vram_nonlocal_pct=-"), "S9.vram: a segment the OS did not answer for is '-'");
  SlowRig s;
  s.run(5, 10, 72.0, 83.1);
  s.run(30, 10, 72.0, 83.1);
  s.frame(72.0, 83.1);
  formatSlowLine(line, sizeof(line), reportAt(s.reports, 1), nullptr);
  check(std::string(line).rfind("native_slow_regime,event=still_slow,regime=1,duration_s=35.0,window_s=30.0,frames=300,fps=10.00,", 0) == 0, "S9.still-slow: the still_slow line says how long the regime has lasted and covers 30 s");
  s.run(3, 72, 72.0, 5.0);
  formatSlowLine(line, sizeof(line), reportAt(s.reports, s.reports.size() ? s.reports.size() - 1 : 0), nullptr);
  check(std::string(line).rfind("native_slow_regime,event=end,reason=recovered,regime=1,duration_s=35.0,window_s=35.0,frames=350,", 0) == 0, "S9.end: the end line says why, how long, and covers the whole regime");
  SlowReport nobody;
  nobody.event = SlowEvent::Slow;
  nobody.owner = SlowOwner::None;
  nobody.largest = SlowOwner::Game;
  nobody.largestShare = 0.31;
  nobody.frameMs = 100.0;
  formatSlowLine(line, sizeof(line), nobody, nullptr);
  check(contains(line, "held_by=none,") && contains(line, "summary=no single owner; the largest is the game's own frame at 31% of a 100.0 ms frame"), "S9.none: no single owner is said, with the largest and its share");
  SlowReport nothing;
  nothing.event = SlowEvent::StillSlow;
  nothing.owner = SlowOwner::NoFrames;
  formatSlowLine(line, sizeof(line), nothing, nullptr);
  check(contains(line, "held_by=no_frames,") && contains(line, "summary=held by nothing: no frame reached xrEndFrame in this window"), "S9.no-frames: a window with no frames is said to be held by nothing");
  SlowReport widest;
  widest.event = SlowEvent::End;
  widest.reason = "session_close";
  widest.regime = 4000000000u;
  widest.durationS = widest.windowS = 123456789.0;
  widest.frames = 18446744073709551615ull;
  widest.fps = widest.refHz = widest.fraction = widest.frameMs = widest.vendorEndMs = widest.vendorWaitMs = widest.vendorSwapMs = widest.copyMs = widest.edvrMs = widest.gameMs =
      widest.endMaxMs = 123456789.123;
  widest.context = SlowContext::NoLayers;
  widest.loadingFrames = widest.emptyFrames = 18446744073709551615ull;
  widest.owner = SlowOwner::VendorSwapchain;
  widest.ownerMs = 123456789.123;
  widest.ownerShare = widest.vendorShare = 99999.999;
  VramFigures big;
  big.local.valid = big.nonLocal.valid = true;
  big.local.usage = big.local.budget = big.nonLocal.usage = big.nonLocal.budget = ~0ull;
  const size_t n = formatSlowLine(line, sizeof(line), widest, &big);
  check(n > 0 && n < sizeof(line) - 1 && contains(line, "summary=held by the vendor runtime's swapchain calls"), "S9.fit: the widest SLOW line fits the 1100 bytes the host gives it, tail intact");
}

void armedCases() {
  char line[800];
  formatSlowArmed(line, sizeof(line), "available");
  check(std::string(line) == "native_slow_regime,armed=1,threshold_fraction=0.40,of=display_rate,hold_s=5,end_s=2,still_slow_s=30,owner_share=0.35,frames=xrEndFrame_returns,"
                             "owners=vendor_end_frame|vendor_wait_frame|vendor_swapchain|edvr_copy|edvr_work|game,vram=available",
        "S10.armed: the armed line says the threshold, the three durations, the owner rule, the owners and whether VRAM can be read");
  formatSlowArmed(line, sizeof(line), "unavailable: IDXGIAdapter3 is not offered, and QueryVideoMemoryInfo needs it");
  check(contains(line, ",vram=unavailable: IDXGIAdapter3 is not offered; and QueryVideoMemoryInfo needs it") && std::count(line, line + std::strlen(line), ',') == 10,
        "S10.armed: a reason with a comma in it does not add a field to the line");
  formatSlowArmed(line, sizeof(line), nullptr);
  check(contains(line, ",vram=unavailable"), "S10.armed: no state is unavailable");
  SlowRig r;
  r.run(10, 10, 72.0, 83.1);
  r.frame(72.0, 83.1);
  r.run(3, 72, 72.0, 5.0);
  check(r.regime.framesSeen() == 100 + 1 + 216 && r.regime.secondsClosed() == 12 && r.regime.slowSeconds() == 10 && r.regime.regimes() == 1,
        "S10.counters: frames, seconds closed, slow seconds and regimes are counted");
  formatSlowSummary(line, sizeof(line), "session_close", r.regime);
  check(std::string(line).rfind("native_slow_regime_summary,reason=session_close,regimes=1,open=0,frames=317,seconds=", 0) == 0 && contains(line, ",threshold=0.40"),
        "S10.summary: the summary says how many regimes, whether one is open, the frames and the seconds");
  SlowRig quiet;
  formatSlowSummary(line, sizeof(line), "periodic", quiet.regime);
  check(std::string(line) == "native_slow_regime_summary,reason=periodic,regimes=0,open=0,frames=0,seconds=0,slow_seconds=0,threshold=0.40",
        "S10.summary: a session with no frames says zeros, which is the proof the instrument ran");
  formatSlowSummary(line, sizeof(line), nullptr, quiet.regime);
  check(std::string(line).rfind("native_slow_regime_summary,regimes=0,", 0) == 0, "S10.summary: without a reason there is no reason field");
}

// ---- T1 ----------------------------------------------------------------------------------------------------------
void scheduleCases() {
  check(kSlowTestStartMs == 90000 && kSlowTestForMs == 40000 && kSlowTestMaxMs == 500, "T1.constants: the hold begins 90 s in, lasts 40 s, and is at most 500 ms a call");
  SlowTestSchedule off;
  off.arm(1000, 0);
  check(!off.on() && off.tick(1000 + 100000) == SlowTestSchedule::Step::None && off.holdNowMs() == 0 && !off.active(), "T1.off: a key of 0 never holds anything");
  SlowTestSchedule never;
  check(never.tick(500000) == SlowTestSchedule::Step::None && never.holdNowMs() == 0, "T1.unarmed: a schedule never armed never holds");
  SlowTestSchedule s;
  s.arm(5000, 80);
  check(s.on() && s.configuredMs() == 80 && s.holdNowMs() == 0, "T1.armed: armed with 80, nothing yet");
  check(s.tick(5000) == SlowTestSchedule::Step::None && s.tick(5000 + 89999) == SlowTestSchedule::Step::None && s.holdNowMs() == 0, "T1.wait: nothing for 90 s");
  check(s.tick(5000 + 90000) == SlowTestSchedule::Step::Began && s.active() && s.holdNowMs() == 80, "T1.begin: at 90 s the hold begins, with the key's value");
  check(s.tick(5000 + 90001) == SlowTestSchedule::Step::None && s.tick(5000 + 129999) == SlowTestSchedule::Step::None && s.holdNowMs() == 80, "T1.during: and holds for 40 s, said once");
  check(s.tick(5000 + 130000) == SlowTestSchedule::Step::Ended && !s.active() && s.holdNowMs() == 0 && s.done(), "T1.end: at 130 s it ends, said once");
  check(s.tick(5000 + 400000) == SlowTestSchedule::Step::None && s.holdNowMs() == 0, "T1.done: and never again");
  SlowTestSchedule clamp;
  clamp.arm(0, 9999);
  SlowTestSchedule negative;
  negative.arm(0, -5);
  check(clamp.configuredMs() == 500 && !negative.on(), "T1.clamp: a value over 500 is 500, a negative one is off");
  SlowTestSchedule late;
  late.arm(1000, 80);
  check(late.tick(1000 + 500000) == SlowTestSchedule::Step::Began && late.tick(1000 + 500001) == SlowTestSchedule::Step::Ended,
        "T1.late: a frame that arrives after the whole window (a suspend) begins it and the next ends it: the hold is not skipped silently, and not left on");
  SlowTestSchedule back;
  back.arm(10000, 80);
  check(back.tick(5000) == SlowTestSchedule::Step::None && back.holdNowMs() == 0, "T1.clock: a clock behind the arming reading holds nothing");
}

// ---- G1 ----------------------------------------------------------------------------------------------------------
std::string slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

size_t at(const std::string& text, const char* needle, size_t from = 0) { return text.find(needle, from); }

void sourcePins(const std::string& root) {
  const std::string host = slurp(root + "\\src\\openxr\\native_runtime_host.h");
  const std::string boundary = slurp(root + "\\src\\openxr\\frame_boundary.h");
  const std::string session = slurp(root + "\\src\\openxr\\session_state.h");
  const std::string perf = slurp(root + "\\src\\d3d11\\perf_monitor.cpp");
  const std::string flag = slurp(root + "\\src\\common\\frame_flag.cpp");
  const std::string flagHeader = slurp(root + "\\src\\common\\frame_flag.h");
  const std::string bat = slurp(root + "\\build.bat");
  const std::string ini = slurp(root + "\\edvr.ini");
  check(!host.empty() && !boundary.empty() && !session.empty() && !perf.empty() && !flag.empty() && !flagHeader.empty() && !bat.empty() && !ini.empty(),
        "G1.read: the eight sources the glue lives in were read");
  if (host.empty() || boundary.empty() || session.empty() || perf.empty() || flag.empty() || flagHeader.empty() || bat.empty() || ini.empty()) return;
  const size_t none = std::string::npos;
  // FrameBoundary: the hold is INSIDE the timed scope, the report follows it; the empty ends are reported too.
  const size_t scope = at(boundary, "{ SubmissionWallScope measured(&endFrameMs_);\n      lastResult_ = session_.end(frame_, end, {geometryReady_, pixels});\n      sink_.holdEndFrame(); }");
  const size_t report = at(boundary, "sink_.endFrameReturned({sequence, endFrameMs_, lastPeriod_, shouldRender, pixels, background, turbo_, lastResult_});");
  const size_t scene = at(boundary, "if (!background) sink_.sceneFinished(pixels, lastResult_);");
  check(scope != none && report != none && scene != none && scope < report && report < scene,
        "G1.boundary: finish() holds inside the timed xrEndFrame scope, then reports the call, then tells the scene it finished");
  check(at(boundary, "lastResult_ = endEmpty(frame_);") != none && at(boundary, "const auto ended = endEmpty(scratch);") != none &&
            at(boundary, "{ SubmissionWallScope measured(&ms); r = session_.end(frame, empty, {false, false}); }\n    sink_.endFrameReturned({sequence, ms, lastPeriod_, shouldRender, false, false, turbo_, r});") != none,
        "G1.boundary: the empty ends clear() and drain() make are timed and reported through the same hook");
  check(at(session, "if (observer_) observer_(buffer, observerContext_);") != none && at(session, "observer_ = nullptr; observerContext_ = nullptr;") != none,
        "G1.session: pollEvents hands every received event to the observer, and reset() clears it");
  // The host: the three instruments, fed and armed and closed.
  check(at(host, "state.setEventObserver(vendorEvent,this);") != none && at(host, "state.setUnhandledEventSink(referenceEvent,this);") != none &&
            at(host, "state.setUnhandledEventSink(referenceEvent,this);") < at(host, "state.setEventObserver(vendorEvent,this);"),
        "G1.host: the vendor's events are observed, from the same place the policy is bound");
  check(at(host, "if(host.vendorEvents.note(buffer,lagMs,GetTickCount64(),line,sizeof(line)))nativeTracePuts(line);") != none &&
            at(host, "XR_SUCCEEDED(host.api.convertTime(host.instance,&counter,&now))") != none,
        "G1.host: each event's line is written, a state change with how long ago the vendor made it");
  const size_t open = at(host, "if(!open(startupOptions))return vr::VRInitError_Init_Internal;");
  const size_t armed = at(host, "startInstruments();", open);
  check(open != none && armed != none && armed - open < 200, "G1.host: the three armed lines are written at startup, right after the runtime opens");
  check(at(host, "VendorEventLog::formatArmed(line,sizeof(line));nativeTracePuts(line);") != none && at(host, "EndFrameEpisodes::formatArmed(line,sizeof(line));nativeTracePuts(line);") != none &&
            at(host, "formatSlowArmed(line,sizeof(line),vramState);nativeTracePuts(line);") != none,
        "G1.host: each instrument writes its own armed line");
  const size_t hold = at(host, "void holdEndFrame() override {");
  check(hold != none && at(host, "const uint32_t ms=endFrameHoldMs();", hold) != none && at(host, "Sleep(ms);", hold) != none && at(host, "Sleep(ms);", hold) < at(host, "void writeSlowLine(", hold),
        "G1.host: the test hold reads frame_flag's request and sleeps for it");
  const size_t returned = at(host, "void endFrameReturned(const EndFrameInfo& info) override {");
  check(returned != none && at(host, "endFrameEpisodes.observe(call,endFrameLines);", returned) != none && at(host, "slowRegime.observe(frame,[&](const SlowReport& report){writeSlowLine(report);});", returned) != none,
        "G1.host: every xrEndFrame feeds the episodes and the slow regime");
  check(at(host, "frame.vendorWaitMs=boundary.waitBlockMs()+boundary.pacerBlockMs();", returned) != none && at(host, "frame.vendorEndMs=info.ms;", returned) != none &&
            at(host, "frame.vendorSwapMs=acquire+wait+release;", returned) != none && at(host, "frame.copyMs=copy+draw;", returned) != none &&
            at(host, "const double copy=scene?transferWall.producerDispatch+submitSample.receiveMs:0.0;", returned) != none,
        "G1.host: each owner's figure is taken where the runtime measures it: the pose wait and the pacer's block, the end call, the swapchain calls, the copy");
  check(at(host, "frame.background=info.background;", returned) != none && at(host, "frame.noLayers=!info.layers&&!info.background;", returned) != none,
        "G1.host: the context's two counts are fed from the call's own flags: a loading frame is the boundary's background, an empty end has no layers and is not one");
  check(at(host, "finishingOverlapped=true; // endFrameReturned names the path\n    const auto r=boundary.finishPair()") != none &&
            at(host, "call.overlapped=finishingOverlapped&&!info.background;") != none,
        "G1.host: the overlapped finish says so, so the episode line can name its path");
  const size_t summary = at(host, "if(tracing)writeLongCycleSummary(nullptr,true);");
  check(summary != none && at(host, "if(tracing)finishInstruments();", summary) != none && at(host, "writeInstrumentSummaries(\"session_close\");") != none &&
            at(host, "writeInstrumentSummaries(\"periodic\");") != none,
        "G1.host: at close the open episode and regime end and the summaries are written; they are written every five minutes too");
  check(at(host, "vramAdapter.Attach(vramAdapterOf(graphics.device(),&why))") != none && at(host, "if(vramAdapter){figures=vramRead(vramAdapter.Get());vram=&figures;}") != none,
        "G1.host: the SLOW line's figures are read from the runtime's own device's adapter at the moment it is written");
  // The measurement the attribution needs is always on (it was taken only while the 30 s window had room).
  check(at(host, "submitStats.full()?nullptr:&wall") == none && at(host, "submitStats.full()?nullptr:&transferWall") == none && at(host, "submitStats.full()?nullptr:&submitSample.receiveMs") == none &&
            at(host, "const bool measure=counterNow(&dispatchBegan);") != none,
        "G1.host: the swapchain, copy and treatment times are measured every frame, not only while the submit window has room");
  // The test trigger: the key, the schedule, the request.
  check(at(perf, "Config::get().getIntInRange(\"advanced.slow_test_ms\", 0, 0, 500)") != none && at(perf, "requestEndFrameHold(s.slowTest.holdNowMs());") != none &&
            at(perf, "requestEndFrameHold(0);") != none && at(perf, "    freezeTestTick();\n    slowTestTick();\n") != none,
        "G1.trigger: perf_monitor.cpp reads advanced.slow_test_ms, begins and ends the request on the schedule, every frame");
  check(at(flagHeader, "constexpr uint32_t kFrameFlagVersion = 37;") != none && at(flag, "L\"Local\\\\edvr_glitch_frame_v37_%lu\"") != none && at(flag, "volatile LONG     endFrameHold;") != none &&
            at(flag, "ms > 5000u ? 5000u : ms") != none,
        "G1.flag: the request crosses frame_flag: layout v37, the field, and the runtime never holds longer than 5 s");
  check(at(bat, "tools\\slow_regime_test\\slow_regime_test.cpp") != none, "G1.build: build.bat compiles this rig");
  check(at(ini, "#slow_test_ms = 0") != none && at(ini, "# TEST ONLY. 0 (the default) does nothing. A value from 1 to 500 makes the") != none,
        "G1.ini: advanced.slow_test_ms is documented in edvr.ini as test only, default 0");
}

}  // namespace

int main(int argc, char** argv) {
  bool selfTest = false, dryRun = false;
  std::string root;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--self-test") selfTest = true;
    else if (a == "--dry-run") dryRun = true;
    else if (selfTest && root.empty() && a.rfind("--", 0) != 0) root = a;
  }
  if (dryRun) {
    std::printf("[slow_regime_test] dry-run: touches nothing (no log, no file, no thread)\n");
    return 0;
  }
  if (!selfTest) {
    std::printf("usage: slow_regime_test.exe --dry-run | --self-test [root]\n");
    return 2;
  }
  nameCases();
  decodeCases();
  stateCases();
  unknownCases();
  capCases();
  summaryCases();
  observerCases();
  thresholdCases();
  startLineCases();
  lifeCases();
  percentileCases();
  limiterCases();
  closeCases();
  hookCases();
  judgeCases();
  halfRateCases();
  beginCases();
  stillSlowCases();
  endCases();
  ownerCases();
  gapCases();
  finishCases();
  contextCases();
  lineCases();
  armedCases();
  scheduleCases();
  if (root.empty()) std::printf("[slow_regime_test] SKIPPED the G1 source pins: no repository root given\n");
  else sourcePins(root);
  if (g_failures) {
    std::printf("FAIL: slow regime: %u of %u checks failed\n", g_failures, g_checks);
    return 1;
  }
  std::printf("PASS: %u slow regime checks\n", g_checks);
  return 0;
}

#pragma once
// What the vendor's runtime tells ours, one line each (docs/headset-lock-vdxr-2026-10-02.md, instrument 2).
//
// WHY. SessionState::pollEvents (session_state.h) takes what the vendor sends through xrPollEvent and acts on the
// few events it needs; until this header it logged none of them. The host's own service_lifecycle line is the
// policy's coarse enum, written when the owner's idle poll happens to see it change, with no time and no way to tell
// IDLE from SYNCHRONIZED. A Quest 3 user's headset locked while VDXR held every xrEndFrame for 83 ms; whether the
// runtime had changed the session's state, lost events, warned of an instance loss or moved a reference space around
// that moment was not in any log. This is the log of it.
//
// WHAT. Every event the runtime receives, decoded: session state changes (from -> to, with the event's own time and
// how long ago that was when we read it), events lost, instance loss pending, reference space change pending,
// interaction profile changed, and the extension events the host already enables (visibility mask, display refresh
// rate, performance settings). Any other type is named once by its number, so a vendor extension nobody decodes yet
// still leaves a line the first time it appears.
//
// BOUNDED. A session writes at most kVendorEventLineCap lines of decoded events and names at most
// kVendorUnknownTypesNamed undecoded types; everything past that is counted, and the counts are in the summary line
// written at close and every five minutes. The last session state is always kept, written or not.
//
// This header is pure: it decodes a buffer it is handed and formats lines into buffers it is handed. The host passes
// each received event here from SessionState's observer, on the owner thread, and writes what comes back. Nothing
// here calls the runtime or the OS.
#include <openxr/openxr.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr::openxr {

constexpr unsigned kVendorEventLineCap = 200;
constexpr unsigned kVendorUnknownTypesNamed = 24;

namespace vendor_detail {
inline size_t fit(char* buf, size_t cap, int n) {
  if (n < 0) { buf[0] = 0; return 0; }
  return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}
}  // namespace vendor_detail

// The state's own name; a number this build does not know prints as STATE_<n> into `scratch`.
inline const char* xrSessionStateName(XrSessionState s, char (&scratch)[24]) {
  switch (s) {
    case XR_SESSION_STATE_UNKNOWN: return "UNKNOWN";
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    default: break;
  }
  std::snprintf(scratch, sizeof(scratch), "STATE_%d", static_cast<int>(s));
  return scratch;
}

inline const char* xrReferenceSpaceName(XrReferenceSpaceType t, char (&scratch)[24]) {
  switch (t) {
    case XR_REFERENCE_SPACE_TYPE_VIEW: return "VIEW";
    case XR_REFERENCE_SPACE_TYPE_LOCAL: return "LOCAL";
    case XR_REFERENCE_SPACE_TYPE_STAGE: return "STAGE";
    default: break;
  }
  std::snprintf(scratch, sizeof(scratch), "SPACE_%d", static_cast<int>(t));
  return scratch;
}

inline const char* xrPerfDomainName(XrPerfSettingsDomainEXT d, char (&scratch)[24]) {
  if (d == XR_PERF_SETTINGS_DOMAIN_CPU_EXT) return "CPU";
  if (d == XR_PERF_SETTINGS_DOMAIN_GPU_EXT) return "GPU";
  std::snprintf(scratch, sizeof(scratch), "DOMAIN_%d", static_cast<int>(d));
  return scratch;
}

inline const char* xrPerfSubDomainName(XrPerfSettingsSubDomainEXT d, char (&scratch)[24]) {
  if (d == XR_PERF_SETTINGS_SUB_DOMAIN_COMPOSITING_EXT) return "COMPOSITING";
  if (d == XR_PERF_SETTINGS_SUB_DOMAIN_RENDERING_EXT) return "RENDERING";
  if (d == XR_PERF_SETTINGS_SUB_DOMAIN_THERMAL_EXT) return "THERMAL";
  std::snprintf(scratch, sizeof(scratch), "SUB_DOMAIN_%d", static_cast<int>(d));
  return scratch;
}

inline const char* xrPerfLevelName(XrPerfSettingsNotificationLevelEXT l, char (&scratch)[24]) {
  if (l == XR_PERF_SETTINGS_NOTIF_LEVEL_NORMAL_EXT) return "NORMAL";
  if (l == XR_PERF_SETTINGS_NOTIF_LEVEL_WARNING_EXT) return "WARNING";
  if (l == XR_PERF_SETTINGS_NOTIF_LEVEL_IMPAIRED_EXT) return "IMPAIRED";
  std::snprintf(scratch, sizeof(scratch), "LEVEL_%d", static_cast<int>(l));
  return scratch;
}

class VendorEventLog {
 public:
  struct Counts {
    uint64_t received = 0, logged = 0, suppressed = 0;
    uint64_t sessionStateChanged = 0, eventsLost = 0, lostEvents = 0, instanceLossPending = 0, referenceSpaceChangePending = 0,
             interactionProfileChanged = 0, visibilityMaskChanged = 0, displayRefreshRateChanged = 0, perfSettings = 0,
             unknownEvents = 0, unknownTypes = 0;
  };

  // One received event. `lagMs` is how long ago the vendor made it, when the host can tell (NaN when it cannot; only
  // a session state change carries a time of its own to measure it by); `nowMs` is a millisecond clock that only
  // moves forward, kept for stateAgeMs. Returns the length of the line written into `line`, 0 when there is none to
  // write (a cap reached, or an undecoded type already named).
  size_t note(const XrEventDataBuffer& e, double lagMs, uint64_t nowMs, char* line, size_t cap) {
    if (!line || cap == 0) return 0;
    line[0] = 0;
    ++counts_.received;
    char a[24], b[24], c[24];
    char body[400];
    body[0] = 0;
    const char* kind = nullptr;
    bool decoded = true;
    switch (e.type) {
      case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
        const auto& v = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&e);
        ++counts_.sessionStateChanged;
        kind = "session_state_changed";
        char lag[40];
        if (std::isfinite(lagMs)) std::snprintf(lag, sizeof(lag), "%.3f", lagMs);
        else std::snprintf(lag, sizeof(lag), "unknown");
        std::snprintf(body, sizeof(body), ",from=%s,to=%s,event_time=%lld,lag_ms=%s", xrSessionStateName(state_, a), xrSessionStateName(v.state, b),
                      static_cast<long long>(v.time), lag);
        state_ = v.state;
        stateTime_ = v.time;
        stateNotedMs_ = nowMs;
        everState_ = true;
        break;
      }
      case XR_TYPE_EVENT_DATA_EVENTS_LOST: {
        const auto& v = *reinterpret_cast<const XrEventDataEventsLost*>(&e);
        ++counts_.eventsLost;
        counts_.lostEvents += v.lostEventCount;
        kind = "events_lost";
        std::snprintf(body, sizeof(body), ",lost_count=%u", static_cast<unsigned>(v.lostEventCount));
        break;
      }
      case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
        const auto& v = *reinterpret_cast<const XrEventDataInstanceLossPending*>(&e);
        ++counts_.instanceLossPending;
        kind = "instance_loss_pending";
        std::snprintf(body, sizeof(body), ",loss_time=%lld", static_cast<long long>(v.lossTime));
        break;
      }
      case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
        const auto& v = *reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&e);
        ++counts_.referenceSpaceChangePending;
        kind = "reference_space_change_pending";
        std::snprintf(body, sizeof(body), ",space=%s,change_time=%lld,pose_valid=%u", xrReferenceSpaceName(v.referenceSpaceType, a),
                      static_cast<long long>(v.changeTime), v.poseValid ? 1u : 0u);
        break;
      }
      case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
        ++counts_.interactionProfileChanged;
        kind = "interaction_profile_changed";
        break;
      case XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR: {
        const auto& v = *reinterpret_cast<const XrEventDataVisibilityMaskChangedKHR*>(&e);
        ++counts_.visibilityMaskChanged;
        kind = "visibility_mask_changed";
        std::snprintf(body, sizeof(body), ",view_config=%d,view=%u", static_cast<int>(v.viewConfigurationType), static_cast<unsigned>(v.viewIndex));
        break;
      }
      case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
        const auto& v = *reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB*>(&e);
        ++counts_.displayRefreshRateChanged;
        kind = "display_refresh_rate_changed";
        std::snprintf(body, sizeof(body), ",from_hz=%.3f,to_hz=%.3f", static_cast<double>(v.fromDisplayRefreshRate), static_cast<double>(v.toDisplayRefreshRate));
        break;
      }
      case XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT: {
        const auto& v = *reinterpret_cast<const XrEventDataPerfSettingsEXT*>(&e);
        ++counts_.perfSettings;
        kind = "perf_settings";
        char d[24];
        std::snprintf(body, sizeof(body), ",domain=%s,sub_domain=%s,from=%s,to=%s", xrPerfDomainName(v.domain, a), xrPerfSubDomainName(v.subDomain, b),
                      xrPerfLevelName(v.fromLevel, c), xrPerfLevelName(v.toLevel, d));
        break;
      }
      default:
        decoded = false;
        break;
    }
    if (!decoded) {
      ++counts_.unknownEvents;
      for (unsigned i = 0; i < namedTypes_; ++i)
        if (named_[i] == static_cast<int32_t>(e.type)) return 0;   // this type has had its line
      if (namedTypes_ >= kVendorUnknownTypesNamed) return 0;
      named_[namedTypes_++] = static_cast<int32_t>(e.type);
      counts_.unknownTypes = namedTypes_;
      ++counts_.logged;
      return vendor_detail::fit(line, cap, std::snprintf(line, cap, "native_xr_event,n=%llu,type=unknown,number=%d,first_of_type=1",
                                                       static_cast<unsigned long long>(counts_.received), static_cast<int>(e.type)));
    }
    if (counts_.logged - unknownLines() >= kVendorEventLineCap) {
      ++counts_.suppressed;
      if (capNoted_) return 0;
      capNoted_ = true;
      return vendor_detail::fit(line, cap,
                                std::snprintf(line, cap,
                                              "native_xr_event,n=%llu,suppressed=1,limit=%u,the rest of the decoded events are counted in native_xr_events_summary",
                                              static_cast<unsigned long long>(counts_.received), kVendorEventLineCap));
    }
    ++counts_.logged;
    return vendor_detail::fit(line, cap, std::snprintf(line, cap, "native_xr_event,n=%llu,type=%s%s", static_cast<unsigned long long>(counts_.received), kind, body));
  }

  // The vendor's last session state (UNKNOWN until one has been received), and how long ago we read it.
  XrSessionState lastState() const { return state_; }
  bool anyState() const { return everState_; }
  double stateAgeMs(uint64_t nowMs) const { return everState_ && nowMs >= stateNotedMs_ ? static_cast<double>(nowMs - stateNotedMs_) : -1.0; }
  const Counts& counts() const { return counts_; }

  // The counts, written at close and every five minutes (the zeros are the proof the instrument ran).
  size_t formatSummary(char* buf, size_t cap, const char* reason = nullptr) const {
    if (!buf || cap == 0) return 0;
    char scratch[24];
    return vendor_detail::fit(
        buf, cap,
        std::snprintf(buf, cap,
                      "native_xr_events_summary,%s%s%sreceived=%llu,logged=%llu,suppressed=%llu,session_state_changed=%llu,events_lost=%llu,lost_events=%llu,"
                      "instance_loss_pending=%llu,reference_space_change_pending=%llu,interaction_profile_changed=%llu,visibility_mask_changed=%llu,"
                      "display_refresh_rate_changed=%llu,perf_settings=%llu,unknown_events=%llu,unknown_types=%llu,last_state=%s",
                      reason ? "reason=" : "", reason ? reason : "", reason ? "," : "",
                      static_cast<unsigned long long>(counts_.received), static_cast<unsigned long long>(counts_.logged),
                      static_cast<unsigned long long>(counts_.suppressed), static_cast<unsigned long long>(counts_.sessionStateChanged),
                      static_cast<unsigned long long>(counts_.eventsLost), static_cast<unsigned long long>(counts_.lostEvents),
                      static_cast<unsigned long long>(counts_.instanceLossPending), static_cast<unsigned long long>(counts_.referenceSpaceChangePending),
                      static_cast<unsigned long long>(counts_.interactionProfileChanged), static_cast<unsigned long long>(counts_.visibilityMaskChanged),
                      static_cast<unsigned long long>(counts_.displayRefreshRateChanged), static_cast<unsigned long long>(counts_.perfSettings),
                      static_cast<unsigned long long>(counts_.unknownEvents), static_cast<unsigned long long>(counts_.unknownTypes),
                      xrSessionStateName(state_, scratch)));
  }

  // The line written once at startup: the proof that this instrument ran, whatever the session then sends.
  static size_t formatArmed(char* buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    return vendor_detail::fit(
        buf, cap,
        std::snprintf(buf, cap,
                      "native_xr_events,armed=1,source=xrPollEvent,line_cap=%u,unknown_types_named=%u,decoded=session_state_changed|events_lost|"
                      "instance_loss_pending|reference_space_change_pending|interaction_profile_changed|visibility_mask_changed|"
                      "display_refresh_rate_changed|perf_settings,undecoded=named_once_by_number",
                      kVendorEventLineCap, kVendorUnknownTypesNamed));
  }

 private:
  // Lines the cap does not count: the undecoded types' first-of-type lines have their own allowance.
  uint64_t unknownLines() const { return counts_.unknownTypes; }

  Counts counts_;
  XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
  XrTime stateTime_ = 0;
  uint64_t stateNotedMs_ = 0;
  bool everState_ = false;
  bool capNoted_ = false;
  int32_t named_[kVendorUnknownTypesNamed]{};
  unsigned namedTypes_ = 0;
};

}  // namespace edvr::openxr

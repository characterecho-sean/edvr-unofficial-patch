#pragma once
// Seat memory: the pure decisions (docs/seat-memory-2026-10-14.md, reviews\boarding-recentre-2026-10-14.md).
//
// The seated origin is where Elite's cockpit eye point sits in tracking space (the OpenVR seated convention), so the cockpit lines up whenever the
// origin is the player's real seated head position and heading. The game moves that origin with ResetSeatedZeroPose from wherever the head happens
// to be (standing, turned, mid-boarding). This remembers where the player really sits, as a pose in the runtime's natural LOCAL space, and puts the
// origin back there at every seated transition instead of trusting the head at the moment the game asks.
//
// No windows.h and no OpenXR types: the rig (tools\seat_memory_test) and the runtime host both include it. The host feeds it the context word
// (src/common/seat_context.h), head samples and the clock; it answers with decisions and the host acts. File input and output is seat_store.h.
#include "../common/seat_context.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace edvr::openxr {

constexpr double kSeatPi = 3.14159265358979323846;

// The seated origin in natural LOCAL: a position and a heading about +Y (what seatedOriginFromHead builds: no pitch, no roll).
struct SeatPose {
  double x = 0, y = 0, z = 0, yaw = 0;
};

inline double seatWrap(double a) {
  while (a > kSeatPi) a -= 2 * kSeatPi;
  while (a <= -kSeatPi) a += 2 * kSeatPi;
  return a;
}
inline double seatRadToDeg(double r) { return r * 180.0 / kSeatPi; }
inline double seatDegToRad(double d) { return d * kSeatPi / 180.0; }
inline bool seatPoseFinite(const SeatPose& p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && std::isfinite(p.yaw) && std::fabs(p.x) < 50.0 &&
         std::fabs(p.z) < 50.0 && std::fabs(p.y) < 10.0 && std::fabs(p.yaw) < 7.0;
}
inline double seatDistance(const SeatPose& a, const SeatPose& b) {
  return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// What to remember when there is no seat yet: the head, unless it is already near the runtime's own centre (LOCAL's identity), in which case the
// runtime's centre is kept exactly, so a player who set up their runtime's seated centre at the chair gets that and no EDVR offset.
constexpr double kSeatSnapMetres = 0.25;
constexpr double kSeatSnapDegrees = 20.0;

struct SeatPick {
  SeatPose seat;
  bool snapped = false;
  double distanceM = 0, yawDeg = 0;   // the head's distance and heading from LOCAL's origin
};

inline SeatPick pickSeatFromHead(const SeatPose& head) {
  SeatPick r;
  r.seat = head;
  r.distanceM = seatDistance(head, SeatPose{});
  r.yawDeg = std::fabs(seatRadToDeg(seatWrap(head.yaw)));
  if (r.distanceM <= kSeatSnapMetres && r.yawDeg <= kSeatSnapDegrees) {
    r.seat = SeatPose{};
    r.snapped = true;
  }
  return r;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// "Worn and still": a rolling window of head samples. Still is a bound on how far the head wanders from the window's mean over at least 0.4 s;
// the statistics are returned so the log can say what the head did (a headset on a stand and a head on a neck differ in these, and the first
// flight is where that is measured, not assumed).
class SeatStillness {
 public:
  static constexpr size_t kCapacity = 96;
  static constexpr size_t kMinSamples = 30;
  static constexpr uint64_t kMinSpanMs = 400;
  static constexpr uint64_t kWindowMs = 800;
  static constexpr double kMaxSpreadM = 0.02;
  static constexpr double kMaxYawSpreadDeg = 4.0;

  struct Stats {
    size_t n = 0;
    uint64_t spanMs = 0;
    double spreadCm = 0, yawSpreadDeg = 0;
    SeatPose mean;
    bool still = false;
  };

  void clear() { n_ = 0; }
  void add(uint64_t ms, const SeatPose& head) {
    if (!seatPoseFinite(head)) { clear(); return; }
    if (n_ && ms < t_[n_ - 1]) clear();     // a clock that stepped back starts the window again
    if (n_ == kCapacity) shift(1);
    t_[n_] = ms;
    p_[n_] = head;
    ++n_;
    size_t drop = 0;
    while (drop < n_ && ms - t_[drop] > kWindowMs) ++drop;
    if (drop) shift(drop);
  }
  Stats stats() const {
    Stats s;
    s.n = n_;
    if (!n_) return s;
    s.spanMs = t_[n_ - 1] - t_[0];
    double sx = 0, sy = 0, sz = 0, ss = 0, sc = 0;
    for (size_t i = 0; i < n_; ++i) {
      sx += p_[i].x; sy += p_[i].y; sz += p_[i].z;
      ss += std::sin(p_[i].yaw); sc += std::cos(p_[i].yaw);
    }
    s.mean = SeatPose{sx / n_, sy / n_, sz / n_, std::atan2(ss, sc)};
    double spread = 0, yawSpread = 0;
    for (size_t i = 0; i < n_; ++i) {
      spread = std::max(spread, seatDistance(p_[i], s.mean));
      yawSpread = std::max(yawSpread, std::fabs(seatWrap(p_[i].yaw - s.mean.yaw)));
    }
    s.spreadCm = spread * 100.0;
    s.yawSpreadDeg = seatRadToDeg(yawSpread);
    s.still = n_ >= kMinSamples && s.spanMs >= kMinSpanMs && spread <= kMaxSpreadM && s.yawSpreadDeg <= kMaxYawSpreadDeg;
    return s;
  }

 private:
  void shift(size_t by) {
    for (size_t i = by; i < n_; ++i) { t_[i - by] = t_[i]; p_[i - by] = p_[i]; }
    n_ -= by;
  }
  uint64_t t_[kCapacity]{};
  SeatPose p_[kCapacity]{};
  size_t n_ = 0;
};

// The capture at the splash, when no seat is stored: sample until the head has been still for a window, give up after a few seconds (and the
// caller then does what it did before this existed: recentre on the head as it is).
class SeatCapture {
 public:
  enum class State { Idle, Sampling, Done, GaveUp };
  static constexpr uint64_t kTimeoutMs = 4000;

  void begin(uint64_t now) { state_ = State::Sampling; began_ = now; window_.clear(); }
  State add(uint64_t now, const SeatPose& head) {
    if (state_ != State::Sampling) return state_;
    window_.add(now, head);
    last_ = window_.stats();
    if (last_.still) { state_ = State::Done; result_ = last_.mean; return state_; }
    if (now - began_ >= kTimeoutMs) state_ = State::GaveUp;
    return state_;
  }
  State state() const { return state_; }
  uint64_t elapsed(uint64_t now) const { return state_ == State::Idle ? 0 : now - began_; }
  const SeatStillness::Stats& last() const { return last_; }
  const SeatPose& result() const { return result_; }

 private:
  State state_ = State::Idle;
  uint64_t began_ = 0;
  SeatStillness window_;
  SeatStillness::Stats last_{};
  SeatPose result_;
};

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Persistence: text, one line per runtime key.
//   edvr-seat 1
//   <key> TAB <x m> TAB <y m> TAB <z m> TAB <heading degrees>
// The key is the headset_key (runtime and system), because LOCAL differs between runtimes (Pimax and SteamVR sit at different heights over the
// same room), so a seat is only meaningful for the runtime that measured it. A line that does not parse is skipped, never fatal.
constexpr const char* kSeatBookHeader = "edvr-seat 1";
constexpr size_t kSeatBookMaxRecords = 16;

struct SeatRecord {
  std::string key;
  SeatPose pose;
};

inline std::string seatKeySanitised(const std::string& key) {
  std::string k;
  for (char ch : key) k += (static_cast<unsigned char>(ch) < 32 || ch == '\t' || ch == '\x7f') ? '_' : ch;
  if (k.size() > 120) k.resize(120);
  return k;
}

inline std::string seatBookSerialize(const std::vector<SeatRecord>& records) {
  std::string out = std::string(kSeatBookHeader) + "\n";
  char line[512];
  for (const SeatRecord& r : records) {
    std::snprintf(line, sizeof line, "%s\t%.9g\t%.9g\t%.9g\t%.9g\n", seatKeySanitised(r.key).c_str(), r.pose.x, r.pose.y, r.pose.z,
                  seatRadToDeg(r.pose.yaw));
    out += line;
  }
  return out;
}

inline bool seatBookParse(const std::string& text, std::vector<SeatRecord>& out, unsigned* skipped = nullptr) {
  out.clear();
  if (skipped) *skipped = 0;
  size_t pos = 0;
  bool first = true;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (first) {
      first = false;
      if (line != kSeatBookHeader) return false;
      continue;
    }
    if (line.empty()) continue;
    std::string fields[5];
    size_t at = 0;
    bool ok = true;
    for (int i = 0; i < 5 && ok; ++i) {
      const size_t tab = line.find('\t', at);
      if (i < 4) {
        if (tab == std::string::npos) { ok = false; break; }
        fields[i] = line.substr(at, tab - at);
        at = tab + 1;
      } else {
        fields[i] = line.substr(at);
      }
    }
    SeatRecord r;
    if (ok) {
      char* e = nullptr;
      double v[4];
      for (int i = 0; i < 4 && ok; ++i) {
        v[i] = std::strtod(fields[i + 1].c_str(), &e);
        ok = e && e != fields[i + 1].c_str() && *e == '\0';
      }
      if (ok) {
        r.key = fields[0];
        r.pose = SeatPose{v[0], v[1], v[2], seatDegToRad(v[3])};
        ok = !r.key.empty() && seatPoseFinite(r.pose);
      }
    }
    if (!ok) { if (skipped) ++*skipped; continue; }
    if (out.size() < kSeatBookMaxRecords) out.push_back(r);
  }
  return !first;
}

inline bool seatBookFind(const std::vector<SeatRecord>& book, const std::string& key, SeatPose& out) {
  const std::string k = seatKeySanitised(key);
  for (const SeatRecord& r : book)
    if (r.key == k) { out = r.pose; return true; }
  return false;
}

inline void seatBookPut(std::vector<SeatRecord>& book, const std::string& key, const SeatPose& pose) {
  const std::string k = seatKeySanitised(key);
  for (SeatRecord& r : book)
    if (r.key == k) { r.pose = pose; return; }
  if (book.size() >= kSeatBookMaxRecords) book.erase(book.begin());
  book.push_back(SeatRecord{k, pose});
}

inline void seatBookErase(std::vector<SeatRecord>& book, const std::string& key) {
  const std::string k = seatKeySanitised(key);
  book.erase(std::remove_if(book.begin(), book.end(), [&](const SeatRecord& r) { return r.key == k; }), book.end());
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// The decision table.

enum class SeatWhere { Unknown, OnFoot, Aboard };

// THE RULE ON FOOT. Sean, 2026-10-14: "let's assume for now that on foot everyone is sitting". While this is true, on foot is a seat like a
// cockpit: boarding and disembarking are both transitions that put the stored seat back, and a game reset on foot is decided exactly as one
// aboard (replaced inside a window, otherwise the player's own recentre). Native on-foot VR (standing, room scale) changes this in this one
// place: false makes on foot a place where the game's resets pass through and no seat is applied, as the first build had it. Read by
// seatHolds() and nothing else; deliberately not a config key.
constexpr bool kOnFootIsSeated = true;
constexpr bool onFootIsSeated() { return kOnFootIsSeated; }
// Is the commander somewhere the seat applies?
constexpr bool seatHolds(SeatWhere w) { return w == SeatWhere::Aboard || (w == SeatWhere::OnFoot && onFootIsSeated()); }
inline const char* seatWhereName(SeatWhere w) {
  return w == SeatWhere::Aboard ? "aboard" : w == SeatWhere::OnFoot ? "on_foot" : "unknown";
}

// What to do with a ResetSeatedZeroPose the game just made.
enum class SeatResetDecision {
  GameMode,          // the graphics half has not spoken (the word is 0), or the memory is switched off: honoured, nothing remembered (the old behaviour)
  PassThrough,       // in a menu or a load (or on foot, when onFootIsSeated() is false): honoured, nothing remembered
  ReplaceWithSeat,   // aboard or on foot, inside a transition window, a seat is stored: the stored seat replaces the head pose
  HonourNoSeat,      // aboard, inside a window, nothing stored: honoured as today (an automatic reset is not evidence of where the player sits)
  HonourStore        // aboard, outside a window: the player's own recentre; honoured and remembered
};
inline const char* seatResetDecisionName(SeatResetDecision d) {
  switch (d) {
    case SeatResetDecision::GameMode: return "game_mode";
    case SeatResetDecision::PassThrough: return "pass_through";
    case SeatResetDecision::ReplaceWithSeat: return "replace_with_seat";
    case SeatResetDecision::HonourNoSeat: return "honour_no_seat";
    case SeatResetDecision::HonourStore: return "honour_store";
  }
  return "unknown";
}

// What the host does at the very start, and at the splash.
enum class SeatStartPlan { TodaysCentre, ApplySeat, CaptureLate };
inline const char* seatStartPlanName(SeatStartPlan p) {
  return p == SeatStartPlan::ApplySeat ? "apply_seat" : p == SeatStartPlan::CaptureLate ? "capture_late" : "todays_centre";
}
inline SeatStartPlan seatStartPlan(bool keep, bool hasSeat) {
  if (!keep) return SeatStartPlan::TodaysCentre;
  return hasSeat ? SeatStartPlan::ApplySeat : SeatStartPlan::CaptureLate;
}
enum class SeatIntroPlan { TodaysRecentre, SkipSeatInPlace, ApplySeat, Capture };
inline const char* seatIntroPlanName(SeatIntroPlan p) {
  switch (p) {
    case SeatIntroPlan::TodaysRecentre: return "todays_recentre";
    case SeatIntroPlan::SkipSeatInPlace: return "skip_seat_in_place";
    case SeatIntroPlan::ApplySeat: return "apply_seat";
    case SeatIntroPlan::Capture: return "capture";
  }
  return "unknown";
}
inline SeatIntroPlan seatIntroPlan(bool keep, bool hasSeat, bool originIsSeat) {
  if (!keep) return SeatIntroPlan::TodaysRecentre;
  if (!hasSeat) return SeatIntroPlan::Capture;
  return originIsSeat ? SeatIntroPlan::SkipSeatInPlace : SeatIntroPlan::ApplySeat;
}

struct SeatEvents {
  bool windowOpened = false;
  bool applyWanted = false;     // put the stored seat back (the host retries until it lands, then calls applyDone)
  bool noSeat = false;          // an apply was due and nothing is stored
  const char* why = "";
  // A manual-looking recentre made less than kRetroMs before this transition is retroactively inside its window: it is dropped and the seat
  // it was going to replace stands. The host logs it (seat_store,action=discard_pending).
  bool discardedPending = false;
  uint64_t discardAgeMs = 0;
  SeatPose discardedSeat, restoredSeat;
};

class SeatMemory {
 public:
  // The window also reaches BACKWARD: Elite's reset on the way up the ramp lands 2.6 to 4.4 s before the boarding flip is even visible, so a
  // reset outside any window is held for this long before it is believed to be the player's own (the disk is not written until then).
  static constexpr uint64_t kRetroMs = 5000;     // 8 s until 2026-10-14 (Sean: shorter); the ONE place it is set
  static constexpr uint64_t kWindowMs = 10000;   // measured: game resets near an Embark fall between 4.4 s before and 4.2 s after its journal line

  // --- the stored seat
  bool has() const { return has_; }
  const SeatPose& seat() const { return seat_; }
  void setSeat(const SeatPose& s) { seat_ = s; has_ = true; pendingStore_ = false; }
  void clearSeat() { has_ = false; originIsSeat_ = false; pendingApply_ = false; pendingStore_ = false; }
  // The seated origin right now IS the stored seat (set after the host applies or stores it; cleared by any other origin change).
  bool originIsSeat() const { return originIsSeat_; }
  void noteOrigin(bool isSeat) { originIsSeat_ = isSeat && has_; }

  // --- the context
  const SeatContext& context() const { return ctx_; }
  bool keep() const { return ctx_.published && ctx_.keep; }
  SeatWhere where() const { return where_; }
  bool inWindow(uint64_t now) const { return windowOpen_ && now >= windowBegan_ && now - windowBegan_ <= kWindowMs; }
  uint64_t windowAgeMs(uint64_t now) const { return windowOpen_ && now >= windowBegan_ ? now - windowBegan_ : 0; }
  bool applyPending() const { return pendingApply_ && has_; }
  // A recentre the runtime made (SteamVR's dashboard, Pimax Play) is always the player's: the head as it is in the new LOCAL is the seat.
  // `applied` true: the host has already made it the origin (it is then exactly HonourStore's outcome, and the caller says so with noteReset).
  // This is the other case: the head could not be sampled, LOCAL's own origin is the seat, and it goes back as soon as a frame allows.
  void storeAndApplyLater(const SeatPose& s) { seat_ = s; has_ = true; originIsSeat_ = false; pendingApply_ = true; pendingStore_ = false; }
  // The runtime's recentre has been applied as the origin: always the player, never deferred. True: write the seat out now.
  bool noteRuntimeRecentre(const SeatPose& origin) {
    seat_ = origin; has_ = true; originIsSeat_ = true; pendingApply_ = false; pendingStore_ = false;
    return true;
  }
  // A deferred store has waited kRetroMs with no transition: it is the player's. True once, with how long it waited; the host writes it out.
  bool takeCommit(uint64_t now, uint64_t& deferredMs) {
    if (!pendingStore_ || now < pendingAt_ || now - pendingAt_ < kRetroMs) return false;
    pendingStore_ = false;
    deferredMs = now - pendingAt_;
    return true;
  }
  bool storePending() const { return pendingStore_; }
  const SeatPose& previousSeat() const { return pendingPrev_; }
  void applyDone() { pendingApply_ = false; }

  static SeatWhere whereOf(const SeatContext& c) {
    if (c.seatedKnown && c.seated) return SeatWhere::Aboard;     // a sample that says both is read as seated (journal_watch.h)
    if (c.onFootKnown && c.onFoot) return SeatWhere::OnFoot;
    return SeatWhere::Unknown;
  }

  SeatEvents onContext(const SeatContext& c, uint64_t now) {
    SeatEvents ev;
    if (!c.published) { ctx_ = c; return ev; }
    const SeatWhere w = whereOf(c);
    if (!seen_) {
      seen_ = true;
      ctx_ = c; where_ = w;
      if (seatHolds(w)) return transition(ev, now, w == SeatWhere::Aboard ? "seated_at_start" : "on_foot_at_start", true);
      return ev;
    }
    const SeatContext prev = ctx_;
    const SeatWhere prevWhere = where_;
    ctx_ = c; where_ = w;
    if (w != prevWhere) {
      if (seatHolds(w)) {
        const char* why = w == SeatWhere::Aboard ? (prevWhere == SeatWhere::OnFoot ? "boarded" : "seated")
                                                  : (prevWhere == SeatWhere::Aboard ? "disembarked" : "on_foot");
        return transition(ev, now, why, true);
      }
      // Into a menu or a load: the window closes and a pending apply is cancelled, and the game's resets pass through.
      windowOpen_ = false; pendingApply_ = false;
      ev.why = w == SeatWhere::OnFoot ? "left_seat" : "unknown_state";
      return ev;
    }
    if (seatHolds(w)) {
      if (w == SeatWhere::Aboard && c.seatKind != prev.seatKind) return transition(ev, now, "cockpit_changed", true);
      if (prev.cameraOpen && !c.cameraOpen) return transition(ev, now, "camera_closed", true);
      if (!prev.cameraOpen && c.cameraOpen) return transition(ev, now, "camera_opened", false);
      if (c.transitions != prev.transitions) return transition(ev, now, "journal_event", false);
    }
    return ev;
  }

  SeatResetDecision classifyReset(uint64_t now) const {
    if (!keep()) return SeatResetDecision::GameMode;
    if (!seatHolds(where_)) return SeatResetDecision::PassThrough;
    if (inWindow(now)) return has_ ? SeatResetDecision::ReplaceWithSeat : SeatResetDecision::HonourNoSeat;
    return SeatResetDecision::HonourStore;
  }

  // The host honoured or replaced the reset: the origin is now `origin` (for a replacement, the stored seat itself). Returns true when the stored
  // seat changed and must be written out NOW. A player-looking recentre over a stored seat is not: the seat it replaces is kept for kRetroMs
  // (the memory holds the new seat meanwhile, as it always did), and a transition inside that time throws the new one away. With no seat
  // stored there is nothing to go back to and it is written at once.
  bool noteReset(SeatResetDecision d, const SeatPose& origin, uint64_t now = 0) {
    switch (d) {
      case SeatResetDecision::ReplaceWithSeat: originIsSeat_ = has_; pendingApply_ = false; return false;
      case SeatResetDecision::HonourStore: {
        const bool deferred = has_;
        if (deferred && !pendingStore_) pendingPrev_ = seat_;      // a second reset inside the wait keeps the FIRST previous seat
        seat_ = origin; has_ = true; originIsSeat_ = true; pendingApply_ = false;
        pendingStore_ = deferred;
        pendingAt_ = now;
        return !deferred;
      }
      default: originIsSeat_ = false; return false;
    }
  }

 private:
  SeatEvents& transition(SeatEvents& ev, uint64_t now, const char* why, bool apply) {
    windowOpen_ = true; windowBegan_ = now;
    ev.windowOpened = true; ev.why = why;
    if (pendingStore_) {
      ev.discardedPending = true;
      ev.discardAgeMs = now >= pendingAt_ ? now - pendingAt_ : 0;
      ev.discardedSeat = seat_; ev.restoredSeat = pendingPrev_;
      seat_ = pendingPrev_; originIsSeat_ = false; pendingStore_ = false;     // the origin is the discarded pose; the old seat goes back
      apply = true;
    }
    if (apply) {
      if (!has_) ev.noSeat = true;
      else if (!originIsSeat_) { ev.applyWanted = true; pendingApply_ = true; }
    }
    return ev;
  }

  SeatContext ctx_{};
  SeatWhere where_ = SeatWhere::Unknown;
  bool seen_ = false;
  bool has_ = false, originIsSeat_ = false, pendingApply_ = false;
  bool pendingStore_ = false;
  uint64_t pendingAt_ = 0;
  SeatPose pendingPrev_;
  bool windowOpen_ = false;
  uint64_t windowBegan_ = 0;
  SeatPose seat_;
};

// ---------------------------------------------------------------------------------------------------------------------------------------------
// The misalignment meter: where the head is in the seated space, in the numbers a person reads. Sitting normally in a cockpit that lines up
// reads about 0, 0, 0 (OpenVR's seated zero is the eye point). `head` and `origin` are both in natural LOCAL.
struct SeatMeter {
  double offsetCm = 0;    // horizontal distance of the head from the origin
  double heightCm = 0;    // head height above the origin (negative: below)
  double yawDeg = 0;      // the head's heading from the origin's, about +Y
};
inline SeatMeter seatMeter(const SeatPose& head, const SeatPose& origin) {
  SeatMeter m;
  const double dx = head.x - origin.x, dz = head.z - origin.z;
  m.offsetCm = std::sqrt(dx * dx + dz * dz) * 100.0;
  m.heightCm = (head.y - origin.y) * 100.0;
  m.yawDeg = seatRadToDeg(seatWrap(head.yaw - origin.yaw));
  return m;
}

}  // namespace edvr::openxr

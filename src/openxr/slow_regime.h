#pragma once
// Sustained slow frames, and who held them (docs/headset-lock-vdxr-2026-10-02.md, instrument 4).
//
// WHY. A Quest 3 user's game ran at exactly 10 fps for a minute while the headset image locked. Every frame was
// 100 ms: under the 250 ms FREEZE line, under the 150 ms stall sampler, and the LONG FRAME line says one frame in five
// seconds. Nothing in either log said "the frame rate has been a seventh of the display's for a minute, and the
// time is in the vendor's xrEndFrame". A regime of that kind is not a freeze; it is a state the session is in, and
// this is the line that says so, names who holds it, and says when it ends.
//
// WHAT. The caller feeds every frame's end (the moment the vendor's xrEndFrame returned, with the wall time that
// frame spent inside each owner's code). The frames are counted in one-second buckets. A bucket is SLOW when it holds
// fewer frames than kSlowFraction (40%) of the display rate: the vendor's half-rate reprojection mode (50%) is
// not slow, 10 fps at 72 Hz (14%) is. kSlowHoldSeconds (5) slow buckets in a row begin a regime and write the SLOW
// line, which carries the window's means by owner and the owner that holds the most:
//
//   vendor_end_frame    the vendor's xrEndFrame
//   vendor_wait_frame   the vendor's xrWaitFrame and xrBeginFrame (the pose wait), and the deferred pacer's block
//   vendor_swapchain    the vendor's xrAcquire / xrWait / xrReleaseSwapchainImage
//   edvr_copy           EDVR's copy of the eyes into the swapchain
//   edvr_work           EDVR's own work in the runtime (the treatments)
//   game                the game's own frame: whatever of the frame none of the above spent
//
// An owner is named only when it holds at least kSlowOwnerShare (35%) of the frame; otherwise the line says no
// single owner, and gives the largest. Every kSlowStillSlowSeconds (30) of a regime that goes on a still_slow line
// follows; kSlowEndSeconds (2) normal buckets in a row end it with an end line that says how long it lasted and
// carries the whole regime's means. A regime open at session close ends there.
//
// CONTEXT. The owner says whose time it was, not what the frames were. Every line also carries `context=` (scene, loading,
// no_layers or none) and the counts behind it, `loading_frames=` (frames the runtime made itself while the game loaded) and
// `empty_frames=` (ends with no layers for the vendor), so a regime the game's own frame holds can be told apart: a load
// (loading, no_layers) from a GPU- or CPU-bound stretch of scenes. The reader sets its SLOW verdict only for a vendor or EDVR owner.
//
// SHARES, NOT A PARTITION. Each owner's figure is the mean wall time per frame that frame spent inside that owner's
// code, measured where the runtime calls it. With frame_end_overlap the game and the xrEndFrame run side by side, so
// the figures can add to more than the frame; `game` is the part of the frame no other owner accounts for, and is
// never negative. A share is the figure over the frame's mean length.
//
// This header is pure: the caller owns the clock, the VRAM read and the log. observe() calls the emit it is handed
// once for each line due, at the moment it is due, with the report to format.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../common/vram_watch.h"

namespace edvr::openxr {

constexpr double kSlowFraction = 0.40;
constexpr unsigned kSlowHoldSeconds = 5;
constexpr unsigned kSlowEndSeconds = 2;
constexpr unsigned kSlowStillSlowSeconds = 30;
constexpr double kSlowOwnerShare = 0.35;
constexpr unsigned kSlowBucketMs = 1000;
// A gap of more than this many whole seconds between two frames (a suspend, a debugger) closes this many empty
// buckets and no more.
constexpr unsigned kSlowMaxCatchUp = 120;

enum class SlowOwner { None, NoFrames, VendorEndFrame, VendorWaitFrame, VendorSwapchain, EdvrCopy, EdvrWork, Game };
enum class SlowEvent { None, Slow, StillSlow, End };

inline const char* slowOwnerKey(SlowOwner o) {
  switch (o) {
    case SlowOwner::NoFrames: return "no_frames";
    case SlowOwner::VendorEndFrame: return "vendor_end_frame";
    case SlowOwner::VendorWaitFrame: return "vendor_wait_frame";
    case SlowOwner::VendorSwapchain: return "vendor_swapchain";
    case SlowOwner::EdvrCopy: return "edvr_copy";
    case SlowOwner::EdvrWork: return "edvr_work";
    case SlowOwner::Game: return "game";
    case SlowOwner::None: break;
  }
  return "none";
}

inline const char* slowOwnerText(SlowOwner o) {
  switch (o) {
    case SlowOwner::NoFrames: return "nothing: no frame reached xrEndFrame";
    case SlowOwner::VendorEndFrame: return "the vendor runtime's xrEndFrame";
    case SlowOwner::VendorWaitFrame: return "the vendor runtime's xrWaitFrame and xrBeginFrame (the pose wait)";
    case SlowOwner::VendorSwapchain: return "the vendor runtime's swapchain calls (xrAcquire, xrWait, xrReleaseSwapchainImage)";
    case SlowOwner::EdvrCopy: return "EDVR's copy of the eyes";
    case SlowOwner::EdvrWork: return "EDVR's own work in the runtime";
    case SlowOwner::Game: return "the game's own frame";
    case SlowOwner::None: break;
  }
  return "no single owner";
}

inline const char* slowEventKey(SlowEvent e) {
  switch (e) {
    case SlowEvent::Slow: return "SLOW";
    case SlowEvent::StillSlow: return "still_slow";
    case SlowEvent::End: return "end";
    case SlowEvent::None: break;
  }
  return "none";
}

// What kind of frames the window held, which the owner alone does not say: a "game" regime is a different thing when the
// runtime was making its own loading frames (a load) than when the game was submitting scenes (a GPU- or CPU-bound stretch).
// The reader (tools\edvr_log.py --freezes) sets SLOW only for a vendor or EDVR owner and reports a game-owned regime as a load
// (INFO) or a slow stretch of the game's scenes (WARN).
//   scene      at least half of the frames were the game's own, with layers for the vendor
//   loading    at least half were frames the runtime made itself while the game was loading (FrameBoundary::background)
//   no_layers  at least half ended with nothing for the vendor to show (the game submitted none)
//   none       the window held no frame
enum class SlowContext { None, Scene, Loading, NoLayers };

inline const char* slowContextKey(SlowContext c) {
  switch (c) {
    case SlowContext::Scene: return "scene";
    case SlowContext::Loading: return "loading";
    case SlowContext::NoLayers: return "no_layers";
    case SlowContext::None: break;
  }
  return "none";
}

struct SlowFrame {
  uint64_t nowMs = 0;        // a millisecond clock that only moves forward, read when xrEndFrame returned
  double refHz = 0;          // the display rate to judge by, when the vendor reported one (0 = it did not)
  double periodMs = 0;       // the vendor's last predicted display period: the display rate when refHz is 0
  bool background = false;   // a loading frame the runtime made itself (counted as a frame, its owners are not known)
  bool noLayers = false;     // the call ended with no layers for the vendor, and was not a loading frame
  double vendorWaitMs = 0, vendorEndMs = 0, vendorSwapMs = 0, copyMs = 0, edvrMs = 0;
};

// Sums over a span of buckets.
struct SlowSums {
  uint64_t frames = 0;
  uint64_t spanMs = 0;
  uint64_t loading = 0, empty = 0;   // the frames that were the runtime's own loading frames, and the ones that ended with no layers
  double wait = 0, end = 0, swap = 0, copy = 0, edvr = 0, endMax = 0;
  void add(const SlowFrame& f) {
    ++frames;
    if (f.background) ++loading;
    else if (f.noLayers) ++empty;
    wait += f.vendorWaitMs;
    end += f.vendorEndMs;
    swap += f.vendorSwapMs;
    copy += f.copyMs;
    edvr += f.edvrMs;
    if (f.vendorEndMs > endMax) endMax = f.vendorEndMs;
  }
  void merge(const SlowSums& o) {
    frames += o.frames;
    spanMs += o.spanMs;
    loading += o.loading;
    empty += o.empty;
    wait += o.wait;
    end += o.end;
    swap += o.swap;
    copy += o.copy;
    edvr += o.edvr;
    if (o.endMax > endMax) endMax = o.endMax;
  }
};

// What a line says: the means over a span, who holds the most, and where the regime stands.
struct SlowReport {
  SlowEvent event = SlowEvent::None;
  const char* reason = nullptr;     // End: "recovered" or "session_close"
  unsigned regime = 0;              // the regime's number this session
  double durationS = 0;             // how long the regime has lasted (End: all of it)
  double windowS = 0;               // the span the means below cover
  uint64_t frames = 0;
  double fps = 0, refHz = 0, fraction = 0, frameMs = 0;
  double vendorEndMs = 0, vendorWaitMs = 0, vendorSwapMs = 0, copyMs = 0, edvrMs = 0, gameMs = 0, endMaxMs = 0;
  SlowOwner owner = SlowOwner::None;
  double ownerMs = 0, ownerShare = 0, vendorShare = 0;
  SlowOwner largest = SlowOwner::None;   // when no owner reaches the share: the largest, for the line to name
  double largestShare = 0;
  SlowContext context = SlowContext::None;
  uint64_t loadingFrames = 0, emptyFrames = 0;   // of `frames`: the runtime's loading frames, and the ends with no layers
};

inline SlowReport slowReport(SlowEvent event, const SlowSums& s, double refHz, double durationS, unsigned regime, const char* reason) {
  SlowReport r;
  r.event = event;
  r.reason = reason;
  r.regime = regime;
  r.durationS = durationS;
  r.windowS = static_cast<double>(s.spanMs) / 1000.0;
  r.frames = s.frames;
  r.loadingFrames = s.loading;
  r.emptyFrames = s.empty;
  r.refHz = refHz;
  if (!s.frames || !s.spanMs) {
    r.owner = SlowOwner::NoFrames;
    return r;
  }
  // At least half decides, a loading frame first: a load is what the runtime was doing when most of the window was its own.
  r.context = s.loading * 2 >= s.frames ? SlowContext::Loading : s.empty * 2 >= s.frames ? SlowContext::NoLayers : SlowContext::Scene;
  const double n = static_cast<double>(s.frames);
  r.fps = n * 1000.0 / static_cast<double>(s.spanMs);
  r.fraction = refHz > 0 ? r.fps / refHz : 0;
  r.frameMs = static_cast<double>(s.spanMs) / n;
  r.vendorEndMs = s.end / n;
  r.vendorWaitMs = s.wait / n;
  r.vendorSwapMs = s.swap / n;
  r.copyMs = s.copy / n;
  r.edvrMs = s.edvr / n;
  r.endMaxMs = s.endMax;
  const double accounted = r.vendorEndMs + r.vendorWaitMs + r.vendorSwapMs + r.copyMs + r.edvrMs;
  r.gameMs = std::max(0.0, r.frameMs - accounted);
  struct Item { SlowOwner owner; double ms; };
  const Item items[] = {{SlowOwner::VendorEndFrame, r.vendorEndMs}, {SlowOwner::VendorWaitFrame, r.vendorWaitMs}, {SlowOwner::VendorSwapchain, r.vendorSwapMs},
                        {SlowOwner::EdvrCopy, r.copyMs}, {SlowOwner::EdvrWork, r.edvrMs}, {SlowOwner::Game, r.gameMs}};
  const Item* best = &items[0];
  for (const Item& it : items)
    if (it.ms > best->ms) best = &it;
  r.vendorShare = (r.vendorEndMs + r.vendorWaitMs + r.vendorSwapMs) / r.frameMs;
  r.largest = best->owner;
  r.largestShare = best->ms / r.frameMs;
  if (r.largestShare >= kSlowOwnerShare) {
    r.owner = best->owner;
    r.ownerMs = best->ms;
    r.ownerShare = r.largestShare;
  }
  return r;
}

// The one line, in the runtime trace's own form (name,key=value,...). `vram` is the figures the caller read at the
// moment of writing, null when it has none; the free text is last because it has spaces.
inline size_t formatSlowLine(char* buf, size_t cap, const SlowReport& r, const VramFigures* vram) {
  if (!buf || cap == 0) return 0;
  char figures[360];
  if (vram && (vram->local.valid || vram->nonLocal.valid)) formatVramFigures(figures, sizeof(figures), *vram, ',', "vram_");
  else std::snprintf(figures, sizeof(figures), "vram=unavailable");
  char summary[400];
  if (r.owner == SlowOwner::NoFrames)
    std::snprintf(summary, sizeof(summary), "held by %s in this window", slowOwnerText(r.owner));
  else if (r.owner == SlowOwner::None)
    std::snprintf(summary, sizeof(summary), "no single owner; the largest is %s at %.0f%% of a %.1f ms frame", slowOwnerText(r.largest), 100.0 * r.largestShare, r.frameMs);
  else
    std::snprintf(summary, sizeof(summary), "held by %s: %.1f ms of every %.1f ms frame (%.0f%%)", slowOwnerText(r.owner), r.ownerMs, r.frameMs, 100.0 * r.ownerShare);
  char reason[48] = "";
  if (r.event == SlowEvent::End && r.reason) std::snprintf(reason, sizeof(reason), "reason=%s,", r.reason);
  const int n = std::snprintf(
      buf, cap,
      "native_slow_regime,event=%s,%sregime=%u,duration_s=%.1f,window_s=%.1f,frames=%llu,fps=%.2f,display_hz=%.2f,fraction=%.3f,threshold=%.2f,frame_ms=%.2f,"
      "context=%s,loading_frames=%llu,empty_frames=%llu,"
      "held_by=%s,held_share=%.3f,vendor_share=%.3f,vendor_end_frame_ms=%.2f,vendor_wait_frame_ms=%.2f,vendor_swapchain_ms=%.2f,edvr_copy_ms=%.2f,"
      "edvr_work_ms=%.2f,game_ms=%.2f,end_frame_max_ms=%.2f,%s,units=wall_ms,summary=%s",
      slowEventKey(r.event), reason, r.regime, r.durationS, r.windowS, static_cast<unsigned long long>(r.frames), r.fps, r.refHz, r.fraction, kSlowFraction,
      r.frameMs, slowContextKey(r.context), static_cast<unsigned long long>(r.loadingFrames), static_cast<unsigned long long>(r.emptyFrames),
      slowOwnerKey(r.owner), r.ownerShare, r.vendorShare, r.vendorEndMs, r.vendorWaitMs, r.vendorSwapMs, r.copyMs, r.edvrMs, r.gameMs, r.endMaxMs,
      figures, summary);
  if (n < 0) {
    buf[0] = 0;
    return 0;
  }
  return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

// The line written once at startup: the proof that this instrument ran, whatever the session then does.
// `vramState` says whether the figures can be read ("available", or "unavailable" and why, without commas).
inline size_t formatSlowArmed(char* buf, size_t cap, const char* vramState) {
  if (!buf || cap == 0) return 0;
  // The state is free text with a reason in it; the line is comma-separated, so its commas become semicolons.
  char state[240];
  std::snprintf(state, sizeof(state), "%s", vramState && vramState[0] ? vramState : "unavailable");
  for (char* p = state; *p; ++p)
    if (*p == ',') *p = ';';
  const int n = std::snprintf(
      buf, cap,
      "native_slow_regime,armed=1,threshold_fraction=%.2f,of=display_rate,hold_s=%u,end_s=%u,still_slow_s=%u,owner_share=%.2f,frames=xrEndFrame_returns,"
      "owners=vendor_end_frame|vendor_wait_frame|vendor_swapchain|edvr_copy|edvr_work|game,vram=%s",
      kSlowFraction, kSlowHoldSeconds, kSlowEndSeconds, kSlowStillSlowSeconds, kSlowOwnerShare, state);
  if (n < 0) {
    buf[0] = 0;
    return 0;
  }
  return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

class SlowRegime {
 public:
  // One frame's end. `emit(const SlowReport&)` is called, in order, once for each line this frame (and the seconds
  // that passed since the last) made due.
  template <class Emit>
  void observe(const SlowFrame& f, Emit&& emit) {
    if (f.refHz > 0) refHz_ = f.refHz;
    else if (f.periodMs > 0) refHz_ = 1000.0 / f.periodMs;
    ++framesSeen_;
    if (!started_) {
      started_ = true;
      bucketStartMs_ = f.nowMs;
    }
    if (f.nowMs >= bucketStartMs_ + kSlowBucketMs) {
      unsigned closed = 0;
      while (f.nowMs >= bucketStartMs_ + kSlowBucketMs) {
        if (closed >= kSlowMaxCatchUp) {
          // Past the catch-up: jump to the second this frame is in.
          bucketStartMs_ += ((f.nowMs - bucketStartMs_) / kSlowBucketMs) * kSlowBucketMs;
          break;
        }
        closeBucket(emit);
        bucketStartMs_ += kSlowBucketMs;
        ++closed;
      }
    }
    bucket_.add(f);
  }

  // Session close: a regime still open ends here. (The partial second in progress is not counted.)
  template <class Emit>
  void finish(Emit&& emit) {
    if (!inRegime_) return;
    emit(slowReport(SlowEvent::End, regimeSums_, refHz_, durationS(), regime_, "session_close"));
    inRegime_ = false;
  }

  bool inRegime() const { return inRegime_; }
  unsigned regimes() const { return regime_; }
  uint64_t framesSeen() const { return framesSeen_; }
  uint64_t secondsClosed() const { return secondsClosed_; }
  uint64_t slowSeconds() const { return slowSeconds_; }

 private:
  double durationS() const { return static_cast<double>(lastSlowEndMs_ - regimeStartMs_) / 1000.0; }

  template <class Emit>
  void closeBucket(Emit& emit) {
    bucket_.spanMs = kSlowBucketMs;
    const bool slow = refHz_ > 0 && static_cast<double>(bucket_.frames) < kSlowFraction * refHz_;
    ++secondsClosed_;
    if (slow) ++slowSeconds_;
    if (!inRegime_) {
      if (slow) {
        run_.merge(bucket_);
        if (++runBuckets_ == 1) regimeStartMs_ = bucketStartMs_;
        lastSlowEndMs_ = bucketStartMs_ + kSlowBucketMs;
        if (runBuckets_ >= kSlowHoldSeconds) {
          inRegime_ = true;
          ++regime_;
          regimeSums_ = run_;
          windowSums_ = SlowSums{};
          windowBuckets_ = 0;
          pending_ = SlowSums{};
          normalBuckets_ = 0;
          emit(slowReport(SlowEvent::Slow, run_, refHz_, durationS(), regime_, nullptr));
          run_ = SlowSums{};
          runBuckets_ = 0;
        }
      } else {
        run_ = SlowSums{};
        runBuckets_ = 0;
      }
    } else if (slow) {
      // Normal seconds that came between slow ones belong to the regime: fold them in first.
      if (normalBuckets_) {
        regimeSums_.merge(pending_);
        windowSums_.merge(pending_);
        windowBuckets_ += normalBuckets_;
        pending_ = SlowSums{};
        normalBuckets_ = 0;
      }
      regimeSums_.merge(bucket_);
      windowSums_.merge(bucket_);
      ++windowBuckets_;
      lastSlowEndMs_ = bucketStartMs_ + kSlowBucketMs;
      if (windowBuckets_ >= kSlowStillSlowSeconds) {
        emit(slowReport(SlowEvent::StillSlow, windowSums_, refHz_, durationS(), regime_, nullptr));
        windowSums_ = SlowSums{};
        windowBuckets_ = 0;
      }
    } else {
      pending_.merge(bucket_);
      if (++normalBuckets_ >= kSlowEndSeconds) {
        emit(slowReport(SlowEvent::End, regimeSums_, refHz_, durationS(), regime_, "recovered"));
        inRegime_ = false;
        pending_ = SlowSums{};
        normalBuckets_ = 0;
      }
    }
    bucket_ = SlowSums{};
  }

  bool started_ = false, inRegime_ = false;
  uint64_t bucketStartMs_ = 0, regimeStartMs_ = 0, lastSlowEndMs_ = 0;
  double refHz_ = 0;
  SlowSums bucket_, run_, regimeSums_, windowSums_, pending_;
  unsigned runBuckets_ = 0, windowBuckets_ = 0, normalBuckets_ = 0, regime_ = 0;
  uint64_t framesSeen_ = 0, secondsClosed_ = 0, slowSeconds_ = 0;
};

// The counts, written at close and every five minutes: a session with no slow regime says so in zeros, which is the
// proof the detector ran (the armed line says it started; this says it saw frames).
inline size_t formatSlowSummary(char* buf, size_t cap, const char* reason, const SlowRegime& s) {
  if (!buf || cap == 0) return 0;
  const int n = std::snprintf(buf, cap, "native_slow_regime_summary,%s%s%sregimes=%u,open=%u,frames=%llu,seconds=%llu,slow_seconds=%llu,threshold=%.2f",
                              reason ? "reason=" : "", reason ? reason : "", reason ? "," : "", s.regimes(), s.inRegime() ? 1u : 0u,
                              static_cast<unsigned long long>(s.framesSeen()), static_cast<unsigned long long>(s.secondsClosed()),
                              static_cast<unsigned long long>(s.slowSeconds()), kSlowFraction);
  if (n < 0) {
    buf[0] = 0;
    return 0;
  }
  return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

}  // namespace edvr::openxr

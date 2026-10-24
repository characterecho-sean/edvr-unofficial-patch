#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include "../common/call_probe_budget.h"
#include "owner_invoke_stats.h"

namespace edvr::openxr {

// Where an owner job comes from. OwnerCallKind (owner_invoke_stats.h) is the VR frame-cycle view and lumps every unremarkable hop under
// "other"; the flat instrument needs those told apart, and the jobs queued with OwnerService::submit (no caller wait) named too.
enum class OwnerJobSite : uint8_t {
  WaitPoses = 0, LocateHead, PollEvent, SubmitHandoff, SetTrackingSpace, ResetSeated, ClearSubmitted, ClearSkybox,
  FrameEndFinish, HandoffAsync, Other, Count,
  Auto = 0xFF   // invoke only: take the site from the OwnerCallKind
};

inline OwnerJobSite ownerJobSiteForKind(OwnerCallKind kind) {
  switch (kind) {
    case OwnerCallKind::WaitPoses: return OwnerJobSite::WaitPoses;
    case OwnerCallKind::LocateHead: return OwnerJobSite::LocateHead;
    case OwnerCallKind::PollEvent: return OwnerJobSite::PollEvent;
    case OwnerCallKind::Submit: return OwnerJobSite::SubmitHandoff;
    default: return OwnerJobSite::Other;
  }
}

inline const char* ownerJobSiteName(OwnerJobSite site) {
  switch (site) {
    case OwnerJobSite::WaitPoses: return "wait_poses";
    case OwnerJobSite::LocateHead: return "locate_head";
    case OwnerJobSite::PollEvent: return "poll_event";
    case OwnerJobSite::SubmitHandoff: return "submit_handoff";
    case OwnerJobSite::SetTrackingSpace: return "set_tracking_space";
    case OwnerJobSite::ResetSeated: return "reset_seated";
    case OwnerJobSite::ClearSubmitted: return "clear_submitted";
    case OwnerJobSite::ClearSkybox: return "clear_skybox";
    case OwnerJobSite::FrameEndFinish: return "frame_end_finish";
    case OwnerJobSite::HandoffAsync: return "handoff_async";
    default: return "other";
  }
}

struct FlatOwnerCallerStack {
  unsigned captured = 0, gameFrames = 0;
  char rvas[512]{};
};

// What the owner thread's jobs cost the game while the graphics half reads flat. In flat the owner's idle pump runs an empty headset frame
// (flatClearFrame: xrWaitFrame, paced by the headset), and a game call that hops to the owner queues behind that pump. Every number here is
// a measurement of that queue: no behaviour reads any of it.
//
// A job is tracked when flat() is true at its enqueue. It records the enqueue instant, the start and finish on the owner, whether it was
// enqueued while an idle pump (behind_pump) or a flat-clear frame (behind_clear) was in progress or one began before it started, and
// whether its caller was the game's render thread. The queue wait is start - enqueue. For OwnerService::invoke the caller is blocked for
// queue + run, and that total goes to the render-thread or other-thread sum; a submit job has no caller wait and only counts.
//
// The window line (flat_owner_waits) prints every five seconds while flat, from the owner thread's loop, whether or not a job waited:
// no line means the instrument never ran, zeros mean it ran and nothing hopped. The caller line (flat_owner_wait_caller) names the game
// call stack of a job that waited kCallerWaitMs or more behind a clear frame, from the thread that enqueued it.
class FlatOwnerWaits final {
 public:
  static constexpr uint64_t kWindowUs = 5000000;
  static constexpr int64_t kCallerWaitUs = 10000;
  static constexpr unsigned kSites = unsigned(OwnerJobSite::Count);
  static constexpr unsigned kSamples = 256;   // per site per window, for the percentiles; counts and sums stay exact
  static constexpr unsigned kCallerCaptures = 16;
  static constexpr size_t kLineCap = 2560;

  // Plain functions so the instrument holds no pointer into the host. All but stack and print are required.
  struct Hooks {
    uint64_t (*nowUs)() = nullptr;
    bool (*flat)() = nullptr;                          // the graphics half reads flat (!vrProfileActive)
    uint32_t (*thread)() = nullptr;                    // the calling thread's id
    void (*stack)(FlatOwnerCallerStack&) = nullptr;    // the calling thread's game frames
    void (*print)(const char* line) = nullptr;         // a line ending in a newline
  };

  // One owner job's record. Written by the enqueuing thread, then by the owner (started, finished); the caller reads it after the request's
  // own mutex handed the completion over, as OwnerService's other per-request timings.
  struct Ticket {
    bool tracked = false, ran = false, viaInvoke = false, behindClear = false, behindPump = false, renderThread = false;
    bool clearAtEnqueue = false, pumpAtEnqueue = false;
    OwnerJobSite site = OwnerJobSite::Other;
    uint64_t enqueuedUs = 0, startedUs = 0, finishedUs = 0, clearSeq = 0, pumpSeq = 0;
    std::unique_ptr<FlatOwnerCallerStack> stack;
  };

  struct SiteReport {
    uint64_t n = 0, behindClear = 0;
    double p50Ms = 0, p95Ms = 0, maxMs = 0, sumMs = 0, runMaxMs = 0;
  };
  // Why a flat head locate was not answered from the pose publication (pose_cache.h answerFlat) and took the owner hop.
  enum class CacheFallback : unsigned { Unpublished = 0, Keys, Stale, Count };
  struct Snapshot {
    uint64_t pumps = 0, clearFrames = 0, jobs = 0, behindClear = 0, behindPump = 0, localPose = 0, localPoll = 0;
    double pumpMaxMs = 0, clearWaitSumMs = 0, clearWaitMaxMs = 0, renderWaitSumMs = 0, otherWaitSumMs = 0;
    // The flat head locate: answered from the owner's per-headset-frame publication, hopped because it could not be, or refused (not seated).
    uint64_t cacheAnswers = 0, cacheFallbacks = 0, cacheRefused = 0, cacheFallbackReasons[unsigned(CacheFallback::Count)] = {};
    double cacheAgeP50Ms = 0, cacheAgeP95Ms = 0, cacheAgeMaxMs = 0;
    SiteReport sites[kSites];
  };

  explicit FlatOwnerWaits(const Hooks& hooks) : hooks_(hooks) {}
  FlatOwnerWaits(const FlatOwnerWaits&) = delete;
  FlatOwnerWaits& operator=(const FlatOwnerWaits&) = delete;

  void bindRenderThread(uint32_t id) { renderThread_.store(id, std::memory_order_release); }
  bool tracking() const { return hooks_.nowUs && hooks_.flat && hooks_.thread && hooks_.flat(); }

  // The idle pump (OwnerService::run). idleBegin runs under the service mutex that picked the pump, so a job pushed afterwards sees it.
  void idleBegin() {
    pumpSeq_.fetch_add(1, std::memory_order_relaxed);
    state_.fetch_or(kPump, std::memory_order_release);
    pumpBeganUs_ = tracking() ? hooks_.nowUs() : 0;
  }
  void idleEnd() {
    state_.fetch_and(~kPump, std::memory_order_release);
    if (!pumpBeganUs_) return;
    const uint64_t now = hooks_.nowUs();
    const double ms = now > pumpBeganUs_ ? double(now - pumpBeganUs_) / 1000.0 : 0.0;
    pumpBeganUs_ = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    ++w_.pumps; w_.pumpMaxMs = (std::max)(w_.pumpMaxMs, ms);
  }

  // The flat-clear frame, around flatClearFrame on the owner. noteClearWait is the xrWaitFrame inside it.
  void clearEnter() {
    clearSeq_.fetch_add(1, std::memory_order_relaxed);
    state_.fetch_or(kClear, std::memory_order_release);
  }
  void clearLeave() { state_.fetch_and(~kClear, std::memory_order_release); }
  struct ClearScope {
    explicit ClearScope(FlatOwnerWaits* w) : w_(w) { if (w_) w_->clearEnter(); }
    ~ClearScope() { if (w_) w_->clearLeave(); }
    ClearScope(const ClearScope&) = delete;
    ClearScope& operator=(const ClearScope&) = delete;
   private:
    FlatOwnerWaits* w_;
  };
  void noteClearWait(uint64_t us) {
    const double ms = double(us) / 1000.0;
    std::lock_guard<std::mutex> lock(mutex_);
    ++w_.clearFrames; w_.clearWaitSumMs += ms; w_.clearWaitMaxMs = (std::max)(w_.clearWaitMaxMs, ms);
  }

  // Whether the graphics half reads flat now: the route the host takes for a head locate (the same test the instrument itself uses).
  bool flatNow() const { return hooks_.flat && hooks_.flat(); }
  // A flat head locate answered from the publication, at the publication's age; hopped because it could not be; refused (not the seated origin).
  void noteFlatCacheAnswer(uint64_t ageUs) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++w_.cacheAnswers;
    w_.cacheAgeMaxMs = (std::max)(w_.cacheAgeMaxMs, double(ageUs) / 1000.0);
    if (ages_.stored < kAgeSamples) ages_.us[ages_.stored++] = int64_t(ageUs);
  }
  void noteFlatCacheFallback(CacheFallback why) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++w_.cacheFallbacks;
    if (unsigned(why) < unsigned(CacheFallback::Count)) ++w_.cacheFallbackReasons[unsigned(why)];
  }
  void noteFlatCacheRefused() { std::lock_guard<std::mutex> lock(mutex_); ++w_.cacheRefused; }

  // A game call answered without a hop, in the same window.
  void noteLocalPose() { if (hooks_.flat && hooks_.flat()) localPose_.fetch_add(1, std::memory_order_relaxed); }
  void noteLocalPoll() { if (hooks_.flat && hooks_.flat()) localPoll_.fetch_add(1, std::memory_order_relaxed); }

  // The enqueuing thread, just before the job is pushed. Leaves the ticket untracked outside flat.
  void enqueue(Ticket& t, OwnerJobSite site, bool viaInvoke) {
    t.tracked = false;
    if (!tracking()) return;
    t.site = site < OwnerJobSite::Count ? site : OwnerJobSite::Other;
    t.viaInvoke = viaInvoke;
    const uint32_t state = state_.load(std::memory_order_acquire);
    t.clearAtEnqueue = (state & kClear) != 0;
    t.pumpAtEnqueue = (state & kPump) != 0;
    t.clearSeq = clearSeq_.load(std::memory_order_relaxed);
    t.pumpSeq = pumpSeq_.load(std::memory_order_relaxed);
    const uint32_t render = renderThread_.load(std::memory_order_acquire);
    t.renderThread = render != 0 && hooks_.thread() == render;
    // The stack is taken here, on the caller's thread, only when a clear frame is already in progress and the budget of reports is not
    // spent: the job cannot know yet whether it will wait, and a capture is not free.
    if (t.clearAtEnqueue && hooks_.stack && callerReports_.load(std::memory_order_relaxed) < kCallerCaptures) {
      try {
        t.stack = std::make_unique<FlatOwnerCallerStack>();
        hooks_.stack(*t.stack);
      } catch (...) { t.stack.reset(); }
    }
    t.enqueuedUs = hooks_.nowUs();
    t.tracked = true;
  }
  // The owner thread, when it picks the job up and when its body returns.
  void started(Ticket& t) {
    t.startedUs = hooks_.nowUs();
    t.ran = true;
    t.behindClear = t.clearAtEnqueue || clearSeq_.load(std::memory_order_relaxed) != t.clearSeq;
    t.behindPump = t.pumpAtEnqueue || pumpSeq_.load(std::memory_order_relaxed) != t.pumpSeq;
  }
  void finished(Ticket& t) { t.finishedUs = hooks_.nowUs(); }

  // Folds a finished job into the window; the caller thread for an invoke, the owner for a submit job. Prints the caller line when the
  // job waited behind a clear frame long enough.
  void note(const Ticket& t) {
    if (!t.tracked || !t.ran) return;
    const int64_t queueUs = int64_t(t.startedUs) - int64_t(t.enqueuedUs);
    const int64_t runUs = int64_t(t.finishedUs) - int64_t(t.startedUs);
    const double queueMs = double(queueUs) / 1000.0, runMs = double(runUs) / 1000.0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++w_.jobs;
      if (t.behindClear) ++w_.behindClear;
      if (t.behindPump) ++w_.behindPump;
      Site& s = sites_[unsigned(t.site)];
      SiteReport& r = w_.sites[unsigned(t.site)];
      if (s.stored < kSamples) s.queueUs[s.stored++] = queueUs;
      ++r.n;
      if (t.behindClear) ++r.behindClear;
      r.maxMs = (std::max)(r.maxMs, queueMs); r.sumMs += queueMs; r.runMaxMs = (std::max)(r.runMaxMs, runMs);
      if (t.viaInvoke) (t.renderThread ? w_.renderWaitSumMs : w_.otherWaitSumMs) += queueMs + runMs;
    }
    if (t.behindClear && queueUs >= kCallerWaitUs && hooks_.print) {
      const unsigned sample = budget_.take(hooks_.nowUs() / 1000);
      if (sample) {
        callerReports_.fetch_add(1, std::memory_order_relaxed);
        char line[1024]{};
        const FlatOwnerCallerStack* st = t.stack.get();
        const int n = formatFlatOwnerCallerLine(t, queueMs, sample, st, line, sizeof line);
        if (n > 0) hooks_.print(line);
      }
    }
  }

  // The owner thread's loop, every iteration. Prints the window line once kWindowUs has passed while flat; leaving flat drops the
  // partial window, so a stretch starts clean.
  void tick() {
    if (!tracking()) {
      if (windowBeganUs_) { windowBeganUs_ = 0; (void)take(); }
      return;
    }
    const uint64_t now = hooks_.nowUs();
    if (!windowBeganUs_) { windowBeganUs_ = now; return; }
    if (now < windowBeganUs_ || now - windowBeganUs_ < kWindowUs) return;
    const double elapsedMs = double(now - windowBeganUs_) / 1000.0;
    windowBeganUs_ = now;
    const Snapshot snap = take();
    ++windowNumber_;
    char line[kLineCap]{};
    if (hooks_.print && formatFlatOwnerWaitsLine(windowNumber_, elapsedMs, snap, line, sizeof line)) hooks_.print(line);
  }

  // Drains the window. Not for the hot path.
  Snapshot take() {
    Snapshot out;
    std::lock_guard<std::mutex> lock(mutex_);
    out = w_;
    for (unsigned k = 0; k < kSites; ++k) {
      Site& s = sites_[k];
      if (s.stored) {
        out.sites[k].p50Ms = double(pick(s.queueUs, s.stored, 50)) / 1000.0;
        out.sites[k].p95Ms = double(pick(s.queueUs, s.stored, 95)) / 1000.0;
      }
      s.stored = 0;
    }
    if (ages_.stored) {
      out.cacheAgeP50Ms = double(pick(ages_.us, ages_.stored, 50)) / 1000.0;
      out.cacheAgeP95Ms = double(pick(ages_.us, ages_.stored, 95)) / 1000.0;
    }
    ages_.stored = 0;
    out.localPose = localPose_.exchange(0, std::memory_order_relaxed);
    out.localPoll = localPoll_.exchange(0, std::memory_order_relaxed);
    w_ = Snapshot{};
    return out;
  }

  // flat_owner_waits,window=N,window_s=5,elapsed_ms=E,idle_pumps=P,idle_pump_ms_max=M,clear_frames=C,clear_wait_ms_mean=a,clear_wait_ms_max=b,
  //   jobs=J,behind_clear=B,behind_pump=BP,render_thread_wait_ms_sum=r,other_thread_wait_ms_sum=o,local_pose_cache=L,local_poll_event=Q,
  //   flat_cache_answers=A,flat_cache_fallbacks=F,flat_cache_fallback_reasons=unpublished:a/keys:b/stale:c,flat_cache_refused=R,
  //   flat_cache_age_ms_p50/p95/max=x/y/z,kinds=site:n/behind_clear/queue_p50/queue_p95/queue_max/queue_sum/run_max|...
  // Every site prints, with explicit zeros. ms with one decimal. Returns the length written, or 0 when the buffer is too small.
  static int formatFlatOwnerWaitsLine(uint64_t window, double elapsedMs, const Snapshot& s, char* buf, size_t cap) {
    if (!buf || cap < 2) return 0;
    const double mean = s.clearFrames ? s.clearWaitSumMs / double(s.clearFrames) : 0.0;
    int n = std::snprintf(buf, cap,
        "flat_owner_waits,window=%llu,window_s=%llu,elapsed_ms=%.0f,idle_pumps=%llu,idle_pump_ms_max=%.1f,clear_frames=%llu,"
        "clear_wait_ms_mean=%.1f,clear_wait_ms_max=%.1f,jobs=%llu,behind_clear=%llu,behind_pump=%llu,"
        "render_thread_wait_ms_sum=%.1f,other_thread_wait_ms_sum=%.1f,local_pose_cache=%llu,local_poll_event=%llu,"
        "flat_cache_answers=%llu,flat_cache_fallbacks=%llu,flat_cache_fallback_reasons=unpublished:%llu/keys:%llu/stale:%llu,"
        "flat_cache_refused=%llu,flat_cache_age_ms_p50/p95/max=%.1f/%.1f/%.1f,kinds=",
        ull(window), ull(kWindowUs / 1000000), elapsedMs, ull(s.pumps), s.pumpMaxMs, ull(s.clearFrames), mean, s.clearWaitMaxMs,
        ull(s.jobs), ull(s.behindClear), ull(s.behindPump), s.renderWaitSumMs, s.otherWaitSumMs, ull(s.localPose), ull(s.localPoll),
        ull(s.cacheAnswers), ull(s.cacheFallbacks), ull(s.cacheFallbackReasons[0]), ull(s.cacheFallbackReasons[1]),
        ull(s.cacheFallbackReasons[2]), ull(s.cacheRefused), s.cacheAgeP50Ms, s.cacheAgeP95Ms, s.cacheAgeMaxMs);
    if (n < 0 || size_t(n) >= cap) return 0;
    for (unsigned k = 0; k < kSites; ++k) {
      const SiteReport& r = s.sites[k];
      const int w = std::snprintf(buf + n, cap - size_t(n), "%s%s:%llu/%llu/%.1f/%.1f/%.1f/%.1f/%.1f", k ? "|" : "",
          ownerJobSiteName(OwnerJobSite(k)), ull(r.n), ull(r.behindClear), r.p50Ms, r.p95Ms, r.maxMs, r.sumMs, r.runMaxMs);
      if (w < 0 || size_t(w) >= cap - size_t(n)) return 0;
      n += w;
    }
    if (size_t(n) + 2 > cap) return 0;
    buf[n++] = '\n'; buf[n] = 0;
    return n;
  }
  // flat_owner_wait_caller,kind=K,wait_ms=W,run_ms=R,thread=render|other,via=invoke|submit,sample=S/16,stack=0|1,game_frames=N,game_rvas=...
  static int formatFlatOwnerCallerLine(const Ticket& t, double waitMs, unsigned sample, const FlatOwnerCallerStack* st, char* buf, size_t cap) {
    const double runMs = double(int64_t(t.finishedUs) - int64_t(t.startedUs)) / 1000.0;
    const int n = std::snprintf(buf, cap,
        "flat_owner_wait_caller,kind=%s,wait_ms=%.3f,run_ms=%.3f,thread=%s,via=%s,sample=%u/%u,stack=%u,game_frames=%u,game_rvas=%s\n",
        ownerJobSiteName(t.site), waitMs, runMs, t.renderThread ? "render" : "other", t.viaInvoke ? "invoke" : "submit", sample,
        kCallerCaptures, st ? 1u : 0u, st ? st->gameFrames : 0u, st ? st->rvas : "");
    return n > 0 && size_t(n) < cap ? n : 0;
  }

 private:
  static constexpr uint32_t kPump = 1, kClear = 2;
  struct Site { unsigned stored = 0; int64_t queueUs[kSamples]; };
  static constexpr unsigned kAgeSamples = 1024;   // flat_cache ages per window: ~175 answers per 5 s at one locate per game frame
  struct Ages { unsigned stored = 0; int64_t us[kAgeSamples]; };
  static unsigned long long ull(uint64_t v) { return static_cast<unsigned long long>(v); }
  // Nearest rank, as OwnerInvokeStats: v[(n*pct+99)/100 - 1]. Sorts the first n.
  static int64_t pick(int64_t* v, unsigned n, unsigned pct) {
    std::sort(v, v + n);
    return v[(size_t(n) * pct + 99) / 100 - 1];
  }

  const Hooks hooks_;
  std::atomic<uint32_t> state_{0}, renderThread_{0};
  std::atomic<uint64_t> clearSeq_{0}, pumpSeq_{0}, localPose_{0}, localPoll_{0};
  std::atomic<unsigned> callerReports_{0};
  edvr::GameCallProbeBudget budget_;
  uint64_t pumpBeganUs_ = 0;                // owner thread only
  uint64_t windowBeganUs_ = 0, windowNumber_ = 0;   // owner thread only
  std::mutex mutex_;
  Snapshot w_;
  Site sites_[kSites];
  Ages ages_;
};

} // namespace edvr::openxr

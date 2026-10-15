#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

namespace edvr::openxr {

// Per-kind accounting of the caller's wait for OwnerService::invoke, per frame-cycle window (native_owner_invoke). The
// caller's wall time is split at the owner's start of the job:
//   queue = the owner starts the job  - the caller enqueued it
//   run   = the job's body returns    - the owner starts it
// behind_wait counts the requests that arrived while the owner was running a waitPoses job. The owner records
// its kind and start/finish instants on the request; the caller folds them in after its wait returns. Only
// requests that ran are counted: a rejected one never reached the owner, a cancelled one never started.
//
// Kinds are the host's entry points: waitPoses (xrWaitFrame and its begin), locateHead (GetDeviceToAbsoluteTrackingPose),
// pollEvent (PollNextEvent), submit (the synchronous Submit handoff), other (everything else that hops to the owner).
// The overlapped frame-end jobs (OwnerService::submit) have no caller wait and are not here.
//
// The counters are exact; the percentiles are over the first `capacity` samples of the window, and `dropped`
// counts the rest. Nearest-rank percentiles, as FrameCycleStats::dist. A kind with no sample prints na.
enum class OwnerCallKind : uint8_t { WaitPoses = 0, LocateHead, PollEvent, Submit, Other, Count };

class OwnerInvokeStats final {
 public:
  static constexpr unsigned capacity = 4096;
  static constexpr unsigned kinds = unsigned(OwnerCallKind::Count);

  struct Sample {
    OwnerCallKind kind = OwnerCallKind::Other;
    bool behindWait = false;
    int64_t queueUs = 0, runUs = 0;
  };

  struct KindReport {
    uint64_t calls = 0, behindWait = 0;
    unsigned dropped = 0;
    bool queueValid = false, runValid = false;
    double queueP50Ms = 0, queueP95Ms = 0, runP50Ms = 0, runP95Ms = 0;
  };

  // Caller thread, after the owner has finished the job. Thread-safe.
  void note(const Sample& s) {
    std::lock_guard<std::mutex> lock(mutex_);
    Window& w = windows_[unsigned(s.kind)];
    ++w.calls;
    if (s.behindWait) ++w.behindWait;
    if (w.queue.size() < capacity) { w.queue.push_back(s.queueUs); w.run.push_back(s.runUs); }
    else ++w.dropped;
  }

  // Fills out[kinds] with the window's figures and clears them.
  void takeWindow(KindReport (&out)[kinds]) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (unsigned k = 0; k < kinds; ++k) {
      Window& w = windows_[k];
      KindReport& r = out[k];
      r = KindReport{};
      r.calls = w.calls;
      r.behindWait = w.behindWait;
      r.dropped = w.dropped;
      if (!w.queue.empty()) {
        r.queueValid = true;
        r.queueP50Ms = ms(pick(w.queue, 50)); r.queueP95Ms = ms(pick(w.queue, 95));
      }
      if (!w.run.empty()) {
        r.runValid = true;
        r.runP50Ms = ms(pick(w.run, 50)); r.runP95Ms = ms(pick(w.run, 95));
      }
      w = Window{};
    }
  }

  static const char* kindName(OwnerCallKind k) {
    switch (k) {
      case OwnerCallKind::WaitPoses: return "waitPoses";
      case OwnerCallKind::LocateHead: return "locateHead";
      case OwnerCallKind::PollEvent: return "pollEvent";
      case OwnerCallKind::Submit: return "submit";
      default: return "other";
    }
  }

 private:
  struct Window {
    uint64_t calls = 0, behindWait = 0;
    unsigned dropped = 0;
    std::vector<int64_t> queue, run;
  };
  // Nearest rank, as FrameCycleStats::dist: v[(n*pct+99)/100 - 1]. Sorts v.
  static int64_t pick(std::vector<int64_t>& v, unsigned pct) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return v[(n * pct + 99) / 100 - 1];
  }
  static double ms(int64_t us) { return double(us) / 1000.0; }

  std::mutex mutex_;
  Window windows_[kinds];
};

// native_owner_invoke,window=N,kind=K,calls=C,per_frame=P,queue_p50/p95=a/b,run_p50/p95=c/d,behind_wait=B,dropped=D
// ms with three decimals; na where a kind has no sample (queue, run) or the window had no valid frame (per_frame).
// Returns the length written, or 0 when the buffer is too small.
inline int formatOwnerInvokeLine(uint64_t window, OwnerCallKind kind, const OwnerInvokeStats::KindReport& r,
                                 unsigned frames, char* buf, size_t cap) {
  char perFrame[24], q50[24], q95[24], r50[24], r95[24];
  if (frames) std::snprintf(perFrame, sizeof perFrame, "%.3f", double(r.calls) / double(frames));
  else std::snprintf(perFrame, sizeof perFrame, "na");
  auto put = [](char* dst, size_t n, bool valid, double v) {
    if (valid) std::snprintf(dst, n, "%.3f", v); else std::snprintf(dst, n, "na");
  };
  put(q50, sizeof q50, r.queueValid, r.queueP50Ms);
  put(q95, sizeof q95, r.queueValid, r.queueP95Ms);
  put(r50, sizeof r50, r.runValid, r.runP50Ms);
  put(r95, sizeof r95, r.runValid, r.runP95Ms);
  const int n = std::snprintf(buf, cap,
      "native_owner_invoke,window=%llu,kind=%s,calls=%llu,per_frame=%s,queue_p50/p95=%s/%s,run_p50/p95=%s/%s,"
      "behind_wait=%llu,dropped=%u\n",
      static_cast<unsigned long long>(window), OwnerInvokeStats::kindName(kind),
      static_cast<unsigned long long>(r.calls), perFrame, q50, q95, r50, r95,
      static_cast<unsigned long long>(r.behindWait), r.dropped);
  return n > 0 && size_t(n) < cap ? n : 0;
}

} // namespace edvr::openxr

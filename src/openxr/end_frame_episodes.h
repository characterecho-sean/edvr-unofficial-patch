#pragma once
// Long xrEndFrame calls, one episode at a time (docs/headset-lock-vdxr-2026-10-02.md, instrument 3).
//
// WHY. A Quest 3 user's game ran at exactly 10 fps for a minute with every frame's xrEndFrame held about 83 ms (six
// display periods at 72 Hz) inside Virtual Desktop's OpenXR runtime. The freeze diagnostics saw nothing: a frame
// of 100 ms is under the 250 ms FREEZE line and the 150 ms stall sampler, and the LONG FRAME line is limited to one
// every five seconds. The runtime's own submit-phase windows did hold it, as a p50 over 256 frames every 30 s, with
// no word of which call, when it began or what the session was doing. This is the call by call account.
//
// WHAT. An xrEndFrame of kEndFramePeriods display periods or more starts an EPISODE. The first call is written on
// its own line with what the runtime knew about it: the frame's sequence, how long, in periods, the predicted period,
// whether the vendor said to render, the frame's own swapchain acquire / wait / draw / release and copy times, the
// path it took (inside Submit, queued behind it, or a loading frame) and the pacing, and the vendor's last session
// state with its age. While the episode lasts every call is counted. After kEndFrameEndAfterNormal calls in a row
// that are under the threshold it ends, and one line says how many calls were slow, how long it lasted, and their
// p50 and max. An episode still open at session close ends there.
//
// RATE LIMIT. Start lines: at most one per kEndFrameStartLineEveryMs and kEndFrameStartLineCap a session, so a
// vendor that stutters every few seconds writes a line for the first of each burst and no more. An episode whose
// start line was held back still counts, and still gets its end line when it was long (kEndFrameEndLineMinSlow slow
// calls), so a long episode is never invisible; the shorter ones are in the summary's counts.
//
// The caller (the host, from FrameBoundary's end-of-call hook, on the owner thread) feeds every end-frame call here,
// ALL paths: the synchronous one inside the second Submit, the overlapped one the owner runs after Submit returned,
// the deferred-pacing (turbo) frames and the loading frames the runtime makes itself. This header is pure: no clock
// of its own, no OS call, no log. The percentiles come from a fixed histogram so a day-long episode costs nothing.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr::openxr {

constexpr double kEndFramePeriods = 3.0;
constexpr unsigned kEndFrameEndAfterNormal = 8;
constexpr uint64_t kEndFrameStartLineEveryMs = 2000;
constexpr unsigned kEndFrameStartLineCap = 100;
constexpr unsigned kEndFrameEndLineMinSlow = 20;

struct EndFrameCall {
  uint64_t sequence = 0;       // the runtime's frame sequence (the number native_long_cycle prints); 0 when the frame has none
  double ms = 0;               // the timed region: xrEndFrame, and anything held inside it
  double periodMs = 0;         // the last real predicted display period (0 until the vendor has told us one)
  uint64_t nowMs = 0;          // a millisecond clock that only moves forward, read when the call returned
  bool shouldRender = false;   // the vendor's own answer for the frame
  bool layers = false;         // layers went to the vendor with the call
  bool background = false;     // a loading frame the runtime made itself
  bool turbo = false;          // deferred pacing (the wait ran on the pacer thread)
  bool overlapped = false;     // the call ran on the owner after Submit had returned (frame_end_overlap)
  int result = 0;              // XrResult
  double acquireMs = 0, waitMs = 0, drawMs = 0, releaseMs = 0;   // that frame's swapchain calls (0 for a loading frame)
  double copyMs = 0;           // that frame's copy of the eyes, producer and consumer
  const char* sessionState = "UNKNOWN";
  double stateAgeMs = -1;      // how long ago the session state last changed (-1: not known)
};

class EndFrameEpisodes {
 public:
  static constexpr unsigned kBins = 1024;       // 0.5 ms each: 0 to 512 ms, the last bin holds everything longer
  static constexpr double kBinMs = 0.5;

  struct Lines {                                // what to write after one call; the lengths are 0 for "nothing"
    size_t startLen = 0, endLen = 0;
    char start[720];
    char end[720];
  };
  struct Summary {
    uint64_t calls = 0, slowCalls = 0, episodes = 0, startLines = 0, endLines = 0;
    double longestMs = 0;
  };

  // One end-frame call. Fills `out` (always: both lengths are reset). Never both lines from one call: an episode
  // ends on a normal call and starts on a slow one.
  void observe(const EndFrameCall& c, Lines& out) {
    out.startLen = out.endLen = 0;
    out.start[0] = out.end[0] = 0;
    ++summary_.calls;
    const bool slow = c.periodMs > 0 && std::isfinite(c.ms) && c.ms >= kEndFramePeriods * c.periodMs;
    if (slow) {
      ++summary_.slowCalls;
      if (c.ms > summary_.longestMs) summary_.longestMs = c.ms;
    }
    if (!open_) {
      if (!slow) return;
      begin(c, out);
      return;
    }
    ++calls_;
    if (slow) {
      ++count_;
      normalRun_ = 0;
      add(c);
      lastSlowEndMs_ = c.nowMs;
      lastSequence_ = c.sequence;
      return;
    }
    if (++normalRun_ >= kEndFrameEndAfterNormal) close("normal_calls", out);
  }

  // Session close: an episode still open ends here.
  void finish(Lines& out) {
    out.startLen = out.endLen = 0;
    out.start[0] = out.end[0] = 0;
    if (open_) close("session_close", out);
  }

  const Summary& summary() const { return summary_; }
  bool open() const { return open_; }

  size_t formatSummary(char* buf, size_t cap, const char* reason) const {
    if (!buf || cap == 0) return 0;
    return fit(buf, cap,
               std::snprintf(buf, cap,
                             "native_end_frame_episodes_summary,%s%s%sthreshold_periods=%.1f,episodes=%llu,start_lines=%llu,end_lines=%llu,calls=%llu,slow_calls=%llu,"
                             "longest_ms=%.4f,open=%u,units=wall_ms",
                             reason ? "reason=" : "", reason ? reason : "", reason ? "," : "", kEndFramePeriods,
                             static_cast<unsigned long long>(summary_.episodes), static_cast<unsigned long long>(summary_.startLines),
                             static_cast<unsigned long long>(summary_.endLines), static_cast<unsigned long long>(summary_.calls),
                             static_cast<unsigned long long>(summary_.slowCalls), summary_.longestMs, open_ ? 1u : 0u));
  }

  // The line written once at startup: the proof that this instrument ran, whatever the session then does.
  static size_t formatArmed(char* buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    return fit(buf, cap,
               std::snprintf(buf, cap,
                             "native_end_frame_episodes,armed=1,threshold_periods=%.1f,period=last_real_predicted_display_period,end_after_normal_calls=%u,"
                             "start_lines=1_per_%llu_ms_max_%u,long_episode_end_line_from_slow_calls=%u,paths=synchronous|overlapped|loading,units=wall_ms",
                             kEndFramePeriods, kEndFrameEndAfterNormal, static_cast<unsigned long long>(kEndFrameStartLineEveryMs), kEndFrameStartLineCap,
                             kEndFrameEndLineMinSlow));
  }

  static const char* pathName(const EndFrameCall& c) { return c.background ? "loading" : c.overlapped ? "overlapped" : "synchronous"; }

 private:
  static size_t fit(char* buf, size_t cap, int n) {
    if (n < 0) { buf[0] = 0; return 0; }
    return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
  }

  void add(const EndFrameCall& c) {
    sumMs_ += c.ms;
    if (c.ms > maxMs_) maxMs_ = c.ms;
    unsigned bin = c.ms <= 0 ? 0u : static_cast<unsigned>(c.ms / kBinMs);
    if (bin >= kBins) bin = kBins - 1;
    ++bins_[bin];
  }

  // The p50 of the slow calls, to the histogram's half millisecond (the middle of its bin); the true max is kept apart.
  double p50() const {
    if (!count_) return 0;
    const uint64_t rank = (count_ + 1) / 2;
    uint64_t seen = 0;
    for (unsigned i = 0; i < kBins; ++i) {
      seen += bins_[i];
      if (seen >= rank) return (i + 0.5) * kBinMs;
    }
    return maxMs_;
  }

  void begin(const EndFrameCall& c, Lines& out) {
    open_ = true;
    ++summary_.episodes;
    count_ = 1;
    calls_ = 1;
    normalRun_ = 0;
    sumMs_ = 0;
    maxMs_ = 0;
    std::memset(bins_, 0, sizeof(bins_));
    add(c);
    startedMs_ = c.nowMs >= static_cast<uint64_t>(c.ms) ? c.nowMs - static_cast<uint64_t>(c.ms) : 0;
    lastSlowEndMs_ = c.nowMs;
    firstSequence_ = lastSequence_ = c.sequence;
    periodMs_ = c.periodMs;
    startWritten_ = false;
    if (summary_.startLines < kEndFrameStartLineCap && (!everWritten_ || c.nowMs - lastStartLineMs_ >= kEndFrameStartLineEveryMs)) {
      startWritten_ = true;
      everWritten_ = true;
      lastStartLineMs_ = c.nowMs;
      ++summary_.startLines;
      out.startLen = fit(out.start, sizeof(out.start),
                         std::snprintf(out.start, sizeof(out.start),
                                       "native_end_frame_episode,episode=%llu,sequence=%llu,ms=%.4f,periods=%.2f,period_ms=%.4f,should_render=%u,layers=%u,path=%s,pacing=%s,"
                                       "result=%d,xr_acquire_ms=%.4f,xr_wait_ms=%.4f,xr_draw_ms=%.4f,xr_release_ms=%.4f,copy_ms=%.4f,session_state=%s,state_age_ms=%.1f,"
                                       "units=wall_ms",
                                       static_cast<unsigned long long>(summary_.episodes), static_cast<unsigned long long>(c.sequence), c.ms, c.ms / c.periodMs,
                                       c.periodMs, c.shouldRender ? 1u : 0u, c.layers ? 1u : 0u, pathName(c), c.turbo ? "deferred" : "runtime", c.result,
                                       c.acquireMs, c.waitMs, c.drawMs, c.releaseMs, c.copyMs, c.sessionState ? c.sessionState : "UNKNOWN", c.stateAgeMs));
    }
  }

  void close(const char* reason, Lines& out) {
    open_ = false;
    const bool write = startWritten_ || count_ >= kEndFrameEndLineMinSlow;
    if (!write) return;
    ++summary_.endLines;
    const double duration = lastSlowEndMs_ >= startedMs_ ? static_cast<double>(lastSlowEndMs_ - startedMs_) : 0.0;
    // The trailing run of normal calls that ended the episode is not part of it.
    const uint64_t inEpisode = calls_ >= normalRun_ ? calls_ - normalRun_ : calls_;
    out.endLen = fit(out.end, sizeof(out.end),
                     std::snprintf(out.end, sizeof(out.end),
                                   "native_end_frame_episode_end,episode=%llu,reason=%s,start_line=%u,calls=%llu,slow_calls=%llu,duration_ms=%.1f,p50_ms=%.2f,max_ms=%.4f,"
                                   "mean_ms=%.4f,period_ms=%.4f,max_periods=%.2f,first_sequence=%llu,last_sequence=%llu,units=wall_ms",
                                   static_cast<unsigned long long>(summary_.episodes), reason, startWritten_ ? 1u : 0u, static_cast<unsigned long long>(inEpisode),
                                   static_cast<unsigned long long>(count_), duration, p50(), maxMs_, count_ ? sumMs_ / static_cast<double>(count_) : 0.0, periodMs_,
                                   periodMs_ > 0 ? maxMs_ / periodMs_ : 0.0, static_cast<unsigned long long>(firstSequence_),
                                   static_cast<unsigned long long>(lastSequence_)));
  }

  Summary summary_;
  bool open_ = false, startWritten_ = false, everWritten_ = false;
  uint64_t calls_ = 0, count_ = 0, normalRun_ = 0;
  double sumMs_ = 0, maxMs_ = 0, periodMs_ = 0;
  uint64_t startedMs_ = 0, lastSlowEndMs_ = 0, lastStartLineMs_ = 0, firstSequence_ = 0, lastSequence_ = 0;
  uint32_t bins_[kBins]{};
};

}  // namespace edvr::openxr

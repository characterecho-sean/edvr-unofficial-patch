#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "owner_invoke_cases.h"
#include "../../src/openxr/flat_owner_waits.h"
#include "../../src/openxr/owner_service.h"

// Cases for src/openxr/flat_owner_waits.h and its hooks in OwnerService. The clock, the flat gate and the thread id are fakes the test
// sets; the owner thread, its idle pump and the callers are real, with the pump held inside a flagged flat-clear frame while a
// locate_head invoke and a frame_end_finish submit queue behind it.
namespace edvr::openxr::test {
namespace flatwaits_detail {
inline std::atomic<uint64_t> gNowUs{0};
inline std::atomic<bool> gFlat{true};
inline thread_local uint32_t tTid = 0;
inline std::mutex gLinesMutex;
inline std::vector<std::string> gLines;

inline uint64_t nowUs() { return gNowUs.load(); }
inline bool flat() { return gFlat.load(); }
inline uint32_t thread() { return tTid; }
inline void stack(FlatOwnerCallerStack& out) {
  out.captured = 5; out.gameFrames = 2;
  std::snprintf(out.rvas, sizeof out.rvas, "0x1A2B/0x3C4D");
}
inline void print(const char* line) { std::lock_guard<std::mutex> lock(gLinesMutex); gLines.emplace_back(line); }

inline FlatOwnerWaits::Hooks hooks() {
  FlatOwnerWaits::Hooks h;
  h.nowUs = &nowUs; h.flat = &flat; h.thread = &thread; h.stack = &stack; h.print = &print;
  return h;
}
inline std::vector<std::string> linesStarting(const char* prefix) {
  std::lock_guard<std::mutex> lock(gLinesMutex);
  std::vector<std::string> out;
  for (const auto& l : gLines) if (l.rfind(prefix, 0) == 0) out.push_back(l);
  return out;
}
inline void clearLines() { std::lock_guard<std::mutex> lock(gLinesMutex); gLines.clear(); }
inline bool approx(double a, double b) { return std::fabs(a - b) < 1e-6; }
template<class Pred> bool spinUntil(Pred pred) {
  for (unsigned i = 0; i < 4000 && !pred(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return pred();
}
} // namespace flatwaits_detail

template<class Check> void runFlatOwnerWaitsCases(Check check) {
  using namespace flatwaits_detail;
  using ownerinvoke_detail::parseOwnerFields;
  using Kind = OwnerCallKind;
  using Site = OwnerJobSite;

  // The sites are distinct, each named once, and the unremarkable hops are told apart from "other".
  std::set<std::string> names;
  for (unsigned k = 0; k < FlatOwnerWaits::kSites; ++k) names.insert(ownerJobSiteName(Site(k)));
  check(names.size() == FlatOwnerWaits::kSites && !names.count(""), "flat-owner-waits: every site has its own name");
  check(ownerJobSiteForKind(Kind::WaitPoses) == Site::WaitPoses && ownerJobSiteForKind(Kind::LocateHead) == Site::LocateHead &&
    ownerJobSiteForKind(Kind::PollEvent) == Site::PollEvent && ownerJobSiteForKind(Kind::Submit) == Site::SubmitHandoff &&
    ownerJobSiteForKind(Kind::Other) == Site::Other, "flat-owner-waits: a call kind maps to its own site");
  check(std::string(ownerJobSiteName(Site::SetTrackingSpace)) != ownerJobSiteName(Site::Other) &&
    std::string(ownerJobSiteName(Site::ResetSeated)) != ownerJobSiteName(Site::ClearSkybox), "flat-owner-waits: the formerly kind-less hops are distinguishable");

  // The window line: nothing before the window is up, then one line with zeros printed explicitly.
  gNowUs = 1000; gFlat = true; clearLines();
  {
    FlatOwnerWaits w(hooks());
    w.tick();
    gNowUs = 1000 + 4999999; w.tick();
    check(linesStarting("flat_owner_waits").empty(), "flat-owner-waits: no window line before the window is up");
    gNowUs = 1000 + 5000000; w.tick();
    const auto lines = linesStarting("flat_owner_waits,");
    check(lines.size() == 1, "flat-owner-waits: one window line when the window is up, with nothing queued");
    if (lines.size() == 1) {
      const auto f = parseOwnerFields(lines[0].c_str());
      check(lines[0].size() + 1 < FlatOwnerWaits::kLineCap && lines[0].size() < 4000, "flat-owner-waits: the window line fits the logger's cap");
      check(f.at("window") == "1" && f.at("window_s") == "5" && f.at("elapsed_ms") == "5000" && f.at("idle_pumps") == "0" &&
        f.at("clear_frames") == "0" && f.at("clear_wait_ms_mean") == "0.0" && f.at("clear_wait_ms_max") == "0.0" && f.at("jobs") == "0" &&
        f.at("behind_clear") == "0" && f.at("behind_pump") == "0" && f.at("render_thread_wait_ms_sum") == "0.0" &&
        f.at("other_thread_wait_ms_sum") == "0.0" && f.at("local_pose_cache") == "0" && f.at("local_poll_event") == "0" &&
        f.at("flat_cache_answers") == "0" && f.at("flat_cache_fallbacks") == "0" && f.at("flat_cache_refused") == "0" &&
        f.at("flat_cache_fallback_reasons") == "unpublished:0/keys:0/stale:0" && f.at("flat_cache_age_ms_p50/p95/max") == "0.0/0.0/0.0",
        "flat-owner-waits: a window with nothing queued prints its zeros");
      const std::string kinds = f.at("kinds");
      size_t entries = 0, pos = 0;
      bool allZero = true;
      while (pos <= kinds.size()) {
        size_t bar = kinds.find('|', pos);
        if (bar == std::string::npos) bar = kinds.size();
        const std::string entry = kinds.substr(pos, bar - pos);
        const size_t colon = entry.find(':');
        allZero = allZero && colon != std::string::npos && entry.substr(colon + 1) == "0/0/0.0/0.0/0.0/0.0/0.0";
        ++entries; pos = bar + 1;
      }
      check(entries == FlatOwnerWaits::kSites && allZero, "flat-owner-waits: every site prints, zeros explicit");
    }
    // Leaving flat drops the partial window and prints nothing; the next flat stretch starts its own.
    clearLines();
    gFlat = false; gNowUs = 20000000; w.tick();
    gFlat = true; gNowUs = 30000000; w.tick();
    check(linesStarting("flat_owner_waits").empty(), "flat-owner-waits: no line outside flat, and a new stretch starts a fresh window");
    gNowUs = 30000000 + 5000000; w.tick();
    check(linesStarting("flat_owner_waits,").size() == 1, "flat-owner-waits: the new stretch prints when its own window is up");
  }

  // Window totals: clear waits, local answers, the line with large numbers, and a buffer that is too small.
  gNowUs = 0; gFlat = true; clearLines();
  {
    FlatOwnerWaits w(hooks());
    w.noteClearWait(70000); w.noteClearWait(60000); w.noteClearWait(80000);
    w.noteLocalPose(); w.noteLocalPose(); w.noteLocalPose(); w.noteLocalPoll(); w.noteLocalPoll();
    gFlat = false; w.noteLocalPose(); w.noteLocalPoll(); gFlat = true;
    const auto snap = w.take();
    check(snap.clearFrames == 3 && approx(snap.clearWaitMaxMs, 80.0) && approx(snap.clearWaitSumMs / 3.0, 70.0), "flat-owner-waits: clear frame waits counted");
    check(snap.localPose == 3 && snap.localPoll == 2, "flat-owner-waits: local answers counted in flat only");
    char line[FlatOwnerWaits::kLineCap];
    check(FlatOwnerWaits::formatFlatOwnerWaitsLine(1, 5000, snap, line, sizeof line) > 0 &&
      parseOwnerFields(line).at("clear_wait_ms_mean") == "70.0" && parseOwnerFields(line).at("local_pose_cache") == "3",
      "flat-owner-waits line: clear wait mean and local answers parse back");
    FlatOwnerWaits::Snapshot big;
    big.pumps = big.clearFrames = big.jobs = big.behindClear = big.behindPump = big.localPose = big.localPoll = 18446744073709551615ull;
    big.cacheAnswers = big.cacheFallbacks = big.cacheRefused = 18446744073709551615ull;
    for (auto& r : big.cacheFallbackReasons) r = 18446744073709551615ull;
    big.cacheAgeP50Ms = big.cacheAgeP95Ms = big.cacheAgeMaxMs = 1e12;
    big.pumpMaxMs = big.clearWaitSumMs = big.clearWaitMaxMs = big.renderWaitSumMs = big.otherWaitSumMs = 1e12;
    for (auto& s : big.sites) { s.n = s.behindClear = 18446744073709551615ull; s.p50Ms = s.p95Ms = s.maxMs = s.sumMs = s.runMaxMs = 1e12; }
    const int len = FlatOwnerWaits::formatFlatOwnerWaitsLine(99999999, 123456789.0, big, line, sizeof line);
    check(len > 0 && size_t(len) < FlatOwnerWaits::kLineCap && len < 4000, "flat-owner-waits line: the worst case still fits the cap");
    char tiny[64];
    check(FlatOwnerWaits::formatFlatOwnerWaitsLine(1, 5000, snap, tiny, sizeof tiny) == 0, "flat-owner-waits line: a buffer too small reports 0");
  }

  // The behind_clear rule on a ticket: a clear frame that began after the enqueue counts, one that never ran does not, outside flat nothing tracks.
  gNowUs = 100; gFlat = true; tTid = 4;
  {
    FlatOwnerWaits w(hooks());
    FlatOwnerWaits::Ticket quiet;
    w.enqueue(quiet, Site::LocateHead, true); gNowUs = 120; w.started(quiet); gNowUs = 130; w.finished(quiet);
    FlatOwnerWaits::Ticket late;
    w.enqueue(late, Site::PollEvent, true);
    w.clearEnter(); w.clearLeave();   // the owner ran a flat-clear frame while the job waited
    gNowUs = 200; w.started(late); w.finished(late);
    check(quiet.tracked && !quiet.behindClear && !quiet.behindPump, "flat-owner-waits: a job enqueued with no pump running is not behind_clear");
    check(late.tracked && late.behindClear && !late.clearAtEnqueue, "flat-owner-waits: a clear frame that began during the wait counts as behind_clear");
    gFlat = false;
    FlatOwnerWaits::Ticket outside;
    w.enqueue(outside, Site::Other, true);
    check(!outside.tracked, "flat-owner-waits: a job enqueued outside flat is not tracked");
    gFlat = true;
  }

  // The caller line: only a wait of 10 ms or more behind a clear frame, four at once, then the budget.
  gNowUs = 5000000; gFlat = true; clearLines(); tTid = 7;
  {
    FlatOwnerWaits w(hooks());
    w.bindRenderThread(7);
    auto job = [&](Site site, uint64_t waitUs, bool clearing) {
      FlatOwnerWaits::Ticket t;
      if (clearing) w.clearEnter();
      w.enqueue(t, site, true);
      if (clearing) w.clearLeave();
      gNowUs += waitUs; w.started(t); w.finished(t); w.note(t);
    };
    job(Site::LocateHead, 9999, true);      // under 10 ms
    job(Site::PollEvent, 50000, false);     // 50 ms, but no clear frame was in progress
    check(linesStarting("flat_owner_wait_caller").empty(), "flat-owner-waits: a short wait, or one not behind a clear frame, names no caller");
    for (unsigned i = 0; i < 6; ++i) job(Site::LocateHead, 20000, true);
    const auto callers = linesStarting("flat_owner_wait_caller,");
    check(callers.size() == 4, "flat-owner-waits: four callers are named at once, the rest wait for the budget");
    if (!callers.empty()) {
      const auto f = parseOwnerFields(callers[0].c_str());
      check(f.at("kind") == "locate_head" && f.at("wait_ms") == "20.000" && f.at("thread") == "render" && f.at("via") == "invoke" &&
        f.at("sample") == "1/16" && f.at("stack") == "1" && f.at("game_frames") == "2" && f.at("game_rvas") == "0x1A2B/0x3C4D",
        "flat-owner-waits caller line: kind, wait, thread and stack parse back");
    }
    const auto snap = w.take();
    check(snap.jobs == 8 && snap.behindClear == 7, "flat-owner-waits: jobs and behind_clear counted");
  }

  // A real service: the owner's idle pump holds a flagged clear frame while two calls queue behind it.
  gNowUs = 1000; gFlat = true; clearLines(); tTid = 0;
  {
    auto w = std::make_shared<FlatOwnerWaits>(hooks());
    w->bindRenderThread(7);
    OwnerService svc;
    svc.setFlatWaits(w);
    std::atomic<bool> armPump{false};
    std::promise<void> pumpEntered, releasePump;
    auto releaseFuture = releasePump.get_future().share();
    check(svc.start([&] {
      if (!armPump.exchange(false)) return;
      FlatOwnerWaits::ClearScope clearing(w.get());
      pumpEntered.set_value();
      releaseFuture.wait();
    }), "flat-owner-waits: service starts");

    // First, nothing in progress: an invoke goes straight through and is not behind a clear frame.
    tTid = 7;
    check(svc.invoke([] {}, Kind::LocateHead), "flat-owner-waits: an invoke with no pump in progress runs");
    {
      const auto snap = w->take();
      check(snap.jobs == 1 && snap.behindClear == 0 && approx(snap.sites[unsigned(Site::LocateHead)].maxMs, 0.0),
        "flat-owner-waits: a job enqueued while no flat-clear frame runs is not behind_clear and waits nothing");
    }

    armPump = true;
    pumpEntered.get_future().wait();
    gNowUs = 1000;
    std::thread render([&] {
      tTid = 7;
      svc.invoke([] { gNowUs = 61500; }, Kind::LocateHead);
    });
    check(spinUntil([&] { return svc.pending() == 1; }), "flat-owner-waits: the invoke queues behind the held pump");
    std::thread other([&] {
      tTid = 9;
      svc.submit([] {}, {}, Site::FrameEndFinish);
    });
    check(spinUntil([&] { return svc.pending() == 2; }), "flat-owner-waits: both calls queue behind the held pump");
    gNowUs = 61000;
    releasePump.set_value();
    render.join(); other.join();
    // The submit job is folded in by the owner after its completion; wait for both.
    FlatOwnerWaits::Snapshot snap;
    bool got = false;
    for (unsigned i = 0; i < 4000 && !got; ++i) {
      FlatOwnerWaits::Snapshot s = w->take();
      snap.jobs += s.jobs; snap.behindClear += s.behindClear; snap.behindPump += s.behindPump;
      snap.renderWaitSumMs += s.renderWaitSumMs; snap.otherWaitSumMs += s.otherWaitSumMs;
      for (unsigned k = 0; k < FlatOwnerWaits::kSites; ++k) {
        snap.sites[k].n += s.sites[k].n; snap.sites[k].behindClear += s.sites[k].behindClear;
        snap.sites[k].maxMs = (std::max)(snap.sites[k].maxMs, s.sites[k].maxMs);
        snap.sites[k].runMaxMs = (std::max)(snap.sites[k].runMaxMs, s.sites[k].runMaxMs);
      }
      got = snap.jobs >= 2;
      if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto& loc = snap.sites[unsigned(Site::LocateHead)];
    const auto& fin = snap.sites[unsigned(Site::FrameEndFinish)];
    check(got && snap.jobs == 2 && snap.behindClear == 2 && snap.behindPump == 2, "flat-owner-waits: both jobs counted behind the clear frame and the pump");
    check(loc.n == 1 && loc.behindClear == 1 && approx(loc.maxMs, 60.0) && approx(loc.runMaxMs, 0.5), "flat-owner-waits: the invoke's queue wait (60 ms) and run (0.5 ms) measured");
    check(fin.n == 1 && fin.behindClear == 1 && approx(fin.maxMs, 60.5), "flat-owner-waits: the submit job's queue wait measured, under its own site");
    check(approx(snap.renderWaitSumMs, 60.5) && approx(snap.otherWaitSumMs, 0.0),
      "flat-owner-waits: the render thread's blocked time is its queue plus run; a submit job blocks nobody");
    const auto callers = linesStarting("flat_owner_wait_caller,");
    bool sawInvoke = false, sawSubmit = false;
    for (const auto& l : callers) {
      const auto f = parseOwnerFields(l.c_str());
      if (f.at("kind") == "locate_head" && f.at("thread") == "render" && f.at("via") == "invoke" && f.at("stack") == "1") sawInvoke = true;
      if (f.at("kind") == "frame_end_finish" && f.at("thread") == "other" && f.at("via") == "submit" && f.at("stack") == "1") sawSubmit = true;
    }
    check(callers.size() == 2 && sawInvoke && sawSubmit, "flat-owner-waits: the caller of each worst wait is named, from the thread that enqueued it");

    // Outside flat the same service tracks nothing.
    gFlat = false; tTid = 7;
    check(svc.invoke([] {}, Kind::LocateHead), "flat-owner-waits: an invoke outside flat runs");
    gFlat = true;
    check(w->take().jobs == 0, "flat-owner-waits: a job outside flat is not in the window");
    svc.stop();
  }
  tTid = 0; gFlat = true;
}
} // namespace edvr::openxr::test

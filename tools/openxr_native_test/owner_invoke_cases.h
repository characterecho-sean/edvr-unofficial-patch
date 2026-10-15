#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <map>
#include <string>
#include <thread>
#include "../../src/openxr/owner_invoke_stats.h"
#include "../../src/openxr/owner_service.h"

// Cases for src/openxr/owner_invoke_stats.h and the instrumented OwnerService::invoke. The owner's clock is fake: a
// global the test sets, so queue and run are exact. Real threads are used, with the owner held inside a waitPoses-kind
// job while a pollEvent-kind request is queued behind it.
namespace edvr::openxr::test {
namespace ownerinvoke_detail {
inline std::atomic<uint64_t> gFakeNowUs{0};
inline uint64_t fakeNow() { return gFakeNowUs.load(); }

inline std::map<std::string, std::string> parseOwnerFields(const char* line) {
  std::map<std::string, std::string> out;
  std::string s(line);
  if (!s.empty() && s.back() == '\n') s.pop_back();
  size_t pos = 0;
  bool first = true;
  while (pos <= s.size()) {
    size_t comma = s.find(',', pos);
    if (comma == std::string::npos) comma = s.size();
    const std::string token = s.substr(pos, comma - pos);
    if (first) { out["_name"] = token; first = false; }
    else {
      const size_t eq = token.find('=');
      if (eq != std::string::npos) out[token.substr(0, eq)] = token.substr(eq + 1);
    }
    pos = comma + 1;
  }
  return out;
}
inline const OwnerInvokeStats::KindReport& row(const OwnerInvokeStats::KindReport (&rows)[OwnerInvokeStats::kinds], OwnerCallKind k) {
  return rows[unsigned(k)];
}
} // namespace ownerinvoke_detail

template<class Check> void runOwnerInvokeCases(Check check) {
  using namespace ownerinvoke_detail;
  using Kind = OwnerCallKind;
  using Rows = OwnerInvokeStats::KindReport[OwnerInvokeStats::kinds];

  // Pure accounting: nearest-rank percentiles, behind_wait, drain, capacity, and the line.
  OwnerInvokeStats s;
  s.note({Kind::WaitPoses, false, 1000 * 1, 9000});
  s.note({Kind::WaitPoses, true, 3000, 9500});
  s.note({Kind::WaitPoses, false, 0, 500});
  Rows rows{};
  s.takeWindow(rows);
  const auto& w = row(rows, Kind::WaitPoses);
  check(w.calls == 3 && w.behindWait == 1 && w.dropped == 0, "owner-invoke: calls and behind_wait counted per kind");
  check(w.queueValid && std::fabs(w.queueP50Ms - 1.0) < 1e-9 && std::fabs(w.queueP95Ms - 3.0) < 1e-9,
    "owner-invoke: queue p50/p95 nearest rank over the window");
  check(w.runValid && std::fabs(w.runP50Ms - 9.0) < 1e-9 && std::fabs(w.runP95Ms - 9.5) < 1e-9, "owner-invoke: run p50/p95");
  check(row(rows, Kind::LocateHead).calls == 0 && !row(rows, Kind::LocateHead).queueValid, "owner-invoke: a kind with no calls is empty");
  Rows again{};
  s.takeWindow(again);
  check(row(again, Kind::WaitPoses).calls == 0, "owner-invoke: a drained window starts empty");

  OwnerInvokeStats full;
  for (unsigned i = 0; i < OwnerInvokeStats::capacity + 5; ++i) full.note({Kind::PollEvent, false, 10, 20});
  Rows fullRows{};
  full.takeWindow(fullRows);
  check(row(fullRows, Kind::PollEvent).calls == OwnerInvokeStats::capacity + 5 && row(fullRows, Kind::PollEvent).dropped == 5,
    "owner-invoke: calls stay exact past capacity, the rest counted as dropped");

  char line[320];
  const int len = formatOwnerInvokeLine(4, Kind::WaitPoses, w, 10, line, sizeof line);
  check(len > 0, "owner-invoke: line fits its buffer");
  const auto f = parseOwnerFields(line);
  check(f.count("_name") && f.at("_name") == "native_owner_invoke" && f.at("window") == "4" && f.at("kind") == "waitPoses" &&
    f.at("calls") == "3" && f.at("per_frame") == "0.300" && f.at("queue_p50/p95") == "1.000/3.000" &&
    f.at("run_p50/p95") == "9.000/9.500" && f.at("behind_wait") == "1" && f.at("dropped") == "0",
    "owner-invoke line: fields parse back");
  char emptyLine[320];
  formatOwnerInvokeLine(5, Kind::LocateHead, row(rows, Kind::LocateHead), 0, emptyLine, sizeof emptyLine);
  const auto e = parseOwnerFields(emptyLine);
  check(e.at("kind") == "locateHead" && e.at("per_frame") == "na" && e.at("queue_p50/p95") == "na/na" && e.at("run_p50/p95") == "na/na",
    "owner-invoke line: na for an empty kind and for a window with no frames");

  // A real OwnerService on the fake clock.
  OwnerService svc;
  OwnerInvokeStats live;
  svc.setInvokeStats(&live, &fakeNow);
  check(svc.start(), "owner-invoke: service starts");

  // Idle owner: enqueued at 100, started at 100, the job moves the clock to 130.
  gFakeNowUs = 100;
  const bool idleOk = svc.invoke([] { gFakeNowUs = 130; }, Kind::LocateHead);
  Rows idleRows{};
  live.takeWindow(idleRows);
  const auto& loc = row(idleRows, Kind::LocateHead);
  check(idleOk && loc.calls == 1 && loc.behindWait == 0 && std::fabs(loc.queueP50Ms) < 1e-9 && std::fabs(loc.runP50Ms - 0.030) < 1e-9,
    "owner-invoke: an idle owner gives zero queue and the job's run");

  // The owner held in a waitPoses job; a pollEvent queued behind it. The clock is set between enqueue and release.
  gFakeNowUs = 200;
  std::promise<void> entered, release;
  auto releaseFuture = release.get_future().share();
  std::thread waitCaller([&] {
    svc.invoke([&] { entered.set_value(); releaseFuture.wait(); }, Kind::WaitPoses);
  });
  entered.get_future().wait();
  // The owner is inside the wait job: the poll enqueued now sees it busy.
  std::thread pollCaller([&] {
    svc.invoke([] { gFakeNowUs = 270; }, Kind::PollEvent);
  });
  for (unsigned spin = 0; spin < 2000000 && svc.pending() == 0; ++spin) std::this_thread::yield();
  check(svc.pending() == 1, "owner-invoke: the poll is queued behind the wait job");
  gFakeNowUs = 260;
  release.set_value();
  waitCaller.join();
  pollCaller.join();
  Rows liveRows{};
  live.takeWindow(liveRows);
  const auto& wait = row(liveRows, Kind::WaitPoses);
  const auto& poll = row(liveRows, Kind::PollEvent);
  // The wait started at 200 and finished at 260: run 60, no queue. The poll enqueued at 200, started at 260, ran 260 to 270.
  check(wait.calls == 1 && wait.behindWait == 0 && std::fabs(wait.queueP50Ms) < 1e-9 && std::fabs(wait.runP50Ms - 0.060) < 1e-9,
    "owner-invoke: the waitPoses job's run, queued from an idle owner");
  check(poll.calls == 1 && poll.behindWait == 1 && std::fabs(poll.queueP50Ms - 0.060) < 1e-9 && std::fabs(poll.runP50Ms - 0.010) < 1e-9,
    "owner-invoke: the poll queued behind a wait job: queue 60 us, run 10 us, behind_wait counted");

  // Without stats the same path still works and reads no clock.
  OwnerService plain;
  check(plain.start(), "owner-invoke: plain service starts");
  check(plain.invoke([] {}, Kind::Submit), "owner-invoke: an invoke with no stats still runs its job");
  plain.stop();
  svc.stop();
}
} // namespace edvr::openxr::test

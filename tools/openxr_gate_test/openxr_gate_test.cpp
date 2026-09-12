#include "../../src/openxr/runtime_gate.h"

#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

using edvr::openxr::RuntimeGate;
namespace {
unsigned checks = 0, failures = 0;
void check(bool value, const char* message) {
  ++checks;
  if (!value) { ++failures; std::printf("FAIL: %s\n", message); }
}

void selfTest() {
  static_assert(!std::is_copy_constructible<RuntimeGate::Lease>::value, "lease copy");
  static_assert(!std::is_copy_assignable<RuntimeGate::Lease>::value, "lease copy assign");
  RuntimeGate gate;
  const auto generation = gate.beginGeneration();
  check(generation != 0, "generation starts nonzero");
  auto first = gate.tryEnter(generation);
  check(first.valid() && !gate.tryEnter(generation), "single lease and reentry rejected");
  check(gate.requestStop(generation), "stop accepted");
  check(!gate.tryEnter(generation) && !gate.canDestroy(generation), "stop blocks and waits for lease");
  first = RuntimeGate::Lease();
  check(gate.canDestroy(generation), "destroy allowed after release");
  check(gate.finishGeneration(generation), "finish retires generation");
  check(!gate.finishGeneration(generation), "finish is one shot");

  const auto next = gate.beginGeneration();
  check(next > generation, "reinit is monotonic");
  check(!gate.tryEnter(generation) && !gate.requestStop(generation), "stale generation rejected");
  auto moved = gate.tryEnter(next);
  RuntimeGate::Lease movedTo;
  movedTo = std::move(moved);
  check(!moved && movedTo, "move transfers ownership");
  check(gate.requestStop(next), "active generation stops");
  check(!gate.canDestroy(next), "active lease prevents teardown");
  movedTo = RuntimeGate::Lease();
  check(gate.canDestroy(next) && gate.finishGeneration(next), "finished moved lease generation");

  RuntimeGate asyncGate;
  const auto asyncGeneration = asyncGate.beginGeneration();
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false, unblock = false, workerWoke = false, workerLeased = false;
  std::thread worker([&] {
    auto lease = asyncGate.tryEnter(asyncGeneration);
    workerLeased = bool(lease);
    { std::lock_guard<std::mutex> lock(mutex); entered = true; cv.notify_one(); }
    std::unique_lock<std::mutex> lock(mutex);
    workerWoke = cv.wait_for(lock, std::chrono::seconds(2), [&] { return unblock; });
  });
  { std::unique_lock<std::mutex> lock(mutex);
    check(cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }), "blocked operation entered");
  }
  check(asyncGate.requestStop(asyncGeneration), "stop returns while operation is blocked");
  check(!asyncGate.canDestroy(asyncGeneration), "blocked operation delays destroy");
  { std::lock_guard<std::mutex> lock(mutex); unblock = true; cv.notify_one(); }
  worker.join();
  check(workerLeased && workerWoke, "blocked operation held its lease and woke");
  check(asyncGate.canDestroy(asyncGeneration) && asyncGate.finishGeneration(asyncGeneration), "async teardown completes");
}
}

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
    std::puts("Would test OpenXR runtime operation lifetime gate; no runtime or files.");
    return 0;
  }
  if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) return 2;
  selfTest();
  std::printf("openxr_gate_test: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}

#pragma once

#include "../../src/openxr/owner_service.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

namespace edvr::openxr::module_test {

struct OwnerTaskResult {
  bool submitted = false;
  bool completed = false;
  bool succeeded = false;
  bool pumpSucceeded = true;
  unsigned pumpCalls = 0;
};

// Submit owns both the callback and completion captures. While the owner
// executes the callback, the caller continues its real render/present pump.
// A caller-owned watchdog may terminate a broken fixture, but this helper
// never returns while a callback can still access its captures.
template<class Pump, class Task>
OwnerTaskResult submitWithPump(OwnerService& owner, Pump&& pump, Task&& task) {
  struct State {
    std::atomic<bool> done{false};
    bool success = false; // published by done's release/acquire pair
  };
  std::shared_ptr<State> state;
  bool submitted = false;
  try {
    state = std::make_shared<State>();
    submitted = owner.submit(std::forward<Task>(task), [state](bool success) {
      state->success = success;
      state->done.store(true, std::memory_order_release);
    });
  } catch (...) {
    return {};
  }
  if (!submitted) return {false, false, false, true, 0};

  bool pumpSucceeded = true;
  unsigned pumpCalls = 0;
  while (!state->done.load(std::memory_order_acquire)) {
    bool pumped = true;
    try { pumped = static_cast<bool>(pump()); }
    catch (...) { pumped = false; }
    ++pumpCalls;
    pumpSucceeded = pumped && pumpSucceeded;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return {true, true, state->success, pumpSucceeded, pumpCalls};
}

}  // namespace edvr::openxr::module_test

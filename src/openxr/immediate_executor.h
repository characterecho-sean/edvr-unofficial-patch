#pragma once

#include <functional>

namespace edvr::openxr {

// Synchronous execution on the host's exclusively owned immediate context.
// Return only after a started callback and its user captures are retired;
// rejected/cancelled callbacks never run later. This interface grants no
// ownership against game code by itself. Destruction requires drained calls.
class ImmediateExecutor {
 public:
  virtual ~ImmediateExecutor() = default;
  virtual bool invoke(std::function<void()> callback) = 0;
};

}  // namespace edvr::openxr

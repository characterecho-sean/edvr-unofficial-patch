#pragma once
#include "../common/onfoot_recorder_core.h"
#include <windows.h>

namespace edvr::openxr {
// F213: the runtime's way into the on-foot flight recorder (src\d3d11\onfoot_recorder.h). The runtime has no config, no thread and no file of its own: it builds one record
// (rt_eye, rt_recentre), stamps its QPC and hands it to the graphics half's export edvrOnFootRecordPush, which says false when the recorder is off. A graphics half without the
// export (mixed DLLs) is never called: nothing is recorded and nothing else changes. Owner thread only; nothing is held across the call.
class OnFootRecorderClient final {
 public:
  template<class R> bool push(HMODULE provider, R& rec) {
    if (!bind(provider)) return false;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    rec.h.qpc = static_cast<uint64_t>(q.QuadPart);
    return push_(&rec, static_cast<uint32_t>(sizeof(R)));
  }
  // Whether the graphics half has the export (resolved once per provider).
  bool bound() const { return push_ != nullptr; }
 private:
  using PushFn = bool(__stdcall*)(const void*, uint32_t);
  bool bind(HMODULE provider) {
    if (provider != provider_) {
      provider_ = provider;
      push_ = provider ? reinterpret_cast<PushFn>(GetProcAddress(provider, "edvrOnFootRecordPush")) : nullptr;
    }
    return push_ != nullptr;
  }
  HMODULE provider_ = nullptr;
  PushFn push_ = nullptr;
};
} // namespace edvr::openxr

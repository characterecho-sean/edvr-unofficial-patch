#pragma once
#include <windows.h>
#include <cstdio>
#include "native_trace.h"

namespace edvr::openxr {
struct ShutdownTrace {
  const char* name; bool enabled,ended=false;
  ShutdownTrace(const char* stage,bool on=true):name(stage),enabled(on) {
    if(enabled) { nativeTracePrintf("shutdown_stage,begin=%s,thread=%lu,tick=%llu\n",name,
      (unsigned long)GetCurrentThreadId(),(unsigned long long)GetTickCount64()); std::fflush(stdout); }
  }
  void end(bool ok) {
    if(enabled&&!ended) { ended=true; nativeTracePrintf("shutdown_stage,end=%s,ok=%u,thread=%lu,tick=%llu\n",name,
      unsigned(ok),(unsigned long)GetCurrentThreadId(),(unsigned long long)GetTickCount64()); std::fflush(stdout); }
  }
  ~ShutdownTrace() { if(enabled&&!ended) { nativeTracePrintf("shutdown_stage,abandoned=%s,thread=%lu,tick=%llu\n",name,
      (unsigned long)GetCurrentThreadId(),(unsigned long long)GetTickCount64()); std::fflush(stdout); } }
};
} // namespace edvr::openxr

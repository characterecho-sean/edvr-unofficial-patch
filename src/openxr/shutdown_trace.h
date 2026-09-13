#pragma once
#include <windows.h>
#include <cstdio>

namespace edvr::openxr {
struct ShutdownTrace {
  const char* name; bool enabled,ended=false;
  ShutdownTrace(const char* stage,bool on=true):name(stage),enabled(on) {
    if(enabled) { std::printf("shutdown_stage,begin=%s,thread=%lu,tick=%llu\n",name,
      (unsigned long)GetCurrentThreadId(),(unsigned long long)GetTickCount64()); std::fflush(stdout); }
  }
  void end(bool ok) {
    if(enabled&&!ended) { ended=true; std::printf("shutdown_stage,end=%s,ok=%u,thread=%lu,tick=%llu\n",name,
      unsigned(ok),(unsigned long)GetCurrentThreadId(),(unsigned long long)GetTickCount64()); std::fflush(stdout); }
  }
  ~ShutdownTrace() { if(enabled&&!ended) { std::printf("shutdown_stage,abandoned=%s,thread=%lu,tick=%llu\n",name,
      (unsigned long)GetCurrentThreadId(),(unsigned long long)GetTickCount64()); std::fflush(stdout); } }
};
} // namespace edvr::openxr

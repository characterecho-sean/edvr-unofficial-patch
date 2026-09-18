#pragma once
#include <windows.h>
#include <evntprov.h>
#include <atomic>
#include <cstdint>
#include "../common/native_cpu_trace_events.h"
#include "../common/native_present_trace.h"
#ifdef _MSC_VER
#pragma comment(lib,"advapi32.lib")
#endif

namespace edvr::openxr {

class NativeCpuTrace final {
 public:
  struct Counters { uint64_t emitted=0,failures=0; ULONG lastError=ERROR_SUCCESS; };
  static NativeCpuTrace& get() noexcept { static auto* value=new NativeCpuTrace;return *value; }
  bool start() noexcept {
    AcquireSRWLockExclusive(&lock_);
    if(handle_){ReleaseSRWLockExclusive(&lock_);return true;}
    REGHANDLE registered=0;
    const ULONG error=EventRegister(&providerGuid(),&enableCallback,this,&registered);
    if(error==ERROR_SUCCESS) {
      handle_=registered;++registrationGeneration_;
      lastError_.store(ERROR_SUCCESS,std::memory_order_relaxed);
      enabled_.store(EventProviderEnabled(handle_,4,1)!=FALSE,std::memory_order_release);
      if(enabled_.load(std::memory_order_relaxed))writeClockLocked();
    } else {lastError_.store(error,std::memory_order_relaxed);failures_.fetch_add(1,std::memory_order_relaxed);}
    ReleaseSRWLockExclusive(&lock_);return error==ERROR_SUCCESS;
  }
  void stop() noexcept {
    AcquireSRWLockExclusive(&lock_);
    const REGHANDLE handle=handle_;
    if(handle) {
      if(enabled_.load(std::memory_order_acquire))writeClockLocked();
      enabled_.store(false,std::memory_order_release);handle_=0;++registrationGeneration_;
      const ULONG error=EventUnregister(handle);
      if(error!=ERROR_SUCCESS){lastError_.store(error,std::memory_order_relaxed);failures_.fetch_add(1,std::memory_order_relaxed);}
    }
    ReleaseSRWLockExclusive(&lock_);
  }
  bool enabled() const noexcept {return enabled_.load(std::memory_order_acquire);}
  bool emitFrame(const EdvrNativeCpuCompletedFramePayload& payload) noexcept {
    if(!enabled())return false;
    AcquireSRWLockShared(&lock_);const bool active=handle_&&enabled();
    const bool written=active&&(maybeClockLocked(),writeLocked(frameDescriptor(),payload));
    ReleaseSRWLockShared(&lock_);return written;
  }
  Counters counters() const noexcept {
    return {emitted_.load(std::memory_order_relaxed),failures_.load(std::memory_order_relaxed),
      lastError_.load(std::memory_order_relaxed)};
  }

 private:
  friend class NativeCpuTraceSpan;
  static const GUID& providerGuid() noexcept {
    static const GUID value={EDVR_NATIVE_CPU_TRACE_GUID_DATA1,EDVR_NATIVE_CPU_TRACE_GUID_DATA2,
      EDVR_NATIVE_CPU_TRACE_GUID_DATA3,{0xaf,0x88,0x63,0xb2,0x01,0x2b,0x11,0x1e}};
    return value;
  }
  static const EVENT_DESCRIPTOR& descriptor(USHORT id) noexcept {
    static const EVENT_DESCRIPTOR values[]={
      {EdvrNativeCpuClockEvent,1,0,4,0,0,1},
      {EdvrNativeCpuSpanEvent,1,0,4,0,0,1},
      {EdvrNativeCpuCompletedFrameEvent,1,0,4,0,0,1}};
    return values[id-1];
  }
  static const EVENT_DESCRIPTOR& clockDescriptor() noexcept{return descriptor(EdvrNativeCpuClockEvent);}
  static const EVENT_DESCRIPTOR& spanDescriptor() noexcept{return descriptor(EdvrNativeCpuSpanEvent);}
  static const EVENT_DESCRIPTOR& frameDescriptor() noexcept{return descriptor(EdvrNativeCpuCompletedFrameEvent);}
  static VOID NTAPI enableCallback(LPCGUID,ULONG enabled,UCHAR level,ULONGLONG any,
      ULONGLONG,PEVENT_FILTER_DESCRIPTOR,PVOID context) noexcept {
    auto& self=*static_cast<NativeCpuTrace*>(context);
    const bool accepted=enabled&&(level==0||level>=4)&&(any==0||(any&1));
    self.enabled_.store(accepted,std::memory_order_release);
    if(accepted)self.clockPending_.store(true,std::memory_order_release);
  }
  bool beginSpan(uint16_t operation,EdvrNativeCpuSpanPayload& payload,uint64_t& generation) noexcept {
    if(!enabled())return false;
    AcquireSRWLockShared(&lock_);
    if(!handle_||!enabled()){ReleaseSRWLockShared(&lock_);return false;}
    payload={};payload.callId=nextCall_.fetch_add(1,std::memory_order_relaxed)+1;
    payload.beginUs=edvrNativeTraceNowUs();payload.thread=GetCurrentThreadId();payload.operation=operation;generation=registrationGeneration_;
    const bool valid=payload.beginUs!=0;ReleaseSRWLockShared(&lock_);return valid;
  }
  bool endSpan(EdvrNativeCpuSpanPayload& payload,uint64_t generation) noexcept {
    payload.timestampUs=edvrNativeTraceNowUs();
    AcquireSRWLockShared(&lock_);
    const bool valid=handle_&&enabled()&&generation==registrationGeneration_&&payload.timestampUs>=payload.beginUs;
    const bool written=valid&&(maybeClockLocked(),writeLocked(spanDescriptor(),payload));
    ReleaseSRWLockShared(&lock_);return written;
  }
  void maybeClockLocked() noexcept {
    const uint64_t n=dataEvents_.fetch_add(1,std::memory_order_relaxed)+1;
    if(clockPending_.exchange(false,std::memory_order_acq_rel)||(n&1023u)==0)writeClockLocked();
  }
  void writeClockLocked() noexcept {
    LARGE_INTEGER ticks{},frequency{};FILETIME time{};
    QueryPerformanceCounter(&ticks);QueryPerformanceFrequency(&frequency);GetSystemTimeAsFileTime(&time);
    EdvrNativeCpuClockPayload payload{};payload.qpcTicks=uint64_t(ticks.QuadPart);
    payload.qpcFrequency=uint64_t(frequency.QuadPart);payload.timestampUs=edvrNativeTraceUs(ticks.QuadPart);
    payload.systemTime100ns=(uint64_t(time.dwHighDateTime)<<32)|time.dwLowDateTime;
    writeLocked(clockDescriptor(),payload);
  }
  template<class T> bool writeLocked(const EVENT_DESCRIPTOR& descriptor,const T& payload) noexcept {
    EVENT_DATA_DESCRIPTOR data{};EventDataDescCreate(&data,&payload,ULONG(sizeof(payload)));
    const ULONG error=EventWrite(handle_,&descriptor,1,&data);
    if(error==ERROR_SUCCESS){emitted_.fetch_add(1,std::memory_order_relaxed);return true;}
    failures_.fetch_add(1,std::memory_order_relaxed);lastError_.store(error,std::memory_order_relaxed);return false;
  }
  NativeCpuTrace() noexcept=default;
  SRWLOCK lock_=SRWLOCK_INIT;REGHANDLE handle_=0;uint64_t registrationGeneration_=0;
  std::atomic<bool> enabled_{false},clockPending_{false};
  std::atomic<uint64_t> nextCall_{0},dataEvents_{0},emitted_{0},failures_{0};
  std::atomic<ULONG> lastError_{ERROR_SUCCESS};
};

class NativeCpuTraceSpan final {
 public:
  explicit NativeCpuTraceSpan(uint16_t operation) noexcept:active_(NativeCpuTrace::get().beginSpan(operation,payload_,generation_)){}
  ~NativeCpuTraceSpan(){if(active_)NativeCpuTrace::get().endSpan(payload_,generation_);}
  NativeCpuTraceSpan(const NativeCpuTraceSpan&)=delete;
  NativeCpuTraceSpan& operator=(const NativeCpuTraceSpan&)=delete;
  template<class T>T finish(T value) noexcept {
    if(active_){payload_.result=static_cast<int64_t>(value);payload_.flags|=EdvrNativeCpuResultValid;}return value;
  }
  void finishVoid(int64_t result=0) noexcept {
    if(active_){payload_.result=result;payload_.flags|=EdvrNativeCpuResultValid;}
  }
 private:
  EdvrNativeCpuSpanPayload payload_;uint64_t generation_=0;bool active_=false;
};

} // namespace edvr::openxr

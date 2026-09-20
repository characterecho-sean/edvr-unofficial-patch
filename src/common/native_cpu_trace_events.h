#pragma once
#include <cstddef>
#include <cstdint>

// Manifest-free ETW payload contract. All fields are little-endian and every
// event is written as one contiguous EVENT_DATA_DESCRIPTOR. Keep the numeric
// values and byte offsets stable so an ETL decoder needs no matching binary.
constexpr uint32_t EDVR_NATIVE_CPU_TRACE_GUID_DATA1=0xd3885fa1u;
constexpr uint16_t EDVR_NATIVE_CPU_TRACE_GUID_DATA2=0x0b70u;
constexpr uint16_t EDVR_NATIVE_CPU_TRACE_GUID_DATA3=0x44f1u;
constexpr uint8_t EDVR_NATIVE_CPU_TRACE_GUID_DATA4[8]={0xaf,0x88,0x63,0xb2,0x01,0x2b,0x11,0x1e};

enum EdvrNativeCpuTraceEventId : uint16_t {
  EdvrNativeCpuClockEvent=1, EdvrNativeCpuSpanEvent=2,
  EdvrNativeCpuCompletedFrameEvent=3
};
enum EdvrNativeCpuOperation : uint16_t {
  EdvrCpuWaitGetPoses=1, EdvrCpuSubmit=2, EdvrCpuPostPresentHandoff=3,
  EdvrCpuGetLastPoses=4, EdvrCpuGetLastPoseForTrackedDeviceIndex=5,
  EdvrCpuGetFrameTiming=6, EdvrCpuGetFrameTimeRemaining=7,
  EdvrCpuCanRenderScene=8, EdvrCpuGetTimeSinceLastVsync=9,
  EdvrCpuGetDeviceToAbsoluteTrackingPose=10,
  EdvrCpuFixtureCycle=0x100, EdvrCpuFixtureBusy=0x101, EdvrCpuFixtureWait=0x102
};
enum EdvrNativeCpuSpanFlags : uint16_t { EdvrNativeCpuResultValid=1u };
enum EdvrNativeCpuFrameFlags : uint16_t {
  EdvrNativeCpuPostValid=1u, EdvrNativeCpuSinglePresent=2u
};
enum EdvrNativeCpuFrameStatus : uint16_t {
  EdvrCpuPostAvailable=0, EdvrCpuProviderMissing=1, EdvrCpuBadProviderVersion=2,
  EdvrCpuBadProviderSize=3, EdvrCpuBadProviderGeneration=4,
  EdvrCpuNotYetObservable=5, EdvrCpuLostPresentHistory=6,
  EdvrCpuPartialPresent=7, EdvrCpuWrongPresentThread=8,
  EdvrCpuFailedPresent=9, EdvrCpuTestPresent=10, EdvrCpuMalformedPresent=11
};

#pragma pack(push,1)
struct EdvrNativeCpuClockPayload {
  uint64_t timestampUs;
  uint64_t qpcTicks;
  uint64_t qpcFrequency;
  uint64_t systemTime100ns;
};
struct EdvrNativeCpuSpanPayload {
  uint64_t timestampUs; // span end and ETW emission time
  uint64_t callId;
  uint64_t beginUs;
  int64_t result;
  uint32_t thread;
  uint16_t operation;
  uint16_t flags;
};
struct EdvrNativeCpuCompletedFramePayload {
  uint64_t timestampUs; // completed next WaitGetPoses return and emission time
  uint64_t sequence;
  uint64_t generation;
  uint64_t featureEpoch;
  uint64_t waitReturnUs;
  uint64_t secondSubmitReturnUs;
  uint64_t nextWaitEntryUs;
  uint64_t nextWaitReturnUs;
  uint64_t presentBeginUs;
  uint64_t presentEndUs;
  uint32_t callerThread;
  uint32_t nextWaitThread;
  uint16_t status; // FrameCycleStats::PostUnavailable; zero means available
  uint16_t flags;
  uint32_t sceneReady;
};
#pragma pack(pop)

static_assert(sizeof(EdvrNativeCpuClockPayload)==32,"native CPU clock ETW ABI");
static_assert(sizeof(EdvrNativeCpuSpanPayload)==40,"native CPU span ETW ABI");
static_assert(sizeof(EdvrNativeCpuCompletedFramePayload)==96,"native CPU frame ETW ABI");
static_assert(offsetof(EdvrNativeCpuSpanPayload,thread)==32,"native CPU span offsets");
static_assert(offsetof(EdvrNativeCpuCompletedFramePayload,status)==88,"native CPU frame offsets");

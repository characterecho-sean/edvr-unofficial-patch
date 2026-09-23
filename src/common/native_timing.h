#pragma once
#include <windows.h>
#include <d3d11.h>
#include <stddef.h>
#include <stdint.h>

#define EDVR_NATIVE_TIMING_VERSION_3 3u
// Version 4 appends baseDisplayHz to the END of EdvrNativeTimingFrame and
// nothing else. The two DLLs are copied apart by hand all the time (the same
// rule native_frame.h states), so publishCpu still accepts a version 3 frame
// of the smaller size; it simply carries no base display rate, and the
// consumer falls back to the session's first predicted period.
#define EDVR_NATIVE_TIMING_VERSION_4 4u
// Version 5 appends callerWorkMs and callerWorkValid to the END and nothing
// else, under the same hand-copied-DLLs rule: publishCpu still accepts a
// version 3 or 4 frame of its smaller size, whose missing caller work reads
// as absent (0, not valid), and the consumer falls back to the producer's
// application time.
#define EDVR_NATIVE_TIMING_VERSION_5 5u
// Private paired-module measurement capability. Acquire binds the producer;
// gpuEye runs only on that thread. All other callbacks are CPU-only and may
// run on the XR owner. The host retains the module/device through close.
struct EdvrNativeTimingRequest {
    uint32_t size, version;
    ID3D11Device* device;
    uint64_t generation;
};
struct EdvrNativeTimingFrame {
    uint32_t size, version;
    uint64_t sequence;
    // Submit/transfer/compose wall time includes producer rendezvous and
    // driver/runtime waits. Temporal/menu are treatment wall time inside the
    // batched producer callback, excluding its queue wait. Not CPU/GPU busy time.
    double submitMs[2], temporalMs[2], menuMs[2], transferMs[2];
    double composeMs;
    // Per-frame workload identity. Input is Elite's submitted region, output
    // is the active XR target, and game FOV is before temporal jitter (radians,
    // left/right/up/down). Never substitute runtime recommendations for input.
    uint32_t inputWidth[2], inputHeight[2], outputWidth[2], outputHeight[2];
    float gameFov[2][4];
    uint32_t treatments[2];
    uint64_t featureEpoch;
    // Version 4 and later. The panel's base refresh rate in hertz, from the
    // same publication the display_frequency log line names (0 when the
    // runtime has published none). The consumer compares the predicted period
    // against this base to flag a throttled display rate.
    float baseDisplayHz;
    // Version 5 and later. The caller (game) thread's wall time per frame
    // cycle outside the pose wait: from one WaitGetPoses return to the next
    // one's entry -- the runtime's cycle minus its next-wait roundtrip
    // (native_frame_cycle_phase cycle - next_wait_roundtrip), i.e. the game's
    // work before the first submit, both submit roundtrips with the waits
    // inside them, the time between the eyes and after the second submit:
    // everything the display period has to hold. A cycle completes only at
    // the NEXT wait's return, so a frame carries the cycle before it.
    // callerWorkValid is 1 only when the cycle that ended at this frame's own
    // pose wait completed whole (both eyes, consistent clocks); else 0, and
    // callerWorkMs is 0.
    double callerWorkMs;
    uint32_t callerWorkValid;
};
// The size the fields through featureEpoch occupy, which is what a version 3
// caller's struct is. baseDisplayHz follows with no tail padding in play.
#define EDVR_NATIVE_TIMING_FRAME_SIZE_3 \
    ((uint32_t)offsetof(EdvrNativeTimingFrame, baseDisplayHz))
// A version 4 caller's struct ends at baseDisplayHz plus its tail padding to
// the struct's 8-byte alignment, which is exactly where the double
// callerWorkMs lands; the two sizes below are the shipped ABI.
#define EDVR_NATIVE_TIMING_FRAME_SIZE_4 \
    ((uint32_t)offsetof(EdvrNativeTimingFrame, callerWorkMs))
static_assert(EDVR_NATIVE_TIMING_FRAME_SIZE_3 == 168 && EDVR_NATIVE_TIMING_FRAME_SIZE_4 == 176,
              "the version 3 and 4 timing frame sizes are fixed by shipped DLLs");
enum EdvrNativeGpuStatus : uint32_t {
    EdvrNativeGpuPending, EdvrNativeGpuValid, EdvrNativeGpuDisabled,
    EdvrNativeGpuIncomplete, EdvrNativeGpuQueryFailure, EdvrNativeGpuStale,
    EdvrNativeGpuNotSeparate
};
struct EdvrNativeDeviceGpuSample {
    uint32_t size, version;
    uint64_t sequence, completedAtMs;
    uint32_t status;
    // Four narrow spans on the separate XR immediate context. They exclude
    // producer work, keyed-mutex waits and the runtime's final compositor.
    double transferMs[2], composeMs[2];
};
struct EdvrNativeTimingTable {
    uint32_t size, version;
    void* context;
    // Allocates the graphics receiver's globally increasing frame sequence.
    // Disarms prior GPU measurement and starts the owner pose-call wall clock.
    uint64_t (WINAPI *waitBegin)(void*);
    HRESULT (WINAPI *waitEnd)(void*, uint64_t, uint32_t valid, int64_t predictedPeriodNs);
    // eye 0/1 begin=1 starts before native eye processing; the matching
    // begin=0 acceptance marker is sent AFTER native Submit returns. Reserved
    // eyes 2..6 carry CPU segment and GPU producer-boundary events. Pause/end
    // issue timestamp markers on the bound producer context; resume admits the
    // next real command. Return 1 only when the marker was accepted.
    uint32_t (WINAPI *gpuEye)(void*, uint64_t, uint32_t eye, uint32_t begin,
                            uint32_t accepted, ID3D11Texture2D*);
    // Only complete successfully copied/submitted stereo pairs are published.
    HRESULT (WINAPI *publishCpu)(void*, const EdvrNativeTimingFrame*);
    HRESULT (WINAPI *invalidate)(void*);
    HRESULT (WINAPI *close)(void*);
    // CPU-only configuration and delayed publication; never dispatch graphics.
    uint32_t (WINAPI *gpuEnabled)(void*);
    HRESULT (WINAPI *publishDeviceGpu)(void*, const EdvrNativeDeviceGpuSample*);
};
extern "C" HRESULT WINAPI edvrAcquireNativeTiming(const EdvrNativeTimingRequest*, EdvrNativeTimingTable*);

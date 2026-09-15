#pragma once
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>

#define EDVR_NATIVE_TIMING_VERSION_3 3u
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
};
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

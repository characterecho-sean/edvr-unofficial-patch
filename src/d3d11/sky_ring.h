#pragma once
// The sky ring (I-2 of docs\sky-flicker-2026-10-13.md): the module. TEMPORARY, behind diag.sky_ring = off|on (default off, VR only: the flat profile does
// not list the key, so it reads off there). The pure half is src\common\sky_ring.h, the GPU half sky_ring_gpu.h, the shader sky_ring_shader.h.
//
// temporal_pass.cpp calls skyRingEyeFrame once per treated eye-frame, after the upscaler, and skyRingRequestDump from the Pause dump with the camera-row
// events it found. The request copies the ring on the render thread's next call and prints when the copy is readable, a few frames later, without
// waiting for the GPU.
#include <cstdint>
#include <utility>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace edvr {

// One eye-call's camera-row step, for the join: the rows' rotation and translation from the previous same-eye frame.
struct SkyJoinRow {
    uint32_t frame = 0;
    uint32_t eye = 0;
    bool have = false;
    float rotDeg = 0.0f;
    float mvM = 0.0f;
};

// diag.sky_ring. Live: turning it off stops the dispatches (the ring keeps what it has).
void skyRingConfigure(bool on);
bool skyRingOn();

// Once per treated eye-frame. A no-op with the key off. Returns whether it dispatched. Creates the ring and the shader on the first call.
bool skyRingEyeFrame(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t eye, uint32_t frame, ID3D11ShaderResourceView* raw,
                     ID3D11ShaderResourceView* out, ID3D11ShaderResourceView* mv, ID3D11ShaderResourceView* depth, uint32_t inW, uint32_t inH,
                     uint32_t outW, uint32_t outH, float projA, bool depthValid);

// The dump key: events are (first frame, run) pairs from camera_row_events.h; rows are the history's steps. Prints "sky ring: no samples (off or never
// ran)" at once when the ring never ran; otherwise queues the readback.
void skyRingRequestDump(const char* trigger, const std::vector<std::pair<uint32_t, uint32_t>>& events, const std::vector<SkyJoinRow>& rows);

uint64_t skyRingDispatches();
void skyRingShutdown();

}  // namespace edvr

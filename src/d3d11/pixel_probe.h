// "Who drew this pixel" -- names every draw that changes a handful of
// chosen texels during ONE armed eye-dump frame (device_hook.cpp's
// hotkey.dump_eyes, via temporalPassArmEyeDump/beginEyeRun, the same seam
// object_probe.cpp's ledger arms from). advanced.pixel_probe set the icon
// smear under FSR (eye_113353) to open this: the dump's own draw snapshot
// only records watched shader families, and the icon's draw was not one.
//
// Mechanism: while armed, on the probed frame only, every draw that lands
// in an eye render target gets a 16x16 window around each configured point
// copied into its own slot of one shared staging atlas (CopySubresourceRegion,
// GPU-side, no Map, no wait) -- a baseline slot before the first draw into
// each eye this frame, then one slot per draw after it (and any EDVR
// reissues for it) have fully returned. At the closing frame boundary the
// atlas is Mapped ONCE and each slot diffed against the previous slot for
// the same point and eye; a draw whose window changed is named in the log
// with its shader hashes, topology, counts and blend/depth state.
//
// Narrow on purpose: no Config, no binding_shadow, no State. The caller
// (vscreen.cpp) resolves the eye and the RTV0 identity it already tracks
// (the rtv0Eye pattern) and gathers DrawInfo from its own bindings; this
// module only knows draws, points, slots and bytes, which is what lets
// tools/pixel_probe_test drive it without any of vscreen.cpp's machinery.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11Resource;

namespace edvr {

class Config;

// advanced.pixel_probe: up to 4 points, x,y in normalised eye-texture
// coordinates (0..1), applied to both eyes.
struct PixelProbePoint {
    float x = 0.0f;
    float y = 0.0f;
};
constexpr uint32_t kPixelProbeMaxPoints = 4;

// Reads advanced.pixel_probe, live: up to 4 "x,y" points separated by ';'.
// Refuses the whole value with one log line rather than half-applying it
// (the advanced.clear_probe style, vscreen.cpp's readCensusSkip) and calls
// the narrow overload below with what it parsed. Called from vscreen.cpp
// beside objectProbeConfigure(cfg).
void pixelProbeConfigure(Config& cfg);

// Replaces the configured point set directly (0 points is off) -- the
// narrow entry a rig can drive without a Config at all.
void pixelProbeConfigure(const PixelProbePoint* points, uint32_t count);

// asked per eye draw, no cross-TU call -- the binding_shadow.h rationale:
// /O2 with no /GL means a one-line getter in the .cpp is a real call.
// WantsDraws is true only inside the probed frame (set at the boundary that
// opens it, cleared at the one that closes it), so a configured but idle
// probe costs the hooks this one load and nothing else.
namespace detail {
extern uint32_t g_pixelProbePointCount;
extern bool g_pixelProbeActive;
}
inline bool pixelProbeWantsDraws() { return detail::g_pixelProbeActive; }
inline bool pixelProbeConfigured() { return detail::g_pixelProbePointCount != 0; }

// Arms the probe for the whole frame after the next boundary. The dump key
// is polled inside Present before vScreenFrameBoundary (device_hook.cpp), so
// the frame in progress at arm time is already ending; the menu's arm lands
// mid-frame and gets the same next full frame. A run already armed, or no
// points configured, is left alone.
void pixelProbeArm();

// The frame edge: called every frame, beside objectProbeFrameBoundary.
// Closes and reports the probed frame once it has fully elapsed (Map once,
// diff, log, release); a no-op otherwise, and cheap when unarmed.
void pixelProbeFrameBoundary(ID3D11DeviceContext* ctx);

// Before a hooked draw that may land in an eye target: takes the baseline
// copy the first time this eye is seen in the probed frame. eye is 0/1 for
// a resolved eye target, -1 otherwise (a no-op then, so callers can pass it
// unconditionally). resource is RTV0's resolved identity (binding_shadow's
// ResourceInfo::resource, cast back) -- never dereferenced beyond the one
// QueryInterface/GetDesc this module needs to size the copy and decline an
// array or multisampled target.
void pixelProbeBeforeDraw(ID3D11DeviceContext* ctx, ID3D11Resource* resource, int eye);

// What a draw looked like, for the log line and the change report -- filled
// in by the caller from whatever the hook already has in hand (a shadowed
// shader hash) or a live IA/OM query for the rest (eye_draw_snapshot.h's
// capture() is the same idiom). topology/blendSrc/blendDest/depthWriteMask/
// depthFunc carry the raw D3D11 enum values.
struct DrawInfo {
    uint64_t vsHash = 0;
    uint64_t psHash = 0;
    uint32_t topology = 0;
    uint32_t count = 0;          // vertex or index count
    uint32_t instances = 1;
    bool     blendEnable = false;
    uint32_t blendSrc = 0;       // RT0
    uint32_t blendDest = 0;      // RT0
    bool     depthEnable = false;
    uint32_t depthWriteMask = 0;
    uint32_t depthFunc = 0;
    bool     indirect = false;
};

// After a hooked draw (and any EDVR reissues for it) has fully returned:
// copies this draw's window per configured point, to be diffed against the
// previous slot for the same point and eye at the closing frame boundary.
// A no-op when eye < 0, unarmed, or off the probed frame.
void pixelProbeAfterDraw(ID3D11DeviceContext* ctx, ID3D11Resource* resource, int eye,
                         const DrawInfo& info);

void pixelProbeShutdown();

}  // namespace edvr

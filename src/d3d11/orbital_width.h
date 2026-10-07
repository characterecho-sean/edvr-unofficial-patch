// fix.ui_quality -- the orbit lines' width (docs/ui-layer-2026-09-23.md, "2026-10-06: the orbit lines").
//
// The orange orbits and cyan rings Elite draws in supercruise are scene geometry (vs C7FA0C0F5DD49180, a strip of
// 8194 vertices, a handful of instances), made in the scene's render target and upscaled with the world. The vertex
// shader takes each instance's half-width in PIXELS of the target it thinks it fills (2.0 for an orbit, 1.5 for a
// ring: vertex buffer 1, the fourth float) and turns it into clip space with cb1[332].zw = 1/W, 1/H, so the strip is
// 2 x w pixels wide at the render size whatever the upscaler does after. At HMD Quality 0.5 that is 4 px of a 2016
// wide target, about 5.4 px of the 4032 wide output -- beside a HUD whose panels the engine patch (ui_panel_scale.h)
// now makes at the target's density, which is f = W_render / (W_output x target) times their game size: the lines
// look heavy. Every other part of fix.ui_quality brings what the game draws as interface to the density of the target;
// the lines are the one interface element that is scene geometry, so they follow the same factor.
//
// THE MECHANISM, the same family as ui_holo_remap.h's pixel-shader edit: the game's own bytecode, admitted by size and
// FNV hash, with ONE instruction changed -- the literal 2 of `mul r0.zw, cb1[332].zzzw, l(0,0,2,2)` becomes a read of
// a private vertex-shader constant buffer (b13, which the game's shader does not declare) holding 2 x f. The patched
// copy is made once from the bytes the game created the shader with, bound for the game's own draw of that shader
// and put back at once after it (the binding shadow never told: it keeps describing the game's shader). At f = 1 the
// copy is not used at all; with 2 in the buffer its output is byte-identical to the game's (the rig proves both).
// The coverage twin (stellar_coverage.h) reads the same factor from its own copy of that buffer, so its footprint
// stays the visible line's.
//
// WHEN: only while the panel patch is live (fix.ui_quality 100 or 125; never at off), in VR, with f < 1. The factor is
// the panel patch's written one (uiPanelScaleFactor), read once a frame at the frame boundary into the one atomic
// below, so every draw of a frame -- both eyes, the game's and the twin -- sees the same value.
//
// This header is what ui_depth.cpp includes: the factor and the twin's constant buffer, with no .cpp behind them (the
// rigs that include ui_depth.cpp whole need nothing more). orbital_width_patch.h holds the DXBC edit and the cache and
// binding the game's own draw uses; orbital_width.cpp the module.
#pragma once

#include "../common/guard.h"

#include <d3d11.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace edvr {
namespace orbital_width {

inline constexpr uint64_t kVs = 0xC7FA0C0F5DD49180ull;  // the game's orbit-line vertex shader (2412 bytes)
inline constexpr uint64_t kPs = 0x6EEF165A350DA30Full;  // and its pixel shader: shapes the alpha, unchanged here
inline constexpr uint32_t kInstanceStride = 60;         // vertex buffer 1: 15 floats an instance, the half-width the fourth
inline constexpr uint32_t kSlot = 13;                    // the private vertex-shader constant buffer: the game's shader declares none above CB1

namespace detail {
// The factor the game's draws of the orbit-line shader are made with right now: 1 unless the module has found the
// panel patch live, the shader copy ready and the factor below 1. Written by the render thread's frame boundary
// only; read by the draws on the same thread and by anything that asks.
inline std::atomic<double> g_factor{1.0};
}  // namespace detail

inline double factor() { return detail::g_factor.load(std::memory_order_relaxed); }
inline bool active() { return factor() < 1.0; }

// What the frame boundary hands to the one decision below.
struct Inputs {
    bool vr = false;            // the VR profile: the flat build has no panel patch and no orbit lines
    bool refused = false;       // the shader copy was refused or failed: stays at 1 for the session
    bool ready = false;         // the copy and its constants exist on this device
    bool panelLive = false;     // uiPanelScaleLive()
    double panelFactor = 1.0;   // uiPanelScaleFactor()
};

// The factor the draws take: the panel patch's when it is live, the shader copy is ready and the factor is below 1
// (a floor at 1 means HMD Quality is at the target already: nothing to do); otherwise exactly 1.
inline double target(const Inputs& in) {
    if (!in.vr || in.refused || !in.ready || !in.panelLive) return 1.0;
    const double f = in.panelFactor;
    if (!std::isfinite(f) || !(f > 0.0) || !(f < 1.0 - 1e-6)) return 1.0;
    return f;
}

template <class T>
inline void release(T*& value) {
    T* p = value;
    value = nullptr;
    if (p) guarded("orbital.width.release", [&] { p->Release(); });
}

// The private vertex-shader constant (b13) the patched shader and the coverage twin read: x = 2 x the factor, which is
// exactly the game's own literal 2 at f = 1; the other three lanes hold the same, so any swizzle reads it. One 16 byte
// buffer per device, made on first use and rewritten when the value changes, never per draw. A failed create or
// write is not retried on that device: get() answers null and the caller declines.
class Constants {
    ID3D11Device* device_ = nullptr;  // identity only: the buffer holds its device alive, so the address cannot be reused
    ID3D11Buffer* buffer_ = nullptr;
    float written_ = 0.0f;
    bool failed_ = false;

public:
    Constants() = default;
    Constants(const Constants&) = delete;
    Constants& operator=(const Constants&) = delete;
    ~Constants() { reset(); }

    static float value(double f) { return static_cast<float>(2.0 * f); }

    ID3D11Buffer* get(ID3D11DeviceContext* ctx, double f) {
        if (!ctx || !std::isfinite(f) || !(f > 0.0)) return nullptr;
        ID3D11Device* d = nullptr;
        if (!guarded("orbital.width.device", [&] { ctx->GetDevice(&d); }) || !d) {
            release(d);
            return nullptr;
        }
        if (d != device_) {
            reset();
            device_ = d;
        }
        const float v = value(f);
        const float lanes[4] = {v, v, v, v};
        ID3D11Buffer* result = nullptr;
        if (!failed_) {
            if (!buffer_) {
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = sizeof(lanes);
                bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                const D3D11_SUBRESOURCE_DATA init{lanes, 0, 0};
                ID3D11Buffer* made = nullptr;
                HRESULT hr = E_FAIL;
                const bool ran = guarded("orbital.width.create", [&] { hr = d->CreateBuffer(&bd, &init, &made); });
                if (!ran || FAILED(hr) || !made) {
                    release(made);
                    failed_ = true;
                } else {
                    buffer_ = made;
                    written_ = v;
                }
            } else if (v != written_) {
                if (guarded("orbital.width.write", [&] { ctx->UpdateSubresource(buffer_, 0, nullptr, lanes, 0, 0); }))
                    written_ = v;
                else
                    failed_ = true;
            }
            if (!failed_) result = buffer_;
        }
        release(d);
        return result;
    }

    bool failed() const { return failed_; }
    ID3D11Buffer* buffer() const { return buffer_; }
    void reset() {
        release(buffer_);
        device_ = nullptr;
        written_ = 0.0f;
        failed_ = false;
    }
};

}  // namespace orbital_width

// ---------------------------------------------------------------- the module (orbital_width.cpp)

// device_hook.cpp's CreateVertexShader hook: the game's orbit-line shader (hash orbital_width::kVs) is checked
// byte for byte and its patched copy made and kept; any other shader returns at the first compare. A refusal is said
// once, by reason, and leaves the game's shader as it is.
void orbitalWidthRememberVs(ID3D11VertexShader* shader, uint64_t hash, const void* bytes, size_t count, bool linked);

// vscreen.cpp, around the game's own issue of a draw whose bound vertex shader is orbital_width::kVs (owner context,
// into an eye target): Begin counts the draw and, when the factor is below 1, binds the patched copy and the factor's
// buffer, answering true; End puts the game's shader and its slot 13 back. End is called only after a true Begin.
bool orbitalWidthBegin(ID3D11DeviceContext* ctx, uint32_t instances, uint32_t startInstance);
void orbitalWidthEnd(ID3D11DeviceContext* ctx);

// Once a frame, render thread, after the panel patch's own boundary: makes the copy on the first frame it is wanted,
// decides the factor, settles a binding that did not restore, reads back the first draw's half-widths.
void orbitalWidthFrameBoundary(ID3D11DeviceContext* ctx);

// The 30 s line, from ui_layer's totals beside the panels' own.
void orbitalWidthLog();

// DLL unload: the copy, its buffer and the readback released.
void orbitalWidthShutdown();

}  // namespace edvr

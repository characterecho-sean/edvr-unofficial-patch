// fix.ui_quality -- the supercruise bars' private pass: the module. supercruise_bars.h says what and why;
// supercruise_bars_binding.h holds the constants, the rasterizer states and the binding, which
// tools/supercruise_bars_test runs on WARP; supercruise_bars_shader.h is the geometry shader's HLSL, compiled at build time.
//
// WHAT THE LOG SHOWS (a session in which this code never ran must read differently from one where it did):
//   * "supercruise bars: precompiled geometry shader supercruise bars strip created ..." (shader_swap.cpp), or the failure,
//     once, at the first decision that asks for it.
//   * "supercruise bars: not available (<reason>) ..." once per reason, when the decision cannot be served.
//   * "supercruise bars: first draw through the strip shader (frame N) ..." once, with the tent and the viewports.
//   * Every 30 s, "ui quality: supercruise bars: ...": the draws issued through the shader, what was asked and refused by
//     reason, and the layer's half (how many the HDR layer took and left, from ui_layer.cpp's decided table).
#include "supercruise_bars.h"

#include "supercruise_bars_binding.h"

#include "shader_swap.h"
#include "temporal_shader_bytecode.h"

#include "../common/guard.h"
#include "../common/log.h"

#include <windows.h>

#include <d3d11.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace edvr {

namespace sb = supercruise_bars;

namespace {

enum class Why : uint8_t {
    kNone = 0,
    kShader,      // the strip shader could not be made
    kConstants,   // its constant buffer could not be made or written
    kRaster,      // a rasterizer state could not be made
    kGameGs,      // the game has a geometry shader of its own bound
    kPending,     // a binding the last draw could not restore is waiting for the frame boundary
    kViewport,    // the layer's viewport or the game's was not usable at the issue
    kBind,        // the bind itself failed at the issue
    kRestore,     // the game's state could not be put back after the issue (settled at the frame boundary)
    kCount
};

const char* whyText(Why w) {
    switch (w) {
        case Why::kShader: return "the strip shader could not be made";
        case Why::kConstants: return "its constant buffer could not be made or written";
        case Why::kRaster: return "a rasterizer state could not be made";
        case Why::kGameGs: return "the game has a geometry shader of its own bound";
        case Why::kPending: return "a binding the last draw could not restore waits for the frame boundary";
        case Why::kViewport: return "the layer's viewport or the game's was not usable";
        case Why::kBind: return "binding the strip shader failed";
        case Why::kRestore: return "the game's state could not be put back after the issue";
        default: return "?";
    }
}

const char* whyKey(Why w) {
    switch (w) {
        case Why::kShader: return "shader";
        case Why::kConstants: return "constants";
        case Why::kRaster: return "raster-state";
        case Why::kGameGs: return "game-gs";
        case Why::kPending: return "restore-pending";
        case Why::kViewport: return "viewport";
        case Why::kBind: return "bind";
        case Why::kRestore: return "restore";
        default: return "?";
    }
}

// All of it on the render thread (the draw hook and the frame boundary); shutdown runs after the last of them.
ID3D11GeometryShader* g_gs = nullptr;
ID3D11Device* g_gsDevice = nullptr;  // identity only: the shader holds its device alive
bool g_gsTried = false;
sb::Constants g_constants;
sb::RasterStates g_raster;
sb::Binding g_binding;

float g_renderViewportW = 0.0f;  // the game's own viewport width, read by Prepare
uint32_t g_frame = 0, g_firstFrame = 0;
bool g_firstNoted = false, g_settleNoted = false;
uint32_t g_saidWhy = 0;

// The totals of the session and of the 30 s window.
uint64_t g_asked = 0, g_issued = 0, g_segments = 0, g_clamped = 0;
uint64_t g_windowAsked = 0, g_windowIssued = 0, g_windowFrames = 0;
uint64_t g_refused[static_cast<size_t>(Why::kCount)] = {};
uint64_t g_windowRefused[static_cast<size_t>(Why::kCount)] = {};
sb::StripParams g_lastParams;

// A reason the decision could not be served (kShader .. kPending): the draw stays in the scene, as the game made it. A reason
// at the issue (kViewport, kBind, kRestore): the draw was already taken into the layer, and went in without the strip shader
// (plain one-pixel lines) or with its restore left to the frame boundary. Each said once; the 30 s line counts every one.
void refuse(Why w) {
    ++g_refused[static_cast<size_t>(w)];
    ++g_windowRefused[static_cast<size_t>(w)];
    const uint32_t bit = 1u << static_cast<unsigned>(w);
    if (g_saidWhy & bit) return;
    g_saidWhy |= bit;
    const bool atIssue = w == Why::kViewport || w == Why::kBind || w == Why::kRestore;
    Log::get().note("supercruise bars: not available (%s) -- %s", whyText(w),
                    atIssue ? "at the issue: the draw went into the layer as the game's plain lines, or its state is settled at "
                              "the frame boundary. Said once; the 30 s line counts every one."
                            : "the draw stays in the scene, as the game made it. Said once; the 30 s line counts every one.");
}

void releaseGs() {
    sb::release(g_gs);
    g_gsDevice = nullptr;
}

// The strip shader, made from the build's bytecode once per device (a device that comes and goes re-makes it; a failure is
// not retried on the same device). shaderSwapCreateGs is guarded and says the outcome in the log.
bool ensureGs(ID3D11DeviceContext* ctx) {
    ID3D11Device* d = nullptr;
    if (!guarded("supercruise.bars.device", [&] { ctx->GetDevice(&d); }) || !d) {
        sb::release(d);
        return false;
    }
    if (d != g_gsDevice) {
        releaseGs();
        g_gsDevice = d;
        g_gsTried = false;
    }
    sb::release(d);
    if (!g_gs && !g_gsTried) {
        g_gsTried = true;
        g_gs = shaderSwapCreateGs(ctx, kSupercruiseBarsGsBytecode, sizeof(kSupercruiseBarsGsBytecode), "supercruise bars strip",
                                  "supercruise bars");
    }
    return g_gs != nullptr;
}

std::string refusedText(const uint64_t* counts) {
    std::string s;
    char one[64];
    for (size_t w = 1; w < static_cast<size_t>(Why::kCount); ++w) {
        if (!counts[w]) continue;
        std::snprintf(one, sizeof(one), "%s%s %llu", s.empty() ? "" : ", ", whyKey(static_cast<Why>(w)),
                      static_cast<unsigned long long>(counts[w]));
        s += one;
    }
    return s.empty() ? std::string("none") : s;
}

}  // namespace

bool supercruiseBarsReady(ID3D11DeviceContext* ctx, const char** why) {
    if (why) *why = "";
    ++g_asked;
    ++g_windowAsked;
    Why w = Why::kNone;
    if (!ctx) {
        w = Why::kBind;
    } else if (g_binding.needsRestore()) {
        w = Why::kPending;
    } else if (!ensureGs(ctx)) {
        w = Why::kShader;
    } else {
        // The constants and both states are made here, once, with a placeholder the first draw's own numbers replace: so the
        // decision that takes a draw has every private object in hand before the HDR layer is made for it.
        const sb::StripParams probe{1.0f, 1.0f, 1.0f, sb::kClipW};
        if (!g_constants.get(ctx, probe)) {
            w = Why::kConstants;
        } else if (!g_raster.get(ctx, false) || !g_raster.get(ctx, true)) {
            w = Why::kRaster;
        } else {
            ID3D11GeometryShader* game = nullptr;
            bool read = guarded("supercruise.bars.ready.gs", [&] { ctx->GSGetShader(&game, nullptr, nullptr); });
            const bool bound = game != nullptr;
            sb::release(game);
            if (!read) w = Why::kBind;
            else if (bound) w = Why::kGameGs;
        }
    }
    if (w == Why::kNone) return true;
    refuse(w);
    if (why) *why = whyText(w);
    return false;
}

void supercruiseBarsPrepare(ID3D11DeviceContext* ctx) {
    g_renderViewportW = 0.0f;
    if (!ctx) return;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT n = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    if (guarded("supercruise.bars.prepare", [&] { ctx->RSGetViewports(&n, vp); }) && n >= 1) g_renderViewportW = vp[0].Width;
}

bool supercruiseBarsBegin(ID3D11DeviceContext* ctx, uint32_t vertices) {
    if (!ctx || !g_gs) {
        refuse(Why::kBind);
        return false;
    }
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT n = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    const bool read = guarded("supercruise.bars.viewport", [&] { ctx->RSGetViewports(&n, vp); });
    bool clamped = false;
    const float half = read && n >= 1 ? sb::tentHalfWidth(vp[0].Width, g_renderViewportW, &clamped) : 0.0f;
    if (!(half > 0.0f) || !(vp[0].Height > 0.0f)) {
        refuse(Why::kViewport);
        return false;
    }
    if (clamped) ++g_clamped;
    const sb::StripParams params{vp[0].Width, vp[0].Height, half, sb::kClipW};
    ID3D11Buffer* constants = g_constants.get(ctx, params);
    if (!constants) {
        refuse(Why::kConstants);
        return false;
    }
    const auto begun = g_binding.begin(ctx, g_gs, constants, [&](bool scissor) { return g_raster.get(ctx, scissor); });
    if (begun == sb::Binding::Begin::kGameHasGs) {
        refuse(Why::kGameGs);
        return false;
    }
    if (begun != sb::Binding::Begin::kBound) {
        refuse(Why::kBind);
        return false;
    }
    ++g_issued;
    ++g_windowIssued;
    g_segments += vertices / 2;
    g_lastParams = params;
    if (!g_firstNoted) {
        g_firstNoted = true;
        g_firstFrame = g_frame;
        Log::get().note("supercruise bars: first draw through the strip shader (frame %u) -- %u vertices (%u segments) of the "
                        "game's line list are drawn as triangle strips with a tent alpha profile, half-width %.2f layer "
                        "pixels (the layer's viewport %.0fx%.0f over the game's %.0f wide: one render pixel of the line the "
                        "game drew, which is the weight the tent keeps), cut at clip w %.3f; the game's pixel shader, blend "
                        "and everything else are unchanged, and its geometry stage, constant slot and rasterizer state go "
                        "back after the issue.",
                        g_frame, vertices, vertices / 2, static_cast<double>(half), static_cast<double>(vp[0].Width),
                        static_cast<double>(vp[0].Height), static_cast<double>(g_renderViewportW),
                        static_cast<double>(sb::kClipW));
    }
    return true;
}

void supercruiseBarsEnd(ID3D11DeviceContext* ctx) {
    const auto done = g_binding.finish(ctx);
    if (!done.restored) refuse(Why::kRestore);
}

void supercruiseBarsFrameBoundary(ID3D11DeviceContext* ctx) {
    ++g_frame;
    ++g_windowFrames;
    if (g_binding.needsRestore() && ctx && g_binding.settle(ctx) && !g_settleNoted) {
        g_settleNoted = true;
        Log::get().note("supercruise bars: the game's geometry stage, constant slot and rasterizer state were put back at the "
                        "frame boundary.");
    }
}

void supercruiseBarsLog(const char* layerText) {
    const uint64_t asked = g_windowAsked, issued = g_windowIssued;
    const double frames = g_windowFrames ? static_cast<double>(g_windowFrames) : 1.0;
    const std::string refused = refusedText(g_windowRefused);
    g_windowAsked = g_windowIssued = g_windowFrames = 0;
    for (auto& r : g_windowRefused) r = 0;
    const char* layer = layerText && *layerText ? layerText : "layer text not supplied";
    if (issued) {
        Log::get().note("ui quality: supercruise bars: %llu draw(s) this window through the strip shader (%.2f a frame), %llu in "
                        "all since frame %u; tent half-width %.2f layer px in a %.0fx%.0f viewport, %llu segments in all%s; "
                        "asked %llu, refused: %s; %s.",
                        static_cast<unsigned long long>(issued), static_cast<double>(issued) / frames,
                        static_cast<unsigned long long>(g_issued), g_firstFrame, static_cast<double>(g_lastParams.halfWidth),
                        static_cast<double>(g_lastParams.viewportW), static_cast<double>(g_lastParams.viewportH),
                        static_cast<unsigned long long>(g_segments), g_clamped ? " (the tent was clamped at least once)" : "",
                        static_cast<unsigned long long>(asked), refused.c_str(), layer);
    } else if (asked) {
        Log::get().note("ui quality: supercruise bars: %llu draw(s) asked of the private pass this window, none issued through "
                        "the strip shader; refused: %s; %s.",
                        static_cast<unsigned long long>(asked), refused.c_str(), layer);
    } else {
        Log::get().note("ui quality: supercruise bars: none asked of the private pass this window (strip shader %s); refused: "
                        "%s; %s.",
                        g_gs ? "made" : (g_gsTried ? "not made" : "not yet needed"), refused.c_str(), layer);
    }
}

void supercruiseBarsShutdown() {
    g_binding.clear();
    g_constants.reset();
    g_raster.reset();
    releaseGs();
    g_gsTried = false;
}

}  // namespace edvr

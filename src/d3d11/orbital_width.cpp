// fix.ui_quality -- the orbit lines' width: the module. orbital_width.h says what and why; orbital_width_patch.h holds
// the DXBC edit, the cache and the binding, which tools/orbital_width_test runs on WARP.
//
// WHAT THE LOG SHOWS. Once, when the game creates the shader: "orbit lines: captured ..." (the creation hook ran and the
// patched copy exists) or a refusal by reason. Once, at the first scaled draw: "orbit lines: first scaled draw ..."
// with the factor, and a line after it with the half-widths the instance stream held (a copy read back without
// waiting). Every 30 s beside the panels' line: "ui quality: orbit lines: ...", whose draw counts are what tell a
// session that never ran this code (no capture line, no first draw, zero scaled) from one at f = 1 (draws seen,
// none scaled, the factor named).
#include "orbital_width.h"

#include "orbital_width_patch.h"
#include "ui_panel_scale.h"
#include "vscreen.h"

#include "../common/log.h"
#include "../common/runtime_profile.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace edvr {

namespace ow = orbital_width;

namespace {

ow::Cache g_cache;
ow::Binding g_binding;
ow::Constants g_constants;
ow::InstanceProbe g_probe;

// The first refusal stands the module down for the session: factor 1, the game's own shader. Written by the creation
// threads (the capture) and the render thread; each reason is said once.
std::atomic<uint8_t> g_refused{0};
std::atomic<uint32_t> g_saidReasons{0};
std::atomic<bool> g_capturedNoted{false};

// The render thread's own.
uint32_t g_frame = 0, g_firstFrame = 0;
uint64_t g_seen = 0, g_scaled = 0, g_windowSeen = 0, g_windowScaled = 0;
bool g_firstNoted = false;
bool g_settleNoted = false;

void refuse(ow::Why why, const std::string& detail) {
    if (why == ow::Why::kNone || why == ow::Why::kNotOurs) return;
    uint8_t none = 0;
    g_refused.compare_exchange_strong(none, static_cast<uint8_t>(why), std::memory_order_acq_rel);
    ow::detail::g_factor.store(1.0, std::memory_order_relaxed);
    const uint32_t bit = 1u << static_cast<unsigned>(why);
    if (g_saidReasons.fetch_or(bit, std::memory_order_acq_rel) & bit) return;
    Log::get().note("orbit lines: NOT scaled -- %s: %s. The game's own shader draws them, as it always did.",
                    ow::whyName(why), detail.c_str());
}

// "2.00 x1, 1.50 x4": the half-widths, each value once with its count (values within 0.005 px are one).
std::string widthsText(const float* w, uint32_t n, double factor) {
    float values[ow::InstanceProbe::kMaxInstances];
    uint32_t counts[ow::InstanceProbe::kMaxInstances];
    uint32_t kinds = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t k = 0;
        while (k < kinds && std::fabs(values[k] - w[i]) > 0.005f) ++k;
        if (k == kinds) {
            values[kinds] = w[i];
            counts[kinds++] = 0;
        }
        ++counts[k];
    }
    std::string text;
    char one[64];
    for (uint32_t k = 0; k < kinds; ++k) {
        std::snprintf(one, sizeof(one), "%s%.2f px x%u (%.2f)", k ? ", " : "", static_cast<double>(values[k]), counts[k],
                      static_cast<double>(values[k]) * factor);
        text += one;
    }
    return text;
}

void pollProbe(ID3D11DeviceContext* ctx) {
    if (!g_probe.pending()) return;
    float widths[ow::InstanceProbe::kMaxInstances] = {};
    uint32_t count = 0;
    switch (g_probe.poll(ctx, g_frame, widths, ow::InstanceProbe::kMaxInstances, &count)) {
        case ow::InstanceProbe::Poll::kDone:
            Log::get().note("orbit lines: the first scaled draw's instance stream held %u half-widths, in the game's "
                            "own pixels of its render target (drawn as the bracketed value, x%.4f): %s.",
                            count, ow::factor(), widthsText(widths, count, ow::factor()).c_str());
            break;
        case ow::InstanceProbe::Poll::kFailed:
            Log::get().note("orbit lines: the first scaled draw's half-widths could not be read back (the instance "
                            "stream was not the 60-byte records the shader reads, or the copy did not arrive in four "
                            "seconds); the scaling does not depend on it.");
            break;
        default:
            break;
    }
}

}  // namespace

void orbitalWidthRememberVs(ID3D11VertexShader* shader, uint64_t hash, const void* bytes, size_t count, bool linked) {
    if (hash != ow::kVs) return;
    const ow::Result r = g_cache.remember(shader, hash, bytes, count, linked);
    if (!r.ok()) {
        refuse(r.why, r.detail);
        return;
    }
    if (!g_capturedNoted.exchange(true, std::memory_order_acq_rel))
        Log::get().note("orbit lines: captured the game's orbit-line vertex shader %016llX (%zu bytes, build 332841's "
                        "program, checked instruction by instruction) and made its patched copy: the half-width literal "
                        "2 of its clip-space offset read from a private b13. The copy draws the game's own orbit lines "
                        "while the panel patch is live and its factor is below 1; otherwise the game's shader does.",
                        static_cast<unsigned long long>(hash), count);
}

bool orbitalWidthBegin(ID3D11DeviceContext* ctx, uint32_t instances, uint32_t startInstance) {
    ++g_seen;
    ++g_windowSeen;
    const double f = ow::factor();
    if (!(f < 1.0)) return false;
    ID3D11VertexShader* patched = g_cache.shader();
    ID3D11Buffer* constants = patched ? g_constants.get(ctx, f) : nullptr;
    if (!patched || !constants) {
        refuse(patched ? ow::Why::kConstants : ow::Why::kCreate, "the patched shader or the factor's buffer is not there at a draw "
                                                          "the frame boundary had found them ready for");
        return false;
    }
    // A binding the last draw could not give back is settled at the frame boundary; this draw is the game's own.
    if (g_binding.needsRestore()) return false;
    const bool bound = g_binding.begin(
        ctx, patched, constants, [&](ID3D11VertexShader* now) { return ow::Cache::isOriginal(now, ctx); }, vScreenVSSetShaderRaw);
    if (!bound) {
        refuse(ow::Why::kBind, "at a draw the binding shadow named as the orbit-line shader, the vertex shader bound was not "
                           "the one the creation hook checked (or it carries class instances)");
        return false;
    }
    ++g_scaled;
    ++g_windowScaled;
    if (!g_firstNoted) {
        g_firstNoted = true;
        g_firstFrame = g_frame;
        Log::get().note("orbit lines: first scaled draw (frame %u) -- the game's draw of vs %016llX, %u instance(s), is "
                        "issued through the patched copy of that shader with its line half-width multiplied by %.4f, the "
                        "panel patch's factor: the lines are made at the density the HUD panels are, not the render "
                        "size's. The coverage twin follows the same factor. The half-widths the instance stream holds "
                        "are read back next (a later line).",
                        g_frame, static_cast<unsigned long long>(ow::kVs), instances, f);
        g_probe.begin(ctx, instances, startInstance, g_frame);
    }
    return true;
}

void orbitalWidthEnd(ID3D11DeviceContext* ctx) {
    const auto done = g_binding.finish(ctx, vScreenVSSetShaderRaw);
    if (!done.restored)
        refuse(ow::Why::kRestore, "the game's vertex shader or its slot 13 could not be put back after the draw; the frame "
                              "boundary settles it");
}

void orbitalWidthFrameBoundary(ID3D11DeviceContext* ctx) {
    ++g_frame;
    if (g_binding.needsRestore() && g_binding.settle(ctx, vScreenVSSetShaderRaw) && !g_settleNoted) {
        g_settleNoted = true;
        Log::get().note("orbit lines: the game's vertex shader and its slot 13 were put back at the frame boundary.");
    }
    pollProbe(ctx);
    ow::Inputs in;
    in.vr = runtimeVrProfile();
    in.refused = g_refused.load(std::memory_order_acquire) != 0;
    in.panelLive = uiPanelScaleLive();
    in.panelFactor = in.panelLive ? uiPanelScaleFactor() : 1.0;
    // The copy and the factor's buffer are made on the first frame they are wanted (a device call, so here, on the
    // render thread, and not in the creation hook), and kept; their failure is a refusal for the session.
    if (in.vr && !in.refused && in.panelLive && in.panelFactor < 1.0 - 1e-6 && g_cache.remembered()) {
        ow::Result r;
        ID3D11VertexShader* shader = g_cache.prepare(ctx, &r);
        if (!shader && !r.ok()) {
            refuse(r.why, r.detail);
            in.refused = true;
        } else if (shader && !g_constants.get(ctx, in.panelFactor)) {
            refuse(ow::Why::kConstants, "the factor's 16-byte constant buffer could not be made or written");
            in.refused = true;
        }
        in.ready = shader != nullptr && !in.refused;
    }
    ow::detail::g_factor.store(ow::target(in), std::memory_order_relaxed);
}

void orbitalWidthLog() {
    const ow::Why why = static_cast<ow::Why>(g_refused.load(std::memory_order_acquire));
    const bool live = uiPanelScaleLive();
    const uint64_t seen = g_windowSeen, scaled = g_windowScaled;
    g_windowSeen = g_windowScaled = 0;
    if (!live && !g_scaled && why == ow::Why::kNone) return;  // fix.ui_quality is off: nothing to say
    if (why != ow::Why::kNone) {
        Log::get().note("ui quality: orbit lines: not scaled (%s) -- %llu draw(s) this window with the game's own shader.",
                        ow::whyName(why), static_cast<unsigned long long>(seen));
    } else if (scaled) {
        Log::get().note("ui quality: orbit lines: half-width x%.4f (the panel patch's factor) -- %llu of %llu orbit-line "
                        "draws this window through the shader copy, %llu in all, since frame %u.",
                        ow::factor(), static_cast<unsigned long long>(scaled), static_cast<unsigned long long>(seen),
                        static_cast<unsigned long long>(g_scaled), g_firstFrame);
    } else if (seen) {
        Log::get().note("ui quality: orbit lines: %llu draw(s) this window, none scaled -- %s.",
                        static_cast<unsigned long long>(seen),
                        !live            ? "the panel patch is not live"
                        : !g_cache.remembered() ? "the game's orbit-line shader was not captured at its creation"
                        : !(ow::factor() < 1.0)     ? "the factor is 1 (HMD Quality is at the target, or the panels are not under the engine's sizing)"
                                                : "the shader copy is not ready");
    } else {
        Log::get().note("ui quality: orbit lines: none drawn this window (shader %s, factor %.4f).",
                        g_cache.remembered() ? "captured" : "not captured", ow::factor());
    }
}

void orbitalWidthShutdown() {
    ow::detail::g_factor.store(1.0, std::memory_order_relaxed);
    g_binding.clear();
    g_probe.reset();
    g_constants.reset();
    g_cache.reset();
}

}  // namespace edvr

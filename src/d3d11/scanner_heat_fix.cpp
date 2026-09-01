#include "scanner_heat_fix.h"

#include <windows.h>

#include <cstdint>
#include <string>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "exposure_fix.h"   // lookupShaderHash: the shared shader registry

namespace edvr {
namespace {

// The two filter-overlay pixel shaders (2026-09-01), measured from the
// census's ph= column in the fumaroles capture and confirmed absent from
// every unfiltered and normal-space capture. Matching on the PIXEL shader
// because it is the overlay's material; the vertex shader is a general
// world-quad pipeline the mapped-area fill shares.
constexpr uint64_t kHeatFill   = 0x3B47A4BCE1891CC8ULL;  // vs A1E56637
constexpr uint64_t kHeatMarker = 0x5FC9FC1E3B008DF1ULL;  // vs 7C5DA553

// The shell the game draws over the planet just before the overlay,
// carrying its own depth WRITE (census DC 0 #44, ds=17wA) where the
// overlay's own draws carry none (ds=17wZ) -- the depth-rejection
// hypothesis's other half. Matched the same way, on the pixel shader alone.
constexpr uint64_t kShellPs = 0x6EF82262EB12A037ULL;  // vs 41E245D488BFE83E

enum class Mode : uint32_t { kOverlay, kShell };

FaultBudget g_budget("scannerHeat", 8);

bool     g_on = false;
Mode     g_mode = Mode::kOverlay;
uint32_t g_passes = 0;

// The shell recognizer's only knowledge that a filter is selected: the
// frame number the overlay was last matched on, in EITHER mode. The overlay
// shaders exist nowhere outside the scanner with a filter up, so this one
// sighting is the whole gate.
uint32_t g_overlaySeenFrame = 0;

// The derived depth-stencil state. ONE slot rather than one per mode,
// because only one mode is ever live and a mode switch drops it outright
// (see scannerHeatConfigure) instead of trying to keep both around -- it
// was built from a desc for whichever draw the OTHER mode touches, and
// reusing it across the switch would apply the wrong field to the wrong
// draw.
ID3D11DepthStencilState* g_derived = nullptr;
bool                     g_derivedTried = false;
ID3D11DepthStencilState* g_savedDs = nullptr;
UINT                     g_savedRef = 0;
bool                     g_engaged = false;

uint32_t g_applied = 0;        // overlay draws the kScannerHeat verdict carried
uint32_t g_shellApplied = 0;   // shell draws actually redrawn with write off

void releaseState() {
    if (g_derived) {
        g_derived->Release();
        g_derived = nullptr;
    }
    g_derivedTried = false;
}

// The shared tail of both Begin/End pairs: put the game's state back and
// release the reference. Never called with nothing engaged, and it clears
// the flag before doing anything else, the same shape resolve_probe.cpp
// uses, so a fault mid-restore cannot leave the next draw thinking it still
// owes a restore.
void restoreDs(ID3D11DeviceContext* ctx) {
    if (!g_engaged || !ctx) return;
    g_engaged = false;
    guardedBudget(g_budget, [&] {
        ctx->OMSetDepthStencilState(g_savedDs, g_savedRef);
        if (g_savedDs) {
            g_savedDs->Release();
            g_savedDs = nullptr;
        }
    });
}

}  // namespace

void scannerHeatConfigure(Config& cfg) {
    const std::string v = cfg.getString("fix.scanner_heat", "off");
    const bool want = (v == "on" || v == "1");

    // overlay|shell, the same two-choice shape advanced.cull_guard_submit
    // uses: anything other than the word "shell" reads as overlay, which is
    // also the default and the mode the first field test ran under.
    const std::string modeWord =
        cfg.getString("advanced.scanner_heat_mode", "overlay");
    const Mode mode = (modeWord == "shell") ? Mode::kShell : Mode::kOverlay;

    // 0..6: zero is the shipped default now that the overlay is expected to
    // reach its own stock strength once whichever mode stops it being
    // rejected -- the days when 2 was "the default" were tuning against a
    // mis-registered 7% this fix never actually explained. Six is four-plus
    // times over, well past flat.
    const uint32_t passes = static_cast<uint32_t>(
        cfg.getIntInRange("advanced.scanner_heat_passes", 0, 0, 6));

    // A session that STARTS with the fix off says so, once -- the same
    // lesson resolve_bind_fix learned the same day: this key defaults off,
    // so without this line every stock session's bundle is silent about a
    // state the next report will need.
    static bool announced = false;
    const bool modeChanged = (mode != g_mode);
    const bool changed =
        (want != g_on) || (want && (passes != g_passes || modeChanged));
    g_on = want;
    g_passes = passes;
    if (modeChanged) {
        g_mode = mode;
        // Derived from a desc for the OTHER draw -- the overlay's own or
        // the shell's own -- so it cannot survive the switch. Dropped here;
        // the next engagement builds fresh from whichever draw the new
        // mode actually touches.
        releaseState();
    }
    if (!changed) {
        if (!announced && !g_on) {
            Log::get().note("scanner heat fix off: the filter overlay is "
                            "left at the game's own strength.");
        }
        announced = true;
        return;
    }
    announced = true;

    if (g_on) {
        if (g_mode == Mode::kOverlay) {
            Log::get().note(
                "scanner heat fix ON: the DSS signal filter's overlay is "
                "drawn with its depth test off, so the atmosphere shell the "
                "game stamps in front of the planet can no longer reject "
                "it. Touches only the two shaders that exist while a "
                "filter is selected; extra passes: %u.",
                g_passes);
        } else {
            Log::get().note(
                "scanner heat fix ON (shell mode): the atmosphere shell the "
                "game stamps in front of the planet before the DSS signal "
                "filter's overlay is drawn with its depth WRITE off, so it "
                "can no longer reject the overlay that follows it. The "
                "overlay itself is left exactly as the game issues it; "
                "extra passes: %u.",
                g_passes);
        }
    } else {
        Log::get().note("scanner heat fix off: the filter overlay is left at "
                        "the game's own strength.");
        releaseState();
    }
}

bool scannerHeatWants() { return g_on; }

bool scannerHeatOnDraw(ID3D11DeviceContext* ctx, char kind, uint32_t frameNo) {
    if (!g_on || !ctx || kind != 'X') return false;
    bool match = false;
    guardedBudget(g_budget, [&] {
        ID3D11PixelShader* ps = nullptr;
        ctx->PSGetShader(&ps, nullptr, nullptr);
        if (ps) {
            const uint64_t h = lookupShaderHash(ps);
            match = (h == kHeatFill || h == kHeatMarker);
            ps->Release();
        }
    });
    if (match) g_overlaySeenFrame = frameNo;
    return match;
}

bool scannerHeatShellOnDraw(ID3D11DeviceContext* ctx, char kind,
                            uint32_t frameNo) {
    if (!g_on || g_mode != Mode::kShell || !ctx || kind != 'X') return false;
    // The shell draws BEFORE the overlay within a frame, so the latch this
    // reads is necessarily last frame's -- a filter picked THIS frame is
    // caught one frame later, which is fine. Slack of two rather than one
    // so a single dropped frame does not cost the latch.
    if (g_overlaySeenFrame == 0 || frameNo - g_overlaySeenFrame > 2) {
        return false;
    }
    bool match = false;
    guardedBudget(g_budget, [&] {
        ID3D11PixelShader* ps = nullptr;
        ctx->PSGetShader(&ps, nullptr, nullptr);
        if (ps) {
            match = (lookupShaderHash(ps) == kShellPs);
            ps->Release();
        }
    });
    return match;
}

void scannerHeatBegin(ID3D11DeviceContext* ctx) {
    if (!ctx || g_mode != Mode::kOverlay) return;
    guardedBudget(g_budget, [&] {
        // The game's own state is read first and DERIVED from, every time
        // this engages, because authoring one from nothing would change
        // comparisons and masks nobody asked about.
        ctx->OMGetDepthStencilState(&g_savedDs, &g_savedRef);
        if (!g_derived && !g_derivedTried) {
            g_derivedTried = true;
            D3D11_DEPTH_STENCIL_DESC d{};
            if (g_savedDs) {
                g_savedDs->GetDesc(&d);
            } else {
                // No state bound is the D3D11 default: depth on, LESS,
                // write all, stencil off. Spelled out rather than left
                // zeroed, which would silently mean depth OFF already.
                d.DepthEnable = TRUE;
                d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
                d.DepthFunc = D3D11_COMPARISON_LESS;
                d.StencilEnable = FALSE;
            }
            // The game's own values, captured BEFORE anything of ours
            // changes them -- a line that prints OUR values under THE
            // GAME'S name is worse than no line at all.
            const bool wasDepth = d.DepthEnable != FALSE;
            const unsigned wasFunc = static_cast<unsigned>(d.DepthFunc);
            const bool wasWriteAll =
                d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
            const bool wasStencil = d.StencilEnable != FALSE;

            // The rasterizer state for the SAME draw, read here rather than
            // guessed at: with depth off, whether the planet's far
            // hemisphere can show through the near one depends entirely on
            // whether back-face culling is doing that job instead.
            D3D11_RASTERIZER_DESC rd{};
            ID3D11RasterizerState* savedRs = nullptr;
            ctx->RSGetState(&savedRs);
            if (savedRs) {
                savedRs->GetDesc(&rd);
                savedRs->Release();
            } else {
                rd.CullMode = D3D11_CULL_BACK;
                rd.FrontCounterClockwise = FALSE;
                rd.DepthBias = 0;
                rd.SlopeScaledDepthBias = 0.0f;
                rd.DepthBiasClamp = 0.0f;
            }

            d.DepthEnable = FALSE;
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (dev) {
                dev->CreateDepthStencilState(&d, &g_derived);
                dev->Release();
            }
            if (g_derived) {
                Log::get().note(
                    "scanner heat fix: THE GAME'S depth-stencil for the "
                    "overlay draw is depth=%s func=%u write=%s stencil=%s "
                    "ref=%u; OURS is depth=off, everything else unchanged. "
                    "The game's rasterizer for the same draw is cull=%s "
                    "front=%s depthbias=%d slopebias=%.4f biasclamp=%.4f. "
                    "Counting silently from here.",
                    wasDepth ? "on" : "off", wasFunc,
                    wasWriteAll ? "all" : "zero",
                    wasStencil ? "on" : "off", g_savedRef,
                    rd.CullMode == D3D11_CULL_NONE    ? "none"
                    : rd.CullMode == D3D11_CULL_FRONT ? "front"
                                                       : "back",
                    rd.FrontCounterClockwise ? "ccw" : "cw", rd.DepthBias,
                    static_cast<double>(rd.SlopeScaledDepthBias),
                    static_cast<double>(rd.DepthBiasClamp));
            } else {
                Log::get().note(
                    "scanner heat fix: could not create the depth-off state "
                    "for the overlay draw -- drawing stock from here, the "
                    "overlay stays exactly as the game issues it.");
            }
        }
        if (!g_derived) {
            if (g_savedDs) {
                g_savedDs->Release();
                g_savedDs = nullptr;
            }
            return;   // draw stock
        }
        ctx->OMSetDepthStencilState(g_derived, g_savedRef);
        g_engaged = true;
    });
}

void scannerHeatEnd(ID3D11DeviceContext* ctx) { restoreDs(ctx); }

void scannerHeatShellBegin(ID3D11DeviceContext* ctx) {
    if (!ctx || g_mode != Mode::kShell) return;
    guardedBudget(g_budget, [&] {
        ctx->OMGetDepthStencilState(&g_savedDs, &g_savedRef);
        if (!g_derived && !g_derivedTried) {
            g_derivedTried = true;
            D3D11_DEPTH_STENCIL_DESC d{};
            if (g_savedDs) {
                g_savedDs->GetDesc(&d);
            } else {
                d.DepthEnable = TRUE;
                d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
                d.DepthFunc = D3D11_COMPARISON_LESS;
                d.StencilEnable = FALSE;
            }
            const bool wasDepth = d.DepthEnable != FALSE;
            const unsigned wasFunc = static_cast<unsigned>(d.DepthFunc);
            const bool wasWriteAll =
                d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
            const bool wasStencil = d.StencilEnable != FALSE;

            d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (dev) {
                dev->CreateDepthStencilState(&d, &g_derived);
                dev->Release();
            }
            if (g_derived) {
                Log::get().note(
                    "scanner heat fix (shell mode): THE GAME'S depth-"
                    "stencil for the shell draw is depth=%s func=%u "
                    "write=%s stencil=%s ref=%u; OURS is write=zero, "
                    "everything else unchanged -- the shell still "
                    "depth-tests against itself, it just stops stamping a "
                    "nearer value for the overlay behind it to fail "
                    "against. Counting silently from here.",
                    wasDepth ? "on" : "off", wasFunc,
                    wasWriteAll ? "all" : "zero",
                    wasStencil ? "on" : "off", g_savedRef);
            } else {
                Log::get().note(
                    "scanner heat fix (shell mode): could not create the "
                    "write-off state for the shell draw -- drawing stock "
                    "from here, the shell keeps stamping depth exactly as "
                    "the game issues it.");
            }
        }
        if (!g_derived) {
            if (g_savedDs) {
                g_savedDs->Release();
                g_savedDs = nullptr;
            }
            return;   // draw stock
        }
        ctx->OMSetDepthStencilState(g_derived, g_savedRef);
        g_engaged = true;
        ++g_shellApplied;
    });
}

void scannerHeatShellEnd(ID3D11DeviceContext* ctx) { restoreDs(ctx); }

uint32_t scannerHeatExtraPasses() { return g_passes; }

// Counts every overlay draw the kScannerHeat verdict carried, in both
// modes: passes are a strength knob independent of which mode is fixing
// the rejection, so this tally doubles as "was a filter ever up" in shell
// mode, where the overlay draw itself is otherwise left alone and
// scannerHeatBegin never has anything of its own to report.
void scannerHeatNoteApplied() { ++g_applied; }

void scannerHeatShutdown() {
    if (g_applied || g_shellApplied) {
        Log::get().note(
            "scanner heat fix: %u overlay draw(s) handled this session (%u "
            "extra pass(es) each), %u shell draw(s) handled. A zero count "
            "on both means no signal filter was up while the fix was on.",
            g_applied, g_passes, g_shellApplied);
    }
    g_applied = 0;
    g_shellApplied = 0;
    g_overlaySeenFrame = 0;
    releaseState();
    if (g_savedDs) {
        g_savedDs->Release();
        g_savedDs = nullptr;
    }
    g_engaged = false;
}

}  // namespace edvr

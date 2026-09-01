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

FaultBudget g_budget("scannerHeat", 8);

bool     g_on = false;
uint32_t g_passes = 2;        // EXTRA re-issues; total strength is passes+1

uint32_t g_applied = 0;       // matched draws boosted this session
bool     g_engagedNoted = false;

}  // namespace

void scannerHeatConfigure(Config& cfg) {
    const std::string v = cfg.getString("fix.scanner_heat", "off");
    const bool want = (v == "on" || v == "1");
    // 1..6: one extra pass barely lifts it, six is four-plus times over and
    // well past flat before it ever reaches garish. Default two -- three
    // times the stock contribution, which lands the measured 7% near 20%.
    const uint32_t passes = static_cast<uint32_t>(
        cfg.getIntInRange("advanced.scanner_heat_passes", 2, 1, 6));

    // A session that STARTS with the fix off says so, once -- the same
    // lesson resolve_bind_fix learned the same day: this key defaults off,
    // so without this line every stock session's bundle is silent about a
    // state the next faint-heat-map report will need.
    static bool announced = false;
    const bool changed = (want != g_on) || (want && passes != g_passes);
    g_on = want;
    g_passes = passes;
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
        g_engagedNoted = false;
        Log::get().note(
            "scanner heat fix ON: the DSS signal filter's blue overlay is "
            "re-drawn %u extra time(s) -- %ux its stock strength -- so it "
            "reads in VR the way it does on a flat monitor. Additive and "
            "depth-read-only, so it can only brighten where the overlay "
            "already is. Off outside the scanner and whenever no filter is "
            "selected.",
            g_passes, g_passes + 1);
    } else {
        Log::get().note("scanner heat fix off: the filter overlay is left at "
                        "the game's own strength.");
    }
}

bool scannerHeatWants() { return g_on; }

bool scannerHeatOnDraw(ID3D11DeviceContext* ctx, char kind) {
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
    return match;
}

uint32_t scannerHeatExtraPasses() { return g_passes; }

void scannerHeatNoteApplied() {
    ++g_applied;
    if (!g_engagedNoted) {
        g_engagedNoted = true;
        Log::get().note(
            "scanner heat fix: engaged -- the filter overlay is being "
            "boosted. Counting silently from here.");
    }
}

void scannerHeatShutdown() {
    if (g_applied) {
        Log::get().note(
            "scanner heat fix: %u overlay draw(s) boosted this session "
            "(%u extra pass(es) each). A count of zero means no signal "
            "filter was up while it was on.",
            g_applied, g_passes);
    }
    g_applied = 0;
    g_engagedNoted = false;
}

}  // namespace edvr

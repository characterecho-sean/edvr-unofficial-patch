#include "wake_pulse.h"

#include <windows.h>

#include <string>

#include "../common/config.h"
#include "../common/log.h"

namespace edvr {
namespace {

// The panel surface, by its ASPECT.
//
// This shipped keyed on the surface's fraction of the eye, and that was
// wrong. Measured across three render configurations:
//
//   eye 5424x5356   2440x1996   aspect 1.22244   w/eye 0.4499
//   eye 4340x4284   1952x1597   aspect 1.22229   w/eye 0.4498
//   eye 3072x3264   1492x1221   aspect 1.22195   w/eye 0.4857
//
// The aspect holds to four figures; the width fraction does not, because
// the surface tracks the game's INTERNAL render width, which is not a fixed
// multiple of the submitted eye. On the third configuration the fraction is
// 0.036 out -- nine times the tolerance this used -- so the fix silently
// never engaged there at all. Reported from the field before it was found
// here, which is the honest order.
//
// Each panel has its own stable aspect (1.222 here, 1.600, 4.65 and 0.667
// for the others), so this is what identifies one panel among them.
constexpr float kAspect = 1.2224f;
constexpr float kAspectTol = 0.01f;

// And a floor, so a small texture that happens to be 1.22:1 cannot match.
// The real surface is a render target sized from the display.
constexpr uint32_t kMinWidth = 512;

constexpr char     kKind = 'X';
constexpr uint32_t kIndices = 1728;   // 288 quads, on game build 332753

bool     g_off = false;
uint32_t g_indices = kIndices;
uint64_t g_dropped = 0;
uint64_t g_lastReported = 0;   // the count the last line printed
uint32_t g_reports = 0;        // how many lines have been printed at all

// Told apart so a field report can say WHICH half failed: the panel was
// never found, or it was found and no draw of the named size landed in it.
uint64_t g_surfaceSeen = 0;
uint32_t g_panelW = 0, g_panelH = 0;
bool     g_saidNoMatch = false;

}  // namespace

void wakePulseConfigure(Config& cfg) {
    const bool was = g_off;
    const std::string m = cfg.getString("fix.wake_pulse", "off");
    if (m == "stock") {
        g_off = false;
    } else if (m == "off") {
        g_off = true;
    } else {
        g_off = false;
        Log::get().note("wake_pulse \"%s\" is not stock or off; running "
                        "stock.", m.c_str());
    }
    g_indices = static_cast<uint32_t>(
        cfg.getIntInRange("advanced.wake_pulse_indices",
                          static_cast<int>(kIndices), 1, 1000000));

    if (was != g_off) {
        Log::get().note(
            "wake pulse: %s. With a high wake selected, a marker under the "
            "speed readout flashes in time with the target indicator -- both "
            "are drawn into the same holo panel. off drops the one draw that "
            "paints it (%u indices of the GUI's textureless vector shader) "
            "on the frames the flash is on, and leaves the rest of the panel "
            "alone. If the count below stays at zero, the index count has "
            "moved: re-derive it with a census and set "
            "advanced.wake_pulse_indices.",
            g_off ? "off" : "stock", g_indices);
    }
}

bool wakePulseWantsDraws() { return g_off; }

bool wakePulseSkips(char kind, uint32_t count, uint32_t targetW,
                    uint32_t targetH) {
    if (!g_off) return false;
    if (targetW < kMinWidth || targetH == 0) return false;
    const float aspect = static_cast<float>(targetW) / static_cast<float>(targetH);
    if (aspect < kAspect - kAspectTol || aspect > kAspect + kAspectTol) {
        return false;
    }
    // The panel is here. Remember that separately from whether the flash
    // draw was, so a log can say which half failed.
    ++g_surfaceSeen;
    g_panelW = targetW;
    g_panelH = targetH;

    if (kind != kKind || count != g_indices) return false;
    ++g_dropped;
    return true;
}

void wakePulseReport() {
    if (!g_off) return;

    // The diagnosis a field report needs, said once. "Still flashing" has
    // two causes and they want different answers: the panel was never
    // recognised, or it was and the flash draw is not the size named. This
    // says which, with the panel's real size to re-derive from.
    if (!g_saidNoMatch && g_dropped == 0 && g_surfaceSeen > 20000) {
        g_saidNoMatch = true;
        Log::get().note(
            "wake pulse: the panel surface IS being found (%ux%u, seen %llu "
            "times) but no %c draw of %u indices has landed in it, so nothing "
            "is being held off. The index count is content-dependent and has "
            "moved: take a census with census_offscreen on while the marker "
            "flashes, find the draw into that surface that appears only on "
            "the flashing frames, and set advanced.wake_pulse_indices to its "
            "n=. Said once.",
            g_panelW, g_panelH, static_cast<unsigned long long>(g_surfaceSeen),
            kKind, g_indices);
        return;
    }

    // Only when it moved, and only a handful of times: a suppression that
    // says nothing is indistinguishable from one that never matched, and a
    // line per pulse would be the log.
    if (g_dropped == g_lastReported || g_reports >= 6) return;
    ++g_reports;
    g_lastReported = g_dropped;
    Log::get().note("wake pulse: %llu flash draw(s) dropped so far. The "
                    "marker under the speed readout is held off; nothing "
                    "else on that panel is touched. Said at most 6 times.",
                    static_cast<unsigned long long>(g_dropped));
}

}  // namespace edvr

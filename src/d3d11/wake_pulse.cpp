#include "wake_pulse.h"

#include <windows.h>

#include <string>

#include "../common/config.h"
#include "../common/frame_flag.h"   // eyeTextureSize
#include "../common/log.h"

namespace edvr {
namespace {

// The panel surface, as a FRACTION of the render resolution rather than a
// literal size. 2440x1996 on a 5424x5356 eye and 1952x1597 on a 4340x4284
// one both give these to four figures. A literal size is valid only for the
// session it was measured in; a ratio survives a headset change.
constexpr float kFracW = 0.4498f;
constexpr float kFracH = 0.3726f;
constexpr float kFracTol = 0.004f;

constexpr char     kKind = 'X';
constexpr uint32_t kIndices = 1728;   // 288 quads, on game build 332753

bool     g_off = false;
uint32_t g_indices = kIndices;
uint64_t g_dropped = 0;
uint64_t g_lastReported = 0;   // the count the last line printed
uint32_t g_reports = 0;        // how many lines have been printed at all

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
    if (kind != kKind || count != g_indices) return false;
    uint32_t ew = 0, eh = 0;
    if (!eyeTextureSize(&ew, &eh) || !ew || !eh) return false;
    const float fw = static_cast<float>(targetW) / static_cast<float>(ew);
    const float fh = static_cast<float>(targetH) / static_cast<float>(eh);
    if (fw < kFracW - kFracTol || fw > kFracW + kFracTol ||
        fh < kFracH - kFracTol || fh > kFracH + kFracTol) {
        return false;
    }
    ++g_dropped;
    return true;
}

void wakePulseReport() {
    // Only when it moved, and only a handful of times: a suppression that
    // says nothing is indistinguishable from one that never matched, and a
    // line per pulse would be the log.
    if (!g_off || g_dropped == g_lastReported || g_reports >= 6) return;
    ++g_reports;
    g_lastReported = g_dropped;
    Log::get().note("wake pulse: %llu flash draw(s) dropped so far. The "
                    "marker under the speed readout is held off; nothing "
                    "else on that panel is touched. Said at most 6 times.",
                    static_cast<unsigned long long>(g_dropped));
}

}  // namespace edvr

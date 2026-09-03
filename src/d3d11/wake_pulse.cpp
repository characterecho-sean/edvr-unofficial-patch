#include "wake_pulse.h"

#include <windows.h>

#include <cstdio>    // _snprintf_s: the candidate list
#include <cstdlib>   // strtoul: the index-count list
#include <string>

#include "../common/config.h"
#include "binding_shadow.h"
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

// 288 quads, measured where the panel comes out 2440x1996. The count is
// TESSELLATION, not content: the GUI subdivides its vector curves by pixel
// size, so the same widget on a smaller panel needs fewer segments. Measured
// on one machine, two headsets, same build and same ship -- panel 2440x1996
// takes 1728 indices and panel 1002x820 does not, which is why this shipped
// working on one headset and dead on the other.
//
// So the default is only ever right for one panel size, and the useful thing
// this module does about it is NAME THE CANDIDATES in the log rather than
// leave the owner to census it. See the intermittency table below.
constexpr uint32_t kIndices = 1728;    // panel 2440x1996
constexpr uint32_t kIndicesAlt = 891;  // panel 1002x820, same shader

// A LIST, not a number, and that is the honest shape of it. Two measured
// points -- 576 triangles on a 2440-wide panel, 297 on a 1002-wide one --
// fit neither a linear law nor a square-root one, so the count cannot be
// computed from the panel size with any confidence from two samples. What
// it can be is a set of known values, each acted on only when it lands in
// THIS panel with this shape, so a wrong entry costs nothing anywhere else.
constexpr uint32_t kMaxIndices = 8;

// SELF-CALIBRATION, because a list only ever covers the panels somebody has
// already measured. The marker is identified by three things that do not
// change with resolution, build or ship:
//
//   it is drawn by the GUI's TEXTURELESS vector shader (no sampler bound),
//   it is a SHAPE and not a single quad (hundreds of indices, never six),
//   and it is there on some frames and not others -- which is what a pulse
//   IS, and the reason none of the sizes in this file were ever the
//   invariant.
//
// So after enough frames of watching one panel, the count that behaves that
// way MOST is the marker, whatever number it happens to be on this rig --
// this HUD blinks a lot of small things occasionally, and the marker pulses,
// so it leads them clearly or the evidence is not good enough to act on.
// Adopted out loud, and any ini list wins over it.
constexpr uint32_t kMinShapeIndices = 60;    // a quad is 6; the marker is not
constexpr uint32_t kCalibFrames = 600;       // panel frames before deciding
constexpr uint32_t kCalibLoPct = 5;          // below this it is incidental
constexpr uint32_t kCalibHiPct = 95;         // at 100 it is furniture, not a pulse
constexpr uint32_t kCalibLead = 2;           // and it must lead the next by this
bool     g_calibrated = false;

bool     g_off = false;
uint32_t g_indices[kMaxIndices] = {kIndices, kIndicesAlt};
uint32_t g_indexCount = 2;
uint64_t g_dropped = 0;
uint64_t g_lastReported = 0;   // the count the last line printed
uint32_t g_reports = 0;        // how many lines have been printed at all

// Told apart so a field report can say WHICH half failed: the panel was
// never found, or it was found and no draw of the named size landed in it.
uint64_t g_surfaceSeen = 0;
uint32_t g_panelW = 0, g_panelH = 0;
bool     g_saidNoMatch = false;

// The intermittency table: which index counts land in the panel, and in how
// many DISTINCT frames each.
//
// The flash is the one draw that is there on some frames and not others --
// that is what a pulse is, and it is the property that does not change with
// resolution, build or ship. Everything else on the panel is drawn every
// frame. So a count seen intermittently is a candidate, the leader among
// them is used, and the log names them all either way rather than asking
// the owner for a census.
constexpr uint32_t kSeenSlots = 24;
struct Seen {
    uint32_t indices = 0;
    uint32_t frames = 0;      // distinct frames it appeared in
    uint32_t lastFrame = 0;   // so one frame counts once
};
Seen     g_seen[kSeenSlots];
uint32_t g_seenCount = 0;
uint32_t g_frame = 0;         // every frame, so one frame counts a count once
uint32_t g_panelFrames = 0;   // frames the panel was drawn in -- the denominator
uint32_t g_panelLastFrame = 0;

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
    // Comma-separated, and empty means the measured set. A list because the
    // count is tessellation and every panel size has its own.
    const std::string spec = cfg.getString("advanced.wake_pulse_indices", "");
    uint32_t parsed[kMaxIndices];
    uint32_t n = 0;
    bool ok = true;
    for (const char* p = spec.c_str(); *p && ok;) {
        while (*p == ' ' || *p == ',' || *p == '	') ++p;
        if (!*p) break;
        char* end = nullptr;
        const unsigned long v = strtoul(p, &end, 10);
        if (end == p || v == 0 || v > 1000000 || n == kMaxIndices) { ok = false; break; }
        parsed[n++] = static_cast<uint32_t>(v);
        p = end;
    }
    if (!spec.empty() && (!ok || n == 0)) {
        Log::get().note("wake_pulse_indices \"%s\" is not up to %u whole "
                        "numbers separated by commas; the measured set is "
                        "used instead.", spec.c_str(), kMaxIndices);
    } else if (n) {
        for (uint32_t i = 0; i < n; ++i) g_indices[i] = parsed[i];
        g_indexCount = n;
    } else {
        g_indices[0] = kIndices;
        g_indices[1] = kIndicesAlt;
        g_indexCount = 2;
    }

    if (was != g_off) {
        Log::get().note(
            "wake pulse: %s. With a high wake selected, a marker under the "
            "speed readout flashes in time with the target indicator -- both "
            "are drawn into the same holo panel. off drops the one draw that "
            "paints it (%u known index counts of the GUI's textureless vector "
            "shader) "
            "on the frames the flash is on, and leaves the rest of the panel "
            "alone. That count is tessellation and depends on the panel's "
            "pixel size, so if the count below stays at zero this build "
            "will name the candidates it saw and one of them wants "
            "setting in advanced.wake_pulse_indices.",
            g_off ? "off" : "stock", g_indexCount);
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
    // Frames the panel was drawn in, not frames overall: menus and loading
    // draw no panel, and counting them would dilute a real pulse below the
    // threshold that is meant to find it.
    if (g_panelLastFrame != g_frame) {
        g_panelLastFrame = g_frame;
        ++g_panelFrames;
    }

    if (kind == kKind) {
        for (uint32_t i = 0; i < g_indexCount; ++i) {
            if (count != g_indices[i]) continue;
            ++g_dropped;
            return true;
        }
    }

    // Not ours. Record it while we still have nothing, so that if the count
    // is wrong for this panel the log can say what the alternatives were.
    // Stops costing anything the moment a real match happens or the table
    // fills.
    // Only textureless SHAPES are candidates: the vector shader binds no
    // sampler at all, which is what separates the marker from the textured
    // widgets sharing this panel, and six indices is a quad rather than a
    // drawn shape.
    const bool vectorShape =
        kind == kKind && count >= kMinShapeIndices &&
        !bindingGet(BindSlot::PsSrv0) && !bindingGet(BindSlot::PsSrv1) &&
        !bindingGet(BindSlot::PsSrv2) && !bindingGet(BindSlot::PsSrv3);
    if (vectorShape && g_dropped == 0 && !g_saidNoMatch) {
        for (uint32_t i = 0; i < g_seenCount; ++i) {
            if (g_seen[i].indices != count) continue;
            if (g_seen[i].lastFrame != g_frame) {
                g_seen[i].lastFrame = g_frame;
                ++g_seen[i].frames;
            }
            return false;
        }
        if (g_seenCount < kSeenSlots) {
            g_seen[g_seenCount].indices = count;
            g_seen[g_seenCount].frames = 1;
            g_seen[g_seenCount].lastFrame = g_frame;
            ++g_seenCount;
        }
    }
    return false;
}

void wakePulseReport() {
    if (!g_off) return;
    ++g_frame;

    // Self-calibration, before the giving-up line. If exactly ONE textureless
    // shape on this panel is behaving like a pulse, that is the marker on
    // this rig whatever its index count happens to be -- and this is the
    // only part of the fix that does not depend on somebody having measured
    // this panel size before.
    //
    // Exactly one, deliberately. Two candidates means the evidence does not
    // say which, and picking either would be suppressing a draw on a guess.
    if (!g_calibrated && !g_saidNoMatch && g_dropped == 0 &&
        g_panelFrames >= kCalibFrames) {
        // Rank them, do not count them. The first build of this asked for
        // EXACTLY ONE candidate and a Q3 produced eight: 891 at 56 per cent
        // of frames, then 60 at 21, 120 at 18, and five more below 8. Seven
        // were in band and the right one was above the ceiling, so it
        // refused -- correctly by its own rule, and uselessly.
        //
        // The shape of that data is the answer. This HUD blinks a lot of
        // small things occasionally; the marker pulses, so it is drawn far
        // more often than any of them. Take the leader, and only when it
        // leads clearly: 56 against 21 is a different claim from 8 against 7.
        uint32_t best = 0, bestPct = 0, runnerUp = 0;
        for (uint32_t i = 0; i < g_seenCount; ++i) {
            const uint32_t pct = g_seen[i].frames * 100 / g_panelFrames;
            if (pct < kCalibLoPct || pct > kCalibHiPct) continue;
            if (pct > bestPct) {
                runnerUp = bestPct;
                bestPct = pct;
                best = g_seen[i].indices;
            } else if (pct > runnerUp) {
                runnerUp = pct;
            }
        }
        if (best && bestPct >= runnerUp * kCalibLead) {
            g_calibrated = true;
            g_indices[0] = best;
            g_indexCount = 1;
            Log::get().note(
                "wake pulse: none of the known index counts landed in this "
                "%ux%u panel, so it was worked out from behaviour instead. Of "
                "the textureless shapes drawn on it, %u indices appears in "
                "%u%% of frames and the next most frequent in %u%% -- a pulse "
                "leading incidental blinking. Using it. Set "
                "advanced.wake_pulse_indices if this is the wrong draw, and "
                "it is worth reporting either way so the shipped set can "
                "grow.",
                g_panelW, g_panelH, best, bestPct, runnerUp);
            return;
        }
    }

    // The diagnosis a field report needs, said once. "Still flashing" has
    // two causes and they want different answers: the panel was never
    // recognised, or it was and the flash draw is not the size named. This
    // says which, with the panel's real size to re-derive from.
    if (!g_saidNoMatch && g_dropped == 0 && g_surfaceSeen > 20000) {
        g_saidNoMatch = true;
        // The candidates, named. A pulse is a draw present in a MINORITY of
        // frames; everything else on this panel is drawn in all of them. Two
        // per cent to sixty is wide enough for a slow blink and a fast one
        // and still excludes the constant furniture.
        char list[220] = "";
        int at = 0, found = 0;
        for (uint32_t i = 0; i < g_seenCount && at < 180; ++i) {
            const uint32_t pct =
                g_panelFrames ? g_seen[i].frames * 100 / g_panelFrames : 0;
            if (pct < 2 || pct > 60) continue;
            const int n = _snprintf_s(list + at, sizeof(list) - at, _TRUNCATE,
                                      "%s%u (in %u%% of frames)",
                                      found ? ", " : "", g_seen[i].indices, pct);
            if (n <= 0) break;
            at += n;
            ++found;
        }
        Log::get().note(
            "wake pulse: the panel surface IS being found (%ux%u, seen %llu "
            "times) but no %c draw of any known index count has landed in it, "
            "so nothing "
            "is being held off. The count is TESSELLATION -- the GUI "
            "subdivides its curves by pixel size, so a smaller panel needs "
            "fewer -- and %u is right for a 2440x1996 panel. %s%s Set "
            "advanced.wake_pulse_indices to the one that stops the flashing. "
            "Said once.",
            g_panelW, g_panelH, static_cast<unsigned long long>(g_surfaceSeen),
            kKind, kIndices,
            found ? "Drawn in only some frames here, so a pulse: " : "",
            found ? list
                  : "Nothing on this panel was drawn intermittently while "
                    "this watched, so the marker was probably never up -- "
                    "select a high wake and look again.");
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

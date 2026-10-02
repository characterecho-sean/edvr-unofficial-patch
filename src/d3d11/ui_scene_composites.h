// The interface composites the layer left in the scene -- pure and header-only: no device, no
// Config, no Log. docs/ui-layer-2026-09-23.md, "2026-10-01: Disable GUI effects". The DLL
// (ui_layer.cpp counts, vscreen.cpp reports each draw) and the build-gate rig
// (tools/ui_composite_census_test) include this one file.
//
// WHY. The layer names an interface draw by its vertex shader. A draw into the lit HDR eye that
// samples an interface surface and has a vertex shader no family names gets no decision and no
// refusal line: it stays in the scene, is upscaled with it, and the log says nothing. User 5's
// cockpit panels were that for a month -- Elite's Disable GUI effects switches the holo panels to
// another shader pair -- and the only trace was ui depth's totals line counting 22 draws a frame
// "left alone" under no name. This is the line that names them, every 30 s, in the layer's own
// census.
//
// WHAT. Every owner draw into an eye target that samples a learned interface surface (ui_depth's
// test, the one that makes a draw a composite) is counted once: taken into the layer, or left in
// the game's frame -- and a left one is named by its vertex shader, its pixel shader and the family
// the rule gave it (none when it named nothing) -- or kept (below). Every window the layer prints
// ONE line, with zeros: a line that says "0 of 5108" is the count having run and found nothing; no
// line at all is the code never having run (the layer off, or a build before this one).
//
// A composite no family names is the case this exists for; a named one that was not taken (the
// layer not armed for the frame, the world-screen gate holding the 2D screen) is also left in the
// scene and shows here with its family, beside the reasons the decided table already gives.
//
// KEPT. ui depth calls a draw a composite when a learned interface surface is bound at a pixel
// slot, whether or not the pixel shader reads it. One such draw is not interface at all: the
// engine's NULL-OUTPUT QUAD (vs B018D143700AB803 / ps 258B95AC99520C1F), a six-index quad drawn
// once an eye after the loading screen's composites, and in a cockpit between an eye's two holo
// quads (vs A888D51024D9798E / ps 015EF9349EC097E8), with the surfaces of the draw before it still
// bound. Its pixel shader is `mov o0, 0` and reads nothing, its blend is straight alpha (src
// alpha, inverse src alpha) and its depth and stencil are off in every census of it (Frontier
// logs 2026-09-06 and 2026-09-16): it changes no pixel, so there is nothing to take, and ui depth
// keeps it on its exclusion list by vertex shader (kNullPsMesh, since 2026-09-07), which the layer
// asks too. The first census (13c62cd6) counted it as a composite left in the scene with no
// family, and the reader called that a STOP on every loading screen -- with Disable GUI effects on
// or off. It is now named here, by its exact pair, as KEPT: counted in `seen`, not in `left`, and
// said in its own clause. The same vertex shader with any other pixel shader is another draw and
// still counts as left.
//
// LIMITS. A draw is a composite where ui depth's classifier says so, and that loop runs for an eye
// draw with a depth target bound (every cockpit composite and every recorded menu composite binds
// one): a composite drawn into an eye with none is not seen. A draw the layer took and then refused
// at issue still counts as taken here; the gates line counts those on its own.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "ui_layer_math.h"

namespace edvr {

// Pairs a window names. A frame of cockpit has at most a handful of composite pairs; more than this
// many DIFFERENT pairs left in the scene is itself the finding, and the rest are counted, unnamed.
constexpr size_t kUiSceneCompositePairs = 8;

struct UiSceneCompositePair {
    uint64_t vs = 0, ps = 0, draws = 0;
    int family = 0;  // UiLayerFamily as an int; 0 = no family named it
};

// The pairs the scene keeps on purpose (KEPT, above), each named by its exact vertex and pixel shader. Only a pair that no family names is
// ever kept, and only a pair whose vertex shader is on ui depth's exclusion list (tools\ui_composite_census_test pins that against
// ui_depth.cpp): the census must never call a draw the layer could take "kept".
struct UiSceneKeptPair {
    uint64_t vs, ps;
    const char* name;
};
inline constexpr UiSceneKeptPair kUiSceneKeptPairs[] = {
    {0xB018D143700AB803ull, 0x258B95AC99520C1Full, "null-output quad"},
};
constexpr size_t kUiSceneKeptCount = sizeof(kUiSceneKeptPairs) / sizeof(kUiSceneKeptPairs[0]);

// The index of the kept pair this exact (vs, ps) is, or -1.
inline int uiSceneKeptIndex(uint64_t vs, uint64_t ps) {
    for (size_t i = 0; i < kUiSceneKeptCount; ++i)
        if (kUiSceneKeptPairs[i].vs == vs && kUiSceneKeptPairs[i].ps == ps) return static_cast<int>(i);
    return -1;
}

struct UiSceneCompositeWindow {
    uint64_t seen = 0;        // composite draws into an eye target: taken, left, or kept
    uint64_t left = 0;        // ...the ones left in the game's frame, a defect or a reason the layer gives (not the kept)
    uint64_t kept = 0;        // ...the ones that are not interface and stay in the scene on purpose (kUiSceneKeptPairs)
    uint64_t pastTable = 0;   // of left: draws of pairs the table had no room for
    uint64_t framesLive = 0;  // frames of the window the layer was live, so the count could run
    uint32_t used = 0;
    UiSceneCompositePair pairs[kUiSceneCompositePairs];
    uint64_t keptDraws[kUiSceneKeptCount] = {};

    // A composite the layer took (or whose take is the layer's business): counted, not named.
    void noteTaken() { ++seen; }

    // A composite left in the game's frame, by (vs, ps, family). A pair no family names that the scene keeps on purpose is KEPT
    // instead: seen, not left.
    void noteLeft(uint64_t vs, uint64_t ps, int family) {
        ++seen;
        if (family == 0) {
            const int k = uiSceneKeptIndex(vs, ps);
            if (k >= 0) {
                ++kept;
                ++keptDraws[k];
                return;
            }
        }
        ++left;
        for (uint32_t i = 0; i < used; ++i) {
            UiSceneCompositePair& p = pairs[i];
            if (p.vs == vs && p.ps == ps && p.family == family) {
                ++p.draws;
                return;
            }
        }
        if (used < kUiSceneCompositePairs) {
            UiSceneCompositePair& p = pairs[used++];
            p.vs = vs;
            p.ps = ps;
            p.family = family;
            p.draws = 1;
            return;
        }
        ++pastTable;
    }

    void reset() { *this = UiSceneCompositeWindow{}; }
};

// The line, whole, with its "ui quality: " prefix (the Log adds the time). frames: every frame the
// boundary counted in the window. detectorOn: the interface depth pass was running at the print --
// without it nothing is learned, so no draw is a composite and the zero would mean nothing.
inline std::string uiSceneCompositeText(const UiSceneCompositeWindow& w, uint64_t frames, bool detectorOn) {
    char buf[320];
    const double perFrame = frames ? 1.0 / static_cast<double>(frames) : 0.0;
    std::string out = "ui quality: composites left in the scene: ";
    if (!detectorOn) {
        std::snprintf(buf, sizeof(buf),
                      "NOT COUNTED (the interface depth pass is not running: fix.temporal_aa is off or it stood "
                      "down, so no draw is recognised as a composite) in %llu frames.",
                      static_cast<unsigned long long>(frames));
        return out + buf;
    }
    std::snprintf(buf, sizeof(buf), "%llu of %llu composite draws (%.2f a frame) in %llu frames (%llu live) -- ",
                  static_cast<unsigned long long>(w.left), static_cast<unsigned long long>(w.seen),
                  static_cast<double>(w.left) * perFrame, static_cast<unsigned long long>(frames),
                  static_cast<unsigned long long>(w.framesLive));
    out += buf;
    if (!w.seen) {
        out += "no draw into an eye sampled an interface surface in this window.";
        return out;
    }
    // The clauses, joined by "; ": the verdict when nothing is left, the pairs left, the overflow, and last the pairs the scene keeps on
    // purpose (KEPT). A window with nothing kept makes exactly the line it made before kept existed.
    bool first = true;
    const auto join = [&] {
        if (!first) out += "; ";
        first = false;
    };
    if (!w.left) {
        join();
        out += "none: every interface composite drawn into an eye went into the layer";
    }
    for (uint32_t i = 0; i < w.used; ++i) {
        const UiSceneCompositePair& p = w.pairs[i];
        char label[64];
        if (p.family == 0) {
            std::snprintf(label, sizeof(label), "no family");
        } else {
            std::snprintf(label, sizeof(label), "%s, not taken", uiLayerFamilyName(static_cast<UiLayerFamily>(p.family)));
        }
        join();
        std::snprintf(buf, sizeof(buf), "vs %016llX ps %016llX (%s) %.2f a frame",
                      static_cast<unsigned long long>(p.vs), static_cast<unsigned long long>(p.ps), label,
                      static_cast<double>(p.draws) * perFrame);
        out += buf;
    }
    if (w.pastTable) {
        join();
        std::snprintf(buf, sizeof(buf), "%llu draws of pairs past the table's %u (%.2f a frame)",
                      static_cast<unsigned long long>(w.pastTable), static_cast<unsigned>(kUiSceneCompositePairs),
                      static_cast<double>(w.pastTable) * perFrame);
        out += buf;
    }
    for (size_t i = 0; i < kUiSceneKeptCount; ++i) {
        if (!w.keptDraws[i]) continue;
        join();
        std::snprintf(buf, sizeof(buf), "vs %016llX ps %016llX (%s, kept in the scene by design) %llu draws, %.2f a frame",
                      static_cast<unsigned long long>(kUiSceneKeptPairs[i].vs), static_cast<unsigned long long>(kUiSceneKeptPairs[i].ps),
                      kUiSceneKeptPairs[i].name, static_cast<unsigned long long>(w.keptDraws[i]),
                      static_cast<double>(w.keptDraws[i]) * perFrame);
        out += buf;
    }
    out += ".";
    return out;
}

// The same into a caller's buffer, truncated (never overrun) and always terminated; the length
// written. The Log's line holds 1200 characters and the layer's buffer 1100; the longest line this makes (a full
// table, the overflow and the kept pair) is under 1100 (the rig's R5 holds it).
inline size_t uiSceneCompositeFormat(char* out, size_t cap, const UiSceneCompositeWindow& w, uint64_t frames,
                                     bool detectorOn) {
    if (!out || !cap) return 0;
    const std::string text = uiSceneCompositeText(w, frames, detectorOn);
    const size_t n = text.size() < cap - 1 ? text.size() : cap - 1;
    std::memcpy(out, text.data(), n);
    out[n] = 0;
    return n;
}

}  // namespace edvr

// The flat scene picker's candidate log (docs/sky-flicker-2026-10-13.md; reviews/skybox-flicker-2026-10-13.md, "Flat"): a diagnostic for the F8
// render-size flip between "Elite renders 1440x810" and "256x256" that edvr_gfx_20261001_103559 showed five times in 10 s, and which the review inferred
// (not proved) is the reflection probe's 256x256 deferred view out-drawing the scene on some frames.
//
// flatSceneFacts (flat_copy_structure.h) names the scene: the R11G11B10F target with a depth of its own size that took the most draws, at least eight
// of them, preferring on-shape targets. This lists EVERY target that passes its tests -- size, format, draws, on-shape or not, and whether the VR eye-target
// rule's flat counterpart would exclude it as a tiny square -- once per 5 s and whenever the winner changes, with the number of frames each size has won in
// the window. The flat counterpart is NOT applied (it has not been flown; the VR rule is proven, the flat inference is not): the line says what it WOULD drop
// (`would_reject`), whether that would change the winner, and how many frames in the window a would-be-rejected size won, so a later flat flight can decide.
// It reads the prefix model and changes nothing.
//
// Pure: no Windows, no globals. The rig is tools\sky_flicker_test.
#pragma once
#include "flat_copy_structure.h"
#include <cstdio>
#include <cstring>
#include <utility>

namespace edvr {

struct FlatSceneCandidate {
    uint32_t width = 0, height = 0, format = 0, draws = 0;
    bool onShape = false;
    bool tinySquare = false;      // the rule excludes it (counted whether or not the key is on)
};
constexpr uint32_t kFlatSceneMaxCandidates = 8;

// Every target flatSceneFacts would consider (the same tests, in the prefix's order), up to kFlatSceneMaxCandidates, biggest draw count first.
inline uint32_t flatSceneCandidates(const FlatRuntimePrefix& p, uint32_t outW, uint32_t outH, FlatSceneCandidate out[kFlatSceneMaxCandidates]) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < p.targetsUsed; ++i) {
        const auto& t = p.targets[i];
        const auto& k = t.writes.key;
        if (t.writes.draws < kFlatSceneMinDraws || k.format != 26 || !k.color || !k.rtv || !k.depth || !k.dsv ||
            !k.width || !k.height || k.depthWidth != k.width || k.depthHeight != k.height)
            continue;
        if (n >= kFlatSceneMaxCandidates) break;
        FlatSceneCandidate& c = out[n++];
        c.width = k.width; c.height = k.height; c.format = k.format; c.draws = t.writes.draws;
        c.onShape = flatRenderFitsOutput(k.width, k.height, outW, outH);
        c.tinySquare = flatTinySquare(k.width, k.height, outW, outH);
    }
    for (uint32_t a = 1; a < n; ++a)
        for (uint32_t b = a; b > 0 && out[b].draws > out[b - 1].draws; --b) std::swap(out[b], out[b - 1]);
    return n;
}

// The tracker: frames won by each size in the window, the last winner, and when a line is due.
struct FlatSceneLog {
    static constexpr uint32_t kSizes = 6;
    struct Won { uint32_t w = 0, h = 0, frames = 0; };
    Won won[kSizes];
    uint32_t otherWon = 0;
    uint32_t noneFrames = 0;
    uint32_t tinyWon = 0;              // frames in the window won by a size the rule would drop
    uint32_t frames = 0;
    uint32_t lastW = 0, lastH = 0;     // the last winner; 0x0 is "no scene"
    bool haveLast = false;
    uint64_t lastLogMs = 0;
    uint32_t changeLines = 0;          // winner-change lines in this 30 s: capped so a flip every frame cannot fill the log
    uint64_t changeWindowMs = 0;
    static constexpr uint32_t kChangeLineCap = 12;
    static constexpr uint64_t kPeriodMs = 5000;

    // One final copy's winner. Returns true when a line is due: the first, the winner changed (within the cap), or 5 s have passed. nowMs from any clock.
    bool note(uint64_t nowMs, uint32_t winW, uint32_t winH, bool wonByWouldReject = false) {
        ++frames;
        if (wonByWouldReject) ++tinyWon;
        if (!winW) {
            ++noneFrames;
        } else {
            bool placed = false;
            for (Won& s : won) {
                if (s.frames && s.w == winW && s.h == winH) { ++s.frames; placed = true; break; }
            }
            for (Won& s : won) {
                if (placed) break;
                if (!s.frames) { s.w = winW; s.h = winH; s.frames = 1; placed = true; }
            }
            if (!placed) ++otherWon;
        }
        if (nowMs - changeWindowMs >= 30000) { changeWindowMs = nowMs; changeLines = 0; }
        const bool first = !haveLast;
        const bool changed = haveLast && (winW != lastW || winH != lastH);
        haveLast = true;
        lastW = winW; lastH = winH;
        if (first) { lastLogMs = nowMs; return true; }
        if (changed && changeLines < kChangeLineCap) { ++changeLines; lastLogMs = nowMs; return true; }
        if (nowMs - lastLogMs >= kPeriodMs) { lastLogMs = nowMs; return true; }
        return false;
    }
    // The window's counts start over after a line that printed them.
    void resetWindow() {
        for (Won& s : won) s = Won{};
        otherWon = noneFrames = tinyWon = frames = 0;
    }
};

// The winner the flat counterpart of the eye-target rule WOULD leave: the candidates that are not tiny squares, on-shape first, then by draws. width 0 is none.
struct FlatSceneWould {
    uint32_t rejected = 0;     // candidates it would drop
    uint32_t width = 0, height = 0, draws = 0;
    bool onShape = false;
};
inline FlatSceneWould flatSceneWould(const FlatSceneCandidate* cands, uint32_t candN) {
    FlatSceneWould w;
    bool haveOn = false;
    for (uint32_t i = 0; i < candN; ++i) {
        if (cands[i].tinySquare) { ++w.rejected; continue; }
        const bool better = (w.width == 0) || (cands[i].onShape && !haveOn) || (cands[i].onShape == haveOn && cands[i].draws > w.draws);
        if (better) { w.width = cands[i].width; w.height = cands[i].height; w.draws = cands[i].draws; haveOn = cands[i].onShape; }
    }
    w.onShape = haveOn;
    return w;
}

// "flat scene candidates (frame N, output WxH, why): winner 1440x810 off-shape draws=120; candidates: 256x256 fmt=26 draws=285 off-shape tiny-square, 1440x810
//  fmt=26 draws=120 off-shape; would_reject=1 (the rule would leave 1440x810, the winner changes); frames won by a would-be-rejected size: 12;
//  frames won in this window (302 frames): 1440x810:290 256x256:12 none:0". The rule is not applied; the line says what it would do. Returns the length
// written, or 0 when the buffer is too small.
inline int flatSceneFormatLine(char* buf, size_t n, uint64_t frame, uint32_t outW, uint32_t outH, const FlatSceneFacts& winner,
                               const FlatSceneCandidate* cands, uint32_t candN, const FlatSceneLog& log, const char* why) {
    int len = std::snprintf(buf, n, "flat scene candidates (frame %llu, output %ux%u, %s): winner ",
                            static_cast<unsigned long long>(frame), outW, outH, why);
    if (len <= 0 || static_cast<size_t>(len) >= n) return 0;
    auto add = [&](const char* fmt, auto... args) {
        if (len <= 0 || static_cast<size_t>(len) >= n) return;
        const int w = std::snprintf(buf + len, n - static_cast<size_t>(len), fmt, args...);
        if (w < 0 || static_cast<size_t>(len) + static_cast<size_t>(w) >= n) len = 0; else len += w;
    };
    const bool haveWinner = winner.onShape || winner.offShape;
    if (haveWinner) add("%ux%u %s draws=%u", winner.width, winner.height, winner.onShape ? "on-shape" : "off-shape", winner.draws);
    else add("none");
    add("; candidates:");
    if (!candN) add(" none");
    for (uint32_t i = 0; i < candN; ++i)
        add("%s %ux%u fmt=%u draws=%u %s%s", i ? "," : "", cands[i].width, cands[i].height, cands[i].format, cands[i].draws,
            cands[i].onShape ? "on-shape" : "off-shape", cands[i].tinySquare ? " tiny-square" : "");
    const FlatSceneWould would = flatSceneWould(cands, candN);
    add("; would_reject=%u", would.rejected);
    if (would.rejected) {
        const bool same = haveWinner ? (would.width == winner.width && would.height == winner.height) : (would.width == 0);
        if (same) add(" (not applied; the winner would not change)");
        else if (would.width) add(" (not applied; the rule would leave %ux%u, the winner changes)", would.width, would.height);
        else add(" (not applied; the rule would leave no scene, the winner changes)");
    } else {
        add(" (none of the candidates is a tiny square)");
    }
    add("; frames won by a would-be-rejected size: %u", log.tinyWon);
    add("; frames won in this window (%u frames):", log.frames);
    for (const auto& s : log.won) if (s.frames) add(" %ux%u:%u", s.w, s.h, s.frames);
    if (log.otherWon) add(" other:%u", log.otherWon);
    add(" none:%u", log.noneFrames);
    return len;
}
}  // namespace edvr

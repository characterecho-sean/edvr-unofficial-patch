// C1b: the sight census's withhold cycle (docs\design-vr-scope-magnification-2026-10-28.md, section 12).
//
// The question is which draw of the sight's effects paints which part of the picture: the scope's glass tint, ticks and honeycomb edge, the reticle, the other additive and alpha draws in the
// same pass. A census names the draws (shader pair, indices, instances, blend) but not what they paint. The answer is by subtraction: during a MANUAL eye dump taken in ADS, each of the
// dump's frames leaves one candidate draw out of the scene, cycling through the list below, and the dump's json records which one and how many draws it left out. The dump's raw crops
// (C00..C15, the picture the game hands the upscaler) then differ from each other exactly where a candidate paints, and tools\sight_candidates.py reports the footprint.
//
// The cycle runs only between the key press that arms a manual dump (temporal_pass.cpp beginEyeRun, never the airborne companion's) and the dump's write, only while the sights are up, and
// ends itself after kMaxFrames frames whatever happens. The frame the key was pressed in, and any before it, are never touched: they are the run's un-withheld baseline.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace edvr {
namespace sightwithhold {

struct Candidate {
    const char* name;      // in the dump's json and in tools\sight_candidates.py's report
    uint64_t vs, ps;       // the pair
    uint32_t indices;      // the draw's index (or vertex) count; 0: any
    uint32_t instances;    // ... and instance count; 0: any
};
// VS 025B4B9FF54622ED is the GUI material's vertex shader: with PS C5A5C7E8216CB9AF it is the scope glass (six draws an eye on the Executioner, alpha over, tex1024x1024f70,f83); with 46F92DC71BF8DFA5
// it is the gunsight quad (the control: it is certain to paint). The others are the unidentified draws of the same pass (the census of edvr_gfx_20261028_173510.log and edvr_gfx_20261029_120218.log).
// A C5A5 shape is (indices, instances); two draws of one shape (n9 i1 twice on the Executioner) are one candidate, because only a shape is told apart.
constexpr uint64_t kVsMaterial = 0x025B4B9FF54622EDull;
constexpr Candidate kCandidates[] = {
    {"C5A5_n18_i1", kVsMaterial, 0xC5A5C7E8216CB9AFull, 18, 1},
    {"C5A5_n6_i5", kVsMaterial, 0xC5A5C7E8216CB9AFull, 6, 5},
    {"C5A5_n6_i2", kVsMaterial, 0xC5A5C7E8216CB9AFull, 6, 2},
    {"C5A5_n9_i1", kVsMaterial, 0xC5A5C7E8216CB9AFull, 9, 1},
    {"C5A5_n12_i2", kVsMaterial, 0xC5A5C7E8216CB9AFull, 12, 2},
    {"C5A5_n6_i1", kVsMaterial, 0xC5A5C7E8216CB9AFull, 6, 1},   // the Intimidator's one C5A5 draw
    {"PS_0925AA1B", kVsMaterial, 0x0925AA1BD7A95BBAull, 0, 0},
    {"PS_66B08F89", 0x98397963AAEC45D3ull, 0x66B08F89E4926C01ull, 0, 0},
    {"PS_AFED1D4B", 0xF512712C40D93C12ull, 0xAFED1D4B087E18A9ull, 0, 0},
    {"PS_912477AE", 0x94D5C556DFD6D705ull, 0x912477AEF6958379ull, 0, 0},
    {"QUAD_46F9", kVsMaterial, 0x46F92DC71BF8DFA5ull, 6, 1},   // the control: the gunsight quad
};
constexpr uint32_t kCount = sizeof(kCandidates) / sizeof(kCandidates[0]);
constexpr uint32_t kRun = 16;                // the eye dump's frames
constexpr uint32_t kMaxFrames = 600;         // the cycle ends this many game frames after the key press, written dump or not

inline const char* name(int index) noexcept { return index >= 0 && static_cast<uint32_t>(index) < kCount ? kCandidates[index].name : "none"; }

// A vertex shader that is any candidate's: asked first, so an ordinary draw reads no pixel shader hash.
inline bool vsIsCandidate(uint64_t vs) noexcept {
    for (const Candidate& c : kCandidates)
        if (c.vs == vs) return true;
    return false;
}
// The candidate a draw is, or -1. indices and instances are the draw's counts as the census prints them (n=, i=).
inline int match(uint64_t vs, uint64_t ps, uint32_t indices, uint32_t instances) noexcept {
    for (uint32_t i = 0; i < kCount; ++i) {
        const Candidate& c = kCandidates[i];
        if (c.vs == vs && c.ps == ps && (!c.indices || c.indices == indices) && (!c.instances || c.instances == instances)) return static_cast<int>(i);
    }
    return -1;
}
// The candidate a game frame leaves out: none for the frame the key was pressed in and every frame before it, then the list in order, one a frame.
inline int cycleIndex(uint32_t frame, uint32_t armFrame) noexcept {
    if (frame <= armFrame) return -1;
    return static_cast<int>((frame - armFrame - 1u) % kCount);
}

// What one frame's eye 0 draws came to: the candidate the cycle named for it (-1: none, or the sights were down) and the draws of it left out, per eye.
struct Record {
    bool have = false;
    uint32_t frame = 0;
    int candidate = -1;
    uint32_t draws[2] = {};
};

// The draw path's one relaxed load while no dump is under way, and the frame thread's and the dump's flags.
inline std::atomic<bool> g_live{false};
inline std::atomic<bool> g_adsUp{false};      // the frame thread's: a press of the sights is open
inline std::atomic<uint32_t> g_armFrame{0};

struct State {
    std::mutex mu;                            // the records and the phase; held only while a cycle is live (a dump)
    // the phase of the frame whose eye 0 draws were seen last
    uint32_t phaseFrame = ~0u;
    int phaseCandidate = -1;
    uint32_t phaseDraws[2] = {};
    Record records[kRun];
    uint32_t armed = 0;                       // cycles started this session
};
inline State& state() {
    static State s;
    return s;
}

inline bool live() noexcept { return g_live.load(std::memory_order_relaxed); }
inline void publishAdsUp(bool up) noexcept { g_adsUp.store(up, std::memory_order_relaxed); }
inline bool adsUp() noexcept { return g_adsUp.load(std::memory_order_relaxed); }

// Arm for a manual dump begun in frame `frame`: only with the sights up. False (nothing armed) otherwise.
inline bool arm(uint32_t frame) noexcept {
    if (!g_adsUp.load(std::memory_order_relaxed)) return false;
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mu);
    for (Record& r : s.records) r = Record{};
    s.phaseFrame = ~0u;
    s.phaseCandidate = -1;
    s.phaseDraws[0] = s.phaseDraws[1] = 0;
    g_armFrame.store(frame, std::memory_order_relaxed);
    ++s.armed;
    g_live.store(true, std::memory_order_relaxed);
    return true;
}
inline void disarm() noexcept { g_live.store(false, std::memory_order_relaxed); }

// The draw path (an eye draw, the owner context). True when this draw is the candidate the frame's cycle names: the caller leaves it out. Eye 0's draws also start the frame's phase, so a
// frame in which the candidate was not drawn at all still has its record. psHash is called only for a draw whose vertex shader is a candidate's.
template <class PsHash>
inline bool skips(uint32_t frame, int eye, uint64_t vs, uint32_t indices, uint32_t instances, PsHash&& psHash) noexcept {
    if (!g_live.load(std::memory_order_relaxed)) return false;
    State& s = state();
    const uint32_t armedAt = g_armFrame.load(std::memory_order_relaxed);
    if (frame > armedAt + kMaxFrames) {
        g_live.store(false, std::memory_order_relaxed);
        return false;
    }
    const int cand = g_adsUp.load(std::memory_order_relaxed) ? cycleIndex(frame, armedAt) : -1;
    if (eye == 0) {
        std::lock_guard<std::mutex> lock(s.mu);
        if (s.phaseFrame != frame) {
            s.phaseFrame = frame;
            s.phaseCandidate = cand;
            s.phaseDraws[0] = s.phaseDraws[1] = 0;
        }
    }
    if (cand < 0 || !vsIsCandidate(vs)) return false;
    if (match(vs, psHash(), indices, instances) != cand) return false;
    if (eye == 0 || eye == 1) {
        std::lock_guard<std::mutex> lock(s.mu);
        if (s.phaseFrame == frame) ++s.phaseDraws[eye];
    }
    return true;
}

// The temporal pass took eye 0's raw crop number k: the phase of the frame just drawn is its record.
inline void noteCapture(uint32_t k) noexcept {
    State& s = state();
    if (k >= kRun) return;
    std::lock_guard<std::mutex> lock(s.mu);
    Record& r = s.records[k];
    r.have = true;
    r.frame = s.phaseFrame == ~0u ? 0u : s.phaseFrame;
    r.candidate = s.phaseFrame == ~0u ? -1 : s.phaseCandidate;
    r.draws[0] = s.phaseDraws[0];
    r.draws[1] = s.phaseDraws[1];
}
inline Record record(uint32_t k) noexcept {
    State& s = state();
    Record r;
    if (k >= kRun) return r;
    std::lock_guard<std::mutex> lock(s.mu);
    return s.records[k];
}
inline uint32_t armFrame() noexcept { return g_armFrame.load(std::memory_order_relaxed); }

}  // namespace sightwithhold
}  // namespace edvr

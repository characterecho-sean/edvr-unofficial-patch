// visual.star_glow's rules, pure (no D3D, no globals): flat_star_glow.cpp drives them, tools\ui_quality_test drives every function here.
//
// WHAT THE SETTING DOES. A star draws as a handful of passes into the scene's HDR target H (the pre-pass, the surface, the halo quads, the
// ring, the arcs; a jet's add and flare for a neutron star or white dwarf). The game's bloom chain reads H at its first level and blurs it, so
// the halo around a star is the bloom of the star's own light. At visual.star_glow = 150 the first level reads H' = H + HUD + m * S instead,
// with k = 1.50 and m = k - 1 = 0.50: S is the star draws alone, replayed into a layer of their own as the game issues them, so the bloom
// of H' is the bloom of H plus m times the bloom of S -- a halo k times as strong around the stars and the same one everywhere else. The
// tonemap still reads the original H: only the bloom's input changes.
//
// WHAT IS HERE.
//   - the percentage snapped to its five choices, k and m;
//   - the pair table: which (vertex shader, pixel shader) pair is which pass of a star;
//   - the run machine: for each pair the scope sees, whether to replay it into S, or which reason refuses it. The gates are the frame's
//     pre-pass (a real star frame: a frame marker, drawn into whatever target the game draws it into -- the G-buffer phase's format-23 target
//     in the 10-22 flight -- and independent of the H-candidate gate and of the run), an H-candidate target (the scene's own R11G11B10F at its
//     extent with a depth bound; the 240x150, 256x256 and 512x512 probe copies the game also draws are not), never after the bloom trigger,
//     the ring only straight after a quad (the ring's pair is also the drives' smoke ribbon), and the jet's add only with its source draws
//     earlier in the frame;
//   - the window's counts and the line that prints them, and the line for the setting going back to 100;
//   - the first level's input as arithmetic (the contract the GPU pass is held to) and the blend equation that builds it.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {

// ---- the setting --------------------------------------------------------------------------------------------------------------
// The percentage, snapped to the nearest of 100, 125, 150, 175, 200 (the setting reads only those; a caller that holds another number gets
// the nearest choice, and anything under 100 is the stock 100).
inline int flatStarGlowSnap(int percent) {
    if (percent <= 100) return 100;
    if (percent >= 200) return 200;
    return 100 + ((percent - 100 + 12) / 25) * 25;
}
inline double flatStarGlowK(int percent) { return flatStarGlowSnap(percent) / 100.0; }
// The star layer's weight in the first level's input: k - 1.
inline double flatStarGlowM(int percent) { return flatStarGlowK(percent) - 1.0; }
inline bool flatStarGlowActive(int percent) { return flatStarGlowSnap(percent) > 100; }

// ---- the pairs ----------------------------------------------------------------------------------------------------------------
// Full 64-bit hashes (eye_draw_snapshot.h solarDraw / solarPixel; the jet chain from edvr_gfx_20261022_114920 #433-#448).
constexpr uint64_t kFlatStarPreVs = 0x0EE43D81E394E70Cull;       // the depth pre-pass: no pixel shader, no colour
constexpr uint64_t kFlatStarSurfaceVs = 0x4D516EF05C68FFA5ull, kFlatStarSurfacePs = 0x147E748F4CD3AE9Aull;
constexpr uint64_t kFlatStarInstancedVs = 0x8BD7C37ABCEE7E45ull, kFlatStarInstancedPs = 0x94676B1FD0DF150Full;
constexpr uint64_t kFlatStarQuadAVs = 0xD95905C18B7FAD93ull, kFlatStarQuadAPs = 0x5BCB6B95BE7C0700ull;
constexpr uint64_t kFlatStarQuadBVs = 0xD1281DF454A153ADull, kFlatStarQuadBPs = 0x97DBC87FCAA429C4ull;
constexpr uint64_t kFlatStarRingVs = 0x5E417E9DF2E7F9E6ull, kFlatStarRingPs = 0xBD801F2FB02522EBull;  // also the drives' smoke ribbon
constexpr uint64_t kFlatStarArcsVs = 0x1F3AD1584D7FA3C8ull, kFlatStarArcsPs = 0xBA65C50BBA1ECCBBull;
constexpr uint64_t kFlatStarJetSourceVs = 0xB7413239ADAFF32Aull, kFlatStarJetSourcePs = 0x66F17D60A9A835ADull;  // into a depth-less target of its own
constexpr uint64_t kFlatStarJetAddVs = 0xCFA91824129ECBBCull, kFlatStarJetAddPs = 0xDE3602618E7E7F7Eull;       // the bloom blur pair, here adding into H
constexpr uint64_t kFlatStarFlareVs = 0xF7AF326DCBE36AE9ull, kFlatStarFlarePs = 0x952BBB182CE12A51ull;

// The nine passes the window counts, in the order the line prints them. kJetSource has no slot of its own: it is the evidence a jet exists
// (it sets a flag), not a pass that is replayed.
enum class FlatStarRole : uint8_t {
    kNone = 0,
    kPre,        // slot 0
    kSurface,    // 1
    kQuadA,      // 2
    kInstanced,  // 3
    kQuadB,      // 4
    kRing,       // 5
    kArcs,       // 6
    kJet,        // 7 -- the jet's final add into H
    kFlare,      // 8
    kJetSource,  // no slot
    kCount
};
constexpr size_t kFlatStarSlots = 9;
inline int flatStarSlot(FlatStarRole r) {
    const int v = static_cast<int>(r);
    return v >= 1 && v <= static_cast<int>(kFlatStarSlots) ? v - 1 : -1;
}
inline const char* flatStarSlotName(size_t slot) {
    static const char* const kNames[kFlatStarSlots] = {"pre", "surf", "qa", "inst", "qb", "ring", "arcs", "jet", "flare"};
    return slot < kFlatStarSlots ? kNames[slot] : "?";
}

// Which pass a (vs, ps) pair is. The jet's add is the game's ordinary bloom blur pair (CFA91824 with DE360261), which the chain also draws into
// depth-less targets all frame: it is a jet's add only when it writes an H candidate, so `hCandidate` is part of its identity.
inline FlatStarRole flatStarRoleOf(uint64_t vs, uint64_t ps, bool hCandidate) {
    switch (vs) {
        case kFlatStarPreVs: return ps == 0 ? FlatStarRole::kPre : FlatStarRole::kNone;
        case kFlatStarSurfaceVs: return ps == kFlatStarSurfacePs ? FlatStarRole::kSurface : FlatStarRole::kNone;
        case kFlatStarInstancedVs: return ps == kFlatStarInstancedPs ? FlatStarRole::kInstanced : FlatStarRole::kNone;
        case kFlatStarQuadAVs: return ps == kFlatStarQuadAPs ? FlatStarRole::kQuadA : FlatStarRole::kNone;
        case kFlatStarQuadBVs: return ps == kFlatStarQuadBPs ? FlatStarRole::kQuadB : FlatStarRole::kNone;
        case kFlatStarRingVs: return ps == kFlatStarRingPs ? FlatStarRole::kRing : FlatStarRole::kNone;
        case kFlatStarArcsVs: return ps == kFlatStarArcsPs ? FlatStarRole::kArcs : FlatStarRole::kNone;
        case kFlatStarJetSourceVs: return ps == kFlatStarJetSourcePs ? FlatStarRole::kJetSource : FlatStarRole::kNone;
        case kFlatStarJetAddVs: return ps == kFlatStarJetAddPs && hCandidate ? FlatStarRole::kJet : FlatStarRole::kNone;
        case kFlatStarFlareVs: return ps == kFlatStarFlarePs ? FlatStarRole::kFlare : FlatStarRole::kNone;
        default: return FlatStarRole::kNone;
    }
}
// Only the jet's add asks the context for its blend (it must be ONE / ONE): the other passes take the game's state as it is.
inline bool flatStarRoleNeedsBlend(FlatStarRole r) { return r == FlatStarRole::kJet; }
// The pass is replayed into S when the gates allow (the pre-pass and the jet's source are only evidence).
inline bool flatStarRoleReplays(FlatStarRole r) {
    return r != FlatStarRole::kNone && r != FlatStarRole::kPre && r != FlatStarRole::kJetSource;
}

// ---- the run machine ------------------------------------------------------------------------------------------------------------
// Why a pass the setting could have replayed was not (the last four are the GPU half's, counted by the module, never decided here).
enum class FlatStarRefusal : uint8_t {
    kNone = 0,
    kNotHCandidate,   // the pass writes a target that is not an H candidate (the game's probe copies)
    kAfterTrigger,    // the bloom chain has already started this frame: S could not reach its first level
    kNoPrePass,       // no pre-pass (into any target) earlier this frame: not a star frame
    kRingNotInRun,    // the ring's pair, but not straight after a quad (the drives' smoke ribbon)
    kJetNoSource,     // the jet's add or the flare without the jet's source draws earlier, or an add that is not ONE / ONE
    kSizeMismatch,    // S and H are not the same size
    kStateFault,      // the replay's bracket faulted
    kLayerFail,       // S, or the first level's pass, could not be made
    kDepthCloneFail,  // the bound depth-stencil state could not be cloned without its writes
    kBloomOff,        // the frame's trigger is not the bloom chain's first level (the game's Bloom is off)
    kCount
};
constexpr size_t kFlatStarRefusals = static_cast<size_t>(FlatStarRefusal::kCount) - 1;
inline const char* flatStarRefusalName(FlatStarRefusal r) {
    switch (r) {
        case FlatStarRefusal::kNotHCandidate: return "not-h-candidate";
        case FlatStarRefusal::kAfterTrigger: return "after-trigger";
        case FlatStarRefusal::kNoPrePass: return "no-pre-pass";
        case FlatStarRefusal::kRingNotInRun: return "ring-not-in-run";
        case FlatStarRefusal::kJetNoSource: return "jet-no-source";
        case FlatStarRefusal::kSizeMismatch: return "size-mismatch";
        case FlatStarRefusal::kStateFault: return "state-fault";
        case FlatStarRefusal::kLayerFail: return "layer-fail";
        case FlatStarRefusal::kDepthCloneFail: return "depth-clone-fail";
        case FlatStarRefusal::kBloomOff: return "bloom-off";
        default: return "?";
    }
}

// One frame's state. Reset at the frame boundary.
struct FlatStarFrame {
    bool pre = false;             // a pre-pass was drawn this frame, into any target: the frame's marker, kept apart from the run (prev)
    bool jetSource = false;       // the jet's source draws were drawn
    FlatStarRole prev = FlatStarRole::kNone;  // the last star pass in the current run (an H-candidate pass); a non-star H-candidate draw ends the run
    bool anyStar = false;         // a star pass of any target was seen: the window's star-frames
    bool anyReplay = false;       // a replay was bound (set by the module, not by the decision): S was cleared and holds light this frame
};
inline void flatStarBeginFrame(FlatStarFrame& f) { f = FlatStarFrame{}; }

// What the scope read about the draw.
struct FlatStarFacts {
    bool hCandidate = false;   // flatHdrCandidateDraw: an R11G11B10F target at the scene's extent with a depth bound
    bool triggered = false;    // the HDR route's trigger (the bloom chain's first level) has come this frame
    bool blendOneOne = false;  // the bound blend is ONE / ONE, ADD (read only for the jet's add)
};
struct FlatStarVerdict {
    FlatStarRole role = FlatStarRole::kNone;
    bool replay = false;
    FlatStarRefusal refusal = FlatStarRefusal::kNone;
};

inline bool flatStarRunAllowsRing(FlatStarRole prev) {
    return prev == FlatStarRole::kQuadA || prev == FlatStarRole::kInstanced || prev == FlatStarRole::kQuadB;
}

// A draw that is not a star pass. An H-candidate one ends the run, so the ring that follows it is not a star's.
inline void flatStarNoteOther(FlatStarFrame& f, bool hCandidate) {
    if (hCandidate) f.prev = FlatStarRole::kNone;
}

// A star pass, in the order the scope sees draws. Updates the frame and answers whether to replay it.
inline FlatStarVerdict flatStarDecide(FlatStarFrame& f, FlatStarRole role, const FlatStarFacts& facts) {
    FlatStarVerdict v;
    v.role = role;
    if (role == FlatStarRole::kNone) {
        flatStarNoteOther(f, facts.hCandidate);
        return v;
    }
    f.anyStar = true;
    if (role == FlatStarRole::kPre) {
        // The frame's marker, set whatever the target is: the game draws the depth pre-pass in its G-buffer phase, long before the star run and
        // into a target that is not an H candidate. It is never replayed and never refused, and it does not touch the run.
        f.pre = true;
        return v;
    }
    if (role == FlatStarRole::kJetSource) {
        f.jetSource = true;   // any target: the source draws go into a depth-less target of their own
        return v;
    }
    FlatStarRefusal why = FlatStarRefusal::kNone;
    if (!facts.hCandidate) why = FlatStarRefusal::kNotHCandidate;
    else if (facts.triggered) why = FlatStarRefusal::kAfterTrigger;
    else if (!f.pre) why = FlatStarRefusal::kNoPrePass;
    else if (role == FlatStarRole::kRing && !flatStarRunAllowsRing(f.prev)) why = FlatStarRefusal::kRingNotInRun;
    else if (role == FlatStarRole::kJet && !(f.jetSource && facts.blendOneOne)) why = FlatStarRefusal::kJetNoSource;
    else if (role == FlatStarRole::kFlare && !f.jetSource) why = FlatStarRefusal::kJetNoSource;
    if (facts.hCandidate) f.prev = role;   // a refused pass of a star is still the run's last pass
    v.refusal = why;
    v.replay = why == FlatStarRefusal::kNone;
    return v;
}

// ---- the window ---------------------------------------------------------------------------------------------------------------
struct FlatStarWindow {
    uint64_t starFrames = 0;
    uint64_t seen[kFlatStarSlots] = {};
    uint64_t replayed[kFlatStarSlots] = {};
    uint64_t layerBuilt = 0;   // frames in which S took its first replay (cleared, drawn into)
    uint64_t asked = 0;        // first-level draws that asked for the star input
    uint64_t fed = 0;          // ... and read H' with it
    uint64_t refused[kFlatStarRefusals] = {};
};
inline void flatStarCountRefusal(FlatStarWindow& w, FlatStarRefusal r) {
    const int i = static_cast<int>(r) - 1;
    if (i >= 0 && static_cast<size_t>(i) < kFlatStarRefusals) ++w.refused[i];
}
// One decided pass into the window: seen under its slot, or the refusal. A pass the gates allowed is counted as replayed only once its
// replay is bound (flatStarCountReplayed): the GPU half can still refuse it.
inline void flatStarCountVerdict(FlatStarWindow& w, const FlatStarVerdict& v) {
    const int slot = flatStarSlot(v.role);
    if (slot >= 0) ++w.seen[slot];
    if (v.refusal != FlatStarRefusal::kNone) flatStarCountRefusal(w, v.refusal);
}
inline void flatStarCountReplayed(FlatStarWindow& w, FlatStarRole role) {
    const int slot = flatStarSlot(role);
    if (slot >= 0) ++w.replayed[slot];
}
// A bounded appender for the lines below: every write is cut at the buffer's end and the text stays terminated.
struct FlatStarText {
    char* out;
    size_t n;
    size_t used = 0;
    FlatStarText(char* o, size_t cap) : out(o), n(cap) { if (out && n) out[0] = 0; }
    void put(const char* text) {
        for (; text && *text && used + 1 < n; ++text) out[used++] = *text;
        if (out && n) out[used] = 0;
    }
    void putUnsigned(uint64_t v) {
        char tmp[24];
        std::snprintf(tmp, sizeof(tmp), "%llu", static_cast<unsigned long long>(v));
        put(tmp);
    }
    void putList(const uint64_t* values, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            if (i) put(",");
            putUnsigned(values[i]);
        }
    }
};
// The 5 s line, printed only while the setting is above 100. setting=150 with every count zero means the scope never matched a star pass.
inline int flatStarWindowLine(char* out, size_t n, int percent, const FlatStarWindow& w) {
    if (!out || n < 16) return 0;
    FlatStarText t(out, n);
    char head[96];
    std::snprintf(head, sizeof(head), "flat star glow: window=5s setting=%d k=%.2f star-frames=", flatStarGlowSnap(percent), flatStarGlowK(percent));
    t.put(head);
    t.putUnsigned(w.starFrames);
    t.put(" seen[");
    for (size_t i = 0; i < kFlatStarSlots; ++i) { if (i) t.put(","); t.put(flatStarSlotName(i)); }
    t.put("]=");
    t.putList(w.seen, kFlatStarSlots);
    t.put(" replayed[");
    for (size_t i = 0; i < kFlatStarSlots; ++i) { if (i) t.put(","); t.put(flatStarSlotName(i)); }
    t.put("]=");
    t.putList(w.replayed, kFlatStarSlots);
    t.put(" layer-built=");
    t.putUnsigned(w.layerBuilt);
    t.put(" asked=");
    t.putUnsigned(w.asked);
    t.put(" fed=");
    t.putUnsigned(w.fed);
    t.put(" refused[");
    for (size_t i = 0; i < kFlatStarRefusals; ++i) { if (i) t.put(","); t.put(flatStarRefusalName(static_cast<FlatStarRefusal>(i + 1))); }
    t.put("]=");
    t.putList(w.refused, kFlatStarRefusals);
    return static_cast<int>(t.used);
}
// The line when the setting goes back to 100: the window's last counts are not printed, the feature is off.
inline int flatStarOffLine(char* out, size_t n, int fromPercent) {
    if (!out || n < 16) return 0;
    const int c = std::snprintf(out, n, "flat star glow: off (setting=100, was %d); nothing runs and nothing is created at 100", flatStarGlowSnap(fromPercent));
    return c < 0 ? 0 : static_cast<int>(static_cast<size_t>(c) < n ? static_cast<size_t>(c) : n - 1);
}

// ---- the first level's input ------------------------------------------------------------------------------------------------------
// H' = H + HUD + m * S, per channel: the contract the GPU passes are held to. The HUD feed (kUiGlowFeedPsHlsl) writes H + HUD, the star pass
// adds m * S by blending (the blend factor is (m, m, m, 0): source factor BLEND_FACTOR, destination factor ONE, ADD). S never holds the HUD
// and H holds neither, so the HUD and the star light each enter once. The target is R11G11B10_FLOAT: negative values store as 0 and the red
// and green channels top out at 65024 (blue at 64512).
constexpr float kFlatStarR11Max = 65024.0f, kFlatStarB10Max = 64512.0f;
inline float flatStarClampR11(float v, int channel) {
    const float top = channel == 2 ? kFlatStarB10Max : kFlatStarR11Max;
    if (!(v > 0.0f)) return 0.0f;   // negative, zero or NaN
    return v < top ? v : top;
}
// What the star pass reads out of S: non-finite texels read zero, the sum stays inside the format (kStarGlowAddPsHlsl).
inline float flatStarSanitise(float s) {
    if (!std::isfinite(s) || s < 0.0f) return 0.0f;
    return s < 60000.0f ? s : 60000.0f;
}
inline float flatStarFirstLevel(float h, float hud, float s, double m, int channel) {
    const float feed = flatStarClampR11(h + hud, channel);   // the HUD feed's pass (or the plain copy of H) writes this first
    return flatStarClampR11(feed + static_cast<float>(m) * flatStarSanitise(s), channel);
}
// The blend equation of the star pass, as the D3D11_BLEND values (ui_layer.cpp static_asserts each against its enum): colour is
// Src * BLEND_FACTOR + Dst * ONE; the alpha channel adds nothing.
constexpr int kFlatStarBlendSrc = 14;     // D3D11_BLEND_BLEND_FACTOR
constexpr int kFlatStarBlendDst = 2;      // D3D11_BLEND_ONE
constexpr int kFlatStarBlendOp = 1;       // D3D11_BLEND_OP_ADD
constexpr int kFlatStarBlendSrcAlpha = 1; // D3D11_BLEND_ZERO
constexpr int kFlatStarBlendDstAlpha = 2; // D3D11_BLEND_ONE
// The blend, evaluated: what the pass leaves in a channel of H' (before the format's clamp) for the star pass's output `src`, the target's
// current value `dst` and the factor.
inline float flatStarBlendRgb(float src, float dst, float factor) { return src * factor + dst; }

}  // namespace edvr

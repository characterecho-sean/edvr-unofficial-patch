// visual.star_glow in the headset: the rules, pure (no D3D, no globals). vr_star_glow.cpp drives them, tools\ui_quality_test drives every function here.
//
// WHAT IT IS. The flat mechanism (flat_star_glow_math.h), per eye. A star draws as a handful of passes into the eye's HDR target H; each pass the gates
// let through is issued once more into a layer S of that H, and the bloom chain's first level reads H' = H + m * S instead of H for its one draw (m = k - 1,
// k = the setting over 100). The exposure reduction and the composite read the real H. The star light is one term of H': the eye's HDR HUD layer is the other
// (vr_hud_glow_math.h), and the pass that makes H' adds both; the rules in this header are the star light's alone.
//
// WHAT IS DIFFERENT FROM FLAT, and why this header exists:
//   - Everything is keyed by the H a draw writes. VR renders two eyes with two separate H textures and their draws interleave in submission order, so
//     "the current eye" and "the draw before this one" mean nothing across eyes: the slot table, the run of each pass, the first level and the light
//     each belong to one H.
//   - The first level is found by its pixel shader (143AAE05...) reading an H at t0, not by the HDR route's trigger (the trigger is flat's, and the
//     flat rule wants a first level of half H per axis; VR's is H-sized, 2016x2016 of 2016x2017).
//   - The depth pre-pass is drawn into the G-buffer, not into H, so it is keyed by the depth view it shares with that eye's H (the 10-27 census: the pre-pass
//     #97 and the star passes #316-#321 bind the same depth view, as do #197 and #329-#334 for the other eye).
//   - The setting is off at 100, and off while Elite's Bloom is known to be off (the hint's reader: there is no first level to feed then).
//
// WHAT IS HERE. The mode (on, paused for Bloom, off); the first level's identity and the verdict for the H it reads; the slot table; the pre-pass table;
// the per-H run (flat's run machine, fed this header's facts); the window and the lines that print it.
#pragma once

#include "../common/star_glow_bloom.h"
#include "flat_star_glow_math.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {

// ---- when it runs --------------------------------------------------------------------------------------------------------------
// ON: the setting is above 100 and Elite's Bloom is not known to be off (unknown runs: a hint that may be wrong is worse than none, and a Bloom that is
// really off simply never shows a first level). PAUSED: above 100 but Bloom is known off, so there is no halo to strengthen. OFF: 100. Only ON replays.
// PAUSED is the menus' hint condition itself (sgb::hint is null exactly when the glow can do something), so the headset's menu and this driver cannot
// disagree about when Bloom being off matters.
enum class VrStarMode : uint8_t { kOff = 0, kOn, kPaused };
static_assert(sgb::kStockPercent == 100, "the Bloom hint's stock threshold is the setting's stock 100");
inline VrStarMode vrStarMode(int percent, sgb::Bloom bloom) {
    if (!flatStarGlowActive(percent)) return VrStarMode::kOff;
    return sgb::hint(percent, bloom) ? VrStarMode::kPaused : VrStarMode::kOn;
}

// ---- the first level -----------------------------------------------------------------------------------------------------------
// The bloom chain's first level: the draw with this pixel shader, reading an H at t0. Four draws in the 10-27 census, one an eye each, and nothing else
// uses the shader. The exposure reduction (DEF19B03 / 831DF02E), the composite (2D78DC3F / D0A16B9E), the chain's downsample (5ECFE708) and its
// upsample-add (129F602B / 8826CACC) read H or the bloom too and are NOT this draw: they keep the real H.
constexpr uint64_t kVrStarFirstLevelPs = 0x143AAE0597E2F7BFull;
inline bool vrStarIsFirstLevel(uint64_t ps) { return ps == kVrStarFirstLevelPs; }

// What the first level's t0 turned out to be, and what is done about it.
enum class VrFirstLevel : uint8_t {
    kFeed = 0,       // an H that holds this frame's star light and has not been fed yet: bind H' for this draw
    kUnknownH,       // t0 is not an H the slot table holds (the 2D screen's scene, an H no star pass touched)
    kHoldsNoLight,   // an H the table holds, but no star pass of it was replayed this frame
    kRepeat,         // an H already fed this frame (a second first level reading it): never fed twice
};
inline VrFirstLevel vrStarFirstLevel(bool knownH, bool lit, bool done) {
    if (!knownH) return VrFirstLevel::kUnknownH;
    if (done) return VrFirstLevel::kRepeat;
    return lit ? VrFirstLevel::kFeed : VrFirstLevel::kHoldsNoLight;
}

// ---- the H slots ---------------------------------------------------------------------------------------------------------------
// One slot per H a star pass was seen writing, found by the H's resource identity. Two eyes need two; four leave room for a pool the game cycles through.
constexpr size_t kVrStarSlots = 4;
constexpr uint64_t kVrStarSlotIdleFrames = 600;   // a slot unused this long is released (its H is gone: a resolution change made a new one)
constexpr uint64_t kVrStarSlotStaleFrames = 2;    // a full table evicts a slot only if it was not used this frame or the one before

struct VrStarSlotTable {
    const void* key[kVrStarSlots] = {};
    uint64_t lastFrame[kVrStarSlots] = {};

    int find(const void* k) const {
        if (!k) return -1;
        for (size_t i = 0; i < kVrStarSlots; ++i)
            if (key[i] == k) return static_cast<int>(i);
        return -1;
    }
    // The slot of `k`: the one it holds, a free one, or the idlest one that is stale. -1: the table is full of H in use (slots-full). *fresh: the slot
    // was not this key's before (the caller builds it again).
    int claim(const void* k, uint64_t frame, bool* fresh) {
        if (fresh) *fresh = false;
        if (!k) return -1;
        int at = find(k);
        if (at < 0) {
            for (size_t i = 0; i < kVrStarSlots && at < 0; ++i)
                if (!key[i]) at = static_cast<int>(i);
            if (at < 0) {
                uint64_t oldest = ~0ull;
                for (size_t i = 0; i < kVrStarSlots; ++i) {
                    if (lastFrame[i] + kVrStarSlotStaleFrames <= frame && lastFrame[i] < oldest) {
                        oldest = lastFrame[i];
                        at = static_cast<int>(i);
                    }
                }
            }
            if (at < 0) return -1;
            key[at] = k;
            if (fresh) *fresh = true;
        }
        lastFrame[at] = frame;
        return at;
    }
    void release(int i) {
        if (i >= 0 && static_cast<size_t>(i) < kVrStarSlots) {
            key[i] = nullptr;
            lastFrame[i] = 0;
        }
    }
    bool idle(int i, uint64_t frame) const {
        return i >= 0 && static_cast<size_t>(i) < kVrStarSlots && key[i] && frame > lastFrame[i] && frame - lastFrame[i] > kVrStarSlotIdleFrames;
    }
};

// ---- the pre-pass ----------------------------------------------------------------------------------------------------------------
// The depth pre-pass (0EE43D81, no pixel shader) is drawn into the G-buffer, so it is no draw of H. What it shares with its eye's H is the depth view,
// and that is the key: a star pass of an H is "in a real star frame" when a pre-pass bound the same depth view earlier in the same frame.
constexpr size_t kVrStarPreSlots = 4;
struct VrStarPreTable {
    const void* dsv[kVrStarPreSlots] = {};
    uint64_t frame[kVrStarPreSlots] = {};
    size_t next = 0;

    void note(const void* d, uint64_t f) {
        if (!d) return;
        for (size_t i = 0; i < kVrStarPreSlots; ++i) {
            if (dsv[i] == d) {
                frame[i] = f;
                return;
            }
        }
        dsv[next] = d;
        frame[next] = f;
        next = (next + 1) % kVrStarPreSlots;
    }
    bool seen(const void* d, uint64_t f) const {
        if (!d) return false;
        for (size_t i = 0; i < kVrStarPreSlots; ++i)
            if (dsv[i] == d && frame[i] == f) return true;
        return false;
    }
};

// ---- the run of one H ------------------------------------------------------------------------------------------------------------
// Flat's run machine, one per H. `f.pre` and `f.jetSource` are the frame's evidence as it applies to this H; `done` is this H's first level having
// come (flat's "triggered", per H: eye A's first level does not refuse eye B's passes); `lit` is S holding this frame's star light.
struct VrStarH {
    FlatStarFrame f;
    bool lit = false;
    bool done = false;
};
inline void vrStarHBeginFrame(VrStarH& h) { h = VrStarH{}; }

struct VrStarFacts {
    bool preSeen = false;     // a pre-pass bound this H's depth view earlier this frame (VrStarPreTable::seen)
    bool jetSource = false;   // the jet's source was drawn earlier this frame (any target: its own, depth-less)
    bool blendOneOne = false; // the bound blend is ONE / ONE, ADD (read only for the jet's add)
};
// One draw into an H, in submission order. `role` is flatStarRoleOf(vs, ps, true): H is a candidate by construction (the driver asks only of draws that
// write an eye H with depth bound). A draw that is no star pass ends this H's run (the ring that follows is not a star's).
inline FlatStarVerdict vrStarDecide(VrStarH& h, FlatStarRole role, const VrStarFacts& facts) {
    h.f.pre = h.f.pre || facts.preSeen;
    h.f.jetSource = h.f.jetSource || facts.jetSource;
    FlatStarFacts ff;
    ff.hCandidate = true;
    ff.triggered = h.done;
    ff.blendOneOne = facts.blendOneOne;
    return flatStarDecide(h.f, role, ff);
}

// ---- refusals --------------------------------------------------------------------------------------------------------------------
// Flat's four decisions, then this driver's own. No not-h-candidate (a draw is looked at only if it writes an eye H) and no bloom-off (the first level is
// found by its shader: a frame with light and no first level is no-first-level, counted when the frame ends). A star pass of an H the full table cannot
// hold has no slot to count in: it is the line head's slots-full.
enum class VrStarRefusal : uint8_t {
    kNone = 0,
    kAfterTrigger,    // this H's first level has already come this frame: S could not reach it
    kNoPrePass,       // no pre-pass bound this H's depth view earlier this frame: not a star frame
    kRingNotInRun,    // the ring's pair, but not straight after a quad (the drives' smoke ribbon)
    kJetNoSource,     // the jet's add or the flare without the jet's source earlier, or an add that is not ONE / ONE
    kNotPlain,        // the game's own draw was altered or withheld by another fix: it is not replayed
    kStateFault,      // the replay's bracket faulted, or the fault budget is spent
    kLayerFail,       // S could not be made
    kDepthCloneFail,  // the bound depth-stencil state could not be cloned without its writes
    kNoFirstLevel,    // the frame ended with S holding light and no first level read this H
    kFeedFail,        // the first level asked for H' and the pass could not make it
    kCount
};
constexpr size_t kVrStarRefusals = static_cast<size_t>(VrStarRefusal::kCount) - 1;
inline const char* vrStarRefusalName(VrStarRefusal r) {
    switch (r) {
        case VrStarRefusal::kAfterTrigger: return "after-trigger";
        case VrStarRefusal::kNoPrePass: return "no-pre-pass";
        case VrStarRefusal::kRingNotInRun: return "ring-not-in-run";
        case VrStarRefusal::kJetNoSource: return "jet-no-source";
        case VrStarRefusal::kNotPlain: return "not-plain";
        case VrStarRefusal::kStateFault: return "state-fault";
        case VrStarRefusal::kLayerFail: return "layer-fail";
        case VrStarRefusal::kDepthCloneFail: return "depth-clone-fail";
        case VrStarRefusal::kNoFirstLevel: return "no-first-level";
        case VrStarRefusal::kFeedFail: return "feed-fail";
        default: return "?";
    }
}
// The four reasons flatStarDecide gives, in this header's names (anything else it can say is no refusal here).
inline VrStarRefusal vrStarRefusalOf(FlatStarRefusal r) {
    switch (r) {
        case FlatStarRefusal::kAfterTrigger: return VrStarRefusal::kAfterTrigger;
        case FlatStarRefusal::kNoPrePass: return VrStarRefusal::kNoPrePass;
        case FlatStarRefusal::kRingNotInRun: return VrStarRefusal::kRingNotInRun;
        case FlatStarRefusal::kJetNoSource: return VrStarRefusal::kJetNoSource;
        default: return VrStarRefusal::kNone;
    }
}

// ---- the window ------------------------------------------------------------------------------------------------------------------
// The passes an H can have replayed: flat's nine without the pre-pass (evidence of the frame, counted once in the header of the line).
constexpr size_t kVrStarRoles = kFlatStarSlots - 1;
inline int vrStarRoleIndex(FlatStarRole r) {
    const int s = flatStarSlot(r);
    return s >= 1 ? s - 1 : -1;
}
inline const char* vrStarRoleName(size_t i) { return flatStarSlotName(i + 1); }

struct VrStarSlotWindow {
    uint32_t w = 0, h = 0;          // the H's size (0: the slot is free)
    uint64_t starFrames = 0;        // frames in which a star pass of this H was seen
    uint64_t seen[kVrStarRoles] = {};       // star passes decided, refused ones included
    uint64_t replayed[kVrStarRoles] = {};   // ... and bound into S
    uint64_t layers = 0;            // frames in which S took its first replay (cleared, drawn into)
    uint64_t firstLevels = 0;       // first-level draws that read this H while it held light
    uint64_t fed = 0;               // ... and read H' with it
    uint64_t refused[kVrStarRefusals] = {};
};
struct VrStarWindow {
    uint64_t draws = 0;             // draws the hook looked at (the owner context's, while armed)
    uint64_t hDraws = 0;            // ... of them into an eye H with depth bound
    uint64_t prePasses = 0;         // pre-passes seen (any target)
    uint64_t jetSources = 0;        // jet sources seen
    uint64_t slotsFull = 0;         // star passes of an H the full table could not hold
    uint64_t firstLevelUnknownH = 0, firstLevelNoLight = 0, firstLevelRepeat = 0;   // first-level draws that read an H nothing was fed to
    VrStarSlotWindow slot[kVrStarSlots];
};
inline void vrStarCountRefusal(VrStarSlotWindow& w, VrStarRefusal r) {
    const int i = static_cast<int>(r) - 1;
    if (i >= 0 && static_cast<size_t>(i) < kVrStarRefusals) ++w.refused[i];
}

// The 10 s line's head: the hook's own liveness and the first levels nothing was fed to. Printed while the mode is ON. draws=0 means the hook never ran
// (no owner draw reached it); h-draws=0 with draws above zero means no draw wrote an eye H.
inline int vrStarWindowLine(char* out, size_t n, int percent, size_t slotsInUse, const VrStarWindow& w) {
    if (!out || n < 16) return 0;
    FlatStarText t(out, n);
    char head[224];
    std::snprintf(head, sizeof(head),
                  "vr star glow: window=10s setting=%d k=%.2f h-slots=%u slots-full=%llu draws=%llu h-draws=%llu pre-passes=%llu jet-sources=%llu "
                  "first-level-not-fed[unknown-h,no-light,repeat]=",
                  flatStarGlowSnap(percent), flatStarGlowK(percent), static_cast<unsigned>(slotsInUse), static_cast<unsigned long long>(w.slotsFull),
                  static_cast<unsigned long long>(w.draws), static_cast<unsigned long long>(w.hDraws), static_cast<unsigned long long>(w.prePasses),
                  static_cast<unsigned long long>(w.jetSources));
    t.put(head);
    const uint64_t not_fed[3] = {w.firstLevelUnknownH, w.firstLevelNoLight, w.firstLevelRepeat};
    t.putList(not_fed, 3);
    return static_cast<int>(t.used);
}
// One H of the window (index = its slot, printed from 1): its size, its star frames, what was seen and replayed by pass, the layers, the first levels and
// the feeds, and every refusal by name. Printed for each slot that has an H.
inline int vrStarSlotLine(char* out, size_t n, size_t index, const VrStarSlotWindow& w) {
    if (!out || n < 16) return 0;
    FlatStarText t(out, n);
    char head[96];
    std::snprintf(head, sizeof(head), "vr star glow: h%u %ux%u star-frames=", static_cast<unsigned>(index + 1), w.w, w.h);
    t.put(head);
    t.putUnsigned(w.starFrames);
    t.put(" seen[");
    for (size_t i = 0; i < kVrStarRoles; ++i) { if (i) t.put(","); t.put(vrStarRoleName(i)); }
    t.put("]=");
    t.putList(w.seen, kVrStarRoles);
    t.put(" replayed[");
    for (size_t i = 0; i < kVrStarRoles; ++i) { if (i) t.put(","); t.put(vrStarRoleName(i)); }
    t.put("]=");
    t.putList(w.replayed, kVrStarRoles);
    t.put(" layers=");
    t.putUnsigned(w.layers);
    t.put(" first-levels=");
    t.putUnsigned(w.firstLevels);
    t.put(" fed=");
    t.putUnsigned(w.fed);
    t.put(" refused[");
    for (size_t i = 0; i < kVrStarRefusals; ++i) { if (i) t.put(","); t.put(vrStarRefusalName(static_cast<VrStarRefusal>(i + 1))); }
    t.put("]=");
    t.putList(w.refused, kVrStarRefusals);
    return static_cast<int>(t.used);
}
// The line when the setting goes back to 100.
inline int vrStarOffLine(char* out, size_t n, int fromPercent) {
    if (!out || n < 16) return 0;
    const int c = std::snprintf(out, n, "vr star glow: off (setting=100, was %d); nothing runs and nothing is created at 100", flatStarGlowSnap(fromPercent));
    return c < 0 ? 0 : static_cast<int>(static_cast<size_t>(c) < n ? static_cast<size_t>(c) : n - 1);
}
// The line when the setting stays above 100 and Elite's Bloom is known off (or comes back on).
inline int vrStarPausedLine(char* out, size_t n, int percent) {
    if (!out || n < 16) return 0;
    const int c = std::snprintf(out, n, "vr star glow: paused at setting=%d: Elite's Bloom is known to be off, so there is no first level to feed; nothing runs until Bloom is on",
                                flatStarGlowSnap(percent));
    return c < 0 ? 0 : static_cast<int>(static_cast<size_t>(c) < n ? static_cast<size_t>(c) : n - 1);
}

}  // namespace edvr

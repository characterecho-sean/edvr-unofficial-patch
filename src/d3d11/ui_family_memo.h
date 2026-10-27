#pragma once
// The pure logic of vscreen's memo of the UI layer's plain-decline family decision, built 2026-10-27
// (docs\engine-render-performance-2026-09-19.md, "Desk design: three more draw-path trims", trim F). No Windows and no D3D here, so
// tools\aa_off_test drives every decision the draw path takes.
//
// WHAT THE MEMO IS. forwardWithVerdict asks uiLayerFamilyOf for every owner eye draw while the layer is live, to learn which piece
// of the interface (if any) the draw is. For an ordinary scene draw the answer is "none", and what it is depends on four things only:
//   - the bound vertex shader's hash, and the bound pixel shader's hash (read by the rule only for the nine VSs that are named by a pair);
//   - the target kind of render target 0 (ui_layer.cpp uiLayerTargetKind: cached on that slot's binding generation);
//   - the on-foot sharp-HUD flag (nativeOnFootSharpHudWanted).
// The kind-1 rule (ui_layer_math.h uiLayerFamilyFor, the lit HDR target) is a pure function of those and constant tables. The memo
// remembers the key of the last full run; a draw whose key is the same, after a full run that was a plain decline, is told "none" without
// the run. The key is the two hashes, Rtv0's generation, the flag and the layer's epoch (ui_layer.h uiLayerFamilyEpoch: bumped where the
// target-kind cache is reset other than by its own refresh, and where the layer's liveness is recomputed).
//
// WHAT A PLAIN DECLINE IS. A full run that returned none, at a target kind other than 2, for a vertex shader that is neither the menu
// panel's nor the loader's. Kind 2 (the post-tonemap target) is the one branch that reads more state (ui_depth's exclude list and
// learned surfaces, the panel-size test), and the panel and loader vertex shaders feed the family census (uiLayerNoteFamilyProbe), a
// call that counts: every other exit stores "not a plain decline" and the next call runs the whole decision.
//
// The memo has no config key and no switch: a decision that always gives the same answer. The verification and the counters below are
// TEMPORARY (the ui_depth memo's, which was stripped once its flight proved it exact): on a rotating one frame in sixteen that is not a
// sampled frame, a hit also runs the full decision and compares, and the window's line says how often.
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace uifamilymemo {

// What a stored decline was computed at. Every field is something the decision reads; each of the counters only moves forward.
struct Key {
    uint64_t vs = 0;              // bindingShaderHash(Vs)
    uint64_t ps = 0;              // bindingShaderHash(Ps)
    uint32_t rtvGeneration = 0;   // bindingGeneration(Rtv0): the target-kind cache's own key
    uint32_t epoch = 0;           // uiLayerFamilyEpoch()
    bool     onFoot = false;      // nativeOnFootSharpHudWanted()
};

inline bool sameKey(const Key& a, const Key& b) {
    return a.vs == b.vs && a.ps == b.ps && a.rtvGeneration == b.rtvGeneration && a.epoch == b.epoch && a.onFoot == b.onFoot;
}

// Which exit a full run took. kPlainDecline is the one answer the key alone determines; kOther is every other exit.
enum class Exit : uint8_t { kOther = 0, kPlainDecline = 1 };

struct Memo {
    Key  key;
    Exit exit = Exit::kOther;
    bool stored = false;   // false until the first full run stores
};

// A call hits only when something is stored, the stored exit was a plain decline and every field of the key is the stored one.
inline bool hit(const Memo& m, const Key& now) {
    return m.stored && m.exit == Exit::kPlainDecline && sameKey(m.key, now);
}

inline void store(Memo& m, const Key& key, Exit exit) {
    m.key = key;
    m.exit = exit;
    m.stored = true;
}

// Why a call was not a hit (asked only after hit() said no): nothing was stored, or what was stored was not a plain decline; or a
// plain decline was stored and the key had moved.
enum class MissCause : uint8_t { kNothingStored = 0, kKeyMoved = 1 };
inline MissCause missCause(const Memo& m) {
    return m.stored && m.exit == Exit::kPlainDecline ? MissCause::kKeyMoved : MissCause::kNothingStored;
}

// The exit a full run earns. targetKind is uiLayerTargetKind() read after the run (0 when the caller did not need it: a run that was
// not a decline is kOther whatever the kind). flagStill: the on-foot flag read after the run is the one the key holds (the flag is
// written from another thread, and the run reads it itself; a run that may have seen the other value stores nothing it can hit on).
inline Exit exitOf(bool familyIsNone, int targetKind, bool probeVs, bool flagStill) {
    return familyIsNone && targetKind != 2 && !probeVs && flagStill ? Exit::kPlainDecline : Exit::kOther;
}

// ---- TEMPORARY: the verification and its counters ----------------------------------------------------------------------------
// The verify frames: (n + n/16) % 16 == 8 over n, the count of frame boundaries this module has seen. The perf monitor samples the
// draw clock on frames numbered 0 mod 16; a plain n % 16 == 8 would never meet one, and this phase slips by one every 16 frames, so
// over 256 frames each phase is verified once and a sampled frame is met by chance. A hit is verified only off a sampled frame
// (perfMonitorSampleDraws): the step clock measures those frames and must see the real cost.
inline bool verifyFrame(uint64_t n) { return (n + n / 16) % 16 == 8; }
inline bool verifyNow(uint64_t n, bool sampledFrame) { return !sampledFrame && verifyFrame(n); }

struct Counters {
    uint64_t calls = 0;         // owner eye draws that asked the family decision while the layer was live
    uint64_t hits = 0;          // answered "none" from the memo
    uint64_t missNothing = 0;   // ran in full: nothing stored, or the last full run was not a plain decline
    uint64_t missMoved = 0;     // ran in full: a plain decline was stored and the key had moved
    uint64_t verified = 0;      // hits that also ran the full decision
    uint64_t mismatches = 0;    // ...and the full decision named a family
    void reset() { *this = Counters(); }
};

// The window's line, written at the 1800-frame edge: about 600 characters, under the log line's 1150 at its worst (the rig pins it).
inline size_t formatLine(char* out, size_t cap, uint32_t endFrame, const Counters& c) {
    if (!out || cap == 0) return 0;
    const int n = std::snprintf(
        out, cap,
        "ui layer family cache: TEMPORARY (built 2026-10-27), 1800-frame window ending %u. Owner eye draws that asked the family decision: "
        "%llu; answered none from the memo: %llu; ran in full: %llu (nothing stored or the last run was not a plain decline %llu, the key "
        "moved %llu). Verification, on a rotating one frame in sixteen that is not a sampled frame: %llu hits also ran the full decision, "
        "%llu of them named a family. The memo itself is behaviour and has no key; this line and the verification are removed once the "
        "flight reads zero mismatches.",
        static_cast<unsigned>(endFrame), static_cast<unsigned long long>(c.calls), static_cast<unsigned long long>(c.hits),
        static_cast<unsigned long long>(c.missNothing + c.missMoved), static_cast<unsigned long long>(c.missNothing),
        static_cast<unsigned long long>(c.missMoved), static_cast<unsigned long long>(c.verified),
        static_cast<unsigned long long>(c.mismatches));
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    const size_t len = static_cast<size_t>(n);
    return len < cap ? len : cap - 1;
}

}  // namespace uifamilymemo
}  // namespace edvr

#pragma once
// The pure logic of ui_depth's eye-draw memo, built 2026-10-27 (docs\engine-render-performance-2026-09-19.md, "Desk design: the
// ui_depth eye-draw memo"). No Windows and no D3D here, so tools\aa_off_test drives every decision the draw path takes.
//
// WHAT THE MEMO IS. uiDepthOnEyeDraw and uiDepthHologramOnEyeDraw each decide, for every eye draw, whether the draw is theirs. About
// nine draws in ten repeat the previous draw's bindings (the 2026-10-27 binding-repeat flight), and almost all of those are declined
// by the cheapest test the classifier has: the family gate (eye draws) or the VS-hash lists (hologram). That decline depends on two
// things only: the bindings (one aggregate generation, binding_shadow.h bindingEyeKeyGeneration) and the classifier's own state (an
// epoch bumped at every write to its surface table, its lists and its view memo, ui_depth.cpp). The memo remembers the key at the
// last such decline; a call whose key is the same, after a decline of that kind, returns the same decline without redoing the work.
// The memo covers that decline ONLY. Every other exit (a family, a composite, a planet, a world screen, a decline that counts or
// logs) stores "not a plain decline", so the next call runs the whole classifier and its counts and notes stay exact.
//
// It has no config key and no switch: a fix that always helps. Flown 2026-10-27 at a settlement pad, 91% of eye draws hit in both
// classifiers; about 5M hits were each checked against a full run of the classifier, and none differed in a return value, an output
// or the exit; v:ui-depth fell from 0.47 to 0.26 ms a frame (docs\engine-render-performance-2026-09-19.md).
#include <cstdint>

namespace edvr {
namespace classifiermemo {

// What a stored answer was computed at: the bindings' aggregate generation and the classifier's epoch. Either moving makes the
// answer unknown. Both are plain counters that only ever move forward, so a recycled value cannot occur inside a session.
struct Key {
    uint32_t generation = 0;
    uint32_t epoch = 0;
};

// Which exit a full classifier run took. kPlainDecline is the one answer a key alone determines: the eye draws' family-gate return
// and the hologram's cheap declines. kOther is every other exit, including each that counts, logs or reads state outside the key.
enum class Exit : uint8_t { kOther = 0, kPlainDecline = 1 };

struct Memo {
    Key  key;
    Exit exit = Exit::kOther;
    bool stored = false;   // false until the first full run stores, and after invalidate()
};

// A call hits only when something is stored, both halves of its key equal the stored key, AND the stored exit was a plain decline.
inline bool hit(const Memo& m, const Key& now) {
    return m.stored && m.exit == Exit::kPlainDecline && m.key.generation == now.generation && m.key.epoch == now.epoch;
}

inline void store(Memo& m, const Key& key, Exit exit) {
    m.key = key;
    m.exit = exit;
    m.stored = true;
}

inline void invalidate(Memo& m) {
    m = Memo();
}

}  // namespace classifiermemo
}  // namespace edvr

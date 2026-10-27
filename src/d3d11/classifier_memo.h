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
// THE MEMO AND ITS VERIFICATION ARE DIFFERENT THINGS. The memo (Key, Memo, probe, store, invalidate) is behaviour: it has no key of
// its own and no switch, being a fix that always helps. Everything from Tally down (the verify frame, the tally, the two lines) is a
// TEMPORARY instrument that proves the memo's key misses no input: on a verify frame a hit ALSO runs the full classifier and compares
// every output it writes. It comes out when the flight shows no mismatch (the desk design's "The flight should show").
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>

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

// How a call found the memo. A miss has ONE cause, tested in this order, so the three counts add up to the misses and the figure the
// flight wants stays clean: kMissNoPlain is the call whose key matched but whose stored answer was not a plain decline (or that found
// nothing stored), which is exactly the repeats the memo does not cover; a call whose generation moved is counted there even if its
// epoch moved too.
enum class Probe : uint8_t { kHit = 0, kMissNoPlain, kMissGeneration, kMissEpoch, kCount };

inline Probe probe(const Memo& m, const Key& now) {
    if (!m.stored) return Probe::kMissNoPlain;
    if (m.key.generation != now.generation) return Probe::kMissGeneration;
    if (m.key.epoch != now.epoch) return Probe::kMissEpoch;
    return m.exit == Exit::kPlainDecline ? Probe::kHit : Probe::kMissNoPlain;
}

inline void store(Memo& m, const Key& key, Exit exit) {
    m.key = key;
    m.exit = exit;
    m.stored = true;
}

inline void invalidate(Memo& m) {
    m = Memo();
}

// Verify frames: one frame in 16 by a counter of ui_depth's own (advanced at its frame boundary), and never a draw-sample frame (the
// caller adds that test; perfMonitorSampleDraws). The step clock measures only sampled frames and must measure the memo's real
// cost, so a sampled frame never pays for a verification. The phase moves by one each 16 frames: the two counters advance together,
// so a fixed phase would either never meet a sampled frame or ALWAYS meet one, and in that case nothing would ever be verified. This
// way exactly one frame in each block of 16 verifies, and across 256 frames every residue of the sample cadence gets its turn.
constexpr uint32_t kVerifyEvery = 16;
constexpr uint32_t kVerifyPhase = 8;
inline bool verifyFrame(uint32_t counter) {
    return (counter + counter / kVerifyEvery) % kVerifyEvery == kVerifyPhase;
}

// The outputs of uiDepthOnEyeDraw that a hit must reproduce, in the order the line prints them. Bit i of a difference mask is
// output i.
enum EyeOutput : unsigned {
    kEyeReturn = 0, kEyeComposite, kEyePlanet, kEyePlanetSolar, kEyeWorldScreen, kEyeCorona, kEyeCoronaMotion, kEyeMode,
    kEyeWantRebind, kEyeWantMask, kEyeRebindEye, kEyeDrawEye, kEyeShader, kEyeMaskSlot, kEyeExit, kEyeOutputCount
};
// ...and of uiDepthHologramOnEyeDraw.
enum HoloOutput : unsigned { kHoloReturn = 0, kHoloEye, kHoloExit, kHoloOutputCount };

inline const char* eyeOutputName(unsigned i) {
    static const char* const names[kEyeOutputCount] = {
        "return", "composite", "planet", "solar planet", "world screen", "corona", "corona motion", "mode",
        "rebind", "mask", "rebind eye", "draw eye", "shader", "mask slot", "exit"};
    return i < kEyeOutputCount ? names[i] : "?";
}
inline const char* holoOutputName(unsigned i) {
    static const char* const names[kHoloOutputCount] = {"return", "eye", "exit"};
    return i < kHoloOutputCount ? names[i] : "?";
}

constexpr unsigned kMaxOutputs = 16;
static_assert(kEyeOutputCount <= kMaxOutputs && kHoloOutputCount <= kMaxOutputs, "the tally holds every output");
constexpr uint32_t kMaxNamed = 3;   // mismatches named in a log line each, per classifier, for the session

// One classifier's window: how its calls found the memo, how many hits were verified, and what differed.
struct Tally {
    uint32_t probes[static_cast<unsigned>(Probe::kCount)] = {};
    uint32_t verified = 0;     // hits that also ran the full classifier
    uint32_t mismatched = 0;   // ...of which differed in at least one output
    uint32_t byOutput[kMaxOutputs] = {};
    uint32_t named = 0;        // session total, kept across windows

    void noteProbe(Probe p) { ++probes[static_cast<unsigned>(p)]; }
    uint32_t hits() const { return probes[static_cast<unsigned>(Probe::kHit)]; }
    uint32_t missNoPlain() const { return probes[static_cast<unsigned>(Probe::kMissNoPlain)]; }
    uint32_t missGeneration() const { return probes[static_cast<unsigned>(Probe::kMissGeneration)]; }
    uint32_t missEpoch() const { return probes[static_cast<unsigned>(Probe::kMissEpoch)]; }
    // The calls past the on test: every one is a hit or a miss with one cause.
    uint32_t calls() const { return hits() + missNoPlain() + missGeneration() + missEpoch(); }

    // A verified hit with its difference mask (0 when every output matched). A hit differing in three outputs is one mismatched hit
    // and one count in each of the three outputs.
    void noteVerified(uint32_t differMask) {
        ++verified;
        if (!differMask) return;
        ++mismatched;
        for (unsigned i = 0; i < kMaxOutputs; ++i) {
            if (differMask & (1u << i)) ++byOutput[i];
        }
    }
    // True for the first kMaxNamed mismatches of the session: the caller writes a line for each.
    bool takeName() { return named < kMaxNamed ? (++named, true) : false; }
    // The window closes; the naming budget belongs to the session and stays.
    void resetWindow() {
        const uint32_t keep = named;
        *this = Tally();
        named = keep;
    }
};

namespace textutil {
// Appends to a fixed buffer: truncates, never overruns, always NUL-terminated.
struct Out {
    char*  buf;
    size_t cap;
    size_t len = 0;
    Out(char* b, size_t c) : buf(b), cap(c) {
        if (cap) buf[0] = '\0';
    }
    void add(const char* fmt, ...) {
        if (cap == 0 || len + 1 >= cap) return;
        va_list ap;
        va_start(ap, fmt);
        const int n = std::vsnprintf(buf + len, cap - len, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        const size_t room = cap - 1 - len;
        len += static_cast<size_t>(n) < room ? static_cast<size_t>(n) : room;
    }
};
// A figure printed as text: "-" when there is nothing to say, which a reader must never take for zero.
struct Fig {
    char s[16];
};
inline Fig count(bool known, uint32_t v) {
    Fig f;
    if (known) std::snprintf(f.s, sizeof(f.s), "%u", v);
    else std::snprintf(f.s, sizeof(f.s), "-");
    return f;
}
inline Fig rate(uint32_t part, uint32_t whole) {
    Fig f;
    if (whole) std::snprintf(f.s, sizeof(f.s), "%.1f%%", static_cast<double>(part) * 100.0 / static_cast<double>(whole));
    else std::snprintf(f.s, sizeof(f.s), "-");
    return f;
}
}  // namespace textutil

// One classifier's part of the window line. The mismatch figures are "-" while no hit was verified: a count of zero from a check that
// never ran is not a clean result.
inline void formatPart(textutil::Out& o, const char* what, const Tally& t, unsigned outputs, const char* (*name)(unsigned)) {
    const bool checked = t.verified != 0;
    o.add("%s: %u calls past the on test, %u hits (%s), misses: no plain decline stored %u, generation moved %u, epoch moved %u; "
          "%u hits verified, mismatches %s (",
          what, t.calls(), t.hits(), textutil::rate(t.hits(), t.calls()).s, t.missNoPlain(), t.missGeneration(), t.missEpoch(),
          t.verified, textutil::count(checked, t.mismatched).s);
    for (unsigned i = 0; i < outputs; ++i) {
        o.add("%s %s%s", name(i), textutil::count(checked, t.byOutput[i]).s, i + 1 < outputs ? ", " : ")");
    }
}

// The one "ui depth memo:" line a window closes with. The log line holds about 1170 characters (Log::note's 1200 less its stamp and
// truncation mark); at its worst this one stays under 1150, which tools\aa_off_test holds.
inline size_t formatWindowLine(char* out, size_t cap, uint32_t endFrame, const Tally& eye, const Tally& holo) {
    textutil::Out o(out, cap);
    o.add("ui depth memo: 1800-frame window ending %u. A hit skips the classifier and returns its plain decline. On one frame in %u, "
          "never a draw-sample frame, a hit also runs the full classifier and compares every output (a temporary check). ",
          endFrame, kVerifyEvery);
    formatPart(o, "Eye draws", eye, kEyeOutputCount, eyeOutputName);
    o.add("; ");
    formatPart(o, "hologram draws", holo, kHoloOutputCount, holoOutputName);
    o.add(". A dash means nothing to report, not zero.");
    return o.len;
}

// A mismatch, named: the first kMaxNamed of each classifier get a line of their own, with the vertex shader and what differed.
inline size_t formatMismatchLine(char* out, size_t cap, const char* what, unsigned ordinal, uint64_t vsHash, uint32_t differMask,
                                 unsigned outputs, const char* (*name)(unsigned)) {
    textutil::Out o(out, cap);
    o.add("ui depth memo mismatch %u of %u named (%s): a hit and the full classifier differed for vs %016llX in:", ordinal, kMaxNamed,
          what, static_cast<unsigned long long>(vsHash));
    bool any = false;
    for (unsigned i = 0; i < outputs && i < kMaxOutputs; ++i) {
        if (!(differMask & (1u << i))) continue;
        o.add("%s %s", any ? "," : "", name(i));
        any = true;
    }
    o.add(any ? ". The full classifier's answer stood." : " nothing named.");
    return o.len;
}

}  // namespace classifiermemo
}  // namespace edvr

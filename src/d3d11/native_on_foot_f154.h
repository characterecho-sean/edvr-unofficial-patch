// F154/F156 (TEMPORARY instrument, native on-foot VR): find the coat's pack among the local body's draws, one kind of draw hidden at a time. Header only, no game or D3D11 state, so
// native_frame_test runs the very functions the draw thread uses.
//
// The pilot's coat outfit draws a backpack and an antenna that come into the eye with the head turned to the left shoulder and with the kinetic AR in the sights. They belong to
// EVASuit_Gear_Back, the part the coat needs for its torso, so the part itself cannot be hidden. Every draw of the local body into an eye is listed here, in the order it first
// appears; the dial (experimental.native_on_foot_body_draw_hide) leaves one listed draw out of both eyes, and the pilot steps the number until the pack goes.
//
// The local body is the draws with stencil ref 21 (F18: other people draw the same meshes with another ref) into an eye render target, with any shader. A draw is told apart by the
// vertex and pixel shaders bound, the index count and the instance count. Its index buffer, start index and base vertex are not part of that: the same mesh comes back every frame
// with a new start index and base vertex (the game moves its meshes about a pooled index buffer), so those keep changing and F154's identity, which held them, gave one mesh a new
// number in almost every frame. They are kept as variants instead: the first draw's three values are recorded with the entry, and the entry counts how many different ones were merged
// into it during the pass (up to kVariantCap; past that it says so). The list is numbered by first sight and never renumbered within a pass; it holds kCap entries and a draw of
// another identity past that is neither listed nor hidden, and the identities turned away are counted. A pass begins when the dial is set from -1 to a number (reset() empties the
// list, so the new pass starts with what is drawn now). One writer (the draw thread, one draw at a time; reset() runs with the writer excluded); the frame thread reads what was
// published.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f154 {

constexpr int kOff = -1;                       // the dial's value for "hide nothing"
constexpr int kCap = 128;                      // identities listed; the dial runs kOff..kCap-1
constexpr uint32_t kStencilRef = 21;           // the local player's body
constexpr uint32_t kSlots = 256;               // the lookup table: a power of two, twice the cap, so a probe always reaches an empty slot
static_assert((kSlots & (kSlots - 1)) == 0 && kSlots >= 2u * kCap, "the table is a power of two with room to spare");
static_assert(kCap < 255, "a slot holds the entry number plus one in a byte");

// Per entry, the different (index buffer, start index, base vertex) told apart: kVariantCap of them in a row of kVariantSlots fingerprints (half empty, so a probe always ends).
constexpr uint32_t kVariantSlots = 128;
constexpr uint32_t kVariantCap = 64;
// Across the list, the different identities turned away once it was full: kTurnedCap of them in kTurnedSlots fingerprints.
constexpr uint32_t kTurnedSlots = 1024;
constexpr uint32_t kTurnedCap = 768;

struct Identity {
    uint64_t vs = 0, ps = 0;   // the bound shaders' content hashes
    uint32_t count = 0;        // index count per instance
    uint32_t instances = 0;    // instance count
};

// Where one draw reads its geometry from: this changes from frame to frame for the same mesh, so it is not part of the identity.
struct Variant {
    uint64_t ib = 0;           // the bound index buffer, as an identity only
    uint32_t start = 0;        // StartIndexLocation
    int32_t base = 0;          // BaseVertexLocation
};

inline bool sameIdentity(const Identity& a, const Identity& b) noexcept {
    return a.vs == b.vs && a.ps == b.ps && a.count == b.count && a.instances == b.instances;
}

inline uint64_t identityHash(const Identity& id) noexcept {
    uint64_t h = id.vs * 0x9E3779B97F4A7C15ull;
    h = (h ^ id.ps) * 0xC2B2AE3D27D4EB4Full;
    h = (h ^ ((static_cast<uint64_t>(id.count) << 32) | id.instances)) * 0x165667B19E3779F9ull;
    return h ^ (h >> 32);
}

// A nonzero 64-bit fingerprint of a variant, and of an identity: what the sets below hold.
inline uint64_t variantFingerprint(const Variant& v) noexcept {
    uint64_t h = v.ib * 0x9E3779B97F4A7C15ull;
    h = (h ^ ((static_cast<uint64_t>(v.start) << 32) | static_cast<uint32_t>(v.base))) * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 29;
    h *= 0x165667B19E3779F9ull;
    h ^= h >> 32;
    return h ? h : 1ull;
}
inline uint64_t identityFingerprint(const Identity& id) noexcept {
    const uint64_t h = identityHash(id);
    return h ? h : 1ull;
}

// A set of nonzero fingerprints in a row of Slots (a power of two), holding at most Limit (below Slots, so a probe always reaches an empty slot). `used` is how many the row holds;
// the caller keeps that count. kFull: the fingerprint is not in the row and the row has no room for it.
enum class Seen { kPresent, kAdded, kFull };
template <uint32_t Limit, uint32_t Slots>
inline Seen addFingerprint(uint64_t (&row)[Slots], uint32_t used, uint64_t fp) noexcept {
    static_assert((Slots & (Slots - 1)) == 0 && Limit < Slots, "a power of two with at least one empty slot");
    uint32_t at = static_cast<uint32_t>(fp) & (Slots - 1);
    for (;;) {
        const uint64_t s = row[at];
        if (s == fp) return Seen::kPresent;
        if (s == 0) break;
        at = (at + 1) & (Slots - 1);
    }
    if (used >= Limit) return Seen::kFull;
    row[at] = fp;
    return Seen::kAdded;
}

// The eyes a draw was seen in. An eye the target's table could not name counts as unknown (the draw is still listed and still hidden).
constexpr uint8_t kEye0 = 1, kEye1 = 2, kEyeUnknown = 4;
inline uint8_t eyeBit(int eye) noexcept { return eye == 0 ? kEye0 : eye == 1 ? kEye1 : kEyeUnknown; }
inline int eyeSlot(int eye) noexcept { return eye == 0 ? 0 : eye == 1 ? 1 : 2; }
inline const char* eyeText(uint8_t mask) noexcept {
    const uint8_t known = static_cast<uint8_t>(mask & (kEye0 | kEye1));
    return known == (kEye0 | kEye1) ? "both" : known == kEye0 ? "0" : known == kEye1 ? "1" : "unknown";
}

// The local body: stencil ref 21 into an eye render target, with any shader. Both are required: another person's body has another ref, and the same mesh drawn into a shadow map or
// an offscreen target is not what the eye shows.
inline bool listable(uint32_t stencilRef, bool eyeTarget) noexcept { return eyeTarget && stencilRef == kStencilRef; }

// The draw numbered `index` is the dial's, and the draw is the local body's in an eye: it is left out. The dial is kOff or a listed number; a draw that was not listed (index < 0,
// the list full) is never hidden.
inline bool hides(int dial, int index, uint32_t stencilRef, bool eyeTarget) noexcept {
    return dial >= 0 && dial < kCap && index == dial && listable(stencilRef, eyeTarget);
}

// The dial names a draw that is in the list of `listed` draws.
inline bool inList(int dial, int listed) noexcept { return dial >= 0 && dial < listed && dial < kCap; }

// A pass begins when the dial is set from kOff to a number (the list is emptied and numbered from 0 again); moving between numbers, or from a number to kOff, does not begin one.
inline bool beginsPass(int previousDial, int dial) noexcept { return previousDial < 0 && dial >= 0; }

struct Entry {
    Identity id;                          // fixed once published
    Variant first;                        // the first draw's index buffer, start index and base vertex (fixed once published)
    uint32_t firstFrame = 0;              // the frame stamp at first sight (fixed once published)
    std::atomic<uint8_t> eyes{0};         // kEye0 | kEye1 | kEyeUnknown: where it was seen
    std::atomic<uint32_t> variants;       // the different (index buffer, start index, base vertex) merged into it, at most kVariantCap
    std::atomic<uint32_t> variantsLost;   // draws whose variant was not in the row and found it full (the entry has more than variants)
    std::atomic<uint32_t> skipped[3];     // draws left out since the frame thread last took them: eye 0, eye 1, unknown
    std::atomic<uint32_t> skipFrames;     // frames with at least one of them, likewise
    std::atomic<uint32_t> lastSkipFrame;  // the writer's: the last frame a skip was counted in
    std::atomic<uint32_t> drawnFrames[3]; // frames with at least one draw of it in eye 0, eye 1, unknown since the pass began
    std::atomic<uint32_t> lastDrawnFrame[3];   // the writer's: the last frame counted in each

    Entry() noexcept { clearCounters(); }

    void clearCounters() noexcept {
        for (int k = 0; k < 3; ++k) {
            skipped[k].store(0, std::memory_order_relaxed);
            drawnFrames[k].store(0, std::memory_order_relaxed);
            lastDrawnFrame[k].store(~0u, std::memory_order_relaxed);
        }
        skipFrames.store(0, std::memory_order_relaxed);
        lastSkipFrame.store(~0u, std::memory_order_relaxed);
        variants.store(0, std::memory_order_relaxed);
        variantsLost.store(0, std::memory_order_relaxed);
    }

    void clear() noexcept {
        id = Identity{};
        first = Variant{};
        firstFrame = 0;
        eyes.store(0, std::memory_order_relaxed);
        clearCounters();
    }
};

class DrawList {
public:
    DrawList() noexcept { std::memset(turned_, 0, sizeof(turned_)); }

    struct Note {
        int index;          // 0..kCap-1, or -1: the list is full and this identity is not in it
        bool added;         // this call put the identity in the list
        bool newVariant;    // this call merged a variant the entry had not seen (always true when added)
    };

    // The draw thread (one at a time). The identity's number: its place if listed, else the next free number (kCap identities at most; the numbers never change within a pass).
    // `v` is where this draw reads its geometry from, counted as a variant of the entry. `frame` is the stamp recorded at first sight and the one each eye's frames are counted by.
    Note note(const Identity& id, const Variant& v, int eye, uint32_t frame) noexcept {
        uint32_t at = static_cast<uint32_t>(identityHash(id)) & (kSlots - 1);
        for (;;) {
            const uint8_t s = slot_[at];
            if (s == 0) break;
            Entry& e = entries_[s - 1];
            if (sameIdentity(e.id, id)) {
                const uint8_t bit = eyeBit(eye);
                if (!(e.eyes.load(std::memory_order_relaxed) & bit)) e.eyes.fetch_or(bit, std::memory_order_relaxed);
                countDrawn(e, eye, frame);
                const bool fresh = countVariant(s - 1, v);
                return Note{s - 1, false, fresh};
            }
            at = (at + 1) & (kSlots - 1);
        }
        const int n = published_.load(std::memory_order_relaxed);
        if (n >= kCap) {
            refused_.fetch_add(1, std::memory_order_relaxed);
            switch (addFingerprint<kTurnedCap>(turned_, turnedIds_.load(std::memory_order_relaxed), identityFingerprint(id))) {
            case Seen::kAdded: turnedIds_.fetch_add(1, std::memory_order_relaxed); break;
            case Seen::kFull: turnedLost_.fetch_add(1, std::memory_order_relaxed); break;
            case Seen::kPresent: break;
            }
            return Note{-1, false, false};
        }
        Entry& e = entries_[n];
        e.id = id;
        e.first = v;
        e.firstFrame = frame;
        e.eyes.store(eyeBit(eye), std::memory_order_relaxed);
        countDrawn(e, eye, frame);
        std::memset(variantRow_[n], 0, sizeof(variantRow_[n]));
        countVariant(n, v);
        slot_[at] = static_cast<uint8_t>(n + 1);
        published_.store(n + 1, std::memory_order_release);
        return Note{n, true, true};
    }

    // Any thread: the number of identities listed; the draws past the cap that were turned away; how many different identities those were (a lower bound once turnedLost() is
    // above 0: the draws of identities the table of kTurnedCap could not tell from ones it had).
    int count() const noexcept { return published_.load(std::memory_order_acquire); }
    uint32_t refused() const noexcept { return refused_.load(std::memory_order_relaxed); }
    uint32_t turnedIdentities() const noexcept { return turnedIds_.load(std::memory_order_relaxed); }
    uint32_t turnedLost() const noexcept { return turnedLost_.load(std::memory_order_relaxed); }
    // Any thread, for an index below count().
    Entry& at(int i) noexcept { return entries_[i]; }
    const Entry& at(int i) const noexcept { return entries_[i]; }

    // The draw thread's: one draw of the numbered entry left out, in the eye's counter, and its frame counted once.
    void countSkip(int index, int eye, uint32_t frame) noexcept {
        Entry& e = entries_[index];
        e.skipped[eyeSlot(eye)].fetch_add(1, std::memory_order_relaxed);
        if (e.lastSkipFrame.load(std::memory_order_relaxed) != frame) {
            e.lastSkipFrame.store(frame, std::memory_order_relaxed);
            e.skipFrames.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // A new pass: the list is empty and the numbers start again at 0. Only with the writer excluded (the caller holds the draw thread's flag): the table, the entries and the
    // counts are rewritten. An entry's variant row is emptied when the entry is made, not here.
    void reset() noexcept {
        const int n = published_.load(std::memory_order_relaxed);
        published_.store(0, std::memory_order_release);
        for (int i = 0; i < n; ++i) entries_[i].clear();
        std::memset(slot_, 0, sizeof(slot_));
        std::memset(turned_, 0, sizeof(turned_));
        refused_.store(0, std::memory_order_relaxed);
        turnedIds_.store(0, std::memory_order_relaxed);
        turnedLost_.store(0, std::memory_order_relaxed);
    }

private:
    // The writer's: a frame in which the entry was drawn in the eye is counted once, however many of its draws the frame held.
    static void countDrawn(Entry& e, int eye, uint32_t frame) noexcept {
        const int k = eyeSlot(eye);
        if (e.lastDrawnFrame[k].load(std::memory_order_relaxed) == frame) return;
        e.lastDrawnFrame[k].store(frame, std::memory_order_relaxed);
        e.drawnFrames[k].fetch_add(1, std::memory_order_relaxed);
    }

    // The writer's: the variant is counted in the entry's row if it is new and there is room; true when it was new and counted.
    bool countVariant(int index, const Variant& v) noexcept {
        Entry& e = entries_[index];
        switch (addFingerprint<kVariantCap>(variantRow_[index], e.variants.load(std::memory_order_relaxed), variantFingerprint(v))) {
        case Seen::kAdded: e.variants.fetch_add(1, std::memory_order_relaxed); return true;
        case Seen::kFull: e.variantsLost.fetch_add(1, std::memory_order_relaxed); return false;
        case Seen::kPresent: break;
        }
        return false;
    }

    Entry entries_[kCap];
    uint8_t slot_[kSlots]{};   // entry number + 1; 0 is empty. The writer's.
    uint64_t variantRow_[kCap][kVariantSlots];   // each entry's variant fingerprints, the writer's; a row is emptied when its entry is made
    uint64_t turned_[kTurnedSlots];              // the fingerprints of the identities turned away, the writer's; emptied by the constructor and by reset()
    std::atomic<int> published_{0};
    std::atomic<uint32_t> refused_{0};
    std::atomic<uint32_t> turnedIds_{0};
    std::atomic<uint32_t> turnedLost_{0};
};

}  // namespace f154
}  // namespace nofoot
}  // namespace edvr

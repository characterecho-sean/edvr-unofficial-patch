// F154/F155 (TEMPORARY instrument, native on-foot VR): find the coat's pack among the draws that carry the game's vertex clip modifier, one draw hidden at a time. Header only, no
// game or D3D11 state, so native_frame_test runs the very functions the draw thread uses.
//
// The pilot's coat outfit draws a backpack and an antenna that come into the eye with the head turned to the left shoulder and with the kinetic AR in the sights. They belong to
// EVASuit_Gear_Back, the part the coat needs for its torso, so the part itself cannot be hidden. The body parts that carry the vertex clip modifier (pVertexClipAmount, which the
// Gear_Back has) are drawn with the clip pixel shader; every eye draw with that shader is listed here by its identity, in the order it first appears, and the dial
// (experimental.native_on_foot_body_draw_hide) leaves one listed draw out of both eyes; the pilot steps the number until the pack goes.
//
// A draw is listed when it uses the clip pixel shader (kClipPs) and goes into an eye render target: any vertex shader and any stencil ref (F154 asked for stencil ref 21, the local
// body, and listed far more than the body's own draws: its first 64 numbers were all spent before the clip shader's draws were reached). A draw is told apart by the shaders bound,
// the index buffer, the start index, the index count and the base vertex; the stencil ref is recorded with it. The list is numbered by first sight and never renumbered within a
// pass; it holds kCap draws and a draw past that is neither listed nor hidden. A pass begins when the dial is set from -1 to a number (reset() empties the list, so the new pass
// starts with what is drawn now). One writer (the draw thread, one draw at a time; reset() runs with the writer excluded); the frame thread reads what was published.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f154 {

constexpr int kOff = -1;                       // the dial's value for "hide nothing"
constexpr int kCap = 128;                      // draws listed; the dial runs kOff..kCap-1
constexpr uint64_t kClipPs = 0x0DF03E64DF9DBEF1ull;   // the pixel shader of the body parts that carry the vertex clip modifier
constexpr uint32_t kSlots = 256;               // the lookup table: a power of two, twice the cap, so a probe always reaches an empty slot
static_assert((kSlots & (kSlots - 1)) == 0 && kSlots >= 2u * kCap, "the table is a power of two with room to spare");
static_assert(kCap < 255, "a slot holds the entry number plus one in a byte");

struct Identity {
    uint64_t vs = 0, ps = 0;   // the bound shaders' content hashes
    uint64_t ib = 0;           // the bound index buffer, as an identity only
    uint32_t start = 0;        // StartIndexLocation
    uint32_t count = 0;        // index count per instance
    int32_t base = 0;          // BaseVertexLocation
};

inline bool sameIdentity(const Identity& a, const Identity& b) noexcept {
    return a.vs == b.vs && a.ps == b.ps && a.ib == b.ib && a.start == b.start && a.count == b.count && a.base == b.base;
}

inline uint64_t identityHash(const Identity& id) noexcept {
    uint64_t h = id.vs * 0x9E3779B97F4A7C15ull;
    h = (h ^ id.ps) * 0xC2B2AE3D27D4EB4Full;
    h = (h ^ id.ib) * 0x165667B19E3779F9ull;
    h = (h ^ ((static_cast<uint64_t>(id.start) << 32) | id.count)) * 0x9E3779B97F4A7C15ull;
    h = (h ^ static_cast<uint32_t>(id.base)) * 0xC2B2AE3D27D4EB4Full;
    return h ^ (h >> 32);
}

// The eyes a draw was seen in. An eye the target's table could not name counts as unknown (the draw is still listed and still hidden).
constexpr uint8_t kEye0 = 1, kEye1 = 2, kEyeUnknown = 4;
inline uint8_t eyeBit(int eye) noexcept { return eye == 0 ? kEye0 : eye == 1 ? kEye1 : kEyeUnknown; }
inline int eyeSlot(int eye) noexcept { return eye == 0 ? 0 : eye == 1 ? 1 : 2; }
inline const char* eyeText(uint8_t mask) noexcept {
    const uint8_t known = static_cast<uint8_t>(mask & (kEye0 | kEye1));
    return known == (kEye0 | kEye1) ? "both" : known == kEye0 ? "0" : known == kEye1 ? "1" : "unknown";
}

// A draw that is listed: the clip pixel shader into an eye render target. The stencil ref and the vertex shader are not asked: the pixel shader is what the clip modifier's parts have
// in common, and the same mesh drawn into a shadow map or an offscreen target is not what the eye shows.
inline bool listable(uint64_t ps, bool eyeTarget) noexcept { return eyeTarget && ps == kClipPs; }

// The draw numbered `index` is the dial's, and the draw is a listable one: it is left out. The dial is kOff or a listed number; a draw that was not listed (index < 0, the list
// full) is never hidden.
inline bool hides(int dial, int index, uint64_t ps, bool eyeTarget) noexcept {
    return dial >= 0 && dial < kCap && index == dial && listable(ps, eyeTarget);
}

// The dial names a draw that is in the list of `listed` draws.
inline bool inList(int dial, int listed) noexcept { return dial >= 0 && dial < listed && dial < kCap; }

// A pass begins when the dial is set from kOff to a number (the list is emptied and numbered from 0 again); moving between numbers, or from a number to kOff, does not begin one.
inline bool beginsPass(int previousDial, int dial) noexcept { return previousDial < 0 && dial >= 0; }

struct Entry {
    Identity id;                          // fixed once published
    uint32_t instances = 0;               // the instance count at first sight (fixed once published)
    uint32_t firstFrame = 0;              // the frame stamp at first sight (fixed once published)
    uint32_t stencil = 0;                 // the stencil ref at first sight (fixed once published)
    std::atomic<uint8_t> eyes{0};         // kEye0 | kEye1 | kEyeUnknown: where it was seen
    std::atomic<bool> stencilVaries{false};   // a later draw of the same identity had another stencil ref
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
    }

    void clear() noexcept {
        id = Identity{};
        instances = firstFrame = stencil = 0;
        eyes.store(0, std::memory_order_relaxed);
        stencilVaries.store(false, std::memory_order_relaxed);
        clearCounters();
    }
};

class DrawList {
public:
    struct Note {
        int index;    // 0..kCap-1, or -1: the list is full and this identity is not in it
        bool added;   // this call put it in the list
    };

    // The draw thread (one at a time). The identity's number: its place if listed, else the next free number (kCap draws at most; the numbers never change within a pass). `frame` is
    // the stamp recorded at first sight and the one each eye's frames are counted by; `stencil` is the stencil ref of this draw.
    Note note(const Identity& id, uint32_t instances, int eye, uint32_t frame, uint32_t stencil) noexcept {
        uint32_t at = static_cast<uint32_t>(identityHash(id)) & (kSlots - 1);
        for (;;) {
            const uint8_t s = slot_[at];
            if (s == 0) break;
            Entry& e = entries_[s - 1];
            if (sameIdentity(e.id, id)) {
                const uint8_t bit = eyeBit(eye);
                if (!(e.eyes.load(std::memory_order_relaxed) & bit)) e.eyes.fetch_or(bit, std::memory_order_relaxed);
                if (e.stencil != stencil && !e.stencilVaries.load(std::memory_order_relaxed)) e.stencilVaries.store(true, std::memory_order_relaxed);
                countDrawn(e, eye, frame);
                return Note{s - 1, false};
            }
            at = (at + 1) & (kSlots - 1);
        }
        const int n = published_.load(std::memory_order_relaxed);
        if (n >= kCap) {
            refused_.fetch_add(1, std::memory_order_relaxed);
            return Note{-1, false};
        }
        Entry& e = entries_[n];
        e.id = id;
        e.instances = instances;
        e.firstFrame = frame;
        e.stencil = stencil;
        e.eyes.store(eyeBit(eye), std::memory_order_relaxed);
        countDrawn(e, eye, frame);
        slot_[at] = static_cast<uint8_t>(n + 1);
        published_.store(n + 1, std::memory_order_release);
        return Note{n, true};
    }

    // Any thread: the number of draws listed, and the ones past the cap that were turned away (draws, not distinct identities).
    int count() const noexcept { return published_.load(std::memory_order_acquire); }
    uint32_t refused() const noexcept { return refused_.load(std::memory_order_relaxed); }
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
    // counts are rewritten.
    void reset() noexcept {
        const int n = published_.load(std::memory_order_relaxed);
        published_.store(0, std::memory_order_release);
        for (int i = 0; i < n; ++i) entries_[i].clear();
        std::memset(slot_, 0, sizeof(slot_));
        refused_.store(0, std::memory_order_relaxed);
    }

private:
    // The writer's: a frame in which the entry was drawn in the eye is counted once, however many of its draws the frame held.
    static void countDrawn(Entry& e, int eye, uint32_t frame) noexcept {
        const int k = eyeSlot(eye);
        if (e.lastDrawnFrame[k].load(std::memory_order_relaxed) == frame) return;
        e.lastDrawnFrame[k].store(frame, std::memory_order_relaxed);
        e.drawnFrames[k].fetch_add(1, std::memory_order_relaxed);
    }

    Entry entries_[kCap];
    uint8_t slot_[kSlots]{};   // entry number + 1; 0 is empty. The writer's.
    std::atomic<int> published_{0};
    std::atomic<uint32_t> refused_{0};
};

}  // namespace f154
}  // namespace nofoot
}  // namespace edvr

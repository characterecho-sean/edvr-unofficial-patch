// F158 (MEASURE ONLY, native on-foot VR): the local body's draws over the first 30 s on foot. Header only, no game or D3D11 state, so native_frame_test runs the very functions the draw
// thread and the frame thread use.
//
// The coat outfit's pack was still seen for the first seconds after spawning, although F157 leaves its full-detail draw (30312 indices) out of both eyes from the first second it is armed.
// In flight edvr_gfx_20261028_113409 native on foot went live at 11:36:00, the skip was armed from 11:36:01, a rifle was drawn at 11:36:04 and the first skip came at 11:36:07. So the game
// drew the pack another way at first, most likely a lower-detail mesh while the full-detail meshes stream in. The full-detail pack is two adjacent index ranges in one index buffer
// (3474 indices, then 30312). This window lists every kind of draw the local body makes in the first 30 s, with the times it was drawn, so the lower-detail mesh shows as a kind that
// stops about when the full-detail pack starts.
//
// A window opens at each rising edge of native on foot being active and at each change of F144's Gear_Back decision to the outer garment while it is active, and runs 30 s or until
// native on foot ends. One window runs at a time: a trigger that comes while one runs is counted and opens nothing. While it runs, every indexed draw into an eye-sized target with stencil
// ref 21 (the local player's body; any shader) is listed by its identity (the vertex shader, the pixel shader, the index count and the instance count). Per identity the list keeps the
// times (ms from the window's start) of its first and last frame, the frames it was drawn in each eye, the first draw's index buffer, start index and base vertex, how many different
// ones were merged into it, and the previous indexed draw into the same eye (its index buffer, start index and index count) at first sight. The pack's identity is F157's. Its first draw
// in the window is the switch time. The draws are counted as the game issues them, before F157's skip. The list holds kCap identities and counts the rest.
//
// Nothing is written while a window runs; the whole window is written once, when it ends (formatHeader, formatIdentity, formatPairs). The draw thread is the one writer (a draw at a
// time); the frame thread reads the list only with the writer excluded, so nothing here is atomic.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "native_on_foot_f157.h"

namespace edvr {
namespace nofoot {
namespace f158 {

constexpr uint32_t kWindowMs = 30000;        // a window's length
constexpr uint32_t kCap = 256;               // identities listed
constexpr uint32_t kSlots = 512;             // the lookup table: a power of two, twice the cap, so a probe always reaches an empty slot
constexpr uint32_t kStopMs = 500;            // an identity has stopped when its last frame was more than this before the window's end
constexpr uint32_t kNearMs = 1000;           // ... and stopped near the switch when its last frame was within this of the switch time
constexpr uint32_t kNever = 0xFFFFFFFFu;     // a time that never came
constexpr uint32_t kStencilRef = f157::kStencilRef;   // the local player's body
// Per identity, the different (index buffer, start index, base vertex) told apart: kVariantCap of them in a row of kVariantSlots fingerprints (half empty, so a probe always ends).
constexpr uint32_t kVariantSlots = 128;
constexpr uint32_t kVariantCap = 64;
// Past the cap, the different identities turned away: kTurnedCap of them in kTurnedSlots fingerprints.
constexpr uint32_t kTurnedSlots = 1024;
constexpr uint32_t kTurnedCap = 768;
static_assert((kSlots & (kSlots - 1)) == 0 && kSlots >= 2u * kCap, "the table is a power of two with room to spare");
static_assert(kNearMs == 1000u && kStopMs == 500u, "the pairs line's text says 1 s");

// ---- the window's rules (the frame thread) ------------------------------------------------------------------------------------------------------------------------------------------

enum class Trigger : uint8_t { kNone = 0, kNativeOnFoot = 1, kCoatOutfit = 2 };
enum class Ended : uint8_t { kNone = 0, kTime = 1, kNativeOnFoot = 2 };

inline const char* triggerText(Trigger t) noexcept {
    return t == Trigger::kNativeOnFoot ? "native on foot beginning" : t == Trigger::kCoatOutfit ? "the coat outfit decided" : "nothing";
}
inline const char* endedText(Ended e) noexcept { return e == Ended::kTime ? "the clock" : e == Ended::kNativeOnFoot ? "native on foot ending" : "nothing"; }

// The draw path records only while a window is open, and only an indexed draw into an eye-sized target; the identity is listed only for stencil ref 21 (any shader).
inline bool recordsDraw(bool windowOpen, bool indexed, bool eyeTarget) noexcept { return windowOpen && indexed && eyeTarget; }
inline bool listable(uint32_t stencilRef) noexcept { return stencilRef == kStencilRef; }

// When a window opens and closes. step() is called once a frame with the state of that frame: native on foot active, and F144's Gear_Back decided as the outer garment. A window opens
// at the rising edge of native on foot being active, and at a change of the decision to the outer garment while it is active (a rising edge with the decision already made is the one
// trigger, the edge). It closes after kWindowMs, or at once when native on foot ends. A trigger while a window runs opens nothing and is counted (the first trigger of a flapping decision
// is not worth a window of its own). A window that closes by the clock in the same step as a trigger lets the trigger open the next one.
class WindowRules {
public:
    struct Step {
        Ended closed = Ended::kNone;               // a window closed in this step, and why
        uint32_t closedMs = 0;                     // how long that window ran
        Trigger closedTrigger = Trigger::kNone;    // what opened it
        uint32_t closedIgnored = 0;                // the triggers that came while it ran
        Trigger opened = Trigger::kNone;           // a window opened in this step, and what opened it
    };

    Step step(bool nativeActive, bool coatDecided, uint64_t nowMs) noexcept {
        Step r;
        const bool coat = nativeActive && coatDecided;
        const Trigger t = nativeActive && !prevActive_ ? Trigger::kNativeOnFoot : coat && !prevCoat_ ? Trigger::kCoatOutfit : Trigger::kNone;
        prevActive_ = nativeActive;
        prevCoat_ = coat;
        if (open_) {
            const uint64_t ran = nowMs > startMs_ ? nowMs - startMs_ : 0;
            Ended why = Ended::kNone;
            if (!nativeActive) why = Ended::kNativeOnFoot;
            else if (ran >= kWindowMs) why = Ended::kTime;
            if (why != Ended::kNone) {
                r.closed = why;
                r.closedMs = static_cast<uint32_t>(ran);
                r.closedTrigger = trigger_;
                r.closedIgnored = ignored_;
                open_ = false;
            }
        }
        if (open_) {
            if (t != Trigger::kNone) ++ignored_;
            return r;
        }
        if (t != Trigger::kNone) {
            open_ = true;
            startMs_ = nowMs;
            trigger_ = t;
            ignored_ = 0;
            r.opened = t;
        }
        return r;
    }

    bool open() const noexcept { return open_; }
    uint64_t startMs() const noexcept { return startMs_; }

private:
    bool open_ = false;
    bool prevActive_ = false;   // native on foot was active in the last step
    bool prevCoat_ = false;     // ... and the decision was the outer garment
    uint64_t startMs_ = 0;
    Trigger trigger_ = Trigger::kNone;
    uint32_t ignored_ = 0;
};

// ---- what a draw is (the draw thread) -----------------------------------------------------------------------------------------------------------------------------------------------

struct Identity {
    uint64_t vs = 0, ps = 0;   // the bound shaders' content hashes
    uint32_t count = 0;        // index count per instance
    uint32_t instances = 0;    // instance count
};

// Where one draw reads its geometry from; it changes from frame to frame for the same mesh (the game moves its meshes about a pooled index buffer), so it is not part of the identity.
struct Variant {
    uint64_t ib = 0;           // the bound index buffer, as an identity only
    uint32_t start = 0;        // StartIndexLocation
    int32_t base = 0;          // BaseVertexLocation
};

// An indexed draw as the next one sees it: the index buffer, start index and index count.
struct Prev {
    bool valid = false;
    uint64_t ib = 0;
    uint32_t start = 0;
    uint32_t count = 0;
};

inline bool sameIdentity(const Identity& a, const Identity& b) noexcept { return a.vs == b.vs && a.ps == b.ps && a.count == b.count && a.instances == b.instances; }
inline bool isPack(const Identity& id) noexcept { return f157::packIdentity(id.vs, id.ps, id.count, id.instances); }

inline uint64_t identityHash(const Identity& id) noexcept {
    uint64_t h = id.vs * 0x9E3779B97F4A7C15ull;
    h = (h ^ id.ps) * 0xC2B2AE3D27D4EB4Full;
    h = (h ^ ((static_cast<uint64_t>(id.count) << 32) | id.instances)) * 0x165667B19E3779F9ull;
    return h ^ (h >> 32);
}
inline uint64_t identityFingerprint(const Identity& id) noexcept {
    const uint64_t h = identityHash(id);
    return h ? h : 1ull;
}
inline uint64_t variantFingerprint(const Variant& v) noexcept {
    uint64_t h = v.ib * 0x9E3779B97F4A7C15ull;
    h = (h ^ ((static_cast<uint64_t>(v.start) << 32) | static_cast<uint32_t>(v.base))) * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 29;
    h *= 0x165667B19E3779F9ull;
    h ^= h >> 32;
    return h ? h : 1ull;
}

// A set of nonzero fingerprints in a row of Slots (a power of two), holding at most Limit (below Slots, so a probe always reaches an empty slot). `used` is how many the row holds; the
// caller keeps that count. kFull: the fingerprint is not in the row and the row has no room for it.
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

struct Entry {
    Identity id;                                       // fixed once listed
    Variant first;                                     // the first draw's index buffer, start index and base vertex
    Prev prev;                                         // the previous indexed draw into the same eye, when this one was first seen
    uint32_t firstMs = 0, lastMs = 0;                  // ms from the window's start: the first and the last draw
    uint32_t frames[3] = {0, 0, 0};                    // frames with at least one draw in eye 0, eye 1 and an eye the target's table could not name
    uint32_t lastFrame[3] = {~0u, ~0u, ~0u};           // the last frame counted in each
    uint32_t variants = 0;                             // the different (index buffer, start index, base vertex) merged into it, at most kVariantCap
    uint32_t variantsLost = 0;                         // draws whose variant was not in the row and found it full (the entry has more than `variants`)
};

// A window's list. begin() empties it (with the writer excluded); note() is the draw thread's, one draw at a time.
class SpawnLog {
public:
    SpawnLog() noexcept { begin(0); }

    // A window opens: the list is empty, the times count from startMs.
    void begin(uint64_t startMs) noexcept {
        startMs_ = startMs;
        count_ = 0;
        switchMs_ = kNever;
        refused_ = turnedIds_ = turnedLost_ = 0;
        std::memset(slot_, 0, sizeof(slot_));
        std::memset(turned_, 0, sizeof(turned_));
        for (Prev& p : prev_) p = Prev{};
    }

    // The previous indexed draw into the eye (invalid for the eye's first), replaced by this one.
    Prev swapPrev(int eye, const Prev& now) noexcept {
        Prev& p = prev_[f157::eyeSlot(eye)];
        const Prev before = p;
        p = now;
        return before;
    }

    struct Note {
        int index;     // 0..kCap-1, or -1: the list is full and this identity is not in it
        bool added;    // this call put the identity in the list
    };

    // One draw of the local body (stencil ref 21) into an eye, at nowMs (the same clock as begin's). `v` is where it reads its geometry from; `prev` the previous indexed draw into
    // the eye (kept only when the identity is new); `frame` counts the frames per eye. The pack's first draw sets the switch time, even when the list is full.
    Note note(const Identity& id, const Variant& v, const Prev& prev, int eye, uint32_t frame, uint64_t nowMs) noexcept {
        const uint32_t ms = nowMs > startMs_ ? static_cast<uint32_t>(nowMs - startMs_) : 0u;
        if (switchMs_ == kNever && isPack(id)) switchMs_ = ms;
        uint32_t at = static_cast<uint32_t>(identityHash(id)) & (kSlots - 1);
        for (;;) {
            const uint16_t s = slot_[at];
            if (s == 0) break;
            Entry& e = entries_[s - 1];
            if (sameIdentity(e.id, id)) {
                if (ms > e.lastMs) e.lastMs = ms;
                countFrame(e, eye, frame);
                countVariant(static_cast<uint32_t>(s) - 1u, v);
                return Note{static_cast<int>(s) - 1, false};
            }
            at = (at + 1) & (kSlots - 1);
        }
        if (count_ >= kCap) {
            ++refused_;
            switch (addFingerprint<kTurnedCap>(turned_, turnedIds_, identityFingerprint(id))) {
            case Seen::kAdded: ++turnedIds_; break;
            case Seen::kFull: ++turnedLost_; break;
            case Seen::kPresent: break;
            }
            return Note{-1, false};
        }
        const uint32_t n = count_;
        Entry& e = entries_[n];
        e = Entry{};
        e.id = id;
        e.first = v;
        e.prev = prev;
        e.firstMs = e.lastMs = ms;
        countFrame(e, eye, frame);
        std::memset(variantRow_[n], 0, sizeof(variantRow_[n]));
        countVariant(n, v);
        slot_[at] = static_cast<uint16_t>(n + 1);
        count_ = n + 1;
        return Note{static_cast<int>(n), true};
    }

    uint32_t count() const noexcept { return count_; }
    const Entry& at(uint32_t i) const noexcept { return entries_[i]; }
    uint32_t switchMs() const noexcept { return switchMs_; }          // kNever: the pack was not drawn
    uint32_t refused() const noexcept { return refused_; }           // draws of identities past the cap
    uint32_t turnedIdentities() const noexcept { return turnedIds_; }   // how many different identities those were (a lower bound once turnedLost() is above 0)
    uint32_t turnedLost() const noexcept { return turnedLost_; }

private:
    static void countFrame(Entry& e, int eye, uint32_t frame) noexcept {
        const int k = f157::eyeSlot(eye);
        if (e.lastFrame[k] == frame) return;
        e.lastFrame[k] = frame;
        ++e.frames[k];
    }
    void countVariant(uint32_t index, const Variant& v) noexcept {
        Entry& e = entries_[index];
        switch (addFingerprint<kVariantCap>(variantRow_[index], e.variants, variantFingerprint(v))) {
        case Seen::kAdded: ++e.variants; break;
        case Seen::kFull: ++e.variantsLost; break;
        case Seen::kPresent: break;
        }
    }

    Entry entries_[kCap];
    uint16_t slot_[kSlots];                      // entry number + 1; 0 is empty
    uint64_t variantRow_[kCap][kVariantSlots];   // each entry's variant fingerprints; a row is emptied when its entry is made
    uint64_t turned_[kTurnedSlots];              // the fingerprints of the identities turned away
    Prev prev_[3];                               // the previous indexed draw into eye 0, eye 1 and an eye the target's table could not name
    uint64_t startMs_ = 0;
    uint32_t count_ = 0;
    uint32_t switchMs_ = kNever;
    uint32_t refused_ = 0, turnedIds_ = 0, turnedLost_ = 0;
};

// ---- reading a window (the frame thread, after the window ended) ----------------------------------------------------------------------------------------------------------------

// The window ended at endMs and the identity's last frame was more than kStopMs before it.
inline bool stopped(uint32_t lastMs, uint32_t endMs) noexcept { return endMs > lastMs && endMs - lastMs > kStopMs; }

// ... and its last frame was within kNearMs of the switch time (before or after it). Never when the pack was not drawn.
inline bool stoppedNearSwitch(uint32_t lastMs, uint32_t endMs, uint32_t switchMs) noexcept {
    if (switchMs == kNever || !stopped(lastMs, endMs)) return false;
    const uint32_t d = lastMs > switchMs ? lastMs - switchMs : switchMs - lastMs;
    return d <= kNearMs;
}

// Two identities are adjacent when their first draws read from one index buffer and the first one's range ends where the second one's begins: a's start + a's index count = b's start.
inline bool adjacent(const Entry& a, const Entry& b) noexcept {
    return a.first.ib != 0 && a.first.ib == b.first.ib && static_cast<uint64_t>(a.first.start) + a.id.count == b.first.start;
}

// The adjacent pairs (a, b) of a list, counted: how many, and how many of them have an identity that stopped near the switch.
struct PairCount {
    uint32_t total = 0;
    uint32_t marked = 0;
};
// Who of a pair stopped near the switch: bit 0 the first, bit 1 the second.
inline uint32_t pairMark(const Entry& a, const Entry& b, uint32_t endMs, uint32_t switchMs) noexcept {
    return (stoppedNearSwitch(a.lastMs, endMs, switchMs) ? 1u : 0u) | (stoppedNearSwitch(b.lastMs, endMs, switchMs) ? 2u : 0u);
}
inline PairCount countPairs(const SpawnLog& log, uint32_t endMs) noexcept {
    PairCount c;
    for (uint32_t i = 0; i < log.count(); ++i)
        for (uint32_t j = 0; j < log.count(); ++j) {
            if (i == j || !adjacent(log.at(i), log.at(j))) continue;
            ++c.total;
            if (pairMark(log.at(i), log.at(j), endMs, log.switchMs())) ++c.marked;
        }
    return c;
}

// ---- the log lines --------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Each is built into the caller's buffer and returns its length. The pack's text and the optional pieces are formats of their own, so the --health self-test renders them from here.

inline int clampLen(int n, size_t cap) noexcept { return n < 0 ? 0 : n >= static_cast<int>(cap) ? static_cast<int>(cap) - 1 : n; }

// The header: why the window opened, how long it ran and why it ended, the switch time, the identities listed and the overflow.
inline int formatHeader(char* out, size_t cap, uint32_t window, Trigger trigger, uint32_t ranMs, Ended ended, const SpawnLog& log, uint32_t ignored) noexcept {
    char pack[48];
    if (log.switchMs() == kNever) std::snprintf(pack, sizeof(pack), "not drawn in the window");
    else std::snprintf(pack, sizeof(pack), "first drawn at %u ms", static_cast<unsigned>(log.switchMs()));
    return clampLen(std::snprintf(out, cap,
                                  "body draws at spawn (F158) window %u: opened by %s, ran %u ms (ended by %s), the pack %s, %u identity(ies) listed, %u draw(s) of %u%s identity(ies) past the cap of %u "
                                  "turned away, %u later trigger(s) ignored, counted as the game issues them (before the pack's skip)",
                                  static_cast<unsigned>(window), triggerText(trigger), static_cast<unsigned>(ranMs), endedText(ended), pack, static_cast<unsigned>(log.count()),
                                  static_cast<unsigned>(log.refused()), static_cast<unsigned>(log.turnedIdentities()), log.turnedLost() ? "+" : "", static_cast<unsigned>(kCap),
                                  static_cast<unsigned>(ignored)),
                    cap);
}

// One identity, in first-seen order (n counts from 0): the shaders, count and instances, the first and last ms, the frames in each eye, the first variant and how many were merged, the
// previous draw at first sight, whether it is the pack, and `stopped at T ms` (T its last frame) when it stopped before the window's end.
inline int formatIdentity(char* out, size_t cap, uint32_t window, uint32_t n, const Entry& e, uint32_t endMs) noexcept {
    char unknown[40] = "";
    if (e.frames[2]) std::snprintf(unknown, sizeof(unknown), ", eye unknown %u", static_cast<unsigned>(e.frames[2]));
    char prev[88] = "none";
    if (e.prev.valid) std::snprintf(prev, sizeof(prev), "ib %llX start %u count %u", static_cast<unsigned long long>(e.prev.ib), static_cast<unsigned>(e.prev.start), static_cast<unsigned>(e.prev.count));
    char tail[64] = "";
    size_t used = 0;
    if (isPack(e.id)) used += static_cast<size_t>(clampLen(std::snprintf(tail, sizeof(tail), ", is the pack"), sizeof(tail)));
    if (stopped(e.lastMs, endMs)) std::snprintf(tail + used, sizeof(tail) - used, ", stopped at %u ms", static_cast<unsigned>(e.lastMs));
    return clampLen(std::snprintf(out, cap,
                                  "body draws at spawn (F158) window %u identity %u: vs %016llX ps %016llX count %u instances %u, first %u ms, last %u ms, frames eye 0 %u, eye 1 %u%s, "
                                  "first variant ib %llX start %u base %d, %u%s variant(s), previous draw %s%s",
                                  static_cast<unsigned>(window), static_cast<unsigned>(n), static_cast<unsigned long long>(e.id.vs), static_cast<unsigned long long>(e.id.ps),
                                  static_cast<unsigned>(e.id.count), static_cast<unsigned>(e.id.instances), static_cast<unsigned>(e.firstMs), static_cast<unsigned>(e.lastMs),
                                  static_cast<unsigned>(e.frames[0]), static_cast<unsigned>(e.frames[1]), unknown, static_cast<unsigned long long>(e.first.ib),
                                  static_cast<unsigned>(e.first.start), static_cast<int>(e.first.base), static_cast<unsigned>(e.variants), e.variantsLost ? "+" : "", prev, tail),
                    cap);
}

// The adjacent pairs, `a>b` (identity b's range begins where a's ends), with the identity that stopped within 1 s of the switch time marked (`a>b stopped:a`, `stopped:b` or
// `stopped:both`). The marked pairs come first; the line stops at its budget and says how many more there were.
inline int formatPairs(char* out, size_t cap, uint32_t window, const SpawnLog& log, uint32_t endMs) noexcept {
    const PairCount c = countPairs(log, endMs);
    char list[900];   // with the line's own text (about 125 characters) it fits the 1100-character buffer the caller gives, with room to spare
    size_t len = 0;
    list[0] = '\0';
    uint32_t written = 0;
    const size_t budget = sizeof(list) - 32;
    for (int pass = 0; pass < 2 && len < budget; ++pass) {   // pass 0: the marked pairs; pass 1: the rest
        for (uint32_t i = 0; i < log.count() && len < budget; ++i)
            for (uint32_t j = 0; j < log.count() && len < budget; ++j) {
                if (i == j || !adjacent(log.at(i), log.at(j))) continue;
                const uint32_t mark = pairMark(log.at(i), log.at(j), endMs, log.switchMs());
                if ((mark != 0) != (pass == 0)) continue;
                char one[48];
                if (!mark) std::snprintf(one, sizeof(one), "%u>%u", static_cast<unsigned>(i), static_cast<unsigned>(j));
                else std::snprintf(one, sizeof(one), "%u>%u stopped:%s", static_cast<unsigned>(i), static_cast<unsigned>(j), mark == 3u ? "both" : mark == 1u ? "a" : "b");
                len += static_cast<size_t>(clampLen(std::snprintf(list + len, sizeof(list) - len, "%s%s", written ? ", " : "", one), sizeof(list) - len));
                ++written;
            }
    }
    if (written < c.total) std::snprintf(list + len, sizeof(list) - len, ", +%u more", static_cast<unsigned>(c.total - written));
    return clampLen(std::snprintf(out, cap, "body draws at spawn (F158) window %u adjacent pairs: %u pair(s), %u with an identity stopped within 1 s of the switch time: %s",
                                  static_cast<unsigned>(window), static_cast<unsigned>(c.total), static_cast<unsigned>(c.marked), c.total ? list : "none"),
                    cap);
}

}  // namespace f158
}  // namespace nofoot
}  // namespace edvr

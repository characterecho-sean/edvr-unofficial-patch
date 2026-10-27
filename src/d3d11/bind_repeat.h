#pragma once
// Do consecutive draws repeat the bindings the per-draw classifiers read? (built 2026-10-27). A TEMPORARY instrument for the AA-on
// draw-hook increment (docs\engine-render-performance-2026-09-19.md, the 2026-10-27 entry).
//
// WHY. The step clock (draw_step_clock.h) found AA on's extra draw-hook CPU spread over per-draw classifiers (the interface depth, the
// census gate, engine velocity's pre-draw checks, the UI layer's family test, the screen motion's recognition, ...), each of which
// answers from what is bound. The lever that suggests itself is a cache keyed on the bindings: answer once, reuse while the bindings
// repeat. Before it is designed the evidence is needed, and it is three questions:
//   1. do consecutive draws repeat the bindings these classifiers read;
//   2. when they do, do the classifiers' answers actually repeat (would such a cache ever be WRONG);
//   3. how much of the step time lands on draws that repeat (what a cache could skip).
// This file answers 1 and 2. The step time by class (3) is draw_step_clock.h's, which takes the class from here.
//
// THE KEY. Eight slots: the vertex and pixel shaders, pixel shader resources 0-3, render target 0 and the depth view. For each, the
// slot's GENERATION and its POINTER, from the binding shadow (binding_shadow.h). The two are different questions: a generation bumps
// on EVERY rebind, even of the same pointer, and once a frame, so "no generation moved" is the strong claim (nothing was set) and
// "same pointers, a generation moved" is the game setting what was already there. Which of them a cache could key on is exactly what
// the counts below say.
//
// THE CLASSES, each draw against the previous one:
//   repeat    no generation of the 8 moved (and so no pointer)
//   rebound   every pointer is the same, but some generation moved
//   changed   some pointer differs
//   first     the first draw of a sampled frame (or, for the eye class, the first eye draw of one): nothing to compare with. Counted
//             apart, never as changed.
// Every draw on the owner context gets the class against the previous OWNER draw of any kind; an eye draw gets a second, against the
// previous EYE draw. Only sampled frames are captured, so the counts are per sampled frame.
//
// ANSWER STABILITY, on eye draws. After beginPanelOverride the draw's answer tuple is known (the verdict, and the three flags it sets
// for forwardWithVerdict: the interface depth, the composite and the hologram pass) and so is its argument tuple (kind, count,
// instances, start index, base vertex, start instance). For an eye draw classed repeat, and again for one classed rebound, the answer
// tuple is compared with the previous eye draw's: a mismatch is a draw a cache keyed on the bindings would have answered wrongly, and
// it is counted by component, with how many of those had the same arguments (so a key that adds the arguments would have been right).
//
// Header only and pure (no Windows, no Direct3D); the binding reads are a reader the caller supplies, so tools\aa_off_test drives
// every function with scripted bindings.

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace bindrepeat {

// ---- the key ------------------------------------------------------------------------------
constexpr unsigned kSlots = 8;
// Index order: Vs, Ps, PsSrv0-3, Rtv0, Dsv0 (vscreen.cpp's table names the BindSlots in this order).
inline const char* slotName(unsigned slot) {
    static const char* const kNames[kSlots] = {"vs", "ps", "srv0", "srv1", "srv2", "srv3", "rtv0", "dsv0"};
    return slot < kSlots ? kNames[slot] : "?";
}

struct Key {
    uint32_t  gen[kSlots];
    uintptr_t ptr[kSlots];
    uint32_t  frame;   // the frame the draw belongs to: a key from another frame is not a previous draw
};

// `reader` supplies gen(slotIndex) and ptr(slotIndex): the binding shadow in production, a script in the rig.
template <class Reader>
inline void capture(Key& key, uint32_t frame, const Reader& reader) {
    for (unsigned i = 0; i < kSlots; ++i) {
        key.gen[i] = reader.gen(i);
        key.ptr[i] = reader.ptr(i);
    }
    key.frame = frame;
}

// ---- the classes -------------------------------------------------------------------------------
enum Class : uint8_t { kRepeat = 0, kRebound, kChanged, kFirst, kClassCount };
// A steps-mode draw that has no class (not the owner context's) lands in one bucket past the classes.
constexpr uint8_t kUnclassified = kClassCount;
constexpr unsigned kBuckets = kClassCount + 1;
inline const char* className(unsigned c) {
    static const char* const kNames[kBuckets] = {"repeat", "rebound", "changed", "first", "unclassified"};
    return c < kBuckets ? kNames[c] : "?";
}

// Any pointer that differs is `changed`, whatever the generations did; otherwise a moved generation is `rebound`; otherwise `repeat`.
inline Class classify(const Key& prev, const Key& cur) {
    bool moved = false;
    for (unsigned i = 0; i < kSlots; ++i) {
        if (prev.ptr[i] != cur.ptr[i]) return kChanged;
        if (prev.gen[i] != cur.gen[i]) moved = true;
    }
    return moved ? kRebound : kRepeat;
}

// ---- the answers ---------------------------------------------------------------------------------
struct Answer {
    uint8_t verdict;   // the DrawVerdict beginPanelOverride returned
    bool    uiDepth;   // t_uiDepthThisDraw
    bool    composite; // t_compositeThisDraw
    bool    holo;      // t_holoDepthThisDraw
};
struct Args {
    char     kind;
    uint32_t count;
    uint32_t instances;
    uint32_t start;
    int32_t  base;
    uint32_t startInstance;
};
inline bool sameArgs(const Args& a, const Args& b) {
    return a.kind == b.kind && a.count == b.count && a.instances == b.instances && a.start == b.start && a.base == b.base &&
           a.startInstance == b.startInstance;
}

// ---- the tallies --------------------------------------------------------------------------------
// Eye draws of one eye class compared with the previous eye draw's answer.
struct Stability {
    uint64_t draws;              // eye draws of this class (each had a previous eye draw to compare with)
    uint64_t differing;          // ...whose answer tuple differed in some component
    uint64_t verdict;            // ...by component (a draw can differ in several)
    uint64_t uiDepth;
    uint64_t composite;
    uint64_t holo;
    uint64_t differingSameArgs;  // differing draws whose argument tuple was the previous eye draw's
};

struct Tally {
    uint64_t owner[kClassCount];     // owner draws by class against the previous owner draw
    uint64_t eye[kClassCount];       // eye draws by class against the previous eye draw
    uint64_t eyeCompared;            // eye draws that had a previous eye draw (all but first)
    uint64_t genMoved[kSlots];       // per slot, on those: the generation moved
    uint64_t ptrChanged[kSlots];     // ...the pointer changed
    Stability repeat;                // answer stability of the eye draws classed repeat
    Stability rebound;               // ...and of those classed rebound
    void reset() { std::memset(this, 0, sizeof(*this)); }
};

// The previous owner draw and the previous eye draw, and the tallies. Plain accumulation from the drawing thread (the owner context's
// draws), read and reset at the frame boundary: perfMonitorDrawTicks' arrangement.
struct Tracker {
    Key     prev;
    Key     prevEye;
    Answer  prevEyeAnswer;
    Args    prevEyeArgs;
    bool    havePrev;
    bool    havePrevEye;
    Tally   tally;

    // Everything, previous draws included (a static Tracker is zero already; a rig's local one is not).
    void clear() { std::memset(this, 0, sizeof(*this)); }

    struct Result {
        Class any;    // against the previous owner draw
        Class eye;    // against the previous eye draw (meaningful when isEye)
        bool  isEye;
    };

    // One owner-context draw, once its verdict is known. `cur` is the key captured at the thunk's entry.
    Result note(const Key& cur, bool isEye, const Answer& answer, const Args& args) {
        Result r;
        r.isEye = isEye;
        r.eye = kFirst;
        r.any = (havePrev && prev.frame == cur.frame) ? classify(prev, cur) : kFirst;
        prev = cur;
        havePrev = true;
        ++tally.owner[r.any];
        if (!isEye) return r;
        if (havePrevEye && prevEye.frame == cur.frame) {
            r.eye = classify(prevEye, cur);
            ++tally.eyeCompared;
            for (unsigned i = 0; i < kSlots; ++i) {
                if (prevEye.gen[i] != cur.gen[i]) ++tally.genMoved[i];
                if (prevEye.ptr[i] != cur.ptr[i]) ++tally.ptrChanged[i];
            }
            if (r.eye == kRepeat || r.eye == kRebound) {
                Stability& s = r.eye == kRepeat ? tally.repeat : tally.rebound;
                ++s.draws;
                const bool dv = prevEyeAnswer.verdict != answer.verdict;
                const bool du = prevEyeAnswer.uiDepth != answer.uiDepth;
                const bool dc = prevEyeAnswer.composite != answer.composite;
                const bool dh = prevEyeAnswer.holo != answer.holo;
                s.verdict += dv;
                s.uiDepth += du;
                s.composite += dc;
                s.holo += dh;
                if (dv || du || dc || dh) {
                    ++s.differing;
                    if (sameArgs(prevEyeArgs, args)) ++s.differingSameArgs;
                }
            }
        }
        ++tally.eye[r.eye];
        prevEye = cur;
        prevEyeAnswer = answer;
        prevEyeArgs = args;
        havePrevEye = true;
        return r;
    }
};

// ---- the report ------------------------------------------------------------------------------------
namespace textutil {
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
// A window holds millions of draws, not hundreds of millions: the clamp bounds the line's length, not the data.
inline unsigned long long clampCount(uint64_t v) { return v > 99999999ull ? 99999999ull : static_cast<unsigned long long>(v); }
inline double clampPerFrame(double v) { return v > 99999999.0 ? 99999999.0 : (v < 0.0 ? 0.0 : v); }
}  // namespace textutil

// A share in percent, or "-" with nothing to share.
inline void percent(char* out, size_t cap, uint64_t part, uint64_t whole) {
    if (whole == 0) std::snprintf(out, cap, "-");
    else std::snprintf(out, cap, "%.1f%%", 100.0 * static_cast<double>(part) / static_cast<double>(whole));
}

// "draw bindings, sampled frames:" -- the classes of the owner draws and of the eye draws, how often each slot moved on the eye draws,
// and the answer stability. Under about 1150 characters at its worst (the log line holds about 1170).
inline size_t formatBindingsLine(char* out, size_t cap, const Tally& t, uint32_t endFrame, uint32_t sampledFrames) {
    textutil::Out o(out, cap);
    uint64_t owner = 0, eye = 0;
    for (unsigned c = 0; c < kClassCount; ++c) {
        owner += t.owner[c];
        eye += t.eye[c];
    }
    const double perFrame = sampledFrames ? 1.0 / static_cast<double>(sampledFrames) : 0.0;
    o.add("draw bindings, sampled frames: 1800-frame window ending %u; %u sampled frames. Owner draws a frame %.0f, against the previous "
          "owner draw: ",
          endFrame, sampledFrames, textutil::clampPerFrame(static_cast<double>(owner) * perFrame));
    char s[16];
    for (unsigned c = 0; c < kClassCount; ++c) {
        percent(s, sizeof(s), t.owner[c], owner);
        o.add("%s %s%s", className(c), s, c + 1 < kClassCount ? ", " : ". ");
    }
    o.add("Eye draws a frame %.0f, against the previous eye draw: ", textutil::clampPerFrame(static_cast<double>(eye) * perFrame));
    for (unsigned c = 0; c < kClassCount; ++c) {
        percent(s, sizeof(s), t.eye[c], eye);
        o.add("%s %s%s", className(c), s, c + 1 < kClassCount ? ", " : ". ");
    }
    o.add("(repeat: no generation of the 8 slots moved; rebound: same pointers, a generation moved; changed: a pointer differs; first: "
          "nothing before it in the frame.) Per slot on eye draws, generation moved / pointer changed: ");
    for (unsigned i = 0; i < kSlots; ++i) {
        char g[16], p[16];
        percent(g, sizeof(g), t.genMoved[i], t.eyeCompared);
        percent(p, sizeof(p), t.ptrChanged[i], t.eyeCompared);
        o.add("%s %s / %s%s", slotName(i), g, p, i + 1 < kSlots ? ", " : ". ");
    }
    o.add("Eye draws whose answer (verdict, interface depth, composite, hologram) differed from the previous eye draw's: ");
    for (int k = 0; k < 2; ++k) {
        const Stability& st = k == 0 ? t.repeat : t.rebound;
        o.add("of %llu %s draws %llu differed (verdict %llu, interface depth %llu, composite %llu, hologram %llu; same arguments %llu)%s",
              textutil::clampCount(st.draws), k == 0 ? "repeat" : "rebound", textutil::clampCount(st.differing),
              textutil::clampCount(st.verdict), textutil::clampCount(st.uiDepth), textutil::clampCount(st.composite),
              textutil::clampCount(st.holo), textutil::clampCount(st.differingSameArgs), k == 0 ? "; " : ".");
    }
    return o.len;
}

}  // namespace bindrepeat
}  // namespace edvr

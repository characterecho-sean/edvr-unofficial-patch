// F116 (native on-foot VR): the HUD hidden while a terminal fills the view. The pure parts: the hide's hysteresis on the in-world screens' footprint and the
// frame order it is fed in, the HRC submit hook's decision, the game's hide counters (read and logged only) with their byte pins, the episodes and the lines.
// Header only, no game or D3D11 state, so native_frame_test runs the very functions ui_depth.cpp (the footprint, on the render thread), the HRC submit hook
// (P2, on the game's thread) and the frame thread use. Journal: the arc doc, 2026-10-25, F116.
//
// WHY. The pilot (log edvr_gfx_20261025_071049, 33e05706, Pimax Crystal Super): "the hud clips through/is displayed while interacting with a terminal". In eye
// dumps 072154496003 and 060611711003 the radar, the compass, the objective and the suit block are drawn over a settlement terminal's screen, 0.19 m from the
// eye, in front of a screen farther away. Flat Elite does not hide its HUD at a terminal either (the pilot), but there the elements sit at the screen's edges.
//
// THE HIDE. The terminal's screen is the F105 pair (world_screen_coverage.h); F108's census counts its pixels per eye every frame a screen is drawn. While the
// screen covers more than kHideOver of an eye's render pixels (the larger of the two eyes) for kHideFrames frames in a row, the HRC submit hook (+0x1A16F10,
// the function that submits the on-foot HUD's meshes) returns 0 without calling the game's function, which is what the game's own gate does when it answers no
// (nothing before that gate has a side effect). The HUD comes back after kShowFrames frames in a row under kShowUnder. Only the HUD meshes: the panel cursor
// (its own quad, VS 4EF6DDB075A927FA), the screen and its prompts are other draws. Limits: terminals drawn by the F105 pair only, and only while an external
// upscaler runs (F105 marks the screen only then); with neither, the footprint is 0 and the HUD is never hidden.
//
// THE GAME'S HIDE COUNTERS (read and logged only). The game decides whether to submit the HUD's meshes by one virtual call on the HUD controller at
// [HRC+0x1D0]: its slot +0x80 (+0x1A8F950) is "visible (slot +0x78) AND the hide counter [controller+0x380] is zero AND not an HMD (modes 3 and 4)". The
// controller pushes and pops that counter (slots +0x60, +0x58) and a sibling at +0x384 (slots +0x70, +0x68; slot +0x88 reads it for another component).
// EDVR's creation patch (+0x1A16F48, 0x80 -> 0x78) asks slot +0x78 alone, so it drops the counter with the HMD test. Whether the game pushes it at a terminal is
// not known: the hook reads both counters and the frame thread logs every rise, with the footprint at the time; the HUD's behaviour does not depend on them.
// Every address is pinned against build 334925 (verified offline with capstone on that exe, and at run time by the bytes below), and per call the
// controller's vtable must be the verified one before its counters are read.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f116 {

// ---- The hide ----------------------------------------------------------------------------------------------------------------------------------------------
constexpr float kHideOver = 0.15f;      // a frame counts toward the hide when the larger eye's footprint is over this
constexpr float kShowUnder = 0.10f;     // ...and toward the show when it is under this
constexpr uint32_t kHideFrames = 2;     // frames in a row over kHideOver that hide the HUD
constexpr uint32_t kShowFrames = 10;    // frames in a row under kShowUnder that show it again
constexpr uint64_t kFreshMs = 250;      // the hook honours the hide only while the render thread stepped it this recently (a stalled step shows the HUD)

// An eye's footprint: its screen pixels over its render pixels (0 for an eye with no pixels).
inline float eyeFootprint(uint32_t screen, uint64_t pixels) noexcept {
    return pixels ? static_cast<float>(static_cast<double>(screen) / static_cast<double>(pixels)) : 0.0f;
}
// A frame's footprint: the larger of the two eyes'.
inline float frameFootprint(uint32_t screen0, uint64_t pixels0, uint32_t screen1, uint64_t pixels1) noexcept {
    const float a = eyeFootprint(screen0, pixels0), b = eyeFootprint(screen1, pixels1);
    return a > b ? a : b;
}

// The hysteresis, stepped once per frame in frame order. Returns whether the HUD is hidden after this frame.
struct Hysteresis {
    bool hidden = false;
    uint32_t over = 0, under = 0;
    bool step(float footprint) noexcept {
        if (!hidden) {
            over = footprint > kHideOver ? over + 1 : 0;
            if (over >= kHideFrames) {
                hidden = true;
                under = 0;
            }
        } else {
            under = footprint < kShowUnder ? under + 1 : 0;
            if (under >= kShowFrames) {
                hidden = false;
                over = 0;
            }
        }
        return hidden;
    }
};

// The frames in order: a frame with a screen waits for its census row (read back a few frames later), a frame with none is 0 at once, a frame whose census was
// skipped is unknown (the hysteresis holds). pop() hands the frames on in frame order up to the first still waiting. A full queue drops its oldest as unknown.
class FrameOrder {
public:
    static constexpr unsigned kCap = 16;
    enum State : uint8_t { kWaiting = 0, kReady = 1, kUnknown = 2 };
    struct Slot {
        uint64_t frame = 0;
        uint64_t pixels[2] = {};
        float footprint = 0.0f;
        State state = kUnknown;
    };
    uint32_t dropped = 0;   // waiting frames dropped by a full queue (handed on as unknown)

    void waiting(uint64_t frame, uint64_t pixels0, uint64_t pixels1) noexcept {
        Slot s;
        s.frame = frame;
        s.pixels[0] = pixels0;
        s.pixels[1] = pixels1;
        s.state = kWaiting;
        push(s);
    }
    void none(uint64_t frame) noexcept {
        Slot s;
        s.frame = frame;
        s.state = kReady;
        push(s);
    }
    void unknown(uint64_t frame) noexcept {
        Slot s;
        s.frame = frame;
        s.state = kUnknown;
        push(s);
    }
    // A census row for `frame` (ok false: lost on read-back). Rows come in frame order; one for a frame not waiting is ignored.
    void row(uint64_t frame, bool ok, uint32_t screen0, uint32_t screen1) noexcept {
        for (unsigned i = 0; i < n_; ++i) {
            Slot& s = q_[(head_ + i) % kCap];
            if (s.frame != frame || s.state != kWaiting) continue;
            s.footprint = frameFootprint(screen0, s.pixels[0], screen1, s.pixels[1]);
            s.state = ok ? kReady : kUnknown;
            return;
        }
    }
    // f(frame, known, footprint) for each frame no longer waiting, oldest first.
    template <class F>
    void pop(F&& f) {
        while (n_ && q_[head_].state != kWaiting) {
            const Slot& s = q_[head_];
            f(s.frame, s.state == kReady, s.footprint);
            head_ = (head_ + 1) % kCap;
            --n_;
        }
    }
    unsigned size() const noexcept { return n_; }
    void clear() noexcept {
        head_ = n_ = 0;
    }

private:
    Slot q_[kCap];
    unsigned head_ = 0, n_ = 0;
    void push(const Slot& s) noexcept {
        if (n_ == kCap) {   // full: the oldest (still waiting) is dropped, which the hysteresis sees as an unknown frame (it holds)
            ++dropped;
            head_ = (head_ + 1) % kCap;
            --n_;
        }
        q_[(head_ + n_) % kCap] = s;
        ++n_;
    }
};

// The HRC submit hook's decision: skip the submit while the hide is on and was stepped within kFreshMs.
inline bool decideSkip(bool hidden, bool stepped, uint64_t ageMs) noexcept { return hidden && stepped && ageMs <= kFreshMs; }

// ---- The game's hide counters (read and logged only) -------------------------------------------------------------------------------------------------------
constexpr uint32_t kControllerOff = 0x1D0;      // [HRC+0x1D0], the object the gate's call is made on
constexpr uint32_t kHideOff = 0x380;            // the hide counter the HUD meshes' test reads
constexpr uint32_t kSiblingOff = 0x384;         // the sibling counter (slot +0x88's)
static_assert(kSiblingOff == kHideOff + 4, "the two counters are read as one eight-byte block");
constexpr uintptr_t kVtableRva = 0x520D958;
constexpr uint32_t kSlotPop = 0x58, kSlotPush = 0x60, kSlotSiblingPop = 0x68, kSlotSiblingPush = 0x70, kSlotVisible = 0x78, kSlotTest = 0x80;
constexpr uintptr_t kPopRva = 0x1A912E0, kPushRva = 0x1A4A9F0, kSiblingPopRva = 0x1A917B0, kSiblingPushRva = 0x1A4AC10, kVisibleRva = 0x1A8F9D0,
                    kTestRva = 0x1A8F950;
constexpr size_t kSlotCount = 6;
constexpr uint32_t kSlots[kSlotCount] = {kSlotPop, kSlotPush, kSlotSiblingPop, kSlotSiblingPush, kSlotVisible, kSlotTest};
constexpr uintptr_t kSlotTargets[kSlotCount] = {kPopRva, kPushRva, kSiblingPopRva, kSiblingPushRva, kVisibleRva, kTestRva};
// The push: inc dword ptr [rcx+0x380]; ret. The pop: mov eax, [rcx+0x380]; test eax, eax; je +8; dec eax; mov [rcx+0x380], eax; ret.
constexpr uint8_t kPushPin[7] = {0xFF, 0x81, 0x80, 0x03, 0x00, 0x00, 0xC3};
constexpr uint8_t kPopPin[19] = {0x8B, 0x81, 0x80, 0x03, 0x00, 0x00, 0x85, 0xC0, 0x74, 0x08, 0xFF, 0xC8, 0x89, 0x81, 0x80, 0x03, 0x00, 0x00, 0xC3};
constexpr uint8_t kSiblingPushPin[7] = {0xFF, 0x81, 0x84, 0x03, 0x00, 0x00, 0xC3};
constexpr uint8_t kSiblingPopPin[19] = {0x8B, 0x81, 0x84, 0x03, 0x00, 0x00, 0x85, 0xC0, 0x74, 0x08, 0xFF, 0xC8, 0x89, 0x81, 0x84, 0x03, 0x00, 0x00, 0xC3};
// The test: push rbx; sub rsp, 0x20; mov rax, [rcx]; mov rbx, rcx; call [rax+0x78]; test al, al; je +0x35; cmp dword ptr [rbx+0x380], 0; jne +0x2C.
constexpr uint8_t kTestPin[28] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0xD9, 0xFF, 0x50, 0x78, 0x84, 0xC0, 0x74, 0x35,
                                  0x83, 0xBB, 0x80, 0x03, 0x00, 0x00, 0x00, 0x75, 0x2C};
constexpr size_t kTestPinCounterAt = 21;   // the cmp's displacement inside kTestPin
// The gate's call site in the HRC function: mov rcx, [rcx+0x1D0]; xor edx, edx; mov r12, r8; mov rax, [rcx]; call qword ptr [rax+disp32]. The displacement
// byte that follows is 0x80 (the game's) or 0x78 (EDVR's creation patch), so it is not pinned here.
constexpr uintptr_t kGateSiteRva = 0x1A16F37;
constexpr uint8_t kGateSitePin[17] = {0x48, 0x8B, 0x89, 0xD0, 0x01, 0x00, 0x00, 0x33, 0xD2, 0x4D, 0x8B, 0xE0, 0x48, 0x8B, 0x01, 0xFF, 0x90};

constexpr uint32_t pinDword(const uint8_t* p, size_t at) noexcept {
    return static_cast<uint32_t>(p[at]) | (static_cast<uint32_t>(p[at + 1]) << 8) | (static_cast<uint32_t>(p[at + 2]) << 16) |
           (static_cast<uint32_t>(p[at + 3]) << 24);
}
static_assert(pinDword(kPushPin, 2) == kHideOff && pinDword(kPopPin, 2) == kHideOff && pinDword(kPopPin, 14) == kHideOff, "the push and pop pins move +0x380");
static_assert(pinDword(kSiblingPushPin, 2) == kSiblingOff && pinDword(kSiblingPopPin, 2) == kSiblingOff && pinDword(kSiblingPopPin, 14) == kSiblingOff,
              "the sibling pins move +0x384");
static_assert(pinDword(kTestPin, kTestPinCounterAt) == kHideOff, "the test pin reads +0x380");
static_assert(pinDword(kGateSitePin, 3) == kControllerOff, "the gate's call is made on [HRC+0x1D0]");

// One call's read of the counters.
enum class Read : uint8_t { kRead, kNotVerified, kNoController, kWrongClass, kUnreadable, kCount };
inline const char* readName(Read r) noexcept {
    switch (r) {
        case Read::kRead: return "read";
        case Read::kNotVerified: return "the addresses were not verified";
        case Read::kNoController: return "no controller at HRC+0x1D0";
        case Read::kWrongClass: return "the controller's vtable is not the verified one";
        case Read::kUnreadable: return "the controller could not be read";
        default: return "?";
    }
}
// verified: the pins held at install. controllerRead / controller: [HRC+0x1D0]. vtableRead / vtableOk: its vtable pointer, the verified one. countersRead: the
// eight bytes at +0x380.
inline Read readCounters(bool verified, bool controllerRead, uintptr_t controller, bool vtableRead, bool vtableOk, bool countersRead) noexcept {
    if (!verified) return Read::kNotVerified;
    if (!controllerRead) return Read::kUnreadable;
    if (!controller) return Read::kNoController;
    if (!vtableRead) return Read::kUnreadable;
    if (!vtableOk) return Read::kWrongClass;
    if (!countersRead) return Read::kUnreadable;
    return Read::kRead;
}

// ---- Episodes (the frame thread, once a frame) -------------------------------------------------------------------------------------------------------------
// A footprint of -1 is unknown.
struct TerminalEpisode {
    bool on = false;
    uint32_t number = 0, frames = 0;
    uint64_t startMs = 0, lastMs = 0, calls0 = 0, skipped0 = 0, calls = 0, skipped = 0;
    float startFootprint = -1.0f, peak = -1.0f;
    enum Edge : uint8_t { kNone = 0, kBegan = 1, kEnded = 2 };
    // One frame: the hide as the render thread last stepped it, the footprint, and the hook's running call and skip counts.
    Edge step(bool hidden, float footprint, uint64_t nowMs, uint64_t callsTotal, uint64_t skippedTotal) noexcept {
        Edge e = kNone;
        if (hidden && !on) {
            on = true;
            ++number;
            frames = 0;
            startMs = nowMs;
            calls0 = callsTotal;
            skipped0 = skippedTotal;
            startFootprint = peak = footprint;
            e = kBegan;
        }
        if (!on) return kNone;
        lastMs = nowMs;
        calls = callsTotal - calls0;
        skipped = skippedTotal - skipped0;
        if (!hidden) {
            on = false;
            return kEnded;
        }
        ++frames;
        if (footprint > peak) peak = footprint;
        return e;
    }
    double lengthS() const noexcept { return static_cast<double>(lastMs - startMs) / 1000.0; }
};

// A rise of either game counter (log only): from the first frame either reads nonzero to the first both read zero.
struct CounterEpisode {
    bool on = false;
    uint32_t number = 0, frames = 0, startHide = 0, startSibling = 0, maxHide = 0, maxSibling = 0;
    uint64_t startMs = 0, lastMs = 0;
    float startFootprint = -1.0f, peak = -1.0f;
    enum Edge : uint8_t { kNone = 0, kBegan = 1, kEnded = 2 };
    Edge step(uint32_t hide, uint32_t sibling, float footprint, uint64_t nowMs) noexcept {
        const bool nowOn = hide != 0 || sibling != 0;
        Edge e = kNone;
        if (nowOn && !on) {
            on = true;
            ++number;
            frames = 0;
            startHide = hide;
            startSibling = sibling;
            maxHide = maxSibling = 0;
            startMs = nowMs;
            startFootprint = peak = footprint;
            e = kBegan;
        }
        if (!on) return kNone;
        lastMs = nowMs;
        if (!nowOn) {
            on = false;
            return kEnded;
        }
        ++frames;
        if (hide > maxHide) maxHide = hide;
        if (sibling > maxSibling) maxSibling = sibling;
        if (footprint > peak) peak = footprint;
        return e;
    }
    double lengthS() const noexcept { return static_cast<double>(lastMs - startMs) / 1000.0; }
};

// The HUD submitted while a terminal filled the view (footprint over kHideOver, the hook called and nothing skipped this frame): the seconds and the longest
// run. A frame whose footprint is not known, with no hook call, or with a skip ends a run. Two frames of it are the hysteresis; seconds of it are a failure.
struct SubmittedRun {
    double totalS = 0.0, longestS = 0.0, runS = 0.0;
    void step(bool known, float footprint, bool called, bool skipped, double dtS) noexcept {
        if (!known || !(footprint > kHideOver) || !called || skipped || !(dtS > 0.0) || dtS > 1.0) {
            runS = 0.0;
            return;
        }
        totalS += dtS;
        runS += dtS;
        if (runS > longestS) longestS = runS;
    }
    void resetWindow() noexcept {
        const double run = runS;
        *this = SubmittedRun{};
        runS = run;
    }
};

// ---- The lines (the Log adds the time); tools\edvr_log.py --health's `HUD hide (F116)` reads them by their first words --------------------------------
inline void footprintText(char* out, size_t size, float footprint) noexcept {
    if (footprint >= 0.0f) std::snprintf(out, size, "%.1f%%", static_cast<double>(footprint) * 100.0);
    else std::snprintf(out, size, "unknown");
}
inline int formatVerified(char* out, size_t size) noexcept {
    return std::snprintf(out, size,
        "HUD hide (F116): the game's hide counters are read (logged only): the HUD controller at HRC+0x%X (vtable +0x%llX), its counter +0x%X (push +0x%llX, "
        "pop +0x%llX; the HUD meshes' test +0x%llX reads it) and the sibling +0x%X (push +0x%llX, pop +0x%llX), all pinned.",
        kControllerOff, static_cast<unsigned long long>(kVtableRva), kHideOff, static_cast<unsigned long long>(kPushRva), static_cast<unsigned long long>(kPopRva),
        static_cast<unsigned long long>(kTestRva), kSiblingOff, static_cast<unsigned long long>(kSiblingPushRva), static_cast<unsigned long long>(kSiblingPopRva));
}
inline int formatRefusedInstall(char* out, size_t size, const char* why) noexcept {
    return std::snprintf(out, size, "HUD hide (F116): the game's hide counters are NOT read -- %s (the hide on the terminal footprint is unaffected).",
                         why ? why : "?");
}
inline int formatRefusedRead(char* out, size_t size, Read r) noexcept {
    return std::snprintf(out, size, "HUD hide (F116): a counter read refused -- %s (said once per reason).", readName(r));
}
inline int formatTerminalBegan(char* out, size_t size, const TerminalEpisode& e) noexcept {
    char fp[32];
    footprintText(fp, sizeof(fp), e.startFootprint);
    return std::snprintf(out, size, "HUD hide (F116): terminal episode %u began: the HUD hidden, terminal footprint %s.", e.number, fp);
}
inline int formatTerminalEnded(char* out, size_t size, const TerminalEpisode& e) noexcept {
    char pk[32];
    footprintText(pk, sizeof(pk), e.peak);
    return std::snprintf(out, size,
        "HUD hide (F116): terminal episode %u ended after %.2f s, %u frames: footprint peak %s; HRC calls %llu, skipped %llu (frames the HUD was hidden).",
        e.number, e.lengthS(), e.frames, pk, static_cast<unsigned long long>(e.calls), static_cast<unsigned long long>(e.skipped));
}
inline int formatCounterBegan(char* out, size_t size, const CounterEpisode& e) noexcept {
    char fp[32];
    footprintText(fp, sizeof(fp), e.startFootprint);
    return std::snprintf(out, size, "HUD hide (F116): the game's counter rose (episode %u): +0x380 %u, +0x384 %u; terminal footprint %s.", e.number,
                         e.startHide, e.startSibling, fp);
}
inline int formatCounterEnded(char* out, size_t size, const CounterEpisode& e) noexcept {
    char fs[32], pk[32];
    footprintText(fs, sizeof(fs), e.startFootprint);
    footprintText(pk, sizeof(pk), e.peak);
    return std::snprintf(out, size,
        "HUD hide (F116): the game's counter fell (episode %u) after %.2f s, %u frames: at most +0x380 %u, +0x384 %u; terminal footprint at the rise %s, peak %s.",
        e.number, e.lengthS(), e.frames, e.maxHide, e.maxSibling, fs, pk);
}
// The 30 s window, zeros included.
struct Window {
    uint64_t calls = 0, skipped = 0;
    uint64_t stepped = 0, over = 0, hiddenFrames = 0, censusRows = 0;
    uint32_t terminalBegan = 0, terminalEnded = 0, counterBegan = 0, counterEnded = 0;
    uint64_t readCalls = 0, hideSet = 0, siblingSet = 0;
    uint64_t refused[static_cast<size_t>(Read::kCount)] = {};
    bool upscaler = false;   // an external upscaler ran at some frame boundary of the window (F105 marks only then)
    uint64_t refusedTotal() const noexcept {
        uint64_t n = 0;
        for (size_t i = 1; i < static_cast<size_t>(Read::kCount); ++i) n += refused[i];
        return n;
    }
};
inline int formatWindow(char* out, size_t size, const Window& w, const SubmittedRun& s, float footprintNow) noexcept {
    char now[32];
    footprintText(now, sizeof(now), footprintNow);
    return std::snprintf(out, size,
        "HUD hide (F116) 30 s: HRC calls %llu, skipped %llu (the HUD hidden); terminal frames %llu (census rows %llu, an external upscaler %s): over 15%% %llu, "
        "hidden %llu; terminal episodes began %u, ended %u; submitted with a terminal over 15%% %.1f s, longest %.1f s; footprint now %s; the game's counters "
        "(logged only): calls read %llu, refused %llu (not verified %llu, no controller %llu, wrong class %llu, unreadable %llu), with +0x380 set %llu, with "
        "+0x384 set %llu, rises %u, falls %u. Limits: F105-pair terminals only, and only with an external upscaler.",
        static_cast<unsigned long long>(w.calls), static_cast<unsigned long long>(w.skipped), static_cast<unsigned long long>(w.stepped),
        static_cast<unsigned long long>(w.censusRows), w.upscaler ? "ran" : "did not run", static_cast<unsigned long long>(w.over),
        static_cast<unsigned long long>(w.hiddenFrames), w.terminalBegan, w.terminalEnded, s.totalS, s.longestS, now,
        static_cast<unsigned long long>(w.readCalls), static_cast<unsigned long long>(w.refusedTotal()),
        static_cast<unsigned long long>(w.refused[static_cast<size_t>(Read::kNotVerified)]),
        static_cast<unsigned long long>(w.refused[static_cast<size_t>(Read::kNoController)]),
        static_cast<unsigned long long>(w.refused[static_cast<size_t>(Read::kWrongClass)]),
        static_cast<unsigned long long>(w.refused[static_cast<size_t>(Read::kUnreadable)]), static_cast<unsigned long long>(w.hideSet),
        static_cast<unsigned long long>(w.siblingSet), w.counterBegan, w.counterEnded);
}

}  // namespace f116
}  // namespace nofoot
}  // namespace edvr

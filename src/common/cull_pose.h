#pragma once

// advanced.cull_pose (TEMPORARY, the terrain-culling arc, docs\terrain-culling.md): which instant Elite's game-thread head pose is
// located at, and whether Elite's latched-pose branch is bypassed. Pure: no windows.h, no runtime state. Both halves include it (the
// graphics half decides and patches, the runtime locates), and tools\openxr_pose_test and tools\native_frame_test drive every case.
//
// WHY. Round 6's disassembly of build 332841 found two head-pose sources behind Elite's pose function 0x4E3690:
//   - the render thread (arg6 = 1): IVRCompositor::WaitGetPoses' render pose (call at 0x4E3715), latched at [W+0x111] until Present
//     clears it; EDVR locates that at the frame's predictedDisplayTime, the pose that is drawn;
//   - the game thread (arg6 = 0, the controller tick): IVRSystem::GetDeviceToAbsoluteTrackingPose (`call rbx` at 0x4E387F, returning
//     to 0x4E3881) with a prediction of about 0 s, i.e. "now"; or, when the latch is set, the cached WaitGetPoses pose, by the
//     branch `je 0x4E384F` at 0x4E36EE (0F 84 5B 01 00 00).
// Shipped (head_pose_time.h): that "now" request is answered at the latest frame's display time, which closed the squares of a gaze
// switch (flight 3). HYPOTHESIS H-onef: the game thread prepares frame N+1 while the render thread draws N, and while N renders the
// latch is set, so its cull camera is the pose it was handed for N, drawn one frame later: the display time leaves about one period
// of lag, ~11 ms, invisible at a normal gaze switch and ~2.8 degrees at 250 deg/s. The modes below test it: `next` answers at the
// display time plus one period, and `_direct` stops the latch from serving the game thread a cached pose at all.
//
// The modes. `display` is the default and is exactly the shipped behaviour, so an unset (or unreadable) key changes nothing; `now` is
// the behaviour before the fix, as a control. The wire code 0 is `display`, so a half that cannot carry the field reads the default.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr::cullpose {

enum class Mode : uint32_t { Display = 0, Now = 1, Next = 2, DisplayDirect = 3, NextDirect = 4 };
constexpr uint32_t kModeCodes = 5;
// A code past the last is the default, display: what an unset key means.
inline Mode modeFromCode(uint32_t code) { return code < kModeCodes ? static_cast<Mode>(code) : Mode::Display; }
inline const char* modeName(Mode mode) {
    switch (mode) {
        case Mode::Now: return "now";
        case Mode::Next: return "next";
        case Mode::DisplayDirect: return "display_direct";
        case Mode::NextDirect: return "next_direct";
        default: return "display";
    }
}
// Case-insensitive and exact: a near miss, a number, an empty value and junk are the default, display.
inline Mode parseMode(const char* text) {
    if (!text) return Mode::Display;
    for (uint32_t code = 0; code < kModeCodes; ++code) {
        const char* name = modeName(static_cast<Mode>(code));
        const char* a = text;
        const char* b = name;
        while (*a && *b && ((*a >= 'A' && *a <= 'Z') ? *a + ('a' - 'A') : *a) == *b) { ++a; ++b; }
        if (!*a && !*b) return static_cast<Mode>(code);
    }
    return Mode::Display;
}

// The instant an Elite "now" call is located at: Now is the behaviour before the fix (the wall clock, plus the prediction the game passed).
enum class Time : uint8_t { Now, Display, Next };
inline Time timeOf(Mode mode) {
    switch (mode) {
        case Mode::Display: case Mode::DisplayDirect: return Time::Display;
        case Mode::Next: case Mode::NextDirect: return Time::Next;
        default: return Time::Now;
    }
}
inline bool wantsBypass(Mode mode) { return mode == Mode::DisplayDirect || mode == Mode::NextDirect; }
inline const char* locatedAt(Mode mode) {
    switch (timeOf(mode)) {
        case Time::Display: return "the frame's display time";
        case Time::Next: return "display time + one period";
        default: return "now+prediction";
    }
}

// ---- Elite build 332841 --------------------------------------------------------------------------------------------------------
constexpr uint32_t kBuildStamp = 1788384820u, kBuildImageSize = 104894464u;
// The only direct IVRSystem::GetDeviceToAbsoluteTrackingPose call Elite makes: `call rbx` (FF D3) at 0x4E387F, returning here.
constexpr uint32_t kDirectPoseReturnRva = 0x4E3881;

// ---- the engine patch ----------------------------------------------------------------------------------------------------------
// 0x4E36E7: cmp byte [rsi+0x111],0 (the latch) / 0x4E36EE: je 0x4E384F / 0x4E36F4: cmp byte [rsi+0x111],0 ; jne 0x4E37E8 (the cached
// pose). Reached with arg6 == 0 only (arg6 != 0 jumps over it at 0x4E36E5), so making the branch unconditional sends every game-thread
// caller to the direct call at 0x4E384F and none to the cached pose.
constexpr uint32_t kLatchWordRva = 0x4E36E8;     // the aligned 8-byte word that holds the branch's two opcode bytes
constexpr uint32_t kLatchBranchRva = 0x4E36EE;   // ...at its offset 6
constexpr uint32_t kLatchTargetRva = 0x4E384F;
constexpr unsigned kLatchBranchOffset = kLatchBranchRva - kLatchWordRva;
inline constexpr uint8_t kLatchOriginal[6] = {0x0F, 0x84, 0x5B, 0x01, 0x00, 0x00};   // je rel32
inline constexpr uint8_t kLatchPatched[6] = {0x90, 0xE9, 0x5B, 0x01, 0x00, 0x00};    // nop ; jmp rel32 (the same displacement)

// Where the 6 bytes at 0x4E36EE lead, for either form: `0F 84 rel32` (je, ends at +6) or `90 E9 rel32` (nop, then jmp ending at +6).
// 0 when it is neither.
inline uint32_t latchTarget(const uint8_t* six) {
    const bool je = six[0] == 0x0F && six[1] == 0x84, jmp = six[0] == 0x90 && six[1] == 0xE9;
    if (!je && !jmp) return 0;
    int32_t rel;
    std::memcpy(&rel, six + 2, sizeof(rel));
    return static_cast<uint32_t>(kLatchBranchRva + 6 + rel);
}
static_assert(kLatchBranchOffset == 6, "the branch's opcode bytes are the last two of the aligned word");
static_assert(kLatchWordRva % 8 == 0, "one aligned eight-byte store");

// What the gate reads: the PE stamp and size, and the 16 bytes from 0x4E36E8 (the word and the one after it).
struct LatchImage {
    bool headers = false, bytesRead = false;
    uint32_t stamp = 0, imageSize = 0;
    uint8_t bytes[16] = {};
};
inline bool branchIs(const uint8_t* sixteen, bool patched) {
    return std::memcmp(sixteen + kLatchBranchOffset, patched ? kLatchPatched : kLatchOriginal, 6) == 0;
}
// Null when the word may be changed from its current form (`patched` says which form the bytes must be in), else why not.
inline const char* latchGate(const LatchImage& image, bool patched) {
    if (!image.headers) return "the executable's headers could not be read";
    if (image.stamp != kBuildStamp || image.imageSize != kBuildImageSize) return "not build 332841";
    if (!image.bytesRead) return "the branch's bytes could not be read";
    if (!branchIs(image.bytes, patched))
        return patched ? "the branch is not the bytes this patch wrote" : "the branch's bytes differ from build 332841's";
    return nullptr;
}
// The eight-byte word to store: the image's current word with the branch's two opcode bytes in the wanted form.
inline uint64_t latchWord(const uint8_t* sixteen, bool bypass) {
    uint8_t word[8];
    std::memcpy(word, sixteen, sizeof(word));
    const uint8_t* wanted = bypass ? kLatchPatched : kLatchOriginal;
    word[kLatchBranchOffset] = wanted[0];
    word[kLatchBranchOffset + 1] = wanted[1];
    uint64_t value;
    std::memcpy(&value, word, sizeof(value));
    return value;
}

// The bypass as the graphics half holds it: applied or not, and why not when it was refused. `read` returns a LatchImage; `write`
// stores the new eight-byte word atomically and returns whether it did. A refusal is final until the mode leaves bypass (an
// executable that did not match at the first frame does not match at the next).
class LatchBypass {
public:
    enum class Kind { Off, On, Refused };
    template <class Read, class Write>
    void update(bool want, Read&& read, Write&& write) {
        if (want) {
            if (applied_ || refused_) return;
            const LatchImage image = read();
            if (const char* why = latchGate(image, false)) { refused_ = why; return; }
            if (!write(latchWord(image.bytes, true))) { refused_ = "the write failed"; return; }
            applied_ = true;
            return;
        }
        refused_ = nullptr;
        if (!applied_) return;
        applied_ = false;
        const LatchImage image = read();
        if (const char* why = latchGate(image, true)) { refused_ = why; return; }
        if (!write(latchWord(image.bytes, false))) refused_ = "the restore write failed";
    }
    Kind kind() const { return applied_ ? Kind::On : refused_ ? Kind::Refused : Kind::Off; }
    const char* why() const { return refused_ ? refused_ : ""; }
    bool applied() const { return applied_; }
private:
    bool applied_ = false;
    const char* refused_ = nullptr;
};

// The one line a change makes.
//   cull pose: <mode> -- Elite's game-thread head pose is located at <now+prediction | the frame's display time |
//   display time + one period>; latched-pose bypass <on|off|refused: why>
// The time modes need no build (the runtime's filter is a return address inside the game's image), so only the bypass can be refused.
inline const char* poseLine(char (&buffer)[256], Mode requested, LatchBypass::Kind bypass, const char* why) {
    char latch[96];
    if (bypass == LatchBypass::Kind::On) std::snprintf(latch, sizeof(latch), "on");
    else if (bypass == LatchBypass::Kind::Refused) std::snprintf(latch, sizeof(latch), "refused: %s", why);
    else std::snprintf(latch, sizeof(latch), "off");
    std::snprintf(buffer, sizeof(buffer), "cull pose: %s -- Elite's game-thread head pose is located at %s; latched-pose bypass %s",
                  modeName(requested), locatedAt(requested), latch);
    return buffer;
}

// The graphics half's whole decision, one call per frame boundary (native_frame.cpp's beginFrame). Returns the mode code the runtime
// is told, the requested mode's. Logs every change of (mode, bypass) once; the starting state, the default mode with no patch, is the
// one a session begins in, so a key left unset writes nothing.
class Driver {
public:
    template <class Read, class Write, class Sink>
    uint32_t frame(Mode requested, Read&& read, Write&& write, Sink&& sink) {
        latch_.update(wantsBypass(requested), read, write);
        if (requested != lastRequested_ || latch_.kind() != lastKind_) {
            lastRequested_ = requested;
            lastKind_ = latch_.kind();
            char line[256];
            sink(poseLine(line, requested, latch_.kind(), latch_.why()));
        }
        return static_cast<uint32_t>(requested);
    }
    const LatchBypass& latch() const { return latch_; }
private:
    LatchBypass latch_;
    Mode lastRequested_ = Mode::Display;
    LatchBypass::Kind lastKind_ = LatchBypass::Kind::Off;
};

}  // namespace edvr::cullpose

#pragma once
// The LoD view probe, pure half (docs/elite-ss-hangar-door-2026-10-21.md). No Windows, no engine: tools\lod_view_test drives it. lod_view_hook.cpp
// is the glue that patches the game's scene-view builder, reads each call's view under SEH and hands it here.
//
// THE FUNCTION. EliteDangerous64.exe (build 334473, PE stamp 1791840573, size 104906752) RVA 0x2811E40, the scene-view builder: rcx the
// scene-view array (64 slots of 0x570 bytes, the count at +0x15C00), rdx the caller's view descriptor. It fills slot `count` and increments
// the count. Among the slot's fields it writes the view's pixel size, A at +0x530 = P34 * (2 / P22) * (1 / H) and B at +0x540 = P44 * (2 / P22)
// * (1 / H), P the camera's projection and H the descriptor's height at +0x264 (its width at +0x260). For a perspective camera A = 2 tan(fovy/2) / H,
// the world size of one target pixel at unit distance, and B = 0. 0x281C5B0 copies A and B to the render views (+0x550, +0x560), where every
// per-view LoD and cull test reads them: a part is culled when A * (d - r) * s exceeds its LoD table's first threshold, and its level is picked
// from the same number. So the render size enters every LoD decision through H.
//
// WHAT THE PROBE ANSWERS. Whether the flat main view's H is the render height (the one link of the mechanism not traced in the code), which
// caller builds it, and, with advanced.detail_at_output_size on, whether evaluating LoD at the output's pixel size brings back the hangar door
// pieces Supersampling 0.5 drops. The key scales A and B of a view drawn at the render size by render height / output height, after the
// game wrote them; the game's own A is what the lines print, so H0 reads the same whichever way the key is set.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr::lodview {

// ---- identity: the pair every build-keyed hook here checks, and the sixteen bytes of the entry --------------------------------------------
constexpr uint32_t kExpectedTimestamp = 1791840573u;
constexpr uint32_t kExpectedImageSize = 104906752u;
constexpr uintptr_t kBuilderRva = 0x2811E40u;
constexpr size_t kPrologueBytes = 16;
// mov [rsp+0x18],rbx; push rbp; push rsi; push rdi; sub rsp,0x30; mov r8d,[rcx+0x15C00] (its first four bytes). CodeHook moves the first five
// bytes (no relative operand).
constexpr uint8_t kPrologue[kPrologueBytes] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x44, 0x8B, 0x81, 0x00};

enum class Install : uint8_t { kInstall = 0, kUnreadable, kWrongIdentity, kWrongPrologue };
// Whether to patch: the identity first (a stamp or size that is not the build's is final), then the sixteen bytes. A refusal is final for the session.
inline Install installDecision(bool identityRead, uint32_t stamp, uint32_t size, const uint8_t* got, size_t n) noexcept {
    if (!identityRead || got == nullptr || n < kPrologueBytes) return Install::kUnreadable;
    if (stamp != kExpectedTimestamp || size != kExpectedImageSize) return Install::kWrongIdentity;
    if (std::memcmp(got, kPrologue, kPrologueBytes) != 0) return Install::kWrongPrologue;
    return Install::kInstall;
}

// ---- the layout the builder writes (decompiled from build 334473) -------------------------------------------------------------------------
constexpr size_t kSlotStride = 0x570;
constexpr size_t kCountOffset = 0x15C00;
constexpr uint32_t kSlotCount = static_cast<uint32_t>(kCountOffset / kSlotStride);  // 64: the count sits right after the last slot
constexpr size_t kSlotA = 0x530;
constexpr size_t kSlotB = 0x540;
constexpr size_t kDescWidth = 0x260;
constexpr size_t kDescHeight = 0x264;
constexpr size_t kDescPrimary = 0x26A;  // a byte; set, the builder records this slot as the array's primary (+0x15C04 = count)

// The slot the call just wrote, from the count it left: count - 1, or kSlotCount when the count is out of the array (nothing is read then).
inline uint32_t writtenSlot(uint32_t countAfter) noexcept { return countAfter == 0 || countAfter > kSlotCount ? kSlotCount : countAfter - 1; }

// The factor the key applies to a view's A and B: render height over output height, for a view whose size is the render size while the render
// size is below the output's on both axes; 1 otherwise, and the caller writes nothing. A render size above the output's (Supersampling over 1)
// is left alone: there the game's finer LoD is what the player asked for.
inline float outputFactor(uint32_t viewW, uint32_t viewH, uint32_t renderW, uint32_t renderH, uint32_t outputW, uint32_t outputH) noexcept {
    if (!viewW || !viewH || !renderW || !renderH || !outputW || !outputH) return 1.0f;
    if (viewW != renderW || viewH != renderH) return 1.0f;
    if (renderW >= outputW || renderH >= outputH) return 1.0f;
    return static_cast<float>(renderH) / static_cast<float>(outputH);
}

// ---- the per-view table: one entry per (caller, size), reported when first seen and then every 5 s while it is called -------------------
constexpr uint64_t kReportEveryMs = 5000;
constexpr size_t kEntries = 32;

// One call, as the glue read it. a and b are the game's own values, before any factor; factor is what the key applied (1 when nothing).
struct Observation {
    uint32_t callerRva = 0;
    uint32_t width = 0, height = 0;
    uint32_t slot = 0;
    float a = 0.0f, b = 0.0f;
    uint8_t primary = 0;
    float factor = 1.0f;
};

struct Entry {
    Observation last;
    uint64_t calls = 0;             // since the entry was made
    uint64_t callsAtReport = 0;     // the calls the last line covered
    uint64_t reportedMs = 0;
    bool reported = false;
};

// What a report hands the glue for one line: the entry as it stands, whether it is the first line for it, and the calls since the last.
struct Due {
    Entry entry;
    bool first = false;
    uint64_t callsSince = 0;
};

class ViewTable {
public:
    // Find or make the entry for this caller and size, and record the call. A full table counts the call as overflow and keeps nothing.
    void observe(const Observation& o) noexcept {
        for (size_t i = 0; i < used_; ++i) {
            Entry& e = entries_[i];
            if (e.last.callerRva == o.callerRva && e.last.width == o.width && e.last.height == o.height) {
                e.last = o;
                ++e.calls;
                return;
            }
        }
        if (used_ == kEntries) {
            ++overflow_;
            return;
        }
        Entry& e = entries_[used_++];
        e = Entry{};
        e.last = o;
        e.calls = 1;
    }

    // The entries due a line at nowMs, at most cap of them, into out: never reported, or reported kReportEveryMs ago or more and called since.
    // Marks them reported. Returns how many it wrote.
    size_t takeDue(uint64_t nowMs, Due* out, size_t cap) noexcept {
        size_t n = 0;
        for (size_t i = 0; i < used_ && n < cap; ++i) {
            Entry& e = entries_[i];
            const bool first = !e.reported;
            const bool again = e.reported && nowMs - e.reportedMs >= kReportEveryMs && e.calls != e.callsAtReport;
            if (!first && !again) continue;
            out[n].entry = e;
            out[n].first = first;
            out[n].callsSince = e.calls - e.callsAtReport;
            ++n;
            e.reported = true;
            e.reportedMs = nowMs;
            e.callsAtReport = e.calls;
        }
        return n;
    }

    size_t used() const noexcept { return used_; }
    uint64_t overflow() const noexcept { return overflow_; }

private:
    Entry entries_[kEntries];
    size_t used_ = 0;
    uint64_t overflow_ = 0;
};

// ---- the line texts ---------------------------------------------------------------------------------------------------------------------
// One view: "lod view: first|every 5s caller=0x... size=WxH slot=N primary=0|1 A=... B=... calls=N | scene RxR output OxO key on|off
// factor F". scene and output are the flat runtime's own measurement (0x0 until it has one).
inline int formatView(char* buf, size_t cap, const Due& d, uint32_t renderW, uint32_t renderH, uint32_t outputW, uint32_t outputH,
                      bool keyOn) noexcept {
    const Observation& o = d.entry.last;
    return std::snprintf(buf, cap,
                         "lod view: %s caller=0x%X size=%ux%u slot=%u primary=%u A=%.6e B=%.6e calls=%llu | scene %ux%u output %ux%u key %s "
                         "factor %.4f",
                         d.first ? "first" : "every 5s", o.callerRva, o.width, o.height, o.slot, static_cast<unsigned>(o.primary),
                         static_cast<double>(o.a), static_cast<double>(o.b), static_cast<unsigned long long>(d.callsSince), renderW, renderH,
                         outputW, outputH, keyOn ? "on" : "off", static_cast<double>(o.factor));
}

}  // namespace edvr::lodview

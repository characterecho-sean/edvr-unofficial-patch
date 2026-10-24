// The pure half of native on foot's avatar hooks (native_on_foot_hooks.h): the build-334925 identity the build-keyed hooks share, the three
// hooked functions' prologues, the camera controller's mode byte, the avatar dither-fade guard, the avatar component's part table and the
// local-avatar tests, the census text, and the machine code of the callback relay. No Windows, no game and no Config, so
// tools\native_on_foot_hooks_test drives the same code the DLL compiles.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

namespace edvr {
namespace avh {

// ---- identity: build 334925 ---------------------------------------------------------------------------------------------
constexpr uintptr_t kControllerRva = 0x2DF3EA0;   // VesselCameraMountControl's update (rcx = the controller), vtable only
constexpr uintptr_t kAvatarFadeRva = 0x3DD8C60;   // AvatarModelComponent's per-frame dither fade (rcx = the component, void): the head hiding's hook
constexpr uintptr_t kFadeModeRva = 0x5EA0C78;     // int, .data: the dither fade's mode, -1 = auto (the game's own); when not -1 every avatar gets
                                                  // enabled = (mode != 0) and amount = the float at kFadeAmountRva
constexpr uintptr_t kFadeAmountRva = 0x60211F8;   // float, .data (zero-initialised, so 0)
constexpr uint32_t kExpectedTimestamp = 1792704584u;   // the PE TimeDateStamp and SizeOfImage the other build-keyed hooks use
constexpr uint32_t kExpectedImageSize = 104906752u;

constexpr size_t kControllerPrologueBytes = 15;
// `mov [rsp+8],rbx; mov [rsp+10h],rbp; mov [rsp+18h],rsi`. CodeHook steals the first (5 bytes).
inline constexpr uint8_t kControllerPrologue[kControllerPrologueBytes] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18};

constexpr size_t kAvatarFadePrologueBytes = 16;
// `mov r11,rsp; push rbx; push rsi; push rdi; sub rsp,110h; mov rax,[rip+disp32]`: CodeHook steals 5 (`mov r11,rsp`, `push rbx`, `push rsi`), no
// rip-relative byte among them (the disp32 is past byte 16, which is not compared). The entry is 32-byte aligned (32 mod 64 on build 334925).
inline constexpr uint8_t kAvatarFadePrologue[kAvatarFadePrologueBytes] = {
    0x4C, 0x8B, 0xDC, 0x53, 0x56, 0x57, 0x48, 0x81, 0xEC, 0x10, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x05};

// The skeleton interface's FindJoint(const char* name) -> u16 (0xFFFF = none), shared by RuntimeRigComponent and AnimatedObject (rcx = the
// interface, rdx = the name): native on foot hooks it (callback relay, original first) to learn which interfaces the game attaches the local
// player's avatars to. `mov [rsp+8],rbx` is the first instruction, five bytes with no rip-relative displacement: CodeHook steals exactly five.
constexpr uintptr_t kFindJointRva = 0xFDEBB0;
constexpr size_t kFindJointPrologueBytes = 16;
inline constexpr uint8_t kFindJointPrologue[kFindJointPrologueBytes] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0xFA};

// The camera controller (VesselCameraMountControl). The controller copies its shared record's +0x30 into +0x3E0 at the start of an update
// and back at its end, so the byte read after the update is the camera suite's mode for the frame (native_on_foot.h, F93d).
constexpr uint32_t kOffCtlPending = 0x3E1;     // a byte: the suite's pending-retry flag (read with the mode, never written)
constexpr uint32_t kOffCtlMode = 0x3E0;        // a byte (= the shared record's +0x30): the camera mode
inline const char* modeText(uint32_t mode) {
    switch (mode) {
        case 0: return "closed";
        case 1: case 2: return "suite open on a preset";
        case 3: return "free camera";
        case 4: return "free camera, relative lock";
        case 5: return "detached (world lock)";
        case 6: return "variant";
        default: return "unknown";
    }
}

// ---- the avatar fade global ---------------------------------------------------------------------------------------------------
// With the view inside the commander, the game's dither fade (AvatarModelComponent's per-frame update) fades the whole avatar away (F2).
// The fade has a mode global, -1 = auto; when it is not -1 every avatar gets enabled = (mode != 0), amount = a float that is 0. So 0 is the
// plain opaque draw. While native stereo's body is on foot, write 0, but only if it reads -1 first (anything else means someone else owns
// it); put -1 back only if it still reads 0, when the body ends (the guard's camera input is always closed for it). Also restored at unload.
enum class FadeEvent : uint32_t { None = 0, Written, Foreign, Restored, RestoredAtUnload, ChangedUnderUs, Unreadable, WriteFailed, RestoreFailed };
constexpr int32_t kFadeAuto = -1;
// The guard is general: a claim wants the global while `active` and `placed` hold and gives it back once `session` is over and the camera
// (`ctlMode`) is closed or detached. Native stereo's body passes body, body, body and 0.
struct FadeIn {
    bool active = false;      // the claimant is on
    bool placed = false;      // the claimant wants the opaque draw now
    bool session = false;     // the claimant's span is still on
    uint8_t ctlMode = 0;      // the camera controller's mode byte (0: closed)
    bool readOk = false;      // the global could be read
    int32_t value = 0;
    bool unload = false;      // the DLL is going away
};
struct FadeStep {
    enum class Act : uint8_t { None, Write, Restore };
    Act act = Act::None;
    FadeEvent ev = FadeEvent::None;
    int32_t seen = 0;
};
class FadeGuard {
public:
    bool ours() const { return m_ours; }
    void reset() { *this = FadeGuard(); }

    FadeStep step(const FadeIn& in) {
        FadeStep s;
        s.seen = in.value;
        if (in.unload) {
            if (m_ours) {
                if (in.readOk && in.value == 0) {
                    s.act = FadeStep::Act::Restore;
                    s.ev = FadeEvent::RestoredAtUnload;
                } else if (in.readOk) {
                    s.ev = FadeEvent::ChangedUnderUs;
                }
                m_ours = false;
            }
            return s;
        }
        if (m_ours) {
            const bool sessionOver = !in.session;
            const bool detachedRelease = !in.placed && (in.ctlMode == 5 || in.ctlMode == 6);
            const bool cameraGone = in.ctlMode == 0 || in.ctlMode == 5 || in.ctlMode == 6;
            if ((sessionOver || detachedRelease) && cameraGone) {
                if (!in.readOk) {
                    if (!m_unreadableNoted) {
                        m_unreadableNoted = true;
                        s.ev = FadeEvent::Unreadable;
                    }
                } else if (in.value == 0) {
                    s.act = FadeStep::Act::Restore;
                    s.ev = FadeEvent::Restored;
                    m_ours = false;
                } else {
                    s.ev = FadeEvent::ChangedUnderUs;   // someone else changed it: not ours to put back
                    m_ours = false;
                }
            }
            return s;
        }
        if (!(in.active && in.placed)) {
            m_refused = false;   // the next claim may try again
            m_unreadableNoted = false;
            return s;
        }
        if (m_refused) return s;
        if (!in.readOk) {
            if (!m_unreadableNoted) {
                m_unreadableNoted = true;
                s.ev = FadeEvent::Unreadable;
            }
            return s;
        }
        if (in.value == kFadeAuto) {
            s.act = FadeStep::Act::Write;
            s.ev = FadeEvent::Written;
            m_ours = true;
        } else {
            s.ev = FadeEvent::Foreign;
            m_refused = true;
        }
        return s;
    }
    // The caller could not do what step() said: a write that failed leaves it not ours and the claim refused; a restore that
    // failed stays ours so the next frame tries again.
    void writeFailed() { m_ours = false; m_refused = true; }
    void restoreFailed() { m_ours = true; }

private:
    bool m_ours = false;
    bool m_refused = false;
    bool m_unreadableNoted = false;
};

// One producer (a hook thread, serialised by the glue), one consumer (the frame thread). The producer never waits: a full ring
// drops the item and counts it.
template <typename T, size_t N>
class Ring {
public:
    bool push(const T& e) {
        const uint64_t head = m_head.load(std::memory_order_relaxed);
        const uint64_t tail = m_tail.load(std::memory_order_acquire);
        if (head - tail >= N) {
            m_lost.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        m_items[head % N] = e;
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }
    bool take(T* out) {
        const uint64_t tail = m_tail.load(std::memory_order_relaxed);
        const uint64_t head = m_head.load(std::memory_order_acquire);
        if (tail == head) return false;
        *out = m_items[tail % N];
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }
    uint64_t lost() const { return m_lost.load(std::memory_order_relaxed); }

private:
    std::atomic<uint64_t> m_head{0};
    std::atomic<uint64_t> m_tail{0};
    std::atomic<uint64_t> m_lost{0};
    T m_items[N];
};

// ---- text ----------------------------------------------------------------------------------------------------------------
// Every line the hooks' frame thread writes about the hooks starts with this, so a flight log is read with one grep.
inline const char* prefix() { return "avatar hooks:"; }
constexpr size_t kLineBytes = 1100;   // under Log's 1200-byte line

struct Line {
    char* buf;
    size_t cap;
    size_t n = 0;
    Line(char* b, size_t c) : buf(b), cap(c) { if (cap) buf[0] = 0; }
    void put(const char* fmt, ...) {
        if (n + 1 >= cap) return;
        va_list ap;
        va_start(ap, fmt);
        const int w = std::vsnprintf(buf + n, cap - n, fmt, ap);
        va_end(ap);
        if (w < 0) return;
        if (static_cast<size_t>(w) >= cap - n) n = cap - 1;
        else n += static_cast<size_t>(w);
    }
};

using SinkFn = void (*)(void* ctx, const char* line);
struct Sink {
    SinkFn fn = nullptr;
    void* ctx = nullptr;
    void operator()(const char* line) const { if (fn) fn(ctx, line); }
};

// ---- hiding the head parts of the local third-person avatar ---------------------------------------------------------------------
// The avatar (AvatarModelComponent, "AMC") keeps 54 parts at AMC+0x11B0, 0x680 bytes each (the part enum is the index; the names are the table at
// +0x5E9F820). A part has up to four instances (variants k = 0..3 at entry+0x10+8k) and, per instance, a VIEW MASK qword at entry+0x5F0+8k that the game's
// visibility job zeroes and ORs every frame and the submit job (the one that calls the dither fade, 0x3DD8C60, at 0x3DA4A5B) reads right after the fade:
// an instance whose mask is zero is skipped (cmp qword ptr [r14],0 at 0x3DA4CFE). So a zero written after the fade returns removes the instance from every
// view for that frame, race-free (the same thread), and nothing is restored: the game rewrites the masks next frame. (NOTES_p2.txt.)
constexpr uintptr_t kPartNameTableRva = 0x5E9F820;
constexpr uint32_t kPartCount = 54;
constexpr uint32_t kPartStride = 0x680, kPartVariants = 4;
constexpr uint32_t kOffAmcParts = 0x11B0;          // the part entry: AMC + this + idx*stride (the instance and mask offsets below are inside it)
constexpr uint32_t kOffPartClipMod = 0x610;        // F19: the entry's "pVertexClipAmount" modifier pointer (set by the item-apply function 0x3DDCAC0)
constexpr uint32_t kClipModAmount = 0x8, kClipModEnable = 0xC, kClipModDirty = 0xD;   // the modifier's float amount, enable byte, dirty byte (F19)
constexpr uint32_t kOffAmcInstances = 0x11C0;      // AMC + this + idx*stride + 8k: instance k of part idx (0 = none)
constexpr uint32_t kOffAmcMasks = 0x17A0;          // ...the view mask of instance k
constexpr uint32_t kOffAmcPartFlags = 0x1814;      // a byte: 0x10 = the part is EXCLUDED, |5 set when built
constexpr uint32_t kOffAmcPartPending = 0x1818;    // an int32: the pending item (-1 once consumed)
constexpr uint32_t kOffAmcPartCurrent = 0x181C;    // an int32: the current item
constexpr uint32_t kOffAmcPartState = 0x1820;      // an int32: 0 never visible, 1 visible, 2 shadow-ish views only, 3 main views only
constexpr uint32_t kOffAmcAvatarPoseId = 0x260, kOffAmcAvatarPose = 0x268;   // the AvatarPose handle slot: id dword (-1 once resolved) and the resolved pointer
constexpr uint32_t kOffAmcParams = 0x50, kOffParamsMode = 0x14;              // AMC+0x50 -> creation params; +0x14 = 3 for the third-person view
constexpr uint32_t kOffAmcViewFilter = 0x2A8, kOffAmcAlpha = 0x314, kOffAmcContext = 0x334, kOffAmcFlagBytes = 0x320;
constexpr uint32_t kAvatarPoseToSkeleton = 0x30;   // RuntimeRigComponent exports the skeleton interface at RR+0xB8 and AvatarPose at RR+0xE8
constexpr uint32_t kAmcBytes = 0x1D100;            // the highest byte read: the last mask qword of part 53, +0x1D040
constexpr uint32_t kHeadPartCount = 12;
constexpr uint32_t kHeadParts[kHeadPartCount] = {0, 2, 3, 4, 5, 12, 13, 15, 20, 25, 26, 32};   // Head Eyes Helmet SkullCap Hair Beard Teeth Hat EyeWear EVASuit_Helmet EVASuit_Eyewear EVASuit_Gear_Head
constexpr uint32_t kMaxHideFaults = 3;             // guarded accesses of the AMC that may fault before head hiding stands down for the session
constexpr uint64_t kAmcNoMatchCalls = 300;         // fade calls without one matching AMC before the log says nothing is being hidden
inline constexpr const char* kPartNames[kPartCount] = {
    "Head", "Body", "Eyes", "Helmet", "SkullCap", "Hair", "BodyAccessory_01", "BodyAccessory_02", "BodyAccessory_03", "BodyAccessory_04", "BodyAccessory_05",
    "PortraitBackground", "Beard", "Teeth", "ShoulderPads", "Hat", "FlightSuit_Legs", "FlightSuit_Torso", "OuterClothing_Legs", "OuterClothing_Torso", "EyeWear",
    "FlightSuit_Arms", "FlightSuit_Hands", "OuterClothing_Arms", "EVASuit", "EVASuit_Helmet", "EVASuit_Eyewear", "EVASuit_Torso", "EVASuit_Arms", "EVASuit_Hands",
    "EVASuit_Legs", "EVASuit_Feet", "EVASuit_Gear_Head", "EVASuit_Gear_Neck", "EVASuit_Gear_Shoulders", "EVASuit_Gear_UpperArms", "EVASuit_Gear_UpperArmL",
    "EVASuit_Gear_UpperArmR", "EVASuit_Gear_LowerArms", "EVASuit_Gear_LowerArmL", "EVASuit_Gear_LowerArmR", "EVASuit_Gear_Chest", "EVASuit_Gear_Waist",
    "EVASuit_Gear_Thighs", "EVASuit_Gear_Knees", "EVASuit_Gear_KneeL", "EVASuit_Gear_KneeR", "EVASuit_Gear_Feet", "EVASuit_Gear_Back", "FirstPersonSkeleton",
    "Shoulder_Left", "Shoulder_Right", "EVASuit_Gear_Arms", "EVASuit_Gear_Legs"};
inline bool isHeadPart(uint32_t idx) {
    for (uint32_t h : kHeadParts)
        if (h == idx) return true;
    return false;
}
// F15 (native on foot, TEMPORARY E1): the first-person body's parts, hidden on the local first-person AMC (mode 2). Looked up by name in the part-name
// table, never by index; -1 when the name is not in the table.
inline constexpr const char* kFpBodyPartNames[3] = {"EVASuit_Arms", "EVASuit_Hands", "FirstPersonSkeleton"};
inline int partIndexByName(const char* name) {
    for (uint32_t i = 0; i < kPartCount; ++i)
        if (std::strcmp(kPartNames[i], name) == 0) return static_cast<int>(i);
    return -1;
}
// The local third-person avatar: its AvatarPose handle is resolved (the id dword reads 0xFFFFFFFF), the resolved pointer minus 0x30 is the skeleton
// interface the game called FindJoint on for the local player's third-person body (site 1), and its creation parameters say third person (mode 3).
inline bool isLocalAmc(uint32_t avatarPoseId, uint64_t avatarPose, uint64_t skeleton, int32_t mode) {
    return avatarPoseId == 0xFFFFFFFFu && skeleton != 0 && avatarPose >= kAvatarPoseToSkeleton && avatarPose - kAvatarPoseToSkeleton == skeleton && mode == 3;
}
// Which FindJoint call is an avatar attach: the caller is one of the two attach sites AND the name is the povCamera literal. Trivial on purpose: the hook sees
// every FindJoint call of every system. 0 = site 1, 1 = site 2, -1 = neither.
constexpr uintptr_t kFindSite1Rva = 0x19B3FD5, kFindSite2Rva = 0x19B4059, kPovNameRva = 0x51FCAD0;
inline int findCaptureSite(uintptr_t returnAddress, uintptr_t name, uintptr_t site1, uintptr_t site2, uintptr_t povName) {
    if (povName == 0 || name != povName) return -1;
    if (returnAddress == site1) return 0;
    if (returnAddress == site2) return 1;
    return -1;
}
// Is a site-2 attach the second half of the same invocation as the thread's last site-1 attach? 0x19B3F40 attaches ONE humanoid's avatars per call: index 1
// (third person) at site 1, then index 0 (first person) at site 2, both from one frame (no push between: the return-address slot is the same address), and only
// the local player has a first-person avatar. A pair is the local avatar's when the site-1 attach is still pending on this thread, sits at the same stack slot,
// and is a different interface. A lone site-1 attach is any humanoid's (an NPC's) and never latches.
inline bool isLocalPair(bool site1Valid, uintptr_t site1Where, uintptr_t site2Where, uint64_t site1Iface, uint64_t site2Iface) {
    return site1Valid && site1Where == site2Where && site1Where != 0 && site1Iface != 0 && site2Iface != 0 && site1Iface != site2Iface;
}

// What a local AMC holds, taken once when it is first seen: which parts have instances, which variants, their state and flags.
struct PartCensus {
    uint8_t idx = 0;
    uint8_t variants = 0;    // bit k: instance k exists
    uint8_t maskBits = 0;    // bit k: instance k's view mask is non-zero right now
    uint8_t flags = 0;       // the part's flag byte (0x10 = excluded)
    int32_t state = 0;
    int32_t pending = 0;
    int32_t current = 0;
};
struct AmcCensus {
    uint64_t amc = 0;
    uint64_t avatarPose = 0;
    uint64_t skeleton = 0;
    int32_t mode = 0;
    int32_t viewFilter = 0;
    float alpha = 0;
    uint32_t context = 0;
    uint8_t flagBytes[4] = {};
    uint32_t count = 0;
    PartCensus part[kPartCount];
};
static_assert(std::is_trivially_copyable<AmcCensus>::value, "the census is copied through a ring");
inline const char* prefixHide() { return "avatar head hide:"; }
// The AMC's world transform, read the way the fade code reads it: rcx = *(AMC+0x208), that object's vtable slot +0x20 returns a matrix whose origin is at +0x30.
constexpr uint32_t kOffAmcTransform = 0x208, kTransformMatrixSlot = 0x20, kMatrixOriginOff = 0x30;
inline void putVariants(Line& o, uint8_t bits) {
    for (uint32_t k = 0; k < kPartVariants; ++k)
        if (bits & (1u << k)) o.put("%u", k);
}
// The census as log lines: a header, then the parts, as many to a line as fit under the log's line length.
inline void formatAmcCensus(const AmcCensus& c, const Sink& sink) {
    char line[kLineBytes];
    {
        Line o(line, sizeof(line));
        o.put("%s census of the local third-person avatar AMC 0x%llX (mode %d, AvatarPose 0x%llX minus 0x30 = skeleton 0x%llX): %u of %u parts have instances; "
              "view_filter(+0x2A8)=%d alpha(+0x314)=%.3f context(+0x334)=%u flag_bytes(+0x320..323)=%02X %02X %02X %02X. Per part: idx name variants(k) state "
              "flags pending/current item; HIDE = in the head set, its masks are zeroed while the body is on; m = view mask non-zero now",
              prefixHide(), static_cast<unsigned long long>(c.amc), c.mode, static_cast<unsigned long long>(c.avatarPose),
              static_cast<unsigned long long>(c.skeleton), c.count, kPartCount, c.viewFilter, static_cast<double>(c.alpha), c.context, c.flagBytes[0],
              c.flagBytes[1], c.flagBytes[2], c.flagBytes[3]);
        sink(line);
    }
    Line o(line, sizeof(line));
    bool open = false;
    for (uint32_t i = 0; i < c.count && i < kPartCount; ++i) {
        const PartCensus& p = c.part[i];
        char item[200];
        Line it(item, sizeof(item));
        it.put("[%u %s v=", p.idx, p.idx < kPartCount ? kPartNames[p.idx] : "?");
        putVariants(it, p.variants);
        it.put(" st=%d fl=0x%02X item=%d/%d m=", p.state, p.flags, p.pending, p.current);
        putVariants(it, p.maskBits);
        it.put("%s%s]", p.idx < kPartCount && isHeadPart(p.idx) ? " HIDE" : "", (p.flags & 0x10) != 0 ? " EXCLUDED" : "");
        if (open && o.n + std::strlen(item) + 2 > 900) {
            sink(line);
            o = Line(line, sizeof(line));
            open = false;
        }
        if (!open) {
            o.put("%s census parts:", prefixHide());
            open = true;
        }
        o.put(" %s", item);
    }
    if (open) sink(line);
}

// ---- the relays' machine code ------------------------------------------------------------------------------------------------
// This DLL loads more than two gigabytes from the game, so a five-byte E9 cannot reach a C++ replacement. CodeHook patches the
// target with an E9 to a relay placed within two gigabytes, and the relay does the rest. The relay is copied from
// pose_reader_watch.cpp / object_record_writer_hook.cpp's pattern (grep kRelayBytes); RAX, R11 and the flags are volatile and
// none of the hooked functions reads them on entry.
//
// THE CALLBACK RELAY (44 bytes): a gate in memory, a C++ callback, the trampoline. Each hook that uses it (here the controller's update,
// the dither fade and FindJoint; native_on_foot.cpp's and skin_entity_hook.cpp's own) has a gate and a callback of its own.
//   mov rax,&gate; cmp qword ptr [rax],0; je original; jmp [callback]; original: jmp [trampoline]
constexpr size_t kCallbackRelayBytes = 44, kCallbackRelayTrampolineAt = 36, kCallbackRelayGateAt = 2, kCallbackRelayCallbackAt = 22;
inline void buildCallbackRelay(uint8_t* code, const void* gate, const void* callback) {
    const uint8_t body[kCallbackRelayBytes] = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                     //  0: mov rax, imm64 (&gate)
        0x48, 0x83, 0x38, 0x00,                                 // 10: cmp qword ptr [rax], 0
        0x74, 0x0E,                                             // 14: je original (30)
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,        // 16: jmp qword ptr [rip+0]; dq callback   [at 22]
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};       // 30: original: jmp qword ptr [rip+0]; dq trampoline [at 36]
    std::memcpy(code, body, sizeof(body));
    const uintptr_t g = reinterpret_cast<uintptr_t>(gate), c = reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code + kCallbackRelayGateAt, &g, 8);
    std::memcpy(code + kCallbackRelayCallbackAt, &c, 8);
}

}  // namespace avh
}  // namespace edvr

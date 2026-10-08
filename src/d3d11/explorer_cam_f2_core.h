// The F2 instruments' pure half (advanced.explorer_cam_probe; docs\design-explorer-cam-free-camera-2026-10-07.md, "Phase 1c").
//
// Three LOG-ONLY observers for the Explorer Cam redesign's flight F2, removed when the arc closes with the probe. Nothing here writes
// the game's memory:
//   I3 pressed  the free-camera activity's three action objects' pressed ints (ToggleRotationLock +0x4F8, FixCameraWorldToggle
//               +0x500, FixCameraRelativeToggle +0x508), on change: which of the player's keys takes the camera to the world lock.
//   I4          the camera controller (VesselCameraMountControl, update 0x2DF14C0): its mode byte (+0x3E0) on every transition, the
//               pressed ints of PhotoCameraToggle (+0x310), ToggleFreeCam (+0x328) and QuitCamera (+0x340) on change, the preset kind
//               (+0x2E8), and a 5 s heartbeat of its call count: does it run on foot with the camera CLOSED? (F5 from first person
//               rests on that; the heartbeat says "idle=no-call-in-window" when it does not.)
//   F  (fade)   the avatar dither fade (AvatarModelComponent's per-frame update, 0x3DD6040): after the original, the component's dither block
//               (comp+0x378 -> +0x90 enabled, +0x120 amount) and eased level (comp+0x380) are read, and a 5 s heartbeat counts the calls, the distinct
//               components, how many had enabled = 1 and the least amount among them. With Explorer Cam holding the fade global at 0, enabled = 1 must
//               read 0; if the body still vanishes with it at 0, the dither was not the cause, and this line proves which.
//   N  (neck)   the commander's eye. The HumanoidEyeComponent's view-point interface (vtable at EliteDangerous64.exe+0x51FCE98) keeps
//               a world-space 4x4 at interface+0x268, driven by the skeleton joint def_c_povCamera_joint, so it should follow stance.
//               Nothing was found that WRITES it, so its READERS are observed: slot +0x20 of that one vtable (`lea rax,[rcx+268h]; ret`,
//               shared by six vtables through identical-code folding, which is why the slot is replaced and the code is not hooked)
//               is swapped for a function that notes the call and returns exactly what the original returns. A call whose return
//               address is 0x1073946 is the first-person camera activity's own read, which only ever reads the LOCAL commander's eye:
//               that interface pointer is kept. Read from the free-camera hook's post-call (paired with that update's pose), and
//               on the frame thread at 1 Hz.
// The decisions and the text of every line are here, with no Windows and no game, so tools\explorer_cam_probe_test drives them.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "explorer_cam_core.h"
#include "explorer_cam_probe_core.h"

namespace edvr {
namespace f2 {

// ---- the neck: identity, build 332841 -----------------------------------------------------------------------------------
constexpr uintptr_t kEyeVtableRva = 0x51FCE98;     // the view-point interface's vtable (.rdata)
constexpr uint32_t kEyeGetterSlot = 4;              // slot +0x20 holds the matrix accessor; slot +0x18 (the origin accessor) calls through it
constexpr uintptr_t kEyeGetterRva = 0xF88330;       // what the slot holds: shared by 6 vtables, so never code-hooked
constexpr size_t kEyeGetterBytes = 8;
inline constexpr uint8_t kEyeGetterCode[kEyeGetterBytes] = {0x48, 0x8D, 0x81, 0x68, 0x02, 0x00, 0x00, 0xC3};   // lea rax,[rcx+268h]; ret
constexpr uintptr_t kLocalEyeSiteRva = 0x1073946;   // the return address after the first-person camera activity's `call [rdx+20h]`
constexpr uint32_t kOffEyeMatrix = 0x268;           // 4x4 world-space, row-major: the eye
constexpr uint32_t kOffEyeB = 0x1A8, kOffEyeC = 0x1E8, kOffEyeD = 0x228;   // three more 4x4, roles unknown
constexpr uint32_t kOffEyeVec = 0x2A8;              // a vec4, role unknown
constexpr uint32_t kEyeInterfaceBytes = 0x2B8;

// The four addresses the neck depends on, all derived from the image base in production and set by hand in the rig.
struct NeckTargets {
    uintptr_t vtable = 0;      // the vtable array
    uintptr_t slot = 0;        // &vtable[4]
    uintptr_t getter = 0;      // what the slot must hold, and where the 8 code bytes must be
    uintptr_t localSite = 0;   // the first-person camera activity's return address
};
inline NeckTargets neckTargetsFromBase(uintptr_t base) {
    NeckTargets t;
    t.vtable = base + kEyeVtableRva;
    t.slot = t.vtable + 8u * kEyeGetterSlot;
    t.getter = base + kEyeGetterRva;
    t.localSite = base + kLocalEyeSiteRva;
    return t;
}
// Does the slot hold what the build-332841 table holds, and is that the accessor's code? Both must be true before one byte is written.
inline bool neckSlotOk(uintptr_t slotValue, const NeckTargets& t) { return slotValue == t.getter && (t.slot & 7u) == 0; }
inline bool neckCodeOk(const uint8_t* code) { return std::memcmp(code, kEyeGetterCode, kEyeGetterBytes) == 0; }
// Is this return address the first-person camera activity's own read?
inline bool isLocalEyeSite(uintptr_t returnAddress, const NeckTargets& t) { return t.localSite != 0 && returnAddress == t.localSite; }
// An interface pointer is live only while its first qword is the one vtable.
inline bool neckInterfaceOk(uintptr_t vptr, const NeckTargets& t) { return vptr != 0 && vptr == t.vtable; }

// ---- the commander's frame from the free camera's two poses ----------------------------------------------------------------
// The free-camera activity holds its pose twice: commander-local at +0x3B0 and world at +0x70, and under the relative lock
// world = local x F + root, rows being axes (row vectors times matrices). With L the local axes (3x3, rows 0-2) and W the world
// axes: W = L x F, so F = L^T x W for an orthonormal L; F's rows are the commander's right, up and forward in world space. The
// root is what is left of the world origin once the local origin has been carried through F:  root = W.origin - L.origin x F.
// A point in world space is in commander-local axes at  local[i] = dot(point - root, F.row(i)).
struct CommanderFrame {
    float f[9] = {};        // F, row-major 3x3
    float root[3] = {};
    bool valid = false;     // both 3x3s finite and their rows near unit length
};
inline bool rowsNearUnit(const float m16[16]) {
    for (int r = 0; r < 3; ++r) {
        const float x = m16[r * 4], y = m16[r * 4 + 1], z = m16[r * 4 + 2];
        const float len2 = x * x + y * y + z * z;
        if (!(len2 > 0.25f && len2 < 4.0f)) return false;   // also false for NaN
    }
    return true;
}
inline CommanderFrame commanderFrame(const float local[16], const float world[16]) {
    CommanderFrame c;
    if (!rowsNearUnit(local) || !rowsNearUnit(world)) return c;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            float sum = 0;
            for (int k = 0; k < 3; ++k) sum += local[k * 4 + i] * world[k * 4 + j];   // (L^T x W)[i][j]
            c.f[i * 3 + j] = sum;
        }
    for (int j = 0; j < 3; ++j) {
        float carried = 0;
        for (int i = 0; i < 3; ++i) carried += local[12 + i] * c.f[i * 3 + j];       // (L.origin x F)[j]
        c.root[j] = world[12 + j] - carried;
    }
    c.valid = true;
    return c;
}
// A world point in the commander's axes: x right, y up, z forward (the +0x3B0 pose's convention).
inline void worldToCommanderLocal(const CommanderFrame& c, const float point[3], float out[3]) {
    for (int i = 0; i < 3; ++i) {
        float sum = 0;
        for (int j = 0; j < 3; ++j) sum += (point[j] - c.root[j]) * c.f[i * 3 + j];
        out[i] = sum;
    }
}

// ---- what the free-camera hook's post-call publishes for the neck ------------------------------------------------------------
struct NeckSample {
    uint64_t activity = 0;
    uint64_t iface = 0;
    uint64_t localSiteCalls = 0;     // the replacement's local-site count when the sample was taken
    uint32_t state = 0;              // the activity's +0x48C
    uint32_t pad = 0;
    float world[16] = {};            // +0x70
    float local[16] = {};            // +0x3B0
    float eye[16] = {};              // interface+0x268
};
static_assert(std::is_trivially_copyable<NeckSample>::value && sizeof(NeckSample) % 4 == 0, "a sample is published as words");

// ---- I3 pressed / I4 controller: what is read, and on what change it is reported --------------------------------------------------
constexpr int32_t kUnreadable = -1;   // an action pointer that is NULL, implausible or faulted
struct Pressed3 {
    int32_t a = 0, b = 0, c = 0;
    bool operator==(const Pressed3& o) const { return a == o.a && b == o.b && c == o.c; }
};
struct ControllerSnap {
    uint64_t controller = 0;
    uint64_t call = 0;
    uint32_t mode = 0;
    int32_t photo = 0, free = 0, quit = 0;
    int32_t presetKind = 0;
    bool sameAs(const ControllerSnap& o) const {
        return mode == o.mode && photo == o.photo && free == o.free && quit == o.quit && presetKind == o.presetKind;
    }
};
struct F2Event {
    uint64_t seq = 0;
    uint64_t object = 0;
    uint64_t call = 0;
    uint32_t kind = 0;          // F2Kind
    uint32_t threadId = 0;
    int32_t a = 0, b = 0, c = 0;      // new values: I3 the three ints; I4 photo, free, quit
    int32_t pa = 0, pb = 0, pc = 0;   // the values before
    uint32_t mode = 0, prevMode = 0;
    int32_t presetKind = 0;
    uint32_t first = 0;
};
static_assert(std::is_trivially_copyable<F2Event>::value, "events are copied through the ring");
enum class F2Kind : uint32_t { None = 0, Pressed3, Controller };

// ---- text ----------------------------------------------------------------------------------------------------------------------
inline const char* prefixPressed() { return "explorer cam probe I3 pressed:"; }
inline const char* prefixI4Armed() { return "explorer cam probe I4 armed:"; }
inline const char* prefixI4Down() { return "explorer cam probe I4 stood down:"; }
inline const char* prefixI4Change() { return "explorer cam probe I4 change:"; }
inline const char* prefixI4Heartbeat() { return "explorer cam probe I4 heartbeat:"; }
inline const char* prefixNArmed() { return "explorer cam probe N armed:"; }
inline const char* prefixNDown() { return "explorer cam probe N stood down:"; }
inline const char* prefixNHeartbeat() { return "explorer cam probe N heartbeat:"; }
inline const char* prefixNMatrix() { return "explorer cam probe N matrix:"; }
inline const char* prefixNLocal() { return "explorer cam probe N local:"; }
inline const char* prefixNStale() { return "explorer cam probe N stale:"; }
inline const char* prefixFArmed() { return "explorer cam probe F armed:"; }
inline const char* prefixFDown() { return "explorer cam probe F stood down:"; }
inline const char* prefixFHeartbeat() { return "explorer cam probe F heartbeat:"; }

// ---- F: the avatar fade counter ---------------------------------------------------------------------------------------------------
struct FadeHeartbeatIn {
    double windowSeconds = 0;
    const char* hook = "armed";
    uint64_t calls = 0, callsWindow = 0;
    size_t distinctComponents = 0;
    uint32_t distinctOverflow = 0;
    uint64_t enabledCalls = 0, enabledWindow = 0;     // calls that read enabled = 1
    size_t componentsEnabledWindow = 0;                // distinct components that read enabled = 1 in the window
    bool haveMin = false;
    float minAmount = 0;                               // the least amount among the enabled ones in the window
    bool globalKnown = false;
    int32_t global = -1;
    uint64_t enabledWhileZero = 0, enabledWhileZeroWindow = 0;   // enabled = 1 read while the global read 0
    uint64_t readFaults = 0, noBlock = 0;
};
inline void formatFadeHeartbeat(char* out, size_t cap, const FadeHeartbeatIn& h) {
    ecp::Line o(out, cap);
    o.put("%s window=%.1fs hook=%s calls=%llu(+%llu) distinct_components=%zu%s enabled_calls=%llu(+%llu) components_enabled=%zu min_amount_enabled=", prefixFHeartbeat(),
          h.windowSeconds, h.hook, static_cast<unsigned long long>(h.calls), static_cast<unsigned long long>(h.callsWindow), h.distinctComponents,
          h.distinctOverflow ? "+" : "", static_cast<unsigned long long>(h.enabledCalls), static_cast<unsigned long long>(h.enabledWindow),
          h.componentsEnabledWindow);
    if (h.haveMin) o.put("%.3f", static_cast<double>(h.minAmount));
    else o.put("none");
    if (!h.globalKnown) o.put(" global=unreadable");
    else o.put(" global=%d (%s)", h.global, h.global == -1 ? "auto, the game's own" : h.global == 0 ? "0, held by Explorer Cam or someone" : "someone else's");
    o.put(" enabled_while_global_zero=%llu(+%llu) no_block=%llu read_faults=%llu", static_cast<unsigned long long>(h.enabledWhileZero),
          static_cast<unsigned long long>(h.enabledWhileZeroWindow), static_cast<unsigned long long>(h.noBlock), static_cast<unsigned long long>(h.readFaults));
    if (std::strcmp(h.hook, "armed") != 0) {
        o.put(" idle=the hook is %s: no call can be seen", h.hook);
    } else if (h.calls == 0) {
        o.put(" idle=no-call-yet (the dither-fade update is not reached)");
    } else if (h.callsWindow == 0) {
        o.put(" idle=no-call-in-window");
    } else if (h.globalKnown && h.global == 0) {
        if (h.enabledWhileZeroWindow > 0)
            o.put(" VERDICT: enabled = 1 was read while the global was 0, so the global does NOT switch the dither off: if the body still vanishes, the "
                  "dither is not the cause or not the only one");
        else
            o.put(" VERDICT: with the global at 0, enabled = 1 read 0 on every call of the window, as it must");
    }
}

inline void putPressed(ecp::Line& o, int32_t v) {
    if (v == kUnreadable) o.put("unreadable");
    else o.put("%d", v);
}
inline void formatF2Event(char* out, size_t cap, const F2Event& e, uint32_t frame) {
    ecp::Line o(out, cap);
    const unsigned long long obj = static_cast<unsigned long long>(e.object);
    if (static_cast<F2Kind>(e.kind) == F2Kind::Pressed3) {
        o.put("%s act=0x%llX thread=%u call=%llu frame=%u %s rotation_lock(+0x4F8)=", prefixPressed(), obj, e.threadId,
              static_cast<unsigned long long>(e.call), frame, e.first ? "first-sight" : "change");
        putPressed(o, e.a);
        o.put(" world_fix(+0x500)=");
        putPressed(o, e.b);
        o.put(" relative_fix(+0x508)=");
        putPressed(o, e.c);
        if (!e.first) {
            o.put(" (was %d/%d/%d)", e.pa, e.pb, e.pc);
        }
    } else if (static_cast<F2Kind>(e.kind) == F2Kind::Controller) {
        o.put("%s ctl=0x%llX thread=%u call=%llu frame=%u %s mode(+0x3E0) %u->%u (%s) photo(+0x310)=", prefixI4Change(), obj, e.threadId,
              static_cast<unsigned long long>(e.call), frame, e.first ? "first-sight" : "change", e.prevMode, e.mode, ecm::modeText(e.mode));
        putPressed(o, e.a);
        o.put(" free(+0x328)=");
        putPressed(o, e.b);
        o.put(" quit(+0x340)=");
        putPressed(o, e.c);
        o.put(" preset_kind(+0x2E8)=%d", e.presetKind);
    } else {
        o.put("explorer cam probe F2: event %u (unnamed)", e.kind);
    }
}

struct I4HeartbeatIn {
    double windowSeconds = 0;
    const char* hook = "armed";      // "armed" | "stood down" | "not tried"
    uint64_t callsWindow = 0, callsTotal = 0;
    uint32_t mode = 0;
    uint64_t lastController = 0;
    uint64_t lost = 0;
};
inline void formatI4Heartbeat(char* out, size_t cap, const I4HeartbeatIn& h) {
    ecp::Line o(out, cap);
    o.put("%s window=%.1fs hook=%s calls=%llu total_calls=%llu +0x3E0=%u (%s) ctl=0x%llX events_lost=%llu", prefixI4Heartbeat(), h.windowSeconds, h.hook,
          static_cast<unsigned long long>(h.callsWindow), static_cast<unsigned long long>(h.callsTotal), h.mode, ecm::modeText(h.mode),
          static_cast<unsigned long long>(h.lastController), static_cast<unsigned long long>(h.lost));
    if (std::strcmp(h.hook, "armed") != 0) o.put(" idle=the hook is %s: no call can be seen", h.hook);
    else if (h.callsTotal == 0) o.put(" idle=no-call-yet (the controller's update is not reached, or the hook is not)");
    else if (h.callsWindow == 0) o.put(" idle=no-call-in-window (the controller is not called with the camera in this state)");
}

struct NeckHeartbeatIn {
    double windowSeconds = 0;
    const char* hook = "armed";
    uint64_t calls = 0, callsWindow = 0;
    uint64_t localSiteCalls = 0, localSiteWindow = 0;
    size_t distinctInterfaces = 0;
    uint32_t distinctOverflow = 0;
    bool localEyeSet = false;
    uint64_t localEye = 0;
    int64_t localSiteAgeMs = -1;     // -1: never
    bool stale = false;
};
inline void formatNeckHeartbeat(char* out, size_t cap, const NeckHeartbeatIn& h) {
    ecp::Line o(out, cap);
    o.put("%s window=%.1fs hook=%s calls=%llu(+%llu) local_site_calls=%llu(+%llu) distinct_interfaces=%zu%s local_eye=%s", prefixNHeartbeat(),
          h.windowSeconds, h.hook, static_cast<unsigned long long>(h.calls), static_cast<unsigned long long>(h.callsWindow),
          static_cast<unsigned long long>(h.localSiteCalls), static_cast<unsigned long long>(h.localSiteWindow), h.distinctInterfaces,
          h.distinctOverflow ? "+" : "", h.localEyeSet ? "set" : "not set");
    if (h.localEyeSet) o.put(" (0x%llX)", static_cast<unsigned long long>(h.localEye));
    if (h.localSiteAgeMs < 0) o.put(" local_site_last_seen=never");
    else o.put(" local_site_last_seen=%.1fs ago", static_cast<double>(h.localSiteAgeMs) / 1000.0);
    if (std::strcmp(h.hook, "armed") != 0) o.put(" idle=the hook is %s: no call can be seen", h.hook);
    else if (h.calls == 0) o.put(" idle=no-call-yet (nothing has read the eye matrix since the swap)");
    else if (h.callsWindow == 0) o.put(" idle=no-call-in-window");
}

inline void putMatrixRows(ecp::Line& o, const float m[16]) {
    o.put("origin=(%.3f,%.3f,%.3f) rows=[(%.3f,%.3f,%.3f)(%.3f,%.3f,%.3f)(%.3f,%.3f,%.3f)]", m[12], m[13], m[14], m[0], m[1], m[2], m[4], m[5], m[6],
          m[8], m[9], m[10]);
}
inline void formatNeckMatrix(char* out, size_t cap, const char* name, uintptr_t iface, const float m[16], const char* phase) {
    ecp::Line o(out, cap);
    o.put("%s phase=%s iface=0x%llX %s ", prefixNMatrix(), phase, static_cast<unsigned long long>(iface), name);
    putMatrixRows(o, m);
}
inline void formatNeckVec(char* out, size_t cap, uintptr_t iface, const float v[4], const char* phase) {
    ecp::Line o(out, cap);
    o.put("%s phase=%s iface=0x%llX vec4(+0x2A8)=(%.3f,%.3f,%.3f,%.3f)", prefixNMatrix(), phase, static_cast<unsigned long long>(iface), v[0], v[1], v[2],
          v[3]);
}
// The stance test: the +0x268 eye origin in the commander's own axes, from the activity's two poses read in the same update.
inline void formatNeckLocal(char* out, size_t cap, const NeckSample& s, const CommanderFrame& c, const float eyeLocal[3], int64_t ageMs) {
    ecp::Line o(out, cap);
    o.put("%s act=0x%llX state=%u iface=0x%llX sample_age=%lldms eye_world(+0x268)=(%.3f,%.3f,%.3f) ", prefixNLocal(),
          static_cast<unsigned long long>(s.activity), s.state, static_cast<unsigned long long>(s.iface), static_cast<long long>(ageMs), s.eye[12],
          s.eye[13], s.eye[14]);
    if (!c.valid) {
        o.put("commander_frame=invalid (the activity's +0x3B0 or +0x70 rows are not near unit length): no local figure");
        return;
    }
    o.put("root=(%.3f,%.3f,%.3f) eye_in_commander_local(right,up,forward)=(%.3f,%.3f,%.3f) commander_frame_rows=[(%.3f,%.3f,%.3f)(%.3f,%.3f,%.3f)"
          "(%.3f,%.3f,%.3f)] activity_world_origin(+0xA0)=(%.3f,%.3f,%.3f) activity_local_origin(+0x3E0)=(%.3f,%.3f,%.3f); ASSUMPTION: +0x268 is in the "
          "same world frame as the activity's +0x70 (the raw world origins above are printed so that can be checked)",
          c.root[0], c.root[1], c.root[2], eyeLocal[0], eyeLocal[1], eyeLocal[2], c.f[0], c.f[1], c.f[2], c.f[3], c.f[4], c.f[5], c.f[6], c.f[7], c.f[8],
          s.world[12], s.world[13], s.world[14], s.local[12], s.local[13], s.local[14]);
}

// ---- H: the third-person avatar's head joint (build 332841) -----------------------------------------------------------------------
// Where the head is, for the stance test: the local commander's humanoid component HUM (constructor 0x2A90D40) is reached from the free-camera
// activity's cached interface, h = *(activity+0x368) = HUM+0x70 (the offset READ; that +0x368 holds exactly that pointer is INFERRED, so HUM's
// own vtable is checked before anything else is read). HUM+0x178 is the third-person avatar's EntityRef slot (INFERRED), +0x170 the first-person's.
// A slot is an EntityRef*: live when ER != 0 and *(int32*)(ER+0xC0) >= 5, the entity at *(void**)(ER+0xC8). The entity's component container
// (entity+8) answers a lookup by type id (a GAME CALL: container.vtable[0](container, id), id = the u32 at 0x5F2866C), which returns the skeleton
// interface: RuntimeRigComponent's (RR, vtable 0x559CF90) or AnimatedObject's (AO, vtable 0x517DC20). Their slots (all READ, each address
// checked on the exe by the overseer and again here): +0x18 GetPoseData() -> P (the u16 at P+0 is the joint count), +0x30 FindJoint(const char*)
// -> u16 (0xFFFF none), +0x48 GetJointTransform(idx, float[16]*) = the joint's world matrix (+0x58's result x the entity transform), +0x58
// GetJointModelMatrix(idx, float[16]*) = avatar-root relative. ARGUMENTS (disassembled, 0x43F6EB0 / 0xFDDD10 / 0x43F7180 / 0xFDDEB0): rcx = the
// interface, edx = the joint index as a u32 (`mov edi,edx` / `mov esi,edx`), r8 = a 64-byte buffer (four unaligned 16-byte stores), void return.
// RR's +0x58 reserves a 0x3080-byte frame through __chkstk and takes CRITICAL_SECTION iface+0x348 (= RR+0x400); +0x48 calls +0x58 itself, so it is
// never the cheaper of the two.
constexpr uint32_t kOffActivityHum = 0x368;
constexpr uint32_t kHumFromInterface = 0x70;
constexpr uintptr_t kHumVtableRva = 0x5309EB8;
constexpr uint32_t kOffHumThirdPerson = 0x178, kOffHumFirstPerson = 0x170;
constexpr uint32_t kOffErState = 0xC0, kOffErEntity = 0xC8;
constexpr int32_t kErLive = 5;
constexpr uint32_t kOffEntityContainer = 8;
constexpr uintptr_t kSkeletonIdRva = 0x5F2866C;
constexpr uintptr_t kRrVtableRva = 0x559CF90, kAoVtableRva = 0x517DC20;
constexpr uint32_t kHeadSlots = 4;                                                    // the slots used: +0x18, +0x30, +0x48, +0x58
constexpr uint32_t kHeadSlotIndex[kHeadSlots] = {3, 6, 9, 11};
constexpr uintptr_t kRrFunctions[kHeadSlots] = {0x43FB900, 0xFDDB10, 0x43F7180, 0x43F6EB0};
constexpr uintptr_t kAoFunctions[kHeadSlots] = {0xFDE310, 0xFDDB10, 0xFDDEB0, 0xFDDD10};
constexpr uint32_t kRrCachedFlag = 0x2C0, kAoCachedFlag = 0xF8;                      // iface+: the joint matrices are cached when it is non-zero
constexpr uintptr_t kHeadNameRva = 0x554EE10, kPovNameRva = 0x51F9930;
inline constexpr char kHeadName[] = "def_c_head_joint";
inline constexpr char kPovName[] = "def_c_povCamera_joint";
constexpr uint32_t kNoJoint = 0xFFFF;
constexpr uint32_t kHeadIntervalMs = 1000;                                            // at most one evaluation a second: the calls take a lock

enum class HeadKind : uint8_t { None = 0, Runtime = 1, Animated = 2 };
enum class HeadSlotState : uint8_t { NotTried = 0, Resolved, NoHum, NoRef, NotLive, NoEntity, NoIface };
enum class HeadWhy : uint32_t {
    None = 0,
    BuildUnknown,       // the exe is not build 332841
    LiteralMismatch,    // a joint-name literal in the exe is not what it should be (a = 1 head, 2 pov)
    HumVtable,          // *(HUM) is not HUM's vtable (a = found, b = expected, c = h)
    ContainerVtable,    // the entity's container vtable or its slot 0 is not inside the image (a = vtable, b = slot 0)
    IfaceVtable,        // the skeleton interface's vtable is neither RR's nor AO's (a = found, b = slot index, c = iface)
    SlotMismatch,       // a slot does not hold the expected function (a = slot byte offset, b = found, c = expected)
    Fault,              // a read or a call faulted (a = HeadStage)
};
enum class HeadStage : uint32_t { ReadActivity = 1, ReadHum, ReadSlot, ReadEntity, Lookup, ReadInterface, PoseCall, FindCall, ModelCall, WorldCall, ReadFrame };

struct HeadTargets {
    uintptr_t base = 0;
    size_t imageSize = 0;
    uintptr_t humVtable = 0, skeletonId = 0, rrVtable = 0, aoVtable = 0, headName = 0, povName = 0;
    uintptr_t rrFn[kHeadSlots] = {}, aoFn[kHeadSlots] = {};
    bool inImage(uintptr_t p) const { return base != 0 && p >= base && p < base + imageSize; }
};
inline HeadTargets headTargetsFromBase(uintptr_t base, size_t imageSize) {
    HeadTargets t;
    t.base = base;
    t.imageSize = imageSize;
    t.humVtable = base + kHumVtableRva;
    t.skeletonId = base + kSkeletonIdRva;
    t.rrVtable = base + kRrVtableRva;
    t.aoVtable = base + kAoVtableRva;
    t.headName = base + kHeadNameRva;
    t.povName = base + kPovNameRva;
    for (uint32_t i = 0; i < kHeadSlots; ++i) {
        t.rrFn[i] = base + kRrFunctions[i];
        t.aoFn[i] = base + kAoFunctions[i];
    }
    return t;
}
inline HeadKind headKindOfVtable(uint64_t vptr, const HeadTargets& t) {
    if (vptr == 0) return HeadKind::None;
    if (vptr == t.rrVtable) return HeadKind::Runtime;
    if (vptr == t.aoVtable) return HeadKind::Animated;
    return HeadKind::None;
}
// The function slot i (0 pose, 1 find, 2 world, 3 model) must hold for this kind.
inline uintptr_t headExpectedFunction(HeadKind k, uint32_t i, const HeadTargets& t) {
    if (i >= kHeadSlots) return 0;
    return k == HeadKind::Runtime ? t.rrFn[i] : k == HeadKind::Animated ? t.aoFn[i] : 0;
}
inline uintptr_t headVtableOf(HeadKind k, const HeadTargets& t) { return k == HeadKind::Runtime ? t.rrVtable : k == HeadKind::Animated ? t.aoVtable : 0; }
inline uint32_t headCachedFlagOffset(HeadKind k) { return k == HeadKind::Runtime ? kRrCachedFlag : kAoCachedFlag; }
// The checks that come before a call: the vtable pointer is the kind's and every slot used holds exactly the expected function. 0 = fine,
// 1 = the vtable differs, 2 = slot i differs (slotOut = its index, foundOut = what it holds).
inline int headVerifySlots(uint64_t vptr, const uint64_t found[kHeadSlots], HeadKind k, const HeadTargets& t, uint32_t* slotOut, uint64_t* foundOut) {
    if (k == HeadKind::None || vptr != headVtableOf(k, t)) return 1;
    for (uint32_t i = 0; i < kHeadSlots; ++i)
        if (found[i] != headExpectedFunction(k, i, t)) {
            *slotOut = i;
            *foundOut = found[i];
            return 2;
        }
    return 0;
}

// One skeleton interface's reading: the state of the slot, what was found, and the four matrices.
struct HeadSlot {
    uint64_t er = 0, entity = 0, iface = 0, lookupRva = 0;
    int32_t erState = 0;
    uint8_t state = 0;       // HeadSlotState
    uint8_t kind = 0;        // HeadKind
    uint8_t cached = 0;      // the joint-matrix cache flag
    uint8_t have = 0;        // bit 0 head model, 1 pov model, 2 head world, 3 pov world
    uint16_t joints = 0, headIdx = 0xFFFF, povIdx = 0xFFFF, pad = 0;
    float model[2][16] = {};   // +0x58: [0] head, [1] pov
    float world[2][16] = {};   // +0x48
};
struct HeadSample {
    uint64_t activity = 0, hum = 0, h = 0, steps = 0;
    float local[16] = {};      // the activity's +0x3B0
    float actWorld[16] = {};   // the activity's +0x70
    HeadSlot slot[2];          // [0] third-person, [1] first-person
};
static_assert(std::is_trivially_copyable<HeadSample>::value && sizeof(HeadSample) % 4 == 0 && sizeof(HeadSlot) % 4 == 0, "a sample is published as words");
// Why H stopped for the session: written once by the hook thread, said once by the tick.
struct HeadDown {
    HeadWhy why = HeadWhy::None;
    uint32_t slot = 0;
    uint64_t a = 0, b = 0, c = 0;
};
// The time the calls took, in microseconds, over a window and over the session.
struct HeadTiming {
    uint32_t n = 0, minUs = 0, maxUs = 0, allMaxUs = 0;
};

inline const char* prefixHArmed() { return "explorer cam probe H armed:"; }
inline const char* prefixHDown() { return "explorer cam probe H stood down:"; }
inline const char* prefixHSlot() { return "explorer cam probe H slot:"; }
inline const char* prefixHJoints() { return "explorer cam probe H joints:"; }
inline const char* prefixHHeartbeat() { return "explorer cam probe H heartbeat:"; }
inline const char* headKindName(uint8_t k) { return k == 1 ? "RR (RuntimeRigComponent)" : k == 2 ? "AO (AnimatedObject)" : "unknown"; }
inline const char* headSlotName(int i) { return i == 0 ? "third-person(HUM+0x178)" : "first-person(HUM+0x170)"; }
inline const char* headStateText(uint8_t s) {
    switch (static_cast<HeadSlotState>(s)) {
        case HeadSlotState::Resolved: return "resolved";
        case HeadSlotState::NoHum: return "waiting: the activity has no humanoid component cached yet (+0x368 is 0)";
        case HeadSlotState::NoRef: return "waiting: the slot's EntityRef is null";
        case HeadSlotState::NotLive: return "not live: the EntityRef's state (ER+0xC0) is below 5";
        case HeadSlotState::NoEntity: return "waiting: the live EntityRef has no entity (ER+0xC8 is 0)";
        case HeadSlotState::NoIface: return "waiting: the component lookup returned no skeleton interface (the type id at +0x5F2866C is 0, or the entity has none)";
        default: return "not tried";
    }
}
inline const char* headStageName(uint32_t s) {
    switch (static_cast<HeadStage>(s)) {
        case HeadStage::ReadActivity: return "reading the activity's +0x368";
        case HeadStage::ReadHum: return "reading the humanoid component";
        case HeadStage::ReadSlot: return "reading an avatar slot's EntityRef";
        case HeadStage::ReadEntity: return "reading the entity's component container";
        case HeadStage::Lookup: return "the component lookup call";
        case HeadStage::ReadInterface: return "reading the skeleton interface's vtable";
        case HeadStage::PoseCall: return "the GetPoseData call";
        case HeadStage::FindCall: return "the FindJoint call";
        case HeadStage::ModelCall: return "the +0x58 model-matrix call";
        case HeadStage::WorldCall: return "the +0x48 world-matrix call";
        case HeadStage::ReadFrame: return "reading the activity's two poses";
        default: return "an unknown step";
    }
}
inline void formatHeadDown(char* out, size_t cap, const HeadDown& d, const HeadTargets& t) {
    ecp::Line o(out, cap);
    o.put("%s ", prefixHDown());
    const unsigned long long a = static_cast<unsigned long long>(d.a), b = static_cast<unsigned long long>(d.b), c = static_cast<unsigned long long>(d.c);
    switch (d.why) {
        case HeadWhy::BuildUnknown:
            o.put("the game build differs from build 332841 (the exe's identity check refused it), so no address is trusted");
            break;
        case HeadWhy::LiteralMismatch:
            o.put("the game build differs: the %s joint-name literal at EliteDangerous64.exe+0x%llX is not \"%s\"", d.a == 1 ? "head" : "pov",
                  static_cast<unsigned long long>(d.a == 1 ? kHeadNameRva : kPovNameRva), d.a == 1 ? kHeadName : kPovName);
            break;
        case HeadWhy::HumVtable:
            o.put("the first qword of HUM (0x%llX = *(activity+0x368) 0x%llX minus 0x70) is 0x%llX, not the humanoid component's vtable 0x%llX "
                  "(EliteDangerous64.exe+0x%llX): the +0x368 cache is not HUM+0x70 as inferred",
                  static_cast<unsigned long long>(d.c - kHumFromInterface), c, a, b, static_cast<unsigned long long>(kHumVtableRva));
            break;
        case HeadWhy::ContainerVtable:
            o.put("the entity's component container (entity+8) has vtable 0x%llX with slot 0 = 0x%llX, which is not inside the game image [0x%llX, 0x%llX): "
                  "the component lookup is not called",
                  a, b, static_cast<unsigned long long>(t.base), static_cast<unsigned long long>(t.base + t.imageSize));
            break;
        case HeadWhy::IfaceVtable:
            o.put("the skeleton interface 0x%llX (%s slot) has vtable 0x%llX, neither RuntimeRigComponent's 0x%llX nor AnimatedObject's 0x%llX", c,
                  b == 0 ? "third-person" : "first-person", a, static_cast<unsigned long long>(t.rrVtable), static_cast<unsigned long long>(t.aoVtable));
            break;
        case HeadWhy::SlotMismatch:
            o.put("vtable slot +0x%llX holds 0x%llX, not the build-332841 function 0x%llX (RVA +0x%llX)", a, b, c, c - t.base);
            break;
        case HeadWhy::Fault:
            o.put("a read or call faulted while %s (the %s slot)", headStageName(static_cast<uint32_t>(d.a)), d.slot == 0 ? "third-person" : "first-person");
            break;
        default:
            o.put("unnamed reason %u", static_cast<uint32_t>(d.why));
            break;
    }
    o.put(". H will not run this session; nothing was changed.");
}
// Once per change of a slot's identity (kind, interface, joint count, cache flag, indexes) and when it stops being live.
inline void formatHeadSlot(char* out, size_t cap, const HeadSlot& s, int index) {
    ecp::Line o(out, cap);
    o.put("%s slot=%s state=%s er=0x%llX er_state=%d entity=0x%llX", prefixHSlot(), headSlotName(index), headStateText(s.state), static_cast<unsigned long long>(s.er),
          s.erState, static_cast<unsigned long long>(s.entity));
    if (static_cast<HeadSlotState>(s.state) != HeadSlotState::Resolved) return;
    o.put(" container_lookup=EliteDangerous64.exe+0x%llX skeleton_iface=0x%llX kind=%s joint_count=%u joint_cache_flag=%u head_joint(\"%s\")_idx=", static_cast<unsigned long long>(s.lookupRva),
          static_cast<unsigned long long>(s.iface), headKindName(s.kind), s.joints, s.cached, kHeadName);
    if (s.headIdx == kNoJoint) o.put("none");
    else o.put("%u", s.headIdx);
    o.put(" pov_joint(\"%s\")_idx=", kPovName);
    if (s.povIdx == kNoJoint) o.put("none");
    else o.put("%u", s.povIdx);
}
inline void putHeadPoint(ecp::Line& o, const char* name, const float m[16], bool have) {
    if (have) o.put(" %s=(%.3f,%.3f,%.3f)", name, m[12], m[13], m[14]);
    else o.put(" %s=unread", name);
}
// Each second the free camera runs: the two joints' translations from +0x58 (avatar-root relative) and +0x48 (world), and the world ones in the
// commander's own axes (the frame of the activity's two poses, as N does), with the activity's raw +0x70 origin beside them.
inline void formatHeadJoints(char* out, size_t cap, const HeadSample& smp, int index, const CommanderFrame& cf) {
    const HeadSlot& s = smp.slot[index];
    ecp::Line o(out, cap);
    o.put("%s slot=%s kind=%s iface=0x%llX step=%llu", prefixHJoints(), headSlotName(index), headKindName(s.kind), static_cast<unsigned long long>(s.iface),
          static_cast<unsigned long long>(smp.steps));
    putHeadPoint(o, "head_model(+0x58)", s.model[0], (s.have & 1) != 0);
    putHeadPoint(o, "head_world(+0x48)", s.world[0], (s.have & 4) != 0);
    float local[3] = {0, 0, 0};
    if (cf.valid && (s.have & 4)) {
        worldToCommanderLocal(cf, s.world[0] + 12, local);
        o.put(" head_in_commander_local(right,up,forward)=(%.3f,%.3f,%.3f)", local[0], local[1], local[2]);
    } else {
        o.put(" head_in_commander_local=%s", cf.valid ? "unread" : "no frame (the activity's poses are not near unit length)");
    }
    putHeadPoint(o, "pov_model(+0x58)", s.model[1], (s.have & 2) != 0);
    putHeadPoint(o, "pov_world(+0x48)", s.world[1], (s.have & 8) != 0);
    if (cf.valid && (s.have & 8)) {
        worldToCommanderLocal(cf, s.world[1] + 12, local);
        o.put(" pov_in_commander_local(right,up,forward)=(%.3f,%.3f,%.3f)", local[0], local[1], local[2]);
    }
    o.put(" activity_world_origin(+0xA0)=(%.3f,%.3f,%.3f) commander_root=(%.3f,%.3f,%.3f); ASSUMPTION: +0x48 is in the same world frame as the activity's +0x70",
          smp.actWorld[12], smp.actWorld[13], smp.actWorld[14], cf.root[0], cf.root[1], cf.root[2]);
}

struct HeadHeartbeatIn {
    double windowSeconds = 0;
    const char* state = "armed";             // "armed" | "stood down" | "not tried"
    uint64_t steps = 0, stepsWindow = 0;     // evaluations (at most one a second)
    uint64_t calls = 0, callsWindow = 0;     // game calls made: lookup, GetPoseData, FindJoint, +0x58, +0x48
    uint64_t faults = 0;
    const char* last = "none";
    HeadTiming m58, m48, lookup, find;       // microseconds, this window (allMax over the session)
};
inline void putHeadTiming(ecp::Line& o, const char* name, const HeadTiming& t) {
    if (t.n == 0) o.put(" %s_us(n/min/max/session_max)=0/-/-/%u", name, t.allMaxUs);
    else o.put(" %s_us(n/min/max/session_max)=%u/%u/%u/%u", name, t.n, t.minUs, t.maxUs, t.allMaxUs);
}
inline void formatHeadHeartbeat(char* out, size_t cap, const HeadHeartbeatIn& h) {
    ecp::Line o(out, cap);
    o.put("%s window=%.1fs hook=%s steps=%llu(+%llu) game_calls=%llu(+%llu) faults=%llu last=%s", prefixHHeartbeat(), h.windowSeconds, h.state,
          static_cast<unsigned long long>(h.steps), static_cast<unsigned long long>(h.stepsWindow), static_cast<unsigned long long>(h.calls),
          static_cast<unsigned long long>(h.callsWindow), static_cast<unsigned long long>(h.faults), h.last);
    putHeadTiming(o, "model58", h.m58);
    putHeadTiming(o, "world48", h.m48);
    putHeadTiming(o, "lookup", h.lookup);
    putHeadTiming(o, "find", h.find);
    if (std::strcmp(h.state, "armed") != 0) o.put(" idle=the instrument is %s: no step can run", h.state);
    else if (h.steps == 0) o.put(" idle=no-step-yet (the free camera has not been updated since H armed)");
    else if (h.stepsWindow == 0) o.put(" idle=no-step-in-window (the free camera is not running)");
}

}  // namespace f2
}  // namespace edvr

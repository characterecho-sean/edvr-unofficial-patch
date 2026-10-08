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

}  // namespace f2
}  // namespace edvr

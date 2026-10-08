// Explorer Cam's pure half (fix.explorer_cam; docs\design-explorer-cam-free-camera-2026-10-07.md, "Phase 1a: placement built").
//
// Everything here is decisions, bytes and text, with no Windows, no game and no Config, so tools\explorer_cam_test drives the same
// code the DLL compiles: the build-332841 identity, the eye keys' clamp, the sixteen floats written into the free camera's
// commander-local pose, the per-activity state machine, the stale watch, the event ring the hook thread logs through, every log
// line's text, and the machine code of the two relays.
//
// WHAT THE FEATURE DOES (Sean's decision D1, 2026-10-07: exactly these three writes into game memory, nothing else):
//   1. PLACE   Elite's own free camera (FreeCameraActivity, the update at EliteDangerous64.exe+0x1071980) is put at the
//              commander's head: its commander-local pose (+0x3B0) is written before every update while it runs, as the
//              identity rotation and an origin (right, up, forward) in metres from the commander's feet.
//   2. SKIP    the camera's collision step (FUN 0x1091140, called only by that update) is made to return 0 -- "no collision
//              edit" -- for that one activity, by a relay in machine code in front of the game's function.
//   3. LOCK    the game's relative lock is pressed once: the pressed-int of the action object at activity+0x508 is set to 1
//              before ONE update and restored after it returns.
// Nothing else is written: not +0x48C, not the shared record, not +0x470/+0x471/+0x473, not anything replicated.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace edvr {
namespace ecm {

// ---- identity: build 332841 ---------------------------------------------------------------------------------------------
constexpr uintptr_t kFreeCameraRva = 0x1071980;   // FreeCameraActivity's per-frame update, rcx = the activity
constexpr uintptr_t kCollisionRva = 0x1091140;    // the camera's sweep/ray collision step, called only by that update
constexpr uint32_t kExpectedTimestamp = 1788384820u;   // the PE TimeDateStamp and SizeOfImage the other build-keyed hooks use
constexpr uint32_t kExpectedImageSize = 104894464u;

constexpr size_t kFreeCameraPrologueBytes = 28;
// `mov [rsp+20h],rbx; push rbp; push rdi; push r13; push r14; push r15; lea rbp,[rsp-2C0h]; sub rsp,3C0h`. No rip-relative byte in
// it; the instruction boundaries are 5, 6, 7, 9, 11, 13, 21, 28, so CodeHook steals 5.
inline constexpr uint8_t kFreeCameraPrologue[kFreeCameraPrologueBytes] = {
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48,
    0x8D, 0xAC, 0x24, 0x40, 0xFD, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xC0, 0x03, 0x00, 0x00};

constexpr size_t kCollisionPrologueBytes = 26;
// `push rbp; push rbx; push rsi; push rdi; push r12; push r14; push r15; lea rbp,[rsp-1B0h]; sub rsp,250h`, the first push with a
// REX prefix (40 55). Boundaries at 2, 3, 4, 5, 7, 9, 11, 19, 26, so CodeHook steals 5. It takes a FIFTH argument on the stack (a
// byte at [rsp+28h] on entry) and returns al; the entry is 64-byte aligned.
inline constexpr uint8_t kCollisionPrologue[kCollisionPrologueBytes] = {
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D,
    0xAC, 0x24, 0xB0, 0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x50, 0x02, 0x00, 0x00};

// ---- the activity's fields (Phase 0a, "The object") -------------------------------------------------------------------
constexpr uint32_t kOffLocalPose = 0x3B0;      // 16 floats, row-major 4x4: rows 0-2 = right, up, forward; row 3 = origin (x right, y up, z forward)
constexpr uint32_t kOffRelative = 0x470;       // 1 = relative to the commander's frame, 0 = world
constexpr uint32_t kOffRotationLock = 0x471;   // 1 = follow the live frame
constexpr uint32_t kOffPresetPending = 0x473;  // 1 only on the first update, which seeds the pose from the preset
constexpr uint32_t kOffState = 0x48C;          // a byte, mirrored from a shared record every update: NEVER written here
constexpr uint32_t kOffLockAction = 0x508;     // a qword: the relative-lock action object
constexpr uint32_t kOffActionPressed = 0x1C;   // in the action object: an int, nonzero on a press
constexpr uint32_t kActivityBytes = 0x510;     // the highest byte this feature touches is the qword at +0x508

constexpr uint8_t kStateOff = 0, kStateFree = 3, kStateRelativeLock = 4, kStateWorldLock = 5, kStateVariant = 6;

// What one pre-call read of the activity yields.
struct Observed {
    uint8_t relative = 0, rotationLock = 0, presetPending = 0, state = 0;
};
constexpr uint32_t packObserved(const Observed& o) {
    return static_cast<uint32_t>(o.relative) | (static_cast<uint32_t>(o.rotationLock) << 8) |
           (static_cast<uint32_t>(o.presetPending) << 16) | (static_cast<uint32_t>(o.state) << 24);
}

// ---- the eye ------------------------------------------------------------------------------------------------------------
// Metres in the commander's frame from their feet: up, forward (the way they face), right. Live-reloadable.
struct Eye {
    float up = 1.68f, forward = 0.10f, right = 0.0f;
};
constexpr float kEyeUpDefault = 1.68f, kEyeForwardDefault = 0.10f, kEyeRightDefault = 0.0f;
constexpr float kEyeUpMin = 0.5f, kEyeUpMax = 2.5f, kEyeSideMin = -0.5f, kEyeSideMax = 0.5f;

inline float clampOrDefault(float v, float lo, float hi, float fallback) {
    if (!(v == v)) return fallback;   // NaN
    return v < lo ? lo : (v > hi ? hi : v);
}
inline Eye clampEye(float up, float forward, float right) {
    Eye e;
    e.up = clampOrDefault(up, kEyeUpMin, kEyeUpMax, kEyeUpDefault);
    e.forward = clampOrDefault(forward, kEyeSideMin, kEyeSideMax, kEyeForwardDefault);
    e.right = clampOrDefault(right, kEyeSideMin, kEyeSideMax, kEyeRightDefault);
    return e;
}
inline bool sameEye(const Eye& a, const Eye& b) { return a.up == b.up && a.forward == b.forward && a.right == b.right; }

// The sixteen floats written at +0x3B0: rows right (1,0,0), up (0,1,0), forward (0,0,1) and the origin (right, up, forward), each
// row's fourth float exactly as the game held it. `current` is what the game holds now, `out` what goes back.
inline void buildLocalPose(const float current[16], const Eye& eye, float out[16]) {
    const float rows[4][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {eye.right, eye.up, eye.forward}};
    for (int r = 0; r < 4; ++r) {
        out[r * 4 + 0] = rows[r][0];
        out[r * 4 + 1] = rows[r][1];
        out[r * 4 + 2] = rows[r][2];
        out[r * 4 + 3] = current[r * 4 + 3];
    }
}

// ---- the per-activity state machine ---------------------------------------------------------------------------------------
// Why a session ended. The text is in whyText(); the numbers are logged only through it.
enum class Why : uint32_t {
    None = 0,
    LeftFreeCamera = 1,   // +0x48C became 0
    WorldLock = 2,        // +0x48C became 5
    Variant = 3,          // +0x48C became 6
    UnexpectedState = 4,  // +0x48C became something the design does not know
    KeyOff = 5,           // fix.explorer_cam turned off (or the feature stood down)
    Stale = 6,            // the activity was not called for kStaleFrames frames
    Fault = 7,            // a guarded access to the activity faulted
    FaultLimit = 8,       // the fault budget ran out: the feature is off for the session
    NewSession = 9        // +0x473 went back to 1 on an activity that was already placed
};
inline const char* whyText(Why w) {
    switch (w) {
        case Why::LeftFreeCamera: return "left the free camera (+0x48C = 0)";
        case Why::WorldLock: return "the world lock took over (+0x48C = 5)";
        case Why::Variant: return "the variant state took over (+0x48C = 6)";
        case Why::UnexpectedState: return "+0x48C is a state the design does not know";
        case Why::KeyOff: return "fix.explorer_cam turned off (or Explorer Cam stood down)";
        case Why::Stale: return "the activity was not called for 30 frames";
        case Why::Fault: return "a guarded access to the activity faulted";
        case Why::FaultLimit: return "the fault budget ran out; Explorer Cam is off for the session";
        case Why::NewSession: return "a new free-camera session began on the same activity (+0x473 = 1 again)";
        default: return "?";
    }
}

// What the hook thread must do for one call, decided before the original runs.
struct Step {
    bool foreign = false;       // another activity than the tracked one: ignored, nothing to do
    bool entered = false;       // a session began on this call
    bool released = false;      // the session ended on this call; no write, no press
    Why why = Why::None;
    bool pressResult = false;   // the previous call's lock press, resolved by this call's state byte
    uint8_t pressBefore = 0, pressAfter = 0;
    bool write = false;         // write the commander-local pose before the original
    bool placeNow = false;      // ...and this is the session's first write: publish "placed"
    bool alreadyLocked = false; // the first write found +0x48C = 4: the user locked it; no press is needed
    bool press = false;         // set the lock action's pressed-int to 1 before the original, restore it after
    bool waiting = false;       // tracked and wanted, but not yet placeable (+0x473 = 1 or +0x470 = 0)
};

// ONE tracked activity at a time. Hook thread only (the glue serialises calls); a reset from the frame thread arrives as a flag the
// hook thread acts on at its next call.
//
//   Idle     waiting for an entry: a call that sees +0x48C == 3 on a FRESH session. A session is fresh at the start, after the state
//            byte is seen at 0, when +0x473 == 1 (the first update of a new session, whatever stale state the byte held), when a new
//            activity pointer appears and after a reset; it stops being fresh when the byte is seen at 4, 5 or 6 without +0x473 set
//            (a session that began without us -- the user's own lock must not be taken for an entry later) and after a release
//            by the world lock, the variant state, an unknown state or a fault.
//   Placing  from the entry on. The first update with +0x473 == 0 and +0x470 != 0 is the first write; from then on EVERY update
//            while +0x48C is 3 or 4 writes the pose. The lock is pressed once, on the first write if +0x48C is 3.
class Machine {
public:
    enum class Phase : uint8_t { Idle, Placing };

    Phase phase() const { return m_phase; }
    uint64_t activity() const { return m_activity; }
    bool placed() const { return m_placed; }
    bool pressed() const { return m_pressed; }

    void reset() { *this = Machine(); }

    // Decide one call. `wanted` is "Explorer Cam is armed and on".
    Step step(uint64_t activity, const Observed& o, bool wanted) {
        Step s;
        if (m_phase == Phase::Placing) {
            if (activity != m_activity) {
                s.foreign = true;
                return s;
            }
            if (m_pressPending) {
                m_pressPending = false;
                s.pressResult = true;
                s.pressBefore = kStateFree;
                s.pressAfter = o.state;
            }
            if (!wanted) {
                endSession(s, Why::KeyOff, true);
                return s;
            }
            if (o.presetPending != 0 && m_placed) {   // a brand-new session on an activity we thought we still held
                endSession(s, Why::NewSession, true);
                // ...and this very call may start the next one: fall through to the Idle half.
            } else {
                switch (o.state) {
                    case kStateOff: endSession(s, Why::LeftFreeCamera, true); return s;
                    case kStateWorldLock: endSession(s, Why::WorldLock, false); return s;
                    case kStateVariant: endSession(s, Why::Variant, false); return s;
                    case kStateFree:
                    case kStateRelativeLock: break;
                    default: endSession(s, Why::UnexpectedState, false); return s;
                }
                placing(s, o);
                return s;
            }
        }
        // Idle half.
        if (!wanted) return s;
        if (activity != m_seen) {
            m_seen = activity;
            m_fresh = true;
        }
        if (o.presetPending != 0) {
            m_fresh = true;
        } else if (o.state == kStateOff) {
            m_fresh = true;
        } else if (o.state == kStateRelativeLock || o.state == kStateWorldLock || o.state == kStateVariant) {
            m_fresh = false;
        }
        if (o.state != kStateFree || !m_fresh) return s;
        m_phase = Phase::Placing;
        m_activity = activity;
        m_placed = false;
        m_pressed = false;
        m_pressPending = false;
        m_fresh = false;
        s.entered = true;
        placing(s, o);
        return s;
    }

    // The hook thread ends the session itself (a fault, the fault budget): the same bookkeeping as a state-driven release.
    Step abort(Why why) {
        Step s;
        if (m_phase == Phase::Placing) endSession(s, why, false);
        else m_fresh = false;
        return s;
    }

private:
    void placing(Step& s, const Observed& o) {
        const bool ready = o.relative != 0 && o.presetPending == 0;
        if (!ready) {
            s.waiting = true;
            return;
        }
        s.write = true;
        if (!m_placed) {
            m_placed = true;
            s.placeNow = true;
            if (o.state == kStateRelativeLock) {
                m_pressed = true;   // already locked: nothing to press, and nothing to press later
                s.alreadyLocked = true;
            }
        }
        if (!m_pressed && o.state == kStateFree) {
            m_pressed = true;
            m_pressPending = true;
            s.press = true;
        }
    }
    void endSession(Step& s, Why why, bool freshAfter) {
        s.released = true;
        s.why = why;
        m_phase = Phase::Idle;
        m_activity = 0;
        m_placed = false;
        m_pressed = false;
        m_pressPending = false;
        m_fresh = freshAfter;
    }

    Phase m_phase = Phase::Idle;
    uint64_t m_activity = 0;   // the tracked activity while Placing
    uint64_t m_seen = 0;       // the last activity seen while Idle
    bool m_fresh = true;
    bool m_placed = false;
    bool m_pressed = false;
    bool m_pressPending = false;
};

// The frame thread's watch: a session whose activity stops being called is released. One tick per frame boundary.
class StaleWatch {
public:
    static constexpr uint32_t kStaleFrames = 30;
    // `active` = a session is tracked; `calls` = the count of calls from the tracked activity. True = release it now.
    bool tick(bool active, uint64_t calls) {
        if (!active) {
            m_have = false;
            m_idle = 0;
            return false;
        }
        if (!m_have || calls != m_last) {
            m_have = true;
            m_last = calls;
            m_idle = 0;
            return false;
        }
        if (++m_idle >= kStaleFrames) {
            m_have = false;
            m_idle = 0;
            return true;
        }
        return false;
    }

private:
    bool m_have = false;
    uint64_t m_last = 0;
    uint32_t m_idle = 0;
};

constexpr uint32_t kMaxFaults = 8;   // after this many faulting guarded accesses the feature is off for the session

// ---- events: the hook thread says, the frame thread logs -------------------------------------------------------------------
enum class EvKind : uint32_t { None = 0, FirstCall, Entered, Placed, LockResult, Released, Fault, FaultLimit };
// Where a guarded access faulted.
enum class FaultSite : uint32_t { None = 0, ReadActivity = 1, Validate = 2, ReadPose = 3, WritePose = 4, SetLock = 5, RestoreLock = 6 };
inline const char* faultSiteText(uint32_t site) {
    switch (static_cast<FaultSite>(site)) {
        case FaultSite::ReadActivity: return "the read of +0x470/+0x471/+0x473/+0x48C";
        case FaultSite::Validate: return "the entry check of the activity's memory";
        case FaultSite::ReadPose: return "the read of the pose at +0x3B0";
        case FaultSite::WritePose: return "the write of the pose at +0x3B0";
        case FaultSite::SetLock: return "the lock press (the action object at +0x508)";
        case FaultSite::RestoreLock: return "the restore of the lock action's pressed-int";
        default: return "an unnamed access";
    }
}

struct Event {
    uint64_t activity = 0;
    uint64_t updates = 0;      // pose writes so far
    uint32_t kind = 0;         // EvKind
    uint32_t threadId = 0;
    uint32_t why = 0;          // Released: Why; Fault: FaultSite; Placed: 1 = already locked
    uint32_t before = 0;       // LockResult: +0x48C before the press
    uint32_t after = 0;        // the +0x48C at the event (LockResult: after the press)
    uint32_t flags = 0;        // packObserved
    float eye[3] = {0, 0, 0};  // Placed: up, forward, right
    uint32_t count = 0;        // Fault: the fault number
};
static_assert(std::is_trivially_copyable<Event>::value, "events are copied through the ring");

// One producer (the hook thread, serialised by the glue), one consumer (the frame thread). The producer never waits: a full ring
// drops the event and counts it.
template <size_t N>
class EventRing {
public:
    bool push(const Event& e) {
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
    bool take(Event* out) {
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
    Event m_items[N];
};

// ---- text ----------------------------------------------------------------------------------------------------------------
// Every line this feature writes starts with this, so a flight log is read with one grep. (The probe's lines start "explorer cam
// probe", which this does not match.)
inline const char* prefix() { return "explorer cam:"; }
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

inline void formatEvent(char* out, size_t cap, const Event& e, uint32_t frame) {
    Line o(out, cap);
    const uint32_t relative = e.flags & 0xFFu, rot = (e.flags >> 8) & 0xFFu, pending = (e.flags >> 16) & 0xFFu;
    const unsigned long long act = static_cast<unsigned long long>(e.activity);
    switch (static_cast<EvKind>(e.kind)) {
        case EvKind::FirstCall:
            o.put("%s first free-camera update reached the hook: act=0x%llX thread=%u +0x48C=%u +0x470=%u +0x471=%u +0x473=%u frame=%u",
                  prefix(), act, e.threadId, e.after, relative, rot, pending, frame);
            break;
        case EvKind::Entered:
            o.put("%s entered the free camera: act=0x%llX thread=%u +0x48C=%u +0x470=%u +0x471=%u +0x473=%u frame=%u; the first write is "
                  "the first update with +0x473 = 0 and +0x470 != 0",
                  prefix(), act, e.threadId, e.after, relative, rot, pending, frame);
            break;
        case EvKind::Placed:
            o.put("%s placed: act=0x%llX thread=%u eye(up=%.3f forward=%.3f right=%.3f) m in the commander's frame, +0x48C=%u +0x470=%u "
                  "+0x471=%u frame=%u; the pose is written before every update from now on and the collision step is skipped for this "
                  "activity%s",
                  prefix(), act, e.threadId, e.eye[0], e.eye[1], e.eye[2], e.after, relative, rot, frame,
                  e.why == 1 ? "; the camera was already locked (+0x48C = 4), so the lock is not pressed" : "");
            break;
        case EvKind::LockResult:
            o.put("%s lock pressed: act=0x%llX +0x48C before=%u after=%u (read on the next update) frame=%u%s",
                  prefix(), act, e.before, e.after, frame,
                  e.after == kStateRelativeLock ? "; the relative lock is on"
                                                : "; the press did not move the state to 4, and it is not repeated this session");
            break;
        case EvKind::Released:
            o.put("%s released: act=0x%llX why=%s; pose writes so far=%llu, +0x48C=%u frame=%u; the collision step is the game's again",
                  prefix(), act, whyText(static_cast<Why>(e.why)), static_cast<unsigned long long>(e.updates), e.after, frame);
            break;
        case EvKind::Fault:
            o.put("%s fault %u of %u: %s faulted (act=0x%llX); the session is released", prefix(), e.count, kMaxFaults,
                  faultSiteText(e.why), act);
            break;
        case EvKind::FaultLimit:
            o.put("%s stood down for the session: %u guarded accesses to the free-camera activity faulted; nothing more is written and "
                  "the collision step is the game's again",
                  prefix(), e.count);
            break;
        default:
            o.put("%s event %u (unnamed)", prefix(), e.kind);
            break;
    }
}

struct HeartbeatIn {
    double windowSeconds = 0;
    const char* phase = "placed";   // "placed" | "waiting"
    uint64_t activity = 0;
    uint32_t state = 0;             // the last +0x48C the hook read
    uint64_t updates = 0, updatesWindow = 0;
    uint64_t bypassed = 0, bypassedWindow = 0;
    uint64_t forwarded = 0, forwardedWindow = 0;
    uint64_t hookCalls = 0, hookCallsWindow = 0;
    uint64_t waiting = 0, contended = 0, foreign = 0, lost = 0;
    uint32_t faults = 0;
    Eye eye;
};
inline void formatHeartbeat(char* out, size_t cap, const HeartbeatIn& h) {
    Line o(out, cap);
    o.put("%s heartbeat: phase=%s act=0x%llX +0x48C=%u window=%.1fs updates_placed=%llu(+%llu) collision_bypassed=%llu(+%llu) "
          "collision_forwarded=%llu(+%llu) hook_calls=%llu(+%llu) faults=%u waiting_updates=%llu contended=%llu foreign=%llu "
          "events_lost=%llu eye(up=%.3f forward=%.3f right=%.3f)",
          prefix(), h.phase, static_cast<unsigned long long>(h.activity), h.state, h.windowSeconds,
          static_cast<unsigned long long>(h.updates), static_cast<unsigned long long>(h.updatesWindow),
          static_cast<unsigned long long>(h.bypassed), static_cast<unsigned long long>(h.bypassedWindow),
          static_cast<unsigned long long>(h.forwarded), static_cast<unsigned long long>(h.forwardedWindow),
          static_cast<unsigned long long>(h.hookCalls), static_cast<unsigned long long>(h.hookCallsWindow), h.faults,
          static_cast<unsigned long long>(h.waiting), static_cast<unsigned long long>(h.contended),
          static_cast<unsigned long long>(h.foreign), static_cast<unsigned long long>(h.lost), h.eye.up, h.eye.forward, h.eye.right);
}

// ---- the relays' machine code ------------------------------------------------------------------------------------------------
// This DLL loads more than two gigabytes from the game, so a five-byte E9 cannot reach a C++ replacement. CodeHook patches the
// target with an E9 to a relay placed within two gigabytes, and the relay does the rest. Both relays are copied from
// pose_reader_watch.cpp / object_record_writer_hook.cpp's pattern (grep kRelayBytes); RAX, R11 and the flags are volatile and
// neither observed function reads them on entry.
//
// THE FREE-CAMERA RELAY (44 bytes): a gate in memory, a C++ callback, the trampoline.
//   mov rax,&gate; cmp qword ptr [rax],0; je original; jmp [callback]; original: jmp [trampoline]
constexpr size_t kFreeCameraRelayBytes = 44, kFreeCameraRelayTrampolineAt = 36, kFreeCameraRelayGateAt = 2, kFreeCameraRelayCallbackAt = 22;
inline void buildFreeCameraRelay(uint8_t* code, const void* gate, const void* callback) {
    const uint8_t body[kFreeCameraRelayBytes] = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                     //  0: mov rax, imm64 (&gate)
        0x48, 0x83, 0x38, 0x00,                                 // 10: cmp qword ptr [rax], 0
        0x74, 0x0E,                                             // 14: je original (30)
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,        // 16: jmp qword ptr [rip+0]; dq callback   [at 22]
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};       // 30: original: jmp qword ptr [rip+0]; dq trampoline [at 36]
    std::memcpy(code, body, sizeof(body));
    const uintptr_t g = reinterpret_cast<uintptr_t>(gate), c = reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code + kFreeCameraRelayGateAt, &g, 8);
    std::memcpy(code + kFreeCameraRelayCallbackAt, &c, 8);
}

// THE COLLISION RELAY (68 bytes). No C function sits between the game and the original: the stack argument is never touched.
//   mov rax,&placed; mov rax,[rax]; test rax,rax; je forward        ; nobody placed: the game's collision runs
//   cmp rcx,rax; jne forward                                          ; a different activity: the game's collision runs
//   mov r11,&bypassed; lock inc qword ptr [r11]; xor eax,eax; ret    ; the placed activity: "no collision edit"
//   forward: mov r11,&forwarded; lock inc qword ptr [r11]; jmp [trampoline]
constexpr size_t kCollisionRelayBytes = 68, kCollisionRelayTrampolineAt = 60, kCollisionRelayPlacedAt = 2,
                 kCollisionRelayBypassedAt = 25, kCollisionRelayForwardedAt = 42;
inline void buildCollisionRelay(uint8_t* code, const void* placed, void* bypassed, void* forwarded) {
    const uint8_t body[kCollisionRelayBytes] = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,        //  0: mov rax, imm64 (&placed)
        0x48, 0x8B, 0x00,                          // 10: mov rax, [rax]
        0x48, 0x85, 0xC0,                          // 13: test rax, rax
        0x74, 0x16,                                // 16: je forward (40)
        0x48, 0x39, 0xC1,                          // 18: cmp rcx, rax
        0x75, 0x11,                                // 21: jne forward (40)
        0x49, 0xBB, 0, 0, 0, 0, 0, 0, 0, 0,        // 23: mov r11, imm64 (&bypassed)   [imm at 25]
        0xF0, 0x49, 0xFF, 0x03,                    // 33: lock inc qword ptr [r11]
        0x31, 0xC0,                                // 37: xor eax, eax
        0xC3,                                      // 39: ret
        0x49, 0xBB, 0, 0, 0, 0, 0, 0, 0, 0,        // 40: forward: mov r11, imm64 (&forwarded) [imm at 42]
        0xF0, 0x49, 0xFF, 0x03,                    // 50: lock inc qword ptr [r11]
        0xFF, 0x25, 0, 0, 0, 0,                    // 54: jmp qword ptr [rip+0]
        0, 0, 0, 0, 0, 0, 0, 0};                   // 60: dq trampoline
    std::memcpy(code, body, sizeof(body));
    const uintptr_t p = reinterpret_cast<uintptr_t>(placed), b = reinterpret_cast<uintptr_t>(bypassed),
                    f = reinterpret_cast<uintptr_t>(forwarded);
    std::memcpy(code + kCollisionRelayPlacedAt, &p, 8);
    std::memcpy(code + kCollisionRelayBypassedAt, &b, 8);
    std::memcpy(code + kCollisionRelayForwardedAt, &f, 8);
}

}  // namespace ecm
}  // namespace edvr

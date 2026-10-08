// Explorer Cam's pure half (fix.explorer_cam; docs\design-explorer-cam-free-camera-2026-10-07.md, "Phase 1a" and "Phase 1c").
//
// Everything here is decisions, bytes and text, with no Windows, no game and no Config, so tools\explorer_cam_test drives the same
// code the DLL compiles: the build-332841 identity, the eye keys' clamp, the sixteen floats written into the free camera's
// commander-local pose, the per-activity placement machine, the F5 sequencer that opens the camera and switches to the free
// camera, the camera-UI hider, the F5 key's decision, the stale watch, the event rings the hook threads log through, every log
// line's text, and the machine code of the two relays.
//
// WHAT THE FEATURE DOES (Sean's decision D1, 2026-10-07: these writes into game memory, nothing else):
//   1. PLACE   Elite's own free camera (FreeCameraActivity, the update at EliteDangerous64.exe+0x1071980) is put at the
//              commander's head: its commander-local pose (+0x3B0) is written before every update while it runs, as the
//              identity rotation and an origin (right, up, forward) in metres from the commander's feet.
//   2. SKIP    the camera's two position edits (the collision step FUN 0x1091140 and the commander's box push FUN 0x108F1B0,
//              each called only by that update) are made to return 0 -- "no edit" -- for that one activity, by relays in
//              machine code in front of the game's functions.
//   3. PRESS   the game's own actions, each for exactly ONE update: the pressed-int of an action object is set to 1 before the
//              update and restored after it returns. The relative lock (activity+0x508), FreeCamToggleHUD (the camera UI's
//              +0x1D8), and, in F5's sequence, PhotoCameraToggle (+0x310) and ToggleFreeCam (+0x328) of the camera controller.
// Nothing else is written: not +0x48C, not the mode byte, not the shared record, not +0x470/+0x471/+0x473, not anything replicated.
//
// THE ONLY WAY IN IS F5 (hotkey.explorer_cam). The game's own camera key and TAB place nothing: placement needs an Explorer Cam
// SESSION, which F5 starts and ends (F5Sequencer). Everything else is stock.
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
constexpr uintptr_t kBoxPushRva = 0x108F1B0;      // pushes the point out of the commander's box (+0.25); one caller, 0x10728B6
constexpr uintptr_t kCameraUiRva = 0x47C7640;     // VanityCameraUIActivity's update (rcx = the object), reached by a job thunk
constexpr uintptr_t kControllerRva = 0x2DF14C0;   // VesselCameraMountControl's update (rcx = the controller), vtable only
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

constexpr size_t kBoxPushPrologueBytes = 20;
// `mov rax,rsp; push rbp; push rbx; push rsi; push r14; push r15; mov rbp,rsp; sub rsp,80h`. Boundaries at 3, 4, 5, 6, 8, 10, 13,
// 20; no rip-relative byte; CodeHook steals 5 (`mov rax,rsp`, `push rbp`, `push rbx`), and the trampoline re-runs the `mov rax,rsp`.
// int FUN(activity, point*): the count of boxes the point was pushed out of; the point is rewritten in place.
inline constexpr uint8_t kBoxPushPrologue[kBoxPushPrologueBytes] = {
    0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x8B, 0xEC, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00};

constexpr size_t kCameraUiPrologueBytes = 19;
// `push rbp; push r14; lea rbp,[rsp-0B8h]; sub rsp,1B8h`. Boundaries at 2, 4, 12, 19; the SIB + disp32 `lea` is not rip-relative.
// A five-byte E9 steals 12 bytes: two pushes and the `lea`.
inline constexpr uint8_t kCameraUiPrologue[kCameraUiPrologueBytes] = {
    0x40, 0x55, 0x41, 0x56, 0x48, 0x8D, 0xAC, 0x24, 0x48, 0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xB8, 0x01, 0x00, 0x00};

constexpr size_t kControllerPrologueBytes = 15;
// `mov [rsp+8],rbx; mov [rsp+10h],rbp; mov [rsp+18h],rsi`. CodeHook steals the first (5 bytes).
inline constexpr uint8_t kControllerPrologue[kControllerPrologueBytes] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18};

// ---- the free-camera activity's fields (Phase 0a, "The object") -------------------------------------------------------------
constexpr uint32_t kOffLocalPose = 0x3B0;      // 16 floats, row-major 4x4: rows 0-2 = right, up, forward; row 3 = origin (x right, y up, z forward)
constexpr uint32_t kOffRelative = 0x470;       // 1 = relative to the commander's frame, 0 = world
constexpr uint32_t kOffRotationLock = 0x471;   // 1 = follow the live frame
constexpr uint32_t kOffPresetPending = 0x473;  // 1 only on the first update, which seeds the pose from the preset
constexpr uint32_t kOffState = 0x48C;          // a byte, mirrored from a shared record every update: NEVER written here
constexpr uint32_t kOffToggleRotationAction = 0x4F8;   // qwords: action objects (the F2 probe reads their pressed ints)
constexpr uint32_t kOffWorldFixAction = 0x500;
constexpr uint32_t kOffLockAction = 0x508;     // the relative-lock action object
constexpr uint32_t kOffActionPressed = 0x1C;   // in any action object: an int, nonzero on a press
constexpr uint32_t kActivityBytes = 0x510;     // the highest byte this feature touches is the qword at +0x508

// The camera UI (VanityCameraUIActivity).
constexpr uint32_t kOffUiHidden = 0x1A0;       // a byte; the update toggles it on FreeCamToggleHUD: 1 = hidden (the update fires
                                               // VanityCamGui_Hide when it is non-zero, _Show when it is zero)
constexpr uint32_t kOffUiHideAction = 0x1D8;   // a qword, NULL unless the game stored FreeCamToggleHUD's handle
constexpr uint32_t kUiBytes = 0x1E0;

// The camera controller (VesselCameraMountControl).
constexpr uint32_t kOffCtlPresetKind = 0x2E8;  // an int: 0 = ToggleFreeCam from a preset gives the free camera; 1 = it toggles presets 1 and 2
constexpr uint32_t kOffCtlPhotoAction = 0x310; // PhotoCameraToggle: opens the camera when the mode is 0, closes it otherwise
constexpr uint32_t kOffCtlFreeAction = 0x328;  // ToggleFreeCam (TAB), read only in modes 1 and 2
constexpr uint32_t kOffCtlQuitAction = 0x340;  // QuitCamera
constexpr uint32_t kOffCtlMode = 0x3E0;        // a byte (= the shared record's +0x30): the camera mode
constexpr uint32_t kCtlBytes = 0x3E8;

// The mode byte of the controller and the state byte of the free-camera activity share their numbering.
constexpr uint8_t kStateOff = 0, kStateFree = 3, kStateRelativeLock = 4, kStateWorldLock = 5, kStateVariant = 6;
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

// What one pre-call read of the free-camera activity yields.
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

// ---- the per-activity placement machine -------------------------------------------------------------------------------------
// Why a placement ended. The text is in whyText(); the numbers are logged only through it.
enum class Why : uint32_t {
    None = 0,
    LeftFreeCamera = 1,   // +0x48C became 0
    WorldLock = 2,        // +0x48C became 5
    Variant = 3,          // +0x48C became 6
    UnexpectedState = 4,  // +0x48C became something the design does not know
    KeyOff = 5,           // the Explorer Cam session ended, or fix.explorer_cam turned off (or the feature stood down)
    Stale = 6,            // the activity was not called for kStaleFrames frames
    Fault = 7,            // a guarded access to the activity faulted
    FaultLimit = 8,       // the fault budget ran out: the feature is off for the session
    NewSession = 9        // +0x473 went back to 1 on an activity that was already placed
};
inline const char* whyText(Why w) {
    switch (w) {
        case Why::LeftFreeCamera: return "left the free camera (+0x48C = 0)";
        case Why::WorldLock: return "the world lock took over (+0x48C = 5, the camera is detached)";
        case Why::Variant: return "the variant state took over (+0x48C = 6)";
        case Why::UnexpectedState: return "+0x48C is a state the design does not know";
        case Why::KeyOff: return "the Explorer Cam session ended (F5, the camera closed, or fix.explorer_cam turned off)";
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
    bool entered = false;       // a placement began on this call
    bool released = false;      // the placement ended on this call; no write, no press
    Why why = Why::None;
    bool pressResult = false;   // the previous call's lock press, resolved by this call's state byte
    uint8_t pressBefore = 0, pressAfter = 0;
    bool write = false;         // write the commander-local pose before the original
    bool placeNow = false;      // ...and this is the placement's first write: publish "placed"
    bool alreadyLocked = false; // the first write found +0x48C = 4: the user locked it; no press is needed
    bool press = false;         // set the lock action's pressed-int to 1 before the original, restore it after
    bool waiting = false;       // tracked and wanted, but not yet placeable (+0x473 = 1 or +0x470 = 0)
};

// ONE tracked activity at a time. Hook thread only (the glue serialises calls); a reset from the frame thread arrives as a flag the
// hook thread acts on at its next call.
//
// `wanted` is "an Explorer Cam session is on" (F5 started it and no end has been seen). That is the only gate: with it, the first
// call that shows +0x48C = 3 or 4 enters, whatever came before -- a detach (5) mid-session releases and the return to 3 or 4
// places again, locks again and hides the UI again; without it nothing is ever placed, whatever the user presses.
//
//   Idle     waiting for an entry.
//   Placing  from the entry on. The first update with +0x473 == 0 and +0x470 != 0 is the first write; from then on EVERY update
//            while +0x48C is 3 or 4 writes the pose. The lock is pressed once per placement, on the first write if +0x48C is 3.
class Machine {
public:
    enum class Phase : uint8_t { Idle, Placing };

    Phase phase() const { return m_phase; }
    uint64_t activity() const { return m_activity; }
    bool placed() const { return m_placed; }
    bool pressed() const { return m_pressed; }

    void reset() { *this = Machine(); }

    // Decide one call.
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
                endPlacement(s, Why::KeyOff);
                return s;
            }
            if (o.presetPending != 0 && m_placed) {   // a brand-new session on an activity we thought we still held
                endPlacement(s, Why::NewSession);
                // ...and this very call may start the next one: fall through to the Idle half.
            } else {
                switch (o.state) {
                    case kStateOff: endPlacement(s, Why::LeftFreeCamera); return s;
                    case kStateWorldLock: endPlacement(s, Why::WorldLock); return s;
                    case kStateVariant: endPlacement(s, Why::Variant); return s;
                    case kStateFree:
                    case kStateRelativeLock: break;
                    default: endPlacement(s, Why::UnexpectedState); return s;
                }
                placing(s, o);
                return s;
            }
        }
        // Idle half.
        if (!wanted) return s;
        if (m_blocked) {
            // After a fault the same state must not re-enter at once (a fault loop); any other state, or a new session, lifts it.
            if ((o.state == kStateFree || o.state == kStateRelativeLock) && o.presetPending == 0) return s;
            m_blocked = false;
        }
        if (o.state != kStateFree && o.state != kStateRelativeLock) return s;
        m_phase = Phase::Placing;
        m_activity = activity;
        m_placed = false;
        m_pressed = false;
        m_pressPending = false;
        s.entered = true;
        placing(s, o);
        return s;
    }

    // The hook thread ends the placement itself (a fault, the fault budget): the same bookkeeping as a state-driven release.
    Step abort(Why why) {
        Step s;
        if (m_phase == Phase::Placing) endPlacement(s, why);
        m_blocked = true;
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
    void endPlacement(Step& s, Why why) {
        s.released = true;
        s.why = why;
        m_phase = Phase::Idle;
        m_activity = 0;
        m_placed = false;
        m_pressed = false;
        m_pressPending = false;
    }

    Phase m_phase = Phase::Idle;
    uint64_t m_activity = 0;   // the tracked activity while Placing
    bool m_placed = false;
    bool m_pressed = false;
    bool m_pressPending = false;
    bool m_blocked = false;
};

// The frame thread's watch: something that stops being called is released. One tick per frame boundary.
class StaleWatch {
public:
    static constexpr uint32_t kStaleFrames = 30;
    // `active` = something is tracked; `calls` = the count of its calls. True = release it now.
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

// Has a call counter moved within the last kStaleFrames ticks? The camera controller's liveness, for F5: a counter that has never
// moved is not alive, and one that moved long ago is not either.
class LiveWatch {
public:
    void tick(uint64_t calls) {
        if (calls != m_last) {
            m_last = calls;
            m_idle = 0;
            m_everMoved = true;
        } else if (m_idle < 1000000u) {
            ++m_idle;
        }
    }
    bool alive() const { return m_everMoved && m_idle < StaleWatch::kStaleFrames; }
    void reset() { *this = LiveWatch(); }

private:
    uint64_t m_last = 0;
    uint32_t m_idle = 0;
    bool m_everMoved = false;
};

constexpr uint32_t kMaxFaults = 8;   // after this many faulting guarded accesses the feature is off for the session

// ---- F5: the sequence that opens the camera and switches to the free camera ---------------------------------------------------
// The controller's pre-call, one decision per update (VesselCameraMountControl's update, mode byte +0x3E0):
//   ENTER from mode 0 : press PhotoCameraToggle, wait for 1 or 2, press ToggleFreeCam, wait for 3. A session is on from the request.
//   ENTER from 1 or 2 : press ToggleFreeCam, wait for 3.      From 3 or 4: nothing to press, the placement machine takes over.
//   ENTER from 5, 6   : refused (a detached camera must be left first); no session.
//   EXIT              : unhide the camera UI if EDVR hid it (the UI hook does it, this waits), then press PhotoCameraToggle, which
//                       closes the camera from any mode; the session ends at mode 0.
//   Mode 0 by ANY route ends the session once the camera has been open; a detach (5) mid-session keeps it.
// One press per update. Each wait is kWaitUpdates updates; past it the sequence aborts with a line.
enum class F5Req : uint32_t { None = 0, Enter = 1, Exit = 2 };
enum class CtlPress : uint8_t { None = 0, Photo = 1, Free = 2 };
enum class SeqEvent : uint32_t {
    None = 0,
    EnterFromClosed,        // mode 0: opening the camera
    EnterFromPreset,        // mode 1 or 2: switching to the free camera
    EnterFromFree,          // mode 3 or 4: already in the free camera
    EnterOpened,            // the camera is open on a preset: pressing ToggleFreeCam
    EnterAttached,          // the free camera is up: the placement machine takes over
    EnterRefusedDetached,   // mode 5
    EnterRefusedVariant,    // mode 6 or unknown
    EnterTimeoutOpen,       // the camera did not open
    EnterTimeoutFree,       // the free camera did not come up
    EnterIgnored,           // a second ENTER while a session is on
    ExitStart,
    ExitUnhiding,           // waiting for the UI hook to give the camera UI back
    ExitUnhideTimeout,      // it did not; closing anyway
    ExitClosing,            // pressing PhotoCameraToggle
    ExitDone,               // mode 0 after the close
    ExitTimeout,            // the camera did not close
    ExitIgnored,            // EXIT with no session
    SessionEnded            // mode 0 on its own (the user closed the camera, or it closed)
};
constexpr uint32_t kSeqWaitUpdates = 90;

struct SeqStep {
    CtlPress press = CtlPress::None;
    SeqEvent ev[3] = {SeqEvent::None, SeqEvent::None, SeqEvent::None};
    uint8_t nev = 0;
    uint8_t mode = 0;
    bool sessionActive = false;   // after this step
    bool exiting = false;         // after this step: the EXIT part of the sequence is running
    void add(SeqEvent e) { if (nev < 3) ev[nev++] = e; }
};

class F5Sequencer {
public:
    enum class Stage : uint8_t { Idle, OpenWait, FreeWait, Active, ExitUnhide, ExitClose };

    Stage stage() const { return m_stage; }
    bool sessionActive() const { return m_stage != Stage::Idle; }
    bool exiting() const { return m_stage == Stage::ExitUnhide || m_stage == Stage::ExitClose; }
    void reset() { *this = F5Sequencer(); }

    // One controller update. `mode` is the mode byte as the previous update left it; `uiHiddenByUs` is the UI hook's word.
    SeqStep step(uint8_t mode, F5Req req, bool uiHiddenByUs) {
        SeqStep s;
        s.mode = mode;
        if (m_stage == Stage::Idle) {
            if (req == F5Req::Enter) begin(s, mode);
            else if (req == F5Req::Exit) s.add(SeqEvent::ExitIgnored);
            return finish(s);
        }
        if (req == F5Req::Exit && !exiting()) {
            beginExit(s, mode, uiHiddenByUs);
            return finish(s);
        }
        if (req == F5Req::Enter) s.add(SeqEvent::EnterIgnored);
        switch (m_stage) {
            case Stage::OpenWait:
                if (mode == 1 || mode == 2) {
                    m_stage = Stage::FreeWait;
                    m_waited = 0;
                    s.press = CtlPress::Free;
                    s.add(SeqEvent::EnterOpened);
                } else if (mode >= 3) {
                    m_stage = Stage::Active;
                    s.add(SeqEvent::EnterAttached);
                } else if (++m_waited >= kSeqWaitUpdates) {
                    s.add(SeqEvent::EnterTimeoutOpen);
                    end();
                }
                break;
            case Stage::FreeWait:
                if (mode >= 3) {
                    m_stage = Stage::Active;
                    s.add(SeqEvent::EnterAttached);
                } else if (mode == 0) {
                    s.add(SeqEvent::SessionEnded);
                    end();
                } else if (++m_waited >= kSeqWaitUpdates) {
                    s.add(SeqEvent::EnterTimeoutFree);
                    end();
                }
                break;
            case Stage::Active:
                if (mode == 0) {
                    s.add(SeqEvent::SessionEnded);
                    end();
                }
                break;
            case Stage::ExitUnhide:
                if (!uiHiddenByUs) {
                    closeCamera(s, mode);
                } else if (++m_waited >= kSeqWaitUpdates) {
                    s.add(SeqEvent::ExitUnhideTimeout);
                    closeCamera(s, mode);
                }
                break;
            case Stage::ExitClose:
                if (mode == 0) {
                    s.add(SeqEvent::ExitDone);
                    end();
                } else if (++m_waited >= kSeqWaitUpdates) {
                    s.add(SeqEvent::ExitTimeout);
                    end();
                }
                break;
            default: break;
        }
        return finish(s);
    }

private:
    SeqStep finish(SeqStep& s) {
        s.sessionActive = m_stage != Stage::Idle;
        s.exiting = exiting();
        return s;
    }
    void end() {
        m_stage = Stage::Idle;
        m_waited = 0;
    }
    void begin(SeqStep& s, uint8_t mode) {
        switch (mode) {
            case 0:
                m_stage = Stage::OpenWait;
                m_waited = 0;
                s.press = CtlPress::Photo;
                s.add(SeqEvent::EnterFromClosed);
                break;
            case 1:
            case 2:
                m_stage = Stage::FreeWait;
                m_waited = 0;
                s.press = CtlPress::Free;
                s.add(SeqEvent::EnterFromPreset);
                break;
            case 3:
            case 4:
                m_stage = Stage::Active;
                s.add(SeqEvent::EnterFromFree);
                break;
            case 5:
                s.add(SeqEvent::EnterRefusedDetached);
                break;
            default:
                s.add(SeqEvent::EnterRefusedVariant);
                break;
        }
    }
    void beginExit(SeqStep& s, uint8_t mode, bool uiHiddenByUs) {
        s.add(SeqEvent::ExitStart);
        if (mode == 0) {
            s.add(SeqEvent::ExitDone);
            end();
        } else if (uiHiddenByUs) {
            m_stage = Stage::ExitUnhide;
            m_waited = 0;
            s.add(SeqEvent::ExitUnhiding);
        } else {
            closeCamera(s, mode);
        }
    }
    void closeCamera(SeqStep& s, uint8_t mode) {
        if (mode == 0) {
            s.add(SeqEvent::ExitDone);
            end();
            return;
        }
        m_stage = Stage::ExitClose;
        m_waited = 0;
        s.press = CtlPress::Photo;
        s.add(SeqEvent::ExitClosing);
    }

    Stage m_stage = Stage::Idle;
    uint32_t m_waited = 0;
};

// What F5 does when pressed. Pure, so the table is a test.
enum class F5Action : uint32_t { None = 0, Enter, Exit, RefuseOff, RefuseControllerIdle, RefuseNotOnFoot };
struct F5Inputs {
    bool pressed = false;           // the key's edge this frame (Hotkey: foreground-gated, never captured)
    bool gameplay = false;          // the journal says gameplay has started (or there is no journal to ask)
    bool active = false;            // fix.explorer_cam on and every hook it needs armed
    bool sessionActive = false;     // an Explorer Cam session is on
    bool controllerAlive = false;   // the camera controller's update has been called within the last 30 frames
    uint8_t mode = 0;               // the controller's mode byte as last read
    bool onFootKnown = false, onFoot = false;   // Status.json
};
inline F5Action decideF5(const F5Inputs& in) {
    if (!in.pressed || !in.gameplay) return F5Action::None;
    if (!in.active) return F5Action::RefuseOff;
    if (in.sessionActive) return F5Action::Exit;
    if (!in.controllerAlive) return F5Action::RefuseControllerIdle;
    if (in.mode == 0 && in.onFootKnown && !in.onFoot) return F5Action::RefuseNotOnFoot;
    return F5Action::Enter;
}

// ---- the camera UI ------------------------------------------------------------------------------------------------------------
// FreeCamToggleHUD toggles +0x1A0 (1 = hidden) when the pressed-int at *(+0x1D8)+0x1C is nonzero. The hider presses it once per
// placement, only if the UI is showing, remembers that EDVR hid it, and presses again to give it back when the placement ends
// and EDVR's hide is still the state. A NULL handle is "the game offers no hide-UI here": said once, not a fault.
enum class UiEvent : uint32_t { None = 0, Hidden, Unhidden, HideNoEffect, UnhideNoEffect, NoHandle, UserShowed };
struct UiObserved {
    bool handle = false;    // the qword at +0x1D8 is non-null
    uint8_t hidden = 0;     // the byte at +0x1A0
};
struct UiStep {
    bool press = false;
    UiEvent ev = UiEvent::None;
    uint8_t hidden = 0;
};
class UiHider {
public:
    bool hiddenByUs() const { return m_hiddenByUs; }
    bool pending() const { return m_pending != Pending::None; }   // a press is set and its result not yet read
    void reset() { *this = UiHider(); }

    UiStep step(uint64_t object, const UiObserved& o, bool wantHidden) {
        UiStep s;
        s.hidden = o.hidden;
        if (object != m_object) {   // a different camera UI: the old one is gone, and so is whatever we did to it
            const bool noted = m_noHandleNoted;
            *this = UiHider();
            m_noHandleNoted = noted;
            m_object = object;
        }
        if (m_pending == Pending::Hide) {
            if (o.hidden != 0) { m_hiddenByUs = true; s.ev = UiEvent::Hidden; }
            else s.ev = UiEvent::HideNoEffect;
        } else if (m_pending == Pending::Unhide) {
            if (o.hidden == 0) s.ev = UiEvent::Unhidden;
            else s.ev = UiEvent::UnhideNoEffect;
            m_hiddenByUs = false;   // either way EDVR stops: a press that did not work is not repeated
        }
        m_pending = Pending::None;
        if (wantHidden && !m_lastWant) m_pressedThisPlacement = false;   // a new placement: hide again
        m_lastWant = wantHidden;
        if (!o.handle) {
            if ((wantHidden || m_hiddenByUs) && !m_noHandleNoted) {
                m_noHandleNoted = true;
                if (s.ev == UiEvent::None) s.ev = UiEvent::NoHandle;
            }
            return s;
        }
        if (wantHidden) {
            if (!m_hiddenByUs && o.hidden == 0 && !m_pressedThisPlacement) {
                m_pressedThisPlacement = true;
                m_pending = Pending::Hide;
                s.press = true;
            } else if (m_hiddenByUs && o.hidden == 0) {
                m_hiddenByUs = false;   // the player showed the UI again themselves: it is theirs
                if (s.ev == UiEvent::None) s.ev = UiEvent::UserShowed;
            }
        } else if (m_hiddenByUs) {
            if (o.hidden != 0) {
                m_pending = Pending::Unhide;
                s.press = true;
            } else {
                m_hiddenByUs = false;   // already showing
            }
        }
        return s;
    }

private:
    enum class Pending : uint8_t { None, Hide, Unhide };
    uint64_t m_object = 0;
    bool m_hiddenByUs = false;
    bool m_pressedThisPlacement = false;
    bool m_lastWant = false;
    bool m_noHandleNoted = false;
    Pending m_pending = Pending::None;
};

// ---- events: the hook threads say, the frame thread logs -------------------------------------------------------------------
enum class EvKind : uint32_t {
    None = 0, FirstCall, Entered, Placed, LockResult, Released, Fault, FaultLimit,
    Seq,                // F5 sequence: why = SeqEvent, after = mode, count = the preset kind (+0x2E8)
    Ui,                 // the camera UI: why = UiEvent, after = the +0x1A0 byte
    ControllerFirstCall // the camera controller update reached the hook: after = mode
};
// Where a guarded access faulted.
enum class FaultSite : uint32_t {
    None = 0, ReadActivity = 1, Validate = 2, ReadPose = 3, WritePose = 4, SetLock = 5, RestoreLock = 6,
    ReadController = 7, SetControllerPress = 8, RestoreControllerPress = 9, ReadUi = 10, SetUiPress = 11, RestoreUiPress = 12
};
inline const char* faultSiteText(uint32_t site) {
    switch (static_cast<FaultSite>(site)) {
        case FaultSite::ReadActivity: return "the read of +0x470/+0x471/+0x473/+0x48C";
        case FaultSite::Validate: return "the entry check of the activity's memory";
        case FaultSite::ReadPose: return "the read of the pose at +0x3B0";
        case FaultSite::WritePose: return "the write of the pose at +0x3B0";
        case FaultSite::SetLock: return "the lock press (the action object at +0x508)";
        case FaultSite::RestoreLock: return "the restore of the lock action's pressed-int";
        case FaultSite::ReadController: return "the read of the camera controller's mode (+0x3E0)";
        case FaultSite::SetControllerPress: return "the camera controller's press (the action object at +0x310 or +0x328)";
        case FaultSite::RestoreControllerPress: return "the restore of the camera controller's pressed-int";
        case FaultSite::ReadUi: return "the read of the camera UI's +0x1A0/+0x1D8";
        case FaultSite::SetUiPress: return "the camera UI's hide press (the action object at +0x1D8)";
        case FaultSite::RestoreUiPress: return "the restore of the camera UI's pressed-int";
        default: return "an unnamed access";
    }
}

struct Event {
    uint64_t seq = 0;          // global order across the rings
    uint64_t activity = 0;     // the object the event is about (activity, controller or camera UI)
    uint64_t updates = 0;      // pose writes so far
    uint32_t kind = 0;         // EvKind
    uint32_t threadId = 0;
    uint32_t why = 0;          // Released: Why; Fault: FaultSite; Placed: 1 = already locked; Seq: SeqEvent; Ui: UiEvent
    uint32_t before = 0;       // LockResult: +0x48C before the press
    uint32_t after = 0;        // the +0x48C (mode, +0x1A0) at the event (LockResult: after the press)
    uint32_t flags = 0;        // packObserved
    float eye[3] = {0, 0, 0};  // Placed: up, forward, right
    uint32_t count = 0;        // Fault: the fault number; Seq: the preset kind
};
static_assert(std::is_trivially_copyable<Event>::value, "events are copied through the ring");
static_assert(sizeof(Event) == 64, "an event is one cache line");

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
template <size_t N>
using EventRing = Ring<Event, N>;

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

inline void putSeqEvent(Line& o, SeqEvent e, uint32_t mode, uint32_t presetKind) {
    switch (e) {
        case SeqEvent::EnterFromClosed:
            o.put("F5 enter: the camera is closed (mode 0); opening it (PhotoCameraToggle), then the free camera (ToggleFreeCam)");
            break;
        case SeqEvent::EnterFromPreset:
            o.put("F5 enter: the camera is open on a preset (mode %u); pressing ToggleFreeCam (preset kind +0x2E8 = %u)", mode, presetKind);
            break;
        case SeqEvent::EnterFromFree:
            o.put("F5 enter: already in the free camera (mode %u); nothing to press, the pose is placed on the next update", mode);
            break;
        case SeqEvent::EnterOpened:
            o.put("F5 enter: the camera opened on a preset (mode %u); pressing ToggleFreeCam (preset kind +0x2E8 = %u)", mode, presetKind);
            break;
        case SeqEvent::EnterAttached:
            o.put("F5 enter: the free camera is up (mode %u, %s); the pose is placed on the next update", mode, modeText(mode));
            break;
        case SeqEvent::EnterRefusedDetached:
            o.put("F5 enter refused: the camera is detached (mode 5, the world lock); leave it first (the game's own camera key), then press F5");
            break;
        case SeqEvent::EnterRefusedVariant:
            o.put("F5 enter refused: the camera is in mode %u (%s), which Explorer Cam does not enter from", mode, modeText(mode));
            break;
        case SeqEvent::EnterTimeoutOpen:
            o.put("F5 enter aborted: the camera did not open within %u updates (mode still %u); the game would not open it just now", kSeqWaitUpdates, mode);
            break;
        case SeqEvent::EnterTimeoutFree:
            o.put("F5 enter aborted: the free camera did not come up within %u updates (mode %u, preset kind +0x2E8 = %u; kind 1 makes "
                  "ToggleFreeCam toggle presets 1 and 2 instead)",
                  kSeqWaitUpdates, mode, presetKind);
            break;
        case SeqEvent::EnterIgnored:
            o.put("F5 enter ignored: an Explorer Cam session is already on");
            break;
        case SeqEvent::ExitStart:
            o.put("F5 exit: leaving Explorer Cam (camera mode %u, %s)", mode, modeText(mode));
            break;
        case SeqEvent::ExitUnhiding:
            o.put("F5 exit: giving the camera UI back first (EDVR hid it)");
            break;
        case SeqEvent::ExitUnhideTimeout:
            o.put("F5 exit: the camera UI did not come back within %u updates; closing the camera anyway", kSeqWaitUpdates);
            break;
        case SeqEvent::ExitClosing:
            o.put("F5 exit: closing the camera (PhotoCameraToggle)");
            break;
        case SeqEvent::ExitDone:
            o.put("F5 exit: the camera is closed (mode 0); the Explorer Cam session is over");
            break;
        case SeqEvent::ExitTimeout:
            o.put("F5 exit aborted: the camera did not close within %u updates (mode %u); the Explorer Cam session is over", kSeqWaitUpdates, mode);
            break;
        case SeqEvent::ExitIgnored:
            o.put("F5 exit ignored: no Explorer Cam session is on");
            break;
        case SeqEvent::SessionEnded:
            o.put("the Explorer Cam session ended: the camera closed (mode 0)");
            break;
        default:
            o.put("F5 sequence event %u (unnamed)", static_cast<uint32_t>(e));
            break;
    }
}

inline const char* uiEventText(UiEvent e) {
    switch (e) {
        case UiEvent::Hidden: return "the camera UI is hidden (FreeCamToggleHUD pressed once; +0x1A0 is now 1)";
        case UiEvent::Unhidden: return "the camera UI is back (FreeCamToggleHUD pressed again; +0x1A0 is now 0)";
        case UiEvent::HideNoEffect: return "the hide press did not set +0x1A0 to 1, and it is not repeated this placement";
        case UiEvent::UnhideNoEffect: return "the unhide press did not clear +0x1A0, and it is not repeated";
        case UiEvent::NoHandle: return "the game offers no hide-UI here (the camera UI's FreeCamToggleHUD handle at +0x1D8 is NULL); the camera UI stays";
        case UiEvent::UserShowed: return "the camera UI was shown again by the player; EDVR leaves it";
        default: return "camera UI event (unnamed)";
    }
}

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
                  "+0x471=%u frame=%u; the pose is written before every update from now on and the collision step and the box push are "
                  "skipped for this activity%s",
                  prefix(), act, e.threadId, e.eye[0], e.eye[1], e.eye[2], e.after, relative, rot, frame,
                  e.why == 1 ? "; the camera was already locked (+0x48C = 4), so the lock is not pressed" : "");
            break;
        case EvKind::LockResult:
            o.put("%s lock pressed: act=0x%llX +0x48C before=%u after=%u (read on the next update) frame=%u%s",
                  prefix(), act, e.before, e.after, frame,
                  e.after == kStateRelativeLock ? "; the relative lock is on"
                                                : "; the press did not move the state to 4, and it is not repeated this placement");
            break;
        case EvKind::Released:
            o.put("%s released: act=0x%llX why=%s; pose writes so far=%llu, +0x48C=%u frame=%u; the collision step and the box push are the game's again",
                  prefix(), act, whyText(static_cast<Why>(e.why)), static_cast<unsigned long long>(e.updates), e.after, frame);
            break;
        case EvKind::Fault:
            o.put("%s fault %u of %u: %s faulted (obj=0x%llX); the placement and the sequence are ended", prefix(), e.count, kMaxFaults,
                  faultSiteText(e.why), act);
            break;
        case EvKind::FaultLimit:
            o.put("%s stood down for the session: %u guarded accesses to the camera objects faulted; nothing more is written and "
                  "the game's position edits are the game's again",
                  prefix(), e.count);
            break;
        case EvKind::Seq:
            o.put("%s ", prefix());
            putSeqEvent(o, static_cast<SeqEvent>(e.why), e.after, e.count);
            o.put(" (ctl=0x%llX frame=%u)", act, frame);
            break;
        case EvKind::Ui:
            o.put("%s %s (ui=0x%llX frame=%u)", prefix(), uiEventText(static_cast<UiEvent>(e.why)), act, frame);
            break;
        case EvKind::ControllerFirstCall:
            o.put("%s the camera controller update reached the hook: ctl=0x%llX thread=%u mode=%u (%s) frame=%u%s", prefix(), act, e.threadId,
                  e.after, modeText(e.after), frame,
                  e.after == 0 ? "; it runs with the camera closed, so F5 works from first person" : "");
            break;
        default:
            o.put("%s event %u (unnamed)", prefix(), e.kind);
            break;
    }
}

// The F5 key's own lines (frame thread).
inline void formatF5(char* out, size_t cap, F5Action a, uint32_t mode, bool onFootKnown, bool onFoot) {
    Line o(out, cap);
    switch (a) {
        case F5Action::Enter:
            o.put("%s F5 pressed: entering Explorer Cam (camera mode %u, %s; on foot: %s)", prefix(), mode, modeText(mode),
                  onFootKnown ? (onFoot ? "yes" : "no") : "unknown");
            break;
        case F5Action::Exit:
            o.put("%s F5 pressed: leaving Explorer Cam (camera mode %u, %s)", prefix(), mode, modeText(mode));
            break;
        case F5Action::RefuseOff:
            o.put("%s F5 pressed, but Explorer Cam is not running (fix.explorer_cam is off, or a hook stood down: the lines above say which)", prefix());
            break;
        case F5Action::RefuseControllerIdle:
            o.put("%s F5 pressed, but the camera controller is idle: open the camera first (it is not being called with the camera closed; "
                  "from inside the camera F5 works)",
                  prefix());
            break;
        case F5Action::RefuseNotOnFoot:
            o.put("%s F5 pressed, but you are not on foot and the camera is closed: Explorer Cam starts on foot, or from inside the camera", prefix());
            break;
        default:
            o.put("%s F5 pressed: nothing to do", prefix());
            break;
    }
}

struct HeartbeatIn {
    double windowSeconds = 0;
    const char* phase = "placed";   // "placed" | "waiting" | "session"
    uint64_t activity = 0;
    uint32_t state = 0;             // the last +0x48C the hook read
    uint64_t updates = 0, updatesWindow = 0;
    uint64_t bypassed = 0, bypassedWindow = 0;        // the collision step
    uint64_t forwarded = 0, forwardedWindow = 0;
    uint64_t boxBypassed = 0, boxBypassedWindow = 0;  // the box push
    uint64_t boxForwarded = 0, boxForwardedWindow = 0;
    uint64_t hookCalls = 0, hookCallsWindow = 0;
    uint64_t ctlCalls = 0, ctlCallsWindow = 0;
    uint32_t ctlMode = 0;
    bool session = false;
    bool uiHiddenByUs = false;
    uint64_t uiCalls = 0;
    uint64_t waiting = 0, contended = 0, foreign = 0, lost = 0;
    uint32_t faults = 0;
    Eye eye;
};
inline void formatHeartbeat(char* out, size_t cap, const HeartbeatIn& h) {
    Line o(out, cap);
    o.put("%s heartbeat: phase=%s session=%s act=0x%llX +0x48C=%u window=%.1fs updates_placed=%llu(+%llu) collision_bypassed=%llu(+%llu) "
          "collision_forwarded=%llu(+%llu) box_bypassed=%llu(+%llu) box_forwarded=%llu(+%llu) hook_calls=%llu(+%llu) "
          "controller_calls=%llu(+%llu) controller_mode=%u ui_hidden_by_edvr=%s ui_calls=%llu faults=%u waiting_updates=%llu contended=%llu "
          "foreign=%llu events_lost=%llu eye(up=%.3f forward=%.3f right=%.3f)",
          prefix(), h.phase, h.session ? "on" : "off", static_cast<unsigned long long>(h.activity), h.state, h.windowSeconds,
          static_cast<unsigned long long>(h.updates), static_cast<unsigned long long>(h.updatesWindow),
          static_cast<unsigned long long>(h.bypassed), static_cast<unsigned long long>(h.bypassedWindow),
          static_cast<unsigned long long>(h.forwarded), static_cast<unsigned long long>(h.forwardedWindow),
          static_cast<unsigned long long>(h.boxBypassed), static_cast<unsigned long long>(h.boxBypassedWindow),
          static_cast<unsigned long long>(h.boxForwarded), static_cast<unsigned long long>(h.boxForwardedWindow),
          static_cast<unsigned long long>(h.hookCalls), static_cast<unsigned long long>(h.hookCallsWindow),
          static_cast<unsigned long long>(h.ctlCalls), static_cast<unsigned long long>(h.ctlCallsWindow), h.ctlMode,
          h.uiHiddenByUs ? "yes" : "no", static_cast<unsigned long long>(h.uiCalls), h.faults,
          static_cast<unsigned long long>(h.waiting), static_cast<unsigned long long>(h.contended),
          static_cast<unsigned long long>(h.foreign), static_cast<unsigned long long>(h.lost), h.eye.up, h.eye.forward, h.eye.right);
}

// ---- the relays' machine code ------------------------------------------------------------------------------------------------
// This DLL loads more than two gigabytes from the game, so a five-byte E9 cannot reach a C++ replacement. CodeHook patches the
// target with an E9 to a relay placed within two gigabytes, and the relay does the rest. Both relays are copied from
// pose_reader_watch.cpp / object_record_writer_hook.cpp's pattern (grep kRelayBytes); RAX, R11 and the flags are volatile and
// none of the observed functions reads them on entry (the box push's first instruction, `mov rax,rsp`, sets RAX itself, and the
// trampoline re-runs it).
//
// THE CALLBACK RELAY (44 bytes): a gate in memory, a C++ callback, the trampoline. The free-camera update, the camera UI's update
// and the controller's update use it, each with a gate and a callback of its own.
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

// THE BYPASS RELAY (68 bytes), for the collision step and the box push. No C function sits between the game and the original: the
// stack argument is never touched and the point the box push would rewrite is left alone.
//   mov rax,&placed; mov rax,[rax]; test rax,rax; je forward        ; nobody placed: the game's function runs
//   cmp rcx,rax; jne forward                                          ; a different activity: the game's function runs
//   mov r11,&bypassed; lock inc qword ptr [r11]; xor eax,eax; ret    ; the placed activity: "no edit"
//   forward: mov r11,&forwarded; lock inc qword ptr [r11]; jmp [trampoline]
constexpr size_t kBypassRelayBytes = 68, kBypassRelayTrampolineAt = 60, kBypassRelayPlacedAt = 2,
                 kBypassRelayBypassedAt = 25, kBypassRelayForwardedAt = 42;
inline void buildBypassRelay(uint8_t* code, const void* placed, void* bypassed, void* forwarded) {
    const uint8_t body[kBypassRelayBytes] = {
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
    std::memcpy(code + kBypassRelayPlacedAt, &p, 8);
    std::memcpy(code + kBypassRelayBypassedAt, &b, 8);
    std::memcpy(code + kBypassRelayForwardedAt, &f, 8);
}

}  // namespace ecm
}  // namespace edvr

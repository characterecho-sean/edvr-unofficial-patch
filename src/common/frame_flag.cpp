// GENERATED from src/common/frame_flag.cpp in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 4735533268d70aad]
#include "frame_flag.h"

#include <windows.h>

#include <cstdio>  // _snwprintf_s

namespace edvr {
namespace {

// flag      the frame in progress is marked
// consumer  openvr_api.dll has a live Submit hook and can act on the flag
//
// There was a third field, `counted`. Nothing ever read it: it was written by
// markGlitchFrame and reset by clearGlitchFrame, and the session total it once
// fed moved into a local counter in glitch_frame.cpp long ago. Removed along
// with the header's explanation of a mechanism that no longer exists.
struct Shared {
    volatile LONG flag;
    volatile LONG consumer;
    // externalCam  the player is on foot in the external camera, having come
    //              there from the flat panel
    //
    // Same shape of problem as `flag` and so the same channel: d3d11.dll is the
    // only half that can tell the modes apart -- it watches the panel composite
    // -- and openvr_api.dll is the only half that can act on the answer, because
    // the head pose passes through it. Neither can do the other's job.
    volatile LONG externalCam;
    // externalCamStamp  bumped on every write of externalCam, including the
    //                   writes that do not change it
    //
    // externalCam alone cannot distinguish "d3d11 says no" from "d3d11 has
    // stopped saying anything", and the difference decides whether a player's
    // viewpoint is still being moved in the cockpit. The writer sits inside a
    // fault-budgeted guard that stops running permanently after a few faults,
    // so "stopped saying anything" is a reachable state and not a theoretical
    // one.
    //
    // A counter rather than a timestamp: no clock, no wraparound handling worth
    // the name (2^32 frames is over a year at 90 Hz), and it compares with a
    // plain !=.
    volatile LONG externalCamStamp;
    // holdFrames  frames the openvr half should decline to submit, counting
    //             down, set by d3d11 when the player presses a key that starts
    //             a transition
    //
    // NOT a detection. Every other route in this file is one half telling the
    // other what it INFERRED; this is the player telling us directly. They
    // pressed the external-camera key, so a transition is starting -- there is
    // nothing to detect and nothing to get wrong about which mode we are in.
    //
    // It exists because during that transition Elite draws several frames from
    // somewhere the player is not, and no amount of detection helps: withholding
    // shows the PREVIOUS frame, and by the time a detector has recognised a bad
    // frame the previous one is already bad too. Starting the hold at the press
    // means the frame being held is the last good one before any of it.
    //
    // Separate from `flag` because `flag` is cleared at every WaitGetPoses by
    // design -- one detection must not suppress the frames after it -- and this
    // is the opposite: a deliberate run, counted down by the reader.
    volatile LONG holdFrames;
};

// Per PROCESS, not per logon session.
//
// This said Local\edvr_glitch_frame_v1 under a comment claiming it was scoped
// "so two copies of the game do not share one flag". Local\ is the per-logon
// BaseNamedObjects namespace: every process one user is running shares it. Two
// Elite clients -- a normal thing for multi-account play -- therefore shared a
// single flag, and each one's per-frame clear wiped the other's mark before it
// could be read. One client's flash was shown anyway, the other withheld a good
// frame, and the d3d11 half of a client with no openvr proxy installed at all
// would report "withheld" because the OTHER process had announced itself.
//
// The name is built once, at first use. The two DLLs are in the same process,
// so the channel between them is unaffected.
//
// _v5 because the struct changed again -- holdFrames was added. _v4 was
// externalCamStamp, _v3 the field before that. A mismatched pair from different
// builds must not agree on a layout they disagree about, and a d3d11.dll writing
// a fifth field into a four-field mapping made by an older openvr_api.dll would
// write past the end of it.
//
// The version bump matters more for this field than for the last one. An old
// openvr_api.dll paired with a new d3d11.dll would find no stamp at all, read
// zeros, and conclude the gate is dead -- which fails safe -- but the reverse
// pairing would have a new reader trusting a stamp nobody writes. Separate
// mappings make both pairings inert instead of subtly wrong.
const wchar_t* mappingName() {
    static wchar_t name[64];
    static bool built = false;
    if (!built) {
        _snwprintf_s(name, _TRUNCATE, L"Local\\edvr_glitch_frame_v5_%lu",
                     GetCurrentProcessId());
        built = true;
    }
    return name;
}

Shared* map() {
    static Shared* s = [] () -> Shared* {
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(Shared), mappingName());
        if (!h) return nullptr;
        // Deliberately not closed. The mapping must outlive both proxies, and a
        // handle leaked once per process is the cheapest way to guarantee it.
        void* p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
        return static_cast<Shared*>(p);
    }();
    return s;
}

}  // namespace

void markGlitchFrame() {
    Shared* s = map();
    if (!s) return;
    InterlockedExchange(&s->flag, 1);
}

bool glitchFrameMarked() {
    Shared* s = map();
    return s && InterlockedCompareExchange(&s->flag, 0, 0) != 0;
}

void unmarkGlitchFrame() {
    Shared* s = map();
    if (s) InterlockedExchange(&s->flag, 0);
}

void clearGlitchFrame() {
    Shared* s = map();
    if (!s) return;
    InterlockedExchange(&s->flag, 0);
}

void announceGlitchConsumer() {
    Shared* s = map();
    if (s) InterlockedExchange(&s->consumer, 1);
}

bool glitchConsumerPresent() {
    Shared* s = map();
    return s && InterlockedCompareExchange(&s->consumer, 0, 0) != 0;
}

void setExternalCameraOnFoot(bool on) {
    Shared* s = map();
    if (!s) return;
    InterlockedExchange(&s->externalCam, on ? 1 : 0);
    // The stamp moves on every call, not on every change. A gate that has
    // settled on "no" is publishing just as actively as one that is toggling,
    // and a reader that could not tell those apart would have to treat silence
    // as consent.
    InterlockedIncrement(&s->externalCamStamp);
}

bool externalCameraOnFoot() {
    Shared* s = map();
    // FALSE when the mapping could not be made, which is the safe direction: a
    // head offset that fails to apply leaves the game exactly as it was, while
    // one that fails to STOP applying moves the player's viewpoint in the
    // cockpit. Unlike the glitch flag, this one persists across frames, so a
    // wrong answer here does not expire on its own -- which is what
    // externalCameraOnFootLive is for.
    return s && InterlockedCompareExchange(&s->externalCam, 0, 0) != 0;
}

void requestSubmitHold(uint32_t frames) {
    Shared* s = map();
    if (!s) return;
    // Set, not added. A second press during a hold restarts it rather than
    // extending it: two transitions in quick succession is one event as far as
    // the player is concerned, and adding would let a rapid double-press hold
    // for twice as long as either press asked for.
    InterlockedExchange(&s->holdFrames, static_cast<LONG>(frames));
}

bool takeSubmitHoldFrame() {
    Shared* s = map();
    if (!s) return false;
    const LONG n = InterlockedCompareExchange(&s->holdFrames, 0, 0);
    if (n <= 0) return false;
    InterlockedDecrement(&s->holdFrames);
    return true;
}

bool externalCameraEverPublished() {
    Shared* s = map();
    // The stamp only ever moves when setExternalCameraOnFoot is called, so a
    // nonzero stamp is proof somebody published -- regardless of what they said.
    return s && InterlockedCompareExchange(&s->externalCamStamp, 0, 0) != 0;
}

bool externalCameraOnFootLive(uint32_t maxAgeFrames) {
    Shared* s = map();
    if (!s) return false;
    const LONG stamp = InterlockedCompareExchange(&s->externalCamStamp, 0, 0);

    // Reader-side state, so the writer needs no cooperation beyond bumping the
    // stamp. Function-local statics: each DLL has its own copy, and only the
    // openvr half calls this, once per frame from WaitGetPoses.
    static LONG lastStamp = 0;
    static uint32_t sinceMoved = 0;
    static bool everMoved = false;

    if (stamp != lastStamp) {
        lastStamp = stamp;
        sinceMoved = 0;
        everMoved = true;
    } else if (everMoved && sinceMoved < 0xFFFFFFFFu) {
        ++sinceMoved;
    }

    // Never moved means d3d11.dll has not published once -- not installed, or
    // its hooks never committed. That is not a "no" that has gone stale, it is
    // an absence of anybody to ask, and guessing "yes" would apply the offset
    // in every mode with no gate at all.
    if (!everMoved) return false;
    if (sinceMoved > maxAgeFrames) return false;
    return InterlockedCompareExchange(&s->externalCam, 0, 0) != 0;
}

}  // namespace edvr

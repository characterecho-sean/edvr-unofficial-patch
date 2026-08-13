// GENERATED from src/common/frame_flag.cpp in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 03e76ae774d92652]
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
// _v3 because the struct changed again -- externalCam was added. A mismatched
// pair from different builds must not agree on a layout they disagree about,
// and a d3d11.dll writing a third field into a two-field mapping made by an
// older openvr_api.dll would write past the end of it.
const wchar_t* mappingName() {
    static wchar_t name[64];
    static bool built = false;
    if (!built) {
        _snwprintf_s(name, _TRUNCATE, L"Local\\edvr_glitch_frame_v3_%lu",
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
    if (s) InterlockedExchange(&s->externalCam, on ? 1 : 0);
}

bool externalCameraOnFoot() {
    Shared* s = map();
    // FALSE when the mapping could not be made, which is the safe direction: a
    // head offset that fails to apply leaves the game exactly as it was, while
    // one that fails to STOP applying moves the player's viewpoint in the
    // cockpit. Unlike the glitch flag, this one persists across frames, so a
    // wrong answer here does not expire on its own.
    return s && InterlockedCompareExchange(&s->externalCam, 0, 0) != 0;
}

}  // namespace edvr

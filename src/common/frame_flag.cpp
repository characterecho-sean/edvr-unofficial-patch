#include "frame_flag.h"

#include <windows.h>

namespace edvr {
namespace {

// flag    the frame in progress is marked
// counted this frame has already been added to the total
//
// counted is separate from flag because the two have different lifetimes: flag
// goes up and down within a frame as the detector re-decides, counted is set
// once and survives until the frame boundary.
struct Shared {
    volatile LONG flag;
    volatile LONG counted;
    // Set by openvr_api.dll when its hook is live. Without it, d3d11.dll is
    // detecting bad frames that nothing is in a position to withhold.
    volatile LONG consumer;
};

// Session-local, so two copies of the game do not share one flag.
constexpr wchar_t kName[] = L"Local\\edvr_glitch_frame_v1";

Shared* map() {
    static Shared* s = [] () -> Shared* {
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(Shared), kName);
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
    InterlockedCompareExchange(&s->counted, 1, 0);
}

bool glitchFrameMarked() {
    Shared* s = map();
    return s && InterlockedCompareExchange(&s->flag, 0, 0) != 0;
}

void unmarkGlitchFrame() {
    Shared* s = map();
    if (s) InterlockedExchange(&s->flag, 0);   // counted deliberately left set
}

void clearGlitchFrame() {
    Shared* s = map();
    if (!s) return;
    InterlockedExchange(&s->flag, 0);
    InterlockedExchange(&s->counted, 0);
}

void announceGlitchConsumer() {
    Shared* s = map();
    if (s) InterlockedExchange(&s->consumer, 1);
}

bool glitchConsumerPresent() {
    Shared* s = map();
    return s && InterlockedCompareExchange(&s->consumer, 0, 0) != 0;
}

}  // namespace edvr

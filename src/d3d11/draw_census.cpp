#include "draw_census.h"

#include <cstdio>   // _snprintf_s: the sampled-slot list is built before logging

#include "../common/log.h"
#include "binding_shadow.h"

namespace edvr {
namespace {

// Three frames: enough that "present in every frame of one census and no
// frame of the other" separates a steady overlay from frame-to-frame churn,
// while two censuses stay near one percent of the default 4 MB log cap.
constexpr uint32_t kCensusFrames = 3;

// A frame on foot has been measured at 1174 eye draws, so three frames can
// legitimately want ~3500 lines. The cap exists so a mode this was never
// pointed at cannot eat the log; the end line says when it bit.
constexpr uint32_t kMaxLines = 4096;

// Distinct bound objects seen across one census. HUD-heavy frames bind a few
// dozen textures; 160 leaves an order of magnitude of headroom, and overflow
// degrades to an unresolved token rather than a lost line.
constexpr uint32_t kMaxInterned = 160;

// Everything below runs on the render thread: the draw records come from the
// immediate context's draw hooks (foreign contexts are filtered before the
// census is consulted), and both the arming hotkey and the frame boundary
// live in the Present path. No locks, and none are missing.
struct Interned {
    void*        ptr = nullptr;
    ResourceInfo info;
    bool         resolved = false;
};

bool     g_pending = false;      // key pressed, waiting for a frame edge
uint32_t g_framesLeft = 0;       // frames still to record; >0 means capturing
uint32_t g_censusNo = 0;         // numbers the begin/end lines, so the diff
                                 // tool can pair "absent" with "present"
uint32_t g_frameOrdinal = 0;     // 0-based frame within the running census
uint32_t g_draws = 0;            // draws recorded (kept counting past the cap)
uint32_t g_drawsThisFrame = 0;
uint32_t g_lines = 0;
uint32_t g_overflow = 0;         // intern-table misses
Interned g_tab[kMaxInterned];
uint32_t g_tabCount = 0;

// The table index for a bound object, interning on first sight. Resolution
// happens here, at record time, while the binding is certainly alive --
// resolving at dump time would probe pointers three frames stale, which is
// the exact crash class binding_shadow exists to contain. -1 is "not bound",
// -2 is "table full".
int internOf(void* ptr, bool isView) {
    if (!ptr) return -1;
    for (uint32_t i = 0; i < g_tabCount; ++i) {
        if (g_tab[i].ptr == ptr) return static_cast<int>(i);
    }
    if (g_tabCount >= kMaxInterned) {
        ++g_overflow;
        return -2;
    }
    Interned& e = g_tab[g_tabCount];
    e.ptr = ptr;
    // Constant buffers are not views: bindingResolve casts to ID3D11View and
    // asks GetResource, which on a buffer is a different method entirely.
    // Their identity still groups draws; their contents are not the census's
    // business.
    e.resolved = isView && bindingResolve(ptr, &e.info);
    return static_cast<int>(g_tabCount++);
}

// "@3", "-" (not bound) or "?" (table full) into a caller's buffer.
const char* idToken(int id, char* buf, size_t n) {
    if (id == -1) return "-";
    if (id == -2) return "?";
    _snprintf_s(buf, n, _TRUNCATE, "@%d", id);
    return buf;
}

void dumpInternTable() {
    for (uint32_t i = 0; i < g_tabCount; ++i) {
        const Interned& e = g_tab[i];
        if (!e.resolved) {
            Log::get().note("DC id @%u ?", i);
        } else if (e.info.isTexture2D) {
            Log::get().note("DC id @%u tex %ux%u fmt=%u", i, e.info.a, e.info.b,
                            e.info.fmt);
        } else if (e.info.isBuffer) {
            Log::get().note("DC id @%u buf %u", i, e.info.a);
        } else {
            Log::get().note("DC id @%u ?", i);
        }
    }
}

void finish() {
    dumpInternTable();
    Log::get().note("DC end census=%u draws=%u lines=%u interned=%u overflow=%u "
                    "truncated=%u",
                    g_censusNo, g_draws, g_lines, g_tabCount, g_overflow,
                    g_draws > g_lines ? g_draws - g_lines : 0);
}

}  // namespace

bool drawCensusArmed() { return g_pending || g_framesLeft > 0; }

void drawCensusRequest() {
    if (drawCensusArmed()) {
        Log::get().note("DC: the census key was pressed while a census is "
                        "already running; ignored. One at a time.");
        return;
    }
    g_pending = true;
    Log::get().note("DC: census armed -- the next %u whole frames of eye-texture "
                    "draws will be logged. Diff two of these with "
                    "tools/diff_draw_census.py.",
                    kCensusFrames);
}

void drawCensusEyeDraw(char kind, uint32_t count, uint32_t instances,
                       uint32_t eyeDrawIndex) {
    if (g_framesLeft == 0) return;   // pending counts draws only once started
    ++g_draws;
    ++g_drawsThisFrame;
    if (g_lines >= kMaxLines) return;
    ++g_lines;

    char rb[8], db[8], cb[8], s0b[8], s1b[8], s2b[8], s3b[8];
    const char* r = idToken(internOf(bindingGet(BindSlot::Rtv0), true), rb, sizeof(rb));
    const char* d = idToken(internOf(bindingGet(BindSlot::Dsv0), true), db, sizeof(db));
    const char* c = idToken(internOf(bindingGet(BindSlot::VsCb0), false), cb, sizeof(cb));
    const char* s0 = idToken(internOf(bindingGet(BindSlot::PsSrv0), true), s0b, sizeof(s0b));
    const char* s1 = idToken(internOf(bindingGet(BindSlot::PsSrv1), true), s1b, sizeof(s1b));
    const char* s2 = idToken(internOf(bindingGet(BindSlot::PsSrv2), true), s2b, sizeof(s2b));
    const char* s3 = idToken(internOf(bindingGet(BindSlot::PsSrv3), true), s3b, sizeof(s3b));

    Log::get().note("DC %u #%u %c n=%u i=%u r=%s d=%s c=%s s=%s,%s,%s,%s",
                    g_frameOrdinal, eyeDrawIndex, kind, count, instances, r, d, c,
                    s0, s1, s2, s3);
}

void drawCensusFrameBoundary(uint32_t frameNo) {
    if (g_framesLeft > 0) {
        Log::get().note("DC frame %u draws=%u", g_frameOrdinal, g_drawsThisFrame);
        g_drawsThisFrame = 0;
        ++g_frameOrdinal;
        if (--g_framesLeft == 0) finish();
    }
    if (g_pending) {
        g_pending = false;
        ++g_censusNo;
        g_framesLeft = kCensusFrames;
        g_frameOrdinal = 0;
        g_draws = 0;
        g_drawsThisFrame = 0;
        g_lines = 0;
        g_overflow = 0;
        g_tabCount = 0;
        for (Interned& e : g_tab) e = Interned();
        Log::get().note("DC begin census=%u frames=%u frame=%u", g_censusNo,
                        kCensusFrames, frameNo);
    }
}

}  // namespace edvr

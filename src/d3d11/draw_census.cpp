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

// Distinct bound objects seen across one census. The first field capture
// refuted the "a few dozen" estimate this started from: a cockpit census
// interned 160 and missed 1216 more -- Elite runs hundreds of distinct
// eye-sized views and per-state textures through three frames, and the
// table filled before the LATE-frame render targets arrived, which cost the
// exact draws the census existed to name. 512 covers what was measured with
// three times over; past it, tokens degrade to inline resolution (below),
// never to an unusable '?'.
constexpr uint32_t kMaxInterned = 512;

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

// The token a draw line carries for one bound object: "@N" while the table
// has room, "-" for unbound -- and on a full table the resolved form INLINE
// ("tex512x64f28", "buf256"), so a signature survives the overflow. The
// first field capture wrote '?' instead, and the '?' landed on the late
// render targets and half the sampled slots: the diff ran but could not
// name what it found. Same compact spelling the diff tool normalises the
// id table to, so a slot interned in one census and inline in the other
// still compares equal. Constant buffers are not views and get no inline
// form; measured, they intern within the first frames and never overflowed.
const char* bindingToken(void* ptr, bool isView, char* buf, size_t n) {
    if (!ptr) return "-";
    const int id = internOf(ptr, isView);
    if (id >= 0) {
        _snprintf_s(buf, n, _TRUNCATE, "@%d", id);
        return buf;
    }
    ResourceInfo info;
    if (isView && bindingResolve(ptr, &info)) {
        if (info.isTexture2D) {
            _snprintf_s(buf, n, _TRUNCATE, "tex%ux%uf%u", info.a, info.b,
                        info.fmt);
            return buf;
        }
        if (info.isBuffer) {
            _snprintf_s(buf, n, _TRUNCATE, "buf%u", info.a);
            return buf;
        }
    }
    return "?";
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

    char rb[24], db[24], cb[24], s0b[24], s1b[24], s2b[24], s3b[24];
    const char* r = bindingToken(bindingGet(BindSlot::Rtv0), true, rb, sizeof(rb));
    const char* d = bindingToken(bindingGet(BindSlot::Dsv0), true, db, sizeof(db));
    const char* c = bindingToken(bindingGet(BindSlot::VsCb0), false, cb, sizeof(cb));
    const char* s0 = bindingToken(bindingGet(BindSlot::PsSrv0), true, s0b, sizeof(s0b));
    const char* s1 = bindingToken(bindingGet(BindSlot::PsSrv1), true, s1b, sizeof(s1b));
    const char* s2 = bindingToken(bindingGet(BindSlot::PsSrv2), true, s2b, sizeof(s2b));
    const char* s3 = bindingToken(bindingGet(BindSlot::PsSrv3), true, s3b, sizeof(s3b));

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

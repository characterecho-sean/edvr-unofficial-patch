#include "fss_res.h"

#include <windows.h>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/frame_flag.h"   // eyeTextureSize: the published per-eye size
#include "../common/log.h"

namespace edvr {

// fssResActive reads this from the header with no call: asked on the
// viewport paths, and the build has no /GL to fold a cross-TU getter.
namespace detail {
uint32_t g_fssResCount = 0;
}  // namespace detail

namespace {

// Entries are never removed -- the game gives no release signal we hook --
// so a stale pointer can sit here until overwritten. It is compared by
// identity and never dereferenced, and a recycled allocation would have to
// land on the same address AND present the pre-inflation viewport against
// that target to be acted on; accepted, and the note below says which
// texture every action was for.
//
// Four zoom-ins of colour+depth pairs is eight; the ring must never evict a
// texture that is STILL BOUND (its viewport would stop being scaled while
// its target stays inflated, and the body would draw into a corner of
// itself). The array is pointers and two sizes; 32 is headroom.
constexpr uint32_t kTracked = 32;

// The game asks for eye/2 per axis. Halving an odd size floors or ceils
// depending on the engine's arithmetic, and the published eye size itself is
// rounded from bounds -- so both halves are accepted, the same reasoning as
// the eye test's own two-pixel tolerance.
bool halfOf(uint32_t v, uint32_t whole) {
    return v == whole / 2 || v == (whole + 1) / 2;
}

struct Tracked {
    void*    tex = nullptr;
    uint32_t w = 0;   // the size the game asked for -- viewports arrive in it
    uint32_t h = 0;
    float    scale = 1.0f;    // what it grew by; the viewport paths multiply by it
};

struct State {
    bool     fssRule = false;   // experimental.fss_res: the half-eye matcher
    bool     announced = false;
    Tracked  tracked[kTracked];
    uint32_t next = 0;
    uint32_t inflateNotes = 0;
    uint32_t scaled = 0;         // viewports scaled at RSSetViewports
    uint32_t scaledLate = 0;     // caught by the draw-time backstop instead
    uint32_t scaleNotes = 0;
    uint32_t copyNotes = 0;      // a copy/resolve touched a tracked texture
};
State g_s;

}  // namespace

void fssResConfigure(Config& cfg) {
    const bool was = g_s.fssRule;
    g_s.fssRule = cfg.getBool("experimental.fss_res", false);
    if (g_s.fssRule && !was && !g_s.announced) {
        g_s.announced = true;
        Log::get().note(
            "fss res: ON. The scanner's body layer will be created at full "
            "eye resolution instead of half when the FSS is next opened -- "
            "textures are made per zoom, so no restart is needed.");
    }
}

bool fssResWantsCreates() {
    return g_s.fssRule;
}

bool fssResMaybeInflate(D3D11_TEXTURE2D_DESC* d, bool hasInitialData, float* scaleOut) {
    if (!g_s.fssRule || !d || hasInitialData) return false;
    // Only the exact shape measured: a single-mip, non-MSAA render target or
    // depth texture. Anything else -- staging, arrays, mip chains -- is not
    // the body layer, whatever its size.
    if (d->ArraySize != 1 || d->SampleDesc.Count != 1 || d->MipLevels > 1) {
        return false;
    }
    if (!(d->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL))) {
        return false;
    }
    uint32_t ew = 0, eh = 0;
    if (!eyeTextureSize(&ew, &eh) || !halfOf(d->Width, ew) || !halfOf(d->Height, eh)) {
        return false;
    }
    d->Width *= 2;
    d->Height *= 2;
    if (scaleOut) *scaleOut = 2.0f;
    return true;
}

void fssResNoteCreated(void* texture, uint32_t origW, uint32_t origH,
                       uint32_t newW, uint32_t newH, float scale) {
    if (!texture || !(scale > 1.0f)) return;
    Tracked& t = g_s.tracked[g_s.next];
    g_s.next = (g_s.next + 1) % kTracked;
    t.tex = texture;
    t.w = origW;
    t.h = origH;
    t.scale = scale;
    ++detail::g_fssResCount;
    if (g_s.inflateNotes < 8) {
        ++g_s.inflateNotes;
        Log::get().note(
            "fss res: a %ux%u texture was created at %ux%u (%gx). Its "
            "viewports are scaled to match as they arrive. Said at most 8 "
            "times.",
            origW, origH, newW, newH, static_cast<double>(scale));
    }
}

bool fssResIsInflated(void* resource) {
    if (!resource || detail::g_fssResCount == 0) return false;
    for (const Tracked& t : g_s.tracked) {
        if (t.tex == resource) return true;
    }
    return false;
}

bool fssResOrigSize(void* resource, uint32_t* w, uint32_t* h) {
    if (!resource || detail::g_fssResCount == 0) return false;
    for (const Tracked& t : g_s.tracked) {
        if (t.tex == resource) {
            *w = t.w;
            *h = t.h;
            return true;
        }
    }
    return false;
}

float fssResScaleOf(void* resource) {
    if (!resource || detail::g_fssResCount == 0) return 1.0f;
    for (const Tracked& t : g_s.tracked) {
        if (t.tex == resource) return t.scale;
    }
    return 1.0f;
}

// Turning the rule off does NOT untrack what it already inflated: those
// textures are still the wrong size for their viewports, and the game holds
// them until it releases them. So this stays true while anything is tracked.
void fssResNoteViewportScaled(bool late) {
    if (late) {
        ++g_s.scaledLate;
    } else {
        ++g_s.scaled;
    }
    // The first few of each kind, then silence: the counts prove the
    // mechanism engaged, and "late" firing at all means the game set the
    // viewport before binding the target -- worth knowing, not worth a
    // line per draw.
    if (g_s.scaleNotes < 6) {
        ++g_s.scaleNotes;
        Log::get().note(
            "fss res: an inflated target's viewport was scaled to match it "
            "(%s; %u at set, %u at draw so far).",
            late ? "by the draw-time backstop" : "as it was set", g_s.scaled,
            g_s.scaledLate);
    }
}

void fssResNoteCopyMaybeMismatched(void* dst, void* src) {
    if (detail::g_fssResCount == 0) return;
    void* which = nullptr;
    if (dst && fssResIsInflated(dst)) {
        which = dst;
    } else if (src && fssResIsInflated(src)) {
        which = src;
    }
    if (!which) return;
    uint32_t ow = 0, oh = 0;
    fssResOrigSize(which, &ow, &oh);
    const float scale = fssResScaleOf(which);
    if (g_s.copyNotes < 8) {
        ++g_s.copyNotes;
        Log::get().note(
            "fss res: a copy or resolve touched an inflated texture (%s, "
            "tracked at %ux%u -> %.0fx%.0f, %gx). This does not rescale the "
            "copy's own box or check the other side's size -- no such copy "
            "has been observed landing in one of these surfaces, so this is "
            "a detector, not a fix, until one is. Said at most 8 times.",
            which == dst ? "as the destination" : "as the source", ow, oh,
            static_cast<double>(ow) * static_cast<double>(scale),
            static_cast<double>(oh) * static_cast<double>(scale),
            static_cast<double>(scale));
    }
}

}  // namespace edvr

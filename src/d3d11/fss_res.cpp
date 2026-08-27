#include "fss_res.h"

#include <windows.h>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/frame_flag.h"   // eyeTextureSize: the published per-eye size
#include "../common/log.h"

namespace edvr {
namespace {

// Enough for four zoom-ins of color+depth pairs before the oldest entry is
// recycled. Entries are never removed -- the game gives no release signal we
// hook -- so a stale pointer can sit here until overwritten. It is compared
// by identity and never dereferenced, and a recycled allocation would have
// to land on the same address AND present the half-eye viewport against an
// eye-sized target to be acted on; accepted, and the note below says which
// texture every action was for.
constexpr uint32_t kTracked = 8;

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
};

struct State {
    bool     enabled = false;
    bool     announced = false;
    Tracked  tracked[kTracked];
    uint32_t next = 0;
    uint32_t count = 0;        // total ever tracked; >0 makes fssResActive true
    uint32_t inflateNotes = 0;
    uint32_t scaled = 0;       // viewports scaled at RSSetViewports
    uint32_t scaledLate = 0;   // caught by the draw-time backstop instead
    uint32_t scaleNotes = 0;
};
State g_s;

}  // namespace

void fssResConfigure(Config& cfg) {
    const bool was = g_s.enabled;
    g_s.enabled = cfg.getBool("experimental.fss_res", false);
    if (g_s.enabled && !was && !g_s.announced) {
        g_s.announced = true;
        Log::get().note(
            "fss res: ON. The scanner's body layer will be created at full "
            "eye resolution instead of half when the FSS is next opened -- "
            "textures are made per zoom, so no restart is needed.");
    }
}

bool fssResWantsCreates() { return g_s.enabled; }

bool fssResMaybeInflate(D3D11_TEXTURE2D_DESC* d, bool hasInitialData) {
    if (!g_s.enabled || !d || hasInitialData) return false;
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
    if (!eyeTextureSize(&ew, &eh)) return false;
    if (!halfOf(d->Width, ew) || !halfOf(d->Height, eh)) return false;
    d->Width *= 2;
    d->Height *= 2;
    return true;
}

void fssResNoteCreated(void* texture, uint32_t origW, uint32_t origH) {
    if (!texture) return;
    Tracked& t = g_s.tracked[g_s.next];
    g_s.next = (g_s.next + 1) % kTracked;
    t.tex = texture;
    t.w = origW;
    t.h = origH;
    ++g_s.count;
    if (g_s.inflateNotes < 8) {
        ++g_s.inflateNotes;
        Log::get().note(
            "fss res: a %ux%u body-layer texture was created at %ux%u -- "
            "full eye resolution. Its viewports are scaled to match as they "
            "arrive. Said at most 8 times.",
            origW, origH, origW * 2, origH * 2);
    }
}

bool fssResIsInflated(void* resource) {
    if (!resource || g_s.count == 0) return false;
    for (const Tracked& t : g_s.tracked) {
        if (t.tex == resource) return true;
    }
    return false;
}

bool fssResOrigSize(void* resource, uint32_t* w, uint32_t* h) {
    if (!resource || g_s.count == 0) return false;
    for (const Tracked& t : g_s.tracked) {
        if (t.tex == resource) {
            *w = t.w;
            *h = t.h;
            return true;
        }
    }
    return false;
}

bool fssResActive() { return g_s.enabled && g_s.count != 0; }

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
            "fss res: a body-layer viewport was scaled to full resolution "
            "(%s; %u at set, %u at draw so far).",
            late ? "by the draw-time backstop" : "as it was set", g_s.scaled,
            g_s.scaledLate);
    }
}

}  // namespace edvr

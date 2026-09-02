#include "fss_res.h"

#include <windows.h>

#include <d3d11.h>

#include <cstdlib>   // strtoul: the surface_inflate spec parser
#include <cstring>   // memcpy: the spec string kept to log only on a change
#include <string>

#include "../common/config.h"
#include "../common/frame_flag.h"   // eyeTextureSize: the published per-eye size
#include "../common/log.h"

namespace edvr {
namespace {

// Entries are never removed -- the game gives no release signal we hook --
// so a stale pointer can sit here until overwritten. It is compared by
// identity and never dereferenced, and a recycled allocation would have to
// land on the same address AND present the pre-inflation viewport against
// that target to be acted on; accepted, and the note below says which
// texture every action was for.
//
// Sized 32, not the 8 that served the FSS alone. Eight was four zoom-ins of
// colour+depth pairs, which was ample when the only matcher produced two
// textures. surface_inflate takes four sizes, each with a depth partner:
// three named surfaces plus an FSS body pair is exactly eight, and the
// moment anything is recreated the ring evicts an entry whose texture is
// STILL BOUND. Its viewport then stops being scaled while its target stays
// inflated, so that panel draws into a corner of itself -- a loud failure,
// but one that costs a flight to diagnose. The array is pointers and two
// sizes; there is no reason to be thrifty with it.
constexpr uint32_t kTracked = 32;

// The game asks for eye/2 per axis. Halving an odd size floors or ceils
// depending on the engine's arithmetic, and the published eye size itself is
// rounded from bounds -- so both halves are accepted, the same reasoning as
// the eye test's own two-pixel tolerance.
bool halfOf(uint32_t v, uint32_t whole) {
    return v == whole / 2 || v == (whole + 1) / 2;
}

// Four named surfaces at once. The cockpit census that motivated this found
// four interface surfaces in one frame, and naming them one at a time is how
// you find out which is which -- but naming two at once is how you find out
// whether they are the same panel seen twice.
constexpr uint32_t kSpecs = 4;

// D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION. A spec that would cross it is
// refused when it is read rather than at the create, so the log says the
// number is impossible instead of the texture quietly staying stock.
constexpr uint32_t kMaxDim = 16384;

struct Spec {
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t scale = 0;
};

struct Tracked {
    void*    tex = nullptr;
    uint32_t w = 0;   // the size the game asked for -- viewports arrive in it
    uint32_t h = 0;
    uint32_t scale = 1;   // what it grew by; the viewport paths multiply by it
};

struct State {
    bool     fssRule = false;   // experimental.fss_res: the half-eye matcher
    bool     announced = false;
    Spec     specs[kSpecs];
    uint32_t specCount = 0;
    char     specStr[128] = {};  // the raw setting, to log only on a change
    Tracked  tracked[kTracked];
    uint32_t next = 0;
    uint32_t count = 0;        // total ever tracked; >0 makes fssResActive true
    uint32_t inflateNotes = 0;
    uint32_t scaled = 0;       // viewports scaled at RSSetViewports
    uint32_t scaledLate = 0;   // caught by the draw-time backstop instead
    uint32_t scaleNotes = 0;
};
State g_s;

// "1952x1597:2, 908x1361" -- a size, optionally a factor, comma separated.
// The factor defaults to 2 and is capped at 4, because the point of the
// instrument is to see whether inflation helps at all and 4x is already
// sixteen times the pixels of a surface that may not be the right one.
//
// Refuses the WHOLE setting on any malformed entry rather than applying the
// entries it understood. A probe that half-applies is a probe whose result
// cannot be read: the clear_probe parser next door takes the same line, and
// for the same reason.
bool parseSpecs(const std::string& text, Spec* out, uint32_t* countOut) {
    uint32_t n = 0;
    const char* p = text.c_str();
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
        if (!*p) break;
        if (n == kSpecs) return false;
        char* end = nullptr;
        const unsigned long w = strtoul(p, &end, 10);
        if (end == p || (*end != 'x' && *end != 'X')) return false;
        const char* q = end + 1;
        const unsigned long h = strtoul(q, &end, 10);
        if (end == q) return false;
        unsigned long s = 2;
        if (*end == ':') {
            const char* r = end + 1;
            s = strtoul(r, &end, 10);
            if (end == r) return false;
        }
        while (*end == ' ' || *end == '\t') ++end;
        if (*end && *end != ',') return false;
        if (w == 0 || h == 0 || s < 2 || s > 4) return false;
        if (w * s > kMaxDim || h * s > kMaxDim) return false;
        out[n].w = static_cast<uint32_t>(w);
        out[n].h = static_cast<uint32_t>(h);
        out[n].scale = static_cast<uint32_t>(s);
        ++n;
        p = end;
    }
    *countOut = n;
    return true;
}

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

    // The size-named matcher. Logged only when the setting's TEXT changes,
    // the way census_skip is: this runs on every ini reload, and a probe
    // that reprints itself every few seconds buries the receipts it exists
    // to produce.
    const std::string spec = cfg.getString("advanced.surface_inflate", "");
    if (spec.length() >= sizeof(g_s.specStr)) {
        Log::get().note("surface inflate: the spec is longer than %u "
                        "characters and was ignored.",
                        static_cast<unsigned>(sizeof(g_s.specStr)) - 1);
        return;
    }
    if (spec == g_s.specStr) return;
    memcpy(g_s.specStr, spec.c_str(), spec.length() + 1);

    if (spec.empty()) {
        g_s.specCount = 0;
        Log::get().note("surface inflate: off. Surfaces are created at the "
                        "size the game asks for.");
        return;
    }
    // n == 0 from text that is not empty means a spec of nothing but
    // separators. Refused out loud with the rest: the one outcome this
    // instrument must never have is doing nothing quietly.
    Spec parsed[kSpecs];
    uint32_t n = 0;
    if (!parseSpecs(spec, parsed, &n) || n == 0) {
        g_s.specCount = 0;
        Log::get().note(
            "surface inflate: \"%s\" is not up to %u entries of "
            "WIDTHxHEIGHT[:FACTOR], factor 2 to 4 and no side over %u after "
            "scaling. Refused whole rather than half-applied; no surface is "
            "inflated. The setting is surface_inflate under [advanced].",
            spec.c_str(), kSpecs, kMaxDim);
        return;
    }
    g_s.specCount = n;
    for (uint32_t i = 0; i < n; ++i) {
        g_s.specs[i] = parsed[i];
        Log::get().note(
            "surface inflate: a %ux%u render target or depth texture will be "
            "created at %ux%u (%ux) and its viewport scaled to match. Takes "
            "effect the next time the game makes one -- for a cockpit panel "
            "that is the next trip through the main menu. If nothing below "
            "says a texture WAS created, the size is not one the game asks "
            "for on this rig: read it from your own census.",
            g_s.specs[i].w, g_s.specs[i].h, g_s.specs[i].w * g_s.specs[i].scale,
            g_s.specs[i].h * g_s.specs[i].scale, g_s.specs[i].scale);
    }
}

bool fssResWantsCreates() { return g_s.fssRule || g_s.specCount != 0; }

bool fssResMaybeInflate(D3D11_TEXTURE2D_DESC* d, bool hasInitialData) {
    if (!fssResWantsCreates() || !d || hasInitialData) return false;
    // Only the exact shape measured: a single-mip, non-MSAA render target or
    // depth texture. Anything else -- staging, arrays, mip chains -- is not
    // the body layer, whatever its size. An interface surface is the same
    // shape: colour target plus a depth partner, one mip, no MSAA, and a
    // spec naming its size matches BOTH, which is what the panel needs.
    if (d->ArraySize != 1 || d->SampleDesc.Count != 1 || d->MipLevels > 1) {
        return false;
    }
    if (!(d->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL))) {
        return false;
    }
    // The FSS rule first: it is the shipped one, and it is the narrower
    // match of the two -- a spec that happened to name eye/2 would then be
    // redundant rather than fighting it.
    if (g_s.fssRule) {
        uint32_t ew = 0, eh = 0;
        if (eyeTextureSize(&ew, &eh) && halfOf(d->Width, ew) &&
            halfOf(d->Height, eh)) {
            d->Width *= 2;
            d->Height *= 2;
            return true;
        }
    }
    for (uint32_t i = 0; i < g_s.specCount; ++i) {
        const Spec& s = g_s.specs[i];
        if (d->Width != s.w || d->Height != s.h) continue;
        d->Width *= s.scale;
        d->Height *= s.scale;
        return true;
    }
    return false;
}

void fssResNoteCreated(void* texture, uint32_t origW, uint32_t origH,
                       uint32_t scale) {
    if (!texture || scale < 2) return;
    Tracked& t = g_s.tracked[g_s.next];
    g_s.next = (g_s.next + 1) % kTracked;
    t.tex = texture;
    t.w = origW;
    t.h = origH;
    t.scale = scale;
    ++g_s.count;
    if (g_s.inflateNotes < 8) {
        ++g_s.inflateNotes;
        Log::get().note(
            "fss res: a %ux%u texture was created at %ux%u (%ux). Its "
            "viewports are scaled to match as they arrive. Said at most 8 "
            "times.",
            origW, origH, origW * scale, origH * scale, scale);
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

uint32_t fssResScaleOf(void* resource) {
    if (!resource || g_s.count == 0) return 1;
    for (const Tracked& t : g_s.tracked) {
        if (t.tex == resource) return t.scale;
    }
    return 1;
}

// Turning a matcher off does NOT untrack what it already inflated: those
// textures are still the wrong size for their viewports, and the game holds
// them until it releases them. So this stays true while anything is tracked.
bool fssResActive() { return g_s.count != 0; }

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

}  // namespace edvr

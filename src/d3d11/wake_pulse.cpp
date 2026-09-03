#include "wake_pulse.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>     // hypotf: the ring test
#include <cstdio>    // _snprintf_s: the candidate list
#include <cstdlib>   // strtoul: the index-count list
#include <string>

#include "../common/config.h"
#include "binding_shadow.h"
#include "../common/log.h"

namespace edvr {
namespace {

// The panel surface, by its ASPECT.
//
// This shipped keyed on the surface's fraction of the eye, and that was
// wrong. Measured across three render configurations:
//
//   eye 5424x5356   2440x1996   aspect 1.22244   w/eye 0.4499
//   eye 4340x4284   1952x1597   aspect 1.22229   w/eye 0.4498
//   eye 3072x3264   1492x1221   aspect 1.22195   w/eye 0.4857
//
// The aspect holds to four figures; the width fraction does not, because
// the surface tracks the game's INTERNAL render width, which is not a fixed
// multiple of the submitted eye. On the third configuration the fraction is
// 0.036 out -- nine times the tolerance this used -- so the fix silently
// never engaged there at all. Reported from the field before it was found
// here, which is the honest order.
//
// Each panel has its own stable aspect (1.222 here, 1.600, 4.65 and 0.667
// for the others), so this is what identifies one panel among them.
constexpr float kAspect = 1.2224f;
constexpr float kAspectTol = 0.01f;

// And a floor, so a small texture that happens to be 1.22:1 cannot match.
// The real surface is a render target sized from the display.
constexpr uint32_t kMinWidth = 512;

constexpr char     kKind = 'X';

// 288 quads, measured where the panel comes out 2440x1996. The count is
// TESSELLATION, not content: the GUI subdivides its vector curves by pixel
// size, so the same widget on a smaller panel needs fewer segments. Measured
// on one machine, two headsets, same build and same ship -- panel 2440x1996
// takes 1728 indices and panel 1002x820 does not, which is why this shipped
// working on one headset and dead on the other.
//
// So the default is only ever right for one panel size, and the useful thing
// this module does about it is NAME THE CANDIDATES in the log rather than
// leave the owner to census it. See the intermittency table below.
constexpr uint32_t kIndices = 1728;    // panel 2440x1996
constexpr uint32_t kIndicesAlt = 576;   // panel 1002x820, same shader

// 891 stood here as the small panel's count for a day and was never this
// widget: 891 is not divisible by six, so it cannot be a quad list at all.
// It was picked by ranking counts on how OFTEN they were drawn, which is a
// measure of blinking rather than of being the marker, and it dropped draws
// without stopping the flash. The geometry below replaced that method.

// A LIST, not a number, because the count is TESSELLATION. Both measured
// points are the same widget at two subdivisions -- 96 quads on a 1002-wide
// panel, 288 on a 2440-wide one -- so a third render resolution would want
// a third number and no list ever finishes. The list is a head start for
// the two panels already measured; the ring test below is what actually
// identifies it.
constexpr uint32_t kMaxIndices = 8;

// SELF-CALIBRATION BY SHAPE, because a list only covers panels somebody has
// already measured, and the count moves with the panel's pixel size.
//
// What the marker IS, measured on a Q3 (2026-09-03) by capturing every
// vector draw into this panel across six frames without a wake and three
// with one, and keeping the geometry present in all three and none of the
// six: ONE draw, 576 indices, 96 quads, every quad centred on a circle
// 0.46 to 0.49 of the way out from the panel's centre and spaced about 11
// degrees apart. A 32-segment ring hugging the panel's edge, and the only
// thing on that panel drawn exclusively while the flash is up.
//
// The tessellation changes with the panel; the ring does not. So the test
// is: are ALL of this draw's vertices inside a thin annulus about the
// panel's centre? Nothing else on this panel is -- the clip frames reach
// the corners, the widgets are rectangles, the text is a strip. One
// vertex-buffer readback settles it, and afterwards the adopted count
// matches for free.
constexpr uint32_t kMinShapeIndices = 60;    // a quad is 6; the marker is not

// The coordinate space these panels are authored in: every position measured
// on them lands in -32765..32764, and the clip frames span it exactly. A
// draw whose vertices fall outside it is not in the space this test assumes,
// and is refused rather than guessed at.
constexpr float kSpaceHalf = 32765.0f;
constexpr float kSpaceSize = 65529.0f;

// The annulus. Measured 0.460..0.491; the band is wide enough for a ring
// drawn a little in or out of that on another panel and nowhere near wide
// enough to admit a rectangle, whose corners reach 0.64 or more.
constexpr float kRingLo = 0.38f;
constexpr float kRingHi = 0.56f;
constexpr uint32_t kRingMinQuads = 24;   // 96 measured; a token circle is not this

// A candidate that is drawn in nearly every frame is furniture, not a pulse,
// whatever its shape -- a permanent ring border on some other panel must not
// be adopted. Geometry says WHICH; this says it was ever absent.
constexpr uint32_t kMaxCandPct = 80;

constexpr uint32_t kRingSettle = 4;      // frames before the copies are mapped
constexpr uint32_t kVbCopyMax = 4u << 20;
constexpr uint32_t kTestNotes = 8;       // shape tests named in the log

bool     g_learned = false;
uint32_t g_testNotes = 0;

// The one capture in flight, if any.
ID3D11Buffer* g_ibStage = nullptr;
ID3D11Buffer* g_vbStage = nullptr;
bool          g_capPending = false;
uint32_t      g_capSettle = 0;
uint32_t      g_capCount = 0;
int           g_capBase = 0;
uint32_t      g_capStride = 0;
uint32_t      g_capVbBytes = 0;
bool          g_capI16 = true;
// The context the copies were queued on, held so the readback a few frames
// later does not need one handed to it. wakePulseReport takes no arguments
// and this is not reason enough to change that.
ID3D11DeviceContext* g_capCtx = nullptr;

bool     g_off = false;
uint32_t g_indices[kMaxIndices] = {kIndices, kIndicesAlt};
uint32_t g_indexCount = 2;
uint64_t g_dropped = 0;
uint64_t g_lastReported = 0;   // the count the last line printed
uint32_t g_reports = 0;        // how many lines have been printed at all

// Told apart so a field report can say WHICH half failed: the panel was
// never found, or it was found and no draw of the named size landed in it.
uint64_t g_surfaceSeen = 0;
uint32_t g_panelW = 0, g_panelH = 0;
bool     g_saidNoMatch = false;

// The intermittency table: which index counts land in the panel, and in how
// many DISTINCT frames each.
//
// The flash is the one draw that is there on some frames and not others --
// that is what a pulse is, and it is the property that does not change with
// resolution, build or ship. Everything else on the panel is drawn every
// frame. So a count seen intermittently is a candidate, the leader among
// them is used, and the log names them all either way rather than asking
// the owner for a census.
// 48, not 24. A 1253x1025 panel draws about twenty-five distinct index
// counts in ordinary flight, so a 24-slot table is FULL by the time the
// marker first appears -- and the marker's count arrives late by definition,
// because it is only drawn during a wake. The first cut of the shape test
// asked for a candidate to be in this table and so could never see the one
// draw it was looking for (2026-09-03).
constexpr uint32_t kSeenSlots = 48;
struct Seen {
    uint32_t indices = 0;
    uint32_t frames = 0;      // distinct frames it appeared in
    uint32_t lastFrame = 0;   // so one frame counts once
    bool     tried = false;   // the shape test has already ruled it out
};
Seen     g_seen[kSeenSlots];
uint32_t g_seenCount = 0;
uint32_t g_frame = 0;         // every frame, so one frame counts a count once
uint32_t g_panelFrames = 0;   // frames the panel was drawn in -- the denominator
uint32_t g_panelLastFrame = 0;


void ringDropCapture() {
    g_capPending = false;
    g_capSettle = 0;
    g_capCount = 0;
    if (g_capCtx) { g_capCtx->Release(); g_capCtx = nullptr; }
}

// Copy this draw's index slice and its vertex buffer, to be read back a few
// frames later. Nothing is mapped here: mapping a resource the GPU has not
// finished with is the stall this whole design exists to avoid.
void ringStartCapture(ID3D11DeviceContext* ctx, uint32_t count,
                      uint32_t startIndex, int baseVertex) {
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN;
    UINT ibOff = 0;
    ctx->IAGetIndexBuffer(&ib, &ibFmt, &ibOff);
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, vbOff = 0;
    ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &vbOff);
    if (!ib || !vb || stride < 8) {
        if (ib) ib->Release();
        if (vb) vb->Release();
        return;
    }
    const UINT idxSize = (ibFmt == DXGI_FORMAT_R16_UINT) ? 2u : 4u;
    const UINT need = count * idxSize;
    D3D11_BUFFER_DESC vd{};
    vb->GetDesc(&vd);
    const UINT vbBytes = vd.ByteWidth > kVbCopyMax ? kVbCopyMax : vd.ByteWidth;

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    bool ok = dev != nullptr && need <= 64u * 1024u;
    if (ok && !g_ibStage) {
        D3D11_BUFFER_DESC sd{};
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.ByteWidth = 64u * 1024u;
        ok = SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &g_ibStage));
    }
    if (ok && (!g_vbStage || g_capVbBytes < vbBytes)) {
        if (g_vbStage) { g_vbStage->Release(); g_vbStage = nullptr; }
        D3D11_BUFFER_DESC sd{};
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.ByteWidth = vbBytes;
        ok = SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &g_vbStage));
    }
    if (ok) {
        D3D11_BOX box{};
        box.left = ibOff + startIndex * idxSize;
        box.right = box.left + need;
        box.bottom = 1;
        box.back = 1;
        ctx->CopySubresourceRegion(g_ibStage, 0, 0, 0, 0, ib, 0, &box);
        D3D11_BOX vbox{};
        vbox.left = 0;
        vbox.right = vbBytes;
        vbox.bottom = 1;
        vbox.back = 1;
        ctx->CopySubresourceRegion(g_vbStage, 0, 0, 0, 0, vb, 0, &vbox);
        g_capPending = true;
        g_capSettle = kRingSettle;
        g_capCount = count;
        // baseVertex alone, as the probe that found this widget addresses
        // it. The copy starts at the buffer's byte zero either way.
        g_capBase = baseVertex;
        g_capStride = stride;
        g_capVbBytes = vbBytes;
        g_capI16 = idxSize == 2;
        if (g_capCtx) g_capCtx->Release();
        g_capCtx = ctx;
        g_capCtx->AddRef();
    }
    if (dev) dev->Release();
    ib->Release();
    vb->Release();
}

// Is every vertex of the captured draw on one thin circle about the panel's
// centre? Positions are the float2 at offset 0, which is where every draw
// measured on these panels carries them.
bool ringDecide(ID3D11DeviceContext* ctx, float* loOut, float* hiOut) {
    *loOut = 0.0f;
    *hiOut = 0.0f;
    if (!g_ibStage || !g_vbStage || !g_capCount || !g_capStride) return false;
    D3D11_MAPPED_SUBRESOURCE mi{}, mv{};
    if (FAILED(ctx->Map(g_ibStage, 0, D3D11_MAP_READ, 0, &mi)) || !mi.pData) {
        return false;
    }
    if (FAILED(ctx->Map(g_vbStage, 0, D3D11_MAP_READ, 0, &mv)) || !mv.pData) {
        ctx->Unmap(g_ibStage, 0);
        return false;
    }
    const uint8_t* vb = static_cast<const uint8_t*>(mv.pData);
    const uint16_t* i16 = static_cast<const uint16_t*>(mi.pData);
    const uint32_t* i32 = static_cast<const uint32_t*>(mi.pData);
    float lo = 1e30f, hi = -1e30f;
    bool ok = true;
    for (uint32_t i = 0; i < g_capCount; ++i) {
        const uint32_t idx = g_capI16 ? i16[i] : i32[i];
        const long long v = static_cast<long long>(idx) + g_capBase;
        if (v < 0) { ok = false; break; }
        const unsigned long long at =
            static_cast<unsigned long long>(v) * g_capStride;
        if (at + 8 > g_capVbBytes) { ok = false; break; }
        float x = 0.0f, y = 0.0f;
        memcpy(&x, vb + at, 4);
        memcpy(&y, vb + at + 4, 4);
        // Outside the space these panels are authored in, so this test's
        // assumption does not hold for this draw.
        if (!(x > -kSpaceHalf * 1.05f && x < kSpaceHalf * 1.05f &&
              y > -kSpaceHalf * 1.05f && y < kSpaceHalf * 1.05f)) {
            ok = false;
            break;
        }
        const float r = hypotf(x / kSpaceSize, y / kSpaceSize);
        if (r < lo) lo = r;
        if (r > hi) hi = r;
    }
    ctx->Unmap(g_vbStage, 0);
    ctx->Unmap(g_ibStage, 0);
    if (!ok) return false;
    *loOut = lo;
    *hiOut = hi;
    return g_capCount / 6 >= kRingMinQuads && lo >= kRingLo && hi <= kRingHi;
}

}  // namespace

void wakePulseConfigure(Config& cfg) {
    const bool was = g_off;
    const std::string m = cfg.getString("fix.wake_pulse", "off");
    if (m == "stock") {
        g_off = false;
    } else if (m == "off") {
        g_off = true;
    } else {
        g_off = false;
        Log::get().note("wake_pulse \"%s\" is not stock or off; running "
                        "stock.", m.c_str());
    }
    // Comma-separated, and empty means the measured set. A list because the
    // count is tessellation and every panel size has its own.
    const std::string spec = cfg.getString("advanced.wake_pulse_indices", "");
    uint32_t parsed[kMaxIndices];
    uint32_t n = 0;
    bool ok = true;
    for (const char* p = spec.c_str(); *p && ok;) {
        while (*p == ' ' || *p == ',' || *p == '	') ++p;
        if (!*p) break;
        char* end = nullptr;
        const unsigned long v = strtoul(p, &end, 10);
        if (end == p || v == 0 || v > 1000000 || n == kMaxIndices) { ok = false; break; }
        parsed[n++] = static_cast<uint32_t>(v);
        p = end;
    }
    if (!spec.empty() && (!ok || n == 0)) {
        Log::get().note("wake_pulse_indices \"%s\" is not up to %u whole "
                        "numbers separated by commas; the measured set is "
                        "used instead.", spec.c_str(), kMaxIndices);
    } else if (n) {
        for (uint32_t i = 0; i < n; ++i) g_indices[i] = parsed[i];
        g_indexCount = n;
    } else {
        g_indices[0] = kIndices;
        g_indices[1] = kIndicesAlt;
        g_indexCount = 2;
    }

    if (was != g_off) {
        Log::get().note(
            "wake pulse: %s. With a high wake selected, a marker under the "
            "speed readout flashes in time with the target indicator -- both "
            "are drawn into the same holo panel. off drops the one draw that "
            "paints it (%u known index counts of the GUI's textureless vector "
            "shader) "
            "on the frames the flash is on, and leaves the rest of the panel "
            "alone. That count is tessellation and depends on the panel's "
            "pixel size, so if the count below stays at zero this build "
            "will name the candidates it saw and one of them wants "
            "setting in advanced.wake_pulse_indices.",
            g_off ? "off" : "stock", g_indexCount);
    }
}

bool wakePulseWantsDraws() { return g_off; }

bool wakePulseSkips(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                    uint32_t targetW, uint32_t targetH, uint32_t startIndex,
                    int baseVertex) {
    if (!g_off) return false;
    if (targetW < kMinWidth || targetH == 0) return false;
    const float aspect = static_cast<float>(targetW) / static_cast<float>(targetH);
    if (aspect < kAspect - kAspectTol || aspect > kAspect + kAspectTol) {
        return false;
    }
    // The panel is here. Remember that separately from whether the flash
    // draw was, so a log can say which half failed.
    ++g_surfaceSeen;
    g_panelW = targetW;
    g_panelH = targetH;
    // Frames the panel was drawn in, not frames overall: menus and loading
    // draw no panel, and counting them would dilute a real pulse below the
    // threshold that is meant to find it.
    if (g_panelLastFrame != g_frame) {
        g_panelLastFrame = g_frame;
        ++g_panelFrames;
    }

    if (kind == kKind) {
        for (uint32_t i = 0; i < g_indexCount; ++i) {
            if (count != g_indices[i]) continue;
            ++g_dropped;
            return true;
        }
    }

    // Not ours. Record it while we still have nothing, so that if the count
    // is wrong for this panel the log can say what the alternatives were.
    // Stops costing anything the moment a real match happens or the table
    // fills.
    // Only textureless SHAPES are candidates: the vector shader binds no
    // sampler at all, which is what separates the marker from the textured
    // widgets sharing this panel, and six indices is a quad rather than a
    // drawn shape.
    const bool vectorShape =
        kind == kKind && count >= kMinShapeIndices &&
        !bindingGet(BindSlot::PsSrv0) && !bindingGet(BindSlot::PsSrv1) &&
        !bindingGet(BindSlot::PsSrv2) && !bindingGet(BindSlot::PsSrv3);
    if (!vectorShape || g_dropped != 0) return false;

    Seen* seen = nullptr;
    for (uint32_t i = 0; i < g_seenCount; ++i) {
        if (g_seen[i].indices != count) continue;
        seen = &g_seen[i];
        break;
    }
    if (!seen && g_seenCount < kSeenSlots) {
        seen = &g_seen[g_seenCount++];
        seen->indices = count;
    }
    if (seen && seen->lastFrame != g_frame) {
        seen->lastFrame = g_frame;
        ++seen->frames;
    }

    // The ring test, on one candidate at a time. Quads only (six indices to
    // one), never a count already ruled out, and never one drawn in so many
    // frames that it is furniture rather than a pulse -- a permanent circular
    // border would pass the geometry and must not be adopted on it alone.
    //
    // A count with no table slot left is still tested. The marker's count
    // arrives LATE -- it is only drawn during a wake -- so it is exactly the
    // one a full table would turn away, which is what the first cut of this
    // did.
    if (g_learned || g_capPending || !ctx) return false;
    if (count % 6 || (seen && seen->tried)) return false;
    const uint32_t frames = seen ? seen->frames : 1;
    const uint32_t pct = g_panelFrames ? frames * 100 / g_panelFrames : 100;
    if (pct > kMaxCandPct) return false;
    ringStartCapture(ctx, count, startIndex, baseVertex);
    return false;
}

void wakePulseReport() {
    if (!g_off) return;
    ++g_frame;

    // The ring test's readback, once the copies have had time to execute.
    // Whatever it decides, the count is not asked about again: a pass adopts
    // it, a failure rules it out, and either way the next candidate gets its
    // turn on a later frame.
    if (g_capPending && g_capCount) {
        if (g_capSettle) {
            --g_capSettle;
        } else {
            const uint32_t count = g_capCount;
            float lo = 0.0f, hi = 0.0f;
            const bool isRing =
                g_capCtx && ringDecide(g_capCtx, &lo, &hi);
            for (uint32_t i = 0; i < g_seenCount; ++i) {
                if (g_seen[i].indices == count) { g_seen[i].tried = true; break; }
            }
            ringDropCapture();
            if (isRing) {
                g_learned = true;
                g_indices[0] = count;
                g_indexCount = 1;
                Log::get().note(
                    "wake pulse: the marker on this %ux%u panel is the %u-index "
                    "draw -- all %u of its quads sit on one circle %.2f to "
                    "%.2f of the way out from the panel's centre, which is the "
                    "ring the flash paints and nothing else on this panel is. "
                    "Found by its shape rather than its size, so it did not "
                    "need this panel to have been measured before. Holding it "
                    "off from here.",
                    g_panelW, g_panelH, count, count / 6, lo, hi);
                return;
            }
            // Every test named, up to a handful. A shape test that decides
            // nothing is indistinguishable in a log from one that never ran,
            // and the radii say WHY a candidate was refused -- a rectangle
            // reaches 0.64 and more, a ring does not.
            if (g_testNotes < kTestNotes) {
                ++g_testNotes;
                Log::get().note(
                    "wake pulse: no known index count matched this %ux%u "
                    "panel, so its draws are tested by shape -- the marker is "
                    "a ring about the panel's centre. %u indices (%u quads) "
                    "is not one: its vertices run %.2f to %.2f out from the "
                    "centre, and a ring is a thin band near 0.47. Ruled out; "
                    "others follow. The ring has to be ON SCREEN to be "
                    "recognised, so the first high wake of a session may still "
                    "flash. Said at most %u times.",
                    g_panelW, g_panelH, count, count / 6, lo, hi, kTestNotes);
            }
        }
    }

    // The diagnosis a field report needs, said once. "Still flashing" has
    // two causes and they want different answers: the panel was never
    // recognised, or it was and the flash draw is not the size named. This
    // says which, with the panel's real size to re-derive from.
    if (!g_saidNoMatch && g_dropped == 0 && g_surfaceSeen > 20000) {
        g_saidNoMatch = true;
        // The candidates, named. A pulse is a draw present in a MINORITY of
        // frames; everything else on this panel is drawn in all of them. Two
        // per cent to sixty is wide enough for a slow blink and a fast one
        // and still excludes the constant furniture.
        char list[220] = "";
        int at = 0, found = 0;
        for (uint32_t i = 0; i < g_seenCount && at < 180; ++i) {
            const uint32_t pct =
                g_panelFrames ? g_seen[i].frames * 100 / g_panelFrames : 0;
            if (pct < 2 || pct > 60) continue;
            const int n = _snprintf_s(list + at, sizeof(list) - at, _TRUNCATE,
                                      "%s%u (in %u%% of frames)",
                                      found ? ", " : "", g_seen[i].indices, pct);
            if (n <= 0) break;
            at += n;
            ++found;
        }
        Log::get().note(
            "wake pulse: the panel surface IS being found (%ux%u, seen %llu "
            "times) but no %c draw of any known index count has landed in it, "
            "so nothing "
            "is being held off. The count is TESSELLATION -- the GUI "
            "subdivides its curves by pixel size, so a smaller panel needs "
            "fewer -- and %u is right for a 2440x1996 panel. %s%s Set "
            "advanced.wake_pulse_indices to the one that stops the flashing. "
            "Said once.",
            g_panelW, g_panelH, static_cast<unsigned long long>(g_surfaceSeen),
            kKind, kIndices,
            found ? "Drawn in only some frames here, so a pulse: " : "",
            found ? list
                  : "Nothing on this panel was drawn intermittently while "
                    "this watched, so the marker was probably never up -- "
                    "select a high wake and look again.");
        return;
    }

    // Only when it moved, and only a handful of times: a suppression that
    // says nothing is indistinguishable from one that never matched, and a
    // line per pulse would be the log.
    if (g_dropped == g_lastReported || g_reports >= 6) return;
    ++g_reports;
    g_lastReported = g_dropped;
    Log::get().note("wake pulse: %llu flash draw(s) dropped so far. The "
                    "marker under the speed readout is held off; nothing "
                    "else on that panel is touched. Said at most 6 times.",
                    static_cast<unsigned long long>(g_dropped));
}

void wakePulseShutdown() {
    ringDropCapture();
    if (g_ibStage) { g_ibStage->Release(); g_ibStage = nullptr; }
    if (g_vbStage) { g_vbStage->Release(); g_vbStage = nullptr; }
    g_capVbBytes = 0;
}

}  // namespace edvr

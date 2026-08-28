#include "quad_probe.h"

#include <windows.h>

#include <d3d11.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/timing.h"  // the wall-clock gate

namespace edvr {
namespace {

// Six indices to a quad at topology 4, which the census reports for this
// family. Anything not a multiple of six is not this shape and is refused --
// for the INDEXED kinds. See kStripQuadVerts.
constexpr uint32_t kIndicesPerQuad = 6;

// A four-vertex triangle strip is also a quad, and the non-indexed kinds
// draw them: no index buffer, vertices at startVertex + 0..3.
//
// Refused until 2026-08-28, when the intro flight needed exactly this. The
// movie's own composite turned out to be a full-screen quad with nothing in
// it to move (docs/intro-video.md), which puts the question on the draw that
// FILLS its 1920x1080 source -- an N of four vertices. A probe that could
// not be aimed at that shape could not ask where the picture inside the
// surface actually sits.
constexpr uint32_t kStripQuadVerts = 4;

bool kindIsIndexed(char k) { return k == 'I' || k == 'X'; }

// The vertex buffer these draws share is 4 MB and rewritten every frame, so
// the copy has to be taken AT the matched draws, once, in their own frame.
constexpr uint32_t kMaxVertexBytes = 4u << 20;

// Matching draws recorded in the capture frame. The hunt that built this
// found two; a frame with sixteen same-signature draws is a different
// mystery, and the log says how many were left uncopied.
constexpr uint32_t kMaxOccurrences = 16;

// Index bytes the capture will hold across all occurrences.
constexpr uint32_t kIbStageBytes = 64u << 10;

// Frames to let the copies execute before mapping. panel_quad's number: long
// enough that the map never stalls the render thread, short enough that a
// capture is retired within a blink.
constexpr uint32_t kSettleFrames = 4;

// Hex bytes of a quad's first vertex printed after its rectangle: everything
// past the float2 position, capped to keep a log line a log line.
constexpr uint32_t kTailBytesMax = 32;

FaultBudget g_budget("quadProbe", 4);

bool     g_armed = false;
uint32_t g_wantW = 0, g_wantH = 0;
char     g_wantKind = 0;
uint32_t g_wantN = 0;
// Matching frames to let pass before capturing. The first frame containing
// a match is usually a fade-in frame: the first flight of this armed at
// launch and caught the backdrop alone, one occurrence of the three the
// census had already counted in steady state.
uint32_t g_wantSkip = 0;
uint32_t g_skipLeft = 0;
uint32_t g_lastSkipFrame = 0;

// advanced.quad_probe_at_ms: no capture before this many milliseconds after
// the probe's first frame. 0 is off, and off is the shipped state.
//
// WHY A CLOCK AS WELL AS A FRAME COUNT. The skip above counts MATCHING
// FRAMES, which is the right unit for "let the fade-in finish" and the wrong
// one for "wait until the intro movie is over". Measured on the field rig:
// the movie plays at 178 fps and the menu behind it runs at 13, so a skip
// large enough to clear a twenty-second movie leaves the player sitting at
// the menu for minutes while the last few hundred frames trickle past. A
// flight was spent on exactly that -- 1200 frames was meant to land after
// the movie and landed nineteen seconds early, because the estimate came
// from per-frame deltas that GetTickCount64's 15.6 ms resolution had already
// destroyed.
//
// common/timing.h states the rule this key exists to obey: if it answers
// "how long", it is milliseconds. The two compose -- both have to be
// satisfied -- so "two seconds of steady frames, but not before the movie
// ends" is expressible, and neither alone could say it.
uint32_t g_wantAtMs = 0;
uint64_t g_firstTickMs = 0;
bool     g_taken = false;          // one capture per session; re-arm by
                                   // setting the spec off and on again

// Distinct vertex buffers one capture may hold.
//
// It used to hold ONE, copied at the first occurrence, and every later
// occurrence was decoded out of it whatever buffer it had actually bound.
// That was right for the widget panels this began with -- they all share a
// single 4 MB pool -- and silently wrong the first time it met draws that do
// not. The intro flight is the case: the two eyes' movie composites carry
// one 80-byte buffer EACH, so occurrence 1 was decoded from occurrence 0's
// bytes and reported geometry identical to it by construction. An
// instrument agreeing with itself reads exactly like a measurement.
//
// Slots are keyed by the bound buffer, so the shared-pool case still costs
// one copy however many occurrences index it.
constexpr uint32_t kMaxVbSlots = 8;

struct VbSlot {
    void*         key = nullptr;    // the ID3D11Buffer the draw had bound
    ID3D11Buffer* stage = nullptr;
    uint32_t      stride = 0;
    uint32_t      bytes = 0;
};

struct Occ {
    uint32_t ibOffset = 0;         // bytes into the index staging buffer
    int      baseVertex = 0;       // non-indexed kinds: the start vertex
    uint32_t startIndex = 0;
    uint32_t instances = 0;
    int      vbSlot = -1;          // -1: its buffer did not fit the capture
    DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
    // The rasterizer state live at the draw. Flight 2 proved the widget
    // system sizes these panels with the VIEWPORT -- identical full-space
    // vertices, different screen rects -- so the rect an occurrence renders
    // into is here, not in the buffers.
    D3D11_VIEWPORT vp = {};
    bool     vpKnown = false;
    D3D11_RECT sc = {};
    bool     scKnown = false;
    bool     scEnabled = false;
};

ID3D11Buffer* g_ibStage = nullptr;
VbSlot        g_vb[kMaxVbSlots];
uint32_t      g_vbCount = 0;
Occ           g_occ[kMaxOccurrences];
uint32_t      g_occCount = 0;
uint32_t      g_occDropped = 0;
uint32_t      g_ibFill = 0;
bool          g_windowOpen = false;   // the capture frame is still running
uint32_t      g_windowFrame = 0;
uint32_t      g_pendingFrame = 0;     // 0 = nothing settling
uint32_t      g_frame = 0;

void failOnce(const char* why) {
    static bool noted = false;
    if (noted) return;
    noted = true;
    Log::get().note("quad probe: %s. No capture this session.", why);
}

void dropCapture() {
    if (g_ibStage) { g_ibStage->Release(); g_ibStage = nullptr; }
    for (VbSlot& s : g_vb) {
        if (s.stage) s.stage->Release();
        s = VbSlot();
    }
    g_vbCount = 0;
    g_occCount = 0;
    g_occDropped = 0;
    g_ibFill = 0;
    g_windowOpen = false;
    g_pendingFrame = 0;
}

}  // namespace

void quadProbeConfigure(Config& cfg) {
    const std::string spec = cfg.getString("advanced.quad_probe", "");

    uint32_t w = 0, h = 0, n = 0, skip = 0;
    char kind = 0;
    if (!spec.empty()) {
        const char* p = spec.c_str();
        char* end = nullptr;
        const unsigned long pw = strtoul(p, &end, 10);
        unsigned long ph = 0, pn = 0, pskip = 0;
        bool ok = (end != p) && (*end == 'x' || *end == 'X');
        if (ok) { const char* q = end + 1; ph = strtoul(q, &end, 10); ok = end != q; }
        if (ok) ok = (*end == ':') && strchr("DINX", end[1]) && end[2] == ':';
        if (ok) {
            kind = end[1];
            const char* q = end + 3;
            pn = strtoul(q, &end, 10);
            ok = end != q;
        }
        // The optional fourth field: matching frames to let pass first, so
        // the capture describes steady state rather than the first fade-in
        // frame the widget appears in.
        if (ok && *end == ':') {
            const char* q = end + 1;
            pskip = strtoul(q, &end, 10);
            ok = end != q;
        }
        while (*end == ' ' || *end == '\t') ++end;
        // COUNT means indices for the indexed kinds (six to a quad) and
        // VERTICES for the non-indexed ones, where the only shape this
        // decodes is the four-vertex strip quad.
        const bool countOk =
            ok && pn != 0 &&
            (kindIsIndexed(kind) ? (pn % kIndicesPerQuad == 0)
                                 : (pn == kStripQuadVerts));
        if (!ok || *end || pw == 0 || ph == 0 || !countOk) {
            Log::get().note("quad probe: \"%s\" is not "
                            "WIDTHxHEIGHT:KIND:COUNT[:SKIPFRAMES] with COUNT "
                            "a multiple of six for I and X, or exactly %u for "
                            "D and N; refused rather than half-applied.",
                            spec.c_str(), kStripQuadVerts);
        } else {
            w = static_cast<uint32_t>(pw);
            h = static_cast<uint32_t>(ph);
            n = static_cast<uint32_t>(pn);
            skip = static_cast<uint32_t>(pskip);
        }
    }
    const uint32_t atMs = static_cast<uint32_t>(cfg.getIntInRange(
        "advanced.quad_probe_at_ms", 0, 0, 3600000));

    const bool armed = w != 0;
    // A re-armed probe is a fresh request: turning it off and on again is how
    // a second capture is asked for without a relaunch. The clock gate counts
    // as part of the request, so moving it alone also asks again.
    if (armed && (w != g_wantW || h != g_wantH || kind != g_wantKind ||
                  n != g_wantN || skip != g_wantSkip || atMs != g_wantAtMs)) {
        g_taken = false;
        g_skipLeft = skip;
        g_lastSkipFrame = 0;
    }
    g_wantW = w; g_wantH = h; g_wantKind = kind; g_wantN = n;
    g_wantSkip = skip;
    g_wantAtMs = atMs;
    if (armed && !g_armed) {
        char when[128] = "";
        if (atMs) {
            snprintf(when, sizeof(when),
                     " and not before %u ms into the session", atMs);
        }
        Log::get().note("quad probe ARMED on %c:%u draws into a %ux%u target: "
                        "after letting %u matching frame(s) pass%s, the first "
                        "frame containing one has EVERY such draw copied, and "
                        "each occurrence's quads are logged with their extent, "
                        "uv span and the bytes past the position. Nothing is "
                        "changed. Set the spec off and on again for another "
                        "capture.",
                        kind, n, w, h, skip, when);
    }
    g_armed = armed;
}

bool quadProbeWants() { return g_armed && !g_taken; }

bool quadProbeOnDraw(ID3D11DeviceContext* ctx, uint32_t targetW,
                     uint32_t targetH, char kind, uint32_t count,
                     uint32_t instances, uint32_t startIndex, int baseVertex) {
    if (!g_armed || g_taken || g_pendingFrame || !ctx) return false;
    if (targetW != g_wantW || targetH != g_wantH) return false;
    if (kind != g_wantKind || count != g_wantN) return false;

    // The clock gate, BEFORE the frame skip: while it holds the probe is
    // simply not looking, so matching frames must not be spent against the
    // skip either. The two are meant to compose as "this long in, and then
    // that many steady frames", not to race each other.
    if (g_wantAtMs && !elapsedMs(g_firstTickMs, g_wantAtMs)) return false;

    // Skipped frames pass whole: one decrement per frame that contains a
    // match, however many matches it holds -- INCLUDING the frame that
    // spends the last skip. Flights 2 and 3 opened the window mid-frame on
    // that frame's later matches and never showed the first occurrence.
    if (g_skipLeft) {
        if (g_frame != g_lastSkipFrame) {
            --g_skipLeft;
            g_lastSkipFrame = g_frame;
        }
        return false;
    }
    if (g_lastSkipFrame && g_frame == g_lastSkipFrame) return false;

    // The capture window is the FIRST frame a match lands in. A match in a
    // later frame indexes a rewritten buffer and cannot join this capture.
    if (g_windowOpen && g_windowFrame != g_frame) return false;

    if (g_occCount >= kMaxOccurrences) {
        ++g_occDropped;
        return false;
    }

    const bool indexed = kindIsIndexed(kind);

    bool started = false;
    guardedBudget(g_budget, [&] {
        ID3D11Buffer* ib = nullptr;
        DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN;
        UINT ibOff = 0;
        // Asked for only when it can matter: a non-indexed draw may legally
        // have a stale index buffer bound, and reading one would put a
        // meaningless format on the log line.
        if (indexed) ctx->IAGetIndexBuffer(&ib, &ibFmt, &ibOff);
        ID3D11Buffer* vb = nullptr;
        UINT stride = 0, vbOff = 0;
        ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &vbOff);
        if ((indexed && !ib) || !vb || stride == 0) {
            if (ib) ib->Release();
            if (vb) vb->Release();
            failOnce(indexed ? "the draw had no index or vertex buffer bound"
                             : "the draw had no vertex buffer bound");
            return;
        }
        const UINT idxSize = (ibFmt == DXGI_FORMAT_R16_UINT) ? 2u : 4u;
        const UINT need = indexed ? count * idxSize : 0u;

        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            bool ok = true;
            if (indexed && !g_ibStage) {
                D3D11_BUFFER_DESC sd{};
                sd.Usage = D3D11_USAGE_STAGING;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                sd.ByteWidth = kIbStageBytes;
                ok = SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &g_ibStage));
            }
            // This draw's vertex buffer, by identity. A second occurrence
            // that bound the SAME buffer joins the copy already queued for
            // it; one that bound a different buffer gets its own slot. See
            // kMaxVbSlots for what the single shared copy cost.
            int slot = -1;
            for (uint32_t i = 0; ok && i < g_vbCount; ++i) {
                if (g_vb[i].key == vb) { slot = static_cast<int>(i); break; }
            }
            if (ok && slot < 0 && g_vbCount < kMaxVbSlots) {
                VbSlot& s = g_vb[g_vbCount];
                D3D11_BUFFER_DESC vd{};
                vb->GetDesc(&vd);
                D3D11_BUFFER_DESC sd{};
                sd.Usage = D3D11_USAGE_STAGING;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                sd.ByteWidth = vd.ByteWidth > kMaxVertexBytes ? kMaxVertexBytes
                                                              : vd.ByteWidth;
                ok = SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &s.stage));
                if (ok) {
                    // The whole buffer, once per buffer. Every occurrence
                    // that indexes it does so this frame, and the game
                    // appends with no-overwrite maps, so a copy queued at the
                    // first of them sees the frame's writes by the time the
                    // GPU executes it.
                    D3D11_BOX all{};
                    all.right = sd.ByteWidth;
                    all.bottom = 1; all.back = 1;
                    ctx->CopySubresourceRegion(s.stage, 0, 0, 0, 0, vb, 0, &all);
                    s.key = vb;
                    s.bytes = sd.ByteWidth;
                    s.stride = stride;
                    slot = static_cast<int>(g_vbCount++);
                }
            }
            if (ok && (!indexed || g_ibStage) &&
                g_ibFill + need <= kIbStageBytes) {
                if (indexed) {
                    D3D11_BOX box{};
                    box.left = ibOff + startIndex * idxSize;
                    box.right = box.left + need;
                    box.bottom = 1; box.back = 1;
                    ctx->CopySubresourceRegion(g_ibStage, 0, g_ibFill, 0, 0,
                                               ib, 0, &box);
                }
                Occ& o = g_occ[g_occCount];
                o = Occ{};
                o.ibOffset = g_ibFill;
                o.baseVertex = baseVertex;
                o.startIndex = startIndex;
                o.instances = instances;
                o.vbSlot = slot;
                o.ibFormat = ibFmt;
                UINT nv = 1;
                ctx->RSGetViewports(&nv, &o.vp);
                o.vpKnown = nv >= 1;
                UINT ns = 1;
                ctx->RSGetScissorRects(&ns, &o.sc);
                o.scKnown = ns >= 1;
                ID3D11RasterizerState* rs = nullptr;
                ctx->RSGetState(&rs);
                if (rs) {
                    D3D11_RASTERIZER_DESC rd;
                    rs->GetDesc(&rd);
                    o.scEnabled = rd.ScissorEnable != FALSE;
                    rs->Release();
                }
                ++g_occCount;
                g_ibFill += need;
                g_windowOpen = true;
                g_windowFrame = g_frame;
                started = true;
            } else if (ok) {
                ++g_occDropped;
            } else {
                failOnce("the staging buffers could not be created");
                dropCapture();
                g_taken = true;   // do not retry into the same failure
            }
            dev->Release();
        }
        if (ib) ib->Release();   // null for the non-indexed kinds
        vb->Release();
    });
    return started;
}

void quadProbeTick(ID3D11DeviceContext* ctx) {
    ++g_frame;
    // The clock gate's zero. Stamped at the first frame this module sees, so
    // "30 s in" means the same thing however long the loader took to reach
    // the first frame -- the same origin census_at_ms uses.
    if (!g_firstTickMs) g_firstTickMs = stampMs();
    // The capture frame ended: close the window and let the copies settle.
    if (g_windowOpen && g_frame > g_windowFrame) {
        g_windowOpen = false;
        g_taken = true;   // one window per session, whatever it yields
        g_pendingFrame = g_occCount ? g_frame + kSettleFrames : 0;
        if (!g_pendingFrame) dropCapture();
    }
    if (!g_pendingFrame || g_frame < g_pendingFrame || !ctx) return;
    g_pendingFrame = 0;

    const bool indexed = kindIsIndexed(g_wantKind);

    guardedBudget(g_budget, [&] {
        D3D11_MAPPED_SUBRESOURCE mi{};
        const uint8_t* ibBase = nullptr;
        if (indexed) {
            if (FAILED(ctx->Map(g_ibStage, 0, D3D11_MAP_READ, 0, &mi)) ||
                !mi.pData) {
                failOnce("the index copy could not be mapped");
                return;
            }
            ibBase = static_cast<const uint8_t*>(mi.pData);
        }
        // Every buffer this capture holds, mapped for the decode. A slot
        // that will not map is left null and the occurrences using it say
        // so rather than being decoded out of somebody else's bytes.
        const uint8_t* vbData[kMaxVbSlots] = {nullptr};
        uint32_t mapped = 0;
        for (uint32_t i = 0; i < g_vbCount; ++i) {
            D3D11_MAPPED_SUBRESOURCE mv{};
            if (g_vb[i].stage &&
                SUCCEEDED(ctx->Map(g_vb[i].stage, 0, D3D11_MAP_READ, 0, &mv)) &&
                mv.pData) {
                vbData[i] = static_cast<const uint8_t*>(mv.pData);
                ++mapped;
            }
        }
        if (!mapped) {
            if (ibBase) ctx->Unmap(g_ibStage, 0);
            failOnce("no vertex copy could be mapped");
            return;
        }
        const uint32_t quads = indexed ? g_wantN / kIndicesPerQuad : 1u;
        const uint32_t vertsPerQuad = indexed ? kIndicesPerQuad : kStripQuadVerts;
        Log::get().note(
            "quad probe: %u occurrence(s) of %c:%u into %ux%u in one frame%s, "
            "over %u distinct vertex buffer(s). Rectangles are the raw float2 "
            "at offset 0; the hex after each is the first vertex's remaining "
            "bytes -- colour, uv, whatever the layout holds. Same-signature "
            "occurrences that differ in extent are DIFFERENT rectangles "
            "sharing one widget; occurrences on different buffers were each "
            "read from their own.",
            g_occCount, g_wantKind, g_wantN, g_wantW, g_wantH,
            g_occDropped ? " (more matched than fit; the excess was not "
                           "copied)" : "",
            g_vbCount);
        for (uint32_t oi = 0; oi < g_occCount; ++oi) {
            const Occ& o = g_occ[oi];
            const bool i16 = o.ibFormat == DXGI_FORMAT_R16_UINT;
            char vps[64] = "viewport ?";
            if (o.vpKnown) {
                snprintf(vps, sizeof(vps), "viewport %.0f,%.0f %.0fx%.0f",
                         o.vp.TopLeftX, o.vp.TopLeftY, o.vp.Width,
                         o.vp.Height);
            }
            char scs[80] = "";
            if (o.scKnown) {
                snprintf(scs, sizeof(scs), ", scissor %ld,%ld-%ld,%ld %s",
                         o.sc.left, o.sc.top, o.sc.right, o.sc.bottom,
                         o.scEnabled ? "on" : "off");
            }
            const uint8_t* vb =
                (o.vbSlot >= 0) ? vbData[o.vbSlot] : nullptr;
            const uint32_t stride = (o.vbSlot >= 0) ? g_vb[o.vbSlot].stride : 0;
            const uint32_t vbBytes = (o.vbSlot >= 0) ? g_vb[o.vbSlot].bytes : 0;
            Log::get().note("  occurrence %u: %s %d, startIndex %u, "
                            "instances %u, index format %s, stride %u, "
                            "buffer %d, %s%s",
                            oi, indexed ? "baseVertex" : "startVertex",
                            o.baseVertex, o.startIndex, o.instances,
                            indexed ? (i16 ? "R16" : "R32") : "none",
                            stride, o.vbSlot, vps, scs);
            if (!vb || !stride) {
                Log::get().note("    its vertex buffer was not captured -- "
                                "more distinct buffers than the capture "
                                "holds, or the copy would not map.");
                continue;
            }
            for (uint32_t q = 0; q < quads; ++q) {
                float lo[2] = {1e30f, 1e30f};
                float hi[2] = {-1e30f, -1e30f};
                // The float2 AFTER the position, ranged the same way.
                //
                // On every layout this has met it is the texture coordinate,
                // and its RANGE is the measurement the first-vertex hex dump
                // could never give. The intro movie is the case that forced
                // it: both its stages draw full-screen quads -- position
                // -1..1, viewport the whole target -- and the picture is
                // still small and surrounded by black, which only a uv span
                // reaching outside 0..1 explains. One corner's bytes cannot
                // say that; two corners can.
                float uvLo[2] = {1e30f, 1e30f};
                float uvHi[2] = {-1e30f, -1e30f};
                const bool haveUv = stride >= 16;
                int64_t firstOff = -1;
                bool bad = false;
                for (uint32_t k = 0; k < vertsPerQuad; ++k) {
                    int64_t v;
                    if (indexed) {
                        const uint32_t at = q * kIndicesPerQuad + k;
                        const uint8_t* ip =
                            ibBase + o.ibOffset + at * (i16 ? 2 : 4);
                        const uint32_t vi =
                            i16 ? *reinterpret_cast<const uint16_t*>(ip)
                                : *reinterpret_cast<const uint32_t*>(ip);
                        v = static_cast<int64_t>(vi) + o.baseVertex;
                    } else {
                        // No index buffer: the strip's vertices are
                        // consecutive from the draw's start vertex, which
                        // the caller passes in baseVertex -- it plays the
                        // same role there that it does for an indexed draw.
                        v = static_cast<int64_t>(o.baseVertex) + k;
                    }
                    const int64_t off = v * stride;
                    if (v < 0 ||
                        off + stride > static_cast<int64_t>(vbBytes)) {
                        bad = true;
                        break;
                    }
                    if (firstOff < 0) firstOff = off;
                    float p[2];
                    memcpy(p, vb + off, sizeof(p));
                    for (int c = 0; c < 2; ++c) {
                        if (p[c] < lo[c]) lo[c] = p[c];
                        if (p[c] > hi[c]) hi[c] = p[c];
                    }
                    if (haveUv) {
                        float t[2];
                        memcpy(t, vb + off + 8, sizeof(t));
                        for (int c = 0; c < 2; ++c) {
                            if (t[c] < uvLo[c]) uvLo[c] = t[c];
                            if (t[c] > uvHi[c]) uvHi[c] = t[c];
                        }
                    }
                }
                if (bad) {
                    Log::get().note("    quad %u: a vertex landed outside the "
                                    "copied range -- %s %d, stride %u, %u "
                                    "bytes copied.",
                                    q, indexed ? "baseVertex" : "startVertex",
                                    o.baseVertex, stride, vbBytes);
                    continue;
                }
                char tail[kTailBytesMax * 2 + 1] = "";
                if (firstOff >= 0 && stride > 8) {
                    uint32_t nTail = stride - 8;
                    if (nTail > kTailBytesMax) nTail = kTailBytesMax;
                    for (uint32_t t = 0; t < nTail; ++t) {
                        snprintf(tail + t * 2, 3, "%02X",
                                 vb[firstOff + 8 + t]);
                    }
                }
                char uvs[96] = "";
                if (haveUv) {
                    snprintf(uvs, sizeof(uvs),
                             "  uv %.4f..%.4f, %.4f..%.4f (span %.4f x %.4f)",
                             uvLo[0], uvHi[0], uvLo[1], uvHi[1],
                             uvHi[0] - uvLo[0], uvHi[1] - uvLo[1]);
                }
                Log::get().note("    quad %u: x %.3f..%.3f  y %.3f..%.3f  "
                                "(w %.3f h %.3f)%s  +%s",
                                q, lo[0], hi[0], lo[1], hi[1],
                                hi[0] - lo[0], hi[1] - lo[1], uvs, tail);
            }
        }
        for (uint32_t i = 0; i < g_vbCount; ++i) {
            if (vbData[i]) ctx->Unmap(g_vb[i].stage, 0);
        }
        if (ibBase) ctx->Unmap(g_ibStage, 0);
    });
    dropCapture();
}

void quadProbeShutdown() {
    dropCapture();
}

}  // namespace edvr

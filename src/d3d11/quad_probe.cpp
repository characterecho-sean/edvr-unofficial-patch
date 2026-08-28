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

namespace edvr {
namespace {

// Six indices to a quad at topology 4, which the census reports for this
// family. Anything not a multiple of six is not this shape and is refused.
constexpr uint32_t kIndicesPerQuad = 6;

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
bool     g_taken = false;          // one capture per session; re-arm by
                                   // setting the spec off and on again

struct Occ {
    uint32_t ibOffset = 0;         // bytes into the index staging buffer
    int      baseVertex = 0;
    uint32_t startIndex = 0;
    uint32_t instances = 0;
    DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
};

ID3D11Buffer* g_ibStage = nullptr;
ID3D11Buffer* g_vbStage = nullptr;
Occ           g_occ[kMaxOccurrences];
uint32_t      g_occCount = 0;
uint32_t      g_occDropped = 0;
uint32_t      g_ibFill = 0;
bool          g_windowOpen = false;   // the capture frame is still running
uint32_t      g_windowFrame = 0;
uint32_t      g_pendingFrame = 0;     // 0 = nothing settling
uint32_t      g_frame = 0;
uint32_t      g_capStride = 0;
uint32_t      g_capVertexBytes = 0;

void failOnce(const char* why) {
    static bool noted = false;
    if (noted) return;
    noted = true;
    Log::get().note("quad probe: %s. No capture this session.", why);
}

void dropCapture() {
    if (g_ibStage) { g_ibStage->Release(); g_ibStage = nullptr; }
    if (g_vbStage) { g_vbStage->Release(); g_vbStage = nullptr; }
    g_occCount = 0;
    g_occDropped = 0;
    g_ibFill = 0;
    g_windowOpen = false;
    g_pendingFrame = 0;
}

}  // namespace

void quadProbeConfigure(Config& cfg) {
    const std::string spec = cfg.getString("advanced.quad_probe", "");

    uint32_t w = 0, h = 0, n = 0;
    char kind = 0;
    if (!spec.empty()) {
        const char* p = spec.c_str();
        char* end = nullptr;
        const unsigned long pw = strtoul(p, &end, 10);
        unsigned long ph = 0, pn = 0;
        bool ok = (end != p) && (*end == 'x' || *end == 'X');
        if (ok) { const char* q = end + 1; ph = strtoul(q, &end, 10); ok = end != q; }
        if (ok) ok = (*end == ':') && strchr("DINX", end[1]) && end[2] == ':';
        if (ok) {
            kind = end[1];
            const char* q = end + 3;
            pn = strtoul(q, &end, 10);
            ok = end != q;
        }
        while (*end == ' ' || *end == '\t') ++end;
        if (!ok || *end || pw == 0 || ph == 0 || pn == 0 ||
            pn % kIndicesPerQuad != 0) {
            Log::get().note("quad probe: \"%s\" is not WIDTHxHEIGHT:KIND:COUNT "
                            "with COUNT a multiple of six; refused rather than "
                            "half-applied.", spec.c_str());
        } else {
            w = static_cast<uint32_t>(pw);
            h = static_cast<uint32_t>(ph);
            n = static_cast<uint32_t>(pn);
        }
    }
    const bool armed = w != 0;
    // A re-armed probe is a fresh request: turning it off and on again is how
    // a second capture is asked for without a relaunch.
    if (armed && (w != g_wantW || h != g_wantH || kind != g_wantKind ||
                  n != g_wantN)) {
        g_taken = false;
    }
    g_wantW = w; g_wantH = h; g_wantKind = kind; g_wantN = n;
    if (armed && !g_armed) {
        Log::get().note("quad probe ARMED on %c:%u draws into a %ux%u target: "
                        "the first frame containing one has EVERY such draw "
                        "copied, and each occurrence's quads are logged with "
                        "the bytes past the position. Nothing is changed. Set "
                        "the spec off and on again for another capture.",
                        kind, n, w, h);
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

    // The capture window is the FIRST frame a match lands in. A match in a
    // later frame indexes a rewritten buffer and cannot join this capture.
    if (g_windowOpen && g_windowFrame != g_frame) return false;

    if (g_occCount >= kMaxOccurrences) {
        ++g_occDropped;
        return false;
    }

    bool started = false;
    guardedBudget(g_budget, [&] {
        ID3D11Buffer* ib = nullptr;
        DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN;
        UINT ibOff = 0;
        ctx->IAGetIndexBuffer(&ib, &ibFmt, &ibOff);
        ID3D11Buffer* vb = nullptr;
        UINT stride = 0, vbOff = 0;
        ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &vbOff);
        if (!ib || !vb || stride == 0) {
            if (ib) ib->Release();
            if (vb) vb->Release();
            failOnce("the draw had no index or vertex buffer bound");
            return;
        }
        const UINT idxSize = (ibFmt == DXGI_FORMAT_R16_UINT) ? 2u : 4u;
        const UINT need = count * idxSize;

        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            bool ok = true;
            if (!g_ibStage) {
                D3D11_BUFFER_DESC sd{};
                sd.Usage = D3D11_USAGE_STAGING;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                sd.ByteWidth = kIbStageBytes;
                ok = SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &g_ibStage));
            }
            if (ok && !g_vbStage) {
                D3D11_BUFFER_DESC vd{};
                vb->GetDesc(&vd);
                D3D11_BUFFER_DESC sd{};
                sd.Usage = D3D11_USAGE_STAGING;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                sd.ByteWidth = vd.ByteWidth > kMaxVertexBytes ? kMaxVertexBytes
                                                              : vd.ByteWidth;
                ok = SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &g_vbStage));
                if (ok) {
                    // The whole buffer, once. Every occurrence this frame
                    // indexes into it, and the game appends with no-overwrite
                    // maps, so a copy queued at the first occurrence sees the
                    // frame's writes by the time the GPU executes it.
                    D3D11_BOX all{};
                    all.right = sd.ByteWidth;
                    all.bottom = 1; all.back = 1;
                    ctx->CopySubresourceRegion(g_vbStage, 0, 0, 0, 0,
                                               vb, 0, &all);
                    g_capVertexBytes = sd.ByteWidth;
                    g_capStride = stride;
                }
            }
            if (ok && g_ibStage && g_vbStage && g_ibFill + need <= kIbStageBytes) {
                D3D11_BOX box{};
                box.left = ibOff + startIndex * idxSize;
                box.right = box.left + need;
                box.bottom = 1; box.back = 1;
                ctx->CopySubresourceRegion(g_ibStage, 0, g_ibFill, 0, 0,
                                           ib, 0, &box);
                Occ& o = g_occ[g_occCount];
                o.ibOffset = g_ibFill;
                o.baseVertex = baseVertex;
                o.startIndex = startIndex;
                o.instances = instances;
                o.ibFormat = ibFmt;
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
        ib->Release();
        vb->Release();
    });
    return started;
}

void quadProbeTick(ID3D11DeviceContext* ctx) {
    ++g_frame;
    // The capture frame ended: close the window and let the copies settle.
    if (g_windowOpen && g_frame > g_windowFrame) {
        g_windowOpen = false;
        g_taken = true;   // one window per session, whatever it yields
        g_pendingFrame = g_occCount ? g_frame + kSettleFrames : 0;
        if (!g_pendingFrame) dropCapture();
    }
    if (!g_pendingFrame || g_frame < g_pendingFrame || !ctx) return;
    g_pendingFrame = 0;

    guardedBudget(g_budget, [&] {
        D3D11_MAPPED_SUBRESOURCE mi{}, mv{};
        if (FAILED(ctx->Map(g_ibStage, 0, D3D11_MAP_READ, 0, &mi)) || !mi.pData) {
            failOnce("the index copy could not be mapped");
            return;
        }
        if (FAILED(ctx->Map(g_vbStage, 0, D3D11_MAP_READ, 0, &mv)) || !mv.pData) {
            ctx->Unmap(g_ibStage, 0);
            failOnce("the vertex copy could not be mapped");
            return;
        }
        const uint8_t* ibBase = static_cast<const uint8_t*>(mi.pData);
        const uint8_t* vb = static_cast<const uint8_t*>(mv.pData);
        const uint32_t quads = g_wantN / kIndicesPerQuad;
        Log::get().note(
            "quad probe: %u occurrence(s) of %c:%u into %ux%u in one frame%s, "
            "stride %u. Rectangles are the raw float2 at offset 0; the hex "
            "after each is the first vertex's remaining bytes -- colour, uv, "
            "whatever the layout holds. Same-signature occurrences that "
            "differ in extent are DIFFERENT rectangles sharing one widget.",
            g_occCount, g_wantKind, g_wantN, g_wantW, g_wantH,
            g_occDropped ? " (more matched than fit; the excess was not "
                           "copied)" : "",
            g_capStride);
        for (uint32_t oi = 0; oi < g_occCount; ++oi) {
            const Occ& o = g_occ[oi];
            const bool i16 = o.ibFormat == DXGI_FORMAT_R16_UINT;
            Log::get().note("  occurrence %u: baseVertex %d, startIndex %u, "
                            "instances %u, index format %s",
                            oi, o.baseVertex, o.startIndex, o.instances,
                            i16 ? "R16" : "R32");
            for (uint32_t q = 0; q < quads; ++q) {
                float lo[2] = {1e30f, 1e30f};
                float hi[2] = {-1e30f, -1e30f};
                int64_t firstOff = -1;
                bool bad = false;
                for (uint32_t k = 0; k < kIndicesPerQuad; ++k) {
                    const uint32_t at = q * kIndicesPerQuad + k;
                    const uint8_t* ip = ibBase + o.ibOffset + at * (i16 ? 2 : 4);
                    uint32_t vi = i16 ? *reinterpret_cast<const uint16_t*>(ip)
                                      : *reinterpret_cast<const uint32_t*>(ip);
                    const int64_t v = static_cast<int64_t>(vi) + o.baseVertex;
                    const int64_t off = v * g_capStride;
                    if (v < 0 ||
                        off + g_capStride > static_cast<int64_t>(g_capVertexBytes)) {
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
                }
                if (bad) {
                    Log::get().note("    quad %u: an index landed outside the "
                                    "copied range -- baseVertex %d, stride %u.",
                                    q, o.baseVertex, g_capStride);
                    continue;
                }
                char tail[kTailBytesMax * 2 + 1] = "";
                if (firstOff >= 0 && g_capStride > 8) {
                    uint32_t nTail = g_capStride - 8;
                    if (nTail > kTailBytesMax) nTail = kTailBytesMax;
                    for (uint32_t t = 0; t < nTail; ++t) {
                        snprintf(tail + t * 2, 3, "%02X",
                                 vb[firstOff + 8 + t]);
                    }
                }
                Log::get().note("    quad %u: x %.3f..%.3f  y %.3f..%.3f  "
                                "(w %.3f h %.3f)  +%s",
                                q, lo[0], hi[0], lo[1], hi[1],
                                hi[0] - lo[0], hi[1] - lo[1], tail);
            }
        }
        ctx->Unmap(g_vbStage, 0);
        ctx->Unmap(g_ibStage, 0);
    });
    dropCapture();
}

void quadProbeShutdown() {
    dropCapture();
}

}  // namespace edvr

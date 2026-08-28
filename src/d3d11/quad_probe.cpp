#include "quad_probe.h"

#include <windows.h>

#include <d3d11.h>

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
// the copy has to be taken AT the matched draw. It is taken once per session.
constexpr uint32_t kMaxVertexBytes = 4u << 20;

// Frames to let the copy execute before mapping. panel_quad's number: long
// enough that the map never stalls the render thread, short enough that a
// capture is retired within a blink.
constexpr uint32_t kSettleFrames = 4;

FaultBudget g_budget("quadProbe", 4);

bool     g_armed = false;
uint32_t g_wantW = 0, g_wantH = 0;
char     g_wantKind = 0;
uint32_t g_wantN = 0;
bool     g_taken = false;          // one capture per session
// The fix supplies its own capture criteria when no probe spec is set. Zero
// target dimensions then mean "any offscreen surface big enough to be an
// interface", because the fix must work on a rig whose surface is not the
// size this was measured on -- that number moves with render scale.

ID3D11Buffer* g_ibStage = nullptr;
ID3D11Buffer* g_vbStage = nullptr;
uint32_t      g_pendingFrame = 0;  // 0 = nothing pending
uint32_t      g_frame = 0;
uint32_t      g_capIndexCount = 0;
uint32_t      g_capStride = 0;
uint32_t      g_capVertexBytes = 0;
int           g_capBaseVertex = 0;
DXGI_FORMAT   g_capIbFormat = DXGI_FORMAT_UNKNOWN;

void failOnce(const char* why) {
    static bool noted = false;
    if (noted) return;
    noted = true;
    Log::get().note("quad probe: %s. No capture this session.", why);
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
        Log::get().note("quad probe ARMED on the %c:%u draw into a %ux%u "
                        "target: its %u quads will be copied once and their "
                        "rectangles logged. Nothing is changed.",
                        kind, n, w, h, n / kIndicesPerQuad);
    }
    g_armed = armed;
}

bool quadProbeWants() { return g_armed && !g_taken; }

bool quadProbeOnDraw(ID3D11DeviceContext* ctx, uint32_t targetW,
                     uint32_t targetH, char kind, uint32_t count,
                     uint32_t instances, uint32_t startIndex, int baseVertex) {
    (void)instances;
    if (!g_armed || g_taken || g_pendingFrame || !ctx) return false;
    if (targetW != g_wantW || targetH != g_wantH) return false;
    if (kind != g_wantKind || count != g_wantN) return false;

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

        D3D11_BUFFER_DESC vd{};
        vb->GetDesc(&vd);
        const UINT vBytes = vd.ByteWidth > kMaxVertexBytes ? kMaxVertexBytes
                                                           : vd.ByteWidth;

        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            D3D11_BUFFER_DESC sd{};
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.ByteWidth = count * idxSize;
            bool ok = SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &g_ibStage));
            sd.ByteWidth = vBytes;
            ok = ok && SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &g_vbStage));
            if (ok) {
                D3D11_BOX ibBox{};
                ibBox.left = ibOff + startIndex * idxSize;
                ibBox.right = ibBox.left + count * idxSize;
                ibBox.bottom = 1; ibBox.back = 1;
                ctx->CopySubresourceRegion(g_ibStage, 0, 0, 0, 0, ib, 0, &ibBox);
                D3D11_BOX vbBox{};
                vbBox.left = 0;
                vbBox.right = vBytes;
                vbBox.bottom = 1; vbBox.back = 1;
                ctx->CopySubresourceRegion(g_vbStage, 0, 0, 0, 0, vb, 0, &vbBox);
                g_capIndexCount = count;
                g_capStride = stride;
                g_capVertexBytes = vBytes;
                g_capBaseVertex = baseVertex;
                g_capIbFormat = ibFmt;
                g_pendingFrame = g_frame + kSettleFrames;
                started = true;
            } else {
                failOnce("the staging buffers could not be created");
                if (g_ibStage) { g_ibStage->Release(); g_ibStage = nullptr; }
                if (g_vbStage) { g_vbStage->Release(); g_vbStage = nullptr; }
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
    if (!g_pendingFrame || g_frame < g_pendingFrame || !ctx) return;
    g_pendingFrame = 0;
    g_taken = true;   // one attempt, whatever it yields

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
        const uint8_t* vb = static_cast<const uint8_t*>(mv.pData);
        const uint32_t quads = g_capIndexCount / kIndicesPerQuad;
        Log::get().note("quad probe: %u quads, stride %u, index format %s. "
                        "Rectangles below are the raw vertex positions -- "
                        "plausible numbers mean the layout guess (float3 at "
                        "offset 0) is right.",
                        quads, g_capStride,
                        g_capIbFormat == DXGI_FORMAT_R16_UINT ? "R16" : "R32");
        for (uint32_t q = 0; q < quads; ++q) {
            float lo[3] = {1e30f, 1e30f, 1e30f};
            float hi[3] = {-1e30f, -1e30f, -1e30f};
            bool bad = false;
            for (uint32_t k = 0; k < kIndicesPerQuad; ++k) {
                const uint32_t at = q * kIndicesPerQuad + k;
                uint32_t vi;
                if (g_capIbFormat == DXGI_FORMAT_R16_UINT) {
                    vi = static_cast<const uint16_t*>(mi.pData)[at];
                } else {
                    vi = static_cast<const uint32_t*>(mi.pData)[at];
                }
                const int64_t v = static_cast<int64_t>(vi) + g_capBaseVertex;
                const int64_t off = v * g_capStride;
                if (v < 0 || off + 12 > static_cast<int64_t>(g_capVertexBytes)) {
                    bad = true;
                    break;
                }
                float p[3];
                memcpy(p, vb + off, sizeof(p));
                for (int c = 0; c < 3; ++c) {
                    if (p[c] < lo[c]) lo[c] = p[c];
                    if (p[c] > hi[c]) hi[c] = p[c];
                }
            }
            if (bad) {
                Log::get().note("  quad %u: an index landed outside the copied "
                                "range -- baseVertex %d, stride %u.",
                                q, g_capBaseVertex, g_capStride);
                continue;
            }
            Log::get().note("  quad %u: x %.3f..%.3f  y %.3f..%.3f  z %.3f..%.3f"
                            "   (w %.3f h %.3f)",
                            q, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2],
                            hi[0] - lo[0], hi[1] - lo[1]);
        }
        ctx->Unmap(g_vbStage, 0);
        ctx->Unmap(g_ibStage, 0);
    });
    quadProbeShutdown();
}

void quadProbeShutdown() {
    if (g_ibStage) { g_ibStage->Release(); g_ibStage = nullptr; }
    if (g_vbStage) { g_vbStage->Release(); g_vbStage = nullptr; }
    g_pendingFrame = 0;
}

}  // namespace edvr

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

// The loading dialog's bordered panel: one fill plus four edge strips, six
// indices each. Measured by this probe on 2026-08-28.
constexpr uint32_t kPanelIndices = 30;

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
bool     g_wantAnyTarget = false;

ID3D11Buffer* g_ibStage = nullptr;
ID3D11Buffer* g_vbStage = nullptr;
uint32_t      g_pendingFrame = 0;  // 0 = nothing pending
uint32_t      g_frame = 0;
uint32_t      g_capIndexCount = 0;
uint32_t      g_capStride = 0;
uint32_t      g_capVertexBytes = 0;
int           g_capBaseVertex = 0;
DXGI_FORMAT   g_capIbFormat = DXGI_FORMAT_UNKNOWN;

// The fix's half: the scale, and the geometry built from the capture.
float         g_scale = 0.0f;
ID3D11Buffer* g_ourVb = nullptr;
ID3D11Buffer* g_ourIb = nullptr;
uint32_t      g_ourIndices = 0;
uint32_t      g_ourStride = 0;
bool          g_builtNoted = false;

void failOnce(const char* why) {
    static bool noted = false;
    if (noted) return;
    noted = true;
    Log::get().note("quad probe: %s. No capture this session.", why);
}

}  // namespace

void quadProbeConfigure(Config& cfg) {
    const float scale = cfg.getFloat("fix.loading_panel_scale", 0.0f);
    const float clamped = (scale > 0.0f && scale <= 1.0f) ? scale : 0.0f;
    if (clamped != g_scale) {
        // A changed factor needs new geometry, and the capture it is built
        // from is still good -- only the buffers are rebuilt.
        if (g_ourVb) { g_ourVb->Release(); g_ourVb = nullptr; }
        if (g_ourIb) { g_ourIb->Release(); g_ourIb = nullptr; }
        g_ourIndices = 0;
        g_builtNoted = false;
        g_taken = false;   // re-capture, so the rebuild has vertices to use
    }
    g_scale = clamped;
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
    g_wantAnyTarget = false;
    // With the fix on and no probe spec, the fix names the draw itself: the
    // loading dialog's bordered panel, thirty indices, on any interface-sized
    // surface. Without this the capture waits for a spec that is not set and
    // the fix silently never engages -- which is exactly what it did on
    // 2026-08-28 before this existed.
    if (!armed && g_scale > 0.0f) {
        g_wantKind = 'X';
        g_wantN = kPanelIndices;
        g_wantW = 0;
        g_wantH = 0;
        g_wantAnyTarget = true;
    }
    if (armed && !g_armed) {
        Log::get().note("quad probe ARMED on the %c:%u draw into a %ux%u "
                        "target: its %u quads will be copied once and their "
                        "rectangles logged. Nothing is changed.",
                        kind, n, w, h, n / kIndicesPerQuad);
    }
    g_armed = armed;
}

bool quadProbeWants() { return (g_armed || g_scale > 0.0f) && !g_taken; }

bool quadScaleWants() { return g_scale > 0.0f; }

bool quadProbeOnDraw(ID3D11DeviceContext* ctx, uint32_t targetW,
                     uint32_t targetH, char kind, uint32_t count,
                     uint32_t instances, uint32_t startIndex, int baseVertex) {
    (void)instances;
    if (!g_armed || g_taken || g_pendingFrame || !ctx) return false;
    if (g_wantAnyTarget) {
        if (targetW < 1024 || targetH < 512) return false;
    } else if (targetW != g_wantW || targetH != g_wantH) {
        return false;
    }
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
        // THE FIX: one vertex per index, positions scaled toward the set's
        // own centre, every other byte copied verbatim. A private index
        // buffer of 0..n-1 means no index remapping and no dependence on
        // where the game's vertices happened to sit.
        if (g_scale > 0.0f && !g_ourVb) {
            const uint32_t n = g_capIndexCount;
            std::vector<uint8_t> verts(static_cast<size_t>(n) * g_capStride);
            std::vector<uint16_t> idx(n);
            bool ok = true;
            float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
            for (uint32_t k = 0; k < n && ok; ++k) {
                uint32_t vi = (g_capIbFormat == DXGI_FORMAT_R16_UINT)
                                  ? static_cast<const uint16_t*>(mi.pData)[k]
                                  : static_cast<const uint32_t*>(mi.pData)[k];
                const int64_t v = static_cast<int64_t>(vi) + g_capBaseVertex;
                const int64_t off = v * g_capStride;
                if (v < 0 || off + g_capStride >
                                 static_cast<int64_t>(g_capVertexBytes)) {
                    ok = false;
                    break;
                }
                memcpy(&verts[static_cast<size_t>(k) * g_capStride],
                       vb + off, g_capStride);
                idx[k] = static_cast<uint16_t>(k);
                float p[2];
                memcpy(p, vb + off, sizeof(p));
                for (int c = 0; c < 2; ++c) {
                    if (p[c] < lo[c]) lo[c] = p[c];
                    if (p[c] > hi[c]) hi[c] = p[c];
                }
            }
            if (ok) {
                const float cx = (lo[0] + hi[0]) * 0.5f;
                const float cy = (lo[1] + hi[1]) * 0.5f;
                for (uint32_t k = 0; k < n; ++k) {
                    float* p = reinterpret_cast<float*>(
                        &verts[static_cast<size_t>(k) * g_capStride]);
                    p[0] = cx + (p[0] - cx) * g_scale;
                    p[1] = cy + (p[1] - cy) * g_scale;
                }
                ID3D11Device* dev = nullptr;
                ctx->GetDevice(&dev);
                if (dev) {
                    D3D11_BUFFER_DESC bd{};
                    D3D11_SUBRESOURCE_DATA sr{};
                    bd.Usage = D3D11_USAGE_IMMUTABLE;
                    bd.ByteWidth = static_cast<UINT>(verts.size());
                    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                    sr.pSysMem = verts.data();
                    bool made = SUCCEEDED(dev->CreateBuffer(&bd, &sr, &g_ourVb));
                    bd.ByteWidth = static_cast<UINT>(idx.size() * 2);
                    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
                    sr.pSysMem = idx.data();
                    made = made && SUCCEEDED(dev->CreateBuffer(&bd, &sr, &g_ourIb));
                    dev->Release();
                    if (made) {
                        g_ourIndices = n;
                        g_ourStride = g_capStride;
                    } else {
                        if (g_ourVb) { g_ourVb->Release(); g_ourVb = nullptr; }
                        if (g_ourIb) { g_ourIb->Release(); g_ourIb = nullptr; }
                        failOnce("the scaled panel's buffers could not be made");
                    }
                }
            } else {
                failOnce("an index landed outside the copied vertex range");
            }
        }
        ctx->Unmap(g_vbStage, 0);
        ctx->Unmap(g_ibStage, 0);
    });
    // The staging copies go; the built geometry stays for the session.
    if (g_ibStage) { g_ibStage->Release(); g_ibStage = nullptr; }
    if (g_vbStage) { g_vbStage->Release(); g_vbStage = nullptr; }
}

bool quadScaleSubstitute(ID3D11DeviceContext* ctx, PfnDrawIndexedInstanced draw,
                         uint32_t instances, uint32_t startInstance) {
    if (!ctx || !draw || !g_ourVb || !g_ourIb || g_ourIndices == 0) return false;
    bool done = false;
    guardedBudget(g_budget, [&] {
        // Save what the game had bound and put every piece of it back. Our
        // vertices are byte-identical in format to its own, so the input
        // layout it already set serves both.
        ID3D11Buffer* savedVb = nullptr;
        UINT savedStride = 0, savedOff = 0;
        ctx->IAGetVertexBuffers(0, 1, &savedVb, &savedStride, &savedOff);
        ID3D11Buffer* savedIb = nullptr;
        DXGI_FORMAT savedFmt = DXGI_FORMAT_UNKNOWN;
        UINT savedIbOff = 0;
        ctx->IAGetIndexBuffer(&savedIb, &savedFmt, &savedIbOff);

        const UINT stride = g_ourStride, zero = 0;
        ctx->IASetVertexBuffers(0, 1, &g_ourVb, &stride, &zero);
        ctx->IASetIndexBuffer(g_ourIb, DXGI_FORMAT_R16_UINT, 0);
        draw(ctx, g_ourIndices, instances, 0, 0, startInstance);
        ctx->IASetVertexBuffers(0, 1, &savedVb, &savedStride, &savedOff);
        ctx->IASetIndexBuffer(savedIb, savedFmt, savedIbOff);
        if (savedVb) savedVb->Release();
        if (savedIb) savedIb->Release();
        done = true;

        if (!g_builtNoted) {
            g_builtNoted = true;
            Log::get().note("loading panel: drawn at %.2f of its own size -- "
                            "%u vertices copied from the game's own, positions "
                            "scaled toward their centre, everything else "
                            "including colour untouched.", g_scale,
                            g_ourIndices);
        }
    });
    return done;
}

void quadProbeShutdown() {
    if (g_ibStage) { g_ibStage->Release(); g_ibStage = nullptr; }
    if (g_vbStage) { g_vbStage->Release(); g_vbStage = nullptr; }
    if (g_ourVb) { g_ourVb->Release(); g_ourVb = nullptr; }
    if (g_ourIb) { g_ourIb->Release(); g_ourIb = nullptr; }
    g_ourIndices = 0;
    g_pendingFrame = 0;
}

}  // namespace edvr

#include "loader_panel.h"

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

// Six indices to a quad at topology 4, which is what the census reports for
// this whole family.
constexpr uint32_t kIndicesPerQuad = 6;

// The panel: one fill plus four border strips. Measured 2026-08-28.
constexpr uint32_t kPanelIndices = 30;

// A draw whose quads cover this much of the widest thing in the frame is a
// backdrop, not content. The panel spans the surface; a second full-surface
// rectangle is another backdrop and must not drag the dialog's bounds out to
// the edges with it.
constexpr float kBackdropFraction = 0.80f;

// Margin around the dialog, as a fraction of its size. A backing exactly the
// dialog's bounds looks cropped; this is the padding a panel would have.
constexpr float kMargin = 0.06f;

// How many draws into the surface one measurement collects. The loader's
// frame had eight; this leaves room without inviting a scan.
constexpr uint32_t kMaxDraws = 16;

// Index bytes one measurement will hold. The loader's draws totalled about
// 3,600 indices; 64 KB is far above that and still trivial.
constexpr uint32_t kIbStageBytes = 64u << 10;

// Frames to let the copies execute before mapping, so the map never stalls
// the render thread. panel_quad's number.
constexpr uint32_t kSettleFrames = 4;

FaultBudget g_budget("loaderPanel", 6);

bool g_on = false;

struct Rect {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    bool valid() const { return x1 >= x0 && y1 >= y0; }
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
    void add(float x, float y) {
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
};

// One draw collected into the pending measurement.
struct CapDraw {
    uint32_t count = 0;
    uint32_t ibOffset = 0;   // bytes into the index staging buffer
    int      baseVertex = 0;
    bool     isPanel = false;
};

// --- the pending measurement -------------------------------------------
ID3D11Buffer* g_ibStage = nullptr;
ID3D11Buffer* g_vbStage = nullptr;
CapDraw       g_draws[kMaxDraws];
uint32_t      g_drawCount = 0;
uint32_t      g_ibFill = 0;
uint32_t      g_capStride = 0;
uint32_t      g_capVertexBytes = 0;
DXGI_FORMAT   g_capIbFormat = DXGI_FORMAT_UNKNOWN;
uint32_t      g_capFrame = 0;      // frame the collection started
bool          g_collecting = false;
uint32_t      g_frame = 0;
uint32_t      g_settleAt = 0;      // 0 = nothing pending

// --- the built result ---------------------------------------------------
ID3D11Buffer* g_ourVb = nullptr;
ID3D11Buffer* g_ourIb = nullptr;
uint32_t      g_ourIndices = 0;
uint32_t      g_ourStride = 0;
bool          g_noted = false;

// The shape the last measurement was taken from. When the dialog changes,
// the draws into the surface change with it, and that is the trigger to
// measure again rather than serve a panel sized to the previous dialog.
uint32_t      g_shapeHash = 0;
uint32_t      g_liveShapeHash = 0;
uint32_t      g_thisFrameShape = 0;
uint32_t      g_thisFrameNo = 0;
uint32_t      g_measurements = 0;

void failOnce(const char* why) {
    static bool noted = false;
    if (noted) return;
    noted = true;
    Log::get().note("loading panel: %s. The panel draws stock.", why);
}

void dropPending() {
    if (g_ibStage) { g_ibStage->Release(); g_ibStage = nullptr; }
    if (g_vbStage) { g_vbStage->Release(); g_vbStage = nullptr; }
    g_drawCount = 0;
    g_ibFill = 0;
    g_collecting = false;
    g_settleAt = 0;
}

void dropBuilt() {
    if (g_ourVb) { g_ourVb->Release(); g_ourVb = nullptr; }
    if (g_ourIb) { g_ourIb->Release(); g_ourIb = nullptr; }
    g_ourIndices = 0;
    g_shapeHash = 0;
}

}  // namespace

void loaderPanelConfigure(Config& cfg) {
    const bool was = g_on;
    const std::string m = cfg.getString("fix.loading_panel", "stock");
    if (m == "stock") {
        g_on = false;
    } else if (m == "fit") {
        g_on = true;
    } else {
        g_on = false;
        Log::get().note("loading_panel \"%s\" is not stock or fit; running "
                        "stock.", m.c_str());
    }
    if (was != g_on) {
        Log::get().note(
            "loading panel: %s. The dark panel behind the loader's dialog is "
            "%s. It is drawn at the full size of the interface surface, which "
            "on a monitor is a modal scrim and in a headset is most of your "
            "view (docs/loading-scrim.md).",
            g_on ? "FIT" : "stock",
            g_on ? "measured against the dialog it backs and redrawn to that "
                   "size, its own art and colours untouched"
                 : "the game's own");
    }
}

bool loaderPanelWants() { return g_on; }

bool loaderPanelOnDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                       uint32_t instances, uint32_t startIndex, int baseVertex,
                       uint32_t targetW, uint32_t targetH) {
    (void)instances;
    (void)targetW;
    (void)targetH;
    if (!g_on || !ctx || kind != 'X' || count == 0) return false;

    // Every draw into this surface contributes to the frame's shape, panel
    // included. A changed shape is a changed dialog.
    if (g_thisFrameNo != g_frame) {
        g_thisFrameNo = g_frame;
        g_liveShapeHash = g_thisFrameShape;
        g_thisFrameShape = 2166136261u;
    }
    g_thisFrameShape = (g_thisFrameShape ^ count) * 16777619u;

    const bool isPanel = (count == kPanelIndices);

    // Collect a measurement when there is none for this shape. Collection
    // runs for exactly one frame: a draw arriving in a later frame belongs to
    // a different snapshot of the buffer and must not be mixed in.
    const bool needMeasure = (g_ourIndices == 0 || g_shapeHash != g_liveShapeHash);
    if (needMeasure && !g_settleAt) {
        if (!g_collecting) {
            g_collecting = true;
            g_capFrame = g_frame;
            g_drawCount = 0;
            g_ibFill = 0;
        }
        if (g_capFrame == g_frame && g_drawCount < kMaxDraws) {
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
                    return;
                }
                const UINT idxSize =
                    (ibFmt == DXGI_FORMAT_R16_UINT) ? 2u : 4u;
                const UINT need = count * idxSize;
                ID3D11Device* dev = nullptr;
                ctx->GetDevice(&dev);
                if (dev && g_ibFill + need <= kIbStageBytes) {
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
                        sd.ByteWidth = vd.ByteWidth;
                        ok = SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &g_vbStage));
                        if (ok) {
                            // The whole buffer, once: every draw in this frame
                            // indexes into it and the copy must be of the same
                            // snapshot they all drew from.
                            D3D11_BOX all{};
                            all.right = vd.ByteWidth;
                            all.bottom = 1; all.back = 1;
                            ctx->CopySubresourceRegion(g_vbStage, 0, 0, 0, 0,
                                                       vb, 0, &all);
                            g_capVertexBytes = vd.ByteWidth;
                            g_capStride = stride;
                        }
                    }
                    if (ok && g_ibStage && g_vbStage) {
                        D3D11_BOX box{};
                        box.left = ibOff + startIndex * idxSize;
                        box.right = box.left + need;
                        box.bottom = 1; box.back = 1;
                        ctx->CopySubresourceRegion(g_ibStage, 0, g_ibFill, 0, 0,
                                                   ib, 0, &box);
                        g_draws[g_drawCount].count = count;
                        g_draws[g_drawCount].ibOffset = g_ibFill;
                        g_draws[g_drawCount].baseVertex = baseVertex;
                        g_draws[g_drawCount].isPanel = isPanel;
                        ++g_drawCount;
                        g_ibFill += need;
                        g_capIbFormat = ibFmt;
                        g_settleAt = 0;   // set when the frame ends, below
                    }
                }
                if (dev) dev->Release();
                ib->Release();
                vb->Release();
            });
        }
    }

    // The panel's own draw is the one that gets substituted, and only once a
    // measurement has produced geometry.
    return isPanel && g_ourIndices != 0;
}

bool loaderPanelSubstitute(ID3D11DeviceContext* ctx, PfnDrawIndexedInstanced draw,
                           uint32_t instances, uint32_t startInstance) {
    if (!ctx || !draw || !g_ourVb || !g_ourIb || g_ourIndices == 0) return false;
    bool done = false;
    guardedBudget(g_budget, [&] {
        // Save what the game had bound and put all of it back. Our vertices
        // are byte-identical in format to its own, so the input layout it
        // already set serves both.
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
    });
    return done;
}

void loaderPanelTick(ID3D11DeviceContext* ctx) {
    ++g_frame;
    if (!g_on) {
        if (g_ibStage || g_vbStage) dropPending();
        return;
    }
    // A collection that ran last frame is now complete: give the copies time
    // to execute before mapping them.
    if (g_collecting && g_frame > g_capFrame && !g_settleAt) {
        g_collecting = false;
        g_settleAt = g_drawCount ? g_frame + kSettleFrames : 0;
        if (!g_settleAt) dropPending();
    }
    if (!g_settleAt || g_frame < g_settleAt || !ctx) return;
    g_settleAt = 0;

    guardedBudget(g_budget, [&] {
        D3D11_MAPPED_SUBRESOURCE mi{}, mv{};
        if (FAILED(ctx->Map(g_ibStage, 0, D3D11_MAP_READ, 0, &mi)) || !mi.pData ||
            FAILED(ctx->Map(g_vbStage, 0, D3D11_MAP_READ, 0, &mv)) || !mv.pData) {
            if (mi.pData) ctx->Unmap(g_ibStage, 0);
            failOnce("the measurement could not be mapped");
            return;
        }
        const uint8_t* ibBase = static_cast<const uint8_t*>(mi.pData);
        const uint8_t* vbBase = static_cast<const uint8_t*>(mv.pData);
        const bool i16 = g_capIbFormat == DXGI_FORMAT_R16_UINT;

        // Pass one: every draw's bounds, and the widest of them, so
        // "spans the surface" has something to be a fraction of.
        Rect bounds[kMaxDraws];
        float widest = 0.0f;
        for (uint32_t d = 0; d < g_drawCount; ++d) {
            const CapDraw& cd = g_draws[d];
            for (uint32_t k = 0; k < cd.count; ++k) {
                const uint8_t* at = ibBase + cd.ibOffset + k * (i16 ? 2 : 4);
                uint32_t vi = i16 ? *reinterpret_cast<const uint16_t*>(at)
                                  : *reinterpret_cast<const uint32_t*>(at);
                const int64_t v = static_cast<int64_t>(vi) + cd.baseVertex;
                const int64_t off = v * g_capStride;
                if (v < 0 || off + 8 > static_cast<int64_t>(g_capVertexBytes)) {
                    continue;
                }
                float p[2];
                memcpy(p, vbBase + off, sizeof(p));
                bounds[d].add(p[0], p[1]);
            }
            if (bounds[d].valid() && bounds[d].w() > widest) widest = bounds[d].w();
        }

        // Pass two: the dialog is everything that is NOT a backdrop.
        Rect dialog;
        Rect panel;
        int panelIndex = -1;
        for (uint32_t d = 0; d < g_drawCount; ++d) {
            if (!bounds[d].valid()) continue;
            const bool spansAll =
                widest > 0.0f && bounds[d].w() >= widest * kBackdropFraction &&
                bounds[d].h() >= widest * kBackdropFraction * 0.5f;
            if (g_draws[d].isPanel && spansAll && panelIndex < 0) {
                panelIndex = static_cast<int>(d);
                panel = bounds[d];
                continue;
            }
            if (spansAll) continue;   // another backdrop, not content
            dialog.add(bounds[d].x0, bounds[d].y0);
            dialog.add(bounds[d].x1, bounds[d].y1);
        }

        if (panelIndex < 0 || !panel.valid() || !dialog.valid() ||
            panel.w() <= 0.0f || panel.h() <= 0.0f) {
            Log::get().note("loading panel: %u draw(s) measured but no panel "
                            "and dialog could be told apart; drawing stock.",
                            g_drawCount);
            ctx->Unmap(g_vbStage, 0);
            ctx->Unmap(g_ibStage, 0);
            return;
        }

        // The target: the dialog's bounds with a margin, so the backing has
        // the padding a panel would have rather than ending on the text.
        const float mx = dialog.w() * kMargin;
        const float my = dialog.h() * kMargin;
        const float tx0 = dialog.x0 - mx, tx1 = dialog.x1 + mx;
        const float ty0 = dialog.y0 - my, ty1 = dialog.y1 + my;

        // Rebuild the panel: its vertices copied verbatim, positions mapped
        // linearly from its own bounds onto the target. Only the float2 at
        // offset 0 changes, so colour and everything else in the 24-byte
        // vertex survives and no encoding has to be understood.
        const CapDraw& pd = g_draws[panelIndex];
        std::vector<uint8_t> verts(static_cast<size_t>(pd.count) * g_capStride);
        std::vector<uint16_t> idx(pd.count);
        bool ok = true;
        for (uint32_t k = 0; k < pd.count && ok; ++k) {
            const uint8_t* at = ibBase + pd.ibOffset + k * (i16 ? 2 : 4);
            uint32_t vi = i16 ? *reinterpret_cast<const uint16_t*>(at)
                              : *reinterpret_cast<const uint32_t*>(at);
            const int64_t v = static_cast<int64_t>(vi) + pd.baseVertex;
            const int64_t off = v * g_capStride;
            if (v < 0 || off + g_capStride > static_cast<int64_t>(g_capVertexBytes)) {
                ok = false;
                break;
            }
            uint8_t* dst = &verts[static_cast<size_t>(k) * g_capStride];
            memcpy(dst, vbBase + off, g_capStride);
            float* p = reinterpret_cast<float*>(dst);
            p[0] = tx0 + (p[0] - panel.x0) / panel.w() * (tx1 - tx0);
            p[1] = ty0 + (p[1] - panel.y0) / panel.h() * (ty1 - ty0);
            idx[k] = static_cast<uint16_t>(k);
        }
        ctx->Unmap(g_vbStage, 0);
        ctx->Unmap(g_ibStage, 0);
        if (!ok) {
            failOnce("an index landed outside the copied vertex range");
            return;
        }

        dropBuilt();
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return;
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
        if (!made) {
            dropBuilt();
            failOnce("the resized panel's buffers could not be made");
            return;
        }
        g_ourIndices = pd.count;
        g_ourStride = g_capStride;
        g_shapeHash = g_liveShapeHash;
        ++g_measurements;
        if (!g_noted || g_measurements <= 4) {
            g_noted = true;
            Log::get().note(
                "loading panel: FIT -- measurement %u from %u draws. The panel "
                "spans %.0fx%.0f; the dialog it backs measures %.0fx%.0f, so "
                "it is redrawn there with a %.0f%% margin. Its own vertices, "
                "positions remapped and nothing else touched.",
                g_measurements, g_drawCount, panel.w(), panel.h(),
                dialog.w(), dialog.h(), kMargin * 100.0f);
        }
    });
    dropPending();
}

void loaderPanelShutdown() {
    dropPending();
    dropBuilt();
}

}  // namespace edvr

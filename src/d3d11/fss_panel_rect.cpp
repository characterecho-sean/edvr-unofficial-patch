#include "fss_panel_rect.h"

#include <cmath>
#include <cstring>

#include <windows.h>

#include <d3d11.h>

#include "../common/frame_flag.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "device_hook.h"   // deviceHookFssModeLatch

namespace edvr {
namespace {

constexpr uint32_t kMaxOrd = 12;

// One matched draw of the capture frame: its args and its own vertex
// window, staged AT that draw so the churning pools cohere.
struct Ord {
    uint32_t startInstance = 0;
    int32_t  baseVertex = 0;
    void*    rtPtr = nullptr;   // which eye's target this draw painted
    ID3D11Buffer* stVerts = nullptr;
};

Ord g_ord[kMaxOrd];
uint32_t g_ordCount = 0;
ID3D11Buffer* g_stCb0A = nullptr;   // ordinal 0's eye
ID3D11Buffer* g_stCb0B = nullptr;   // ordinal 1's eye (the pair partner)
ID3D11Buffer* g_stCb1 = nullptr;
ID3D11Buffer* g_stVb0 = nullptr;    // the whole instance stream
ID3D11Buffer* g_stT33 = nullptr;    // the whole record pool
uint32_t g_t33First = 0;
uint32_t g_t33Stride = 336;
uint32_t g_t33Bytes = 0;
uint32_t g_vb0Stride = 8;
uint32_t g_vb0Offset = 0;
uint32_t g_vb0Bytes = 0;

int  g_countdown = -1;    // frames until readback (counted at ordinal 0)
bool g_capturing = false; // this frame's draws are being windowed
bool g_published = false;
bool g_failNoted = false;
uint32_t g_derives = 0;
uint32_t g_skipMask = 0;
float g_pickU[2] = {};
bool g_pickLeftIsA = true;

FaultBudget g_budget("fssPanelRect", 8);

// The render target's resource pointer: the two eyes paint two distinct
// textures, and THAT -- not draw order -- is what tells them apart. The
// 45h flight's uA == uB was the lesson: each quad draws twice per eye
// (depth prepass + colour pass), so ordinals 0 and 1 are the SAME eye.
void* rtResource(ID3D11DeviceContext* ctx) {
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return nullptr;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    rtv->Release();
    if (!res) return nullptr;
    void* p = res;
    res->Release();
    return p;
}

void releaseAll() {
    for (Ord& o : g_ord) {
        if (o.stVerts) {
            o.stVerts->Release();
            o.stVerts = nullptr;
        }
    }
    g_ordCount = 0;
    for (ID3D11Buffer** b :
         {&g_stCb0A, &g_stCb0B, &g_stCb1, &g_stVb0, &g_stT33}) {
        if (*b) {
            (*b)->Release();
            *b = nullptr;
        }
    }
}

ID3D11Buffer* stageRange(ID3D11DeviceContext* ctx, ID3D11Device* dev,
                         ID3D11Buffer* src, uint32_t offset, uint32_t want) {
    D3D11_BUFFER_DESC sd{};
    src->GetDesc(&sd);
    if (offset >= sd.ByteWidth) return nullptr;
    const uint32_t avail = sd.ByteWidth - offset;
    const uint32_t n = avail < want ? avail : want;
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = n;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* st = nullptr;
    if (FAILED(dev->CreateBuffer(&d, nullptr, &st)) || !st) return nullptr;
    D3D11_BOX box{offset, 0, 0, offset + n, 1, 1};
    ctx->CopySubresourceRegion(st, 0, 0, 0, 0, src, 0, &box);
    return st;
}

ID3D11Buffer* stageWhole(ID3D11DeviceContext* ctx, ID3D11Device* dev,
                         ID3D11Buffer* src, uint32_t* bytes) {
    D3D11_BUFFER_DESC sd{};
    src->GetDesc(&sd);
    *bytes = sd.ByteWidth;
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = sd.ByteWidth;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* st = nullptr;
    if (FAILED(dev->CreateBuffer(&d, nullptr, &st)) || !st) return nullptr;
    ctx->CopyResource(st, src);
    return st;
}

bool mapRead(ID3D11DeviceContext* ctx, ID3D11Buffer* st, void* out,
             uint32_t bytes) {
    if (!st) return false;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m)) || !m.pData) {
        return false;
    }
    D3D11_BUFFER_DESC d{};
    st->GetDesc(&d);
    const uint32_t n = d.ByteWidth < bytes ? d.ByteWidth : bytes;
    memcpy(out, m.pData, n);
    ctx->Unmap(st, 0);
    return n == bytes;
}

// The engine's own decodes and rotation, unnormalised on purpose -- the
// shader's exact arithmetic (fss_panel_vs.h).
void quatRotate(const float q[4], float px, float py, float pz, float* ox,
                float* oy, float* oz) {
    const float cxv = q[1] * pz - py * q[2];
    const float cyv = q[2] * px - pz * q[0];
    const float czv = q[0] * py - px * q[1];
    const float d = q[0] * px + q[1] * py + q[2] * pz;
    const float t = q[3] * (q[3] + q[3]);
    *ox = t * px - px + d * (q[0] + q[0]) + cxv * (q[3] + q[3]);
    *oy = t * py - py + d * (q[1] + q[1]) + cyv * (q[3] + q[3]);
    *oz = t * pz - pz + d * (q[2] + q[2]) + czv * (q[3] + q[3]);
}

bool decodePos(const uint32_t pva[4], float out[3]) {
    const uint32_t fmt = (pva[2] >> 24u) & 0x7Fu;
    if (fmt == 64u) {
        const float lx = static_cast<float>(pva[0] & 0xFFFFu);
        const float hx = static_cast<float>(pva[0] >> 16u);
        const float ly = static_cast<float>(pva[1] & 0xFFFFu);
        const float hy = static_cast<float>(pva[1] >> 16u);
        const float s = exp2f(hy * 0.000244f) - 1.0f;
        out[0] = (lx * 0.000031f - 1.0f) * s;
        out[1] = (hx * 0.000031f - 1.0f) * s;
        out[2] = (ly * 0.000031f - 1.0f) * s;
        return true;
    }
    if (fmt >= 20u) return false;
    const uint32_t sh = (pva[2] >> 24u) & 31u;
    const float scl = 1.0f / static_cast<float>((1u << (20u - fmt)) - 1u);
    const float offs = static_cast<float>(1u << sh);
    const uint32_t xb = pva[0] & 0x1FFFFFu;
    const uint32_t yb = (pva[0] >> 21u) + ((pva[1] & 0x3FFu) << 11u);
    const uint32_t zb = (pva[1] >> 10u) & 0x1FFFFFu;
    out[0] = static_cast<float>(xb) * scl - offs;
    out[1] = static_cast<float>(yb) * scl - offs;
    out[2] = static_cast<float>(zb) * scl - offs;
    return true;
}

bool project(const float rows[16], const float p3[3], float* u, float* v) {
    const float p[4] = {p3[0], p3[1], p3[2], 1.0f};
    float clip[4];
    for (int r = 0; r < 4; ++r) {
        clip[r] = rows[r * 4 + 0] * p[0] + rows[r * 4 + 1] * p[1] +
                  rows[r * 4 + 2] * p[2] + rows[r * 4 + 3];
    }
    if (clip[3] < 1e-3f) return false;
    *u = (clip[0] / clip[3] + 1.0f) * 0.5f;
    *v = (1.0f - clip[1] / clip[3]) * 0.5f;
    return true;
}

}  // namespace

uint32_t fssPanelRectSkipMask() { return g_skipMask; }

void fssPanelRectOnComposite(ID3D11DeviceContext* ctx, uint32_t ordinal,
                             uint32_t startInstance, int32_t baseVertex) {
    if (!ctx) return;
    if (!deviceHookFssModeLatch()) {
        g_published = false;
        g_capturing = false;
        g_countdown = -1;
        g_skipMask = 0;
        releaseAll();
        return;
    }
    if (g_published) return;

    guardedBudget(g_budget, [&] {
        ID3D11Device* dev = nullptr;

        if (g_capturing && ordinal > 0 && ordinal < kMaxOrd &&
            g_countdown == 3) {
            // Still the capture frame: window this draw's own data.
            ctx->GetDevice(&dev);
            if (dev) {
                Ord& o = g_ord[ordinal];
                o.startInstance = startInstance;
                o.baseVertex = baseVertex;
                o.rtPtr = rtResource(ctx);
                ID3D11Buffer* pool = nullptr;
                UINT stride = 0, off = 0;
                ctx->IAGetVertexBuffers(1, 1, &pool, &stride, &off);
                if (pool) {
                    o.stVerts = stageRange(
                        ctx, dev, pool,
                        off + static_cast<uint32_t>(baseVertex) *
                                  (stride ? stride : 40u),
                        4 * (stride ? stride : 40u));
                    pool->Release();
                }
                // The OTHER eye's constants: the first draw painting a
                // DIFFERENT target than ordinal 0's. Draw order cannot
                // say this -- prepass and colour pass share an eye.
                if (!g_stCb0B && o.rtPtr && o.rtPtr != g_ord[0].rtPtr) {
                    ID3D11Buffer* cb0 = nullptr;
                    ctx->VSGetConstantBuffers(0, 1, &cb0);
                    if (cb0) {
                        g_stCb0B = stageRange(ctx, dev, cb0, 0, 192);
                        cb0->Release();
                    }
                }
                if (ordinal + 1 > g_ordCount) g_ordCount = ordinal + 1;
                dev->Release();
            }
            return;
        }

        if (ordinal != 0) return;   // countdown and readback ride ordinal 0

        if (g_countdown > 0) {
            g_capturing = false;
            --g_countdown;
            return;
        }
        if (g_countdown < 0) {
            // Idle: this frame becomes the capture frame.
            ctx->GetDevice(&dev);
            if (!dev) return;
            releaseAll();
            g_ord[0].startInstance = startInstance;
            g_ord[0].baseVertex = baseVertex;
            g_ord[0].rtPtr = rtResource(ctx);
            g_ordCount = 1;
            bool queued = true;

            ID3D11Buffer* cb0 = nullptr;
            ctx->VSGetConstantBuffers(0, 1, &cb0);
            if (cb0) {
                g_stCb0A = stageRange(ctx, dev, cb0, 0, 192);
                cb0->Release();
            }
            queued = queued && g_stCb0A != nullptr;

            ID3D11Buffer* cb1 = nullptr;
            ctx->VSGetConstantBuffers(1, 1, &cb1);
            if (cb1) {
                g_stCb1 = stageRange(ctx, dev, cb1, 268u * 16u, 144);
                cb1->Release();
            }
            queued = queued && g_stCb1 != nullptr;

            ID3D11Buffer* vb0 = nullptr;
            UINT stride = 0, off = 0;
            ctx->IAGetVertexBuffers(0, 1, &vb0, &stride, &off);
            if (vb0) {
                g_vb0Stride = stride ? stride : 8u;
                g_vb0Offset = off;
                g_stVb0 = stageWhole(ctx, dev, vb0, &g_vb0Bytes);
                vb0->Release();
            }
            queued = queued && g_stVb0 != nullptr;

            ID3D11Buffer* pool = nullptr;
            UINT pStride = 0, pOff = 0;
            ctx->IAGetVertexBuffers(1, 1, &pool, &pStride, &pOff);
            if (pool) {
                g_ord[0].stVerts = stageRange(
                    ctx, dev, pool,
                    pOff + static_cast<uint32_t>(baseVertex) *
                               (pStride ? pStride : 40u),
                    4 * (pStride ? pStride : 40u));
                pool->Release();
            }
            queued = queued && g_ord[0].stVerts != nullptr;

            ID3D11ShaderResourceView* srv = nullptr;
            ctx->VSGetShaderResources(33, 1, &srv);
            if (srv) {
                D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                srv->GetDesc(&vd);
                g_t33First = 0;
                if (vd.ViewDimension == D3D11_SRV_DIMENSION_BUFFER) {
                    g_t33First = vd.Buffer.FirstElement;
                } else if (vd.ViewDimension ==
                           D3D11_SRV_DIMENSION_BUFFEREX) {
                    g_t33First = vd.BufferEx.FirstElement;
                }
                ID3D11Resource* res = nullptr;
                srv->GetResource(&res);
                srv->Release();
                ID3D11Buffer* buf = nullptr;
                if (res) {
                    res->QueryInterface(__uuidof(ID3D11Buffer),
                                        reinterpret_cast<void**>(&buf));
                    res->Release();
                }
                if (buf) {
                    D3D11_BUFFER_DESC bd{};
                    buf->GetDesc(&bd);
                    g_t33Stride = bd.StructureByteStride
                                      ? bd.StructureByteStride
                                      : 336u;
                    g_stT33 = stageWhole(ctx, dev, buf, &g_t33Bytes);
                    buf->Release();
                }
            }
            queued = queued && g_stT33 != nullptr;

            dev->Release();
            if (queued) {
                g_capturing = true;
                g_countdown = 3;
            } else {
                releaseAll();
            }
            return;
        }

        // countdown == 0 at ordinal 0: the readback.
        g_countdown = -1;
        g_capturing = false;

        uint8_t cb0A[192] = {}, cb0B[192] = {}, cb1buf[144] = {};
        const bool haveA = mapRead(ctx, g_stCb0A, cb0A, sizeof(cb0A));
        const bool haveB = mapRead(ctx, g_stCb0B, cb0B, sizeof(cb0B));
        bool ok = haveA && mapRead(ctx, g_stCb1, cb1buf, sizeof(cb1buf));
        float rebase[4] = {};
        if (ok) memcpy(rebase, cb1buf + 112, sizeof(rebase));

        // Every ordinal's record and mesh, classified.
        struct Quad {
            float scale = 0.0f;
            float q[4] = {};
            float pos[3] = {};
            float minX = 0, maxX = 0, minY = 0, maxY = 0, z = 0;
            bool screen = false;
            bool valid = false;
        };
        Quad quads[kMaxOrd];
        uint8_t* vb0all = nullptr;
        uint8_t* t33all = nullptr;
        D3D11_MAPPED_SUBRESOURCE mv{}, mt{};
        if (ok && g_stVb0 &&
            SUCCEEDED(ctx->Map(g_stVb0, 0, D3D11_MAP_READ, 0, &mv))) {
            vb0all = static_cast<uint8_t*>(mv.pData);
        }
        if (ok && g_stT33 &&
            SUCCEEDED(ctx->Map(g_stT33, 0, D3D11_MAP_READ, 0, &mt))) {
            t33all = static_cast<uint8_t*>(mt.pData);
        }
        ok = ok && vb0all && t33all;

        uint32_t screenCount = 0;
        for (uint32_t i = 0; ok && i < g_ordCount && i < kMaxOrd; ++i) {
            Quad& qd = quads[i];
            // A quad draws twice per eye (prepass + colour): count each
            // instance entry once for the family and the union.
            bool dup = false;
            for (uint32_t k = 0; k < i; ++k) {
                if (g_ord[k].startInstance == g_ord[i].startInstance) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;
            const uint32_t win =
                g_vb0Offset + g_ord[i].startInstance * g_vb0Stride;
            if (win + 4 > g_vb0Bytes) continue;
            uint32_t instX;
            memcpy(&instX, vb0all + win, 4);
            const uint32_t recOff = (g_t33First + instX) * g_t33Stride;
            if (recOff + 28 > g_t33Bytes) continue;
            const uint8_t* r = t33all + recOff;
            uint32_t quatXY, quatZW;
            memcpy(&qd.scale, r + 4, 4);
            memcpy(&quatXY, r + 8, 4);
            memcpy(&quatZW, r + 12, 4);
            memcpy(qd.pos, r + 16, 12);
            qd.q[0] = (quatXY & 0xFFFFu) * 0.000031f - 1.0f;
            qd.q[1] = (quatXY >> 16u) * 0.000031f - 1.0f;
            qd.q[2] = (quatZW & 0xFFFFu) * 0.000031f - 1.0f;
            qd.q[3] = (quatZW >> 16u) * 0.000031f - 1.0f;
            if (!g_ord[i].stVerts) continue;
            uint8_t verts[160] = {};
            if (!mapRead(ctx, g_ord[i].stVerts, verts,
                         4 * (g_vb0Stride == 8 ? 40u : 40u))) {
                continue;
            }
            bool first = true;
            bool meshOk = true;
            for (int vtx = 0; vtx < 4 && meshOk; ++vtx) {
                uint32_t pva[4];
                memcpy(pva, verts + vtx * 40, 16);
                float m[3];
                meshOk = decodePos(pva, m);
                if (!meshOk) break;
                if (first || m[0] < qd.minX) qd.minX = m[0];
                if (first || m[0] > qd.maxX) qd.maxX = m[0];
                if (first || m[1] < qd.minY) qd.minY = m[1];
                if (first || m[1] > qd.maxY) qd.maxY = m[1];
                if (first) qd.z = m[2];
                first = false;
            }
            if (!meshOk || qd.z <= 1.0f) continue;
            const float qn = qd.q[0] * qd.q[0] + qd.q[1] * qd.q[1] +
                             qd.q[2] * qd.q[2] + qd.q[3] * qd.q[3];
            if (qd.scale < 0.1f || qd.scale > 4.0f || qn < 0.8f ||
                qn > 1.3f) {
                continue;
            }
            qd.valid = true;
            const float pmag = sqrtf(qd.pos[0] * qd.pos[0] +
                                     qd.pos[1] * qd.pos[1] +
                                     qd.pos[2] * qd.pos[2]);
            qd.screen = pmag < 0.1f;
            if (qd.screen) ++screenCount;
        }
        if (vb0all) ctx->Unmap(g_stVb0, 0);
        if (t33all) ctx->Unmap(g_stT33, 0);

        ok = ok && screenCount >= 1;

        // The screen family's union, perspective-normalised to the
        // farthest family z, projected through the LEFT eye's rows --
        // identified by where the family centre lands (nasal of image
        // centre only in the left eye).
        float corners[16] = {};
        uint32_t mask = 0;
        if (ok) {
            float refZ = 0.0f, refScale = 1.0f;
            float refQ[4] = {}, refPos[3] = {};
            for (uint32_t i = 0; i < g_ordCount; ++i) {
                if (quads[i].valid && quads[i].screen &&
                    quads[i].z * quads[i].scale > refZ) {
                    refZ = quads[i].z * quads[i].scale;
                    refScale = quads[i].scale;
                    memcpy(refQ, quads[i].q, sizeof(refQ));
                    memcpy(refPos, quads[i].pos, sizeof(refPos));
                }
            }
            float uMinX = 0, uMaxX = 0, uMinY = 0, uMaxY = 0;
            bool first = true;
            for (uint32_t i = 0; i < g_ordCount; ++i) {
                const Quad& qd = quads[i];
                if (!qd.valid) continue;
                if (!qd.screen) {
                    if (i < 32) mask |= 1u << i;
                    continue;
                }
                const float zi = qd.z * qd.scale;
                const float norm = refZ / zi;
                const float nMinX = qd.minX * qd.scale * norm;
                const float nMaxX = qd.maxX * qd.scale * norm;
                const float nMinY = qd.minY * qd.scale * norm;
                const float nMaxY = qd.maxY * qd.scale * norm;
                // An oversized member is the translucent BACKDROP sheet,
                // not the screen (the field's screenshot showed sky above
                // the screen inside the crop): drawn, but not framed.
                if (nMaxX - nMinX > 2.2f * 164.7f ||
                    nMaxY - nMinY > 2.2f * 92.655f) {
                    continue;
                }
                if (first || nMinX < uMinX) uMinX = nMinX;
                if (first || nMaxX > uMaxX) uMaxX = nMaxX;
                if (first || nMinY < uMinY) uMinY = nMinY;
                if (first || nMaxY > uMaxY) uMaxY = nMaxY;
                first = false;
            }
            ok = !first;

            const float* rowsA = reinterpret_cast<const float*>(cb0A + 64);
            const float* rowsB =
                haveB ? reinterpret_cast<const float*>(cb0B + 64) : nullptr;
            const float* rowsL = rowsA;
            const float* rowsR = rowsB;
            if (ok) {
                float cw[3];
                quatRotate(refQ, 0.0f, 0.0f, refZ, &cw[0], &cw[1], &cw[2]);
                const float centre[3] = {cw[0] + refPos[0] - rebase[0],
                                         cw[1] + refPos[1] - rebase[1],
                                         cw[2] + refPos[2] - rebase[2]};
                float ua = 0.5f, va = 0.5f, ub = 0.5f, vb = 0.5f;
                const bool pa = project(rowsA, centre, &ua, &va);
                const bool pb =
                    rowsB ? project(rowsB, centre, &ub, &vb) : false;
                // For the SAME far point the left eye's u is always the
                // larger. Relative, pose-proof (the 45f lesson). Both
                // eyes' sets are published: the renderer stitches, each
                // eye clean on its temporal side (the nose-mask fix).
                if (pa && pb) {
                    rowsL = ua >= ub ? rowsA : rowsB;
                    rowsR = ua >= ub ? rowsB : rowsA;
                } else if (pb) {
                    rowsL = rowsB;
                    rowsR = nullptr;
                } else {
                    rowsR = nullptr;
                }
                ok = pa || pb;
                g_pickU[0] = ua;
                g_pickU[1] = ub;
                g_pickLeftIsA = rowsL == rowsA;
            }
            if (ok) {
                const float cx[5] = {uMinX, uMaxX, uMaxX, uMinX, 0.0f};
                const float cy[5] = {uMaxY, uMaxY, uMinY, uMinY, 0.0f};
                float w5[5][3];
                for (int i = 0; i < 5; ++i) {
                    float w[3];
                    quatRotate(refQ, cx[i], cy[i], refZ, &w[0], &w[1],
                               &w[2]);
                    w5[i][0] = w[0] + refPos[0] - rebase[0];
                    w5[i][1] = w[1] + refPos[1] - rebase[1];
                    w5[i][2] = w[2] + refPos[2] - rebase[2];
                }
                float cu[5], cv[5];
                for (int i = 0; i < 5 && ok; ++i) {
                    ok = project(rowsL, w5[i], &cu[i], &cv[i]);
                }
                if (ok) {
                    for (int i = 0; i < 4; ++i) {
                        corners[i * 2 + 0] = cu[i];
                        corners[i * 2 + 1] = cv[i];
                        ok = ok && cu[i] > -0.5f && cu[i] < 1.5f &&
                             cv[i] > -0.5f && cv[i] < 1.5f;
                    }
                    ok = ok && corners[2] - corners[0] > 0.2f &&
                         corners[7] - corners[1] > 0.1f;
                }
                // The RIGHT eye's set, zeroed when unavailable -- the
                // renderer then samples the left eye alone.
                if (ok && rowsR) {
                    bool okR = true;
                    float ru[4], rv[4];
                    for (int i = 0; i < 4 && okR; ++i) {
                        okR = project(rowsR, w5[i], &ru[i], &rv[i]);
                    }
                    if (okR) {
                        for (int i = 0; i < 4; ++i) {
                            corners[8 + i * 2 + 0] = ru[i];
                            corners[8 + i * 2 + 1] = rv[i];
                        }
                    }
                }
            }
        }

        const uint32_t ordCount = g_ordCount;
        releaseAll();

        if (ok) {
            publishFssPanelRect(corners);
            g_skipMask = mask;
            g_published = true;
            ++g_derives;
            Log::get().note(
                "fss panel rect: LIVE union of the screen family -- %u "
                "quads, %u screen, scenery mask 0x%X; eye pick uA=%.3f "
                "uB=%.3f -> %s; TL (%.4f,%.4f) TR "
                "(%.4f,%.4f) BR (%.4f,%.4f) BL (%.4f,%.4f) (%u this "
                "session).",
                ordCount, screenCount, mask,
                static_cast<double>(g_pickU[0]),
                static_cast<double>(g_pickU[1]),
                g_pickLeftIsA ? "A(first)" : "B(second)",
                static_cast<double>(corners[0]),
                static_cast<double>(corners[1]),
                static_cast<double>(corners[2]),
                static_cast<double>(corners[3]),
                static_cast<double>(corners[4]),
                static_cast<double>(corners[5]),
                static_cast<double>(corners[6]),
                static_cast<double>(corners[7]), g_derives);
        } else if (!g_failNoted) {
            g_failNoted = true;
            Log::get().note(
                "fss panel rect: the capture frame did not decode to a "
                "plausible screen family; the theater keeps its centred "
                "band and no draw is skipped. Said once.");
        }
    });
}

void fssPanelRectShutdown() { releaseAll(); }

}  // namespace edvr

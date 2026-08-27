#include "fss_panel_rect.h"

#include <cstring>

#include <windows.h>

#include <d3d11.h>

#include "../common/frame_flag.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "device_hook.h"      // deviceHookFssModeLatch
#include "fss_panel_probe.h"  // fssPanelProbeStartInstance

namespace edvr {
namespace {

// The frame quad's mesh corners (probe flights, 2026-08-27): the one
// thing safely constant -- geometry, not state. Everything else -- the
// record's scale, quaternion and position, the rebase origin, the view
// rows -- is read LIVE at the derive composite, because the hardcoded
// quaternion of round 45d turned out to drift against the drawn quad
// (the field saw a right-side gap widening toward the bottom: a yaw
// plus roll mismatch).
constexpr float kHalfW = 164.700f;
constexpr float kHalfH = 92.655f;
constexpr float kDist = 180.000f;

ID3D11Buffer* g_stCb0 = nullptr;
ID3D11Buffer* g_stCb1 = nullptr;
ID3D11Buffer* g_stVb0 = nullptr;
ID3D11Buffer* g_stT33 = nullptr;
uint32_t g_t33First = 0;
uint32_t g_t33Stride = 336;
uint32_t g_t33Bytes = 0;
int  g_countdown = -1;
bool g_published = false;
bool g_failNoted = false;
uint32_t g_derives = 0;

FaultBudget g_budget("fssPanelRect", 8);

void releaseStagings() {
    for (ID3D11Buffer** b : {&g_stCb0, &g_stCb1, &g_stVb0, &g_stT33}) {
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

// The engine's own quaternion rotation, unnormalised on purpose -- the
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

}  // namespace

void fssPanelRectOnComposite(ID3D11DeviceContext* ctx) {
    if (!ctx) return;
    if (!deviceHookFssModeLatch()) {
        g_published = false;
        g_countdown = -1;
        releaseStagings();
        return;
    }
    if (g_published) return;

    guardedBudget(g_budget, [&] {
        if (g_countdown > 0) {
            --g_countdown;
            return;
        }
        if (g_countdown == 0) {
            g_countdown = -1;
            float rows[16] = {};
            float rebase[4] = {};
            uint32_t instPair[2] = {};
            uint8_t cb0buf[192] = {};
            uint8_t cb1buf[144] = {};
            bool ok = mapRead(ctx, g_stCb0, cb0buf, sizeof(cb0buf)) &&
                      mapRead(ctx, g_stCb1, cb1buf, sizeof(cb1buf)) &&
                      mapRead(ctx, g_stVb0, instPair, sizeof(instPair));
            if (ok) {
                memcpy(rows, cb0buf + 64, sizeof(rows));
                memcpy(rebase, cb1buf + 112, sizeof(rebase));
            }
            float scale = 0.0f, q[4] = {}, pos[3] = {};
            if (ok && g_stT33) {
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(ctx->Map(g_stT33, 0, D3D11_MAP_READ, 0, &m)) &&
                    m.pData) {
                    const uint32_t recOff =
                        (g_t33First + instPair[0]) * g_t33Stride;
                    if (recOff + 28 <= g_t33Bytes) {
                        const uint8_t* r =
                            static_cast<const uint8_t*>(m.pData) + recOff;
                        uint32_t quatXY, quatZW;
                        memcpy(&scale, r + 4, 4);
                        memcpy(&quatXY, r + 8, 4);
                        memcpy(&quatZW, r + 12, 4);
                        memcpy(pos, r + 16, 12);
                        q[0] = (quatXY & 0xFFFFu) * 0.000031f - 1.0f;
                        q[1] = (quatXY >> 16u) * 0.000031f - 1.0f;
                        q[2] = (quatZW & 0xFFFFu) * 0.000031f - 1.0f;
                        q[3] = (quatZW >> 16u) * 0.000031f - 1.0f;
                    } else {
                        ok = false;
                    }
                    ctx->Unmap(g_stT33, 0);
                } else {
                    ok = false;
                }
            } else {
                ok = false;
            }
            releaseStagings();

            // A live record that is not a screen-shaped transform means
            // the pool moved under us; publish nothing, keep the band.
            const float qn = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] +
                             q[3] * q[3];
            ok = ok && scale > 0.5f && scale < 2.0f && qn > 0.8f &&
                 qn < 1.3f &&
                 pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2] <
                     25.0f;

            float cu[5], cv[5];
            if (ok) {
                const float cx[5] = {-kHalfW, kHalfW, kHalfW, -kHalfW, 0.0f};
                const float cy[5] = {kHalfH, kHalfH, -kHalfH, -kHalfH, 0.0f};
                for (int i = 0; i < 5 && ok; ++i) {
                    float wx, wy, wz;
                    quatRotate(q, cx[i] * scale, cy[i] * scale,
                               kDist * scale, &wx, &wy, &wz);
                    const float p[4] = {wx + pos[0] - rebase[0],
                                        wy + pos[1] - rebase[1],
                                        wz + pos[2] - rebase[2], 1.0f};
                    float clip[4];
                    for (int r = 0; r < 4; ++r) {
                        clip[r] = rows[r * 4 + 0] * p[0] +
                                  rows[r * 4 + 1] * p[1] +
                                  rows[r * 4 + 2] * p[2] + rows[r * 4 + 3];
                    }
                    if (clip[3] < 1e-3f) {
                        ok = false;
                        break;
                    }
                    cu[i] = (clip[0] / clip[3] + 1.0f) * 0.5f;
                    cv[i] = (1.0f - clip[1] / clip[3]) * 0.5f;
                }
            }
            float corners[8] = {};
            if (ok) {
                // Shrink about the projected centre until every corner is
                // inside the rendered eye -- content past the edge was
                // never drawn (45b's black strip on the right).
                float f = 0.998f;
                for (int i = 0; i < 4; ++i) {
                    const float du = cu[i] - cu[4];
                    const float dv = cv[i] - cv[4];
                    if (du > 0 && (0.998f - cu[4]) / du < f) {
                        f = (0.998f - cu[4]) / du;
                    }
                    if (du < 0 && (0.002f - cu[4]) / du < f) {
                        f = (0.002f - cu[4]) / du;
                    }
                    if (dv > 0 && (0.998f - cv[4]) / dv < f) {
                        f = (0.998f - cv[4]) / dv;
                    }
                    if (dv < 0 && (0.002f - cv[4]) / dv < f) {
                        f = (0.002f - cv[4]) / dv;
                    }
                }
                ok = f > 0.5f;
                for (int i = 0; i < 4 && ok; ++i) {
                    corners[i * 2 + 0] = cu[4] + (cu[i] - cu[4]) * f;
                    corners[i * 2 + 1] = cv[4] + (cv[i] - cv[4]) * f;
                }
            }
            if (ok) {
                const float w1 = corners[2] - corners[0];
                const float h1 = corners[7] - corners[1];
                ok = w1 > 0.2f && h1 > 0.1f;
            }
            if (ok) {
                publishFssPanelRect(corners);
                g_published = true;
                ++g_derives;
                Log::get().note(
                    "fss panel rect: corners derived LIVE for this engage "
                    "-- record scale %.3f quat (%.4f,%.4f,%.4f,%.4f) pos "
                    "(%.3f,%.3f,%.3f); TL (%.4f,%.4f) TR (%.4f,%.4f) BR "
                    "(%.4f,%.4f) BL (%.4f,%.4f) (%u this session).",
                    static_cast<double>(scale), static_cast<double>(q[0]),
                    static_cast<double>(q[1]), static_cast<double>(q[2]),
                    static_cast<double>(q[3]), static_cast<double>(pos[0]),
                    static_cast<double>(pos[1]),
                    static_cast<double>(pos[2]),
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
                    "fss panel rect: the live record or its projection "
                    "was not a plausible screen; the theater keeps its "
                    "centred-band crop. Said once.");
            }
            return;
        }

        // Idle: queue the same-frame snapshot at this composite -- the
        // survey's lesson: the pool churns, only same-frame data coheres.
        // cb0, the cb1 rebase row, the instance-stream pair this draw
        // fetches, and the whole record pool.
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return;
        bool queued = true;

        ID3D11Buffer* cb0 = nullptr;
        ctx->VSGetConstantBuffers(0, 1, &cb0);
        if (cb0) {
            g_stCb0 = stageRange(ctx, dev, cb0, 0, 192);
            cb0->Release();
        }
        queued = queued && g_stCb0 != nullptr;

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
            const uint32_t win =
                off + fssPanelProbeStartInstance() * (stride ? stride : 8u);
            g_stVb0 = stageRange(ctx, dev, vb0, win, 8);
            vb0->Release();
        }
        queued = queued && g_stVb0 != nullptr;

        ID3D11ShaderResourceView* srv = nullptr;
        ctx->VSGetShaderResources(33, 1, &srv);
        if (srv) {
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            srv->GetDesc(&vd);
            g_t33First = 0;
            if (vd.ViewDimension == D3D11_SRV_DIMENSION_BUFFER) {
                g_t33First = vd.Buffer.FirstElement;
            } else if (vd.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX) {
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
                g_t33Stride =
                    bd.StructureByteStride ? bd.StructureByteStride : 336u;
                g_t33Bytes = bd.ByteWidth;
                D3D11_BUFFER_DESC d{};
                d.ByteWidth = bd.ByteWidth;
                d.Usage = D3D11_USAGE_STAGING;
                d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (SUCCEEDED(dev->CreateBuffer(&d, nullptr, &g_stT33)) &&
                    g_stT33) {
                    ctx->CopyResource(g_stT33, buf);
                }
                buf->Release();
            }
        }
        queued = queued && g_stT33 != nullptr;

        dev->Release();
        if (queued) {
            g_countdown = 3;
        } else {
            releaseStagings();
        }
    });
}

void fssPanelRectShutdown() { releaseStagings(); }

}  // namespace edvr

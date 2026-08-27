#include "fss_panel_rect.h"

#include <cstring>

#include <windows.h>

#include <d3d11.h>

#include "../common/frame_flag.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "device_hook.h"   // deviceHookFssModeLatch

namespace edvr {
namespace {

// The frame quad's measured model geometry (probe flights, 2026-08-27):
// corners +-kHalfW x +-kHalfH at z = kDist, identity orientation, origin
// position. The rebase origin (centimetres) and the quaternion's
// quantisation tilt (sub-degree) are ignored on purpose.
// The OUTER frame quad: the display-only crop cut the UI boxes painted
// between the display and the frame extents (field, third flight). The
// black-strip lesson rides in fitShrink below, and the tilt in the quat.
constexpr float kHalfW = 164.700f;
constexpr float kHalfH = 92.655f;
constexpr float kDist = 180.000f;

// The record's orientation as the GAME decodes it: raw 0x7FFF/0xFFFF
// through the engine's own asymmetric unorm scale -- NOT identity, and
// not normalised. This ~3-degree compound tilt is why the screen leaned
// in the axis-aligned crop; projecting with the same numbers the shader
// uses puts our corners exactly on the drawn quad.
constexpr float kQx = 0.015777f;
constexpr float kQy = 0.015777f;
constexpr float kQz = 0.015777f;
constexpr float kQw = 1.031585f;

// p' = (2w^2 - 1) p + 2 (q.p) q + 2 w (q x p), the game's expansion,
// unnormalised on purpose.
void quatRotate(float px, float py, float pz, float* ox, float* oy,
                float* oz) {
    const float cxv = kQy * pz - py * kQz;
    const float cyv = kQz * px - pz * kQx;
    const float czv = kQx * py - px * kQy;
    const float d = kQx * px + kQy * py + kQz * pz;
    const float t = kQw * (kQw + kQw);
    *ox = t * px - px + d * (kQx + kQx) + cxv * (kQw + kQw);
    *oy = t * py - py + d * (kQy + kQy) + cyv * (kQw + kQw);
    *oz = t * pz - pz + d * (kQz + kQz) + czv * (kQw + kQw);
}

ID3D11Buffer* g_stCb = nullptr;
int  g_countdown = -1;      // >=0: copies queued, counting down to readback
bool g_published = false;   // this engage's rect is out; idle until reset
bool g_failNoted = false;
uint32_t g_derives = 0;

FaultBudget g_budget("fssPanelRect", 8);

}  // namespace

void fssPanelRectOnComposite(ID3D11DeviceContext* ctx) {
    if (!ctx) return;
    if (!deviceHookFssModeLatch()) {
        // The engage ended; the next one derives afresh.
        g_published = false;
        g_countdown = -1;
        if (g_stCb) { g_stCb->Release(); g_stCb = nullptr; }
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
            bool got = false;
            if (g_stCb) {
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(ctx->Map(g_stCb, 0, D3D11_MAP_READ, 0, &m)) &&
                    m.pData) {
                    // cb0 rows 4..7 = bytes 64..127 of the 192-byte copy.
                    memcpy(rows, static_cast<const uint8_t*>(m.pData) + 64,
                           sizeof(rows));
                    ctx->Unmap(g_stCb, 0);
                    got = true;
                }
                g_stCb->Release();
                g_stCb = nullptr;
            }
            if (!got) return;
            // Corner order TL,TR,BR,BL in model space (+y up).
            const float cx[4] = {-kHalfW, kHalfW, kHalfW, -kHalfW};
            const float cy[4] = {kHalfH, kHalfH, -kHalfH, -kHalfH};
            float cu[5], cv[5];   // 4 corners + the centre
            bool ok = true;
            for (int i = 0; i < 5 && ok; ++i) {
                float wx, wy, wz;
                if (i < 4) {
                    quatRotate(cx[i], cy[i], kDist, &wx, &wy, &wz);
                } else {
                    quatRotate(0.0f, 0.0f, kDist, &wx, &wy, &wz);
                }
                const float p[4] = {wx, wy, wz, 1.0f};
                float clip[4];
                for (int r = 0; r < 4; ++r) {
                    clip[r] = rows[r * 4 + 0] * p[0] +
                              rows[r * 4 + 1] * p[1] +
                              rows[r * 4 + 2] * p[2] + rows[r * 4 + 3];
                }
                if (clip[3] < 1e-3f) {
                    ok = false;   // a corner behind the eye: not our screen
                    break;
                }
                cu[i] = (clip[0] / clip[3] + 1.0f) * 0.5f;
                cv[i] = (1.0f - clip[1] / clip[3]) * 0.5f;
            }
            float corners[8] = {};
            if (ok) {
                // Shrink about the projected centre until every corner
                // sits inside the rendered eye -- content past the edge
                // was never drawn, and sampling it was the 45b black
                // strip on the right.
                float f = 0.998f;
                for (int i = 0; i < 4; ++i) {
                    const float du = cu[i] - cu[4];
                    const float dv = cv[i] - cv[4];
                    if (du > 0) f = f < (0.998f - cu[4]) / du ? f : (0.998f - cu[4]) / du;
                    if (du < 0) f = f < (0.002f - cu[4]) / du ? f : (0.002f - cu[4]) / du;
                    if (dv > 0) f = f < (0.998f - cv[4]) / dv ? f : (0.998f - cv[4]) / dv;
                    if (dv < 0) f = f < (0.002f - cv[4]) / dv ? f : (0.002f - cv[4]) / dv;
                }
                ok = f > 0.5f;   // needing to halve the screen is not a screen
                for (int i = 0; i < 4 && ok; ++i) {
                    corners[i * 2 + 0] = cu[4] + (cu[i] - cu[4]) * f;
                    corners[i * 2 + 1] = cv[4] + (cv[i] - cv[4]) * f;
                }
            }
            if (ok) {
                // Spans still plausible after the fit.
                const float w1 = corners[2] - corners[0];
                const float h1 = corners[7] - corners[1];
                ok = w1 > 0.2f && h1 > 0.1f;
            }
            if (ok) {
                publishFssPanelRect(corners);
                g_published = true;
                ++g_derives;
                Log::get().note(
                    "fss panel rect: corners derived for this engage -- TL "
                    "(%.4f,%.4f) TR (%.4f,%.4f) BR (%.4f,%.4f) BL "
                    "(%.4f,%.4f) (%u this session). The cinema screen "
                    "rectifies the scanner's screen quad.",
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
                    "fss panel rect: the projected corners were not a "
                    "plausible screen; the theater keeps its centred-band "
                    "crop. Said once.");
            }
            return;
        }

        // Idle: queue the cb0 copy at this composite.
        ID3D11Buffer* cb = nullptr;
        ctx->VSGetConstantBuffers(0, 1, &cb);
        if (!cb) return;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = 192;
            d.Usage = D3D11_USAGE_STAGING;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (SUCCEEDED(dev->CreateBuffer(&d, nullptr, &g_stCb)) &&
                g_stCb) {
                D3D11_BOX box{0, 0, 0, 192, 1, 1};
                ctx->CopySubresourceRegion(g_stCb, 0, 0, 0, 0, cb, 0, &box);
                g_countdown = 3;
            }
            dev->Release();
        }
        cb->Release();
    });
}

void fssPanelRectShutdown() {
    if (g_stCb) { g_stCb->Release(); g_stCb = nullptr; }
}

}  // namespace edvr

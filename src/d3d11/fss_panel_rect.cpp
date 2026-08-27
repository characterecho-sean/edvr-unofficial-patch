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
constexpr float kHalfW = 164.700f;
constexpr float kHalfH = 92.655f;
constexpr float kDist = 180.000f;

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
            const float cx[4] = {kHalfW, kHalfW, -kHalfW, -kHalfW};
            const float cy[4] = {kHalfH, -kHalfH, -kHalfH, kHalfH};
            float u0 = 2.0f, u1 = -1.0f, v0 = 2.0f, v1 = -1.0f;
            bool ok = true;
            for (int i = 0; i < 4 && ok; ++i) {
                const float p[4] = {cx[i], cy[i], kDist, 1.0f};
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
                const float u = (clip[0] / clip[3] + 1.0f) * 0.5f;
                const float v = (1.0f - clip[1] / clip[3]) * 0.5f;
                if (u < u0) u0 = u;
                if (u > u1) u1 = u;
                if (v < v0) v0 = v;
                if (v > v1) v1 = v;
            }
            // Sanity: a plausible screen spans a real fraction of the eye
            // and overlaps it. Anything else publishes nothing.
            ok = ok && u1 - u0 > 0.2f && u1 - u0 < 1.6f &&
                 v1 - v0 > 0.1f && v1 - v0 < 1.2f && u1 > 0.05f &&
                 u0 < 0.95f && v1 > 0.05f && v0 < 0.95f;
            if (ok) {
                publishFssPanelRect(u0, v0, u1, v1);
                g_published = true;
                ++g_derives;
                Log::get().note(
                    "fss panel rect: derived for this engage -- u "
                    "[%.4f..%.4f] v [%.4f..%.4f] (%u this session). The "
                    "cinema screen crops to exactly the scanner's screen.",
                    static_cast<double>(u0), static_cast<double>(u1),
                    static_cast<double>(v0), static_cast<double>(v1),
                    g_derives);
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

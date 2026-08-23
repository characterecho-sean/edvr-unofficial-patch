#include "panel_quad.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/timing.h"
#include "binding_shadow.h"

namespace edvr {
namespace {

// The census measured 80 bytes. The cap is generous against that and still
// refuses anything that could only be a pooled buffer -- which would not be
// this draw's quad, and copying which would be a real cost rather than a
// rounding error.
constexpr uint32_t kMaxBytes = 4096;

// A few frames at any refresh rate. Mapping a staging buffer in the same
// frame the copy was queued blocks the CPU until the GPU drains, which is a
// stall on the render thread for a diagnostic -- the exact trade cb_peek's
// pool sampler already made and named.
constexpr uint64_t kReadbackLagMs = 50;

// Two floats are "the same corner value" within this. The quad's positions
// are exact constants in the asset, so anything looser is not needed and
// anything tighter would fail on a legitimate -1.0f against -0.99999994f.
constexpr float kEps = 1e-4f;

bool          g_wanted = false;      // config says take a capture
bool          g_pending = false;     // copy queued, readback owed
uint64_t      g_copyMs = 0;
ID3D11Buffer* g_staging = nullptr;   // owned
uint32_t      g_stagingBytes = 0;    // what it was CREATED at, which is not
                                     // g_bytes: CopyResource refuses two
                                     // buffers of different sizes, so a
                                     // re-arm that meets a different quad has
                                     // to build a new one rather than copy
                                     // into the old
bool          g_lastOn = false;      // config's previous answer, so arming is
                                     // edge-triggered: the ini is re-read
                                     // whenever its write time moves, and a
                                     // level-triggered arm would fire again
                                     // on any unrelated edit
uint32_t      g_bytes = 0;           // what was copied
uint32_t      g_stride = 0;
uint32_t      g_offset = 0;

FaultBudget g_budget("panelQuad.capture", 5);

// One vertex's floats as text, and the same bytes as hex.
//
// The hex is not redundant. A layout that is not floats at all -- packed
// normals, half-precision UVs, a colour -- reads as garbage in the float
// column and as something recognisable in the hex one, and the whole point
// of this instrument is to stay readable when the guess is wrong.
void logVertex(uint32_t index, const unsigned char* v, uint32_t stride) {
    char floats[160] = "";
    char hex[160] = "";
    uint32_t fo = 0, ho = 0;
    for (uint32_t b = 0; b + 4 <= stride && b < 40; b += 4) {
        float f = 0.0f;
        uint32_t raw = 0;
        memcpy(&f, v + b, sizeof(f));
        memcpy(&raw, v + b, sizeof(raw));
        const int nf = _snprintf_s(floats + fo, sizeof(floats) - fo, _TRUNCATE,
                                   "%s%.4f", fo ? " " : "", f);
        const int nh = _snprintf_s(hex + ho, sizeof(hex) - ho, _TRUNCATE,
                                   "%s%08X", ho ? " " : "", raw);
        if (nf < 0 || nh < 0) break;
        fo += static_cast<uint32_t>(nf);
        ho += static_cast<uint32_t>(nh);
    }
    Log::get().note("panel quad: v%u  %s   [%s]", index, floats, hex);
}

// What the four vertices say about the format, as separate checks rather
// than one verdict. Three facts somebody can read beat a "yes" about a
// layout nobody has.
void describeLayout(const unsigned char* data, uint32_t stride, uint32_t count) {
    if (stride != 20 || count != 4) {
        Log::get().note(
            "panel quad: %u vertices of %u bytes is not the 4x20 the census "
            "measured, so no position/UV reading is offered -- the floats and hex "
            "above are the whole of what was found.",
            count, stride);
        return;
    }

    float pos[4][3], uv[4][2];
    for (uint32_t i = 0; i < 4; ++i) {
        memcpy(pos[i], data + i * stride, sizeof(pos[i]));
        memcpy(uv[i], data + i * stride + 12, sizeof(uv[i]));
    }

    bool uvInRange = true;
    for (uint32_t i = 0; i < 4; ++i) {
        for (uint32_t k = 0; k < 2; ++k) {
            if (uv[i][k] < -kEps || uv[i][k] > 1.0f + kEps) uvInRange = false;
        }
    }

    // One plane, and two distinct values on each of the other two axes: that
    // is what "a rectangle" has to mean for a quad whose bend will be
    // computed in this same space.
    bool flatZ = true;
    for (uint32_t i = 1; i < 4; ++i) {
        if (fabsf(pos[i][2] - pos[0][2]) > kEps) flatZ = false;
    }
    uint32_t distinctX = 0, distinctY = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        bool seenX = false, seenY = false;
        for (uint32_t j = 0; j < i; ++j) {
            if (fabsf(pos[i][0] - pos[j][0]) <= kEps) seenX = true;
            if (fabsf(pos[i][1] - pos[j][1]) <= kEps) seenY = true;
        }
        if (!seenX) ++distinctX;
        if (!seenY) ++distinctY;
    }

    Log::get().note(
        "panel quad: read as float3 position + float2 UV -- UVs inside 0..1: %s. "
        "Positions in one plane (z constant at %.4f): %s. Two distinct x and two "
        "distinct y: %s (found %u and %u). All three yes means the format is "
        "understood and the curved grid can be built in this space; any no means "
        "the stride is 20 for some other reason and the hex above is what to read.",
        uvInRange ? "yes" : "NO", pos[0][2], flatZ ? "yes" : "NO",
        (distinctX == 2 && distinctY == 2) ? "yes" : "NO", distinctX, distinctY);

    // The two things a stride can never say, spelled out for whoever builds
    // the grid: where the first corner is, and which way V runs against Y.
    // Getting the second wrong renders the screen upside down, which is the
    // failure most likely to be mistaken for the bend being wrong.
    const bool vFallsAsYRises = (uv[0][1] < 0.5f) == (pos[0][1] > 0.0f);
    Log::get().note(
        "panel quad: v0 is at (%.4f, %.4f) with UV (%.4f, %.4f), so V %s as Y "
        "rises. The grid's UVs must follow that; the other convention renders "
        "the screen upside down, which reads as the bend being wrong.",
        pos[0][0], pos[0][1], uv[0][0], uv[0][1],
        vFallsAsYRises ? "falls" : "rises");
}

void readBack(ID3D11DeviceContext* ctx) {
    guardedBudget(g_budget, [&] {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(g_staging, 0, D3D11_MAP_READ, 0, &m)) || !m.pData) {
            Log::get().note(
                "panel quad: the staging copy could not be mapped, so nothing was "
                "read. Set panel_quad_dump = 0 and back to 1 to try again.");
            return;
        }
        const unsigned char* data = static_cast<const unsigned char*>(m.pData);
        const uint32_t count = g_stride ? g_bytes / g_stride : 0;
        Log::get().note(
            "panel quad: the composite's vertex buffer is %u bytes at stride %u, "
            "stream offset %u -- %u vertices. This is the quad the curved screen "
            "replaces, and the format below is the one its grid has to be in.",
            g_bytes, g_stride, g_offset, count);
        for (uint32_t i = 0; i < count && i < 8; ++i) {
            logVertex(i, data + i * g_stride, g_stride);
        }
        describeLayout(data, g_stride, count);
        ctx->Unmap(g_staging, 0);
    });
    // One capture per arming, whether it parsed or not. A diagnostic that
    // keeps firing is one somebody turns off before reading it.
    g_wanted = false;
    g_pending = false;
}

void queueCopy(ID3D11DeviceContext* ctx) {
    guardedBudget(g_budget, [&] {
        ID3D11Buffer* vb = nullptr;
        UINT stride = 0, offset = 0;
        ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
        if (!vb) {
            Log::get().note(
                "panel quad: the composite has NO vertex buffer bound in slot 0, so "
                "its corners are synthesised in the shader and there is no quad to "
                "read. That is what the design calls Path B, and it changes the plan "
                "rather than this capture. Standing down.");
            g_wanted = false;
            return;
        }

        // Its size, through the one guarded resolver rather than a sixth
        // hand-written GetDesc.
        ResourceInfo info;
        const bool known = bindingResolveResource(vb, &info) && info.isBuffer;
        const uint32_t bytes = known ? info.a : 0;
        if (!known || !bytes || !stride || bytes > kMaxBytes) {
            Log::get().note(
                "panel quad: the bound vertex buffer is %u bytes at stride %u, which "
                "is not a quad this can read (the census measured 80 at 20, and "
                "anything over %u is a pooled buffer whose slice this cannot find). "
                "Standing down without copying.",
                bytes, stride, kMaxBytes);
            vb->Release();
            g_wanted = false;
            return;
        }

        if (g_staging && g_stagingBytes != bytes) {
            g_staging->Release();
            g_staging = nullptr;
            g_stagingBytes = 0;
        }
        if (!g_staging) {
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (!dev) {
                vb->Release();
                return;
            }
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = bytes;
            bd.Usage = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            dev->CreateBuffer(&bd, nullptr, &g_staging);
            dev->Release();
            if (!g_staging) {
                Log::get().note("panel quad: a %u-byte staging buffer could not be "
                                "created; standing down.",
                                bytes);
                vb->Release();
                g_wanted = false;
                return;
            }
            g_stagingBytes = bytes;
        }

        // The whole buffer, not a region: it is eighty bytes, and CopyResource
        // needs no box, no offset arithmetic, and gives neither a chance to be
        // wrong. The stream offset is recorded rather than applied, because a
        // quad at a non-zero offset would mean this is a pooled buffer after
        // all and the size check above would already have refused it.
        ctx->CopyResource(g_staging, vb);
        vb->Release();
        g_bytes = bytes;
        g_stride = stride;
        g_offset = offset;
        g_copyMs = nowMs();
        g_pending = true;
    });
}

}  // namespace

void panelQuadConfigure(Config& cfg) {
    const bool on = cfg.getBool("advanced.panel_quad_dump", false);
    // Edge-triggered, not level-triggered. The ini is re-read whenever its
    // write time moves, so a level-triggered arm would take a fresh capture
    // every time an unrelated key was edited -- and the second capture would
    // land in whatever mode the player happened to be in, under a log line
    // claiming it was asked for.
    if (on && !g_lastOn) {
        // Any state from a previous capture is dropped here, so a second one
        // cannot report the first one's sizes.
        g_bytes = g_stride = g_offset = 0;
        g_wanted = true;
        g_pending = false;
        Log::get().note(
            "panel quad: armed. The next panel composite's vertex buffer will be "
            "copied and read back a few frames later, once, and its vertices "
            "logged. Get on foot or into HMD Cinema Mode with the screen up.");
    }
    if (!on) {
        g_wanted = false;
        g_pending = false;
    }
    g_lastOn = on;
}

bool panelQuadWants() { return g_wanted || g_pending; }

void panelQuadOnComposite(ID3D11DeviceContext* ctx) {
    if (!ctx) return;
    if (g_pending) {
        if (g_staging && nowMs() - g_copyMs >= kReadbackLagMs) readBack(ctx);
        return;
    }
    if (g_wanted) queueCopy(ctx);
}

void panelQuadShutdown() {
    if (g_staging) {
        g_staging->Release();
        g_staging = nullptr;
    }
    g_stagingBytes = 0;
    g_wanted = false;
    g_pending = false;
    g_lastOn = false;
}

}  // namespace edvr

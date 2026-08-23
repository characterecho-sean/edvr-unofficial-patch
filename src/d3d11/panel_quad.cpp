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
#include "shader_sig.h"

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

// The index buffer and the cull state, which between them decide WINDING.
//
// The vertices cannot say it. The four corners arrive bottom-left,
// bottom-right, top-left, top-right -- a strip's order -- but the draw is a
// six-index triangle LIST, so which way each triangle turns lives in the
// index buffer and nowhere else. A grid generated the other way round is
// culled entirely and the screen goes black: a loud failure, but still a
// flight spent finding it. The cull state decides whether it matters at all,
// since a composite drawn with culling off does not care.
ID3D11Buffer* g_ibStaging = nullptr;   // owned
uint32_t      g_ibStagingBytes = 0;
uint32_t      g_ibBytes = 0;           // 0 when there was none to copy
uint32_t      g_ibFmt = 0;             // DXGI_FORMAT: 57 is R16_UINT, 42 R32
uint32_t      g_ibOffset = 0;
uint32_t      g_cullMode = 0;          // D3D11_CULL_MODE: 1 none, 2 front, 3 back
bool          g_frontCCW = false;
bool          g_rsDefault = false;     // no state object bound: D3D's defaults

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

        // What the shader actually READS of all that.
        //
        // The vertex data cannot say it: a third float that is 0.0 in all four
        // corners is what an unused field looks like as much as a flat one, and
        // that ambiguity sent the curvature work chasing a failure that was not
        // there. The DXBC input signature settles it -- used=xyz means the z a
        // substituted mesh writes is consumed, used=xy means it never could be.
        ID3D11VertexShader* vs = nullptr;
        ctx->VSGetShader(&vs, nullptr, nullptr);
        if (vs) {
            const char* sig = shaderSigOf(vs);
            vs->Release();
            if (sig) {
                Log::get().note(
                    "panel quad: the composite's vertex shader reads %s. What "
                    "matters is the POSITION entry: used=xyz means a substituted "
                    "mesh's z reaches the screen, used=xy means no geometry "
                    "substitution could ever bend it.",
                    sig);
            } else {
                Log::get().note(
                    "panel quad: the composite's vertex shader was created before "
                    "the hooks went in, or its signature would not parse, so what "
                    "it reads is not known from here.");
            }
        }

        // Winding, which the vertices cannot say on their own.
        if (g_ibBytes && g_ibStaging) {
            D3D11_MAPPED_SUBRESOURCE im{};
            if (SUCCEEDED(ctx->Map(g_ibStaging, 0, D3D11_MAP_READ, 0, &im)) && im.pData) {
                const unsigned char* ib = static_cast<const unsigned char*>(im.pData);
                const bool wide = g_ibFmt == 42;   // R32_UINT; 57 is R16_UINT
                const uint32_t n = g_ibBytes / (wide ? 4u : 2u);
                char idx[128] = "";
                uint32_t o = 0;
                for (uint32_t i = 0; i < n && i < 12; ++i) {
                    uint32_t v = 0;
                    if (wide) {
                        memcpy(&v, ib + i * 4, 4);
                    } else {
                        unsigned short h = 0;
                        memcpy(&h, ib + i * 2, 2);
                        v = h;
                    }
                    const int w = _snprintf_s(idx + o, sizeof(idx) - o, _TRUNCATE,
                                              "%s%u", o ? "," : "", v);
                    if (w < 0) break;
                    o += static_cast<uint32_t>(w);
                }
                Log::get().note(
                    "panel quad: indices %s (%u of them, %u-bit, buffer %u bytes at "
                    "offset %u). The grid's triangles have to turn the same way as "
                    "these two do.",
                    idx, n, wide ? 32u : 16u, g_ibBytes, g_ibOffset);
                ctx->Unmap(g_ibStaging, 0);
            }
        } else {
            Log::get().note(
                "panel quad: the index buffer was not readable -- none bound, or too "
                "large to be this draw's own -- so the winding is not known from here. "
                "The c = 0 substitution test settles it instead: the wrong winding "
                "renders nothing at all, which is unmistakable.");
        }

        const char* cull = g_cullMode == 1 ? "none"
                         : g_cullMode == 2 ? "front"
                                           : "back";
        Log::get().note(
            "panel quad: rasterizer cull = %s, front face is %s%s. Cull none would "
            "mean winding cannot matter at all; anything else and the grid has to "
            "match the indices above.",
            cull, g_frontCCW ? "counter-clockwise" : "clockwise",
            g_rsDefault ? " (no state object bound, so these are D3D's defaults)" : "");
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

        // The index buffer, same treatment. Failing to get it is not fatal --
        // the vertices are the point, and the c = 0 test can settle winding on
        // its own -- so every exit below leaves g_ibBytes at zero and the
        // capture carries on.
        g_ibBytes = 0;
        ID3D11Buffer* ib = nullptr;
        DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN;
        UINT ibOffset = 0;
        ctx->IAGetIndexBuffer(&ib, &ibFmt, &ibOffset);
        if (ib) {
            ResourceInfo ii;
            const bool iknown = bindingResolveResource(ib, &ii) && ii.isBuffer;
            // Small enough to be this draw's own. Locating a slice inside a
            // pooled index buffer would need the draw's startIndex, which is
            // not plumbed here and does not need to be: a quad with its own
            // eighty-byte vertex buffer does not share an index buffer.
            if (iknown && ii.a && ii.a <= kMaxBytes) {
                if (g_ibStaging && g_ibStagingBytes != ii.a) {
                    g_ibStaging->Release();
                    g_ibStaging = nullptr;
                    g_ibStagingBytes = 0;
                }
                if (!g_ibStaging) {
                    ID3D11Device* dev = nullptr;
                    ctx->GetDevice(&dev);
                    if (dev) {
                        D3D11_BUFFER_DESC bd{};
                        bd.ByteWidth = ii.a;
                        bd.Usage = D3D11_USAGE_STAGING;
                        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        dev->CreateBuffer(&bd, nullptr, &g_ibStaging);
                        dev->Release();
                        if (g_ibStaging) g_ibStagingBytes = ii.a;
                    }
                }
                if (g_ibStaging) {
                    ctx->CopyResource(g_ibStaging, ib);
                    g_ibBytes = ii.a;
                    g_ibFmt = static_cast<uint32_t>(ibFmt);
                    g_ibOffset = ibOffset;
                }
            }
            ib->Release();
        }

        // The cull state. No state object bound means D3D's documented
        // defaults -- back-face culling, clockwise front faces -- and the log
        // says which of the two it is reporting rather than leaving the reader
        // to assume.
        ID3D11RasterizerState* rs = nullptr;
        ctx->RSGetState(&rs);
        if (rs) {
            D3D11_RASTERIZER_DESC rd{};
            rs->GetDesc(&rd);
            g_cullMode = static_cast<uint32_t>(rd.CullMode);
            g_frontCCW = rd.FrontCounterClockwise != FALSE;
            g_rsDefault = false;
            rs->Release();
        } else {
            g_cullMode = static_cast<uint32_t>(D3D11_CULL_BACK);
            g_frontCCW = false;
            g_rsDefault = true;
        }

        g_copyMs = nowMs();
        g_pending = true;
    });
}

}  // namespace

void panelQuadConfigure(Config& cfg) {
    const bool on = false;   // retired instrument: the capture is done and in the docs
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
    if (g_ibStaging) {
        g_ibStaging->Release();
        g_ibStaging = nullptr;
    }
    g_stagingBytes = 0;
    g_ibStagingBytes = 0;
    g_wanted = false;
    g_pending = false;
    g_lastOn = false;
}

}  // namespace edvr

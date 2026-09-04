#include "depth_probe.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <windows.h>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/timing.h"
#include "shader_swap.h"
#include "temporal_pass.h"

namespace edvr {
namespace {

// The sampler: 256 depth values on a 16x16 grid across the target, read
// through a view typed to the depth channel. Load, not Sample -- a depth
// view has no filterable format, and the grid wants exact texels.
constexpr char kSampleCsHlsl[] = R"HLSL(
Texture2D<float> D : register(t0);
RWStructuredBuffer<float> O : register(u0);
cbuffer P : register(b0) { uint2 size; uint2 pad0; };
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= 16 || id.y >= 16) return;
    uint2 p = uint2((id.x * 2 + 1) * size.x / 32, (id.y * 2 + 1) * size.y / 32);
    p = min(p, size - 1);
    O[id.y * 16 + id.x] = D.Load(int3(int2(p), 0));
}
)HLSL";

constexpr int      kMaxTargets = 32;   // the cockpit binds shadow maps by the handful
constexpr uint64_t kSampleIntervalMs = 10000;
constexpr int      kMaxSampleLines = 6;

// The near plane the game names when it asks for its projection (0.025,
// every session's receiver line), for turning a reversed-Z value into
// metres: with an infinite far plane, value = near / z. Believed, not
// measured -- the probe prints the raw values beside it so the belief can
// be checked against a known distance.
constexpr double kNearPlaneM = 0.025;

struct Target {
    void*       dsv = nullptr;          // identity only; dereferenced only inside the game's own call
    ID3D11Texture2D* tex = nullptr;     // the texture behind it, a reference HELD, so the
                                        // frame-boundary read cannot touch a freed object
    uint32_t    lastSeenFrame = 0;
    uint32_t    w = 0, h = 0;
    DXGI_FORMAT texFmt = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT dsvFmt = DXGI_FORMAT_UNKNOWN;
    UINT        dsvFlags = 0;           // D3D11_DSV_READ_ONLY_DEPTH and friends
    UINT        bindFlags = 0;
    UINT        samples = 1;
    UINT        arraySize = 1;
    UINT        mips = 1;
    uint32_t    firstEyeDraw = 0;       // the lowest eye-draw index it was bound at; 0 = never at one
    uint32_t    framesSeen = 0;
    // The census: draws bound to it per frame, by what sat beside it.
    uint32_t    drawsThisFrame = 0, drawsLastFrame = 0;
    uint32_t    firstBindThisFrame = 0, firstBindLastFrame = 0;   // the frame's draw index at first bind
    uint32_t    eyeRtvDrawsThisFrame = 0, eyeRtvDrawsLastFrame = 0;
    uint32_t    nullRtvDrawsThisFrame = 0, nullRtvDrawsLastFrame = 0;
    uint32_t    unbindsThisFrame = 0;   // how many times the game switched away from it
    uint32_t    unbindsLastFrame = 0;
    uint64_t    lastSampleMs = 0;
    int         sampleLines = 0;
    float       clearValue = -1.0f;     // what the game clears it to; -1 = not seen
    bool        boundThisFrame = false;
    bool        announced = false;
    bool        unreadableNoted = false;
};
Target   g_targets[kMaxTargets];
int      g_targetCount = 0;
void*    g_lastDsv = nullptr;
void*    g_lastDrawDsv = nullptr;   // the per-draw fast path's cache
int      g_lastDrawIdx = -1;
bool     g_wanted = false;
uint32_t g_distinctThisFrame = 0;
uint32_t g_maxDistinct = 0;
uint32_t g_eyeFrames = 0;   // frames that had at least one eye draw
bool     g_eyeDrawThisFrame = false;
bool     g_summaryNoted = false;
// The census of all draws, per frame.
uint32_t g_drawsThisFrame = 0, g_drawsLastFrame = 0;
uint32_t g_drawsWithDsvThisFrame = 0, g_drawsWithDsvLastFrame = 0;
uint32_t g_indirectThisFrame = 0, g_indirectLastFrame = 0;
uint32_t g_eyeRtvDrawsThisFrame = 0, g_eyeRtvDrawsLastFrame = 0;
uint32_t g_boundaryNext = 0;        // round-robin over the targets
uint64_t g_censusLastMs = 0;
constexpr uint64_t kCensusIntervalMs = 20000;
uint64_t g_boundaryLastMs = 0;
bool     g_stagingAtBoundary = false;
uint32_t g_frameNo = 0;
constexpr uint32_t kReleaseAfterFrames = 120;

// The GPU side: the shader, its parameter buffer, the 1 KB result, TWO
// staging twins (one per read path), and an owned copy of the target.
ID3D11ComputeShader*       g_cs = nullptr;
bool                       g_csTried = false;
ID3D11Buffer*              g_cb = nullptr;
ID3D11Buffer*              g_out = nullptr;
ID3D11UnorderedAccessView* g_outUav = nullptr;
ID3D11Buffer*              g_staging[2] = {};   // 0 the direct view, 1 the copy
bool                       g_stagingInFlight = false;
bool                       g_stagingHas[2] = {};
int                        g_stagingTarget = -1;
uint32_t                   g_stagingCycle = 0, g_stagingCycles = 0;
HRESULT                    g_stagingDirectHr = S_OK;
ID3D11Texture2D*           g_copyTex = nullptr;
uint32_t                   g_copyW = 0, g_copyH = 0;
DXGI_FORMAT                g_copyFmt = DXGI_FORMAT_UNKNOWN;

FaultBudget g_budget("depthProbe", 6);

const char* fmtName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R24G8_TYPELESS:            return "R24G8_TYPELESS";
        case DXGI_FORMAT_D24_UNORM_S8_UINT:         return "D24_UNORM_S8_UINT";
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:     return "R24_UNORM_X8_TYPELESS";
        case DXGI_FORMAT_R32_TYPELESS:              return "R32_TYPELESS";
        case DXGI_FORMAT_D32_FLOAT:                 return "D32_FLOAT";
        case DXGI_FORMAT_R32_FLOAT:                 return "R32_FLOAT";
        case DXGI_FORMAT_R32G8X24_TYPELESS:         return "R32G8X24_TYPELESS";
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:      return "D32_FLOAT_S8X24_UINT";
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:  return "R32_FLOAT_X8X24_TYPELESS";
        case DXGI_FORMAT_R16_TYPELESS:              return "R16_TYPELESS";
        case DXGI_FORMAT_D16_UNORM:                 return "D16_UNORM";
        case DXGI_FORMAT_R16_UNORM:                 return "R16_UNORM";
        default:                                    return "?";
    }
}

// The view format that reads the depth channel of a texture of this
// format, and the typeless format an owned copy of it must have. UNKNOWN
// when this build knows no such view.
DXGI_FORMAT depthReadFormat(DXGI_FORMAT tex, DXGI_FORMAT* copyFmt) {
    switch (tex) {
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            *copyFmt = DXGI_FORMAT_R24G8_TYPELESS;
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
            *copyFmt = DXGI_FORMAT_R32_TYPELESS;
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            *copyFmt = DXGI_FORMAT_R32G8X24_TYPELESS;
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
            *copyFmt = DXGI_FORMAT_R16_TYPELESS;
            return DXGI_FORMAT_R16_UNORM;
        default:
            *copyFmt = DXGI_FORMAT_UNKNOWN;
            return DXGI_FORMAT_UNKNOWN;
    }
}

int findTarget(void* dsv) {
    for (int i = 0; i < g_targetCount; ++i) {
        if (g_targets[i].dsv == dsv) return i;
    }
    return -1;
}

void releaseGpu() {
    if (g_copyTex) { g_copyTex->Release(); g_copyTex = nullptr; }
    g_copyW = g_copyH = 0;
    g_copyFmt = DXGI_FORMAT_UNKNOWN;
    for (ID3D11Buffer*& s : g_staging) {
        if (s) { s->Release(); s = nullptr; }
    }
    if (g_outUav) { g_outUav->Release(); g_outUav = nullptr; }
    if (g_out) { g_out->Release(); g_out = nullptr; }
    if (g_cb) { g_cb->Release(); g_cb = nullptr; }
    if (g_cs) { g_cs->Release(); g_cs = nullptr; }
    g_stagingInFlight = false;
    g_stagingHas[0] = g_stagingHas[1] = false;
}

bool ensureGpu(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (!g_cs && !g_csTried) {
        g_csTried = true;
        g_cs = shaderSwapCompileCs(ctx, kSampleCsHlsl, sizeof(kSampleCsHlsl) - 1,
                                   "main", "depth_probe_cs", nullptr,
                                   "depth probe");
    }
    if (!g_cs) return false;
    if (!g_out) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 256 * 4;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 256;
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth = 256 * 4;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        D3D11_BUFFER_DESC cd{};
        cd.ByteWidth = 16;
        cd.Usage = D3D11_USAGE_DYNAMIC;
        cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        const bool made = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_out)) &&
                          SUCCEEDED(dev->CreateUnorderedAccessView(g_out, &ud, &g_outUav)) &&
                          SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &g_staging[0])) &&
                          SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &g_staging[1])) &&
                          SUCCEEDED(dev->CreateBuffer(&cd, nullptr, &g_cb));
        if (!made) {
            releaseGpu();
            return false;
        }
    }
    return true;
}

// One run of the sampler: the grid of `srv` into g_out, copied to
// `staging`. The compute stage is saved and put back around it.
bool runSampler(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srv,
                uint32_t w, uint32_t h, ID3D11Buffer* staging) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) {
        return false;
    }
    uint32_t prm[4] = {w, h, 0, 0};
    memcpy(m.pData, prm, sizeof(prm));
    ctx->Unmap(g_cb, 0);

    ID3D11ComputeShader* savedCs = nullptr;
    ID3D11ShaderResourceView* savedSrv = nullptr;
    ID3D11UnorderedAccessView* savedUav = nullptr;
    ID3D11Buffer* savedCb = nullptr;
    ctx->CSGetShader(&savedCs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 1, &savedSrv);
    ctx->CSGetUnorderedAccessViews(0, 1, &savedUav);
    ctx->CSGetConstantBuffers(0, 1, &savedCb);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ctx->CSSetShaderResources(0, 1, &nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ctx->CSSetShader(g_cs, nullptr, 0);
    ID3D11Buffer* cb = g_cb;
    ctx->CSSetConstantBuffers(0, 1, &cb);
    ctx->CSSetShaderResources(0, 1, &srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_outUav, nullptr);
    ctx->Dispatch(1, 1, 1);
    ctx->CSSetShaderResources(0, 1, &nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ctx->CopyResource(staging, g_out);

    ctx->CSSetShader(savedCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &savedSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &savedUav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &savedCb);
    if (savedCs) savedCs->Release();
    if (savedSrv) savedSrv->Release();
    if (savedUav) savedUav->Release();
    if (savedCb) savedCb->Release();
    return true;
}

// The copy path: the target's contents into an owned texture of the same
// typeless family, viewable, ours. A copy is not subject to the binding
// hazards a shader view is, so this reads whatever the direct view could
// not.
ID3D11ShaderResourceView* copiedView(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                                     ID3D11Texture2D* tex, uint32_t w, uint32_t h,
                                     DXGI_FORMAT copyFmt, DXGI_FORMAT readFmt) {
    if (!g_copyTex || g_copyW != w || g_copyH != h || g_copyFmt != copyFmt) {
        if (g_copyTex) { g_copyTex->Release(); g_copyTex = nullptr; }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = copyFmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_copyTex)) && g_copyTex) {
            g_copyW = w;
            g_copyH = h;
            g_copyFmt = copyFmt;
        } else {
            g_copyTex = nullptr;
            return nullptr;
        }
    }
    ctx->CopyResource(g_copyTex, tex);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = readFmt;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    dev->CreateShaderResourceView(g_copyTex, &sd, &srv);
    return srv;
}

// Metres for a reversed-Z value under an infinite far plane: z = near /
// value. Zero (the far plane) is infinity; the caller prints that as such.
double metresOf(float v) {
    if (!(v > 0.0f)) return 0.0;
    return kNearPlaneM / static_cast<double>(v);
}

struct GridStats {
    float mn = 1e30f, mx = -1e30f, centre = 0.0f;
    int atClear = 0, bandFar = 0, bandKm = 0, bandHm = 0, bandM = 0, bandNear = 0;
    float block[16] = {};   // the 4x4 map: each block's NEAREST sample (reversed-Z max)
};

GridStats gridStats(const float* v, float clear) {
    GridStats g;
    for (int i = 0; i < 256; ++i) {
        const float d = v[i];
        if (d < g.mn) g.mn = d;
        if (d > g.mx) g.mx = d;
        if (clear >= 0.0f && fabsf(d - clear) < 1e-7f) ++g.atClear;
        // Bands, for the reversed-Z reading: value = near / z.
        if (d <= 0.0f) ++g.bandFar;
        else if (d < 2.5e-6f) ++g.bandKm;     // beyond 10 km
        else if (d < 2.5e-4f) ++g.bandHm;     // 100 m .. 10 km
        else if (d < 1.25e-2f) ++g.bandM;     // 2 m .. 100 m
        else ++g.bandNear;                    // within 2 m
    }
    g.centre = v[7 * 16 + 7];
    for (int by = 0; by < 4; ++by) {
        for (int bx = 0; bx < 4; ++bx) {
            float m = 0.0f;
            for (int y = by * 4; y < by * 4 + 4; ++y) {
                for (int x = bx * 4; x < bx * 4 + 4; ++x) {
                    if (v[y * 16 + x] > m) m = v[y * 16 + x];
                }
            }
            g.block[by * 4 + bx] = m;
        }
    }
    return g;
}

// The 4x4 map as text, rows top to bottom, metres under the reversed-Z
// reading; "-" for a block whose nearest sample is the far plane. Where
// the HUD sits in depth is read straight off it.
void mapText(const GridStats& g, char* out, size_t n) {
    size_t used = 0;
    for (int i = 0; i < 16 && used < n; ++i) {
        const float d = g.block[i];
        int m;
        if (d > 0.0f) {
            m = snprintf(out + used, n - used, "%s%.1f", i ? (i % 4 ? " " : " | ") : "",
                         metresOf(d));
        } else {
            m = snprintf(out + used, n - used, "%s-", i ? (i % 4 ? " " : " | ") : "");
        }
        if (m > 0) used += static_cast<size_t>(m);
    }
}

}  // namespace

void depthProbeConfigure(Config& cfg) {
    const std::string mode = cfg.getString("fix.temporal_aa", "off");
    g_wanted = _stricmp(mode.c_str(), "off") != 0 && !mode.empty();
}

namespace {

// A view seen for the first time: its texture described once, under the
// budget, while the game has it bound. -1 when it cannot be described or
// the table is full.
int discoverTarget(void* dsv) {
    bool room = g_targetCount < kMaxTargets;
    for (int i = 0; i < g_targetCount && !room; ++i) room = !g_targets[i].dsv;
    if (!room) return -1;
    Target t;
    t.dsv = dsv;
    bool ok = false;
    guardedBudget(g_budget, [&] {
            ID3D11DepthStencilView* v = static_cast<ID3D11DepthStencilView*>(dsv);
            D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
            v->GetDesc(&vd);
            ID3D11Resource* res = nullptr;
            v->GetResource(&res);
            if (res) {
                ID3D11Texture2D* tex = nullptr;
                res->QueryInterface(__uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&tex));
                if (tex) {
                    D3D11_TEXTURE2D_DESC td{};
                    tex->GetDesc(&td);
                    t.w = td.Width;
                    t.h = td.Height;
                    t.texFmt = td.Format;
                    t.bindFlags = td.BindFlags;
                    t.samples = td.SampleDesc.Count;
                    t.arraySize = td.ArraySize;
                    t.mips = td.MipLevels;
                    t.dsvFmt = vd.Format;
                    t.dsvFlags = vd.Flags;
                    ok = true;
                    t.tex = tex;   // the reference stays with the entry
                }
                res->Release();
            }
    });
    if (!ok) return -1;
    t.lastSeenFrame = g_frameNo;
    int idx = -1;
    for (int i = 0; i < g_targetCount; ++i) {
        if (!g_targets[i].dsv) { idx = i; break; }   // an evicted slot
    }
    if (idx < 0) idx = g_targetCount++;
    g_targets[idx] = t;
    return idx;
}

}  // namespace

int  g_scenePick[2] = {-1, -1};   // the pair in use, for hysteresis
bool g_sceneLatchedThisFrame = false;   // the temporal pass's camera latch fired

void depthProbeNoteDraw(ID3D11DeviceContext* ctx, void* dsv, bool rtvEyeSized,
                        bool rtvNull) {
    (void)ctx;
    if (!g_wanted) return;
    ++g_drawsThisFrame;
    if (rtvEyeSized) ++g_eyeRtvDrawsThisFrame;
    if (!dsv) return;
    ++g_drawsWithDsvThisFrame;
    int idx;
    if (dsv == g_lastDrawDsv) {
        idx = g_lastDrawIdx;
    } else {
        idx = findTarget(dsv);
        if (idx < 0) idx = discoverTarget(dsv);
        g_lastDrawDsv = dsv;
        g_lastDrawIdx = idx;
    }
    if (idx < 0) return;
    Target& t = g_targets[idx];
    ++t.drawsThisFrame;
    // The temporal pass's camera latch: the frame's FIRST draw into the
    // scene pair's depth is drawn with the scene camera by construction,
    // whichever eye it is (the first bound is the first rendered). The
    // first eye-sized draw was the latch until 2026-09-04, and in space
    // it caught another camera on some frames: the rows' delta read 36
    // to 40 degrees a frame against the head's and NVIDIA's history
    // purged on each, a pulse every couple of seconds (temporal_pass.h).
    if (!g_sceneLatchedThisFrame && t.drawsThisFrame == 1 &&
        (idx == g_scenePick[0] || idx == g_scenePick[1])) {
        g_sceneLatchedThisFrame = true;
        temporalPassNoteFirstEyeDraw();
    }
    if (rtvEyeSized) ++t.eyeRtvDrawsThisFrame;
    if (rtvNull) ++t.nullRtvDrawsThisFrame;
    t.lastSeenFrame = g_frameNo;
    if (!t.boundThisFrame) {
        t.boundThisFrame = true;
        t.firstBindThisFrame = g_drawsThisFrame;
        ++t.framesSeen;
        ++g_distinctThisFrame;
    }
}


bool depthProbeSceneDepth(uint32_t w, uint32_t h, int eye, ID3D11Texture2D** tex) {
    if (!tex) return false;
    *tex = nullptr;
    if (!g_wanted || eye < 0 || eye > 1) return false;
    // The scene's targets: the two BUSIEST of this size last frame,
    // whatever the count -- the second depth flight (2026-09-03) had the
    // world stop being drawn for a minute (the station's own screens)
    // while the cockpit alone went to a pair with four to nine draws,
    // which is exactly the depth the text needs, and a threshold of fifty
    // refused it. Ordered by first bind in the frame; the pair in use is
    // kept while it stays within half of the busiest, so two pairs the
    // game alternates between do not flap.
    int best[2] = {-1, -1};
    for (int i = 0; i < g_targetCount; ++i) {
        const Target& t = g_targets[i];
        if (!t.dsv || !t.tex || t.w != w || t.h != h || t.samples > 1) continue;
        if (t.drawsLastFrame < 1) continue;
        if (best[0] < 0 || t.drawsLastFrame > g_targets[best[0]].drawsLastFrame) {
            best[1] = best[0];
            best[0] = i;
        } else if (best[1] < 0 || t.drawsLastFrame > g_targets[best[1]].drawsLastFrame) {
            best[1] = i;
        }
    }
    if (best[0] < 0 || best[1] < 0) return false;
    // Hysteresis: the pair in use stays while both are still of this size
    // and drawn into at least half as much as the busiest.
    bool keep = g_scenePick[0] >= 0 && g_scenePick[1] >= 0;
    for (int k = 0; k < 2 && keep; ++k) {
        const int i = g_scenePick[k];
        if (i >= g_targetCount) { keep = false; break; }
        const Target& t = g_targets[i];
        keep = t.dsv && t.tex && t.w == w && t.h == h &&
               t.drawsLastFrame * 2 >= g_targets[best[0]].drawsLastFrame &&
               t.drawsLastFrame >= 1;
    }
    if (!keep) {
        g_scenePick[0] = best[0];
        g_scenePick[1] = best[1];
    }
    // The first bound in the frame is the first eye rendered: the left.
    int first = g_scenePick[0], second = g_scenePick[1];
    if (g_targets[second].firstBindLastFrame < g_targets[first].firstBindLastFrame) {
        const int tmp = first; first = second; second = tmp;
    }
    *tex = g_targets[eye == 0 ? first : second].tex;
    return true;
}

uint32_t depthProbeSceneDraws() {
    if (!g_wanted || g_scenePick[0] < 0 || g_scenePick[1] < 0 ||
        g_scenePick[0] >= g_targetCount || g_scenePick[1] >= g_targetCount) {
        return 0;
    }
    const uint32_t a = g_targets[g_scenePick[0]].drawsLastFrame;
    const uint32_t b = g_targets[g_scenePick[1]].drawsLastFrame;
    return a < b ? a : b;
}

void depthProbeNoteIndirectDraw(ID3D11DeviceContext* ctx, void* dsv) {
    if (!g_wanted) return;
    ++g_indirectThisFrame;
    depthProbeNoteDraw(ctx, dsv, false, false);
}

void depthProbeNoteEyeDraw(ID3D11DeviceContext* ctx, void* dsv,
                           uint32_t eyeDrawIndex) {
    (void)ctx;
    if (!g_wanted) return;
    g_eyeDrawThisFrame = true;
    // The common case is one compare: the same view as the last eye draw.
    if (dsv == g_lastDsv) return;
    g_lastDsv = dsv;
    if (!dsv) return;
    int idx = findTarget(dsv);
    if (idx < 0) idx = discoverTarget(dsv);
    if (idx < 0) return;
    Target& t = g_targets[idx];
    if (t.firstEyeDraw == 0 || eyeDrawIndex < t.firstEyeDraw) {
        t.firstEyeDraw = eyeDrawIndex;
    }
}

bool depthProbeWantsSample(void* current, void* next) {
    if (!g_wanted || !current || current == next) return false;
    const int idx = findTarget(current);
    if (idx < 0) return false;
    Target& t = g_targets[idx];
    // Every switch away from an eye-draw target counts, whether or not it
    // is sampled: the LAST one in a frame is where the contents are
    // complete, and last frame's count says which one that is.
    ++t.unbindsThisFrame;
    if (g_stagingInFlight) return false;
    if (t.sampleLines >= kMaxSampleLines) return false;
    if (t.lastSampleMs && !elapsedMs(t.lastSampleMs, kSampleIntervalMs)) return false;
    if (t.unbindsLastFrame > 0 && t.unbindsThisFrame < t.unbindsLastFrame) return false;
    return true;
}

void depthProbeSample(ID3D11DeviceContext* ctx, void* dsvPtr) {
    if (!g_wanted || !dsvPtr || !ctx) return;
    const int idx = findTarget(dsvPtr);
    if (idx < 0) return;
    Target& t = g_targets[idx];
    t.lastSampleMs = stampMs();

    if (t.samples > 1) {
        if (!t.unreadableNoted) {
            t.unreadableNoted = true;
            Log::get().note(
                "depth probe: target #%d is multisampled (%u samples); its "
                "values cannot be read with a plain load, so v2 would need a "
                "resolve of its own first.",
                idx, t.samples);
        }
        return;
    }
    DXGI_FORMAT copyFmt = DXGI_FORMAT_UNKNOWN;
    const DXGI_FORMAT readFmt = depthReadFormat(t.texFmt, &copyFmt);
    if (readFmt == DXGI_FORMAT_UNKNOWN) {
        if (!t.unreadableNoted) {
            t.unreadableNoted = true;
            Log::get().note(
                "depth probe: target #%d's format %s (%d) has no depth-"
                "reading view this build knows; v2 would need one. Please "
                "report this log.",
                idx, fmtName(t.texFmt), static_cast<int>(t.texFmt));
        }
        return;
    }

    ID3D11DepthStencilView* dsv = static_cast<ID3D11DepthStencilView*>(dsvPtr);
    guardedBudget(g_budget, [&] {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return;
        ID3D11Resource* res = nullptr;
        dsv->GetResource(&res);
        ID3D11Texture2D* tex = nullptr;
        if (res) {
            res->QueryInterface(__uuidof(ID3D11Texture2D),
                                reinterpret_cast<void**>(&tex));
        }
        if (tex && ensureGpu(dev, ctx)) {
            // Both paths, every sample, so a read that fails one way and
            // not the other says so in one line.
            g_stagingHas[0] = g_stagingHas[1] = false;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = readFmt;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            ID3D11ShaderResourceView* direct = nullptr;
            g_stagingDirectHr = (t.bindFlags & D3D11_BIND_SHADER_RESOURCE)
                                    ? dev->CreateShaderResourceView(tex, &sd, &direct)
                                    : E_FAIL;
            if (direct) {
                g_stagingHas[0] = runSampler(ctx, direct, t.w, t.h, g_staging[0]);
                direct->Release();
            }
            ID3D11ShaderResourceView* copied =
                copiedView(dev, ctx, tex, t.w, t.h, copyFmt, readFmt);
            if (copied) {
                g_stagingHas[1] = runSampler(ctx, copied, t.w, t.h, g_staging[1]);
                copied->Release();
            }
            if (g_stagingHas[0] || g_stagingHas[1]) {
                g_stagingInFlight = true;
                g_stagingTarget = idx;
                g_stagingCycle = t.unbindsThisFrame;
                g_stagingCycles = t.unbindsLastFrame;
            }
        }
        if (tex) tex->Release();
        if (res) res->Release();
        dev->Release();
    });
}

void depthProbeNoteClear(ID3D11DepthStencilView* dsv, float depth) {
    if (!g_wanted || !dsv) return;
    const int idx = findTarget(dsv);
    if (idx < 0) return;
    g_targets[idx].clearValue = depth;
}

void depthProbeFrameBoundary(ID3D11DeviceContext* ctx) {
    if (!g_wanted) return;
    ++g_frameNo;
    if (g_eyeDrawThisFrame) ++g_eyeFrames;
    g_eyeDrawThisFrame = false;
    g_sceneLatchedThisFrame = false;
    g_drawsLastFrame = g_drawsThisFrame;
    g_drawsWithDsvLastFrame = g_drawsWithDsvThisFrame;
    g_indirectLastFrame = g_indirectThisFrame;
    g_eyeRtvDrawsLastFrame = g_eyeRtvDrawsThisFrame;
    g_drawsThisFrame = g_drawsWithDsvThisFrame = g_indirectThisFrame = 0;
    g_eyeRtvDrawsThisFrame = 0;
    // A target not bound for a while is EVICTED: its texture released (the
    // game may have let it go, and holding it would keep it alive for
    // nothing) and its slot freed. The depth flight of 2026-09-03 found
    // the scene's depth at 3096 wide, then the cull guard rebuilt every
    // target at 3358, and the table -- full of the old ones -- had no room
    // for the new, so the depth was lost for the rest of the session.
    // Announced targets keep their index in the log by leaving a hole that
    // a later discovery may fill.
    for (int i = 0; i < g_targetCount; ++i) {
        Target& t = g_targets[i];
        if (t.dsv && g_frameNo - t.lastSeenFrame > kReleaseAfterFrames) {
            if (t.tex) { t.tex->Release(); t.tex = nullptr; }
            t.dsv = nullptr;
            t.drawsLastFrame = t.drawsThisFrame = 0;
            t.unbindsLastFrame = t.unbindsThisFrame = 0;
        }
    }
    if (g_distinctThisFrame > g_maxDistinct) g_maxDistinct = g_distinctThisFrame;
    g_distinctThisFrame = 0;
    g_lastDsv = nullptr;   // a new frame: its first eye draw notes its view afresh
    g_lastDrawDsv = nullptr;
    g_lastDrawIdx = -1;
    for (int i = 0; i < g_targetCount; ++i) {
        Target& t = g_targets[i];
        t.boundThisFrame = false;
        t.unbindsLastFrame = t.unbindsThisFrame;
        t.unbindsThisFrame = 0;
        t.drawsLastFrame = t.drawsThisFrame;
        t.firstBindLastFrame = t.firstBindThisFrame;
        t.eyeRtvDrawsLastFrame = t.eyeRtvDrawsThisFrame;
        t.nullRtvDrawsLastFrame = t.nullRtvDrawsThisFrame;
        t.drawsThisFrame = t.eyeRtvDrawsThisFrame = t.nullRtvDrawsThisFrame = 0;
        // Announced after a full frame of counting, so the line carries
        // the census rather than a zero.
        if (t.announced || t.framesSeen < 2) continue;
        t.announced = true;
        Log::get().note(
            "depth probe: depth target #%d -- %ux%u, texture format %s (%d), "
            "view format %s (%d) flags 0x%X, bind flags 0x%X, %u sample(s), "
            "array %u, mips %u. Per frame: %u draws bound to it, %u of them "
            "with an eye-sized colour target, %u with no colour target at "
            "all%s. %s (docs\\anti-aliasing.md Phase 0 item 3, measured.)",
            i, t.w, t.h, fmtName(t.texFmt), static_cast<int>(t.texFmt),
            fmtName(t.dsvFmt), static_cast<int>(t.dsvFmt), t.dsvFlags,
            t.bindFlags, t.samples, t.arraySize, t.mips, t.drawsLastFrame,
            t.eyeRtvDrawsLastFrame, t.nullRtvDrawsLastFrame,
            t.firstEyeDraw ? "" : " -- never at an eye draw",
            (t.bindFlags & D3D11_BIND_SHADER_RESOURCE)
                ? "A shader view can be made over it directly."
                : "No shader-resource bind: v2 would copy it out once per "
                  "eye (CopyResource, same typeless family) before reading.");
    }
    // The census, again every 20 s: the second depth flight showed the
    // scene's draws moving between pairs of targets and dropping to a
    // cockpit-only pass for a minute, which one line at 120 frames could
    // never have shown.
    if (g_summaryNoted && g_eyeFrames > 120 &&
        (g_censusLastMs == 0 || elapsedMs(g_censusLastMs, kCensusIntervalMs))) {
        g_censusLastMs = stampMs();
        int order[kMaxTargets];
        int live = 0;
        for (int i = 0; i < g_targetCount; ++i) {
            if (g_targets[i].dsv) order[live++] = i;
        }
        for (int i = 1; i < live; ++i) {
            for (int j = i; j > 0 && g_targets[order[j]].drawsLastFrame >
                                         g_targets[order[j - 1]].drawsLastFrame; --j) {
                const int tmp = order[j]; order[j] = order[j - 1]; order[j - 1] = tmp;
            }
        }
        char busy[320] = "";
        size_t used = 0;
        for (int k = 0; k < live && k < 5; ++k) {
            const Target& b = g_targets[order[k]];
            const int m = snprintf(busy + used, sizeof(busy) - used, "%s#%d %ux%u %u draws",
                                   k ? ", " : "", order[k], b.w, b.h, b.drawsLastFrame);
            if (m > 0) used += static_cast<size_t>(m);
            if (used >= sizeof(busy)) break;
        }
        Log::get().note(
            "depth probe census: %u draws last frame, %u with a depth target; "
            "the busiest: %s; the pass's scene pair: #%d and #%d.",
            g_drawsLastFrame, g_drawsWithDsvLastFrame, busy, g_scenePick[0],
            g_scenePick[1]);
    }
    if (!g_summaryNoted && g_eyeFrames >= 120) {
        g_summaryNoted = true;
        if (g_targetCount == 0) {
            Log::get().note(
                "depth probe: no depth-stencil view was bound during the eye "
                "draws of the first 120 frames that had any -- the game may "
                "bind depth by a path the binding shadow does not see, or "
                "draw the eyes without it. v2 needs another way in; please "
                "report this log.");
        } else {
            // The busiest targets by draws last frame, so a cockpit census
            // with a handful of shadow maps still names the scene's depth.
            int order[kMaxTargets];
            for (int i = 0; i < g_targetCount; ++i) order[i] = i;
            for (int i = 1; i < g_targetCount; ++i) {
                for (int j = i; j > 0 && g_targets[order[j]].drawsLastFrame >
                                             g_targets[order[j - 1]].drawsLastFrame; --j) {
                    const int tmp = order[j]; order[j] = order[j - 1]; order[j - 1] = tmp;
                }
            }
            char busy[256] = "";
            size_t used = 0;
            for (int k = 0; k < g_targetCount && k < 4; ++k) {
                const Target& b = g_targets[order[k]];
                const int m = snprintf(busy + used, sizeof(busy) - used, "%s#%d %ux%u %u draws",
                                       k ? ", " : "", order[k], b.w, b.h, b.drawsLastFrame);
                if (m > 0) used += static_cast<size_t>(m);
                if (used >= sizeof(busy)) break;
            }
            Log::get().note(
                "depth probe: %d distinct depth target(s) seen at draws over "
                "120 frames, at most %u in one frame. Last frame: %u draws "
                "hooked, %u of them with a depth target bound, %u with an "
                "eye-sized colour target, %u indirect; the busiest targets: %s. "
                "Each target is sampled at the LAST moment in a frame the game "
                "switches away from it, both through a view and through a "
                "copy, and at the frame boundary through a copy, every %llu s.",
                g_targetCount, g_maxDistinct, g_drawsLastFrame,
                g_drawsWithDsvLastFrame, g_eyeRtvDrawsLastFrame,
                g_indirectLastFrame, busy,
                static_cast<unsigned long long>(kSampleIntervalMs / 1000));
        }
    }
    // The frame-boundary read: one target per interval, round-robin, through
    // the reference held on its texture and the copy path -- after every
    // draw of the frame has been issued, whenever in the frame the depth
    // was written. Nothing is unbound for it: a copy is not a view.
    if (!g_stagingInFlight && ctx && g_targetCount > 0 &&
        (g_boundaryLastMs == 0 || elapsedMs(g_boundaryLastMs, kSampleIntervalMs))) {
        for (int tries = 0; tries < g_targetCount; ++tries) {
            const int idx = static_cast<int>(g_boundaryNext++ % static_cast<uint32_t>(g_targetCount));
            Target& t = g_targets[idx];
            if (!t.tex || t.samples > 1 || t.sampleLines >= kMaxSampleLines * 2) continue;
            if (t.drawsLastFrame == 0 && g_targetCount > 4) continue;
            DXGI_FORMAT copyFmt = DXGI_FORMAT_UNKNOWN;
            const DXGI_FORMAT readFmt = depthReadFormat(t.texFmt, &copyFmt);
            if (readFmt == DXGI_FORMAT_UNKNOWN) continue;
            g_boundaryLastMs = stampMs();
            guardedBudget(g_budget, [&] {
                ID3D11Device* dev = nullptr;
                ctx->GetDevice(&dev);
                if (!dev) return;
                if (ensureGpu(dev, ctx)) {
                    g_stagingHas[0] = g_stagingHas[1] = false;
                    ID3D11ShaderResourceView* copied =
                        copiedView(dev, ctx, t.tex, t.w, t.h, copyFmt, readFmt);
                    if (copied) {
                        g_stagingHas[1] = runSampler(ctx, copied, t.w, t.h, g_staging[1]);
                        copied->Release();
                    }
                    if (g_stagingHas[1]) {
                        g_stagingInFlight = true;
                        g_stagingTarget = idx;
                        g_stagingCycle = 0;
                        g_stagingCycles = t.unbindsLastFrame;
                        g_stagingAtBoundary = true;
                        g_stagingDirectHr = S_OK;
                    }
                }
                dev->Release();
            });
            break;
        }
    }
    if (g_stagingInFlight && ctx) {
        // Both stagings were written in the same call; the second is ready
        // when the first is, and DO_NOT_WAIT on each keeps this a poll.
        float vals[2][256];
        bool got[2] = {};
        bool stillDrawing = false;
        for (int s = 0; s < 2; ++s) {
            if (!g_stagingHas[s] || !g_staging[s]) continue;
            D3D11_MAPPED_SUBRESOURCE m{};
            const HRESULT hr = ctx->Map(g_staging[s], 0, D3D11_MAP_READ,
                                        D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            if (SUCCEEDED(hr) && m.pData) {
                memcpy(vals[s], m.pData, sizeof(vals[s]));
                ctx->Unmap(g_staging[s], 0);
                got[s] = true;
            } else if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                stillDrawing = true;
            }
        }
        if (!stillDrawing) {
            g_stagingInFlight = false;
            const int tIdx = g_stagingTarget >= 0 ? g_stagingTarget : 0;
            Target& t = g_targets[tIdx];
            ++t.sampleLines;
            const float clear = t.clearValue;
            char part[2][520];
            for (int s = 0; s < 2; ++s) {
                if (!g_stagingHas[s]) {
                    if (s == 0 && g_stagingAtBoundary) {
                        snprintf(part[s], sizeof(part[s]), "direct view: not tried at the boundary");
                        continue;
                    }
                    snprintf(part[s], sizeof(part[s]),
                             s == 0 ? "direct view: not made (hr 0x%08lX)"
                                    : "copy: not made",
                             static_cast<unsigned long>(g_stagingDirectHr));
                    continue;
                }
                if (!got[s]) {
                    snprintf(part[s], sizeof(part[s]), "%s: unreadable",
                             s == 0 ? "direct view" : "copy");
                    continue;
                }
                const GridStats g = gridStats(vals[s], clear);
                const bool reversed = clear >= 0.0f ? clear < 0.5f : (g.mx < 0.5f);
                const float nearest = reversed ? g.mx : g.mn;
                char map[200] = "";
                if (s == 1 || !g_stagingHas[1]) mapText(g, map, sizeof(map));
                snprintf(part[s], sizeof(part[s]),
                         "%s: min %.8f, max %.8f, centre %.8f (%.2f m); %d at "
                         "the far plane, %d beyond 10 km, %d at 100 m..10 km, "
                         "%d at 2..100 m, %d within 2 m; nearest %.8f = %.2f m%s%s",
                         s == 0 ? "direct view" : "copy",
                         static_cast<double>(g.mn), static_cast<double>(g.mx),
                         static_cast<double>(g.centre), metresOf(g.centre),
                         g.bandFar, g.bandKm, g.bandHm, g.bandM, g.bandNear,
                         static_cast<double>(nearest), metresOf(nearest),
                         map[0] ? "; nearest per 4x4 block, top row first, in m: " : "",
                         map);
            }
            Log::get().note(
                "depth probe: target #%d (%u draws/frame, %u with no colour "
                "target) sampled on a 16x16 grid %s (clear value %.3f%s; "
                "metres read as reversed-Z with near %.3f m) -- %s; %s.",
                tIdx, t.drawsLastFrame, t.nullRtvDrawsLastFrame,
                g_stagingAtBoundary ? "at the frame boundary" : "at its unbind",
                static_cast<double>(clear), clear < 0.0f ? " = never seen" : "",
                kNearPlaneM, part[0], part[1]);
            g_stagingAtBoundary = false;
        }
    }
}

void depthProbeShutdown() {
    if (g_targetCount > 0) {
        Log::get().note("depth probe: %d depth target(s) seen at the eye draws "
                        "this session.", g_targetCount);
    }
    for (int i = 0; i < g_targetCount; ++i) {
        if (g_targets[i].tex) { g_targets[i].tex->Release(); g_targets[i].tex = nullptr; }
    }
    releaseGpu();
    g_targetCount = 0;
    g_lastDsv = nullptr;
    g_lastDrawDsv = nullptr;
    g_lastDrawIdx = -1;
}

}  // namespace edvr

// The desk test of the read path (tools/smoke): a depth texture of the
// family the game uses, cleared to a known value through a depth view,
// unbound, then read both ways with the probe's own sampler. Returns bits:
// 1 the setup was made, 2 the direct view read the value, 4 the copy did.
extern "C" __declspec(dllexport) unsigned edvrDepthProbeSelftest(void* devicePtr) {
    using namespace edvr;
    ID3D11Device* dev = static_cast<ID3D11Device*>(devicePtr);
    if (!dev) return 0;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx) return 0;
    unsigned bits = 0;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 64;
    td.Height = 48;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* tex = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    const bool wantedBefore = g_wanted;
    g_wanted = true;
    if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &tex)) && tex &&
        SUCCEEDED(dev->CreateDepthStencilView(tex, &dd, &dsv)) && dsv &&
        ensureGpu(dev, ctx)) {
        bits |= 1u;
        ctx->OMSetRenderTargets(0, nullptr, dsv);
        ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 0.5f, 0);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        DXGI_FORMAT copyFmt = DXGI_FORMAT_UNKNOWN;
        const DXGI_FORMAT readFmt = depthReadFormat(td.Format, &copyFmt);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = readFmt;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        for (int s = 0; s < 2; ++s) {
            ID3D11ShaderResourceView* srv = nullptr;
            if (s == 0) dev->CreateShaderResourceView(tex, &sd, &srv);
            else srv = copiedView(dev, ctx, tex, 64, 48, copyFmt, readFmt);
            if (!srv) continue;
            const bool ran = runSampler(ctx, srv, 64, 48, g_staging[s]);
            srv->Release();
            if (!ran) continue;
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx->Map(g_staging[s], 0, D3D11_MAP_READ, 0, &m)) && m.pData) {
                const float* v = static_cast<const float*>(m.pData);
                bool all = true;
                for (int i = 0; i < 256; ++i) {
                    if (fabsf(v[i] - 0.5f) > 1e-6f) all = false;
                }
                ctx->Unmap(g_staging[s], 0);
                if (all) bits |= s == 0 ? 2u : 4u;
            }
        }
    }
    g_wanted = wantedBefore;
    if (dsv) dsv->Release();
    if (tex) tex->Release();
    releaseGpu();
    g_csTried = false;
    ctx->Release();
    return bits;
}

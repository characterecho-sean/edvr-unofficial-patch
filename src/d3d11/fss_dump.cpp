#include "fss_dump.h"

#include <cstdio>

#include <windows.h>

#include <d3d11.h>

#include <string>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "exposure_fix.h"   // lookupShaderHash
#include "shader_swap.h"    // shaderSwapCompileCs: the series reducer

namespace edvr {
namespace {

constexpr uint64_t kRingQuadHash  = 0x7E38A6AA1269C901ull;
constexpr uint64_t kCompositeHash = 0x953C8123AD8DC13Bull;
// The tonemap (round 25): the residual squares survived every HDR-stage
// draw skip, so the dump now brackets the LDR stage too -- its output at
// the tonemap, and again at frame end after the compose chain and the
// panel quads have drawn over it. Holes appearing between c4 and c6 name
// the LDR interval; holes already at c4 name the tonemap's input.
constexpr uint64_t kTonemapHash   = 0x2D78DC3FD2C0C543ull;
// The reconstruction pair (round 30): the mono-submit probe proved the
// submitted images DIFFER -- occurrence 1 (the left eye) carries the
// squares, occurrence 2 does not -- so the dump now brackets the last
// interval: each eye's accumulate input before the dispatch and the
// packed output after it.
constexpr uint64_t kAccumHash  = 0xE861F611375E7ECCull;
constexpr uint64_t kOutputHash = 0xB74273EC13F7CD59ull;

// Checkpoint layout: [checkpoint][eye]. c0/c1 = before/after the ring quad
// (HDR), c2/c3 = before/after the composite (HDR), c4 = after the tonemap
// (LDR), c5 = frame end HDR, c6 = frame end LDR, c7 = the accumulate
// dispatch's input as read (per eye), c8 = the packed output as written
// (per eye) -- the submitted texture.
constexpr uint32_t kCheckpoints = 9;
constexpr uint32_t kEyes = 2;
const char* const kTags[kCheckpoints] = {"pre-ring",  "post-ring",
                                         "pre-comp",  "post-comp",
                                         "post-tone", "end-hdr", "end-ldr",
                                         "recon-in",  "recon-out"};

uint32_t g_dumpFrame = 0;      // N from the key; 0 = off
uint32_t g_bodyFrames = 0;     // distinct frames with matched draws
uint32_t g_lastFrameNo = 0;
bool     g_dumping = false;    // this frame is a dump frame
bool     g_done = false;       // one dump per arming
// TWO consecutive frames per dump (round 26): the scan art FLICKERS frame
// to frame while real content holds still, so the difference between the
// passes is an art map the single-frame hole counter never was -- ring
// shadows and space stay put, squares blink.
uint32_t g_dumpPass = 0;

struct Slot {
    ID3D11Texture2D* staging = nullptr;
    uint32_t w = 0, h = 0, fmt = 0;
    void*    srcPtr = nullptr;
    bool     filled = false;
};
Slot g_slots[kCheckpoints][kEyes];

// The targets remembered for the frame-end captures: HDR at the composite,
// LDR at the tonemap.
ID3D11Resource* g_eyeTex[kEyes] = {};
ID3D11Resource* g_ldrTex[kEyes] = {};

// Per-frame occurrence counters for the matched families.
uint8_t g_occRing = 0;
uint8_t g_occComp = 0;
uint8_t g_occTone = 0;
uint8_t g_occAccum = 0;
uint8_t g_occOut = 0;

// The pending action chosen in OnEyeDraw, taken in Begin/End.
// kind: 0 = ring quad (c0/c1), 1 = composite (c2/c3), 2 = tonemap (c4 at
// End only).
uint32_t g_pendingKind = 0;
uint32_t g_pendingEye = 0;

uint64_t g_copies = 0;
bool     g_armedNoted = false;

// The eye SERIES (round 32, the field's own design: "take maybe 30
// snapshots once we zoom and diff between left and right"). Full frames
// at that count would be gigabytes and stalls; a tiny GPU reduce is
// neither: at every packed-output dispatch, one compute pass folds the
// submitted eye image into a 16-pixel tile-luminance row of a small
// atlas -- N consecutive frames, both eyes, ~14 MB total, no readback
// until the end. The offline diff then has the one thing every two-frame
// dump lacked: the per-eye CADENCE of the flashes across the whole
// window the eye actually watches.
constexpr char kSeriesCsHlsl[] = R"HLSL(
Texture2D<float4> src : register(t0);
RWTexture2D<float> outt : register(u0);
cbuffer P : register(b0) { uint4 off; }   // x = yOffset, y = tw, z = th
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= off.y || id.y >= off.z) return;
    float s = 0;
    for (uint j = 0; j < 16; ++j)
        for (uint i = 0; i < 16; ++i) {
            float4 c = src.Load(int3(id.x * 16 + i, id.y * 16 + j, 0));
            s += dot(c.rgb, float3(0.299, 0.587, 0.114));
        }
    outt[uint2(id.x, off.x + id.y)] = s / 256.0;
}
)HLSL";

constexpr uint32_t kSeriesMax = 48;   // atlas height 335*48 fits 16384
uint32_t g_seriesWant = 0;            // frames from the key; 0 = off
bool     g_seriesDone = false;
uint32_t g_seriesCount[2] = {};
uint32_t g_seriesTw = 0, g_seriesTh = 0;
ID3D11ComputeShader*       g_seriesCs = nullptr;
bool                       g_seriesTried = false;
ID3D11Texture2D*           g_seriesAtlas[2] = {};
ID3D11UnorderedAccessView* g_seriesUav[2] = {};
ID3D11Buffer*              g_seriesCb = nullptr;
bool                       g_seriesFailedNoted = false;

void seriesRelease() {
    for (int e = 0; e < 2; ++e) {
        if (g_seriesUav[e]) { g_seriesUav[e]->Release(); g_seriesUav[e] = nullptr; }
        if (g_seriesAtlas[e]) { g_seriesAtlas[e]->Release(); g_seriesAtlas[e] = nullptr; }
    }
    if (g_seriesCb) { g_seriesCb->Release(); g_seriesCb = nullptr; }
    g_seriesCount[0] = g_seriesCount[1] = 0;
}

void seriesCapture(ID3D11DeviceContext* ctx, ID3D11Resource* res,
                   uint32_t eye) {
    if (eye > 1 || g_seriesCount[eye] >= g_seriesWant) return;
    ID3D11Texture2D* tex = nullptr;
    res->QueryInterface(__uuidof(ID3D11Texture2D),
                        reinterpret_cast<void**>(&tex));
    if (!tex) return;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    tex->Release();
    const uint32_t tw = (td.Width + 15) / 16;
    const uint32_t th = (td.Height + 15) / 16;

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;

    if (!g_seriesAtlas[0] || g_seriesTw != tw || g_seriesTh != th) {
        seriesRelease();
        bool ok = true;
        for (int e = 0; e < 2 && ok; ++e) {
            D3D11_TEXTURE2D_DESC ad{};
            ad.Width = tw;
            ad.Height = th * g_seriesWant;
            ad.MipLevels = 1;
            ad.ArraySize = 1;
            ad.Format = DXGI_FORMAT_R32_FLOAT;
            ad.SampleDesc.Count = 1;
            ad.Usage = D3D11_USAGE_DEFAULT;
            ad.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            ok = SUCCEEDED(dev->CreateTexture2D(&ad, nullptr,
                                                &g_seriesAtlas[e])) &&
                 SUCCEEDED(dev->CreateUnorderedAccessView(
                     g_seriesAtlas[e], nullptr, &g_seriesUav[e]));
        }
        if (ok && !g_seriesCb) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 16;
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_seriesCb));
        }
        if (!ok) {
            seriesRelease();
            if (!g_seriesFailedNoted) {
                g_seriesFailedNoted = true;
                Log::get().note("fss series: atlas creation failed; the "
                                "series stands down.");
            }
            dev->Release();
            return;
        }
        g_seriesTw = tw;
        g_seriesTh = th;
    }

    if (!g_seriesCs && !g_seriesTried) {
        g_seriesTried = true;
        g_seriesCs = shaderSwapCompileCs(ctx, kSeriesCsHlsl,
                                         sizeof(kSeriesCsHlsl) - 1, "main",
                                         "fss_series_cs", nullptr,
                                         "fss series");
    }
    if (!g_seriesCs) {
        dev->Release();
        return;
    }

    // The source needs an SRV; the submitted texture carries the shader-
    // resource bind (delivery samples it). A refusal stands the series
    // down with a note rather than faulting.
    ID3D11ShaderResourceView* srcSrv = nullptr;
    if (FAILED(dev->CreateShaderResourceView(res, nullptr, &srcSrv))) {
        if (!g_seriesFailedNoted) {
            g_seriesFailedNoted = true;
            Log::get().note("fss series: the submitted texture refuses an "
                            "SRV; the series stands down.");
        }
        dev->Release();
        return;
    }
    dev->Release();

    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(g_seriesCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) &&
        m.pData) {
        uint32_t vals[4] = {g_seriesCount[eye] * g_seriesTh, g_seriesTw,
                            g_seriesTh, 0};
        memcpy(m.pData, vals, sizeof(vals));
        ctx->Unmap(g_seriesCb, 0);
    } else {
        srcSrv->Release();
        return;
    }

    ID3D11ComputeShader* savedCs = nullptr;
    ID3D11ShaderResourceView* savedSrv = nullptr;
    ID3D11UnorderedAccessView* savedUav = nullptr;
    ID3D11Buffer* savedCb = nullptr;
    ctx->CSGetShader(&savedCs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 1, &savedSrv);
    ctx->CSGetUnorderedAccessViews(0, 1, &savedUav);
    ctx->CSGetConstantBuffers(0, 1, &savedCb);

    UINT keep = 0;
    ctx->CSSetShader(g_seriesCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &srcSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_seriesUav[eye], &keep);
    ctx->CSSetConstantBuffers(0, 1, &g_seriesCb);
    ctx->Dispatch((g_seriesTw + 7) / 8, (g_seriesTh + 7) / 8, 1);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ctx->CSSetShaderResources(0, 1, &nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, &keep);
    ctx->CSSetShader(savedCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &savedSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &savedUav, &keep);
    ctx->CSSetConstantBuffers(0, 1, &savedCb);
    if (savedCs) savedCs->Release();
    if (savedSrv) savedSrv->Release();
    if (savedUav) savedUav->Release();
    if (savedCb) savedCb->Release();
    srcSrv->Release();

    ++g_seriesCount[eye];
}

void seriesWrite(ID3D11DeviceContext* ctx) {
    CreateDirectoryA("edvr_logs", nullptr);
    CreateDirectoryA("edvr_logs\\dumps", nullptr);
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    for (int e = 0; e < 2; ++e) {
        if (!g_seriesAtlas[e]) continue;
        D3D11_TEXTURE2D_DESC td{};
        g_seriesAtlas[e]->GetDesc(&td);
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* stage = nullptr;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &stage)) || !stage) {
            continue;
        }
        ctx->CopyResource(stage, g_seriesAtlas[e]);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m)) && m.pData) {
            char path[128];
            _snprintf_s(path, sizeof(path), _TRUNCATE,
                        "edvr_logs\\dumps\\fssseries_eye%d.bin", e);
            FILE* f = nullptr;
            fopen_s(&f, path, "wb");
            if (f) {
                const uint8_t* row = static_cast<const uint8_t*>(m.pData);
                for (uint32_t y = 0; y < td.Height; ++y) {
                    fwrite(row, 1, static_cast<size_t>(td.Width) * 4, f);
                    row += m.RowPitch;
                }
                fclose(f);
                Log::get().note(
                    "FSSSERIES eye=%d tw=%u th=%u frames=%u file=%s", e,
                    g_seriesTw, g_seriesTh, g_seriesCount[e], path);
            }
            ctx->Unmap(stage, 0);
        }
        stage->Release();
    }
    dev->Release();
    Log::get().note("fss series: complete. Re-arming needs the key cleared "
                    "and set again.");
}

FaultBudget g_budget("fssDump", 8);

void releaseAll() {
    for (uint32_t c = 0; c < kCheckpoints; ++c) {
        for (uint32_t e = 0; e < kEyes; ++e) {
            if (g_slots[c][e].staging) {
                g_slots[c][e].staging->Release();
                g_slots[c][e].staging = nullptr;
            }
            g_slots[c][e].filled = false;
        }
    }
    for (uint32_t e = 0; e < kEyes; ++e) {
        if (g_eyeTex[e]) {
            g_eyeTex[e]->Release();
            g_eyeTex[e] = nullptr;
        }
        if (g_ldrTex[e]) {
            g_ldrTex[e]->Release();
            g_ldrTex[e] = nullptr;
        }
    }
}

// Copy the current RTV0's resource into the checkpoint slot's staging.
// remember: 0 = no, 1 = into g_eyeTex (HDR), 2 = into g_ldrTex (LDR).
void capture(ID3D11DeviceContext* ctx, uint32_t c, uint32_t e,
             int remember) {
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    rtv->Release();
    if (!res) return;

    ID3D11Texture2D* tex = nullptr;
    res->QueryInterface(__uuidof(ID3D11Texture2D),
                        reinterpret_cast<void**>(&tex));
    if (!tex) {
        res->Release();
        return;
    }
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);

    Slot& s = g_slots[c][e];
    if (!s.staging || s.w != td.Width || s.h != td.Height ||
        s.fmt != static_cast<uint32_t>(td.Format)) {
        if (s.staging) {
            s.staging->Release();
            s.staging = nullptr;
        }
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            D3D11_TEXTURE2D_DESC sd = td;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            sd.MipLevels = 1;
            dev->CreateTexture2D(&sd, nullptr, &s.staging);
            dev->Release();
        }
        s.w = td.Width;
        s.h = td.Height;
        s.fmt = static_cast<uint32_t>(td.Format);
    }
    if (s.staging) {
        ctx->CopyResource(s.staging, tex);
        s.srcPtr = res;
        s.filled = true;
        ++g_copies;
    }
    if (remember && e < kEyes) {
        ID3D11Resource** slot = remember == 1 ? &g_eyeTex[e] : &g_ldrTex[e];
        if (*slot) (*slot)->Release();
        *slot = res;   // keep the GetResource ref
        tex->Release();
        return;
    }
    tex->Release();
    res->Release();
}

// The frame-end capture of a remembered per-eye texture into checkpoint c.
void captureRemembered(ID3D11DeviceContext* ctx, ID3D11Resource* res,
                       uint32_t c, uint32_t e) {
    if (!res) return;
    ID3D11Texture2D* tex = nullptr;
    res->QueryInterface(__uuidof(ID3D11Texture2D),
                        reinterpret_cast<void**>(&tex));
    if (!tex) return;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    Slot& s = g_slots[c][e];
    if (!s.staging || s.w != td.Width || s.h != td.Height ||
        s.fmt != static_cast<uint32_t>(td.Format)) {
        if (s.staging) {
            s.staging->Release();
            s.staging = nullptr;
        }
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            D3D11_TEXTURE2D_DESC sd = td;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            sd.MipLevels = 1;
            dev->CreateTexture2D(&sd, nullptr, &s.staging);
            dev->Release();
        }
        s.w = td.Width;
        s.h = td.Height;
        s.fmt = static_cast<uint32_t>(td.Format);
    }
    if (s.staging) {
        ctx->CopyResource(s.staging, tex);
        s.srcPtr = res;
        s.filled = true;
        ++g_copies;
    }
    tex->Release();
}

// Bytes per texel by DXGI format: the reconstruction surfaces are 64-bit
// (fmt 10/11, R16G16B16A16); everything else this dump touches is 32-bit.
uint32_t texelBytes(uint32_t fmt) {
    return (fmt == 10 || fmt == 11) ? 8u : 4u;
}

// Capture an arbitrary resource (not the bound RTV) into a checkpoint.
void captureResource(ID3D11DeviceContext* ctx, ID3D11Resource* res,
                     uint32_t c, uint32_t e) {
    if (res) captureRemembered(ctx, res, c, e);
}

void writeOut(ID3D11DeviceContext* ctx, uint32_t pass) {
    CreateDirectoryA("edvr_logs", nullptr);
    CreateDirectoryA("edvr_logs\\dumps", nullptr);
    for (uint32_t c = 0; c < kCheckpoints; ++c) {
        for (uint32_t e = 0; e < kEyes; ++e) {
            Slot& s = g_slots[c][e];
            if (!s.filled || !s.staging) continue;
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(ctx->Map(s.staging, 0, D3D11_MAP_READ, 0, &m)) ||
                !m.pData) {
                Log::get().note("fss dump: map failed for %s eye%u.",
                                kTags[c], e);
                continue;
            }
            char path[128];
            _snprintf_s(path, sizeof(path), _TRUNCATE,
                        "edvr_logs\\dumps\\fssdump_p%u_c%u_%s_eye%u.bin",
                        pass, c, kTags[c], e);
            FILE* f = nullptr;
            fopen_s(&f, path, "wb");
            if (f) {
                const uint8_t* row = static_cast<const uint8_t*>(m.pData);
                const size_t bpr =
                    static_cast<size_t>(s.w) * texelBytes(s.fmt);
                for (uint32_t y = 0; y < s.h; ++y) {
                    fwrite(row, 1, bpr, f);
                    row += m.RowPitch;
                }
                fclose(f);
                Log::get().note(
                    "FSSDUMP p=%u c=%u tag=%s eye=%u w=%u h=%u fmt=%u "
                    "pitch=%u res=%p file=%s",
                    pass, c, kTags[c], e, s.w, s.h, s.fmt, m.RowPitch,
                    s.srcPtr, path);
            } else {
                Log::get().note("fss dump: could not open %s.", path);
            }
            ctx->Unmap(s.staging, 0);
            s.filled = false;
        }
    }
    Log::get().note("fss dump: complete -- %llu copies taken. The key stays "
                    "set but a second dump needs a fresh arm (clear it, "
                    "save, set it again).",
                    static_cast<unsigned long long>(g_copies));
}

}  // namespace

void fssDumpConfigure(Config& cfg) {
    {
        int n = cfg.getInt("advanced.fss_eye_series", 0);
        if (n < 0) n = 0;
        if (n > static_cast<int>(kSeriesMax)) n = kSeriesMax;
        const uint32_t want = static_cast<uint32_t>(n);
        if (want != g_seriesWant) {
            g_seriesWant = want;
            g_seriesDone = false;
            g_seriesTried = false;
            seriesRelease();
            if (g_seriesWant) {
                Log::get().note(
                    "fss series ARMED: from the first scanner frame, every "
                    "submitted eye image is folded to a 16-pixel tile "
                    "luminance row -- %u consecutive frames, both eyes, "
                    "written to edvr_logs\\dumps\\fssseries_eye*.bin "
                    "when the count fills. No hitches; the per-eye flash "
                    "CADENCE is the measurement.",
                    g_seriesWant);
            }
        }
    }
    const int n = cfg.getInt("advanced.fss_eye_dump", 0);
    const uint32_t want = n > 0 ? static_cast<uint32_t>(n) : 0;
    if (want == g_dumpFrame) return;
    g_dumpFrame = want;
    g_bodyFrames = 0;
    g_lastFrameNo = 0;
    g_dumping = false;
    g_done = false;
    g_dumpPass = 0;
    g_armedNoted = false;
    if (g_dumpFrame) {
        Log::get().note(
            "fss dump ARMED: on body frame %u after this note and again "
            "the frame after, both eyes are captured around the ring quad, "
            "the composite, the tonemap and at frame end (HDR and LDR) -- "
            "28 raw images in edvr_logs\\dumps\\, two hitches, one dump "
            "per arm. Tiles that BLINK between the passes are the scan art; "
            "tiles that hold are content. Zoom a ringed body and let the "
            "build play.",
            g_dumpFrame);
    }
}

bool fssDumpWantsDraws() {
    return (g_dumpFrame != 0 && !g_done) || (g_seriesWant != 0 && !g_seriesDone);
}

bool fssDumpOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                      uint32_t instances) {
    if (!fssDumpWantsDraws() || !ctx) return false;
    if (kind != 'N' || instances != 1 ||
        (count != 3 && count != 4 && count != 6)) {
        return false;
    }
    uint64_t h = 0;
    guardedBudget(g_budget, [&] {
        ID3D11VertexShader* vs = nullptr;
        ctx->VSGetShader(&vs, nullptr, nullptr);
        h = lookupShaderHash(vs);
        if (vs) vs->Release();
    });
    const bool ring = (h == kRingQuadHash && count == 4);
    const bool comp = (h == kCompositeHash && count == 6);
    const bool tone = (h == kTonemapHash && count == 3);
    if (!ring && !comp && !tone) return false;

    // Occurrence -> eye. The counters also run on non-dump frames so the
    // mapping is warm, and reset every boundary.
    const uint8_t occ = ring ? ++g_occRing : (comp ? ++g_occComp : ++g_occTone);
    if (occ > kEyes) return false;
    if (!g_dumping) return false;
    g_pendingKind = ring ? 0 : (comp ? 1 : 2);
    g_pendingEye = occ - 1;
    return true;
}

void fssDumpBegin(ID3D11DeviceContext* ctx) {
    if (!ctx || !g_dumping || g_pendingKind == 2) return;
    guardedBudget(g_budget, [&] {
        capture(ctx, g_pendingKind == 0 ? 0 : 2, g_pendingEye, 0);
    });
}

void fssDumpEnd(ID3D11DeviceContext* ctx) {
    if (!ctx || !g_dumping) return;
    guardedBudget(g_budget, [&] {
        // Post checkpoints; the composite's remembers the HDR eye texture,
        // the tonemap's remembers the LDR target.
        if (g_pendingKind == 0) {
            capture(ctx, 1, g_pendingEye, 0);
        } else if (g_pendingKind == 1) {
            capture(ctx, 3, g_pendingEye, 1);
        } else {
            capture(ctx, 4, g_pendingEye, 2);
        }
    });
}

// Called around every owner-context Dispatch while a dump is armed: the
// reconstruction runs as compute, and its input can only be read before
// the dispatch, its output only after.
void fssDumpDispatchPre(ID3D11DeviceContext* ctx) {
    if (!g_dumping || !ctx) return;
    guardedBudget(g_budget, [&] {
        ID3D11ComputeShader* cs = nullptr;
        ctx->CSGetShader(&cs, nullptr, nullptr);
        const uint64_t h = lookupShaderHash(cs);
        if (cs) cs->Release();
        if (h == kAccumHash) {
            const uint8_t occ = ++g_occAccum;
            if (occ > kEyes) return;
            ID3D11ShaderResourceView* srv = nullptr;
            ctx->CSGetShaderResources(0, 1, &srv);
            if (srv) {
                ID3D11Resource* res = nullptr;
                srv->GetResource(&res);
                if (res) {
                    captureResource(ctx, res, 7, occ - 1);
                    res->Release();
                }
                srv->Release();
            }
        }
    });
}

void fssDumpDispatchPost(ID3D11DeviceContext* ctx) {
    const bool seriesLive = g_seriesWant != 0 && !g_seriesDone;
    if ((!g_dumping && !seriesLive) || !ctx) return;
    guardedBudget(g_budget, [&] {
        // The output pass is matched by SHAPE, not hash: the 32c probe
        // showed the learned hash never appearing -- shader variants move
        // with graphics settings and headsets, but "the dispatch writing
        // an eye-scale R10G10B10A2 texture" is the pack pass on any rig.
        ID3D11UnorderedAccessView* uav = nullptr;
        ctx->CSGetUnorderedAccessViews(0, 1, &uav);
        if (!uav) return;
        ID3D11Resource* res = nullptr;
        uav->GetResource(&res);
        uav->Release();
        if (!res) return;
        bool isOutput = false;
        {
            ID3D11Texture2D* tex = nullptr;
            res->QueryInterface(__uuidof(ID3D11Texture2D),
                                reinterpret_cast<void**>(&tex));
            if (tex) {
                D3D11_TEXTURE2D_DESC td{};
                tex->GetDesc(&td);
                isOutput = td.Width >= 3000 &&
                           td.Format == DXGI_FORMAT_R10G10B10A2_UNORM;
                tex->Release();
            }
        }
        // Pipeline reconnaissance: during scanner frames, name the first
        // several dispatch outputs outright -- the reconstruction stopped
        // appearing between two same-day sessions, and what runs INSTEAD
        // is the question.
        if (seriesLive && g_occComp != 0) {
            static int s_seen = 0;
            if (s_seen < 8) {
                ID3D11Texture2D* dtex = nullptr;
                res->QueryInterface(__uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&dtex));
                if (dtex) {
                    D3D11_TEXTURE2D_DESC dd{};
                    dtex->GetDesc(&dd);
                    ID3D11ComputeShader* dcs = nullptr;
                    ctx->CSGetShader(&dcs, nullptr, nullptr);
                    const uint64_t dh = lookupShaderHash(dcs);
                    if (dcs) dcs->Release();
                    ++s_seen;
                    Log::get().note(
                        "fss series: scanner dispatch %d writes %ux%u "
                        "fmt=%u ch=%016llX", s_seen, dd.Width, dd.Height,
                        static_cast<unsigned>(dd.Format),
                        static_cast<unsigned long long>(dh));
                    dtex->Release();
                }
            }
        }
        if (isOutput) {
            static bool s_matchNoted = false;
            if (!s_matchNoted && seriesLive) {
                s_matchNoted = true;
                Log::get().note("fss series: first output dispatch matched "
                                "(by shape).");
            }
            const uint8_t occ = ++g_occOut;
            if (occ <= kEyes) {
                {
                    if (g_dumping) captureResource(ctx, res, 8, occ - 1);
                    // The series gates on the scanner itself: the body
                    // composites draw before the reconstruction in every
                    // frame, so a zero count here means a non-FSS frame.
                    if (seriesLive && g_occComp != 0) {
                        static bool s_capNoted = false;
                        if (!s_capNoted) {
                            s_capNoted = true;
                            Log::get().note("fss series: capturing.");
                        }
                        seriesCapture(ctx, res, occ - 1);
                    } else if (seriesLive) {
                        static bool s_blockNoted = false;
                        if (!s_blockNoted) {
                            s_blockNoted = true;
                            Log::get().note(
                                "fss series: output dispatch seen but no "
                                "composite counted this frame -- the "
                                "scanner gate never opened for the series. "
                                "Said once.");
                        }
                    }
                }
            }
        }
        res->Release();
    });
}

void fssDumpFrameBoundary(ID3D11DeviceContext* ctx) {
    if (g_seriesWant && !g_seriesDone && ctx &&
        g_seriesCount[0] >= g_seriesWant && g_seriesCount[1] >= g_seriesWant) {
        guardedBudget(g_budget, [&] { seriesWrite(ctx); });
        g_seriesDone = true;
        seriesRelease();
    }
    const bool sawBody = g_occRing != 0 || g_occComp != 0;
    g_occRing = 0;
    g_occComp = 0;
    g_occTone = 0;
    g_occAccum = 0;
    g_occOut = 0;
    if (!fssDumpWantsDraws() || !ctx) return;
    guardedBudget(g_budget, [&] {
        if (g_dumping) {
            // The dump frame just ended: frame-end captures of the HDR and
            // LDR textures remembered mid-frame, then write it all.
            for (uint32_t e = 0; e < kEyes; ++e) {
                captureRemembered(ctx, g_eyeTex[e], 5, e);
                captureRemembered(ctx, g_ldrTex[e], 6, e);
            }
            writeOut(ctx, g_dumpPass);
            if (g_dumpPass == 0) {
                g_dumpPass = 1;   // the very next frame is pass two
                return;
            }
            g_dumping = false;
            g_done = true;
            releaseAll();
            return;
        }
        if (sawBody) {
            ++g_bodyFrames;
            if (!g_armedNoted) {
                g_armedNoted = true;
                Log::get().note("fss dump: body frames counting.");
            }
            if (g_bodyFrames == g_dumpFrame) g_dumping = true;
        }
    });
}

void fssDumpShutdown() {
    releaseAll();
    seriesRelease();
    if (g_seriesCs) {
        g_seriesCs->Release();
        g_seriesCs = nullptr;
    }
}

}  // namespace edvr

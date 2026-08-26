#include "fss_ring.h"

#include <windows.h>

#include <d3d11.h>

#include <string>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "exposure_fix.h"   // lookupShaderHash
#include "shader_swap.h"    // shaderSwapCompilePs: the paved compile path

namespace edvr {
namespace {

// The three ring-draw families, by vertex-shader content hash, with how many
// times each runs PER EYE per frame (measured 30/30 in the round-16/17
// captures: the n=4 quad and the n=14 mesh once per eye, the f60 writer
// twice per eye). Occurrences 1..k are the first eye, k+1..2k the second;
// anything past 2k passes through untouched.
struct Family {
    uint64_t vh;
    uint8_t  perEye;
};
constexpr Family kFamilies[] = {
    {0x7E38A6AA1269C901ull, 1},
    {0x0357BBB2DEE43C1Full, 1},
    {0x53211E8C072CD02Eull, 2},
};
constexpr uint32_t kFamilyCount =
    static_cast<uint32_t>(sizeof(kFamilies) / sizeof(kFamilies[0]));
constexpr uint32_t kMaxPerEye = 2;
constexpr uint32_t kSlots = 8;
constexpr uint32_t kCbSlots = 4;

// 0 = stock, 1 = "second" (first eye receives, second lends),
// 2 = "first" (second eye receives, first lends), 3 = "depth" (nothing is
// fed; every matched draw in BOTH eyes runs with depth and stencil tests
// off -- the round-19 constants feed was engaged-and-null, and per-eye
// depth/stencil culling is the last per-draw channel these draws own),
// 4 = "mask255" / 5 = "mask0": the FIX ARMS. The eye-image dump and the
// stock-install control settled the arc: the black-square dissolve is
// painted into BOTH eyes by the ring quad from its reveal MASK at PS
// slot 3, and the crisp-vs-smooth eye split is born below the game.
// Substituting a flat fully-revealed mask at exactly those draws removes
// the black-square phase in both eyes and leaves the progressive
// sharpening (which lives in the ring's scene render) untouched. Which
// byte means "revealed" depends on the shader's compare direction --
// fss_scan_level's lesson -- so both polarities ship and one look names
// the right one.
uint8_t g_mode = 0;

// Mode 6 = "clean": the surgical fix. The ring quad's own pixel shader,
// transcribed from docs/fss-ring-ps.asm with the flag byte's bit 4 -- the
// dissolve's hard-black unrevealed state, THE black squares -- treated as
// never set. The soft fade and every lighting term are unchanged; the
// desk-compile matches the stock output signature exactly. Compiled once
// at first engage on the paved shader_swap path; null means draw stock.
constexpr char kCleanPsHlsl[] = R"HLSL(
// The FSS ring quad's pixel shader (ps 7CECABDE34FFBE9E), transcribed
// mechanically from docs/fss-ring-ps.asm with ONE semantic change: the
// flag byte's bit 4 -- the dissolve's hard-black "unrevealed" state, which
// collapses the exposure lerp to zero and paints the black squares -- is
// treated as never set. The soft cb2[46] fade-in and every lighting term
// are transcribed unchanged.

Texture2D<float4> t0 : register(t0);   // x = ao, yz = packed normal
Texture2D<float4> t1 : register(t1);   // rgb = albedo, a*255 = flag byte
Texture2D<float4> t2 : register(t2);   // material (loaded .yzxw)
Texture2D<float4> t3 : register(t3);   // illumination
cbuffer CB2 : register(b2) { float4 c[47]; }

struct PsIn {
    float2 uv   : TEXCOORD0;
    float3 view : TEXCOORD1;
};
struct PsOut {
    float3 c0 : SV_Target0;
    float  c1 : SV_Target1;
};

PsOut main(PsIn i) {
    float2 fp = floor(i.uv * c[1].xy);
    uint2  lim = uint2(c[1].xy + float2(-1.0, -1.0));
    uint2  p = min(lim, uint2(fp));

    float4 alb = t1.Load(int3(p, 0));
    uint flags = (uint)(alb.w * 255.0 + 0.5);

    float3 na = t0.Load(int3(p, 0)).xyz;
    float2 n2 = na.yz * 4.0 - 2.0;
    float  d2 = dot(n2, n2);
    float  tz = sqrt(max(1.0 - d2 * 0.25, 0.0));
    float3 ns = normalize(float3(n2 * tz, d2 * 0.5 - 1.0));
    float3 N = ns.x * c[2].xyz + ns.y * c[3].xyz + ns.z * c[4].xyz;
    float  ao = clamp(na.x, 0.05, 1.0);

    float4 mat = t2.Load(int3(p, 0)).yzxw;

    float atten, attenS;
    if (flags & 128u) {
        atten = 1.0;
        attenS = 1.0;
    } else {
        if (flags & 64u) mat.xy = mat.zz;
        float fade = 1.0 - mat.w;
        atten  = 1.0 - fade * c[46].x;
        attenS = 1.0 - fade * c[46].y;
    }

    float3 V = normalize(i.view);

    // Stock: float blackFlag = (flags & 4u) ? 0.0 : 1.0;  -- THE SQUARES.
    const float blackFlag = 1.0;

    if (((flags & 32u) != 0u) && dot(-c[42].xyz, N) < 0.0) N = -N;

    float3 H = normalize(-V - c[42].xyz);
    float NdL = saturate(dot(N, -c[42].xyz));
    float NdV = saturate(dot(N, -V));
    float NdH = saturate(dot(N, H));

    float  dif = NdL * 0.318310;
    float3 diffuse = alb.xyz * dif;

    float ao2 = ao * ao;
    float ao4 = ao2 * ao2;
    float den = min((NdH * ao4 - NdH) * NdH + 1.000001, 1.0);
    den = den * den * 3.141592;
    float D = ao4 / den;

    float fh = 1.0 - abs(dot(-c[42].xyz, H));
    float f2 = fh * fh;
    float fres = fh * (f2 * f2);
    float3 F = mat.zxy + (1.0 - mat.zxy) * fres;

    float visL = NdL * (2.0 - ao2) + ao2;
    float visV = NdV * (2.0 - ao2) + ao2;
    float3 spec = (D / (visL * visV)) * F * NdL * attenS;

    float3 col = (diffuse * atten + spec) * c[40].xyz;
    float3 aux = dif * c[40].xyz;

    float lum = dot(t3.Load(int3(p, 0)).xyz, c[44].xyz);
    float scale = (c[36].x * (blackFlag - lum) + lum) * c[36].y;

    PsOut o;
    o.c0 = scale * col * c[0].x;
    o.c1 = dot(scale * aux, float3(0.308600, 0.609400, 0.082000)) * c[0].x;
    return o;
}
)HLSL";

ID3D11PixelShader* g_cleanPs = nullptr;
bool               g_cleanTried = false;
ID3D11PixelShader* g_savedPs = nullptr;
bool               g_psEngaged = false;

// The fade patch, clean mode's second half. Both ring shaders darken
// unrevealed pixels through atten = 1 - (1 - fade) * cb2[46].xy -- the
// QUAD alongside its bit-4 hard black, the MESH as its only black path
// (docs/fss-ring-mesh-ps.asm) -- so a fade value of zero paints the
// squares even with bit 4 gone. Zeroing cb2[46].xy forces atten to 1 in
// both shaders without touching either's lighting: the game's b2 is
// GPU-copied into an EDVR buffer, vector 46's xy is overwritten from an
// 8-byte zero buffer by region copy, and the copy is bound for exactly
// the matched draws. No CPU readback, no stalls, restored per draw.
constexpr uint32_t kFadeVecOffset = 46 * 16;
ID3D11Buffer* g_zeroBuf = nullptr;      // 8 bytes of zeros
ID3D11Buffer* g_patchedCb = nullptr;    // sized to the game's b2
uint32_t      g_patchedBytes = 0;
ID3D11Buffer* g_savedCb2 = nullptr;
bool          g_cbEngaged = false;
bool          g_fadeFailedNoted = false;

// The flat mask, built to the displaced view's size on first engage.
ID3D11Texture2D*          g_maskTex = nullptr;
ID3D11ShaderResourceView* g_maskSrv = nullptr;
uint32_t g_maskW = 0, g_maskH = 0;
uint8_t  g_maskByte = 0;
bool     g_maskFailedNoted = false;

ID3D11DepthStencilState* g_noDepth = nullptr;
ID3D11DepthStencilState* g_savedDepth = nullptr;
UINT                     g_savedStencilRef = 0;
bool                     g_depthEngaged = false;

// The learned SRV sets, AddRef-held across frames: [family][j][slot], where
// j is the occurrence index within the lending eye.
ID3D11ShaderResourceView* g_learned[kFamilyCount][kMaxPerEye][kSlots] = {};
bool g_learnedValid[kFamilyCount][kMaxPerEye] = {};

// The lender's PS constant contents, captured by GPU CopyResource at its
// draws into EDVR-owned buffers -- the command stream orders the copy
// against the draw, so the bytes are exactly the ones the lender read. The
// round-18 texture feed alone came back null: the ring quad's pixel shader
// runs its own ordered dissolve, and a per-eye PROGRESS in its constants is
// the composite's cb1[119] story one shader over. Objects would not do --
// the game may rewrite one buffer between the eyes' draws.
ID3D11Buffer* g_cbCopy[kFamilyCount][kMaxPerEye][kCbSlots] = {};
uint32_t      g_cbCopyBytes[kFamilyCount][kMaxPerEye][kCbSlots] = {};
bool          g_cbValid[kFamilyCount][kMaxPerEye][kCbSlots] = {};

// Per-frame occurrence counters, and the pending apply between OnEyeDraw
// and Begin (the draw path is single-threaded; the pattern every fss module
// here uses).
uint8_t  g_occ[kFamilyCount] = {};
uint32_t g_pendingFam = 0;
uint32_t g_pendingJ = 0;

// Begin/End state: the displaced live views, to restore.
bool                      g_engaged = false;
ID3D11ShaderResourceView* g_displaced[kSlots] = {};
ID3D11Buffer*             g_displacedCb[kCbSlots] = {};

uint64_t g_applied = 0;
bool     g_engagedNoted = false;
bool     g_learnNoted = false;

FaultBudget g_budget("fssRing", 8);

void releaseLearned() {
    for (uint32_t f = 0; f < kFamilyCount; ++f) {
        for (uint32_t j = 0; j < kMaxPerEye; ++j) {
            for (uint32_t s = 0; s < kSlots; ++s) {
                if (g_learned[f][j][s]) {
                    g_learned[f][j][s]->Release();
                    g_learned[f][j][s] = nullptr;
                }
            }
            g_learnedValid[f][j] = false;
            for (uint32_t s = 0; s < kCbSlots; ++s) {
                if (g_cbCopy[f][j][s]) {
                    g_cbCopy[f][j][s]->Release();
                    g_cbCopy[f][j][s] = nullptr;
                }
                g_cbCopyBytes[f][j][s] = 0;
                g_cbValid[f][j][s] = false;
            }
        }
    }
}

}  // namespace

void fssRingConfigure(Config& cfg) {
    const std::string m = cfg.getString("experimental.fss_ring_feed", "stock");
    uint8_t mode = 0;
    if (m == "stock") {
        mode = 0;
    } else if (m == "second") {
        mode = 1;
    } else if (m == "first") {
        mode = 2;
    } else if (m == "depth") {
        mode = 3;
    } else if (m == "mask255") {
        mode = 4;
    } else if (m == "mask0") {
        mode = 5;
    } else if (m == "clean") {
        mode = 6;
    } else {
        Log::get().note("fss_ring_feed \"%s\" is not stock, second, first, "
                        "depth, mask255, mask0 or clean; running stock.",
                        m.c_str());
    }
    if (mode == g_mode) return;
    g_mode = mode;
    g_engagedNoted = false;
    g_learnNoted = false;
    releaseLearned();
    if (g_mode == 6) {
        Log::get().note(
            "fss ring ARMED: the ring quad draws through EDVR's transcribed "
            "pixel shader with the dissolve's hard-black state removed -- "
            "the flag that painted the black squares is ignored, the soft "
            "fade and the lighting are the game's own. Compiles at the "
            "first zoom; a compile failure is logged and the game draws "
            "stock. Clear to restore.");
    } else if (g_mode >= 4) {
        Log::get().note(
            "fss ring ARMED: the ring quad's reveal mask (PS slot 3) is "
            "replaced with flat %u for exactly those draws, both eyes -- "
            "every cell reads as one reveal state. If the ring appears "
            "WHOLE (no black squares) this polarity is the fix; if it "
            "VANISHES or never resolves, flip to the other. Clear to "
            "restore.",
            g_mode == 4 ? 255u : 0u);
    } else if (g_mode == 3) {
        Log::get().note(
            "fss ring ARMED: every ring-family draw runs with depth and "
            "stencil tests OFF, both eyes. Squares dying here means the "
            "left eye's squares are ring pixels CULLED by per-eye "
            "depth/stencil content. Clear to restore.");
    } else if (g_mode) {
        Log::get().note(
            "fss ring ARMED: the %s eye's ring draws run with the %s eye's "
            "sampled inputs AND pixel-stage constants, restored after every "
            "draw. The zoomed body sits at optical infinity, so one eye's "
            "imagery is correct for both -- if the black squares die, the "
            "lagging per-eye state is measured and this is the fix's shape. "
            "Clear to restore.",
            g_mode == 1 ? "FIRST" : "SECOND", g_mode == 1 ? "SECOND" : "FIRST");
    } else {
        Log::get().note("fss ring: stock; each eye's ring draws read their "
                        "own inputs (%llu draws were fed while armed).",
                        static_cast<unsigned long long>(g_applied));
    }
}

bool fssRingWantsDraws() { return g_mode != 0; }

bool fssRingOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                      uint32_t instances) {
    if (!g_mode || !ctx) return false;
    // The cheap gate first: every family is a tiny fixed-geometry draw.
    if (!((kind == 'N' && instances == 1 && (count == 3 || count == 4)) ||
          (kind == 'X' && count == 14))) {
        return false;
    }
    uint64_t h = 0;
    guardedBudget(g_budget, [&] {
        ID3D11VertexShader* vs = nullptr;
        ctx->VSGetShader(&vs, nullptr, nullptr);
        h = lookupShaderHash(vs);
        if (vs) vs->Release();
    });
    for (uint32_t f = 0; f < kFamilyCount; ++f) {
        if (kFamilies[f].vh != h) continue;
        const uint8_t k = kFamilies[f].perEye;
        const uint8_t occ = ++g_occ[f];
        if (occ > 2 * k) return false;   // unexpected extra: leave alone
        const uint8_t eye = static_cast<uint8_t>((occ - 1) / k);
        const uint8_t j = static_cast<uint8_t>((occ - 1) % k);
        if (g_mode == 3) {
            // Depth mode: no learning, no feeding; every matched draw in
            // both eyes gets the no-test state in Begin/End.
            g_pendingFam = f;
            g_pendingJ = j;
            return true;
        }
        if (g_mode >= 4) {
            // Mask modes touch only the quad. Clean covers the quad (bit-4
            // shader plus fade patch) and the mesh (fade patch alone --
            // docs/fss-ring-mesh-ps.asm has no bit-4 path); the f60 mask
            // writer passes through untouched.
            if (g_mode == 6 ? f > 1 : f != 0) return false;
            g_pendingFam = f;
            g_pendingJ = j;
            return true;
        }
        const uint8_t lender = (g_mode == 1) ? 1 : 0;
        if (eye == lender) {
            // Learn, inline: a read costs nothing the draw can notice.
            guardedBudget(g_budget, [&] {
                ID3D11ShaderResourceView* live[kSlots] = {};
                ctx->PSGetShaderResources(0, kSlots, live);
                for (uint32_t s = 0; s < kSlots; ++s) {
                    if (g_learned[f][j][s]) g_learned[f][j][s]->Release();
                    g_learned[f][j][s] = live[s];   // keep the Get's ref
                }
                // The constants, by content: snapshot each bound PS buffer
                // into an EDVR copy on the GPU timeline, sized on demand.
                ID3D11Buffer* cbs[kCbSlots] = {};
                ctx->PSGetConstantBuffers(0, kCbSlots, cbs);
                for (uint32_t s = 0; s < kCbSlots; ++s) {
                    g_cbValid[f][j][s] = false;
                    if (!cbs[s]) continue;
                    D3D11_BUFFER_DESC bd{};
                    cbs[s]->GetDesc(&bd);
                    if (!g_cbCopy[f][j][s] ||
                        g_cbCopyBytes[f][j][s] != bd.ByteWidth) {
                        if (g_cbCopy[f][j][s]) {
                            g_cbCopy[f][j][s]->Release();
                            g_cbCopy[f][j][s] = nullptr;
                        }
                        ID3D11Device* dev = nullptr;
                        ctx->GetDevice(&dev);
                        if (dev) {
                            D3D11_BUFFER_DESC nd{};
                            nd.ByteWidth = bd.ByteWidth;
                            nd.Usage = D3D11_USAGE_DEFAULT;
                            nd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                            dev->CreateBuffer(&nd, nullptr,
                                              &g_cbCopy[f][j][s]);
                            dev->Release();
                        }
                        g_cbCopyBytes[f][j][s] =
                            g_cbCopy[f][j][s] ? bd.ByteWidth : 0;
                    }
                    if (g_cbCopy[f][j][s]) {
                        ctx->CopyResource(g_cbCopy[f][j][s], cbs[s]);
                        g_cbValid[f][j][s] = true;
                    }
                    cbs[s]->Release();
                }
                g_learnedValid[f][j] = true;
                if (!g_learnNoted) {
                    g_learnNoted = true;
                    Log::get().note("fss ring: lender inputs learned; the "
                                    "receiving eye is fed from the next "
                                    "matching draw on.");
                }
            });
            return false;
        }
        if (!g_learnedValid[f][j]) return false;
        g_pendingFam = f;
        g_pendingJ = j;
        return true;
    }
    return false;
}

void fssRingBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    g_depthEngaged = false;
    g_psEngaged = false;
    if (!ctx || !g_mode) return;
    if (g_mode == 6) {
        guardedBudget(g_budget, [&] {
            // The fade patch, for quad and mesh alike.
            ID3D11Buffer* gameCb = nullptr;
            ctx->PSGetConstantBuffers(2, 1, &gameCb);
            if (gameCb) {
                D3D11_BUFFER_DESC bd{};
                gameCb->GetDesc(&bd);
                if (bd.ByteWidth >= kFadeVecOffset + 8) {
                    ID3D11Device* dev = nullptr;
                    if (!g_zeroBuf || !g_patchedCb ||
                        g_patchedBytes != bd.ByteWidth) {
                        if (g_patchedCb) {
                            g_patchedCb->Release();
                            g_patchedCb = nullptr;
                        }
                        ctx->GetDevice(&dev);
                        if (dev) {
                            if (!g_zeroBuf) {
                                const uint32_t zeros[4] = {};
                                D3D11_BUFFER_DESC zd{};
                                zd.ByteWidth = 16;
                                zd.Usage = D3D11_USAGE_IMMUTABLE;
                                zd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                                D3D11_SUBRESOURCE_DATA zi{};
                                zi.pSysMem = zeros;
                                dev->CreateBuffer(&zd, &zi, &g_zeroBuf);
                            }
                            D3D11_BUFFER_DESC pd{};
                            pd.ByteWidth = bd.ByteWidth;
                            pd.Usage = D3D11_USAGE_DEFAULT;
                            pd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                            dev->CreateBuffer(&pd, nullptr, &g_patchedCb);
                            dev->Release();
                            g_patchedBytes = g_patchedCb ? bd.ByteWidth : 0;
                            if ((!g_zeroBuf || !g_patchedCb) &&
                                !g_fadeFailedNoted) {
                                g_fadeFailedNoted = true;
                                Log::get().note(
                                    "fss ring: fade-patch buffers failed; "
                                    "the fade stays stock.");
                            }
                        }
                    }
                    if (g_zeroBuf && g_patchedCb) {
                        ctx->CopyResource(g_patchedCb, gameCb);
                        D3D11_BOX box{};
                        box.left = 0;
                        box.right = 8;
                        box.top = 0;
                        box.bottom = 1;
                        box.front = 0;
                        box.back = 1;
                        ctx->CopySubresourceRegion(g_patchedCb, 0,
                                                   kFadeVecOffset, 0, 0,
                                                   g_zeroBuf, 0, &box);
                        g_savedCb2 = gameCb;   // keep the Get's ref
                        gameCb = nullptr;
                        ID3D11Buffer* ours = g_patchedCb;
                        ctx->PSSetConstantBuffers(2, 1, &ours);
                        g_cbEngaged = true;
                    }
                }
                if (gameCb) gameCb->Release();
            }

            // The cleaned shader, for the quad alone (bit-4 lives there).
            if (g_pendingFam == 0) {
                if (!g_cleanPs && !g_cleanTried) {
                    g_cleanTried = true;
                    g_cleanPs = shaderSwapCompilePs(
                        ctx, kCleanPsHlsl, sizeof(kCleanPsHlsl) - 1, "main",
                        "fss_ring_clean_ps", nullptr, "fss ring clean");
                }
                if (g_cleanPs) {
                    ctx->PSGetShader(&g_savedPs, nullptr, nullptr);
                    ctx->PSSetShader(g_cleanPs, nullptr, 0);
                    g_psEngaged = true;
                }
            }
            if (g_psEngaged || g_cbEngaged) {
                ++g_applied;
                if (!g_engagedNoted) {
                    g_engagedNoted = true;
                    Log::get().note(
                        "fss ring: engaged -- quad and mesh draw with the "
                        "fade zeroed%s, restored after each draw.",
                        g_psEngaged ? " and the quad through the cleaned "
                                      "shader" : "");
                }
            }
        });
        return;
    }
    if (g_mode >= 4) {
        guardedBudget(g_budget, [&] {
            // Size the flat mask to whatever the game bound at slot 3.
            ID3D11ShaderResourceView* cur = nullptr;
            ctx->PSGetShaderResources(3, 1, &cur);
            uint32_t w = 0, h = 0;
            if (cur) {
                ID3D11Resource* res = nullptr;
                cur->GetResource(&res);
                if (res) {
                    ID3D11Texture2D* tex = nullptr;
                    res->QueryInterface(__uuidof(ID3D11Texture2D),
                                        reinterpret_cast<void**>(&tex));
                    if (tex) {
                        D3D11_TEXTURE2D_DESC td{};
                        tex->GetDesc(&td);
                        w = td.Width;
                        h = td.Height;
                        tex->Release();
                    }
                    res->Release();
                }
            }
            const uint8_t want = g_mode == 4 ? 0xFF : 0x00;
            if (!w || !h) {
                if (cur) cur->Release();
                return;
            }
            if (!g_maskSrv || g_maskW != w || g_maskH != h ||
                g_maskByte != want) {
                if (g_maskSrv) {
                    g_maskSrv->Release();
                    g_maskSrv = nullptr;
                }
                if (g_maskTex) {
                    g_maskTex->Release();
                    g_maskTex = nullptr;
                }
                ID3D11Device* dev = nullptr;
                ctx->GetDevice(&dev);
                if (dev) {
                    std::string fill(static_cast<size_t>(w) * h,
                                     static_cast<char>(want));
                    D3D11_TEXTURE2D_DESC td{};
                    td.Width = w;
                    td.Height = h;
                    td.MipLevels = 1;
                    td.ArraySize = 1;
                    td.Format = DXGI_FORMAT_R8_UNORM;
                    td.SampleDesc.Count = 1;
                    td.Usage = D3D11_USAGE_IMMUTABLE;
                    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    D3D11_SUBRESOURCE_DATA init{};
                    init.pSysMem = fill.data();
                    init.SysMemPitch = w;
                    HRESULT hr = dev->CreateTexture2D(&td, &init, &g_maskTex);
                    if (SUCCEEDED(hr) && g_maskTex) {
                        hr = dev->CreateShaderResourceView(g_maskTex, nullptr,
                                                           &g_maskSrv);
                    }
                    dev->Release();
                    if (FAILED(hr) || !g_maskSrv) {
                        if (g_maskTex) {
                            g_maskTex->Release();
                            g_maskTex = nullptr;
                        }
                        if (!g_maskFailedNoted) {
                            g_maskFailedNoted = true;
                            Log::get().note(
                                "fss ring: could not create the %ux%u flat "
                                "mask (hr=0x%08X); the ring draws stock.",
                                w, h, static_cast<unsigned>(hr));
                        }
                        if (cur) cur->Release();
                        return;
                    }
                    g_maskW = w;
                    g_maskH = h;
                    g_maskByte = want;
                }
            }
            if (!g_maskSrv) {
                if (cur) cur->Release();
                return;
            }
            g_displaced[3] = cur;   // keep the Get's ref; may be null
            ID3D11ShaderResourceView* sub = g_maskSrv;
            ctx->PSSetShaderResources(3, 1, &sub);
            g_engaged = true;
            ++g_applied;
            if (!g_engagedNoted) {
                g_engagedNoted = true;
                Log::get().note(
                    "fss ring: engaged -- the ring quad's reveal mask is "
                    "flat %u for exactly those draws, restored after each.",
                    g_maskByte);
            }
        });
        return;
    }
    if (g_mode == 3) {
        guardedBudget(g_budget, [&] {
            if (!g_noDepth) {
                ID3D11Device* dev = nullptr;
                ctx->GetDevice(&dev);
                if (!dev) return;
                D3D11_DEPTH_STENCIL_DESC dd{};   // depth off, stencil off
                dev->CreateDepthStencilState(&dd, &g_noDepth);
                dev->Release();
                if (!g_noDepth) return;
            }
            ctx->OMGetDepthStencilState(&g_savedDepth, &g_savedStencilRef);
            ctx->OMSetDepthStencilState(g_noDepth, 0);
            g_depthEngaged = true;
            ++g_applied;
            if (!g_engagedNoted) {
                g_engagedNoted = true;
                Log::get().note("fss ring: engaged -- ring-family draws are "
                                "running with depth and stencil off, "
                                "restored after each.");
            }
        });
        return;
    }
    guardedBudget(g_budget, [&] {
        ctx->PSGetShaderResources(0, kSlots, g_displaced);
        ID3D11ShaderResourceView* feed[kSlots];
        for (uint32_t s = 0; s < kSlots; ++s) {
            feed[s] = g_learned[g_pendingFam][g_pendingJ][s];
        }
        ctx->PSSetShaderResources(0, kSlots, feed);
        ctx->PSGetConstantBuffers(0, kCbSlots, g_displacedCb);
        ID3D11Buffer* cfeed[kCbSlots];
        for (uint32_t s = 0; s < kCbSlots; ++s) {
            cfeed[s] = g_cbValid[g_pendingFam][g_pendingJ][s]
                           ? g_cbCopy[g_pendingFam][g_pendingJ][s]
                           : g_displacedCb[s];
        }
        ctx->PSSetConstantBuffers(0, kCbSlots, cfeed);
        g_engaged = true;
        ++g_applied;
        if (!g_engagedNoted) {
            g_engagedNoted = true;
            Log::get().note(
                "fss ring: engaged -- the receiving eye's ring draws are "
                "running on the lending eye's inputs, restored after each.");
        }
    });
}

void fssRingEnd(ID3D11DeviceContext* ctx) {
    if ((g_psEngaged || g_cbEngaged) && ctx && g_mode == 6) {
        if (g_psEngaged) {
            g_psEngaged = false;
            ctx->PSSetShader(g_savedPs, nullptr, 0);
            if (g_savedPs) {
                g_savedPs->Release();
                g_savedPs = nullptr;
            }
        }
        if (g_cbEngaged) {
            g_cbEngaged = false;
            ctx->PSSetConstantBuffers(2, 1, &g_savedCb2);
            if (g_savedCb2) {
                g_savedCb2->Release();
                g_savedCb2 = nullptr;
            }
        }
        return;
    }
    if (g_psEngaged && ctx) {
        g_psEngaged = false;
        ctx->PSSetShader(g_savedPs, nullptr, 0);
        if (g_savedPs) {
            g_savedPs->Release();
            g_savedPs = nullptr;
        }
        return;
    }
    if (g_engaged && ctx && g_mode >= 4) {
        g_engaged = false;
        ctx->PSSetShaderResources(3, 1, &g_displaced[3]);
        if (g_displaced[3]) {
            g_displaced[3]->Release();
            g_displaced[3] = nullptr;
        }
        return;
    }
    if (g_depthEngaged && ctx) {
        g_depthEngaged = false;
        ctx->OMSetDepthStencilState(g_savedDepth, g_savedStencilRef);
        if (g_savedDepth) {
            g_savedDepth->Release();
            g_savedDepth = nullptr;
        }
        return;
    }
    if (!g_engaged || !ctx) return;
    g_engaged = false;
    ctx->PSSetShaderResources(0, kSlots, g_displaced);
    for (uint32_t s = 0; s < kSlots; ++s) {
        if (g_displaced[s]) {
            g_displaced[s]->Release();
            g_displaced[s] = nullptr;
        }
    }
    ctx->PSSetConstantBuffers(0, kCbSlots, g_displacedCb);
    for (uint32_t s = 0; s < kCbSlots; ++s) {
        if (g_displacedCb[s]) {
            g_displacedCb[s]->Release();
            g_displacedCb[s] = nullptr;
        }
    }
}

void fssRingFrameBoundary() {
    for (uint32_t f = 0; f < kFamilyCount; ++f) g_occ[f] = 0;
}

void fssRingShutdown() {
    releaseLearned();
    if (g_noDepth) {
        g_noDepth->Release();
        g_noDepth = nullptr;
    }
    if (g_maskSrv) {
        g_maskSrv->Release();
        g_maskSrv = nullptr;
    }
    if (g_maskTex) {
        g_maskTex->Release();
        g_maskTex = nullptr;
    }
    if (g_cleanPs) {
        g_cleanPs->Release();
        g_cleanPs = nullptr;
    }
    if (g_zeroBuf) {
        g_zeroBuf->Release();
        g_zeroBuf = nullptr;
    }
    if (g_patchedCb) {
        g_patchedCb->Release();
        g_patchedCb = nullptr;
    }
}

}  // namespace edvr

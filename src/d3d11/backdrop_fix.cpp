#include "backdrop_fix.h"

#include <windows.h>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "binding_shadow.h"
#include "journal_watch.h"
#include "shader_swap.h"

namespace edvr {
namespace {

// One deband pass. The kernel is run several times at growing radius, each
// reading the previous pass's output, because that is what the offline
// simulation this was validated against did -- widening the search for a
// flat neighbourhood without ever widening the blur applied inside one.
constexpr char kBackdropCsHlsl[] = R"HLSL(
Texture2D<float4>   S : register(t0);
RWTexture2D<float4> O : register(u0);
cbuffer P : register(b0) {
    float4 p;   // x = radius in texels, y = flatness threshold, z = dither
};

float mx3(float3 v) { return max(max(v.x, v.y), v.z); }

// Interleaved gradient noise: a pure function of position. The bake is
// therefore deterministic, and since ONE texture feeds both eyes the dither
// cannot differ between them -- the failure mode the FSS arc spent forty
// rounds on, absent here by construction rather than by care.
float ign(float2 q) {
    return frac(52.9829189 * frac(dot(q, float2(0.06711056, 0.00583715))));
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    O.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;

    int2 c0 = int2(id.xy);
    int  r  = int(p.x);
    int2 lo = int2(0, 0);
    int2 hi = int2(int(w) - 1, int(h) - 1);

    float4 c  = S[c0];
    float3 n0 = S[clamp(c0 + int2( r,  0), lo, hi)].rgb;
    float3 n1 = S[clamp(c0 + int2(-r,  0), lo, hi)].rgb;
    float3 n2 = S[clamp(c0 + int2( 0,  r), lo, hi)].rgb;
    float3 n3 = S[clamp(c0 + int2( 0, -r), lo, hi)].rgb;

    // The threshold is the whole difference between a deband and a blur. A
    // star, a hull edge, any real structure exceeds it and passes through
    // untouched; only a neighbourhood already flat to within a quantization
    // step or two is averaged -- which is exactly where the step between two
    // block endpoints shows as a contour.
    float d = max(max(mx3(abs(n0 - c.rgb)), mx3(abs(n1 - c.rgb))),
                  max(mx3(abs(n2 - c.rgb)), mx3(abs(n3 - c.rgb))));
    // A SOFT weight, not a hard switch. "d < threshold ? average : centre"
    // makes adjacent pixels land on opposite sides of a cliff, and five
    // chained passes bake each cliff in and re-average it -- which the first
    // field run saw as blotches that are not in the source. Fading the
    // average out as the neighbourhood stops being flat has no boundary to
    // see, and at d = 0 it is still the full average.
    float flat = saturate(1.0 - d / max(p.y, 1e-6));
    float3 o = lerp(c.rgb, (n0 + n1 + n2 + n3) * 0.25, flat);

    // Dither on the final pass only: about one LSB, enough to break the last
    // residual contour and far below what reads as noise.
    if (p.z > 0.0) o += (ign(float2(c0)) - 0.5) * p.z;

    O[id.xy] = float4(saturate(o), c.a);
}
)HLSL";

// Radius and threshold multiplier per pass, in order. Five passes reaching
// sixteen texels: at the magnification this still is shown at, a contour is
// several screen pixels wide and a one-texel search never finds its far side.
struct Pass {
    int   radius;
    float scale;
};
constexpr Pass kPasses[] = {
    {1, 1.00f}, {2, 0.80f}, {4, 0.60f}, {8, 0.50f}, {16, 0.40f}};
constexpr int kPassCount = static_cast<int>(sizeof(kPasses) / sizeof(kPasses[0]));

// The BC1 family, typeless and both typed spellings. The census saw the
// texture created TYPELESS; the view over it decides the rest.
bool isBc1(uint32_t fmt) {
    return fmt == DXGI_FORMAT_BC1_TYPELESS || fmt == DXGI_FORMAT_BC1_UNORM ||
           fmt == DXGI_FORMAT_BC1_UNORM_SRGB;
}

bool isSrgb(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_BC1_UNORM_SRGB ||
           f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

// A still small enough to be an icon is not the backdrop. The census had
// three BC1 textures -- 1920x1080, 512x512 and 16x16 -- and this floor is
// what separates the one from the other two without pinning the exact size a
// future menu art pass might change.
constexpr uint32_t kMinWidth = 1024;
constexpr uint32_t kMinHeight = 512;

// A backdrop is WIDE. The first field run matched the 3840x2160 still, then a
// 1920x1080 one, then two different 1024x1024 ones -- square UI atlases that
// are not backdrops and should never have been touched. Both real candidates
// were 16:9 to four decimal places and both impostors were 1:1, so aspect
// separates them outright and costs one divide.
constexpr float kMinAspect = 1.70f;
constexpr float kMaxAspect = 1.85f;

// ONE BAKED TEXTURE PER STILL, and the field is what earned it.
//
// The menu has at least two backdrops -- a 3840x2160 and a 1920x1080, both
// 16:9, both sampled by the same shape of blit, the second replacing the
// first about four seconds in. A single slot rebuilt when it saw the second,
// which meant the FIRST one silently went back to stock. The field verdict on
// that build was "it looks like it originally did now", which is exactly what
// a fix that reverted the still you were looking at would look like.
//
// So each distinct source gets its own bake, kept for the session, and every
// matched draw is served the bake belonging to ITS source. Nothing reverts,
// and nothing rebuilds while the menu alternates.
//
// The cap is a runaway guard, not a policy: four is comfortably above the two
// the field found, and past it the stills already baked keep their draws while
// anything new draws stock and the log says so once.
constexpr int kMaxSlots = 4;

FaultBudget g_budget("backdrop", 8);

bool g_on = false;
// "splash": serve the BEST still we hold for every matched draw, rather than
// each draw's own.
//
// The field found the menu shows two stills in sequence -- a 3840x2160 that
// arrives first and looks good, then a 1920x1080 that replaces it about four
// seconds later and is the one people call ugly. Both are the game's own art,
// both are already loaded, and both are already baked into slots here. So the
// nicer one can simply keep the screen.
//
// Nothing is shipped or redistributed to do this: it is the game's own asset,
// used a few seconds longer than the game intended.
bool g_splash = false;
float g_threshold = 6.0f / 255.0f;
float g_dither = 1.0f / 255.0f;

// One baked still. `srcRes` is the source resource as an IDENTITY only --
// never dereferenced, only compared, the same bargain binding_shadow states
// for ResourceInfo::resource.
// At most this many blit targets per still. The census showed two (one per
// eye's composite); four leaves room without inviting a scan.
constexpr int kMaxTargets = 4;

struct Slot {
    void*                      srcRes = nullptr;
    // The render targets this still's blit wrote into, as identities. The
    // composite that later samples one of them is handed OUR bake instead,
    // which is how the engine's downsample gets bypassed -- see
    // backdropOnComposite.
    void*                      targets[kMaxTargets] = {};
    int                        targetCount = 0;
    ID3D11Texture2D*           tex = nullptr;   // what the game will sample
    ID3D11ShaderResourceView*  gameSrv = nullptr;  // ... through this view
    ID3D11ShaderResourceView*  readSrv = nullptr;  // ... and this, chaining
    ID3D11UnorderedAccessView* uav = nullptr;
    uint32_t                   w = 0, h = 0;
};
Slot g_slots[kMaxSlots];
int  g_slotsUsed = 0;

// Which slot the last matched draw belongs to, so Begin binds the bake for
// THIS draw's source rather than whichever was made last. -1 is "none".
int  g_hit = -1;
bool g_notedSplash = false;

ID3D11Texture2D*           g_tmp = nullptr;      // ping-pong scratch
ID3D11ShaderResourceView*  g_tmpRead = nullptr;
ID3D11UnorderedAccessView* g_tmpUav = nullptr;
ID3D11Buffer*              g_cb = nullptr;
ID3D11ComputeShader*       g_cs = nullptr;

bool g_rebuild = false;
bool g_notedFull = false;
bool g_notedComposite = false;
bool g_failed = false;
bool g_notedBuilt = false;

// Set by Begin so End restores exactly what it displaced, and nothing when
// Begin did nothing.
ID3D11ShaderResourceView* g_saved = nullptr;
bool g_bound = false;

// Remember the render target the currently matched blit is writing into.
// Called at every match, not only the first: the two composites read two
// different targets and both have to be known before either can be served.
void noteTarget(Slot& slot) {
    ResourceInfo rt{};
    if (!bindingResolve(bindingGet(BindSlot::Rtv0), &rt)) return;
    if (!rt.isTexture2D || !rt.resource) return;
    for (int i = 0; i < slot.targetCount; ++i) {
        if (slot.targets[i] == rt.resource) return;
    }
    if (slot.targetCount >= kMaxTargets) return;
    slot.targets[slot.targetCount++] = rt.resource;
}

// Which bake to actually bind for a matched draw. Normally the draw's own.
// In splash mode, the largest still we hold -- resolution is the one ordering
// available without judging art, and in the field it picks the 3840x2160 the
// menu shows first over the 1920x1080 that replaces it.
int chooseSlot(int hit) {
    if (!g_splash || hit < 0) return hit;
    int best = hit;
    for (int i = 0; i < g_slotsUsed; ++i) {
        if (!g_slots[i].gameSrv) continue;
        if (static_cast<uint64_t>(g_slots[i].w) * g_slots[i].h >
            static_cast<uint64_t>(g_slots[best].w) * g_slots[best].h) {
            best = i;
        }
    }
    return best;
}

void failOnce(const char* why) {
    if (g_failed) return;
    g_failed = true;
    Log::get().note("menu backdrop: %s. The backdrop draws stock for the "
                    "rest of this session.", why);
}

// The built IMAGE only. The kernel and its parameter buffer outlive it on
// purpose: they have nothing to do with which texture was baked, and dropping
// them meant a rebuild paid for D3DCompile again on the render thread. The
// first field run showed exactly that -- two "compute shader compiled" lines
// for two builds four seconds apart.
void releaseSlot(Slot& s) {
    if (s.uav) { s.uav->Release(); s.uav = nullptr; }
    if (s.readSrv) { s.readSrv->Release(); s.readSrv = nullptr; }
    if (s.gameSrv) { s.gameSrv->Release(); s.gameSrv = nullptr; }
    if (s.tex) { s.tex->Release(); s.tex = nullptr; }
    s.srcRes = nullptr;
    s.w = s.h = 0;
}

void releaseAll() {
    if (g_tmpUav) { g_tmpUav->Release(); g_tmpUav = nullptr; }
    if (g_tmpRead) { g_tmpRead->Release(); g_tmpRead = nullptr; }
    if (g_tmp) { g_tmp->Release(); g_tmp = nullptr; }
    for (Slot& s : g_slots) releaseSlot(s);
    g_slotsUsed = 0;
    g_hit = -1;
}

// Free the scratch half once the chain has finished with it. The output and
// its two views stay for the session; the ping-pong partner is dead weight.
void dropScratch() {
    if (g_tmpUav) { g_tmpUav->Release(); g_tmpUav = nullptr; }
    if (g_tmpRead) { g_tmpRead->Release(); g_tmpRead = nullptr; }
    if (g_tmp) { g_tmp->Release(); g_tmp = nullptr; }
}

// Everything the build needs, resolved from the bound view under the guard.
struct Source {
    ID3D11Resource* res = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    bool srgb = false;
    // The blit's RENDER TARGET, read at the matched draw. Not needed by the
    // pass -- needed by the log, because it answers a question two censuses
    // could not: the auto-armed one fires on the first draw into that target
    // and so lands BEFORE the still has streamed in, and a keypress lands
    // whenever a human reacted. Naming it here reports it from the one moment
    // that is certainly the right one.
    uint32_t targetW = 0;
    uint32_t targetH = 0;
};

bool setParams(ID3D11DeviceContext* ctx, float radius, float threshold,
               float dither) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) {
        return false;
    }
    const float vals[4] = {radius, threshold, dither, 0.0f};
    memcpy(m.pData, vals, sizeof(vals));
    ctx->Unmap(g_cb, 0);
    return true;
}

// Bake one still into `slot`. Runs at most once per source texture: the
// census established these assets are never written, so a second build would
// be answering a question nobody asked.
bool build(ID3D11DeviceContext* ctx, const Source& src, Slot& slot) {
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;

    bool ok = true;

    // Read the BC1 through OUR OWN view, forced non-sRGB. The whole pass runs
    // in the stored encoding: the source is decoded to the same 8-bit values
    // the game's sampler would see, debanded there, and written back
    // unconverted -- so the game-facing view below can carry the original's
    // sRGB-ness and the game's sampler does exactly what it always did. Doing
    // the arithmetic in linear instead would mean undoing a transfer function
    // on read and reapplying it on write, and a typed UAV cannot reapply it.
    ID3D11ShaderResourceView* srcRead = nullptr;
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_BC1_UNORM;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        ok = SUCCEEDED(dev->CreateShaderResourceView(src.res, &sd, &srcRead));
        if (!ok) failOnce("the source still could not be viewed as BC1");
    }

    const DXGI_FORMAT kTypeless = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    const DXGI_FORMAT kTyped = DXGI_FORMAT_R8G8B8A8_UNORM;
    const DXGI_FORMAT kGame =
        src.srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;

    if (ok) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = src.width;
        td.Height = src.height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = kTypeless;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &slot.tex)) &&
             SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_tmp));
        if (!ok) failOnce("the debanded still could not be created");
    }
    if (ok) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Format = kTyped;
        ok = SUCCEEDED(
                 dev->CreateShaderResourceView(slot.tex, &sd, &slot.readSrv)) &&
             SUCCEEDED(dev->CreateShaderResourceView(g_tmp, &sd, &g_tmpRead));
        sd.Format = kGame;
        ok = ok && SUCCEEDED(dev->CreateShaderResourceView(slot.tex, &sd,
                                                           &slot.gameSrv));
        if (!ok) failOnce("the debanded still could not be viewed");
    }
    if (ok) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = kTyped;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        ok = SUCCEEDED(
                 dev->CreateUnorderedAccessView(slot.tex, &ud, &slot.uav)) &&
             SUCCEEDED(dev->CreateUnorderedAccessView(g_tmp, &ud, &g_tmpUav));
        if (!ok) failOnce("the debanded still is not writable by compute");
    }
    if (ok && !g_cb) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_cb));
        if (!ok) failOnce("the parameter buffer could not be created");
    }
    if (ok && !g_cs) {
        g_cs = shaderSwapCompileCs(ctx, kBackdropCsHlsl,
                                   sizeof(kBackdropCsHlsl) - 1, "main",
                                   "backdrop deband", nullptr,
                                   "menu backdrop");
        if (!g_cs) failOnce("the deband kernel would not compile");
        ok = g_cs != nullptr;
    }

    if (ok) {
        // Save every compute slot this touches and put all of it back. The
        // game is mid-frame and owns this context; fss_heal learned the same
        // discipline the same way.
        ID3D11ComputeShader* savedCs = nullptr;
        ID3D11ShaderResourceView* savedSrv = nullptr;
        ID3D11UnorderedAccessView* savedUav = nullptr;
        ID3D11Buffer* savedCb = nullptr;
        ctx->CSGetShader(&savedCs, nullptr, nullptr);
        ctx->CSGetShaderResources(0, 1, &savedSrv);
        ctx->CSGetUnorderedAccessViews(0, 1, &savedUav);
        ctx->CSGetConstantBuffers(0, 1, &savedCb);

        UINT keep = 0;
        ctx->CSSetShader(g_cs, nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, &g_cb);

        const UINT gx = (src.width + 15) / 16;
        const UINT gy = (src.height + 15) / 16;

        // Pass 0 reads the game's BC1; every later pass reads the previous
        // pass's output. Odd passes land in the slot, even ones in the shared
        // scratch, so with an odd pass count the final image is in the slot --
        // which is what the game-facing view was made over. kPassCount is
        // asserted odd below.
        for (int i = 0; ok && i < kPassCount; ++i) {
            const bool intoOut = (i % 2) == 0;
            ID3D11ShaderResourceView* in =
                (i == 0) ? srcRead : (intoOut ? g_tmpRead : slot.readSrv);
            ID3D11UnorderedAccessView* out = intoOut ? slot.uav : g_tmpUav;
            ID3D11ShaderResourceView* nullSrv = nullptr;
            ID3D11UnorderedAccessView* nullUav = nullptr;

            // Unbind the other side first: a texture cannot be an SRV and a
            // UAV in the same dispatch, and D3D silently nulls the SRV if it
            // is asked to, which would read black instead of the last pass.
            ctx->CSSetShaderResources(0, 1, &nullSrv);
            ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, &keep);

            const float t = g_threshold * kPasses[i].scale;
            const float d = (i == kPassCount - 1) ? g_dither : 0.0f;
            if (!setParams(ctx, static_cast<float>(kPasses[i].radius), t, d)) {
                ok = false;
                failOnce("the parameter buffer could not be written");
                break;
            }
            ctx->CSSetShaderResources(0, 1, &in);
            ctx->CSSetUnorderedAccessViews(0, 1, &out, &keep);
            ctx->Dispatch(gx, gy, 1);
        }

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
    }

    if (srcRead) srcRead->Release();
    dev->Release();

    if (!ok) {
        releaseSlot(slot);
        dropScratch();
        return false;
    }
    dropScratch();
    slot.srcRes = src.res;
    slot.w = src.width;
    slot.h = src.height;
    if (g_notedBuilt) {
        Log::get().note("menu backdrop: a second still (%ux%u, target %ux%u) "
                        "was debanded into its own slot -- both keep their "
                        "draws now.", src.width, src.height, src.targetW,
                        src.targetH);
    }
    if (!g_notedBuilt) {
        g_notedBuilt = true;
        Log::get().note(
            "menu backdrop: SMOOTH. The %ux%u BC1 still behind the menu was "
            "debanded once into an EDVR texture (%d passes, threshold %.1f/255, "
            "dither %.1f/255) and that draw now samples ours. The game's shader "
            "is untouched, and one texture feeds both eyes. The draw's target "
            "is %ux%u%s",
            src.width, src.height, kPassCount, g_threshold * 255.0f,
            g_dither * 255.0f, src.targetW, src.targetH,
            (src.targetW && src.targetW < src.width)
                ? " -- SMALLER than the still, so the engine is discarding "
                  "detail it already has before anything magnifies it."
                : ".");
    }
    return true;
}

}  // namespace

static_assert(kPassCount % 2 == 1,
              "the ping-pong lands the final pass in the slot only for an odd "
              "pass count -- add or remove a pass in pairs, or the game "
              "samples the second-to-last image");

void backdropConfigure(Config& cfg) {
    const std::string mode = cfg.getString("fix.menu_backdrop", "stock");
    const bool splash = mode == "splash";
    const bool on = splash || mode == "smooth";

    // 0..64 of 255. Above about 24 this stops being a deband and starts
    // being a blur that eats stars, which is a thing to be able to SEE
    // rather than a thing to be protected from.
    const float t =
        static_cast<float>(cfg.getIntInRange("advanced.menu_backdrop_threshold",
                                             6, 0, 64)) /
        255.0f;
    const float d =
        static_cast<float>(cfg.getIntInRange("advanced.menu_backdrop_dither",
                                             1, 0, 8)) /
        255.0f;

    // A tunable that changed has to reach the image, and the image is baked.
    // But nothing here frees a D3D object: this runs on the config reload
    // path, and the textures are read by Begin/End on the render thread.
    // Requesting a rebuild lets the draw path do the freeing, where it is the
    // only thread that can be looking.
    if (g_slotsUsed > 0 && (t != g_threshold || d != g_dither)) {
        g_rebuild = true;
        g_failed = false;
        g_notedBuilt = false;
    }
    g_threshold = t;
    g_dither = d;
    g_on = on;
    if (g_splash != splash) g_notedSplash = false;
    g_splash = splash;
}

bool backdropWantsDraws() { return g_on; }

bool backdropOnDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                    uint32_t instances) {
    if (!g_on || g_failed || !ctx) return false;
    // This is the MENU's backdrop. The first field run had no such gate, kept
    // matching after LoadGame, and spent its rebuild budget on in-game
    // textures -- eye-draw count 22 (menu) to 724 (flying) in the same
    // session, the fix still hunting throughout. Once gameplay has started
    // there is no backdrop to fix and nothing here should run again.
    if (journalGameplay()) return false;
    // The cheap half of the signature first, so a frame full of other draws
    // costs two comparisons each.
    if (kind != 'N' || count != 4 || instances != 1) return false;

    ID3D11ShaderResourceView* srv = static_cast<ID3D11ShaderResourceView*>(
        bindingGet(BindSlot::PsSrv0));
    if (!srv) return false;
    ResourceInfo info{};
    if (!bindingResolve(srv, &info)) return false;   // not knowable: do nothing
    if (!info.isTexture2D || !isBc1(info.fmt)) return false;
    if (info.a < kMinWidth || info.b < kMinHeight) return false;
    const float aspect = static_cast<float>(info.a) / static_cast<float>(info.b);
    if (aspect < kMinAspect || aspect > kMaxAspect) return false;

    // A tunable moved under us. The freeing happens HERE, on the render
    // thread, and not in Configure -- Begin/End read these textures and
    // Configure runs on the reload path.
    if (g_rebuild) {
        g_rebuild = false;
        releaseAll();
    }

    // Already baked? Serve THIS draw's own bake. This is the whole point of
    // the cache: the menu's two stills each keep their own, so neither
    // reverts to stock when the other appears.
    for (int i = 0; i < g_slotsUsed; ++i) {
        if (g_slots[i].srcRes == info.resource) {
            g_hit = i;
            noteTarget(g_slots[i]);
            return true;
        }
    }

    if (g_slotsUsed >= kMaxSlots) {
        if (!g_notedFull) {
            g_notedFull = true;
            Log::get().note(
                "menu backdrop: a %ux%u still matched too and all %d slots are "
                "baked. The baked stills keep their draws; this one draws "
                "stock. Said once.", info.a, info.b, kMaxSlots);
        }
        return false;
    }

    bool made = false;
    guardedBudget(g_budget, [&] {
        Source src{};
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        srv->GetDesc(&sd);
        src.srgb = isSrgb(sd.Format);
        srv->GetResource(&src.res);
        if (!src.res) return;
        src.width = info.a;
        src.height = info.b;
        ResourceInfo rt{};
        if (bindingResolve(bindingGet(BindSlot::Rtv0), &rt) && rt.isTexture2D) {
            src.targetW = rt.a;
            src.targetH = rt.b;
        }
        made = build(ctx, src, g_slots[g_slotsUsed]);
        src.res->Release();
    });
    if (!made) {
        if (!g_failed) {
            // The guard ate a fault rather than the build reporting one.
            failOnce("the deband faulted while building");
        }
        return false;
    }
    g_hit = g_slotsUsed++;
    noteTarget(g_slots[g_hit]);
    return true;
}

// The composite: the six-index quad that lifts a blit target into an eye.
//
// This is where the resolution is actually won. The engine blits a 3840x2160
// still into a 1920x1080 target and the composite magnifies THAT across the
// eye, so three quarters of an asset the game already loaded is discarded
// before anything is displayed (measured; docs/menu-backdrop.md). Handing the
// composite our full-resolution bake instead means the 1080p intermediate is
// simply never read.
//
// Matched by IDENTITY, not by shape. Every earlier signature in this file was
// a shape and every one of them was too loose at least once -- the square
// atlases, the in-game textures. Here the blit above has already told us the
// exact resource its target is, so the test is "is slot 0 that texture", and
// nothing else can answer yes.
bool backdropOnComposite(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                         uint32_t instances) {
    if (!g_on || g_failed || !ctx || g_slotsUsed == 0) return false;
    if (journalGameplay()) return false;
    if (kind != 'X' || count != 6 || instances != 1) return false;

    void* srv = bindingGet(BindSlot::PsSrv0);
    if (!srv) return false;
    ResourceInfo info{};
    if (!bindingResolve(srv, &info)) return false;
    if (!info.isTexture2D || !info.resource) return false;

    for (int i = 0; i < g_slotsUsed; ++i) {
        for (int k = 0; k < g_slots[i].targetCount; ++k) {
            if (g_slots[i].targets[k] != info.resource) continue;
            if (!g_slots[i].gameSrv) return false;
            g_hit = i;
            if (!g_notedComposite) {
                g_notedComposite = true;
                Log::get().note(
                    "menu backdrop: the composite reads our %ux%u bake "
                    "directly now, instead of the game's %ux%u copy of it -- "
                    "the engine's downsample is bypassed. Said once.",
                    g_slots[i].w, g_slots[i].h, info.a, info.b);
            }
            return true;
        }
    }
    return false;
}

void backdropBegin(ID3D11DeviceContext* ctx) {
    g_bound = false;
    g_saved = nullptr;
    if (!g_on || !ctx) return;
    if (g_hit < 0 || g_hit >= g_slotsUsed) return;
    const int use = chooseSlot(g_hit);
    ID3D11ShaderResourceView* mine = g_slots[use].gameSrv;
    if (!mine) return;
    if (use != g_hit && !g_notedSplash) {
        g_notedSplash = true;
        Log::get().note(
            "menu backdrop: SPLASH. The %ux%u still the menu shows first is "
            "kept in place of the %ux%u one that would replace it -- the "
            "game's own art, held a few seconds longer. Said once.",
            g_slots[use].w, g_slots[use].h, g_slots[g_hit].w,
            g_slots[g_hit].h);
    }
    guardedBudget(g_budget, [&] {
        ctx->PSGetShaderResources(0, 1, &g_saved);
        // Armed BEFORE the substitution, not after. A fault between the two
        // calls would otherwise leave our texture bound with nothing owing a
        // restore, and every later draw in the frame would sample it.
        g_bound = true;
        ctx->PSSetShaderResources(0, 1, &mine);
    });
}

void backdropEnd(ID3D11DeviceContext* ctx) {
    if (!g_bound || !ctx) {
        if (g_saved) { g_saved->Release(); g_saved = nullptr; }
        return;
    }
    g_bound = false;
    guardedBudget(g_budget, [&] {
        ctx->PSSetShaderResources(0, 1, &g_saved);
    });
    if (g_saved) { g_saved->Release(); g_saved = nullptr; }
}

void backdropShutdown() {
    releaseAll();
    if (g_cs) { g_cs->Release(); g_cs = nullptr; }
    if (g_cb) { g_cb->Release(); g_cb = nullptr; }
}

}  // namespace edvr

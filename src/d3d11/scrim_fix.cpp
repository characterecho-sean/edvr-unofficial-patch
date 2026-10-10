#include "scrim_fix.h"

#include <windows.h>

#include <d3d11.h>

#include <string>

#include "../common/config.h"
#include "../common/runtime_profile.h"
#include "../common/intro_mode.h"
#include "../common/log.h"
#include "binding_shadow.h"

namespace edvr {

// scrimWantsDraws reads this from the header with no call: asked per
// draw, and the build has no /GL to fold a cross-TU getter.
namespace detail {
bool g_scrimOn = false;
}  // namespace detail

namespace {

// The wash's own texture: sixteen pixels, block-compressed, stretched across
// the whole curved surface. Nothing else in the loader's frame binds a BC1
// that small, which is what makes this the discriminator rather than the
// vertex shader hash.
constexpr uint32_t kWashW = 16;
constexpr uint32_t kWashH = 16;

// The interface surface the wash dims, in slot 1. Only a size FLOOR: the
// census measured 4259x2395 on one rig, but that is a render-scale-dependent
// number and pinning it would switch this fix off silently on anyone else's
// headset -- the mistake holo_fix made in the field on 2026-08-19 and records
// in its own comments.
constexpr uint32_t kUiMinW = 1024;

// A mesh, not a quad. The wash is stretched over the game's curved surface,
// which the census showed at 5760 indices; the floor allows for geometry that
// re-tessellates without letting a six-index HUD quad in.
// The mesh re-tessellates: the field measured 5760 indices under one modal
// and 360 under the next, same shaders and same textures both times. A floor
// of 1000 would have matched the first and silently missed the second.
constexpr char     kKind = 'X';
constexpr uint32_t kMinIndices = 100;
constexpr uint32_t kInstances = 1;
// The shape test itself is scrimWashShape (scrim_fix.h), inline so the draw
// path can ask it first; it must say what these three constants say.
static_assert(kKind == 'X' && kMinIndices == 100 && kInstances == 1,
              "scrimWashShape (scrim_fix.h) must match the wash mesh's shape");

bool isBc1(uint32_t fmt) {
    return fmt == DXGI_FORMAT_BC1_TYPELESS || fmt == DXGI_FORMAT_BC1_UNORM ||
           fmt == DXGI_FORMAT_BC1_UNORM_SRGB;
}

// 0, and the disassembly is why. From ps 9107E72CB016CC02:
//
//   mad r0.xyzw, r2.xyzw, r0.xxxx, r3.xyzw   ; r0.x, r0.y are the t0 samples
//   add r0.xyzw, r0.xyzw, r1.xyzw            ; + the sharp UI on top
//
// The two samples of t0 MULTIPLY the blurred, desaturated, tinted layer that
// is the wash. White therefore turns it to full strength -- which is what
// this shipped first, and the field duly reported no change for the better.
// Black collapses both terms, leaving r0 = r1: the sharp interface alone,
// with the shader's own discard (all channels < 5/255) throwing the empty
// area away. That is precisely what the no-wash variant 85565E9261812E2F
// does with its own discard, reached by a different route.
constexpr uint32_t g_level = 0;   // the uniform's channel value: black

ID3D11Texture2D*          g_tex = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;
uint32_t                  g_texLevel = 0xFFFFFFFFu;
bool                      g_createFailedNoted = false;

bool                      g_engaged = false;
ID3D11ShaderResourceView* g_displaced = nullptr;
uint64_t                  g_applied = 0;

// Resource metadata is immutable for a view's lifetime. The binding shadow's
// generation changes on every setter (even the same pointer), ClearState,
// ExecuteCommandList(false), and every frame boundary. A pointer plus that
// generation is therefore a bounded identity: the cache never dereferences or
// owns the pointer, and pointer reuse cannot inherit an older classification.
// Failed resolves stay unknown and are retried; only successful answers cache.
struct MetadataCache {
    void* view = nullptr;
    uint32_t generation = 0;
    bool known = false;
    bool matches = false;
};

MetadataCache g_washMetadata;
MetadataCache g_uiMetadata;

// Trace-only copy of raw descriptor scalars. The view pointer is an internal
// cache key only and is never copied into an observation or selector input.
struct TraceRawShadow final {
    void* view = nullptr;
    uint32_t generation = 0;
    bool valid = false;
    bool texture2D = false;
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t fmt = 0;
};

TraceRawShadow g_washTraceShadow;
TraceRawShadow g_uiTraceShadow;

#ifdef EDVR_SCRIM_METADATA_TEST
uint64_t g_metadataResolveCalls = 0;
#endif

void resetMetadataCaches() {
    g_washMetadata = MetadataCache{};
    g_uiMetadata = MetadataCache{};
}

bool resolveMetadata(void* view, ResourceInfo* out) {
#ifdef EDVR_SCRIM_METADATA_TEST
    if (view) ++g_metadataResolveCalls;
#endif
    return bindingResolve(view, out);
}

template <typename Predicate>
bool cachedMetadata(BindSlot slot, MetadataCache& cache, Predicate matches) {
    void* const view = bindingGet(slot);
    const uint32_t generation = bindingGeneration(slot);
    if (cache.known && cache.view == view && cache.generation == generation) {
        return cache.matches;
    }

    ResourceInfo info;
    if (!resolveMetadata(view, &info)) {
        cache.view = view;
        cache.generation = generation;
        cache.known = false;
        cache.matches = false;
        return false;
    }

    cache.view = view;
    cache.generation = generation;
    cache.known = true;
    cache.matches = matches(info);
    return cache.matches;
}

template <typename Predicate>
bool cachedMetadataObserved(BindSlot slot, MetadataCache& cache,
                            TraceRawShadow& shadow,
                            holo_scrim_observation::ResourceObservation& fact,
                            Predicate matches) {
    using holo_scrim_observation::Tri;
    using holo_scrim_observation::ResourceSource;
    void* const view = bindingGet(slot);
    const uint32_t generation = bindingGeneration(slot);
    if (cache.known && cache.view == view && cache.generation == generation) {
        fact.resolveReached = Tri::No;
        if (shadow.valid && shadow.view == view && shadow.generation == generation) {
            fact.source = ResourceSource::RawShadowHit;
            fact.resolved = Tri::Yes;
            fact.rawAvailable = Tri::Yes;
            fact.texture2D = shadow.texture2D ? Tri::Yes : Tri::No;
            fact.a = shadow.a;
            fact.b = shadow.b;
            fact.fmt = shadow.fmt;
        } else {
            fact.source = ResourceSource::WarmCacheWithoutRawShadow;
            fact.resolved = Tri::Unknown;
            fact.rawAvailable = Tri::Unknown;
            fact.texture2D = Tri::Unknown;
        }
        // This is the production selector's cache outcome. The recorded raw
        // fact remains independently re-evaluable; a missing shadow does not
        // turn cache.matches into a serialized selector input.
        return cache.matches;
    }

    ResourceInfo info;
    fact.resolveReached = Tri::Yes;
    const bool resolved = resolveMetadata(view, &info);
    fact.source = resolved ? ResourceSource::FreshResolveSuccess
                           : ResourceSource::FreshResolveFailure;
    fact.resolved = resolved ? Tri::Yes : Tri::No;
    fact.rawAvailable = resolved ? Tri::Yes : Tri::No;
    fact.texture2D = info.isTexture2D ? Tri::Yes : Tri::No;
    if (resolved) {
        fact.a = info.a;
        fact.b = info.b;
        fact.fmt = info.fmt;
        shadow.view = view;
        shadow.generation = generation;
        shadow.valid = true;
        shadow.texture2D = info.isTexture2D;
        shadow.a = info.a;
        shadow.b = info.b;
        shadow.fmt = info.fmt;
    } else {
        // An observed failed resolver makes this identity's old raw shadow
        // unusable; the ordinary cache still retries exactly as before.
        shadow = TraceRawShadow{};
    }

    if (!resolved) {
        cache.view = view;
        cache.generation = generation;
        cache.known = false;
        cache.matches = false;
        return false;
    }

    cache.view = view;
    cache.generation = generation;
    cache.known = true;
    cache.matches = matches(info);
    return cache.matches;
}

ID3D11ShaderResourceView* uniformSrv(ID3D11DeviceContext* ctx) {
    if (g_srv && g_texLevel == g_level) return g_srv;

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;

    const uint8_t v = static_cast<uint8_t>(g_level);
    const uint8_t pixel[4] = {v, v, v, 255};
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 1;
    td.Height = 1;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = pixel;
    init.SysMemPitch = 4;

    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = dev->CreateTexture2D(&td, &init, &tex);
    if (SUCCEEDED(hr) && tex) {
        hr = dev->CreateShaderResourceView(tex, nullptr, &srv);
    }
    dev->Release();
    if (FAILED(hr) || !srv) {
        if (tex) tex->Release();
        if (!g_createFailedNoted) {
            g_createFailedNoted = true;
            Log::get().note("loading dim: the uniform texture could not be "
                            "created (0x%08lX); the wash draws stock.",
                            static_cast<unsigned long>(hr));
        }
        return nullptr;
    }
    if (g_srv) g_srv->Release();
    if (g_tex) g_tex->Release();
    g_tex = tex;
    g_srv = srv;
    g_texLevel = g_level;
    return g_srv;
}

}  // namespace

void scrimConfigure(Config& cfg) {
    const bool was = detail::g_scrimOn;
    const std::string m = runtimeVrProfile() ?
        cfg.getString("fix.loading_dim", "screen") : "stock";
    const LoadingDimMode dm = loadingDimParse(m);
    if (!dm.recognised) {
        Log::get().note("loading_dim \"%s\" is not screen or stock; running "
                        "the default, screen.", m.c_str());
    }
    detail::g_scrimOn = dm.washOff;

    if (was != detail::g_scrimOn) {
        resetMetadataCaches();
        Log::get().note(
            "loading dim: %s. The wash the loader's dialog lays over "
            "everything behind it is %s; level %u. Found by diffing two "
            "censuses -- it is a 16x16 texture stretched over the interface "
            "composite, not a draw of its own (docs/loading-scrim.md).",
            detail::g_scrimOn ? "OFF" : "stock",
            detail::g_scrimOn ? "replaced with a uniform for that one draw"
                 : "the game's own",
            g_level);
    }
}

bool scrimOnEyeDraw(char kind, uint32_t count, uint32_t instances) {
    if (!detail::g_scrimOn) return false;
    if (!scrimWashShape(kind, count, instances)) {
        return false;
    }
    // Slot 0 first: a 16x16 BC1 is the rare binding, and every other mesh in
    // the frame fails here without paying for a second resolve.
    if (!cachedMetadata(BindSlot::PsSrv0, g_washMetadata,
                        [](const ResourceInfo& wash) {
                            return wash.isTexture2D && wash.a == kWashW &&
                                   wash.b == kWashH && isBc1(wash.fmt);
                        })) {
        return false;
    }
    // Slot 1 must be the interface surface the wash is dimming. Without this
    // the fix would fire on any mesh that happened to carry a small BC1.
    if (!cachedMetadata(BindSlot::PsSrv1, g_uiMetadata,
                        [](const ResourceInfo& ui) {
                            return ui.isTexture2D && ui.a >= kUiMinW;
                        })) {
        return false;
    }
    return true;
}

bool scrimOnEyeDrawObserved(char kind, uint32_t count, uint32_t instances,
                            holo_scrim_observation::ScrimObservation* observation) {
    using holo_scrim_observation::Tri;
    holo_scrim_observation::ScrimObservation local{};
    auto& out = observation ? *observation : local;
    out = holo_scrim_observation::ScrimObservation{};
    auto finish = [&](bool matched) {
        out.predicateResult = matched ? Tri::Yes : Tri::No;
        return matched;
    };

    const bool outerEnabled = detail::g_scrimOn;
    out.gates.enabled = outerEnabled ? Tri::Yes : Tri::No;
    if (!outerEnabled) return finish(false);

    out.gates.shapeReached = Tri::Yes;
    const bool outerShape = scrimWashShape(kind, count, instances);
    out.gates.shapeMatched = outerShape ? Tri::Yes : Tri::No;
    if (!outerShape) return finish(false);

    out.gates.helperReached = Tri::Yes;
    const bool helperEnabled = detail::g_scrimOn;
    out.gates.helperEnabled = helperEnabled ? Tri::Yes : Tri::No;
    if (!helperEnabled) return finish(false);
    out.gates.helperShapeReached = Tri::Yes;
    const bool helperShape = scrimWashShape(kind, count, instances);
    out.gates.helperShapeMatched = helperShape ? Tri::Yes : Tri::No;
    if (!helperShape) return finish(false);

    if (!cachedMetadataObserved(BindSlot::PsSrv0, g_washMetadata,
                                g_washTraceShadow, out.wash,
                                [](const ResourceInfo& wash) {
                                    return wash.isTexture2D && wash.a == kWashW &&
                                           wash.b == kWashH && isBc1(wash.fmt);
                                })) {
        return finish(false);
    }
    if (!cachedMetadataObserved(BindSlot::PsSrv1, g_uiMetadata,
                                g_uiTraceShadow, out.ui,
                                [](const ResourceInfo& ui) {
                                    return ui.isTexture2D && ui.a >= kUiMinW;
                                })) {
        return finish(false);
    }
    return finish(true);
}

void scrimBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    ID3D11ShaderResourceView* uniform = uniformSrv(ctx);
    if (!uniform) return;   // stock behaviour, which the log explained once

    g_displaced = static_cast<ID3D11ShaderResourceView*>(
        bindingGet(BindSlot::PsSrv0));
    // Armed before the substitution: a fault between the two would otherwise
    // leave our texture bound with nothing owing a restore.
    g_engaged = true;
    ctx->PSSetShaderResources(0, 1, &uniform);

    if (++g_applied == 1) {
        Log::get().note("loading dim: OFF engaged -- the wash term is uniform "
                        "level %u for exactly this draw, so the loader's "
                        "dialog sits on undimmed art. The game's texture is "
                        "restored after every draw. Level 0 collapses the "
                        "blur layer the shader multiplies by it.", g_texLevel);
    }
}

void scrimEnd(ID3D11DeviceContext* ctx) {
    if (!g_engaged) return;
    g_engaged = false;
    ID3D11ShaderResourceView* orig = g_displaced;
    g_displaced = nullptr;
    ctx->PSSetShaderResources(0, 1, &orig);
}

void scrimShutdown() {
    if (g_srv) { g_srv->Release(); g_srv = nullptr; }
    if (g_tex) { g_tex->Release(); g_tex = nullptr; }
    g_texLevel = 0xFFFFFFFFu;
    resetMetadataCaches();
}

#ifdef EDVR_SCRIM_METADATA_TEST
uint64_t scrimMetadataResolveCallsForTest() { return g_metadataResolveCalls; }

void scrimMetadataResetResolveCallsForTest() { g_metadataResolveCalls = 0; }
#endif

}  // namespace edvr

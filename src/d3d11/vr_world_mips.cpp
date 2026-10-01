// The VR world route's mipped screen (vr_world_mips.h; design doc section 82, "Layer"). The header says what it is for and
// why the mips are generated through an sRGB view and sampled through the UNORM one; this file is how, and what it refuses.
//
// Render thread only, no locks. Once the texture exists nothing here allocates: the views, the sampler table, the refusal
// memory and the counters are fixed-size state, and the log takes a line only for a creation or for a refusal it has not
// said before. The state holds raw pointers and no destructors, so nothing runs at DLL unload.
//
// THE HOOKS. The route's D3D calls go through the hooked vtable, and every state hook steps aside only for
// g_flatComputeInternal (the draw and dispatch thunks for g_vrWorldInternal), so every call below that reaches the device or
// the context runs inside a VrWorldInternalScope.
#include "vr_world_mips.h"
#include "gpu_census.h"
#include "vr_world_route.h"
#include "../common/log.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace {

// How many distinct refusals, and how many creations, are written to the log: a texture that flaps between sizes or is
// refused every frame must not fill it. Every one is counted (VrWorldMipsStats).
constexpr uint32_t kRefusalLines = 8;
constexpr uint32_t kCreationLines = 8;
// The sampler table. The game has one or two screen samplers; eight is room for a few menus' worth.
constexpr int kSamplerSlots = 8;

// The mipped copy of the screen. `device`, `source` and `sourceFormat` are identities compared against what the next call
// brings and never used to reach anything; `device` is safe to compare while `texture` lives, because a device outlives
// its children.
struct Mips {
    ID3D11Device* device = nullptr;
    ID3D11Texture2D* texture = nullptr;              // R8G8B8A8_TYPELESS, the full chain, GENERATE_MIPS: owned
    ID3D11ShaderResourceView* srgbView = nullptr;    // owned; only GenerateMips reads through this one
    ID3D11ShaderResourceView* unormView = nullptr;   // owned; what the caller samples through
    uint32_t width = 0, height = 0, levels = 0;
    DXGI_FORMAT sourceFormat = DXGI_FORMAT_UNKNOWN;
    // The frame this texture's mips were last made for, and from which screen.
    ID3D11Texture2D* source = nullptr;
    uint64_t frame = 0;
    bool frameDone = false;
};

struct SamplerSlot {
    D3D11_SAMPLER_DESC key;      // the sampler as made: the game's addressing, the route's filter and LOD range
    ID3D11SamplerState* state;   // owned; null when the slot is free
    uint64_t serial;             // creation order, so the oldest is the one evicted
};
struct Samplers {
    ID3D11Device* device = nullptr;
    SamplerSlot slot[kSamplerSlots] = {};
    uint64_t nextSerial = 1;
};

// A refusal as the log remembers it: the reason and the texture it was about.
struct RefusalSeen {
    uint32_t reason, format, width, height, samples, slices, levels;
};

Mips g_mips;
Samplers g_samplers;
VrWorldMipsStats g_stats;
RefusalSeen g_seen[kRefusalLines];
uint32_t g_seenCount = 0;

template <class T>
void releaseAndNull(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// The views first, then the texture they hold a reference to.
void releaseMips() {
    if (g_mips.texture) ++g_stats.releases;
    releaseAndNull(g_mips.unormView);
    releaseAndNull(g_mips.srgbView);
    releaseAndNull(g_mips.texture);
    g_mips = Mips{};
}

void releaseSamplers() {
    for (SamplerSlot& s : g_samplers.slot) releaseAndNull(s.state);
    g_samplers.device = nullptr;
    g_samplers.nextSerial = 1;
}

// What the whole chain takes, 4 bytes a texel, each level halved (and never below 1) from the one before. The level count is
// the runtime's (MipLevels 0 asks for every level down to the last, 13 for 5040x2835): it is read back from the texture.
uint64_t chainBytes(uint32_t w, uint32_t h, uint32_t levels) {
    uint64_t total = 0;
    for (uint32_t l = 0; l < levels; ++l) {
        const uint64_t lw = (w >> l) ? (w >> l) : 1;
        const uint64_t lh = (h >> l) ? (h >> l) : 1;
        total += lw * lh * 4;
    }
    return total;
}

const char* refusalExplanation(VrWorldMipsRefusal why) {
    switch (why) {
    case VrWorldMipsRefusal::NullArgument:  return "a null context or texture";
    case VrWorldMipsRefusal::Multisampled:  return "a multisampled texture cannot be copied into a single-sample chain";
    case VrWorldMipsRefusal::TextureArray:  return "an array texture has more than one slice";
    case VrWorldMipsRefusal::AlreadyMipped: return "the texture already has more than one mip level";
    case VrWorldMipsRefusal::NotRgba8:      return "the format is neither R8G8B8A8_TYPELESS nor R8G8B8A8_UNORM";
    case VrWorldMipsRefusal::SrgbTyped:
        return "R8G8B8A8_UNORM_SRGB: the route's colour-space decision assumes display-encoded UNORM data";
    case VrWorldMipsRefusal::CreateFailed:  return "the mipped texture or one of its views could not be created";
    case VrWorldMipsRefusal::None:
    case VrWorldMipsRefusal::Count:         break;
    }
    return "";
}

// Every refusal is counted; the first kRefusalLines DISTINCT ones (the reason and the texture it was about) are also logged,
// once each. Always returns null, so a refusal is one `return refuse(...)`. `d` is null for a call that had no texture.
ID3D11ShaderResourceView* refuse(VrWorldMipsRefusal why, const D3D11_TEXTURE2D_DESC* d, HRESULT hr) {
    ++g_stats.refusals[static_cast<int>(why)];
    RefusalSeen key{};
    key.reason = static_cast<uint32_t>(why);
    if (d) {
        key.format = static_cast<uint32_t>(d->Format);
        key.width = d->Width;
        key.height = d->Height;
        key.samples = d->SampleDesc.Count;
        key.slices = d->ArraySize;
        key.levels = d->MipLevels;
    }
    for (uint32_t i = 0; i < g_seenCount; ++i)
        if (std::memcmp(&g_seen[i], &key, sizeof key) == 0) return nullptr;
    if (g_seenCount >= kRefusalLines) return nullptr;
    g_seen[g_seenCount++] = key;
    ++g_stats.refusalLines;
    char failed[32] = "";
    if (FAILED(hr)) std::snprintf(failed, sizeof failed, ", hr=0x%08lX", static_cast<unsigned long>(hr));
    Log::get().note("vr world mips: refused the screen texture (%s: %s): %ux%u, DXGI format %u, %u samples, %u slices, "
                    "%u levels%s; the eye route keeps the eye.",
                    vrWorldMipsRefusalName(why), refusalExplanation(why), key.width, key.height, key.format, key.samples,
                    key.slices, key.levels, failed);
    return nullptr;
}

// The first rule the texture breaks, in the order the desc is read; None when the route can take it.
VrWorldMipsRefusal classify(const D3D11_TEXTURE2D_DESC& d) {
    if (d.SampleDesc.Count != 1) return VrWorldMipsRefusal::Multisampled;
    if (d.ArraySize != 1) return VrWorldMipsRefusal::TextureArray;
    if (d.MipLevels != 1) return VrWorldMipsRefusal::AlreadyMipped;
    if (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) return VrWorldMipsRefusal::SrgbTyped;
    if (d.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
        return VrWorldMipsRefusal::NotRgba8;
    return VrWorldMipsRefusal::None;
}

// The mipped texture, sized like `src`, with its two views; everything made here is released again if any step fails.
bool createMips(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& src, HRESULT* failure) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = src.Width;
    d.Height = src.Height;
    d.MipLevels = 0;                                // the full chain, down to the last level
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;       // the two views below decide how a texel reads
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;   // GenerateMips renders the levels
    d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* srgb = nullptr;
    ID3D11ShaderResourceView* unorm = nullptr;
    HRESULT hr = device->CreateTexture2D(&d, nullptr, &texture);
    if (SUCCEEDED(hr) && texture) {
        texture->GetDesc(&d);                       // the level count the runtime settled on
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        v.Texture2D.MostDetailedMip = 0;
        v.Texture2D.MipLevels = d.MipLevels;
        v.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        hr = device->CreateShaderResourceView(texture, &v, &srgb);
        if (SUCCEEDED(hr) && srgb) {
            v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            hr = device->CreateShaderResourceView(texture, &v, &unorm);
        }
    }
    if (FAILED(hr) || !texture || !srgb || !unorm) {
        releaseAndNull(unorm);
        releaseAndNull(srgb);
        releaseAndNull(texture);
        *failure = FAILED(hr) ? hr : E_FAIL;
        return false;
    }
    g_mips = Mips{};
    g_mips.device = device;
    g_mips.texture = texture;
    g_mips.srgbView = srgb;
    g_mips.unormView = unorm;
    g_mips.width = d.Width;
    g_mips.height = d.Height;
    g_mips.levels = d.MipLevels;
    g_mips.sourceFormat = src.Format;
    ++g_stats.creations;
    if (g_stats.creationLines < kCreationLines) {
        ++g_stats.creationLines;
        Log::get().note("vr world mips: mipped screen %ux%u, %u levels, %.1f MB (linear-light mips through an sRGB view, "
                        "sampled through the UNORM view)",
                        d.Width, d.Height, d.MipLevels,
                        static_cast<double>(chainBytes(d.Width, d.Height, d.MipLevels)) / 1.0e6);
    }
    return true;
}

}  // namespace

ID3D11ShaderResourceView* vrWorldMipsScreen(ID3D11DeviceContext* ctx, ID3D11Texture2D* screen, uint64_t frame) {
    if (!ctx || !screen) return refuse(VrWorldMipsRefusal::NullArgument, nullptr, S_OK);

    // The frame's own answer: the same screen, asked for again in the same frame (the second eye), needs no GPU work.
    if (g_mips.unormView && g_mips.frameDone && g_mips.frame == frame && g_mips.source == screen) {
        ++g_stats.frameHits;
        return g_mips.unormView;
    }

    D3D11_TEXTURE2D_DESC sd{};
    screen->GetDesc(&sd);
    const VrWorldMipsRefusal why = classify(sd);
    if (why != VrWorldMipsRefusal::None) return refuse(why, &sd, S_OK);

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    screen->GetDevice(device.GetAddressOf());
    if (!device) return refuse(VrWorldMipsRefusal::CreateFailed, &sd, E_POINTER);

    // Everything from here reaches the device or the context.
    VrWorldInternalScope internal;

    // The texture belongs to another device than the one the mips live on: let the old ones go, the new device gets its own.
    if (g_mips.texture && g_mips.device != device.Get()) releaseMips();
    // A new size or a new source format remakes the copy; the same sizes from another texture only redo the frame's work
    // (a game that alternates two screen textures must not cost a 76 MB allocation a frame).
    if (g_mips.texture && (g_mips.width != sd.Width || g_mips.height != sd.Height || g_mips.sourceFormat != sd.Format))
        releaseMips();
    if (!g_mips.texture) {
        HRESULT failure = S_OK;
        if (!createMips(device.Get(), sd, &failure)) return refuse(VrWorldMipsRefusal::CreateFailed, &sd, failure);
    }

    {
        GpuCensusScope census(ctx, GpuCensusSection::FrameWorldMips);
        // Mip 0 is a byte copy; the format family is shared, so typeless or UNORM in, typeless out.
        ctx->CopySubresourceRegion(g_mips.texture, 0, 0, 0, 0, screen, 0, nullptr);
        ++g_stats.copies;
        // The box filter averages in linear light through the sRGB view and stores the result re-encoded.
        ctx->GenerateMips(g_mips.srgbView);
        ++g_stats.generates;
    }
    g_mips.source = screen;
    g_mips.frame = frame;
    g_mips.frameDone = true;
    return g_mips.unormView;
}

ID3D11SamplerState* vrWorldMipsSampler(ID3D11Device* device, const D3D11_SAMPLER_DESC& game) {
    if (!device) return nullptr;
    // Another device than the one the table was made for: its samplers go, and the table starts again.
    if (g_samplers.device && g_samplers.device != device) releaseSamplers();

    // The game's addressing (the three modes and the border colour) with the route's filter and LOD range.
    D3D11_SAMPLER_DESC want = game;
    want.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    want.MipLODBias = 0.0f;
    want.MinLOD = 0.0f;
    want.MaxLOD = D3D11_FLOAT32_MAX;
    want.MaxAnisotropy = 1;
    want.ComparisonFunc = D3D11_COMPARISON_NEVER;

    for (SamplerSlot& s : g_samplers.slot) {
        if (s.state && std::memcmp(&s.key, &want, sizeof want) == 0) {
            ++g_stats.samplerHits;
            return s.state;
        }
    }

    ID3D11SamplerState* state = nullptr;
    {
        VrWorldInternalScope internal;
        if (FAILED(device->CreateSamplerState(&want, &state)) || !state) {
            releaseAndNull(state);
            ++g_stats.samplerFailures;
            return nullptr;
        }
    }
    ++g_stats.samplerCreates;

    // A free slot, or the oldest entry's (its pointer is then no longer the table's to hand out).
    SamplerSlot* take = nullptr;
    for (SamplerSlot& s : g_samplers.slot) {
        if (!s.state) {
            take = &s;
            break;
        }
    }
    if (!take) {
        take = &g_samplers.slot[0];
        for (SamplerSlot& s : g_samplers.slot)
            if (s.serial < take->serial) take = &s;
        releaseAndNull(take->state);
        ++g_stats.samplerEvictions;
    }
    take->key = want;
    take->state = state;
    take->serial = g_samplers.nextSerial++;
    g_samplers.device = device;
    return state;
}

void vrWorldMipsReset() {
    releaseMips();
    releaseSamplers();
}

// ---- test section (vr_world_mips.h) -----------------------------------------------------------------------------------
const char* vrWorldMipsRefusalName(VrWorldMipsRefusal why) {
    switch (why) {
    case VrWorldMipsRefusal::None:          return "none";
    case VrWorldMipsRefusal::NullArgument:  return "null-argument";
    case VrWorldMipsRefusal::Multisampled:  return "multisampled";
    case VrWorldMipsRefusal::TextureArray:  return "texture-array";
    case VrWorldMipsRefusal::AlreadyMipped: return "already-mipped";
    case VrWorldMipsRefusal::NotRgba8:      return "not-rgba8";
    case VrWorldMipsRefusal::SrgbTyped:     return "srgb-typed";
    case VrWorldMipsRefusal::CreateFailed:  return "create-failed";
    case VrWorldMipsRefusal::Count:         break;
    }
    return "unknown";
}

VrWorldMipsStats vrWorldMipsStats() {
    VrWorldMipsStats s = g_stats;
    s.width = g_mips.width;
    s.height = g_mips.height;
    s.levels = g_mips.levels;
    s.samplersLive = 0;
    for (const SamplerSlot& slot : g_samplers.slot)
        if (slot.state) ++s.samplersLive;
    return s;
}

void vrWorldMipsStatsClear() {
    g_stats = VrWorldMipsStats{};
    g_seenCount = 0;
}

}  // namespace edvr

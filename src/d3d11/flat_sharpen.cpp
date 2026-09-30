#include "flat_sharpen.h"

#include "sharpen_pass.h"        // edvrSharpen: the one pass, VR's, not a copy of it
#include "../common/config.h"
#include "../common/log.h"

#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cmath>

namespace edvr {
namespace {

using Microsoft::WRL::ComPtr;

// The setting is live and a slider walks it: the lines that say it changed are
// capped, like the FSS latch's.
constexpr uint32_t kMaxToggleNotes = 6;

// One per resolve output. The temporal resolve alternates between two textures
// (TAA) or writes one every frame (DLSS, FSR), and the pass keeps its cached view
// of its source and its output texture per slot. A slot per source texture means
// neither churns: the pass sees the same source in the same slot every second frame
// instead of a new one every frame, and our view over its output stays valid.
struct Slot {
    // Which resolve texture owns the slot. Identity only, no reference: the pass's
    // own cached view of it holds one, so the address cannot be reused while it
    // stands.
    ID3D11Resource*           source = nullptr;
    uint64_t                  lastUse = 0;
    // The pass's texture this view is over (identity; the view below holds the
    // reference) and the format the view was made in.
    ID3D11Texture2D*          out = nullptr;
    DXGI_FORMAT               viewFormat = DXGI_FORMAT_UNKNOWN;
    ID3D11ShaderResourceView* srv = nullptr;
};

struct State {
    Slot              slot[2];
    ID3D11Device*     device = nullptr;   // identity only: a change is a new session
    uint64_t          call = 0;
    FlatSharpenCounts counts;
    bool              everOn = false, wasOn = false, offNoted = false;
    bool              contextNoted = false, dimensionNoted = false;
    uint32_t          toggleNotes = 0;
};
State g;

void releaseViews() {
    for (Slot& s : g.slot) {
        if (s.srv) s.srv->Release();
        s = Slot{};
    }
}

// The same reading VR's door does (native_sharpen.cpp): finite, 0 to 1, anything
// else nothing to do. In the flat profile Config answers 0 for a key the profile's
// gate does not list (runtime_profile.h), which is why the key is listed there and
// why a rig fails the build if it is not.
float readStrength() {
    const float v = Config::get().getFloat("fix.render_sharpness", 0.0f);
    if (!std::isfinite(v) || v <= 0.0f) return 0.0f;
    return v > 1.0f ? 1.0f : v;
}

int slotFor(ID3D11Resource* source) {
    for (int i = 0; i < 2; ++i) {
        if (g.slot[i].source == source) { g.slot[i].lastUse = g.call; return i; }
    }
    const int take = !g.slot[0].source ? 0
                   : !g.slot[1].source ? 1
                   : (g.slot[0].lastUse <= g.slot[1].lastUse ? 0 : 1);
    g.slot[take].source = source;
    g.slot[take].lastUse = g.call;
    return take;
}

}  // namespace

ID3D11ShaderResourceView* flatSharpenView(ID3D11DeviceContext* ctx,
                                          ID3D11ShaderResourceView* resolved) {
    if (!resolved) return resolved;
    ++g.call;

    const float strength = readStrength();
    if (strength <= 0.0f) {
        ++g.counts.passedOff;
        if (g.wasOn) {
            g.wasOn = false;
            if (g.toggleNotes < kMaxToggleNotes) {
                ++g.toggleNotes;
                Log::get().note(
                    "flat sharpen: turned off (fix.render_sharpness is 0); the "
                    "resolved frames go to the game's output copy as they are.");
            }
        } else if (!g.everOn && !g.offNoted) {
            g.offNoted = true;
            Log::get().note(
                "flat sharpen: off (fix.render_sharpness is 0), so the resolved "
                "frames go to the game's output copy as they are. Said once.");
        }
        return resolved;
    }
    if (!g.wasOn) {
        g.wasOn = true;
        if (g.everOn && g.toggleNotes < kMaxToggleNotes) {
            ++g.toggleNotes;
            Log::get().note("flat sharpen: on again at strength %.2f.",
                            static_cast<double>(strength));
        }
        g.everOn = true;
    }

    ComPtr<ID3D11Resource> resource;
    resolved->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource.As(&texture))) return resolved;
    ComPtr<ID3D11Device> device;
    texture->GetDevice(&device);
    if (device.Get() != g.device) {
        // A new device is a new session: nothing cached belongs to it, and a
        // refusal on the old one says nothing about this one. The pass does the same
        // for everything it made (sharpen_pass.cpp, adoptDevice): before that it kept
        // the old device's result texture and handed it back, this wrapper's view over
        // it failed on the new device, and the sharpening stood down for the session.
        releaseViews();
        g.counts.stoodDown = false;
        g.device = device.Get();
    }
    if (g.counts.stoodDown) {
        ++g.counts.passedStoodDown;
        return resolved;
    }

    // The pass dispatches on its source's device's immediate context (that is what
    // the flat runtime's copy draw runs on); a deferred context here would put the
    // work in the wrong stream.
    if (!ctx || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        if (!g.contextNoted) {
            g.contextNoted = true;
            Log::get().note(
                "flat sharpen: the game's output copy is on a context that is not "
                "the immediate one, which the pass cannot use; those frames go "
                "through as they are. Said once.");
        }
        return resolved;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    resolved->GetDesc(&viewDesc);
    if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) {
        if (!g.dimensionNoted) {
            g.dimensionNoted = true;
            Log::get().note(
                "flat sharpen: the resolve's view is not a plain 2D view "
                "(dimension %d); those frames go through as they are. Said once.",
                static_cast<int>(viewDesc.ViewDimension));
        }
        return resolved;
    }

    D3D11_TEXTURE2D_DESC textureDesc{};
    texture->GetDesc(&textureDesc);
    const int slot = slotFor(resource.Get());
    Slot& s = g.slot[slot];

    // The pass owns its result: per slot, region-sized, the source's own format,
    // valid until the next call for that slot. The source is only read.
    ID3D11Texture2D* out =
        static_cast<ID3D11Texture2D*>(edvrSharpen(texture.Get(), slot, nullptr, strength));
    if (!out) {
        ++g.counts.refusals;
        g.counts.stoodDown = true;
        Log::get().note(
            "flat sharpen: the sharpening pass refused a %ux%u frame (its own "
            "'render sharpening:' line says why); standing down for this session, "
            "and the resolved frames go to the game's output copy as they are.",
            textureDesc.Width, textureDesc.Height);
        return resolved;
    }
    if (!s.srv || s.out != out || s.viewFormat != viewDesc.Format) {
        // The view is over the pass's texture in the resolve view's own format, so
        // the game's copy decodes it exactly as it would have decoded the resolve's
        // (sRGB or plain). It holds a reference to the texture, which is what keeps
        // `out` from being recycled at the same address under a stale cache.
        if (s.srv) { s.srv->Release(); s.srv = nullptr; }
        ComPtr<ID3D11ShaderResourceView> view;
        const HRESULT hr = device->CreateShaderResourceView(out, &viewDesc, &view);
        if (FAILED(hr) || !view) {
            ++g.counts.refusals;
            g.counts.stoodDown = true;
            Log::get().note(
                "flat sharpen: a view over the sharpened %ux%u frame could not be "
                "made (HRESULT 0x%08lX, view format %d); standing down for this "
                "session, and the resolved frames go to the game's output copy as "
                "they are.",
                textureDesc.Width, textureDesc.Height, static_cast<unsigned long>(hr),
                static_cast<int>(viewDesc.Format));
            s.out = nullptr;
            return resolved;
        }
        s.srv = view.Detach();
        s.out = out;
        s.viewFormat = viewDesc.Format;
    }
    ++g.counts.sharpened;
    return s.srv;
}

FlatSharpenCounts flatSharpenCounts() { return g.counts; }

void flatSharpenReset() {
    releaseViews();
    g = State{};
}

}  // namespace edvr

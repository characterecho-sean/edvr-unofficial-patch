#include "resubmit_shadow.h"

#include <windows.h>
#include <d3d11.h>

#include <cstring>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"

namespace edvr {
namespace {

// Speculative dereferences of a handle the game owns get their own fault
// budget -- the camera_view lesson, imported whole: a read that touches
// somebody else's pointer every frame forever needs a small budget and a
// clean retirement, or the guard absorbs a fault per frame and the log
// becomes the casualty. Eight, because a handful of failures here means the
// game's texture handles are not what Submit's contract says they are, and
// this module should stop guessing for the session.
constexpr uint32_t kMaxFaults = 8;

struct State {
    bool enabled = true;
    bool snapshot = false;   // experimental.submit_snapshot; live-reloaded
    ID3D11Texture2D*     shadow[2] = {};
    D3D11_TEXTURE2D_DESC desc[2] = {};
    bool                 valid[2] = {};
    uint32_t resubmits = 0;
    uint32_t fallbacks = 0;
    uint32_t faults = 0;
    bool faultsNoted = false;
    bool mismatchNoted = false;
    bool createFailNoted = false;
};
State g_s;

// The fields that decide whether CopyResource between two textures is legal
// and whether the compositor would treat the copy as the same kind of thing.
// Usage, bind, CPU-access and misc flags deliberately excluded: the shadow is
// created from the live desc verbatim, so they only differ if the GAME's
// flags changed -- and then the identity fields will have changed with them
// on any real engine, while a spurious mismatch on flags alone would throw
// away a copy that is still pixel-compatible.
bool sameShape(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b) {
    return a.Width == b.Width && a.Height == b.Height &&
           a.MipLevels == b.MipLevels && a.ArraySize == b.ArraySize &&
           a.Format == b.Format && a.SampleDesc.Count == b.SampleDesc.Count &&
           a.SampleDesc.Quality == b.SampleDesc.Quality;
}

// GetDesc through the guard: the handle is the game's and Submit's contract,
// not ours, and a stale or garbage handle must cost a fault entry rather
// than the process.
bool descOf(void* handle, D3D11_TEXTURE2D_DESC* out) {
    bool ok = false;
    guarded("resubmit/desc", [&] {
        static_cast<ID3D11Texture2D*>(handle)->GetDesc(out);
        ok = true;
    });
    return ok;
}

bool budgetSpent() {
    if (g_s.faults <= kMaxFaults) return false;
    if (!g_s.faultsNoted) {
        g_s.faultsNoted = true;
        Log::get().note(
            "resubmit: %u faults reading submitted texture handles, so withheld "
            "frames go back to classic withholding for this session. The fix "
            "still works; it just costs the old ~80 ms per withhold again.",
            g_s.faults);
    }
    return true;
}

}  // namespace

void resubmitShadowConfigure() {
    g_s.enabled =
        Config::get().getBool("advanced.transition_flash_resubmit", true);
    const bool was = g_s.snapshot;
    g_s.snapshot =
        Config::get().getBool("experimental.submit_snapshot", false);
    // Said on every flip, because the flip IS the experiment: the player is
    // in a headset watching a ring build, and this line is their receipt
    // that the edit took.
    if (g_s.snapshot != was) {
        Log::get().note(
            g_s.snapshot
                ? "submit snapshot ON: every forwarded frame is delivered to "
                  "the compositor as a per-eye copy taken at Submit, so both "
                  "eyes' content is latched at the same point in the frame."
                : "submit snapshot OFF: live eye textures are submitted, as "
                  "stock EDVR does.");
    }
}

bool resubmitShadowSnapshotWanted() { return g_s.snapshot; }

bool resubmitShadowNoteForwarded(uint32_t eye, void* d3d11Texture) {
    State& s = g_s;
    if ((!s.enabled && !s.snapshot) || eye > 1 || !d3d11Texture) return false;
    if (budgetSpent()) return false;

    D3D11_TEXTURE2D_DESC d{};
    if (!descOf(d3d11Texture, &d)) {
        ++s.faults;
        return false;
    }

    // Release-before-recreate on any shape change -- the ourCb lifecycle at
    // texture scale. The first frame after a change pays one recreate and one
    // copy; nothing is reused across a shape boundary.
    if (!s.shadow[eye] || !sameShape(s.desc[eye], d)) {
        if (s.shadow[eye]) {
            s.shadow[eye]->Release();
            s.shadow[eye] = nullptr;
        }
        s.valid[eye] = false;

        ID3D11Device* dev = nullptr;
        guarded("resubmit/device", [&] {
            static_cast<ID3D11Texture2D*>(d3d11Texture)->GetDevice(&dev);
        });
        if (!dev) {
            ++s.faults;
            return false;
        }
        ID3D11Texture2D* made = nullptr;
        const HRESULT hr = dev->CreateTexture2D(&d, nullptr, &made);
        dev->Release();
        if (FAILED(hr) || !made) {
            if (!s.createFailNoted) {
                s.createFailNoted = true;
                Log::get().note(
                    "resubmit: creating the %ux%u copy failed (0x%08lX), so "
                    "withheld frames use classic withholding until a create "
                    "succeeds. Said once; the totals line carries the count.",
                    d.Width, d.Height, static_cast<unsigned long>(hr));
            }
            return false;
        }
        s.shadow[eye] = made;
        s.desc[eye] = d;
    }

    // The copy is queued on the immediate context AFTER the game's rendering
    // of this frame, so what lands in the shadow is the completed frame. The
    // device and context are taken and released inside the call: holding them
    // across frames would add nothing but a shutdown ordering problem.
    ID3D11Device* dev = nullptr;
    s.shadow[eye]->GetDevice(&dev);
    if (!dev) return false;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    bool copied = false;
    if (ctx) {
        guarded("resubmit/copy", [&] {
            ctx->CopyResource(s.shadow[eye],
                              static_cast<ID3D11Texture2D*>(d3d11Texture));
            copied = true;
        });
        if (copied) {
            s.valid[eye] = true;
        } else {
            ++s.faults;
        }
        ctx->Release();
    }
    dev->Release();
    return copied;
}

void* resubmitShadowCurrent(uint32_t eye) {
    State& s = g_s;
    if (eye > 1 || !s.valid[eye]) return nullptr;
    return s.shadow[eye];
}

void* resubmitShadowForWithhold(uint32_t eye, void* liveD3d11Texture) {
    State& s = g_s;
    if (!s.enabled || eye > 1) return nullptr;
    if (!s.valid[eye] || !s.shadow[eye] || budgetSpent()) {
        ++s.fallbacks;
        return nullptr;
    }

    // The live texture's shape must still match the copy's. A desc change
    // mid-flight (resolution change, SS change) falls back to classic
    // withholding for this frame; the next FORWARDED frame recreates the copy
    // at the new shape. Never a copy here: a withheld frame's content must
    // not reach the shadow.
    D3D11_TEXTURE2D_DESC d{};
    if (!liveD3d11Texture || !descOf(liveD3d11Texture, &d)) {
        ++s.faults;
        ++s.fallbacks;
        return nullptr;
    }
    if (!sameShape(s.desc[eye], d)) {
        if (!s.mismatchNoted) {
            s.mismatchNoted = true;
            Log::get().note(
                "resubmit: the eye texture changed shape mid-flight (%ux%u -> "
                "%ux%u), so this withhold is classic. The copy rebuilds on the "
                "next forwarded frame. Said once.",
                s.desc[eye].Width, s.desc[eye].Height, d.Width, d.Height);
        }
        ++s.fallbacks;
        return nullptr;
    }

    ++s.resubmits;
    return s.shadow[eye];
}

uint32_t resubmitShadowResubmits() { return g_s.resubmits; }
uint32_t resubmitShadowFallbacks() { return g_s.fallbacks; }

void resubmitShadowShutdown() {
    for (uint32_t e = 0; e < 2; ++e) {
        if (g_s.shadow[e]) {
            g_s.shadow[e]->Release();
            g_s.shadow[e] = nullptr;
        }
        g_s.valid[e] = false;
    }
}

}  // namespace edvr

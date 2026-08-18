#include "guard_crop.h"

#include <windows.h>
#include <d3d11.h>

#include <cmath>

#include "../common/guard.h"
#include "../common/log.h"

namespace edvr {
namespace {

// Same budget and same reasoning as the resubmit shadow, whose patterns this
// module copies deliberately: every dereference here touches a handle the
// game owns, and a read that faults every frame forever must retire itself
// rather than bleed the log.
constexpr uint32_t kMaxFaults = 8;

struct State {
    ID3D11Texture2D*     dst[2] = {};
    D3D11_TEXTURE2D_DESC dstDesc[2] = {};
    uint32_t copies = 0;
    uint32_t faults = 0;
    bool faultsNoted = false;
    bool createFailNoted = false;
    bool kindNoted = false;
};
State g_s;

bool descOf(void* handle, D3D11_TEXTURE2D_DESC* out) {
    bool ok = false;
    guarded("guardCrop/desc", [&] {
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
            "cull guard: %u faults reading submitted texture handles; the "
            "copy path retires for this session and the guard stands down.",
            g_s.faults);
    }
    return true;
}

}  // namespace

void* guardCropCopy(uint32_t eye, void* srcHandle,
                    const vr::VRTextureBounds_t* srcBounds,
                    const float fractions[4], uint32_t snapW, uint32_t snapH,
                    vr::VRTextureBounds_t* outBounds) {
    State& s = g_s;
    if (eye > 1 || !srcHandle || !fractions || !outBounds) return nullptr;
    if (budgetSpent()) return nullptr;

    D3D11_TEXTURE2D_DESC sd{};
    if (!descOf(srcHandle, &sd)) {
        ++s.faults;
        return nullptr;
    }
    // The kinds this path does not handle, refused rather than guessed at: a
    // multisampled source cannot take a region copy, and an array source
    // means the game is submitting something structurally unlike the eye
    // textures both field rigs measured.
    if (sd.SampleDesc.Count > 1 || sd.ArraySize != 1 || sd.MipLevels != 1) {
        if (!s.kindNoted) {
            s.kindNoted = true;
            Log::get().note(
                "cull guard: the submitted texture is %ux%u samples=%u "
                "array=%u mips=%u, a kind the copy path does not handle. The "
                "guard stands down.",
                sd.Width, sd.Height, sd.SampleDesc.Count, sd.ArraySize,
                sd.MipLevels);
        }
        return nullptr;
    }

    // Compose the game's bounds with the crop fractions -- the same formula
    // the bounds mechanism used, kept in bounds-space so a flipped span
    // composes without special cases -- then take the pixel box from the
    // min/max ends and remember only the direction for the outgoing bounds.
    vr::VRTextureBounds_t base = {0.0f, 0.0f, 1.0f, 1.0f};
    if (srcBounds) base = *srcBounds;
    const float du = base.uMax - base.uMin;
    const float dv = base.vMax - base.vMin;
    const float cu0 = base.uMin + fractions[0] * du;
    const float cu1 = base.uMin + fractions[2] * du;
    const float cv0 = base.vMin + fractions[1] * dv;
    const float cv1 = base.vMin + fractions[3] * dv;

    auto toPixel = [](float frac, uint32_t extent) -> long {
        long v = lroundf(frac * static_cast<float>(extent));
        if (v < 0) v = 0;
        if (v > static_cast<long>(extent)) v = static_cast<long>(extent);
        return v;
    };
    long x0 = toPixel(cu0 < cu1 ? cu0 : cu1, sd.Width);
    long x1 = toPixel(cu0 < cu1 ? cu1 : cu0, sd.Width);
    long y0 = toPixel(cv0 < cv1 ? cv0 : cv1, sd.Height);
    long y1 = toPixel(cv0 < cv1 ? cv1 : cv0, sd.Height);
    if (x1 - x0 < 16 || y1 - y0 < 16) return nullptr;  // not a usable eye image

    // The snap: land on the canonical size exactly, by nudging the fraction
    // box inside the guard's own margin. A large disagreement means the
    // source is not the adopted-size target this stage was promised --
    // refuse rather than submit a shape the transport has never served,
    // which is the failure class this whole mechanism exists to end.
    auto snapAxis = [](long& a0, long& a1, uint32_t want, uint32_t extent) {
        if (!want) return true;
        const long excess = (a1 - a0) - static_cast<long>(want);
        if (excess > 64 || excess < -64) return false;
        a0 += excess / 2;
        a1 = a0 + static_cast<long>(want);
        if (a0 < 0) { a0 = 0; a1 = static_cast<long>(want); }
        if (a1 > static_cast<long>(extent)) {
            a1 = static_cast<long>(extent);
            a0 = a1 - static_cast<long>(want);
        }
        return a0 >= 0;
    };
    if (!snapAxis(x0, x1, snapW, sd.Width) ||
        !snapAxis(y0, y1, snapH, sd.Height)) {
        return nullptr;
    }

    const uint32_t cw = static_cast<uint32_t>(x1 - x0);
    const uint32_t ch = static_cast<uint32_t>(y1 - y0);

    // Release-before-recreate on any shape change, like the resubmit shadow:
    // the target is the source's desc verbatim except for its size, so the
    // compositor is handed the same KIND of texture the game submits.
    if (!s.dst[eye] || s.dstDesc[eye].Width != cw ||
        s.dstDesc[eye].Height != ch || s.dstDesc[eye].Format != sd.Format) {
        if (s.dst[eye]) {
            s.dst[eye]->Release();
            s.dst[eye] = nullptr;
        }
        D3D11_TEXTURE2D_DESC nd = sd;
        nd.Width = cw;
        nd.Height = ch;

        ID3D11Device* dev = nullptr;
        guarded("guardCrop/device", [&] {
            static_cast<ID3D11Texture2D*>(srcHandle)->GetDevice(&dev);
        });
        if (!dev) {
            ++s.faults;
            return nullptr;
        }
        ID3D11Texture2D* made = nullptr;
        const HRESULT hr = dev->CreateTexture2D(&nd, nullptr, &made);
        dev->Release();
        if (FAILED(hr) || !made) {
            if (!s.createFailNoted) {
                s.createFailNoted = true;
                Log::get().note(
                    "cull guard: creating the %ux%u crop texture failed "
                    "(0x%08lX); the guard stands down. Said once.",
                    cw, ch, static_cast<unsigned long>(hr));
            }
            return nullptr;
        }
        s.dst[eye] = made;
        s.dstDesc[eye] = nd;
    }

    // Queued on the immediate context behind this frame's rendering, exactly
    // like the resubmit shadow's copy: ordering on the context is what
    // guarantees the copy holds the completed frame. Submit and Present were
    // measured on one thread (2026-08-15), so this is the render thread.
    ID3D11Device* dev = nullptr;
    s.dst[eye]->GetDevice(&dev);
    if (!dev) return nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    bool copied = false;
    if (ctx) {
        D3D11_BOX box{};
        box.left = static_cast<UINT>(x0);
        box.top = static_cast<UINT>(y0);
        box.front = 0;
        box.right = static_cast<UINT>(x1);
        box.bottom = static_cast<UINT>(y1);
        box.back = 1;
        guarded("guardCrop/copy", [&] {
            ctx->CopySubresourceRegion(s.dst[eye], 0, 0, 0, 0,
                                       static_cast<ID3D11Texture2D*>(srcHandle),
                                       0, &box);
            copied = true;
        });
        ctx->Release();
    }
    dev->Release();
    if (!copied) {
        ++s.faults;
        return nullptr;
    }

    // Full-span bounds, preserving only the direction the game's own bounds
    // ran in: the pixels are the pixels, and a flipped-origin submission
    // stays a flipped-origin submission.
    outBounds->uMin = du >= 0.0f ? 0.0f : 1.0f;
    outBounds->uMax = du >= 0.0f ? 1.0f : 0.0f;
    outBounds->vMin = dv >= 0.0f ? 0.0f : 1.0f;
    outBounds->vMax = dv >= 0.0f ? 1.0f : 0.0f;
    ++s.copies;
    return s.dst[eye];
}

uint32_t guardCropCopies() { return g_s.copies; }

void guardCropShutdown() {
    for (uint32_t e = 0; e < 2; ++e) {
        if (g_s.dst[e]) {
            g_s.dst[e]->Release();
            g_s.dst[e] = nullptr;
        }
    }
}

}  // namespace edvr

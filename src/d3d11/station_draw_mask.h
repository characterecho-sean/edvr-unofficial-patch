// The optimized station's sun draw with every write masked (built 2026-10-21; docs\optimized-station-2026-10-12.md, Status).
//
// In the flat profile the station's sun skip no longer returns before the flat runtime's draw scope: the scope observes the sun draw like any draw, so
// the AA model still sees the deferred lighting pass that gives the HDR target its camera. Only the draw's effect on the image is removed. When the
// scope has planned work around the game's issue (needsActualDraw), that work must run around a real draw, so the draw is issued with colour writes
// off for all eight render targets and depth and stencil writes off, and the context's state is put back before the scope's end call.
//
// Pure descriptions (blendDesc, maskedDepthStencilDesc, sameDepthStencilDesc) are checked by tools\optimized_station_test. Mask itself needs a device
// and is exercised in a flight: it only ever calls the context's getters and the two setters it is handed (the hook's un-hooked entry points, so no
// binding shadow, epoch or generation moves: the state it sets is the state it puts back).
#pragma once

#include <d3d11.h>

#include <cstring>

namespace edvr::stationmask {

constexpr UINT kTargets = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;   // 8

// A blend state that writes no colour to any of the eight render targets, whatever the pixel shader outputs.
inline D3D11_BLEND_DESC blendDesc() noexcept {
    D3D11_BLEND_DESC d{};
    d.AlphaToCoverageEnable = FALSE;
    d.IndependentBlendEnable = TRUE;
    for (UINT i = 0; i < kTargets; ++i) {
        D3D11_RENDER_TARGET_BLEND_DESC& rt = d.RenderTarget[i];
        rt.BlendEnable = FALSE;
        rt.SrcBlend = D3D11_BLEND_ONE;
        rt.DestBlend = D3D11_BLEND_ZERO;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_ZERO;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = 0;
    }
    return d;
}

// D3D's default depth-stencil state, which a context reports as a null state.
inline D3D11_DEPTH_STENCIL_DESC defaultDepthStencilDesc() noexcept {
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = TRUE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    d.DepthFunc = D3D11_COMPARISON_LESS;
    d.StencilEnable = FALSE;
    d.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
    d.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
    d.FrontFace.StencilFailOp = d.FrontFace.StencilDepthFailOp = d.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    d.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    d.BackFace = d.FrontFace;
    return d;
}

// The bound depth-stencil state with its writes off and nothing else changed: the depth test and the stencil test still run, but neither buffer is written.
inline D3D11_DEPTH_STENCIL_DESC maskedDepthStencilDesc(const D3D11_DEPTH_STENCIL_DESC& bound) noexcept {
    D3D11_DEPTH_STENCIL_DESC d = bound;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    d.StencilWriteMask = 0;
    return d;
}

// Field by field: the structure has padding after the two stencil masks, and a desc read back from a state does not define it.
inline bool sameDepthStencilDesc(const D3D11_DEPTH_STENCIL_DESC& a, const D3D11_DEPTH_STENCIL_DESC& b) noexcept {
    const auto face = [](const D3D11_DEPTH_STENCILOP_DESC& x, const D3D11_DEPTH_STENCILOP_DESC& y) {
        return x.StencilFailOp == y.StencilFailOp && x.StencilDepthFailOp == y.StencilDepthFailOp && x.StencilPassOp == y.StencilPassOp &&
               x.StencilFunc == y.StencilFunc;
    };
    return a.DepthEnable == b.DepthEnable && a.DepthWriteMask == b.DepthWriteMask && a.DepthFunc == b.DepthFunc && a.StencilEnable == b.StencilEnable &&
           a.StencilReadMask == b.StencilReadMask && a.StencilWriteMask == b.StencilWriteMask && face(a.FrontFace, b.FrontFace) && face(a.BackFace, b.BackFace);
}

typedef void(STDMETHODCALLTYPE* SetBlendFn)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT[4], UINT);
typedef void(STDMETHODCALLTYPE* SetDepthFn)(ID3D11DeviceContext*, ID3D11DepthStencilState*, UINT);

// Owner thread only. begin() makes (and caches) the two masking states, remembers the context's blend state, blend factor, sample mask, depth-stencil
// state and stencil reference, and binds the masks. end() binds exactly what begin() remembered. The cache lives for the process (the states are
// small, a handful at most, and the device they belong to is held with them).
class Mask {
public:
    // False, with nothing changed, when no device is reachable or a state could not be made; the caller then does not issue the draw.
    bool begin(ID3D11DeviceContext* ctx, SetBlendFn setBlend, SetDepthFn setDepth) noexcept {
        if (active_ || !ctx || !setBlend || !setDepth) return false;
        ID3D11Device* device = nullptr;
        ctx->GetDevice(&device);
        if (!device) return false;
        if (device != device_) drop(device);
        ctx->OMGetBlendState(&prevBlend_, factor_, &sampleMask_);
        ctx->OMGetDepthStencilState(&prevDepth_, &stencilRef_);
        D3D11_DEPTH_STENCIL_DESC bound = defaultDepthStencilDesc();
        if (prevDepth_) prevDepth_->GetDesc(&bound);
        ID3D11BlendState* maskBlend = blendState();
        ID3D11DepthStencilState* maskDepth = depthState(maskedDepthStencilDesc(bound));
        device->Release();   // the cache holds its own reference to it
        if (!maskBlend || !maskDepth) {
            release();
            return false;
        }
        setBlend_ = setBlend;
        setDepth_ = setDepth;
        setBlend(ctx, maskBlend, factor_, sampleMask_);
        setDepth(ctx, maskDepth, stencilRef_);
        active_ = true;
        return true;
    }

    // Immediately after the masked draw. A no-op unless begin() succeeded.
    void end(ID3D11DeviceContext* ctx) noexcept {
        if (!active_) return;
        setBlend_(ctx, prevBlend_, factor_, sampleMask_);
        setDepth_(ctx, prevDepth_, stencilRef_);
        active_ = false;
        release();
    }

    bool active() const noexcept { return active_; }

private:
    static constexpr unsigned kDepthCache = 8;
    struct Entry {
        D3D11_DEPTH_STENCIL_DESC desc;
        ID3D11DepthStencilState* state;
    };

    void release() noexcept {
        if (prevBlend_) prevBlend_->Release();
        if (prevDepth_) prevDepth_->Release();
        prevBlend_ = nullptr;
        prevDepth_ = nullptr;
    }
    // A new device: the old states are dropped. `device` is the caller's reference; the cache takes its own.
    void drop(ID3D11Device* device) noexcept {
        if (blend_) blend_->Release();
        blend_ = nullptr;
        for (Entry& e : depth_) {
            if (e.state) e.state->Release();
            e.state = nullptr;
        }
        used_ = 0;
        if (device_) device_->Release();
        device_ = device;
        device_->AddRef();
    }
    ID3D11BlendState* blendState() noexcept {
        if (!blend_) {
            const D3D11_BLEND_DESC d = blendDesc();
            if (FAILED(device_->CreateBlendState(&d, &blend_))) blend_ = nullptr;
        }
        return blend_;
    }
    ID3D11DepthStencilState* depthState(const D3D11_DEPTH_STENCIL_DESC& want) noexcept {
        for (unsigned i = 0; i < used_; ++i)
            if (depth_[i].state && sameDepthStencilDesc(depth_[i].desc, want)) return depth_[i].state;
        ID3D11DepthStencilState* made = nullptr;
        if (FAILED(device_->CreateDepthStencilState(&want, &made)) || !made) return nullptr;
        const unsigned slot = used_ < kDepthCache ? used_++ : (next_++ % kDepthCache);
        if (depth_[slot].state) depth_[slot].state->Release();
        depth_[slot].desc = want;
        depth_[slot].state = made;
        return made;
    }

    ID3D11Device* device_ = nullptr;
    ID3D11BlendState* blend_ = nullptr;
    Entry depth_[kDepthCache] = {};
    unsigned used_ = 0;
    unsigned next_ = 0;
    // The context's state while the masks are bound.
    ID3D11BlendState* prevBlend_ = nullptr;
    ID3D11DepthStencilState* prevDepth_ = nullptr;
    FLOAT factor_[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    UINT sampleMask_ = 0xFFFFFFFFu;
    UINT stencilRef_ = 0;
    SetBlendFn setBlend_ = nullptr;
    SetDepthFn setDepth_ = nullptr;
    bool active_ = false;
};

}  // namespace edvr::stationmask

// The resolver's explicit context-state block: the game's whole pipeline state taken out of the context by Get calls and put
// back by Set calls, for a device whose ID3D11DeviceContext1::SwapDeviceContextState aborts the process (DXMT, the D3D11-to-Metal
// layer under CrossOver). docs\macos-dxmt-2026-09-30.md has the evidence; flat_isolation_mode.h decides which isolation a
// device gets and how a DXMT device is told.
//
// WHY. The resolver isolates the game's state from its own work and from its backends' (NGX's DLSS, the FSR 3.1 port): the
// game's context state is swapped out for a fresh one, the work runs, the game's is swapped back. On Windows that is one call
// each way, proven and cheap, and it stays. DXMT implements the call as UNIMPLEMENTED(), which is abort(): the process ended at
// the first capture-state crumb with "SwapDeviceContextState is not implemented." in CrossOver's log and raise(22) after it, and
// no SEH filter ever saw it. This block does the same job with calls DXMT implements (every one used here was read in DXMT's
// d3d11_context_impl.cpp; none is UNIMPLEMENTED), the way ReShade's state block does.
//
// WHAT THE SWAP GIVES, and this mirrors it. The resolver starts from the default state and leaves the game's exactly as it
// was. Isolate (flat_mono_resolve.cpp) does: capture(), ClearState(), the work, ClearState(), restore(). The two ClearState
// calls are the same two the swap path makes beside its Swap calls, so the resolver starts from the defaults as it does after
// a swap (ClearState is the default baseline on every device: D3D11's documented one, and on DXMT "end the render pass and
// state_ = {}"), and what the work or a backend left bound is gone before the game's state goes back. restore() ends by
// releasing every reference it took; nothing is held after it, and a block that is asked to capture again lets go first.
//
// WHAT IS CAPTURED, stage by stage and slot by slot. The slot ranges are what D3D11 allows, cut where DXMT's binding tables
// end (src\dxmt\dxmt_binding_set.hpp, src\d3d11\d3d11_context_state.hpp: SRV 128, constant buffer 14, sampler 16, UAV 64,
// vertex buffer 16, stream output 4, render target 8, viewport and scissor 16):
//
//   IA   input layout, primitive topology, vertex buffers 0..31 with their strides and offsets (0..15 on DXMT, whose table
//        holds 16), index buffer with its format and offset
//   VS HS DS GS PS CS, each   the shader; shader resources 0..127; constant buffers 0..13 with their first-constant and
//        constant-count (the D3D11.1 offset window, read and set through the *ConstantBuffers1 calls); samplers 0..15
//   CS   unordered-access views 0..63 (0..7 below feature level 11_1, where D3D11 has 8)
//   SO   stream-output targets 0..3
//   OM   render targets 0..7, depth-stencil view, output-merger UAVs over the same slot range as the CS ones, blend state with
//        its factor and sample mask, depth-stencil state with its stencil reference
//   RS   rasterizer state, viewports 0..15, scissor rectangles 0..15
//   and  predication: the predicate and its value
//
// Every stage a backend or the resolver can reach is in it: the resolver binds CS, VS, PS, IA, RS and OM itself, NGX's DLSS
// and the FSR port can touch any of them, and a stage neither uses costs one Get call. What cannot be captured, and why it does
// not matter: class instances (ClearState resets them; DXMT does not support them at all, and the resolver and Elite bind
// none), a UAV's append/consume counter and a stream-output target's write offset (neither is readable through the Get calls;
// the counter lives in the resource, which this never touches, and SO is restored at offset 0), and queries in flight (not
// context state on any device).
//
// WHAT RESTORE DOES NOT CALL. After ClearState everything is unbound, so a slot that was empty is not set again: a restore
// calls only what the game had bound, over the range of slots that held something, and the states that have no "unbound" (blend,
// depth-stencil, rasterizer) are always set, because DXMT's defaults after ClearState are not D3D11's (a zero blend factor where
// D3D11 has one) and a round trip through the game's own values is exact on both. No call passes a null array (DXMT reads the
// array it is handed before it reads the count).
//
// THE CRUMBS. capture() and restore() write one span per group (capture-ia, capture-vs ... capture-cs, capture-so, capture-om,
// capture-rs, capture-predication, and restore-... the same) when the caller says so, which Isolate does for the session's first
// capture and first restore: the route's budget is for the steps, and a Metal layer that has never been under these calls is
// likeliest to refuse the first. A capture span's end line says what the game had bound there.
#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>

#include <cstdint>
#include <atomic>

#include "flat_hdr_crumbs.h"

namespace edvr {
inline std::atomic<uint64_t> g_flatCbFirstNonzero{0};

constexpr UINT kCtxSrvSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
constexpr UINT kCtxCbSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
constexpr UINT kCtxSamplerSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
constexpr UINT kCtxVertexBufferSlots = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
constexpr UINT kCtxUavSlots = D3D11_1_UAV_SLOT_COUNT;
constexpr UINT kCtxRtvSlots = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
constexpr UINT kCtxSoSlots = D3D11_SO_BUFFER_SLOT_COUNT;
constexpr UINT kCtxViewportSlots = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
// DXMT's vertex-buffer table (BindingSet<VERTEX_BUFFER_B, 16>); D3D11's is 32.
constexpr UINT kCtxDxmtVertexBufferSlots = 16;
static_assert(kCtxSrvSlots == 128 && kCtxCbSlots == 14 && kCtxSamplerSlots == 16 && kCtxVertexBufferSlots == 32 &&
                  kCtxUavSlots == 64 && kCtxRtvSlots == 8 && kCtxSoSlots == 4 && kCtxViewportSlots == 16 &&
                  D3D11_PS_CS_UAV_REGISTER_COUNT == 8,
              "the explicit capture's slot ranges are the ones its header comment documents");

// How far the UAV and vertex-buffer captures reach on this device.
struct FlatContextRanges {
    UINT vertexBuffers = kCtxVertexBufferSlots;
    UINT uavs = kCtxUavSlots;
};
inline FlatContextRanges flatContextRanges(D3D_FEATURE_LEVEL level, bool dxmt) {
    FlatContextRanges r;
    if (level < D3D_FEATURE_LEVEL_11_1) r.uavs = D3D11_PS_CS_UAV_REGISTER_COUNT;
    if (dxmt) r.vertexBuffers = kCtxDxmtVertexBufferSlots;
    return r;
}

class FlatContextState {
public:
    // The six shader stages, in the order the capture takes them.
    enum Stage : unsigned { kVs, kHs, kDs, kGs, kPs, kCs, kStageCount };

    FlatContextState() = default;
    FlatContextState(const FlatContextState&) = delete;
    FlatContextState& operator=(const FlatContextState&) = delete;
    ~FlatContextState() { release(); }

    // Reads the game's whole pipeline state into the block. Every reference a Get call hands back is kept (and dropped by
    // release()); nothing is bound or changed on the context.
    void capture(ID3D11DeviceContext1* c, const FlatContextRanges& r, bool crumbs) {
        release();
        ranges_ = r;
        {
            HdrCrumbSpan span(crumbs, "capture-ia");
            c->IAGetInputLayout(layout_.GetAddressOf());
            c->IAGetPrimitiveTopology(&topology_);
            c->IAGetVertexBuffers(0, ranges_.vertexBuffers, vb_, vbStride_, vbOffset_);
            c->IAGetIndexBuffer(ib_.GetAddressOf(), &ibFormat_, &ibOffset_);
            if (span.armed())
                span.result("layout=%u vb=%u ib=%u topology=%u", layout_ ? 1u : 0u, count(vb_, kCtxVertexBufferSlots), ib_ ? 1u : 0u,
                            static_cast<unsigned>(topology_));
        }
        static const char* const kStep[kStageCount] = {"capture-vs", "capture-hs", "capture-ds", "capture-gs", "capture-ps", "capture-cs"};
        for (unsigned s = 0; s < kStageCount; ++s) {
            HdrCrumbSpan span(crumbs, kStep[s]);
            captureStage(c, s);
            if (s == kCs) c->CSGetUnorderedAccessViews(0, ranges_.uavs, csUav_);
            if (span.armed())
                span.result("shader=%u srv=%u cb=%u sampler=%u uav=%u", shaderBound(s) ? 1u : 0u, count(stage_[s].srv, kCtxSrvSlots),
                            count(stage_[s].cb, kCtxCbSlots), count(stage_[s].sampler, kCtxSamplerSlots),
                            s == kCs ? count(csUav_, kCtxUavSlots) : 0u);
        }
        {
            HdrCrumbSpan span(crumbs, "capture-so");
            c->SOGetTargets(kCtxSoSlots, so_);
            if (span.armed()) span.result("targets=%u", count(so_, kCtxSoSlots));
        }
        {
            HdrCrumbSpan span(crumbs, "capture-om");
            c->OMGetRenderTargetsAndUnorderedAccessViews(kCtxRtvSlots, rtv_, dsv_.GetAddressOf(), 0, ranges_.uavs, omUav_);
            c->OMGetBlendState(blend_.GetAddressOf(), blendFactor_, &sampleMask_);
            c->OMGetDepthStencilState(depthStencil_.GetAddressOf(), &stencilRef_);
            if (span.armed())
                span.result("rtv=%u dsv=%u uav=%u blend=%u depth-stencil=%u", count(rtv_, kCtxRtvSlots), dsv_ ? 1u : 0u,
                            count(omUav_, kCtxUavSlots), blend_ ? 1u : 0u, depthStencil_ ? 1u : 0u);
        }
        {
            HdrCrumbSpan span(crumbs, "capture-rs");
            c->RSGetState(raster_.GetAddressOf());
            viewportCount_ = kCtxViewportSlots;
            c->RSGetViewports(&viewportCount_, viewports_);
            scissorCount_ = kCtxViewportSlots;
            c->RSGetScissorRects(&scissorCount_, scissors_);
            if (viewportCount_ > kCtxViewportSlots) viewportCount_ = kCtxViewportSlots;
            if (scissorCount_ > kCtxViewportSlots) scissorCount_ = kCtxViewportSlots;
            if (span.armed()) span.result("state=%u viewports=%u scissors=%u", raster_ ? 1u : 0u, viewportCount_, scissorCount_);
        }
        {
            HdrCrumbSpan span(crumbs, "capture-predication");
            c->GetPredication(predicate_.GetAddressOf(), &predicateValue_);
            if (span.armed()) span.result("predicate=%u value=%d", predicate_ ? 1u : 0u, predicateValue_ ? 1 : 0);
        }
        held_ = true;
    }

    // Puts back what capture() took, on a context that has just been ClearState()d, and lets go of every reference.
    void restore(ID3D11DeviceContext1* c, bool crumbs) {
        if (!held_) return;
        {
            HdrCrumbSpan span(crumbs, "restore-ia");
            UINT first = 0, n = 0;
            if (layout_) c->IASetInputLayout(layout_.Get());
            if (topology_ != D3D_PRIMITIVE_TOPOLOGY_UNDEFINED) c->IASetPrimitiveTopology(topology_);
            if (run(vb_, ranges_.vertexBuffers, &first, &n)) c->IASetVertexBuffers(first, n, vb_ + first, vbStride_ + first, vbOffset_ + first);
            if (ib_ || ibFormat_ != DXGI_FORMAT_UNKNOWN || ibOffset_) c->IASetIndexBuffer(ib_.Get(), ibFormat_, ibOffset_);
        }
        static const char* const kStep[kStageCount] = {"restore-vs", "restore-hs", "restore-ds", "restore-gs", "restore-ps", "restore-cs"};
        for (unsigned s = 0; s < kStageCount; ++s) {
            HdrCrumbSpan span(crumbs, kStep[s]);
            restoreStage(c, s);
            UINT first = 0, n = 0;
            if (s == kCs && run(csUav_, ranges_.uavs, &first, &n)) c->CSSetUnorderedAccessViews(first, n, csUav_ + first, nullptr);
        }
        {
            HdrCrumbSpan span(crumbs, "restore-so");
            UINT n = 0;
            for (UINT i = 0; i < kCtxSoSlots; ++i)
                if (so_[i]) n = i + 1;
            if (n) {
                const UINT offsets[kCtxSoSlots] = {};
                c->SOSetTargets(n, so_, offsets);
            }
        }
        {
            HdrCrumbSpan span(crumbs, "restore-om");
            UINT rtvs = 0;
            for (UINT i = 0; i < kCtxRtvSlots; ++i)
                if (rtv_[i]) rtvs = i + 1;
            UINT uavFirst = 0, uavCount = 0;
            const bool uavs = run(omUav_, ranges_.uavs, &uavFirst, &uavCount);
            if (uavs)
                c->OMSetRenderTargetsAndUnorderedAccessViews(rtvs || dsv_ ? rtvs : D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, rtv_,
                                                             dsv_.Get(), uavFirst, uavCount, omUav_ + uavFirst, nullptr);
            else if (rtvs || dsv_)
                c->OMSetRenderTargets(rtvs, rtv_, dsv_.Get());
            c->OMSetBlendState(blend_.Get(), blendFactor_, sampleMask_);
            c->OMSetDepthStencilState(depthStencil_.Get(), stencilRef_);
        }
        {
            HdrCrumbSpan span(crumbs, "restore-rs");
            c->RSSetState(raster_.Get());
            if (viewportCount_) c->RSSetViewports(viewportCount_, viewports_);
            if (scissorCount_) c->RSSetScissorRects(scissorCount_, scissors_);
        }
        {
            HdrCrumbSpan span(crumbs, "restore-predication");
            if (predicate_) c->SetPredication(predicate_.Get(), predicateValue_);
        }
        release();
    }

    // Lets go of every reference the block holds. Safe to call at any time, and twice.
    void release() {
        layout_.Reset();
        ib_.Reset();
        vs_.Reset();
        hs_.Reset();
        ds_.Reset();
        gs_.Reset();
        ps_.Reset();
        cs_.Reset();
        dsv_.Reset();
        blend_.Reset();
        depthStencil_.Reset();
        raster_.Reset();
        predicate_.Reset();
        releaseAll(vb_);
        releaseAll(csUav_);
        releaseAll(so_);
        releaseAll(rtv_);
        releaseAll(omUav_);
        for (Slots& s : stage_) {
            releaseAll(s.srv);
            releaseAll(s.cb);
            releaseAll(s.sampler);
        }
        topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ibFormat_ = DXGI_FORMAT_UNKNOWN;
        ibOffset_ = 0;
        viewportCount_ = scissorCount_ = 0;
        predicateValue_ = FALSE;
        held_ = false;
    }

    // True between a capture and the restore (or release) that lets go.
    bool holding() const { return held_; }

private:
    struct Slots {
        ID3D11ShaderResourceView* srv[kCtxSrvSlots] = {};
        ID3D11Buffer* cb[kCtxCbSlots] = {};
        UINT cbFirst[kCtxCbSlots] = {};
        UINT cbCount[kCtxCbSlots] = {};
        ID3D11SamplerState* sampler[kCtxSamplerSlots] = {};
    };

    template <class T, UINT N>
    static void releaseAll(T* (&a)[N]) {
        for (T*& p : a)
            if (p) { p->Release(); p = nullptr; }
    }
    // How many of the first `n` entries hold an object.
    template <class T>
    static unsigned count(T* const* a, UINT n) {
        unsigned k = 0;
        for (UINT i = 0; i < n; ++i) k += a[i] ? 1u : 0u;
        return k;
    }
    // The run of slots from the first held entry to the last of the first `n`; false when none is held.
    template <class T>
    static bool run(T* const* a, UINT n, UINT* first, UINT* length) {
        UINT lo = n, hi = 0;
        for (UINT i = 0; i < n; ++i)
            if (a[i]) { if (lo == n) lo = i; hi = i; }
        if (lo == n) return false;
        *first = lo;
        *length = hi - lo + 1;
        return true;
    }
    bool shaderBound(unsigned s) const {
        switch (s) {
        case kVs: return vs_ != nullptr;
        case kHs: return hs_ != nullptr;
        case kDs: return ds_ != nullptr;
        case kGs: return gs_ != nullptr;
        case kPs: return ps_ != nullptr;
        default: return cs_ != nullptr;
        }
    }
    // Shader, shader resources, constant buffers (with their windows) and samplers of one stage. Class instances are not read.
    void captureStage(ID3D11DeviceContext1* c, unsigned s) {
        Slots& d = stage_[s];
        for (UINT i = 0; i < kCtxCbSlots; ++i) { d.cbFirst[i] = 0; d.cbCount[i] = D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT; }
        switch (s) {
        case kVs:
            c->VSGetShader(vs_.GetAddressOf(), nullptr, nullptr);
            c->VSGetShaderResources(0, kCtxSrvSlots, d.srv);
            c->VSGetConstantBuffers1(0, kCtxCbSlots, d.cb, d.cbFirst, d.cbCount);
            c->VSGetSamplers(0, kCtxSamplerSlots, d.sampler);
            break;
        case kHs:
            c->HSGetShader(hs_.GetAddressOf(), nullptr, nullptr);
            c->HSGetShaderResources(0, kCtxSrvSlots, d.srv);
            c->HSGetConstantBuffers1(0, kCtxCbSlots, d.cb, d.cbFirst, d.cbCount);
            c->HSGetSamplers(0, kCtxSamplerSlots, d.sampler);
            break;
        case kDs:
            c->DSGetShader(ds_.GetAddressOf(), nullptr, nullptr);
            c->DSGetShaderResources(0, kCtxSrvSlots, d.srv);
            c->DSGetConstantBuffers1(0, kCtxCbSlots, d.cb, d.cbFirst, d.cbCount);
            c->DSGetSamplers(0, kCtxSamplerSlots, d.sampler);
            break;
        case kGs:
            c->GSGetShader(gs_.GetAddressOf(), nullptr, nullptr);
            c->GSGetShaderResources(0, kCtxSrvSlots, d.srv);
            c->GSGetConstantBuffers1(0, kCtxCbSlots, d.cb, d.cbFirst, d.cbCount);
            c->GSGetSamplers(0, kCtxSamplerSlots, d.sampler);
            break;
        case kPs:
            c->PSGetShader(ps_.GetAddressOf(), nullptr, nullptr);
            c->PSGetShaderResources(0, kCtxSrvSlots, d.srv);
            c->PSGetConstantBuffers1(0, kCtxCbSlots, d.cb, d.cbFirst, d.cbCount);
            c->PSGetSamplers(0, kCtxSamplerSlots, d.sampler);
            break;
        default:
            c->CSGetShader(cs_.GetAddressOf(), nullptr, nullptr);
            c->CSGetShaderResources(0, kCtxSrvSlots, d.srv);
            c->CSGetConstantBuffers1(0, kCtxCbSlots, d.cb, d.cbFirst, d.cbCount);
            c->CSGetSamplers(0, kCtxSamplerSlots, d.sampler);
            break;
        }
        // Count offset bindings from the state query already made above.
        for (UINT i=3;i<kCtxCbSlots;++i)
            if (d.cb[i] && d.cbFirst[i]) ++g_flatCbFirstNonzero;
    }
    void restoreStage(ID3D11DeviceContext1* c, unsigned s) {
        Slots& d = stage_[s];
        UINT srvLo = 0, srvN = 0, cbLo = 0, cbN = 0, smpLo = 0, smpN = 0;
        const bool srvs = run(d.srv, kCtxSrvSlots, &srvLo, &srvN);
        const bool cbs = run(d.cb, kCtxCbSlots, &cbLo, &cbN);
        const bool smps = run(d.sampler, kCtxSamplerSlots, &smpLo, &smpN);
        switch (s) {
        case kVs:
            if (vs_) c->VSSetShader(vs_.Get(), nullptr, 0);
            if (srvs) c->VSSetShaderResources(srvLo, srvN, d.srv + srvLo);
            if (cbs) c->VSSetConstantBuffers1(cbLo, cbN, d.cb + cbLo, d.cbFirst + cbLo, d.cbCount + cbLo);
            if (smps) c->VSSetSamplers(smpLo, smpN, d.sampler + smpLo);
            break;
        case kHs:
            if (hs_) c->HSSetShader(hs_.Get(), nullptr, 0);
            if (srvs) c->HSSetShaderResources(srvLo, srvN, d.srv + srvLo);
            if (cbs) c->HSSetConstantBuffers1(cbLo, cbN, d.cb + cbLo, d.cbFirst + cbLo, d.cbCount + cbLo);
            if (smps) c->HSSetSamplers(smpLo, smpN, d.sampler + smpLo);
            break;
        case kDs:
            if (ds_) c->DSSetShader(ds_.Get(), nullptr, 0);
            if (srvs) c->DSSetShaderResources(srvLo, srvN, d.srv + srvLo);
            if (cbs) c->DSSetConstantBuffers1(cbLo, cbN, d.cb + cbLo, d.cbFirst + cbLo, d.cbCount + cbLo);
            if (smps) c->DSSetSamplers(smpLo, smpN, d.sampler + smpLo);
            break;
        case kGs:
            if (gs_) c->GSSetShader(gs_.Get(), nullptr, 0);
            if (srvs) c->GSSetShaderResources(srvLo, srvN, d.srv + srvLo);
            if (cbs) c->GSSetConstantBuffers1(cbLo, cbN, d.cb + cbLo, d.cbFirst + cbLo, d.cbCount + cbLo);
            if (smps) c->GSSetSamplers(smpLo, smpN, d.sampler + smpLo);
            break;
        case kPs:
            if (ps_) c->PSSetShader(ps_.Get(), nullptr, 0);
            if (srvs) c->PSSetShaderResources(srvLo, srvN, d.srv + srvLo);
            if (cbs) c->PSSetConstantBuffers1(cbLo, cbN, d.cb + cbLo, d.cbFirst + cbLo, d.cbCount + cbLo);
            if (smps) c->PSSetSamplers(smpLo, smpN, d.sampler + smpLo);
            break;
        default:
            if (cs_) c->CSSetShader(cs_.Get(), nullptr, 0);
            if (srvs) c->CSSetShaderResources(srvLo, srvN, d.srv + srvLo);
            if (cbs) c->CSSetConstantBuffers1(cbLo, cbN, d.cb + cbLo, d.cbFirst + cbLo, d.cbCount + cbLo);
            if (smps) c->CSSetSamplers(smpLo, smpN, d.sampler + smpLo);
            break;
        }
    }

    FlatContextRanges ranges_;
    bool held_ = false;
    // IA
    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout_;
    D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer* vb_[kCtxVertexBufferSlots] = {};
    UINT vbStride_[kCtxVertexBufferSlots] = {};
    UINT vbOffset_[kCtxVertexBufferSlots] = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> ib_;
    DXGI_FORMAT ibFormat_ = DXGI_FORMAT_UNKNOWN;
    UINT ibOffset_ = 0;
    // The six stages
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11HullShader> hs_;
    Microsoft::WRL::ComPtr<ID3D11DomainShader> ds_;
    Microsoft::WRL::ComPtr<ID3D11GeometryShader> gs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> cs_;
    Slots stage_[kStageCount];
    ID3D11UnorderedAccessView* csUav_[kCtxUavSlots] = {};
    // SO
    ID3D11Buffer* so_[kCtxSoSlots] = {};
    // OM
    ID3D11RenderTargetView* rtv_[kCtxRtvSlots] = {};
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv_;
    ID3D11UnorderedAccessView* omUav_[kCtxUavSlots] = {};
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    FLOAT blendFactor_[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    UINT sampleMask_ = 0xffffffffu;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStencil_;
    UINT stencilRef_ = 0;
    // RS
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    UINT viewportCount_ = 0, scissorCount_ = 0;
    D3D11_VIEWPORT viewports_[kCtxViewportSlots] = {};
    D3D11_RECT scissors_[kCtxViewportSlots] = {};
    // Predication
    Microsoft::WRL::ComPtr<ID3D11Predicate> predicate_;
    BOOL predicateValue_ = FALSE;
};

}  // namespace edvr

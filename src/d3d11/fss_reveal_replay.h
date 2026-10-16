// Re-running a draw from a saved copy of the state it was issued with, and putting the context back exactly as it was (flight d7769063, docs/fss-scanner.md "v11 flown").
//
// The reveal's coarse input t1 is a 63 x 63 downsample of the ring layer that the left eye's pass draws between the ring's two draw groups. Replaying that draw just before the left reveal
// composite, from the finished layer, gives the left eye the t1 the right eye already gets. This header is the mechanism: capture every piece of pipeline state a draw uses (constant buffers
// by COPY, so a Map between the draw and the replay cannot change what it reads), apply it, draw, and apply the state that was bound before. The rig runs the same code on a WARP device and
// compares the state before and after; the hook supplies the game's un-hooked entry points so none of its own state tracking sees the replay.
#pragma once

#include <d3d11_1.h>

#include <cstdint>
#include <cstring>

#include "gpu_census.h"   // GpuCensusScope: the fix's own GPU work is priced in the 30 s census (FrameFssReveal)

namespace edvr {
namespace fssreveal {

constexpr uint32_t kRsCb = 4;        // constant buffer slots b0-b3 of each stage
constexpr uint32_t kRsSmp = 4;       // sampler slots s0-s3
constexpr uint32_t kRsVsSrv = 4;     // vertex-shader textures t0-t3
constexpr uint32_t kRsPsSrv = 8;     // pixel-shader textures t0-t7
constexpr uint32_t kRsRtv = 8;
constexpr uint32_t kRsVp = 16;       // viewports and scissor rectangles
constexpr UINT     kRsCbMaxBytes = 65536;

// Un-hooked entry points, any of which may be null (the context's own member is then called). The game's hooks track and sometimes change state; a replay that went through them would be
// seen by them. vscreen passes the original function pointers it kept.
struct ReplayCalls {
    void (STDMETHODCALLTYPE* vsSetCb)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*) = nullptr;
    void (STDMETHODCALLTYPE* psSetCb)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*) = nullptr;
    void (STDMETHODCALLTYPE* psSetSrv)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*) = nullptr;
    void (STDMETHODCALLTYPE* vsSetSrv)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*) = nullptr;
    void (STDMETHODCALLTYPE* vsSetShader)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT) = nullptr;
    void (STDMETHODCALLTYPE* psSetShader)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT) = nullptr;
    void (STDMETHODCALLTYPE* omSetRts)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*) = nullptr;
    void (STDMETHODCALLTYPE* omSetBlend)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT[4], UINT) = nullptr;
    void (STDMETHODCALLTYPE* rsSetViewports)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*) = nullptr;
    void (STDMETHODCALLTYPE* drawInstanced)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT) = nullptr;
};

// Every piece of pipeline state a DrawInstanced of a fullscreen triangle uses. All COM pointers are references this struct owns.
struct DrawState {
    bool valid = false;
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* vb = nullptr;
    UINT vbStride = 0, vbOffset = 0;
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN;
    UINT ibOffset = 0;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* vs = nullptr;
    ID3D11Buffer* vsCb[kRsCb] = {};
    UINT vsFirst[kRsCb] = {}, vsNum[kRsCb] = {};
    ID3D11ShaderResourceView* vsSrv[kRsVsSrv] = {};
    ID3D11SamplerState* vsSmp[kRsSmp] = {};
    ID3D11GeometryShader* gs = nullptr;
    ID3D11RasterizerState* rs = nullptr;
    UINT nvp = 0, nsc = 0;
    D3D11_VIEWPORT vp[kRsVp] = {};
    D3D11_RECT sc[kRsVp] = {};
    ID3D11PixelShader* ps = nullptr;
    ID3D11ShaderResourceView* psSrv[kRsPsSrv] = {};
    ID3D11SamplerState* psSmp[kRsSmp] = {};
    ID3D11Buffer* psCb[kRsCb] = {};
    UINT psFirst[kRsCb] = {}, psNum[kRsCb] = {};
    ID3D11RenderTargetView* rtv[kRsRtv] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* bs = nullptr;
    FLOAT bf[4] = {0, 0, 0, 0};
    UINT sampleMask = 0xFFFFFFFFu;
    ID3D11DepthStencilState* dss = nullptr;
    UINT stencilRef = 0;
    UINT perInstance = 0, instances = 0, startVertex = 0, startInstance = 0;

    template <class T> static void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
    void release() {
        rel(layout); rel(vb); rel(ib); rel(vs); rel(gs); rel(rs); rel(ps); rel(dsv); rel(bs); rel(dss);
        for (auto& x : vsCb) rel(x);
        for (auto& x : vsSrv) rel(x);
        for (auto& x : vsSmp) rel(x);
        for (auto& x : psSrv) rel(x);
        for (auto& x : psSmp) rel(x);
        for (auto& x : psCb) rel(x);
        for (auto& x : rtv) rel(x);
        *this = DrawState();
    }
};

// Private copies of the constant buffers a record read, reused from frame to frame (a CreateBuffer a frame would cost more than the copy).
struct CbPool {
    ID3D11Buffer* buf[2][kRsCb] = {};
    UINT bytes[2][kRsCb] = {};
    void release() {
        for (auto& st : buf) for (auto& b : st) DrawState::rel(b);
        for (auto& st : bytes) for (auto& n : st) n = 0;
    }
};

// A constant buffer bound whole reads back as (0, 0) or (0, 4096) or (0, its size in constants) depending on the API that bound it; those are the same binding.
inline bool sameCbRange(ID3D11Buffer* buf, UINT f1, UINT n1, UINT f2, UINT n2) {
    if (!buf) return true;
    D3D11_BUFFER_DESC bd{};
    buf->GetDesc(&bd);
    auto whole = [&](UINT f, UINT n) { return f == 0 && (n == 0 || n >= 4096u || n * 16u >= bd.ByteWidth); };
    if (whole(f1, n1) && whole(f2, n2)) return true;
    return f1 == f2 && n1 == n2;
}

inline bool sameState(const DrawState& a, const DrawState& b) {
    if (a.layout != b.layout || a.vb != b.vb || a.vbStride != b.vbStride || a.vbOffset != b.vbOffset || a.ib != b.ib || a.ibFmt != b.ibFmt || a.ibOffset != b.ibOffset || a.topo != b.topo) return false;
    if (a.vs != b.vs || a.gs != b.gs || a.rs != b.rs || a.ps != b.ps || a.dsv != b.dsv || a.bs != b.bs || a.dss != b.dss || a.stencilRef != b.stencilRef || a.sampleMask != b.sampleMask) return false;
    if (std::memcmp(a.bf, b.bf, sizeof(a.bf)) != 0 || a.nvp != b.nvp || a.nsc != b.nsc) return false;
    if (std::memcmp(a.vp, b.vp, a.nvp * sizeof(D3D11_VIEWPORT)) != 0 || std::memcmp(a.sc, b.sc, a.nsc * sizeof(D3D11_RECT)) != 0) return false;
    for (uint32_t i = 0; i < kRsCb; ++i) {
        if (a.vsCb[i] != b.vsCb[i] || a.psCb[i] != b.psCb[i]) return false;
        if (!sameCbRange(a.vsCb[i], a.vsFirst[i], a.vsNum[i], b.vsFirst[i], b.vsNum[i])) return false;
        if (!sameCbRange(a.psCb[i], a.psFirst[i], a.psNum[i], b.psFirst[i], b.psNum[i])) return false;
    }
    for (uint32_t i = 0; i < kRsVsSrv; ++i) if (a.vsSrv[i] != b.vsSrv[i]) return false;
    for (uint32_t i = 0; i < kRsSmp; ++i) if (a.vsSmp[i] != b.vsSmp[i] || a.psSmp[i] != b.psSmp[i]) return false;
    for (uint32_t i = 0; i < kRsPsSrv; ++i) if (a.psSrv[i] != b.psSrv[i]) return false;
    for (uint32_t i = 0; i < kRsRtv; ++i) if (a.rtv[i] != b.rtv[i]) return false;
    return true;
}

namespace detail_replay {
inline void getCbs(ID3D11DeviceContext* ctx, ID3D11DeviceContext1* c1, bool vs, ID3D11Buffer** b, UINT* first, UINT* num) {
    if (c1) {
        if (vs) c1->VSGetConstantBuffers1(0, kRsCb, b, first, num);
        else c1->PSGetConstantBuffers1(0, kRsCb, b, first, num);
    } else if (vs) {
        ctx->VSGetConstantBuffers(0, kRsCb, b);
    } else {
        ctx->PSGetConstantBuffers(0, kRsCb, b);
    }
}
// Replaces each bound buffer by a private copy of the range it exposes. False if any copy could not be made.
inline bool snapshotCbs(ID3D11Device* dev, ID3D11DeviceContext* ctx, CbPool& pool, uint32_t stage, ID3D11Buffer** b, UINT* first, UINT* num) {
    for (uint32_t i = 0; i < kRsCb; ++i) {
        if (!b[i]) continue;
        D3D11_BUFFER_DESC bd{};
        b[i]->GetDesc(&bd);
        const UINT off = first[i] * 16u;
        if (off >= bd.ByteWidth) return false;
        UINT range = (num[i] && num[i] < 4096u) ? num[i] * 16u : bd.ByteWidth - off;
        if (range > bd.ByteWidth - off) range = bd.ByteWidth - off;
        if (range > kRsCbMaxBytes) range = kRsCbMaxBytes;
        range &= ~15u;
        if (!range) return false;
        if (!pool.buf[stage][i] || pool.bytes[stage][i] != range) {
            DrawState::rel(pool.buf[stage][i]);
            D3D11_BUFFER_DESC nd{};
            nd.ByteWidth = range;
            nd.Usage = D3D11_USAGE_DEFAULT;
            nd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(dev->CreateBuffer(&nd, nullptr, &pool.buf[stage][i])) || !pool.buf[stage][i]) { pool.buf[stage][i] = nullptr; return false; }
            pool.bytes[stage][i] = range;
        }
        D3D11_BOX box{};
        box.left = off;
        box.right = off + range;
        box.bottom = box.back = 1;
        ctx->CopySubresourceRegion(pool.buf[stage][i], 0, 0, 0, 0, b[i], 0, &box);
        b[i]->Release();
        b[i] = pool.buf[stage][i];
        b[i]->AddRef();
        first[i] = 0;
        num[i] = 0;
    }
    return true;
}
}  // namespace detail_replay

// Reads the state the next draw will use. With `pool` the constant buffers are replaced by private copies of their bound ranges (taken now, on the GPU timeline).
inline bool captureState(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11DeviceContext1* c1, CbPool* pool, DrawState& s) {
    s.release();
    ctx->IAGetInputLayout(&s.layout);
    ctx->IAGetVertexBuffers(0, 1, &s.vb, &s.vbStride, &s.vbOffset);
    ctx->IAGetIndexBuffer(&s.ib, &s.ibFmt, &s.ibOffset);
    ctx->IAGetPrimitiveTopology(&s.topo);
    ctx->VSGetShader(&s.vs, nullptr, nullptr);
    detail_replay::getCbs(ctx, c1, true, s.vsCb, s.vsFirst, s.vsNum);
    ctx->VSGetShaderResources(0, kRsVsSrv, s.vsSrv);
    ctx->VSGetSamplers(0, kRsSmp, s.vsSmp);
    ctx->GSGetShader(&s.gs, nullptr, nullptr);
    ctx->RSGetState(&s.rs);
    s.nvp = kRsVp;
    ctx->RSGetViewports(&s.nvp, s.vp);
    s.nsc = kRsVp;
    ctx->RSGetScissorRects(&s.nsc, s.sc);
    ctx->PSGetShader(&s.ps, nullptr, nullptr);
    ctx->PSGetShaderResources(0, kRsPsSrv, s.psSrv);
    ctx->PSGetSamplers(0, kRsSmp, s.psSmp);
    detail_replay::getCbs(ctx, c1, false, s.psCb, s.psFirst, s.psNum);
    ctx->OMGetRenderTargets(kRsRtv, s.rtv, &s.dsv);
    ctx->OMGetBlendState(&s.bs, s.bf, &s.sampleMask);
    ctx->OMGetDepthStencilState(&s.dss, &s.stencilRef);
    s.valid = true;
    if (pool) {
        // The private copies of the constant buffers are GPU work (CopySubresourceRegion), so they are priced; the state reads above are CPU-only and are not. The pool is
        // null for the replay's own before/after read-backs, which therefore open no scope.
        GpuCensusScope census(ctx, GpuCensusSection::FrameFssReveal);
        census.markIssued();
        if (!detail_replay::snapshotCbs(dev, ctx, *pool, 0, s.vsCb, s.vsFirst, s.vsNum) || !detail_replay::snapshotCbs(dev, ctx, *pool, 1, s.psCb, s.psFirst, s.psNum)) {
            s.release();
            return false;
        }
    }
    return true;
}

namespace detail_replay {
inline void setCbs(ID3D11DeviceContext* ctx, ID3D11DeviceContext1* c1, const ReplayCalls& rc, bool vs, ID3D11Buffer* const* b, const UINT* first, const UINT* num) {
    for (uint32_t i = 0; i < kRsCb; ++i) {
        ID3D11Buffer* buf = b[i];
        bool ranged = false;
        if (c1 && buf && num[i] && num[i] < 4096u) {
            D3D11_BUFFER_DESC bd{};
            buf->GetDesc(&bd);
            ranged = first[i] != 0 || num[i] * 16u < bd.ByteWidth;
        }
        if (ranged) {
            UINT f = first[i], n = num[i];
            if (vs) c1->VSSetConstantBuffers1(i, 1, &buf, &f, &n);
            else c1->PSSetConstantBuffers1(i, 1, &buf, &f, &n);
        } else if (vs) {
            if (rc.vsSetCb) rc.vsSetCb(ctx, i, 1, &buf);
            else ctx->VSSetConstantBuffers(i, 1, &buf);
        } else {
            if (rc.psSetCb) rc.psSetCb(ctx, i, 1, &buf);
            else ctx->PSSetConstantBuffers(i, 1, &buf);
        }
    }
}
inline void setPsSrvs(ID3D11DeviceContext* ctx, const ReplayCalls& rc, const DrawState& s) {
    if (rc.psSetSrv) rc.psSetSrv(ctx, 0, kRsPsSrv, s.psSrv);
    else ctx->PSSetShaderResources(0, kRsPsSrv, s.psSrv);
}
inline void setRts(ID3D11DeviceContext* ctx, const ReplayCalls& rc, const DrawState& s) {
    if (rc.omSetRts) rc.omSetRts(ctx, kRsRtv, s.rtv, s.dsv);
    else ctx->OMSetRenderTargets(kRsRtv, s.rtv, s.dsv);
}
}  // namespace detail_replay

// Binds `s`. A texture that is a render target in `s` must not still be bound as an input: a replay (srvFirst) unbinds the inputs of the state it leaves before the targets change; a restore sets
// the targets first and then the inputs, so the texture the replay drew into is a render target for no longer than the replay.
inline void applyState(ID3D11DeviceContext* ctx, ID3D11DeviceContext1* c1, const ReplayCalls& rc, const DrawState& s, bool srvFirst) {
    ctx->IASetInputLayout(s.layout);
    ctx->IASetVertexBuffers(0, 1, &s.vb, &s.vbStride, &s.vbOffset);
    ctx->IASetIndexBuffer(s.ib, s.ibFmt, s.ibOffset);
    ctx->IASetPrimitiveTopology(s.topo);
    if (rc.vsSetShader) rc.vsSetShader(ctx, s.vs, nullptr, 0);
    else ctx->VSSetShader(s.vs, nullptr, 0);
    detail_replay::setCbs(ctx, c1, rc, true, s.vsCb, s.vsFirst, s.vsNum);
    if (rc.vsSetSrv) rc.vsSetSrv(ctx, 0, kRsVsSrv, s.vsSrv);
    else ctx->VSSetShaderResources(0, kRsVsSrv, s.vsSrv);
    ctx->VSSetSamplers(0, kRsSmp, s.vsSmp);
    ctx->GSSetShader(s.gs, nullptr, 0);
    ctx->RSSetState(s.rs);
    if (rc.rsSetViewports) rc.rsSetViewports(ctx, s.nvp, s.vp);
    else ctx->RSSetViewports(s.nvp, s.vp);
    ctx->RSSetScissorRects(s.nsc, s.sc);
    if (rc.psSetShader) rc.psSetShader(ctx, s.ps, nullptr, 0);
    else ctx->PSSetShader(s.ps, nullptr, 0);
    ctx->PSSetSamplers(0, kRsSmp, s.psSmp);
    detail_replay::setCbs(ctx, c1, rc, false, s.psCb, s.psFirst, s.psNum);
    if (srvFirst) detail_replay::setPsSrvs(ctx, rc, s);
    detail_replay::setRts(ctx, rc, s);
    if (!srvFirst) detail_replay::setPsSrvs(ctx, rc, s);
    if (rc.omSetBlend) rc.omSetBlend(ctx, s.bs, s.bf, s.sampleMask);
    else ctx->OMSetBlendState(s.bs, s.bf, s.sampleMask);
    ctx->OMSetDepthStencilState(s.dss, s.stencilRef);
}

// Runs the draw `rec` again and leaves the context as it found it. `verified` is true when the state read back afterwards equals the state read before. False return: nothing was drawn.
inline bool replayDraw(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11DeviceContext1* c1, const ReplayCalls& rc, const DrawState& rec, bool* verified) {
    DrawState before;
    if (!rec.valid || !captureState(dev, ctx, c1, nullptr, before)) return false;
    {
        // From here GPU work is issued: the scope covers the replay draw and the two state applications around it (not the no-op returns above, and not the verification read-back below).
        GpuCensusScope census(ctx, GpuCensusSection::FrameFssReveal);
        census.markIssued();
        applyState(ctx, c1, rc, rec, true);
        if (rc.drawInstanced) rc.drawInstanced(ctx, rec.perInstance, rec.instances, rec.startVertex, rec.startInstance);
        else ctx->DrawInstanced(rec.perInstance, rec.instances, rec.startVertex, rec.startInstance);
        applyState(ctx, c1, rc, before, false);
    }
    if (verified) {
        DrawState after;
        captureState(dev, ctx, c1, nullptr, after);
        *verified = sameState(before, after);
        after.release();
    }
    before.release();
    return true;
}

}  // namespace fssreveal
}  // namespace edvr

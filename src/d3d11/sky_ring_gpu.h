#pragma once
// The sky ring's GPU half (I-2 of docs\sky-flicker-2026-10-13.md; TEMPORARY, diag.sky_ring): the ring buffer, the per-eye-frame dispatch and the
// asynchronous readback. Header-only so tools\sky_flicker_test can drive it on WARP with the shipped HLSL and no other part of the DLL. No globals, no
// logging: sky_ring.cpp owns the policy (the key, the shader, the requests, the lines).
//
// The ring is a raw 64 KB buffer of 2048 32-byte entries (src\common\sky_ring.h), zero-filled at creation so an empty slot reads seq 0. A dispatch
// writes entry number `seq` into slot (seq - 1) % 2048. A readback copies the ring into a staging buffer and polls it with D3D11_MAP_FLAG_DO_NOT_WAIT
// on later frames: it never waits, and never costs the frame a stall.

#include <d3d11.h>
#include <cstdint>
#include <cstring>
#include <vector>

#include "sky_ring_shader.h"

namespace edvr {

class SkyRingGpu {
public:
    static constexpr int kStaging = 2;   // readbacks in flight at once (the Pause key dumps twice, two seconds apart)

    ~SkyRingGpu() { release(); }

    // cs is borrowed (the caller keeps its reference; this holds its own).
    bool create(ID3D11Device* dev, ID3D11ComputeShader* cs) {
        release();
        if (!dev || !cs) return false;
        dev_ = dev; dev_->AddRef();
        cs_ = cs; cs_->AddRef();
        std::vector<uint32_t> zeros(skyring::kRingBytes / 4, 0u);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = skyring::kRingBytes;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_SUBRESOURCE_DATA init{zeros.data(), 0, 0};
        if (FAILED(dev->CreateBuffer(&bd, &init, &ring_))) { release(); return false; }
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = skyring::kRingBytes / 4;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(dev->CreateUnorderedAccessView(ring_, &ud, &uav_))) { release(); return false; }
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(SkyRingParams);
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&cb, nullptr, &cb_))) { release(); return false; }
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth = skyring::kRingBytes;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (int i = 0; i < kStaging; ++i)
            if (FAILED(dev->CreateBuffer(&sd, nullptr, &staging_[i]))) { release(); return false; }
        return true;
    }

    void release() {
        for (auto& s : staging_) { if (s) { s->Release(); s = nullptr; } busy_[&s - staging_] = false; }
        if (cb_) { cb_->Release(); cb_ = nullptr; }
        if (uav_) { uav_->Release(); uav_ = nullptr; }
        if (ring_) { ring_->Release(); ring_ = nullptr; }
        if (cs_) { cs_->Release(); cs_ = nullptr; }
        if (dev_) { dev_->Release(); dev_ = nullptr; }
        dispatches_ = 0;
    }

    bool ready() const { return ring_ != nullptr; }
    ID3D11Device* device() const { return dev_; }
    uint64_t dispatches() const { return dispatches_; }

    // One eye-frame. raw: R8G8B8A8 colour at the input size; out: the upscaler output; mv: R16G16F at the input size; depth: R32F at the input size
    // (the depth copy). Binds its own compute state and leaves every slot it used unbound; the caller restores whatever else it needs.
    bool dispatch(ID3D11DeviceContext* ctx, uint32_t eye, uint32_t frame, ID3D11ShaderResourceView* raw, ID3D11ShaderResourceView* out,
                  ID3D11ShaderResourceView* mv, ID3D11ShaderResourceView* depth, uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH,
                  float projA, bool depthValid) {
        if (!ready() || !ctx || !raw || !out || !mv || !depth || !inW || !inH || !outW || !outH) return false;
        ++dispatches_;
        const uint32_t seq = static_cast<uint32_t>(dispatches_);
        SkyRingParams p{};
        p.slotByte = skyring::slotOf(seq) * skyring::kEntryBytes;
        p.frame = frame;
        p.eyeFlags = (eye & 1u) | (depthValid ? 2u : 0u);
        p.seq = seq;
        p.inW = inW; p.inH = inH; p.outW = outW; p.outH = outH;
        p.projA = projA;
        const uint32_t ratio = outW / inW;
        p.outSide = ratio < 1u ? 1u : (ratio > 2u ? 2u : ratio);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) return false;
        std::memcpy(m.pData, &p, sizeof(p));
        ctx->Unmap(cb_, 0);
        ID3D11ShaderResourceView* srvs[4] = {raw, out, mv, depth};
        UINT keep = 0;
        ctx->CSSetShader(cs_, nullptr, 0);
        ctx->CSSetShaderResources(0, 4, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, &uav_, &keep);
        ctx->CSSetConstantBuffers(0, 1, &cb_);
        ctx->Dispatch(1, 1, 1);
        ID3D11ShaderResourceView* nullSrv[4] = {};
        ID3D11UnorderedAccessView* nullUav = nullptr;
        ctx->CSSetShaderResources(0, 4, nullSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, &keep);
        return true;
    }

    // Copies the ring into a free staging buffer; returns its handle (0 or 1), or -1 when both are busy. The copy is ordered after every dispatch so far.
    int requestCopy(ID3D11DeviceContext* ctx) {
        if (!ready() || !ctx) return -1;
        for (int i = 0; i < kStaging; ++i) {
            if (busy_[i]) continue;
            ctx->CopyResource(staging_[i], ring_);
            busy_[i] = true;
            return i;
        }
        return -1;
    }

    // Polls a copy without waiting. True when it was ready: the entries are decoded into out (oldest first) and the buffer is free again. False means
    // not ready yet (try next frame), or the map failed outright, which frees the buffer too and sets *failed.
    bool poll(ID3D11DeviceContext* ctx, int handle, std::vector<skyring::Entry>* out, bool* failed = nullptr) {
        if (failed) *failed = false;
        if (handle < 0 || handle >= kStaging || !busy_[handle] || !ctx) { if (failed) *failed = true; return false; }
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = ctx->Map(staging_[handle], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
        busy_[handle] = false;
        if (FAILED(hr) || !m.pData) { if (failed) *failed = true; return false; }
        *out = skyring::decodeRing(static_cast<const uint32_t*>(m.pData));
        ctx->Unmap(staging_[handle], 0);
        return true;
    }

private:
    ID3D11Device* dev_ = nullptr;
    ID3D11ComputeShader* cs_ = nullptr;
    ID3D11Buffer* ring_ = nullptr;
    ID3D11UnorderedAccessView* uav_ = nullptr;
    ID3D11Buffer* cb_ = nullptr;
    ID3D11Buffer* staging_[kStaging] = {};
    bool busy_[kStaging] = {};
    uint64_t dispatches_ = 0;
};

}  // namespace edvr

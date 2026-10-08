#pragma once
// THE SHADOW OF THE SIBLING MODEL, on the GPU (flat_foreground_shadow.h says what it measures; the two compute shaders are in
// flat_foreground_motion_shader.h). It owns the shaders, the moments and result tables, the constants and the staging the results are read
// back through a few frames late. It reads the frame's captured draws and writes nothing the map, the sibling pass or the backend reads: a
// frame that samples costs two dispatches a draw and a copy, and a frame that does not costs nothing.
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include "cs_stage_save.h"
#include "flat_foreground_shadow.h"
#include "shader_swap.h"
#include "temporal_shader_bytecode.h"

namespace edvr {

class FlatForegroundShadow {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
public:
    static constexpr unsigned kDraws = 128, kSlots = 4;
    // One draw of the frame: the eleven views the donor pass binds (t0 the draw's current positions, t1..t4 its priors', t5 its identity words, t6..t9
    // the priors' identity words, t10 the retained instance index), the map's constants for it (with sibling.y its index, sibling.z its vertex
    // count and sibling.w the frame's draw count), and whether the first shader runs for it (it has priors).
    struct DrawInput {
        ID3D11ShaderResourceView* views[11] = {};
        const void* constants = nullptr;
        unsigned constantBytes = 0;
        bool dispatchMoments = false;
    };

    // The two passes for a frame's draws, and the copy of the results to staging. False when the frame could not be taken (no free slot, a resource
    // that could not be made, too many draws); the map and the sibling pass are unaffected either way.
    bool run(ID3D11DeviceContext* ctx, const DrawInput* draws, unsigned n, unsigned frame) {
        if (!ctx || !draws || !n || n > kDraws || !draws[0].constants || !draws[0].constantBytes) return false;
        Slot* slot = nullptr;
        for (Slot& s : slots_) if (!s.pending) { slot = &s; break; }
        if (!slot) return false;
        if (!create(ctx, draws[0].constantBytes)) { ++stats_.failed; return false; }
        if (!slot->stage) {
            Ptr<ID3D11Device> dev;
            ctx->GetDevice(&dev);
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = kDraws * 3 * 16; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; d.StructureByteStride = 16;
            if (FAILED(dev->CreateBuffer(&d, nullptr, &slot->stage))) { ++stats_.failed; return false; }
        }
        CsStageSave saved;
        saved.save(ctx);
        const UINT zero[4]{};
        ID3D11UnorderedAccessView* noUav = nullptr;
        ID3D11ShaderResourceView* noViews[12]{};
        ctx->ClearUnorderedAccessViewUint(momentsUav_.Get(), zero);
        ctx->ClearUnorderedAccessViewUint(resultsUav_.Get(), zero);
        ctx->CSSetConstantBuffers(0, 1, constants_.GetAddressOf());
        // The first pass: the moments of every draw that has priors.
        ctx->CSSetShader(momentsCs_.Get(), nullptr, 0);
        ID3D11UnorderedAccessView* uav = momentsUav_.Get();
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        for (unsigned i = 0; i < n; ++i) {
            if (!draws[i].dispatchMoments) continue;
            ctx->UpdateSubresource(constants_.Get(), 0, nullptr, draws[i].constants, 0, 0);
            ctx->CSSetShaderResources(0, 11, draws[i].views);
            ctx->Dispatch(1, 1, 1);
        }
        ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
        ctx->CSSetShaderResources(0, 11, noViews);
        // The second: every draw, against the first's table.
        ctx->CSSetShader(evalCs_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* moments = momentsSrv_.Get();
        ctx->CSSetShaderResources(11, 1, &moments);
        uav = resultsUav_.Get();
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        for (unsigned i = 0; i < n; ++i) {
            ctx->UpdateSubresource(constants_.Get(), 0, nullptr, draws[i].constants, 0, 0);
            ctx->CSSetShaderResources(0, 11, draws[i].views);
            ctx->CSSetShaderResources(11, 1, &moments);
            ctx->Dispatch(1, 1, 1);
        }
        ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
        ctx->CSSetShaderResources(0, 12, noViews);
        saved.restore(ctx);
        const D3D11_BOX box{0, 0, 0, n * 3 * 16, 1, 1};
        ctx->CopySubresourceRegion(slot->stage.Get(), 0, 0, 0, 0, results_.Get(), 0, &box);
        slot->pending = true; slot->frame = frame; slot->draws = n;
        return true;
    }

    // Reads the results that are ready (all of them, waiting, in a rig). The capture does it once a frame beside the other readbacks.
    void poll(ID3D11DeviceContext* ctx, unsigned frame, bool wait = false) {
        if (!ctx) return;
        for (Slot& s : slots_) {
            if (!s.pending) continue;
            if (!wait && frame - s.frame < 2) continue;
            D3D11_MAPPED_SUBRESOURCE m{};
            const HRESULT hr = ctx->Map(s.stage.Get(), 0, D3D11_MAP_READ, wait ? 0u : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            if (FAILED(hr) || !m.pData) {
                if (frame - s.frame > 8) { s.pending = false; ++stats_.notReady; }
                continue;
            }
            std::vector<ShadowRecord> records(s.draws);
            const float* f = static_cast<const float*>(m.pData);
            for (unsigned d = 0; d < s.draws; ++d) records[d] = decode(f + d * 12);
            ctx->Unmap(s.stage.Get(), 0);
            s.pending = false;
            flatShadowAccumulate(stats_, records.data(), s.draws);
            lastRecords_ = std::move(records);
        }
    }

    // FOR THE RIGS ONLY: the last read's records (poll with wait to have read the frame just run).
    const std::vector<ShadowRecord>& lastRecords() const { return lastRecords_; }
    const ShadowStats& stats() const { return stats_; }
    static ShadowRecord decode(const float* f) {
        ShadowRecord r;
        r.kind = f[0]; r.donorDraws = f[1]; r.rmsMean = f[2]; r.rmsAffine = f[3];
        r.residual = f[4]; r.spread = f[5]; r.pooled = f[6]; r.gates = f[7];
        r.ownMean[0] = f[8]; r.ownMean[1] = f[9]; r.evaluated = f[10];
        return r;
    }

private:
    struct Slot { Ptr<ID3D11Buffer> stage; bool pending = false; unsigned frame = 0, draws = 0; };
    bool create(ID3D11DeviceContext* ctx, unsigned constantBytes) {
        if (momentsCs_ && evalCs_ && moments_ && results_ && constants_ && constantBytes_ == constantBytes) return true;
        Ptr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        if (!momentsCs_) momentsCs_.Attach(shaderSwapCreateCs(ctx, kFlatForegroundShadowMomentsBytecode, sizeof(kFlatForegroundShadowMomentsBytecode),
                                                              "flat foreground shadow moments", "flat foreground motion"));
        if (!evalCs_) evalCs_.Attach(shaderSwapCreateCs(ctx, kFlatForegroundShadowEvalBytecode, sizeof(kFlatForegroundShadowEvalBytecode),
                                                        "flat foreground shadow evaluation", "flat foreground motion"));
        if (!momentsCs_ || !evalCs_) return false;
        const auto table = [&](unsigned elements, bool srv, Ptr<ID3D11Buffer>& buffer, Ptr<ID3D11ShaderResourceView>* view,
                               Ptr<ID3D11UnorderedAccessView>& uav) {
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = elements * 16; d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | (srv ? D3D11_BIND_SHADER_RESOURCE : 0u);
            d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; d.StructureByteStride = 16;
            if (FAILED(dev->CreateBuffer(&d, nullptr, &buffer))) return false;
            D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
            u.Format = DXGI_FORMAT_UNKNOWN; u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; u.Buffer.NumElements = elements;
            if (FAILED(dev->CreateUnorderedAccessView(buffer.Get(), &u, &uav))) return false;
            if (!srv || !view) return true;
            D3D11_SHADER_RESOURCE_VIEW_DESC s{};
            s.Format = DXGI_FORMAT_UNKNOWN; s.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; s.Buffer.NumElements = elements;
            return SUCCEEDED(dev->CreateShaderResourceView(buffer.Get(), &s, &*view));
        };
        if (!moments_ && !table(kDraws * 8, true, moments_, &momentsSrv_, momentsUav_)) { moments_.Reset(); return false; }
        if (!results_ && !table(kDraws * 3, false, results_, nullptr, resultsUav_)) { results_.Reset(); return false; }
        constants_.Reset();
        D3D11_BUFFER_DESC c{};
        c.ByteWidth = (constantBytes + 15) & ~15u; c.Usage = D3D11_USAGE_DEFAULT; c.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev->CreateBuffer(&c, nullptr, &constants_))) return false;
        constantBytes_ = constantBytes;
        return true;
    }
    ShadowStats stats_{};
    std::array<Slot, kSlots> slots_{};
    Ptr<ID3D11ComputeShader> momentsCs_, evalCs_;
    Ptr<ID3D11Buffer> moments_, results_, constants_;
    Ptr<ID3D11ShaderResourceView> momentsSrv_;
    Ptr<ID3D11UnorderedAccessView> momentsUav_, resultsUav_;
    unsigned constantBytes_ = 0;
    std::vector<ShadowRecord> lastRecords_;
};

}  // namespace edvr

#pragma once
// WHY A DRAW WITH PRIORS STILL HAS NO HISTORY (design doc section 104, the pistol's identity-differs window).
//
// A draw the history gave candidates is matched to one of them on the GPU, by the identity words of its pool record (the map's vertex
// shader, flat_foreground_motion_shader.h): identity.x equal, and identity.y equal but for byte 30. When none matches the draw's pixels
// are refused with reason 5 (identity-differs), and the census says only how many pixels. The 2026-10-07 pistol flight showed 355 064 such
// pixels in a 5 s window, on a single-candidate draw, and nothing says which word moved. This samples a draw in thirteen with candidates:
// the current identity and its candidates' go to a staging buffer, are read a few frames later without waiting, and are classified by the
// same two tests the shader makes. The verdict is what the map WILL have decided for that draw (it reads the same buffers); the census'
// identity-differs pixels are the cross-check.
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstring>
#include "animated_history_ledger.h"
#include "flat_foreground_identity_verdict.h"

namespace edvr {

class FlatIdentitySampler {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
public:
    static constexpr unsigned slots = 16, every = 13, maxPriors = 4, maxAgeFrames = 8;
    struct Sample {
        IdentityWords current, prior[maxPriors];
        unsigned priors = 0, frame = 0;
        IdentityVerdict verdict = IdentityVerdict::Match;
        HistoryKey key;
        uint64_t vs = 0, ps = 0;
    };

    // Offered every draw the history gave candidates. One in `every` is read back. Returns whether this one was taken.
    bool consider(ID3D11DeviceContext* ctx, unsigned frame, ID3D11ShaderResourceView* current,
                  ID3D11ShaderResourceView* const* priors, unsigned n, const HistoryKey& key, uint64_t vs, uint64_t ps) {
        if (!ctx || !current || !n || n > maxPriors) return false;
        if (++offered_ % every) return false;
        Slot* free = nullptr;
        for (Slot& s : slots_) if (!s.pending) { free = &s; break; }
        if (!free) { ++skipped_; return false; }
        if (!free->stage) {
            Ptr<ID3D11Device> dev; ctx->GetDevice(&dev);
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = (1 + maxPriors) * 16; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; d.StructureByteStride = 16;
            if (FAILED(dev->CreateBuffer(&d, nullptr, &free->stage))) { ++skipped_; return false; }
        }
        auto copy = [&](ID3D11ShaderResourceView* view, unsigned at) {
            Ptr<ID3D11Resource> resource; view->GetResource(&resource);
            if (!resource) return false;
            ctx->CopySubresourceRegion(free->stage.Get(), 0, at * 16, 0, 0, resource.Get(), 0, nullptr);
            return true;
        };
        if (!copy(current, 0)) { ++skipped_; return false; }
        for (unsigned i = 0; i < n; ++i) if (!priors[i] || !copy(priors[i], 1 + i)) { ++skipped_; return false; }
        free->pending = true; free->frame = frame; free->priors = n; free->key = key; free->vs = vs; free->ps = ps;
        return true;
    }

    // Reads the samples that are ready (all of them, waiting, when `wait`), calls `done(sample)` for each, and drops the ones that stayed
    // unready for maxAgeFrames. Called at most once a frame; reading is DO_NOT_WAIT, never a stall.
    template<class Done>
    void poll(ID3D11DeviceContext* ctx, unsigned frame, bool wait, Done&& done) {
        if (!ctx) return;
        for (Slot& s : slots_) {
            if (!s.pending) continue;
            if (!wait && frame - s.frame < 2) continue;
            D3D11_MAPPED_SUBRESOURCE m{};
            const HRESULT hr = ctx->Map(s.stage.Get(), 0, D3D11_MAP_READ, wait ? 0u : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            if (FAILED(hr) || !m.pData) {
                if (frame - s.frame > maxAgeFrames) { s.pending = false; ++notReady_; }
                continue;
            }
            Sample out;
            const uint32_t* w = static_cast<const uint32_t*>(m.pData);
            out.current = {w[0], w[1], w[2], w[3]};
            out.priors = s.priors;
            for (unsigned i = 0; i < s.priors; ++i) out.prior[i] = {w[4 + i * 4], w[5 + i * 4], w[6 + i * 4], w[7 + i * 4]};
            ctx->Unmap(s.stage.Get(), 0);
            out.frame = s.frame; out.key = s.key; out.vs = s.vs; out.ps = s.ps;
            out.verdict = classifyIdentity(out.current, out.prior, out.priors);
            s.pending = false;
            done(out);
        }
    }
    uint64_t skipped() const { return skipped_; }
    uint64_t notReady() const { return notReady_; }

private:
    struct Slot {
        Ptr<ID3D11Buffer> stage;
        bool pending = false;
        unsigned frame = 0, priors = 0;
        HistoryKey key;
        uint64_t vs = 0, ps = 0;
    };
    Slot slots_[slots];
    uint64_t offered_ = 0, skipped_ = 0, notReady_ = 0;
};

}  // namespace edvr

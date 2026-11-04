// The Galaxy Map's panels and mouse arrow at the stars' depth (docs/galaxy-map-vr-2026-11-03.md, Fix 3, "(a)").
//
// The map's four UI vertex shaders (C4B4B334B26E81A9, B10B032BDFD46700, 5DAD41AA66757CE3 for the panels and popups, 4EF6DDB075A927FA for the arrow)
// subtract cb1[275].xyz, the eye's origin in the UI frame, and use it nowhere else (each declares CB1[276] and has the one subtract). Their clip rows
// are camera-relative and keep the head's rotation. With row 275 = (0,0,0) each eye sees the UI from the UI frame's origin: no disparity and no
// parallax from head translation, so the arrow and the panels sit where the stars are.
//
// THE ROUTE (the same legal partial copy billboard_rollfree.h uses): a DEFAULT constant buffer of the bound buffer's ByteWidth, CopyResource from the
// bound one, then CopySubresourceRegion of 12 bytes from a zeroed patch buffer to byte 275 * 16, then VSSetConstantBuffers(1, ...) for the draw. The
// pixel stage keeps the game's buffer. The copy is made at each such draw (about six an eye while the map is open): no CPU read and no write tee, so
// there is no stale block to read.
#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>

#include "flat_compute_readback.h"   // FlatComputeInternalScope: EDVR's own calls do not reach the game's tees

namespace edvr {
namespace mapuidepth {

constexpr uint32_t kRow = 275;
constexpr uint32_t kRowOffset = kRow * 16;
constexpr uint32_t kMinWidth = (kRow + 1) * 16;   // the shaders declare CB1[276]
constexpr uint32_t kPatchBytes = 12;              // xyz
constexpr uint64_t kVsPanelA = 0xC4B4B334B26E81A9ull, kVsPanelB = 0xB10B032BDFD46700ull, kVsPanelC = 0x5DAD41AA66757CE3ull, kVsArrow = 0x4EF6DDB075A927FAull;

inline int shaderIndex(uint64_t vs) {
    return vs == kVsPanelA ? 0 : vs == kVsPanelB ? 1 : vs == kVsPanelC ? 2 : vs == kVsArrow ? 3 : -1;
}

enum Decline : uint32_t { kNoBuffer, kNarrow, kRange, kCreateFailed, kDeclineCount };
inline const char* declineName(uint32_t d) {
    static const char* const names[kDeclineCount] = {"no buffer at VS slot 1", "narrower than 276 rows", "bound at a constant offset or as a range", "buffer creation failed"};
    return d < kDeclineCount ? names[d] : "?";
}

// Counters; atomics because the 30 s line is read on the frame thread while the draws run on the render thread.
struct Counters {
    std::atomic<uint64_t> bound[4];
    std::atomic<uint64_t> declined[kDeclineCount];
};

class MapUiDepth {
public:
    Counters counters;

    // The draw is one of the four shaders: bind a copy with row 275 zeroed at VS slot 1 and remember the game's buffer. True when bound; restore() then
    // puts the game's buffer back. False: the draw goes as the game made it (counted when a shader matched and the route declined).
    bool begin(ID3D11DeviceContext* ctx, uint64_t vs) {
        const int which = shaderIndex(vs);
        if (which < 0 || !ctx || bound_) return false;
        FlatComputeInternalScope internal;
        Microsoft::WRL::ComPtr<ID3D11Buffer> game;
        ctx->VSGetConstantBuffers(1, 1, &game);
        if (!game) return decline(kNoBuffer);
        D3D11_BUFFER_DESC gd{};
        game->GetDesc(&gd);
        if (gd.ByteWidth < kMinWidth || !(gd.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return decline(kNarrow);
        Microsoft::WRL::ComPtr<ID3D11DeviceContext1> ctx1;
        if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) {
            Microsoft::WRL::ComPtr<ID3D11Buffer> again;
            UINT first = 0, count = 0;
            ctx1->VSGetConstantBuffers1(1, 1, &again, &first, &count);
            if (first != 0 || (count != 0 && count * 16u < gd.ByteWidth)) return decline(kRange);
        }
        Microsoft::WRL::ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        if (!dev) return decline(kCreateFailed);
        if (!copy_ || width_ != gd.ByteWidth) {
            copy_.Reset();
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = gd.ByteWidth;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(dev->CreateBuffer(&d, nullptr, &copy_))) return decline(kCreateFailed);
            width_ = gd.ByteWidth;
        }
        if (!patch_) {
            const float zero[4] = {};
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = sizeof(zero);
            d.Usage = D3D11_USAGE_DEFAULT;
            D3D11_SUBRESOURCE_DATA init{};
            init.pSysMem = zero;
            if (FAILED(dev->CreateBuffer(&d, &init, &patch_))) return decline(kCreateFailed);
        }
        ctx->CopyResource(copy_.Get(), game.Get());
        const D3D11_BOX box{0, 0, 0, kPatchBytes, 1, 1};
        ctx->CopySubresourceRegion(copy_.Get(), 0, kRowOffset, 0, 0, patch_.Get(), 0, &box);
        ID3D11Buffer* ours = copy_.Get();
        ctx->VSSetConstantBuffers(1, 1, &ours);
        saved_ = game;
        bound_ = true;
        counters.bound[which].fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void restore(ID3D11DeviceContext* ctx) {
        if (!bound_) return;
        bound_ = false;
        if (ctx) {
            FlatComputeInternalScope internal;
            ID3D11Buffer* was = saved_.Get();
            ctx->VSSetConstantBuffers(1, 1, &was);
        }
        saved_.Reset();
    }

    bool bound() const { return bound_; }

    void shutdown() {
        copy_.Reset();
        patch_.Reset();
        saved_.Reset();
        bound_ = false;
        width_ = 0;
    }

private:
    Microsoft::WRL::ComPtr<ID3D11Buffer> copy_, patch_, saved_;
    uint32_t width_ = 0;
    bool bound_ = false;

    bool decline(Decline why) {
        counters.declined[why].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
};

inline MapUiDepth g_mapUiDepth;

} // namespace mapuidepth
} // namespace edvr

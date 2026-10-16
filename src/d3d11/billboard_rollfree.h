// A roll-free copy of the game's scene constant block, for the camera-facing billboards that are not
// transcribed (docs/billboard-root-cause-2026-10-16.md, "E1, narrow").
//
// THE BUG. Billboard vertex shaders span their quads on the camera basis in the 5376-byte scene constant block
// (VS slot 1): cb1[277] right, cb1[278] up, cb1[279] forward. The cross-product forms build right and up from
// cross([278],[279]); the direct forms add [277]*x + [278]*y to the particle. A camera basis in a headset is a
// HEAD basis, so every such quad rolls with the head.
//
// WHY NOT WRITE THE GAME'S BLOCK. About 190 G-buffer pixel shaders encode normals with [277..279], and some
// vertex shaders move sun and light vectors into view space with them, so rows written into the game's block
// would change lighting. The fix is therefore VERTEX-SLOT-1-ONLY, per draw, for the billboard vertex shaders
// only: the pixel stage keeps the game's block.
//
// THE ROUTE (checked on WARP with the debug layer, tools/billboard_rollfree_test):
//   1. CopyResource(ours, game's block)             ours: DEFAULT, 5376 bytes, BIND_CONSTANT_BUFFER
//   2. UpdateSubresource(patch, rows 277 and 278)   patch: DEFAULT, 32 bytes, no bind flags
//   3. CopySubresourceRegion(ours @ 277*16, patch)  a partial copy INTO a constant buffer: legal, where a partial
//                                                   UpdateSubresource of a constant buffer is not before D3D11.1
// then VS slot 1 is pointed at ours for the draw and put back. No CPU read of mapped memory beyond the 160 bytes
// the particle fix already keeps (particle_capture_core.h): the new rows come from the camera's forward vector.
// The copy is LAZY: made at the first billboard draw after a write of that block, once however many draws follow.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "flat_compute_readback.h"   // FlatComputeInternalScope: EDVR's own calls do not reach the game's tees
#include "particle_capture_core.h"

namespace edvr {
namespace rollfree {

constexpr uint32_t kBlockBytes = 5376;
constexpr uint32_t kPatchRow = 277;
constexpr uint32_t kPatchOffset = kPatchRow * 16;   // rows 277 and 278
constexpr uint32_t kPatchBytes = 32;
constexpr uint32_t kMaxBlocks = 4;

// The roll-free right and up for a camera whose rows are r277 (right), r278 (up) and r279 (forward), each a
// float4 straight from the block. "Roll-free": built from the forward vector and WORLD UP, the convention the
// steady plume shader uses (right = normalize(cross(worldUp, face))). out[0..3] is the new [277], out[4..7] the
// new [278]. The w components, the rows' lengths and the basis' handedness are the game's own: a quad's size and
// mirroring do not change, only the roll. False when forward is not unit length or points within about a degree
// of world up, where "up" has no defined roll; the caller then draws with the game's block.
inline bool basis(const float r277[4], const float r278[4], const float r279[4], float out[8]) {
    const float fl = std::sqrt(r279[0] * r279[0] + r279[1] * r279[1] + r279[2] * r279[2]);
    if (!(fl > 0.9f && fl < 1.1f)) return false;
    const float f[3] = {r279[0] / fl, r279[1] / fl, r279[2] / fl};
    // cross(worldUp = (0,1,0), f) = (f.z, 0, -f.x)
    const float rl = std::sqrt(f[0] * f[0] + f[2] * f[2]);
    if (!(rl > 0.02f)) return false;
    const float r[3] = {f[2] / rl, 0.0f, -f[0] / rl};
    // cross(f, r)
    const float u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    // The game's handedness: is [277] along cross([278],[279]) or against it?
    const float c[3] = {r278[1] * r279[2] - r278[2] * r279[1], r278[2] * r279[0] - r278[0] * r279[2],
                        r278[0] * r279[1] - r278[1] * r279[0]};
    const float side = r277[0] * c[0] + r277[1] * c[1] + r277[2] * c[2];
    const float s = side < 0.0f ? -1.0f : 1.0f;
    const float lr = std::sqrt(r277[0] * r277[0] + r277[1] * r277[1] + r277[2] * r277[2]);
    const float lu = std::sqrt(r278[0] * r278[0] + r278[1] * r278[1] + r278[2] * r278[2]);
    if (!(lr > 1e-6f) || !(lu > 1e-6f)) return false;
    for (int i = 0; i < 3; ++i) {
        out[i] = s * r[i] * lr;
        out[4 + i] = u[i] * lu;
    }
    out[3] = r277[3];
    out[7] = r278[3];
    return true;
}

// What a block's counters and buffers are; nothing here logs.
struct Block {
    const void*                      resource = nullptr;    // identity of the game's block; no reference held
    particlecap::Shadow              shadow;                // rows 270..279 of its last write
    uint32_t                         width = 0;             // its ByteWidth
    uint32_t                         writeSeq = 0;          // bumped by every captured write
    uint32_t                         copiedSeq = 0xFFFFFFFFu;
    Microsoft::WRL::ComPtr<ID3D11Buffer> copy;              // ours, DEFAULT, BIND_CONSTANT_BUFFER
    bool                             basisOk = false;       // the last copy had a defined basis
};

struct Counters {
    uint64_t copies = 0;          // block copies made (a copy = the three calls above)
    uint64_t binds = 0;           // draws that got a roll-free block
    uint64_t noBasis = 0;         // draws that kept the game's block: forward within a degree of world up
    uint64_t failures = 0;        // a D3D call or a creation that failed
};

class Blocks {
public:
    Counters counters;
    bool     enabled = true;

    // ---- identity -----------------------------------------------------------------------------------------------
    int find(const void* resource) const {
        if (!resource) return -1;
        for (uint32_t i = 0; i < kMaxBlocks; ++i)
            if (blocks_[i].resource == resource) return static_cast<int>(i);
        return -1;
    }

    // Starts watching a block seen bound at VS slot 1. Refuses anything that is not the 5376-byte constant buffer.
    // Replaces the oldest entry when the table is full. Returns the slot, -1 refused.
    int learn(ID3D11Buffer* buffer) {
        if (!buffer || !enabled) return -1;
        D3D11_BUFFER_DESC d{};
        buffer->GetDesc(&d);
        // The scene block is 5376 bytes; any constant buffer that reaches row 279 (4480) and fits the shadow is the same
        // block for this purpose, as the plume fix's own shadow has always taken it.
        if (d.ByteWidth < particlecap::kRangeEnd || d.ByteWidth > particlecap::kMaxShadow ||
            !(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER))
            return -1;
        int slot = -1;
        for (uint32_t i = 0; i < kMaxBlocks; ++i)
            if (!blocks_[i].resource) { slot = static_cast<int>(i); break; }
        if (slot < 0) { slot = static_cast<int>(next_++ % kMaxBlocks); }
        Block& b = blocks_[slot];
        if (b.width != d.ByteWidth) b.copy.Reset();   // the copy is made at the block's own width
        b.width = d.ByteWidth;
        b.resource = buffer;
        b.shadow.valid = false;
        b.writeSeq = 0;
        b.copiedSeq = 0xFFFFFFFFu;
        b.basisOk = false;
        publish();
        return slot;
    }

    // The Unmap tee: a write of a watched block. Copies the 160 bytes the fix reads. Returns the bytes read.
    template <class Copy>
    uint32_t capture(const void* resource, bool steady, const void* data, uint32_t bytes, Copy&& copy) {
        const int i = find(resource);
        if (i < 0) return 0;
        Block& b = blocks_[i];
        const uint32_t n = particlecap::capture(b.shadow, steady, data, bytes, copy);
        ++b.writeSeq;
        return n;
    }

    Block* at(int slot) { return slot >= 0 && slot < static_cast<int>(kMaxBlocks) ? &blocks_[slot] : nullptr; }
    const Block* at(int slot) const { return slot >= 0 && slot < static_cast<int>(kMaxBlocks) ? &blocks_[slot] : nullptr; }

    // Retirement or stock: forget every identity (the GPU copies stay allocated: 5376 bytes each).
    void clear() {
        for (auto& b : blocks_) {
            b.resource = nullptr;
            b.shadow.valid = false;
            b.copiedSeq = 0xFFFFFFFFu;
            b.basisOk = false;
        }
        publish();
    }

    // The watched identities, published for the Map tee's inline compare (particle_fix.h).
    const void* const* watched() const { return watched_; }

    // ---- the draw -----------------------------------------------------------------------------------------------
    // Would prepare() make the copy now?
    bool needsCopy(int slot) const {
        const Block* b = at(slot);
        return b && b->shadow.valid && b->copiedSeq != b->writeSeq;
    }

    // The roll-free block for a draw whose VS slot 1 holds the watched block `slot`: the lazy copy first, when the
    // block has been written since. Null when nothing is to be rebound (the draw goes as the game made it).
    ID3D11Buffer* prepare(ID3D11DeviceContext* ctx, int slot, ID3D11Buffer* gameBlock) {
        Block* b = at(slot);
        if (!enabled || !ctx || !gameBlock || !b || !b->shadow.valid || b->resource != gameBlock) return nullptr;
        if (b->copiedSeq != b->writeSeq) {
            if (!copyBlock(ctx, *b, gameBlock)) { ++counters.failures; return nullptr; }
        }
        if (!b->basisOk) { ++counters.noBasis; return nullptr; }
        ++counters.binds;
        return b->copy.Get();
    }

    // Binds `ours` at VS slot 1, remembering what was there. restore() puts it back. Pixel stage never touched.
    bool bind(ID3D11DeviceContext* ctx, ID3D11Buffer* ours) {
        if (!ctx || !ours || bound_) return false;
        FlatComputeInternalScope internal;
        ctx->VSGetConstantBuffers(1, 1, saved_.GetAddressOf());
        ctx->VSSetConstantBuffers(1, 1, &ours);
        bound_ = true;
        return true;
    }
    bool bound() const { return bound_; }
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

    void shutdown() {
        clear();
        for (auto& b : blocks_) b.copy.Reset();
        patch_.Reset();
        saved_.Reset();
        bound_ = false;
    }

private:
    Block blocks_[kMaxBlocks];
    const void* watched_[kMaxBlocks] = {};
    uint32_t next_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> patch_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> saved_;
    bool bound_ = false;

    void publish() {
        for (uint32_t i = 0; i < kMaxBlocks; ++i) watched_[i] = blocks_[i].resource;
    }

    bool copyBlock(ID3D11DeviceContext* ctx, Block& b, ID3D11Buffer* gameBlock) {
        FlatComputeInternalScope internal;
        Microsoft::WRL::ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        if (!dev) return false;
        if (!b.copy) {
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = b.width;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(dev->CreateBuffer(&d, nullptr, &b.copy))) return false;
        }
        if (!patch_) {
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = kPatchBytes;
            d.Usage = D3D11_USAGE_DEFAULT;
            if (FAILED(dev->CreateBuffer(&d, nullptr, &patch_))) return false;
        }
        const float* rows = reinterpret_cast<const float*>(b.shadow.bytes);
        float out[8];
        b.basisOk = basis(rows + 277 * 4, rows + 278 * 4, rows + 279 * 4, out);
        b.copiedSeq = b.writeSeq;
        if (!b.basisOk) return true;   // nothing to rebind; the copy is not needed this write
        ++counters.copies;
        ctx->CopyResource(b.copy.Get(), gameBlock);
        ctx->UpdateSubresource(patch_.Get(), 0, nullptr, out, 0, 0);
        D3D11_BOX box{0, 0, 0, kPatchBytes, 1, 1};
        ctx->CopySubresourceRegion(b.copy.Get(), 0, kPatchOffset, 0, 0, patch_.Get(), 0, &box);
        return true;
    }
};

}  // namespace rollfree
}  // namespace edvr

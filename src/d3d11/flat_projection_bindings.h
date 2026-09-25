#pragma once

// CPU-only storage and patch preparation. The caller owns resource lifetime,
// shader admission, D3D binding/restoration, and the decision to use jitter.
#include "flat_projection_math.h"
#include "flat_lighting_contract.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

namespace edvr {

struct FlatProjectionShadowView {
    const void* identity = nullptr;
    uint64_t identityGeneration = 0;
    uint64_t writeGeneration = 0;
    uint64_t bankEpoch = 0;
    const unsigned char* bytes = nullptr;
    uint32_t width = 0;
};

struct FlatProjectionCacheKey {
    const void* identity = nullptr;
    uint64_t identityGeneration = 0;
    uint64_t writeGeneration = 0;
    uint64_t bankEpoch = 0;
    uint32_t phase = 0;
};

inline FlatProjectionCacheKey flatProjectionCacheKey(const FlatProjectionShadowView& view,
                                                      uint32_t phase) {
    return {view.identity, view.identityGeneration, view.writeGeneration,
            view.bankEpoch, phase};
}

// At most MaxBuffers * MaxWidth bytes, allocated once at construction. Caller
// must register only stable identities and release them before resource reuse.
// In particular, a pointer reused for a new COM object needs a new nonzero
// identityGeneration. One owner thread must serialize all calls.
template<size_t MaxBuffers = 64, size_t MaxWidth = 65536>
class FlatProjectionShadowBank {
    static_assert(MaxBuffers > 0 && MaxBuffers <= 128, "bounded identity count");
    static_assert(MaxWidth >= 16 && MaxWidth <= 65536 && MaxWidth % 16 == 0,
                  "D3D11 constant-buffer width bound");
    static_assert(MaxBuffers * MaxWidth <= 8u * 1024u * 1024u, "bounded total storage");
    struct Entry {
        const void* identity = nullptr;
        uint64_t identityGeneration = 0;
        uint64_t writeGeneration = 0;
        uint32_t width = 0;
        bool valid = false;
        bool mapping = false;
        unsigned char bytes[MaxWidth] = {};
    };
    std::unique_ptr<Entry[]> entries_;
    uint64_t epoch_ = 1;

    Entry* find(const void* identity, uint64_t identityGeneration) const {
        if (!entries_ || !identity || !identityGeneration) return nullptr;
        for (size_t i = 0; i < MaxBuffers; ++i)
            if (entries_[i].identity == identity &&
                entries_[i].identityGeneration == identityGeneration)
                return &entries_[i];
        return nullptr;
    }
    bool advance(Entry& entry) {
        entry.valid = false;
        if (entry.writeGeneration == UINT64_MAX) return false;
        ++entry.writeGeneration;
        return true;
    }
public:
    FlatProjectionShadowBank() : entries_(new (std::nothrow) Entry[MaxBuffers]) {}
    FlatProjectionShadowBank(const FlatProjectionShadowBank&) = delete;
    FlatProjectionShadowBank& operator=(const FlatProjectionShadowBank&) = delete;
    bool ready() const { return entries_ != nullptr; }
    static constexpr size_t capacity() { return MaxBuffers; }
    static constexpr size_t storageBytes() { return MaxBuffers * MaxWidth; }

    bool registerBuffer(const void* identity, uint64_t identityGeneration,
                        uint32_t width) {
        if (!ready() || !identity || !identityGeneration || width < 16 ||
            width > MaxWidth || width % 16) return false;
        if (Entry* old = find(identity, identityGeneration)) {
            if (old->width == width) return true;
            advance(*old); old->mapping = false;
            return false;
        }
        // A still-registered pointer cannot silently acquire a new lifetime.
        for (size_t i = 0; i < MaxBuffers; ++i)
            if (entries_[i].identity == identity) return false;
        for (size_t i = 0; i < MaxBuffers; ++i) if (!entries_[i].identity) {
            Entry& e = entries_[i]; e.identity = identity;
            e.identityGeneration = identityGeneration; e.width = width;
            return true;
        }
        return false;
    }
    bool releaseBuffer(const void* identity, uint64_t identityGeneration) {
        Entry* e = find(identity, identityGeneration);
        if (!e) return false;
        e->identity = nullptr; e->identityGeneration = 0; e->width = 0;
        e->valid = e->mapping = false;
        // Do not recycle a write generation into a key from the old lifetime.
        e->writeGeneration = 0;
        return true;
    }
    void reset() {
        if (!entries_) return;
        for (size_t i = 0; i < MaxBuffers; ++i) {
            Entry& e = entries_[i]; e.identity = nullptr;
            e.identityGeneration = e.writeGeneration = 0; e.width = 0;
            e.valid = e.mapping = false;
        }
        if (epoch_ != UINT64_MAX) ++epoch_;
    }

    // Map start invalidates the previous contents immediately. A failed/short
    // Unmap capture leaves the entry invalid; there is no inferred old data.
    bool beginMap(const void* identity, uint64_t identityGeneration) {
        Entry* e = find(identity, identityGeneration);
        if (!e || e->mapping || !advance(*e)) return false;
        e->mapping = true;
        return true;
    }
    bool finishMapFull(const void* identity, uint64_t identityGeneration,
                       const void* bytes, uint32_t width) {
        Entry* e = find(identity, identityGeneration);
        if (!e || !e->mapping) return false;
        e->mapping = false;
        if (!bytes || width != e->width || e->writeGeneration == UINT64_MAX) return false;
        std::memcpy(e->bytes, bytes, width);
        ++e->writeGeneration; e->valid = true;
        return true;
    }
    bool captureFullWrite(const void* identity, uint64_t identityGeneration,
                          const void* bytes, uint32_t width) {
        Entry* e = find(identity, identityGeneration);
        if (!e || !advance(*e)) return false;
        if (e->mapping) { e->mapping = false; return false; }
        e->mapping = false;
        if (!bytes || width != e->width) return false;
        std::memcpy(e->bytes, bytes, width); e->valid = true;
        return true;
    }
    // Call for partial/unknown/copy/GPU writes, or any untracked mutation.
    bool invalidate(const void* identity, uint64_t identityGeneration) {
        Entry* e = find(identity, identityGeneration);
        if (!e || !advance(*e)) return false;
        e->mapping = false;
        return true;
    }
    bool lookup(const void* identity, uint64_t identityGeneration,
                FlatProjectionShadowView& out) const {
        const Entry* e = find(identity, identityGeneration);
        if (!e || !e->valid || e->mapping) return false;
        out = {identity, identityGeneration, e->writeGeneration,
               epoch_, e->bytes, e->width};
        return true;
    }
    bool matches(const FlatProjectionCacheKey& key, uint32_t phase) const {
        FlatProjectionShadowView view{};
        return key.phase == phase && lookup(key.identity, key.identityGeneration, view) &&
               view.writeGeneration == key.writeGeneration && view.bankEpoch == key.bankEpoch;
    }
};

enum class FlatProjectionPatchLayout : uint8_t {
    ForwardColumns, ForwardDp4, InverseUvRay, InverseClip,
    InverseScreenRay, LightingUvRay
};

struct FlatProjectionLightingContract {
    float pixelX = 0, pixelY = 0;
    uint32_t width = 0, height = 0, tileWidth = 0, gridX = 0,
             gridY = 0, sampleCount = 0;
};

struct FlatProjectionPatchRequest {
    FlatProjectionPatchLayout layout = FlatProjectionPatchLayout::ForwardColumns;
    uint32_t byteOffset = 0;
    FlatProjectionLightingContract lighting{}; // used only for LightingUvRay
};

// Pure preparation: each request names an already-qualified layout/span.
// Validate all transforms in small stack temporaries before touching dest.
// Source and destination must not overlap; raw source snapshots stay intact.
inline bool flatPrepareProjectionPatch(const void* source, uint32_t width,
                                       const FlatProjectionPatchRequest* requests,
                                       uint32_t count, const FlatProjectionJitter& jitter,
                                       void* dest, uint32_t capacity) {
    if (!source || !dest || source == dest || !requests || !count || count > 8 ||
        width < 16 || width > 65536 || width % 16 || capacity < width ||
        !flat_projection_detail::finite(jitter)) return false;
    struct Prepared { uint32_t offset = 0, length = 0; float rows[4][4] = {}; };
    Prepared prepared[8]{};
    const auto* src = static_cast<const unsigned char*>(source);
    const auto srcAddress = reinterpret_cast<uintptr_t>(source);
    const auto dstAddress = reinterpret_cast<uintptr_t>(dest);
    if (srcAddress <= dstAddress ? dstAddress - srcAddress < width :
                                   srcAddress - dstAddress < width) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& r = requests[i];
        uint32_t length = r.layout == FlatProjectionPatchLayout::InverseUvRay ||
                          r.layout == FlatProjectionPatchLayout::LightingUvRay ? 48u : 64u;
        if (r.byteOffset % 16 || r.byteOffset > width || length > width - r.byteOffset)
            return false;
        for (uint32_t j = 0; j < i; ++j)
            if (r.byteOffset < prepared[j].offset + prepared[j].length &&
                prepared[j].offset < r.byteOffset + length) return false;
        Prepared& p = prepared[i]; p.offset = r.byteOffset; p.length = length;
        std::memcpy(p.rows, src + p.offset, length);
        bool ok = false;
        switch (r.layout) {
        case FlatProjectionPatchLayout::ForwardColumns:
            ok = flatJitterForwardColumns(p.rows, jitter); break;
        case FlatProjectionPatchLayout::ForwardDp4:
            ok = flatJitterForwardDp4(p.rows, jitter); break;
        case FlatProjectionPatchLayout::InverseUvRay:
        { float rays[3][4]; std::memcpy(rays, p.rows, sizeof(rays));
          ok = flatJitterInverseUvRay(rays, jitter);
          if (ok) std::memcpy(p.rows, rays, sizeof(rays)); break; }
        case FlatProjectionPatchLayout::InverseClip:
            ok = flatJitterInverseClip(p.rows, jitter); break;
        case FlatProjectionPatchLayout::InverseScreenRay:
            ok = flatJitterInverseScreenRay(p.rows, jitter); break;
        case FlatProjectionPatchLayout::LightingUvRay: {
            const auto& c = r.lighting;
            FlatProjectionJitter phase{};
            float rays[3][4]; std::memcpy(rays, p.rows, sizeof(rays));
            ok = flatLightingLookupInvariant(c.pixelX, c.pixelY, c.width, c.height,
                                             c.tileWidth, c.gridX, c.gridY,
                                             c.sampleCount) &&
                 flatProjectionJitter(c.pixelX, c.pixelY, c.width, c.height, phase) &&
                 phase.ndcX == jitter.ndcX && phase.ndcY == jitter.ndcY &&
                 phase.uvX == jitter.uvX && phase.uvY == jitter.uvY &&
                 flatJitterInverseUvRay(rays, jitter);
            if (ok) std::memcpy(p.rows, rays, sizeof(rays));
            break;
        }
        default: return false;
        }
        if (!ok) return false;
    }
    std::memcpy(dest, source, width);
    for (uint32_t i = 0; i < count; ++i)
        std::memcpy(static_cast<unsigned char*>(dest) + prepared[i].offset,
                    prepared[i].rows, prepared[i].length);
    return true;
}

} // namespace edvr

#pragma once

#include <cstdint>
#include <type_traits>

namespace edvr {
namespace holo_scrim_observation {

// These values map to JSON strings "unknown", "no", and "yes".
enum class Tri : std::uint8_t { Unknown = 0, No = 1, Yes = 2 };

struct Gates final {
    Tri enabled;
    Tri shapeReached;
    Tri shapeMatched;
    Tri helperReached;
    Tri helperEnabled;
    Tri helperShapeReached;
    Tri helperShapeMatched;
};

// JSON source values: 0=not reached, 1=fresh resolve succeeded,
// 2=fresh resolve failed, 3=trace-shadow raw hit, 4=warm cache hit without
// raw shadow. No identity or cached match is part of this serialized input.
enum class ResourceSource : std::uint8_t {
    NotReached = 0,
    FreshResolveSuccess = 1,
    FreshResolveFailure = 2,
    RawShadowHit = 3,
    WarmCacheWithoutRawShadow = 4,
};

// ResourceInfo-shaped, pointer-free raw source facts. For a failed resolve,
// `texture2D=No` records the helper's default-initialized local; a/b/fmt are
// unavailable and serialize as zero. a/b are dimensions only for Texture2D.
struct ResourceObservation final {
    ResourceSource source;
    Tri resolveReached;
    Tri resolved;
    Tri rawAvailable;
    Tri texture2D;
    std::uint32_t a;
    std::uint32_t b;
    std::uint32_t fmt;
};

enum EyeSizeRead : std::uint8_t {
    kDepthWidthRead = 1u << 0,
    kDepthHeightRead = 1u << 1,
    kEyeWidthRead = 1u << 2,
    kEyeHeightRead = 1u << 3,
    kRenderWidthRead = 1u << 4,
    kRenderHeightRead = 1u << 5,
};

// `statePresent` and consumed scalar dimensions are selector inputs. `result`
// is a production observation for consistency checking only. The read mask
// preserves short-circuit availability; Python evaluates <=2 only for read
// comparison axes and must not add a nonzero-height guard.
struct EyeSizeObservation final {
    Tri reached;
    Tri statePresent;
    Tri result;
    std::uint8_t readMask;
    std::uint32_t depthW;
    std::uint32_t depthH;
    std::uint32_t eyeW;
    std::uint32_t eyeH;
    std::uint32_t renderW;
    std::uint32_t renderH;
};

// Getter supplies statePresent(), eyeW(), eyeH(), renderW(), renderH().
// Each dimension getter is called lazily in the frozen selector order.
// The arithmetic is the literal near2 rule used by vScreenIsEyeSized.
template <class Getter>
inline bool isEyeSizedObserved(std::uint32_t w, std::uint32_t h,
                               Getter& getter,
                               EyeSizeObservation* observation) {
    EyeSizeObservation local{};
    EyeSizeObservation& out = observation ? *observation : local;
    out = EyeSizeObservation{};
    out.reached = Tri::Yes;
    out.statePresent = getter.statePresent() ? Tri::Yes : Tri::No;
    if (out.statePresent != Tri::Yes) {
        out.result = Tri::No;
        return false;
    }

    out.readMask |= kDepthWidthRead;
    out.depthW = w;
    if (!w) {
        out.result = Tri::No;
        return false;
    }
    out.readMask |= kDepthHeightRead;
    out.depthH = h;
    if (!h) {
        out.result = Tri::No;
        return false;
    }

    out.readMask |= kEyeWidthRead;
    out.eyeW = getter.eyeW();
    if (out.eyeW && (w > out.eyeW ? w - out.eyeW : out.eyeW - w) <= 2u) {
        out.readMask |= kEyeHeightRead;
        out.eyeH = getter.eyeH();
        if ((h > out.eyeH ? h - out.eyeH : out.eyeH - h) <= 2u) {
            out.result = Tri::Yes;
            return true;
        }
    }

    out.readMask |= kRenderWidthRead;
    out.renderW = getter.renderW();
    if (out.renderW &&
        (w > out.renderW ? w - out.renderW : out.renderW - w) <= 2u) {
        out.readMask |= kRenderHeightRead;
        out.renderH = getter.renderH();
        if ((h > out.renderH ? h - out.renderH : out.renderH - h) <= 2u) {
            out.result = Tri::Yes;
            return true;
        }
    }
    out.result = Tri::No;
    return false;
}

struct HoloObservation final {
    Gates gates;
    ResourceObservation pattern;
    ResourceObservation depth;
    EyeSizeObservation eyeSize;
    Tri predicateResult;
    std::uint64_t missedBefore;
    std::uint64_t missedAfter;
    Tri missNotedBefore;
    Tri missNotedAfter;
    bool missedDeltaKnown;
    std::uint64_t missedDelta;
};

struct ScrimObservation final {
    Gates gates;
    ResourceObservation wash;
    ResourceObservation ui;
    Tri predicateResult;
};

static_assert(std::is_standard_layout<Gates>::value &&
                  std::is_trivially_copyable<Gates>::value &&
                  std::is_standard_layout<ResourceObservation>::value &&
                  std::is_trivially_copyable<ResourceObservation>::value &&
                  std::is_standard_layout<EyeSizeObservation>::value &&
                  std::is_trivially_copyable<EyeSizeObservation>::value &&
                  std::is_standard_layout<HoloObservation>::value &&
                  std::is_trivially_copyable<HoloObservation>::value &&
                  std::is_standard_layout<ScrimObservation>::value &&
                  std::is_trivially_copyable<ScrimObservation>::value,
              "observations must remain pointer-free PODs");

}  // namespace holo_scrim_observation

// Trace-only bridge implemented where vScreen's State is defined. The regular
// vScreenIsEyeSized entry point remains unchanged for NoTrace callers.
bool vScreenIsEyeSizedObserved(
    std::uint32_t w, std::uint32_t h,
    holo_scrim_observation::EyeSizeObservation* observation);

}  // namespace edvr

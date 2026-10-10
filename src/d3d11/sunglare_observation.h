#pragma once

#include <cstdint>

namespace edvr {

// Trace-only, pointer-free facts for replaying one Sunglare draw through
// sites 61-63. Draw kind/count/instances remain in the existing envelope.
// A skipped read has reached=false, known=false; a reached read whose value
// could not be obtained has reached=true, known=false.
template <class T>
struct SunglareRead {
    bool reached = false;
    bool known = false;
    T value{};
};

enum class SunglareTraceMode : uint8_t {
    kStock = 0,
    kOff = 1,
    kRealistic = 2,
    kVivid = 3,
};

enum class SunglareTraceAction : uint8_t {
    kStock,
    kSkip,
    kClamp,
    kMatch,
};

enum class SunglareTraceFactKind : uint8_t {
    kKind9 = 9,
    kKind10 = 10,
    kKind11 = 11,
};

struct SunglareTextureRead {
    SunglareRead<bool> resolveOk;
    SunglareRead<bool> isTexture2D;
    SunglareRead<uint32_t> width;
    SunglareRead<uint32_t> height;
    SunglareRead<uint32_t> format;
};

struct SunglareSelectorObservation {
    SunglareRead<SunglareTraceMode> outerWantsMode;
    SunglareRead<bool> outerExposureDamping;
    SunglareRead<bool> outerProbe;
    SunglareRead<bool> outerTrainShape;
    SunglareRead<bool> outerWantsResult;

    SunglareRead<SunglareTraceMode> helperWantsMode;
    SunglareRead<bool> helperExposureDamping;
    SunglareRead<bool> helperProbe;
    SunglareRead<bool> helperWantsResult;
    SunglareRead<bool> helperTrainShape;
    SunglareTextureRead ps0;
    SunglareTextureRead ps1;

    SunglareRead<uint64_t> lastSeenBeforeMs;
    SunglareRead<uint64_t> nowMs;
    SunglareRead<uint64_t> lastSeenAfterMs;
    SunglareRead<SunglareTraceMode> actionMode;
    SunglareRead<SunglareTraceAction> action;
};

struct SunglareSiteObservation {
    SunglareRead<bool> source61ActionNotStock;
    SunglareRead<int32_t> worldValue;
    SunglareRead<bool> probeValue;
    SunglareRead<uint32_t> clampBefore;
    SunglareRead<uint32_t> clampAfter;
    SunglareRead<bool> billboardReached;
};

struct SunglareObservation {
    SunglareTraceFactKind kind = SunglareTraceFactKind::kKind9;
    SunglareSelectorObservation selector;

    SunglareRead<uint32_t> common2ClampBefore;
    SunglareRead<uint32_t> common2ClampAfter;
    SunglareSiteObservation site;
};

}  // namespace edvr

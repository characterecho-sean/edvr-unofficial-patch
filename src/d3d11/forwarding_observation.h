#pragma once

#include "basic_draw_observation.h"

#include <cstdint>
#include <type_traits>

namespace edvr {

// Pointer-free raw checkpoints consumed by forwardWithVerdict. The verdict
// and callback result are inputs; owner identity and the draw tuple remain in
// the existing site-2 and DrawFacts observations.
struct ForwardingObservation final {
    std::uint8_t kind = 1;
    std::uint8_t version = 1;

    BasicDrawRead<std::int16_t> verdictOrdinal;
    BasicDrawRead<bool> issueBlockedEntry;
    BasicDrawRead<bool> objectProbeLedgerOn;
    BasicDrawRead<bool> uiDepthThisDraw;
    BasicDrawRead<bool> holoDepthThisDraw;
    BasicDrawRead<bool> compositeThisDraw;
    BasicDrawRead<bool> curveThisDrawBeforeSkipClear;

    // Reached after the kSkip early return, in source-consumption order.
    BasicDrawRead<bool> seedDiagnostics;
    BasicDrawRead<bool> uiLayerLiveEyeGate;
    BasicDrawRead<bool> uiLayerLiveFallbackGate;
    BasicDrawRead<bool> uiLayerWatchingGate;
    BasicDrawRead<bool> curveThisDrawCurveGate;
    BasicDrawRead<std::int32_t> engineVelocityCacheFamily;
    BasicDrawRead<bool> introCurveThisDrawStripGate;
    BasicDrawRead<bool> issueBlockedBeforeOriginal;
    // The actual injected/real draw thunk's return value, not a selector bit.
    BasicDrawRead<bool> originalCallReturned;
    BasicDrawRead<bool> crispPendingAfterOriginal;
    BasicDrawRead<bool> issueBlockedAfterOriginal;
    BasicDrawRead<bool> planetPending;
    BasicDrawRead<bool> planetSolarPending;
};

static_assert(std::is_standard_layout<ForwardingObservation>::value &&
                  std::is_trivially_copyable<ForwardingObservation>::value,
              "forwarding observations must remain pointer-free PODs");

}  // namespace edvr

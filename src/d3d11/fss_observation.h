#pragma once

#include <cstdint>

namespace edvr {

// Trace-only, pointer-free source facts for FSS claim selectors at sites 57
// and 58. Draw kind/count/instances stay in the existing draw envelope.
// Unreached or unavailable reads carry a null value in the JSON writer.
template <class T>
struct FssRead {
    bool reached;
    bool known;
    T value;
};

enum class FssTraceFactKind : uint8_t {
    kPanel = 12,
    kReveal = 13,
};

struct FssHelperObservation {
    FssRead<bool> enabled;
    FssRead<bool> steady;
    FssRead<bool> lockstep;
    FssRead<bool> contextNonNull;
    FssRead<bool> guardCallReached;
    FssRead<bool> callbackEntered;
    FssRead<bool> vsGetShaderCompleted;
    FssRead<bool> shaderNonNull;
    FssRead<bool> lookupReached;
    FssRead<bool> lookupCompleted;
    FssRead<uint64_t> assignedHash;
    FssRead<bool> releaseReached;
    FssRead<bool> releaseCompleted;
    FssRead<bool> callbackCompleted;
    FssRead<bool> guardReturned;
    FssRead<uint64_t> hashAfterGuard;
};

struct FssPanelObservation {
    FssRead<bool> outerEnabled;
    FssRead<uint32_t> bodyFrame;
    FssRead<uint32_t> frameNo;
    FssHelperObservation helper;
    FssRead<uint64_t> matchedHashBefore;
    FssRead<uint64_t> matchedHashAfter;
};

struct FssRevealObservation {
    FssRead<bool> outerSteady;
    FssRead<bool> outerLockstep;
    FssRead<uint32_t> bodyFrame;
    FssRead<uint32_t> bodyFrameNo;
    FssRead<uint32_t> jumpFrame;
    FssRead<uint32_t> jumpFrameNo;
    FssRead<bool> modeLatch;
    FssHelperObservation helper;
    FssRead<bool> arrivalOpen;
    FssRead<uint32_t> arrivalBefore;
    FssRead<uint32_t> arrivalAfter;
};

struct FssObservation {
    FssTraceFactKind kind = FssTraceFactKind::kPanel;
    // True when the actual handler ran; rawProbeReached identifies the
    // separate frozen outer-selector probe on an unvisited handler path.
    bool handlerInvoked = false;
    bool rawProbeReached = false;
    FssPanelObservation panel;
    FssRevealObservation reveal;
};

}  // namespace edvr

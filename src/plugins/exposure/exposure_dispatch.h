#pragma once

#include <d3d11.h>

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace edvr::plugins::exposure {

// Consecutive frames a detected candidate must run exactly twice before the
// fix acts on it.
constexpr uint32_t kExposureConfirmFrames = 5;

// Private state carried by the core hook's existing State allocation. This is
// an internal module boundary, not a stable external ABI.
struct ExposureDispatchObserverState {
    bool     enabled = false;
    uint64_t targetHash = 0;      // pinned by config, or learned by detection
    bool     pinned = false;      // true if the hash came from config
    bool     rejected = false;

    // Shape detection.
    //
    // A bytecode hash identifies one compiled shader and changes whenever the
    // game's shaders are rebuilt, so pinning one means the fix breaks on every
    // update until somebody re-derives it. The pass's SHAPE is far more stable:
    // it writes a small structured buffer of exposure state and a tiny
    // parameter texture, and it runs once per eye. Detecting that costs one
    // evaluation per distinct compute shader and then nothing.
    std::unordered_map<uint64_t, bool> shapeVerdict;
    // Hashes the shape test has ever run on. A counter cannot do this job: the
    // prune below removes negatives every frame, so the map "forgets" a shader
    // and the next frame's probe counts it again -- the give-up notice, which
    // calls itself the thing to report, would print tens of thousands where it
    // means a handful. This set is never pruned; it holds one 64-bit hash per
    // distinct compute shader the game creates.
    std::unordered_set<uint64_t> everExamined;
    uint32_t detectStreak = 0;    // consecutive frames the candidate ran twice
    bool     announced = false;
    bool     gaveUpNotice = false;

    uint32_t seenThisFrame = 0;
    ID3D11UnorderedAccessView* firstEye[4] = {nullptr, nullptr, nullptr, nullptr};
};

}  // namespace edvr::plugins::exposure

namespace edvr {

struct ExposureDispatchTicket {
    uint32_t target;
};

// Begin classifies the currently bound compute shader before the game's
// Dispatch. Complete consumes that verdict after the real Dispatch and reads
// UAVs from the now-current binding shadow.
ExposureDispatchTicket exposurePluginBeginDispatch(void* state);
void exposurePluginCompleteDispatch(void* state, ExposureDispatchTicket ticket,
                                    ID3D11DeviceContext* context);
void exposurePluginResetDispatchFrame(void* state);
void exposurePluginExpireDispatchVerdicts(void* state);

// Core-owned D3D actions remain behind this direct bridge so the observer
// module owns dispatch classification/pairing without owning the damper.
void exposureDispatchApplyPair(void* state, ID3D11DeviceContext* context,
                               ID3D11UnorderedAccessView** first,
                               ID3D11UnorderedAccessView** second);

}  // namespace edvr

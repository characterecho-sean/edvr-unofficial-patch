#pragma once

#include <d3d11.h>

#include <cstdint>

#include "exposure_dispatch.h"

namespace edvr { class Config; }

namespace edvr::plugins::exposure {

// Action state is embedded in the core's one Exposure State allocation. It
// extends the observer state because a confirmed dispatch pair both classifies
// and acts on this same per-context state object.
struct ExposureActionState : ExposureDispatchObserverState {
    uint32_t copyMask = 0xF;
    bool     copyBtoA = false;
    uint64_t applied = 0;

    float    dampK = 0.0f;             // experimental.exposure_damping, 0..1
    float    dampTau = 45.0f;          // experimental.exposure_damping_tau, secs
    ID3D11Texture2D* dampStaging[2] = {nullptr, nullptr}; // strip ping-pong pair
    int      dampCur = 0;
    bool     dampPrevValid = false;
    bool     dampHaveMean = false;
    float    dampMean[6] = {};
    uint64_t dampSeedMs = 0;
    uint64_t dampDevSinceMs = 0;
    uint64_t dampSnaps = 0;
    void*    dampLastStrip = nullptr;  // identity only; never dereferenced
    uint64_t dampStableSinceMs = 0;
    uint64_t dampStepMs = 0;
    uint64_t dampWrites = 0;
    uint64_t dampWritesAtNote = 0;
    uint64_t dampLastNoteMs = 0;
};

void exposurePluginShareExposure(
    ExposureActionState* state, ID3D11DeviceContext* context,
    ID3D11UnorderedAccessView* const* first,
    ID3D11UnorderedAccessView* const* second);
void exposurePluginDamp(ExposureActionState* state,
                        ID3D11DeviceContext* context,
                        ID3D11UnorderedAccessView* firstEyeStrip);
void exposurePluginConfigure(ExposureActionState* state, Config& config);
void exposurePluginShutdownResources(ExposureActionState* state);

}  // namespace edvr::plugins::exposure

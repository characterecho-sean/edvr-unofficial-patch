#pragma once
#include "engine_velocity.h"
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace edvr {
enum class FlatMonoResolveMode { Taa, Dlaa, Dlss, Fsr };
struct FlatMonoResolveFrame {
    ID3D11ShaderResourceView* color = nullptr;
    ID3D11ShaderResourceView* depth = nullptr;
    uint32_t renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
    float camera[6][4] = {}, previousCamera[6][4] = {}; // unjittered b1[270..275]
    EngineVelocityViews engine{};
    uint64_t frame = 0;
    float deltaMs = 0;
    bool reset = true;
    FlatMonoResolveMode mode = FlatMonoResolveMode::Taa;
};
// Owner-thread cumulative diagnostics. A full renderer reset preserves these
// counts so a session summary can expose repeated state or texture rebuilds.
struct FlatMonoResolveStats {
    uint64_t calls = 0;
    uint64_t initializations = 0, contextPointerMismatches = 0;
    uint64_t allocations = 0, fullResets = 0, invalidations = 0;
    uint64_t acceptedResets = 0, acceptedContinues = 0;
    uint64_t requestedResets = 0, lostHistory = 0, frameGaps = 0;
    uint64_t invalidPreviousCameras = 0, formatChanges = 0, cameraCuts = 0;
    uint64_t backendFailures = 0;
    uint64_t currentContinueRun = 0, longestContinueRun = 0;
};
FlatMonoResolveStats flatMonoResolveStats();
// Owner immediate context only. Inputs borrowed for this call; successful output
// is AddRef'd and output-sized. The caller suppresses hook observations throughout
// this call. D3D11.1 context-state isolation is required and restored on every exit.
// This first integration deliberately supplies ZERO jitter to all backends.
bool flatMonoResolve(ID3D11Device*, ID3D11DeviceContext*, const FlatMonoResolveFrame&,
                     ID3D11ShaderResourceView** output, const char** reason);
// Owner thread: release renderer resources/history. Does not shut down shared SDKs.
void flatMonoResolveReset();
// Owner thread: a refused/missing frame breaks only history, preserving resources.
void flatMonoResolveInvalidateHistory();
} // namespace edvr

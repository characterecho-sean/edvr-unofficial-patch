#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace edvr {

// XeSS-SR keeps one feature per upscaler slot (dlaa.h, kUpscalerSlots); each
// slot is keyed and rebuilt on its own. The flat resolver uses slot 0. This
// backend uses Intel's D3D12 SDK over a shared D3D11/D3D12 device pair; it does
// not provide frame generation.
bool xessBuiltIn();
bool xessAvailable(ID3D11Device* device, const char** why);

// Colour, motion, and depth are the flat resolver's render-grid textures;
// output is its display-grid texture. Motion is current -> previous in input
// pixels. Jitter is the positive-right/down projection phase in input pixels.
// On HDR frames colour is R11G11B10_FLOAT and output is R16G16B16A16_FLOAT;
// XeSS receives matched R11 input/output and a precompiled compute copy widens
// the result. LDR uses matched R8G8B8A8_UNORM directly.
bool xessEvaluate(ID3D11DeviceContext* context, unsigned slot,
                  ID3D11Texture2D* colour, ID3D11Texture2D* depth,
                  ID3D11Texture2D* motion, ID3D11Texture2D* output,
                  uint32_t width, uint32_t height, uint32_t outputWidth,
                  uint32_t outputHeight, float jitterX, float jitterY,
                  bool reset, bool hdr, const char** why);

// Releases every slot's feature (each with its bounded GPU wait); the library
// and the D3D12 device stay loaded. Safe when XeSS never started.
void xessReleaseFeatures();
void xessShutdown();

// Rig seams (fsr3_engine.h's fsr3TestContextCreations pattern): features a slot
// has built since the backend started, and whether a slot holds one now.
uint32_t xessTestFeatureCreations(unsigned slot);
bool xessTestFeatureLive(unsigned slot);

// The slot's measured price, as the temporal pass's totals line reads it: evaluations, resets, and the mean and
// maximum CPU milliseconds per successful evaluate. False until the slot has evaluated, and in a build without the SDK.
bool xessSlotTotals(unsigned slot, uint32_t* evaluations, uint32_t* resets, double* avgMs, double* maxMs);

// Bridge timing (log only). Every 30 s, a slot that executed logs one line splitting the D3D11<->D3D12 bridge: the
// D3D11 copy-in, wait and copy-out intervals, the D3D12 execute, and the whole D3D11 span, all from GPU timestamps read
// without a CPU wait. Reads trail the GPU by a few evaluations; `unread` counts evaluations still waiting on it.
struct XessBridgeTiming {
    unsigned evals = 0;    // evaluations read in the window (the line's k)
    unsigned unread = 0;   // evaluations whose timing had not been read when the line was made
    unsigned d11Samples = 0, d12Samples = 0;
    double copyInMedian = 0.0, copyInP95 = 0.0, waitMedian = 0.0, waitP95 = 0.0;
    double executeMedian = 0.0, executeP95 = 0.0, copyOutMedian = 0.0, copyOutP95 = 0.0;
    double totalMedian = 0.0, totalP95 = 0.0;
    // CPU spans per executed evaluation (QueryPerformanceCounter): entry to return, the allocator-reuse wait, and the
    // queue Wait plus ExecuteCommandLists through Signal. `blocked` counts evaluations whose allocator fence had not passed.
    unsigned cpuEvals = 0, blocked = 0;
    double evalMedian = 0.0, evalP95 = 0.0, allocMedian = 0.0, allocP95 = 0.0;
    double submitMedian = 0.0, submitP95 = 0.0;
    bool submitTimed = false;
    char line[768] = "";   // the text logged by the report
};
// Reads back whatever the GPU has finished, for every slot. Never waits. Returns the evaluations still unread.
unsigned xessBridgeTimingSweep(ID3D11DeviceContext* context);
// Logs and fills the slot's line now, then starts a new window. False when the slot has executed nothing in its window.
bool xessBridgeTimingReport(ID3D11DeviceContext* context, unsigned slot, XessBridgeTiming* out);

}  // namespace edvr

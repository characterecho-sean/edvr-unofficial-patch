// The sky ring module: see sky_ring.h.
#include "temporal_shader_bytecode.h"
#include "sky_ring.h"
#include "sky_ring_gpu.h"
#include "shader_swap.h"
#include "../common/log.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

namespace edvr {
namespace {

struct Request {
    std::string trigger;
    std::vector<std::pair<uint32_t, uint32_t>> events;
    std::vector<SkyJoinRow> rows;
    int handle = -1;        // the staging buffer holding the copy, -1 until the copy is made
    uint32_t polls = 0;
};

constexpr size_t kMaxRequests = 4;
constexpr uint32_t kMaxPolls = 600;   // eye-frames; at 180 a second, a few seconds

std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_dispatchCount{0};   // survives a release, for the "never ran" line
std::mutex g_mutex;                         // the request queue (the Pause key and the render path)
std::vector<Request> g_requests;
// Never destroyed: the process teardown can run under the loader lock, where releasing D3D objects is not safe (see temporalPassShutdown).
SkyRingGpu* const g_gpuPtr = new SkyRingGpu();
SkyRingGpu& g_gpu = *g_gpuPtr;
ID3D11ComputeShader* g_cs = nullptr;
bool g_csTried = false;
bool g_createNoted = false;

void printDump(const Request& r, const std::vector<skyring::Entry>& entries) {
    char line[512];
    size_t withSky = 0;
    for (const auto& e : entries) if (e.skyN) ++withSky;
    bool truncated = false;
    const auto picks = skyring::windowsAround(entries, r.events, skyring::kMaxLines, &truncated);
    if (skyring::formatHeader(line, sizeof(line), entries, withSky, r.events.size(), picks.size(), truncated) > 0)
        Log::get().note("%s (trigger: %s)", line, r.trigger.c_str());
    std::unordered_map<uint64_t, const SkyJoinRow*> join;
    for (const auto& row : r.rows) join[(static_cast<uint64_t>(row.frame) << 1) | (row.eye & 1u)] = &row;
    for (const auto& b : skyring::bucketsOf(entries))
        if (skyring::formatBucket(line, sizeof(line), b) > 0) Log::get().note("%s", line);
    for (const auto& p : picks) {
        const skyring::Entry& e = entries[p.entry];
        char head[400];
        if (skyring::formatEntry(head, sizeof(head), e) <= 0) continue;
        char tail[96] = "";
        const auto it = join.find((static_cast<uint64_t>(e.frame) << 1) | (e.eye & 1u));
        if (it != join.end() && it->second->have)
            std::snprintf(tail, sizeof(tail), " rot=%.2f mv=%.3f", static_cast<double>(it->second->rotDeg), static_cast<double>(it->second->mvM));
        Log::get().note("%s d=%+d%s%s", head, p.delta, tail, p.inEvent ? " EVENT" : "");
    }
    Log::get().note("--- end sky ring ---");
}

}  // namespace

void skyRingConfigure(bool on) {
    const bool was = g_on.exchange(on);
    if (was != on)
        Log::get().note("sky ring (diag.sky_ring, temporary): %s", on ? "on -- one small dispatch per eye-frame after the upscaler; the Pause dump prints it" : "off");
}

bool skyRingOn() { return g_on.load(); }
uint64_t skyRingDispatches() { return g_dispatchCount.load(); }

bool skyRingEyeFrame(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t eye, uint32_t frame, ID3D11ShaderResourceView* raw,
                     ID3D11ShaderResourceView* out, ID3D11ShaderResourceView* mv, ID3D11ShaderResourceView* depth, uint32_t inW, uint32_t inH,
                     uint32_t outW, uint32_t outH, float projA, bool depthValid) {
    if (!g_on.load() || !dev || !ctx) return false;
    if (g_gpu.ready() && g_gpu.device() != dev) {   // a new device: the ring starts over
        g_gpu.release();
        if (g_cs) { g_cs->Release(); g_cs = nullptr; }
        g_csTried = false;
    }
    if (!g_gpu.ready()) {
        if (!g_csTried) {
            g_csTried = true;
            g_cs = shaderSwapCreateCs(ctx, kSkyRingBytecode, sizeof(kSkyRingBytecode), "sky_ring_cs", "sky ring");
        }
        if (!g_cs || !g_gpu.create(dev, g_cs)) {
            if (!g_createNoted) {
                g_createNoted = true;
                Log::get().note("sky ring: the ring or its shader could not be created; the key stands down (the Pause dump will say no samples).");
            }
            g_on.store(false);
            return false;
        }
        Log::get().note("sky ring: created -- %u entries of %u bytes, a %ux%u sample grid over each eye, read back only on a Pause press.",
                        skyring::kRingEntries, skyring::kEntryBytes, skyring::kGridDim, skyring::kGridDim);
    }
    const bool ran = g_gpu.dispatch(ctx, eye, frame, raw, out, mv, depth, inW, inH, outW, outH, projA, depthValid);
    if (ran) g_dispatchCount.store(g_gpu.dispatches());

    // The requests: copy the ring after this dispatch for any that wait for one, and poll the copies already made (never waiting).
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_requests.begin(); it != g_requests.end();) {
            Request& r = *it;
            if (r.handle < 0) {
                r.handle = g_gpu.requestCopy(ctx);   // -1 again when both buffers are busy: tried next frame
            } else {
                std::vector<skyring::Entry> entries;
                bool failed = false;
                if (g_gpu.poll(ctx, r.handle, &entries, &failed)) {
                    printDump(r, entries);
                    it = g_requests.erase(it);
                    continue;
                }
                if (failed || ++r.polls > kMaxPolls) {
                    Log::get().note("sky ring: the readback %s; this dump has no SKY lines (trigger: %s).", failed ? "failed" : "did not complete in time", r.trigger.c_str());
                    it = g_requests.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }
    return ran;
}

void skyRingRequestDump(const char* trigger, const std::vector<std::pair<uint32_t, uint32_t>>& events, const std::vector<SkyJoinRow>& rows) {
    if (g_dispatchCount.load() == 0) {
        char line[160];
        if (skyring::formatNoSamples(line, sizeof(line), g_on.load(), 0) > 0) Log::get().note("%s", line);
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_requests.size() >= kMaxRequests) {
        Log::get().note("sky ring: %zu dumps are still waiting for the GPU; this press's SKY lines are dropped.", g_requests.size());
        return;
    }
    Request r;
    r.trigger = trigger ? trigger : "diagnostic request";
    r.events = events;
    r.rows = rows;
    g_requests.push_back(std::move(r));
}

void skyRingShutdown() {
    g_on.store(false);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_requests.clear();   // the ring and the shader are left to the process (see g_gpuPtr)
}

}  // namespace edvr

#include "vram_tick.h"

#include <windows.h>
#include <d3d11.h>

#include "../common/guard.h"
#include "../common/log.h"
#include "../common/timing.h"
#include "../common/vram_query.h"
#include "../common/vram_watch.h"

namespace edvr {
namespace {

VramWatcher g_watcher;

// The adapter's IDXGIAdapter3: one reference, taken at the first sample and never released. It is an object of the
// OS's own dxgi.dll, the watch lives as long as the game does, and a Release at teardown could land after that DLL
// has gone. (perf_monitor.cpp's adapter3 for the Monitor page is held the same way.)
IDXGIAdapter3* g_adapter = nullptr;

FaultBudget g_budget("vramWatch", 3);

}  // namespace

void vramWatchTick(int64_t qpc, ID3D11Device* device) {
    // The one compare a frame costs (vram_watch.h): until the second is up nothing below runs.
    if (!g_watcher.due(qpc)) return;
    guardedBudget(g_budget, [&] {
        g_watcher.tick(
            qpc, qpcFrequency(), nowMs(),
            [&] {
                VramWatcher::Opening opening;
                const char* why = nullptr;
                g_adapter = vramAdapterOf(device, &why, opening.adapter, sizeof(opening.adapter));
                opening.ok = g_adapter != nullptr;
                opening.why = why ? why : "";
                return opening;
            },
            [&](long* hr) {
                HRESULT result = S_OK;
                const VramFigures figures = vramRead(g_adapter, &result);
                *hr = static_cast<long>(result);
                return figures;
            },
            [](const char* line) { Log::get().note("%s", line); });
    });
}

}  // namespace edvr

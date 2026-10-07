// Linked only into build/flat_sdk_bench_proxy.dll.  This reconstructs the
// game's emit-hook availability while retaining the production draw, GPU
// source, owner, H-selection, and resolver implementations.
namespace edvr {
bool edvrOfflineEmitHookLive(const char** why) noexcept {
    if (why) *why = nullptr;
    return true;
}
const char* edvrOfflineEmitAttach() noexcept { return "installed"; }
void edvrOfflineEmitDetach() noexcept {}
}

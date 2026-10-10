#include "../../src/common/config.h"
#include "../../src/d3d11/intro_lifecycle.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {
std::vector<std::string> g_calls;
}

namespace edvr {

Config& Config::get() {
    static Config config;
    return config;
}

void introSkipConfigure(Config&) { g_calls.emplace_back("skip.configure"); }
void introUpscaleConfigure(Config&) { g_calls.emplace_back("upscale.configure"); }
void introSkipTick(bool sceneFrame) {
    g_calls.emplace_back(sceneFrame ? "skip.frame.scene" : "skip.frame.menu");
}
void introUpscaleShutdown() { g_calls.emplace_back("upscale.shutdown"); }
void introSkipShutdown() { g_calls.emplace_back("skip.shutdown"); }

}  // namespace edvr

int main(int argc, char** argv) {
    if (argc > 2 || (argc == 2 && std::string(argv[1]) != "--dry-run" &&
                                  std::string(argv[1]) != "--self-test")) {
        std::puts("Usage: intro_lifecycle_test [--dry-run|--self-test]");
        return 2;
    }
    if (argc == 2 && std::string(argv[1]) == "--dry-run") {
        std::puts("DRY RUN: INTRO_LIFECYCLE validates real callback metadata and order");
        return 0;
    }

    const EdvrPluginLifecycleOps* ops = edvr::introVideoLifecycleOps();
    if (!ops || ops != edvr::introVideoLifecycleOps() ||
        ops->structSize != sizeof(*ops) || ops->manifestIndex != 4 ||
        !ops->manifestId || std::string(ops->manifestId) != "intro" ||
        !ops->configure || !ops->frame || !ops->shutdown) {
        std::puts("FAIL: intro lifecycle ops metadata/identity");
        return 1;
    }

    ops->configure(nullptr);
    if (!g_calls.empty()) {
        std::puts("FAIL: null config invoked intro callbacks");
        return 1;
    }

    edvr::Config& config = edvr::Config::get();
    ops->configure(&config);
    ops->frame(ops->state, 0);
    ops->frame(ops->state, 1);
    ops->frame(ops->state, 2);
    ops->frame(ops->state, UINT32_MAX);
    ops->shutdown(ops->state);

    const std::vector<std::string> expected = {
        "skip.configure", "upscale.configure", "skip.frame.menu",
        "skip.frame.scene", "skip.frame.scene", "skip.frame.scene",
        "upscale.shutdown", "skip.shutdown"};
    if (g_calls != expected) {
        std::puts("FAIL: intro lifecycle callback order/scene mapping");
        for (const std::string& call : g_calls) std::printf("  %s\n", call.c_str());
        return 1;
    }
    std::puts("PASS: INTRO_LIFECYCLE callback order, identity, and edge cases");
    return 0;
}

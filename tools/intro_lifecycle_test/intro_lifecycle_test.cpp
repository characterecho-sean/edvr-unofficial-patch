#include "../../src/common/config.h"
#include "../../src/d3d11/intro_lifecycle.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {
struct Call {
    std::string name;
    void* config = nullptr;
    ID3D11DeviceContext* context = nullptr;
    uint32_t sceneFrame = 0;
};

std::vector<Call> g_calls;

void record(const char* name, void* config = nullptr,
            ID3D11DeviceContext* context = nullptr, uint32_t sceneFrame = 0) {
    g_calls.push_back({name, config, context, sceneFrame});
}

bool expectNames(const std::vector<std::string>& expected) {
    if (g_calls.size() != expected.size()) {
        std::printf("FAIL: callback count expected %zu, got %zu\n",
                    expected.size(), g_calls.size());
        return false;
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        if (g_calls[i].name != expected[i]) {
            std::printf("FAIL: callback %zu expected %s, got %s\n", i,
                        expected[i].c_str(), g_calls[i].name.c_str());
            return false;
        }
    }
    return true;
}

bool expectConfig(size_t index, void* config) {
    if (g_calls[index].config == config) return true;
    std::printf("FAIL: %s received config %p, expected %p\n",
                g_calls[index].name.c_str(), g_calls[index].config, config);
    return false;
}

bool expectFrame(size_t index, ID3D11DeviceContext* context,
                 uint32_t sceneFrame) {
    const Call& call = g_calls[index];
    if (call.context == context && call.sceneFrame == sceneFrame) return true;
    std::printf("FAIL: %s received context %p/scene %u, expected %p/%u\n",
                call.name.c_str(), call.context, call.sceneFrame, context,
                sceneFrame);
    return false;
}
}  // namespace

namespace edvr {

Config& Config::get() {
    static Config config;
    return config;
}

void introSkipConfigure(Config& cfg) { record("skip.configure", &cfg); }
void introUpscaleConfigure(Config& cfg) { record("upscale.configure", &cfg); }
void introSkipTick(bool sceneFrame) {
    record("skip.frame", nullptr, nullptr, sceneFrame ? 1u : 0u);
}
void introUpscaleShutdown() { record("upscale.shutdown"); }
void introSkipShutdown() { record("skip.shutdown"); }

void loaderPanelConfigure(Config& cfg) { record("loader.configure", &cfg); }
void loaderPanelTick(ID3D11DeviceContext* ctx, bool sceneFrame) {
    record("loader.frame", nullptr, ctx, sceneFrame ? 1u : 0u);
}
void loaderPanelShutdown() { record("loader.shutdown"); }
void splashDimConfigure(Config& cfg) { record("splash.configure", &cfg); }
void splashDimShutdown() { record("splash.shutdown"); }
void introPanelConfigure(Config& cfg) { record("panel.configure", &cfg); }
void introPanelTick(ID3D11DeviceContext* ctx, bool sceneFrame) {
    record("panel.frame", nullptr, ctx, sceneFrame ? 1u : 0u);
}
void introPanelShutdown() { record("panel.shutdown"); }
void introCurveTick(ID3D11DeviceContext* ctx, bool sceneFrame) {
    record("curve.frame", nullptr, ctx, sceneFrame ? 1u : 0u);
}
void introCurveShutdown() { record("curve.shutdown"); }
void backdropConfigure(Config& cfg) { record("backdrop.configure", &cfg); }
void backdropShutdown() { record("backdrop.shutdown"); }

}  // namespace edvr

int main(int argc, char** argv) {
    if (argc > 2 || (argc == 2 && std::string(argv[1]) != "--dry-run" &&
                                  std::string(argv[1]) != "--self-test")) {
        std::puts("Usage: intro_lifecycle_test [--dry-run|--self-test]");
        return 2;
    }
    if (argc == 2 && std::string(argv[1]) == "--dry-run") {
        std::puts("DRY RUN: INTRO_LIFECYCLE validates staged callbacks and order");
        return 0;
    }

    const EdvrPluginLifecycleOps* ops = edvr::introVideoLifecycleOps();
    if (!ops || ops != edvr::introVideoLifecycleOps() ||
        ops->structSize != sizeof(*ops) || ops->manifestIndex != 4 ||
        !ops->manifestId || std::string(ops->manifestId) != "intro" ||
        !ops->configure || !ops->frame || !ops->shutdown ||
        !ops->configureStage || !ops->frameStage || !ops->shutdownStage) {
        std::puts("FAIL: intro lifecycle ops metadata/identity/stage callbacks");
        return 1;
    }

    edvr::Config& config = edvr::Config::get();
    ID3D11DeviceContext* context =
        reinterpret_cast<ID3D11DeviceContext*>(uintptr_t{0x1234});

    // Null config is a no-op for both legacy and staged callbacks.
    ops->configure(nullptr);
    ops->configureStage(ops->state, edvr::kIntroLifecycleLoader, nullptr);
    if (!g_calls.empty()) {
        std::puts("FAIL: null config invoked intro callbacks");
        return 1;
    }

    // Unknown stages and operation-incompatible stages must remain no-ops.
    ops->configureStage(ops->state, edvr::kIntroLifecycleCurve, &config);
    ops->configureStage(ops->state, 99, &config);
    ops->frameStage(ops->state, edvr::kIntroLifecycleSplashDim, context, 1);
    ops->frameStage(ops->state, edvr::kIntroLifecycleBackdrop, context, 1);
    ops->frameStage(ops->state, 99, context, 1);
    ops->shutdownStage(ops->state, 99);
    if (!g_calls.empty()) {
        std::puts("FAIL: unknown or unsupported stage invoked a callback");
        return 1;
    }

    // Match VScreen's interleaving: configure stages at their old callsites,
    // then the four independent frame phases, then the separated shutdowns.
    ops->configureStage(ops->state, edvr::kIntroLifecycleLoader, &config);
    ops->configureStage(ops->state, edvr::kIntroLifecycleSplashDim, &config);
    ops->configureStage(ops->state, edvr::kIntroLifecyclePanel, &config);
    ops->configure(&config);
    ops->configureStage(ops->state, edvr::kIntroLifecycleBackdrop, &config);

    ops->frameStage(ops->state, edvr::kIntroLifecyclePanel, context, 0);
    ops->frameStage(ops->state, edvr::kIntroLifecycleCurve, context, 9);
    ops->frame(ops->state, 9);
    ops->frameStage(ops->state, edvr::kIntroLifecycleLoader, context, 9);

    ops->shutdownStage(ops->state, edvr::kIntroLifecycleLoader);
    ops->shutdownStage(ops->state, edvr::kIntroLifecycleSplashDim);
    ops->shutdownStage(ops->state, edvr::kIntroLifecycleBackdrop);
    ops->shutdownStage(ops->state, edvr::kIntroLifecyclePanel);
    ops->shutdownStage(ops->state, edvr::kIntroLifecycleCurve);
    ops->shutdown(ops->state);

    const std::vector<std::string> expected = {
        "loader.configure", "splash.configure", "panel.configure",
        "skip.configure", "upscale.configure", "backdrop.configure",
        "panel.frame", "curve.frame", "skip.frame", "loader.frame",
        "loader.shutdown", "splash.shutdown", "backdrop.shutdown",
        "panel.shutdown", "curve.shutdown", "upscale.shutdown",
        "skip.shutdown"};
    if (!expectNames(expected)) {
        for (const Call& call : g_calls) std::printf("  %s\n", call.name.c_str());
        return 1;
    }

    for (size_t i = 0; i <= 5; ++i) {
        if (!expectConfig(i, &config)) return 1;
    }
    if (!expectFrame(6, context, 0) || !expectFrame(7, context, 1) ||
        g_calls[8].sceneFrame != 1 || !expectFrame(9, context, 1)) {
        std::puts("FAIL: staged context forwarding or nonzero scene mapping");
        return 1;
    }

    std::puts("PASS: INTRO_LIFECYCLE staged callback identity, forwarding, and order");
    return 0;
}

#define CINTERFACE
#define COBJMACROS
#define D3D11_NO_HELPERS
#include <windows.h>
#include <d3d11.h>

#include "../../src/d3d11/particle_fix.h"
#include "../../src/d3d11/binding_shadow.h"
#include "../../src/d3d11/shader_swap.h"
#include "../../src/common/config.h"
#include "../../src/common/guard.h"
#include "../../src/common/log.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace edvr {
namespace detail {
BindingSlot g_bindingSlots[static_cast<size_t>(BindSlot::Count)] = {};
}

uint64_t lookupShaderHash(void* shader);
extern uint64_t g_starsSkipped;

namespace {
unsigned g_failures = 0;
unsigned g_checks = 0;
unsigned g_vsGets = 0;
unsigned g_shaderReleases = 0;
unsigned g_hashLookups = 0;
uint64_t g_lookupHash = 0;
ID3D11VertexShader* g_returnShader = nullptr;

void check(bool condition, const char* label) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", label);
    }
}

void STDMETHODCALLTYPE fakeVsGetShader(ID3D11DeviceContext*,
                                        ID3D11VertexShader** shader,
                                        ID3D11ClassInstance**, UINT* count) {
    ++g_vsGets;
    if (shader) *shader = g_returnShader;
    if (count) *count = 0;
}

ULONG STDMETHODCALLTYPE fakeShaderRelease(ID3D11VertexShader*) {
    ++g_shaderReleases;
    return 1;
}

ID3D11DeviceContext g_context = {};
ID3D11DeviceContextVtbl g_contextVtable = {};
ID3D11VertexShader g_shader = {};
ID3D11VertexShaderVtbl g_shaderVtable = {};

void resetShadow(void* ptr, uint64_t hash) {
    auto& slot = detail::g_bindingSlots[static_cast<size_t>(BindSlot::Vs)];
    slot.ptr = ptr;
    slot.hash = hash;
    slot.gen = 1;
}

void resetCalls(uint64_t fallbackHash, ID3D11VertexShader* shader) {
    g_vsGets = 0;
    g_shaderReleases = 0;
    g_hashLookups = 0;
    g_lookupHash = fallbackHash;
    g_returnShader = shader;
}

constexpr uint64_t kStarHash = 0x9AEC596A2B036EA6ull;
constexpr uint64_t kOtherHash = 0x123456789ABCDEF0ull;

struct Case {
    const char* label;
    bool hidden;
    bool context;
    char kind;
    uint32_t count;
    uint32_t instances;
    void* cachedPointer;
    uint64_t cachedHash;
    ID3D11VertexShader* fallbackShader;
    uint64_t fallbackHash;
    bool expectedSkip;
    bool expectedHashKnown;
    edvr::WitchspaceStarsHashSource expectedSource;
    uint64_t expectedResolvedHash;
    unsigned expectedQueries;
    unsigned expectedReleases;
    unsigned expectedLookups;
};

void runCase(const Case& test) {
    edvr::detail::g_particleHideStars = test.hidden;
    resetShadow(test.cachedPointer, test.cachedHash);
    resetCalls(test.fallbackHash, test.fallbackShader);
    edvr::g_starsSkipped = 0;

    edvr::WitchspaceStarsObservation observation{};
    ID3D11DeviceContext* ctx = test.context ? &g_context : nullptr;
    const bool traced = edvr::witchspaceStarsSkipTraced(
        ctx, test.kind, test.count, test.instances, &observation);
    const unsigned tracedQueries = g_vsGets;
    const unsigned tracedReleases = g_shaderReleases;
    const unsigned tracedLookups = g_hashLookups;
    const uint64_t tracedSkipped = edvr::g_starsSkipped;

    check(traced == test.expectedSkip, test.label);
    check(observation.hidden == test.hidden, "trace reports the switch state");
    check(observation.skippedDeltaKnown &&
              observation.skippedDelta == (test.expectedSkip ? 1u : 0u) &&
              tracedSkipped == (test.expectedSkip ? 1u : 0u),
          "trace and real skipped counter agree by exactly zero or one");
    if (!test.hidden) {
        check(!observation.contextKnown && !observation.shapeReached &&
                  !observation.hashKnown,
              "switch-off trace stops before context and shape inspection");
    } else if (!test.context) {
        check(observation.contextKnown && !observation.contextValid &&
                  !observation.shapeReached && !observation.hashKnown,
              "null-context trace stops before shape and hash inspection");
    } else {
        check(observation.contextKnown && observation.contextValid &&
                  observation.shapeReached,
              "valid context reaches the production shape branch");
        const bool shapeExpected =
            (test.kind == 'X' || test.kind == 'N') && test.count >= 6 &&
            test.instances != 0;
        check(observation.shapeMatched == shapeExpected,
              "trace reports the production helper's shape decision");
        if (shapeExpected) {
            check(observation.hashKnown == test.expectedHashKnown &&
                      observation.hashSource == test.expectedSource &&
                      observation.vsHash == test.expectedResolvedHash,
                  "trace reports actual cached or fallback hash source and value");
        } else {
            check(!observation.hashKnown &&
                      observation.hashSource ==
                          edvr::WitchspaceStarsHashSource::kUnknown,
                  "declined shape does not inspect or invent a shader hash");
        }
    }
    check(tracedQueries == test.expectedQueries &&
              tracedReleases == test.expectedReleases &&
              tracedLookups == test.expectedLookups,
          "traced helper makes only the expected VSGet, Release, and lookup calls");

    // Run the unchanged production helper from the same initial counter state
    // and context setup; compare its actual decision and counter delta.
    resetCalls(test.fallbackHash, test.fallbackShader);
    edvr::g_starsSkipped = 0;
    const bool ordinary = edvr::witchspaceStarsSkip(
        ctx, test.kind, test.count, test.instances);
    check(ordinary == traced,
          "ordinary and traced production helpers return the same decision");
    check(edvr::g_starsSkipped == tracedSkipped,
          "ordinary and traced production helpers increment skip count equally");
    check(g_vsGets == test.expectedQueries &&
              g_shaderReleases == test.expectedReleases &&
              g_hashLookups == test.expectedLookups,
          "ordinary helper retains the expected query, release, and lookup behavior");
}

bool run(bool full) {
    g_context.lpVtbl = &g_contextVtable;
    g_contextVtable.VSGetShader = fakeVsGetShader;
    g_shader.lpVtbl = &g_shaderVtable;
    g_shaderVtable.Release = fakeShaderRelease;

    const Case cases[] = {
        {"hidden-off declines before all context work", false, true, 'X', 6, 1,
         &g_shader, kStarHash, &g_shader, kStarHash, false, false,
         edvr::WitchspaceStarsHashSource::kUnknown, 0, 0, 0, 0},
        {"null context is declined", true, false, 'X', 6, 1, nullptr, 0,
         &g_shader, kStarHash, false, false,
         edvr::WitchspaceStarsHashSource::kUnknown, 0, 0, 0, 0},
        {"X, six vertices, one instance, cached hit", true, true, 'X', 6, 1,
         &g_shader, kStarHash, &g_shader, kOtherHash, true, true,
         edvr::WitchspaceStarsHashSource::kBindingShadow, kStarHash, 0, 0, 0},
        {"N, six vertices, one instance, cached miss", true, true, 'N', 6, 1,
         &g_shader, kOtherHash, &g_shader, kStarHash, false, true,
         edvr::WitchspaceStarsHashSource::kBindingShadow, kOtherHash, 0, 0, 0},
        {"other kind is declined", true, true, 'Y', 6, 1, &g_shader, kStarHash,
         &g_shader, kStarHash, false, false,
         edvr::WitchspaceStarsHashSource::kUnknown, 0, 0, 0, 0},
        {"five vertices is declined", true, true, 'X', 5, 1, &g_shader,
         kStarHash, &g_shader, kStarHash, false, false,
         edvr::WitchspaceStarsHashSource::kUnknown, 0, 0, 0, 0},
        {"zero instances is declined", true, true, 'X', 6, 0, &g_shader,
         kStarHash, &g_shader, kStarHash, false, false,
         edvr::WitchspaceStarsHashSource::kUnknown, 0, 0, 0, 0},
        {"cached pointer with zero hash falls back and hits", true, true, 'X', 6, 1,
         &g_shader, 0, &g_shader, kStarHash, true, true,
         edvr::WitchspaceStarsHashSource::kFallbackShaderLookup, kStarHash, 1, 1, 1},
        {"cached pointer with zero hash falls back and misses", true, true, 'X', 6, 1,
         &g_shader, 0, &g_shader, kOtherHash, false, true,
         edvr::WitchspaceStarsHashSource::kFallbackShaderLookup, kOtherHash, 1, 1, 1},
        {"absent cached pointer falls back and hits", true, true, 'N', 6, 1,
         nullptr, kStarHash, &g_shader, kStarHash, true, true,
         edvr::WitchspaceStarsHashSource::kFallbackShaderLookup, kStarHash, 1, 1, 1},
        {"absent cached pointer falls back and misses", true, true, 'N', 6, 1,
         nullptr, 0, &g_shader, kOtherHash, false, true,
         edvr::WitchspaceStarsHashSource::kFallbackShaderLookup, kOtherHash, 1, 1, 1},
        {"fallback with no shader reports known zero", true, true, 'X', 6, 1,
         &g_shader, 0, nullptr, 0, false, true,
         edvr::WitchspaceStarsHashSource::kFallbackNoShader, 0, 1, 0, 0},
    };
    const size_t count = full ? sizeof(cases) / sizeof(cases[0]) : 3;
    for (size_t i = 0; i < count; ++i) runCase(cases[i]);
    std::printf("witchspace_stars_test: %s (%u checks, %u failures)\n",
                g_failures == 0 ? "PASS" : "FAILED", g_checks, g_failures);
    return g_failures == 0;
}

}  // namespace

uint64_t lookupShaderHash(void*) {
    ++g_hashLookups;
    return g_lookupHash;
}

Log& Log::get() {
    static Log log;
    return log;
}

Log::~Log() {}
void Log::note(const char*, ...) {}

bool Config::getBool(const char*, bool) const {
    std::abort();
}

std::string Config::getString(const char*, const char*) const {
    std::abort();
}

int guardFilter(unsigned long, const char*) {
    std::abort();
}

void FaultBudget::charge() {
    std::abort();
}

ID3D11VertexShader* shaderSwapCreateVs(ID3D11DeviceContext*, const void*,
                                      size_t, const char*, const char*) {
    std::abort();
}

}  // namespace edvr

int main(int argc, char** argv) {
    if (argc == 2 && (std::strcmp(argv[1], "--dry-run") == 0 ||
                      std::strcmp(argv[1], "--self-test") == 0)) {
        return edvr::run(std::strcmp(argv[1], "--self-test") == 0) ? 0 : 1;
    }
    std::puts("Usage: witchspace_stars_test --dry-run | --self-test");
    return 2;
}

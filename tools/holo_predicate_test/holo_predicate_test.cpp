#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

#include "../../src/common/config.h"
#include "../../src/common/frame_flag.h"
#include "../../src/common/log.h"
#include "../../src/d3d11/binding_shadow.h"
#include "../../src/d3d11/holo_fix.h"
#include "../../src/d3d11/holo_scrim_observation.h"

namespace {

using edvr::holo_scrim_observation::EyeSizeObservation;
using edvr::holo_scrim_observation::ResourceSource;
using edvr::holo_scrim_observation::Tri;

struct TestSizeState final {
    bool present = true;
    std::uint32_t eyeW = 0;
    std::uint32_t eyeH = 0;
    std::uint32_t renderW = 0;
    std::uint32_t renderH = 0;
    unsigned eyeWReads = 0;
    unsigned eyeHReads = 0;
    unsigned renderWReads = 0;
    unsigned renderHReads = 0;
};

TestSizeState g_size;
std::uint64_t g_eyeTextureCalls = 0;
bool g_eyeTextureKnown = false;
std::uint32_t g_eyeTextureW = 0;
std::uint32_t g_eyeTextureH = 0;

struct FakeView final {
    bool resolve = true;
    edvr::ResourceInfo info{};
};

FakeView g_pattern;
FakeView g_depth;
std::uint64_t g_resolveCalls = 0;

void* slotView(edvr::BindSlot slot) {
    if (slot == edvr::BindSlot::PsSrv1) return &g_pattern;
    if (slot == edvr::BindSlot::PsSrv0) return &g_depth;
    return nullptr;
}

struct SizeGetter final {
    bool statePresent() { return g_size.present; }
    std::uint32_t eyeW() { ++g_size.eyeWReads; return g_size.eyeW; }
    std::uint32_t eyeH() { ++g_size.eyeHReads; return g_size.eyeH; }
    std::uint32_t renderW() { ++g_size.renderWReads; return g_size.renderW; }
    std::uint32_t renderH() { ++g_size.renderHReads; return g_size.renderH; }
};

bool frozenNear2(std::uint32_t a, std::uint32_t b) {
    return (a > b ? a - b : b - a) <= 2u;
}

// Independent frozen 14a reference for the ordinary vScreen accessor.
bool frozenEyeSized(std::uint32_t w, std::uint32_t h) {
    TestSizeState* const s = g_size.present ? &g_size : nullptr;
    if (!s || !w || !h) return false;
    if (s->eyeW && frozenNear2(w, s->eyeW) && frozenNear2(h, s->eyeH)) return true;
    if (s->renderW && frozenNear2(w, s->renderW) && frozenNear2(h, s->renderH)) return true;
    return false;
}

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void resetSize(std::uint32_t eyeW, std::uint32_t eyeH,
               std::uint32_t renderW = 0, std::uint32_t renderH = 0,
               bool present = true) {
    g_size = TestSizeState{};
    g_size.present = present;
    g_size.eyeW = eyeW;
    g_size.eyeH = eyeH;
    g_size.renderW = renderW;
    g_size.renderH = renderH;
}

void compareSize(std::uint32_t w, std::uint32_t h, bool expected,
                 std::uint8_t expectedMask, const char* message) {
    EyeSizeObservation observed{};
    const bool got = edvr::vScreenIsEyeSizedObserved(w, h, &observed);
    const bool frozen = frozenEyeSized(w, h);
    check(got == expected && frozen == expected, message);
    check(observed.statePresent == (g_size.present ? Tri::Yes : Tri::No),
          "eye-size observation records the raw State presence input");
    check(observed.result == (expected ? Tri::Yes : Tri::No),
          "observed eye-size result agrees with frozen reference");
    check(observed.readMask == expectedMask, "eye-size lazy read mask");
    check(g_size.eyeWReads == ((expectedMask & edvr::holo_scrim_observation::kEyeWidthRead) ? 1u : 0u) &&
              g_size.eyeHReads == ((expectedMask & edvr::holo_scrim_observation::kEyeHeightRead) ? 1u : 0u) &&
              g_size.renderWReads == ((expectedMask & edvr::holo_scrim_observation::kRenderWidthRead) ? 1u : 0u) &&
              g_size.renderHReads == ((expectedMask & edvr::holo_scrim_observation::kRenderHeightRead) ? 1u : 0u),
          "eye-size getter invocation follows the read mask");
}

struct ScratchLog final {
    std::wstring path;
    bool opened = false;

    void open() {
        wchar_t temp[MAX_PATH]{};
        const DWORD length = GetTempPathW(MAX_PATH, temp);
        if (!length || length >= MAX_PATH) throw std::runtime_error("GetTempPathW");
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            wchar_t leaf[96]{};
            _snwprintf_s(leaf, _countof(leaf), _TRUNCATE,
                         L"edvr_holo_predicate_%lu_%lu_%u",
                         GetCurrentProcessId(), GetTickCount(), attempt);
            path.assign(temp, length);
            path += leaf;
            if (CreateDirectoryW(path.c_str(), nullptr)) break;
            if (GetLastError() != ERROR_ALREADY_EXISTS || attempt == 31)
                throw std::runtime_error("create Holo rig scratch directory");
            path.clear();
        }
        if (path.empty()) throw std::runtime_error("reserve Holo rig scratch directory");
        edvr::Config::get().set("log.enabled", "true");
        opened = edvr::Log::get().open(path, L"holo_predicate_test");
        if (!opened) throw std::runtime_error("open Holo rig scratch log");
    }

    std::string readAndClose() {
        if (opened) {
            edvr::Log::get().close();
            opened = false;
        }
        std::string contents;
        WIN32_FIND_DATAW data{};
        const std::wstring pattern = path + L"\\*";
        HANDLE find = FindFirstFileW(pattern.c_str(), &data);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                const std::wstring file = path + L"\\" + data.cFileName;
                std::ifstream input(file, std::ios::binary);
                contents.assign(std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>());
                DeleteFileW(file.c_str());
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
        RemoveDirectoryW(path.c_str());
        path.clear();
        return contents;
    }

    ~ScratchLog() {
        if (opened) edvr::Log::get().close();
        if (!path.empty()) {
            WIN32_FIND_DATAW data{};
            const std::wstring pattern = path + L"\\*";
            HANDLE find = FindFirstFileW(pattern.c_str(), &data);
            if (find != INVALID_HANDLE_VALUE) {
                do {
                    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    DeleteFileW((path + L"\\" + data.cFileName).c_str());
                } while (FindNextFileW(find, &data));
                FindClose(find);
            }
            RemoveDirectoryW(path.c_str());
        }
    }
};

void configureValidResources() {
    g_pattern = FakeView{};
    g_pattern.info.isTexture2D = true;
    g_pattern.info.a = 256;
    g_pattern.info.b = 256;
    g_pattern.info.fmt = 70;
    g_depth = FakeView{};
    g_depth.info.isTexture2D = true;
    g_depth.info.a = 1920;
    g_depth.info.b = 1080;
}

void runEyeSizeCases() {
    using namespace edvr::holo_scrim_observation;
    resetSize(100, 80);
    compareSize(98, 82, true, kDepthWidthRead | kDepthHeightRead |
                kEyeWidthRead | kEyeHeightRead, "eye pair accepts +/-2 on both axes");

    resetSize(100, 80, 0, 0);
    compareSize(97, 80, false, kDepthWidthRead | kDepthHeightRead |
                kEyeWidthRead | kRenderWidthRead, "eye width miss skips eye height");

    resetSize(100, 80, 50, 60);
    compareSize(100, 77, false, kDepthWidthRead | kDepthHeightRead |
                kEyeWidthRead | kEyeHeightRead | kRenderWidthRead,
                "eye height miss reaches render width then short-circuits");

    resetSize(100, 0, 0, 0);
    compareSize(100, 2, true, kDepthWidthRead | kDepthHeightRead |
                kEyeWidthRead | kEyeHeightRead,
                "zero eye height is accepted within two pixels");

    resetSize(100, 80, 1920, 1080);
    compareSize(1922, 1078, true, kDepthWidthRead | kDepthHeightRead |
                kEyeWidthRead | kRenderWidthRead | kRenderHeightRead,
                "render pair fallback accepts +/-2");

    resetSize(100, 80, 0, 0, false);
    compareSize(100, 80, false, 0, "missing State returns before dimensions");

    resetSize(100, 80);
    compareSize(0, 80, false, kDepthWidthRead, "zero depth width short-circuits height");
    resetSize(100, 80);
    compareSize(100, 0, false, kDepthWidthRead | kDepthHeightRead,
                "zero depth height short-circuits State dimensions");
}

void runHoloCases(ScratchLog& scratch) {
    using namespace edvr::holo_scrim_observation;
    configureValidResources();
    g_resolveCalls = 0;
    edvr::detail::g_holoSteady = false;
    edvr::holoPredicateSetCountersForTest(0, false);
    HoloObservation fact{};
    check(!edvr::holoOnEyeDrawObserved('X', 6, 1, &fact), "disabled outer gate declines");
    check(fact.gates.enabled == Tri::No && fact.gates.shapeReached == Tri::Unknown &&
              fact.gates.helperReached == Tri::Unknown && fact.missedDeltaKnown &&
              fact.missedDelta == 0 && fact.missedBefore == 0 && fact.missedAfter == 0 &&
              g_resolveCalls == 0,
          "disabled decline has complete zero-mutation fact and no resolve");

    edvr::detail::g_holoSteady = true;
    fact = HoloObservation{};
    check(!edvr::holoOnEyeDrawObserved('X', 5, 1, &fact), "shape miss declines");
    check(fact.gates.enabled == Tri::Yes && fact.gates.shapeReached == Tri::Yes &&
              fact.gates.shapeMatched == Tri::No && fact.gates.helperReached == Tri::Unknown &&
              fact.pattern.source == ResourceSource::NotReached && fact.missedDeltaKnown &&
              fact.missedDelta == 0 && g_resolveCalls == 0,
          "shape decline is staged without resolving");

    resetSize(1920, 1080);
    edvr::holoPredicateSetCountersForTest(0, false);
    g_resolveCalls = 0;
    const bool ordinaryValid = edvr::holoOnEyeDraw('X', 6, 1);
    const std::uint64_t ordinaryValidResolveCalls = g_resolveCalls;
    edvr::holoPredicateSetCountersForTest(0, false);
    g_resolveCalls = 0;
    fact = HoloObservation{};
    const bool observedValid = edvr::holoOnEyeDrawObserved('X', 6, 1, &fact);
    check(ordinaryValid && observedValid, "valid Holo claim matches both paths");
    check(fact.predicateResult == Tri::Yes &&
              fact.gates.enabled == Tri::Yes && fact.gates.shapeReached == Tri::Yes &&
              fact.gates.shapeMatched == Tri::Yes && fact.gates.helperReached == Tri::Yes &&
              fact.gates.helperEnabled == Tri::Yes &&
              fact.gates.helperShapeReached == Tri::Yes &&
              fact.gates.helperShapeMatched == Tri::Yes &&
              fact.pattern.source == ResourceSource::FreshResolveSuccess &&
              fact.depth.source == ResourceSource::FreshResolveSuccess &&
              fact.eyeSize.result == Tri::Yes && fact.missedDeltaKnown &&
              fact.missedBefore == 0 && fact.missedAfter == 0 && fact.missedDelta == 0 &&
              ordinaryValidResolveCalls == 2 && g_resolveCalls == ordinaryValidResolveCalls,
          "valid helper paths agree, use two resolves, and emit raw descriptors");

    FakeView savedPattern = g_pattern;
    g_pattern.resolve = false;
    edvr::holoPredicateSetCountersForTest(0, false);
    g_resolveCalls = 0;
    const bool ordinaryPatternFailure = edvr::holoOnEyeDraw('X', 6, 1);
    const std::uint64_t ordinaryPatternCalls = g_resolveCalls;
    edvr::holoPredicateSetCountersForTest(0, false);
    g_resolveCalls = 0;
    fact = HoloObservation{};
    const bool observedPatternFailure = edvr::holoOnEyeDrawObserved('X', 6, 1, &fact);
    check(!ordinaryPatternFailure && !observedPatternFailure,
          "pattern resolve failure declines in both paths");
    check(fact.pattern.source == ResourceSource::FreshResolveFailure &&
              fact.pattern.resolved == Tri::No && fact.pattern.texture2D == Tri::No &&
              fact.depth.source == ResourceSource::NotReached &&
              ordinaryPatternCalls == 1 && g_resolveCalls == ordinaryPatternCalls &&
              fact.missedBefore == fact.missedAfter,
          "both pattern-failure paths short-circuit depth with one resolve");
    g_pattern = savedPattern;

    FakeView savedDepth = g_depth;
    g_depth.resolve = false;
    edvr::holoPredicateSetCountersForTest(0, false);
    g_resolveCalls = 0;
    const bool ordinaryDepthFailure = edvr::holoOnEyeDraw('X', 6, 1);
    const std::uint64_t ordinaryDepthCalls = g_resolveCalls;
    edvr::holoPredicateSetCountersForTest(0, false);
    g_resolveCalls = 0;
    fact = HoloObservation{};
    const bool observedDepthFailure = edvr::holoOnEyeDrawObserved('X', 6, 1, &fact);
    check(!ordinaryDepthFailure && !observedDepthFailure,
          "depth resolve failure declines in both paths");
    check(fact.depth.source == ResourceSource::FreshResolveFailure &&
              fact.depth.resolved == Tri::No && fact.depth.texture2D == Tri::No &&
              fact.eyeSize.reached == Tri::Unknown && fact.missedDelta == 0 &&
              ordinaryDepthCalls == 2 && g_resolveCalls == ordinaryDepthCalls,
          "both depth-failure paths resolve twice and use default non-Texture2D guard");
    g_depth = savedDepth;

    g_depth.info.a = 1920;
    g_depth.info.b = 2;
    resetSize(1920, 0);
    fact = HoloObservation{};
    check(edvr::holoOnEyeDrawObserved('X', 6, 1, &fact),
          "production Holo helper accepts zero eye height within tolerance");
    check(fact.eyeSize.eyeH == 0 && fact.eyeSize.result == Tri::Yes,
          "zero eye height remains raw and accepted");

    g_depth.info.a = 1900;
    g_depth.info.b = 1000;
    resetSize(1920, 1080, 0, 0);
    edvr::holoPredicateSetCountersForTest(58, false);
    g_eyeTextureCalls = 0;
    g_resolveCalls = 0;
    const bool ordinaryNearMiss = edvr::holoOnEyeDraw('X', 6, 1);
    const std::uint64_t ordinaryNearMissResolveCalls = g_resolveCalls;
    const std::uint64_t ordinaryNearMissTextureCalls = g_eyeTextureCalls;
    edvr::holoPredicateSetCountersForTest(58, false);
    g_eyeTextureCalls = 0;
    g_resolveCalls = 0;
    g_eyeTextureKnown = false;
    fact = HoloObservation{};
    const bool observedNearMiss = edvr::holoOnEyeDrawObserved('X', 6, 1, &fact);
    check(!ordinaryNearMiss && !observedNearMiss && ordinaryNearMissResolveCalls == 2 &&
              g_resolveCalls == ordinaryNearMissResolveCalls &&
              ordinaryNearMissTextureCalls == 1 && g_eyeTextureCalls == ordinaryNearMissTextureCalls &&
              fact.missedBefore == 58 && fact.missedAfter == 59 &&
              fact.missedDeltaKnown && fact.missedDelta == 1 &&
              fact.missNotedBefore == Tri::No && fact.missNotedAfter == Tri::No,
          "ordinary and observed near-miss paths agree on queries and one counter increment");

    edvr::holoPredicateSetCountersForTest(59, false);
    g_eyeTextureCalls = 0;
    g_resolveCalls = 0;
    fact = HoloObservation{};
    check(!edvr::holoOnEyeDrawObserved('X', 6, 1, &fact), "eye-size mismatch declines at threshold");
    check(fact.missedBefore == 59 && fact.missedAfter == 60 && fact.missedDeltaKnown &&
              fact.missedDelta == 1 && fact.missNotedBefore == Tri::No &&
              fact.missNotedAfter == Tri::Yes && g_eyeTextureCalls > 0,
          "59 to 60 transition increments and notes exactly once");

    const std::uint64_t logCallsBefore = g_eyeTextureCalls;
    fact = HoloObservation{};
    check(!edvr::holoOnEyeDrawObserved('X', 6, 1, &fact), "noted mismatch still declines");
    check(fact.missedBefore == 60 && fact.missedAfter == 60 && fact.missedDelta == 0 &&
              fact.missNotedBefore == Tri::Yes && fact.missNotedAfter == Tri::Yes &&
              g_eyeTextureCalls == logCallsBefore,
          "noted state suppresses later counter and logging work");

    edvr::holoPredicateSetCountersForTest(
        (std::numeric_limits<std::uint64_t>::max)(), false);
    fact = HoloObservation{};
    check(!edvr::holoOnEyeDrawObserved('X', 6, 1, &fact), "wrapped counter mismatch declines");
    check(fact.missedBefore == (std::numeric_limits<std::uint64_t>::max)() &&
              fact.missedAfter == 0 &&
              fact.missedDeltaKnown && fact.missedDelta == 1 &&
              fact.missNotedAfter == Tri::No,
          "counter delta follows uint64 wrap before the >=60 test");

    const std::string logs = scratch.readAndClose();
    const std::string marker = "holo pattern: 60 hologram-shaped draw(s) went by";
    const std::size_t first = logs.find(marker);
    check(first != std::string::npos && logs.find(marker, first + marker.size()) == std::string::npos,
          "threshold note appears exactly once in the isolated scratch log");
}

}  // namespace

namespace edvr {

void* bindingGet(BindSlot slot) { return slotView(slot); }

bool bindingResolve(void* view, ResourceInfo* out) {
    ++g_resolveCalls;
    if (out) *out = ResourceInfo{};
    if (!view || !out) return false;
    const auto* fake = static_cast<const FakeView*>(view);
    if (!fake->resolve) return false;
    *out = fake->info;
    return true;
}

bool eyeTextureSize(std::uint32_t* width, std::uint32_t* height) {
    ++g_eyeTextureCalls;
    if (!g_eyeTextureKnown) return false;
    if (width) *width = g_eyeTextureW;
    if (height) *height = g_eyeTextureH;
    return true;
}

bool vScreenIsEyeSized(std::uint32_t w, std::uint32_t h) {
    return frozenEyeSized(w, h);
}

bool vScreenIsEyeSizedObserved(std::uint32_t w, std::uint32_t h,
                               holo_scrim_observation::EyeSizeObservation* observation) {
    SizeGetter getter;
    return holo_scrim_observation::isEyeSizedObserved(w, h, getter, observation);
}

}  // namespace edvr

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--dry-run") {
        std::puts("holo_predicate_test dry-run PASS");
        return 0;
    }
    if (argc != 2 || std::string(argv[1]) != "--self-test") {
        std::fprintf(stderr, "usage: holo_predicate_test.exe --dry-run|--self-test\n");
        return 2;
    }

    ScratchLog scratch;
    try {
        scratch.open();
        runEyeSizeCases();
        runHoloCases(scratch);
        std::puts("holo_predicate_test PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "holo_predicate_test FAIL: %s\n", error.what());
        return 1;
    }
}

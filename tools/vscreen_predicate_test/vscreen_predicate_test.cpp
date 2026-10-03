#include "vscreen_predicate_test.h"

#include <d3d11.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/common/system_d3d11.h"
#include "../../src/d3d11/vscreen.h"
#include "../../src/d3d11/basic_draw_observation.h"

namespace {

bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

bool readFact(const edvr::VScreenPredicateTestResult& result,
              edvr::BasicDrawObservation* fact) {
    return edvr::draw_ladder_trace::basicFactCountForTest(result.token) == 1 &&
           edvr::draw_ladder_trace::readBasicFactForTest(result.token, 0, fact);
}

bool checkRead(const edvr::BasicDrawRead<std::uintptr_t>& read,
               std::uintptr_t expected) {
    return read.reached && read.known && read.value == expected;
}

bool checkRead(const edvr::BasicDrawRead<std::uint32_t>& read,
               std::uint32_t expected) {
    return read.reached && read.known && read.value == expected;
}

bool checkRead(const edvr::BasicDrawRead<bool>& read, bool expected) {
    return read.reached && read.known && read.value == expected;
}

bool checkSkipped(const edvr::BasicDrawRead<std::uintptr_t>& read) {
    return !read.reached && !read.known && read.value == 0;
}

bool checkSkipped(const edvr::BasicDrawRead<std::uint32_t>& read) {
    return !read.reached && !read.known && read.value == 0;
}

bool checkSkipped(const edvr::BasicDrawRead<bool>& read) {
    return !read.reached && !read.known && !read.value;
}

bool armCapture(const std::wstring& path) {
    edvr::draw_ladder_trace::configure(true, path.c_str());
    if (!edvr::draw_ladder_trace::configured()) return false;
    edvr::draw_ladder_trace::armManual();
    edvr::draw_ladder_trace::frameBegin({});
    return edvr::draw_ladder_trace::capturing();
}

} // namespace

int main(int argc, char** argv) {
    bool dryRun = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0) dryRun = true;
        else if (std::strcmp(argv[i], "--self-test") != 0) {
            std::fprintf(stderr, "unsupported argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (dryRun) {
        std::puts("vscreen_predicate_test dry-run: no work performed");
        return 0;
    }
    wchar_t tempPath[MAX_PATH + 1]{};
    if (!GetTempPathW(MAX_PATH, tempPath)) {
        std::fprintf(stderr, "vscreen_predicate_test: cannot locate temp directory\n");
        return 1;
    }
    const std::wstring logPath = std::wstring(tempPath) + L"edvr_gfx_vscreen_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" +
        std::to_wstring(GetTickCount64()) + L".log";
    HANDLE logFile = CreateFileW(logPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (logFile == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "vscreen_predicate_test: cannot create trace path\n");
        return 1;
    }
    CloseHandle(logFile);

    bool okay = armCapture(logPath);
    if (!okay) {
        std::fprintf(stderr, "vscreen_predicate_test: trace capture did not arm\n");
        edvr::draw_ladder_trace::shutdown();
        DeleteFileW(logPath.c_str());
        return 1;
    }

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* immediate = nullptr;
    ID3D11DeviceContext* deferred = nullptr;
    D3D_FEATURE_LEVEL level{};
    const auto createDevice = edvr::systemD3D11CreateDevice();
    HRESULT hr = createDevice ? createDevice(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, &level, &immediate) : E_FAIL;
    okay &= check(SUCCEEDED(hr) && device && immediate,
                  "WARP supplies real immediate context for owner identity");
    if (SUCCEEDED(hr) && device && immediate) {
        okay &= check(edvr::reportSystemD3D11Only("vscreen_predicate_test"),
                      "WARP uses only System32 d3d11");
        hr = device->CreateDeferredContext(0, &deferred);
        okay &= check(SUCCEEDED(hr) && deferred,
                      "WARP supplies a real deferred context for foreign path");
    }

    if (immediate && deferred) {
        using namespace edvr::draw_ladder;
        constexpr auto contextSite = static_cast<std::uint16_t>(SiteId::kForeignContextNone);
        constexpr auto distanceSite = static_cast<std::uint16_t>(SiteId::kEyeNoDistanceNone);

        edvr::VScreenPredicateTestResult result{};
        okay &= check(edvr::vScreenPredicateTestVisit(
                          contextSite, immediate, immediate, 23, false, true, &result),
                      "trace-enabled owner-context site executes actual visitor");
        okay &= check(result.siteResult.flow == Flow::Continue &&
                          result.siteResult.outcome == SiteOutcome::Observed &&
                          result.glareClampAfter == 0,
                      "owner compare observes the draw and resets glare clamp");
        edvr::BasicDrawObservation fact{};
        okay &= check(readFact(result, &fact),
                      "owner site emits one fact through the real trace pool");
        okay &= check(fact.siteId == contextSite &&
                          fact.kind == edvr::BasicDrawFactKind::kContext &&
                          checkRead(fact.context.contextIdentity,
                                    reinterpret_cast<std::uintptr_t>(immediate)) &&
                          checkRead(fact.context.ownerContextIdentity,
                                    reinterpret_cast<std::uintptr_t>(immediate)) &&
                          checkRead(fact.context.glareClampBefore, 23) &&
                          checkRead(fact.context.glareClampAfter, 0) &&
                          checkSkipped(fact.distance.distanceEnabled),
                      "context fact contains actual identities and before/after reset reads");

        okay &= check(edvr::vScreenPredicateTestVisit(
                          contextSite, deferred, immediate, 19, false, true, &result),
                      "trace-enabled foreign-context site executes actual visitor");
        okay &= check(result.siteResult.flow == Flow::Stop &&
                          result.siteResult.outcome == SiteOutcome::Exited &&
                          result.glareClampAfter == 0,
                      "foreign context exits after the reset");
        okay &= check(readFact(result, &fact) && fact.siteId == contextSite &&
                          checkRead(fact.context.contextIdentity,
                                    reinterpret_cast<std::uintptr_t>(deferred)) &&
                          checkRead(fact.context.ownerContextIdentity,
                                    reinterpret_cast<std::uintptr_t>(immediate)) &&
                          checkRead(fact.context.glareClampBefore, 19) &&
                          checkRead(fact.context.glareClampAfter, 0),
                      "foreign fact records the real deferred and owner context identities");

        okay &= check(edvr::vScreenPredicateTestVisit(
                          distanceSite, immediate, immediate, 0, false, true, &result),
                      "trace-enabled disabled-distance site executes actual visitor");
        okay &= check(result.siteResult.flow == Flow::Stop &&
                          result.siteResult.outcome == SiteOutcome::Exited,
                      "disabled distance setting exits at the real site");
        okay &= check(readFact(result, &fact) && fact.siteId == distanceSite &&
                          fact.kind == edvr::BasicDrawFactKind::kDistance &&
                          checkRead(fact.distance.distanceEnabled, false) &&
                          checkSkipped(fact.context.contextIdentity) &&
                          checkSkipped(fact.context.ownerContextIdentity) &&
                          checkSkipped(fact.context.glareClampBefore) &&
                          checkSkipped(fact.context.glareClampAfter),
                      "distance fact contains only the reached raw gate");

        okay &= check(edvr::vScreenPredicateTestVisit(
                          distanceSite, immediate, immediate, 0, true, true, &result),
                      "trace-enabled enabled-distance site executes actual visitor");
        okay &= check(result.siteResult.flow == Flow::Continue &&
                          result.siteResult.outcome == SiteOutcome::Declined,
                      "enabled distance setting continues past the negative gate");
        okay &= check(readFact(result, &fact) &&
                          checkRead(fact.distance.distanceEnabled, true),
                      "enabled distance fact records the true raw gate");

        okay &= check(edvr::vScreenPredicateTestVisit(
                          contextSite, immediate, immediate, 31, false, false, &result),
                      "NoTrace owner specialization executes the real visitor");
        okay &= check(result.siteResult.flow == Flow::Continue &&
                          result.glareClampAfter == 0 &&
                          edvr::draw_ladder_trace::basicFactCountForTest(result.token) == 0,
                      "NoTrace preserves reset behavior and emits no cold-pool fact");

        okay &= check(edvr::vScreenPredicateTestVisit(
                          distanceSite, immediate, immediate, 0, false, false, &result),
                      "NoTrace distance specialization executes the real visitor");
        okay &= check(result.siteResult.flow == Flow::Stop &&
                          edvr::draw_ladder_trace::basicFactCountForTest(result.token) == 0,
                      "NoTrace preserves the distance exit without facts");
        okay &= check(edvr::vScreenPredicateTestVisit(
                          contextSite, deferred, immediate, 17, false, false, &result) &&
                          result.siteResult.flow == Flow::Stop &&
                          result.siteResult.outcome == SiteOutcome::Exited &&
                          result.glareClampAfter == 0 &&
                          edvr::draw_ladder_trace::basicFactCountForTest(result.token) == 0,
                      "NoTrace foreign specialization resets and exits without facts");
        okay &= check(edvr::vScreenPredicateTestVisit(
                          distanceSite, immediate, immediate, 0, true, false, &result) &&
                          result.siteResult.flow == Flow::Continue &&
                          result.siteResult.outcome == SiteOutcome::Declined &&
                          edvr::draw_ladder_trace::basicFactCountForTest(result.token) == 0,
                      "NoTrace enabled-distance specialization declines without facts");
    }

    if (deferred) deferred->Release();
    if (immediate) immediate->Release();
    if (device) device->Release();
    edvr::draw_ladder_trace::shutdown();
    DeleteFileW(logPath.c_str());
    std::puts(okay ? "vscreen_predicate_test: PASS" : "vscreen_predicate_test: FAILED");
    return okay ? 0 : 1;
}

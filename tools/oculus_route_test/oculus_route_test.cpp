#define EDVR_OCULUS_ROUTE_TEST 1

#include "../../src/d3d11/oculus_route.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

std::atomic<uint32_t> g_originalCalls{0};
std::atomic<DWORD> g_seenIncomingError{ERROR_SUCCESS};
std::atomic<uint32_t> g_afterCasCalls{0};
std::atomic<uint32_t> g_checks{0};
std::atomic<uint32_t> g_reportSinkCalls{0};
edvr::OculusRouteStatus g_lastReported{};
void** g_beforeCasSlot = nullptr;
void* g_beforeCasForeign = nullptr;
constexpr uintptr_t kTestCallerRva = 0x2345;

HMODULE WINAPI fakeLoadLibraryW(LPCWSTR) {
    g_seenIncomingError.store(GetLastError(), std::memory_order_relaxed);
    g_originalCalls.fetch_add(1, std::memory_order_relaxed);
    SetLastError(ERROR_BAD_LENGTH);
    return reinterpret_cast<HMODULE>(static_cast<uintptr_t>(0x1234));
}

void check(bool value, const char* what) {
    g_checks.fetch_add(1, std::memory_order_relaxed);
    if (!value) {
        std::printf("FAIL: %s\n", what);
        std::exit(1);
    }
}

void beforeCasCallback() {
    if (g_beforeCasSlot) *g_beforeCasSlot = g_beforeCasForeign;
}

void reportSink(const edvr::OculusRouteStatus* status) {
    g_lastReported = *status;
    g_reportSinkCalls.fetch_add(1, std::memory_order_relaxed);
}

void afterCasCallback() {
    g_afterCasCalls.fetch_add(1, std::memory_order_relaxed);
    SetLastError(ERROR_SUCCESS);
    check(edvr::oculusRouteTestDispatch(L"LibOVRRT64_1.dll", kTestCallerRva) == nullptr,
          "wrapper filters during the post-CAS pre-bookkeeping window");
    check(GetLastError() == ERROR_MOD_NOT_FOUND,
          "post-CAS rejection reports module-not-found");
}

}  // namespace

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc > 1 && std::strcmp(argv[1], "--dry-run") != 0 &&
        std::strcmp(argv[1], "--self-test") != 0) {
        std::printf("usage: oculus_route_test [--self-test|--dry-run]\n");
        return 2;
    }
    if (argc > 1 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::printf("oculus_route_test: dry-run, 0 checks, 0 failures\n");
        return 0;
    }

    edvr::oculusRouteTestReset();
    void* baselineSlot = reinterpret_cast<void*>(&fakeLoadLibraryW);
    edvr::oculusRouteTestSetProfile(&baselineSlot, 0x2345, true);
    edvr::oculusRouteInstallEarly(false);
    auto baseline = edvr::oculusRouteStatusSnapshot();
    check(baseline.nativeBuild == 0 && baseline.installed == 0 &&
              baselineSlot == reinterpret_cast<void*>(&fakeLoadLibraryW),
          "legacy capability is a no-op and does not touch the IAT");

    edvr::oculusRouteTestReset();
    edvr::oculusRouteTestSetEliteProcess(true);
    edvr::oculusRouteInstallEarly(true);
    auto unknown = edvr::oculusRouteStatusSnapshot();
    check(unknown.nativeBuild == 1 && unknown.profileKnown == 0 &&
              unknown.installed == 0 && unknown.installFailures == 1 &&
              unknown.profileFailureStage == 2,
          "unknown profile refuses installation without touching an IAT");
    check(!edvr::oculusRouteProcessAttachAllowed(),
          "identified Elite profile failure refuses process attach");

    g_reportSinkCalls.store(0, std::memory_order_relaxed);
    edvr::oculusRouteTestSetReportSink(reportSink);
    edvr::oculusRouteReport();
    check(g_reportSinkCalls.load(std::memory_order_relaxed) == 1 &&
              g_lastReported.profileKnown == 0 && g_lastReported.installed == 0 &&
              g_lastReported.installFailures == 1 &&
              g_lastReported.profileFailureStage == 2,
          "initial report records unknown profile without claiming success");

    void* slot = reinterpret_cast<void*>(&fakeLoadLibraryW);
    constexpr uintptr_t kCallerRva = kTestCallerRva;
    edvr::oculusRouteTestReset();
    edvr::oculusRouteTestSetProfile(&slot, kCallerRva, true);
    g_afterCasCalls.store(0, std::memory_order_relaxed);
    edvr::oculusRouteTestSetAfterCas(afterCasCallback);
    edvr::oculusRouteInstallEarly(true);
    auto installed = edvr::oculusRouteStatusSnapshot();
    check(installed.profileKnown == 1 && installed.installed == 1 &&
              installed.profileFailureStage == 0 &&
              installed.originalPublished == 1 && slot != reinterpret_cast<void*>(&fakeLoadLibraryW),
          "validated profile publishes the wrapper into the synthetic IAT slot");
    check(g_afterCasCalls.load(std::memory_order_relaxed) == 1,
          "post-CAS test callback ran before installation bookkeeping");

    // The exchanged slot is callable before any normal EDVR initialisation;
    // this call has a foreign return address and therefore forwards.
    auto slotCall = reinterpret_cast<edvr::OculusRouteLoadLibraryW>(slot);
    check(slotCall(L"LibOVRRT64_1.dll") ==
              reinterpret_cast<HMODULE>(static_cast<uintptr_t>(0x1234)),
          "the production wrapper is reachable through the exchanged slot");

    SetLastError(ERROR_INVALID_HANDLE);
    g_originalCalls.store(0, std::memory_order_relaxed);
    HMODULE refused = edvr::oculusRouteTestDispatch(
        L"C:\\Oculus\\LIBOVRRT64_1.DLL", kCallerRva);
    check(refused == nullptr && GetLastError() == ERROR_MOD_NOT_FOUND &&
              g_originalCalls.load(std::memory_order_relaxed) == 0,
          "exact case-insensitive basename is rejected");

    SetLastError(ERROR_INVALID_HANDLE);
    HMODULE nearResult = edvr::oculusRouteTestDispatch(
        L"C:\\Oculus\\LibOVRRT64_1.dll.bak", kCallerRva);
    check(nearResult == reinterpret_cast<HMODULE>(static_cast<uintptr_t>(0x1234)) &&
              g_seenIncomingError.load(std::memory_order_relaxed) == ERROR_INVALID_HANDLE &&
              GetLastError() == ERROR_BAD_LENGTH,
          "near name forwards and preserves incoming and original last-error semantics");

    SetLastError(ERROR_ACCESS_DENIED);
    HMODULE foreign = edvr::oculusRouteTestDispatch(L"LibOVRRT64_1.dll", kCallerRva + 1);
    check(foreign == reinterpret_cast<HMODULE>(static_cast<uintptr_t>(0x1234)) &&
              GetLastError() == ERROR_BAD_LENGTH,
          "unrecognized caller forwards the exact basename");

    wchar_t malformed[1024];
    for (wchar_t& c : malformed) c = L'x';
    SetLastError(ERROR_INVALID_DATA);
    HMODULE bad = edvr::oculusRouteTestDispatch(malformed, kCallerRva);
    check(bad == reinterpret_cast<HMODULE>(static_cast<uintptr_t>(0x1234)) &&
              GetLastError() == ERROR_BAD_LENGTH,
          "unterminated bounded path forwards without a fault");

    // Concurrent calls exercise the published original and immutable identity
    // while the wrapper is already reachable from the synthetic IAT.
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([kCallerRva] {
            for (int j = 0; j < 100; ++j) {
                edvr::oculusRouteTestDispatch(L"LibOVRRT64_1.dll", kCallerRva);
            }
        });
    }
    for (auto& thread : threads) thread.join();
    auto concurrent = edvr::oculusRouteStatusSnapshot();
    check(concurrent.rejected >= 800 && concurrent.calls >= 803,
          "concurrent wrapper calls retain all bounded counters");

    // A foreign writer between our validated read and CAS makes installation
    // fail closed and leaves that writer's slot untouched.
    edvr::oculusRouteTestReset();
    void* raceSlot = reinterpret_cast<void*>(&fakeLoadLibraryW);
    g_beforeCasSlot = &raceSlot;
    g_beforeCasForeign = reinterpret_cast<void*>(static_cast<uintptr_t>(0x789A));
    edvr::oculusRouteTestSetProfile(&raceSlot, kCallerRva, true);
    edvr::oculusRouteTestSetBeforeCas(beforeCasCallback);
    edvr::oculusRouteInstallEarly(true);
    auto raced = edvr::oculusRouteStatusSnapshot();
    check(raceSlot == g_beforeCasForeign && raced.installed == 0 &&
              raced.uninstallOwned == 0 && raced.publicationRaces == 1,
          "pre-CAS ownership race fails closed and preserves foreign slot");
    g_beforeCasSlot = nullptr;

    // A quiet report is bounded: 32 unrelated changed snapshots consume the
    // normal budget, but the first real rejection still reaches the sink.
    edvr::oculusRouteTestReset();
    edvr::oculusRouteTestPublishSlot(&slot, fakeLoadLibraryW,
                                      reinterpret_cast<uintptr_t>(&main), kCallerRva);
    g_reportSinkCalls.store(0, std::memory_order_relaxed);
    edvr::oculusRouteTestSetReportSink(reportSink);
    edvr::oculusRouteReport();
    for (int i = 0; i < 31; ++i) {
        edvr::oculusRouteTestDispatch(L"other.dll", kCallerRva + 1);
        edvr::oculusRouteReport();
    }
    const uint32_t beforeRejectReports = g_reportSinkCalls.load(std::memory_order_relaxed);
    edvr::oculusRouteTestDispatch(L"LibOVRRT64_1.dll", kCallerRva);
    edvr::oculusRouteReport();
    check(beforeRejectReports == 32 &&
              g_reportSinkCalls.load(std::memory_order_relaxed) == 33 &&
              g_lastReported.rejected == 1 && g_lastReported.reportLimitReached == 1,
          "first actual rejection is reported after unrelated report budget is full");

    edvr::oculusRouteInstallEarly(false);
    auto mismatch = edvr::oculusRouteStatusSnapshot();
    check(mismatch.capabilityMismatches == 1 && mismatch.installed == 1,
          "capability is immutable across repeated attempts");

    // A third party taking the slot after us remains the owner of that value;
    // uninstall clears our state without overwriting it.
    void* foreignTarget = reinterpret_cast<void*>(static_cast<uintptr_t>(0x5678));
    slot = foreignTarget;
    edvr::oculusRouteUninstallEarly();
    check(slot == foreignTarget, "uninstall does not overwrite a foreign owner");
    auto uninstalled = edvr::oculusRouteStatusSnapshot();
    check(uninstalled.installed == 0 && uninstalled.uninstallOwned == 0,
          "uninstall clears ownership state");

    // Restore the fixture target and prove a repeated attempt can own it
    // again, then release it normally.
    slot = reinterpret_cast<void*>(&fakeLoadLibraryW);
    edvr::oculusRouteTestSetProfile(&slot, kCallerRva, true);
    edvr::oculusRouteInstallEarly(true);
    check(slot != reinterpret_cast<void*>(&fakeLoadLibraryW),
          "repeated install reacquires the validated slot");
    edvr::oculusRouteUninstallEarly();
    check(slot == reinterpret_cast<void*>(&fakeLoadLibraryW),
          "normal uninstall restores the original target");

    std::printf("oculus_route_test: %u checks, 0 failures\n",
                g_checks.load(std::memory_order_relaxed));
    return 0;
}

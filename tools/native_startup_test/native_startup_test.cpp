#include "../../src/common/native_startup.h"
#include "../../src/d3d11/oculus_route.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {
unsigned checks = 0;
unsigned failures = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL: %s\n", message); }
}
using Query = BOOL(WINAPI*)(uint32_t, uint32_t, void*);
}

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc == 2 && !std::wcscmp(argv[1], L"--dry-run")) {
        std::puts("native_startup_test: dry-run (no DLLs, runtime, device or files)");
        return 0;
    }
    if (argc != 4 || std::wcscmp(argv[1], L"--self-test") ||
        (std::wcscmp(argv[3], L"native") && std::wcscmp(argv[3], L"legacy")) ||
        std::wcslen(argv[2]) < 4 || argv[2][1] != L':' ||
        (argv[2][2] != L'\\' && argv[2][2] != L'/')) return 2;
    const bool native = !std::wcscmp(argv[3], L"native");
    // One DLL per child; retain the imported-hook owner until process exit.
    HMODULE module = LoadLibraryExW(argv[2], nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    check(module != nullptr, "actual graphics DLL loads in an unrelated executable");
    if (!module) return 1;
    const auto* marker = reinterpret_cast<const EdvrNativeStartupRouting*>(
        GetProcAddress(module, "edvrNativeStartupRouting"));
    auto query = reinterpret_cast<Query>(GetProcAddress(module, "edvrQueryOculusRouting"));
    check(marker && query, "startup data and status query exports are present");
    if (!marker || !query) return 1;
    check(marker->size == 16 && marker->version == 1 && marker->reserved == 0,
          "immutable startup marker ABI");
    check(marker->flags == (native ? EDVR_NATIVE_STARTUP_ROUTE_OCULUS : 0u),
          "native and legacy build capability are distinct");
    MEMORY_BASIC_INFORMATION memory{};
    check(VirtualQuery(marker, &memory, sizeof(memory)) == sizeof(memory) &&
          memory.State == MEM_COMMIT && memory.Type == MEM_IMAGE &&
          memory.Protect == PAGE_READONLY, "marker is read-only mapped image data");
    edvr::OculusRouteStatus status{};
    check(query(1, sizeof(status), &status) != FALSE, "query before first D3D export");
    check(status.version == 1 && status.nativeBuild == (native ? 1u : 0u),
          "DllMain applied immutable capability");
    check(status.installAttempts == 1, "exactly one early installation attempt");
    check(status.profileKnown == 0 && status.installed == 0 && status.originalPublished == 0 &&
          status.iatSlot == 0 && status.rejected == 0,
          "unrelated executable is never patched or classified as migrated");
    check(status.installFailures == (native ? 1u : 0u),
          "unsupported profile and disabled baseline remain distinguishable");
    check(status.profileFailureStage == (native ? 2u : 0u),
          "actual DLL distinguishes unsupported PE headers from disabled routing");
    check(status.reportReady == 0, "CPU query never initializes the log or graphics");
    edvr::OculusRouteStatus sentinel{};
    std::memset(&sentinel, 0x5a, sizeof(sentinel));
    const auto original = sentinel;
    check(!query(99, sizeof(sentinel), &sentinel) &&
          !std::memcmp(&sentinel, &original, sizeof(sentinel)), "bad version leaves output untouched");
    check(!query(1, sizeof(sentinel) - 1, &sentinel) &&
          !std::memcmp(&sentinel, &original, sizeof(sentinel)), "bad size leaves output untouched");
    check(!query(1, sizeof(sentinel), nullptr), "null output rejected");
    check(!query(1, sizeof(sentinel), reinterpret_cast<void*>(1)), "unreadable output rejected");
    HMODULE kernel = LoadLibraryW(L"kernel32.dll");
    check(kernel && kernel == GetModuleHandleW(L"kernel32.dll"), "ordinary executable load still works");
    if (kernel) FreeLibrary(kernel);
    check(query(1, sizeof(status), &status) && !status.calls && !status.rejected,
          "unrelated executable load did not enter the routing wrapper");
    std::printf("native_startup_test: mode=%ls, %u checks, %u failures\n", argv[3], checks, failures);
    return failures ? 1 : 0;
}

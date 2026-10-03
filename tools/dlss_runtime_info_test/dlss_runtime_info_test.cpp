#include <windows.h>
#include <winver.h>

#include <cstdio>
#include <string>
#include <vector>
#include <cwctype>
#include <cstdarg>
#include <cstring>

namespace { std::vector<std::string> g_reportLines; }

#include "../../src/common/log.h"
#include "../../src/d3d11/dlss_runtime_info.h"

// Capture cold report text without opening a log. The production emitter and
// deduplication remain real; only the logger's file boundary is replaced.
namespace edvr {
Log& Log::get() {
    static Log instance;
    return instance;
}
Log::~Log() = default;
void Log::note(const char* format, ...) {
    char line[1200]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    g_reportLines.emplace_back(line);
}
}  // namespace edvr

namespace {

unsigned g_checks = 0;
unsigned g_failures = 0;

bool check(bool value, const char* label) {
    ++g_checks;
    if (!value) {
        ++g_failures;
        std::printf("FAIL: %s\n", label);
    }
    return value;
}

std::wstring modulePath(HMODULE module) {
    std::vector<wchar_t> path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || static_cast<std::size_t>(length) >= path.size()) return {};
    return std::wstring(path.data(), length);
}

std::wstring lowerPath(std::wstring value) {
    for (wchar_t& ch : value) {
        if (ch == L'/') ch = L'\\';
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return value;
}

std::vector<unsigned char> mappedVersionResource(HMODULE module) {
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16));
    if (!resource) return {};
    const DWORD bytes = SizeofResource(module, resource);
    const HGLOBAL loaded = LoadResource(module, resource);
    const auto* data = loaded ? static_cast<const unsigned char*>(LockResource(loaded)) : nullptr;
    return data && bytes ? std::vector<unsigned char>(data, data + bytes)
                         : std::vector<unsigned char>{};
}

std::string independentVersion(HMODULE module) {
    // Independently find the fixed-info signature in the mapped resource;
    // do not use the production root-header parser or reopen its disk path.
    const auto data = mappedVersionResource(module);
    VS_FIXEDFILEINFO fixed{};
    bool found = false;
    for (std::size_t offset = 0; offset + sizeof(fixed) <= data.size(); offset += 4) {
        std::memcpy(&fixed, data.data() + offset, sizeof(fixed));
        if (fixed.dwSignature == VS_FFI_SIGNATURE) { found = true; break; }
    }
    if (!found) return {};
    char text[64]{};
    const int count = std::snprintf(
        text, sizeof(text), "%u.%u.%u.%u",
        static_cast<unsigned>(HIWORD(fixed.dwFileVersionMS)),
        static_cast<unsigned>(LOWORD(fixed.dwFileVersionMS)),
        static_cast<unsigned>(HIWORD(fixed.dwFileVersionLS)),
        static_cast<unsigned>(LOWORD(fixed.dwFileVersionLS)));
    return count > 0 && static_cast<std::size_t>(count) < sizeof(text)
        ? std::string(text, static_cast<std::size_t>(count)) : std::string{};
}

int selfTest(const wchar_t* noVersionDll) {
    using edvr::dlss_runtime_info::inspectModule;

    const auto absent = inspectModule(nullptr);
    check(!absent.loaded && !absent.pathAvailable && !absent.versionAvailable,
          "null HMODULE is explicitly unavailable");
    const HMODULE dlssBefore = GetModuleHandleW(L"nvngx_dlss.dll");
    const auto named = edvr::dlss_runtime_info::inspectLoadedDlssModule();
    check(named.module == dlssBefore && named.loaded == (dlssBefore != nullptr) &&
              GetModuleHandleW(L"nvngx_dlss.dll") == dlssBefore,
          "named snapshot observes an existing runtime without loading an absent one");

    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    const auto actual = inspectModule(kernel);
    wchar_t systemDir[MAX_PATH]{};
    const UINT systemLength = GetSystemDirectoryW(systemDir, MAX_PATH);
    const std::wstring expectedKernel = systemLength && systemLength < MAX_PATH
        ? std::wstring(systemDir, systemLength) + L"\\kernel32.dll" : std::wstring{};
    const std::wstring actualWide = modulePath(kernel);
    check(kernel != nullptr && actual.loaded, "already-loaded kernel32 is inspected");
    check(actual.pathAvailable && !actual.pathUtf8.empty(),
          "actual module path converts to complete UTF-8");
    check(!expectedKernel.empty() && lowerPath(actualWide) == lowerPath(expectedKernel),
          "kernel32 path is its actual System32 module path");
    const std::string apiVersion = independentVersion(kernel);
    check(actual.versionAvailable && !apiVersion.empty() && actual.fileVersion == apiVersion,
          "reported version equals independent mapped-resource fixed-info evidence");

    auto bytes = mappedVersionResource(kernel);
    using edvr::dlss_runtime_info::versionResourceValidForTest;
    check(versionResourceValidForTest(bytes.data(), bytes.size()),
          "actual mapped kernel resource passes bounded parsing");
    check(!versionResourceValidForTest(nullptr, bytes.size()) &&
              !versionResourceValidForTest(bytes.data(), 5),
          "null and truncated headers are rejected");
    if (bytes.size() >= 92) {
        const auto rejectWord = [&](std::size_t offset, WORD value, const char* label) {
            auto damaged = bytes;
            std::memcpy(damaged.data() + offset, &value, sizeof(value));
            check(!versionResourceValidForTest(damaged.data(), damaged.size()), label);
        };
        WORD length = 0;
        std::memcpy(&length, bytes.data(), sizeof(length));
        check(length && !versionResourceValidForTest(bytes.data(), length - 1),
              "declared root length cannot exceed resource extent");
        rejectWord(0, 6, "root length cannot omit its terminated key");
        rejectWord(0, 40, "root length cannot omit fixed structure");
        rejectWord(2, 51, "fixed value length must cover the exact structure");
        rejectWord(4, 1, "fixed version root must be binary");
        rejectWord(6, L'X', "root key must identify VS_VERSION_INFO");
        rejectWord(36, L'X', "root key must terminate within its bounds");
        auto damaged = bytes;
        damaged[40] ^= 1;
        check(!versionResourceValidForTest(damaged.data(), damaged.size()),
              "wrong fixed-info signature is rejected");
        std::vector<unsigned char> unaligned(bytes.size() + 1);
        std::memcpy(unaligned.data() + 1, bytes.data(), bytes.size());
        check(versionResourceValidForTest(unaligned.data() + 1, bytes.size()),
              "bounded parser copies fields without unaligned dereferences");
    }
    using edvr::dlss_runtime_info::reportModuleForTest;
    reportModuleForTest(kernel);
    reportModuleForTest(kernel);
    check(g_reportLines.size() == 1 && g_reportLines.back().find(
              "path=" + actual.pathUtf8 + " fileVersion=" + actual.fileVersion) != std::string::npos,
          "actual mapped module emits the existing log shape once");
    reportModuleForTest(nullptr);
    reportModuleForTest(nullptr);
    check(g_reportLines.size() == 2 &&
              g_reportLines.back().find("module unavailable") != std::string::npos,
          "absent module is explicit and deduplicated");

    if (!check(noVersionDll && *noVersionDll, "no-version fixture path is supplied"))
        return 1;
    HMODULE fixture = LoadLibraryExW(noVersionDll, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    check(fixture != nullptr, "no-version fixture loads in self-test mode");
    if (fixture) {
        const auto noVersion = inspectModule(fixture);
        check(noVersion.loaded && noVersion.pathAvailable,
              "loaded no-version fixture path is observed");
        check(lowerPath(modulePath(fixture)) == lowerPath(noVersionDll),
              "fixture path matches GetModuleFileNameW");
        check(!noVersion.versionAvailable && noVersion.fileVersion.empty(),
              "missing VERSIONINFO is explicitly unavailable");
        reportModuleForTest(fixture);
        reportModuleForTest(fixture);
        check(g_reportLines.size() == 3 && g_reportLines.back().find(
                  "path=" + noVersion.pathUtf8 + "; actual file version unavailable") != std::string::npos,
              "actual no-resource module emits unavailable once");
        reportModuleForTest(kernel);
        check(g_reportLines.size() == 4,
              "returning to an actual prior module emits its changed state again");
        FreeLibrary(fixture);
    }
    std::printf("dlss runtime info: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--dry-run") {
        // Must remain before argument-path access or any load/file operation.
        std::puts("dlss runtime info dry-run: no files read or written; no modules loaded");
        return 0;
    }
    if (argc == 3 && std::wstring(argv[1]) == L"--self-test")
        return selfTest(argv[2]);
    std::fputs("usage: dlss_runtime_info_test.exe --dry-run | --self-test <no-version-dll>\n",
               stderr);
    return 2;
}

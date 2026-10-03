#include "dlss_runtime_info.h"

#include "../common/log.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace edvr::dlss_runtime_info {
namespace {

constexpr std::size_t kPathCapacity = 32768;
// Log::note has a 1200-byte line buffer. Refuse a path that would be cut off
// in that record rather than claim to report a complete path.
constexpr int kMaxPathUtf8Bytes = 768;

bool moduleInfoEqual(const ModuleInfo& a, const ModuleInfo& b) noexcept {
    return a.module == b.module && a.loaded == b.loaded &&
        a.pathAvailable == b.pathAvailable &&
        a.versionAvailable == b.versionAvailable &&
        a.pathUtf8 == b.pathUtf8 && a.fileVersion == b.fileVersion;
}

std::mutex g_reportMutex;
ModuleInfo g_lastReported{};
bool g_hasReported = false;

bool readFixedVersion(const void* data, std::size_t bytes,
                      VS_FIXEDFILEINFO& fixed) noexcept {
    if (!data || bytes < 6) return false;
    const auto* raw = static_cast<const unsigned char*>(data);
    WORD length = 0, valueLength = 0, type = 0;
    std::memcpy(&length, raw, 2);
    std::memcpy(&valueLength, raw + 2, 2);
    std::memcpy(&type, raw + 4, 2);
    if (length > bytes || length < 6 || type != 0 ||
        valueLength != static_cast<WORD>(sizeof(VS_FIXEDFILEINFO))) return false;
    constexpr wchar_t key[] = L"VS_VERSION_INFO";
    std::size_t offset = 6;
    for (wchar_t expected : key) {
        if (offset > length || sizeof(wchar_t) > length - offset) return false;
        wchar_t actual = 0;
        std::memcpy(&actual, raw + offset, sizeof(actual));
        if (actual != expected) return false;
        offset += sizeof(actual);
    }
    // The value starts on a DWORD boundary relative to the resource block.
    offset = (offset + 3u) & ~std::size_t{3u};
    if (offset > length || valueLength > length - offset) return false;
    std::memcpy(&fixed, raw + offset, sizeof(fixed));
    return fixed.dwSignature == VS_FFI_SIGNATURE;
}

}  // namespace

ModuleInfo inspectModule(HMODULE module) noexcept {
    ModuleInfo result{};
    result.module = module;
    result.loaded = module != nullptr;
    if (!module) return result;

    try {
        std::vector<wchar_t> widePath(kPathCapacity, L'\0');
        const DWORD length = GetModuleFileNameW(
            module, widePath.data(), static_cast<DWORD>(widePath.size()));
        if (length == 0 || static_cast<std::size_t>(length) >= widePath.size()) return result;

        const int utf8Bytes = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, widePath.data(), static_cast<int>(length),
            nullptr, 0, nullptr, nullptr);
        if (utf8Bytes > 0 && utf8Bytes <= kMaxPathUtf8Bytes) {
            std::string path(static_cast<std::size_t>(utf8Bytes), '\0');
            const int converted = WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, widePath.data(), static_cast<int>(length),
                path.data(), utf8Bytes, nullptr, nullptr);
            if (converted == utf8Bytes) {
                result.pathUtf8 = std::move(path);
                result.pathAvailable = true;
            }
        }

        const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(1),
                                              MAKEINTRESOURCEW(16)); // RT_VERSION
        if (!resource) return result;
        const DWORD resourceBytes = SizeofResource(module, resource);
        const HGLOBAL loaded = LoadResource(module, resource);
        const void* data = loaded ? LockResource(loaded) : nullptr;
        VS_FIXEDFILEINFO fixed{};
        if (!readFixedVersion(data, resourceBytes, fixed)) return result;

        char version[64]{};
        const int written = std::snprintf(
            version, sizeof(version), "%u.%u.%u.%u",
            static_cast<unsigned>(HIWORD(fixed.dwFileVersionMS)),
            static_cast<unsigned>(LOWORD(fixed.dwFileVersionMS)),
            static_cast<unsigned>(HIWORD(fixed.dwFileVersionLS)),
            static_cast<unsigned>(LOWORD(fixed.dwFileVersionLS)));
        if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(version)) return result;
        result.fileVersion.assign(version, static_cast<std::size_t>(written));
        result.versionAvailable = true;
    } catch (...) {
        // A cold diagnostic must not disturb the feature that already exists.
        result.fileVersion.clear();
        result.versionAvailable = false;
    }
    return result;
}

ModuleInfo inspectLoadedDlssModule() noexcept {
    // This increments only an existing module's reference count; it never
    // loads an absent runtime. Keep the mapped resource alive while copying.
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(0, L"nvngx_dlss.dll", &module)) return {};
    ModuleInfo result = inspectModule(module);
    FreeLibrary(module);
    return result;
}

namespace {
void reportSnapshot(const ModuleInfo& current) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_reportMutex);
        if (g_hasReported && moduleInfoEqual(g_lastReported, current)) return;
        g_lastReported = current;
        g_hasReported = true;

        if (!current.loaded) {
            Log::get().note(
                "dlss: runtime module unavailable (nvngx_dlss.dll is not already loaded); "
                "actual file version unavailable");
        } else if (!current.pathAvailable) {
            if (current.versionAvailable) {
                Log::get().note(
                    "dlss: runtime module is loaded; complete UTF-8 module path unavailable; "
                    "fileVersion=%s",
                    current.fileVersion.c_str());
            } else {
                Log::get().note(
                    "dlss: runtime module is loaded; complete UTF-8 module path unavailable; "
                    "actual file version unavailable");
            }
        } else if (!current.versionAvailable) {
            Log::get().note(
                "dlss: runtime module loaded: path=%s; actual file version unavailable",
                current.pathUtf8.c_str());
        } else {
            Log::get().note("dlss: runtime module loaded: path=%s fileVersion=%s",
                            current.pathUtf8.c_str(), current.fileVersion.c_str());
        }
    } catch (...) {
        // Diagnostics after a successful NGX creation are best effort only.
    }
}
} // namespace

void reportAfterSuccessfulFeatureCreation() noexcept {
    reportSnapshot(inspectLoadedDlssModule());
}

#if defined(EDVR_DLSS_RUNTIME_INFO_TEST)
bool versionResourceValidForTest(const void* data, std::size_t bytes) noexcept {
    VS_FIXEDFILEINFO fixed{};
    return readFixedVersion(data, bytes, fixed);
}
void reportModuleForTest(HMODULE module) noexcept {
    reportSnapshot(inspectModule(module));
}
#endif

}  // namespace edvr::dlss_runtime_info

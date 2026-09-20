#include "vscreen_auto_state.h"

#include <windows.h>

#include <cstdlib>

namespace edvr {
namespace {

// Not headset-keyed, unlike fix.openxr_resolution: this is a single "what did
// we last see" fact, not a per-headset table. Swapping headsets between
// sessions costs one extra restart to settle, which was accepted as the
// simplest workable design rather than built around.
constexpr wchar_t kAutoWidthFile[] = L"vscreen_auto_eye_width.txt";

// A render width below this is implausible enough that a corrupt or hand-
// edited state file should be ignored rather than trusted.
constexpr uint32_t kMinPlausibleWidth = 640;

std::wstring autoWidthStatePath(const std::wstring& logDir) {
    return logDir + L"\\" + kAutoWidthFile;
}

}  // namespace

bool lastKnownEyeWidth(const std::wstring& logDir, uint32_t* outWidth) {
    if (outWidth) *outWidth = 0;
    if (logDir.empty()) return false;
    HANDLE f = CreateFileW(autoWidthStatePath(logDir).c_str(), GENERIC_READ,
                           FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char buf[16] = {};
    DWORD readBytes = 0;
    const bool ok = ReadFile(f, buf, sizeof(buf) - 1, &readBytes, nullptr) != 0;
    CloseHandle(f);
    if (!ok || !readBytes) return false;
    buf[readBytes] = '\0';
    const long value = strtol(buf, nullptr, 10);
    if (value < static_cast<long>(kMinPlausibleWidth) || value > 65535) return false;
    if (outWidth) *outWidth = static_cast<uint32_t>(value);
    return true;
}

void noteResolvedEyeWidthForVScreenAuto(const std::wstring& logDir, uint32_t eyeWidth) {
    if (!eyeWidth || logDir.empty()) return;
    // Log::open() may not have created this directory: log.enabled = 0 is a
    // documented setting, and Sentinel::arm() guards against the same gap.
    CreateDirectoryW(logDir.c_str(), nullptr);
    HANDLE f = CreateFileW(autoWidthStatePath(logDir).c_str(), GENERIC_WRITE, 0,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char text[16];
    const int n = snprintf(text, sizeof(text), "%u\n", eyeWidth);
    DWORD written = 0;
    if (n > 0) WriteFile(f, text, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(f);
}

}  // namespace edvr

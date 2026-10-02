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

// The footprint's own file, beside the eye width's: a different format (key=value
// text) and a different writer (the instrument, not the OpenXR host), so a build
// that predates it never reads it and a build that has it never mistakes one for
// the other.
constexpr wchar_t kAutoFootprintFile[] = L"vscreen_auto_footprint.txt";

// The footprint is saved through a temp file BESIDE it that is then moved over it in one step: a save that fails at any point leaves
// the destination exactly as it was, where CREATE_ALWAYS on the destination itself could leave it empty or cut short.
constexpr wchar_t kAutoFootprintTempSuffix[] = L".tmp";

// A render width below this is implausible enough that a corrupt or hand-
// edited state file should be ignored rather than trusted.
constexpr uint32_t kMinPlausibleWidth = 640;

std::wstring autoWidthStatePath(const std::wstring& logDir) {
    return logDir + L"\\" + kAutoWidthFile;
}

std::wstring autoFootprintStatePath(const std::wstring& logDir) {
    return logDir + L"\\" + kAutoFootprintFile;
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

bool lastKnownPanelFootprint(const std::wstring& logDir, vscreenfit::Record* out) {
    if (out) *out = vscreenfit::Record{};
    if (logDir.empty()) return false;
    HANDLE f = CreateFileW(autoFootprintStatePath(logDir).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char buf[160] = {};
    DWORD readBytes = 0;
    const bool ok = ReadFile(f, buf, sizeof(buf) - 1, &readBytes, nullptr) != 0;
    CloseHandle(f);
    if (!ok || !readBytes) return false;
    buf[readBytes] = '\0';
    return vscreenfit::parseRecord(buf, out);
}

bool noteMeasuredPanelFootprint(const std::wstring& logDir, const vscreenfit::Record& record, uint32_t* win32Error) {
    if (win32Error) *win32Error = 0;
    auto refused = [win32Error](DWORD error) {
        if (win32Error) *win32Error = static_cast<uint32_t>(error);
        return false;
    };
    auto lastError = [] {   // never 0: a failure with no error set is still a failure
        const DWORD error = GetLastError();
        return error ? error : static_cast<DWORD>(ERROR_WRITE_FAULT);
    };
    if (logDir.empty()) return refused(ERROR_INVALID_PARAMETER);
    if (!vscreenfit::plausibleFraction(record.fractionAtUnit)) return refused(ERROR_INVALID_DATA);
    char text[160];
    const int n = vscreenfit::formatRecord(text, sizeof(text), record);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(text)) return refused(ERROR_INSUFFICIENT_BUFFER);
    CreateDirectoryW(logDir.c_str(), nullptr);   // log.enabled = 0 never made it (see above)
    const std::wstring dest = autoFootprintStatePath(logDir);
    const std::wstring tmp = dest + kAutoFootprintTempSuffix;
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return refused(lastError());
    DWORD written = 0;
    DWORD error = ERROR_SUCCESS;
    if (!WriteFile(f, text, static_cast<DWORD>(n), &written, nullptr)) error = lastError();
    else if (written != static_cast<DWORD>(n)) error = ERROR_WRITE_FAULT;   // a short write is a failed save, not a shorter record
    else if (!FlushFileBuffers(f)) error = lastError();
    CloseHandle(f);
    if (error == ERROR_SUCCESS && !MoveFileExW(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        error = lastError();   // the destination held open with no sharing, or a directory in its place: it stays as it was
    if (error != ERROR_SUCCESS) {
        DeleteFileW(tmp.c_str());   // nothing of a failed save is left behind
        return refused(error);
    }
    return true;
}

}  // namespace edvr

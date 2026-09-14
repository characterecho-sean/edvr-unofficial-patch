#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#ifndef EDVR_VERSION_STRING
#define EDVR_VERSION_STRING "unversioned-test"
#endif

namespace edvr::openxr {
// Explicitly enabled at Scene Init, never from DllMain or discovery probes.
// Keep stdout for the desktop fixtures; a GUI game's launcher need not inherit it.
class NativeTrace final {
 public:
  static NativeTrace& get() { static auto* trace = new NativeTrace; return *trace; }
  static bool makeLogPathForExecutable(const wchar_t* executablePath,
                                       const SYSTEMTIME& time, DWORD pid,
                                       std::wstring& out) {
    out.clear();
    if (!executablePath || !*executablePath || !pid) return false;
    const std::wstring path(executablePath);
    const auto slash = path.find_last_of(L"\\/");
    const bool driveAbsolute = path.size() >= 3 && path[1] == L':' &&
        (path[2] == L'\\' || path[2] == L'/');
    const bool uncAbsolute = path.size() >= 3 && path[0] == L'\\' && path[1] == L'\\';
    if (slash == std::wstring::npos || slash + 1 >= path.size() ||
        (!driveAbsolute && !uncAbsolute)) return false;
    wchar_t leaf[128]{};
    if (swprintf_s(leaf, L"edvr_openxr_%04u%02u%02u_%02u%02u%02u_%03u_%lu.log",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
        time.wMilliseconds, pid) < 0) return false;
    out = path.substr(0, slash + 1) + L"edvr_logs\\" + leaf;
    return true;
  }
  bool begin(const void* moduleAddress) {
    (void)moduleAddress; // retained for the module-init call contract
    std::lock_guard<std::mutex> lock(mutex_);
    if (attempted_) return file_ != INVALID_HANDLE_VALUE;
    attempted_ = true;
    wchar_t path[32768]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, _countof(path));
    if (!n || n >= _countof(path)) return false;
    // The filename is used by the installer to group this log with the
    // legacy local-time filenames. Body lines remain explicitly UTC.
    SYSTEMTIME time{}; GetLocalTime(&time);
    std::wstring destination;
    if (!makeLogPathForExecutable(path, time, GetCurrentProcessId(), destination)) return false;
    const auto directory = destination.substr(0, destination.find_last_of(L"\\/"));
    if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
      return false;
    file_ = CreateFileW(destination.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    return file_ != INVALID_HANDLE_VALUE;
  }
  void write(const char* message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ == INVALID_HANDLE_VALUE || bytes_ >= 8 * 1024 * 1024) return;
    SYSTEMTIME time{}; GetSystemTime(&time);
    char line[4608]{};
    const int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
        "%04u-%02u-%02u %02u:%02u:%02u.%03u UTC pid=%lu tid=%lu %s\r\n",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
        time.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId(), message);
    const DWORD length = static_cast<DWORD>(n < 0 ? strlen(line) : n);
    DWORD written = 0;
    if (WriteFile(file_, line, length, &written, nullptr)) bytes_ += written;
  }
 private:
  std::mutex mutex_;
  HANDLE file_ = INVALID_HANDLE_VALUE;
  bool attempted_ = false;
  uint64_t bytes_ = 0;
};
inline void nativeTracePrintf(const char* format, ...) {
  char message[4096]{};
  va_list arguments; va_start(arguments, format);
  _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, arguments);
  va_end(arguments);
  std::fputs(message, stdout);
  size_t n = strlen(message);
  while (n && (message[n - 1] == '\n' || message[n - 1] == '\r')) message[--n] = 0;
  NativeTrace::get().write(message);
}
inline void nativeTracePuts(const char* message) { nativeTracePrintf("%s\n", message); }
} // namespace edvr::openxr

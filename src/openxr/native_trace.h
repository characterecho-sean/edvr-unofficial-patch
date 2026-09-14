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
  bool begin(const void* moduleAddress) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (attempted_) return file_ != INVALID_HANDLE_VALUE;
    attempted_ = true;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(moduleAddress), &module)) return false;
    wchar_t path[32768]{};
    const DWORD n = GetModuleFileNameW(module, path, _countof(path));
    if (!n || n >= _countof(path)) return false;
    const auto slash = std::wstring(path).find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    SYSTEMTIME time{}; GetSystemTime(&time);
    wchar_t leaf[128]{};
    swprintf_s(leaf, L"edvr_openxr_%04u%02u%02u_%02u%02u%02u_%03u_%lu.log",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
        time.wMilliseconds, GetCurrentProcessId());
    const auto destination = std::wstring(path, slash + 1) + leaf;
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

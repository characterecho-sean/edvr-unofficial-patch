#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <utility>

namespace edvr::openxr {

struct BootstrapPaths {
  std::wstring loader;
  std::wstring graphics;
  bool separateDevice = false;
};

enum class BootstrapResult { Unconfigured, Ready, Invalid };

namespace detail {

constexpr size_t kBootstrapMaxPathChars = 32767;

enum class BootstrapEnvironmentResult { Missing, Present, Failed };

inline bool bootstrapDriveAbsolute(const std::wstring& value) noexcept {
  if (value.size() <= 3 || value.size() > kBootstrapMaxPathChars ||
      value.find(L'\0') != std::wstring::npos ||
      value[1] != L':' || (value[2] != L'\\' && value[2] != L'/')) return false;
  const wchar_t drive = value[0];
  return (drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z');
}

template<class Reader>
BootstrapEnvironmentResult readBootstrapEnvironment(Reader& reader, const wchar_t* name,
                                                    std::wstring& value) noexcept {
  value.clear();
  wchar_t probe[1] = {};
  SetLastError(ERROR_SUCCESS);
  const DWORD required = reader(name, probe, 1);
  const DWORD queryError = GetLastError();

  // A zero result with ERROR_ENVVAR_NOT_FOUND is the only missing case. A
  // zero result with another error is a failed read; ERROR_SUCCESS means an
  // existing empty variable, which is present but invalid to the collector.
  if (!required && queryError == ERROR_ENVVAR_NOT_FOUND) {
    return BootstrapEnvironmentResult::Missing;
  }
  if (!required && queryError != ERROR_SUCCESS) {
    return BootstrapEnvironmentResult::Failed;
  }
  if (queryError != ERROR_SUCCESS) {
    return BootstrapEnvironmentResult::Failed;
  }
  if (required > kBootstrapMaxPathChars + 1) {
    return BootstrapEnvironmentResult::Failed;
  }

  wchar_t buffer[kBootstrapMaxPathChars + 1] = {};
  SetLastError(ERROR_SUCCESS);
  const DWORD length = reader(name, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
  const DWORD readError = GetLastError();
  if (readError != ERROR_SUCCESS || !length || length > kBootstrapMaxPathChars ||
      length + 1 != required) {
    return BootstrapEnvironmentResult::Failed;
  }

  try {
    value.assign(buffer, length);
  } catch (...) {
    value.clear();
    return BootstrapEnvironmentResult::Failed;
  }
  if (value.size() != length || value.find(L'\0') != std::wstring::npos) {
    value.clear();
    return BootstrapEnvironmentResult::Failed;
  }
  return BootstrapEnvironmentResult::Present;
}

template<class Reader>
BootstrapResult collectBootstrapPaths(BootstrapPaths& output, Reader& reader) noexcept {
  output.loader.clear();
  output.graphics.clear();
  output.separateDevice = false;
  std::wstring loader;
  std::wstring graphics;
  std::wstring separate;
  const auto loaderResult = readBootstrapEnvironment(reader, L"EDVR_OPENXR_LOADER", loader);
  const auto graphicsResult = readBootstrapEnvironment(reader, L"EDVR_OPENXR_GRAPHICS", graphics);
  const auto separateResult = readBootstrapEnvironment(reader, L"EDVR_OPENXR_SEPARATE_DEVICE", separate);

  if (loaderResult == BootstrapEnvironmentResult::Missing &&
      graphicsResult == BootstrapEnvironmentResult::Missing &&
      separateResult == BootstrapEnvironmentResult::Missing) {
    return BootstrapResult::Unconfigured;
  }
  if (loaderResult != BootstrapEnvironmentResult::Present ||
      graphicsResult != BootstrapEnvironmentResult::Present ||
      (separateResult == BootstrapEnvironmentResult::Present && separate != L"1") ||
      separateResult == BootstrapEnvironmentResult::Failed ||
      !bootstrapDriveAbsolute(loader) || !bootstrapDriveAbsolute(graphics)) {
    return BootstrapResult::Invalid;
  }
  try {
    output.loader = std::move(loader);
    output.graphics = std::move(graphics);
    output.separateDevice = separateResult == BootstrapEnvironmentResult::Present;
  } catch (...) {
    output.loader.clear();
    output.graphics.clear();
    return BootstrapResult::Invalid;
  }
  return BootstrapResult::Ready;
}

struct ProcessEnvironmentReader {
  DWORD operator()(const wchar_t* name, wchar_t* buffer, DWORD capacity) const noexcept {
    return GetEnvironmentVariableW(name, buffer, capacity);
  }
};

}  // namespace detail

inline BootstrapResult readBootstrapPaths(BootstrapPaths& output) noexcept {
  detail::ProcessEnvironmentReader reader;
  return detail::collectBootstrapPaths(output, reader);
}

}  // namespace edvr::openxr

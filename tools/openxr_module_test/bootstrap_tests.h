#pragma once

#include "../../src/openxr/native_bootstrap.h"

#include <cwchar>
#include <string>

namespace edvr::openxr::bootstrap_test {

struct EnvironmentSpec {
  enum class Shape { Normal, ShortFirst, Resize, Oversized };
  enum class Failure { None, FirstRead, SecondRead };
  bool missing = false;
  std::wstring value;
  std::wstring resizedValue;
  Shape shape = Shape::Normal;
  Failure failure = Failure::None;
  unsigned calls = 0;
};

struct EnvironmentReader {
  EnvironmentSpec loader;
  EnvironmentSpec graphics;

  DWORD operator()(const wchar_t* name, wchar_t* buffer, DWORD capacity) noexcept {
    EnvironmentSpec* spec = nullptr;
    if (!std::wcscmp(name, L"EDVR_OPENXR_LOADER")) spec = &loader;
    if (!std::wcscmp(name, L"EDVR_OPENXR_GRAPHICS")) spec = &graphics;
    if (!spec) {
      SetLastError(ERROR_ENVVAR_NOT_FOUND);
      return 0;
    }
    ++spec->calls;
    if (spec->missing) {
      SetLastError(ERROR_ENVVAR_NOT_FOUND);
      return 0;
    }
    if ((spec->failure == EnvironmentSpec::Failure::FirstRead && spec->calls == 1) ||
        (spec->failure == EnvironmentSpec::Failure::SecondRead && spec->calls == 2)) {
      SetLastError(ERROR_ACCESS_DENIED);
      return 0;
    }
    if (spec->shape == EnvironmentSpec::Shape::ShortFirst && spec->calls == 1) {
      SetLastError(ERROR_SUCCESS);
      return 1;
    }
    if (spec->shape == EnvironmentSpec::Shape::Oversized && spec->calls == 1) {
      SetLastError(ERROR_SUCCESS);
      return static_cast<DWORD>(detail::kBootstrapMaxPathChars + 2);
    }

    const std::wstring& value = (spec->shape == EnvironmentSpec::Shape::Resize && spec->calls >= 2 &&
                                 !spec->resizedValue.empty())
                                    ? spec->resizedValue
                                    : spec->value;
    const size_t length = value.size();
    if (length >= capacity) {
      SetLastError(ERROR_SUCCESS);
      return static_cast<DWORD>(length + 1);
    }
    if (buffer) {
      for (size_t i = 0; i < length; ++i) buffer[i] = value[i];
      buffer[length] = L'\0';
    }
    SetLastError(ERROR_SUCCESS);
    return static_cast<DWORD>(length);
  }
};

template<class Check>
void runBootstrapTests(Check&& check) {
  const std::wstring loader = L"C:\\Program Files\\EDVR\\openxr_loader.dll";
  const std::wstring graphics = L"d:/EDVR/d3d11.dll";

  {
    EnvironmentReader reader;
    reader.loader.missing = true;
    reader.graphics.missing = true;
    BootstrapPaths paths{{L"stale-loader"}, {L"stale-graphics"}};
    check(detail::collectBootstrapPaths(paths, reader) == BootstrapResult::Unconfigured &&
              paths.loader.empty() && paths.graphics.empty(),
          "both bootstrap variables absent are unconfigured");
  }
  {
    EnvironmentReader reader;
    reader.loader.value = loader;
    reader.graphics.missing = true;
    BootstrapPaths paths{{L"stale-loader"}, {L"stale-graphics"}};
    check(detail::collectBootstrapPaths(paths, reader) == BootstrapResult::Invalid &&
              paths.loader.empty() && paths.graphics.empty(),
          "one missing bootstrap variable is invalid and clears output");
  }
  {
    EnvironmentReader reader;
    reader.loader.missing = true;
    reader.graphics.value = graphics;
    BootstrapPaths paths;
    check(detail::collectBootstrapPaths(paths, reader) == BootstrapResult::Invalid,
          "missing loader with present graphics is invalid");
  }
  {
    EnvironmentReader reader;
    reader.loader.value = loader;
    reader.graphics.value = graphics;
    BootstrapPaths paths;
    check(detail::collectBootstrapPaths(paths, reader) == BootstrapResult::Ready &&
              paths.loader == loader && paths.graphics == graphics,
          "valid drive absolute paths are copied without normalization");
  }
  {
    EnvironmentReader reader;
    reader.loader.value = L"C:relative-loader.dll";
    reader.graphics.value = graphics;
    BootstrapPaths paths;
    check(detail::collectBootstrapPaths(paths, reader) == BootstrapResult::Invalid &&
              paths.loader.empty() && paths.graphics.empty(),
          "drive relative loader is rejected");
  }
  {
    EnvironmentReader reader;
    reader.loader.value = L"\\\\server\\share\\loader.dll";
    reader.graphics.value = graphics;
    BootstrapPaths paths;
    check(detail::collectBootstrapPaths(paths, reader) == BootstrapResult::Invalid,
          "UNC loader is rejected");
  }
  {
    EnvironmentReader reader;
    reader.loader.value = L"1:\\loader.dll";
    reader.graphics.value = graphics;
    BootstrapPaths paths;
    check(detail::collectBootstrapPaths(paths, reader) == BootstrapResult::Invalid,
          "non alphabetic drive is rejected");
  }
  {
    EnvironmentReader reader;
    reader.loader.value.assign(L"C:\\loader\0.dll", 14);
    reader.graphics.value = graphics;
    BootstrapPaths paths;
    check(detail::collectBootstrapPaths(paths, reader) == BootstrapResult::Invalid,
          "embedded NUL is rejected");
  }
  {
    EnvironmentReader reader;
    reader.loader.value.clear();
    reader.graphics.value = graphics;
    BootstrapPaths paths;
    check(detail::collectBootstrapPaths(paths, reader) == BootstrapResult::Invalid,
          "empty loader is rejected");
  }
  {
    EnvironmentReader reader;
    reader.loader.value = loader;
    reader.graphics.value = graphics;
    reader.loader.shape = EnvironmentSpec::Shape::ShortFirst;
    BootstrapPaths paths;
    check(detail::collectBootstrapPaths(paths, reader) == BootstrapResult::Invalid,
          "short initial environment read is rejected");
  }
  {
    EnvironmentReader reader;
    reader.loader.value = loader;
    reader.loader.resizedValue = L"C:\\different-loader.dll";
    reader.graphics.value = graphics;
    reader.loader.shape = EnvironmentSpec::Shape::Resize;
    BootstrapPaths paths;
    check(detail::collectBootstrapPaths(paths, reader) == BootstrapResult::Invalid,
          "environment size change between reads is rejected");
  }
  {
    EnvironmentReader reader;
    reader.loader.value = L"C:\\loader.dll";
    reader.loader.resizedValue = L"C:\\a-much-longer-loader-path.dll";
    reader.graphics.value = graphics;
    reader.loader.shape = EnvironmentSpec::Shape::Resize;
    BootstrapPaths paths;
    check(detail::collectBootstrapPaths(paths, reader) == BootstrapResult::Invalid,
          "environment growth between reads is rejected");
  }
  {
    EnvironmentReader reader;
    reader.loader.value = L"C:\\";
    reader.loader.value.append(detail::kBootstrapMaxPathChars - reader.loader.value.size(), L'a');
    reader.graphics.value = graphics;
    BootstrapPaths paths;
    check(detail::collectBootstrapPaths(paths, reader) == BootstrapResult::Ready &&
              paths.loader.size() == detail::kBootstrapMaxPathChars,
          "maximum length bootstrap path is accepted");
  }
  {
    EnvironmentReader reader;
    reader.loader.value = L"C:\\";
    reader.loader.value.append(detail::kBootstrapMaxPathChars - reader.loader.value.size() + 1, L'a');
    reader.graphics.value = graphics;
    BootstrapPaths paths;
    check(detail::collectBootstrapPaths(paths, reader) == BootstrapResult::Invalid &&
              reader.loader.calls == 1,
          "one over maximum bootstrap path is rejected before the second read");
  }
  {
    EnvironmentReader reader;
    reader.loader.value = loader;
    reader.graphics.value = graphics;
    reader.loader.shape = EnvironmentSpec::Shape::Oversized;
    BootstrapPaths paths;
    check(detail::collectBootstrapPaths(paths, reader) == BootstrapResult::Invalid &&
              reader.loader.calls == 1,
          "oversized environment value is bounded before the second read");
  }
  {
    EnvironmentReader reader;
    reader.loader.value = loader;
    reader.graphics.value = graphics;
    reader.loader.failure = EnvironmentSpec::Failure::FirstRead;
    BootstrapPaths paths;
    check(detail::collectBootstrapPaths(paths, reader) == BootstrapResult::Invalid,
          "initial environment read failure is rejected");
  }
  {
    EnvironmentReader reader;
    reader.loader.value = loader;
    reader.graphics.value = graphics;
    reader.graphics.failure = EnvironmentSpec::Failure::SecondRead;
    BootstrapPaths paths;
    check(detail::collectBootstrapPaths(paths, reader) == BootstrapResult::Invalid,
          "second environment read failure is rejected");
  }
}

}  // namespace edvr::openxr::bootstrap_test

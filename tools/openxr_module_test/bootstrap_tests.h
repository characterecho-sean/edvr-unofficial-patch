#pragma once

#include "../../src/openxr/native_bootstrap.h"
#include "../../src/openxr/native_local_config.h"

#include <cwchar>
#include <string>

namespace edvr::openxr::bootstrap_test {

inline std::wstring localFixturePath() {
  wchar_t directory[MAX_PATH]{};GetTempPathW(_countof(directory),directory);
  wchar_t path[MAX_PATH]{};GetTempFileNameW(directory,L"edv",0,path);DeleteFileW(path);return path;
}
inline bool writeLocalFixture(const std::wstring& path,const std::string& bytes) {
  HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file==INVALID_HANDLE_VALUE)return false;DWORD written=0;const bool ok=bytes.empty()||WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)!=FALSE;
  CloseHandle(file);return ok&&written==bytes.size();
}

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
  EnvironmentSpec separate{true};

  DWORD operator()(const wchar_t* name, wchar_t* buffer, DWORD capacity) noexcept {
    EnvironmentSpec* spec = nullptr;
    if (!std::wcscmp(name, L"EDVR_OPENXR_LOADER")) spec = &loader;
    if (!std::wcscmp(name, L"EDVR_OPENXR_GRAPHICS")) spec = &graphics;
    if (!std::wcscmp(name, L"EDVR_OPENXR_SEPARATE_DEVICE")) spec = &separate;
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

  const std::wstring localPath=localFixturePath();
  const std::string valid="[openxr]\nversion=1\nloader=C:\\loader.dll\ngraphics=D:\\d3d11.dll\nruntime=E:\\runtime.json\nseparate_device=1\n";
  {
    LocalConfig config;check(writeLocalFixture(localPath,valid)&&readLocalConfig(localPath,config)==LocalConfigResult::Ready&&
      config.loader==L"C:\\loader.dll"&&config.graphics==L"D:\\d3d11.dll"&&config.runtime==L"E:\\runtime.json"&&config.separateDevice,
      "valid local OpenXR config parses strictly");
  }
  for(const std::string& malformed:{
      "[openxr]\nversion=1\nloader=C:\\loader.dll\ngraphics=D:\\d3d11.dll\nruntime=E:\\runtime.json\n",
      "[openxr]\nversion=1\nversion=1\nloader=C:\\loader.dll\ngraphics=D:\\d3d11.dll\nruntime=E:\\runtime.json\nseparate_device=1\n",
      "[openxr]\nversion=1\nloader=relative.dll\ngraphics=D:\\d3d11.dll\nruntime=E:\\runtime.json\nseparate_device=1\n",
      "[openxr]\nversion=1\nloader=C:\\loader.dll\ngraphics=D:\\d3d11.dll\nruntime=E:\\runtime.json\nseparate_device=0\n",
      "[openxr]\nversion=1\nloader=C:\\loader.dll\ngraphics=D:\\d3d11.dll\nruntime=E:\\runtime.json\nseparate_device=1\nunknown=x\n"}) {
    LocalConfig config{{L"stale"},{L"stale"},{L"stale"},true};check(writeLocalFixture(localPath,malformed)&&readLocalConfig(localPath,config)==LocalConfigResult::Invalid&&
      config.loader.empty()&&config.graphics.empty()&&config.runtime.empty()&&!config.separateDevice,
      "malformed local OpenXR config fails closed");
  }
  {
    std::string invalid="[openxr]\nversion=1\nloader=C:\\loader.dll\ngraphics=D:\\d3d11.dll\nruntime=E:\\runtime.json\nseparate_device=1\n";
    invalid[0]=char(0xc3);invalid[1]=char(0x28);LocalConfig config;
    check(writeLocalFixture(localPath,invalid)&&readLocalConfig(localPath,config)==LocalConfigResult::Invalid,
      "invalid UTF-8 local config fails closed");
  }
  {
    const std::string equalPath="[openxr]\nversion=1\nloader=C:\\u=loader.dll\ngraphics=D:\\d3d11.dll\nruntime=E:\\runtime.json\nseparate_device=1\n";
    LocalConfig config;check(writeLocalFixture(localPath,equalPath)&&readLocalConfig(localPath,config)==LocalConfigResult::Ready&&
      config.loader==L"C:\\u=loader.dll", "local config preserves equals in path values");
  }
  {
    std::string oversized(65537,'x');LocalConfig config{{L"stale"},{L"stale"},{L"stale"},true};
    check(writeLocalFixture(localPath,oversized)&&readLocalConfig(localPath,config)==LocalConfigResult::Invalid&&
      config.loader.empty()&&config.graphics.empty()&&config.runtime.empty()&&!config.separateDevice,
      "oversized local config fails closed with empty output");
  }
  {
    const std::wstring runtimePath=localPath+L".runtime";check(writeLocalFixture(runtimePath,"runtime"),"create manifest guard fixture");
    SetEnvironmentVariableW(L"XR_RUNTIME_JSON",nullptr);ScopedRuntimeManifest guard;
    check(guard.apply(runtimePath),"local runtime manifest override applies");
    wchar_t value[32768]{};const DWORD n=GetEnvironmentVariableW(L"XR_RUNTIME_JSON",value,_countof(value));
    check(n==runtimePath.size()&&!std::wstring(value,n).compare(runtimePath),"manifest override is visible in process only");
    guard.restore();SetLastError(ERROR_SUCCESS);GetEnvironmentVariableW(L"XR_RUNTIME_JSON",value,_countof(value));
    check(GetLastError()==ERROR_ENVVAR_NOT_FOUND,"manifest override restores absent inherited value");
    SetEnvironmentVariableW(L"XR_RUNTIME_JSON",L"relative.json");ScopedRuntimeManifest invalid;
    check(!invalid.apply(runtimePath),"invalid inherited runtime manifest is rejected");
    SetEnvironmentVariableW(L"XR_RUNTIME_JSON",runtimePath.c_str());ScopedRuntimeManifest changed;
    check(changed.apply(runtimePath),"manifest override accepts inherited readable value");
    const std::wstring other=runtimePath+L".other";writeLocalFixture(other,"other");SetEnvironmentVariableW(L"XR_RUNTIME_JSON",other.c_str());changed.restore();
    check(GetEnvironmentVariableW(L"XR_RUNTIME_JSON",value,_countof(value))==other.size()&&!std::wstring(value,other.size()).compare(other),
      "manifest restore does not clobber an external environment change");
    SetEnvironmentVariableW(L"XR_RUNTIME_JSON",nullptr);DeleteFileW(runtimePath.c_str());DeleteFileW(other.c_str());
  }
  DeleteFileW(localPath.c_str());

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
    reader.loader.value = loader; reader.graphics.value = graphics;
    BootstrapPaths paths;
    check(detail::collectBootstrapPaths(paths, reader) == BootstrapResult::Ready && !paths.separateDevice,
          "missing separate-device flag preserves v1 bootstrap");
  }
  for (const wchar_t* flag : {L"", L"0", L"2", L"true", L" 1"}) {
    EnvironmentReader reader;
    reader.loader.value = loader; reader.graphics.value = graphics;
    reader.separate.missing = false; reader.separate.value = flag;
    BootstrapPaths paths{{L"stale-loader"},{L"stale-graphics"},true};
    check(detail::collectBootstrapPaths(paths, reader) == BootstrapResult::Invalid &&
          paths.loader.empty() && paths.graphics.empty() && !paths.separateDevice,
          "invalid separate-device flag clears bootstrap output");
  }
  {
    EnvironmentReader reader;
    reader.loader.value = loader; reader.graphics.value = graphics; reader.separate.missing = false; reader.separate.value = L"1";
    BootstrapPaths paths;
    check(detail::collectBootstrapPaths(paths, reader) == BootstrapResult::Ready && paths.separateDevice,
          "exact separate-device flag selects v2 bootstrap");
  }
  {
    EnvironmentReader reader;
    reader.loader.missing = reader.graphics.missing = true;
    reader.separate.missing = false; reader.separate.value = L"1";
    BootstrapPaths paths{{L"stale-loader"}, {L"stale-graphics"}, true};
    check(detail::collectBootstrapPaths(paths, reader) == BootstrapResult::Invalid &&
          paths.loader.empty() && paths.graphics.empty() && !paths.separateDevice,
          "separate-device flag without both paths is invalid");
  }
  {
    EnvironmentReader reader;
    reader.loader.value = loader; reader.graphics.value = graphics;
    reader.separate.missing = false; reader.separate.value = L"1";
    reader.separate.failure = EnvironmentSpec::Failure::SecondRead;
    BootstrapPaths paths;
    check(detail::collectBootstrapPaths(paths, reader) == BootstrapResult::Invalid &&
          paths.loader.empty() && paths.graphics.empty() && !paths.separateDevice,
          "failed separate-device environment read is invalid");
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

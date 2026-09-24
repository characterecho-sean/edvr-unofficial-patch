#pragma once

#include <windows.h>
#include <string>

namespace edvr::openxr {

enum class LocalConfigResult { Absent, Ready, Invalid };

struct LocalConfig {
  std::wstring loader, graphics, runtime;
  bool separateDevice=false;
  // Both optional; absent means the default (high priority, overlap off).
  bool frameThreadPriorityHigh=true;
  bool frameEndOverlap=false;
};

inline bool localConfigUsesSystemRuntime(const std::wstring& value) noexcept {
  return value == L"system";
}

inline bool localConfigAbsolute(const std::wstring& value) noexcept {
  return value.size()>3&&value.size()<32768&&value.find(L'\0')==std::wstring::npos&&
    ((value[0]>='A'&&value[0]<='Z')||(value[0]>='a'&&value[0]<='z'))&&value[1]==L':'&&
    (value[2]==L'\\'||value[2]==L'/');
}

inline bool localConfigReadFile(const std::wstring& path,std::string& bytes) noexcept {
  HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
      nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file==INVALID_HANDLE_VALUE)return false;
  LARGE_INTEGER size{};const bool sized=GetFileSizeEx(file,&size)!=FALSE&&size.QuadPart>=0&&size.QuadPart<=65536;
  if(!sized){CloseHandle(file);return false;}
  bytes.assign(static_cast<size_t>(size.QuadPart),'\0');DWORD read=0;
  const bool readOk=bytes.empty()||ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)!=FALSE;
  CloseHandle(file);if(!readOk||read!=bytes.size())return false;return true;
}

inline LocalConfigResult readLocalConfig(const std::wstring& path,LocalConfig& output) noexcept {
  output={};
  LocalConfig candidate;std::string bytes;
  const DWORD attributes=GetFileAttributesW(path.c_str());
  if(attributes==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND)return LocalConfigResult::Absent;
  if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY))return LocalConfigResult::Invalid;
  if(!localConfigReadFile(path,bytes))return LocalConfigResult::Invalid;
  if(bytes.find('\0')!=std::string::npos)return LocalConfigResult::Invalid;
  int needed=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),static_cast<int>(bytes.size()),nullptr,0);
  if(needed<=0)return LocalConfigResult::Invalid;
  std::wstring text(static_cast<size_t>(needed),L'\0');
  if(needed&&!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),static_cast<int>(bytes.size()),text.data(),needed))return LocalConfigResult::Invalid;
  bool section=false,version=false,loader=false,graphics=false,runtime=false,separate=false;
  bool priority=false,overlap=false; // optional keys: absence is not failure
  size_t pos=0;
  while(pos<=text.size()) {
    const size_t end=text.find_first_of(L"\r\n",pos);std::wstring line=text.substr(pos,end==std::wstring::npos?text.size()-pos:end-pos);
    if(end!=std::wstring::npos){pos=end+1;if(pos<text.size()&&text[end]==L'\r'&&text[pos]==L'\n')++pos;}else pos=text.size()+1;
    size_t first=line.find_first_not_of(L" \t"),last=line.find_last_not_of(L" \t");
    if(first==std::wstring::npos)continue;line=line.substr(first,last-first+1);
    if(line==L"[openxr]"){if(section)return LocalConfigResult::Invalid;section=true;continue;}
    if(!section)return LocalConfigResult::Invalid;
    const size_t equal=line.find(L'=');if(equal==std::wstring::npos)return LocalConfigResult::Invalid;
    std::wstring key=line.substr(0,equal),value=line.substr(equal+1);first=key.find_first_not_of(L" \t");last=key.find_last_not_of(L" \t");
    if(first==std::wstring::npos)return LocalConfigResult::Invalid;key=key.substr(first,last-first+1);
    first=value.find_first_not_of(L" \t");last=value.find_last_not_of(L" \t");if(first==std::wstring::npos)return LocalConfigResult::Invalid;value=value.substr(first,last-first+1);
    bool* seen=nullptr;std::wstring* target=nullptr;
    if(key==L"version"){if(version||value!=L"1")return LocalConfigResult::Invalid;version=true;continue;}
    if(key==L"loader"){seen=&loader;target=&candidate.loader;}
    else if(key==L"graphics"){seen=&graphics;target=&candidate.graphics;}
    else if(key==L"runtime"){seen=&runtime;target=&candidate.runtime;}
    else if(key==L"separate_device"){if(separate||value!=L"1")return LocalConfigResult::Invalid;separate=true;candidate.separateDevice=true;continue;}
    else if(key==L"frame_thread_priority"){
      if(priority||(value!=L"high"&&value!=L"normal"))return LocalConfigResult::Invalid;
      priority=true;candidate.frameThreadPriorityHigh=value==L"high";continue;
    }
    else if(key==L"frame_end_overlap"){
      if(overlap||(value!=L"on"&&value!=L"off"))return LocalConfigResult::Invalid;
      overlap=true;candidate.frameEndOverlap=value==L"on";continue;
    }
    else return LocalConfigResult::Invalid;
    if(*seen||((key!=L"runtime"||!localConfigUsesSystemRuntime(value))&&!localConfigAbsolute(value)))return LocalConfigResult::Invalid;*seen=true;*target=std::move(value);
  }
  if(!section||!version||!loader||!graphics||!runtime||!separate)return LocalConfigResult::Invalid;
  output=std::move(candidate);return LocalConfigResult::Ready;
}

class ScopedRuntimeManifest final {
 public:
  bool apply(const std::wstring& local) noexcept {
    if(local.empty())return true;
    if(!applied_.empty()||systemCleared_)return false;
    wchar_t buffer[32768]{};SetLastError(ERROR_SUCCESS);const DWORD n=GetEnvironmentVariableW(L"XR_RUNTIME_JSON",buffer,_countof(buffer));
    const DWORD error=GetLastError();
    if(n>=_countof(buffer)||(error!=ERROR_SUCCESS&&error!=ERROR_ENVVAR_NOT_FOUND))return false;
    if(localConfigUsesSystemRuntime(local)) {
      // Follow the Windows runtime even if a launcher inherited a diagnostic
      // manifest override. This affects only Elite's process and is restored.
      if(error==ERROR_ENVVAR_NOT_FOUND)return true;
      previous_.assign(buffer,n);
      if(!SetEnvironmentVariableW(L"XR_RUNTIME_JSON",nullptr)){previous_.clear();return false;}
      systemCleared_=true;return true;
    }
    if(n||error==ERROR_SUCCESS)return n&&localConfigAbsolute(std::wstring(buffer,n))&&regular(buffer,n);
    if(!localConfigAbsolute(local)||!regular(local))return false;
    applied_=local;
    if(!SetEnvironmentVariableW(L"XR_RUNTIME_JSON",local.c_str())){applied_.clear();return false;}
    return true;
  }
  void restore() noexcept {
    if(systemCleared_) {
      wchar_t probe[1]{};SetLastError(ERROR_SUCCESS);
      const DWORD n=GetEnvironmentVariableW(L"XR_RUNTIME_JSON",probe,_countof(probe));
      if(!n&&GetLastError()==ERROR_ENVVAR_NOT_FOUND)SetEnvironmentVariableW(L"XR_RUNTIME_JSON",previous_.c_str());
      systemCleared_=false;previous_.clear();
    }
    if(applied_.empty())return;
    wchar_t buffer[32768]{};const DWORD n=GetEnvironmentVariableW(L"XR_RUNTIME_JSON",buffer,_countof(buffer));
    if(n==applied_.size()&&!std::wstring(buffer,n).compare(applied_))SetEnvironmentVariableW(L"XR_RUNTIME_JSON",nullptr);
    applied_.clear();
  }
 private:
  static bool regular(const wchar_t* path,size_t length) noexcept {return regular(std::wstring(path,length));}
  static bool regular(const std::wstring& path) noexcept {
    const DWORD a=GetFileAttributesW(path.c_str());if(a==INVALID_FILE_ATTRIBUTES||(a&FILE_ATTRIBUTE_DIRECTORY))return false;
    const HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;CloseHandle(file);return true;
  }
  std::wstring applied_;
  std::wstring previous_;
  bool systemCleared_=false;
};
}

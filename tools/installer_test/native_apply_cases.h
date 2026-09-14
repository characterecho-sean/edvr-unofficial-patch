#pragma once

#include <windows.h>
#include <functional>
#include <string>
#include <cstring>

#include "../../src/installer/apply.h"

namespace edvr::installer::native_apply_cases {

inline bool writeFile(const std::wstring& path, const char* text) {
  HANDLE h=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(h==INVALID_HANDLE_VALUE)return false;DWORD n=0;const DWORD size=(DWORD)strlen(text);
  const bool ok=WriteFile(h,text,size,&n,nullptr)!=FALSE&&n==size;CloseHandle(h);return ok;
}

template<class Check>
void run(Check check,const std::wstring& root) {
  CreateDirectoryW(root.c_str(),nullptr);
  const auto d3d11=root+L"\\d3d11.dll", runtime=root+L"\\openvr_api.dll", ini=root+L"\\edvr.ini";
  const auto blocker=root+L"\\blocked", backup=root+L"\\edvr_backup";
  check(writeFile(d3d11,"OLD-GRAPHICS"),"native rollback fixture writes old graphics");
  check(writeFile(runtime,"OLD-RUNTIME"),"native rollback fixture writes old runtime");
  check(writeFile(ini,"OLD-CONFIG"),"native rollback fixture writes old config");
  check(writeFile(blocker,"not-a-directory"),"native rollback fixture creates failure blocker");
  Plan plan;plan.backupDir=backup;
  plan.steps={
    {Action::WritePayload,L"",d3d11,"d3d11","",{},{},false},
    {Action::WritePayload,L"",runtime,"openvr","",{},{},false},
    {Action::WriteText,L"",ini,"","NEW-CONFIG",{},{},false},
    {Action::WriteText,L"",blocker+L"\\edvr.ini","","NEW-CONFIG",{},{},false},
  };
  const PayloadProvider payload=[](const std::string& item,const void** data,size_t* size) {
    static const char graphics[]="NEW-GRAPHICS",runtime[]="NEW-RUNTIME";
    if(item=="d3d11"){*data=graphics;*size=sizeof(graphics)-1;return true;}
    if(item=="openvr"){*data=runtime;*size=sizeof(runtime)-1;return true;}
    return false;
  };
  const ApplyResult result=applyPlan(plan,payload);
  check(!result.ok,"native pair write failure is reported");
  check(result.rolledBack,"native pair write failure rolls back");
  check(result.overwrote,"rollback reports that existing pair files were overwritten");
  check(readTextFile(d3d11)=="OLD-GRAPHICS","graphics bytes restored after later failure");
  check(readTextFile(runtime)=="OLD-RUNTIME","runtime bytes restored after later failure");
  check(readTextFile(ini)=="OLD-CONFIG","config bytes remain unchanged after later failure");
  DeleteFileW(d3d11.c_str());DeleteFileW(runtime.c_str());DeleteFileW(ini.c_str());DeleteFileW(blocker.c_str());
}

} // namespace edvr::installer::native_apply_cases

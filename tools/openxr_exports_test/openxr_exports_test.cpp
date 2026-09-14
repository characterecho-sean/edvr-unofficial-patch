#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../src/openvr/compat/openvr_v0_9_20.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

namespace {
unsigned checks=0,failures=0;
void check(bool value,const char* name){++checks;if(!value){++failures;std::printf("FAIL: %s\n",name);}}
using Init=uint32_t(__cdecl*)(vr::EVRInitError*,vr::EVRApplicationType);
using Shutdown=void(__cdecl*)();using Generic=void*(__cdecl*)(const char*,vr::EVRInitError*);
using Valid=bool(__cdecl*)(const char*);using Token=uint32_t(__cdecl*)();using Bind=bool(__cdecl*)();
int test() {
  wchar_t path[32768]{};const auto length=GetModuleFileNameW(nullptr,path,32768);
  check(length>0&&length<32768,"absolute executable path");if(!length||length>=32768)return 1;
  const auto dll=std::filesystem::path(path).parent_path()/L"openxr_export_fixture.dll";
  HMODULE module=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  check(module!=nullptr,"load explicit fixture DLL");if(!module)return 1;
  const auto init=reinterpret_cast<Init>(GetProcAddress(module,"VR_InitInternal"));
  const auto shutdown=reinterpret_cast<Shutdown>(GetProcAddress(module,"VR_ShutdownInternal"));
  const auto generic=reinterpret_cast<Generic>(GetProcAddress(module,"VR_GetGenericInterface"));
  const auto valid=reinterpret_cast<Valid>(GetProcAddress(module,"VR_IsInterfaceVersionValid"));
  const auto token=reinterpret_cast<Token>(GetProcAddress(module,"VR_GetInitToken"));
  const auto bind=reinterpret_cast<Bind>(GetProcAddress(module,"edvrBindRuntimeFixture"));
  check(init&&shutdown&&generic&&valid&&token&&bind,"exact named C exports");
  if(!(init&&shutdown&&generic&&valid&&token&&bind)){FreeLibrary(module);return 1;}
  vr::EVRInitError error=vr::VRInitError_Unknown;
  check(init(&error,vr::VRApplication_Scene)==0&&error==vr::VRInitError_Init_NotInitialized,"unbound init unavailable");
  check(!valid(vr::IVRSystem_Version)&&token()==0,"unbound support/token");shutdown();
  check(bind()&&!bind(),"bind once outside DllMain");
  check(!generic(vr::IVRSystem_Version,&error)&&error==vr::VRInitError_Init_NotInitialized,"getter before init");
  const char* versions[]={vr::IVRSystem_Version,vr::IVRCompositor_Version,vr::IVRChaperone_Version,vr::IVRExtendedDisplay_Version};
  for(const auto version:versions)check(valid(version),"supported exact version");
  check(!valid(nullptr)&&!valid("IVROverlay_011")&&!valid("IVRSystem_013"),"unsupported versions");
  const auto first=init(&error,vr::VRApplication_Scene);
  check(first==1&&error==vr::VRInitError_None&&token()==first,"typed init/token ABI");
  check(init(nullptr,vr::VRApplication_Scene)==first,"repeat init stable");
  void* pointers[4]{};
  for(unsigned i=0;i<4;++i){pointers[i]=generic(versions[i],&error);check(pointers[i]&&error==vr::VRInitError_None&&generic(versions[i],nullptr)==pointers[i],"stable interface identity");}
  if(pointers[0]&&pointers[1]&&pointers[2]&&pointers[3]) {
    auto* system=static_cast<vr::IVRSystem*>(pointers[0]);uint32_t width=0,height=0;
    system->GetRecommendedRenderTargetSize(&width,&height);check(width==640&&height==480,"returned System member ABI");
    const auto matrix=system->GetProjectionMatrix(vr::Eye_Left,.025f,50000,vr::API_DirectX);
    check(matrix.m[3][2]==-1&&matrix.m[0][0]>0,"returned aggregate member ABI");
    check(static_cast<vr::IVRCompositor*>(pointers[1])->GetTrackingSpace()==vr::TrackingUniverseSeated,"returned Compositor member ABI");
    check(static_cast<vr::IVRChaperone*>(pointers[2])->GetCalibrationState()==vr::ChaperoneCalibrationState_Error,"returned Chaperone member ABI");
    int32_t x=7,y=7;static_cast<vr::IVRExtendedDisplay*>(pointers[3])->GetWindowBounds(&x,&y,&width,&height);
    check(x==0&&y==0&&width==1280&&height==480,"returned ExtendedDisplay member ABI");
    bool worker=false;std::thread reader([&]{uint32_t w=0,h=0;system->GetRecommendedRenderTargetSize(&w,&h);worker=w==640&&h==480&&token()==first;});reader.join();
    check(worker,"cross-thread exported token and cached member");
  }
  check(!generic("IVROverlay_011",&error)&&error==vr::VRInitError_Init_InterfaceNotFound,"unknown getter failure");
  check(!generic(nullptr,&error)&&error==vr::VRInitError_Init_InvalidInterface,"null version error");
  shutdown();check(token()==2,"shutdown advances token");
  for(const auto version:versions)check(!generic(version,&error)&&error==vr::VRInitError_Init_NotInitialized,"all getters retired");
  shutdown();check(token()==2,"repeated shutdown idempotent");
  check(init(&error,vr::VRApplication_Scene)==3&&token()==3,"clean reinit generation");
  shutdown();check(token()==4,"reinit shutdown token");
  check(FreeLibrary(module)!=FALSE,"unload after all calls and shutdown");
  std::printf("openxr_exports_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
}
int main(int argc,char** argv) {
  if(argc!=2)return 2;
  if(std::strcmp(argv[1],"--dry-run")==0){std::puts("openxr_exports_test: dry-run (no DLL load, runtime or writes)");return 0;}
  if(std::strcmp(argv[1],"--self-test")!=0)return 2;
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);return test();
}

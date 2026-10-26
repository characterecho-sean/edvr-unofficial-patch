#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../../src/openxr/session_binding.h"
#include "../../src/openxr/geometry_locator.h"
#include "../../src/openxr/published_session.h"
#include "../../src/common/frame_flag.h"
#include "../../src/common/system_d3d11.h"
#include <dxgi.h>
#include <cstdio>
#include <vector>
#include <string>
#include <cstring>
using namespace edvr::openxr;
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0,failures=0;
void check(bool value,const char* message){++checks;if(!value){++failures;std::printf("FAIL: %s\n",message);}}
const auto instance=reinterpret_cast<XrInstance>(1);
const auto session=reinterpret_cast<XrSession>(7);
const auto local=reinterpret_cast<XrSpace>(8), view=reinterpret_cast<XrSpace>(9), stage=reinterpret_cast<XrSpace>(10);
struct Fake {
  ComPtr<ID3D11Device> device,foreign;LUID luid{};D3D_FEATURE_LEVEL level{};
  std::vector<std::string> trace;std::string failure;XrResult result=XR_ERROR_RUNTIME_FAILURE;
  bool live=false,localLive=false,viewLive=false,stageLive=false,wrongAdapter=false,wrongLevel=false;
  bool supportsStage=false;
  bool missingSpace=false,grow=false,badCount=false,hugeCount=false,neverFits=false,invalidTracking=false;
  bool probeDualOrigin=false,stagePoseUnavailable=false;
  bool expectVelocity=false,badVelocityChain=false;
  XrSpace expectedOrigin=local;
  uint32_t viewCount=2;XrTime lastTime=0;unsigned enumCalls=0;
};
Fake* fake=nullptr;
XrResult call(const char* name){fake->trace.push_back(name);return fake->failure==name?fake->result:XR_SUCCESS;}
XrResult XRAPI_PTR requirements(XrInstance i,XrSystemId s,XrGraphicsRequirementsD3D11KHR* out){
  check(i==instance&&s==42&&out->type==XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR&&!out->next,"requirements ABI");
  out->adapterLuid=fake->luid;if(fake->wrongAdapter)out->adapterLuid.LowPart^=1;
  out->minFeatureLevel=fake->wrongLevel?D3D_FEATURE_LEVEL_12_1:fake->level;return call("requirements");
}
XrResult XRAPI_PTR createSession(XrInstance i,const XrSessionCreateInfo* info,XrSession* out){
  check(i==instance&&info->type==XR_TYPE_SESSION_CREATE_INFO&&info->systemId==42&&info->createFlags==0,"session ABI");
  const auto* binding=static_cast<const XrGraphicsBindingD3D11KHR*>(info->next);
  check(binding&&binding->type==XR_TYPE_GRAPHICS_BINDING_D3D11_KHR&&!binding->next&&binding->device==fake->device.Get(),"exact supplied device bound");
  check(fake->trace==std::vector<std::string>({"requirements"}),"requirements queried before create");
  const XrResult r=call("session");if(r==XR_SUCCESS){fake->live=true;*out=session;}else *out=reinterpret_cast<XrSession>(123);
  return r;
}
XrResult XRAPI_PTR destroySession(XrSession s){
  check(s==session&&fake->live&&!fake->localLive&&!fake->viewLive&&!fake->stageLive,"session destroyed after spaces");fake->live=false;return call("destroySession");
}
XrResult XRAPI_PTR enumerateSpaces(XrSession s,uint32_t capacity,uint32_t* count,XrReferenceSpaceType* values){
  check(s==session&&fake->live,"enumeration session");++fake->enumCalls;
  const XrResult r=call(capacity?"enumFill":"enumCount");if(r!=XR_SUCCESS)return r;
  if(fake->hugeCount){*count=65;return XR_SUCCESS;}
  if(fake->neverFits){*count=capacity+1;return capacity?XR_ERROR_SIZE_INSUFFICIENT:XR_SUCCESS;}
  *count=fake->grow&&fake->enumCalls==1?1:(fake->supportsStage?3:2);
  if(!capacity)return XR_SUCCESS;if(capacity<*count)return XR_ERROR_SIZE_INSUFFICIENT;
  values[0]=XR_REFERENCE_SPACE_TYPE_LOCAL;values[1]=fake->missingSpace?XR_REFERENCE_SPACE_TYPE_STAGE:XR_REFERENCE_SPACE_TYPE_VIEW;
  if(fake->supportsStage)values[2]=XR_REFERENCE_SPACE_TYPE_STAGE;
  if(fake->badCount)*count=capacity+1;return XR_SUCCESS;
}
XrResult XRAPI_PTR createSpace(XrSession s,const XrReferenceSpaceCreateInfo* info,XrSpace* out){
  check(s==session&&fake->live&&info->type==XR_TYPE_REFERENCE_SPACE_CREATE_INFO&&!info->next,"space ABI");
  const XrPosef identity{{0,0,0,1},{0,0,0}};check(std::memcmp(&identity,&info->poseInReferenceSpace,sizeof(identity))==0,"identity reference space pose");
  const bool isLocal=info->referenceSpaceType==XR_REFERENCE_SPACE_TYPE_LOCAL;
  const bool isStage=info->referenceSpaceType==XR_REFERENCE_SPACE_TYPE_STAGE;
  check(isLocal||isStage||info->referenceSpaceType==XR_REFERENCE_SPACE_TYPE_VIEW,"required space type");
  const XrResult r=call(isLocal?"local":isStage?"stage":"view");
  if(r==XR_SUCCESS||r==XR_SESSION_LOSS_PENDING){*out=isLocal?local:isStage?stage:view;
    if(isLocal)fake->localLive=true;else if(isStage)fake->stageLive=true;else fake->viewLive=true;}
  else *out=reinterpret_cast<XrSpace>(123);return r;
}
XrResult XRAPI_PTR destroySpace(XrSpace space){
  if(space==view){check(fake->viewLive,"destroy view once");fake->viewLive=false;return call("destroyView");}
  if(space==stage){check(fake->stageLive,"destroy stage once");fake->stageLive=false;return call("destroyStage");}
  check(space==local&&fake->localLive&&!fake->viewLive,"destroy local after view");fake->localLive=false;return call("destroyLocal");
}
XrResult XRAPI_PTR locateViews(XrSession s,const XrViewLocateInfo* info,XrViewState* state,uint32_t capacity,uint32_t* count,XrView* views){
  check(s==session&&info->type==XR_TYPE_VIEW_LOCATE_INFO&&!info->next&&info->viewConfigurationType==XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO&&
        (fake->probeDualOrigin?(info->space==stage||info->space==local):info->space==fake->expectedOrigin)&&capacity==2,"locate view ABI/space");
  check(state->type==XR_TYPE_VIEW_STATE&&!state->next,"view state initialized");
  fake->lastTime=info->displayTime;const XrResult r=call("locateViews");if(r!=XR_SUCCESS)return r;
  *count=fake->viewCount;state->viewStateFlags=fake->invalidTracking||(fake->stagePoseUnavailable&&info->space==stage)?0:
      XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT;
  for(unsigned eye=0;eye<2;++eye){check(views[eye].type==XR_TYPE_VIEW&&!views[eye].next,"typed view output");views[eye].pose={{0,0,0,1},{float(eye),2,3}};views[eye].fov={-.7f,.8f,.9f,-1.f};}
  return XR_SUCCESS;
}
XrResult XRAPI_PTR locateSpace(XrSpace from,XrSpace base,XrTime time,XrSpaceLocation* out){
  check(from==view&&(fake->probeDualOrigin?(base==stage||base==local):base==fake->expectedOrigin)&&
        time==fake->lastTime&&out->type==XR_TYPE_SPACE_LOCATION&&bool(out->next)==fake->expectVelocity,"head shares exact time/base with eye views");
  const XrResult r=call("locateHead");if(r!=XR_SUCCESS)return r;
  out->locationFlags=fake->invalidTracking||(fake->stagePoseUnavailable&&base==stage)?0:
      XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  out->pose={{0,0,0,1},{10,20,30}};
  if(fake->expectVelocity) {
    auto* velocity=static_cast<XrSpaceVelocity*>(out->next);
    check(velocity&&velocity->type==XR_TYPE_SPACE_VELOCITY&&!velocity->next,"typed optional head velocity");
    velocity->velocityFlags=XR_SPACE_VELOCITY_LINEAR_VALID_BIT|XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
    velocity->linearVelocity={1,2,3};velocity->angularVelocity={4,5,6};
    if(fake->badVelocityChain)velocity->next=out;
  }
  return XR_SUCCESS;
}
BindingDispatch dispatch(){return {requirements,createSession,destroySession,enumerateSpaces,createSpace,destroySpace};}
struct Fixture {
  Fake runtime;SessionBinding binding;
  Fixture(){fake=&runtime;
    // Windows' own d3d11 through common/system_d3d11.h, never an import: EDVR's proxy sits beside this exe.
    const auto create=edvr::systemD3D11CreateDevice();check(create!=nullptr,"system D3D11 factory");
    check(create&&SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&runtime.device,&runtime.level,nullptr)),"WARP candidate");
    check(create&&SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&runtime.foreign,nullptr,nullptr)),"same-adapter foreign device");
    static bool reported=false;
    if(!reported){reported=true;check(edvr::reportSystemD3D11Only("openxr_binding_test"),"the rig runs on System32's d3d11.dll and on no other d3d11.dll");}
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;
    check(SUCCEEDED(runtime.device.As(&dxgi))&&SUCCEEDED(dxgi->GetAdapter(&adapter)),"fixture adapter");
    DXGI_ADAPTER_DESC desc{};check(SUCCEEDED(adapter->GetDesc(&desc)),"fixture LUID");runtime.luid=desc.AdapterLuid;
  }
  ~Fixture(){binding.shutdown();}
  XrResult init(XrReferenceSpaceType type=XR_REFERENCE_SPACE_TYPE_LOCAL){return binding.initialize(dispatch(),instance,42,runtime.device.Get(),type);}
};
int selfTest(){
  {Fixture f;check(f.init()==XR_SUCCESS,"initialize existing device");
    check(f.runtime.trace==std::vector<std::string>({"requirements","session","enumCount","enumFill","local","view"}),"full init trace");
    check(f.binding.session()==session&&f.binding.localSpace()==local&&f.binding.viewSpace()==view&&f.binding.device()==f.runtime.device.Get(),"binding exposes owned resources");
    const auto trace=f.runtime.trace;check(f.init()==XR_ERROR_CALL_ORDER_INVALID&&trace==f.runtime.trace,"reinitialize cannot forget live resources");
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=16;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D11Texture2D> own,foreign;check(SUCCEEDED(f.runtime.device->CreateTexture2D(&desc,nullptr,&own))&&SUCCEEDED(f.runtime.foreign->CreateTexture2D(&desc,nullptr,&foreign)),"fixture textures");
    check(f.binding.validateTexture(own.Get())==XR_SUCCESS,"same device texture accepted");check(f.binding.validateTexture(foreign.Get())==XR_ERROR_GRAPHICS_DEVICE_INVALID,"foreign device on same adapter rejected");
    check(f.binding.validateTexture(nullptr)==XR_ERROR_HANDLE_INVALID,"null texture rejected");
    f.runtime.trace.clear();check(f.binding.shutdown()==XR_SUCCESS,"explicit cleanup");
    check(f.runtime.trace==std::vector<std::string>({"destroyView","destroyLocal","destroySession"}),"reverse resource destruction");
    check(!f.binding.device()&&!f.binding.session()&&!f.binding.localSpace()&&!f.binding.viewSpace(),"cleanup clears handles");
    f.runtime.trace.clear();check(f.binding.shutdown()==XR_SUCCESS&&f.runtime.trace.empty(),"cleanup idempotent");
    check(f.init()==XR_SUCCESS,"reinitialize after destruction");
  }
  {Fixture f;f.runtime.supportsStage=true;
    check(f.init(XR_REFERENCE_SPACE_TYPE_STAGE)==XR_SUCCESS,"stage binding initializes when enumerated");
    check(f.binding.naturalType()==XR_REFERENCE_SPACE_TYPE_STAGE&&f.binding.naturalSpace()==stage&&
          f.binding.localSpace()==local,"stage selected without losing local diagnostic space");
    check(f.runtime.trace==std::vector<std::string>({"requirements","session","enumCount","enumFill","local","view","stage"}),
          "stage created after required reference spaces");
    f.runtime.trace.clear();check(f.binding.shutdown()==XR_SUCCESS,"stage binding shutdown");
    check(f.runtime.trace==std::vector<std::string>({"destroyView","destroyStage","destroyLocal","destroySession"}),
          "stage destroyed before local and session");
  }
  {Fixture f;f.runtime.supportsStage=true;f.runtime.probeDualOrigin=true;f.runtime.stagePoseUnavailable=true;
    check(f.init(XR_REFERENCE_SPACE_TYPE_STAGE)==XR_SUCCESS,"unlocatable-stage fixture advertises and creates STAGE");
    Frame frame{};frame.status=FrameStatus::NoRender;frame.sequence=11;frame.predictedDisplayTime=123456789;
    XrViewConfigurationView sizes[2]{};for(auto& s:sizes){s.recommendedImageRectWidth=100;s.recommendedImageRectHeight=101;}
    NaturalSpaceProbe choice=NaturalSpaceProbe::Pending;f.runtime.trace.clear();
    check(probeNaturalSpace({locateViews,locateSpace},f.binding,frame,5,sizes,choice)==XR_SUCCESS&&
          choice==NaturalSpaceProbe::LocalFallback,"invalid STAGE eye/head poses with valid LOCAL poses select LOCAL");
    check(f.runtime.trace==std::vector<std::string>({"locateViews","locateHead","locateViews","locateHead"}),
          "both reference spaces are probed at the same frame time");
    check(f.binding.useLocalFallback()&&f.binding.naturalType()==XR_REFERENCE_SPACE_TYPE_LOCAL&&f.binding.naturalSpace()==local,
          "fallback retains the usable LOCAL binding and leaves STAGE owned for shutdown");
    f.runtime.expectedOrigin=local;f.runtime.probeDualOrigin=false;
    GeometryInput located{};
    check(locateGeometry({locateViews,locateSpace},f.binding,frame,5,sizes,located,nullptr,f.binding.naturalSpace())==XR_SUCCESS&&
          (located.viewFlags&(XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT))==
              (XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT)&&
          (located.headFlags&(XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))==
              (XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT),
          "fallback produces usable LOCAL stereo and head poses");
  }
  {Fixture f;f.runtime.supportsStage=true;f.runtime.probeDualOrigin=true;
    check(f.init(XR_REFERENCE_SPACE_TYPE_STAGE)==XR_SUCCESS,"locatable-stage fixture initializes");
    Frame frame{};frame.status=FrameStatus::NoRender;frame.sequence=12;frame.predictedDisplayTime=123456790;
    XrViewConfigurationView sizes[2]{};for(auto& s:sizes){s.recommendedImageRectWidth=100;s.recommendedImageRectHeight=101;}
    NaturalSpaceProbe choice=NaturalSpaceProbe::Pending;f.runtime.trace.clear();
    check(probeNaturalSpace({locateViews,locateSpace},f.binding,frame,5,sizes,choice)==XR_SUCCESS&&
          choice==NaturalSpaceProbe::Stage&&f.binding.naturalSpace()==stage,
          "valid STAGE poses preserve the tested upright route");
    check(f.runtime.trace==std::vector<std::string>({"locateViews","locateHead"}),
          "valid STAGE route does not probe or select LOCAL");
  }
  {Fixture f;f.runtime.supportsStage=true;f.runtime.probeDualOrigin=true;f.runtime.invalidTracking=true;
    check(f.init(XR_REFERENCE_SPACE_TYPE_STAGE)==XR_SUCCESS,"untracked startup fixture initializes both spaces");
    Frame frame{};frame.status=FrameStatus::NoRender;frame.sequence=13;frame.predictedDisplayTime=123456791;
    XrViewConfigurationView sizes[2]{};for(auto& s:sizes){s.recommendedImageRectWidth=100;s.recommendedImageRectHeight=101;}
    NaturalSpaceProbe choice=NaturalSpaceProbe::Stage;
    check(probeNaturalSpace({locateViews,locateSpace},f.binding,frame,5,sizes,choice)==XR_SUCCESS&&
          choice==NaturalSpaceProbe::Pending&&f.binding.naturalSpace()==stage,
          "when neither route has valid poses startup waits without choosing an untracked LOCAL space");
  }
  {Fixture f;check(f.init(XR_REFERENCE_SPACE_TYPE_STAGE)==XR_SUCCESS,
      "stage request keeps LOCAL when the runtime does not enumerate STAGE");
    check(f.binding.naturalType()==XR_REFERENCE_SPACE_TYPE_LOCAL&&f.binding.naturalSpace()==local,
          "unsupported STAGE request selects the existing LOCAL path");
    check(f.runtime.trace==std::vector<std::string>({"requirements","session","enumCount","enumFill","local","view"}),
          "unsupported STAGE request creates no STAGE space");
  }
  {Fixture f;f.runtime.supportsStage=true;f.runtime.failure="stage";
    check(f.init(XR_REFERENCE_SPACE_TYPE_STAGE)==XR_ERROR_RUNTIME_FAILURE,
          "advertised STAGE creation failure is not silently converted to LOCAL");
    check(!f.runtime.live&&!f.runtime.localLive&&!f.runtime.viewLive&&!f.runtime.stageLive,
          "advertised STAGE creation failure releases the session and required spaces");}
  for(const char* failure:{"requirements","session","enumCount","enumFill","local","view"}){
    Fixture f;f.runtime.failure=failure;check(f.init()==XR_ERROR_RUNTIME_FAILURE,"exact init error");
    check(!f.runtime.live&&!f.runtime.localLive&&!f.runtime.viewLive&&!f.binding.device(),"every partial init releases known resources");
  }
  for(const char* failure:{"requirements","enumCount","local","view"}){
    Fixture f;f.runtime.failure=failure;f.runtime.result=XR_SESSION_LOSS_PENDING;
    check(f.init()==XR_SESSION_LOSS_PENDING,"positive pending result not swallowed");check(!f.runtime.live&&!f.runtime.localLive&&!f.runtime.viewLive,"pending initialization cleans created resources");
  }
  for(const char* point:{"session","local","view"}){
    Fixture f;f.runtime.failure=point;f.runtime.result=XR_TIMEOUT_EXPIRED;
    check(f.init()==XR_TIMEOUT_EXPIRED,"unexpected positive result preserved");
    check(!f.runtime.live&&!f.runtime.localLive&&!f.runtime.viewLive,"unspecified result cannot transfer junk handle ownership");
  }
  for(unsigned mode=0;mode<7;++mode){
    Fixture f;
    if(mode==0)f.runtime.wrongAdapter=true;if(mode==1)f.runtime.wrongLevel=true;if(mode==2)f.runtime.missingSpace=true;
    if(mode==3)f.runtime.grow=true;if(mode==4)f.runtime.badCount=true;if(mode==5)f.runtime.hugeCount=true;if(mode==6)f.runtime.neverFits=true;
    const XrResult r=f.init();check((mode==3)?r==XR_SUCCESS:r!=XR_SUCCESS,"capability/enum contract enforced");
    if(mode<2)check(f.runtime.trace==std::vector<std::string>({"requirements"}),"incompatible device rejected before session creation");
    if(mode==3)check(f.runtime.enumCalls==3,"count growth retry uses new capacity");
    if(mode==6)check(r==XR_ERROR_SIZE_INSUFFICIENT&&f.runtime.enumCalls==4,"bounded retry exhaustion");
  }
  for(const char* point:{"destroyView","destroyLocal","destroySession"}){
    Fixture f;check(f.init()==XR_SUCCESS,"cleanup failure fixture");f.runtime.failure=point;f.runtime.trace.clear();
    check(f.binding.shutdown()==XR_ERROR_RUNTIME_FAILURE,"first cleanup error reported");check(f.runtime.trace.size()==3&&!f.runtime.live,"cleanup continues after failure");
  }
  {Fixture f;BindingDispatch bad=dispatch();bad.destroySpace=nullptr;
    check(f.binding.initialize(bad,instance,42,f.runtime.device.Get())==XR_ERROR_FUNCTION_UNSUPPORTED&&f.runtime.trace.empty(),"complete dispatch before allocation");
    check(f.binding.initialize(dispatch(),XR_NULL_HANDLE,42,f.runtime.device.Get())==XR_ERROR_HANDLE_INVALID,"null instance rejected");
    check(f.binding.initialize(dispatch(),instance,0,f.runtime.device.Get())==XR_ERROR_SYSTEM_INVALID,"null system rejected");
    check(f.binding.initialize(dispatch(),instance,42,nullptr)==XR_ERROR_HANDLE_INVALID&&f.runtime.trace.empty(),"missing game device never waits or dispatches");
  }
  {Fixture f;check(f.init()==XR_SUCCESS,"locator fixture");Frame frame{};frame.status=FrameStatus::NoRender;frame.sequence=11;frame.predictedDisplayTime=123456789;
    XrViewConfigurationView sizes[2]{};for(auto& s:sizes){s.recommendedImageRectWidth=100;s.recommendedImageRectHeight=101;}
    GeometryInput out{};f.runtime.trace.clear();
    check(locateGeometry({locateViews,locateSpace},f.binding,frame,5,sizes,out)==XR_SUCCESS,"geometry located on no-render startup frame");
    check(f.runtime.trace==std::vector<std::string>({"locateViews","locateHead"})&&out.displayTime==123456789&&out.sequence==11&&out.generation==5,"locator does not wait or advance frame");
    check(out.headPose.position.x==10&&out.views[0].pose.position.x==0&&out.views[1].pose.position.x==1,"head is located separately, never eye average");
    f.runtime.expectVelocity=true;XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};f.runtime.trace.clear();
    check(locateGeometry({locateViews,locateSpace},f.binding,frame,5,sizes,out,&velocity)==XR_SUCCESS&&
      velocity.linearVelocity.y==2&&velocity.angularVelocity.z==6&&velocity.next==nullptr&&
      f.runtime.trace==std::vector<std::string>({"locateViews","locateHead"}),"render velocity from same head locate and time");
    const auto originalVelocity=velocity;const auto originalGeometry=out;f.runtime.badVelocityChain=true;
    check(locateGeometry({locateViews,locateSpace},f.binding,frame,5,sizes,out,&velocity)==XR_ERROR_VALIDATION_FAILURE&&
      std::memcmp(&velocity,&originalVelocity,sizeof(velocity))==0&&std::memcmp(&out,&originalGeometry,sizeof(out))==0,"malformed velocity chain publishes neither output");
    f.runtime.badVelocityChain=false;f.runtime.failure="locateHead";f.runtime.result=XR_SESSION_LOSS_PENDING;
    check(locateGeometry({locateViews,locateSpace},f.binding,frame,5,sizes,out,&velocity)==XR_SESSION_LOSS_PENDING&&
      std::memcmp(&velocity,&originalVelocity,sizeof(velocity))==0,"positive head result leaves velocity unchanged");
    f.runtime.failure.clear();f.runtime.expectVelocity=false;
    f.runtime.expectedOrigin=reinterpret_cast<XrSpace>(42);f.runtime.trace.clear();
    check(locateGeometry({locateViews,locateSpace},f.binding,frame,5,sizes,out,nullptr,f.runtime.expectedOrigin)==XR_SUCCESS&&
      f.runtime.trace==std::vector<std::string>({"locateViews","locateHead"}),"selected seated space replaces LOCAL in both queries");
    f.runtime.expectedOrigin=local;
    const auto canary=out;
    for(const char* point:{"locateViews","locateHead"})for(XrResult r:{XR_ERROR_SESSION_LOST,XR_SESSION_LOSS_PENDING}){
      f.runtime.failure=point;f.runtime.result=r;f.runtime.trace.clear();
      check(locateGeometry({locateViews,locateSpace},f.binding,frame,5,sizes,out)==r,"locator preserves runtime result");
      check(std::memcmp(&out,&canary,sizeof(out))==0&&f.runtime.trace.back()==point,"failure leaves output unchanged and stops dispatch");
    }
    f.runtime.failure.clear();f.runtime.invalidTracking=true;
    check(locateGeometry({locateViews,locateSpace},f.binding,frame,5,sizes,out)==XR_SUCCESS&&out.viewFlags==0&&out.headFlags==0,"ordinary tracking invalidity not fabricated");
    f.runtime.viewCount=1;check(locateGeometry({locateViews,locateSpace},f.binding,frame,5,sizes,out)==XR_ERROR_RUNTIME_FAILURE,"stereo locate count required");
    frame.status=FrameStatus::Ended;f.runtime.trace.clear();check(locateGeometry({locateViews,locateSpace},f.binding,frame,5,sizes,out)==XR_ERROR_CALL_ORDER_INVALID&&f.runtime.trace.empty(),"ended frame rejected");
  }
  std::printf("openxr_binding_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
int publishedTest(const char* path){
  // A separate process per invocation: real graphics DLL publication, fake XR.
  if(!path||std::strlen(path)<4||path[1]!=':'||(path[2]!='/'&&path[2]!='\\'))return 2;
  // The graphics DLL below is the only d3d11 this process is meant to run: none arrives with the exe.
  check(edvr::reportNoD3D11Mapped("openxr_binding_test"),"no d3d11.dll is mapped before the rig loads the graphics proxy");
  Fake runtime;fake=&runtime;SessionBinding binding;
  check(!edvr::gameDevice(),"fresh process has no graphics publisher");
  check(initializePublishedSession(binding,dispatch(),instance,42)==XR_ERROR_HANDLE_INVALID&&runtime.trace.empty(),"missing graphics DLL fails before XR calls");
  const int len=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,nullptr,0);if(!len)return 2;
  std::wstring wide(size_t(len),L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide.data(),len);
  const HMODULE graphics=LoadLibraryExW(wide.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if(!graphics){std::printf("FAIL: load graphics fixture %lu\n",GetLastError());return 3;}
  // Keep the hook-owning module loaded for this child process's lifetime.
  const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(graphics,"D3D11CreateDevice"));
  if(!create)return 3;
  ComPtr<ID3D11DeviceContext> context;
  check(SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&runtime.device,&runtime.level,&context)),"device created through actual graphics proxy");
  check(runtime.device&&edvr::gameDevice()==runtime.device.Get(),"actual DLL published exact returned device");
  if(!runtime.device)return 3;
  ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC desc{};
  if(FAILED(runtime.device.As(&dxgi))||FAILED(dxgi->GetAdapter(&adapter))||FAILED(adapter->GetDesc(&desc)))return 3;
  runtime.luid=desc.AdapterLuid;
  check(initializePublishedSession(binding,dispatch(),instance,42)==XR_SUCCESS,"published graphics device enters real binding path");
  check(binding.device()==runtime.device.Get()&&binding.session()==session,"no replacement device or session");
  check(binding.shutdown()==XR_SUCCESS,"published-device binding cleanup");
  std::printf("openxr_published_device_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
}
int main(int argc,char** argv){
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
  if(argc==3&&!std::strcmp(argv[1],"--published-proxy"))return publishedTest(argv[2]);
  if(argc!=2)return 2;if(!std::strcmp(argv[1],"--dry-run")){std::puts("Would test caller-device binding and geometry location with fake XR/WARP; no device/runtime/files.");return 0;}
  return !std::strcmp(argv[1],"--self-test")?selfTest():2;
}

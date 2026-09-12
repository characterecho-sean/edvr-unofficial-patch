#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../../src/openxr/session_binding.h"
#include "../../src/openxr/geometry_locator.h"
#include "../../src/openxr/published_session.h"
#include "../../src/common/frame_flag.h"
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
const auto local=reinterpret_cast<XrSpace>(8), view=reinterpret_cast<XrSpace>(9);
struct Fake {
  ComPtr<ID3D11Device> device,foreign;LUID luid{};D3D_FEATURE_LEVEL level{};
  std::vector<std::string> trace;std::string failure;XrResult result=XR_ERROR_RUNTIME_FAILURE;
  bool live=false,localLive=false,viewLive=false,wrongAdapter=false,wrongLevel=false;
  bool missingSpace=false,grow=false,badCount=false,hugeCount=false,neverFits=false,invalidTracking=false;
  bool expectVelocity=false,badVelocityChain=false;
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
  check(s==session&&fake->live&&!fake->localLive&&!fake->viewLive,"session destroyed after spaces");fake->live=false;return call("destroySession");
}
XrResult XRAPI_PTR enumerateSpaces(XrSession s,uint32_t capacity,uint32_t* count,XrReferenceSpaceType* values){
  check(s==session&&fake->live,"enumeration session");++fake->enumCalls;
  const XrResult r=call(capacity?"enumFill":"enumCount");if(r!=XR_SUCCESS)return r;
  if(fake->hugeCount){*count=65;return XR_SUCCESS;}
  if(fake->neverFits){*count=capacity+1;return capacity?XR_ERROR_SIZE_INSUFFICIENT:XR_SUCCESS;}
  *count=fake->grow&&fake->enumCalls==1?1:2;
  if(!capacity)return XR_SUCCESS;if(capacity<2)return XR_ERROR_SIZE_INSUFFICIENT;
  values[0]=XR_REFERENCE_SPACE_TYPE_LOCAL;values[1]=fake->missingSpace?XR_REFERENCE_SPACE_TYPE_STAGE:XR_REFERENCE_SPACE_TYPE_VIEW;
  if(fake->badCount)*count=capacity+1;return XR_SUCCESS;
}
XrResult XRAPI_PTR createSpace(XrSession s,const XrReferenceSpaceCreateInfo* info,XrSpace* out){
  check(s==session&&fake->live&&info->type==XR_TYPE_REFERENCE_SPACE_CREATE_INFO&&!info->next,"space ABI");
  const XrPosef identity{{0,0,0,1},{0,0,0}};check(std::memcmp(&identity,&info->poseInReferenceSpace,sizeof(identity))==0,"identity reference space pose");
  const bool isLocal=info->referenceSpaceType==XR_REFERENCE_SPACE_TYPE_LOCAL;check(isLocal||info->referenceSpaceType==XR_REFERENCE_SPACE_TYPE_VIEW,"required space type");
  const XrResult r=call(isLocal?"local":"view");
  if(r==XR_SUCCESS||r==XR_SESSION_LOSS_PENDING){*out=isLocal?local:view;if(isLocal)fake->localLive=true;else fake->viewLive=true;}
  else *out=reinterpret_cast<XrSpace>(123);return r;
}
XrResult XRAPI_PTR destroySpace(XrSpace space){
  if(space==view){check(fake->viewLive,"destroy view once");fake->viewLive=false;return call("destroyView");}
  check(space==local&&fake->localLive&&!fake->viewLive,"destroy local after view");fake->localLive=false;return call("destroyLocal");
}
XrResult XRAPI_PTR locateViews(XrSession s,const XrViewLocateInfo* info,XrViewState* state,uint32_t capacity,uint32_t* count,XrView* views){
  check(s==session&&info->type==XR_TYPE_VIEW_LOCATE_INFO&&!info->next&&info->viewConfigurationType==XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO&&info->space==local&&capacity==2,"locate view ABI/space");
  check(state->type==XR_TYPE_VIEW_STATE&&!state->next,"view state initialized");
  fake->lastTime=info->displayTime;const XrResult r=call("locateViews");if(r!=XR_SUCCESS)return r;
  *count=fake->viewCount;state->viewStateFlags=fake->invalidTracking?0:XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT;
  for(unsigned eye=0;eye<2;++eye){check(views[eye].type==XR_TYPE_VIEW&&!views[eye].next,"typed view output");views[eye].pose={{0,0,0,1},{float(eye),2,3}};views[eye].fov={-.7f,.8f,.9f,-1.f};}
  return XR_SUCCESS;
}
XrResult XRAPI_PTR locateSpace(XrSpace from,XrSpace base,XrTime time,XrSpaceLocation* out){
  check(from==view&&base==local&&time==fake->lastTime&&out->type==XR_TYPE_SPACE_LOCATION&&bool(out->next)==fake->expectVelocity,"head shares exact time/base with eye views");
  const XrResult r=call("locateHead");if(r!=XR_SUCCESS)return r;
  out->locationFlags=fake->invalidTracking?0:XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
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
  Fixture(){fake=&runtime;check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&runtime.device,&runtime.level,nullptr)),"WARP candidate");
    check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&runtime.foreign,nullptr,nullptr)),"same-adapter foreign device");
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;
    check(SUCCEEDED(runtime.device.As(&dxgi))&&SUCCEEDED(dxgi->GetAdapter(&adapter)),"fixture adapter");
    DXGI_ADAPTER_DESC desc{};check(SUCCEEDED(adapter->GetDesc(&desc)),"fixture LUID");runtime.luid=desc.AdapterLuid;
  }
  ~Fixture(){binding.shutdown();}
  XrResult init(){return binding.initialize(dispatch(),instance,42,runtime.device.Get());}
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

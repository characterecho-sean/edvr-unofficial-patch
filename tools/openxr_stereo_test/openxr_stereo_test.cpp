#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../../src/openxr/d3d11_stereo.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
using namespace edvr::openxr;
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0,failures=0;
void check(bool value,const char* msg){++checks;if(!value){++failures;std::printf("FAIL: %s\n",msg);}}
constexpr unsigned size=128;
const auto session=reinterpret_cast<XrSession>(1);
const auto space=reinterpret_cast<XrSpace>(2);
struct Eye {ComPtr<ID3D11Texture2D> images[2];bool alive=false,acquired=false,waited=false;unsigned calls=0,index=0;};
struct Fake {
  ComPtr<ID3D11Device> device,foreign;ComPtr<ID3D11DeviceContext> context;Eye eyes[2];
  unsigned creates=0,destroys=0,formatCalls=0;std::vector<std::string> trace;
  std::string failure;XrResult error=XR_ERROR_RUNTIME_FAILURE;
  bool noFormats=false,wrongDimensions=false,wrongDevice=false,typeless=true,unorm=false,grow=false,badCount=false;
};
Fake* fake=nullptr;
int index(XrSwapchain chain){const int n=int(reinterpret_cast<uintptr_t>(chain))-10;check(n>=0&&n<2,"swapchain handle");return n<0||n>1?0:n;}
XrResult outcome(const char* action,int eye){const std::string name=std::string(action)+std::to_string(eye);fake->trace.push_back(name);return fake->failure==name?fake->error:XR_SUCCESS;}
XrResult XRAPI_PTR formats(XrSession s,uint32_t cap,uint32_t* count,int64_t* out){
  check(s==session,"format session");++fake->formatCalls;
  *count=fake->noFormats?0:((fake->grow&&fake->formatCalls==1)?1:2);
  if(!cap)return XR_SUCCESS;if(cap<*count)return XR_ERROR_SIZE_INSUFFICIENT;
  out[0]=fake->unorm?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;out[1]=DXGI_FORMAT_D32_FLOAT;
  return XR_SUCCESS;
}
XrResult XRAPI_PTR create(XrSession s,const XrSwapchainCreateInfo* ci,XrSwapchain* out){
  const unsigned eye=fake->creates++;
  check(s==session&&eye<2&&ci->type==XR_TYPE_SWAPCHAIN_CREATE_INFO&&!ci->next,"create ABI");
  check(ci->width==size&&ci->height==size&&ci->arraySize==1&&ci->faceCount==1&&ci->mipCount==1&&ci->sampleCount==1&&
    ci->usageFlags==XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,"swapchain shape/usage");
  XrResult r=outcome("C",eye);if(r!=XR_SUCCESS)return r;
  auto& e=fake->eyes[eye];
  for(auto& image:e.images){
    D3D11_TEXTURE2D_DESC d{};d.Width=size+(fake->wrongDimensions?1:0);d.Height=size;d.MipLevels=d.ArraySize=1;
    d.Format=fake->typeless?DXGI_FORMAT_R8G8B8A8_TYPELESS:DXGI_FORMAT(ci->format);d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_RENDER_TARGET;
    ID3D11Device* dev=fake->wrongDevice?fake->foreign.Get():fake->device.Get();
    if(FAILED(dev->CreateTexture2D(&d,nullptr,&image)))return XR_ERROR_RUNTIME_FAILURE;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};rd.Format=DXGI_FORMAT(ci->format);rd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> target;check(SUCCEEDED(dev->CreateRenderTargetView(image.Get(),&rd,&target)),"fixture RTV");
    if(!fake->wrongDevice){const float sentinel[]={1,0,1,1};fake->context->ClearRenderTargetView(target.Get(),sentinel);}
  }
  e.alive=true;*out=reinterpret_cast<XrSwapchain>(uintptr_t(10+eye));return XR_SUCCESS;
}
XrResult XRAPI_PTR destroy(XrSwapchain chain){
  const int eye=index(chain);auto& e=fake->eyes[eye];check(e.alive,"destroy once");++fake->destroys;
  // No RTV may remain bound on the diagnostic context when a chain is destroyed.
  ComPtr<ID3D11RenderTargetView> bound;fake->context->OMGetRenderTargets(1,&bound,nullptr);check(!bound,"unbind before destroy");
  e.alive=false;for(auto& image:e.images)image.Reset();return outcome("D",eye);
}
XrResult XRAPI_PTR images(XrSwapchain chain,uint32_t cap,uint32_t* count,XrSwapchainImageBaseHeader* output){
  const int eye=index(chain);*count=2;if(!cap)return XR_SUCCESS;
  XrResult r=outcome("I",eye);if(r!=XR_SUCCESS)return r;
  check(cap>=2,"image capacity");auto* out=reinterpret_cast<XrSwapchainImageD3D11KHR*>(output);
  for(unsigned i=0;i<2;++i){check(out[i].type==XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR&&!out[i].next,"image structure initialization");out[i].texture=fake->eyes[eye].images[i].Get();}
  if(fake->badCount)*count=cap+1;return XR_SUCCESS;
}
XrResult XRAPI_PTR acquire(XrSwapchain chain,const XrSwapchainImageAcquireInfo* info,uint32_t* out){
  const int eye=index(chain);auto& e=fake->eyes[eye];check(info->type==XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO&&!info->next&&e.alive&&!e.acquired,"acquire order/ABI");
  XrResult r=outcome("A",eye);if(r!=XR_SUCCESS)return r;
  *out=e.index=e.calls++%2;e.acquired=true;return XR_SUCCESS;
}
XrResult XRAPI_PTR wait(XrSwapchain chain,const XrSwapchainImageWaitInfo* info){
  const int eye=index(chain);auto& e=fake->eyes[eye];check(info->type==XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO&&!info->next&&info->timeout>0&&info->timeout<=1000000000LL&&e.acquired&&!e.waited,"wait order/ABI");
  XrResult r=outcome("W",eye);if(r!=XR_SUCCESS)return r;e.waited=true;return XR_SUCCESS;
}
XrResult XRAPI_PTR release(XrSwapchain chain,const XrSwapchainImageReleaseInfo* info){
  const int eye=index(chain);auto& e=fake->eyes[eye];check(info->type==XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO&&!info->next&&e.acquired&&e.waited,"release requires successful wait");
  XrResult r=outcome("R",eye);if(r!=XR_SUCCESS)return r;e.waited=e.acquired=false;return XR_SUCCESS;
}
StereoDispatch dispatch(){return {formats,create,destroy,images,acquire,wait,release};}
struct Fixture {
  Fake runtime;D3D11Stereo renderer;XrViewConfigurationView sizes[2]{};XrView views[2]{};
  Fixture(){
    fake=&runtime;D3D_FEATURE_LEVEL level{};
    check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&runtime.device,&level,&runtime.context)),"WARP device");
    check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&runtime.foreign,&level,nullptr)),"foreign WARP device");
    for(unsigned i=0;i<2;++i){sizes[i]={XR_TYPE_VIEW_CONFIGURATION_VIEW};sizes[i].recommendedImageRectWidth=sizes[i].recommendedImageRectHeight=size;
      sizes[i].maxImageRectWidth=sizes[i].maxImageRectHeight=512;sizes[i].maxSwapchainSampleCount=1;
      views[i]={XR_TYPE_VIEW};views[i].pose.orientation.w=1;views[i].fov={-0.785398163f,0.785398163f,0.785398163f,-0.785398163f};}
  }
  ~Fixture(){renderer.shutdown();}
  XrResult init(){return renderer.initialize(dispatch(),session,runtime.device.Get(),sizes);}
  uint32_t pixel(unsigned eye,unsigned image,unsigned x,unsigned y){
    auto* source=runtime.eyes[eye].images[image].Get();D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> read;check(SUCCEEDED(runtime.device->CreateTexture2D(&desc,nullptr,&read)),"readback allocation");
    runtime.context->CopyResource(read.Get(),source);D3D11_MAPPED_SUBRESOURCE map{};
    if(FAILED(runtime.context->Map(read.Get(),0,D3D11_MAP_READ,0,&map))){check(false,"readback map");return 0;}
    uint32_t value=0;std::memcpy(&value,static_cast<const char*>(map.pData)+y*map.RowPitch+x*4,4);runtime.context->Unmap(read.Get(),0);return value;
  }
};
unsigned encode(double linear){return unsigned(std::lround((linear<=.0031308?12.92*linear:1.055*std::pow(linear,1/2.4)-.055)*255));}
void checkPixel(uint32_t pixel){
  // Independent ray/triangle barycentrics at sample (64.5,59.5) in a 128px view.
  const double b=.28125,g=.3854166666666667,r=1-b-g;
  const unsigned expected[]={encode(r),encode(g),encode(b),255};
  for(unsigned i=0;i<4;++i)check(std::abs(int((pixel>>(i*8))&255)-int(expected[i]))<=2,"independent RGB triangle sample");
}
void checkRayPixel(uint32_t pixel,unsigned x,unsigned y,double headX,double yaw,
                   double left=-1,double right=1,double up=1,double down=-1){
  // Trace an independent camera ray to world plane z=-2, then compute
  // triangle barycentrics. This catches inverse-pose and asymmetry signs.
  const double tx=left+(x+.5)/size*(right-left),ty=up-(y+.5)/size*(up-down);
  const double t=2/(std::sin(yaw)*tx+std::cos(yaw));
  const double wx=headX+t*(std::cos(yaw)*tx-std::sin(yaw)),wy=t*ty;
  const double b=wy/.5,g=(wx/.3+1-b)/2,r=1-b-g;
  check(r>0&&g>0&&b>0,"oracle ray intersects diagnostic triangle");
  const unsigned expected[]={encode(r),encode(g),encode(b),255};
  for(unsigned i=0;i<4;++i)check(std::abs(int((pixel>>(i*8))&255)-int(expected[i]))<=3,"independent transformed ray color");
}
int selfTest(){
  for(bool unorm:{false,true}){
    Fixture f;f.runtime.unorm=unorm;f.runtime.grow=true;check(f.init()==XR_SUCCESS,"initialize actual swapchains/RTVs/shaders");
    check(f.runtime.formatCalls==3,"growing format count retry");XrCompositionLayerProjection layer{};
    for(unsigned image=0;image<2;++image){
      f.runtime.trace.clear();check(f.renderer.render(f.views,space,layer)==XR_SUCCESS,"stereo render");
      check(f.runtime.trace==std::vector<std::string>({"A0","W0","R0","A1","W1","R1"}),"exact stereo image order");
      check(layer.space==space&&layer.viewCount==2&&layer.views,"projection layer contract");
      for(unsigned eye=0;eye<2;++eye){
        check(std::memcmp(&layer.views[eye].pose,&f.views[eye].pose,sizeof(XrPosef))==0&&std::memcmp(&layer.views[eye].fov,&f.views[eye].fov,sizeof(XrFovf))==0,"exact submitted pose/FOV");
        check(layer.views[eye].subImage.imageRect.extent.width==size&&layer.views[eye].subImage.imageArrayIndex==0,"full image metadata");
        checkPixel(f.pixel(eye,image,64,59));check(f.pixel(eye,image,0,0)==0xff000000,"defined opaque background");
      }
      if(image==0)check(f.pixel(0,1,0,0)==0xffff00ff,"unacquired image untouched");
    }
    f.views[0].pose.position.x=.2f;f.views[1].pose.orientation={0,float(std::sin(.1)),0,float(std::cos(.1))};
    check(f.renderer.render(f.views,space,layer)==XR_SUCCESS,"translated and rotated view");
    check(f.pixel(0,0,64,59)==0xff000000,"translation moves triangle from central ray");
    check(f.pixel(1,0,64,59)==0xff000000,"yaw moves triangle from central ray");
    checkRayPixel(f.pixel(0,0,58,59),58,59,.2,0);
    checkRayPixel(f.pixel(1,0,77,59),77,59,0,.2);
    for(auto& v:f.views){v.pose.position.x=0;v.pose.orientation={0,0,0,1};v.fov={float(std::atan(-.8)),float(std::atan(1.2)),float(std::atan(1.0)),float(std::atan(-.7))};}
    check(f.renderer.render(f.views,space,layer)==XR_SUCCESS,"asymmetric native FOV render");
    checkRayPixel(f.pixel(0,1,51,67),51,67,0,0,-.8,1.2,1,-.7);
    check(f.renderer.shutdown()==XR_SUCCESS&&f.runtime.destroys==2,"all swapchains destroyed");
  }
  for(const char* point:{"A0","W0","R0","A1","W1","R1"})for(XrResult error:{XR_ERROR_RUNTIME_FAILURE,XR_SESSION_LOSS_PENDING}){
    Fixture f;check(f.init()==XR_SUCCESS,"failure fixture init");f.runtime.failure=point;f.runtime.error=error;f.runtime.trace.clear();
    XrCompositionLayerProjection layer{};check(f.renderer.render(f.views,space,layer)==error,"exact runtime result retained");
    check(layer.viewCount==0&&!layer.views,"failed pair has no layer");
    const auto trace=f.runtime.trace;check(!trace.empty()&&trace.back()==point,"no calls after uncertain failure");
    check(f.renderer.render(f.views,space,layer)==error&&f.runtime.trace==trace,"failed renderer blocks retry");
  }
  for(const char* point:{"W0","W1"}){
    Fixture f;check(f.init()==XR_SUCCESS,"timeout fixture init");f.runtime.failure=point;f.runtime.error=XR_TIMEOUT_EXPIRED;f.runtime.trace.clear();
    XrCompositionLayerProjection layer{};check(f.renderer.render(f.views,space,layer)==XR_TIMEOUT_EXPIRED,"timeout positive preserved");
    check(f.runtime.trace.back()==point,"timeout never releases unwaited image");
    check(f.pixel(point[1]-'0',0,0,0)==0xffff00ff,"timeout image not drawn");
  }
  for(unsigned variant=0;variant<7;++variant){
    Fixture f;
    if(variant==0)f.runtime.noFormats=true;
    if(variant==1)f.runtime.wrongDimensions=true;
    if(variant==2)f.runtime.wrongDevice=true;
    if(variant==3)f.runtime.badCount=true;
    if(variant==4)f.runtime.failure="C1";
    if(variant==5)f.runtime.failure="I1";
    if(variant==6)f.sizes[1].recommendedImageRectHeight=0;
    check(f.init()!=XR_SUCCESS,"partial init rejects invalid contract");
    check(!f.runtime.eyes[0].alive&&!f.runtime.eyes[1].alive,"partial initialization destroys created chains");
  }
  {Fixture f;check(f.init()==XR_SUCCESS,"cleanup error fixture");f.runtime.failure="D0";
    check(f.renderer.shutdown()==XR_ERROR_RUNTIME_FAILURE&&f.runtime.destroys==2,"cleanup error reported, second chain attempted");}
  std::printf("openxr_stereo_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
}
int main(int argc,char** argv){
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
  if(argc!=2)return 2;if(!std::strcmp(argv[1],"--dry-run")){std::puts("Would test stereo rendering with fake XR and WARP; no files/runtime/device created.");return 0;}
  return !std::strcmp(argv[1],"--self-test")?selfTest():2;
}

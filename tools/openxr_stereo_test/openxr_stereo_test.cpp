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

ComPtr<ID3D11Texture2D> pattern(Fixture& f,bool bgra) {
  D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=4;d.MipLevels=d.ArraySize=1;
  d.Format=bgra?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;
  d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  uint32_t pixels[16]{};
  const uint32_t rgba[]={0xff0000ff,0xff00ff00,0xffff0000,0xff808080};
  for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x) {
    auto color=rgba[(y/2)*2+x/2];
    if(bgra)color=(color&0xff00ff00)|((color&255)<<16)|((color>>16)&255);
    pixels[y*4+x]=color;
  }
  D3D11_SUBRESOURCE_DATA data{pixels,16,0};ComPtr<ID3D11Texture2D> texture;
  check(SUCCEEDED(f.runtime.device->CreateTexture2D(&d,&data,&texture)),"pattern texture");return texture;
}
void pixelNear(uint32_t actual,uint32_t expected,const char* name) {
  bool match=true;for(unsigned c=0;c<4;++c)match=match&&std::abs(int((actual>>(8*c))&255)-int((expected>>(8*c))&255))<=2;
  check(match,name);
}
void capturedSelfTest() {
  for(bool unorm:{false,true})for(bool bgra:{false,true}) {
    Fixture f;f.runtime.unorm=unorm;check(f.init()==XR_SUCCESS,"captured initialize");
    EyeCapture capture;check(SUCCEEDED(capture.initialize(f.runtime.device.Get())),"captured owner");
    for(auto color:{vr::ColorSpace_Auto,vr::ColorSpace_Gamma,vr::ColorSpace_Linear})
      for(unsigned crop=0;crop<3;++crop) {
        auto source=pattern(f,bgra);if(!source)return;
        const vr::Texture_t texture{source.Get(),vr::API_DirectX,color};
        const vr::VRTextureBounds_t bounds[]={{0,0,1,1},{0,0,.5f,1},{.5f,1,0,0}};
        for(auto eye:{vr::Eye_Right,vr::Eye_Left})
          check(capture.capture(eye,&texture,&bounds[crop])==vr::VRCompositorError_None,"pattern capture");
        const uint32_t overwritten[16]{};
        f.runtime.context->UpdateSubresource(source.Get(),0,nullptr,overwritten,16,0);
        // Wrong inherited state must not disable the standalone diagnostic pass.
        D3D11_BLEND_DESC blend{};ComPtr<ID3D11BlendState> noWrites;
        check(SUCCEEDED(f.runtime.device->CreateBlendState(&blend,&noWrites)),"no write blend");
        f.runtime.context->OMSetBlendState(noWrites.Get(),nullptr,~0u);
        D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_WIREFRAME;rd.CullMode=D3D11_CULL_FRONT;
        ComPtr<ID3D11RasterizerState> wrongRaster;check(SUCCEEDED(f.runtime.device->CreateRasterizerState(&rd,&wrongRaster)),"wrong raster");
        f.runtime.context->RSSetState(wrongRaster.Get());
        XrCompositionLayerProjection layer{};f.runtime.trace.clear();
        const unsigned image=f.runtime.eyes[0].calls%2;
        check(f.renderer.renderCaptured(f.views,space,capture,layer)==XR_SUCCESS,"captured shader path");
        check(f.runtime.trace==std::vector<std::string>({"A0","W0","R0","A1","W1","R1"}),"captured image order");
        for(unsigned eye=0;eye<2;++eye)for(unsigned y:{16u,111u})for(unsigned x:{16u,111u,127u}) {
          unsigned quadrant=0;
          if(crop==0)quadrant=(y>64?2:0)+(x>64?1:0);
          if(crop==1)quadrant=y>64?2:0;
          if(crop==2)quadrant=y>64?0:2;
          const unsigned gray=color==vr::ColorSpace_Linear?encode(128.0/255):128;
          const uint32_t expected[]={0xff0000ff,0xff00ff00,0xffff0000,0xff000000|gray|(gray<<8)|(gray<<16)};
          pixelNear(f.pixel(eye,image,x,y),expected[quadrant],"RGBA/BGRA, crop/flip, gamma/linear and crop edge");
        }
      }
    capture.reset();f.runtime.trace.clear();XrCompositionLayerProjection layer{};
    check(f.renderer.renderCaptured(f.views,space,capture,layer)==XR_ERROR_VALIDATION_FAILURE&&f.runtime.trace.empty(),"missing pair no acquire");
    auto source=pattern(f,false);vr::Texture_t t{source.Get(),vr::API_DirectX,vr::ColorSpace_Auto};
    check(capture.capture(vr::Eye_Left,&t)==vr::VRCompositorError_None,"left only");
    check(f.renderer.renderCaptured(f.views,space,capture,layer)==XR_ERROR_VALIDATION_FAILURE&&f.runtime.trace.empty(),"missing right no acquire");
    check(capture.capture(vr::Eye_Right,&t)==vr::VRCompositorError_None,"right for validation");
    XrView invalid[2]={f.views[0],f.views[1]};invalid[1].pose.orientation.w=2;
    check(f.renderer.renderCaptured(invalid,space,capture,layer)==XR_ERROR_VALIDATION_FAILURE&&f.runtime.trace.empty(),"invalid second view no acquire");
    check(f.renderer.renderCaptured(f.views,XR_NULL_HANDLE,capture,layer)==XR_ERROR_HANDLE_INVALID&&f.runtime.trace.empty(),"invalid space no acquire");
  }
  for(bool unorm:{false,true}) {
    Fixture f;f.runtime.unorm=unorm;check(f.init()==XR_SUCCESS,"offscreen fixture");
    f.views[0].pose.position.x=.2f;f.views[1].pose.orientation={0,float(std::sin(.1)),0,float(std::cos(.1))};
    XrCompositionLayerProjection layer{};check(f.renderer.render(f.views,space,layer)==XR_SUCCESS,"direct reference scene");
    uint32_t reference[2]={f.pixel(0,0,58,59),f.pixel(1,0,77,59)};
    EyeCapture capture;check(SUCCEEDED(capture.initialize(f.runtime.device.Get())),"offscreen capture owner");
    for(unsigned eye=0;eye<2;++eye) {
      ID3D11Texture2D* source=nullptr;const auto trace=f.runtime.trace;
      check(f.renderer.drawEye(eye,f.views[eye],source)==XR_SUCCESS&&source&&f.runtime.trace==trace,"offscreen draw has no XR calls");
      vr::Texture_t t{source,vr::API_DirectX,vr::ColorSpace_Gamma};
      check(capture.capture(vr::EVREye(eye),&t)==vr::VRCompositorError_None,"offscreen eye copied");
    }
    check(f.renderer.renderCaptured(f.views,space,capture,layer)==XR_SUCCESS,"offscreen pair composed");
    pixelNear(f.pixel(0,1,58,59),reference[0],"offscreen translated view matches direct scene");
    pixelNear(f.pixel(1,1,77,59),reference[1],"offscreen rotated view matches direct scene");
    for(auto& v:f.views){v.pose.position.x=0;v.pose.orientation={0,0,0,1};v.fov={float(std::atan(-.8)),float(std::atan(1.2)),float(std::atan(1.0)),float(std::atan(-.7))};}
    for(unsigned eye=0;eye<2;++eye) {
      ID3D11Texture2D* source=nullptr;check(f.renderer.drawEye(eye,f.views[eye],source)==XR_SUCCESS,"asymmetric offscreen draw");
      vr::Texture_t t{source,vr::API_DirectX,vr::ColorSpace_Gamma};
      check(capture.capture(vr::EVREye(eye),&t)==vr::VRCompositorError_None,"asymmetric capture");
    }
    check(f.renderer.renderCaptured(f.views,space,capture,layer)==XR_SUCCESS,"asymmetric captured pair");
    checkRayPixel(f.pixel(0,0,51,67),51,67,0,0,-.8,1.2,1,-.7);
  }
  for(const char* point:{"A0","W0","R0","A1","W1","R1"})for(XrResult error:{XR_ERROR_RUNTIME_FAILURE,XR_SESSION_LOSS_PENDING,XR_TIMEOUT_EXPIRED}) {
    if(error==XR_TIMEOUT_EXPIRED&&point[0]!='W')continue;
    Fixture f;check(f.init()==XR_SUCCESS,"captured failure fixture");EyeCapture capture;
    check(SUCCEEDED(capture.initialize(f.runtime.device.Get())),"captured failure owner");
    auto source=pattern(f,false);vr::Texture_t t{source.Get(),vr::API_DirectX,vr::ColorSpace_Auto};
    check(capture.capture(vr::Eye_Left,&t)==vr::VRCompositorError_None&&capture.capture(vr::Eye_Right,&t)==vr::VRCompositorError_None,"failure fixture captures");
    f.runtime.failure=point;f.runtime.error=error;f.runtime.trace.clear();XrCompositionLayerProjection layer{};
    check(f.renderer.renderCaptured(f.views,space,capture,layer)==error&&!layer.views&&!layer.viewCount,"captured failure never publishes partial pair");
    const auto trace=f.runtime.trace;
    check(!trace.empty()&&trace.back()==point,"captured failure stops exact operation");
    check(f.renderer.renderCaptured(f.views,space,capture,layer)==error&&f.runtime.trace==trace,"captured failure blocks retry");
    if(point[0]=='W')check(f.pixel(point[1]-'0',0,0,0)==0xffff00ff,"unwaited captured target untouched");
  }
}
struct SkyRay { double x,y,z; };
SkyRay crossSky(SkyRay a,SkyRay b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
double dotSky(SkyRay a,SkyRay b){return a.x*b.x+a.y*b.y+a.z*b.z;}
SkyRay rotateSky(const XrQuaternionf& q,SkyRay p) {
  // Quaternion-vector product, independent of the renderer's matrix formula.
  const SkyRay v{q.x,q.y,q.z},a=crossSky(v,p),b=crossSky(v,a);
  return {p.x+2*(q.w*a.x+b.x),p.y+2*(q.w*a.y+b.y),p.z+2*(q.w*a.z+b.z)};
}
XrQuaternionf productSky(XrQuaternionf a,XrQuaternionf b) {
  return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
          a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
const SkyRay skyForward[6]={{0,0,-1},{0,0,1},{-1,0,0},{1,0,0},{0,1,0},{0,-1,0}};
const SkyRay skyRight[6]={{1,0,0},{-1,0,0},{0,0,-1},{0,0,1},{1,0,0},{1,0,0}};
const SkyRay skyUp[6]={{0,1,0},{0,1,0},{0,1,0},{0,1,0},{0,0,1},{0,0,-1}};
unsigned skyChannel(unsigned face,unsigned x,unsigned y,unsigned channel) {
  if(channel==0)return 12+face*9+x*13;
  if(channel==1)return 7+face*11+y*17;
  return 3+face*14+x*5+y*7;
}
vr::EColorSpace skyColor(unsigned face){return vr::EColorSpace(face%3);}
void captureSky(Fixture& f,SkyboxCapture& capture) {
  check(SUCCEEDED(capture.initialize(f.runtime.device.Get())),"skybox owner");
  ComPtr<ID3D11Texture2D> faces[6];vr::Texture_t textures[6]{};
  for(unsigned face=0;face<6;++face) {
    D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=8;d.MipLevels=d.ArraySize=1;
    d.Format=face&1?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;
    d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    uint32_t pixels[64]{};
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x) {
      const auto r=skyChannel(face,x,y,0),g=skyChannel(face,x,y,1),b=skyChannel(face,x,y,2);
      pixels[y*8+x]=0xff000000u|(face&1?b:r)|(g<<8)|((face&1?r:b)<<16);
    }
    D3D11_SUBRESOURCE_DATA data{pixels,32,0};
    check(SUCCEEDED(f.runtime.device->CreateTexture2D(&d,&data,&faces[face])),"asymmetric skybox source");
    textures[face]={faces[face].Get(),vr::API_DirectX,skyColor(face)};
  }
  check(capture.set(textures,6)==vr::VRCompositorError_None,"six mixed-format skybox faces copied");
  uint32_t black[64]{};for(auto& source:faces)f.runtime.context->UpdateSubresource(source.Get(),0,nullptr,black,32,0);
  // Local COM references disappear here; only private copies may render.
}
uint32_t skyOracle(const XrView& view,unsigned x,unsigned y,unsigned* chosen=nullptr) {
  const auto& f=view.fov;
  const double left=std::tan(f.angleLeft),right=std::tan(f.angleRight),up=std::tan(f.angleUp),down=std::tan(f.angleDown);
  const SkyRay ray=rotateSky(view.pose.orientation,{left+(x+.5)/size*(right-left),up+(y+.5)/size*(down-up),-1});
  unsigned face=0;double distance=-1;
  // Intersect the six independent face planes, not the shader's axis branches.
  for(unsigned i=0;i<6;++i){const double candidate=dotSky(ray,skyForward[i]);if(candidate>distance){distance=candidate;face=i;}}
  if(chosen)*chosen=face;
  const double u=.5+.5*dotSky(ray,skyRight[face])/distance,v=.5-.5*dotSky(ray,skyUp[face])/distance;
  const double sx=(std::max)(0.,(std::min)(7.,u*8-.5)),sy=(std::max)(0.,(std::min)(7.,v*8-.5));
  const unsigned x0=unsigned(sx),y0=unsigned(sy),x1=(std::min)(7u,x0+1),y1=(std::min)(7u,y0+1);
  const double tx=sx-x0,ty=sy-y0;
  uint32_t expected=0xff000000u;
  for(unsigned c=0;c<3;++c) {
    auto linear=[&](unsigned px,unsigned py){
      const double n=skyChannel(face,px,py,c)/255.;
      return skyColor(face)==vr::ColorSpace_Linear?n:n<=.04045?n/12.92:std::pow((n+.055)/1.055,2.4);
    };
    const double top=linear(x0,y0)*(1-tx)+linear(x1,y0)*tx,bottom=linear(x0,y1)*(1-tx)+linear(x1,y1)*tx;
    expected|=encode(top*(1-ty)+bottom*ty)<<(8*c);
  }
  return expected;
}
void skyboxSelfTest() {
  const float h=float(std::sqrt(.5));
  const XrQuaternionf cardinal[6]={{0,0,0,1},{0,1,0,0},{0,h,0,h},{0,-h,0,h},{h,0,0,h},{-h,0,0,h}};
  for(bool unorm:{false,true}) {
    Fixture f;f.runtime.unorm=unorm;
    if(f.init()!=XR_SUCCESS){check(false,"skybox renderer initializes");return;}
    SkyboxCapture capture;captureSky(f,capture);
    unsigned faceHits[6]{};
    for(unsigned direction=0;direction<6;++direction) {
      for(unsigned eye=0;eye<2;++eye) {
        auto& view=f.views[eye];view.pose.orientation=cardinal[direction];
        if(eye)view.pose.orientation=productSky(cardinal[direction],productSky({0,float(std::sin(.09)),0,float(std::cos(.09))},{0,0,float(std::sin(.07)),float(std::cos(.07))}));
        view.fov={float(std::atan(-.83)),float(std::atan(1.13)),float(std::atan(.91)),float(std::atan(-.67))};
        view.pose.position={float(eye?12:-17),3,-8};
      }
      XrCompositionLayerProjection layer{};f.runtime.trace.clear();const auto image=f.runtime.eyes[0].calls%2;
      const auto result=f.renderer.renderSkybox(f.views,space,capture,layer);
      check(result==XR_SUCCESS,"actual skybox shader renders");
      if(result!=XR_SUCCESS)return;
      check(f.runtime.trace==std::vector<std::string>({"A0","W0","R0","A1","W1","R1"}),"skybox exact image order");
      check(layer.viewCount==2&&layer.views&&layer.space==space,"skybox complete projection layer");
      for(unsigned eye=0;eye<2;++eye) {
        check(!std::memcmp(&layer.views[eye].pose,&f.views[eye].pose,sizeof(XrPosef))&&
              !std::memcmp(&layer.views[eye].fov,&f.views[eye].fov,sizeof(XrFovf)),"skybox retains native pose and FOV");
        for(unsigned y:{13u,48u,99u})for(unsigned x:{9u,35u,64u,105u}) {
          unsigned face=0;const auto expected=skyOracle(f.views[eye],x,y,&face);++faceHits[face];
          pixelNear(f.pixel(eye,image,x,y),expected,"independent skybox face/UV, cant, asymmetric FOV and color oracle");
        }
      }
      uint32_t before[2]={f.pixel(0,image,35,48),f.pixel(1,image,35,48)};
      for(auto& view:f.views)view.pose.position={100,-40,52};
      const auto nextImage=f.runtime.eyes[0].calls%2;
      check(f.renderer.renderSkybox(f.views,space,capture,layer)==XR_SUCCESS,"translated infinite skybox");
      for(unsigned eye=0;eye<2;++eye)pixelNear(f.pixel(eye,nextImage,35,48),before[eye],"skybox ignores translation");
    }
    for(unsigned hits:faceHits)check(hits>0,"each of six faces independently sampled");
    XrCompositionLayerProjection layer{};f.runtime.trace.clear();XrView bad[2]={f.views[0],f.views[1]};
    bad[1].pose.orientation.w=2;
    check(f.renderer.renderSkybox(bad,space,capture,layer)==XR_ERROR_VALIDATION_FAILURE&&f.runtime.trace.empty(),"skybox invalid second view before acquire");
    bad[1]=f.views[1];bad[1].fov.angleLeft=bad[1].fov.angleRight;
    check(f.renderer.renderSkybox(bad,space,capture,layer)==XR_ERROR_VALIDATION_FAILURE&&f.runtime.trace.empty(),"skybox invalid FOV before acquire");
    check(f.renderer.renderSkybox(f.views,XR_NULL_HANDLE,capture,layer)==XR_ERROR_HANDLE_INVALID&&f.runtime.trace.empty(),"skybox null space before acquire");
    capture.clear();
    check(f.renderer.renderSkybox(f.views,space,capture,layer)==XR_ERROR_VALIDATION_FAILURE&&f.runtime.trace.empty(),"cleared skybox before acquire");
  }
  // A fixture owns the global fake dispatch until its destructor completes.
  // Keep failure fixtures outside the successful fixture's lifetime.
  for(const char* point:{"A0","W0","R0","A1","W1","R1"})for(XrResult error:{XR_ERROR_RUNTIME_FAILURE,XR_SESSION_LOSS_PENDING,XR_TIMEOUT_EXPIRED}) {
    if(error==XR_TIMEOUT_EXPIRED&&point[0]!='W')continue;
    Fixture f;check(f.init()==XR_SUCCESS,"skybox failure fixture");SkyboxCapture capture;captureSky(f,capture);
    f.runtime.failure=point;f.runtime.error=error;f.runtime.trace.clear();XrCompositionLayerProjection layer{};
    check(f.renderer.renderSkybox(f.views,space,capture,layer)==error&&!layer.views&&!layer.viewCount,"skybox failure never publishes partial layer");
    const auto trace=f.runtime.trace;
    check(!trace.empty()&&trace.back()==point,"skybox stops at exact failed operation");
    check(f.renderer.renderSkybox(f.views,space,capture,layer)==error&&f.runtime.trace==trace,"skybox failed operation never retried");
    if(point[0]=='W')check(f.pixel(point[1]-'0',0,0,0)==0xffff00ff,"skybox timeout target remains untouched");
  }
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
  capturedSelfTest();
  skyboxSelfTest();
  std::printf("openxr_stereo_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
}
int main(int argc,char** argv){
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
  if(argc!=2)return 2;if(!std::strcmp(argv[1],"--dry-run")){std::puts("Would test stereo rendering with fake XR and WARP; no files/runtime/device created.");return 0;}
  return !std::strcmp(argv[1],"--self-test")?selfTest():2;
}

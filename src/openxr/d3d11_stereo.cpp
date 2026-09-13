#include "d3d11_stereo.h"
#include "projection_math.h"
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace edvr::openxr {
namespace {
using Microsoft::WRL::ComPtr;
struct Vertex { float position[3], color[4]; };
struct Constants { float matrix[4][4]; float encodeSRGB=0, pad[3]{}; };
struct BlitConstants { float bounds[4], clampUV[4]; float encodeSRGB=0, pad[3]{}; };
struct SkyConstants { float orient[3][4]; float tangents[4]; float encode=0, pad[3]{}; };
static_assert(sizeof(SkyConstants)==80,"skybox HLSL constant-buffer packing");
const char* shader=R"(
cbuffer Constants : register(b0) { row_major float4x4 mvp; float encodeSRGB; float3 pad; };
struct V { float3 position:POSITION; float4 color:COLOR; };
struct O { float4 position:SV_POSITION; float4 color:COLOR; };
O vs(V v) { O o; o.position=mul(mvp,float4(v.position,1)); o.color=v.color; return o; }
float3 srgb(float3 c) { return lerp(12.92*c,1.055*pow(max(c,0),1.0/2.4)-0.055,step(0.0031308,c)); }
float4 ps(O v):SV_TARGET { return float4(encodeSRGB>0.5?srgb(v.color.rgb):v.color.rgb,1); }
)";
const char* blitShader=R"(
cbuffer Constants : register(b0) { float4 bounds; float4 clampUV; float encodeSRGB; float3 pad; };
Texture2D sourceTexture : register(t0); SamplerState linearSampler : register(s0);
struct O { float4 position:SV_POSITION; float2 uv:TEXCOORD; };
O vs(uint id:SV_VertexID) { O o; float2 p=float2((id<<1)&2,id&2); o.position=float4(p*float2(2,-2)+float2(-1,1),0,1); o.uv=p; return o; }
float3 srgb(float3 c) { return lerp(12.92*c,1.055*pow(max(c,0),1.0/2.4)-0.055,step(0.0031308,c)); }
float4 ps(O v):SV_TARGET { float2 uv=clamp(bounds.xy+v.uv*(bounds.zw-bounds.xy),clampUV.xy,clampUV.zw); float4 c=sourceTexture.Sample(linearSampler,uv); return float4(encodeSRGB>0.5?srgb(c.rgb):c.rgb,c.a); }
)";
const char* skyboxShader=R"(
cbuffer C:register(b0) {
  row_major float3x4 orient;
  float4 tangents; // left, right, up, down
  float encode; float3 pad;
};
Texture2D f0:register(t0); Texture2D f1:register(t1);
Texture2D f2:register(t2); Texture2D f3:register(t3);
Texture2D f4:register(t4); Texture2D f5:register(t5);
SamplerState s:register(s0);
struct O { float4 p:SV_POSITION; float2 uv:TEXCOORD; };
O vs(uint id:SV_VertexID) {
  O o; float2 q=float2((id<<1)&2,id&2);
  o.p=float4(q*float2(2,-2)+float2(-1,1),0,1); o.uv=q; return o;
}
float3 enc(float3 c) {
  return lerp(12.92*c,1.055*pow(max(c,0),1.0/2.4)-0.055,step(0.0031308,c));
}
float4 ps(O o):SV_TARGET {
  float3 q=float3(lerp(tangents.x,tangents.y,o.uv.x),lerp(tangents.z,tangents.w,o.uv.y),-1);
  float3 r=mul(orient,float4(q,0)); float3 a=abs(r); int k;
  if(a.x>a.y && a.x>a.z) k=r.x>0?3:2;
  else if(a.y>a.z) k=r.y>0?4:5;
  else k=r.z>0?1:0;
  // Front/back/left/right/top/bottom, top-left image coordinates.
  float2 uv;
  if(k==0) uv=float2(.5+.5*r.x/(-r.z),.5-.5*r.y/(-r.z));
  else if(k==1) uv=float2(.5-.5*r.x/r.z,.5-.5*r.y/r.z);
  else if(k==2) uv=float2(.5-.5*r.z/(-r.x),.5-.5*r.y/(-r.x));
  else if(k==3) uv=float2(.5+.5*r.z/r.x,.5-.5*r.y/r.x);
  else if(k==4) uv=float2(.5+.5*r.x/r.y,.5-.5*r.z/r.y);
  else uv=float2(.5+.5*r.x/(-r.y),.5+.5*r.z/(-r.y));
  // SRVs decode Gamma/Auto before filtering. Each face clamps at its edge;
  // this path does not promise seamless cubemap filtering.
  float4 c=k==0?f0.Sample(s,uv):k==1?f1.Sample(s,uv):k==2?f2.Sample(s,uv):
           k==3?f3.Sample(s,uv):k==4?f4.Sample(s,uv):f5.Sample(s,uv);
  return float4(encode>0.5?enc(c.rgb):c.rgb,c.a);
}
)";
template<class T,class F> XrResult enumerate(F call,std::vector<T>& out,T initial=T{}) {
  out.clear();uint32_t n=0;XrResult r=call(0,&n,nullptr);if(r!=XR_SUCCESS||!n)return r;
  for(unsigned attempt=0;attempt<3;++attempt) {
    if(n>256)return XR_ERROR_LIMIT_REACHED;
    std::vector<T> values(n,initial);uint32_t count=0;r=call(n,&count,values.data());
    if(r==XR_ERROR_SIZE_INSUFFICIENT && count>n){n=count;continue;}
    if(r!=XR_SUCCESS)return r;if(count>n)return XR_ERROR_RUNTIME_FAILURE;
    values.resize(count);out.swap(values);return XR_SUCCESS;
  }
  return XR_ERROR_SIZE_INSUFFICIENT;
}
bool poseValid(const XrPosef& p) {
  const float a[]={p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w,p.position.x,p.position.y,p.position.z};
  for(float v:a)if(!std::isfinite(v))return false;
  const auto& q=p.orientation;const double norm=double(q.x)*q.x+double(q.y)*q.y+double(q.z)*q.z+double(q.w)*q.w;
  return std::abs(norm-1.0)<0.001;
}
void viewMatrix(const XrPosef& p,float (&m)[4][4]) {
  const float x=p.orientation.x,y=p.orientation.y,z=p.orientation.z,w=p.orientation.w;
  // Inverse rigid pose, column vectors: R^T and -R^T t.
  m[0][0]=1-2*(y*y+z*z);m[0][1]=2*(x*y+z*w);m[0][2]=2*(x*z-y*w);
  m[1][0]=2*(x*y-z*w);m[1][1]=1-2*(x*x+z*z);m[1][2]=2*(y*z+x*w);
  m[2][0]=2*(x*z+y*w);m[2][1]=2*(y*z-x*w);m[2][2]=1-2*(x*x+y*y);
  for(unsigned row=0;row<3;++row)m[row][3]=-(m[row][0]*p.position.x+m[row][1]*p.position.y+m[row][2]*p.position.z);
  m[3][3]=1;
}
bool compatible(DXGI_FORMAT resource,DXGI_FORMAT selected) {
  if(resource==selected)return true;
  if(resource==DXGI_FORMAT_R8G8B8A8_TYPELESS)
    return selected==DXGI_FORMAT_R8G8B8A8_UNORM || selected==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  if(resource==DXGI_FORMAT_B8G8R8A8_TYPELESS)
    return selected==DXGI_FORMAT_B8G8R8A8_UNORM || selected==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  return false;
}
bool sameDevice(ID3D11Device* device,ID3D11Texture2D* texture) {
  ComPtr<ID3D11Device> owner;texture->GetDevice(&owner);
  ComPtr<IUnknown> a,b;
  return SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&a)))&&SUCCEEDED(owner.As(&b))&&a.Get()==b.Get();
}
void skyRotation(const XrQuaternionf& q,float (&r)[3][4]) {
  const float x=q.x,y=q.y,z=q.z,w=q.w;
  r[0][0]=1-2*(y*y+z*z);r[0][1]=2*(x*y-z*w);r[0][2]=2*(x*z+y*w);
  r[1][0]=2*(x*y+z*w);r[1][1]=1-2*(x*x+z*z);r[1][2]=2*(y*z-x*w);
  r[2][0]=2*(x*z-y*w);r[2][1]=2*(y*z+x*w);r[2][2]=1-2*(x*x+y*y);
}
}

D3D11Stereo::~D3D11Stereo(){shutdown();}
XrResult D3D11Stereo::shutdown() {
  XrResult first=XR_SUCCESS;ready_=false;
  if(context_){context_->ClearState();context_->Flush();}
  constants_.Reset();blitConstants_.Reset();blitSampler_.Reset();skyboxConstants_.Reset();skyboxSampler_.Reset();vertices_.Reset();layout_.Reset();pixelShader_.Reset();vertexShader_.Reset();blitPixelShader_.Reset();blitVertexShader_.Reset();skyboxPixelShader_.Reset();skyboxVertexShader_.Reset();rasterizer_.Reset();depth_.Reset();
  for(auto& eye:eyes_){
    eye.diagnosticRtv.Reset();eye.diagnosticTexture.Reset();eye.rtvs.clear();eye.images.clear();
    if(eye.swapchain && dispatch_.destroySwapchain){const XrResult r=dispatch_.destroySwapchain(eye.swapchain);if(r!=XR_SUCCESS && first==XR_SUCCESS)first=r;}
    eye.swapchain=XR_NULL_HANDLE;eye.width=eye.height=0;
  }
  context_.Reset();device_.Reset();session_=XR_NULL_HANDLE;format_=0;dispatch_={};lastResult_=XR_SUCCESS;
  for(auto& view:layerViews_)view={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
  return first;
}
XrResult D3D11Stereo::initialize(const StereoDispatch& d,XrSession session,ID3D11Device* device,
                               const XrViewConfigurationView (&views)[2]) {
  const XrResult closed=shutdown();if(closed!=XR_SUCCESS)return closed;
  if(!session||!device)return XR_ERROR_HANDLE_INVALID;
  if(!d.enumerateSwapchainFormats||!d.createSwapchain||!d.destroySwapchain||!d.enumerateSwapchainImages||
     !d.acquireSwapchainImage||!d.waitSwapchainImage||!d.releaseSwapchainImage)return XR_ERROR_FUNCTION_UNSUPPORTED;
  for(const auto& v:views)if(!v.recommendedImageRectWidth||!v.recommendedImageRectHeight||
    v.recommendedImageRectWidth>v.maxImageRectWidth||v.recommendedImageRectHeight>v.maxImageRectHeight||
    v.recommendedImageRectWidth>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION||v.recommendedImageRectHeight>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION||
    v.maxSwapchainSampleCount<1)return XR_ERROR_VALIDATION_FAILURE;
  if(device->GetFeatureLevel()<D3D_FEATURE_LEVEL_11_0)return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  dispatch_=d;session_=session;device_=device;device_->GetImmediateContext(&context_);
  auto failed=[&](XrResult r){shutdown();return r;};
  std::vector<int64_t> formats;
  XrResult r=enumerate<int64_t>([&](uint32_t c,uint32_t*n,int64_t*p){return d.enumerateSwapchainFormats(session,c,n,p);},formats);
  if(r!=XR_SUCCESS)return failed(r);
  const DXGI_FORMAT candidates[]={DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM};
  for(auto candidate:candidates){
    if(std::find(formats.begin(),formats.end(),int64_t(candidate))==formats.end())continue;
    UINT support=0;if(SUCCEEDED(device->CheckFormatSupport(candidate,&support)) &&
      (support&(D3D11_FORMAT_SUPPORT_TEXTURE2D|D3D11_FORMAT_SUPPORT_RENDER_TARGET))==(D3D11_FORMAT_SUPPORT_TEXTURE2D|D3D11_FORMAT_SUPPORT_RENDER_TARGET)){
      format_=candidate;break;
    }
  }
  if(!format_)return failed(XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED);
  for(unsigned i=0;i<2;++i){
    auto& eye=eyes_[i];XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;ci.format=format_;ci.sampleCount=1;
    ci.width=views[i].recommendedImageRectWidth;ci.height=views[i].recommendedImageRectHeight;ci.faceCount=ci.arraySize=ci.mipCount=1;
    r=d.createSwapchain(session,&ci,&eye.swapchain);if(r!=XR_SUCCESS)return failed(r);
    if(!eye.swapchain)return failed(XR_ERROR_RUNTIME_FAILURE);
    std::vector<XrSwapchainImageD3D11KHR> images;
    r=enumerate<XrSwapchainImageD3D11KHR>([&](uint32_t c,uint32_t*n,XrSwapchainImageD3D11KHR*p){
      return d.enumerateSwapchainImages(eye.swapchain,c,n,reinterpret_cast<XrSwapchainImageBaseHeader*>(p));},images,{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if(r!=XR_SUCCESS)return failed(r);if(images.empty())return failed(XR_ERROR_RUNTIME_FAILURE);
    eye.width=ci.width;eye.height=ci.height;
    for(const auto& image:images){
      if(!image.texture||!sameDevice(device,image.texture))return failed(XR_ERROR_GRAPHICS_DEVICE_INVALID);
      D3D11_TEXTURE2D_DESC desc{};image.texture->GetDesc(&desc);
      if(desc.Width!=ci.width||desc.Height!=ci.height||desc.ArraySize!=1||desc.MipLevels!=1||desc.SampleDesc.Count!=1||desc.SampleDesc.Quality!=0||
         !(desc.BindFlags&D3D11_BIND_RENDER_TARGET)||!compatible(desc.Format,DXGI_FORMAT(format_)))return failed(XR_ERROR_VALIDATION_FAILURE);
      D3D11_RENDER_TARGET_VIEW_DESC rd{};rd.Format=DXGI_FORMAT(format_);rd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
      ComPtr<ID3D11RenderTargetView> rtv;
      if(FAILED(device->CreateRenderTargetView(image.texture,&rd,&rtv)))return failed(XR_ERROR_RUNTIME_FAILURE);
      eye.images.push_back(image.texture);eye.rtvs.push_back(std::move(rtv));
    }
  }
  ComPtr<ID3DBlob> vs,ps,errors,nativeVS;
  if(FAILED(D3DCompile(shader,std::strlen(shader),"EDVR native stereo",nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&vs,&errors))||
     FAILED(D3DCompile(shader,std::strlen(shader),"EDVR native stereo",nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&ps,&errors))||
     FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertexShader_))||
     FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixelShader_)))return failed(XR_ERROR_RUNTIME_FAILURE);
  nativeVS=vs;
  vs.Reset(); ps.Reset(); errors.Reset();
  if(FAILED(D3DCompile(blitShader,std::strlen(blitShader),"EDVR captured blit",nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&vs,&errors))||
     FAILED(D3DCompile(blitShader,std::strlen(blitShader),"EDVR captured blit",nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&ps,&errors))||
     FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&blitVertexShader_))||
     FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&blitPixelShader_)))return failed(XR_ERROR_RUNTIME_FAILURE);
  D3D11_INPUT_ELEMENT_DESC layout[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
    {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}};
  if(FAILED(device->CreateInputLayout(layout,2,nativeVS->GetBufferPointer(),nativeVS->GetBufferSize(),&layout_)))return failed(XR_ERROR_RUNTIME_FAILURE);
  const Vertex vertices[]={{{-.3f,0,-2},{1,0,0,1}},{{.3f,0,-2},{0,1,0,1}},{{0,.5f,-2},{0,0,1,1}}};
  D3D11_BUFFER_DESC bd{sizeof(vertices),D3D11_USAGE_IMMUTABLE,D3D11_BIND_VERTEX_BUFFER,0,0,0};D3D11_SUBRESOURCE_DATA data{vertices,0,0};
  if(FAILED(device->CreateBuffer(&bd,&data,&vertices_)))return failed(XR_ERROR_RUNTIME_FAILURE);
  bd={sizeof(Constants),D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER,0,0,0};
  if(FAILED(device->CreateBuffer(&bd,nullptr,&constants_)))return failed(XR_ERROR_RUNTIME_FAILURE);
  bd={sizeof(BlitConstants),D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER,0,0,0};
  if(FAILED(device->CreateBuffer(&bd,nullptr,&blitConstants_)))return failed(XR_ERROR_RUNTIME_FAILURE);
  D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
  if(FAILED(device->CreateSamplerState(&sampler,&blitSampler_)))return failed(XR_ERROR_RUNTIME_FAILURE);
  if(FAILED(D3DCompile(skyboxShader,std::strlen(skyboxShader),"EDVR skybox",nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&vs,&errors))||
     FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&skyboxVertexShader_))||
     FAILED(D3DCompile(skyboxShader,std::strlen(skyboxShader),"EDVR skybox",nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&ps,&errors))||
     FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&skyboxPixelShader_)))return failed(XR_ERROR_RUNTIME_FAILURE);
  bd={sizeof(SkyConstants),D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER,0,0,0};
  if(FAILED(device->CreateBuffer(&bd,nullptr,&skyboxConstants_)))return failed(XR_ERROR_RUNTIME_FAILURE);
  if(FAILED(device->CreateSamplerState(&sampler,&skyboxSampler_)))return failed(XR_ERROR_RUNTIME_FAILURE);
  D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
  D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthEnable=FALSE;
  if(FAILED(device->CreateRasterizerState(&raster,&rasterizer_))||FAILED(device->CreateDepthStencilState(&depth,&depth_)))return failed(XR_ERROR_RUNTIME_FAILURE);
  ready_=true;return XR_SUCCESS;
}
XrResult D3D11Stereo::render(const XrView (&views)[2],XrSpace space,XrCompositionLayerProjection& layer) {
  layer={XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  if(!ready_)return lastResult_==XR_SUCCESS?XR_ERROR_CALL_ORDER_INVALID:lastResult_;
  auto failed=[&](XrResult r){ready_=false;lastResult_=r;return r;};
  if(!space)return failed(XR_ERROR_HANDLE_INVALID);
  if(FAILED(device_->GetDeviceRemovedReason()))return failed(XR_ERROR_GRAPHICS_DEVICE_INVALID);
  Constants constants[2]{};
  for(unsigned eye=0;eye<2;++eye){
    vr::HmdMatrix44_t projection{};float view[4][4]{};
    if(!poseValid(views[eye].pose)||!projectionMatrix(views[eye].fov,.025f,50000.f,vr::API_DirectX,projection))return failed(XR_ERROR_VALIDATION_FAILURE);
    viewMatrix(views[eye].pose,view);
    for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col)for(unsigned k=0;k<4;++k)
      constants[eye].matrix[row][col]+=projection.m[row][k]*view[k][col];
    constants[eye].encodeSRGB=(format_==DXGI_FORMAT_R8G8B8A8_UNORM||format_==DXGI_FORMAT_B8G8R8A8_UNORM)?1.f:0.f;
  }
  for(unsigned eye=0;eye<2;++eye){
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};uint32_t index=0;
    XrResult r=dispatch_.acquireSwapchainImage(eyes_[eye].swapchain,&acquire,&index);if(r!=XR_SUCCESS)return failed(r);
    if(index>=eyes_[eye].rtvs.size())return failed(XR_ERROR_RUNTIME_FAILURE);
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=1000000000LL;
    r=dispatch_.waitSwapchainImage(eyes_[eye].swapchain,&wait);
    // Positive timeout is not permission to draw/release. Retire this renderer;
    // the owner destroys the swapchain without retrying an uncertain operation.
    if(r!=XR_SUCCESS)return failed(r);
    auto& e=eyes_[eye];ID3D11RenderTargetView* rtv=e.rtvs[index].Get();
    const float black[]={0,0,0,1};context_->ClearRenderTargetView(rtv,black);
    context_->OMSetRenderTargets(1,&rtv,nullptr);context_->OMSetBlendState(nullptr,nullptr,~0u);context_->OMSetDepthStencilState(depth_.Get(),0);
    context_->RSSetState(rasterizer_.Get());D3D11_VIEWPORT viewport{0,0,float(e.width),float(e.height),0,1};context_->RSSetViewports(1,&viewport);
    UINT stride=sizeof(Vertex),offset=0;context_->IASetInputLayout(layout_.Get());
    context_->IASetVertexBuffers(0,1,vertices_.GetAddressOf(),&stride,&offset);context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(),nullptr,0);context_->GSSetShader(nullptr,nullptr,0);context_->HSSetShader(nullptr,nullptr,0);context_->DSSetShader(nullptr,nullptr,0);
    context_->VSSetConstantBuffers(0,1,constants_.GetAddressOf());context_->PSSetConstantBuffers(0,1,constants_.GetAddressOf());context_->PSSetShader(pixelShader_.Get(),nullptr,0);
    context_->UpdateSubresource(constants_.Get(),0,nullptr,&constants[eye],0,0);context_->Draw(3,0);
    context_->OMSetRenderTargets(0,nullptr,nullptr);context_->Flush();
    if(FAILED(device_->GetDeviceRemovedReason()))return failed(XR_ERROR_GRAPHICS_DEVICE_INVALID);
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    r=dispatch_.releaseSwapchainImage(e.swapchain,&release);if(r!=XR_SUCCESS)return failed(r);
  }
  for(unsigned eye=0;eye<2;++eye){
    auto& view=layerViews_[eye];view={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};view.pose=views[eye].pose;view.fov=views[eye].fov;
    view.subImage={eyes_[eye].swapchain,{{0,0},{int32_t(eyes_[eye].width),int32_t(eyes_[eye].height)}},0};
  }
  layer.space=space;layer.viewCount=2;layer.views=layerViews_;return XR_SUCCESS;
}

XrResult D3D11Stereo::drawEye(unsigned eye, const XrView& view, ID3D11Texture2D*& out) {
  out=nullptr;
  if(!ready_)return lastResult_==XR_SUCCESS?XR_ERROR_CALL_ORDER_INVALID:lastResult_;
  if(eye>1)return XR_ERROR_INDEX_OUT_OF_RANGE;
  if(view.type!=XR_TYPE_VIEW||view.next||!poseValid(view.pose))return XR_ERROR_VALIDATION_FAILURE;
  if(FAILED(device_->GetDeviceRemovedReason()))return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  vr::HmdMatrix44_t projection{};float viewMatrixData[4][4]{};
  if(!projectionMatrix(view.fov,.025f,50000.f,vr::API_DirectX,projection))return XR_ERROR_VALIDATION_FAILURE;
  auto& e=eyes_[eye];
  if(!e.diagnosticTexture) {
    D3D11_TEXTURE2D_DESC td{};td.Width=e.width;td.Height=e.height;td.MipLevels=td.ArraySize=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;td.SampleDesc.Count=1;
    td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11RenderTargetView> target;
    if(FAILED(device_->CreateTexture2D(&td,nullptr,&texture)))return XR_ERROR_RUNTIME_FAILURE;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    rd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    if(FAILED(device_->CreateRenderTargetView(texture.Get(),&rd,&target)))return XR_ERROR_RUNTIME_FAILURE;
    e.diagnosticTexture=std::move(texture);e.diagnosticRtv=std::move(target);
  }
  viewMatrix(view.pose,viewMatrixData);Constants c{};
  for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col)for(unsigned k=0;k<4;++k)
    c.matrix[row][col]+=projection.m[row][k]*viewMatrixData[k][col];
  // This diagnostic owns context state; a production pass must save/restore
  // both D3D11 state and EDVR's context shadow instead of clearing it.
  context_->ClearState();
  const float black[]={0,0,0,1};ID3D11RenderTargetView* rtv=e.diagnosticRtv.Get();
  context_->ClearRenderTargetView(rtv,black);context_->OMSetRenderTargets(1,&rtv,nullptr);
  context_->OMSetBlendState(nullptr,nullptr,~0u);context_->OMSetDepthStencilState(depth_.Get(),0);
  context_->RSSetState(rasterizer_.Get());D3D11_VIEWPORT vp{0,0,float(e.width),float(e.height),0,1};
  context_->RSSetViewports(1,&vp);
  UINT stride=sizeof(Vertex),offset=0;context_->IASetInputLayout(layout_.Get());
  context_->IASetVertexBuffers(0,1,vertices_.GetAddressOf(),&stride,&offset);
  context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context_->VSSetShader(vertexShader_.Get(),nullptr,0);context_->PSSetShader(pixelShader_.Get(),nullptr,0);
  context_->VSSetConstantBuffers(0,1,constants_.GetAddressOf());
  context_->PSSetConstantBuffers(0,1,constants_.GetAddressOf());
  context_->UpdateSubresource(constants_.Get(),0,nullptr,&c,0,0);context_->Draw(3,0);
  context_->OMSetRenderTargets(0,nullptr,nullptr);
  if(FAILED(device_->GetDeviceRemovedReason()))return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  // Subsequent capture is on this same immediate context, after Draw.
  out=e.diagnosticTexture.Get();return XR_SUCCESS;
}

XrResult D3D11Stereo::renderCaptured(const XrView (&views)[2],XrSpace space,const EyeCapture& capture,
                                   XrCompositionLayerProjection& layer) {
  layer={XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  if(!ready_)return lastResult_==XR_SUCCESS?XR_ERROR_CALL_ORDER_INVALID:lastResult_;
  if(!space)return XR_ERROR_HANDLE_INVALID;
  if(FAILED(device_->GetDeviceRemovedReason()))return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  ComPtr<ID3D11ShaderResourceView> srvs[2];BlitConstants constants[2]{};
  // Validate both inputs and prepare views before acquiring any runtime image.
  for(unsigned i=0;i<2;++i) {
    const auto eye=vr::EVREye(i);auto* texture=capture.texture(eye);
    if(!texture||views[i].type!=XR_TYPE_VIEW||views[i].next||!poseValid(views[i].pose))
      return XR_ERROR_VALIDATION_FAILURE;
    vr::HmdMatrix44_t ignored{};
    if(!projectionMatrix(views[i].fov,.025f,50000.f,vr::API_DirectX,ignored))return XR_ERROR_VALIDATION_FAILURE;
    D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
    if(!td.Width||!td.Height||td.ArraySize!=1||td.MipLevels!=1||td.SampleDesc.Count!=1||td.SampleDesc.Quality||
       !(td.BindFlags&D3D11_BIND_SHADER_RESOURCE)||!sameDevice(device_.Get(),texture))
      return XR_ERROR_VALIDATION_FAILURE;
    if(td.Format!=DXGI_FORMAT_R8G8B8A8_TYPELESS&&td.Format!=DXGI_FORMAT_B8G8R8A8_TYPELESS)
      return XR_ERROR_VALIDATION_FAILURE;
    const auto b=capture.bounds(eye);
    for(float v:{b.uMin,b.vMin,b.uMax,b.vMax})
      if(!std::isfinite(v)||v<0||v>1)return XR_ERROR_VALIDATION_FAILURE;
    if(b.uMin==b.uMax||b.vMin==b.vMax)return XR_ERROR_VALIDATION_FAILURE;
    const auto color=capture.colorSpace(eye);
    if(color!=vr::ColorSpace_Auto&&color!=vr::ColorSpace_Gamma&&color!=vr::ColorSpace_Linear)
      return XR_ERROR_VALIDATION_FAILURE;
    const bool gamma=color!=vr::ColorSpace_Linear,bgra=td.Format==DXGI_FORMAT_B8G8R8A8_TYPELESS;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format=bgra?(gamma?DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:DXGI_FORMAT_B8G8R8A8_UNORM):
      (gamma?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_UNORM);
    sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
    if(FAILED(device_->CreateShaderResourceView(texture,&sd,&srvs[i])))return XR_ERROR_RUNTIME_FAILURE;
    auto& c=constants[i];
    c.bounds[0]=b.uMin;c.bounds[1]=b.vMin;c.bounds[2]=b.uMax;c.bounds[3]=b.vMax;
    // Clamp filtering inside the selected crop, including reversed bounds, so
    // a double-wide texture cannot bleed the adjacent eye across its edge.
    for(unsigned axis=0;axis<2;++axis) {
      const float lo=(std::min)(c.bounds[axis],c.bounds[axis+2]);
      const float hi=(std::max)(c.bounds[axis],c.bounds[axis+2]);
      const float inset=(std::min)(.5f/float(axis?td.Height:td.Width),(hi-lo)*.5f);
      c.clampUV[axis]=lo+inset;c.clampUV[axis+2]=hi-inset;
    }
    c.encodeSRGB=(format_==DXGI_FORMAT_R8G8B8A8_UNORM||format_==DXGI_FORMAT_B8G8R8A8_UNORM)?1.f:0.f;
  }
  auto failed=[&](XrResult r){ready_=false;lastResult_=r;return r;};
  context_->ClearState(); // standalone diagnostic only
  context_->OMSetBlendState(nullptr,nullptr,~0u);context_->OMSetDepthStencilState(depth_.Get(),0);
  context_->RSSetState(rasterizer_.Get());
  context_->IASetInputLayout(nullptr);context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context_->VSSetShader(blitVertexShader_.Get(),nullptr,0);context_->PSSetShader(blitPixelShader_.Get(),nullptr,0);
  context_->PSSetSamplers(0,1,blitSampler_.GetAddressOf());
  context_->PSSetConstantBuffers(0,1,blitConstants_.GetAddressOf());
  for(unsigned i=0;i<2;++i) {
    uint32_t index=0;XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult r=dispatch_.acquireSwapchainImage(eyes_[i].swapchain,&ai,&index);
    if(r!=XR_SUCCESS)return failed(r);
    if(index>=eyes_[i].rtvs.size())return failed(XR_ERROR_RUNTIME_FAILURE);
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wi.timeout=1000000000LL;
    r=dispatch_.waitSwapchainImage(eyes_[i].swapchain,&wi);
    if(r!=XR_SUCCESS)return failed(r); // timeout is not permission to write/release
    auto* rtv=eyes_[i].rtvs[index].Get();const float black[]={0,0,0,1};
    context_->ClearRenderTargetView(rtv,black);context_->OMSetRenderTargets(1,&rtv,nullptr);
    D3D11_VIEWPORT vp{0,0,float(eyes_[i].width),float(eyes_[i].height),0,1};context_->RSSetViewports(1,&vp);
    context_->PSSetShaderResources(0,1,srvs[i].GetAddressOf());
    context_->UpdateSubresource(blitConstants_.Get(),0,nullptr,&constants[i],0,0);context_->Draw(3,0);
    ID3D11ShaderResourceView* nullSrv=nullptr;context_->PSSetShaderResources(0,1,&nullSrv);
    context_->OMSetRenderTargets(0,nullptr,nullptr);context_->Flush();
    if(FAILED(device_->GetDeviceRemovedReason()))return failed(XR_ERROR_GRAPHICS_DEVICE_INVALID);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    r=dispatch_.releaseSwapchainImage(eyes_[i].swapchain,&ri);if(r!=XR_SUCCESS)return failed(r);
  }
  for(unsigned i=0;i<2;++i) {
    auto& v=layerViews_[i];v={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};v.pose=views[i].pose;v.fov=views[i].fov;
    v.subImage={eyes_[i].swapchain,{{0,0},{int32_t(eyes_[i].width),int32_t(eyes_[i].height)}},0};
  }
  layer.space=space;layer.viewCount=2;layer.views=layerViews_;return XR_SUCCESS;
}

XrResult D3D11Stereo::renderSkybox(const XrView (&views)[2], XrSpace space,
                                   const SkyboxCapture& capture,
                                   XrCompositionLayerProjection& layer) {
  layer={XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  if(!ready_)return lastResult_==XR_SUCCESS?XR_ERROR_CALL_ORDER_INVALID:lastResult_;
  if(!space)return XR_ERROR_HANDLE_INVALID;
  if(!capture.ready())return XR_ERROR_VALIDATION_FAILURE;
  if(FAILED(device_->GetDeviceRemovedReason()))return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  ComPtr<ID3D11ShaderResourceView> faces[6];
  for(unsigned i=0;i<6;++i) {
    auto* texture=capture.texture(i); if(!texture)return XR_ERROR_VALIDATION_FAILURE;
    D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
    if(!td.Width||!td.Height||td.ArraySize!=1||td.MipLevels!=1||td.SampleDesc.Count!=1||td.SampleDesc.Quality||
       !(td.BindFlags&D3D11_BIND_SHADER_RESOURCE)||!sameDevice(device_.Get(),texture))return XR_ERROR_VALIDATION_FAILURE;
    const auto color=capture.colorSpace(i);
    if(color!=vr::ColorSpace_Auto&&color!=vr::ColorSpace_Gamma&&color!=vr::ColorSpace_Linear)return XR_ERROR_VALIDATION_FAILURE;
    const bool gamma=color!=vr::ColorSpace_Linear;
    DXGI_FORMAT f=DXGI_FORMAT_UNKNOWN;
    if(td.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS)f=gamma?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_UNORM;
    if(td.Format==DXGI_FORMAT_B8G8R8A8_TYPELESS)f=gamma?DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:DXGI_FORMAT_B8G8R8A8_UNORM;
    if(f==DXGI_FORMAT_UNKNOWN)return XR_ERROR_VALIDATION_FAILURE;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=f;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
    if(FAILED(device_->CreateShaderResourceView(texture,&sd,&faces[i])))return XR_ERROR_RUNTIME_FAILURE;
  }
  SkyConstants constants[2]{};
  for(unsigned eye=0;eye<2;++eye) {
    vr::HmdMatrix44_t projection{};
    if(views[eye].type!=XR_TYPE_VIEW||views[eye].next||!poseValid(views[eye].pose)||
      !projectionMatrix(views[eye].fov,.025f,50000.f,vr::API_DirectX,projection))return XR_ERROR_VALIDATION_FAILURE;
    skyRotation(views[eye].pose.orientation,constants[eye].orient);
    constants[eye].tangents[0]=std::tan(views[eye].fov.angleLeft);
    constants[eye].tangents[1]=std::tan(views[eye].fov.angleRight);
    constants[eye].tangents[2]=std::tan(views[eye].fov.angleUp);
    constants[eye].tangents[3]=std::tan(views[eye].fov.angleDown);
    constants[eye].encode=(format_==DXGI_FORMAT_R8G8B8A8_UNORM||format_==DXGI_FORMAT_B8G8R8A8_UNORM)?1.f:0.f;
  }
  auto failed=[&](XrResult r){ready_=false;lastResult_=r;return r;};
  context_->ClearState();
  context_->VSSetShader(skyboxVertexShader_.Get(),nullptr,0);
  context_->PSSetShader(skyboxPixelShader_.Get(),nullptr,0);
  context_->PSSetSamplers(0,1,skyboxSampler_.GetAddressOf());
  context_->PSSetConstantBuffers(0,1,skyboxConstants_.GetAddressOf());
  ID3D11ShaderResourceView* faceRaw[6]{};
  for(unsigned i=0;i<6;++i)faceRaw[i]=faces[i].Get();
  context_->IASetInputLayout(nullptr);
  context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context_->RSSetState(rasterizer_.Get());
  context_->OMSetBlendState(nullptr,nullptr,~0u);
  context_->OMSetDepthStencilState(depth_.Get(),0);
  for(unsigned eye=0;eye<2;++eye) {
    auto& e=eyes_[eye];
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};uint32_t index=0;
    XrResult r=dispatch_.acquireSwapchainImage(e.swapchain,&ai,&index);
    if(r!=XR_SUCCESS)return failed(r);
    if(index>=e.rtvs.size())return failed(XR_ERROR_RUNTIME_FAILURE);
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wi.timeout=1000000000LL;
    r=dispatch_.waitSwapchainImage(e.swapchain,&wi);
    if(r!=XR_SUCCESS)return failed(r); // timeout does not permit drawing/release
    auto* rtv=e.rtvs[index].Get();context_->OMSetRenderTargets(1,&rtv,nullptr);
    D3D11_VIEWPORT viewport{0,0,float(e.width),float(e.height),0,1};context_->RSSetViewports(1,&viewport);
    context_->PSSetShaderResources(0,6,faceRaw);
    context_->UpdateSubresource(skyboxConstants_.Get(),0,nullptr,&constants[eye],0,0);
    context_->Draw(3,0);
    ID3D11ShaderResourceView* nulls[6]{};context_->PSSetShaderResources(0,6,nulls);
    context_->OMSetRenderTargets(0,nullptr,nullptr);context_->Flush();
    if(FAILED(device_->GetDeviceRemovedReason()))return failed(XR_ERROR_GRAPHICS_DEVICE_INVALID);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    r=dispatch_.releaseSwapchainImage(e.swapchain,&ri);if(r!=XR_SUCCESS)return failed(r);
  }
  for(unsigned i=0;i<2;++i) {
    auto& view=layerViews_[i];view={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    view.pose=views[i].pose;view.fov=views[i].fov;
    view.subImage={eyes_[i].swapchain,{{0,0},{int32_t(eyes_[i].width),int32_t(eyes_[i].height)}},0};
  }
  layer.space=space;layer.viewCount=2;layer.views=layerViews_;return XR_SUCCESS;
}
} // namespace edvr::openxr

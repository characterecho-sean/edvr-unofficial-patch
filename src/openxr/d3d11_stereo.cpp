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
const char* shader=R"(
cbuffer Constants : register(b0) { row_major float4x4 mvp; float encodeSRGB; float3 pad; };
struct V { float3 position:POSITION; float4 color:COLOR; };
struct O { float4 position:SV_POSITION; float4 color:COLOR; };
O vs(V v) { O o; o.position=mul(mvp,float4(v.position,1)); o.color=v.color; return o; }
float3 srgb(float3 c) { return lerp(12.92*c,1.055*pow(max(c,0),1.0/2.4)-0.055,step(0.0031308,c)); }
float4 ps(O v):SV_TARGET { return float4(encodeSRGB>0.5?srgb(v.color.rgb):v.color.rgb,1); }
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
}

D3D11Stereo::~D3D11Stereo(){shutdown();}
XrResult D3D11Stereo::shutdown() {
  XrResult first=XR_SUCCESS;ready_=false;
  if(context_){context_->ClearState();context_->Flush();}
  constants_.Reset();vertices_.Reset();layout_.Reset();pixelShader_.Reset();vertexShader_.Reset();rasterizer_.Reset();depth_.Reset();
  for(auto& eye:eyes_){
    eye.rtvs.clear();eye.images.clear();
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
  ComPtr<ID3DBlob> vs,ps,errors;
  if(FAILED(D3DCompile(shader,std::strlen(shader),"EDVR native stereo",nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&vs,&errors))||
     FAILED(D3DCompile(shader,std::strlen(shader),"EDVR native stereo",nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&ps,&errors))||
     FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertexShader_))||
     FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixelShader_)))return failed(XR_ERROR_RUNTIME_FAILURE);
  D3D11_INPUT_ELEMENT_DESC layout[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
    {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}};
  if(FAILED(device->CreateInputLayout(layout,2,vs->GetBufferPointer(),vs->GetBufferSize(),&layout_)))return failed(XR_ERROR_RUNTIME_FAILURE);
  const Vertex vertices[]={{{-.3f,0,-2},{1,0,0,1}},{{.3f,0,-2},{0,1,0,1}},{{0,.5f,-2},{0,0,1,1}}};
  D3D11_BUFFER_DESC bd{sizeof(vertices),D3D11_USAGE_IMMUTABLE,D3D11_BIND_VERTEX_BUFFER,0,0,0};D3D11_SUBRESOURCE_DATA data{vertices,0,0};
  if(FAILED(device->CreateBuffer(&bd,&data,&vertices_)))return failed(XR_ERROR_RUNTIME_FAILURE);
  bd={sizeof(Constants),D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER,0,0,0};
  if(FAILED(device->CreateBuffer(&bd,nullptr,&constants_)))return failed(XR_ERROR_RUNTIME_FAILURE);
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
} // namespace edvr::openxr

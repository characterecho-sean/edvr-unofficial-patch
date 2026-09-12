#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../../src/openxr/eye_capture.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <limits>
using Microsoft::WRL::ComPtr;
using edvr::openxr::EyeCapture;
namespace {
unsigned checks=0,failures=0;
void check(bool value,const char* name) {++checks;if(!value){++failures;std::printf("FAIL: %s\n",name);}}
struct Device {
  ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
  bool open() {
    const auto hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context);
    check(SUCCEEDED(hr),"WARP device");return SUCCEEDED(hr);
  }
  ComPtr<ID3D11Texture2D> texture(DXGI_FORMAT format=DXGI_FORMAT_R8G8B8A8_UNORM,UINT width=4,UINT height=4,
      UINT arrays=1,UINT mips=1,UINT samples=1) {
    D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.ArraySize=arrays;d.MipLevels=mips;d.Format=format;
    d.SampleDesc.Count=samples;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> out;check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&out)),"fixture texture");return out;
  }
  void fill(ID3D11Texture2D* texture,UINT color) {
    D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);std::vector<UINT> values(d.Width*d.Height,color);
    context->UpdateSubresource(texture,0,nullptr,values.data(),d.Width*4,0);
  }
  bool pixels(ID3D11Texture2D* texture,UINT color) {
    if(!texture)return false;
    D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
    d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
    ComPtr<ID3D11Texture2D> stage;if(FAILED(device->CreateTexture2D(&d,nullptr,&stage)))return false;
    context->CopyResource(stage.Get(),texture);D3D11_MAPPED_SUBRESOURCE map{};
    if(FAILED(context->Map(stage.Get(),0,D3D11_MAP_READ,0,&map)))return false;
    bool good=true;
    for(UINT y=0;y<d.Height;++y)for(UINT x=0;x<d.Width;++x)
      good=good && reinterpret_cast<const UINT*>(static_cast<const char*>(map.pData)+y*map.RowPitch)[x]==color;
    context->Unmap(stage.Get(),0);return good;
  }
};
void run() {
  Device d,foreign;if(!d.open()||!foreign.open())return;
  EyeCapture cap;check(FAILED(cap.initialize(nullptr)),"null device rejected");check(SUCCEEDED(cap.initialize(d.device.Get())),"initialize");
  constexpr UINT firstColor=0xFF2317D2,secondColor=0xFFCB8241;
  const DXGI_FORMAT formats[]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
    DXGI_FORMAT_R8G8B8A8_TYPELESS,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_TYPELESS};
  for(const auto format:formats)for(unsigned order=0;order<2;++order) {
    auto source=d.texture(format);if(!source)return;
    cap.reset();const auto first=vr::EVREye(order),second=vr::EVREye(order^1);
    const vr::Texture_t texture{source.Get(),vr::API_DirectX,vr::ColorSpace_Gamma};
    d.fill(source.Get(),firstColor);check(cap.capture(first,&texture)==vr::VRCompositorError_None,"first eye accepted");
    d.fill(source.Get(),secondColor);check(cap.capture(second,&texture)==vr::VRCompositorError_None,"second eye accepted");
    check(d.pixels(cap.texture(first),firstColor)&&d.pixels(cap.texture(second),secondColor),"all pixels isolated, either eye order and format");
    check(cap.texture(first)!=source.Get()&&cap.texture(first)!=cap.texture(second),"copies have distinct ownership");
    D3D11_TEXTURE2D_DESC copy{};cap.texture(first)->GetDesc(&copy);
    check(copy.Format==(format==formats[0]||format==formats[1]||format==formats[2]?DXGI_FORMAT_R8G8B8A8_TYPELESS:DXGI_FORMAT_B8G8R8A8_TYPELESS),"private format supports color interpretation");
    // Check both view interpretations, including submitted typed SRGB and UNORM.
    for(bool gamma:{false,true}) {
      D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
      sd.Format=copy.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS?(gamma?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_UNORM):
        (gamma?DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:DXGI_FORMAT_B8G8R8A8_UNORM);
      ComPtr<ID3D11ShaderResourceView> srv;check(SUCCEEDED(d.device->CreateShaderResourceView(cap.texture(first),&sd,&srv)),"gamma and linear SRVs");
    }
  }
  auto source=d.texture();if(!source)return;
  const vr::Texture_t texture{source.Get(),vr::API_DirectX,vr::ColorSpace_Gamma};
  const vr::VRTextureBounds_t flipped{1,1,0,0};d.fill(source.Get(),firstColor);
  check(cap.capture(vr::Eye_Left,&texture,&flipped)==vr::VRCompositorError_None,"flipped bounds accepted");
  ComPtr<ID3D11Texture2D> accepted=cap.texture(vr::Eye_Left);
  const auto preserved=[&] {
    const auto b=cap.bounds(vr::Eye_Left);
    return cap.texture(vr::Eye_Left)==accepted.Get()&&b.uMin==1&&b.vMin==1&&b.uMax==0&&b.vMax==0&&
      cap.colorSpace(vr::Eye_Left)==vr::ColorSpace_Gamma&&d.pixels(accepted.Get(),firstColor);
  };
  const float nan=std::numeric_limits<float>::quiet_NaN(),inf=std::numeric_limits<float>::infinity();
  for(const auto b:{vr::VRTextureBounds_t{-1,0,1,1},{0,0,2,1},{0,0,0,1},{0,0,1,0},{nan,0,1,1},{0,0,1,inf}}) {
    check(cap.capture(vr::Eye_Left,&texture,&b)==vr::VRCompositorError_InvalidTexture&&preserved(),"bad bounds preserve accepted eye");
  }
  auto foreignSource=foreign.texture();if(!foreignSource)return;
  const vr::Texture_t foreignTexture{foreignSource.Get(),vr::API_DirectX,vr::ColorSpace_Auto};
  check(cap.capture(vr::Eye_Left,&foreignTexture)==vr::VRCompositorError_TextureIsOnWrongDevice&&preserved(),"same adapter different device rejected");
  check(cap.capture(vr::Eye_Left,nullptr)==vr::VRCompositorError_InvalidTexture&&preserved(),"null descriptor rejected");
  for(const auto bad:{vr::Texture_t{nullptr,vr::API_DirectX,vr::ColorSpace_Auto},
      {source.Get(),vr::API_OpenGL,vr::ColorSpace_Auto},{source.Get(),vr::API_DirectX,vr::EColorSpace(99)}})
    check(cap.capture(vr::Eye_Left,&bad)==vr::VRCompositorError_InvalidTexture&&preserved(),"invalid texture metadata preserved");
  check(cap.capture(vr::EVREye(99),&texture)==vr::VRCompositorError_IndexOutOfRange&&preserved(),"invalid eye rejected");
  check(cap.capture(vr::Eye_Left,&texture,nullptr,vr::Submit_LensDistortionAlreadyApplied)==vr::VRCompositorError_InvalidTexture&&preserved(),"unsupported flags rejected");
  auto array=d.texture(DXGI_FORMAT_R8G8B8A8_UNORM,4,4,2),mip=d.texture(DXGI_FORMAT_R8G8B8A8_UNORM,4,4,1,2),
    msaa=d.texture(DXGI_FORMAT_R8G8B8A8_UNORM,4,4,1,1,4),unsupported=d.texture(DXGI_FORMAT_R16G16B16A16_FLOAT);
  for(auto bad:{array.Get(),mip.Get(),msaa.Get(),unsupported.Get()}) {
    if(!bad)return;vr::Texture_t t{bad,vr::API_DirectX,vr::ColorSpace_Auto};
    check(cap.capture(vr::Eye_Left,&t)==(bad==unsupported.Get()?vr::VRCompositorError_TextureUsesUnsupportedFormat:vr::VRCompositorError_InvalidTexture)&&preserved(),"unsupported descriptor preserves eye");
  }
  D3D11_BUFFER_DESC bd{16,D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER,0,0,0};ComPtr<ID3D11Buffer> buffer;
  check(SUCCEEDED(d.device->CreateBuffer(&bd,nullptr,&buffer)),"fixture buffer");
  const vr::Texture_t notTexture{buffer.Get(),vr::API_DirectX,vr::ColorSpace_Auto};
  check(cap.capture(vr::Eye_Left,&notTexture)==vr::VRCompositorError_InvalidTexture&&preserved(),"nontexture COM resource rejected");
  d.fill(source.Get(),secondColor);const vr::Texture_t linear{source.Get(),vr::API_DirectX,vr::ColorSpace_Linear};
  check(cap.capture(vr::Eye_Left,&linear,nullptr,vr::Submit_Default,false)==vr::VRCompositorError_None&&preserved(),"no-render validates without changing pixels or metadata");
  check(cap.capture(vr::Eye_Left,&foreignTexture,nullptr,vr::Submit_Default,false)==vr::VRCompositorError_TextureIsOnWrongDevice&&preserved(),"no-render still validates device");
  // A capture is only a copy command: even nondefault pipeline bindings survive.
  ID3D11Buffer* cb=buffer.Get();d.context->VSSetConstantBuffers(3,1,&cb);d.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
  check(cap.capture(vr::Eye_Left,&linear)==vr::VRCompositorError_None&&cap.texture(vr::Eye_Left)==accepted.Get()&&d.pixels(accepted.Get(),secondColor),"allocation reused and pixels replaced");
  ComPtr<ID3D11Buffer> bound;d.context->VSGetConstantBuffers(3,1,&bound);D3D11_PRIMITIVE_TOPOLOGY topology{};d.context->IAGetPrimitiveTopology(&topology);
  check(bound.Get()==cb&&topology==D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP,"pipeline bindings preserved");
  auto bigger=d.texture(DXGI_FORMAT_R8G8B8A8_UNORM,8,4);if(!bigger)return;d.fill(bigger.Get(),firstColor);
  const vr::Texture_t resized{bigger.Get(),vr::API_DirectX,vr::ColorSpace_Auto};
  check(cap.capture(vr::Eye_Left,&resized)==vr::VRCompositorError_None&&cap.texture(vr::Eye_Left)!=accepted.Get()&&d.pixels(cap.texture(vr::Eye_Left),firstColor),"resize replaces allocation");
  const vr::Texture_t own{cap.texture(vr::Eye_Left),vr::API_DirectX,vr::ColorSpace_Auto};
  check(cap.capture(vr::Eye_Left,&own)==vr::VRCompositorError_None&&cap.texture(vr::Eye_Left)!=own.handle&&d.pixels(cap.texture(vr::Eye_Left),firstColor),"self input never copies resource onto itself");
  cap.reset();check(!cap.texture(vr::Eye_Left)&&!cap.texture(vr::Eye_Right),"reset invalidates both eyes");
  cap.shutdown();check(cap.capture(vr::Eye_Left,&texture)==vr::VRCompositorError_InvalidTexture,"shutdown refuses capture");
  check(SUCCEEDED(cap.initialize(d.device.Get()))&&!cap.texture(vr::Eye_Left),"reinitialize starts empty");
}
}
int main(int argc,char** argv) {
  if(argc!=2)return 2;
  if(!std::strcmp(argv[1],"--dry-run")){std::puts("Would test eye copies on WARP; no files or device created.");return 0;}
  if(std::strcmp(argv[1],"--self-test"))return 2;
  run();std::printf("openxr_capture_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}

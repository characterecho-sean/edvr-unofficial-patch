#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <string>
#include "../../src/common/native_fss.h"
#include "../../src/common/config.h"
#include "../../src/common/frame_flag.h"
#include "../../src/d3d11/fss_heal.h"
#include "../../src/d3d11/shader_swap.h"
#include "../../src/d3d11/gpu_census.h"
#include "../../src/common/system_d3d11.h"
using Microsoft::WRL::ComPtr;
namespace edvr {
void breadcrumb(const char*) {}
// The GPU census (issue #38) is cross-cutting; this rig is about fss_heal's
// own effect, not the census's rotation, so it is stubbed like the other
// modules fss_heal.cpp reaches but this rig does not link.
bool gpuCensusBegin(ID3D11DeviceContext*, GpuCensusSection) noexcept { return false; }
void gpuCensusEnd(ID3D11DeviceContext*, GpuCensusSection) noexcept {}
// Compile the production HLSL on WARP without installing game hooks.
ID3D11ComputeShader* shaderSwapCompileCs(ID3D11DeviceContext* context,const char* hlsl,size_t size,
    const char* entry,const char* name,const SwapMacro*,const char*) {
  ComPtr<ID3DBlob> code,error;
  if(FAILED(D3DCompile(hlsl,size,name,nullptr,nullptr,entry,"cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error)))return nullptr;
  ComPtr<ID3D11Device> device;context->GetDevice(&device);ID3D11ComputeShader* shader=nullptr;
  device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader);return shader;
}
}
namespace {
unsigned checks=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
constexpr unsigned W=64,H=32,black=0xFF000000,warm=0xFF4080FF;
struct Device {
  ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
  Device() {
    auto create=edvr::systemD3D11CreateDevice();
    require(create!=nullptr,"System32 D3D11CreateDevice");
    require(create&&SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"WARP device");
  }
  ComPtr<ID3D11Texture2D> texture(unsigned color) {
    std::vector<unsigned> pixels(W*H,color);D3D11_TEXTURE2D_DESC d{};
    d.Width=W;d.Height=H;d.ArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BindFlags=D3D11_BIND_SHADER_RESOURCE;D3D11_SUBRESOURCE_DATA data{pixels.data(),W*4,0};
    ComPtr<ID3D11Texture2D> texture;require(SUCCEEDED(device->CreateTexture2D(&d,&data,&texture)),"source texture");return texture;
  }
  std::vector<unsigned> read(ID3D11Texture2D* source) {
    D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.BindFlags=d.MiscFlags=0;
    ComPtr<ID3D11Texture2D> staging;require(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&staging)),"readback texture");
    context->CopyResource(staging.Get(),source);D3D11_MAPPED_SUBRESOURCE m{};
    require(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&m)),"readback map");
    std::vector<unsigned> pixels(d.Width*d.Height);
    for(unsigned y=0;y<d.Height;++y)std::memcpy(pixels.data()+y*d.Width,static_cast<char*>(m.pData)+y*m.RowPitch,d.Width*4);
    context->Unmap(staging.Get(),0);return pixels;
  }
};
EdvrNativeFssFrame frame(uint64_t generation,uint64_t sequence) {
  EdvrNativeFssFrame f{sizeof(f),2};f.generation=generation;f.referenceGeneration=1;f.sequence=sequence;
  for(unsigned e=0;e<2;++e){f.frusta[e][0]=f.frusta[e][2]=-1;f.frusta[e][1]=f.frusta[e][3]=1;
    f.eyeToHead[e][0]=f.eyeToHead[e][5]=f.eyeToHead[e][10]=1;f.eyeToHead[e][3]=e?.032f:-.032f;}return f;
}
void run() {
  const float corners[16]={0,0,1,0,1,1,0,1,0,0,1,0,1,1,0,1};edvr::publishFssPanelRect(corners);
  const float full[4]={0,0,1,1};
  for(unsigned generation=51;generation<=52;++generation) {
    Device d;auto left=d.texture(black),right=d.texture(warm);
    edvr::Config::get().set("fix.fss_eye_sync","heal");
    EdvrNativeFssRequest request{sizeof(request),2,d.device.Get(),generation};EdvrNativeFssTable table{sizeof(table),2};
    require(edvrAcquireNativeFss(&request,&table)==S_OK,"acquire real shader provider");
    auto f=frame(generation,1);require(table.beginFrame(table.context,&f)==S_OK,"begin arrival");
    edvr::bumpFssArrivalStamp();edvr::bumpFssChromeStamp();ID3D11Texture2D* output=nullptr;uint32_t healedEye=99;
    require(table.treatEye(table.context,1,1,right.Get(),full,&output)==S_FALSE&&!output,"current right snapshot only");
    require(table.treatEye(table.context,1,0,left.Get(),full,&output)==S_FALSE&&!output,"left completes the pair");
    require(table.healPair(table.context,1,&healedEye,&output)==S_OK&&output&&healedEye==0,"actual FSS heal shader output");
    ComPtr<ID3D11Texture2D> healed;healed.Attach(output);auto pixels=d.read(healed.Get());
    require(pixels[(H/2)*W+W/2]==warm&&pixels[0]==black,"actual HLSL fills display interior only");
    require(d.read(left.Get())==std::vector<unsigned>(W*H,black)&&d.read(right.Get())==std::vector<unsigned>(W*H,warm),"shader and copies preserve both game sources");
    f=frame(generation,2);require(table.beginFrame(table.context,&f)==S_OK,"begin left-first frame");
    require(table.treatEye(table.context,2,0,left.Get(),full,&output)==S_FALSE&&!output,"left-first only snapshots");
    require(table.healPair(table.context,2,&healedEye,&output)==E_PENDING,"left alone waits on the right donor");
    require(table.treatEye(table.context,2,1,right.Get(),full,&output)==S_FALSE&&!output,"right completes normal order");
    require(table.healPair(table.context,2,&healedEye,&output)==S_OK&&output&&healedEye==0,"left-first heals from this frame's right");
    healed.Attach(output);pixels=d.read(healed.Get());require(pixels[(H/2)*W+W/2]==warm,"left-first pixels reach real shader");
    // Mirror detects bounded black arrival tiles, with lit pixels ten pixels
    // away; an entirely black eye is deliberately not a matching tile.
    std::vector<unsigned> mirrorLeft(W*H,warm);
    for(unsigned y=H/2-6;y<H/2+6;++y)
      for(unsigned x=W/2-6;x<W/2+6;++x)mirrorLeft[y*W+x]=black;
    d.context->UpdateSubresource(left.Get(),0,nullptr,mirrorLeft.data(),W*4,0);
    edvr::Config::get().set("fix.fss_eye_sync","mirror");f=frame(generation,3);require(table.beginFrame(table.context,&f)==S_OK,"begin mirror mode");
    require(table.treatEye(table.context,3,0,left.Get(),full,&output)==S_FALSE&&!output,"mirror left is donor only");
    require(table.treatEye(table.context,3,1,right.Get(),full,&output)==S_FALSE&&!output,"mirror right only snapshots");
    require(table.healPair(table.context,3,&healedEye,&output)==S_OK&&output&&healedEye==1,"real mirror shader output");
    healed.Attach(output);pixels=d.read(healed.Get());
    require(pixels[(H/2)*W+W/2]==black&&pixels[0]==warm,"mirror stamps bounded left arrival tile and preserves clear regions");
    require(table.close(table.context)==S_OK,"release provider and shader cache");
    require(d.read(healed.Get())==pixels,"AddRef result survives CPU close");
    require(!edvr::submittedTexture(0)&&!edvr::submittedTexture(1),"close clears donor publications");
  }
}
}
int main(int argc,char** argv) {
  SetErrorMode(3);if(argc!=2)return 2;
  if(!std::strcmp(argv[1],"--dry-run")){std::puts("native_fss_gpu_test: dry-run (no device or files)");return 0;}
  if(std::strcmp(argv[1],"--self-test"))return 2;
  try{run();std::printf("native_fss_gpu_test: %u checks, 0 failures\n",checks);return 0;}
  catch(const std::exception& e){edvr::fssHealRelease();std::printf("FAIL: %s\n",e.what());return 1;}
}

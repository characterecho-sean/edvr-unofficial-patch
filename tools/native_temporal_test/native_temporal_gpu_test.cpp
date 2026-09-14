#include "../../src/openxr/native_temporal_client.h"
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using namespace edvr::openxr;
using Microsoft::WRL::ComPtr;
unsigned checks=0;
void require(bool x,const char* why){++checks;if(!x)throw std::runtime_error(why);}
struct Watchdog {
  std::atomic<bool> done{false};std::thread thread;
  Watchdog():thread([this]{const auto until=GetTickCount64()+60000;while(!done){if(GetTickCount64()>until)std::_Exit(9);Sleep(10);}}){}
  ~Watchdog(){done=true;thread.join();}
};
GeometryInput geometry(uint64_t sequence) {
  GeometryInput g{};g.generation=1;g.sequence=sequence;g.displayTime=sequence;
  g.headPose.orientation.w=1;g.headFlags=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  g.viewFlags=XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT;
  for(unsigned e=0;e<2;++e){g.width[e]=600;g.height[e]=456;g.views[e].pose.orientation.w=1;
    g.views[e].pose.position.x=e?.032f:-.032f;g.views[e].fov={e?-.7f:-.85f,e?.85f:.7f,.8f,-.6f};}
  return g;
}
ComPtr<ID3D11Texture2D> texture(ID3D11Device* device,unsigned w,unsigned h,unsigned color){
  std::vector<unsigned> pixels(size_t(w)*h,color);D3D11_TEXTURE2D_DESC d{};
  d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
  d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;D3D11_SUBRESOURCE_DATA data{pixels.data(),w*4,0};
  ComPtr<ID3D11Texture2D> t;require(SUCCEEDED(device->CreateTexture2D(&d,&data,&t)),"texture");return t;
}
void checkColor(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* image,unsigned expected){
  D3D11_TEXTURE2D_DESC d{};image->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;require(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&staging)),"staging");
  context->CopyResource(staging.Get(),image);D3D11_MAPPED_SUBRESOURCE map{};
  require(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map)),"read output");
  const auto* pixel=static_cast<const unsigned char*>(map.pData)+(d.Height/2)*map.RowPitch+(d.Width/2)*4;
  bool color=true;for(unsigned channel=0;channel<3;++channel)color&=std::abs(int(pixel[channel])-int((expected>>(channel*8))&255))<=8;
  context->Unmap(staging.Get(),0);require(color,"actual output color stays within 8/255");
}
void run(const wchar_t* path,bool dlss){
  Watchdog watchdog;
  // Use the proxy's creation export so its config and graphics hooks are
  // initialized exactly as for the game. The isolated directory supplies INI.
  HMODULE module=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  require(module!=nullptr,"graphics DLL loads");
  auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));
  require(create!=nullptr,"creation export");ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
  require(SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context)),"hardware device through graphics proxy");
  NativeTemporalClient client;require(client.acquire(module,device.Get(),77)==S_OK,"real temporal provider");
  const unsigned colors[2]={0xff1430dd,0xff35c024};unsigned w=400,h=304;
  ComPtr<ID3D11Texture2D> source[2]={texture(device.Get(),w,h,colors[0]),texture(device.Get(),w,h,colors[1])};
  auto sentinel=texture(device.Get(),64,64,0xff445566);
  ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11UnorderedAccessView> uav;
  require(SUCCEEDED(device->CreateShaderResourceView(sentinel.Get(),nullptr,&srv)),"sentinel SRV");
  auto other=texture(device.Get(),64,64,0xff667788);require(SUCCEEDED(device->CreateUnorderedAccessView(other.Get(),nullptr,&uav)),"sentinel UAV");
  for(unsigned n=1;n<=9;++n){
    if(n==6){w=432;h=320;for(unsigned e=0;e<2;++e)source[e]=texture(device.Get(),w,h,colors[e]);}
    if(n==9)require(client.invalidate()==S_OK,"history invalidation");
    auto g=geometry(n);float shifts[2][2]{};HRESULT begun=E_FAIL;
    std::thread cpu([&]{begun=client.begin(g,n==9?2:1,shifts);for(unsigned e=0;e<2;++e)client.noteProjection(n,e,.025f,50000);});cpu.join();
    require(begun==S_OK,"CPU begin and projection notes");
    if(n==1)require(shifts[0][0]==0&&shifts[0][1]==0&&shifts[1][0]==0&&shifts[1][1]==0,"first unknown input size is unjittered");
    if(n==2)require(shifts[0][0]!=0||shifts[0][1]!=0,"actual jitter engaged");
    for(unsigned order=0;order<2;++order){const unsigned eye=1-order;
      context->CSSetShaderResources(8,1,srv.GetAddressOf());context->CSSetUnorderedAccessViews(6,1,uav.GetAddressOf(),nullptr);
      ComPtr<ID3D11Texture2D> result;vr::VRTextureBounds_t bounds{};
      require(client.treat(n,eye,source[eye].Get(),nullptr,result,bounds)==S_OK,"real temporal evaluation");
      require(result&&result.Get()!=source[eye].Get(),"real owned output");D3D11_TEXTURE2D_DESC d{};result->GetDesc(&d);
      require(d.Width==(dlss?600:w)&&d.Height==(dlss?456:h),"TAA or DLSS output dimensions");
      require(bounds.uMin==0&&bounds.vMin==0&&bounds.uMax==1&&bounds.vMax==1,"output bounds");
      ComPtr<ID3D11ShaderResourceView> afterSrv;ComPtr<ID3D11UnorderedAccessView> afterUav;
      context->CSGetShaderResources(8,1,&afterSrv);context->CSGetUnorderedAccessViews(6,1,&afterUav);
      require(afterSrv.Get()==srv.Get()&&afterUav.Get()==uav.Get(),"caller SRV/UAV bindings restored");
      checkColor(device.Get(),context.Get(),result.Get(),colors[eye]);
    }
  }
  HRESULT closed=E_FAIL;std::thread cpu([&]{closed=client.close();});cpu.join();require(closed==S_OK,"CPU close without producer callback");
  context->ClearState();context->Flush();
  // Hook-owning graphics DLL remains process lifetime; no unsafe FreeLibrary.
  std::printf("native_temporal_gpu_test: mode=%s, eyes=18, checks=%u, failures=0\n",dlss?"dlss":"on",checks);
}
int wmain(int argc,wchar_t** argv){SetErrorMode(3);
  if(argc==2&&!wcscmp(argv[1],L"--dry-run")){std::puts("native_temporal_gpu_test: dry-run (no DLL, runtime, device or files)");return 0;}
  if(argc!=4||wcscmp(argv[1],L"--self-test")||(wcscmp(argv[3],L"on")&&wcscmp(argv[3],L"dlss")))return 2;
  try{run(argv[2],!wcscmp(argv[3],L"dlss"));return 0;}catch(const std::exception& e){std::printf("FAIL: %s\n",e.what());return 1;}
}

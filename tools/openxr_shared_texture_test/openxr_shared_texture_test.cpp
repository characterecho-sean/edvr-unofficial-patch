#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../../src/openxr/eye_capture.h"
#include "../../src/openxr/skybox_capture.h"
#include "../../src/openxr/shared_texture_transfer.h"
#include "../../src/openxr/immediate_executor.h"
#include "../../src/openxr/gpu_work_observer.h"
#include "../../src/openxr/owner_service.h"
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <thread>
#include <vector>
#include <algorithm>

using Microsoft::WRL::ComPtr;
using namespace edvr::openxr;
namespace {
unsigned checks=0, failures=0;
void check(bool ok,const char* why){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",why);std::fflush(stdout);}}
ID3D11Texture2D* const sentinel=reinterpret_cast<ID3D11Texture2D*>(static_cast<uintptr_t>(1));

class ProducerExecutor final : public ImmediateExecutor {
 public:
  bool start(){return owner_.start();}
  bool invoke(std::function<void()> fn) override {
    const unsigned call = calls_.fetch_add(1,std::memory_order_relaxed) + 1;
    if(!accepting_.load(std::memory_order_acquire))return false;
    if(rejectAt_.load(std::memory_order_acquire)==call)return false;
    const bool ran=owner_.invoke([this,fn=std::move(fn)]() mutable { thread_.store(GetCurrentThreadId(),std::memory_order_release);fn(); });
    return ran&&!failAfterRunning;
  }
  bool failAfterRunning=false; // Set only while no invocation is active.
  void refuse(){accepting_.store(false,std::memory_order_release);}
  void resume(){rejectAt_.store(0,std::memory_order_release);accepting_.store(true,std::memory_order_release);}
  void refuseAt(unsigned call){rejectAt_.store(call,std::memory_order_release);accepting_.store(true,std::memory_order_release);}
  unsigned calls()const{return calls_.load(std::memory_order_acquire);}
  DWORD thread()const{return thread_.load(std::memory_order_acquire);}
  bool stop(){return owner_.stop();}
 private:
  OwnerService owner_;
  std::atomic<bool> accepting_{true};
  std::atomic<unsigned> rejectAt_{0};
  std::atomic<unsigned> calls_{0};
  std::atomic<DWORD> thread_{0};
};

class RecordingObserver final : public GpuWorkObserver {
 public:
  struct Event { unsigned phase; bool begin; ID3D11DeviceContext* context; };
  RecordingObserver() { events.reserve(16); }
  void beginGpuWork(unsigned phase, ID3D11DeviceContext* context) noexcept override { events.push_back({phase,true,context}); }
  void endGpuWork(unsigned phase, ID3D11DeviceContext* context) noexcept override { events.push_back({phase,false,context}); }
  std::vector<Event> events;
};

struct Fixture {
  ComPtr<ID3D11Device> producer, consumer;
  ComPtr<ID3D11DeviceContext> producerContext, consumerContext;
  ProducerExecutor executor;
  bool initialized=false;
  ~Fixture(){if(initialized)check(transfer.shutdown(5000)==S_OK,"transfer shutdown");executor.stop();}
  SharedTextureTransfer transfer;
};

bool createDevice(IDXGIAdapter* adapter, ComPtr<ID3D11Device>& device,
                  ComPtr<ID3D11DeviceContext>& context,UINT flags=0){
  D3D_FEATURE_LEVEL level{};
  HRESULT hr=D3D11CreateDevice(adapter, adapter?D3D_DRIVER_TYPE_UNKNOWN:D3D_DRIVER_TYPE_WARP,
      nullptr, flags|D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context);
  if(hr==DXGI_ERROR_SDK_COMPONENT_MISSING) {
    device.Reset();context.Reset();
    hr=D3D11CreateDevice(adapter, adapter?D3D_DRIVER_TYPE_UNKNOWN:D3D_DRIVER_TYPE_WARP,
        nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context);
  }
  return hr==S_OK;
}
bool luid(ID3D11Device* device,LUID& out){
  ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC d{};
  if(FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))||FAILED(dxgi->GetAdapter(&adapter))||
     FAILED(adapter->GetDesc(&d)))return false;
  out=d.AdapterLuid;return true;
}
bool sameIdentity(IUnknown* a,IUnknown* b){
  ComPtr<IUnknown> aa,bb;return a&&b&&a->QueryInterface(IID_PPV_ARGS(&aa))==S_OK&&
      b->QueryInterface(IID_PPV_ARGS(&bb))==S_OK&&aa.Get()==bb.Get();
}
void checkDebug(ID3D11Device* device,const char* who){
  ComPtr<ID3D11InfoQueue> queue;
  const bool enabled=device->QueryInterface(IID_PPV_ARGS(&queue))==S_OK;
  std::printf("shared_texture debug=%s enabled=%u\n",who,enabled?1u:0u);
  if(!enabled)return;
  bool clean=true;
  for(UINT64 i=0;i<queue->GetNumStoredMessages();++i){
    SIZE_T size=0;if(FAILED(queue->GetMessage(i,nullptr,&size))){clean=false;break;}
    std::vector<unsigned char> storage(size);auto* message=reinterpret_cast<D3D11_MESSAGE*>(storage.data());
    if(FAILED(queue->GetMessage(i,message,&size))){clean=false;break;}
    if(message->Severity==D3D11_MESSAGE_SEVERITY_ERROR||message->Severity==D3D11_MESSAGE_SEVERITY_CORRUPTION){
      std::printf("D3D11 error (%s): %s\n",who,message->pDescription);clean=false;
    }
  }
  check(clean,"D3D11 debug layer has no errors or corruption");
}
bool drainContext(ID3D11DeviceContext* context){
  ComPtr<ID3D11Device> device;context->GetDevice(&device);
  D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT,0};ComPtr<ID3D11Query> query;
  if(device->CreateQuery(&desc,&query)!=S_OK)return false;
  context->End(query.Get());context->Flush();const auto start=GetTickCount64();
  for(;;){BOOL done=FALSE;HRESULT hr=context->GetData(query.Get(),&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if(hr==S_OK&&done)return true;if(FAILED(hr)||GetTickCount64()-start>=5000)return false;Sleep(1);}
}
void printAdapter(ID3D11Device* device,const char* who){
  ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC d{};
  if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))&&SUCCEEDED(dxgi->GetAdapter(&adapter))&&SUCCEEDED(adapter->GetDesc(&d)))
    std::printf("shared_texture adapter=%s name=%ls luid=%08lx:%08lx\n",who,d.Description,
      static_cast<unsigned long>(d.AdapterLuid.HighPart),static_cast<unsigned long>(d.AdapterLuid.LowPart));
}

bool makeFixture(Fixture& f,bool hardware){
  ComPtr<IDXGIFactory1> factory;
  if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return false;
  ComPtr<IDXGIAdapter> chosen;
  if(hardware){
    for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> candidate;DXGI_ADAPTER_DESC1 desc{};
      if(factory->EnumAdapters1(i,&candidate)!=S_OK)break;
      if(candidate->GetDesc1(&desc)==S_OK&&!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)){chosen=candidate;break;}}
    if(!chosen)return false;
  }
  if(!createDevice(chosen.Get(),f.producer,f.producerContext)||
     !createDevice(chosen.Get(),f.consumer,f.consumerContext))return false;
  if(!f.executor.start())return false;
  return f.transfer.initialize(f.producer.Get(),f.consumer.Get(),&f.executor)==S_OK && (f.initialized=true);
}

constexpr UINT W=19,H=13;
const DXGI_FORMAT kFormats[]={DXGI_FORMAT_R8G8B8A8_TYPELESS,DXGI_FORMAT_R8G8B8A8_UNORM,
  DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_TYPELESS,DXGI_FORMAT_B8G8R8A8_UNORM,
  DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
std::vector<unsigned char> pattern(DXGI_FORMAT format,UINT w,UINT h,unsigned seed){
  std::vector<unsigned char> p(size_t(w)*h*4);for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){
    auto* q=&p[(size_t(y)*w+x)*4];q[0]=static_cast<unsigned char>((x*17+y*3+seed)&255);
    q[1]=static_cast<unsigned char>((x*5+y*29+seed*7)&255);q[2]=static_cast<unsigned char>((x*31+y*11+seed*13)&255);
    q[3]=static_cast<unsigned char>((x*7+y*19+seed*23)&255);
  } (void)format; return p;
}
bool sourceTexture(Fixture& f,DXGI_FORMAT format,UINT w,UINT h,unsigned seed,ComPtr<ID3D11Texture2D>& out){
  return f.executor.invoke([&]{D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=1;d.ArraySize=1;
    d.Format=format;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    if(FAILED(f.producer->CreateTexture2D(&d,nullptr,&out)))return;auto p=pattern(format,w,h,seed);
    D3D11_BOX box{0,0,0,w,h,1};f.producerContext->UpdateSubresource(out.Get(),0,&box,p.data(),w*4,0);
  }) && bool(out);
}
bool readback(Fixture& f,ID3D11Texture2D* texture,DXGI_FORMAT format,UINT w,UINT h,const std::vector<unsigned char>& expected){
  if(!texture||expected.size()!=size_t(w)*h*4)return false;
  D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
  if(d.Width!=w||d.Height!=h||d.ArraySize!=1||d.MipLevels!=1||d.SampleDesc.Count!=1||d.MiscFlags)return false;
  d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;if(FAILED(f.consumer->CreateTexture2D(&d,nullptr,&staging)))return false;
  f.consumerContext->CopyResource(staging.Get(),texture);f.consumerContext->Flush();D3D11_MAPPED_SUBRESOURCE m{};
  if(FAILED(f.consumerContext->Map(staging.Get(),0,D3D11_MAP_READ,0,&m)))return false;
  bool same=true;for(UINT y=0;y<h&&same;++y)same=std::memcmp(static_cast<unsigned char*>(m.pData)+size_t(y)*m.RowPitch,
      expected.data()+size_t(y)*w*4,w*4)==0;f.consumerContext->Unmap(staging.Get(),0);(void)format;return same;
}

void invalidInputs(Fixture& f){
  const auto initialCalls=f.executor.calls();
  ID3D11Texture2D* out=sentinel;
  check(f.transfer.copy(nullptr,out)!=S_OK&&!out,"null source rejected and output nulled");
  out=sentinel;check(f.transfer.receive(out)!=S_OK&&!out,"receive without pending rejected and output nulled");
  SharedTextureTransfer same;
  check(same.initialize(f.producer.Get(),f.producer.Get(),&f.executor)!=S_OK,"same device rejected");
  ComPtr<ID3D11Texture2D> consumerTexture;
  D3D11_TEXTURE2D_DESC d{};d.Width=W;d.Height=H;d.MipLevels=1;d.ArraySize=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  check(f.consumer->CreateTexture2D(&d,nullptr,&consumerTexture)==S_OK,"consumer invalid fixture texture");
  out=sentinel;check(f.transfer.copy(consumerTexture.Get(),out)!=S_OK&&!out,"wrong source device rejected and output nulled");
  for(const auto shape: {1u,2u,3u}){
    d.ArraySize=shape==1?2:1;d.MipLevels=shape==2?2:1;d.SampleDesc.Count=shape==3?2:1;
    ComPtr<ID3D11Texture2D> bad;check(f.producer->CreateTexture2D(&d,nullptr,&bad)==S_OK,"invalid shape fixture texture");
    out=sentinel;check(f.transfer.copy(bad.Get(),out)!=S_OK&&!out,"array mip or MSAA source rejected and output nulled");
  }
  d.ArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
  ComPtr<ID3D11Texture2D> unsupported;check(f.producer->CreateTexture2D(&d,nullptr,&unsupported)==S_OK,"unsupported format fixture texture");
  out=sentinel;check(f.transfer.copy(unsupported.Get(),out)!=S_OK&&!out,"unsupported format rejected and output nulled");
  check(f.executor.calls()==initialCalls,"invalid inputs request no producer callbacks");
}

void captureIntegration(Fixture& f) {
  EyeCapture sameEye;
  const unsigned beforeSameEye=f.executor.calls();
  check(sameEye.initializeShared(f.producer.Get(),f.producer.Get(),&f.executor)!=S_OK&&
        f.executor.calls()==beforeSameEye,
    "eye shared initialization rejects same device");
  EyeCapture eyes;
  check(eyes.initializeShared(f.producer.Get(),f.consumer.Get(),&f.executor)==S_OK,
    "eye shared initialization");
  ComPtr<ID3D11Texture2D> leftSource,rightSource,noRenderSource;
  check(sourceTexture(f,DXGI_FORMAT_R8G8B8A8_UNORM,W,H,211,leftSource),
    "left shared source created");
  check(sourceTexture(f,DXGI_FORMAT_B8G8R8A8_UNORM,W,H,212,rightSource),
    "right shared source created");
  check(sourceTexture(f,DXGI_FORMAT_R8G8B8A8_UNORM,W,H,213,noRenderSource),
    "no-render shared source created");
  const vr::Texture_t left{leftSource.Get(),vr::API_DirectX,vr::ColorSpace_Gamma};
  const vr::Texture_t right{rightSource.Get(),vr::API_DirectX,vr::ColorSpace_Linear};
  const vr::Texture_t noRender{noRenderSource.Get(),vr::API_DirectX,vr::ColorSpace_Linear};
  const vr::VRTextureBounds_t flipped{1.f,1.f,0.f,0.f};
  check(eyes.capture(vr::Eye_Left,&left,&flipped)==vr::VRCompositorError_None,
    "shared left capture");
  check(eyes.capture(vr::Eye_Right,&right)==vr::VRCompositorError_None,
    "shared right capture");
  ComPtr<ID3D11Texture2D> leftSnapshot=eyes.texture(vr::Eye_Left);
  ComPtr<ID3D11Texture2D> rightSnapshot=eyes.texture(vr::Eye_Right);
  check(leftSnapshot&&rightSnapshot&&leftSnapshot.Get()!=rightSnapshot.Get(),
    "shared eyes have independent private outputs");
  check(eyes.colorSpace(vr::Eye_Left)==vr::ColorSpace_Gamma&&
        eyes.colorSpace(vr::Eye_Right)==vr::ColorSpace_Linear&&
        eyes.bounds(vr::Eye_Left).uMin==1.f&&eyes.bounds(vr::Eye_Left).uMax==0.f,
    "shared eye metadata and flipped bounds");
  check(readback(f,leftSnapshot.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,W,H,pattern(DXGI_FORMAT_R8G8B8A8_UNORM,W,H,211))&&
        readback(f,rightSnapshot.Get(),DXGI_FORMAT_B8G8R8A8_UNORM,W,H,pattern(DXGI_FORMAT_B8G8R8A8_UNORM,W,H,212)),
    "shared eye pixels preserve both sources");
  const unsigned beforeNoRender=f.executor.calls();
  check(eyes.capture(vr::Eye_Left,&noRender,nullptr,vr::Submit_Default,false)==vr::VRCompositorError_None&&
        eyes.texture(vr::Eye_Left)==leftSnapshot.Get()&&eyes.colorSpace(vr::Eye_Left)==vr::ColorSpace_Gamma&&
        f.executor.calls()==beforeNoRender,
    "shared no-render validates without changing prior eye");

  ComPtr<ID3D11Texture2D> foreign;
  D3D11_TEXTURE2D_DESC foreignDesc{};foreignDesc.Width=W;foreignDesc.Height=H;foreignDesc.MipLevels=1;
  foreignDesc.ArraySize=1;foreignDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;foreignDesc.SampleDesc.Count=1;
  foreignDesc.Usage=D3D11_USAGE_DEFAULT;foreignDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  check(f.consumer->CreateTexture2D(&foreignDesc,nullptr,&foreign)==S_OK,
    "shared wrong-device source created");
  const vr::Texture_t wrong{foreign.Get(),vr::API_DirectX,vr::ColorSpace_Auto};
  const unsigned beforeWrong=f.executor.calls();
  check(eyes.capture(vr::Eye_Left,&wrong)==vr::VRCompositorError_TextureIsOnWrongDevice&&
        eyes.texture(vr::Eye_Left)==leftSnapshot.Get()&&f.executor.calls()==beforeWrong,
    "shared wrong-device eye rejected without mutation");
  EyeCapture previous;
  check(previous.initializeShared(f.producer.Get(),f.consumer.Get(),&f.executor)==S_OK,"shared previous pair initialized");
  const unsigned beforeExchange=f.executor.calls();
  check(eyes.exchangeBuffers(previous)&&f.executor.calls()==beforeExchange&&
    previous.texture(vr::Eye_Left)==leftSnapshot.Get()&&previous.texture(vr::Eye_Right)==rightSnapshot.Get(),
    "shared stereo buffers exchange without producer calls or copies");
  check(eyes.capture(vr::Eye_Left,&noRender)==vr::VRCompositorError_None&&
    readback(f,previous.texture(vr::Eye_Left),DXGI_FORMAT_R8G8B8A8_UNORM,W,H,pattern(DXGI_FORMAT_R8G8B8A8_UNORM,W,H,211))&&
    readback(f,previous.texture(vr::Eye_Right),DXGI_FORMAT_B8G8R8A8_UNORM,W,H,pattern(DXGI_FORMAT_B8G8R8A8_UNORM,W,H,212)),
    "next partial shared transfer cannot overwrite either saved eye");
  check(previous.bounds(vr::Eye_Left).uMin==1.f&&previous.colorSpace(vr::Eye_Right)==vr::ColorSpace_Linear,
    "shared replay preserves flipped bounds and color interpretation");
  check(previous.shutdownShared()==S_OK,"shared previous pair retires independently");previous.shutdown();
  eyes.reset();
  check(!eyes.texture(vr::Eye_Left)&&!eyes.texture(vr::Eye_Right),
    "shared eye reset clears published outputs");
  check(eyes.capture(vr::Eye_Left,&left)==vr::VRCompositorError_None&&eyes.texture(vr::Eye_Left),
    "shared eye slot reusable after reset");
  f.executor.refuse();const unsigned beforeEyeShutdown=f.executor.calls();
  check(eyes.shutdownShared(5000)==S_OK&&f.executor.calls()==beforeEyeShutdown,
    "shared eye shutdown is consumer-only after producer refusal");
  f.executor.resume();eyes.shutdown();

  SkyboxCapture skybox;
  SkyboxCapture sameSkybox;
  const unsigned beforeSameSkybox=f.executor.calls();
  check(sameSkybox.initializeShared(f.producer.Get(),f.producer.Get(),&f.executor)!=S_OK&&
        f.executor.calls()==beforeSameSkybox,
    "skybox shared initialization rejects same device");
  check(skybox.initializeShared(f.producer.Get(),f.consumer.Get(),&f.executor)==S_OK,
    "skybox shared initialization");
  ComPtr<ID3D11Texture2D> first[6],second[6],third[6];
  std::vector<unsigned char> firstPixels[6],secondPixels[6],thirdPixels[6];
  vr::Texture_t firstInputs[6]{},secondInputs[6]{},thirdInputs[6]{};
  bool sourcesReady=true;
  for(unsigned face=0;face<6;++face) {
    firstPixels[face]=pattern(kFormats[face],W,H,300+face);
    secondPixels[face]=pattern(kFormats[face],W,H,400+face);
    thirdPixels[face]=pattern(kFormats[face],W,H,500+face);
    sourcesReady=sourceTexture(f,kFormats[face],W,H,300+face,first[face])&&sourcesReady;
    sourcesReady=sourceTexture(f,kFormats[face],W,H,400+face,second[face])&&sourcesReady;
    sourcesReady=sourceTexture(f,kFormats[face],W,H,500+face,third[face])&&sourcesReady;
    firstInputs[face]={first[face].Get(),vr::API_DirectX,face&1?vr::ColorSpace_Linear:vr::ColorSpace_Gamma};
    secondInputs[face]={second[face].Get(),vr::API_DirectX,face&1?vr::ColorSpace_Gamma:vr::ColorSpace_Linear};
    thirdInputs[face]={third[face].Get(),vr::API_DirectX,vr::ColorSpace_Auto};
  }
  check(sourcesReady,"skybox shared source sets created");
  if(!sourcesReady) {
    check(skybox.shutdownShared(5000)==S_OK,"skybox shared cleanup after source failure");
    skybox.shutdown();
    return;
  }
  check(skybox.set(firstInputs,6)==vr::VRCompositorError_None&&skybox.ready(),
    "skybox first six-face set publishes");
  ID3D11Texture2D* firstPointers[6]{};
  for(unsigned face=0;face<6;++face) {
    firstPointers[face]=skybox.texture(face);
    check(firstPointers[face]&&skybox.colorSpace(face)==firstInputs[face].eColorSpace&&
          readback(f,firstPointers[face],kFormats[face],W,H,firstPixels[face]),
      "skybox first set metadata and pixels");
  }
  // The first two faces of the inactive bank complete; reject the first
  // producer callback of face two (initialize/create/copy = seven attempts).
  const unsigned middleAttempt=f.executor.calls()+7;
  f.executor.refuseAt(middleAttempt);
  check(skybox.set(secondInputs,6)==vr::VRCompositorError_InvalidTexture,
    "skybox middle-face executor refusal rejects transaction");
  f.executor.resume();
  bool preserved=true;
  for(unsigned face=0;face<6;++face)
    preserved=preserved&&skybox.texture(face)==firstPointers[face]&&
      skybox.colorSpace(face)==firstInputs[face].eColorSpace&&
      readback(f,skybox.texture(face),kFormats[face],W,H,firstPixels[face]);
  check(preserved,"skybox failed set preserves entire committed bank");
  check(skybox.set(secondInputs,6)==vr::VRCompositorError_None&&skybox.ready(),
    "skybox successful retry commits all six faces");
  for(unsigned face=0;face<6;++face)
    check(skybox.colorSpace(face)==secondInputs[face].eColorSpace&&
          readback(f,skybox.texture(face),kFormats[face],W,H,secondPixels[face]),
      "skybox retry metadata and pixels");

  // The third set returns to bank zero, exercising the reusable transfer
  // resources created by the first set rather than allocating a third bank.
  check(skybox.set(thirdInputs,6)==vr::VRCompositorError_None,"skybox reusable bank set");
  for(unsigned face=0;face<6;++face)
    check(skybox.texture(face)==firstPointers[face]&&
          skybox.colorSpace(face)==vr::ColorSpace_Auto&&
          readback(f,skybox.texture(face),kFormats[face],W,H,thirdPixels[face]),
      "skybox bank reuse updates pixels and metadata");

  vr::Texture_t wrongInputs[6]{};
  for(unsigned face=0;face<6;++face) wrongInputs[face]=thirdInputs[face];
  wrongInputs[3].handle=foreign.Get();
  const unsigned beforeSkyWrong=f.executor.calls();
  check(skybox.set(wrongInputs,6)==vr::VRCompositorError_TextureIsOnWrongDevice&&
        skybox.texture(0)==firstPointers[0]&&f.executor.calls()==beforeSkyWrong,
    "skybox wrong-device face rejected before transaction");
  skybox.clear();check(!skybox.ready()&&!skybox.texture(0),"shared skybox clear invalidates publication");
  f.executor.refuse();const unsigned beforeSkyShutdown=f.executor.calls();
  check(skybox.shutdownShared(5000)==S_OK&&f.executor.calls()==beforeSkyShutdown,
    "shared skybox shutdown is consumer-only after producer refusal");
  f.executor.resume();skybox.shutdown();
}

void deferredIntegration(Fixture& f) {
  for(auto format:kFormats) {
    SharedTextureTransfer transfer;
    check(transfer.initialize(f.producer.Get(),f.consumer.Get(),&f.executor)==S_OK,"deferred transfer initializes");
    ComPtr<ID3D11Texture2D> source;
    check(sourceTexture(f,format,W,H,701,source),"deferred source");
    TransferWallTimes times{};
    check(transfer.enqueue(source.Get(),100,&times)==S_OK&&transfer.pending()&&!transfer.ready(),"enqueue publishes one pending slot");
    check(transfer.enqueue(source.Get())==E_PENDING,"pending slot cannot be overwritten");
    HRESULT foreign=S_OK;
    std::thread caller([&]{foreign=transfer.enqueue(source.Get());});caller.join();
    check(foreign==E_ACCESSDENIED,"foreign enqueue does not touch the pending slot");
    check(f.executor.invoke([&]{auto bytes=pattern(format,W,H,999);
      f.producerContext->UpdateSubresource(source.Get(),0,nullptr,bytes.data(),W*4,0);
      f.producerContext->Flush();}),"source can be overwritten before consumer acquisition");
    const auto calls=f.executor.calls();f.executor.refuse();
    ID3D11Texture2D* output=nullptr;RecordingObserver observer;
    check(transfer.receive(output,5000,&observer,0,&times)==S_OK&&output&&!transfer.pending(),"consumer completes after producer admission closes");
    check(f.executor.calls()==calls&&observer.events.size()==2,"deferred receive brackets GPU copy without producer callbacks");
    check(readback(f,output,format,W,H,pattern(format,W,H,701)),"deferred pixels precede producer overwrite for every format");
    check(times.producerDispatch>=times.producerAcquire&&times.producerAcquire>=0&&times.producerFlush>=0&&
      times.consumerAcquire>=0&&times.consumerFlush>=0,"transfer wall measurements are finite ordered scopes");
    check(transfer.shutdown()==S_OK&&f.executor.calls()==calls,"deferred transfer retires without producer");
    f.executor.resume();
  }
  for(bool rightFirst:{false,true}) {
    EyeCapture eyes,previous;
    check(eyes.initializeShared(f.producer.Get(),f.consumer.Get(),&f.executor)==S_OK&&
      previous.initializeShared(f.producer.Get(),f.consumer.Get(),&f.executor)==S_OK,"deferred eye banks initialize");
    ComPtr<ID3D11Texture2D> source[2];vr::Texture_t textures[2]{};
    for(unsigned e=0;e<2;++e) {
      check(sourceTexture(f,DXGI_FORMAT_R8G8B8A8_UNORM,W,H,800+e,source[e]),"deferred eye input");
      textures[e]={source[e].Get(),vr::API_DirectX,vr::ColorSpace_Gamma};
    }
    const unsigned first=rightFirst?1u:0u,second=1u-first;
    check(eyes.capture(vr::EVREye(first),&textures[first],nullptr,vr::Submit_Default,true,nullptr,true)==vr::VRCompositorError_None&&
      eyes.hasPending()&&!eyes.texture(vr::EVREye(first)),"first eye returns with a snapshot but no readable consumer image");
    check(!eyes.exchangeBuffers(previous),"pending capture cannot become a retained stereo pair");
    check(eyes.capture(vr::EVREye(second),&textures[second],nullptr,vr::Submit_Default,true,nullptr,true)==vr::VRCompositorError_None,"second eye enqueued");
    const auto calls=f.executor.calls();f.executor.refuse();RecordingObserver observer;
    check(eyes.completePending(&observer)==S_OK&&!eyes.hasPending()&&f.executor.calls()==calls&&observer.events.size()==4,"both eyes consumed in one owner operation without producer calls");
    for(unsigned e=0;e<2;++e)check(readback(f,eyes.texture(vr::EVREye(e)),DXGI_FORMAT_R8G8B8A8_UNORM,W,H,
      pattern(DXGI_FORMAT_R8G8B8A8_UNORM,W,H,800+e)),"completed deferred pair has correct eye pixels");
    check(eyes.exchangeBuffers(previous),"only completed pair is retained");f.executor.resume();
    check(eyes.capture(vr::EVREye(first),&textures[first],nullptr,vr::Submit_Default,true,nullptr,true)==vr::VRCompositorError_None,"partial next pair enqueued");
    eyes.reset();check(!eyes.hasPending()&&!eyes.texture(vr::EVREye(first)),"clear discards pending metadata");
    check(eyes.completePending()==S_OK&&!eyes.texture(vr::EVREye(first)),"clear cannot republish discarded pending pixels");
    f.executor.refuse();const auto shutdownCalls=f.executor.calls();
    check(eyes.shutdownShared()==S_OK&&previous.shutdownShared()==S_OK&&f.executor.calls()==shutdownCalls,"shutdown drains abandoned first eye after producer stop");
    f.executor.resume();
  }
}

int benchmark(){
  std::setvbuf(stdout,nullptr,_IONBF,0);
  Fixture f;if(!makeFixture(f,true))return 1;
  printAdapter(f.producer.Get(),"benchmark");
  ComPtr<ID3D11Texture2D> sources[2];
  for(unsigned e=0;e<2;++e)if(!sourceTexture(f,DXGI_FORMAT_R8G8B8A8_UNORM,4068,4016,31+e,sources[e]))return 1;
  D3D11_TEXTURE2D_DESC readDesc{};readDesc.Width=readDesc.Height=readDesc.MipLevels=readDesc.ArraySize=1;
  readDesc.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;readDesc.SampleDesc.Count=1;
  readDesc.Usage=D3D11_USAGE_STAGING;readDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> readback;if(FAILED(f.consumer->CreateTexture2D(&readDesc,nullptr,&readback)))return 1;
  for(bool deferred:{false,true,true,false}) {
    std::printf("transfer_benchmark_begin,path=%s,warmup=32,samples=128\n",deferred?"pair_receive":"immediate_receive");
    struct Bank {
      SharedTextureTransfer eyes[2];
      ~Bank(){for(auto& eye:eyes)if(eye.shutdown()!=S_OK)ExitProcess(1);}
    } bank;
    auto& eyes=bank.eyes;
    for(auto& eye:eyes)if(eye.initialize(f.producer.Get(),f.consumer.Get(),&f.executor)!=S_OK)return 1;
    bool ok=true;std::vector<double> first,pair;first.reserve(128);pair.reserve(128);
    for(unsigned n=0;n<160&&ok;++n) {
      double firstMs=0,pairMs=0;ID3D11Texture2D* outputs[2]{};
      {SubmissionWallScope all(&pairMs);
        for(unsigned e=0;e<2;++e) {
          SubmissionWallScope eyeTimer(e==0?&firstMs:nullptr);
          const auto result=deferred?eyes[e].enqueue(sources[e].Get()):eyes[e].copy(sources[e].Get(),outputs[e]);
          if(result!=S_OK){ok=false;break;}
        }
        if(deferred&&ok)for(unsigned e=0;e<2;++e)if(eyes[e].receive(outputs[e])!=S_OK)ok=false;
      }
      // Complete both copies before the next sample, without including the
      // test-only readback in either measured interval.
      if(ok) {
        const D3D11_BOX box{0,0,0,1,1,1};D3D11_MAPPED_SUBRESOURCE mapped{};
        f.consumerContext->CopySubresourceRegion(readback.Get(),0,0,0,0,outputs[1],0,&box);
        if(FAILED(f.consumerContext->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped)))ok=false;
        else f.consumerContext->Unmap(readback.Get(),0);
      }
      if(n>=32&&ok){first.push_back(firstMs);pair.push_back(pairMs);}
    }
    for(auto& eye:eyes)if(eye.shutdown()!=S_OK)ok=false;
    if(!ok)return 1;
    std::sort(first.begin(),first.end());std::sort(pair.begin(),pair.end());
    std::printf("transfer_benchmark,path=%s,size=4068x4016,samples=%zu,first_p50=%.6f,pair_p50=%.6f,pair_p95=%.6f,pair_p99=%.6f,units=cpu_wall_ms,runtime=0,readback_included=0\n",
      deferred?"pair_receive":"immediate_receive",pair.size(),first[63],pair[63],pair[121],pair[126]);
  }
  return failures?1:0;
}

int run(bool hardware){
  Fixture f;if(!makeFixture(f,hardware)){check(false,"create two same-adapter devices and initialize");return 1;}
  LUID a{},b{};check(luid(f.producer.Get(),a)&&luid(f.consumer.Get(),b)&&std::memcmp(&a,&b,sizeof(a))==0,"devices share adapter LUID");
  printAdapter(f.producer.Get(),"producer");printAdapter(f.consumer.Get(),"consumer");
  std::printf("shared_texture consumer_thread=%lu\n",static_cast<unsigned long>(GetCurrentThreadId()));
  invalidInputs(f);
  captureIntegration(f);
  deferredIntegration(f);
  {
    ComPtr<ID3D11Device> singleProducer,singleConsumer;ComPtr<ID3D11DeviceContext> singleProducerContext,singleConsumerContext;
    check(createDevice(nullptr,singleProducer,singleProducerContext,D3D11_CREATE_DEVICE_SINGLETHREADED)&&
          createDevice(nullptr,singleConsumer,singleConsumerContext,D3D11_CREATE_DEVICE_SINGLETHREADED),"single-threaded devices created");
    SharedTextureTransfer rejected;const auto before=f.executor.calls();
    check(rejected.initialize(singleProducer.Get(),f.consumer.Get(),&f.executor)!=S_OK,
      "single-threaded producer initialization rejected");
    check(rejected.initialize(f.producer.Get(),singleConsumer.Get(),&f.executor)!=S_OK,
      "single-threaded consumer initialization rejected");
    check(f.executor.calls()==before,"single-threaded rejection precedes producer routing");
  }
  { SharedTextureTransfer empty;check(empty.initialize(f.producer.Get(),f.consumer.Get(),&f.executor)==S_OK&&empty.shutdown(5000)==S_OK,
      "initialize without copy shuts down cleanly"); }
  {
    ComPtr<ID3D11Texture2D> source;check(sourceTexture(f,DXGI_FORMAT_R8G8B8A8_UNORM,W,H,77,source),"rejection fixture source");
    f.executor.refuse();ID3D11Texture2D* rejected=nullptr;
    check(f.transfer.copy(source.Get(),rejected)!=S_OK&&!rejected,"executor rejection does not publish stale output");
    f.executor.resume();
  }
  for(auto format:kFormats)for(unsigned round=0;round<3;++round){
    const UINT w=W+(round==2?5:0),h=H+(round==2?3:0);ComPtr<ID3D11Texture2D> source;check(sourceTexture(f,format,w,h,round+1,source),"producer source created on owner thread");
    ID3D11Texture2D* output=nullptr;const auto beforeCopy=f.executor.calls();const auto started=GetTickCount64();
    RecordingObserver observer; const bool observe=format==kFormats[0]&&round==0;
    const HRESULT copied=f.transfer.copy(source.Get(),output,100,observe?&observer:nullptr,1);
    std::printf("shared_texture copy format=%u round=%u width=%u height=%u hr=%08lx elapsed_ms=%llu producer_attempts=%u pending=%u faulted=%u\n",
      unsigned(format),round,w,h,static_cast<unsigned long>(copied),GetTickCount64()-started,f.executor.calls()-beforeCopy,
      f.transfer.pending()?1u:0u,f.transfer.faulted()?1u:0u);
    check(copied==S_OK&&output,"copy returns borrowed consumer texture");
    if(observe) check(observer.events.size()==2&&observer.events[0].begin&&
      !observer.events[1].begin&&observer.events[0].phase==1&&observer.events[1].phase==1&&
      observer.events[0].context==f.consumerContext.Get()&&observer.events[1].context==f.consumerContext.Get(),
      "observer brackets one consumer copy with phase and context");
    check(!f.transfer.pending()&&f.transfer.ready(),"successful copy is immediately ready");
    ID3D11Texture2D* retry=sentinel;check(f.transfer.receive(retry,0)!=S_OK&&!retry,"receive rejects after successful copy");
    auto expected=pattern(format,w,h,round+1);if(output){
      ComPtr<ID3D11Device> outputDevice;D3D11_TEXTURE2D_DESC od{};output->GetDesc(&od);output->GetDevice(&outputDevice);
      check(sameIdentity(outputDevice.Get(),f.consumer.Get()),"output belongs to consumer device");
      const bool rgba=format==DXGI_FORMAT_R8G8B8A8_TYPELESS||format==DXGI_FORMAT_R8G8B8A8_UNORM||format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
      check(od.Width==w&&od.Height==h&&od.ArraySize==1&&od.MipLevels==1&&od.SampleDesc.Count==1&&od.MiscFlags==0&&
            od.Format==(rgba?DXGI_FORMAT_R8G8B8A8_TYPELESS:DXGI_FORMAT_B8G8R8A8_TYPELESS),"output is private typeless texture with exact shape and family");
      check(readback(f,output,format,w,h,expected),"consumer bytes preserve varied source rows");
      bool overwritten=false;
      check(f.executor.invoke([&]{auto p=pattern(format,w,h,99);D3D11_BOX box{0,0,0,w,h,1};
        f.producerContext->UpdateSubresource(source.Get(),0,&box,p.data(),w*4,0);
        overwritten=drainContext(f.producerContext.Get());})&&overwritten,"producer overwrite completes on GPU");
      check(readback(f,output,format,w,h,expected),"borrowed output remains immutable after producer overwrite");
    }
    check(f.transfer.ready(),"transfer remains reusable after receive rejection");
  }
  HRESULT wrongReceive=S_OK,wrongCopy=S_OK,wrongShutdown=S_OK;bool foreignOutputsCleared=false;
  const auto beforeForeign=f.executor.calls();
  std::thread t([&]{ID3D11Texture2D* o=sentinel;wrongReceive=f.transfer.receive(o,0);
    foreignOutputsCleared=!o;o=sentinel;wrongCopy=f.transfer.copy(nullptr,o,0);
    foreignOutputsCleared=foreignOutputsCleared&&!o;wrongShutdown=f.transfer.shutdown(0);});t.join();
  check(wrongReceive==E_ACCESSDENIED&&wrongCopy==E_ACCESSDENIED&&wrongShutdown==E_ACCESSDENIED&&foreignOutputsCleared,
    "foreign caller cannot copy, receive or shut down");
  check(f.executor.calls()==beforeForeign&&f.transfer.ready(),"foreign rejection leaves live transfer and producer untouched");
  {
    ComPtr<ID3D11Texture2D> source;check(sourceTexture(f,DXGI_FORMAT_B8G8R8A8_UNORM,W+5,H+3,64,source),"completed-callback fixture source");
    f.executor.failAfterRunning=true;ID3D11Texture2D* output=nullptr;
    const HRESULT hr=f.transfer.copy(source.Get(),output,5000);f.executor.failAfterRunning=false;
    check(hr==S_OK&&output,"completed producer work is preserved if executor reports false afterward");
    if(output)check(readback(f,output,DXGI_FORMAT_B8G8R8A8_UNORM,W+5,H+3,pattern(DXGI_FORMAT_B8G8R8A8_UNORM,W+5,H+3,64)),
      "completed callback pixels are preserved");
  }
  // Leave a fresh consumer copy without a staging readback before shutdown.
  ComPtr<ID3D11Texture2D> finalSource;ID3D11Texture2D* finalBorrowed=nullptr;
  check(sourceTexture(f,DXGI_FORMAT_R8G8B8A8_UNORM,1024,768,42,finalSource),"final source prepared");
  check(f.transfer.copy(finalSource.Get(),finalBorrowed,5000)==S_OK&&finalBorrowed,"final copy submitted before producer service closes");
  ComPtr<ID3D11Texture2D> heldOutput=finalBorrowed;
  const unsigned beforeShutdown=f.executor.calls();f.executor.refuse();
  check(f.transfer.shutdown(5000)==S_OK,"consumer shutdown completes after producer work");f.initialized=false;
  check(f.executor.calls()==beforeShutdown,"consumer shutdown made no producer callback after refusal");
  if(heldOutput)check(readback(f,heldOutput.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,1024,768,pattern(DXGI_FORMAT_R8G8B8A8_UNORM,1024,768,42)),
    "final pixels survived consumer-only shutdown via explicit test-owned reference");
  std::printf("shared_texture shutdown_producer_attempts_before=%u after=%u\n",beforeShutdown,f.executor.calls());
  f.executor.resume();check(f.transfer.initialize(f.producer.Get(),f.consumer.Get(),&f.executor)==S_OK,"transfer reinitializes with resumed producer");
  f.initialized=true;check(f.transfer.shutdown(5000)==S_OK,"reinitialized transfer shuts down");f.initialized=false;
  check(f.executor.stop(),"producer owner stops permanently");
  check(f.executor.thread()!=0&&f.executor.thread()!=GetCurrentThreadId(),"producer and consumer have distinct callers");
  checkDebug(f.producer.Get(),"producer");checkDebug(f.consumer.Get(),"consumer");
  std::printf("shared_texture producer_thread=%lu callbacks=%u\n",static_cast<unsigned long>(f.executor.thread()),beforeShutdown);
  std::printf("openxr_shared_texture_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
}

int main(int argc,char** argv){
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
  if(argc==2&&!std::strcmp(argv[1],"--dry-run")){std::puts("Would test two-device WARP shared texture transfer; no devices or files.");return 0;}
  if(argc==2&&!std::strcmp(argv[1],"--benchmark"))return benchmark();
  if(argc<2||std::strcmp(argv[1],"--self-test"))return 2;
  const bool hardware=argc==3&&!std::strcmp(argv[2],"--hardware");if(argc>3||(argc==3&&!hardware))return 2;
  std::thread watchdog([]{Sleep(30000);ExitProcess(124);});watchdog.detach();return run(hardware);
}

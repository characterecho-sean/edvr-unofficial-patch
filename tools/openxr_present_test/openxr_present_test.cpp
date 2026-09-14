#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include "../openxr_native_test/present_device.h"
#include "../../src/common/render_boundary.h"
#include "../../src/openxr/render_boundary_client.h"
#include "../../src/openxr/present_work_queue.h"
#include "../../src/openxr/present_quiescence.h"
#include "../../src/openxr/render_shutdown.h"
#include "../../src/openxr/owner_service.h"
#include "../../src/openxr/render_thread_dispatcher.h"
#include "../../src/openxr/graphics_bridge_client.h"
#include "../../src/openxr/native_graphics_client.h"
#include "../../src/openxr/native_render_binding.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace edvr::openxr;
namespace {
std::atomic<unsigned> checks{0}, failures{0};
void check(bool ok, const char* text) { ++checks; if (!ok) { ++failures; std::printf("FAIL: %s\n", text); } }
bool absolute(const std::wstring& p) { return p.size()>3 && p[1]==L':' && (p[2]==L'\\'||p[2]==L'/'); }
class Event {
 public:
  Event() : handle_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
  ~Event() { if (handle_) CloseHandle(handle_); }
  Event(const Event&) = delete;
  void signal() { SetEvent(handle_); }
  void reset() { ResetEvent(handle_); }
  bool wait(DWORD ms=3000) const { return handle_ && WaitForSingleObject(handle_, ms)==WAIT_OBJECT_0; }
 private: HANDLE handle_{};
};
struct Watchdog { Event done; std::thread t{[this]{ if (!done.wait(20000)) std::_Exit(1); }}; ~Watchdog(){done.signal();t.join();} };
bool reaches(const std::function<bool()>& f) {
  const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);
  while (!f()) { if (std::chrono::steady_clock::now()>=until) return false; std::this_thread::yield(); }
  return true;
}
struct Counts { using Fn=void (*)(uint64_t*,uint64_t*); Fn fn=nullptr; uint64_t privateBefore{}, unknownBefore{}; };

// Keep this fixture ABI-local so it can validate the exported contract before
// the client wrapper is exercised by other harnesses.
constexpr uint32_t kNativeGraphicsVersion = 1;
struct NativeGraphicsRequest { uint32_t size, version; };
struct NativeGraphicsTable {
  uint32_t size, version;
  ID3D11Device* device;
  ID3D11DeviceContext* context;
};
using AcquireNativeGraphics = HRESULT (WINAPI *)(const NativeGraphicsRequest*, NativeGraphicsTable*);
static_assert(sizeof(NativeGraphicsRequest)==8&&sizeof(NativeGraphicsTable)==24,"independent native graphics ABI");

bool nativeGraphicsContracts(HMODULE proxy, ID3D11Device* expectedDevice,
                             ID3D11DeviceContext* expectedContext, bool beforeCreation=false) {
  auto acquire=reinterpret_cast<AcquireNativeGraphics>(GetProcAddress(proxy,"edvrAcquireNativeGraphics"));
  check(acquire!=nullptr,"native graphics export exists"); if (!acquire) return false;
  NativeGraphicsRequest request{sizeof(request),kNativeGraphicsVersion};
  auto empty=[] { return NativeGraphicsTable{sizeof(NativeGraphicsTable),kNativeGraphicsVersion,nullptr,nullptr}; };
  auto reject=[&](NativeGraphicsRequest r) {
    auto table=empty();
    table.device=reinterpret_cast<ID3D11Device*>(1);
    table.context=reinterpret_cast<ID3D11DeviceContext*>(1);
    check(acquire(&r,&table)==E_INVALIDARG&&!table.size&&!table.version&&!table.device&&!table.context,
          "invalid native graphics request clears poisoned output");
  };
  auto r=request; r.size=sizeof(r)+4; reject(r); r=request; r.size=4; reject(r); r=request; r.version=2; reject(r);
  auto table=empty(); table.version=2; check(acquire(&request,&table)==E_INVALIDARG&&!table.size&&!table.version&&!table.device&&!table.context,"unknown native graphics version rejected");
  table=empty(); table.size+=8;
  check(acquire(&request,&table)==E_INVALIDARG&&!table.size&&!table.version&&!table.device&&!table.context,
        "oversized native graphics table rejected");
  check(acquire(nullptr,&table)==E_INVALIDARG,"null native graphics request rejected"); check(acquire(&request,nullptr)==E_INVALIDARG,"null native graphics table rejected");
  alignas(NativeGraphicsTable) unsigned char bytes[sizeof(NativeGraphicsTable)+16]; std::memset(bytes,0xcc,sizeof(bytes));
  uint32_t shortSize=4; std::memcpy(bytes,&shortSize,4);
  check(acquire(&request,reinterpret_cast<NativeGraphicsTable*>(bytes))==E_INVALIDARG,"short native graphics table rejected");
  bool canary=true; for(size_t i=4;i<sizeof(bytes);++i) canary &= bytes[i]==0xcc; check(canary,"short native graphics table canary preserved");
  if (beforeCreation) { table=empty(); check(acquire(&request,&table)==E_NOINTERFACE&&!table.device&&!table.context,"unpublished native graphics rejected before device creation"); return failures==0; }
  table=empty(); check(acquire(&request,&table)==S_OK&&table.device&&table.context,"native graphics snapshot succeeds before first Present");
  if (!table.device||!table.context) return false;
  check(table.device==expectedDevice&&table.context==expectedContext,"native graphics snapshot has exact device and context");
  table.device->Release(); table.context->Release();
  auto second=empty(); check(acquire(&request,&second)==S_OK&&second.device&&second.context,"repeated native graphics snapshot succeeds");
  if (second.device) second.device->Release(); if (second.context) second.context->Release();
  std::atomic<bool> foreignOk{false};
  std::thread foreign([&] { auto foreignTable=empty(); const bool ok=acquire(&request,&foreignTable)==S_OK&&foreignTable.device==expectedDevice&&foreignTable.context==expectedContext; if(foreignTable.device)foreignTable.device->Release(); if(foreignTable.context)foreignTable.context->Release(); foreignOk=ok; });
  foreign.join(); check(foreignOk,"native graphics snapshot succeeds on foreign Init thread");
  return failures==0;
}

void nativeClientContracts(HMODULE proxy, const std::wstring& path,
                           ID3D11Device* expectedDevice, ID3D11DeviceContext* expectedContext) {
  NativeGraphicsClient client;
  for (const auto& bad : {std::wstring(L"d3d11.dll"), std::wstring(L"C:d3d11.dll"),
                          std::wstring(L"C:\\bad\0name", 11)})
    check(client.acquire(bad)==E_INVALIDARG&&!client.provider(),"native client rejects non-absolute path");
  wchar_t systemDir[MAX_PATH]{}; GetSystemDirectoryW(systemDir,MAX_PATH); std::wstring systemPath=systemDir; systemPath+=L"\\kernel32.dll";
  check(client.acquire(systemPath)!=S_OK&&!client.provider(),"native client rejects system DLL without fallback");
  check(FAILED(client.acquire(path+L".not-loaded"))&&!client.provider(),"native client rejects unloaded absolute path");
  const DWORD renderThread=GetCurrentThreadId();
  std::thread init([&] {
    check(GetCurrentThreadId()!=renderThread,"native snapshot uses foreign Init caller");
    check(client.acquire(path)==S_OK&&client.provider()==proxy&&client.device()==expectedDevice&&client.context()==expectedContext,"native client acquires exact published graphics");
    check(client.acquire(path)==E_PENDING,"native client rejects repeated acquire");
    client.reset();
    check(!client.provider()&&!client.device()&&!client.context(),"native client reset releases snapshot");
    check(client.acquire(path)==S_OK&&client.device()==expectedDevice&&client.context()==expectedContext,"native client reacquires same identity");
    client.reset();
  });
  init.join();
}

void nativeRenderBindingContracts(HMODULE proxy, const std::wstring& path,
                                  PresentDevice& present) {
  NativeRenderBinding early;
  std::thread absent([&] {
    check(early.acquire(path)==S_OK,"foreign render binding acquires before Present");
    check(!early.waitForRender(std::chrono::milliseconds(5))&&early.work().pending()==0&&early.callbacks()==0&&early.renderThread()==0,
          "missing Present times out without a render caller or queued work");
    check(early.close()==S_OK&&early.release()==S_OK,"early render binding closes and releases");
  });
  absent.join();

  NativeRenderBinding binding;
  Event registered, allowQueue, entered, releaseCallback;
  const DWORD renderThread=GetCurrentThreadId();
  std::atomic<bool> invoked{false}, invokeResult{false}, acquired{false};
  std::thread init([&] {
    acquired=binding.acquire(path)==S_OK;
    check(acquired&&GetCurrentThreadId()!=renderThread,"render binding registration uses foreign Init caller");
    registered.signal();
    if(!acquired)return;
    const bool ready=binding.waitForRender(std::chrono::seconds(2));
    check(ready&&binding.provider()==proxy&&binding.device()==present.device()&&binding.context()==present.context(),
          "first callback publishes render readiness with paired device");
    if(!ready)return;
    if(!allowQueue.wait())return;
    invokeResult=binding.work().invoke([&] {
      invoked=GetCurrentThreadId()==renderThread&&binding.callbackActive();
      entered.signal();
      check(releaseCallback.wait(),"binding callback gets release signal");
    });
  });
  check(registered.wait(),"foreign registration completes before first Present");
  if(acquired) {
    check(SUCCEEDED(present.present()),"first Present binds discovered render caller");
    allowQueue.signal();
    check(reaches([&] { return binding.work().pending()==1; }),"Init queues work after render readiness");
    std::thread observer([&] {
      check(entered.wait(),"observer sees active binding callback");
      check(binding.close()==E_PENDING&&binding.release()==E_PENDING,
            "active binding retains callback and references during close/release");
      check(binding.provider()==proxy&&binding.device()==present.device(),"pending release retains graphics snapshot");
      releaseCallback.signal();
    });
    check(SUCCEEDED(present.present()),"next Present executes queued Init work");
    observer.join();
  }
  init.join();
  check(invokeResult&&invoked&&binding.callbacks()>0&&binding.correct()&&binding.renderThread()==renderThread,
        "binding executes Init work on observed Present caller");
  check(binding.release()==S_OK&&!binding.provider()&&!binding.device()&&!binding.context(),
        "binding releases graphics only after callback unwinds");
}

void nativeRenderBindingFrameWork(const std::wstring& path,
                                  PresentDevice& present) {
  std::atomic<unsigned> frameCalls{0}, queuedCalls{0}; std::atomic<DWORD> frameThread{0};
  std::atomic<bool> frameOrder{true};
  NativeRenderBinding binding;
  Event registered, firstPresented, queueReady;
  std::thread init([&] {
    const HRESULT acquired=binding.acquire(path,[&] {
      const unsigned call=++frameCalls; frameThread.store(GetCurrentThreadId(),std::memory_order_release);
      if(call==2) frameOrder.store(queuedCalls.load(std::memory_order_acquire)==1 &&
                                   binding.callbackActive(),std::memory_order_release);
    });
    check(acquired==S_OK,"frame work binding acquires on foreign Init caller");
    registered.signal();
    if(acquired!=S_OK)return;
    check(binding.waitForRender(std::chrono::seconds(2)),"frame work binding reaches Present");
    if(binding.renderThread()) {
      // waitForRender wakes inside the first Present callback, before its
      // queue pump. Submit only after that Present returns so this fixture
      // actually tests work waiting for the next Present, on every schedule.
      if(!firstPresented.wait()){check(false,"first frame Present returns before queuing");return;}
      queueReady.signal();
      const bool queued=binding.work().invoke([&]{++queuedCalls;});
      check(queued,"frame work test queues render work");
    }
  });
  check(registered.wait(),"frame work registration completes");
  check(SUCCEEDED(present.present()),"frame work first Present succeeds");
  firstPresented.signal();
  check(queueReady.wait()&&reaches([&]{return binding.work().pending()==1;}),
        "frame work request waits for next Present");
  check(SUCCEEDED(present.present()),"frame work second Present succeeds");
  init.join();
  check(frameCalls==2&&queuedCalls==1&&frameOrder&&frameThread==GetCurrentThreadId()&&
        frameThread==binding.renderThread()&&binding.correct(),
        "frame work runs after queued work on registered render thread");
  check(binding.close()==S_OK&&binding.release()==S_OK,"frame work binding stops cleanly");
  check(SUCCEEDED(present.present())&&frameCalls==2,
        "stopped binding prevents further frame work");

  std::atomic<bool> badRegistered{false}; std::atomic<unsigned> badFrames{0};
  NativeRenderBinding bad;
  std::thread badInit([&] {
    const HRESULT acquired=bad.acquire(path,[&] { ++badFrames; throw 7; });
    check(acquired==S_OK,"throwing frame work binding acquires"); badRegistered=true;
    if(acquired==S_OK)bad.waitForRender(std::chrono::seconds(2));
  });
  check(reaches([&]{return badRegistered.load(std::memory_order_acquire);}),"throwing frame work registration completes");
  check(SUCCEEDED(present.present()),"throwing frame work preserves Present result");
  badInit.join();
  check(badFrames==1&&!bad.callbackActive()&&!bad.correct(),"throwing frame work closes admission safely");
  check(bad.close()==S_OK&&bad.release()==S_OK,"throwing frame work lease retires after unwind");
}

struct PresentHost {
  PresentWorkQueue queue;
  ID3D11Device* device=nullptr; ID3D11DeviceContext* context=nullptr;
  DWORD renderThread{}; std::atomic<unsigned> callbacks{0}; std::atomic<bool> correct{true};
  std::atomic<bool> callbackActive{false};
  std::atomic<bool> presentReturned{true};
  std::atomic<bool> blockCallback{false}; Event entered, release;
  ~PresentHost() { queue.close(); }
  static HRESULT WINAPI callback(void* p, ID3D11Device* device, ID3D11DeviceContext* context) {
    auto& h=*static_cast<PresentHost*>(p);
    if (GetCurrentThreadId()!=h.renderThread || device!=h.device || context!=h.context) { h.correct=false; h.queue.close(); return E_ACCESSDENIED; }
    ++h.callbacks;
    h.callbackActive.store(true,std::memory_order_release);
    h.entered.signal();
    if (h.blockCallback.load(std::memory_order_acquire)) {
      const bool released=h.release.wait();
      check(released,"active callback receives release signal");
      if (!released) { h.callbackActive.store(false,std::memory_order_release); return E_ABORT; }
    }
    h.queue.pump();
    h.callbackActive.store(false,std::memory_order_release);
    return S_OK;
  }
};
struct FailureHost { unsigned calls=0; bool throws=false; };
HRESULT WINAPI failingCallback(void* p, ID3D11Device*, ID3D11DeviceContext*) {
  auto& h=*static_cast<FailureHost*>(p);
  ++h.calls;
  if (h.throws) throw 7;
  return E_FAIL;
}

bool boundaryContracts(HMODULE proxy, ID3D11Device* device) {
  auto acquire=reinterpret_cast<decltype(&edvrAcquireRenderBoundary)>(GetProcAddress(proxy,"edvrAcquireRenderBoundary"));
  check(acquire!=nullptr,"render boundary export exists"); if (!acquire) return false;
  EdvrRenderBoundaryRequest request{sizeof(request),EDVR_RENDER_BOUNDARY_VERSION_1,device,&PresentHost::callback,nullptr};
  auto empty=[] { return EdvrRenderBoundaryTable{sizeof(EdvrRenderBoundaryTable),EDVR_RENDER_BOUNDARY_VERSION_1}; };
  auto reject=[&](EdvrRenderBoundaryRequest r) { auto t=empty(); check(FAILED(acquire(&r,&t))&&!t.size&&!t.version&&!t.lease&&!t.close&&!t.release,"invalid request clears output"); };
  auto r=request; r.size=sizeof(r)+4; reject(r); r=request; r.size=4; reject(r); r=request; r.version=2; reject(r); r=request; r.device=nullptr; reject(r); r=request; r.callback=nullptr; reject(r);
  auto t=empty(); t.version=2; check(FAILED(acquire(&request,&t))&&!t.lease&&!t.close&&!t.release,"unknown table version rejected");
  check(acquire(nullptr,&t)==E_INVALIDARG,"null request rejected"); check(acquire(&request,nullptr)==E_INVALIDARG,"null table rejected");
  alignas(EdvrRenderBoundaryTable) unsigned char bytes[sizeof(EdvrRenderBoundaryTable)+16]; std::memset(bytes,0xcc,sizeof(bytes)); uint32_t shortSize=4; std::memcpy(bytes,&shortSize,4);
  check(acquire(&request,reinterpret_cast<EdvrRenderBoundaryTable*>(bytes))==E_INVALIDARG,"short table rejected"); bool canary=true; for (size_t i=4;i<sizeof(bytes);++i) canary &= bytes[i]==0xcc; check(canary,"short table canary preserved");
  return failures==0;
}

bool runActual(HMODULE proxy, IDXGISwapChain* chain, ID3D11Device* device, ID3D11DeviceContext* context, Counts counts) {
  PresentHost host; host.device=device; host.context=context; host.renderThread=GetCurrentThreadId();
  check(host.queue.bindCurrentThread(),"bind actual Present thread");
  OwnerService owner; RenderThreadDispatcher render(owner);
  const bool started=owner.start();
  check(started && render.bindCurrentThread(),"owner and render dispatcher setup");
  if (!started) return false;
  EdvrRenderBoundaryRequest request{sizeof(request),EDVR_RENDER_BOUNDARY_VERSION_1,device,&PresentHost::callback,&host};
  RenderBoundaryClient missing; check(FAILED(missing.acquire(GetModuleHandleW(L"kernel32.dll"),request))&&!missing.active(),"missing provider rejected without fallback");
  ComPtr<ID3D11Device> otherDevice; ComPtr<ID3D11DeviceContext> otherContext;
  check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
                                     &otherDevice,nullptr,&otherContext)),"independent same-adapter device");
  if (otherDevice) { auto foreign=request; foreign.device=otherDevice.Get(); auto rejected=EdvrRenderBoundaryTable{sizeof(EdvrRenderBoundaryTable),EDVR_RENDER_BOUNDARY_VERSION_1}; check(FAILED(reinterpret_cast<decltype(&edvrAcquireRenderBoundary)>(GetProcAddress(proxy,"edvrAcquireRenderBoundary"))(&foreign,&rejected)),"foreign same-adapter device rejected"); }
  RenderBoundaryClient client;
  Event registered;
  std::atomic<bool> startupDone{false};
  std::thread init([&]{
    check(GetCurrentThreadId()!=host.renderThread,"registration uses foreign Init thread");
    const HRESULT acquired=client.acquire(proxy,request);
    check(acquired==S_OK,"register foreign Init callback before first Present");
    registered.signal();
    if (acquired!=S_OK) return;
    startupDone=host.queue.invoke([&]{
    check(render.invokeOwner([&]{
      check(GetCurrentThreadId()!=host.renderThread,"XR work runs on owner thread");
        D3D11_TEXTURE2D_DESC d{}; d.Width=d.Height=d.ArraySize=d.MipLevels=d.SampleDesc.Count=1; d.Format=DXGI_FORMAT_R8G8B8A8_UNORM; d.BindFlags=D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target,game,staging;
        ComPtr<ID3D11RenderTargetView> rtv,gameRtv;
        ComPtr<ID3D11DeviceContext> deferred;
        ComPtr<ID3D11CommandList> list;
        bool resources=SUCCEEDED(device->CreateTexture2D(&d,nullptr,&target)) &&
            SUCCEEDED(device->CreateRenderTargetView(target.Get(),nullptr,&rtv)) &&
            SUCCEEDED(device->CreateTexture2D(&d,nullptr,&game)) &&
            SUCCEEDED(device->CreateRenderTargetView(game.Get(),nullptr,&gameRtv)) &&
            SUCCEEDED(device->CreateDeferredContext(0,&deferred));
        D3D11_TEXTURE2D_DESC stagingDesc=d; stagingDesc.Usage=D3D11_USAGE_STAGING;
        stagingDesc.BindFlags=0; stagingDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        resources=resources && SUCCEEDED(device->CreateTexture2D(&stagingDesc,nullptr,&staging));
        check(resources,"owner creates private, game and readback resources");
        if (!resources) return;
        const float cyan[]={0,1,1,1};
        ID3D11RenderTargetView* privateBinding=rtv.Get();
        deferred->OMSetRenderTargets(1,&privateBinding,nullptr);
        deferred->ClearRenderTargetView(rtv.Get(),cyan);
        const HRESULT recorded=deferred->FinishCommandList(FALSE,&list);
        check(SUCCEEDED(recorded),"owner records private command list");
        if (FAILED(recorded)) return;
      check(render.invoke([&]{
        check(GetCurrentThreadId()==host.renderThread,"immediate callback runs on Present thread");
        const float red[]={1,0,0,1};
        context->ClearRenderTargetView(gameRtv.Get(),red);
        ID3D11RenderTargetView* gameBinding=gameRtv.Get();
        context->OMSetRenderTargets(1,&gameBinding,nullptr);
        GraphicsBridgeClient bridge; check(bridge.acquire(proxy,device,context)==S_OK,"acquire bridge on immediate callback");
        check(bridge.execute(list.Get())==S_OK,"execute private command list");
        ComPtr<ID3D11RenderTargetView> restored;
        context->OMGetRenderTargets(1,&restored,nullptr);
        check(restored.Get()==gameRtv.Get(),"private execution restores game render target");
        auto pixel=[&](ID3D11Texture2D* source,uint32_t expected) {
          context->CopyResource(staging.Get(),source);
          D3D11_MAPPED_SUBRESOURCE mapped{};
          const HRESULT hr=context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped);
          check(SUCCEEDED(hr),"GPU readback completes");
          if (SUCCEEDED(hr)) {
            uint32_t value{}; std::memcpy(&value,mapped.pData,4);
            check(value==expected,"GPU readback has expected pixel");
            context->Unmap(staging.Get(),0);
          }
        };
        pixel(target.Get(),0xffffff00); pixel(game.Get(),0xff0000ff);
        if (counts.fn) { uint64_t a{},b{}; counts.fn(&a,&b); check(a==counts.privateBefore+1&&b==counts.unknownBefore,"private bridge counter advances exactly"); }
        context->OMSetRenderTargets(0,nullptr,nullptr);
        bridge.reset();
      }),"owner invokes immediate graphics callback");
    }),"Present work invokes XR owner");
  }); });
  check(registered.wait(),"registration completes without Present or immediate work");
  check(reaches([&]{return host.queue.pending()==1;}) ,"foreign Init queues startup work before Present");
  check(host.callbacks==0,"registration does not execute callback");
  RenderBoundaryClient duplicate;
  check(duplicate.acquire(proxy,request)==E_PENDING&&!duplicate.active(),"duplicate registration rejected");
  const unsigned before=host.callbacks; check(SUCCEEDED(chain->Present(0,DXGI_PRESENT_TEST)),"Present TEST succeeds"); check(host.callbacks==before&&host.queue.pending()==1,"Present TEST leaves startup work queued");
  const HRESULT presentResult=chain->Present(0,0); check(SUCCEEDED(presentResult),"ordinary Present preserves success");
  init.join(); check(startupDone,"startup work completed at actual Present"); check(host.callbacks==before+1&&host.correct,"callback exact device/context/thread");

  // A callback that fails closes admission automatically; a new lease is allowed after release.
  check(client.close()==S_OK&&client.release()==S_OK,"release after normal callback");
  check(SUCCEEDED(chain->Present(0,0))&&host.callbacks==before+1,"no callback after release");
  for (bool throws : {false,true}) {
    FailureHost failure{0,throws};
    EdvrRenderBoundaryRequest bad=request; bad.callback=&failingCallback; bad.user=&failure;
    RenderBoundaryClient failed; check(failed.acquire(proxy,bad)==S_OK,"register failure callback");
    check(SUCCEEDED(chain->Present(0,0)),"callback failure preserves Present result");
    check(SUCCEEDED(chain->Present(0,0))&&failure.calls==1,"failure automatically closes admission");
    check(failed.release()==S_OK,"failed callback releases after unwinding");
  }
  host.queue.close(); render.close(); check(owner.stop(),"owner joins after queue close");
  return failures==0;
}

bool closeOrdering(HMODULE proxy, IDXGISwapChain* chain, ID3D11Device* device, ID3D11DeviceContext* context) {
  PresentHost host; host.device=device; host.context=context; host.renderThread=GetCurrentThreadId(); check(host.queue.bindCurrentThread(),"close test queue bind");
  EdvrRenderBoundaryRequest req{sizeof(req),EDVR_RENDER_BOUNDARY_VERSION_1,device,&PresentHost::callback,&host}; RenderBoundaryClient client;
  const HRESULT acquired=client.acquire(proxy,req); check(acquired==S_OK,"close test registration");
  if (acquired!=S_OK) return false;
  host.blockCallback=true;
  std::thread helper([&]{ check(host.entered.wait(),"close helper sees callback entered"); check(client.close()==E_PENDING,"close reports active callback"); check(client.release()==E_PENDING,"release reports active callback"); auto duplicate=req; RenderBoundaryClient second; check(second.acquire(proxy,duplicate)==E_PENDING,"duplicate lease remains blocked while callback active"); host.release.signal(); });
  check(SUCCEEDED(chain->Present(0,0)),"close test Present succeeds"); helper.join(); host.queue.close(); check(client.release()==S_OK,"release succeeds after callback unwinds");
  check(SUCCEEDED(chain->Present(0,0))&&host.callbacks==1,"no callback after active lease releases");
  return failures==0;
}

void quiescentTeardown(HMODULE proxy, PresentDevice& present) {
  PresentHost host; host.device=present.device();host.context=present.context();
  host.renderThread=GetCurrentThreadId();check(host.queue.bindCurrentThread(),"quiescence queue bind");
  PresentQuiescence quiescence;
  EdvrRenderBoundaryRequest req{sizeof(req),EDVR_RENDER_BOUNDARY_VERSION_1,
    host.device,&PresentHost::callback,&host};
  RenderBoundaryClient client;
  const HRESULT acquired=client.acquire(proxy,req);
  check(acquired==S_OK,"quiescence callback registration");
  if(acquired!=S_OK)return;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width=desc.Height=desc.ArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
  desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> target,staging;ComPtr<ID3D11RenderTargetView> rtv;
  bool created=SUCCEEDED(host.device->CreateTexture2D(&desc,nullptr,&target))&&
    SUCCEEDED(host.device->CreateRenderTargetView(target.Get(),nullptr,&rtv));
  desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  created=created&&SUCCEEDED(host.device->CreateTexture2D(&desc,nullptr,&staging));
  check(created,"quiescence teardown GPU resources");
  if(!created)return;
  OwnerService owner;const bool started=owner.start();check(started,"quiescence XR owner started");
  if(!started)return;
  Event cleanupEntered,releaseCleanup;
  std::atomic<bool> retired{false},done{false};bool joined=false;
  std::thread system([&]{
    const bool stopped=quiescence.requestAndWait();
    check(stopped,"System caller waits for acknowledged render stop");
    if(stopped)joined=owner.stop([&]{
      check(GetCurrentThreadId()!=host.renderThread&&retired&&quiescence.stopped(),
        "runtime teardown uses XR owner only after callback retirement");
      // Model a runtime's own immediate-context work during destruction.
      // The main caller must remain out of Present while this executes.
      const float green[]{0,1,0,1};host.context->ClearRenderTargetView(rtv.Get(),green);
      host.context->CopyResource(staging.Get(),target.Get());
      D3D11_MAPPED_SUBRESOURCE mapped{};
      const HRESULT mappedResult=host.context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped);
      check(SUCCEEDED(mappedResult),"runtime teardown GPU readback completes");
      if(SUCCEEDED(mappedResult)) {
        uint32_t pixel{};std::memcpy(&pixel,mapped.pData,sizeof(pixel));
        check(pixel==0xff00ff00,"runtime teardown GPU result is correct");
        host.context->Unmap(staging.Get(),0);
      }
      cleanupEntered.signal();check(releaseCleanup.wait(),"runtime teardown released after message pumping");
    });
    done=true;
  });
  check(reaches([&]{return quiescence.stopRequested();}),"System requests stop before teardown");
  // A request may arrive just after the loop's last admission check. The
  // owner must still wait for this final Present to return and be acknowledged.
  check(SUCCEEDED(present.present())&&host.callbacks==1&&!done,
    "final real Present completes before quiescence acknowledgement");
  host.queue.close();retired=client.close()==S_OK&&client.release()==S_OK;
  const bool acknowledged=quiescence.acknowledge(retired);
  check(retired&&acknowledged,"render caller retires lease and acknowledges stop");
  check(cleanupEntered.wait(),"XR teardown begins after acknowledgement");
  for(unsigned i=0;i<8;++i)present.pumpMessages();
  check(host.callbacks==1&&!done,"message pumping leaves Present stopped throughout teardown");
  releaseCleanup.signal();system.join();
  check(joined&&done,"System joins XR teardown without another Present");
  owner.stop();
}

// The game cannot be assumed to stop presenting after System-thread Shutdown.
// Marshal the final owner stop through the one real Present callback instead:
// the callback keeps the render caller inside hookedPresent while the owner
// destroys its resources, then the System caller retires the lease.
void callbackTeardown(HMODULE proxy, PresentDevice& present) {
  PresentHost host; host.device=present.device(); host.context=present.context();
  host.renderThread=GetCurrentThreadId(); host.presentReturned=false;
  check(host.queue.bindCurrentThread(),"callback teardown queue bind");
  EdvrRenderBoundaryRequest req{sizeof(req),EDVR_RENDER_BOUNDARY_VERSION_1,
    host.device,&PresentHost::callback,&host};
  RenderBoundaryClient client;
  const HRESULT acquired=client.acquire(proxy,req);
  check(acquired==S_OK,"callback teardown registration");
  if(acquired!=S_OK)return;

  D3D11_TEXTURE2D_DESC desc{}; desc.Width=desc.Height=desc.ArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
  desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.BindFlags=D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> target,staging; ComPtr<ID3D11RenderTargetView> rtv;
  bool made=SUCCEEDED(host.device->CreateTexture2D(&desc,nullptr,&target))&&
    SUCCEEDED(host.device->CreateRenderTargetView(target.Get(),nullptr,&rtv));
  desc.BindFlags=0; desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  made=made&&SUCCEEDED(host.device->CreateTexture2D(&desc,nullptr,&staging));
  check(made,"callback teardown GPU resources");
  if(!made){client.close();client.release();return;}

  OwnerService owner; RenderThreadDispatcher render(owner);
  const bool ready=owner.start()&&render.bindCurrentThread();
  check(ready,"callback teardown owner setup");
  if(!ready){owner.stop();return;}
  Event cleanupEntered, cleanupReleased, systemDone;
  std::atomic<bool> cleanupOk{false}; std::atomic<bool> systemResult{false};
  std::thread system([&]{
    const auto stopped=shutdownAtRenderBoundary(host.queue,render,owner,[&]{
        // The callback is still active and the outer Present has not returned.
        check(owner.isOwner()&&GetCurrentThreadId()!=host.renderThread&&
              host.callbackActive.load(std::memory_order_acquire)&&
              !host.presentReturned.load(std::memory_order_acquire),
              "owner finalizer runs while Present callback is active");
        const float green[]={0,1,0,1}; host.context->ClearRenderTargetView(rtv.Get(),green);
        host.context->CopyResource(staging.Get(),target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr=host.context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped);
        check(SUCCEEDED(hr),"callback teardown GPU readback completes");
        if(SUCCEEDED(hr)){uint32_t pixel{};std::memcpy(&pixel,mapped.pData,4);
          check(pixel==0xff00ff00,"callback teardown GPU result is correct");
          host.context->Unmap(staging.Get(),0); cleanupOk=true;}
        cleanupEntered.signal();
        check(cleanupReleased.wait(),"callback teardown release signal");
    });
    systemResult=stopped.joined;
    check(stopped.entered,"System caller queues owner stop through Present"); systemDone.signal();
  });
  check(reaches([&]{return host.queue.pending()==1;}),"owner stop waits for real Present");
  std::thread leaseObserver([&]{
    check(host.entered.wait(),"lease observer sees active callback");
    check(client.close()==E_PENDING,"close reports callback active during owner stop");
    check(client.release()==E_PENDING,"release reports callback active during owner stop");
    check(cleanupEntered.wait(),"lease observer sees owner cleanup");
    check(host.callbackActive.load(std::memory_order_acquire)&&
          !host.presentReturned.load(std::memory_order_acquire),
          "Present remains inside callback during owner cleanup");
    cleanupReleased.signal();
  });
  check(SUCCEEDED(present.present()),"callback teardown Present succeeds");
  host.presentReturned.store(true,std::memory_order_release);
  leaseObserver.join();
  check(cleanupEntered.wait(),"owner cleanup was reached before Present returned");
  check(systemDone.wait(),"System caller returns after callback marshalled cleanup");
  system.join();
  check(client.close()==S_OK&&client.release()==S_OK,"lease retires after callback unwind");
  check(cleanupOk&&systemResult,"callback teardown completed owner cleanup");
  check(host.callbacks==1&&host.correct,"callback teardown used the owned Present context");
  host.queue.close();
  owner.stop();
}

enum class HookExpectation { Features, TransportOnly, Unavailable };

void transportContracts(HMODULE proxy, PresentDevice& present, bool available) {
  const auto hooks=reinterpret_cast<unsigned(__cdecl*)()>(GetProcAddress(proxy,"edvr_selftest_hooks"));
  const auto counts=reinterpret_cast<Counts::Fn>(GetProcAddress(proxy,"edvr_selftest_graphics_bridge"));
  check(hooks&&counts,"transport fixture observation exports exist");
  if (!hooks||!counts) return;
  auto* device=present.device(); auto* context=present.context();
  ComPtr<ID3D11DeviceContext> deferred; ComPtr<ID3D11CommandList> list;
  check(SUCCEEDED(device->CreateDeferredContext(0,&deferred)),"transport fixture deferred context");
  if (!deferred) return;
  deferred->ClearState();
  check(SUCCEEDED(deferred->FinishCommandList(FALSE,&list)),"transport fixture recorded list");
  if (!list) return;
  context->ClearState();
  uint64_t privateBefore{},unknownBefore{},privateAfter{},unknownAfter{};
  counts(&privateBefore,&unknownBefore);
  context->ExecuteCommandList(list.Get(),TRUE);
  context->ExecuteCommandList(list.Get(),FALSE);
  counts(&privateAfter,&unknownAfter);
  check(privateAfter==privateBefore&&unknownAfter==unknownBefore+(available?2:0),
        "unpermitted lists never become private, with either restore flag");
  check(hooks()==0,"vScreen ClearState and ExecuteCommandList hooks remain absent");

  GraphicsBridgeClient bridge;
  const HRESULT acquired=bridge.acquire(proxy,device,context);
  check(acquired==(available?S_OK:E_NOINTERFACE),"transport bridge availability matches requested mode");
  if (!available||acquired!=S_OK) return;
  std::atomic<HRESULT> foreign{S_OK};
  std::thread wrongThread([&]{foreign=bridge.execute(list.Get());}); wrongThread.join();
  check(foreign==E_ACCESSDENIED,"transport bridge rejects execution on a foreign thread");
  check(bridge.execute(list.Get())==S_OK,"minimal transport consumes private permit");
  counts(&privateAfter,&unknownAfter);
  check(privateAfter==privateBefore+1&&unknownAfter==unknownBefore+2,
        "minimal transport accounts for exactly one private execution");
  context->ExecuteCommandList(list.Get(),TRUE);
  counts(&privateAfter,&unknownAfter);
  check(privateAfter==privateBefore+1&&unknownAfter==unknownBefore+3,
        "private permit is not reusable by a subsequent game call");

  // A shared-table hook can see another context on the same adapter. It must
  // forward that call without admitting it as the registered owner's work.
  ComPtr<ID3D11Device> otherDevice; ComPtr<ID3D11DeviceContext> otherContext,otherDeferred;
  ComPtr<ID3D11CommandList> otherList;
  check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
      D3D11_SDK_VERSION,&otherDevice,nullptr,&otherContext)),"transport foreign device");
  if (otherDevice&&SUCCEEDED(otherDevice->CreateDeferredContext(0,&otherDeferred))) {
    otherDeferred->ClearState();
    check(SUCCEEDED(otherDeferred->FinishCommandList(FALSE,&otherList)),"transport foreign list");
    if (otherList) {
      check(bridge.execute(otherList.Get())==E_INVALIDARG,"transport rejects same-adapter foreign list");
      otherContext->ExecuteCommandList(otherList.Get(),TRUE);
      counts(&privateAfter,&unknownAfter);
      check(privateAfter==privateBefore+1&&unknownAfter==unknownBefore+3,
            "foreign context does not change registered-owner counters");
    }
  } else check(false,"transport foreign deferred context");
  bridge.reset();
  check(hooks()==0,"transport testing did not activate optional vScreen hooks");
}

int selfTest(const std::wstring& supplied, HookExpectation expectation=HookExpectation::Features) {
  Watchdog watchdog; wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr,exe,MAX_PATH); std::wstring path=supplied;
  if (path.empty()) { path=exe; const auto slash=path.find_last_of(L"\\/"); path=path.substr(0,slash+1)+L"d3d11.dll"; }
  HMODULE proxy=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS); check(proxy!=nullptr,"load built graphics proxy"); if (!proxy) return 1;
  nativeGraphicsContracts(proxy,nullptr,nullptr,true);
  NativeGraphicsClient preClient;
  check(preClient.acquire(path)==E_NOINTERFACE&&!preClient.provider(),"native client rejects unavailable graphics before device creation");
  check(preClient.acquire(L"d3d11.dll")==E_INVALIDARG&&!preClient.provider(),"native client rejects relative path before device creation");
  auto countsFn=reinterpret_cast<Counts::Fn>(GetProcAddress(proxy,"edvr_selftest_graphics_bridge")); check(countsFn!=nullptr,"graphics bridge counter export exists");
  PresentDevice present; check(SUCCEEDED(present.initialize(proxy,D3D_DRIVER_TYPE_WARP)),"initialize real WARP proxy device"); if (!present.swapchain()) return 1;
  if (expectation!=HookExpectation::Features) {
    transportContracts(proxy,present,expectation==HookExpectation::TransportOnly);
    if (failures) return 1;
  }
  if (expectation==HookExpectation::Unavailable) {
    NativeGraphicsClient missing;
    check(missing.acquire(path)==E_NOINTERFACE&&!missing.provider(),"deliberate context probe leaves native discovery unavailable");
    EdvrRenderBoundaryRequest request{sizeof(request),EDVR_RENDER_BOUNDARY_VERSION_1,present.device(),&failingCallback,nullptr};
    RenderBoundaryClient boundary;
    check(boundary.acquire(proxy,request)==E_NOINTERFACE&&!boundary.active(),"deliberate context probe leaves callback registration unavailable");
    check(SUCCEEDED(present.present()),"ordinary Present works with transport deliberately unavailable");
    return failures?1:0;
  }
  nativeGraphicsContracts(proxy,present.device(),present.context());
  nativeClientContracts(proxy,path,present.device(),present.context());
  nativeRenderBindingContracts(proxy,path,present);
  nativeRenderBindingFrameWork(path,present);
  boundaryContracts(proxy,present.device());
  if (failures) return 1;
  Counts counts{countsFn}; if (counts.fn) counts.fn(&counts.privateBefore,&counts.unknownBefore);
  runActual(proxy,present.swapchain(),present.device(),present.context(),counts);
  closeOrdering(proxy,present.swapchain(),present.device(),present.context());
  quiescentTeardown(proxy,present);
  callbackTeardown(proxy,present);
  if (expectation==HookExpectation::TransportOnly) {
    const auto hooks=reinterpret_cast<unsigned(__cdecl*)()>(GetProcAddress(proxy,"edvr_selftest_hooks"));
    check(hooks&&hooks()==0,"full Present and cleanup suite left optional vScreen hooks absent");
    if (!failures) std::puts("transport_only: PASS (real WARP graphics, no OpenXR runtime)");
  }
  return failures?1:0;
}
}
int wmain(int argc, wchar_t** argv) {
  if (argc==2 && !std::wcscmp(argv[1],L"--dry-run")) { std::puts("Would test the actual WARP Present render-boundary hook; no windows, device, threads or writes created."); return 0; }
  if (argc==2 && !std::wcscmp(argv[1],L"--self-test")) { const int r=selfTest(L""); std::printf("openxr_present_test: %u checks, %u failures\n",checks.load(),failures.load()); return r; }
  if (argc==3 && !std::wcscmp(argv[1],L"--graphics-proxy") && absolute(argv[2])) { const int r=selfTest(argv[2]); std::printf("openxr_present_test: %u checks, %u failures\n",checks.load(),failures.load()); return r; }
  if (argc==4 && !std::wcscmp(argv[1],L"--graphics-proxy") && absolute(argv[2]) &&
      (!std::wcscmp(argv[3],L"--transport-only")||!std::wcscmp(argv[3],L"--unavailable"))) {
    const auto mode=!std::wcscmp(argv[3],L"--transport-only")?HookExpectation::TransportOnly:HookExpectation::Unavailable;
    const int r=selfTest(argv[2],mode); std::printf("openxr_present_test: %u checks, %u failures\n",checks.load(),failures.load());return r;
  }
  std::fputs("usage: openxr_present_test --dry-run|--self-test|--graphics-proxy ABSOLUTE_DLL [--transport-only|--unavailable]\n",stderr); return 2;
}

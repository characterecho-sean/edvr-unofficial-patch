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
#include "../../src/openxr/owner_service.h"
#include "../../src/openxr/render_thread_dispatcher.h"
#include "../../src/openxr/graphics_bridge_client.h"
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

struct PresentHost {
  PresentWorkQueue queue;
  ID3D11Device* device=nullptr; ID3D11DeviceContext* context=nullptr;
  DWORD renderThread{}; std::atomic<unsigned> callbacks{0}; std::atomic<bool> correct{true};
  std::atomic<bool> blockCallback{false}; Event entered, release;
  ~PresentHost() { queue.close(); }
  static HRESULT WINAPI callback(void* p, ID3D11Device* device, ID3D11DeviceContext* context) {
    auto& h=*static_cast<PresentHost*>(p);
    if (GetCurrentThreadId()!=h.renderThread || device!=h.device || context!=h.context) { h.correct=false; h.queue.close(); return E_ACCESSDENIED; }
    ++h.callbacks;
    h.entered.signal();
    if (h.blockCallback.load(std::memory_order_acquire)) {
      const bool released=h.release.wait();
      check(released,"active callback receives release signal");
      if (!released) return E_ABORT;
    }
    h.queue.pump();
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

int selfTest(const std::wstring& supplied) {
  Watchdog watchdog; wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr,exe,MAX_PATH); std::wstring path=supplied;
  if (path.empty()) { path=exe; const auto slash=path.find_last_of(L"\\/"); path=path.substr(0,slash+1)+L"d3d11.dll"; }
  HMODULE proxy=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS); check(proxy!=nullptr,"load built graphics proxy"); if (!proxy) return 1;
  auto countsFn=reinterpret_cast<Counts::Fn>(GetProcAddress(proxy,"edvr_selftest_graphics_bridge")); check(countsFn!=nullptr,"graphics bridge counter export exists");
  PresentDevice present; check(SUCCEEDED(present.initialize(proxy,D3D_DRIVER_TYPE_WARP)),"initialize real WARP proxy device"); if (!present.swapchain()) return 1;
  boundaryContracts(proxy,present.device());
  if (failures) return 1;
  Counts counts{countsFn}; if (counts.fn) counts.fn(&counts.privateBefore,&counts.unknownBefore);
  runActual(proxy,present.swapchain(),present.device(),present.context(),counts);
  closeOrdering(proxy,present.swapchain(),present.device(),present.context());
  return failures?1:0;
}
}
int wmain(int argc, wchar_t** argv) {
  if (argc==2 && !std::wcscmp(argv[1],L"--dry-run")) { std::puts("Would test the actual WARP Present render-boundary hook; no windows, device, threads or writes created."); return 0; }
  if (argc==2 && !std::wcscmp(argv[1],L"--self-test")) { const int r=selfTest(L""); std::printf("openxr_present_test: %u checks, %u failures\n",checks.load(),failures.load()); return r; }
  if (argc==3 && !std::wcscmp(argv[1],L"--graphics-proxy") && absolute(argv[2])) { const int r=selfTest(argv[2]); std::printf("openxr_present_test: %u checks, %u failures\n",checks.load(),failures.load()); return r; }
  std::fputs("usage: openxr_present_test --dry-run|--self-test|--graphics-proxy ABSOLUTE_DLL\n",stderr); return 2;
}

#pragma once
#include "../common/render_boundary.h"
#include <exception>

namespace edvr::openxr {

// CPU registration with an explicit, already-loaded trusted provider. The
// caller keeps user state alive and serializes acquire/close/release. Callback
// code and provider references remain retained until release reports S_OK.
class RenderBoundaryClient final {
 public:
  RenderBoundaryClient() = default;
  RenderBoundaryClient(const RenderBoundaryClient&) = delete;
  RenderBoundaryClient& operator=(const RenderBoundaryClient&) = delete;
  ~RenderBoundaryClient() { if(release()!=S_OK)std::terminate(); }

  HRESULT acquire(HMODULE provider, const EdvrRenderBoundaryRequest& request) {
    if(table_.lease)return E_PENDING;
    if(!provider)return E_INVALIDARG;
    const auto address=GetProcAddress(provider,"edvrAcquireRenderBoundary");
    if(!address)return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    HMODULE retained=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(address),&retained))return HRESULT_FROM_WIN32(GetLastError());
    if(retained!=provider){FreeLibrary(retained);return E_NOINTERFACE;}
    EdvrRenderBoundaryTable table{sizeof(table),EDVR_RENDER_BOUNDARY_VERSION_1};
    const auto acquireBoundary=reinterpret_cast<decltype(&edvrAcquireRenderBoundary)>(address);
    const auto r=acquireBoundary(&request,&table);
    if(FAILED(r)||table.size!=sizeof(table)||table.version!=EDVR_RENDER_BOUNDARY_VERSION_1||
       !table.lease||!table.close||!table.release) {
      // Trusted provider must not publish a callback lease on failure.
      if(table.lease)std::terminate();
      FreeLibrary(retained);return FAILED(r)?r:E_NOINTERFACE;
    }
    table_=table;module_=retained;return S_OK;
  }
  HRESULT close() { return table_.lease?table_.close(table_.lease):S_OK; }
  HRESULT release() {
    if(!table_.lease)return S_OK;
    const auto r=table_.release(table_.lease);
    if(r!=S_OK)return r; // active callback still borrows client state
    table_={};
    const auto module=module_;module_=nullptr;FreeLibrary(module);
    return S_OK;
  }
  bool active()const {return table_.lease!=nullptr;}
 private:
  EdvrRenderBoundaryTable table_{};
  HMODULE module_=nullptr;
};
}

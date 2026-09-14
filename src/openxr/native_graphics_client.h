#pragma once

#include "../common/native_graphics.h"
#include <string>
#include <utility>
#include <wrl/client.h>

namespace edvr::openxr {

// Discovery only: the embedding host supplies the trusted absolute path of
// its already loaded graphics proxy. This never loads a DLL or creates a
// device. References remain owned until reset, after all native users join.
// Acquiring a snapshot does not exclude game context use or reserve a lease.
class NativeGraphicsClient final {
 public:
  NativeGraphicsClient() = default;
  NativeGraphicsClient(const NativeGraphicsClient&) = delete;
  NativeGraphicsClient& operator=(const NativeGraphicsClient&) = delete;
  ~NativeGraphicsClient() { reset(); }

  HRESULT acquire(const std::wstring& trustedAbsolutePath) {
    if (module_) return E_PENDING;
    std::wstring wanted;
    if (!fullPath(trustedAbsolutePath, wanted)) return E_INVALIDARG;
    HMODULE retained = nullptr;
    if (!GetModuleHandleExW(0, wanted.c_str(), &retained))
      return HRESULT_FROM_WIN32(GetLastError());
    struct Reference {
      HMODULE module;
      ~Reference() { if (module) FreeLibrary(module); }
    } reference{retained};
    wchar_t loaded[32768]{};
    const DWORD length = GetModuleFileNameW(retained, loaded, 32768);
    std::wstring actual;
    if (!length || length >= 32768 || !fullPath(loaded, actual) ||
        CompareStringOrdinal(wanted.c_str(), -1, actual.c_str(), -1, TRUE) != CSTR_EQUAL)
      return E_ACCESSDENIED;

    // All paired entry points must live in this module. A forwarded export
    // cannot establish the required provider identity.
    FARPROC snapshot = nullptr;
    for (const char* name : {"edvrAcquireNativeGraphics", "edvrAcquireGraphicsBridge",
                            "edvrAcquireRenderBoundary"}) {
      const auto address = GetProcAddress(retained, name);
      if (!address) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
      HMODULE implementation = nullptr;
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
          reinterpret_cast<LPCWSTR>(address), &implementation))
        return HRESULT_FROM_WIN32(GetLastError());
      const bool same = implementation == retained;
      FreeLibrary(implementation);
      if (!same) return E_NOINTERFACE;
      if (!snapshot) snapshot = address;
    }
    const EdvrNativeGraphicsRequest request{sizeof(request), EDVR_NATIVE_GRAPHICS_VERSION_1};
    EdvrNativeGraphicsTable table{sizeof(table), EDVR_NATIVE_GRAPHICS_VERSION_1};
    const HRESULT result = reinterpret_cast<decltype(&edvrAcquireNativeGraphics)>(snapshot)(&request, &table);
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device.Attach(table.device);
    context.Attach(table.context);
    if (result != S_OK || table.size != sizeof(table) ||
        table.version != EDVR_NATIVE_GRAPHICS_VERSION_1 || !device || !context) {
      context.Reset();
      device.Reset();
      return FAILED(result) ? result : E_NOINTERFACE;
    }
    device_ = std::move(device);
    context_ = std::move(context);
    module_ = retained;
    reference.module = nullptr;
    return S_OK;
  }

  void reset() {
    context_.Reset();
    device_.Reset();
    if (module_) FreeLibrary(module_);
    module_ = nullptr;
  }
  HMODULE provider() const { return module_; }
  ID3D11Device* device() const { return device_.Get(); }
  ID3D11DeviceContext* context() const { return context_.Get(); }

 private:
  static bool fullPath(const std::wstring& input, std::wstring& output) {
    // Match the staged native runner's drive-absolute path contract. Reject
    // embedded NULs so the trusted string and Win32 name cannot disagree.
    if (input.size() <= 3 || input.find(L'\0') != std::wstring::npos ||
        !((input[0] >= L'A' && input[0] <= L'Z') ||
          (input[0] >= L'a' && input[0] <= L'z')) ||
        input[1] != L':' || (input[2] != L'\\' && input[2] != L'/')) return false;
    wchar_t path[32768]{};
    const DWORD length = GetFullPathNameW(input.c_str(), 32768, path, nullptr);
    if (!length || length >= 32768) return false;
    output.assign(path, length);
    return true;
  }
  HMODULE module_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
};

} // namespace edvr::openxr

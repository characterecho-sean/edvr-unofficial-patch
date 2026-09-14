#pragma once

#include "../common/graphics_bridge.h"

namespace edvr::openxr {

// Explicit paired-module binding for the staged renderer. The host supplies
// an already loaded, trusted graphics proxy and keeps its load reference until
// acquire returns. No basename lookup, DLL loading or legacy-device fallback.
// This client and the renderer require exclusive, externally serialized use.
class GraphicsBridgeClient final {
 public:
  GraphicsBridgeClient() = default;
  ~GraphicsBridgeClient() { reset(); }
  GraphicsBridgeClient(const GraphicsBridgeClient&) = delete;
  GraphicsBridgeClient& operator=(const GraphicsBridgeClient&) = delete;

  HRESULT acquire(HMODULE provider, ID3D11Device* device,
                  ID3D11DeviceContext* context) {
    if (table_.lease) return E_PENDING;
    if (!provider || !device || !context) return E_INVALIDARG;
    const auto address = GetProcAddress(provider, "edvrAcquireGraphicsBridge");
    if (!address) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    HMODULE retained = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(address), &retained))
      return HRESULT_FROM_WIN32(GetLastError());
    // Forwarded exports do not establish the required paired-module identity.
    if (retained != provider) {
      FreeLibrary(retained);
      return E_NOINTERFACE;
    }
    EdvrGraphicsBridgeRequest request{sizeof(request), EDVR_GRAPHICS_BRIDGE_VERSION_1,
                                      device, context};
    EdvrGraphicsBridgeTable table{sizeof(table), EDVR_GRAPHICS_BRIDGE_VERSION_1};
    const auto acquireBridge = reinterpret_cast<decltype(&edvrAcquireGraphicsBridge)>(address);
    const HRESULT result = acquireBridge(&request, &table);
    if (FAILED(result) || table.size != sizeof(table) ||
        table.version != EDVR_GRAPHICS_BRIDGE_VERSION_1 ||
        !table.lease || !table.execute || !table.release) {
      if (table.lease && table.release) table.release(table.lease);
      FreeLibrary(retained);
      return FAILED(result) ? result : E_NOINTERFACE;
    }
    module_ = retained;
    table_ = table;
    return S_OK;
  }

  HRESULT execute(ID3D11CommandList* list) const {
    return table_.lease ? table_.execute(table_.lease, list) : E_UNEXPECTED;
  }
  bool active() const { return table_.lease != nullptr; }
  void reset() {
    // The callback's code and retained D3D objects must outlive lease release.
    if (table_.lease) table_.release(table_.lease);
    table_ = {};
    if (module_) FreeLibrary(module_);
    module_ = nullptr;
  }

 private:
  HMODULE module_ = nullptr;
  EdvrGraphicsBridgeTable table_{};
};

} // namespace edvr::openxr

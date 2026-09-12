#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <functional>
#include <vector>

namespace {
struct Api {
  ~Api() { if (module) FreeLibrary(module); }
  HMODULE module = nullptr; PFN_xrGetInstanceProcAddr get = nullptr;
  PFN_xrEnumerateInstanceExtensionProperties ext = nullptr;
  PFN_xrCreateInstance create = nullptr; PFN_xrDestroyInstance destroy = nullptr;
  PFN_xrGetSystem getSystem = nullptr; PFN_xrEnumerateViewConfigurations views = nullptr;
  PFN_xrGetViewConfigurationProperties viewProps = nullptr;
  PFN_xrEnumerateViewConfigurationViews viewSizes = nullptr;
  PFN_xrEnumerateEnvironmentBlendModes blends = nullptr;
  PFN_xrGetSystemProperties systemProps = nullptr;
  PFN_xrGetD3D11GraphicsRequirementsKHR d3dReq = nullptr;
};
struct InstanceCleanup {
  Api& api;
  XrInstance value;
  ~InstanceCleanup() { if (value && api.destroy) api.destroy(value); }
};
int failCount = 0;
void say(const char* s) { std::puts(s); }
void result(const char* what, XrResult r) { std::printf("error,%s,%d\n", what, (int)r); ++failCount; }
bool ok(const char* what, XrResult r) { if (XR_FAILED(r)) { result(what, r); return false; } return true; }
bool loadFn(Api& a, XrInstance i, const char* n, PFN_xrVoidFunction* out) {
  XrResult r = a.get(i, n, out);
  if (r != XR_SUCCESS || !*out) {
    result(n, r != XR_SUCCESS ? r : XR_ERROR_FUNCTION_UNSUPPORTED);
    return false;
  }
  return true;
}
template<class T> bool load(Api& a, XrInstance i, const char* n, T& out) {
  PFN_xrVoidFunction p = nullptr; if (!loadFn(a, i, n, &p)) return false; out = reinterpret_cast<T>(p); return true;
}
std::string narrow(const wchar_t* p) {
  int n = WideCharToMultiByte(CP_UTF8, 0, p, -1, nullptr, 0, nullptr, nullptr);
  if (!n) return std::string(); std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, p, -1, &s[0], n, nullptr, nullptr);
  if (!s.empty() && s.back() == '\0') s.pop_back(); return s;
}
template<class T>
XrResult enumerateRetry(const std::function<XrResult(uint32_t, uint32_t*, T*)>& call,
                        std::vector<T>& out, T initial = T{}, uint32_t limit = 3) {
  out.clear();
  uint32_t count = 0;
  XrResult r = call(0, &count, nullptr);
  if (r != XR_SUCCESS || count == 0) return r;
  for (uint32_t attempt = 0; attempt < limit; ++attempt) {
    if (count > 4096) return XR_ERROR_LIMIT_REACHED;
    std::vector<T> buffer(count, initial);
    uint32_t written = 0;
    r = call(count, &written, buffer.data());
    if (r == XR_ERROR_SIZE_INSUFFICIENT && written > count) { count = written; continue; }
    if (r != XR_SUCCESS) return r;
    if (written > count) return XR_ERROR_RUNTIME_FAILURE;
    buffer.resize(written);
    out.swap(buffer);
    return XR_SUCCESS;
  }
  return XR_ERROR_SIZE_INSUFFICIENT;
}
int selfTest() {
  std::vector<uint32_t> values;
  auto fixed = [](uint32_t cap, uint32_t* written, uint32_t* data) { *written = 2; if (cap < 2) return cap ? XR_ERROR_SIZE_INSUFFICIENT : XR_SUCCESS; data[0] = 4; data[1] = 9; return XR_SUCCESS; };
  if (XR_FAILED(enumerateRetry<uint32_t>(fixed, values)) || values.size() != 2 || values[1] != 9) return 1;
  unsigned growthCalls = 0;
  auto grows = [&](uint32_t cap, uint32_t* written, uint32_t* data) { ++growthCalls; if (!cap) { *written = 1; return XR_SUCCESS; } *written = 3; if (cap < 3) return XR_ERROR_SIZE_INSUFFICIENT; data[0]=1; data[1]=2; data[2]=3; return XR_SUCCESS; };
  if (XR_FAILED(enumerateRetry<uint32_t>(grows, values)) || values.size() != 3 || growthCalls != 3 || values[2] != 3) return 2;
  auto empty = [](uint32_t, uint32_t* written, uint32_t*) { *written = 0; return XR_SUCCESS; };
  if (XR_FAILED(enumerateRetry<uint32_t>(empty, values)) || !values.empty()) return 3;
  auto fail = [](uint32_t, uint32_t*, uint32_t*) { return XR_ERROR_RUNTIME_FAILURE; };
  if (enumerateRetry<uint32_t>(fail, values) != XR_ERROR_RUNTIME_FAILURE) return 4;
  auto never = [](uint32_t cap, uint32_t* written, uint32_t*) { if (!cap) { *written = 1; return XR_SUCCESS; } *written = cap + 1; return XR_ERROR_SIZE_INSUFFICIENT; };
  if (enumerateRetry<uint32_t>(never, values, 0, 2) != XR_ERROR_SIZE_INSUFFICIENT || !values.empty()) return 5;
  auto invalid = [](uint32_t cap, uint32_t* written, uint32_t*) { *written = cap + 1; return XR_SUCCESS; };
  if (enumerateRetry<uint32_t>(invalid, values) != XR_ERROR_RUNTIME_FAILURE || !values.empty()) return 6;
  auto huge = [](uint32_t, uint32_t* written, uint32_t*) { *written = UINT32_MAX; return XR_SUCCESS; };
  if (enumerateRetry<uint32_t>(huge, values) != XR_ERROR_LIMIT_REACHED) return 7;
  auto fillError = [](uint32_t cap, uint32_t* written, uint32_t*) { *written = 1; return cap ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS; };
  if (enumerateRetry<uint32_t>(fillError, values) != XR_ERROR_RUNTIME_FAILURE) return 8;
  auto shrinks = [](uint32_t cap, uint32_t* written, uint32_t* data) { *written = cap ? 1 : 3; if (cap) data[0] = 7; return XR_SUCCESS; };
  if (enumerateRetry<uint32_t>(shrinks, values) != XR_SUCCESS || values.size() != 1 || values[0] != 7) return 9;
  std::vector<XrExtensionProperties> typed;
  auto initialized = [](uint32_t cap, uint32_t* written, XrExtensionProperties* data) {
    *written = 2;
    if (cap) for (unsigned i = 0; i < 2; ++i) {
      if (data[i].type != XR_TYPE_EXTENSION_PROPERTIES || data[i].next) return XR_ERROR_VALIDATION_FAILURE;
      data[i].extensionVersion = i + 10;
    }
    return XR_SUCCESS;
  };
  if (enumerateRetry<XrExtensionProperties>(initialized, typed, {XR_TYPE_EXTENSION_PROPERTIES}) != XR_SUCCESS || typed[1].extensionVersion != 11) return 10;
  say("openxr_probe: 10 production enumeration checks passed (no runtime/session)"); return 0;
}
}

int wmain(int argc, wchar_t** argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  bool self = false, session = false; std::wstring path;
  for (int n = 1; n < argc; ++n) { if (!_wcsicmp(argv[n], L"--self-test")) self = true; else if (!_wcsicmp(argv[n], L"--session")) session = true; else if (!_wcsicmp(argv[n], L"--loader") && n + 1 < argc) path = argv[++n]; else { say("error,arguments,unknown"); return 2; } }
  if (self) return argc == 2 ? selfTest() : 2;
  if (session) { say("error,session,disabled,Phase 0 probe never creates a session"); return 2; }
  if (path.empty() || path.size() < 3 || path[1] != L':' || (path[2] != L'\\' && path[2] != L'/')) { say("unavailable,loader,absolute_path_required"); return 2; }
  Api a; a.module = LoadLibraryExW(path.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!a.module) { std::printf("unavailable,loader,%lu,%s\n", GetLastError(), narrow(path.c_str()).c_str()); return 2; }
  a.get = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(a.module, "xrGetInstanceProcAddr"));
  if (!a.get) { say("unavailable,loader,no_xrGetInstanceProcAddr"); return 2; }
  if (!load(a, XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", a.ext) || !load(a, XR_NULL_HANDLE, "xrCreateInstance", a.create)) { return 2; }
  std::vector<XrExtensionProperties> exts;
  XrResult r = enumerateRetry<XrExtensionProperties>([&](uint32_t cap, uint32_t* written, XrExtensionProperties* data) { return a.ext(nullptr, cap, written, data); }, exts, {XR_TYPE_EXTENSION_PROPERTIES});
  if (!ok("xrEnumerateInstanceExtensionProperties", r)) { return 3; }
  bool d3d = false; say("extensions");
  for (const auto& e : exts) { std::printf("extension,%.*s,%u\n", XR_MAX_EXTENSION_NAME_SIZE, e.extensionName, e.extensionVersion); if (!strncmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_MAX_EXTENSION_NAME_SIZE)) d3d = true; }
  std::printf("capability,XR_KHR_D3D11_enable,%s\n", d3d ? "present" : "absent");
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  std::strncpy(ci.applicationInfo.applicationName, "EDVR OpenXR capability probe", XR_MAX_APPLICATION_NAME_SIZE - 1);
  std::strncpy(ci.applicationInfo.engineName, "EDVR", XR_MAX_ENGINE_NAME_SIZE - 1); ci.applicationInfo.applicationVersion = 0; ci.applicationInfo.engineVersion = 0; ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0); // These queries need only OpenXR 1.0.
  const char* enabled[1] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME}; if (d3d) { ci.enabledExtensionCount = 1; ci.enabledExtensionNames = enabled; }
  XrInstance instance = XR_NULL_HANDLE; r = a.create(&ci, &instance); if (!ok("xrCreateInstance", r)) { return 3; }
  InstanceCleanup cleanup{a, instance};
  bool required = load(a, instance, "xrDestroyInstance", a.destroy) && load(a, instance, "xrGetSystem", a.getSystem) && load(a, instance, "xrGetSystemProperties", a.systemProps) && load(a, instance, "xrEnumerateViewConfigurations", a.views) && load(a, instance, "xrEnumerateViewConfigurationViews", a.viewSizes) && load(a, instance, "xrEnumerateEnvironmentBlendModes", a.blends);
  if (!required) return 3;
  XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES}; PFN_xrGetInstanceProperties getProps = nullptr; if (load(a, instance, "xrGetInstanceProperties", getProps) && ok("xrGetInstanceProperties", getProps(instance, &ip))) std::printf("instance,%.*s,%llu\n", XR_MAX_RUNTIME_NAME_SIZE, ip.runtimeName, (unsigned long long)ip.runtimeVersion);
  XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
  si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  XrSystemId system = XR_NULL_SYSTEM_ID;
  if (!ok("xrGetSystem", a.getSystem(instance, &si, &system))) return 3;
  XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
  if (!ok("xrGetSystemProperties", a.systemProps(instance, system, &sp))) return 3;
  std::printf("system,%llu,%.*s,vendor=%u,max_size=%ux%u,max_layers=%u,orientation=%u,position=%u\n",
      (unsigned long long)system, XR_MAX_SYSTEM_NAME_SIZE, sp.systemName, sp.vendorId,
      sp.graphicsProperties.maxSwapchainImageWidth, sp.graphicsProperties.maxSwapchainImageHeight,
      sp.graphicsProperties.maxLayerCount, sp.trackingProperties.orientationTracking,
      sp.trackingProperties.positionTracking);
  std::vector<XrViewConfigurationType> types;
  if (!ok("xrEnumerateViewConfigurations", enumerateRetry<XrViewConfigurationType>(
      [&](uint32_t cap, uint32_t* n, XrViewConfigurationType* p) {
        return a.views(instance, system, cap, n, p);
      }, types))) return 3;
  bool stereo = false;
  for (auto type : types) {
    std::printf("view_configuration,%d\n", (int)type);
    stereo |= type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  }
  if (!stereo) { say("unavailable,PRIMARY_STEREO"); return 3; }
  const auto stereoType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  if (!load(a, instance, "xrGetViewConfigurationProperties", a.viewProps)) return 3;
  XrViewConfigurationProperties vp{XR_TYPE_VIEW_CONFIGURATION_PROPERTIES};
  if (!ok("xrGetViewConfigurationProperties", a.viewProps(instance, system, stereoType, &vp))) return 3;
  std::printf("stereo,fov_mutable=%u\n", vp.fovMutable);
  std::vector<XrViewConfigurationView> views;
  if (!ok("xrEnumerateViewConfigurationViews", enumerateRetry<XrViewConfigurationView>(
      [&](uint32_t cap, uint32_t* n, XrViewConfigurationView* p) {
        return a.viewSizes(instance, system, stereoType, cap, n, p);
      }, views, {XR_TYPE_VIEW_CONFIGURATION_VIEW}))) return 3;
  if (views.size() != 2) { say("error,primary_stereo_view_count"); return 3; }
  for (size_t j = 0; j < views.size(); ++j) {
    const auto& v = views[j];
    std::printf("view,%zu,recommended=%ux%u,max=%ux%u,samples=%u,max_samples=%u\n", j,
        v.recommendedImageRectWidth, v.recommendedImageRectHeight,
        v.maxImageRectWidth, v.maxImageRectHeight,
        v.recommendedSwapchainSampleCount, v.maxSwapchainSampleCount);
  }
  std::vector<XrEnvironmentBlendMode> blends;
  if (!ok("xrEnumerateEnvironmentBlendModes", enumerateRetry<XrEnvironmentBlendMode>(
      [&](uint32_t cap, uint32_t* n, XrEnvironmentBlendMode* p) {
        return a.blends(instance, system, stereoType, cap, n, p);
      }, blends))) return 3;
  for (auto blend : blends) std::printf("blend_mode,%d\n", (int)blend);
  if (!d3d) { say("unavailable,XR_KHR_D3D11_enable"); return 3; }
  if (d3d && load(a, instance, "xrGetD3D11GraphicsRequirementsKHR", a.d3dReq)) { XrGraphicsRequirementsD3D11KHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR}; if (ok("xrGetD3D11GraphicsRequirementsKHR", a.d3dReq(instance,system,&gr))) std::printf("d3d11_requirements,adapter_luid=%08lx:%08lx,min_feature_level=%d\n",(unsigned long)gr.adapterLuid.HighPart,(unsigned long)gr.adapterLuid.LowPart,(int)gr.minFeatureLevel); }
  ok("xrDestroyInstance", a.destroy(instance)); cleanup.value = XR_NULL_HANDLE;
  return failCount ? 3 : 0;
}

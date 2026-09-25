#include "payload.h"

#include <windows.h>

#include "probe.h"

#ifndef EDVR_VERSION_STRING
#define EDVR_VERSION_STRING "unknown"
#endif

namespace edvr::installer {
namespace {

bool findBlob(int id, const void** data, size_t* size) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return false;
    const DWORD bytes = SizeofResource(nullptr, res);
    HGLOBAL handle = LoadResource(nullptr, res);
    if (!handle || bytes == 0) return false;
    const void* p = LockResource(handle);
    if (!p) return false;
    *data = p;
    *size = bytes;
    return true;
}

int idFor(const std::string& item) {
    if (item == "d3d11") return IDR_EDVR_D3D11;
    if (item == "openvr") return IDR_EDVR_OPENVR;
    if (item == "native_graphics") return IDR_EDVR_D3D11;
    if (item == "native_runtime") return IDR_EDVR_OPENVR;
    if (item == "ini") return IDR_EDVR_INI;
    if (item == "ngx") return IDR_EDVR_NGX;
    if (item == "openxr_loader") return IDR_EDVR_OPENXR_LOADER;
    if (item == "openxr_license") return IDR_EDVR_OPENXR_LICENSE;
    if (item == "profile") return IDR_EDVR_PROFILE;
    return 0;
}

}  // namespace

bool payloadItem(const std::string& item, const void** data, size_t* size) {
    const int id = idFor(item);
    if (id == 0) return false;
    return findBlob(id, data, size);
}

const PayloadInfo& payloadInfo() {
    static PayloadInfo info = [] {
        PayloadInfo p;
        p.version = EDVR_VERSION_STRING;
#ifdef EDVR_INSTALLER_FLAT
        p.profile = "flat";
#endif

        const void* data = nullptr;
        size_t size = 0;
        if (payloadItem("d3d11", &data, &size)) {
            p.haveD3d11 = true;
            p.d3d11Sha = sha256Bytes(data, size);
        }
        if (payloadItem("openvr", &data, &size)) {
            p.haveOpenvr = true;
            p.openvrSha = sha256Bytes(data, size);
        }
        if (payloadItem("ngx", &data, &size)) {
            p.haveNgx = true;
            p.ngxSha = sha256Bytes(data, size);
        }
        const bool graphicsPresent = payloadItem("native_graphics", &data, &size);
        const bool graphics = graphicsPresent && validateNativePayloadBytes(data, size, NativeImageKind::Graphics);
        p.nativeGraphicsValid = graphics;
        const bool runtimePresent = payloadItem("native_runtime", &data, &size);
        const bool runtime = runtimePresent && validateNativePayloadBytes(data, size, NativeImageKind::Runtime);
        const bool loaderPresent = payloadItem("openxr_loader", &data, &size);
        if (loaderPresent) { p.haveOpenxrLoader = true; p.openxrLoaderSha = sha256Bytes(data, size); }
        const bool loader = loaderPresent && validateNativePayloadBytes(data, size, NativeImageKind::Loader);
        const bool license = payloadItem("openxr_license", &data, &size);
        p.nativePairValid = graphics && runtime && loader && license;
        if (license) { p.haveOpenxrLicense = true; p.openxrLicenseSha = sha256Bytes(data, size); }
        if (payloadItem("ini", &data, &size)) {
            p.iniText.assign(static_cast<const char*>(data), size);
        }
        if (payloadItem("profile", &data, &size)) {
            p.descriptorText.assign(static_cast<const char*>(data), size);
            p.descriptorSha = sha256Bytes(data, size);
        }
        return p;
    }();
    return info;
}

}  // namespace edvr::installer

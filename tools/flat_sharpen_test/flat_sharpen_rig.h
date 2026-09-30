// Shared by the two flat sharpening rigs (flat_sharpen_test.cpp, flat_sharpen_pass_test.cpp):
// counted checks, a WARP device on Windows' own d3d11 (never an import: EDVR's proxy sits
// beside the rigs), small texture helpers, and a log that can be read back.
#pragma once

#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "../../src/common/config.h"
#include "../../src/common/log.h"
#include "../../src/common/system_d3d11.h"

namespace rig {

using Microsoft::WRL::ComPtr;

inline unsigned g_checks = 0, g_failures = 0;

inline void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("FAIL: %s\n", what);
}

struct Warp {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    bool ok = false;
};

inline Warp makeWarp() {
    Warp w;
    const auto create = edvr::systemD3D11CreateDevice();
    check(create != nullptr, "system D3D11 loads");
    if (!create) return w;
    D3D_FEATURE_LEVEL level{};
    w.ok = SUCCEEDED(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                            D3D11_SDK_VERSION, &w.device, &level, &w.context));
    check(w.ok, "WARP device");
    return w;
}

inline ComPtr<ID3D11Texture2D> makeTexture(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt,
                                           UINT bind, const std::vector<uint8_t>* bytes = nullptr,
                                           UINT arraySize = 1) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = arraySize;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init{};
    if (bytes) {
        init.pSysMem = bytes->data();
        init.SysMemPitch = w * 4;
    }
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(dev->CreateTexture2D(&d, bytes && arraySize == 1 ? &init : nullptr, &t))) t.Reset();
    return t;
}

inline ComPtr<ID3D11ShaderResourceView> makeSrv(ID3D11Device* dev, ID3D11Resource* res,
                                                DXGI_FORMAT view) {
    D3D11_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = view;
    v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    v.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> s;
    if (FAILED(dev->CreateShaderResourceView(res, &v, &s))) s.Reset();
    return s;
}

// The texture's bytes, as a 4-byte-a-texel image: a copy to staging and a map.
inline std::vector<uint8_t> readBytes(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                                      ID3D11Texture2D* tex) {
    std::vector<uint8_t> out;
    if (!tex) return out;
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging))) return out;
    ctx->CopyResource(staging.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return out;
    out.resize(static_cast<size_t>(d.Width) * d.Height * 4);
    for (UINT y = 0; y < d.Height; ++y) {
        std::memcpy(&out[static_cast<size_t>(y) * d.Width * 4],
                    static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                    static_cast<size_t>(d.Width) * 4);
    }
    ctx->Unmap(staging.Get(), 0);
    return out;
}

inline std::string readWholeFile(const std::wstring& path) {
    std::string out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
    CloseHandle(f);
    return out;
}

// Runs `scenario` with a real log open in a scratch folder, closes it, and returns
// what was written: the text a reporter would paste. `tag` names the file.
inline std::string withLog(const wchar_t* tag, const std::function<void()>& scenario) {
    wchar_t tmp[MAX_PATH];
    const DWORD tn = GetTempPathW(MAX_PATH, tmp);
    if (tn == 0 || tn >= MAX_PATH) {
        check(false, "a temp folder for the log");
        return std::string();
    }
    const std::wstring dir = std::wstring(tmp) + L"edvr_flat_sharpen_rig_" +
                             std::to_wstring(GetCurrentProcessId());
    edvr::Config::get().set("log.enabled", "1");
    if (!edvr::Log::get().open(dir, tag)) {
        check(false, "the log opens in the scratch folder");
        return std::string();
    }
    scenario();
    edvr::Log::get().close();
    std::string text;
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = dir + L"\\edvr_" + tag + L"_*.log";
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring path = dir + L"\\" + fd.cFileName;
            text += readWholeFile(path);
            DeleteFileW(path.c_str());
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
    return text;
}

// How many lines of `text` contain `needle`.
inline int countLines(const std::string& text, const char* needle) {
    int n = 0;
    size_t at = 0;
    while ((at = text.find(needle, at)) != std::string::npos) {
        ++n;
        at += std::strlen(needle);
    }
    return n;
}

// The lines of `text` that contain `needle`, as written (the log's time prefix included).
inline std::vector<std::string> linesWith(const std::string& text, const char* needle) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find(needle) != std::string::npos) out.push_back(line);
        start = end + 1;
    }
    return out;
}

}  // namespace rig

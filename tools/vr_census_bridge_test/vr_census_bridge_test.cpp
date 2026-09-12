// Drives the actual shipping proxies. No installed game or real VR runtime is
// used. Each child has fresh DLL state, config, sentinels and logs beside its EXE.
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "../../src/openvr/compat/openvr_v0_9_20.h"

namespace fs = std::filesystem;
using Begin = BOOL (WINAPI*)(unsigned);
using GetGeneric = void* (__cdecl*)(const char*, vr::EVRInitError*);
using CreateD3D = HRESULT (WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*,
    IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

static void require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL census bridge: %s\n", why); std::exit(1); }
}
static fs::path exePath() {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    require(n > 0 && n < MAX_PATH, "executable path");
    return fs::path(path);
}

static int child(const char* mode) {
    const bool delayed = std::strcmp(mode, "delayed") == 0;
    const bool disabled = std::strcmp(mode, "disabled") == 0;
    const bool appGpu = std::strcmp(mode, "app-gpu") == 0 || std::strcmp(mode, "app-gpu-off") == 0;
    const fs::path stage = exePath().parent_path();
    const HMODULE gfx = LoadLibraryW((stage / "d3d11.dll").c_str());
    require(gfx != nullptr, "load graphics proxy");
    const auto ack = reinterpret_cast<Begin>(GetProcAddress(gfx, "edvrCensusBeginVr"));
    require(ack != nullptr, "export exists");
    require(!ack(1) && !ack(2), "uninitialized receiver cannot acknowledge");

    const HMODULE vrm = LoadLibraryW((stage / "vr" / "openvr_api.dll").c_str());
    require(vrm != nullptr, "load OpenVR proxy");
    const auto get = reinterpret_cast<GetGeneric>(GetProcAddress(vrm, "VR_GetGenericInterface"));
    require(get != nullptr, "generic interface export");
    vr::EVRInitError error = vr::VRInitError_Unknown;
    auto* compositor = static_cast<vr::IVRCompositor*>(get(vr::IVRCompositor_Version, &error));
    require(compositor && error == vr::VRInitError_None, "real typed compositor through proxy");
    if (delayed) require(compositor->WaitGetPoses(nullptr, 0, nullptr, 0) == 0,
                         "pose wait before graphics initialization");

    const auto create = reinterpret_cast<CreateD3D>(GetProcAddress(gfx, "D3D11CreateDeviceAndSwapChain"));
    require(create != nullptr, "device creation export");
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"edvr-census-hidden";
    require(RegisterClassW(&wc) != 0, "register hidden window");
    const HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED,
        0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    require(window != nullptr, "create hidden window"); // Never ShowWindow.
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1;
    desc.BufferDesc.Width = desc.BufferDesc.Height = 64;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* swap = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    require(SUCCEEDED(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &desc, &swap, &device, nullptr, &context)), "create WARP swapchain");
    require(!ack(2), "initialized receiver rejects unknown protocol");
    ID3D11Texture2D* back = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    require(SUCCEEDED(swap->GetBuffer(0, IID_PPV_ARGS(&back))), "get backbuffer");
    require(SUCCEEDED(device->CreateRenderTargetView(back, nullptr, &target)), "create RTV");
    const float clear[] = {0, 0, 0, 1};
    auto render = [&] {
        context->ClearRenderTargetView(target, clear);
        require(SUCCEEDED(swap->Present(0, 0)), "hidden Present");
    };
    if (appGpu) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = 64; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        ID3D11Texture2D *eye = nullptr, *copy = nullptr, *staging = nullptr;
        ID3D11RenderTargetView* eyeTarget = nullptr;
        require(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &eye)), "eye texture");
        require(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &copy)), "copied eye texture");
        require(SUCCEEDED(device->CreateRenderTargetView(eye, nullptr, &eyeTarget)), "eye target");
        td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        require(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &staging)), "readback texture");
        // Slot 16 must still forward the constant-buffer binding with its exact
        // arguments. A mistaken DrawAuto slot silently corrupts this state.
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ID3D11Buffer *cb = nullptr, *observed = nullptr;
        require(SUCCEEDED(device->CreateBuffer(&bd, nullptr, &cb)), "constant buffer");
        context->PSSetConstantBuffers(0, 1, &cb);
        context->PSGetConstantBuffers(0, 1, &observed);
        require(observed == cb, "PS constant-buffer ABI remains intact");
        if (observed) observed->Release();
        const float colour[] = {1, 0, 1, 1};
        vr::Texture_t submitted{copy, vr::API_DirectX, vr::ColorSpace_Auto};
        for (unsigned i = 0; i < 24; ++i) {
            require(compositor->WaitGetPoses(nullptr, 0, nullptr, 0) == 0, "app GPU pose wait");
            context->ClearRenderTargetView(eyeTarget, colour);
            context->CopyResource(copy, eye);
            require(compositor->Submit(vr::Eye_Left, &submitted, nullptr, static_cast<vr::EVRSubmitFlags>(0)) == 0, "app GPU left submit");
            require(compositor->Submit(vr::Eye_Right, &submitted, nullptr, static_cast<vr::EVRSubmitFlags>(0)) == 0, "app GPU right submit");
            require(SUCCEEDED(swap->Present(0, 0)), "app GPU Present");
        }
        // The test's readback is outside the measured pair, including this
        // explicit Flush. Production polling remains DONOTFLUSH.
        context->CopyResource(staging, copy); context->Flush();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)), "map copied pixels");
        for (unsigned y = 0; y < 64; ++y) {
            const auto* row = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch;
            for (unsigned x = 0; x < 64; ++x)
                require(row[4*x] == 255 && row[4*x+1] == 0 && row[4*x+2] == 255 && row[4*x+3] == 255,
                        "paired proxies preserve every copied pixel");
        }
        context->Unmap(staging, 0);
        require(SUCCEEDED(swap->Present(0, 0)), "drain completed frame samples");
        return 0;
    }
    for (unsigned i = 0; i < 70; ++i) render(); // Exhaust startup before signalling.
    for (unsigned i = 0; i < 70; ++i) {
        require(compositor->WaitGetPoses(nullptr, 0, nullptr, 0) == 0, "forward pose wait");
        render();
    }
    // Test idempotence only AFTER production WaitGetPoses has done its work.
    // Otherwise the harness itself could hide a missing production bridge.
    require((ack(1) != FALSE) == !disabled, "receiver enabled/disabled acknowledgement");
    for (unsigned i = 0; i < 70; ++i) render(); // An ACK must never refill budgets.

    // Leave hooks and their referenced COM objects alive until process exit.
    // DLL_PROCESS_DETACH flushes the logs; the parent reads them after exit.
    return 0;
}

static unsigned linesWith(const std::string& text, const std::string& a,
                          const std::string& b = "", const std::string& c = "") {
    unsigned count = 0;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line))
        if (line.find(a) != std::string::npos && line.find(b) != std::string::npos &&
            line.find(c) != std::string::npos) ++count;
    return count;
}
static std::string readLog(const fs::path& stage, const char* prefix) {
    std::string result;
    unsigned files = 0;
    for (const auto& entry : fs::directory_iterator(stage / "edvr_logs")) {
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) == 0 && entry.path().extension() == ".log") {
            std::ifstream in(entry.path(), std::ios::binary);
            require(in.good(), "read child log");
            result.assign(std::istreambuf_iterator<char>(in), {});
            ++files;
        }
    }
    require(files == 1, "exactly one fresh log for each DLL");
    return result;
}
static void verifyLogs(const fs::path& stage, bool delayed, bool disabled) {
    const std::string gfx = readLog(stage, "edvr_gfx_");
    const std::string vrlog = readLog(stage, "edvr_vr_");
    require(linesWith(vrlog, "VR order census bridge: acknowledged") == (disabled ? 0u : 1u),
            "production hook acknowledgement appears exactly once");
    require(linesWith(vrlog, "VR order census bridge: pending") == ((delayed || disabled) ? 1u : 0u),
            "unready receiver explicitly reported");
    require(linesWith(vrlog, "VR order census bridge: exhausted") == (disabled ? 1u : 0u),
            "disabled receiver exhausts bounded retries");
    require(linesWith(vrlog, "VR order census phase: vr") == 1, "one VR phase in OpenVR DLL");
    require(linesWith(gfx, "VR order census phase: vr") == (disabled ? 0u : 1u),
            "one VR phase only in enabled graphics DLL");
    for (const char* event : {"WaitEnter", "WaitExit"})
        require(linesWith(vrlog, std::string("event=") + event + " ", "phase=vr ") == 64,
                "VR wait samples saturate at 64");
    for (const char* phase : {"startup", "vr"}) {
        for (const char* event : {"PresentEnter", "PresentExit", "ClearRtv"}) {
            const unsigned expected = disabled ? 0u : std::strcmp(event, "ClearRtv") == 0 ? 16u : 64u;
            require(linesWith(gfx, std::string("event=") + event + " ",
                              std::string("phase=") + phase + " ") == expected,
                    "graphics startup and VR counts are independent and bounded");
        }
    }
    for (const auto* log : {&gfx, &vrlog})
        require(linesWith(*log, "VR order census:", "sample=65/") == 0,
                "frame event budget did not overflow");
}
static void verifyAppGpuLogs(const fs::path& stage, bool enabled) {
    const std::string gfx = readLog(stage, "edvr_gfx_");
    require(linesWith(gfx, "VR order census:") == 0, "local measurement independent of census");
    if (enabled) {
        require(linesWith(gfx, "Render-to-submit GPU: seq ", " valid, outer ", "frame ") > 0,
                "app GPU timing publishes an actual valid original frame");
        require(linesWith(gfx, "Render-to-submit GPU: seq ", "context ", "thread ") > 0,
                "app GPU result carries actual owner identity");
    } else {
        require(linesWith(gfx, "Render-to-submit GPU: disabled") == 1,
                "app GPU timing disabled line is explicit");
        require(linesWith(gfx, "Render-to-submit GPU: seq ") == 0,
                "app GPU off has no frame query results");
    }
}

static void runCase(const fs::path& build, const char* mode) {
    const bool appGpuOn = std::strcmp(mode, "app-gpu") == 0;
    const fs::path stage = build / (std::string("census-bridge-") + mode + "-" +
        std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    require(fs::create_directory(stage), "fresh unique stage");
    fs::create_directory(stage / "vr");
    fs::copy_file(exePath(), stage / "census_child.exe");
    fs::copy_file(build / "d3d11.dll", stage / "d3d11.dll");
    fs::copy_file(build / "openvr_api.dll", stage / "vr" / "openvr_api.dll");
    fs::copy_file(build / "vr_census_fakevr.dll", stage / "vr" / "openvr_api_orig.dll");
    const bool disabled = std::strcmp(mode, "disabled") == 0;
    auto writeIni = [&](const fs::path& path, bool enabled) {
        std::ofstream out(path, std::ios::binary);
        out << "[advanced]\nopenvr_census = " << (enabled ? "on" : "off") <<
            "\napp_gpu_timing = " << (appGpuOn ? "on" : "off") <<
            "\ncompositor_timing = off\ncontext_hook_mode = live\npanel_hooks_always = on" <<
            "\n[fix]\ntransition_flash = off\nhead_offset_gate = off\ntemporal_aa = off\n"
            "render_sharpness = 0\n[experimental]\nsupersample_resolve = off\n";
        out.close();
        require(out.good(), "write fixture config");
    };
    const bool appMode = std::strcmp(mode, "app-gpu") == 0 || std::strcmp(mode, "app-gpu-off") == 0;
    writeIni(stage / "edvr.ini", !disabled && !appMode);
    writeIni(stage / "vr" / "edvr.ini", !appMode);
    std::wstring command = L"\"" + (stage / "census_child.exe").wstring() + L"\" --child ";
    command.append(mode, mode + std::strlen(mode));
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, stage.c_str(), &si, &pi) != FALSE, "start child");
    const DWORD waited = WaitForSingleObject(pi.hProcess, 60000);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 2);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    std::printf("census bridge %s: child=%lu, logs %ls\n", mode, code, stage.c_str());
    require(waited == WAIT_OBJECT_0 && code == 0, "child completed successfully within timeout");
    if (std::strcmp(mode, "app-gpu") == 0 || std::strcmp(mode, "app-gpu-off") == 0)
        verifyAppGpuLogs(stage, std::strcmp(mode, "app-gpu") == 0);
    else
        verifyLogs(stage, std::strcmp(mode, "delayed") == 0, disabled);
}

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        if (argc == 3 && std::strcmp(argv[1], "--child") == 0) return child(argv[2]);
        require((argc == 3 || argc == 4) && std::strcmp(argv[1], "--self-test") == 0,
                "usage: --self-test <build-dir> [--dry-run]");
        const fs::path build = fs::absolute(argv[2]);
        for (const char* file : {"d3d11.dll", "openvr_api.dll", "vr_census_fakevr.dll"})
            require(fs::is_regular_file(build / file), "required built DLL exists");
        if (argc == 4) {
            require(std::strcmp(argv[3], "--dry-run") == 0, "unknown argument");
            std::puts("Would stage five isolated children and verify their graphics/VR logs; no files written.");
            return 0;
        }
        for (const char* mode : {"ready", "delayed", "disabled", "app-gpu", "app-gpu-off"}) runCase(build, mode);
        std::puts("PASS: actual paired DLLs preserve startup and VR budgets, retry unready receivers, report disabled receivers, and measure real paired render-to-submit frames with exact pixels.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL census bridge: %s\n", error.what());
        return 1;
    }
}

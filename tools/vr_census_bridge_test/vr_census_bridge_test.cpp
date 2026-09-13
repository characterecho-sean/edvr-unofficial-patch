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
#include <thread>
#include <vector>
#include "../../src/openvr/compat/openvr_v0_9_20.h"
#include "../../src/common/shutdown_census_api.h"

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
    const bool absent = std::strcmp(mode, "shutdown-absent") == 0;
    const fs::path stage = exePath().parent_path();
    const HMODULE gfx = absent ? nullptr : LoadLibraryW((stage / "d3d11.dll").c_str());
    const auto ack = gfx ? reinterpret_cast<Begin>(GetProcAddress(gfx, "edvrCensusBeginVr")) : nullptr;
    const auto beginShutdown = gfx ? reinterpret_cast<edvr::BeginShutdownCensus>(GetProcAddress(gfx, "edvrCensusBeginShutdown")) : nullptr;
    const auto endShutdown = gfx ? reinterpret_cast<edvr::EndShutdownCensus>(GetProcAddress(gfx, "edvrCensusEndShutdown")) : nullptr;
    if (!absent) {
        require(gfx && ack && beginShutdown && endShutdown, "graphics census exports exist");
        require(!ack(1) && !ack(2) && !beginShutdown(1), "uninitialized receiver cannot acknowledge");
    }

    const HMODULE vrm = LoadLibraryW((stage / "vr" / "openvr_api.dll").c_str());
    require(vrm != nullptr, "load OpenVR proxy");
    const auto get = reinterpret_cast<GetGeneric>(GetProcAddress(vrm, "VR_GetGenericInterface"));
    require(get != nullptr, "generic interface export");
    vr::EVRInitError error = vr::VRInitError_Unknown;
    auto* compositor = static_cast<vr::IVRCompositor*>(get(vr::IVRCompositor_Version, &error));
    require(compositor && error == vr::VRInitError_None, "real typed compositor through proxy");
    const auto shutdown = reinterpret_cast<void (__cdecl*)()>(GetProcAddress(vrm, "VR_ShutdownInternal"));
    const HMODULE fake = GetModuleHandleW((stage / "vr" / "openvr_api_orig.dll").c_str());
    require(fake && shutdown, "loaded fake and typed shutdown export");
    const auto configureShutdown = reinterpret_cast<void (WINAPI*)(HANDLE, HANDLE)>(GetProcAddress(fake, "edvrFakeShutdownConfigure"));
    const auto shutdownCount = reinterpret_cast<unsigned (WINAPI*)()>(GetProcAddress(fake, "edvrFakeShutdownCount"));
    require(configureShutdown && shutdownCount && shutdownCount() == 0, "fresh fake shutdown controls");
    unsigned expectedShutdowns = 0;
    auto shutdownWithoutPresent = [&] {
        shutdown();
        require(shutdownCount() == ++expectedShutdowns, "shutdown forwarded exactly once");
    };
    if (absent) {
        shutdownWithoutPresent();
        require(GetModuleHandleW((stage / "d3d11.dll").c_str()) == nullptr,
                "shutdown lookup never loads paired graphics");
        return 0;
    }
    if (delayed) shutdownWithoutPresent(); // Explicit unavailable before graphics setup.
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
    require(!beginShutdown(2), "shutdown receiver rejects unknown protocol");
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
        shutdownWithoutPresent(); // Both census keys off must leave forwarding intact.
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

    // Exhausted startup/VR logs must not suppress the independent shutdown
    // window. The fake remains inside the forwarded call until exactly three
    // owned Presents have completed on the actual render thread.
    IDXGIFactory* factory = nullptr;
    require(SUCCEEDED(swap->GetParent(IID_PPV_ARGS(&factory))), "owned swapchain factory");
    desc.OutputWindow = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED,
        0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    IDXGISwapChain* foreign = nullptr;
    require(desc.OutputWindow && SUCCEEDED(factory->CreateSwapChain(device, &desc, &foreign)),
            "foreign swapchain using the same device");
    using Present = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    const auto ownedPresent = reinterpret_cast<Present>((*reinterpret_cast<void***>(swap))[8]);
    const HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    require(entered && release, "shutdown handshake events");
    configureShutdown(entered, release);
    std::thread systemCaller([&] { shutdown(); });
    require(WaitForSingleObject(entered, 10000) == WAIT_OBJECT_0, "entered forwarded shutdown");
    require(SUCCEEDED(swap->Present(0, DXGI_PRESENT_TEST)), "test-only Present");
    require(FAILED(swap->Present(5, 0)), "invalid sync interval fails the real owned Present");
    require(SUCCEEDED(ownedPresent(foreign, 0, 0)), "hook forwards a foreign swapchain without counting it");
    for (unsigned i = 0; i < 3; ++i) render();
    require(SetEvent(release) != FALSE, "release forwarded shutdown");
    systemCaller.join();
    configureShutdown(nullptr, nullptr);
    CloseHandle(entered); CloseHandle(release);
    require(shutdownCount() == ++expectedShutdowns, "pumped shutdown forwarded exactly once");
    shutdownWithoutPresent(); // A measured zero must differ from unavailable.

    if (!disabled) {
        const auto token = beginShutdown(1);
        require(token != 0 && !beginShutdown(1), "busy shutdown window cannot be replaced");
        shutdownWithoutPresent(); // A refused observer must still forward once.
        edvr::ShutdownCensusSnapshot snapshot{};
        auto invalid = snapshot;
        --invalid.size;
        require(!endShutdown(token, &invalid) && invalid.size == sizeof(snapshot) - 1,
                "bad snapshot size rejected without writing it");
        require(!endShutdown(token + 1, &snapshot) && !endShutdown(token, nullptr),
                "stale token and null result cannot retire an active window");
        require(endShutdown(token, &snapshot) && snapshot.token == token && snapshot.samples == 0,
                "original window remains available after rejected observers");
        require(!endShutdown(token, &snapshot), "retired token cannot close twice");
    }

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
static std::string field(const std::string& line, const char* key) {
    const std::string prefix = std::string(key) + "=";
    std::istringstream words(line);
    std::string word;
    while (words >> word) if (word.rfind(prefix, 0) == 0) return word.substr(prefix.size());
    require(false, "missing shutdown census field");
    return {};
}
static void verifyShutdownLogs(const fs::path& stage, const char* mode) {
    const auto vrlog = readLog(stage, "edvr_vr_");
    const bool disabled = std::strcmp(mode, "disabled") == 0;
    const bool delayed = std::strcmp(mode, "delayed") == 0;
    const bool absent = std::strcmp(mode, "shutdown-absent") == 0;
    const bool appGpu = std::strcmp(mode, "app-gpu") == 0 || std::strcmp(mode, "app-gpu-off") == 0;
    std::vector<std::string> measured;
    unsigned unavailable = 0;
    std::istringstream lines(vrlog);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find("VR shutdown Present census:") == std::string::npos) continue;
        require(field(line, "point") == "native_callback_service", "explicit observed service point");
        const auto status = field(line, "status");
        if (status == "measured") measured.push_back(line);
        else {
            require(status == "unavailable", "explicit shutdown availability");
            require(field(line, "reason") == (absent ? "paired_module_unavailable" : "begin_rejected"),
                    "precise unavailable shutdown reason");
            ++unavailable;
        }
    }
    const unsigned expectedMeasured = disabled || absent || appGpu ? 0 : 2;
    const unsigned expectedUnavailable = appGpu ? 0 : disabled || delayed ? 2 : 1;
    require(measured.size() == expectedMeasured && unavailable == expectedUnavailable,
            "exact shutdown observation count including unavailable and disabled paths");
    for (unsigned i = 0; i < measured.size(); ++i) {
        const auto number = [&](const char* key) { return std::stoull(field(measured[i], key)); };
        require(number("call") && number("token") && field(measured[i], "reason") == "none",
                "successful snapshot has real call and token identity");
        require(number("begin_qpc") <= number("forward_begin_qpc") &&
                number("forward_begin_qpc") <= number("forward_end_qpc") &&
                number("forward_end_qpc") <= number("end_qpc"), "observer brackets actual forwarding");
        require(number("samples") == (i == 0 ? 3u : 0u),
                "only successful owned non-TEST boundary Presents count, zero remains measured");
        require(number("mixed_threads") == 0 && number("saturated") == 0, "single render thread with exact count");
        if (i == 0) {
            require(number("forward_begin_qpc") <= number("first_qpc") &&
                    number("first_qpc") <= number("last_qpc") &&
                    number("last_qpc") <= number("forward_end_qpc"), "samples occur strictly during fake shutdown");
            require(number("first_thread") && number("first_thread") == number("last_thread"),
                    "sample thread identity retained");
            std::istringstream exports(vrlog);
            bool matched = false;
            while (std::getline(exports, line)) {
                if (line.find("VR export census:") != std::string::npos &&
                    line.find("phase=begin name=VR_ShutdownInternal ") != std::string::npos &&
                    field(line, "record") == field(measured[i], "call")) {
                    require(std::stoull(field(line, "thread")) != number("first_thread"),
                            "actual shutdown and rendering use different callers");
                    matched = true;
                }
            }
            require(matched, "shutdown observation correlates to lifecycle record");
        } else {
            require(!number("first_qpc") && !number("last_qpc") && !number("first_thread") && !number("last_thread"),
                    "zero progress does not manufacture sample timestamps or caller identity");
        }
    }
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
    verifyShutdownLogs(stage, mode);
    if (std::strcmp(mode, "shutdown-absent") == 0) return;
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
            std::puts("Would stage six isolated children and verify their graphics/VR logs; no files written.");
            return 0;
        }
        for (const char* mode : {"ready", "delayed", "disabled", "app-gpu", "app-gpu-off", "shutdown-absent"}) runCase(build, mode);
        std::puts("PASS: actual paired DLLs preserve startup/VR budgets, measure independent shutdown Present progress, report unavailable receivers, forward shutdown once, and preserve render-to-submit pixels.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL census bridge: %s\n", error.what());
        return 1;
    }
}

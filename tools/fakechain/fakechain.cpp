// fakechain -- a stand-in for EDHM or ReShade, for testing the chain.
//
// Pretends to be another d3d11 proxy: exports D3D11CreateDevice, forwards it to
// the system d3d11.dll, and records that it was called. It deliberately exports
// ONLY that one function, so the fallback path gets tested too -- a real proxy
// wraps a handful of entry points and leaves the rest to the system copy.
//
// It also does work in DllMain, which is the whole point. That is what made the
// first chaining attempt crash: loading this from inside edvr's DllMain would
// run this DllMain under the loader lock. Loading it after DllMain returns must
// not crash, and this file is how that gets checked without needing EDHM.
#include <windows.h>

#include <cstdio>

namespace {

HMODULE g_system = nullptr;

typedef HRESULT(WINAPI* PFN_Create)(void*, UINT, HMODULE, UINT, const void*, UINT, UINT,
                                    void**, UINT*, void**);

void marker(const char* what) {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = 0;
    strcat_s(path, MAX_PATH, "fakechain_was_here.txt");

    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(f, what, static_cast<DWORD>(strlen(what)), &n, nullptr);
    WriteFile(f, "\r\n", 2, &n, nullptr);
    CloseHandle(f);
}

}  // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI D3D11CreateDevice(
    void* adapter, UINT driverType, HMODULE software, UINT flags, const void* levels,
    UINT numLevels, UINT sdk, void** device, UINT* got, void** ctx) {
    marker("D3D11CreateDevice went through fakechain");
    if (!g_system) {
        wchar_t sys[MAX_PATH]{};
        GetSystemDirectoryW(sys, MAX_PATH);
        wcscat_s(sys, MAX_PATH, L"\\d3d11.dll");
        g_system = LoadLibraryW(sys);
    }
    if (!g_system) return E_FAIL;
    auto real = reinterpret_cast<PFN_Create>(GetProcAddress(g_system, "D3D11CreateDevice"));
    if (!real) return E_FAIL;
    return real(adapter, driverType, software, flags, levels, numLevels, sdk, device, got,
                ctx);
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Deliberately non-trivial, the way a real mod's entry point is. Under
        // the loader lock this is the sort of thing that goes wrong.
        marker("fakechain DllMain ran");
    }
    return TRUE;
}

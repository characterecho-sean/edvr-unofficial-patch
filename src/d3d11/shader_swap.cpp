#include "shader_swap.h"

#include <windows.h>

#include <d3d11.h>

#include "../common/guard.h"
#include "../common/log.h"
#include "perf_monitor.h"   // the compile is an event with a duration

namespace edvr {
namespace {

// The FULL eleven-parameter signature. Ten was the shape of the project's
// first crash: without ppErrorMsgs the compiler wrote its error blob
// through the stack slot after the argument list. Named here once so no
// second caller can get it wrong.
typedef HRESULT(WINAPI* PFN_D3DCompile)(const void*, SIZE_T, const char*,
                                        const void*, void*, const char*,
                                        const char*, UINT, UINT, void**,
                                        void**);

// ID3DBlob through raw COM: 0-2 IUnknown, 3 GetBufferPointer, 4
// GetBufferSize. Raw rather than through d3dcompiler's headers so this
// file needs no import library and the DLL stays a runtime dependency
// that can simply be absent.
void* blobPtr(void* blob) {
    typedef void*(STDMETHODCALLTYPE* Fn)(void*);
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(blob))[3])(blob);
}
SIZE_T blobSize(void* blob) {
    typedef SIZE_T(STDMETHODCALLTYPE* Fn)(void*);
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(blob))[4])(blob);
}
void blobRelease(void* blob) {
    typedef ULONG(STDMETHODCALLTYPE* Fn)(void*);
    reinterpret_cast<Fn>((*reinterpret_cast<void***>(blob))[2])(blob);
}

FaultBudget g_budget("shaderSwap.compile", 5);
FaultBudget g_createBudget("shaderSwap.create", 5);

void compileInner(ID3D11DeviceContext* ctx, const char* hlsl, size_t hlslLen,
                  const char* entry, const char* name,
                  const SwapMacro* macros, const char* who,
                  ID3D11VertexShader** out) {
    HMODULE mod = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!mod) {
        Log::get().note("%s: d3dcompiler_47.dll not found; the swap stands "
                        "down and the game draws stock.", who);
        return;
    }
    PFN_D3DCompile compile =
        reinterpret_cast<PFN_D3DCompile>(GetProcAddress(mod, "D3DCompile"));
    if (!compile) {
        Log::get().note("%s: d3dcompiler_47.dll has no D3DCompile; the swap "
                        "stands down and the game draws stock.", who);
        return;
    }

    void* blob = nullptr;
    void* errors = nullptr;
    const HRESULT hr = compile(hlsl, hlslLen, name, macros, nullptr, entry,
                               "vs_5_0", 0, 0, &blob, &errors);
    if (errors) {
        // Errors are worth printing even when the compile succeeded --
        // those are warnings, and a warning in a shader written against
        // somebody else's bytecode is worth reading.
        Log::get().note("%s: shader compiler said: %.400s", who,
                        static_cast<const char*>(blobPtr(errors)));
        blobRelease(errors);
    }
    if (FAILED(hr) || !blob) {
        Log::get().note("%s: shader compile failed (0x%08X); the swap stands "
                        "down and the game draws stock.", who,
                        static_cast<unsigned>(hr));
        if (blob) blobRelease(blob);
        return;
    }

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (dev) {
        dev->CreateVertexShader(blobPtr(blob), blobSize(blob), nullptr, out);
        dev->Release();
    }
    blobRelease(blob);
    Log::get().note("%s: replacement vertex shader %s.", who,
                    *out ? "compiled" : "creation FAILED; drawing stock");
}

// The pixel form, compiled against ps_5_0 and created as a pixel shader.
// One function rather than a templated pair: the two bodies differ in the
// target string and the Create call, and the geyser lesson says FFI-shaped
// code is proof-read line by line, not generated.
void compileInnerPs(ID3D11DeviceContext* ctx, const char* hlsl,
                    size_t hlslLen, const char* entry, const char* name,
                    const SwapMacro* macros, const char* who,
                    ID3D11PixelShader** out) {
    HMODULE mod = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!mod) {
        Log::get().note("%s: d3dcompiler_47.dll not found; the swap stands "
                        "down and the game draws stock.", who);
        return;
    }
    PFN_D3DCompile compile =
        reinterpret_cast<PFN_D3DCompile>(GetProcAddress(mod, "D3DCompile"));
    if (!compile) {
        Log::get().note("%s: d3dcompiler_47.dll has no D3DCompile; the swap "
                        "stands down and the game draws stock.", who);
        return;
    }

    void* blob = nullptr;
    void* errors = nullptr;
    const HRESULT hr = compile(hlsl, hlslLen, name, macros, nullptr, entry,
                               "ps_5_0", 0, 0, &blob, &errors);
    if (errors) {
        Log::get().note("%s: shader compiler said: %.400s", who,
                        static_cast<const char*>(blobPtr(errors)));
        blobRelease(errors);
    }
    if (FAILED(hr) || !blob) {
        Log::get().note("%s: shader compile failed (0x%08X); the swap stands "
                        "down and the game draws stock.", who,
                        static_cast<unsigned>(hr));
        if (blob) blobRelease(blob);
        return;
    }

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (dev) {
        dev->CreatePixelShader(blobPtr(blob), blobSize(blob), nullptr, out);
        dev->Release();
    }
    blobRelease(blob);
    Log::get().note("%s: replacement pixel shader %s.", who,
                    *out ? "compiled" : "creation FAILED; drawing stock");
}

// The compute form: cs_5_0, CreateComputeShader, same contract.
void compileInnerCs(ID3D11DeviceContext* ctx, const char* hlsl,
                    size_t hlslLen, const char* entry, const char* name,
                    const SwapMacro* macros, const char* who,
                    ID3D11ComputeShader** out) {
    HMODULE mod = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!mod) {
        Log::get().note("%s: d3dcompiler_47.dll not found; the swap stands "
                        "down and the game draws stock.", who);
        return;
    }
    PFN_D3DCompile compile =
        reinterpret_cast<PFN_D3DCompile>(GetProcAddress(mod, "D3DCompile"));
    if (!compile) {
        Log::get().note("%s: d3dcompiler_47.dll has no D3DCompile; the swap "
                        "stands down and the game draws stock.", who);
        return;
    }
    void* blob = nullptr;
    void* errors = nullptr;
    const HRESULT hr = compile(hlsl, hlslLen, name, macros, nullptr, entry,
                               "cs_5_0", 0, 0, &blob, &errors);
    if (errors) {
        Log::get().note("%s: shader compiler said: %.400s", who,
                        static_cast<const char*>(blobPtr(errors)));
        blobRelease(errors);
    }
    if (FAILED(hr) || !blob) {
        Log::get().note("%s: shader compile failed (0x%08X); the swap stands "
                        "down and the game draws stock.", who,
                        static_cast<unsigned>(hr));
        if (blob) blobRelease(blob);
        return;
    }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (dev) {
        dev->CreateComputeShader(blobPtr(blob), blobSize(blob), nullptr, out);
        dev->Release();
    }
    blobRelease(blob);
    Log::get().note("%s: replacement compute shader %s.", who,
                    *out ? "compiled" : "creation FAILED; standing down");
}

}  // namespace

ID3D11ComputeShader* shaderSwapCreateCs(ID3D11DeviceContext* ctx,
                                      const void* bytecode, size_t bytecodeLen,
                                      const char* name, const char* who) {
    if (!ctx || !bytecode || !bytecodeLen) return nullptr;
    ID3D11ComputeShader* out = nullptr;
    const int64_t t0 = qpcNow();
    HRESULT hr = E_FAIL;
    guardedBudget(g_createBudget, [&] {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            hr = dev->CreateComputeShader(bytecode, bytecodeLen, nullptr, &out);
            dev->Release();
        }
    });
    if (FAILED(hr) && out) {
        out->Release();
        out = nullptr;
    }
    const int64_t frequency = qpcFrequency();
    const double ms = frequency > 0
        ? static_cast<double>(qpcNow() - t0) * 1000.0 / static_cast<double>(frequency)
        : 0.0;
    Log::get().note("%s: precompiled compute shader %s %s (0x%08X, %.3f ms).",
                    who, name, out ? "created" : "creation FAILED; standing down",
                    static_cast<unsigned>(hr), ms);
    return out;
}

// Every compile is an EVENT for the monitor's drop attribution, with its
// duration: a compile on the render thread is the mod's own classic hitch
// (the theater's 142 ms, the temporal pass's 6 s on issue #20), and a
// dropped frame that coincides with one is explained.
namespace {
struct CompileClock {
    int64_t t0 = qpcNow();
    ~CompileClock() {
        const double ms = qpcFrequency() > 0
                              ? static_cast<double>(qpcNow() - t0) * 1000.0 / static_cast<double>(qpcFrequency())
                              : 0.0;
        perfMonitorNoteEvent(kEvCompile, ms);
    }
};
}  // namespace

ID3D11ComputeShader* shaderSwapCompileCs(ID3D11DeviceContext* ctx,
                                         const char* hlsl, size_t hlslLen,
                                         const char* entry, const char* name,
                                         const SwapMacro* macros,
                                         const char* who) {
    if (!ctx || !hlsl || !hlslLen) return nullptr;
    ID3D11ComputeShader* out = nullptr;
    CompileClock clock;
    guardedBudget(g_budget, [&] {
        compileInnerCs(ctx, hlsl, hlslLen, entry, name, macros, who, &out);
    });
    return out;
}

ID3D11PixelShader* shaderSwapCompilePs(ID3D11DeviceContext* ctx,
                                       const char* hlsl, size_t hlslLen,
                                       const char* entry, const char* name,
                                       const SwapMacro* macros,
                                       const char* who) {
    if (!ctx || !hlsl || !hlslLen) return nullptr;
    ID3D11PixelShader* out = nullptr;
    CompileClock clock;
    guardedBudget(g_budget, [&] {
        compileInnerPs(ctx, hlsl, hlslLen, entry, name, macros, who, &out);
    });
    return out;
}

ID3D11VertexShader* shaderSwapCompileVs(ID3D11DeviceContext* ctx,
                                        const char* hlsl, size_t hlslLen,
                                        const char* entry, const char* name,
                                        const SwapMacro* macros,
                                        const char* who) {
    if (!ctx || !hlsl || !hlslLen) return nullptr;
    ID3D11VertexShader* out = nullptr;
    CompileClock clock;
    guardedBudget(g_budget, [&] {
        compileInner(ctx, hlsl, hlslLen, entry, name, macros, who, &out);
    });
    return out;
}

}  // namespace edvr

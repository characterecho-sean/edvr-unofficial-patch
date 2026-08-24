#include "shader_swap.h"

#include <windows.h>

#include <d3d11.h>

#include "../common/guard.h"
#include "../common/log.h"

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

}  // namespace

ID3D11VertexShader* shaderSwapCompileVs(ID3D11DeviceContext* ctx,
                                        const char* hlsl, size_t hlslLen,
                                        const char* entry, const char* name,
                                        const SwapMacro* macros,
                                        const char* who) {
    if (!ctx || !hlsl || !hlslLen) return nullptr;
    ID3D11VertexShader* out = nullptr;
    guardedBudget(g_budget, [&] {
        compileInner(ctx, hlsl, hlslLen, entry, name, macros, who, &out);
    });
    return out;
}

}  // namespace edvr

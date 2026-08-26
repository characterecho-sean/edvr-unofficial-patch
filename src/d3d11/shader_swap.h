// Compiling a replacement shader at runtime, and the lessons that cost.
//
// The sun-glare fix paved this: when a draw's own vertex shader is what is
// wrong -- geometry emitted flat in screen space, a billboard basis taken
// from the camera -- no constant substitution reaches it, and the only
// honest fix is to hand that one draw a different shader. The machinery is
// small but every part of it is scar tissue:
//
//   * D3DCompile takes ELEVEN parameters. The first field build declared
//     ten, omitting ppErrorMsgs, so the compiler wrote its error-blob
//     pointer through whatever sat in the eleventh slot and the game
//     crashed at the first matched draw -- the project's first crash,
//     bought by an FFI signature nobody proof-read.
//   * The HLSL is desk-compiled before it ever ships (the scratchpad's
//     compile_variants.py). The game is never the compiler's first
//     audience.
//   * Every failure stands the swap down and lets the game draw stock. A
//     missing d3dcompiler_47.dll, a compile error, a failed CreateShader:
//     all of them mean "draw what the game would have drawn".
//   * The whole build runs inside a fault budget, because it runs on the
//     render thread at a matched draw.
#pragma once

#include <cstddef>

struct ID3D11DeviceContext;
struct ID3D11VertexShader;
struct ID3D11PixelShader;

namespace edvr {

// One preprocessor define for a compile: a name and its value, the shape
// D3DCompile's macro array wants. A zeroed entry terminates the list.
struct SwapMacro {
    const char* name;
    const char* value;
};

// Compile HLSL and create a vertex shader from it, or return null having
// said why in the log. `who` names the caller in those messages ("sun
// glare world", "particle billboard") so a log line is attributable
// without a stack trace. `macros` may be null; when given it must end
// with a zeroed entry.
//
// Guarded and budgeted internally: a fault here returns null rather than
// propagating, and the caller's contract is always "null means draw
// stock".
ID3D11VertexShader* shaderSwapCompileVs(ID3D11DeviceContext* ctx,
                                        const char* hlsl, size_t hlslLen,
                                        const char* entry, const char* name,
                                        const SwapMacro* macros,
                                        const char* who);

// The pixel form: ps_5_0, CreatePixelShader, the same contract -- null
// means draw stock, every failure named in the log.
ID3D11PixelShader* shaderSwapCompilePs(ID3D11DeviceContext* ctx,
                                       const char* hlsl, size_t hlslLen,
                                       const char* entry, const char* name,
                                       const SwapMacro* macros,
                                       const char* who);

}  // namespace edvr

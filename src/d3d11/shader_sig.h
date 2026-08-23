// What a vertex shader actually READS from its vertex buffer.
//
// WHY THIS EXISTS
//
// The curved-screen work substitutes a bent strip for the panel composite's
// quad, and at 64 columns it rendered dead flat: the x displacement arrived,
// the z displacement did nothing. Two causes look identical from a headset
// and lead to opposite plans -- the transform drops z, or the shader never
// read z at all -- and no amount of staring at the screen separates them.
//
// The captured vertex data could not either, which is the trap this closes.
// A third float that is 0.0 in all four corners is exactly what an unused
// field looks like, so a float2 position with four bytes of padding fits the
// measurement as well as a float3 does. Stride 20 is silent on the question.
//
// But the shader is not. Every DXBC blob carries an ISGN chunk naming its
// input signature, and each entry carries a ReadWriteMask: which components
// of that input the shader actually consumes. POSITION with used=xy is the
// end of the curvature road; used=xyz means z arrives and the problem is
// elsewhere. That is a fact, readable at shader creation, needing no flight
// and no judgement about whether a screen moved slightly.
//
// So: hook CreateVertexShader, parse the signature once per shader, keep it
// against the shader's pointer, and let whoever recognises a draw ask what
// the shader bound to it reads.
#pragma once

#include <cstddef>

namespace edvr {

// Parse and remember one shader's input signature. Called from the device
// hook at creation, on whatever thread the game streams assets from -- the
// table takes its own lock. Bytecode is not retained.
void shaderSigRegister(void* shader, const void* bytecode, size_t len);

// The remembered signature, in the compact form the log prints:
// "POSITION0 r0 has=xyz used=xy; TEXCOORD0 r0 has=xy used=xy".
// Null when the shader was never seen or its signature would not parse.
const char* shaderSigOf(void* shader);

}  // namespace edvr

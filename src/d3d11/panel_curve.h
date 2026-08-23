// Bending the on-foot screen, by replacing the quad it is drawn on.
//
// WHY THIS WORKS THE WAY IT DOES
//
// A flat quad cannot curve, whatever its transform says. So the panel
// composite has to be re-issued over a finer mesh with the bend baked into
// its local-space positions -- and the whole art of it is changing nothing
// else. The game's vertex shader, pixel shader, input layout, samplers,
// blend and rasterizer state all stay bound and untouched; only the vertex
// buffer, the index buffer and the topology are swapped for one draw, and
// put back immediately after.
//
// That is possible because of what the field sessions measured (all of it in
// docs/screen-curvature.md): the composite draws the CANONICAL UNIT QUAD --
// four vertices of float3 position plus float2 UV at stride 20, corners at
// plus and minus one, z = 0 -- through a 208-byte constant buffer that sizes
// it, places it at its distance and projects it per eye. Bending coordinates
// in that space is bending the screen in its own space, and the game's own
// transform carries it through per eye for free. EDVR never learns what the
// 208 bytes mean.
//
// It composes with the panel distance fix rather than competing with it:
// that fix substitutes the CONSTANT BUFFER for the same draw, this one
// substitutes the GEOMETRY, and the substituted transform serves the
// substituted mesh exactly as it served the flat quad.
//
// THE STAGED PROOF, which is why segments is a setting and not a constant.
// The strip is numbered bottom row first and its triangles are wound with
// the game's own index pattern, so at segments = 1 and curvature = 0 the
// buffers this builds are BYTE-IDENTICAL to the game's quad and index
// buffer. That splits one ambiguous black screen into three failures that
// can be told apart:
//
//   segments = 1,  curvature = 0   the substitution MECHANISM only
//   segments = 64, curvature = 0   the grid GENERATOR
//   curvature > 0                  the bend, and the sign of z
//
// Off by default. At curvature = 0 with the default segment count nothing is
// built, nothing is bound and the composite path does not call in.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// The game's real DrawIndexedInstanced, passed in rather than looked up.
//
// The substituted draw MUST go through the original function pointer. Issuing
// it through the context's vtable would re-enter our own draw thunk, which
// would recognise the composite again and substitute again, without end. The
// pointer lives in vscreen's state; this module is handed it rather than
// keeping a second copy that could drift from it.
// Spelled in plain C++ types rather than UINT/INT so this header needs no
// windows.h. They are the same types -- UINT is unsigned int and INT is int --
// so vscreen's PFN_DrawIndexedInstanced assigns to this without a cast, which
// is the point: a cast here would paper over a signature that had drifted.
typedef void(__stdcall* PanelCurveDrawFn)(ID3D11DeviceContext*, unsigned int,
                                          unsigned int, unsigned int, int,
                                          unsigned int);

// Reads fix.panel_curvature and the two advanced keys. Both config paths,
// live -- the whole staged proof above depends on being able to walk the
// three cases without restarting the game.
void panelCurveConfigure(Config& cfg);

// Is a substitution wanted at all? False when curvature is 0 and the segment
// count is the default, and false for the rest of the session once the fault
// budget has stood the feature down.
bool panelCurveWants();

// Replace one recognised composite draw with the bent strip: save the input
// assembler state actually touched, bind ours, issue the equivalent draw,
// put the saved state back.
//
// Returns whether the game's own draw must now be SWALLOWED. False means
// nothing was substituted and the caller must forward the draw as usual --
// which is the honest answer when the buffers could not be built, and is why
// a failure here is a flat screen rather than a missing one.
bool panelCurveSubstitute(ID3D11DeviceContext* ctx, PanelCurveDrawFn draw);

// Releases the grid buffers. From the vScreen shutdown, which is the only
// place that knows the device is still alive.
void panelCurveShutdown();

}  // namespace edvr

#pragma once
#include <cstdint>

namespace edvr {
// Verified from the original material bytecode. Both sample TEXCOORD8
// through s1, but the unlit variant has one fewer preceding texture.
constexpr uint64_t kHoloLitPs = 0xA2965EC2931A39C8ull;
constexpr uint64_t kHoloUnlitPs = 0xB4786E0A0B199285ull;
constexpr unsigned holoSurfaceSlot(uint64_t ps) {
    return ps == kHoloUnlitPs ? 1u : 2u;
}

// THE SAME PANELS WITH ELITE'S "DISABLE GUI EFFECTS" ON. The cockpit's side and
// centre panels are drawn with this pair instead of vs 81216C77F90DEDD6 and
// the two materials above (docs/ui-layer-2026-09-23.md, "2026-10-01: Disable
// GUI effects"). Disassembled from the dump of 2026-10-01 (D3DDisassemble,
// tools/dxbc_disasm.py):
//   * the vertex shader has the stock one's input layout and the same position
//     arithmetic (clip = cb0[4..7], view = cb0[9..11]), and writes less: its
//     output signature is TEXCOORD0 xyzw, TEXCOORD6 xyz, SV_POSITION. The tangent
//     frame (TEXCOORD4, TEXCOORD7) is gone, and TEXCOORD0.zw carries the surface
//     coordinate the stock shader writes to TEXCOORD8.xy (x16).
//   * the pixel shader is the lit material without its eight-tap smear along the
//     surface and without the second mask sample. It reads TEXCOORD0 and
//     TEXCOORD6 only: no SV_Position, no depth, no Load, no UAV. t0 is the
//     1024x512 four-moment shadow atlas (gathered at the position TEXCOORD6
//     gives), t1 the material mask, t2 the interface surface, sampled through s1
//     at TEXCOORD0.zw -- the same slot as the lit material (holoSurfaceSlot's 2).
// The pair's draw state is the stock pair's (ds=17wZ st=14 bm=7 bl=12,6,1/2,6,1).
// Nothing here is a depth stand-in: ui_depth's kHoloDepthHlsl reads TEXCOORD4,
// TEXCOORD7 and TEXCOORD8, which this vertex shader does not write.
constexpr uint64_t kHoloGuiFxOffVs = 0x1989E6D3B405FDE0ull;
constexpr uint64_t kHoloGuiFxOffPs = 0xEAB8A1C95A13FFBEull;
}

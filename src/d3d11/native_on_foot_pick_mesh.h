// The on-foot panel pick mesh's list and triangle layout in the game's memory (build 334925), pure parts. Header only, no game or D3D11 state. The menu input
// capture and the surface capture read a panel's pick triangles through it; F117 kept it until F155 removed that placement (the triangle search and the turned-mesh
// test went with it, this layout stayed).
#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr {
namespace nofoot {
namespace pickmesh {

// The pick mesh list (panel +0x460): triangles at +0x18, their count at +0x20; a triangle is 0x60 bytes: three vertices (vec4) at +0x00, +0x10, +0x20
// in the pick's camera frame, its plane at +0x30, the three uvs (two floats each) at +0x40, +0x48, +0x50 (0x45412E0 writes them, 0x452EC00 reads them).
constexpr uint32_t kOffList = 0x460, kListTris = 0x18, kListCount = 0x20, kTriStride = 0x60;

struct Tri {
    float v[3][4];
    float plane[4];
    float uv[3][2];
    float pad[2];
};
static_assert(sizeof(Tri) == kTriStride, "a pick mesh triangle is 0x60 bytes");

}  // namespace pickmesh
}  // namespace nofoot
}  // namespace edvr

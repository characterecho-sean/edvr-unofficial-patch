// The vertex shaders (and pairs) the screen and weapon motion fixes name, as pure data and predicates, so the fixes that test a draw
// against them (screen_motion.cpp, weapon_motion.cpp) and the may-want byte stored at the vertex shader's bind (vs_may_want.h) read
// one list. No module state is read here: whether a fix is live, failed or on this frame's source is the caller's own test.
#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr {

struct ScreenMotionPair {
    uint64_t vs, ps;
};

// Flight 14:40:54, final LDR draws 932..938: GUI composites, not the earlier HDR cockpit/world effects or the GUI atlas builders.
constexpr ScreenMotionPair kScreenMotionUiPairs[] = {
    {0xB10B032BDFD46700ull, 0xDB899F4BD577F2E5ull},
    {0xC4B4B334B26E81A9ull, 0x0146ABCC53240479ull},
    {0xA888D51024D9798Eull, 0x015EF9349EC097E8ull},
};
// The 2D screen's composite, the draw that names the world's source (screenMotionRecognize).
constexpr ScreenMotionPair kScreenMotionRecognizePair = {0x5C36AF051B98B9F1ull, 0xCFE84157BC76E921ull};

inline bool screenMotionUiPair(uint64_t vs, uint64_t ps) {
    for (const ScreenMotionPair& p : kScreenMotionUiPairs)
        if (p.vs == vs && p.ps == ps) return true;
    return false;
}
inline bool screenMotionUiPairVs(uint64_t vs) {
    for (const ScreenMotionPair& p : kScreenMotionUiPairs)
        if (p.vs == vs) return true;
    return false;
}
inline bool screenMotionRecognizePair(uint64_t vs, uint64_t ps) {
    return vs == kScreenMotionRecognizePair.vs && ps == kScreenMotionRecognizePair.ps;
}

// The first-person weapon and tool vertex shaders whose animated vertices the weapon motion pass re-reads.
constexpr uint64_t kWeaponMotionFamilyVs[] = {
    0x7B0DC42D383F694Cull, 0x8B589D25B2A0ADDCull, 0x114AF608F86D9ED8ull, 0xAACFDCF2FB9AD809ull, 0x174E8D76363BE337ull,
};
inline bool weaponMotionFamilyHash(uint64_t vs) {
    for (uint64_t v : kWeaponMotionFamilyVs)
        if (v == vs) return true;
    return false;
}

}  // namespace edvr

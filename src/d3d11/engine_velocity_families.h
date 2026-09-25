// Pure shader-family declarations shared by the motion producer and its
// metadata tests. A declared pair is not proof that a runtime patch succeeded.
#pragma once
#include <cstdint>

namespace edvr {
namespace engine_velocity_family {
struct Family {
    uint64_t vs;
    const char* name;
    uint64_t ps[4];
    bool flatOnly = false;
    uint64_t flatPs = 0;
};
constexpr Family kFamilies[] = {
    {0xEB5234DB6ADB491Dull, "vs_EB5234DB6ADB491D", {0xCB9F297EFF264251ull, 0x9ABF60B4B51F2C1Full, 0x3434972DB5336AA4ull, 0}},
    {0x5B4D8E894EEDA8B4ull, "vs_5B4D8E894EEDA8B4", {0x4375B72964F386CDull, 0, 0, 0}},
    {0xBBE58E40FE88EC80ull, "vs_BBE58E40FE88EC80", {0xDB3E8D20CF53FBC0ull, 0, 0, 0}},
    // PS91 needs a separate rasterizer-position input beside SV_IsFrontFace.
    // The captured pair is draw-qualified on WARP; VR remains unqualified.
    {0xDE545DC8EE4FBB87ull, "vs_DE545DC8EE4FBB87", {0xE46E3E4832B2FDB0ull, 0xCB429E043DBB2506ull, 0, 0}, false, 0x91F8937EDA723663ull},
    {0xAACFDCF2FB9AD809ull, "vs_AACFDCF2FB9AD809", {0xCF534B32F491561Aull, 0, 0, 0}},
    {0x66DE2CADB1F4AE6Bull, "vs_66DE2CADB1F4AE6B", {0x864F1F949851B8DEull, 0, 0, 0}},
    {0x61AE8EB05FDC18DDull, "vs_61AE8EB05FDC18DD", {0xFC43E42710010343ull, 0x451A82D4DD1BA254ull, 0, 0}},
    {0x436193B352A2897Eull, "vs_436193B352A2897E", {0x16940F576006BE65ull, 0, 0, 0}},
    {0x889A5279E68F0672ull, "vs_889A5279E68F0672", {0xB46E52A1E0B2F39Cull, 0xEBA95E15B0A66102ull, 0, 0}},
    // Epic flat cockpit shell: the real shader corpus proves the exported
    // t33 slot and byte-identical G-buffer/depth after the MRT6 patch.
    {0xBFE51414CC3024B4ull, "vs_BFE51414CC3024B4", {0xDB79AE788E049DFDull, 0, 0, 0}, true},
};
constexpr int kFamilyCount = static_cast<int>(sizeof(kFamilies) / sizeof(kFamilies[0]));
inline int familyOfVs(uint64_t hash) noexcept {
    for (int i = 0; i < kFamilyCount; ++i) if (kFamilies[i].vs == hash) return i;
    return -1;
}
inline int familyForProfile(uint64_t hash, bool flat) noexcept {
    const int family = familyOfVs(hash);
    return family >= 0 && (!kFamilies[family].flatOnly || flat) ? family : -1;
}
inline bool keyedPs(int family, uint64_t hash, bool flat = false) noexcept {
    if (family < 0 || family >= kFamilyCount || !hash) return false;
    for (uint64_t h : kFamilies[family].ps) if (h == hash) return true;
    return flat && kFamilies[family].flatPs == hash;
}
inline bool supportedPair(uint64_t vs, uint64_t ps) noexcept {
    // Flat recipe/metadata consumers include pairs qualified only for flat.
    return keyedPs(familyOfVs(vs), ps, true);
}
}  // namespace engine_velocity_family
}  // namespace edvr

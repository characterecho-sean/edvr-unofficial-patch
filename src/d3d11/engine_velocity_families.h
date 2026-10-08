// Pure shader-family declarations shared by the motion producer and its
// metadata tests. A declared pair is not proof that a runtime patch succeeded.
#pragma once
#include <cstdint>

namespace edvr {
namespace engine_velocity_family {
struct Family {
    uint64_t vs;
    const char* name;
    uint64_t ps[5];
    bool flatOnly = false;
    uint64_t flatPs = 0;
    bool vrOnly = false;   // keyed in the VR profiles alone (see the 2026-10-08 rows below)
};
constexpr Family kFamilies[] = {
    // 054658: both additional PSs draw certified moving station records;
    // the real corpus preserves their G-buffer/depth and exports exact MRT6.
    {0xEB5234DB6ADB491Dull, "vs_EB5234DB6ADB491D", {0xCB9F297EFF264251ull, 0x9ABF60B4B51F2C1Full, 0x3434972DB5336AA4ull, 0xDC603C35BBE74B31ull, 0x63B1524A9F805A4Cull}},
    {0x5B4D8E894EEDA8B4ull, "vs_5B4D8E894EEDA8B4", {0x4375B72964F386CDull, 0, 0, 0}},
    {0xBBE58E40FE88EC80ull, "vs_BBE58E40FE88EC80", {0xDB3E8D20CF53FBC0ull, 0, 0, 0}},
    // PS91 needs a separate rasterizer-position input beside SV_IsFrontFace.
    // The captured pair is draw-qualified on WARP; VR remains unqualified.
    // 054658: 03B17 draws the station's moving rig; its survivors and discard
    // branch are qualified against the original on WARP.
    {0xDE545DC8EE4FBB87ull, "vs_DE545DC8EE4FBB87", {0xE46E3E4832B2FDB0ull, 0xCB429E043DBB2506ull, 0x03B17F89B31C4788ull, 0}, false, 0x91F8937EDA723663ull},
    {0xAACFDCF2FB9AD809ull, "vs_AACFDCF2FB9AD809", {0xCF534B32F491561Aull, 0, 0, 0}},
    // 2026-09-29, the Krait's main-menu F10 capture: 235567BE is the stock pixel
    // shader of the hull PLATING (5 draws, ~70k vertices). Unkeyed, its draws wrote
    // depth over pool records without writing a slot, so 22.5% of the frame had a
    // stale slot, engineBefore returned "reject", and finish() showed the raw
    // jittered colour there: the panel lines crawled. It is the family's third
    // keyed pixel shader, structurally its sibling 864F1F94 (four targets, no
    // discard, no depth output); the real-corpus harness passes it whole (patched,
    // reflected, created, o0..o3 and depth bit-identical on WARP). Flat-only
    // (flatPs): VR never keyed it and stays as it was. Left for later, not keyed:
    // AACFDCF2/CAD1F585 (EDHM-patched, reads t120) and BBE58E40/7311054A (an
    // SV_Position input); the flat census names them when they draw.
    {0x66DE2CADB1F4AE6Bull, "vs_66DE2CADB1F4AE6B", {0x864F1F949851B8DEull, 0xBBDE4E71FB78528Aull, 0, 0}, false, 0x235567BE2840B3EDull},
    {0x61AE8EB05FDC18DDull, "vs_61AE8EB05FDC18DD", {0xFC43E42710010343ull, 0x451A82D4DD1BA254ull, 0x4504BC268E109C31ull, 0}},
    {0x436193B352A2897Eull, "vs_436193B352A2897E", {0x16940F576006BE65ull, 0x51EE1F922FD220B0ull, 0, 0}},
    {0x889A5279E68F0672ull, "vs_889A5279E68F0672", {0xB46E52A1E0B2F39Cull, 0xEBA95E15B0A66102ull, 0xD31DCAFA7C05CB47ull, 0}},
    // Epic flat cockpit shell: the real shader corpus proves the exported
    // t33 slot and byte-identical G-buffer/depth after the MRT6 patch.
    {0xBFE51414CC3024B4ull, "vs_BFE51414CC3024B4", {0xDB79AE788E049DFDull, 0, 0, 0}, true},
    // 2026-10-08, F10 (6f47ea53, Explorer Cam, a walking settlement NPC; the F10 entries of
    // docs/kinematic-motion-injection-2026-09-19.md). The NPC is a rigid body (17 base-0 records, read by 15 draws of
    // 8B58 and 2 of 7B0D, certified joined, one rigid body to 0.2-0.8 mm) plus a skinned body (20 base != 0 records
    // read by 7B0D, 114A and D99A draws, no previous pose by design). None of these pairs wrote MRT6, so 38.7% of the NPC's pixels
    // (D3) kept a background slot under them (kind 4, stale, refused by the world route) and the rigid body had no
    // motion. Keyed, the rigid records join (kind 1) and a skinned record lands in kind 3 (the camera term stands,
    // no refusal): the emit leaves base != 0 records native, so they carry no marker. Each hash was recomputed
    // (FNV-1a 64, log.cpp fnv1a64) from the dumped bytecode and each pair passed the corpus_identity harness on
    // WARP: patched, created, reflected, o0..o3 and depth bit-identical, MRT6 = 2 slot + 1.
    // VR only: these vertex shaders are also weapon_motion's first-person families (weapon_motion.cpp:92), and
    // flat's foreground and source routing (flat_runtime.cpp, d.supported && weaponMotionFamilyVs) is qualified
    // with them unkeyed. A flat key waits for a flat dump with an NPC.
    {0x8B589D25B2A0ADDCull, "vs_8B589D25B2A0ADDC", {0x7268762D11A610F2ull, 0, 0, 0}, false, 0, true},
    {0x7B0DC42D383F694Cull, "vs_7B0DC42D383F694C", {0x0DF03E64DF9DBEF1ull, 0, 0, 0}, false, 0, true},
    {0x114AF608F86D9ED8ull, "vs_114AF608F86D9ED8", {0xA17504A2627767F2ull, 0, 0, 0}, false, 0, true},
    {0xD99AFDC250D19A3Full, "vs_D99AFDC250D19A3F", {0xE86271E464CCDC1Dull, 0, 0, 0}, false, 0, true},
};
constexpr int kFamilyCount = static_cast<int>(sizeof(kFamilies) / sizeof(kFamilies[0]));
// Historical marker-bearing pairs retained for compatibility. PROVENANCE
// CORRECTION, 2026-09-28: every PS below is EDVR's generated substitution,
// not a native game shader. Re-running engineVelocityPatchPs on the captured
// originals reproduces these exact creation-byte hashes:
//   51EE1F922FD220B0 -> BCF75CEA37060EAE
//   D31DCAFA7C05CB47 -> 2F924695596C8195
//   DB3E8D20CF53FBC0 -> 25CC28229319DFA8
//   CF534B32F491561A -> C4835018A5128866
//   FC43E42710010343 -> C21E17F391CC04AF
// The device's creation hook dumps/registers EDVR shaders too. A census
// reading these hashes live while the PS hook sees zero binds is expected
// substitution, not evidence of a bypass. Do not key them as game variants.
// See docs/kinematic-motion-injection-2026-09-19.md, 2026-09-28 investigation.
struct SelfMarking {
    uint64_t vs, ps;
};
constexpr SelfMarking kSelfMarking[] = {
    {0x436193B352A2897Eull, 0xBCF75CEA37060EAEull},
    {0x889A5279E68F0672ull, 0x2F924695596C8195ull},
    // The 15:46 session's marker-bearing draws: these are also EDVR's
    // generated shaders, as the exact lineage above proves.
    {0xBBE58E40FE88EC80ull, 0x25CC28229319DFA8ull},
    {0xAACFDCF2FB9AD809ull, 0xC4835018A5128866ull},
    {0x61AE8EB05FDC18DDull, 0xC21E17F391CC04AFull},
};
constexpr bool selfMarkingPair(uint64_t vs, uint64_t ps) noexcept {
    for (const SelfMarking& p : kSelfMarking)
        if (p.vs == vs && p.ps == ps) return true;
    return false;
}
// Any keyed or self-marking pixel shader hash: the draw path's probe gate.
constexpr bool anyFamilyPs(uint64_t ps) noexcept {
    for (const Family& f : kFamilies)
        for (uint64_t h : f.ps)
            if (h == ps) return true;
    for (const SelfMarking& p : kSelfMarking)
        if (p.ps == ps) return true;
    return false;
}
// The PS half alone: the shader hook's census of their binds (the draw path
// never saw 095337's seam draws; the bind count separates "never bound through
// the hook" from "bound but never drawn through it" in one flight).
constexpr bool selfMarkingPs(uint64_t ps) noexcept {
    for (const SelfMarking& p : kSelfMarking)
        if (p.ps == ps) return true;
    return false;
}
inline int familyOfVs(uint64_t hash) noexcept {
    for (int i = 0; i < kFamilyCount; ++i) if (kFamilies[i].vs == hash) return i;
    return -1;
}
inline int familyForProfile(uint64_t hash, bool flat) noexcept {
    const int family = familyOfVs(hash);
    return family >= 0 && (!kFamilies[family].flatOnly || flat) && (!kFamilies[family].vrOnly || !flat) ? family : -1;
}
inline bool keyedPs(int family, uint64_t hash, bool flat = false) noexcept {
    if (family < 0 || family >= kFamilyCount || !hash) return false;
    if (flat && kFamilies[family].vrOnly) return false;
    for (uint64_t h : kFamilies[family].ps) if (h == hash) return true;
    return flat && kFamilies[family].flatPs == hash;
}
inline bool supportedPair(uint64_t vs, uint64_t ps) noexcept {
    // Flat recipe/metadata consumers include pairs qualified only for flat.
    return keyedPs(familyOfVs(vs), ps, true);
}
}  // namespace engine_velocity_family
}  // namespace edvr

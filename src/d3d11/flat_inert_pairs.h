#pragma once
#include <cstdint>

namespace edvr {
// Verified inert pairs from the Epic f1ea02fe outcome census. This is an
// explicit classification, not a projection patch or a jitter admission.
inline bool flatProjectionDrawUnchanged(uint64_t vs, uint64_t ps) {
    return (vs == 0xFC1193AFFC596F74ull && ps == 0x258B95AC99520C1Full) ||
           (vs == 0xE8FDC0D92EEBA6D7ull && ps == 0x258B95AC99520C1Full) ||
           (vs == 0x53211E8C072CD02Eull && ps == 0xB403F48CB35D9739ull) ||
           // Epic 6e9bde74: VS passes v0 straight to SV_Position; PS has
           // only CB2[7] UV/filter constants and t0/t1/t2 samples.
           (vs == 0xCFA91824129ECBBCull && ps == 0xFCFAD73924BF45B9ull) ||
           // Epic 85bf7652: VS maps packed UV directly to clip space without
           // a CB; PS filters texture neighbors with no projection consumer.
           (vs == 0x525D47E3D5E2EFF4ull && ps == 0xF0BAE053476F8730ull) ||
           // Epic 20260925_173622, EDHM chain: the same inert packed-UV clip
           // VS; the mod-patched PS variant filters screen colour and samples
           // a vPos-scaled mask. No projection consumer on either side.
           (vs == 0x525D47E3D5E2EFF4ull && ps == 0x0D617929FED842F0ull) ||
           // Epic ad7607c6: fullscreen color/effect composition. VS derives
           // clip coordinates from vertex ID; PS samples/distorts screen
           // color without scene-depth reconstruction or a camera matrix.
           (vs == 0xB553BB479B7C0B97ull && ps == 0x68ABCB9FEF6CA66Cull);
}
// Pairs whose shaders never read VS b1, so the camera a draw happens to have bound there says
// nothing about how it was drawn: the inert pairs above, and the sun-shadow resolve, whose VS
// declares no constant buffer and whose PS reads only its own cb2 (that camera has its own pixel
// recipe). The source-free camera witness neither opens nor conflicts an entry for them: on the
// loadout screen (flat_trace_20491) this pair runs on the scene depth with an auxiliary preview
// scene's camera still bound in b1. The star's depth-only pre-pass is the third: its VS declares one
// constant buffer, cb0[8] (the saved 420-byte bytecode), so it never reads b1; its PS is null (a null
// pixel shader hashes to 0) and its colour write mask is 0. It is the first Screen-kind writer in the
// Detailed Surface Scanner and the Full System Scanner, ahead of the terrain, and as the first writer it
// opened the witness with no camera and the next star draw raised camera-not-current, so the deferred
// lighting pass was refused on every jittered frame (edvr_gfx_20261022_101031, census 7). That does not
// make the pre-pass projection-inert: its depth uses the jittered b0 matrices, so it stays out of
// flatProjectionDrawUnchanged.
inline bool flatProjectionB1Unread(uint64_t vs, uint64_t ps) {
    return flatProjectionDrawUnchanged(vs, ps) ||
           (vs == 0x53211E8C072CD02Eull && ps == 0x7EAC71963E66C5FEull) ||
           (vs == 0x0EE43D81E394E70Cull && ps == 0);
}
}

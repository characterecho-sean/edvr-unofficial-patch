// Compile the production layer whole, like the existing world/device rigs. These
// accessors expose only frame facts and the live tone plan to the WARP fixture.
#include "../../src/d3d11/ui_layer.cpp"

namespace edvr::flatweapontest {
bool weaponCaptured(uint64_t frame) {
    const Eye& e = g_eye[0];
    return e.wSeq == frame && e.wDraws == 1 && e.wSrv && e.wTarget && e.hdrDraws == 0;
}
bool published(uint64_t frame) {
    const Eye& e = g_eye[0];
    return e.seq == frame && e.draws && e.wPubSeq == frame && e.hdrToneSeq == frame;
}
bool matchedPlan() { return g_crispSave.active && g_crispSave.plan.sourcePass && g_crispSave.plan.count == 2; }
bool plainPlan() { return g_crispSave.active && !g_crispSave.plan.sourcePass && g_crispSave.plan.count == 1; }
ID3D11Texture2D* toneLayer() { return g_eye[0].tex.Get(); }
ID3D11Texture2D* weaponLayer() { return g_eye[0].wTex.Get(); }
}  // namespace edvr::flatweapontest

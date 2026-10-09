// The production layer module, compiled whole in this translation unit, with a few accessors to its device-owned caches
// for ui_layer_device_test.cpp (the review of 2026-10-09's P2: device-owned caches surviving a device change). Nothing here
// changes what the module does; it only lets the rig see which device each retained object belongs to.
#include "../../src/d3d11/ui_layer.cpp"

namespace edvr {
namespace devtest {

ID3D11BlendState* rgbOnlyBlend(ID3D11DeviceContext* ctx) {
    UiBlendRt rgbOnly;
    rgbOnly.enable = false;
    rgbOnly.mask = uiblend::kWriteRgb;
    return cachedBlend(ctx, rgbOnly);
}
// The crisp take's whole dependency set (the layers at the door's size, the RGB blend, the coverage shaders, its
// deferred context), as the first HUD take of a frame establishes it.
bool takeReady(ID3D11DeviceContext* ctx) { return crispTakeReady(ctx, 0); }
bool seederReady(ID3D11Device* dev) { return ensureSeeder(dev); }
ID3D11DeviceContext* coverageContext() { return g_crispDeferred.Get(); }
ID3D11DeviceContext* seederContext() { return g_deferred.Get(); }
ID3D11VertexShader* coverageVs() { return g_covVs; }
ID3D11PixelShader* coveragePs() { return g_covPs; }
ID3D11ComputeShader* compositeCs() { return g_cs; }
ID3D11Buffer* compositeCb() { return g_cb; }
ID3D11Texture2D* layerTexture() { return g_eye[0].tex.Get(); }
ID3D11Texture2D* hdrLayerTexture() { return g_eye[0].hdrTex.Get(); }
uint32_t layerWidth() { return g_eye[0].w; }

// This frame's HUD, as the take and the tonemap re-issue would leave it: the 8-bit layer's colour, the HDR layer's
// alpha (transmittance), then the production coverage pass (the HDR alpha into the 8-bit alpha on the deferred context).
bool hudFrame(ID3D11DeviceContext* ctx, uint64_t seq, const float ldr[4], const float hdr[4]) {
    Eye& e = g_eye[0];
    if (!e.rtv || !e.hdrRtv) return false;
    ctx->ClearRenderTargetView(e.rtv.Get(), ldr);
    ctx->ClearRenderTargetView(e.hdrRtv.Get(), hdr);
    e.hdrSeq = seq;
    e.hdrDraws = 1;
    if (!crispCoveragePass(ctx, e, 0, seq)) return false;
    e.seq = seq;
    e.draws = 1;
    e.hdrToneSeq = seq;
    return true;
}

}  // namespace devtest
}  // namespace edvr

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
bool sharpReady(ID3D11DeviceContext* ctx) {
    compileSharpSourceOnce(ctx);
    return g_srcVs && g_srcPs && weaponsTakeReady(ctx, 0) &&
           ensureSharpScratch(ctx, g_eye[0].wW, g_eye[0].wH);
}
bool rippleReady(ID3D11DeviceContext* ctx) {
    return rippleTakeReady(ctx, 0, g_eye[0].door.fullW, g_eye[0].door.fullH) &&
           g_rip.cs && g_rip.blend && g_rip.lin && g_rip.cb && g_rip.rampSrv;
}
bool rippleFirstDrawPending(ID3D11DeviceContext* ctx) {
    if (!ctx || g_rip.check != 0) return false;
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    float rows[91 * 4] = {};
    rows[90 * 4 + 1] = rows[90 * 4 + 2] = 1.0f;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(rows);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA data{rows, 0, 0};
    Ptr<ID3D11Buffer> cb;
    if (FAILED(dev->CreateBuffer(&bd, &data, &cb))) return false;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    Ptr<ID3D11SamplerState> sampler;
    if (FAILED(dev->CreateSamplerState(&sd, &sampler))) return false;
    ID3D11Buffer* cbs[] = {cb.Get()};
    ID3D11SamplerState* samplers[] = {sampler.Get()};
    ctx->PSSetConstantBuffers(1, 1, cbs);
    ctx->PSSetSamplers(0, 1, samplers);
    const bool taken = rippleR1Allows(ctx);
    ID3D11Buffer* noCb[] = {nullptr};
    ID3D11SamplerState* noSampler[] = {nullptr};
    ctx->PSSetConstantBuffers(1, 1, noCb);
    ctx->PSSetSamplers(0, 1, noSampler);
    return !taken && g_rip.check == 1 && g_rip.stage;
}
bool newCachesCleared() {
    return !g_srcVs && !g_srcPs && !g_srcTried && !g_sharpStates.ready() && !g_sharpStates.sourceBlend &&
           !g_sharpStates.toneReplace && !g_sharpStates.toneAdd && !g_sharpStates.toneSubtract &&
           !g_rip.rampTex && !g_rip.rampSrv && !g_rip.rampW && !g_rip.rampH && !g_rip.blend && !g_rip.lin &&
           !g_rip.cb && !g_rip.cs && !g_rip.csTried && !g_rip.stage && !g_rip.check && !g_rip.filter &&
           !g_rip.gainZ && !g_rip.gainY;
}
ID3D11VertexShader* sharpVs() { return g_srcVs; }
ID3D11PixelShader* sharpPs() { return g_srcPs; }
ID3D11BlendState* sharpSourceBlend() { return g_sharpStates.sourceBlend.Get(); }
ID3D11BlendState* sharpReplaceBlend() { return g_sharpStates.toneReplace.Get(); }
ID3D11BlendState* sharpAddBlend() { return g_sharpStates.toneAdd.Get(); }
ID3D11BlendState* sharpSubtractBlend() { return g_sharpStates.toneSubtract.Get(); }
ID3D11Texture2D* sharpScratch() { return g_sharpScratch.tex.Get(); }
ID3D11RenderTargetView* sharpScratchTarget() { return g_sharpScratch.rtv.Get(); }
ID3D11ShaderResourceView* sharpScratchView() { return g_sharpScratch.srv.Get(); }
ID3D11Texture2D* weaponsLayer() { return g_eye[0].wTex.Get(); }
ID3D11RenderTargetView* weaponsTarget() { return g_eye[0].wRtv.Get(); }
ID3D11ShaderResourceView* weaponsView() { return g_eye[0].wSrv.Get(); }
ID3D11Texture2D* rippleLayer() { return g_eye[0].dTex.Get(); }
ID3D11RenderTargetView* rippleTarget() { return g_eye[0].dRtv.Get(); }
ID3D11ShaderResourceView* rippleView() { return g_eye[0].dSrv.Get(); }
ID3D11Texture2D* rippleRamp() { return g_rip.rampTex.Get(); }
ID3D11ShaderResourceView* rippleRampView() { return g_rip.rampSrv.Get(); }
ID3D11BlendState* rippleBlend() { return g_rip.blend.Get(); }
ID3D11SamplerState* rippleSampler() { return g_rip.lin.Get(); }
ID3D11Buffer* rippleCb() { return g_rip.cb.Get(); }
ID3D11ComputeShader* rippleCs() { return g_rip.cs; }
ID3D11Buffer* rippleStage() { return g_rip.stage.Get(); }
ID3D11DeviceContext* coverageContext() { return g_crispDeferred.Get(); }
ID3D11DeviceContext* seederContext() { return g_deferred.Get(); }
ID3D11VertexShader* coverageVs() { return g_covVs; }
ID3D11PixelShader* coveragePs() { return g_covPs; }
ID3D11ComputeShader* compositeCs() { return g_cs; }
ID3D11ComputeShader* compositeInPlaceCs() { return g_csInPlace; }
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

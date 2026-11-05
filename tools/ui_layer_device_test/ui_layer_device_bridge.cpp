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
// The Galaxy Map's cached states (the depth-stencil states, the cloned rasterizer, the two blend states) as the first map issue makes them.
bool mapStatesReady(ID3D11DeviceContext* ctx) {
    return mapDepthState(ctx, false) && mapDepthState(ctx, true) && mapDepthRaster(ctx, nullptr) && mapBlend(ctx, 2) && mapBlend(ctx, 3);
}
bool mapStatesOwnedBy(ID3D11Device* dev) {
    const auto own = [&](ID3D11DeviceChild* c) {
        if (!c) return false;
        ID3D11Device* d = nullptr;
        c->GetDevice(&d);
        const bool same = d == dev;
        if (d) d->Release();
        return same;
    };
    return own(g_mapDss[0].Get()) && own(g_mapDss[1].Get()) && own(g_mapRsBias.Get()) && own(g_mapBlend[0].Get()) && own(g_mapBlend[1].Get());
}
bool mapStatesCleared() {
    return !g_mapDss[0] && !g_mapDss[1] && !g_mapDssTried[0] && !g_mapDssTried[1] && !g_mapRsBias && !g_mapRsGame && !g_mapBlend[0] && !g_mapBlend[1] &&
           !g_mapTone.copy && !g_mapTone.patch && !g_mapTone.width;
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

bool failedCoverageRetiresWithoutPublishing() {
    Eye& e = g_eye[0];
    const uint64_t oldSeq = e.hdrSeq, oldToneSeq = e.hdrToneSeq;
    const uint32_t oldDraws = e.hdrDraws, oldMiss = e.hdrMissStreak;
    const uint64_t oldLost = g_win.hdrLost;
    e.hdrSeq = 10187;
    e.hdrToneSeq = 10186;
    e.hdrDraws = 13;
    e.hdrMissStreak = 1;
    retireFailedCrispCoverage(e, 10188);  // another frame must not retire this one
    const bool wrongFrameUntouched = e.hdrDraws == 13 && e.hdrToneSeq == 10186 &&
        e.hdrMissStreak == 1 && g_win.hdrLost == oldLost;
    retireFailedCrispCoverage(e, 10187);
    const bool retired = e.hdrDraws == 0 && e.hdrToneSeq == 10186 &&
        e.hdrMissStreak == 0 && g_win.hdrLost == oldLost + 1;
    retireFailedCrispCoverage(e, 10187);  // repeated cleanup must not count twice
    const bool once = g_win.hdrLost == oldLost + 1;
    e.hdrSeq = oldSeq;
    e.hdrToneSeq = oldToneSeq;
    e.hdrDraws = oldDraws;
    e.hdrMissStreak = oldMiss;
    g_win.hdrLost = oldLost;
    return wrongFrameUntouched && retired && once;
}

bool missingConsumerLeavesCurrentDrawStock(ID3D11DeviceContext* ctx) {
    if (!ctx) return false;
    Eye& e = g_eye[0];
    ID3D11BlendState* blend = rgbOnlyBlend(ctx);
    if (!blend || !e.rtv || !e.hdrRtv || g_crispStoodDown) return false;
    UiBlendRt game{}, converted{};
    const UiBlendShape shape = shapeOf(blend, &game);
    if (!uiLayerConvertBlend(game, &converted)) return false;
    ID3D11RenderTargetView* stock = e.rtv.Get();
    ctx->OMSetRenderTargets(1, &stock, nullptr);
    ctx->OMSetBlendState(blend, nullptr, 0xffffffffu);
    const ULONG before = blend->AddRef(); blend->Release();

    g_draw = Draw{};
    g_draw.decided = true;
    g_draw.eye = 0;
    g_draw.hdr = true;
    g_draw.seq = 41;
    g_draw.shape = shape;
    e.hdrSeq = 40;
    e.hdrToneSeq = 39;
    e.hdrDraws = 1;
    const uint32_t missLimit = runtimeFlatProfile() ? 1u : kCrispHdrMissFrames;
    e.hdrMissStreak = missLimit - 1;
    const uint64_t lostBefore = g_win.hdrLost;
    const uint64_t refusedBefore = g_win.refusedAtIssue;
    const bool taken = beginInner(ctx, 0);

    ID3D11RenderTargetView* bound = nullptr;
    ctx->OMGetRenderTargets(1, &bound, nullptr);
    const bool stockStillBound = bound == stock;
    if (bound) bound->Release();
    ID3D11BlendState* boundBlend = nullptr;
    FLOAT factor[4]{}; UINT sampleMask = 0;
    ctx->OMGetBlendState(&boundBlend, factor, &sampleMask);
    const bool blendStillBound = boundBlend == blend;
    if (boundBlend) boundBlend->Release();
    const ULONG after = blend->AddRef(); blend->Release();
    const bool ok = !taken && stockStillBound && blendStillBound && before == after &&
        !g_draw.active && !g_draw.saved && !g_draw.blend &&
        g_crispStoodDown && e.hdrSeq == 40 && e.hdrDraws == 0 &&
        e.hdrMissStreak == missLimit && g_win.hdrLost == lostBefore + 1 &&
        g_win.refusedAtIssue == refusedBefore + 1;
    ctx->ClearState();
    g_draw = Draw{};
    return ok;
}

// The eye-draw note's early exit (item 3 of the VR draw-path test build): the out-of-line body's first act is to clear the
// pending tonemap record, so a record that survives a call is proof the body was not entered. Arm the record, with or
// without the header flag, then ask whether it is still pending.
void crispProbeArm(bool headerFlag) {
    g_crispPending = CrispTonePending{};
    g_crispPending.pending = true;
    detail::g_uiLayerCrispPending = headerFlag;
}
bool crispProbeRecordPending() { return g_crispPending.pending; }

// ---- The back-off of the HDR HUD path and the weapons layer (2026-11-03). Each returns null when every step held, else the step that did not.
// They read the production state directly and move the production clock (g_backOffSkewMs) instead of waiting.

void backOffReset(RuntimeProfile profile) {
    runtimeProfileSet(profile);
    g_backOffSkewMs = 0;
    g_flatBackOffStep = 0;
    g_flatBackOffLastMs = 0;
    g_flatBackOffs = 0;
    g_flatBackOffUntilMs = g_crispBackOffUntilMs = g_weaponsBackOffUntilMs = 0;
    g_crispStoodDown = g_weaponsStoodDown = g_stoodDown = false;
    for (Eye& e : g_eye) { e.hdrMissStreak = 0; e.wMissStreak = 0; }
    Config& cfg = Config::get();
    cfg.set("fix.temporal_aa", "dlss");
    cfg.set("fix.ui_quality", "100");
    uiLayerConfigure(cfg);
}

// Milliseconds left on a timer (0 when none is set or it has passed).
uint64_t backOffLeft(uint64_t until) {
    const uint64_t now = backOffNowMs();
    return until > now ? until - now : 0;
}

// The 30-frame miss streak of the HUD layer in VR stands the HDR HUD path down for 2 s, and the frame boundary re-arms it once those 2 s have passed (not before); the
// second failure in a row backs off for 4 s and the third for 8. The first failure goes through the production draw path (missingConsumerLeavesCurrentDrawStock).
const char* vrHudPathBacksOffAndRearms(ID3D11DeviceContext* ctx) {
    backOffReset(RuntimeProfile::Vr);
    const uint64_t expectMs[3] = {2000, 4000, 8000};
    for (int round = 0; round < 3; ++round) {
        if (!missingConsumerLeavesCurrentDrawStock(ctx)) return "the 30th consecutive HUD miss did not stand the path down at the draw";
        if (!g_crispStoodDown || uiLayerCrispOn()) return "stood down, but the path still reads as live";
        const uint64_t left = backOffLeft(g_crispBackOffUntilMs);
        if (left > expectMs[round] || left + 500 < expectMs[round]) return "the back-off is not the ladder's length for this failure (2 s, then 4, then 8)";
        if (g_flatBackOffUntilMs) return "VR set the flat layer's timer";
        g_backOffSkewMs += expectMs[round] - 300;
        uiLayerFrameBoundary(ctx);
        if (!g_crispStoodDown || uiLayerCrispOn()) return "re-armed before its back-off passed";
        g_backOffSkewMs += 600;
        uiLayerFrameBoundary(ctx);
        if (g_crispStoodDown || !uiLayerCrispOn() || g_crispBackOffUntilMs) return "not re-armed at the first frame boundary after its back-off";
        if (g_eye[0].hdrMissStreak != 0 || g_eye[0].hdrDraws != 0) return "re-armed with the old streak or content";
        uiLayerFrameBoundary(ctx);   // a boundary with nothing pending changes nothing
        if (g_crispStoodDown) return "a later boundary stood the path down again";
    }
    return nullptr;
}

// The ladder starts over once the layer has run five clean minutes since its last back-off.
const char* backOffLadderRestartsAfterCleanMinutes() {
    backOffReset(RuntimeProfile::Vr);
    crispStandDown("first");
    if (backOffLeft(g_crispBackOffUntilMs) > 2000) return "the first back-off is not 2 s";
    g_backOffSkewMs += 2100;
    g_crispStoodDown = false;
    crispStandDown("second");
    if (backOffLeft(g_crispBackOffUntilMs) <= 2000) return "the second back-off did not grow";
    g_backOffSkewMs += kFlatBackOffCleanMs + 1000;
    g_crispStoodDown = false;
    crispStandDown("after five clean minutes");
    return backOffLeft(g_crispBackOffUntilMs) <= 2000 ? nullptr : "the ladder did not start over after five clean minutes";
}

// The sharp weapons layer backs off the same way in both profiles: down for 2 s, re-armed at the frame boundary after, 4 s the second time.
const char* weaponsBackOffAndRearm(ID3D11DeviceContext* ctx, RuntimeProfile profile) {
    backOffReset(profile);
    const uint64_t expectMs[2] = {2000, 4000};
    for (int round = 0; round < 2; ++round) {
        if (!uiLayerWeaponsLive()) return "the weapons half is not live before the failure";
        weaponsStandDown("a test failure");
        if (!g_weaponsStoodDown || uiLayerWeaponsLive()) return "stood down, but the weapons half still reads as live";
        const uint64_t left = backOffLeft(g_weaponsBackOffUntilMs);
        if (left > expectMs[round] || left + 500 < expectMs[round]) return "the weapons back-off is not the ladder's length for this failure";
        g_backOffSkewMs += expectMs[round] - 300;
        uiLayerFrameBoundary(ctx);
        if (!g_weaponsStoodDown) return "re-armed before its back-off passed";
        g_backOffSkewMs += 600;
        uiLayerFrameBoundary(ctx);
        if (g_weaponsStoodDown || !uiLayerWeaponsLive() || g_weaponsBackOffUntilMs) return "not re-armed at the first frame boundary after its back-off";
    }
    return nullptr;
}

const char* weaponsBackOffAndRearmVr(ID3D11DeviceContext* ctx) { return weaponsBackOffAndRearm(ctx, RuntimeProfile::Vr); }
const char* weaponsBackOffAndRearmFlat(ID3D11DeviceContext* ctx) { return weaponsBackOffAndRearm(ctx, RuntimeProfile::Flat); }

// The flat profile's HDR HUD path keeps its own timer and its own re-arm, which also lifts the layer's stand-down; VR's timer is not used.
const char* flatHudPathBackOffUnchanged(ID3D11DeviceContext* ctx) {
    backOffReset(RuntimeProfile::Flat);
    crispStandDown("a flat test failure");
    if (!g_crispStoodDown || g_crispBackOffUntilMs) return "the flat profile used VR's HDR HUD timer";
    const uint64_t left = backOffLeft(g_flatBackOffUntilMs);
    if (left > 2000 || left + 500 < 2000) return "the flat back-off is not 2 s";
    g_backOffSkewMs += 2600;
    uiLayerFrameBoundary(ctx);
    if (g_flatBackOffUntilMs) return "the flat profile's timer was not cleared by the frame boundary after its back-off";
    return !g_crispStoodDown ? nullptr : "the flat profile cleared its timer but the HDR HUD path is still stood down";
}

// A live change of fix.ui_quality clears a stood-down weapons layer and HDR HUD path at once, and their back-offs.
const char* toggleClearsBothStandDowns() {
    backOffReset(RuntimeProfile::Vr);
    weaponsStandDown("a test failure");
    crispStandDown("a test failure");
    if (!g_weaponsStoodDown || !g_crispStoodDown) return "(setup) both did not stand down";
    Config& cfg = Config::get();
    cfg.set("fix.ui_quality", "off");
    uiLayerConfigure(cfg);
    cfg.set("fix.ui_quality", "100");
    uiLayerConfigure(cfg);
    if (g_weaponsStoodDown || !uiLayerWeaponsLive()) return "the toggle did not clear the weapons stand-down";
    if (g_crispStoodDown || !uiLayerCrispOn()) return "the toggle did not clear the HDR HUD path's stand-down";
    if (g_weaponsBackOffUntilMs || g_crispBackOffUntilMs) return "the toggle left a back-off timer running";
    return nullptr;
}

// The instrument's budget: a lost HUD layer prints its lines for the episode and no more, and the next episode (a re-arm) gets a fresh 24.
const char* hudLostLinesAreBoundedPerEpisode(ID3D11DeviceContext* ctx) {
    backOffReset(RuntimeProfile::Vr);
    g_hudLostLines = 0;
    g_hudLostEpisodes = 1;
    const Eye& e = g_eye[0];
    for (uint32_t i = 0; i < 40; ++i) noteHudLost(0, e, 100 + i);
    if (g_hudLostLines != kHudLostLines) return "the episode's line budget is not 24";
    g_eye[0].hdrMissStreak = 0;
    crispStandDown("a test failure");
    g_backOffSkewMs += 2600;
    uiLayerFrameBoundary(ctx);
    if (g_hudLostLines != 0 || g_hudLostEpisodes != 2) return "the re-arm did not start a new episode";
    return nullptr;
}

// The lost-HUD instrument names each texture the unmatched tonemap-shaped draw read, with its description; with no lines left in the episode, or under the flat
// profile, it reads nothing (so the sighting stays the pointer alone).
const char* toneSlotsDescribeTheReadTextures(ID3D11DeviceContext* ctx) {
    backOffReset(RuntimeProfile::Vr);
    g_hudLostLines = 0;
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev) return "(setup) no device";
    const auto make = [&](UINT w, UINT h, DXGI_FORMAT f, UINT bind, Ptr<ID3D11Texture2D>* tex, Ptr<ID3D11ShaderResourceView>* srv) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = d.ArraySize = 1; d.Format = f; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
        return SUCCEEDED(dev->CreateTexture2D(&d, nullptr, tex->ReleaseAndGetAddressOf())) && SUCCEEDED(dev->CreateShaderResourceView(tex->Get(), nullptr, srv->ReleaseAndGetAddressOf()));
    };
    Ptr<ID3D11Texture2D> a, b;
    Ptr<ID3D11ShaderResourceView> sa, sb;
    if (!make(96, 54, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, &a, &sa) ||
        !make(64, 32, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, &b, &sb)) return "(setup) the textures could not be made";
    ID3D11ShaderResourceView* views[2] = {sa.Get(), sb.Get()};
    ctx->PSSetShaderResources(0, 2, views);

    noteToneSight(7, a.Get(), -1, 1);
    noteToneSightSlots(ctx);
    ToneSight t = g_toneSights[(g_toneSightCount - 1) % 6];
    const char* bad = nullptr;
    if (t.nSlots != 2) bad = "the sighting did not take both bound textures";
    else if (t.slots[0].res != a.Get() || t.slots[0].slot != 0 || t.slots[0].w != 96 || t.slots[0].h != 54 ||
             t.slots[0].fmt != static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT) ||
             t.slots[0].bind != (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET)) bad = "slot 0's description is wrong";
    else if (t.slots[1].res != b.Get() || t.slots[1].slot != 1 || t.slots[1].w != 64 || t.slots[1].h != 32 ||
             t.slots[1].fmt != static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM) || t.slots[1].bind != D3D11_BIND_SHADER_RESOURCE) bad = "slot 1's description is wrong";
    if (!bad) {
        char text[760];
        formatToneSights(text, sizeof(text));
        if (!std::strstr(text, "96x54") || !std::strstr(text, "64x32") || !std::strstr(text, "t1 ")) bad = "the sightings line does not carry the descriptions";
    }
    if (!bad) {
        g_hudLostLines = kHudLostLines;   // no lines left in this episode: the next sighting reads nothing
        noteToneSight(8, a.Get(), -1, 1);
        noteToneSightSlots(ctx);
        if (g_toneSights[(g_toneSightCount - 1) % 6].nSlots != 0) bad = "a sighting with no lines left still read its textures";
        g_hudLostLines = 0;
    }
    if (!bad) {
        runtimeProfileSet(RuntimeProfile::Flat);
        noteToneSight(9, a.Get(), -1, 1);
        noteToneSightSlots(ctx);
        if (g_toneSights[(g_toneSightCount - 1) % 6].nSlots != 0) bad = "the flat profile read the sighting's textures";
        runtimeProfileSet(RuntimeProfile::Vr);
    }
    ctx->ClearState();
    return bad;
}

}  // namespace devtest
}  // namespace edvr

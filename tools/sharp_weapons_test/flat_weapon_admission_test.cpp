// Production flat adapter + complete shared UI layer on WARP. A first tone
// proves the HDR target; the next frame captures a weapon draw, reissues the
// tone, and publishes its light through the flat output-copy composite.
#define main ui_layer_world_test_main
#define flatUiTrailNoteReissue worldStubFlatUiTrailNoteReissue
#define flatUiTrailReport worldStubFlatUiTrailReport
#include "../ui_layer_world_test/ui_layer_world_test.cpp"
#undef flatUiTrailReport
#undef flatUiTrailNoteReissue
#undef main

#include "../../src/d3d11/flat_ui_layer.h"

namespace edvr {
// The named-pair route below does not use shader-registry or physical-shape
// admission. The adapter's other routes still link in this whole-module rig.
uint64_t lookupShaderHash(void*) { return 0; }
namespace flatweapontest {
bool weaponCaptured(uint64_t frame);
bool published(uint64_t frame);
bool matchedPlan();
bool plainPlan();
ID3D11Texture2D* toneLayer();
ID3D11Texture2D* weaponLayer();
}  // namespace flatweapontest
}  // namespace edvr

namespace {

Tex hdrTarget(Rig& r) {
    Tex t;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = t.w = kEyeW;
    d.Height = t.h = kEyeH;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(r.dev->CreateTexture2D(&d, nullptr, &t.tex))) return {};
    if (FAILED(r.dev->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv))) return {};
    if (FAILED(r.dev->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv))) return {};
    return t;
}

uint16_t halfRedAt(Rig& r, ID3D11Texture2D* tex, uint32_t x, uint32_t y) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(r.dev->CreateTexture2D(&d, nullptr, &staging))) return 0;
    r.ctx->CopyResource(staging.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(r.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return 0;
    const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(m.pData) + y * m.RowPitch);
    const uint16_t red = row[x * 4];
    r.ctx->Unmap(staging.Get(), 0);
    return red;
}

// The game's weapon draw uses the world rig's simple red shader. Its pair is
// classified by the flat adapter; the production Begin redirects its pixels.
void bindWeapon(Rig& r, Tex& hdr) {
    ID3D11RenderTargetView* target = hdr.rtv.Get();
    r.ctx->OMSetRenderTargets(1, &target, nullptr);
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(kEyeW), static_cast<float>(kEyeH), 0, 1};
    r.ctx->RSSetViewports(1, &vp);
    r.ctx->RSSetState(r.raster.Get());
    const D3D11_RECT sc{0, 0, static_cast<LONG>(kEyeW), static_cast<LONG>(kEyeH)};
    r.ctx->RSSetScissorRects(1, &sc);
    r.ctx->OMSetBlendState(r.blend.Get(), nullptr, 0xFFFFFFFFu);
    r.ctx->OMSetDepthStencilState(r.dsOff.Get(), 0);
    ID3D11ShaderResourceView* source = r.screen.srv.Get();
    r.ctx->PSSetShaderResources(0, 1, &source);
    ID3D11SamplerState* sampler = r.gameSampler.Get();
    r.ctx->PSSetSamplers(0, 1, &sampler);
    r.ctx->VSSetShader(r.vs.Get(), nullptr, 0);
    r.ctx->PSSetShader(r.ps.Get(), nullptr, 0);
    ID3D11Buffer* cb = r.rectCb.Get();
    r.ctx->VSSetConstantBuffers(0, 1, &cb);
    r.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    auto& rtv = detail::g_bindingSlots[static_cast<size_t>(BindSlot::Rtv0)];
    rtv.ptr = target;
    ++rtv.gen;
    detail::g_bindingSlots[static_cast<size_t>(BindSlot::Vs)].hash = kUiVsWeaponBeams;
    detail::g_bindingSlots[static_cast<size_t>(BindSlot::Ps)].hash = kUiPsWeaponBeams;
}

FlatUiLayerDraw weaponFacts(uint64_t frame, Tex& hdr) {
    FlatUiLayerDraw d;
    d.frame = frame;
    d.vs = kUiVsWeaponBeams;
    d.ps = kUiPsWeaponBeams;
    d.color = hdr.tex.Get();
    d.width = hdr.w;
    d.height = hdr.h;
    d.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.hdrTarget = true;
    d.upstream = true;
    d.haveRows = true;
    d.rows[0][0] = 1.0f;
    d.rows[1][1] = 1.0f;
    d.rows[2][3] = 1.0f;
    return d;
}

void bindTone(Rig& r, Tex& hdr) {
    ID3D11RenderTargetView* target = r.eye[0].rtv.Get();
    r.ctx->OMSetRenderTargets(1, &target, nullptr);
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(kEyeW), static_cast<float>(kEyeH), 0, 1};
    r.ctx->RSSetViewports(1, &vp);
    r.ctx->RSSetState(r.raster.Get());
    const D3D11_RECT sc{0, 0, static_cast<LONG>(kEyeW), static_cast<LONG>(kEyeH)};
    r.ctx->RSSetScissorRects(1, &sc);
    r.ctx->OMSetBlendState(r.blend.Get(), nullptr, 0xFFFFFFFFu);
    r.ctx->OMSetDepthStencilState(r.dsOff.Get(), 0);
    ID3D11ShaderResourceView* source = hdr.srv.Get();
    r.ctx->PSSetShaderResources(0, 1, &source);
    ID3D11SamplerState* sampler = r.gameSampler.Get();
    r.ctx->PSSetSamplers(0, 1, &sampler);
    r.ctx->VSSetShader(r.vs.Get(), nullptr, 0);
    r.ctx->PSSetShader(r.ps.Get(), nullptr, 0);
    ID3D11Buffer* cb = r.rectCb.Get();
    r.ctx->VSSetConstantBuffers(0, 1, &cb);
    r.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
}

bool candidate(Rig& r, Tex& hdr, uint64_t frame) {
    bindTone(r, hdr);
    return flatUiLayerToneCandidate(r.ctx.Get(), frame, kEyeW, kEyeH, 0, 1, 2, hdr.tex.Get(), kEyeW, kEyeH, "test-hdr");
}

void runCase(Rig& r, Tex& hdr, const char* mode, const char* quality, uint64_t frame,
             bool copyTone = false, Tex* firstDecided = nullptr, bool expectSmoothed = false) {
    auto& cfg = Config::get();
    cfg.set("fix.ui_quality", quality);
    cfg.set("fix.sharp_weapons", mode);
    uiLayerConfigure(cfg);
    check(uiLayerWeaponsLive() && uiLayerCrispOn(), "flat weapon machinery is live in this key combination");
    uiLayerNoteTemporal(frame - 1, 0, r.frame[0].tex.Get());
    uiLayerDoorSeen(frame - 1, 0, r.frame[0].tex.Get());
    if (firstDecided) {
        // The first decision of this frame never reaches Begin. Its target
        // must not choose the copy admitted for later, actually taken pixels.
        g_stubs.eyeRes[0] = firstDecided->tex.Get();
        bindWeapon(r, *firstDecided);
        check(flatUiLayerDecide(r.ctx.Get(), weaponFacts(frame, *firstDecided)) == FlatUiLayerAsk::kDecided,
              "another proved target can be decided before the captured weapon draw");
        g_stubs.eyeRes[0] = hdr.tex.Get();
    }
    bindWeapon(r, hdr);
    const float empty[4] = {};
    r.ctx->ClearRenderTargetView(hdr.rtv.Get(), empty);
    check(flatUiLayerDecide(r.ctx.Get(), weaponFacts(frame, hdr)) == FlatUiLayerAsk::kDecided,
          "the proved flat weapon draw is decided with HUD quality on or off");
    const bool begun = flatUiLayerBegin(r.ctx.Get());
    check(begun, "the production take redirects the weapon draw");
    if (begun) {
        r.ctx->Draw(4, 0);
        flatUiLayerEnd(r.ctx.Get());
        flatUiLayerNoteIssue(kUiVsWeaponBeams, kUiPsWeaponBeams, true);
    }
    check(flatweapontest::weaponCaptured(frame), "the weapon draw is fresh and no HUD draw was captured");
    check(halfRedAt(r, flatweapontest::weaponLayer(), kDoorW / 2, kDoorH / 2) == 0x3C00u &&
              halfRedAt(r, hdr.tex.Get(), kEyeW / 2, kEyeH / 2) == 0,
          "the weapon's red pixel is in its captured HDR layer and absent from the game HDR target");

    // Reject a wrong target, a copy whose source was not captured, and an
    // absent tone source before asking the named flat tone route.
    bindTone(r, hdr);
    char why[128] = {};
    ID3D11ShaderResourceView* wrong = r.tiny.srv.Get();
    r.ctx->PSSetShaderResources(0, 1, &wrong);
    check(!flatUiLayerToneCandidate(r.ctx.Get(), frame, kEyeW, kEyeH, 0, 1, 2,
                                    r.tiny.tex.Get(), kEyeW, kEyeH, "test-hdr"),
          "an unrelated bound HDR target is refused");
    check(!uiLayerCrispAdmitFlat(r.ctx.Get(), 0, r.tiny.tex.Get(), r.tiny.tex.Get(), 1, 2, why, sizeof(why)),
          "an alias of an unrelated source cannot rescue weapon content");
    ID3D11ShaderResourceView* none = nullptr;
    r.ctx->PSSetShaderResources(0, 1, &none);
    check(!uiLayerCrispAdmitFlat(r.ctx.Get(), 0, nullptr, nullptr, 1, 2, why, sizeof(why)),
          "a missing tone source is refused");

    Tex alias;
    Tex* toneInput = &hdr;
    if (copyTone) {
        alias = hdrTarget(r);
        check(alias.srv && alias.rtv, "the tone copy is available");
        if (alias.srv) {
            r.ctx->CopyResource(alias.tex.Get(), hdr.tex.Get());
            flatUiLayerNoteCopy(frame, hdr.tex.Get(), alias.tex.Get());
            toneInput = &alias;
        }
    }
    check(candidate(r, *toneInput, frame), "the named flat tone route admits fresh weapon content or its proven copy");
    r.ctx->Draw(4, 0);  // the game's own tone issue
    const bool reissued = flatUiLayerToneBegin(r.ctx.Get());
    check(reissued, "the production tone bracket begins after admission");
    if (reissued) {
        check(std::strcmp(mode, "on") == 0 ? flatweapontest::matchedPlan() : flatweapontest::plainPlan(),
              "the selected weapon blend has its expected production tone plan");
        do { r.ctx->Draw(4, 0); } while (flatUiLayerToneNext(r.ctx.Get()));
        flatUiLayerToneEnd(r.ctx.Get());
    }
    check(flatweapontest::published(frame), "the tone bracket published this frame's weapon light");

    ID3D11ShaderResourceView* frameSource = r.frame[0].srv.Get();
    const auto originalPixels = readPixels(r, r.frame[0].tex.Get());
    check((at(originalPixels, kDoorW, kDoorW / 2, kDoorH / 2) & 0xFFu) == 0,
          "the uncomposited frame has no weapon red");
    r.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    r.ctx->PSSetShaderResources(0, 1, &frameSource);
    ID3D11ShaderResourceView* original = nullptr;
    bool replaced = false;
    flatUiLayerAtCopy(r.ctx.Get(), frame, true, kDoorW, kDoorH, &original, &replaced);
    // The HUD layer's edge smoothing (fix.hud_antialiasing, off by default): with the key off the adapter asked for the layer as it is and the composite read the
    // raw layer; with it on the composite read the SMAA output. A key the flat allow-list lacked would read off here and fail the smoothed cases.
    const UiLayerEdgeSmoothingResult edge = uiLayerFlatEdgeSmoothingResult();
    check(edge.outcome == (expectSmoothed ? UiLayerEdgeSmoothing::kSmoothed : UiLayerEdgeSmoothing::kRaw) && edge.w && edge.h,
          expectSmoothed ? "with the key on the flat copy's composite read the SMAA output of the layer" : "with the key off (the shipped state) the flat copy's composite read the raw layer");
    ComPtr<ID3D11ShaderResourceView> composite;
    r.ctx->PSGetShaderResources(0, 1, &composite);
    ComPtr<ID3D11Resource> resource;
    if (composite) composite->GetResource(&resource);
    ComPtr<ID3D11Texture2D> output;
    if (resource) resource.As(&output);
    check(replaced && output && composite.Get() != frameSource, "the flat copy binds a published composite");
    if (output) {
        const auto pixels = readPixels(r, output.Get());
        const uint32_t pixel = at(pixels, kDoorW, kDoorW / 2, kDoorH / 2);
        const auto tonePixels = readPixels(r, flatweapontest::toneLayer());
        check((at(tonePixels, kDoorW, kDoorW / 2, kDoorH / 2) & 0xFFu) > 20u && (pixel & 0xFFu) > 20u,
              "the tone layer and published composite both contain visible weapon light");
    }
    if (original) original->Release();
    r.ctx->PSSetShaderResources(0, 1, &none);
    check(!candidate(r, *toneInput, frame), "a second tone of the same frame is refused");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("flat_weapon_admission_test: dry-run (no device, no files)");
        return 0;
    }
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) return 2;
    runtimeProfileSet(RuntimeProfile::Flat);
    Rig r;
    if (!setup(r, false)) return 1;
    Tex hdr = hdrTarget(r);
    if (!hdr.srv || !hdr.rtv) return 1;
    g_stubs.eyeRes[0] = hdr.tex.Get();
    auto& cfg = Config::get();
    cfg.set("fix.temporal_aa", "dlss");
    cfg.set("fix.sharp_weapons", "on");
    cfg.set("fix.ui_quality", "off");
    uiLayerConfigure(cfg);
    const uint64_t first = 100;
    bindWeapon(r, hdr);
    check(flatUiLayerDecide(r.ctx.Get(), weaponFacts(first, hdr)) == FlatUiLayerAsk::kRefused,
          "before a tone proves the HDR target, the weapon stays in the game frame");
    check(!candidate(r, hdr, first), "the proving tone has no captured weapon content to reissue");
    // The rig has not set the key: HUD Anti-Aliasing (fix.hud_antialiasing) is off by default, so this first composite reads the raw layer.
    runCase(r, hdr, "on", "off", first + 1, false, nullptr, false);
    // The key is live: on, the next frame's composite reads the smoothed layer; off again, the raw one; on once more for the frames below.
    cfg.set("fix.hud_antialiasing", "on");
    runCase(r, hdr, "plain", "off", first + 2, false, nullptr, true);
    cfg.set("fix.hud_antialiasing", "off");
    runCase(r, hdr, "on", "100", first + 3, false, nullptr, false);
    cfg.set("fix.hud_antialiasing", "on");
    // Also prove a second physical target for frame N+4. Its first decision
    // in that frame will be abandoned, while the later weapon target is
    // captured and copied into the actual tone input.
    Tex other = hdrTarget(r);
    check(other.srv && other.rtv, "the second HDR target is available");
    if (other.srv) {
        g_stubs.eyeRes[0] = other.tex.Get();
        bindWeapon(r, other);
        check(flatUiLayerDecide(r.ctx.Get(), weaponFacts(first + 3, other)) == FlatUiLayerAsk::kRefused,
              "a new target is still refused until a tone proves it");
        check(!candidate(r, other, first + 3), "the second target is proven without captured content");
        g_stubs.eyeRes[0] = hdr.tex.Get();
        runCase(r, hdr, "plain", "100", first + 4, true, &other, true);
    }
    // The last captured content is stale in the next frame, even though the
    // same HDR target and tone pair are still bound.
    check(!candidate(r, hdr, first + 5), "stale weapon content is refused in the next flat frame");
    uiLayerShutdown();
    std::printf("flat_weapon_admission_test: %u checks, %u failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

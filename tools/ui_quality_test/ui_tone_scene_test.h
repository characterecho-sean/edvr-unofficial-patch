// Included in ui_quality_test.cpp after ui_flat_panel_settle_test.h (check; afterui::functionBody and squeeze; worldroute::readText and has).
//
// THE TONEMAP RE-ISSUE'S SCENE IMAGES (docs/cockpit-hud-layer-design-2026-09-27.md, "2026-10-20: bloom counted twice"). The crisp-HUD re-issue
// swaps only the tone's HDR source; with Elite's Bloom on the tone reads the scene's bloom from a slot of its own, so the layer came out
// T(HUD + bloom) and the composite added the bloom a second time over the whole frame. What these cases hold:
//   * the rule (ui_layer_math.h toneInputIsSceneImage) zeroes the measured bloom input (quarter size at t2 of the flat E894 tone, 1720x720) and
//     a reduced frame rounded either way, from 1/32 of the HDR source to its full size, and keeps the HDR source's own slot, the 3D LUT, the
//     1x1 exposure, a single-channel table on a square eye, a strip, an asset texture, a texture larger than the source, and the DoF
//     composite's blur (the one input the tone blends rather than adds, which the re-issue gives the step's own source instead);
//   * the steps (weapons_fire.h toneStepKeepsSceneImages): every step that reads the HUD, and the plain weapons replace, reads zero; the
//     matched pair keeps them, and both of its terms alike, or their difference would carry the bloom;
//   * the wiring (ui_layer.cpp, by source scan, with controls): Begin finds the images before the first step binds, at every PS slot t0..t15
//     (so a tone of any layout is covered: the Full System Scanner's has the bloom at t1), every step binds by the
//     step's answer -- null at the images and its own source at the DoF blur, or the game's own views -- and End and Begin's fault path put
//     the game's own views back, the blur's included.
namespace tonescene {

void testRule() {
    ToneInputShape bloom;
    bloom.tex2d = true;
    bloom.gpuWritten = true;
    bloom.viewFormat = 26;  // R11G11B10_FLOAT
    bloom.w = 430;
    bloom.h = 180;
    // The 11:40 XeSS cockpit capture (design-flat-ui-quality-2026-10-05.md): the HUD at t1, 1720x720; t2 quarter-sized bloom.
    check(toneInputIsSceneImage(2, 1, -1, bloom, 1720, 720), "the flat E894 tone's quarter-size bloom at t2 is a scene image");
    check(!toneInputIsSceneImage(1, 1, -1, bloom, 1720, 720), "the HDR source's own slot is never zeroed");
    check(!toneInputIsSceneImage(1, 0, 1, bloom, 1720, 720), "the DoF composite's blur (its blend slot, PS1) is not zeroed");
    check(toneInputIsSceneImage(1, 0, -1, bloom, 1720, 720), "9270's bloom at t1 (HDR at t0) is a scene image");
    // The Full System Scanner's tone (6DE70EBB, census 2 of edvr_gfx_20261022_112019): the scene at t0 (960x600 R11G11B10F), a half-size
    // bloom at t1 (480x300 R11G11B10F, a render target of the bloom chain), the same layout as 9270's that the 10-22 logs show ZEROED at t1.
    // Replaying it over the HUD layer must read zero at t1, or the composite would add the scanner's bloom a second time.
    {
        ToneInputShape half = bloom;
        half.w = 480;
        half.h = 300;
        check(toneInputIsSceneImage(1, 0, -1, half, 960, 600), "the scanner tone's half-size bloom at t1 (scene at t0) is a scene image");
        check(!toneInputIsSceneImage(0, 0, -1, half, 960, 600), "the scanner tone's scene at its HDR slot t0 is never zeroed");
        ToneInputShape scene = half;
        scene.w = 960;
        scene.h = 600;
        check(!toneInputIsSceneImage(0, 0, -1, scene, 960, 600), "the scanner tone's own HDR source is not a scene image");
    }
    ToneInputShape s = bloom;
    s.viewFormat = 10;  // R16G16B16A16_FLOAT
    check(toneInputIsSceneImage(2, 1, -1, s, 1720, 720), "an RGBA16F bloom is a scene image");
    s.viewFormat = 28;  // R8G8B8A8_UNORM
    check(toneInputIsSceneImage(2, 1, -1, s, 1720, 720), "an 8-bit colour bloom is a scene image");
    s = bloom;
    s.w = 1720;
    s.h = 720;
    check(toneInputIsSceneImage(3, 1, -1, s, 1720, 720), "the scene itself bound at a second slot is a scene image (the frame carries it)");
    s.w = 54;
    s.h = 23;
    check(toneInputIsSceneImage(2, 1, -1, s, 1720, 720), "1/32 of the source, rounded up, is a scene image");
    s.w = 27;
    s.h = 12;
    check(!toneInputIsSceneImage(2, 1, -1, s, 1720, 720), "below 1/32 of the source it is not");
    s.w = 2150;
    s.h = 900;
    check(!toneInputIsSceneImage(2, 1, -1, s, 1720, 720), "a texture larger than the source (the HUD layer at 125) is not");
    // A VR eye, 2037x1969: its quarter rounded up and down are both the same shape within a pixel; a square 128 is not.
    s.w = 510;
    s.h = 493;
    check(toneInputIsSceneImage(2, 1, -1, s, 2037, 1969), "a VR eye's quarter, rounded up, is a scene image");
    s.w = 509;
    s.h = 492;
    check(toneInputIsSceneImage(2, 1, -1, s, 2037, 1969), "a VR eye's quarter, rounded down, is a scene image");
    s.w = 128;
    s.h = 128;
    check(!toneInputIsSceneImage(2, 1, -1, s, 2037, 1969), "a square 128 on a 2037x1969 eye is not the frame's shape");
    s.w = 1024;
    s.h = 32;
    s.viewFormat = 28;
    check(!toneInputIsSceneImage(2, 1, -1, s, 1720, 720), "a 1024x32 strip (a 2D LUT) is not");
    // A square eye (2016x2016, the 2026-10-16 weapons flight): a single-channel square table there has the frame's shape, so the format
    // carries the decision.
    s.w = 64;
    s.h = 64;
    s.viewFormat = 41;  // R32_FLOAT
    check(!toneInputIsSceneImage(2, 1, -1, s, 2016, 2016), "a single-channel 64x64 table on a square eye is not");
    s.w = 1;
    s.h = 1;
    check(!toneInputIsSceneImage(0, 1, -1, s, 1720, 720), "the 1x1 exposure is not");
    s = bloom;
    s.tex2d = false;
    check(!toneInputIsSceneImage(0, 1, -1, s, 1720, 720), "the 3D LUT at t0 is not");
    s = bloom;
    s.gpuWritten = false;
    check(!toneInputIsSceneImage(2, 1, -1, s, 1720, 720), "an asset texture (no render target or UAV binding) is not");
    check(!toneInputIsSceneImage(2, 1, -1, bloom, 0, 0), "with no HDR source size nothing is zeroed");
    check(toneInputColourFormat(26) && toneInputColourFormat(10) && toneInputColourFormat(2) && toneInputColourFormat(87) &&
              !toneInputColourFormat(41) && !toneInputColourFormat(54) && !toneInputColourFormat(34) && !toneInputColourFormat(0),
          "colour formats are three or more channels; R32F, R16F, RG16F and unknown are not");
}

void testSteps() {
    check(!toneStepKeepsSceneImages(ToneStep::kHudReplace) && !toneStepKeepsSceneImages(ToneStep::kHudAdd),
          "the HUD's steps read zero at the scene's images");
    check(!toneStepKeepsSceneImages(ToneStep::kWeaponsReplace), "the plain weapons replace reads zero at the scene's images");
    check(toneStepKeepsSceneImages(ToneStep::kWeaponsBase) && toneStepKeepsSceneImages(ToneStep::kWeaponsLit),
          "the matched pair keeps them, both terms alike");
    // Every plan: no step that reads the HUD keeps the scene's images; a plan's base and lit terms agree.
    for (int hud = 0; hud < 2; ++hud)
        for (int weapons = 0; weapons < 2; ++weapons)
            for (int mode = 0; mode < 2; ++mode)
                for (int ready = 0; ready < 2; ++ready) {
                    const TonePlan p = sharpTonePlan(hud != 0, weapons != 0, mode ? SharpBlend::kPlain : SharpBlend::kMatched, ready != 0);
                    int base = -1, lit = -1;
                    for (uint8_t i = 0; i < p.count; ++i) {
                        if (toneStepSource(p.steps[i]) == ToneSource::kHud)
                            check(!toneStepKeepsSceneImages(p.steps[i]), "no plan's HUD step keeps the scene's images");
                        if (p.steps[i] == ToneStep::kWeaponsBase) base = toneStepKeepsSceneImages(p.steps[i]) ? 1 : 0;
                        if (p.steps[i] == ToneStep::kWeaponsLit) lit = toneStepKeepsSceneImages(p.steps[i]) ? 1 : 0;
                    }
                    check(base == lit, "a plan's matched base and lit terms bind the scene's images alike");
                }
}

struct Pin {
    const char* id;
    bool ok;
    const char* what;
};

std::vector<Pin> pins(const std::string& text) {
    using afterui::functionBody;
    using afterui::squeeze;
    std::string bind, find, begin, end, restore;
    const bool found = functionBody(text, "void crispBindStep(", &bind) && functionBody(text, "void crispFindSceneImages(", &find) &&
                       functionBody(text, "bool uiLayerCrispToneBegin(", &begin) && functionBody(text, "void uiLayerCrispToneEnd(", &end) &&
                       functionBody(text, "void crispRestoreSceneImages(", &restore);
    const std::string b = squeeze(bind), f = squeeze(find), g = squeeze(begin), e = squeeze(end), r = squeeze(restore);
    std::vector<Pin> out;
    out.push_back({"found", found,
                   "crispBindStep, crispFindSceneImages, crispRestoreSceneImages, uiLayerCrispToneBegin and uiLayerCrispToneEnd are found"});
    out.push_back({"step-binds",
                   worldroute::has(b, "constboolkeep=toneStepKeepsSceneImages(step);") &&
                       worldroute::has(b, "ID3D11ShaderResourceView*v=keep?g_crispSave.sceneSrv[s].Get():nullptr;") &&
                       worldroute::has(b, "vScreenPSSetShaderResourcesRaw(ctx,static_cast<uint32_t>(s),1,&v);"),
                   "every step binds the scene's images by the step's answer: null, or the game's own view"});
    const size_t findAt = g.find("crispFindSceneImages(ctx,gameHdr.Get(),p.hdrSlot,p.vs,p.ps);");
    const size_t firstStep = g.find("crispBindStep(ctx,plan.steps[0]);");
    out.push_back({"begin-finds", findAt != std::string::npos && firstStep != std::string::npos && findAt < firstStep,
                   "Begin finds the scene's images from the admitted draw's own bindings before the first step binds"});
    out.push_back({"blur-binds",
                   worldroute::has(b, "if(g_crispSave.blendSrv){ID3D11ShaderResourceView*v=keep?g_crispSave.blendSrv.Get():src;"
                                      "vScreenPSSetShaderResourcesRaw(ctx,static_cast<uint32_t>(g_crispSave.blendSlot),1,&v);}"),
                   "every step binds the DoF blur by the step's answer: the step's own source (never zero, which would dim it), or the game's"});
    out.push_back({"blur-saved",
                   worldroute::has(f, "g_crispSave.blendSlot=-1;g_crispSave.blendSrv.Reset();") &&
                       worldroute::has(f, "constboolblur=s==blendSlot&&s!=hdrSlot&&in.tex2d;if(blur){g_crispSave.blendSlot=s;"
                                          "g_crispSave.blendSrv=view;}"),
                   "the DoF blur the game bound is saved, and forgotten at every Begin"});
    out.push_back({"blur-restores",
                   worldroute::has(r, "if(g_crispSave.blendSrv){ID3D11ShaderResourceView*v=g_crispSave.blendSrv.Get();"
                                      "vScreenPSSetShaderResourcesRaw(ctx,static_cast<uint32_t>(g_crispSave.blendSlot),1,&v);}"),
                   "the restore puts the game's DoF blur back"});
    out.push_back({"end-restores", worldroute::has(e, "crispRestoreSceneImages(ctx);"), "End puts the game's views back"});
    out.push_back({"fault-restores", worldroute::has(g, "crispRestoreSceneImages(ctx);"), "Begin's fault path puts the game's views back"});
    out.push_back({"scans-every-slot",
                   worldroute::has(f, "for(ints=0;s<kToneSceneScanSlots;++s){Ptr<ID3D11ShaderResourceView>view;"),
                   "the scene's images are looked for at every PS slot t0..t15, whatever the tone's layout (the bloom is at t2 on E894, t1 on 9270 and the scanner's tone)"});
    out.push_back({"find-rule",
                   worldroute::has(f, "constboolscene=toneInputIsSceneImage(s,hdrSlot,blendSlot,in,hdrW,hdrH);") &&
                       worldroute::has(f, "constintblendSlot=ps==flat_mono_detail::kToneDofCompositePs?1:-1;") &&
                       worldroute::has(f, "g_crispSave.sceneMask|=1u<<s;g_crispSave.sceneSrv[s]=view;"),
                   "the scene's images are the rule's, the DoF composite's blur is its blend slot, and each one found is saved"});
    return out;
}

void testWiring() {
    const std::string text = worldroute::readText("src/d3d11/ui_layer.cpp");
    check(!text.empty(), "src/d3d11/ui_layer.cpp is readable from the working directory (tone scene images wiring)");
    if (text.empty()) return;
    for (const Pin& p : pins(text)) {
        char msg[320];
        std::snprintf(msg, sizeof(msg), "tone scene wiring [%s]: %s", p.id, p.what);
        check(p.ok, msg);
    }
    // CONTROLS: one edit each that puts a likely slip back; the pin named must fail.
    struct Control { const char* pin; const char* from; const char* to; };
    const Control controls[] = {
        {"step-binds", "const bool keep = toneStepKeepsSceneImages(step);", "const bool keep = true;"},
        {"step-binds", "keep ? g_crispSave.sceneSrv[s].Get() : nullptr", "g_crispSave.sceneSrv[s].Get()"},
        {"begin-finds", "        crispFindSceneImages(ctx, gameHdr.Get(), p.hdrSlot, p.vs, p.ps);\n", ""},
        {"end-restores", "            vScreenPSSetShaderResourcesRaw(ctx, static_cast<uint32_t>(hdrSlot), 1, &gameHdr);\n            crispRestoreSceneImages(ctx);\n",
         "            vScreenPSSetShaderResourcesRaw(ctx, static_cast<uint32_t>(hdrSlot), 1, &gameHdr);\n"},
        {"fault-restores", "                }\n                crispRestoreSceneImages(ctx);\n", "                }\n"},
        {"scans-every-slot", "    for (int s = 0; s < kToneSceneScanSlots; ++s) {\n        Ptr<ID3D11ShaderResourceView> view;",
         "    for (int s = 2; s < kToneSceneScanSlots; ++s) {\n        Ptr<ID3D11ShaderResourceView> view;"},
        {"find-rule", "ps == flat_mono_detail::kToneDofCompositePs ? 1 : -1", "-1"},
        {"find-rule", "            g_crispSave.sceneSrv[s] = view;\n", ""},
        {"blur-binds", "keep ? g_crispSave.blendSrv.Get() : src", "keep ? g_crispSave.blendSrv.Get() : nullptr"},
        {"blur-binds", "keep ? g_crispSave.blendSrv.Get() : src", "g_crispSave.blendSrv.Get()"},
        {"blur-saved", "            g_crispSave.blendSrv = view;\n", ""},
        {"blur-saved", "    g_crispSave.blendSrv.Reset();\n", ""},
        {"blur-restores", "        ID3D11ShaderResourceView* v = g_crispSave.blendSrv.Get();\n", "        ID3D11ShaderResourceView* v = nullptr;\n"},
    };
    for (const Control& c : controls) {
        const size_t at = text.find(c.from);
        char msg[300];
        if (at == std::string::npos) {
            std::snprintf(msg, sizeof(msg), "control for tone scene [%s]: the text it edits is in ui_layer.cpp", c.pin);
            check(false, msg);
            continue;
        }
        std::string edited = text;
        edited.replace(at, std::strlen(c.from), c.to);
        bool ok = true;
        for (const Pin& p : pins(edited))
            if (std::strcmp(p.id, c.pin) == 0) ok = p.ok;
        std::snprintf(msg, sizeof(msg), "control: one edit trips tone scene wiring [%s]", c.pin);
        check(!ok, msg);
    }
}

void testAll() {
    testRule();
    testSteps();
    testWiring();
}

}  // namespace tonescene

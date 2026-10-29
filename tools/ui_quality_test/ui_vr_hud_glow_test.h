// Included in ui_quality_test.cpp after ui_vr_star_glow_test.h (check; worldroute::readText, has, inOrder, countOf; afterui::squeeze, functionBody).
//
// The HUD's bloom in the headset (docs/cockpit-hud-layer-design-2026-09-27.md, "2026-10-28: the HUD's bloom in the headset"). What these cases hold:
//   * when it runs: on while the HDR HUD take is armed and Elite's Bloom is not known off, paused while it is, off while the take is not armed;
//   * which eye's HUD layer an H is, by identity and never by the eye's index or the draw order: the verdict (feed, no-layer, no-content, tone-done, repeat) for every
//     combination of the four facts, the interleaved eyes of the 10-27 census in either order, and when the hook stops asking (an eye claimed, the frame ended);
//   * the window: what one first-level draw adds to which count, a feed the pass could not make counted as a failed pass and never as a silent miss, and the lines
//     with exact text;
//   * the arithmetic: H' = H + HUD + m * S per channel (flat's formula, which the GPU passes are held to), and that with the HUD at zero it is the star glow's alone;
//   * THE ONCE-ONLY ARGUMENT, as measured inputs and as pins. The scene is tonemapped once (the game's own tone over H and the bloom), the HUD once (the re-issue over the layer
//     with the scene's bloom zeroed), and H' by none: the measured VR tone inputs (log 20261028_175126: t2 1008x1008 R11G11B10_FLOAT beside an HDR source of 2016x2017)
//     are zeroed by the rule that already zeroes flat's; H' is bound at t0 for the first-level draw alone and the game's view comes back through the raw setter; the feed
//     touches no tonemap code; the flat feed is still gated on the flat profile;
//   * the wiring, by source scan with controls.
namespace vrhudglowtest {

using worldroute::countOf;
using worldroute::has;
using worldroute::inOrder;
using worldroute::readText;

UiHeadsetHudEye eyeFacts(bool drawn, bool fresh, bool toned, bool claimed) {
    UiHeadsetHudEye e;
    e.drawnFromH = drawn;
    e.fresh = fresh;
    e.toned = toned;
    e.claimed = claimed;
    return e;
}

// ------------------------------------------------------------------ when it runs

void testMode() {
    const sgb::Bloom states[] = {sgb::Bloom::Unknown, sgb::Bloom::Off, sgb::Bloom::On};
    bool offWhenNotArmed = true;
    for (sgb::Bloom b : states) offWhenNotArmed = offWhenNotArmed && vrHudGlowMode(false, b) == VrHudGlowMode::kOff;
    check(offWhenNotArmed, "vr hud glow: with the HDR HUD take not armed the mode is off whatever Elite's Bloom is (no layer holds anything)");
    check(vrHudGlowMode(true, sgb::Bloom::On) == VrHudGlowMode::kOn && vrHudGlowMode(true, sgb::Bloom::Unknown) == VrHudGlowMode::kOn,
          "vr hud glow: armed with Bloom on, or not known, it runs (a wrong pause costs a halo; a wasted compare costs nothing)");
    check(vrHudGlowMode(true, sgb::Bloom::Off) == VrHudGlowMode::kPaused, "vr hud glow: armed with Bloom known off it is paused (the chain has no first level to feed)");
}

// ------------------------------------------------------------------ which eye's layer an H is

void testPick() {
    int who = 7;
    UiHeadsetHudEye e[2];
    e[0] = eyeFacts(false, false, false, false);
    e[1] = eyeFacts(false, false, false, false);
    check(uiHeadsetHudPick(e, &who) == UiHeadsetHud::kNoLayer && who == -1, "vr hud glow: an H no eye's layer was taken from is no-layer, with no eye");
    check(uiHeadsetHudPick(e, nullptr) == UiHeadsetHud::kNoLayer, "vr hud glow: the eye may be left unasked");
    // The four facts of the eye whose H it is, in every combination (the other eye drawn from another H).
    bool table = true;
    for (int bits = 0; bits < 8; ++bits) {
        const bool fresh = bits & 1, toned = bits & 2, claimed = bits & 4;
        e[0] = eyeFacts(true, fresh, toned, claimed);
        e[1] = eyeFacts(false, true, false, false);   // the other eye holds fresh, unclaimed light of its own H: it must change nothing
        UiHeadsetHud want = UiHeadsetHud::kFeed;
        if (!fresh) want = UiHeadsetHud::kNoContent;
        else if (toned) want = UiHeadsetHud::kToneDone;
        else if (claimed) want = UiHeadsetHud::kRepeat;
        who = 7;
        table = table && uiHeadsetHudPick(e, &who) == want && who == 0;
    }
    check(table, "vr hud glow: the verdict for every combination of fresh, toned and claimed -- stale content is no content, toned beats claimed, and the other eye's layer changes nothing");
    // Identity, not index: the same facts on either eye give the same verdict for the eye they belong to.
    e[0] = eyeFacts(false, true, false, false);
    e[1] = eyeFacts(true, true, false, false);
    check(uiHeadsetHudPick(e, &who) == UiHeadsetHud::kFeed && who == 1, "vr hud glow: the layer is found by the H it was taken from: eye 1's H is fed eye 1's HUD");
    e[0] = eyeFacts(true, true, false, false);
    e[1] = eyeFacts(false, true, false, false);
    check(uiHeadsetHudPick(e, &who) == UiHeadsetHud::kFeed && who == 0, "vr hud glow: ...and eye 0's H eye 0's, whichever eye's first level comes first");
    // Two layers cannot both be this H's; if the table ever said so the first answers (and the second is never fed twice: it is claimed).
    e[0] = eyeFacts(true, true, false, true);
    e[1] = eyeFacts(true, true, false, false);
    check(uiHeadsetHudPick(e, &who) == UiHeadsetHud::kRepeat && who == 0, "vr hud glow: a claimed layer answers repeat and is not fed again");
}

// The hook's asking, over a frame of the 10-27 census's shape: the eyes' draws interleave (eye A's H draws, eye B's HUD, A's first level, B's first level...).
struct Frame {
    UiHeadsetHudEye eyes[2];
    bool pending = false;
    int fed[2] = {0, 0};
    int notFed = 0;
    // The eye's first HUD draw of the frame: its layer holds this frame's light, and the hook wakes (ui_layer.cpp's take).
    void hud(int e) {
        eyes[e] = eyeFacts(true, true, false, false);
        pending = true;
    }
    // A first-level draw reading eye `e`'s H (or an H no layer is: e = -1). The hook asks only while pending; a feed claims the eye.
    void firstLevel(int e) {
        if (!pending) return;
        UiHeadsetHudEye view[2] = {eyes[0], eyes[1]};
        for (int i = 0; i < 2; ++i) view[i].drawnFromH = i == e;
        int who = -1;
        const UiHeadsetHud pick = uiHeadsetHudPick(view, &who);
        if (pick == UiHeadsetHud::kFeed) {
            ++fed[who];
            eyes[who].claimed = true;
            pending = uiHeadsetHudWaiting(eyes);
        } else {
            ++notFed;
        }
    }
    void tone(int e) { eyes[e].toned = true; }
    void boundary() {
        pending = false;
        eyes[0] = eyes[1] = UiHeadsetHudEye{};
    }
};

void testFrames() {
    Frame f;
    f.hud(0);
    f.hud(1);
    check(f.pending, "vr hud glow: the first HUD draw of an eye-frame wakes the hook");
    f.firstLevel(1);
    check(f.fed[1] == 1 && f.fed[0] == 0 && f.pending, "vr hud glow: eye 1's first level feeds eye 1's HUD and the hook keeps asking for eye 0's");
    f.firstLevel(0);
    check(f.fed[0] == 1 && f.fed[1] == 1 && !f.pending, "vr hud glow: eye 0's first level feeds eye 0's HUD, and with no eye waiting the hook stops asking");
    f.firstLevel(0);
    check(f.fed[0] == 1 && f.notFed == 0, "vr hud glow: a second first level of the same frame is not even looked at (the hook is asleep)");
    f.boundary();
    check(!f.pending && !f.eyes[0].fresh && !f.eyes[1].fresh, "vr hud glow: the frame's end puts both eyes back to nothing waiting");

    // A frame whose eye 0 took its HUD after eye 1's first level (the draws interleave): eye 1 is fed, eye 0 waits for its own.
    Frame g;
    g.hud(1);
    g.firstLevel(1);
    check(g.fed[1] == 1 && !g.pending, "vr hud glow: a lone eye's HUD is fed at its own first level");
    g.hud(0);
    check(g.pending, "vr hud glow: ...and the other eye's HUD, taken later, wakes the hook again");
    g.firstLevel(0);
    check(g.fed[0] == 1 && g.fed[1] == 1 && !g.pending, "vr hud glow: ...and is fed at its own first level, once each");

    // A first level that reads an H no layer was taken from (the 2D screen's scene) is counted not-fed and claims nothing.
    Frame h;
    h.hud(0);
    h.firstLevel(-1);
    check(h.notFed == 1 && h.fed[0] == 0 && h.pending, "vr hud glow: a first level reading an H no layer belongs to is no-layer and leaves the eye waiting");
    h.firstLevel(0);
    check(h.fed[0] == 1 && !h.pending, "vr hud glow: ...for its own first level");

    // The tone ran before the first level (the bloom of the frame is made): too late, never fed.
    Frame k;
    k.hud(0);
    k.tone(0);
    check(!uiHeadsetHudWaiting(k.eyes), "vr hud glow: an eye whose content was already tonemapped is not waiting");
    k.firstLevel(0);
    check(k.fed[0] == 0 && k.notFed == 1, "vr hud glow: a first level that comes after the re-issue is tone-done, not fed");
    const UiHeadsetHudEye none[2] = {};
    check(!uiHeadsetHudWaiting(none), "vr hud glow: nothing waits when no eye holds light");
}

// ------------------------------------------------------------------ the window and its lines

void testWindow() {
    VrHudGlowWindow w;
    vrHudCount(w, UiHeadsetHud::kFeed, 0, true, 0);
    vrHudCount(w, UiHeadsetHud::kFeed, 1, true, 0);
    vrHudCount(w, UiHeadsetHud::kFeed, 1, true, 0);
    check(w.firstLevels == 3 && w.fed[0] == 1 && w.fed[1] == 2, "vr hud glow: a feed is counted for its eye, and every first level the hook looked at is counted");
    vrHudCount(w, UiHeadsetHud::kNoLayer, -1, false, 1);
    vrHudCount(w, UiHeadsetHud::kNoContent, 0, false, 1);
    vrHudCount(w, UiHeadsetHud::kToneDone, 0, false, 1);
    vrHudCount(w, UiHeadsetHud::kRepeat, 1, false, 1);
    check(w.notFed[0] == 1 && w.notFed[1] == 1 && w.notFed[2] == 1 && w.notFed[3] == 1 && w.fed[0] == 1 && w.fed[1] == 2,
          "vr hud glow: each reason lands in its own not-fed count and none is a feed");
    // A draw picked for a feed whose pass came to nothing is a failed pass, by the pass's verdict: never a silent miss, never a feed.
    for (int v = 1; v < static_cast<int>(UiGlowFeed::kCount); ++v) vrHudCount(w, UiHeadsetHud::kFeed, 0, false, v);
    bool each = true;
    for (size_t i = 0; i < kVrHudPassFailed; ++i) each = each && w.passFailed[i] == 1;
    check(each && w.fed[0] == 1, "vr hud glow: a feed the pass could not make is counted under the pass's own verdict (no-hud ... fault) and is not a feed");
    vrHudCount(w, UiHeadsetHud::kFeed, 5, true, 0);
    vrHudCount(w, UiHeadsetHud::kFeed, 0, false, 0);
    vrHudCount(w, UiHeadsetHud::kFeed, 0, false, 99);
    check(w.fed[0] == 1 && w.fed[1] == 2, "vr hud glow: an eye outside 0/1, a verdict that is no failure and one out of range count no feed");

    VrHudGlowWindow line;
    line.eyeFrames[0] = 900;
    line.eyeFrames[1] = 900;
    line.firstLevels = 1800;
    line.fed[0] = 900;
    line.fed[1] = 899;
    line.notFed[3] = 1;
    char out[900];
    int n = vrHudGlowWindowLine(out, sizeof(out), line);
    check(n > 0 && n == static_cast<int>(std::strlen(out)) &&
              std::strcmp(out, "vr hud glow: window=10s hud-eye-frames[left,right]=900,900 first-levels=1800 fed[left,right]=900,899 "
                               "not-fed[no-layer,no-content,tone-done,repeat]=0,0,0,1 pass-failed[no-hud,not-ready,binding-not-h,texture,format,fault]=0,0,0,0,0,0") == 0,
          "vr hud glow: the window line is the format the log shows (hud-eye-frames beside the first levels and feeds)");
    VrHudGlowWindow zero;
    n = vrHudGlowWindowLine(out, sizeof(out), zero);
    check(n > 0 && std::strstr(out, "hud-eye-frames[left,right]=0,0 first-levels=0 fed[left,right]=0,0") != nullptr,
          "vr hud glow: with every count zero the line says the hook saw no HUD and no first level");
    char tiny[48];
    n = vrHudGlowWindowLine(tiny, sizeof(tiny), line);
    check(n > 0 && n < static_cast<int>(sizeof(tiny)) && std::strlen(tiny) == static_cast<size_t>(n), "vr hud glow: a short buffer cuts the line and stays terminated");
    check(vrHudGlowWindowLine(nullptr, 100, line) == 0 && vrHudGlowWindowLine(out, 4, line) == 0, "vr hud glow: no buffer, or one too small, prints nothing");

    n = vrHudGlowOnLine(out, sizeof(out), 123);
    check(n > 0 && std::strstr(out, "vr hud glow: on from frame 123: ") == out && std::strstr(out, "143AAE0597E2F7BF") != nullptr, "vr hud glow: the on line names the frame and the first level's shader");
    n = vrHudGlowPausedLine(out, sizeof(out));
    check(n > 0 && std::strstr(out, "vr hud glow: paused: Elite's Bloom is known to be off") == out, "vr hud glow: the paused line says why");
    n = vrHudGlowOffLine(out, sizeof(out));
    check(n > 0 && std::strstr(out, "vr hud glow: off: the HDR HUD take is not armed") == out, "vr hud glow: the off line says why");
    n = vrHudGlowFedLine(out, sizeof(out), 1, 2016, 2017, 77);
    check(n > 0 && std::strstr(out, "vr hud glow: the HUD's light joins the game's bloom -- ") == out && std::strstr(out, "the right eye") != nullptr &&
              std::strstr(out, "(H 2016x2017)") != nullptr && std::strstr(out, "tonemapped once") != nullptr && std::strstr(out, "reaches no tonemap") != nullptr,
          "vr hud glow: the once-only feed line names the eye, H, and says the body is tonemapped once and H' reaches no tonemap");
    n = vrHudGlowFailLine(out, sizeof(out), 0, 3, false);
    check(n > 0 && std::strstr(out, "vr hud glow: H' could not be built for the left eye's first level") == out && std::strstr(out, "verdict 3 = binding-not-h") != nullptr &&
              std::strstr(out, "missing") != nullptr,
          "vr hud glow: the failure line names the eye, the verdict by name and whether the game's view was held");
}

// ------------------------------------------------------------------ the arithmetic and the once-only model

void testOnce() {
    // H' = H + HUD + m * S, per channel, with the HUD at zero the star glow's alone and with m at zero the HUD's alone.
    bool exact = true, starAlone = true, hudAlone = true;
    for (int pct : {100, 125, 150, 200}) {
        const double m = flatStarGlowM(pct);
        for (float h : {0.0f, 0.37f, 12.0f, 2500.0f}) {
            for (float hud : {0.0f, 0.05f, 40.0f}) {
                for (float s : {0.0f, 1.0f, 164.9f}) {
                    const double want = static_cast<double>(h) + hud + m * s;
                    for (int c = 0; c < 3; ++c) {
                        const double got = flatStarFirstLevel(h, hud, s, m, c);
                        exact = exact && std::fabs(got - want) <= 1e-3 + want * 1e-6;
                        if (hud == 0.0f) starAlone = starAlone && std::fabs(got - (h + m * s)) <= 1e-3 + want * 1e-6;
                        if (m == 0.0 || s == 0.0f) hudAlone = hudAlone && std::fabs(got - (static_cast<double>(h) + hud)) <= 1e-3 + want * 1e-6;
                    }
                }
            }
        }
    }
    check(exact && starAlone && hudAlone, "vr hud glow: H' = H + HUD + m * S per channel; the HUD at zero is the star glow's H', m * S at zero is the HUD's alone");

    // The scene's bloom the re-issue must not tonemap again: the measured VR inputs of log 20261028_175126 (ps D0A16B9E, HDR source t1 2016x2017; t2 1008x1008 fmt 26).
    // That zeroing is what keeps the HUD layer's tone to the HUD's own body, so the halo H' sends through the chain is tonemapped once, by the scene's tone.
    ToneInputShape bloom;
    bloom.tex2d = true;
    bloom.gpuWritten = true;
    bloom.viewFormat = 26;
    bloom.w = 1008;
    bloom.h = 1008;
    check(toneInputIsSceneImage(2, 1, -1, bloom, 2016, 2017), "vr hud glow: the VR composite's bloom (t2 1008x1008 R11G11B10_FLOAT beside an HDR source of 2016x2017) is zeroed in the re-issue");
    check(!toneInputIsSceneImage(1, 1, -1, bloom, 2016, 2017), "vr hud glow: ...and the HDR source's own slot never is");
    ToneInputShape lut;
    lut.tex2d = false;
    lut.gpuWritten = false;
    lut.viewFormat = 28;
    lut.w = 32;
    lut.h = 32;
    check(!toneInputIsSceneImage(0, 1, -1, lut, 2016, 2017), "vr hud glow: ...nor the colour LUT");
}

// ------------------------------------------------------------------ the wiring

struct Pin {
    const char* id;
    bool ok;
    const char* what;
};

struct Sources {
    std::string uiLayer, uiLayerH, mod, modH, vrMath;
};

std::vector<Pin> wiringPins(const Sources& src) {
    std::vector<Pin> pins;
    const std::string ui = afterui::squeeze(src.uiLayer), mod = afterui::squeeze(src.mod), hdr = afterui::squeeze(src.modH);
    {
        std::string body;
        const bool found = afterui::functionBody(src.uiLayer, "ID3D11ShaderResourceView* uiLayerHeadsetGlowFeed(", &body);
        body = afterui::squeeze(body);
        pins.push_back({"identity-and-claim",
                        found && inOrder(body, {"facts[e].drawnFromH=c.hdrTarget==hdr;", "facts[e].fresh=haveSeq&&c.hdrSeq==seq&&c.hdrDraws&&c.hdrSrv;",
                                                "facts[e].toned=c.hdrToneSeq==c.hdrSeq;", "facts[e].claimed=c.hdrGlowSeq==c.hdrSeq;", "uiHeadsetHudPick(facts,&eye)",
                                                "hud->hdrGlowSeq=hud->hdrSeq;", "facts[eye].claimed=true;", "g_uiLayerHudGlowPending=uiHeadsetHudWaiting(facts);",
                                                "glowFeedPass(ctx,hud,slot,hdr,v,star)"}),
                        "an H is an eye's layer by the target its HUD draws were taken from, with this frame's content, not yet tonemapped and not yet taken; the layer is claimed before "
                        "the pass runs (a failed pass is not retried by a second first level), the hook is told whether another eye still waits, and the pass takes the layer"});
        pins.push_back({"feed-touches-no-tonemap",
                        found && !has(body, "crispBindStep") && !has(body, "crispFindSceneImages") && !has(body, "uiLayerCrispToneBegin") && !has(body, "g_crispSave") &&
                            has(body, "!runtimeFlatProfile()"),
                        "the headset feed builds H' and touches no part of the tonemap re-issue, and runs outside the flat profile only"});
    }
    {
        std::string body;
        const bool found = afterui::functionBody(src.uiLayer, "ID3D11ShaderResourceView* uiLayerGlowFeed(", &body);
        body = afterui::squeeze(body);
        pins.push_back({"flat-feed-still-flat", found && has(body, "||!runtimeFlatProfile())break;"),
                        "the flat HUD feed is still gated on the flat profile: the headset has its own entry, and the two never share a gate"});
    }
    pins.push_back({"take-wakes-the-hook",
                    inOrder(ui, {"e.hdrSeq=g_draw.seq;", "e.hdrTarget=g_draw.targetRes;", "++e.hdrEyeFrames;", "if(!runtimeFlatProfile())detail::g_uiLayerHudGlowPending=true;"}) &&
                        countOf(ui, "detail::g_uiLayerHudGlowPending=false;") == 1 && countOf(ui, "g_uiLayerHudGlowPending=true;") == 1,
                    "the first HUD draw an eye-frame takes wakes the hook (headset only), and the frame boundary puts it back to sleep"});
    {
        std::string body;
        const bool found = afterui::functionBody(src.mod, "void bindFirstLevel(ID3D11DeviceContext* ctx) {", &body);
        body = afterui::squeeze(body);
        pins.push_back({"hook-asks-once",
                        found && inOrder(body, {"constboolhudAsked=g_hudMode==VrHudGlowMode::kOn&&uiLayerHudGlowPending();", "if(!starIn&&!hudAsked)return;",
                                                "uiLayerHeadsetGlowFeed(ctx,0,info.resource,starIn,&glow);", "vrHudCount(g_hudWin,pick,glow.eye,hudFed,", "if(!canBind)return;",
                                                "vScreenPSSetShaderResourcesRaw(ctx,0,1,&bound);", "g_t0.active=true;", "vrHudGlowFedLine("}),
                        "the first level is looked at for the HUD only while the mode is on and an eye waits; one H' is built for the star light and the HUD together, counted, bound at t0 "
                        "through the raw setter for this draw, and said once"});
        pins.push_back({"h-prime-once",
                        found && countOf(mod, "vScreenPSSetShaderResourcesRaw(ctx,0,1,&bound);") == 1 && countOf(mod, "uiLayerHeadsetGlowFeed(") == 1 &&
                            !has(mod, "vScreenPSSetShaderResourcesRaw(ctx,1,") && !has(mod, "PSSetShaderResources(1,"),
                        "H' is bound in one place, at t0, for the first-level draw alone; the game's view of H comes back after it (the star pins hold endFirstLevel)"});
    }
    pins.push_back({"boundary-order",
                    inOrder(mod, {"voidvrStarGlowFrameBoundary(){", "if(runtimeFlatProfile()){hudStandDown();", "if(!g_budget.shouldRun()){hudStandDown();", "hudFrameBoundary();",
                                  "if(g_mode==VrStarMode::kOn)foldFrame();"}) &&
                        has(mod, "constVrHudGlowModemode=vrHudGlowMode(uiLayerCrispOn(),menuStarGlowBloom());") &&
                        has(mod, "detail::g_vrStarGlowWanted.store(g_mode==VrStarMode::kOn||g_hudMode==VrHudGlowMode::kOn,std::memory_order_relaxed);"),
                    "the HUD's mode is read once a frame from the HDR take and the Bloom reading, before the star glow's own boundary; the hooks stay armed while either runs; the flat "
                    "profile and a spent fault budget stand it down"});
    pins.push_back({"window-line",
                    inOrder(mod, {"voidhudLogWindow(){", "uiLayerHudEyeFrames(e)-g_hudEyeFramesBase[e]", "vrHudGlowWindowLine(line,sizeof(line),g_hudWin);"}),
                    "the window line sets the layer's eye-frames beside the first levels and feeds"});
    pins.push_back({"no-new-key", !has(mod, "getString(\"fix.hud") && !has(mod, "getString(\"visual.hud"),
                    "there is no key: a fix that always helps gets none (the mode follows the HDR take and Elite's Bloom)"});
    pins.push_back({"math-in-header", has(afterui::squeeze(src.vrMath), "inlineVrHudGlowModevrHudGlowMode(booltakeArmed,sgb::Bloombloom){"),
                    "the mode's rule is the header's, the one this rig drives"});
    return pins;
}

void testWiring() {
    Sources src;
    src.uiLayer = readText("src/d3d11/ui_layer.cpp");
    src.uiLayerH = readText("src/d3d11/ui_layer.h");
    src.mod = readText("src/d3d11/vr_star_glow.cpp");
    src.modH = readText("src/d3d11/vr_star_glow.h");
    src.vrMath = readText("src/d3d11/vr_hud_glow_math.h");
    check(!src.uiLayer.empty() && !src.uiLayerH.empty() && !src.mod.empty() && !src.modH.empty() && !src.vrMath.empty(),
          "vr hud glow wiring: ui_layer.cpp and .h, vr_star_glow.cpp and .h and vr_hud_glow_math.h are readable from the working directory");
    for (const Pin& p : wiringPins(src)) {
        char msg[480];
        std::snprintf(msg, sizeof(msg), "vr hud glow wiring [%s]: %s", p.id, p.what);
        check(p.ok, msg);
    }
    // Controls: each edit puts a likely slip back and must trip the pin it names.
    struct Control {
        const char* id;
        int file;   // 0 ui_layer.cpp, 1 ui_layer.h, 2 vr_star_glow.cpp, 3 vr_star_glow.h, 4 vr_hud_glow_math.h
        const char* from;
        const char* to;
    };
    const Control controls[] = {
        {"identity-and-claim", 0, "facts[e].drawnFromH = c.hdrTarget == hdr;", "facts[e].drawnFromH = e == 0;"},
        {"identity-and-claim", 0, "facts[e].toned = c.hdrToneSeq == c.hdrSeq;", "facts[e].toned = false;"},
        {"identity-and-claim", 0, "            hud->hdrGlowSeq = hud->hdrSeq;\n", ""},
        {"identity-and-claim", 0, "            detail::g_uiLayerHudGlowPending = uiHeadsetHudWaiting(facts);\n", ""},
        {"feed-touches-no-tonemap", 0, "        pick = uiHeadsetHudPick(facts, &eye);\n", "        pick = uiHeadsetHudPick(facts, &eye);\n        crispBindStep(ctx, 0);\n"},
        {"feed-touches-no-tonemap", 0, "slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT && !runtimeFlatProfile()) {\n        uint64_t seq = 0;",
         "slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {\n        uint64_t seq = 0;"},
        {"flat-feed-still-flat", 0, "slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT || !runtimeFlatProfile()) break;", "slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) break;"},
        {"take-wakes-the-hook", 0, "        if (!runtimeFlatProfile()) detail::g_uiLayerHudGlowPending = true;\n", ""},
        {"take-wakes-the-hook", 0, "    detail::g_uiLayerHudGlowPending = false;\n    // ...nor does the VR world route's pending re-issue", "    // ...nor does the VR world route's pending re-issue"},
        {"hook-asks-once", 2, "    const bool hudAsked = g_hudMode == VrHudGlowMode::kOn && uiLayerHudGlowPending();", "    const bool hudAsked = g_hudMode == VrHudGlowMode::kOn;"},
        {"hook-asks-once", 2, "    if (!canBind) return;\n", ""},
        {"hook-asks-once", 2, "vrHudCount(g_hudWin, pick, glow.eye, hudFed, ", "vrHudCount(g_hudWin, pick, glow.eye, true, "},
        {"h-prime-once", 2, "    vScreenPSSetShaderResourcesRaw(ctx, 0, 1, &bound);\n", "    vScreenPSSetShaderResourcesRaw(ctx, 0, 1, &bound);\n    vScreenPSSetShaderResourcesRaw(ctx, 1, 1, &bound);\n"},
        {"boundary-order", 2, "    hudFrameBoundary();\n    if (g_mode == VrStarMode::kOn) foldFrame();", "    if (g_mode == VrStarMode::kOn) foldFrame();\n    hudFrameBoundary();"},
        {"boundary-order", 2, "vrHudGlowMode(uiLayerCrispOn(), menuStarGlowBloom())", "vrHudGlowMode(true, menuStarGlowBloom())"},
        {"boundary-order", 2, "g_vrStarGlowWanted.store(g_mode == VrStarMode::kOn || g_hudMode == VrHudGlowMode::kOn, std::memory_order_relaxed);",
         "g_vrStarGlowWanted.store(g_mode == VrStarMode::kOn, std::memory_order_relaxed);"},
        {"window-line", 2, "uiLayerHudEyeFrames(e) - g_hudEyeFramesBase[e];", "g_hudEyeFramesBase[e];"},
        {"no-new-key", 2, "    ++g_boundaries;\n", "    ++g_boundaries;\n    Config::get().getString(\"fix.hud_glow\", \"on\");\n"},
    };
    for (const Control& c : controls) {
        Sources edited = src;
        std::string* files[5] = {&edited.uiLayer, &edited.uiLayerH, &edited.mod, &edited.modH, &edited.vrMath};
        std::string& f = *files[c.file];
        const size_t at = f.find(c.from);
        char msg[240];
        std::snprintf(msg, sizeof(msg), "vr hud glow wiring control [%s]: the edited text is present in the source", c.id);
        check(at != std::string::npos, msg);
        if (at == std::string::npos) continue;
        f.replace(at, std::strlen(c.from), c.to);
        bool tripped = false;
        for (const Pin& p : wiringPins(edited))
            if (std::strcmp(p.id, c.id) == 0 && !p.ok) tripped = true;
        std::snprintf(msg, sizeof(msg), "vr hud glow wiring control [%s]: the edit trips the pin it names", c.id);
        check(tripped, msg);
    }
}

void testAll() {
    testMode();
    testPick();
    testFrames();
    testWindow();
    testOnce();
    testWiring();
}

}  // namespace vrhudglowtest

// The GPU pass profiler's pure logic (src\d3d11\gpu_pass_profile_core.h): classification, cube runs, the stamp budget, the
// shader tally, the summary arithmetic and the CSV text. No D3D11: the device path is exercised in the DLL's own run.
// --self-test exits nonzero on the first failed check.
#include "../../src/d3d11/gpu_pass_profile_core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace edvr::gpupass;

namespace {
unsigned g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

TargetShape shape(uint32_t w, uint32_t h, bool cube = false, uint32_t slices = 1) {
    TargetShape t;
    t.w = w;
    t.h = h;
    t.cube = cube;
    t.slices = slices;
    return t;
}

// Commas outside quotes: a CSV row must have exactly the header's field count.
size_t fieldCount(const std::string& line) {
    size_t n = 1;
    bool quoted = false;
    for (char c : line) {
        if (c == '"') quoted = !quoted;
        else if (c == ',' && !quoted) ++n;
    }
    return n;
}

void testClassify() {
    const EyeSize eye{2016, 1949};
    PassFacts f;
    f.edvr = true;
    f.draws = 5;
    check(classifyPass(f, eye) == PassClass::Edvr, "an EDVR segment is edvr, whatever it drew");

    PassFacts c;
    c.dispatches = 3;
    check(classifyPass(c, eye) == PassClass::Compute, "dispatches without draws are compute");

    PassFacts k;
    k.clears = 1;
    check(classifyPass(k, eye) == PassClass::Clear, "a clear with no draws is clear");
    PassFacts e;
    check(classifyPass(e, eye) == PassClass::Other, "a segment with no work is other");

    PassFacts shadow;
    shadow.draws = 40;
    shadow.hasDepth = true;
    shadow.depth = shape(2048, 2048);
    check(classifyPass(shadow, eye) == PassClass::Shadow, "depth-only square 2048 is a shadow map");
    shadow.depth = shape(256, 256);
    check(classifyPass(shadow, eye) == PassClass::Shadow, "a small square depth-only map off the eye size is a shadow (v13.7: 256x256 D16)");
    shadow.depth = shape(2048, 2048, false, 4);
    check(classifyPass(shadow, eye) == PassClass::Shadow, "an array of depth slices is a shadow (cascades)");

    PassFacts prepass;
    prepass.draws = 300;
    prepass.hasDepth = true;
    prepass.depth = shape(2016, 1949);
    check(classifyPass(prepass, eye) == PassClass::SceneGBuffer, "a depth prepass at the eye size is scene geometry");

    PassFacts face;
    face.draws = 10;
    face.colours = 1;
    face.colour = shape(512, 512, true, 1);
    check(classifyPass(face, eye) == PassClass::ProbeCube, "a square target of a cube resource is a probe");

    PassFacts gbuf;
    gbuf.draws = 800;
    gbuf.colours = 3;
    gbuf.colour = shape(2016, 1949);
    check(classifyPass(gbuf, eye) == PassClass::SceneGBuffer, "three colour targets at the eye size are the g-buffer");

    PassFacts full;
    full.draws = 2;
    full.colours = 1;
    full.colour = shape(2016, 1949);
    check(classifyPass(full, eye) == PassClass::FullscreenAtEye, "one or two draws at the eye size are full-screen");

    PassFacts fwd;
    fwd.draws = 90;
    fwd.colours = 1;
    fwd.colour = shape(2016, 1949);
    check(classifyPass(fwd, eye) == PassClass::ForwardAtEye, "many draws into one eye-sized target are forward");

    PassFacts post;
    post.draws = 3;
    post.colours = 1;
    post.colour = shape(1008, 974);
    check(classifyPass(post, eye) == PassClass::PostSmaller, "draws into a smaller target are post");

    PassFacts big;
    big.draws = 3;
    big.colours = 1;
    big.colour = shape(4000, 4000);
    check(classifyPass(big, eye) == PassClass::Other, "a larger square target that is not a cube is other");

    PassFacts noEye;
    noEye.draws = 3;
    noEye.colours = 1;
    noEye.colour = shape(1008, 974);
    check(classifyPass(noEye, EyeSize{}) == PassClass::Other, "with no eye size known, nothing is post or eye");
}

void testCubeRuns() {
    auto square = [](uint32_t s) {
        PassFacts f;
        f.draws = 4;
        f.colours = 1;
        f.colour = shape(s, s);
        return f;
    };
    const EyeSize eye{2016, 1949};
    std::vector<PassFacts> six(6, square(256));
    std::vector<PassClass> cls;
    for (const PassFacts& f : six) cls.push_back(classifyPass(f, eye));
    markCubeRuns(six, cls);
    bool allCube = true;
    for (PassClass c : cls) allCube = allCube && c == PassClass::ProbeCube;
    check(allCube, "six consecutive same-size square passes read as one cube map");

    std::vector<PassFacts> five(5, square(256));
    cls.assign(5, PassClass::Other);
    markCubeRuns(five, cls);
    bool anyCube = false;
    for (PassClass c : cls) anyCube = anyCube || c == PassClass::ProbeCube;
    check(!anyCube, "five in a row is not a cube map");

    std::vector<PassFacts> broken;
    for (int i = 0; i < 3; ++i) broken.push_back(square(256));
    broken.push_back(square(128));   // a different size ends the run
    for (int i = 0; i < 6; ++i) broken.push_back(square(256));
    cls.assign(broken.size(), PassClass::Other);
    markCubeRuns(broken, cls);
    check(cls[0] == PassClass::Other && cls[3] == PassClass::Other, "a size change breaks the run");
    check(cls[4] == PassClass::ProbeCube && cls[9] == PassClass::ProbeCube, "a later run of six is found");

    PassFacts edvrFace = square(256);
    edvrFace.edvr = true;
    std::vector<PassFacts> withEdvr(6, square(256));
    withEdvr[2] = edvrFace;
    cls.assign(6, PassClass::Other);
    markCubeRuns(withEdvr, cls);
    check(cls[0] == PassClass::Other, "an EDVR segment breaks a cube run");
}

void testStampBudget() {
    StampBudget b;
    uint32_t boundaries = 0;
    while (b.take(1)) ++boundaries;
    check(boundaries == kMaxStampsPerFrame - 1, "a boundary leaves one stamp free for the frame's end");
    check(b.overflow == 1, "the first refused boundary is counted");
    check(b.take(0), "the frame's end stamp fits in the reserved slot");
    check(b.used == kMaxStampsPerFrame, "the frame holds exactly its cap");
    check(!b.take(0) && b.overflow == 2, "past the cap nothing is issued and everything is counted");
}

void testHashes() {
    PassHashes h;
    for (uint64_t i = 1; i <= 10; ++i) h.note(i, 100 + i);
    check(h.folded() == 2, "pairs past the eight slots fold into one count");
    for (int i = 0; i < 4; ++i) h.note(3, 103);
    const std::vector<HashTally> top = h.top();
    check(top.size() == kTopHashes, "the top list is capped at three");
    check(top[0].vs == 3 && top[0].draws == 5, "the most-drawn pair leads, with its count");
    check(top[1].draws == 1, "the rest follow in draw order");
    check(topText(std::vector<HashTally>{}) == "-", "a pass that drew nothing reads '-'");
    check(topText(std::vector<HashTally>{HashTally{1, 2, 5}}) ==
              "0000000000000001:0000000000000002:5",
          "a pair is written as vs:ps:draws in hex");
}

void testCsv() {
    check(csvField("plain") == "plain", "a plain field is not quoted");
    check(csvField("1,2") == "\"1,2\"", "a field with a comma is quoted");
    check(csvField("say \"hi\"") == "\"say \"\"hi\"\"\"", "quotes inside a field are doubled");

    Row r;
    r.frame = 2;
    r.index = 7;
    r.cls = PassClass::ForwardAtEye;
    r.ms = 1.23456;
    r.draws = 90;
    r.colours = 1;
    r.rt = "2016x1949";
    r.dsv = "2016x1949";
    r.top = "a:b:1;c:d:2";
    const std::string line = csvRow(r);
    check(fieldCount(line) == fieldCount(csvHeader()), "a row has as many fields as the header");
    check(line.rfind("2,7,forward_at_eye,0,1.2346,90,", 0) == 0, "the row starts with frame, pass, class, edvr, ms");
    Row odd;
    odd.rt = "a,b";
    check(fieldCount(csvRow(odd)) == fieldCount(csvHeader()), "a comma in a target name is quoted, not a new field");
}

void testSummary() {
    // Frame 0 and frame 1 read; frame 2 not read, and its rows must not count.
    std::vector<Row> rows;
    auto add = [&](uint32_t frame, PassClass c, bool edvr, double ms, const char* rt) {
        Row r;
        r.frame = frame;
        r.cls = c;
        r.edvr = edvr;
        r.ms = ms;
        r.draws = 10;
        r.rt = rt;
        rows.push_back(r);
    };
    add(0, PassClass::Shadow, false, 6.0, "2048x2048");
    add(0, PassClass::ForwardAtEye, false, 2.0, "2016x1949");
    add(0, PassClass::Edvr, true, 2.0, "-");
    add(1, PassClass::Shadow, false, 10.0, "2048x2048");
    add(1, PassClass::ForwardAtEye, false, 6.0, "2016x1949");
    add(1, PassClass::Edvr, true, 4.0, "-");
    add(2, PassClass::Shadow, false, 500.0, "2048x2048");

    const std::vector<double> frameMs{10.0, 20.0, 999.0};
    const std::vector<bool> ok{true, true, false};
    const Summary s = summarise(rows, frameMs, ok);
    check(s.frames == 2, "only the frames that were read are counted");
    check(s.frameMs == 15.0, "the frame mean is over the read frames");
    check(s.eliteMs == 12.0, "elite is the mean of the non-EDVR rows per frame");
    check(s.edvrMs == 3.0, "edvr is the mean of the EDVR rows per frame");
    check(s.cls[static_cast<size_t>(PassClass::Shadow)].ms == 8.0, "a class mean is per frame");
    check(s.cls[static_cast<size_t>(PassClass::Shadow)].passes == 1.0, "a class pass count is per frame");
    check(!s.top.empty() && s.top[0].cls == PassClass::Shadow && s.top[0].ms == 8.0,
          "the top pass is the largest group by median (shadow 8 ms over the two frames), with its per-frame mean");
    check(s.top.size() == 3, "groups are the distinct class and target signatures");

    const Summary none = summarise(rows, frameMs, std::vector<bool>{false, false, false});
    check(none.frames == 0 && none.top.empty(), "with no frame read there are no means at all");
}

// Built v13.7: the HUD class, the depth-only shadow rule, the compute column and the medians.
void testV137() {
    const EyeSize eye{2016, 1949};
    const uint64_t kHudVs = 0xB7790CBFC6554097ull, kOtherVs = 0x1111111111111111ull;
    VsFamily fam;
    fam.vs[0] = kHudVs;
    fam.n = 1;
    PassFacts hud;
    hud.colours = 1;
    hud.colour = shape(eye.w, eye.h);
    hud.draws = 2;
    hud.topVs = kHudVs;
    check(classifyPass(hud, eye, fam) == PassClass::HudOverlay, "two draws of a known HUD VS at the eye size are hud_overlay, not fullscreen_at_eye");
    check(classifyPass(hud, eye) == PassClass::FullscreenAtEye, "with no families given, the same pass is fullscreen_at_eye (the families are the caller's)");
    PassFacts many = hud;
    many.draws = kHudMaxDraws + 1;
    check(classifyPass(many, eye, fam) == PassClass::ForwardAtEye, "more draws than kHudMaxDraws is not a HUD overlay");
    PassFacts notHud = hud;
    notHud.topVs = kOtherVs;
    check(classifyPass(notHud, eye, fam) == PassClass::FullscreenAtEye, "a pass whose top VS is not a known family is not a HUD overlay");
    PassFacts unknownTop = hud;
    unknownTop.topVs = 0;
    check(classifyPass(unknownTop, eye, fam) == PassClass::FullscreenAtEye, "a pass with no drawn VS is not a HUD overlay");
    check(std::string(passClassName(PassClass::HudOverlay)) == "hud_overlay", "the class is named hud_overlay in the CSV and the summary");

    // The depth-only rule: a map off the eye size is a shadow (non-square 2048x1024 D16, small 256x256 D16); a map at the eye size is a G-buffer.
    PassFacts d;
    d.draws = 40;
    d.hasDepth = true;
    d.depth = shape(2048, 1024);
    check(classifyPass(d, eye) == PassClass::Shadow, "the non-square 2048x1024 depth-only pair is a shadow map");
    d.depth = shape(256, 256);
    check(classifyPass(d, eye) == PassClass::Shadow, "the 256x256 depth-only map is a shadow map");
    d.depth = shape(eye.w, eye.h);
    check(classifyPass(d, eye) == PassClass::SceneGBuffer, "a depth-only map at the eye size is still scene_gbuffer");
    PassFacts dn;
    dn.draws = 4;
    dn.hasDepth = true;
    dn.depth = shape(256, 256);
    check(classifyPass(dn, EyeSize{}) == PassClass::Other, "with no eye size known, a small square depth-only map is not claimed as a shadow");

    // The compute column: the bound compute shaders by hash, named from the catalogue when known.
    const std::vector<HashTally> cs = {HashTally{0x6FE04AF836BB1DBAull, 0, 3}, HashTally{0x0123456789ABCDEFull, 0, 1}};
    ComputeName names[] = {{0x6FE04AF836BB1DBAull, "APPLY_BIND_POSE_TRANSFORMS_CS"}};
    const std::string text = csText(cs, names, 1);
    check(text == "6fe04af836bb1dba(APPLY_BIND_POSE_TRANSFORMS_CS):3;0123456789abcdef:1",
          "compute shaders print as hash(NAME):count, most dispatches first, the unknown ones as hex only");
    check(csText({}, names, 1) == "-", "a pass with no dispatch prints a dash");
    check(std::string(csvHeader()).find("top_cs_dispatches") != std::string::npos,
          "the CSV header ends with the top_cs_dispatches column");
    Row rc;
    rc.topCs = "a(b):1";
    check(fieldCount(csvRow(rc)) == fieldCount(csvHeader()), "a row with a compute column still has the header's field count");

    // The medians: one spike moves the mean and not the median.
    std::vector<Row> rows;
    std::vector<double> frameMs = {10.0, 10.0, 10.0};
    std::vector<bool> ok = {true, true, true};
    for (uint32_t f = 0; f < 3; ++f) {
        Row r;
        r.frame = f;
        r.cls = PassClass::Shadow;
        r.ms = f == 2 ? 100.0 : 1.0;
        r.draws = 10;
        r.rt = "-";
        r.dsv = "2048x1024";
        rows.push_back(r);
    }
    const Summary s = summarise(rows, frameMs, ok);
    check(s.cls[static_cast<size_t>(PassClass::Shadow)].median == 1.0, "the class median of one pass per frame is the middle value (1 ms), not the 34 ms mean");
    check(s.cls[static_cast<size_t>(PassClass::Shadow)].ms > 30.0, "the class mean still carries the spike");
    check(!s.top.empty() && s.top[0].median == 1.0, "the group median is printed beside its mean");
    check(medianOf({1.0, 2.0, 3.0, 4.0}) == 2.5 && medianOf({5.0}) == 5.0 && medianOf({}) == 0.0, "the median of an even, an odd and an empty list");
}

// GPU pass profile v2: the lighting class, the shader identity and CS attribution, the median top list and CS totals, the pool sizing,
// and the frame count read from the hotkey's value.
void testV2() {
    const EyeSize eye{2016, 1949};
    const uint64_t kLightVs = 0x0357BBB2DEE43C1Full, kHudVs = 0xB7790CBFC6554097ull;
    VsFamily hud, lighting;
    hud.vs[0] = kHudVs;
    hud.n = 1;
    lighting.vs[0] = kLightVs;
    lighting.vs[1] = 0x8289669D93A18C1Dull;
    lighting.n = 2;

    // Lighting: instanced volumes into the eye-size target, two targets (the lighting pass has colours=2).
    PassFacts light;
    light.colours = 2;
    light.colour = shape(eye.w, eye.h);
    light.hasDepth = true;
    light.depth = shape(eye.w, eye.h);
    light.draws = 9;
    light.topVs = kLightVs;
    check(classifyPass(light, eye, hud, lighting) == PassClass::LightingVolume,
          "a colours=2 pass at the eye size whose most-drawn VS is a light volume is lighting_volume");
    check(classifyPass(light, eye, hud) == PassClass::SceneGBuffer,
          "with no lighting family, the same pass is scene_gbuffer (the families are the caller's)");
    PassFacts lightSmall = light;
    lightSmall.colour = shape(1008, 975);
    check(classifyPass(lightSmall, eye, hud, lighting) != PassClass::LightingVolume,
          "a light-volume VS drawn into a smaller target is not the eye-size lighting pass");
    PassFacts lightOther = light;
    lightOther.topVs = 0x1111111111111111ull;
    check(classifyPass(lightOther, eye, hud, lighting) == PassClass::SceneGBuffer,
          "a pass whose top VS is not a light volume stays scene_gbuffer");
    check(std::string(passClassName(PassClass::LightingVolume)) == "lighting_volume", "the class is named lighting_volume");

    // Shader identity: a draw pass names its most-drawn VS:PS, a dispatch-only pass its compute shader (named when known).
    PassHashes draws;
    draws.note(kLightVs, 0xAAull);
    draws.note(kLightVs, 0xAAull);
    draws.note(kHudVs, 0xBBull);
    ComputeName names[] = {{0x6FE04AF836BB1DBAull, "APPLY_BIND_POSE_TRANSFORMS_CS"}};
    check(identText(draws.top(), {}, names, 1) == "0357bbb2dee43c1f:00000000000000aa",
          "a draw pass is identified by its most-drawn VS:PS pair");
    PassHashes cs;
    cs.note(0x6FE04AF836BB1DBAull, 0);
    cs.note(0x6FE04AF836BB1DBAull, 0);
    cs.note(0x0123456789ABCDEFull, 0);
    check(identText({}, cs.top(), names, 1) == "cs:6fe04af836bb1dba(APPLY_BIND_POSE_TRANSFORMS_CS)",
          "a dispatch-only pass is identified by its most-dispatched compute shader, named when known");
    check(cs.all().size() == 2 && cs.all()[0].draws + cs.all()[1].draws == 3, "all() returns every tracked compute shader with its count");
    PassHashes unhashed;
    unhashed.note(0, 0);   // a dispatch the registry never met: hash 0
    check(identText({}, unhashed.top(), names, 1) == "-", "a zero compute hash is not named: the identity is a dash");
    check(identText({}, {}, names, 1) == "-", "a pass with neither draws nor dispatches has the dash identity");

    // The CS totals: every dispatch counts, not only each pass's top three; the mean is per captured frame.
    std::vector<Row> rows;
    std::vector<double> frameMs = {10.0, 10.0};
    std::vector<bool> ok = {true, true};
    for (uint32_t f = 0; f < 2; ++f) {
        for (int p = 0; p < 2; ++p) {
            Row r;
            r.frame = f;
            r.cls = PassClass::Compute;
            r.ms = 0.5;
            r.dispatches = 1;
            r.ident = "cs:6fe04af836bb1dba";
            r.cs = {HashTally{0x6FE04AF836BB1DBAull, 0, 1}};
            rows.push_back(r);
        }
    }
    const Summary sc = summarise(rows, frameMs, ok);
    check(!sc.cs.empty() && sc.cs[0].hash == 0x6FE04AF836BB1DBAull && sc.cs[0].perFrame == 2.0,
          "the CS total is dispatches per captured frame (2 a frame here)");

    // Median top list: a steady pass outranks a spiky one with a larger mean; the list holds kTopPasses groups.
    std::vector<Row> spiky;
    std::vector<double> fms = {10.0, 10.0, 10.0};
    std::vector<bool> fok = {true, true, true};
    const double spikes[3] = {1.0, 1.0, 100.0};   // median 1, mean 34
    for (uint32_t f = 0; f < 3; ++f) {
        Row r;
        r.frame = f;
        r.cls = PassClass::ForwardAtEye;
        r.ms = spikes[f];
        r.rt = "2016x1949";
        r.colours = 1;
        spiky.push_back(r);
        Row s2;
        s2.frame = f;
        s2.cls = PassClass::PostSmaller;
        s2.ms = 3.0;   // median 3, mean 3
        s2.rt = "1008x975";
        s2.colours = 1;
        spiky.push_back(s2);
    }
    const Summary sm = summarise(spiky, fms, fok);
    check(sm.top.size() == 2 && sm.top[0].cls == PassClass::PostSmaller && sm.top[0].median == 3.0,
          "the top list is by median: the steady 3 ms pass ranks above the 34 ms-mean spiky pass (median 1 ms)");
    check(sm.top[1].median == 1.0 && sm.top[1].ms > 30.0, "the spiky pass is second, with its mean still reported");
    std::vector<Row> many;
    for (uint32_t i = 0; i < kTopPasses + 5; ++i) {
        Row r;
        r.frame = 0;
        r.cls = PassClass::Other;
        r.ms = 1.0 + i;
        r.rt = "w" + std::to_string(i);   // a distinct signature per pass
        many.push_back(r);
    }
    const Summary sMany = summarise(many, std::vector<double>{1.0}, std::vector<bool>{true});
    check(sMany.top.size() == kTopPasses && kTopPasses == 20, "the top list holds 20 passes");
    check(sMany.top[0].median > sMany.top[1].median, "the top list is in descending median order");

    // The pool: sized to the captured frames, capped at kMaxCaptureFrames, grown when a larger capture asks, reused when not.
    check(poolFramesFor(0) == 1 && poolFramesFor(30) == 30 && poolFramesFor(500) == kMaxCaptureFrames,
          "the pool holds the capture's frames: at least 1, at most 120");
    check(poolNeedsGrowth(0, 30), "no pool yet: a capture of 30 frames needs one");
    check(!poolNeedsGrowth(30, 30) && !poolNeedsGrowth(40, 10), "a pool at least as large as the capture is reused");
    check(poolNeedsGrowth(10, 30), "a pool of 10 frames is replaced by a capture of 30");
    check(kMaxStampsPerFrame == 4096 && kDefaultCaptureFrames == 30 && kMaxCaptureFrames == 120,
          "the defaults: 30 frames a press, 120 at most, 4096 stamps a frame");

    // The hotkey's value: "KEY" or "KEY COUNT"; the count is split off, clamped, and a bare digit key is not taken as a count.
    std::string key;
    check(splitCaptureSpec("F9", key) == 30 && key == "F9", "a bare key: the default 30 frames");
    check(splitCaptureSpec("F9 60", key) == 60 && key == "F9", "KEY COUNT: the count is taken, the key is the rest");
    check(splitCaptureSpec("Ctrl+F9  500", key) == 120 && key == "Ctrl+F9", "a count above 120 is capped at 120; a double space is allowed");
    check(splitCaptureSpec("F9 0", key) == 1, "a count below 1 is 1");
    check(splitCaptureSpec("1", key) == 30 && key == "1", "a bare digit key is the key, not a count");
    check(splitCaptureSpec("Ctrl+1 12", key) == 12 && key == "Ctrl+1", "a digit key with a count is split correctly");
    check(splitCaptureSpec("F9 abc", key) == 30 && key == "F9 abc", "a non-numeric tail is not a count (the hotkey then binds nothing and says so)");
    check(splitCaptureSpec("", key) == 30 && key.empty(), "an empty value: the default count and no key (the hotkey is unbound)");
}

// GPU pass profile v3: probe_cube is a small square or a cube array, never a target at the eye size; the eye size is the eye texture.
void testV3Classes() {
    const EyeSize eye{2016, 2016};   // the square eye render of the v2 flight
    // An eye pass with depth at the eye size: scene or fullscreen or forward, never probe_cube (v2 labelled 28-draw ones probe_cube).
    PassFacts forward;
    forward.draws = 28;
    forward.colours = 1;
    forward.colour = shape(2016, 2016);
    forward.hasDepth = true;
    forward.depth = shape(2016, 2016);
    check(classifyPass(forward, eye) == PassClass::ForwardAtEye, "a 2016x2016 eye pass with depth and 28 draws is forward_at_eye, not probe_cube");
    PassFacts fullscreen = forward;
    fullscreen.draws = 2;
    check(classifyPass(fullscreen, eye) == PassClass::FullscreenAtEye, "a 2016x2016 eye pass with 2 draws is fullscreen_at_eye, not probe_cube");
    PassFacts gbuf = forward;
    gbuf.colours = 2;
    check(classifyPass(gbuf, eye) == PassClass::SceneGBuffer, "a 2016x2016 two-target eye pass is scene_gbuffer, not probe_cube");

    // Six of them in a row (the v2 cube-run rule's trigger): still not probe_cube with the eye size known.
    std::vector<PassFacts> eyeRun(6, forward);
    std::vector<PassClass> cls;
    for (const PassFacts& f : eyeRun) cls.push_back(classifyPass(f, eye));
    markCubeRuns(eyeRun, cls, eye);
    bool anyProbe = false;
    for (PassClass c : cls) anyProbe = anyProbe || c == PassClass::ProbeCube;
    check(!anyProbe, "six eye-size passes in a row are not a cube map");

    // probe_cube's shape: a small square (up to 1024) or a cube array (6+ slices). A larger square that is not a cube array is not one.
    PassFacts small;
    small.draws = 10;
    small.colours = 1;
    small.colour = shape(1024, 1024);
    check(classifyPass(small, eye) == PassClass::ProbeCube, "a 1024x1024 square target is a probe");
    PassFacts big;
    big.draws = 10;
    big.colours = 1;
    big.colour = shape(1566, 1566);
    check(classifyPass(big, eye) != PassClass::ProbeCube, "a 1566x1566 square target is not a probe (squareness is not the rule)");
    PassFacts array;
    array.draws = 10;
    array.colours = 1;
    array.colour = shape(2048, 2048, true, 6);
    check(classifyPass(array, eye) == PassClass::ProbeCube, "a cube array (6 slices) is a probe at any size");
    check(!isProbeShape(shape(2016, 2016), eye), "a target at the eye size is never a probe shape");
    check(isProbeShape(shape(2016, 2016), EyeSize{}) == false, "a 2016 square with no eye size known is not a probe either (over 1024)");

    // The matched eye: the largest colour target whose depth is the same size, when there is one.
    std::vector<PassFacts> frame = {forward, fullscreen};
    frame[1].hasDepth = false;
    check(matchedEye(frame).w == 2016 && matchedEye(frame).h == 2016, "the matched eye is the eye-size target with its depth");
    std::vector<PassFacts> noDepth = {fullscreen};
    noDepth[0].hasDepth = false;
    check(matchedEye(noDepth).w == 0, "with no matching depth the matched eye is unknown (0x0)");
}

// GPU pass profile v5: the hotkey's "KEY COUNT DELAY_MS" value, the calibrated iteration count, the segment totals with the delay excluded
// from busy time, the no-work gap, and the suspect flag.
void testV5Delay() {
    CaptureSpec s = parseCaptureSpec("F9");
    check(s.key == "F9" && s.frames == 30 && s.delayMs == 6, "F9 alone: 30 frames and the default 6 ms delay");
    s = parseCaptureSpec("F9 60");
    check(s.key == "F9" && s.frames == 60 && s.delayMs == 6, "F9 60: 60 frames, the default delay");
    s = parseCaptureSpec("F9 60 8");
    check(s.key == "F9" && s.frames == 60 && s.delayMs == 8, "F9 60 8: 60 frames and an 8 ms delay");
    s = parseCaptureSpec("Ctrl+F9 500 20");
    check(s.key == "Ctrl+F9" && s.frames == 120 && s.delayMs == 12, "the frame count caps at 120 and the delay at 12");
    s = parseCaptureSpec("F9 60 1");
    check(s.delayMs == 2, "a delay below 2 ms is raised to 2");
    s = parseCaptureSpec("1 2 3");
    check(s.key == "1" && s.frames == 2 && s.delayMs == 3, "a digit key still binds, with a count and a delay after it");
    s = parseCaptureSpec("1");
    check(s.key == "1" && s.frames == 30 && s.delayMs == 6, "a bare digit is the key, not a count");
    s = parseCaptureSpec("F9 abc 8");
    check(s.key == "F9 abc" && s.frames == 8 && s.delayMs == 6, "a non-number between the key and the count is part of the key");
    s = parseCaptureSpec("");
    check(s.key.empty() && s.frames == 30 && s.delayMs == 6, "an empty value is unbound with the defaults");

    check(delayIterations(0.005, 6) == 1200 && delayIterations(0.0, 6) == 0 && delayIterations(1.0, 12) == 12,
          "the iterations for a delay: delay over the measured cost per iteration; 0 when the cost is unknown");
    check(kDefaultDelayMs == 6 && kMinDelayMs == 2 && kMaxDelayMs == 12, "the delay's default and clamp");

    // Segments: the delay is neither work nor gap; busy is the rest (work plus gaps); gap is the empty time alone.
    const SegTotals t = segmentTotals({1.0, 2.0, 3.0, 0.5}, {SegKind::Work, SegKind::Delay, SegKind::Work, SegKind::Empty});
    check(t.delay == 2.0 && t.busy == 4.5 && t.gap == 0.5, "the delay is excluded from busy time; the empty segment is the gap, and it is in busy");

    // The suspect flag: over 3x the group's own median AND followed by a gap over kLargeGapMs.
    std::vector<Row> rows;
    for (int i = 0; i < 10; ++i) {
        Row r;
        r.ms = 1.0;
        rows.push_back(r);
    }
    Row a;
    a.ms = 5.0;
    a.gapAfterMs = 0.5;
    rows.push_back(a);
    Row b;
    b.ms = 2.5;
    b.gapAfterMs = 0.5;
    rows.push_back(b);
    Row c;
    c.ms = 5.0;
    c.gapAfterMs = 0.1;
    rows.push_back(c);
    const size_t flagged = markSuspects(rows);
    check(flagged == 1 && rows[10].suspect && !rows[11].suspect && !rows[12].suspect,
          "a pass over 3x its median and followed by a large gap is suspect; 2.5x, or no large gap after, is not");
    check(std::string(csvHeader()).find("gap_after_ms,suspect") != std::string::npos, "the CSV header carries the gap and suspect columns");
}

// GPU pass profile v4: the exposure chain's 252x252 passes and its compute dispatches are exposure, never probe_cube; probe_cube's square
// lower bound is 256.
void testV4Exposure() {
    const EyeSize eye{2016, 2016};
    VsFamily exposure;
    exposure.vs[0] = 0xB8E727E94D44D891ull;
    exposure.vs[1] = 0xF1FB2EEFB662F3AAull;
    exposure.n = 2;
    PassFacts post;
    post.draws = 1;
    post.colours = 1;
    post.colour = shape(252, 252);
    post.topVs = 0xdef19b035d5ededcull;
    check(classifyPass(post, eye, VsFamily{}, VsFamily{}, exposure) == PassClass::Exposure,
          "a 252x252 one-draw pass is exposure, not probe_cube");
    check(classifyPass(post, eye) == PassClass::Exposure, "the 252x252 pass is exposure without the families too");
    check(!isProbeShape(shape(252, 252), eye), "252 is below probe_cube's square lower bound");
    PassFacts p256 = post;
    p256.colour = shape(256, 256);
    check(classifyPass(p256, eye) == PassClass::ProbeCube, "a 256x256 one-draw pass is still a probe (the bound is inclusive)");
    check(isProbeShape(shape(256, 256), eye) && isProbeShape(shape(1024, 1024), eye) && !isProbeShape(shape(255, 255), eye),
          "probe_cube's square is 256 to 1024 on a side");
    check(isProbeShape(shape(2048, 2048, true, 6), eye), "a cube array of 6 slices is a probe at any size");
    PassFacts disp;
    disp.dispatches = 3;
    disp.topCs = 0xB8E727E94D44D891ull;
    check(classifyPass(disp, eye, VsFamily{}, VsFamily{}, exposure) == PassClass::Exposure,
          "a dispatch-only pass whose top compute shader is the exposure chain's is exposure");
    PassFacts other = disp;
    other.topCs = 0x6FE04AF836BB1DBAull;
    check(classifyPass(other, eye, VsFamily{}, VsFamily{}, exposure) == PassClass::Compute, "any other compute shader stays compute");
    check(std::string(passClassName(PassClass::Exposure)) == "exposure", "the class is named exposure in the CSV and the summary");
}

// GPU pass profile v3: the capture's frame starts, simulated at the boundaries. Census and application timing are refused by the yield
// while a capture runs, so they hold the clock only before the press (the census timer that began before it) and the application frame
// is ended by the yield at the boundary. Every scenario must give 30 of 30 frames unless a holder never ends.
struct StartRun {
    uint32_t captured = 0, waits = 0, boundaries = 0;
    StartAction last = StartAction::Done;
};
template <class HeldAt>
StartRun runStarts(uint32_t total, uint32_t slots, HeldAt heldAt) {
    StartRun r;
    uint32_t started = 0, waits = 0;
    for (uint32_t b = 0; b < 2000; ++b) {
        r.boundaries = b + 1;
        const StartAction a = startAction(started, total, slots, heldAt(b), waits);
        r.last = a;
        if (a == StartAction::Begin) {
            ++started;
            waits = 0;
            ++r.captured;
        } else if (a == StartAction::Wait) {
            ++waits;
            ++r.waits;
        } else {
            break;
        }
    }
    return r;
}

void testCaptureStarts() {
    const uint32_t total = kDefaultCaptureFrames;   // 30
    check(total == 30 && kMaxCaptureFrames == 120 && kPassScopeSlotCount >= kMaxCaptureFrames,
          "the default capture is 30 frames and the scope slots cover the largest capture");

    // Scenario 1: a census timer began before the press holds the clock for three boundaries; the application frame is ended by the yield.
    const StartRun census = runStarts(total, kPassScopeSlotCount, [](uint32_t b) { return b <= 2; });
    check(census.captured == total && census.last == StartAction::Done, "a census scope held before the press delays the start; all 30 of 30 frames are captured");
    check(census.waits == 3, "the three boundaries the census scope held the clock are waits, not failures");

    // Scenario 2: the application frame is open at the press; the yield ends it at the first boundary, so nothing holds the clock.
    const StartRun appFrame = runStarts(total, kPassScopeSlotCount, [](uint32_t) { return false; });
    check(appFrame.captured == total && appFrame.waits == 0, "with nothing holding the clock, 30 of 30 frames start on the first boundaries");

    // Scenario 3: the v2 regression. Four slots: frame 5 has no slot and the batch ends after 4 frames (what the flight logged).
    const StartRun fourSlots = runStarts(total, 4, [](uint32_t) { return false; });
    check(fourSlots.captured == 4 && fourSlots.last == StartAction::Abort,
          "with 4 scope slots the capture stops at 4 frames: the v2 bug, kept as a regression case");

    // Scenario 4: a holder that ends just in time (held for boundaries 0..239) still gives 30 of 30; one more boundary and the batch aborts.
    const StartRun justInTime = runStarts(total, kPassScopeSlotCount, [](uint32_t b) { return b < kMaxStartWaits; });
    check(justInTime.captured == total && justInTime.last == StartAction::Done, "a holder that ends within kMaxStartWaits boundaries still gives 30 of 30");
    const StartRun tooLong = runStarts(total, kPassScopeSlotCount, [](uint32_t b) { return b <= kMaxStartWaits; });
    check(tooLong.captured == 0 && tooLong.last == StartAction::Abort && tooLong.waits == kMaxStartWaits,
          "a holder that never ends aborts the batch after kMaxStartWaits boundaries, with a note");

    // Scenario 5: frames already begun are not restarted: Done is reported the moment every frame has begun, held or not.
    check(startAction(total, total, kPassScopeSlotCount, true, 0) == StartAction::Done, "done wins over a held clock once every frame has begun");
}

// Built v13.6: the yield follows the capture. Wanted from the press (armed), through every batch frame, and not after the batch's last frame is
// closed; never wanted idle. The application frame is refused while it is wanted, and admitted once it is not (modelled by the flag).
void testYield() {
    check(!yieldWanted(false, false), "idle: the yield is not wanted, so application frames and census timers are not paused");
    check(yieldWanted(true, false), "armed (the press, before the first boundary): the yield is wanted");
    check(yieldWanted(false, true), "a batch running: the yield is wanted through every frame of it");
    check(yieldWanted(true, true), "armed and running together: still wanted");
    // The capture's life, boundary by boundary: press, begin frames 1 to 4, close the last one; the flag is cleared at that boundary.
    bool requested = true, batch = false;
    check(yieldWanted(requested, batch), "press: wanted");
    requested = false;
    batch = true;   // boundary 1: the batch starts
    check(yieldWanted(requested, batch), "boundary 1: the batch has started, wanted");
    batch = true;   // boundaries 2 to 4: frames 2 to 4 begin
    check(yieldWanted(requested, batch), "boundaries 2 to 4: still wanted");
    batch = false;  // boundary 5: the last frame is closed at its top and the batch ends
    check(!yieldWanted(requested, batch), "after the last frame's close (boundary 5): not wanted, so the next application frame may open");
}
}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
    if (!selfTest) {
        std::printf("usage: gpu_pass_profile_test --self-test\n");
        return 2;
    }
    testClassify();
    testCubeRuns();
    testStampBudget();
    testHashes();
    testCsv();
    testSummary();
    testYield();
    testV137();
    testV2();
    testV3Classes();
    testV4Exposure();
    testV5Delay();
    testCaptureStarts();
    std::printf("gpu_pass_profile_test: %u checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

// The final copy's admission by structure on the rig (flat_copy_structure.h, design section 83): the pure function over the
// trace corpus and over mutated and synthetic streams, the census and log lines, the stand-down and panel wording that follow
// from the new reasons, and the runtime wiring no rig can run (as source pins). --trace-structure <file> prints what the
// admission makes of each frame of a trace, the dump behind the pins in this file and in section 83.
#pragma once
#include "../../src/d3d11/flat_copy_structure.h"
#include "../../src/d3d11/flat_elite_settings.h"
#include "flat_hdr_route_tests.h"
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace copy_structure_test {

using hdr_route_test::ParsedFrame;

// What the admission makes of one trace frame: the whitelist's answer at the copy draw, the admission's, and what
// it saw. The replay is the runtime's order: each draw through the reducer, then the detector, then (at the copy)
// the admission over both.
struct CopyFacts {
    uint64_t frame = 0;
    bool copy = false;
    edvr::FlatMonoFrame whitelist{}, admitted{};
    edvr::FlatCopyDiag diag{};
    bool triggered = false;
    uint32_t outputWidth = 0, outputHeight = 0;
    bool menuAccepted = false;
    // The 3D menu's stale-slot policy (flatFrameThroughMenuCopy) as the whitelist's selection and the admission's each ask for it.
    bool staticWhitelist = false, staticAdmitted = false;
};

// An optional rewrite of each draw event before the replay (the mutation tests' hook): returns the draw it wants replayed.
using EventHook = void (*)(edvr::FlatTraceEvent& e, void* context);

// `pretendUnknownTone`: hand the admission a no-known-tone-pass refusal in place of the whitelist's answer, so a frame the
// whitelist selects shows what the structure would have made of it (the equivalence pins).
inline CopyFacts replayCopy(const ParsedFrame& frame, const edvr::FlatCopyPolicy& policy, EventHook hook = nullptr,
                            void* hookContext = nullptr, bool pretendUnknownTone = false) {
    using namespace edvr;
    CopyFacts out;
    const auto& h = frame.header;
    out.frame = h.frame; out.outputWidth = h.width; out.outputHeight = h.height;
    auto prefix = std::make_unique<FlatRuntimePrefix>();
    auto contract = std::make_unique<FlatFrameContract>();
    prefix->frame = h.frame; prefix->output = h.output; prefix->width = h.width; prefix->height = h.height;
    prefix->format = h.format;
    FlatHdrFrame hdr;
    flatHdrBeginFrame(hdr, h.width, h.height);
    for (uint32_t i = 0; i < frame.events.size(); ++i) {
        FlatTraceEvent e = frame.events[i];
        if (e.kind == kFlatTraceEventWriteResource) {
            flatRuntimeWritten(*prefix, e.key.color); flatHdrObserveExplicitWrite(hdr, e.key.color); continue;
        }
        if (e.kind == kFlatTraceEventDispatchWritten) {
            flatRuntimeDispatchObserveWritten(*prefix, e.key.color); flatHdrObserveDispatchWrite(hdr, e.key.color); continue;
        }
        if (e.kind == kFlatTraceEventMarkUncertain) { prefix->uncertain = true; continue; }
        if (e.kind == kFlatTraceEventCameraCapture) { ++prefix->sequence; continue; }
        if (e.kind == kFlatTraceEventResolve) continue;
        if (hdr_route_test::replayOverlayMarker(*prefix,e)) continue;
        if (hook) hook(e, hookContext);
        FlatRuntimeDraw d = flatTraceEventToDraw(e);
        if (e.flags & kFlatTraceForeignWork) prefix->uncertain = true;
        const bool isCopy = hdr_route_test::isCopyDraw(d, *prefix);
        FlatMonoFrame sel{};
        if (isCopy) sel = flatRuntimeObserveContract(*prefix, d, *contract);
        else flatRuntimeObserve(*prefix, d);
        const bool srvKnown = (e.flags & kFlatTraceHdrSrvKnown) != 0 && flatHdrCouldConsume(hdr, d.key);
        flatHdrObserveDraw(hdr, d.key, prefix->sequence, srvKnown ? e.hdrSrv : nullptr, srvKnown);
        if (isCopy && !out.copy) {
            out.copy = true;
            out.whitelist = sel;
            FlatMonoFrame asked = sel;
            if (pretendUnknownTone) { asked = FlatMonoFrame{}; asked.frame = asked.epoch = prefix->frame; asked.reason = FlatMonoReason::NoTonePass; }
            out.admitted = flatCopyAdmit(*prefix, hdr, d, asked, policy, &out.diag);
            out.triggered = hdr.triggered;
            out.menuAccepted = prefix->menuCopiesAccepted != 0;
            out.staticWhitelist = flatFrameThroughMenuCopy(*prefix, out.whitelist.hdr);
            out.staticAdmitted = flatFrameThroughMenuCopy(*prefix, out.admitted.hdr);
        }
    }
    return out;
}

inline bool sameSelection(const edvr::FlatMonoFrame& a, const edvr::FlatMonoFrame& b) {
    return a.reason == b.reason && a.color == b.color && a.hdr == b.hdr && a.depth == b.depth && a.dsv == b.dsv &&
           a.sceneConstants == b.sceneConstants && a.output == b.output && a.renderWidth == b.renderWidth &&
           a.renderHeight == b.renderHeight && a.outputWidth == b.outputWidth && a.outputHeight == b.outputHeight &&
           a.depthFormat == b.depthFormat && std::memcmp(a.camera, b.camera, sizeof(a.camera)) == 0 &&
           a.cameraHash == b.cameraHash && a.supportedDraws == b.supportedDraws && a.unsupportedDraws == b.unsupportedDraws;
}

// --trace-structure <file> [pretend]: one line per frame; "pretend" asks the admission about every frame as if the
// whitelist had refused its tone pass.
inline int traceStructure(const char* path, bool pretend) {
    std::vector<unsigned char> bytes;
    std::vector<ParsedFrame> frames;
    if (!hdr_route_test::readFile(path, &bytes) || !hdr_route_test::parseTrace(bytes, &frames)) {
        std::printf("cannot read or parse %s\n", path); return 2;
    }
    edvr::FlatCopyPolicy policy;
    policy.structure = true; policy.routeLatched = true;   // every frame, so the admission is read even where the route serves
    for (const ParsedFrame& pf : frames) {
        const CopyFacts c = replayCopy(pf, policy, nullptr, nullptr, pretend);
        std::printf("frame %llu: output=%ux%u copy=%u whitelist=%s admission=%s result=%s",
            (unsigned long long)c.frame, c.outputWidth, c.outputHeight, c.copy ? 1u : 0u,
            edvr::flatMonoReasonName(c.whitelist.reason), edvr::flatCopyOutcomeName(c.diag.outcome),
            edvr::flatMonoReasonName(c.admitted.reason));
        if (c.diag.outcome == edvr::FlatCopyOutcome::Declined) std::printf(" why=%s", c.diag.why);
        std::printf(" scene=%ux%u src=%ux%u fmt=%u draws=%u ldr-before=%u/%u menu=%u same-as-whitelist=%u\n",
            c.diag.sceneWidth, c.diag.sceneHeight, c.diag.srcWidth, c.diag.srcHeight, c.diag.srcFormat, c.diag.srcDraws,
            c.diag.ldrTargetsBefore, c.diag.ldrDrawsBefore, c.diag.menu ? 1u : 0u,
            sameSelection(c.whitelist, c.admitted) ? 1u : 0u);
    }
    return 0;
}

}  // namespace copy_structure_test

namespace copy_structure_test {

// ---- synthetic frames ------------------------------------------------------------------------------
// One frame built the way the runtime sees a post chain: the scene's draws into H, the first consumer of H (which is S's own
// writer in the usual chain: the tone pass), optionally a game anti-aliasing filter after it, and the final copy. Every option
// is a way a chain can be wrong; the defaults are the chain the corpus holds (an unknown tone pair, so the whitelist refuses).
struct Build {
    hdr_route_test::Scene sc;
    uint32_t sceneDraws = 10;            // pool (4) and other draws into H; the scene facts need kFlatSceneMinDraws
    bool consumerWritesS = true;         // the first consumer of H is S's writer (star-off); else a bloom level reads it first
    uint32_t sFormat = 27;
    bool sDepth = false;                 // S drawn with a depth buffer bound
    uint32_t sW = 0, sH = 0;             // S's size (0: the scene's)
    bool sFullViewport = true;
    uint32_t sWriters = 1;               // draws into S
    uint32_t llmBefore = 0;              // R-sized R8G8B8A8 passes between the first consumer and S (a game AA chain)
    bool consumer = true;                // a pass reads H at all
    bool sBeforeConsumer = false;        // S is written before anything reads H
    const void* copyReads = nullptr;     // what the copy reads (null: S)
    uint64_t sPs = 0xFE;                 // S's writer's pixel shader (0xFE is no whitelisted pair)
    uint32_t sInstances = 1;
    bool poolSources = true;
    bool stockFamily = false;            // a pool-family draw left stock (no pixel shader the producer substitutes) into the scene's depth
    bool sExplicitWrite = false;         // a Clear, Copy, Update or Map into S after its pass (the prefix model marks S bad)
    bool sClearedBefore = false;         // ...and the game clearing S for its own use BEFORE its pass: not marked
};
struct Built {
    std::unique_ptr<hdr_route_test::Stream> stream;
    edvr::FlatRuntimeDraw copy{};
    edvr::FlatMonoFrame whitelist{};
};
inline edvr::FlatRuntimeDraw withSrv(edvr::FlatRuntimeDraw d, const void* resource) {
    d.key.srvView[0] = hdr_route_test::tok(reinterpret_cast<uintptr_t>(resource) + 2); d.key.srvResource[0] = resource;
    return d;
}
inline Built build(const Build& b) {
    using namespace edvr;
    using hdr_route_test::tok;
    Built out;
    out.stream = std::make_unique<hdr_route_test::Stream>(b.sc);
    auto& s = *out.stream;
    const uint32_t w = b.sW ? b.sW : b.sc.hW, h = b.sH ? b.sH : b.sc.hH;
    const void* S = s.sc.tone;
    const auto writeS = [&](const void* t1 = nullptr) {
        FlatRuntimeDraw d = s.make(S, b.sDepth ? s.sc.probe : nullptr, w, h, b.sFormat, 0xF9, b.sPs, false, false);
        d.instances = b.sInstances;
        if (!b.sFullViewport) d.key.viewport[2] = float(w) - 8.0f;
        s.draw(d, nullptr, t1);
    };
    if (b.sClearedBefore) flatRuntimeWritten(*s.prefix, S);   // S has no draw yet: nothing to mark
    if (b.sBeforeConsumer) writeS();
    s.write(s.sc.h);
    s.sceneDraws(b.poolSources ? 4 : 0, b.sceneDraws - (b.poolSources ? 4 : 0));
    if (b.stockFamily) s.stockFamilyDraw();
    if (b.consumer && !b.consumerWritesS)
        s.draw(s.make(s.sc.half, nullptr, b.sc.hW / 2, b.sc.hH / 2, 26, 0xDF, 0xC1, false, false), s.sc.h);
    for (uint32_t i = 0; i < b.llmBefore; ++i) {
        // a game AA filter: reads the previous LDR image, writes a new R-sized R8G8B8A8 target
        const void* t = tok(0x7000 + 16 * i);
        s.draw(s.make(t, nullptr, w, h, 27, 0x03, 0x04 + i, false, false), nullptr, i ? tok(0x7000 + 16 * (i - 1)) : s.sc.h);
    }
    if (!b.sBeforeConsumer)
        for (uint32_t i = 0; i < b.sWriters; ++i) writeS(b.consumer && b.consumerWritesS && i == 0 ? s.sc.h : nullptr);
    if (b.sExplicitWrite) flatRuntimeWritten(*s.prefix, S);   // the prefix model's own call for a Clear/Copy/Update/Map into a drawn target
    FlatRuntimeDraw c = s.make(s.sc.output, nullptr, b.sc.outW, b.sc.outH, 28, flat_mono_detail::kCopyVs,
                               flat_mono_detail::kCopyPs, false, false);
    out.copy = withSrv(c, b.copyReads ? b.copyReads : S);
    out.whitelist = flatRuntimeObserve(*s.prefix, out.copy);
    // The detector sees the copy draw too, as in the runtime: handed its four slots when its cheap gate asks, the copy reading S.
    const void* srv[4] = {out.copy.key.srvResource[0], nullptr, nullptr, nullptr};
    const bool known = flatHdrCouldConsume(s.hdr, out.copy.key);
    flatHdrObserveDraw(s.hdr, out.copy.key, s.prefix->sequence, known ? srv : nullptr, known);
    return out;
}
inline edvr::FlatCopyPolicy policy(bool structure, edvr::FlatMonoResolveMode mode, bool latched = false) {
    edvr::FlatCopyPolicy p; p.structure = structure; p.mode = mode; p.routeLatched = latched;
    return p;
}
inline edvr::FlatMonoFrame admit(Built& b, const edvr::FlatCopyPolicy& p, edvr::FlatCopyDiag* diag = nullptr) {
    return edvr::flatCopyAdmit(*b.stream->prefix, b.stream->hdr, b.copy, b.whitelist, p, diag);
}

// A hook that renames the known tone pass in a corpus trace (the tone pair becomes one the whitelist has never met).
struct RenameTone { uint64_t ps = 0x00C0FFEE00C0FFEEull; uint32_t renamed = 0; };
inline void renameTone(edvr::FlatTraceEvent& e, void* context) {
    auto* r = static_cast<RenameTone*>(context);
    if (e.kind == edvr::kFlatTraceEventDraw && edvr::flat_mono_detail::toneHdrSlot(e.key.vs, e.key.ps) != ~0u) {
        e.key.ps = r->ps; ++r->renamed;
    }
}

}  // namespace copy_structure_test

// ---- the fixture: tools\flat_upscale_fixture.log, a good flight below the output and three episodes, held to what the formatters write --
// edvr_log.py --flat-upscale's own self-test reads the checked-in file. Regenerate it with `flat_temporal_test.exe --write-fixture <path>`
// after a deliberate change to a line's text; the check in flatCopyStructureTests fails the build until the file says what the formatters
// say. The lines the DLL writes inline (the key's, the route's, the runtime's counters) are copied from real logs (the rc.5 user's of
// 2026-10-01 and Sean's of 2026-09-30) and held to the runtime's source by the pins there. The base is a good flight: a startup of
// pre-scene frames (stood down for no-3d-scene, silent), then a scene rendered at 2880x1620 on a 3840x2160 output, admitted by
// structure, treated. After it, marker lines (`# episode: <name>`) open episodes the reader's self-test appends to the base one at a
// time: a render size that does not fit, a game AA chain the structure declines, and a pre-section-83 build's supersampling advice.
namespace copy_structure_test {

inline std::string flatUpscaleFixtureText() {
    using namespace edvr;
    std::string out;
    char line[1300], words[160];
    // Each segment (the base flight, then each episode) is written in time order, however the code below builds it.
    std::vector<std::pair<std::string, std::string>> segment;
    auto flush = [&] {
        std::stable_sort(segment.begin(), segment.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& entry : segment) { out += "["; out += entry.first; out += "] "; out += entry.second; out += "\n"; }
        segment.clear();
    };
    auto push = [&](const char* ts, const char* text) { segment.emplace_back(ts, text); };
    auto marker = [&](const char* name) { flush(); out += "# episode: "; out += name; out += "\n"; };
    push("15:12:00.100", "version v0.18.0-rc.5-26-g5ec0de01 (build 5EC0DE01) -- this DLL was linked 2026-10-01 20:05:44 UTC");
    push("15:12:02.151", "flat hdr route: experimental.temporal_aa_before_post=auto (read at startup) at frame=1: the route resolves the game's HDR scene "
                         "target before its bloom, depth of field and tone where the render size is at least the output's and the target is R11G11B10F; "
                         "every other frame keeps the copy route, which admits the game's final copy by its structure (an R-sized R8G8B8A8 image made after "
                         "the scene HDR's first consumer, uniformly scaled to the output) when no whitelisted tone pass wrote it, so bloom, depth of field and "
                         "the tone variant do not matter below the output either");

    // The startup: frames with a final copy and no scene.
    FlatStandDown down;
    down.enteredReason = FlatMonoReason::NoScene; down.standReason = FlatMonoReason::NoScene;
    down.enteredAfterMs = 5000; down.enteredAfterFrames = 740; down.standing = true;
    flatStandDownFormatEntered(line, sizeof(line), 742, down);
    push("15:12:08.368", line);
    down.lastStoodDownMs = 10500; down.lastProbes = 7;
    flatStandDownFormatResumed(line, sizeof(line), 1918, down, nullptr);
    push("15:12:18.903", line);

    // The scene: the game renders at 2880x1620 on a 3840x2160 output (supersampling 0.75).
    push("15:12:18.913", "flat route: trained-upscale R=2880x1620 E=3840x2160 D=3840x2160");
    FlatCopyDiag diag;
    diag.outcome = FlatCopyOutcome::Admitted; diag.whitelist = FlatMonoReason::NoTonePass;
    diag.srcWidth = 2880; diag.srcHeight = 1620; diag.srcFormat = 27; diag.srcDraws = 1;
    diag.srcVs = 0xF9CFC798F21E9AEAull; diag.srcPs = 0xE8948389387DA083ull;
    diag.triggerVs = 0xDFED8E1C9E191BECull; diag.triggerPs = 0x143AAE0597E2F7BFull;
    diag.sceneWidth = 2880; diag.sceneHeight = 1620; diag.outputWidth = 3840; diag.outputHeight = 2160;
    flatCopyFormatFirstAdmission(line, sizeof(line), 1919, diag, "trained-upscale");
    push("15:12:18.914", line);
    struct Win { const char* ts; uint64_t copies, whitelist, admitted, noScene; uint64_t treated; };
    const Win wins[] = {{"15:12:11.150", 741, 0, 0, 741, 0}, {"15:12:16.147", 3, 0, 0, 3, 0},
                        {"15:12:21.149", 331, 0, 331, 0, 0}, {"15:12:26.148", 340, 0, 340, 0, 0}, {"15:12:31.150", 338, 0, 338, 0, 0}};
    uint64_t treated = 0;
    for (const Win& w : wins) {
        FlatCopyWindow window;
        window.copies = w.copies; window.whitelist = w.whitelist; window.admitted = w.admitted; window.noScene = w.noScene;
        window.last = w.admitted ? "admitted" : "no-scene";
        if (w.admitted) {
            window.lastSceneW = 2880; window.lastSceneH = 1620; window.lastOutW = 3840; window.lastOutH = 2160;
            window.lastSrcW = 2880; window.lastSrcH = 1620; window.lastSrcVs = diag.srcVs; window.lastSrcPs = diag.srcPs;
        } else {
            window.lastOutW = 3840; window.lastOutH = 2160;
        }
        flatCopyFormatWindow(line, sizeof(line), true, window);
        push(w.ts, line);
        treated += w.admitted;
        char runtime[400];
        std::snprintf(runtime, sizeof(runtime),
            "flat runtime: treated=%llu refused=%llu last=%s render-source=game-SS jitter=(0.125,-0.1667) accepted-reset-5s=%u "
            "accepted-history-5s=%llu treated-streak=%llu longest-treated-streak=%llu",
            static_cast<unsigned long long>(treated), 744ull,
            w.admitted ? "treated-jittered" : "no-3d-scene", w.admitted ? 1u : 0u,
            static_cast<unsigned long long>(w.admitted ? w.admitted - 1 : 0), static_cast<unsigned long long>(treated),
            static_cast<unsigned long long>(treated));
        push(w.ts, runtime);
        if (w.noScene) push(w.ts, "flat runtime refusal 5s: reason=no-3d-scene count=741");
    }
    // Episode: the render size does not fit (Elite's resolution 2560x1440 on a 2560x1600 screen, supersampling 0.85).
    marker("render-size");
    FlatStandDown size;
    size.enteredReason = FlatMonoReason::RenderSize; size.standReason = FlatMonoReason::RenderSize;
    size.enteredAfterMs = 5000; size.enteredAfterFrames = 598; size.standing = true;
    flatRenderSizeWords(words, sizeof(words), 2176, 1224, 2560, 1600);
    flatStandDownFormatEntered(line, sizeof(line), 14827, size, words);
    push("15:14:28.335", line);
    EliteGraphics rc5;
    rc5.folderFound = rc5.presetKnown = rc5.custom = rc5.fileRead = true;
    std::strcpy(rc5.preset, "Custom"); std::strcpy(rc5.file, "Custom.4.4.fxcfg");
    rc5.aaMode = 0; rc5.bloomQuality = 3; rc5.dofEnabled = 2;
    const FlatWarningCause shape = flatWarningCause(true, true, false, true, true, 2176, 1224, 2560, 1600);
    FlatSettingsWarning w;
    flatComposeSettingsWarning("DLSS", rc5, 0, nullptr, nullptr, &w, shape);
    flatFormatSettingsWarningLog(line, sizeof(line), false, "DLSS", "render-size-does-not-fit-output", true, shape, w);
    push("15:14:28.335", line);
    size.probes = 19; size.pausedFrames = 3523; size.probeSeen = FlatFrameSeen::Structural; size.probeReason = FlatMonoReason::RenderSize;
    size.standSinceMs = 1000;
    flatStandDownFormatStill(line, sizeof(line), 22623, size, 31000, words);
    push("15:14:58.340", line);
    push("15:15:07.021", "flat settings warning: hidden (the work is not stood down for the shape of a post chain now, or the mode is off)");
    // Episode: a game AA chain between the tone pass and the copy, which the structure declines (the Anti-aliasing advice stays).
    marker("game-aa");
    FlatCopyDiag aa;
    aa.outcome = FlatCopyOutcome::Declined; aa.why = "r-sized-image-passes-follow-the-first-consumer-of-the-scene-hdr";
    aa.whitelist = FlatMonoReason::NoTonePass; aa.srcWidth = 2880; aa.srcHeight = 1620; aa.srcFormat = 27; aa.srcDraws = 1;
    aa.srcVs = 0x98E6F9986FDC9A53ull; aa.srcPs = 0x4168985B52C5D7C4ull; aa.ldrTargetsBefore = 2; aa.ldrDrawsBefore = 2;
    aa.sceneWidth = 2880; aa.sceneHeight = 1620; aa.outputWidth = 3840; aa.outputHeight = 2160;
    flatCopyFormatDeclined(line, sizeof(line), 20010, aa);
    push("15:16:40.100", line);
    FlatCopyWindow aaWindow;
    for (int i = 0; i < 600; ++i) aaWindow.note(aa);
    flatCopyFormatWindow(line, sizeof(line), true, aaWindow);
    push("15:16:45.149", line);
    FlatStandDown chain;
    chain.enteredReason = FlatMonoReason::NoTonePass; chain.standReason = FlatMonoReason::NoTonePass;
    chain.enteredAfterMs = 5000; chain.enteredAfterFrames = 591; chain.standing = true;
    flatStandDownFormatEntered(line, sizeof(line), 20400, chain);
    push("15:16:50.039", line);
    EliteGraphics aaOn = rc5; aaOn.aaMode = 4; aaOn.bloomQuality = 0; aaOn.dofEnabled = 0;
    const FlatWarningCause admission = flatWarningCause(true, true, false, false, true, 2880, 1620, 3840, 2160);
    flatComposeSettingsWarning("DLSS", aaOn, 0, nullptr, nullptr, &w, admission);
    flatFormatSettingsWarningLog(line, sizeof(line), false, "DLSS", "no-known-tone-pass", true, admission, w);
    push("15:16:50.039", line);
    push("15:16:50.040", "flat runtime refusal 5s: reason=no-known-tone-pass count=591");
    // Episode: the old advice (a build from before section 83): the supersampling paragraph and its bracket.
    marker("old-advice");
    push("15:18:00.100", "flat settings warning: shown (mode=DLAA, frames refused for no-known-tone-pass, work stood down, supersampling below 1.0 "
                         "(render 2880x1620, output 3840x2160)): DLAA is not active: Elite's post-processing is not recognised. Turn off in Elite's "
                         "graphics options: Bloom, Depth of field Supersampling is below 1.0. At 1.0 or above, EDVR anti-aliases before bloom and depth "
                         "of field, so they no longer block it. Raising it costs GPU time.");
    flush();
    return out;
}

}  // namespace copy_structure_test

inline int flatCopyStructureTests() {
    using namespace edvr;
    using namespace copy_structure_test;
    namespace fs = std::filesystem;
    int failures = 0;
    auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: copy structure %s\n", name); ++failures; }
    };
    // An exact line, printed in full when it differs: the lines are what a flight's reader is built on.
    auto expectText = [&](const std::string& actual, const std::string& want, const char* name) {
        if (actual != want) {
            std::printf("FAIL: copy structure %s\nwant: %s\nhave: %s\n", name, want.c_str(), actual.c_str());
            ++failures;
        }
    };
    const FlatCopyPolicy autoDlss = policy(true, FlatMonoResolveMode::Dlss);

    // ---- the corpus: structure is the whitelist, and more -------------------------------------------------------------
    // 46 frames of 17 captures (menu, flight, on foot; R from half of D to 1.5 D; stock, EDHM, Bloom 3 and DoF 2). Three kinds of
    // frame: 42 the whitelist selects (a known tone pass), 3 it refuses for its tone pass (the bloom and DoF captures at R = D),
    // and 1 with no scene (a 2D menu).
    {
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator("tools/flat_temporal_test/traces"))
            if (entry.path().extension() == ".bin") files.push_back(entry.path());
        expect(files.size() == 17, "the corpus is the 17 captures the manifest names");
        uint32_t frames = 0, whitelisted = 0, untouchedWhitelisted = 0, admittedAsked = 0, sameAsked = 0, staticSame = 0,
                 unknownTone = 0, unknownAdmitted = 0, unknownRouteServes = 0, noScene = 0, noSceneAsked = 0, leftover = 0,
                 chainZero = 0, admittedAny = 0;
        for (const fs::path& file : files) {
            std::vector<unsigned char> bytes;
            std::vector<ParsedFrame> parsed;
            if (!hdr_route_test::readFile(file, &bytes) || !hdr_route_test::parseTrace(bytes, &parsed)) {
                expect(false, "every corpus trace reads and parses"); continue;
            }
            for (const ParsedFrame& pf : parsed) {
                ++frames;
                const CopyFacts real = replayCopy(pf, autoDlss);
                const CopyFacts asked = replayCopy(pf, policy(true, FlatMonoResolveMode::Dlss, true), nullptr, nullptr, true);
                const CopyFacts bare = replayCopy(pf, policy(false, FlatMonoResolveMode::Dlss), nullptr, nullptr, true);
                if (real.whitelist.selected()) {
                    ++whitelisted;
                    // The runtime's own policy: a frame the whitelist selects is never touched.
                    if (real.diag.outcome == FlatCopyOutcome::Untouched && sameSelection(real.whitelist, real.admitted))
                        ++untouchedWhitelisted;
                    // The whitelist's tone pass asked away: the structure alone makes the same selection.
                    if (asked.diag.outcome == FlatCopyOutcome::Admitted) ++admittedAsked;
                    if (sameSelection(real.whitelist, asked.admitted)) ++sameAsked;
                    if (real.staticWhitelist == asked.staticAdmitted) ++staticSame;
                    if (asked.diag.ldrTargetsBefore == 0) ++chainZero;
                    // With the key off the structure says nothing about a frame with a scene, whatever the whitelist did.
                    if (bare.diag.outcome != FlatCopyOutcome::Disabled) ++leftover;
                } else if (real.whitelist.reason == FlatMonoReason::NoTonePass) {
                    ++unknownTone;
                    // At R = D the HDR route serves these (DLSS evaluates at R): the structure leaves them to it, refused as before.
                    if (real.diag.outcome == FlatCopyOutcome::RouteServes && real.admitted.reason == FlatMonoReason::NoTonePass)
                        ++unknownRouteServes;
                    // Asked as the copy route's own (route latched off): admitted by structure.
                    if (asked.diag.outcome == FlatCopyOutcome::Admitted && asked.admitted.selected()) ++unknownAdmitted;
                } else {
                    ++noScene;
                    if (asked.diag.outcome == FlatCopyOutcome::NoScene && asked.admitted.reason == FlatMonoReason::NoScene &&
                        real.diag.outcome == FlatCopyOutcome::Untouched && real.admitted.reason == real.whitelist.reason)
                        ++noSceneAsked;
                }
                if (asked.admitted.selected()) ++admittedAny;
            }
        }
        expect(frames == 46 && whitelisted == 42 && unknownTone == 3 && noScene == 1,
               "the corpus is 42 whitelisted frames, 3 with an unknown tone pass (Bloom 3 and DoF 2) and 1 with no scene");
        expect(untouchedWhitelisted == 42,
               "a frame the whitelist selects comes back from the admission byte for byte the same, in all 42 (the key auto)");
        expect(admittedAsked == 42 && sameAsked == 42,
               "with the whitelist's tone pass asked away the structure admits all 42 and selects exactly what the whitelist did: "
               "S, H, depth, camera, extents and sources");
        expect(staticSame == 42,
               "...including the 3D menu's stale-slot policy: the menu frames come through the menu copy's inherited HDR either way");
        expect(chainZero == 42, "no corpus frame has an R-sized image pass between the scene's first consumer and the copy's source");
        expect(leftover == 0, "with the key off the structure leaves every frame with a scene alone");
        expect(unknownAdmitted == 3 && unknownRouteServes == 3,
               "the 3 Bloom 3 / DoF 2 frames are admitted by structure as the copy route's, and at R = D the HDR route serves them");
        expect(noSceneAsked == 1, "the 2D menu frame has no scene: asked as a refusal it is no-3d-scene, as the whitelist left it it is untouched");
        expect(admittedAny == 45, "45 of 46 frames are admitted by structure when asked");
    }

    // ---- the whitelist's tone pass renamed: every R < D cell of the corpus is still treated -------------------------------
    // The rc.5 case (and Sean's 0.75 flight): game supersampling below 1.0, a post chain the whitelist does not know. Each
    // corpus capture below the output, with its tone pair renamed to one nobody has seen, is refused by the whitelist and
    // admitted by the structure to the same selection, for DLSS, FSR and EDVR's TAA, and not at all when the key is off.
    {
        const char* below[] = {"flat_trace_10806.bin", "flat_trace_16271.bin", "flat_trace_36807.bin", "flat_trace_41502.bin",
                               "flat_trace_45992.bin", "flat_trace_52244.bin", "flat_trace_60043.bin", "flat_trace_67594.bin"};
        uint32_t cells = 0, refused = 0, admitted = 0, same = 0, keyOff = 0, modes = 0;
        for (const char* name : below) {
            std::vector<unsigned char> bytes;
            std::vector<ParsedFrame> parsed;
            if (!hdr_route_test::readFile(fs::path("tools/flat_temporal_test/traces") / name, &bytes) ||
                !hdr_route_test::parseTrace(bytes, &parsed)) { expect(false, "the R < D captures read"); continue; }
            for (const ParsedFrame& pf : parsed) {
                ++cells;
                const CopyFacts original = replayCopy(pf, autoDlss);
                for (const FlatMonoResolveMode mode : {FlatMonoResolveMode::Dlss, FlatMonoResolveMode::Fsr, FlatMonoResolveMode::Taa}) {
                    RenameTone rename;
                    const CopyFacts c = replayCopy(pf, policy(true, mode), renameTone, &rename);
                    if (mode == FlatMonoResolveMode::Dlss) {
                        if (!c.whitelist.selected() && c.whitelist.reason == FlatMonoReason::NoTonePass && rename.renamed == 1) ++refused;
                        RenameTone again;
                        const CopyFacts off = replayCopy(pf, policy(false, mode), renameTone, &again);
                        if (off.diag.outcome == FlatCopyOutcome::Disabled && off.admitted.reason == FlatMonoReason::NoTonePass) ++keyOff;
                    }
                    if (c.diag.outcome == FlatCopyOutcome::Admitted) ++admitted;
                    if (sameSelection(original.whitelist, c.admitted)) ++same;
                    ++modes;
                }
            }
        }
        expect(cells == 24 && refused == 24, "the 8 captures below the output are 24 frames, each refused by the whitelist once its tone pair is renamed");
        expect(admitted == 72 && same == 72 && modes == 72,
               "...and each is admitted by structure to exactly the selection the known tone pass gave, in all three modes (72 of 72)");
        expect(keyOff == 24, "...and with the key off none is: the whitelist's refusal stands");
    }

    // ---- where the HDR route serves, the structure does nothing; where it does not, it serves ---------------------------
    // R = D (DLSS, FSR, TAA) and R > D (DLSS, FSR) are the route's; EDVR's TAA above D and a latched route are the copy's.
    {
        struct Cell { const char* file; FlatMonoResolveMode mode; bool latched; FlatCopyOutcome want; const char* what; };
        const Cell cells[] = {
            {"flat_trace_43365.bin", FlatMonoResolveMode::Dlss, false, FlatCopyOutcome::RouteServes, "DLSS at R = D"},
            {"flat_trace_43365.bin", FlatMonoResolveMode::Taa, false, FlatCopyOutcome::RouteServes, "TAA at R = D"},
            {"flat_trace_43365.bin", FlatMonoResolveMode::Fsr, false, FlatCopyOutcome::RouteServes, "FSR at R = D"},
            {"flat_trace_39261.bin", FlatMonoResolveMode::Dlss, false, FlatCopyOutcome::RouteServes, "DLSS at R = 1.5 D"},
            {"flat_trace_40160.bin", FlatMonoResolveMode::Fsr, false, FlatCopyOutcome::RouteServes, "FSR at R = 1.5 D"},
            {"flat_trace_43870.bin", FlatMonoResolveMode::Taa, false, FlatCopyOutcome::Admitted, "TAA at R = 1.5 D (the display-grid TAA)"},
            {"flat_trace_43365.bin", FlatMonoResolveMode::Dlss, true, FlatCopyOutcome::Admitted, "DLSS at R = D, the route latched off"},
            {"flat_trace_39261.bin", FlatMonoResolveMode::Dlss, true, FlatCopyOutcome::Admitted, "DLSS at R = 1.5 D, the route latched off"},
            {"flat_trace_52244.bin", FlatMonoResolveMode::Dlss, false, FlatCopyOutcome::Admitted, "DLSS at R = 0.5 D"},
        };
        uint32_t ok = 0;
        for (const Cell& cell : cells) {
            std::vector<unsigned char> bytes;
            std::vector<ParsedFrame> parsed;
            if (!hdr_route_test::readFile(fs::path("tools/flat_temporal_test/traces") / cell.file, &bytes) ||
                !hdr_route_test::parseTrace(bytes, &parsed) || parsed.empty()) { expect(false, cell.what); continue; }
            RenameTone rename;
            const CopyFacts c = replayCopy(parsed[0], policy(true, cell.mode, cell.latched), renameTone, &rename);
            const bool served = c.diag.outcome == FlatCopyOutcome::RouteServes && c.admitted.reason == FlatMonoReason::NoTonePass;
            const bool admitted = c.diag.outcome == FlatCopyOutcome::Admitted && c.admitted.selected();
            if (cell.want == FlatCopyOutcome::RouteServes ? served : admitted) ++ok;
            else std::printf("  (cell %s: outcome %s)\n", cell.what, flatCopyOutcomeName(c.diag.outcome));
        }
        expect(ok == sizeof(cells) / sizeof(cells[0]),
               "the HDR route serves every mode where it evaluates at the render size; the copy admits TAA above the output, a latched route and every R < D");
    }

    // ---- the chain, by synthetic frames: each way it can be wrong -----------------------------------------------------
    {
        Build good;
        Built g = build(good);
        FlatCopyDiag diag;
        const FlatMonoFrame sel = admit(g, policy(true, FlatMonoResolveMode::Dlss, true), &diag);
        expect(g.whitelist.reason == FlatMonoReason::NoTonePass && diag.outcome == FlatCopyOutcome::Admitted && sel.selected() &&
               sel.color == g.stream->sc.tone && sel.hdr == g.stream->sc.h && sel.depth == g.stream->sc.hDepth &&
               sel.renderWidth == 3840 && sel.renderHeight == 2160 && sel.outputWidth == 3840 && sel.supportedDraws == 4 &&
               sel.toneSequence != 0 && sel.copySequence > sel.toneSequence && diag.ldrTargetsBefore == 0 &&
               diag.srcWidth == 3840 && diag.srcFormat == 27 && diag.srcDraws == 1 && !diag.menu,
               "the canonical unknown chain: the whitelist refuses (no-known-tone-pass), the structure admits S, H, its depth and camera");

        struct Row { const char* name; Build b; const char* why; };
        std::vector<Row> rows;
        auto add = [&](const char* name, const char* why, void (*edit)(Build&)) { Row r{name, Build{}, why}; edit(r.b); rows.push_back(r); };
        add("S is not R8G8B8A8", "the-copy-source-is-not-r8g8b8a8", [](Build& b) { b.sFormat = 9; });
        add("S is drawn with a depth buffer", "the-copy-source-was-drawn-with-a-depth-buffer", [](Build& b) { b.sDepth = true; });
        add("S is a different size than the scene", "the-copy-source-is-not-the-scene-size", [](Build& b) { b.sW = 1920; b.sH = 1080; });
        add("S has two writers", "the-copy-source-is-not-written-by-one-pass", [](Build& b) { b.sWriters = 2; });
        add("S's pass is not full viewport", "the-copy-source-pass-is-not-full-viewport", [](Build& b) { b.sFullViewport = false; });
        add("S's pass draws several instances", "the-copy-source-is-not-written-by-one-pass", [](Build& b) { b.sInstances = 4; });
        add("nothing reads H", "no-pass-read-the-scene-hdr", [](Build& b) { b.consumer = false; });
        add("S is written before H is read", "the-copy-source-was-written-before-the-scene-was-read",
            [](Build& b) { b.sBeforeConsumer = true; b.consumerWritesS = false; });
        add("the copy reads a target nobody drew", "the-copy-source-was-not-rendered-this-frame",
            [](Build& b) { b.copyReads = hdr_route_test::tok(0x9900); });
        add("a game AA filter follows the tone pass", "r-sized-image-passes-follow-the-first-consumer-of-the-scene-hdr",
            [](Build& b) { b.llmBefore = 1; b.consumerWritesS = false; });
        add("two game AA filters follow it", "r-sized-image-passes-follow-the-first-consumer-of-the-scene-hdr",
            [](Build& b) { b.llmBefore = 2; b.consumerWritesS = false; });
        add("S is cleared, copied into or mapped after its pass", "the-copy-source-was-written-outside-a-draw",
            [](Build& b) { b.sExplicitWrite = true; });
        bool all = true;
        for (const Row& r : rows) {
            Built built = build(r.b);
            FlatCopyDiag d;
            const FlatMonoFrame refused = admit(built, policy(true, FlatMonoResolveMode::Dlss, true), &d);
            const bool ok = built.whitelist.reason == FlatMonoReason::NoTonePass && d.outcome == FlatCopyOutcome::Declined &&
                            std::strcmp(d.why, r.why) == 0 && !refused.selected() && refused.reason == FlatMonoReason::NoTonePass;
            if (!ok) { all = false; std::printf("  (row \"%s\": outcome %s why %s)\n", r.name, flatCopyOutcomeName(d.outcome), d.why); }
        }
        expect(all, "each way the chain can be wrong is declined by name and the whitelist's refusal stands");
        Build aa; aa.llmBefore = 2; aa.consumerWritesS = false;
        Built aaBuilt = build(aa);
        FlatCopyDiag aaDiag;
        admit(aaBuilt, policy(true, FlatMonoResolveMode::Dlss, true), &aaDiag);
        expect(aaDiag.ldrTargetsBefore == 2 && aaDiag.ldrDrawsBefore == 2,
               "the chain length rides the decline: two R-sized image passes, one draw each");

        // The refusals that are the scene's, not the chain's.
        Build sources; sources.poolSources = false; sources.stockFamily = true;
        Built sourcesBuilt = build(sources);
        FlatCopyDiag sourcesDiag;
        const FlatMonoFrame noSource = admit(sourcesBuilt, policy(true, FlatMonoResolveMode::Dlss, true), &sourcesDiag);
        expect(sourcesDiag.outcome == FlatCopyOutcome::Refused && noSource.reason == FlatMonoReason::NoSupportedSource &&
               !flatMonoReasonStructural(noSource.reason),
               "a recognised chain with no motion source and a pool-family draw left stock is refused by the selector's own reason (no-supported-motion-source-pair), transient");
        Build poolless; poolless.poolSources = false;
        Built poolBuilt = build(poolless);
        const FlatMonoFrame poolFrame = admit(poolBuilt, policy(true, FlatMonoResolveMode::Dlss, true));
        expect(poolFrame.selected() && poolFrame.sourceFree && poolFrame.supportedDraws == 0,
               "a recognised chain with no pool-family draw at all is selected with no motion source: the pool-less view is treated, not refused");
        Build ambiguous;
        Built amb = build(ambiguous);
        amb.stream->hdr.trigger.ambiguous = true;
        FlatCopyDiag ambDiag;
        admit(amb, policy(true, FlatMonoResolveMode::Dlss, true), &ambDiag);
        expect(ambDiag.outcome == FlatCopyOutcome::Declined && std::strcmp(ambDiag.why, "more-than-one-hdr-candidate") == 0,
               "an ambiguous first consumer (two H candidates) is declined");
        Build elsewhere;
        Built other = build(elsewhere);
        other.stream->hdr.trigger.hdr = hdr_route_test::tok(0x1234);
        FlatCopyDiag otherDiag;
        admit(other, policy(true, FlatMonoResolveMode::Dlss, true), &otherDiag);
        expect(otherDiag.outcome == FlatCopyOutcome::Declined && std::strcmp(otherDiag.why, "the-first-consumer-reads-another-target") == 0,
               "a first consumer that reads another target than the scene's H is declined");
        Build copyBad;
        Built cb = build(copyBad);
        cb.copy.key.viewport[2] = 100.0f;
        FlatCopyDiag cbDiag;
        admit(cb, policy(true, FlatMonoResolveMode::Dlss, true), &cbDiag);
        expect(cbDiag.outcome == FlatCopyOutcome::Declined && std::strcmp(cbDiag.why, "the-copy-is-not-a-plain-full-screen-copy") == 0,
               "the copy draw's own checks are made here (the reducer made none: no tone pass was found): a partial viewport is declined");
        // The explicit write is the one the prefix model marks: a Clear before S's pass (the game readying the target) leaves no mark.
        Build clearedBefore; clearedBefore.sClearedBefore = true;
        Built cbf = build(clearedBefore);
        FlatCopyDiag cbfDiag;
        admit(cbf, policy(true, FlatMonoResolveMode::Dlss, true), &cbfDiag);
        expect(cbfDiag.outcome == FlatCopyOutcome::Admitted,
               "a write into S before its pass is not marked and does not stop the admission (only one after the pass does)");
        // EDVR's `dlaa` has no route below the output (flatResolveRoute: dlaa-requires-native). Admitting such a frame would make it
        // Treatable and the resolver would then refuse every one in silence, with no stand-down and no warning; the structure declines it
        // by name and the whitelist's refusal (and with it the stand-down and the F8 warning) stands, as before this existed.
        {
            Build below; below.sc.hW = 1920; below.sc.hH = 1080;
            Built bb = build(below);
            FlatCopyDiag bd;
            const FlatMonoFrame out = admit(bb, policy(true, FlatMonoResolveMode::Dlaa), &bd);
            expect(bd.outcome == FlatCopyOutcome::Declined && std::strcmp(bd.why, "the-mode-has-no-route-at-this-render-size") == 0 &&
                       !out.selected() && out.reason == FlatMonoReason::NoTonePass,
                   "DLAA below the output has no route: declined by name, the whitelist's refusal stands");
            Build native;
            Built nb = build(native);
            FlatCopyDiag nd;
            admit(nb, policy(true, FlatMonoResolveMode::Dlaa), &nd);
            Built nl = build(native);
            FlatCopyDiag ld;
            admit(nl, policy(true, FlatMonoResolveMode::Dlaa, true), &ld);
            expect(nd.outcome == FlatCopyOutcome::RouteServes && ld.outcome == FlatCopyOutcome::Admitted,
                   "DLAA at R = D is the HDR route's, and the copy's when the route is latched off: the route check is below the output only");
        }

        // Only a tone refusal is touched: every other answer of the reducer is the same answer.
        for (const FlatMonoReason reason : {FlatMonoReason::Truncated, FlatMonoReason::NoHdrCamera, FlatMonoReason::ConflictingHdr,
                                            FlatMonoReason::BrokenLineage, FlatMonoReason::WrongOrder, FlatMonoReason::NoOutputCopy}) {
            Build plain;
            Built p = build(plain);
            FlatMonoFrame wl = p.whitelist; wl.reason = reason;
            FlatCopyDiag d;
            const FlatMonoFrame out = flatCopyAdmit(*p.stream->prefix, p.stream->hdr, p.copy, wl, policy(true, FlatMonoResolveMode::Dlss, true), &d);
            expect(out.reason == reason && d.outcome == FlatCopyOutcome::Untouched, "a refusal that is not about the tone pass is left exactly as it was");
        }
        Build selectedBuild;
        Built sb = build(selectedBuild);
        FlatMonoFrame chosen = sb.whitelist; chosen.reason = FlatMonoReason::Selected; chosen.color = hdr_route_test::tok(0x55);
        FlatCopyDiag keep;
        const FlatMonoFrame kept = flatCopyAdmit(*sb.stream->prefix, sb.stream->hdr, sb.copy, chosen, policy(true, FlatMonoResolveMode::Dlss, true), &keep);
        expect(kept.selected() && kept.color == chosen.color && keep.outcome == FlatCopyOutcome::Untouched,
               "a frame the whitelist selected is returned as it was, whatever else the frame looks like");
        const FlatMonoFrame invalid = [&] {
            FlatMonoFrame wl = sb.whitelist; wl.reason = FlatMonoReason::InvalidTonePass;
            FlatCopyDiag d;
            return flatCopyAdmit(*sb.stream->prefix, sb.stream->hdr, sb.copy, wl, policy(true, FlatMonoResolveMode::Dlss, true), &d);
        }();
        expect(invalid.selected(), "invalid-tone-pass (a known tone pass that fails its checks) is rescued by the structure too");
    }

    // ---- the selector's own extent gates: the route's (R >= D) and the copy structure's (a uniform scale, half to twice) --------------------
    // flatCopyAdmit classifies the scene's size first, so the selector's own gate is a second line of defence. It is held here directly, on the
    // very function both callers use, with the gate named and a detector state made by hand (an off-shape scene has no trigger of its own).
    {
        struct Row { uint32_t w, h; FlatHdrExtentGate gate; FlatMonoReason want; const char* what; };
        const Row rows[] = {
            {3840, 2160, FlatHdrExtentGate::RenderAtLeastOutput, FlatMonoReason::Selected, "the route takes R = D"},
            {5760, 3240, FlatHdrExtentGate::RenderAtLeastOutput, FlatMonoReason::Selected, "the route takes R = 1.5 D"},
            {1920, 1080, FlatHdrExtentGate::RenderAtLeastOutput, FlatMonoReason::HdrExtent, "the route refuses R = 0.5 D"},
            {1920, 1080, FlatHdrExtentGate::UniformHalfToDouble, FlatMonoReason::Selected, "the structure takes R = 0.5 D (exactly half)"},
            {7680, 4320, FlatHdrExtentGate::UniformHalfToDouble, FlatMonoReason::Selected, "the structure takes R = 2 D (exactly twice)"},
            {1900, 1069, FlatHdrExtentGate::UniformHalfToDouble, FlatMonoReason::RenderSize, "the structure refuses a pixel under half"},
            {7700, 4332, FlatHdrExtentGate::UniformHalfToDouble, FlatMonoReason::RenderSize, "the structure refuses over twice"},
            {3840, 2000, FlatHdrExtentGate::UniformHalfToDouble, FlatMonoReason::RenderSize, "the structure refuses a shape that is not the output's"},
            {3840, 2000, FlatHdrExtentGate::RenderAtLeastOutput, FlatMonoReason::HdrExtent, "the route refuses a shape that is not the output's"},
        };
        bool all = true;
        for (const Row& r : rows) {
            Build b; b.sc.hW = r.w; b.sc.hH = r.h;
            Built built = build(b);
            FlatHdrFrame fake;
            flatHdrBeginFrame(fake, b.sc.outW, b.sc.outH);
            fake.triggered = true; fake.trigger.hdr = built.stream->sc.h; fake.trigger.sequence = built.stream->prefix->sequence;
            const FlatMonoFrame sel = flatSelectHdrRouteAt(*built.stream->prefix, fake, [](uint64_t, uint64_t) { return true; },
                                                          fake.trigger.sequence, r.gate);
            const bool ok = sel.reason == r.want &&
                            (r.want != FlatMonoReason::RenderSize && r.want != FlatMonoReason::HdrExtent
                                 ? sel.selected() && sel.renderWidth == r.w
                                 : sel.renderWidth == r.w && sel.renderHeight == r.h && sel.outputWidth == b.sc.outW && sel.outputHeight == b.sc.outH);
            if (!ok) { all = false; std::printf("  (gate row \"%s\": got %s)\n", r.what, flatMonoReasonName(sel.reason)); }
        }
        expect(all, "the selector's extent gates, row by row (the refusals carry the measured sizes)");
    }

    // ---- the scene's size: no scene, a size that does not fit, the key --------------------------------------------------
    {
        // No scene at all: a startup, a loading screen or a 2D menu (here: the draws into H are too few to be a scene).
        Build few; few.sceneDraws = 7; few.poolSources = false;
        Built f = build(few);
        FlatCopyDiag fd;
        const FlatMonoFrame none = admit(f, autoDlss, &fd);
        expect(fd.outcome == FlatCopyOutcome::NoScene && none.reason == FlatMonoReason::NoScene && flatMonoReasonStructural(none.reason) &&
               !flatMonoReasonWarrantsWarning(none.reason) && flatFrameSeenFor(false, none.reason) == FlatFrameSeen::Structural,
               "a copy with no scene is no-3d-scene: structural (the work stands down) and silent");
        Build eight; eight.sceneDraws = 8;
        Built e = build(eight);
        FlatCopyDiag ed;
        admit(e, policy(true, FlatMonoResolveMode::Dlss, true), &ed);
        expect(ed.outcome == FlatCopyOutcome::Admitted, "eight draws into the scene's H are a scene (kFlatSceneMinDraws)");
        // ...and it says so whatever the key and the mode.
        Built f2 = build(few);
        FlatCopyDiag fd2;
        admit(f2, policy(false, FlatMonoResolveMode::Taa), &fd2);
        expect(fd2.outcome == FlatCopyOutcome::NoScene, "no-3d-scene is named with the key off too");

        // The rc.5 user's rig: Elite's resolution 2560x1440 on a 2560x1600 screen, supersampling 0.85: R = 2176x1224.
        Build shape;
        shape.sc.outW = 2560; shape.sc.outH = 1600; shape.sc.hW = 2176; shape.sc.hH = 1224;
        Built s = build(shape);
        FlatCopyDiag sd;
        const FlatMonoFrame refused = admit(s, autoDlss, &sd);
        expect(s.whitelist.reason == FlatMonoReason::NoTonePass && sd.outcome == FlatCopyOutcome::RenderSize &&
               refused.reason == FlatMonoReason::RenderSize && refused.renderWidth == 2176 && refused.renderHeight == 1224 &&
               refused.outputWidth == 2560 && refused.outputHeight == 1600 && flatMonoReasonStructural(refused.reason) &&
               flatMonoReasonWarrantsWarning(refused.reason) && sd.sceneWidth == 2176 && sd.sceneHeight == 1224,
               "the shape is off (2176x1224 on 2560x1600): render-size-does-not-fit-output with the measured sizes, structural, warns");
        Built s2 = build(shape);
        FlatCopyDiag sd2;
        const FlatMonoFrame keyOff = admit(s2, policy(false, FlatMonoResolveMode::Dlss), &sd2);
        expect(sd2.outcome == FlatCopyOutcome::RenderSize && keyOff.reason == FlatMonoReason::RenderSize,
               "...and the same with the key off: the diagnosis is not the admission");
        // The same rig with Elite's resolution set to the screen's: a uniform scale, so admitted.
        Build fixed;
        fixed.sc.outW = 2560; fixed.sc.outH = 1600; fixed.sc.hW = 2176; fixed.sc.hH = 1360;
        Built fx = build(fixed);
        FlatCopyDiag fxd;
        admit(fx, autoDlss, &fxd);
        expect(fxd.outcome == FlatCopyOutcome::Admitted && fxd.sceneWidth == 2176 && fxd.sceneHeight == 1360,
               "supersampling 0.85 at the screen's own resolution (2176x1360 on 2560x1600) is a uniform scale: admitted");
        // Out of the half-to-twice band, in the right shape.
        for (const std::pair<uint32_t, uint32_t> size : {std::pair<uint32_t, uint32_t>{1200, 750}, {5800, 3625}, {7680, 4800}}) {
            Build band; band.sc.outW = 2560; band.sc.outH = 1600; band.sc.hW = size.first; band.sc.hH = size.second;
            Built bb = build(band);
            FlatCopyDiag bd;
            const FlatMonoFrame out = admit(bb, autoDlss, &bd);
            expect(bd.outcome == FlatCopyOutcome::RenderSize && out.reason == FlatMonoReason::RenderSize,
                   "a render size under half or over twice the screen's is render-size-does-not-fit-output too");
        }
        // The bands' edges: exactly half and exactly twice fit.
        for (const std::pair<uint32_t, uint32_t> size : {std::pair<uint32_t, uint32_t>{1280, 800}, {5120, 3200}}) {
            Build edge; edge.sc.outW = 2560; edge.sc.outH = 1600; edge.sc.hW = size.first; edge.sc.hH = size.second;
            Built eb = build(edge);
            FlatCopyDiag edd;
            admit(eb, policy(true, FlatMonoResolveMode::Taa), &edd);
            expect(edd.outcome == FlatCopyOutcome::Admitted, "exactly half and exactly twice the screen's size are in the band");
        }
        // The key off: a frame with a scene and a recognisable chain is left to the whitelist, and says so.
        Build offBuild;
        Built ob = build(offBuild);
        FlatCopyDiag od;
        const FlatMonoFrame off = admit(ob, policy(false, FlatMonoResolveMode::Dlss), &od);
        expect(od.outcome == FlatCopyOutcome::Disabled && off.reason == FlatMonoReason::NoTonePass,
               "the key off: the structure admits nothing (the whitelist alone, as before the route existed)");
        // A route-served frame is the route's: DLSS, FSR and TAA at R = D, DLSS and FSR above; TAA above it, and the latch, are not.
        for (const FlatMonoResolveMode mode : {FlatMonoResolveMode::Dlss, FlatMonoResolveMode::Fsr, FlatMonoResolveMode::Taa}) {
            Build native; Built nb = build(native);
            FlatCopyDiag nd;
            const FlatMonoFrame out = admit(nb, policy(true, mode), &nd);
            expect(nd.outcome == FlatCopyOutcome::RouteServes && out.reason == FlatMonoReason::NoTonePass,
                   "R = D: the HDR route serves every mode, and the whitelist's answer stands");
        }
        Build up; up.sc.hW = 5760; up.sc.hH = 3240;
        for (const FlatMonoResolveMode mode : {FlatMonoResolveMode::Dlss, FlatMonoResolveMode::Fsr, FlatMonoResolveMode::Taa}) {
            Built ub = build(up);
            FlatCopyDiag ud;
            admit(ub, policy(true, mode), &ud);
            expect(ud.outcome == (mode == FlatMonoResolveMode::Taa ? FlatCopyOutcome::Admitted : FlatCopyOutcome::RouteServes),
                   "R = 1.5 D: the route serves DLSS and FSR (they evaluate at R); EDVR's TAA does not, so the copy admits it");
        }
        Build down; down.sc.hW = 1920; down.sc.hH = 1080;
        for (const FlatMonoResolveMode mode : {FlatMonoResolveMode::Dlss, FlatMonoResolveMode::Fsr, FlatMonoResolveMode::Taa}) {
            Built db = build(down);
            FlatCopyDiag dd;
            admit(db, policy(true, mode), &dd);
            expect(dd.outcome == FlatCopyOutcome::Admitted, "R = 0.5 D: the copy admits every mode (the route needs R >= D)");
        }
    }

    // ---- the stand-down and the F8 panel follow the new reasons -------------------------------------------------------
    {
        using stand_down_test::Sim;
        // The startup: pre-scene frames (a copy and no scene) stand the work down and say nothing, then a scene resumes it.
        {
            Sim sim;
            bool warned = sim.machine.warningActive();
            FlatStandDownEvent event = FlatStandDownEvent::None;
            while (event != FlatStandDownEvent::Entered) { event = sim.frame(FlatFrameSeen::Structural, FlatMonoReason::NoScene); warned = warned || sim.machine.warningActive(); }
            for (int i = 0; i < 800; ++i) { sim.frame(FlatFrameSeen::Structural, FlatMonoReason::NoScene); warned = warned || sim.machine.warningActive(); }
            expect(sim.machine.standing && sim.machine.reason() == FlatMonoReason::NoScene && !warned && sim.machine.probes >= 6,
                   "pre-scene frames stand the work down for no-3d-scene and never warn: not at the stand-down, not through its probes");
            bool resumed = false;
            for (int i = 0; i < 200 && !resumed; ++i)
                resumed = sim.frame(FlatFrameSeen::Treatable, FlatMonoReason::Selected) == FlatStandDownEvent::Resumed;
            expect(resumed && !sim.machine.warningActive(), "a probe frame the structure admits resumes the work");
            // A scene whose chain is refused after the pre-scene frames warns from the probe that sees it.
            Sim later;
            while (later.frame(FlatFrameSeen::Structural, FlatMonoReason::NoScene) != FlatStandDownEvent::Entered) {}
            bool warnedAfter = false;
            for (int i = 0; i < 200 && !warnedAfter; ++i) {
                later.frame(FlatFrameSeen::Structural, FlatMonoReason::NoTonePass);
                warnedAfter = later.machine.warningActive();
            }
            expect(warnedAfter && later.machine.reason() == FlatMonoReason::NoTonePass,
                   "...and once a scene arrives with a chain the structure declines, the warning comes with the probe that sees it");
        }
        // The render size: stands the work down after five seconds and warns, and the log line carries the words.
        {
            Sim sim;
            while (sim.frame(FlatFrameSeen::Structural, FlatMonoReason::RenderSize) != FlatStandDownEvent::Entered) {}
            expect(sim.machine.standing && sim.machine.warningActive() && sim.machine.reason() == FlatMonoReason::RenderSize,
                   "a render size that does not fit stands the work down and warns");
            char with[1300], without[1300], other[1300];
            flatStandDownFormatEntered(with, sizeof(with), 742, sim.machine, "Elite renders 2176x1224 on a 2560x1600 screen");
            flatStandDownFormatEntered(without, sizeof(without), 742, sim.machine, nullptr);
            const std::string withText = with;
            expect(withText.find("was refused for render-size-does-not-fit-output (Elite renders 2176x1224 on a 2560x1600 screen), none treated; paused: ") !=
                       std::string::npos &&
                   std::string(without).find("was refused for render-size-does-not-fit-output, none treated; paused: ") != std::string::npos,
                   "the stand-down line names the render size where the reason is the render size's");
            Sim chain;
            while (chain.frame(FlatFrameSeen::Structural, FlatMonoReason::NoTonePass) != FlatStandDownEvent::Entered) {}
            flatStandDownFormatEntered(other, sizeof(other), 742, chain.machine, "Elite renders 2176x1224 on a 2560x1600 screen");
            flatStandDownFormatEntered(without, sizeof(without), 742, chain.machine, nullptr);
            expect(std::string(other) == std::string(without),
                   "and only there: any other reason's line is exactly what it was, whatever sizes are on hand");
            // The reminder line every 30 s names the render size for both the entering reason and the last probe's.
            for (int i = 0; i < 1000; ++i) sim.frame(FlatFrameSeen::Structural, FlatMonoReason::RenderSize);
            char still[1300];
            flatStandDownFormatStill(still, sizeof(still), 9999, sim.machine, sim.now, "Elite renders 2176x1224 on a 2560x1600 screen");
            flatStandDownFormatStill(other, sizeof(other), 9999, sim.machine, sim.now, nullptr);
            expect(std::string(still).find("refused for render-size-does-not-fit-output (Elite renders 2176x1224 on a 2560x1600 screen)") != std::string::npos &&
                       std::string(still).find("(last probe: render-size-does-not-fit-output (Elite renders 2176x1224 on a 2560x1600 screen))") != std::string::npos &&
                       std::string(other).find("(last probe: render-size-does-not-fit-output)") != std::string::npos,
                   "the reminder line names the render size where it names the reason");
            char words[160];
            flatRenderSizeWords(words, sizeof(words), 2176, 1224, 2560, 1600);
            expect(std::string(words) == "Elite renders 2176x1224 on a 2560x1600 screen", "the render size's words");
        }
    }

    // ---- the census and the log lines ---------------------------------------------------------------------------------
    {
        FlatCopyWindow window;
        char line[1300];
        flatCopyFormatWindow(line, sizeof(line), true, window);
        expectText(line, "flat copy structure 5s: key=auto copies=0 whitelist=0 admitted=0 declined=0 selector-refused=0 no-scene=0 "
                                    "render-size=0 route-serves=0 key-off=0 last=none scene=0x0 output=0x0 source=0x0 VS=0000000000000000 "
                                    "PS=0000000000000000 ldr-passes-before-max=0 declines=none",
               "the window line prints with zeros when nothing ran: an absent line is what the admission never running looks like");
        for (int i = 0; i < 30; ++i) window.noteWhitelist();
        Build good;
        Built g = build(good);
        FlatCopyDiag diag;
        admit(g, policy(true, FlatMonoResolveMode::Dlss, true), &diag);
        for (int i = 0; i < 5; ++i) window.note(diag);
        Build aa; aa.llmBefore = 2; aa.consumerWritesS = false;
        Built a = build(aa);
        FlatCopyDiag declined;
        admit(a, policy(true, FlatMonoResolveMode::Dlss, true), &declined);
        for (int i = 0; i < 3; ++i) window.note(declined);
        Build shape; shape.sc.outW = 2560; shape.sc.outH = 1600; shape.sc.hW = 2176; shape.sc.hH = 1224;
        Built s = build(shape);
        FlatCopyDiag size;
        admit(s, autoDlss, &size);
        window.note(size);
        flatCopyFormatWindow(line, sizeof(line), true, window);
        expectText(line, "flat copy structure 5s: key=auto copies=39 whitelist=30 admitted=5 declined=3 selector-refused=0 no-scene=0 "
                                    "render-size=1 route-serves=0 key-off=0 last=render-size scene=2176x1224 output=2560x1600 source=3840x2160 "
                                    "VS=00000000000000F9 PS=00000000000000FE ldr-passes-before-max=2 "
                                    "declines=r-sized-image-passes-follow-the-first-consumer-of-the-scene-hdr:3",
               "a window with traffic: the whitelist's frames, the admissions, the declines by cause and the longest chain seen");
        expect(window.copies == 39, "every ruling is a copy");
        window.reset();
        flatCopyFormatWindow(line, sizeof(line), false, window);
        expect(std::string(line).find("key=off copies=0") != std::string::npos, "the key off says so in the line");
        // More causes than the table holds fall into 'other'.
        FlatCopyWindow many;
        const char* whys[] = {"a1", "a2", "a3", "a4", "a5", "a6", "a7", "a8"};
        for (const char* why : whys) { FlatCopyDiag d; d.outcome = FlatCopyOutcome::Declined; d.why = why; many.note(d); }
        flatCopyFormatWindow(line, sizeof(line), true, many);
        expect(std::string(line).find("declines=a1:1,a2:1,a3:1,a4:1,a5:1,a6:1,other:2") != std::string::npos, "causes past the table are counted as other");

        flatCopyFormatFirstAdmission(line, sizeof(line), 4711, diag, "trained-upscale");
        expectText(line, "flat copy structure: first admission at frame=4711 (experimental.temporal_aa_before_post=auto): the game's final "
                                    "copy reads a 3840x2160 R8G8B8A8 image written by one pass, VS=00000000000000F9 PS=00000000000000FE, after the "
                                    "scene HDR's first consumer (VS=00000000000000F9 PS=00000000000000FE), with no other R-sized image pass in "
                                    "between; no whitelisted tone pass wrote it (the whitelist said no-known-tone-pass); the scene is 3840x2160 on a "
                                    "3840x2160 output, route=trained-upscale; admitted by structure, so bloom, depth of field and the tone variant do "
                                    "not matter",
               "the first admission line, once a session");
        flatCopyFormatDeclined(line, sizeof(line), 4712, declined);
        expect(std::string(line).find("flat copy structure: declined at frame=4712: r-sized-image-passes-follow-the-first-consumer-of-the-scene-hdr "
                                      "(the whitelist said no-known-tone-pass); the final copy reads a 3840x2160 fmt 27 image with 1 writer(s)") == 0 &&
                   std::string(line).find("2 R-sized image pass(es) between the scene HDR's first consumer and it (2 draw(s))") != std::string::npos,
               "the decline line names the cause and the chain length");
    }

    // ---- the wiring no rig can run: the runtime's call, its order and the lines it prints ----------------------------------
    {
        const auto slurp = [&](const char* path) {
            std::vector<unsigned char> bytes;
            std::string text;
            if (hdr_route_test::readFile(fs::path(path), &bytes)) text.assign(bytes.begin(), bytes.end());
            return text;
        };
        const auto count = [](const std::string& text, const char* needle) {
            size_t n = 0, at = 0; const std::string s(needle);
            while ((at = text.find(s, at)) != std::string::npos) { ++n; at += s.size(); }
            return n;
        };
        const std::string runtime = slurp("src/d3d11/flat_runtime.cpp");
        const std::string model = slurp("src/d3d11/flat_runtime_model.h");
        const std::string contract = slurp("src/d3d11/flat_frame_contract.h");
        const std::string selector = slurp("src/d3d11/flat_mono_frame.h");
        expect(!runtime.empty() && !model.empty() && !contract.empty() && !selector.empty(), "the runtime and model sources are readable from the repo root");
        // The reducer, the contract and the whitelist's selector are what they were: none knows the admission.
        expect(count(model, "flatCopyAdmit") == 0 && count(model, "NoScene") == 0 && count(model, "RenderSize") == 0 &&
                   count(contract, "flatCopyAdmit") == 0 && count(selector, "flatCopyAdmit") == 0 &&
                   count(selector, "kFlatSceneMinDraws") == 0,
               "the reducer, the frame contract and the whitelist's selector do not mention the admission (the corpus hashes are the whitelist's)");
        // The call: once, at the copy draw, after the detector and the trace record and before the stand-down merge and the
        // treatment; the whitelist's own answer is what it is handed.
        const size_t record = runtime.find("flatTraceRecord(s.traceRing, d, foreignWork.load(std::memory_order_acquire), hdrSrvKnown ? hdrSrv : nullptr, s.prefix.sequence);");
        const size_t call = runtime.find("if (copy) { flatcpu::Scope reduce(flatcpu::kReduce); selected = copyAdmit(s, d, selected); }");
        const size_t merge = runtime.find("const FlatFrameSeen seen = flatFrameSeenFor(selected.selected(), selected.reason);");
        expect(count(runtime, "selected = copyAdmit(s, d, selected);") == 1 && record != std::string::npos && call != std::string::npos &&
                   merge != std::string::npos && record < call && call < merge,
               "the admission runs once, at the copy draw, after the detector and the trace record and before the stand-down merge");
        expect(count(runtime, "    FlatMonoFrame selected = [&]() -> FlatMonoFrame {") == 1,
               "the reducer's answer is the draw scope's own variable the admission replaces, not a second one");
        expect(count(runtime, "policy.structure = s.hdrKey == FlatHdrKey::Auto;") == 1 && count(runtime, "policy.mode = s.engine;") == 1 &&
                   count(runtime, "policy.routeLatched = s.hdrLatch.tripped;") == 1 &&
                   count(runtime, "flatCopyAdmit(s.prefix, s.hdr, d, whitelist, policy, &diag)") == 1,
               "the policy is the route's key, the mode and the latch; the admission reads the prefix and the detector as they stand");
        expect(count(runtime, "if (whitelist.selected()) s.copyWindow.noteWhitelist(); else s.copyWindow.note(diag);") == 1,
               "every ruling is counted, the whitelist's frames apart");
        expect(count(runtime, "flatCopyFormatWindow(copyText, sizeof(copyText), s.hdrKey == FlatHdrKey::Auto, s.copyWindow);\n"
                              "            Log::get().note(\"%s\", copyText);\n            s.copyWindow.reset();") == 1,
               "the census line prints every window while a temporal mode runs, and the window resets with it");
        expect(count(runtime, "flatCopyFormatFirstAdmission(") == 1 && count(runtime, "flatCopyFormatDeclined(") == 1 &&
                   count(runtime, "s.copyDeclineLines < 12") == 1,
               "the first admission is said once and the declines a dozen a session, each cause once");
        // The panel's inputs: published bits, the sizes, and the stand-down lines that carry them.
        expect(count(runtime, "(s.hdrKey == FlatHdrKey::Auto ? 4u : 0u) | (taaAbove ? 8u : 0u)") == 1 &&
                   count(runtime, "const bool taaAbove = known && s.engine == FlatMonoResolveMode::Taa && rw > ow && rh > oh;") == 1,
               "the refusal word carries the key (bit 2) and EDVR's TAA above the output (bit 3)");
        expect(count(runtime, "flatStandDownFormatEntered(text, sizeof(text), frame, s.standDown, renderSize);") == 1 &&
                   count(runtime, "flatStandDownFormatStill(text, sizeof(text), frame, s.standDown, now, renderSize);") == 1 &&
                   count(runtime, "flatRenderSizeWords(sizes, sizeof(sizes), rw, rh, ow, oh);") == 1,
               "the stand-down lines carry the render size's words");
        expect(count(runtime, "bool flatRuntimeStructureAdmission() {") == 1 && count(runtime, "bool flatRuntimeTaaAboveOutput() {") == 1 &&
                   count(runtime, "bool flatRuntimeSceneSizes(") == 1,
               "the three accessors the panel reads");
        // The key's own words say what auto does now, and what off still names.
        expect(count(runtime, "which admits the game's final copy by its structure") == 1 &&
                   count(runtime, "Log::get().note(\"flat hdr route: experimental.temporal_aa_before_post=%s%s at frame=%llu: %s\",") == 1,
               "the key's log line says the copy route admits by structure below the output");
        expect(count(runtime, "by the whitelist alone (a frame with no scene, or a render size that does not fit the output, is still named ") == 1,
               "the key off's log line says the whitelist decides, and that no-3d-scene and the render size are still named");
        // The startup spell is silent even when a handful of draws into an HDR-shaped target is a candidate: the route's no-consumer
        // verdict does not overwrite a frame the copy stage found no scene in.
        expect(count(runtime, "s.frameSeen <= FlatFrameSeen::Structural && s.frameReason != FlatMonoReason::NoScene) {") == 1,
               "hdrFrameEnd leaves a no-3d-scene frame as it is (the route's no-hdr-consumer would warn)");
        // The lines the fixture copies from the runtime's own format strings (the reader parses exactly these prefixes).
        expect(count(runtime, "\"flat runtime: treated=%llu refused=%llu last=%s render-source=game-SS jitter=(%.5g,%.5g) accepted-reset-5s=%llu ") == 1 &&
                   count(runtime, "\"flat runtime refusal 5s: reason=%s count=%llu\"") == 1 &&
                   count(runtime, "\"flat route: %s R=%ux%u E=%ux%u D=%ux%u%s\"") == 1,
               "the runtime's counter, refusal and route lines are the formats the fixture and the reader read");
        const std::string menu = slurp("src/d3d11/menu.cpp");
        expect(count(menu, "\"flat settings warning: hidden (the work is not stood down for the shape of \"\n"
                           "                        \"a post chain now, or the mode is off)\"") == 1,
               "the panel's hidden line is the one the fixture copies");
    }

    // ---- the fixture: what a good flight below the output writes, and three episodes ---------------------------------------
    {
        std::vector<unsigned char> bytes;
        std::string have;
        if (hdr_route_test::readFile(fs::path("tools/flat_upscale_fixture.log"), &bytes)) have.assign(bytes.begin(), bytes.end());
        for (size_t at; (at = have.find('\r')) != std::string::npos;) have.erase(at, 1);
        expect(!have.empty() && have == flatUpscaleFixtureText(),
               "tools\\flat_upscale_fixture.log is exactly what the formatters write (regenerate it: flat_temporal_test.exe --write-fixture "
               "tools\\flat_upscale_fixture.log)");
    }

    return failures;
}

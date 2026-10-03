// The HDR route's pure half on the rig (flat_hdr_route.h, design section 81): the trigger detector over synthetic
// streams and their mutations, the selector, the latch, the census lines, the trace v4 format, and the trace corpus.
// Two command-line modes share the replay below: --trace-chain prints what the detector finds in a trace, frame by
// frame (the dump behind the pins in section 81 and in this file, so they can be derived again), and --trace-trim
// writes a trace with only the named frames (how the fixtures of the four section-81 captures were cut).
#pragma once
#include "../../src/d3d11/flat_hdr_route.h"
#include "../../src/d3d11/flat_camera_phase.h"
#include "../../src/d3d11/flat_trace.h"
#include "../../src/d3d11/hdr_backend_flags.h"
#include "../../src/d3d11/flat_standdown.h"
#include "flat_standdown_tests.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace hdr_route_test {

// What the detector and the selector make of one trace frame, derived the way the runtime derives it: each draw through
// the prefix model, then the detector; writes, dispatches and camera captures between draws as their own events.
struct FrameFacts {
    uint64_t frame = 0;
    uint32_t outputWidth = 0, outputHeight = 0, events = 0, draws = 0;
    bool produced = false, copySelected = false, hashMatches = false;
    // The H candidate the detector settled on (at the trigger), with the events of its first and last draw.
    bool candidate = false;
    uint32_t hdrWidth = 0, hdrHeight = 0, hdrDraws = 0;
    uint32_t hdrFirstEvent = 0, hdrLastEvent = 0;
    const void* hdrResource = nullptr;
    uint32_t candidates = 0;
    bool triggered = false, ambiguous = false, srvKnown = false, newCandidateLate = false;
    uint32_t triggerEvent = 0, triggerSequence = 0, triggerAfterLast = 0;
    uint64_t triggerVs = 0, triggerPs = 0;
    uint32_t targetWidth = 0, targetHeight = 0, targetFormat = 0;
    uint32_t lateWrites = 0, lateDraws = 0, lateDispatches = 0, lateExplicit = 0;
    edvr::FlatMonoReason selection = edvr::FlatMonoReason::NoHdrConsumer;
    uint32_t resolveMarkers = 0;
    uint32_t markerEvent = 0, markerReason = 0;
};

inline bool isCopyDraw(const edvr::FlatRuntimeDraw& d, const edvr::FlatRuntimePrefix& p) {
    return d.key.vs == edvr::flat_mono_detail::kCopyVs && d.key.ps == edvr::flat_mono_detail::kCopyPs &&
           d.key.color == p.output;
}

// One parsed trace: per frame, the events in order.
struct ParsedFrame {
    edvr::FlatTraceFrameHeader header{};
    std::vector<edvr::FlatTraceEvent> events;
};
inline bool parseTrace(const std::vector<unsigned char>& bytes, std::vector<ParsedFrame>* out) {
    using namespace edvr;
    out->clear();
    return flatTraceParse(bytes.data(), bytes.size(),
        [&](const FlatTraceFrameHeader& h) { out->emplace_back(); out->back().header = h; },
        [&](const FlatTraceEvent& e) { out->back().events.push_back(e); });
}
inline bool readFile(const std::filesystem::path& path, std::vector<unsigned char>* bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    bytes->resize(static_cast<size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes->data()), std::streamsize(bytes->size()));
    return static_cast<bool>(file);
}
inline bool replayOverlayMarker(edvr::FlatRuntimePrefix& prefix, const edvr::FlatTraceEvent& e) {
    using namespace edvr;
    if (e.kind == kFlatTraceEventOverlayFailed) {
        flatRuntimeOverlayFailed(prefix,e.key.color);
        return true;
    }
    if (e.kind == kFlatTraceEventOverlaySeal) {
        flatRuntimeOverlaySeal(prefix,e.key.color);
        return true;
    }
    return false;
}

inline FrameFacts replayFrame(const ParsedFrame& frame) {
    using namespace edvr;
    FrameFacts f;
    const auto& h = frame.header;
    f.frame = h.frame; f.outputWidth = h.width; f.outputHeight = h.height; f.events = uint32_t(frame.events.size());
    f.produced = h.produced != 0;
    auto prefix = std::make_unique<FlatRuntimePrefix>();
    auto contract = std::make_unique<FlatFrameContract>();
    prefix->frame = h.frame; prefix->output = h.output; prefix->width = h.width; prefix->height = h.height;
    prefix->format = h.format;
    FlatHdrFrame hdr;
    flatHdrBeginFrame(hdr, h.width, h.height);
    bool selectionRun = false;
    for (uint32_t i = 0; i < frame.events.size(); ++i) {
        const FlatTraceEvent& e = frame.events[i];
        if (e.kind == kFlatTraceEventWriteResource) {
            flatRuntimeWritten(*prefix, e.key.color); flatHdrObserveExplicitWrite(hdr, e.key.color); continue;
        }
        if (e.kind == kFlatTraceEventDispatchWritten) {
            flatRuntimeDispatchObserveWritten(*prefix, e.key.color); flatHdrObserveDispatchWrite(hdr, e.key.color); continue;
        }
        if (e.kind == kFlatTraceEventMarkUncertain) { prefix->uncertain = true; continue; }
        if (e.kind == kFlatTraceEventCameraCapture) { ++prefix->sequence; continue; }
        if (e.kind == kFlatTraceEventResolve) { ++f.resolveMarkers; f.markerEvent = i; f.markerReason = e.key.count; continue; }
        if (replayOverlayMarker(*prefix,e)) continue;
        FlatRuntimeDraw d = flatTraceEventToDraw(e);
        if (e.flags & kFlatTraceForeignWork) prefix->uncertain = true;
        ++f.draws;
        if (isCopyDraw(d, *prefix)) flatRuntimeObserveContract(*prefix, d, *contract);
        else flatRuntimeObserve(*prefix, d);
        // The runtime resolves the four slots only for the draws the cheap gate passes; a trace that carries them
        // carries them for those draws, and the replay hands the detector the same.
        const bool srvKnown = (e.flags & kFlatTraceHdrSrvKnown) != 0 && flatHdrCouldConsume(hdr, d.key);
        const bool trigger = flatHdrObserveDraw(hdr, d.key, prefix->sequence, srvKnown ? e.hdrSrv : nullptr, srvKnown);
        if (trigger) {
            f.triggerEvent = i; f.triggerSequence = prefix->sequence;
            f.triggerVs = d.key.vs; f.triggerPs = d.key.ps;
            f.targetWidth = d.key.width; f.targetHeight = d.key.height; f.targetFormat = d.key.format;
            f.srvKnown = srvKnown;
            const FlatMonoFrame sel = flatSelectHdrRoute(*prefix, hdr, [](uint64_t, uint64_t) { return true; });
            f.selection = sel.reason; selectionRun = true;
        }
    }
    f.candidates = hdr.candidateCount;
    if (!selectionRun) f.selection = FlatMonoReason::NoHdrConsumer;
    f.triggered = hdr.triggered;
    f.ambiguous = hdr.triggered && hdr.trigger.ambiguous;
    const FlatHdrCandidate* c = hdr.triggered ? flatHdrFindCandidate(hdr, hdr.trigger.hdr)
                                              : (hdr.candidateCount ? &hdr.candidates[0] : nullptr);
    if (c) {
        f.candidate = true; f.hdrWidth = c->width; f.hdrHeight = c->height; f.hdrResource = c->resource;
        // The events of H's first and last draw before the trigger (the candidate's own sequence numbers are the
        // model's q, which also counts camera captures between draws, so the event indices come from a second pass).
        uint32_t first = ~0u, last = 0, draws = 0;
        for (uint32_t i = 0; i < frame.events.size(); ++i) {
            const FlatTraceEvent& e = frame.events[i];
            if (hdr.triggered && i >= f.triggerEvent) break;
            if (e.kind != kFlatTraceEventDraw || e.key.color != c->resource || e.key.format != 26 || !e.key.depth) continue;
            ++draws;
            if (first == ~0u) first = i;
            last = i;
        }
        f.hdrDraws = draws; f.hdrFirstEvent = first == ~0u ? 0 : first; f.hdrLastEvent = last;
        if (f.triggered) f.triggerAfterLast = f.triggerEvent - f.hdrLastEvent;
    }
    f.lateWrites = flatHdrLateWrites(hdr); f.lateDraws = hdr.lateDraws; f.lateDispatches = hdr.lateDispatches;
    f.lateExplicit = hdr.lateExplicit; f.newCandidateLate = hdr.lateNewCandidate;
    f.copySelected = contract->produced && contract->copiesUsed && contract->copies[0].selected();
    f.hashMatches = contract->produced == (h.produced != 0) &&
                    (!contract->produced || flatFrameContractHash(*contract) == h.contractHash);
    return f;
}

// --trace-chain <file>: one line per frame, and the pairs behind each trigger.
inline int traceChain(const char* path) {
    std::vector<unsigned char> bytes;
    std::vector<ParsedFrame> frames;
    if (!readFile(path, &bytes) || !parseTrace(bytes, &frames)) { std::printf("cannot read or parse %s\n", path); return 2; }
    for (const ParsedFrame& pf : frames) {
        const FrameFacts f = replayFrame(pf);
        std::printf("frame %llu: events=%u draws=%u output=%ux%u produced=%u copy-selected=%u candidates=%u\n",
            (unsigned long long)f.frame, f.events, f.draws, f.outputWidth, f.outputHeight, f.produced ? 1u : 0u,
            f.copySelected ? 1u : 0u, f.candidates);
        if (f.candidate)
            std::printf("  H %ux%u %p: %u draw(s), events %u..%u\n", f.hdrWidth, f.hdrHeight, f.hdrResource, f.hdrDraws,
                f.hdrFirstEvent, f.hdrLastEvent);
        if (f.triggered)
            std::printf("  trigger: event %u (+%u after H's last draw) seq %u VS=%016llX PS=%016llX target %ux%u fmt %u "
                        "srv=%s ambiguous=%u late-writes=%u selection=%s resolve-markers=%u\n",
                f.triggerEvent, f.triggerAfterLast, f.triggerSequence, (unsigned long long)f.triggerVs,
                (unsigned long long)f.triggerPs, f.targetWidth, f.targetHeight, f.targetFormat,
                f.srvKnown ? "known" : "not recorded", f.ambiguous ? 1u : 0u, f.lateWrites,
                edvr::flatMonoReasonName(f.selection), f.resolveMarkers);
        else
            std::printf("  no trigger (%s)\n", f.candidates ? "an H, no consumer" : "no H");
    }
    return 0;
}

// --trace-trim <in> <out> <frame[,frame...]>: the named frames, events and headers verbatim. The magic and every
// hash in the kept frame headers are unchanged, so a trimmed trace replays to the hashes it was recorded with.
inline int traceTrim(const char* in, const char* out, const char* list) {
    using namespace edvr;
    std::vector<unsigned char> bytes;
    if (!readFile(in, &bytes) || bytes.size() < sizeof(FlatTraceHeader)) { std::printf("cannot read %s\n", in); return 2; }
    FlatTraceHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    const bool v4 = std::memcmp(header.magic, "EDVRFTR4", 8) == 0;
    if (!v4 && std::memcmp(header.magic, "EDVRFTR3", 8) != 0) { std::printf("%s is not EDVRFTR3/4\n", in); return 2; }
    const size_t eventSize = v4 ? sizeof(FlatTraceEvent) : sizeof(FlatTraceEventV3);
    std::set<uint64_t> wanted;
    {
        std::stringstream ss(list);
        std::string item;
        while (std::getline(ss, item, ',')) if (!item.empty()) wanted.insert(std::stoull(item));
    }
    std::vector<unsigned char> kept;
    uint32_t frames = 0;
    size_t at = sizeof(FlatTraceHeader);
    for (uint32_t f = 0; f < header.frameCount; ++f) {
        if (bytes.size() - at < sizeof(FlatTraceFrameHeader)) { std::printf("%s is truncated\n", in); return 2; }
        FlatTraceFrameHeader fh{};
        std::memcpy(&fh, bytes.data() + at, sizeof(fh));
        const size_t span = sizeof(fh) + size_t(fh.eventCount) * eventSize;
        if (bytes.size() - at < span) { std::printf("%s is truncated\n", in); return 2; }
        if (wanted.count(fh.frame)) {
            kept.insert(kept.end(), bytes.begin() + at, bytes.begin() + at + span);
            ++frames;
        }
        at += span;
    }
    if (frames != wanted.size()) { std::printf("asked for %zu frame(s), found %u\n", wanted.size(), frames); return 1; }
    header.frameCount = frames;
    std::ofstream file(out, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(kept.data()), std::streamsize(kept.size()));
    if (!file) { std::printf("cannot write %s\n", out); return 2; }
    std::printf("wrote %s: %u frame(s), %zu bytes\n", out, frames, sizeof(header) + kept.size());
    return 0;
}

// ---- synthetic streams ---------------------------------------------------------------------------
constexpr uint64_t kFrame = 500;
inline const void* tok(uintptr_t v) { return reinterpret_cast<const void*>(v); }
// Real rows (Epic frame 71751, b1[270..275]): a shape cameraShape accepts.
constexpr float kRows[6][4] = {
    {.674860716f, -.714774430f, 0, .684166729f}, {-.804393589f, -.069881566f, 0, .664024174f},
    {-.240084499f, -1.77504551f, 0, -.301641792f}, {0, 0, .0250000004f, 0},
    {.684166729f, .664024174f, -.301641792f, 0}, {-21.0930309f, -24.5114784f, -1.11009693f, 0}};

// The resources of one scene: H and its depth, the quarter-size exposure target, the half-size bloom target, a
// full-size tone target, the game's output, another HDR-format target, and a copy of H.
struct Scene {
    const void* output = tok(0x100);
    const void* h = tok(0x1000);        // the scene HDR, R11G11B10F, with its own depth
    const void* hDepth = tok(0x1100);
    const void* quarter = tok(0x2000);  // exposure reduction
    const void* half = tok(0x2100);     // bloom's first level
    const void* tone = tok(0x2200);     // tone output, f27
    const void* h2 = tok(0x3000);       // a second HDR candidate
    const void* h2Depth = tok(0x3100);
    const void* copyOfH = tok(0x4000);  // the game's copy of H for refraction and the exposure reduction
    const void* probe = tok(0x5000);    // a 1024x1024 probe target with a depth of its own
    const void* probeDepth = tok(0x5100);
    const void* b1 = tok(0x6000);
    uint32_t outW = 3840, outH = 2160;
    uint32_t hW = 3840, hH = 2160;
};

struct Stream {
    std::unique_ptr<edvr::FlatRuntimePrefix> prefix = std::make_unique<edvr::FlatRuntimePrefix>();
    edvr::FlatHdrFrame hdr;
    Scene sc;
    unsigned char camera[edvr::kFlatCameraBytes]{};
    explicit Stream(const Scene& scene = Scene{}) : sc(scene) {
        std::memcpy(camera, kRows, sizeof(camera));
        prefix->frame = kFrame; prefix->output = sc.output; prefix->width = sc.outW; prefix->height = sc.outH;
        prefix->format = 28;
        edvr::flatHdrBeginFrame(hdr, sc.outW, sc.outH);
        ++prefix->sequence;   // the camera's capture is the first event of the frame: writeSeq 1
    }
    // A draw into `color`, with `depth` (null for none), in the shape the runtime builds it.
    edvr::FlatRuntimeDraw make(const void* color, const void* depth, uint32_t w, uint32_t h, uint32_t format,
                               uint64_t vs, uint64_t ps, bool withCamera, bool supported) {
        using namespace edvr;
        FlatRuntimeDraw d{};
        auto& k = d.key;
        k.color = color; k.rtv = tok(reinterpret_cast<uintptr_t>(color) + 1);
        k.depth = depth; k.dsv = depth ? tok(reinterpret_cast<uintptr_t>(depth) + 1) : nullptr;
        k.width = w; k.height = h; k.format = format;
        if (depth) { k.depthWidth = w; k.depthHeight = h; k.depthFormat = 19; }
        k.vs = vs; k.ps = ps; k.b1 = sc.b1;
        k.viewportCount = 1; k.viewport[2] = float(w); k.viewport[3] = float(h); k.viewport[5] = 1.0f;
        d.supported = supported; d.instances = 1;
        if (withCamera) {
            std::memcpy(d.camera, camera, sizeof(d.camera));
            k.camera = d.camera; k.cameraHash = flatCameraHash(d.camera);
            k.writeEpoch = kFrame; k.writeSeq = 1;
        }
        k.kind = flatContractKind(supported, color, depth, w, h, format, sc.outW, sc.outH, color == sc.output);
        return d;
    }
    // One draw in the runtime's order: the prefix model, then the detector, which is handed the four slots only when
    // its cheap gate asks for them. True at the trigger.
    bool draw(const edvr::FlatRuntimeDraw& made, const void* t0 = nullptr, const void* t1 = nullptr,
              const void* t2 = nullptr, const void* t3 = nullptr, bool slotsKnownAlways = false) {
        using namespace edvr;
        FlatRuntimeDraw d = made;
        if (d.key.camera) d.key.camera = d.camera;   // the pointer make() set names its own local
        flatRuntimeObserve(*prefix, d);
        const void* srv[4] = {t0, t1, t2, t3};
        const bool known = slotsKnownAlways || flatHdrCouldConsume(hdr, d.key);
        return flatHdrObserveDraw(hdr, d.key, prefix->sequence, known ? srv : nullptr, known);
    }
    bool drawBlind(const edvr::FlatRuntimeDraw& made) {   // an old trace: no slots recorded
        edvr::FlatRuntimeDraw d = made;
        if (d.key.camera) d.key.camera = d.camera;
        edvr::flatRuntimeObserve(*prefix, d);
        return edvr::flatHdrObserveDraw(hdr, d.key, prefix->sequence, nullptr, false);
    }
    void write(const void* resource) {
        edvr::flatRuntimeWritten(*prefix, resource); edvr::flatHdrObserveExplicitWrite(hdr, resource);
    }
    void dispatch(const void* uav) {
        edvr::flatRuntimeDispatchObserveWritten(*prefix, uav); edvr::flatHdrObserveDispatchWrite(hdr, uav);
    }
    // The scene's draws into H: the first few are pool-family draws (the motion source), the rest lighting and glass.
    void sceneDraws(uint32_t pool, uint32_t other) {
        for (uint32_t i = 0; i < pool; ++i)
            draw(make(sc.h, sc.hDepth, sc.hW, sc.hH, 26, 0xA1, 0xB1, true, true));
        for (uint32_t i = 0; i < other; ++i)
            draw(make(sc.h, sc.hDepth, sc.hW, sc.hH, 26, 0xA2 + i % 3, 0xB2, true, false));
    }
    // The game's exposure reduction: a quarter-size target reading H (quarter per axis, (iv) drops it).
    bool exposureReduction() {
        return draw(make(sc.quarter, nullptr, sc.hW / 4, sc.hH / 4, 26, 0xC1, 0xD1, false, false), sc.h);
    }
    bool toneTrigger(const void* slot1 = nullptr) {   // a full-size tone pass reading H at t1
        return draw(make(sc.tone, nullptr, sc.hW, sc.hH, 27, 0xF9, 0xFE, false, false), nullptr, slot1 ? slot1 : sc.h);
    }
    edvr::FlatMonoFrame select() {
        return edvr::flatSelectHdrRoute(*prefix, hdr, [](uint64_t, uint64_t) { return true; });
    }
};

}  // namespace hdr_route_test

inline int flatHdrRouteTests() {
    using namespace edvr;
    using namespace hdr_route_test;
    int failures = 0;
    auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: hdr route %s\n", name); ++failures; }
    };

    // ---- the key ---------------------------------------------------------------------------------
    expect(flatHdrKeyFromText("auto") == FlatHdrKey::Auto && flatHdrKeyFromText("AUTO") == FlatHdrKey::Auto &&
           flatHdrKeyFromText("Auto") == FlatHdrKey::Auto,
           "the key reads auto in any case");
    expect(flatHdrKeyFromText("off") == FlatHdrKey::Off && flatHdrKeyFromText("") == FlatHdrKey::Off &&
           flatHdrKeyFromText(nullptr) == FlatHdrKey::Off && flatHdrKeyFromText("on") == FlatHdrKey::Off &&
           flatHdrKeyFromText("autos") == FlatHdrKey::Off && flatHdrKeyFromText("aut") == FlatHdrKey::Off &&
           flatHdrKeyFromText(" auto") == FlatHdrKey::Off,
           "a value that is not auto reads as off: a typo leaves the copy route, it never turns the route on by accident");

    // ---- the backend flags, as pure functions ---------------------------------------------------------
    // The values are the SDKs' own (dlaa.cpp and fsr3_engine.cpp static_assert them against the headers); what is pinned
    // here is which bits each route sets, and that the LDR sets are what they always were.
    expect(flatDlssCreateFlags(false) == (1u << 1 | 1u << 3) && flatDlssCreateFlags(false) == 0x0Au,
           "DLSS LDR flags stay MVLowRes | DepthInverted, bit for bit");
    expect(flatDlssCreateFlags(true) == (1u << 0 | 1u << 1 | 1u << 3 | 1u << 6) && flatDlssCreateFlags(true) == 0x4Bu &&
           (flatDlssCreateFlags(true) & kDlssFlagMvJittered) == 0,
           "DLSS HDR adds IsHDR and AutoExposure, keeps MVLowRes and DepthInverted, and never sets MVJittered");
    expect(flatFsrCreateFlags(false, false, false) == (1u << 3) &&
           flatFsrCreateFlags(true, false, false) == (1u << 3 | 1u << 4) &&
           flatFsrCreateFlags(true, true, false) == (1u << 3 | 1u << 4 | 1u << 8) &&
           flatFsrCreateFlags(false, true, false) == (1u << 3 | 1u << 8),
           "FSR LDR flags stay DEPTH_INVERTED plus infinite depth and debug checking where asked, bit for bit");
    {
        bool only = true;
        for (int infinite = 0; infinite < 2; ++infinite)
            for (int debug = 0; debug < 2; ++debug) {
                const uint32_t ldr = flatFsrCreateFlags(infinite != 0, debug != 0, false);
                const uint32_t hdr = flatFsrCreateFlags(infinite != 0, debug != 0, true);
                only = only && hdr == (ldr | kFsrFlagHighDynamicRange | kFsrFlagAutoExposure) &&
                       (ldr & (kFsrFlagHighDynamicRange | kFsrFlagAutoExposure)) == 0;
            }
        expect(only, "FSR HDR adds exactly the HDR and auto-exposure bits to every LDR combination");
    }

    // ---- the detector, on a canonical frame (the star with bloom on, in miniature) ------------------------
    {
        Stream s;
        s.write(s.sc.h);                               // the clear that opens the frame
        s.sceneDraws(3, 5);
        expect(!s.exposureReduction(), "the exposure reduction at a quarter per axis is not the consumer");
        s.sceneDraws(0, 4);                            // the late draws the reduction runs ahead of
        // A half-size draw that does not read H (bloom's later levels read their own chain): (iii) skips it.
        expect(!s.draw(s.make(s.sc.half, nullptr, 1920, 1080, 26, 0xE1, 0xE2, false, false), s.sc.quarter),
               "a half-size pass that does not read H is not the consumer");
        expect(s.hdr.candidateCount == 1 && !s.hdr.triggered, "one candidate, no trigger yet");
        // Bloom's first level: exactly half per axis, H at t0.
        const bool trigger = s.draw(s.make(s.sc.half, nullptr, 1920, 1080, 26, 0xDF, 0xC1, false, false), s.sc.h);
        expect(trigger && s.hdr.triggered, "the first half-or-larger draw that reads H is the trigger");
        expect(s.hdr.trigger.hdr == s.sc.h && s.hdr.trigger.srvSlot == 0 && s.hdr.trigger.srvKnown &&
               s.hdr.trigger.targetWidth == 1920 && s.hdr.trigger.targetHeight == 1080 && !s.hdr.trigger.ambiguous &&
               s.hdr.trigger.vs == 0xDF && s.hdr.trigger.ps == 0xC1,
               "the trigger names H, the slot it is bound at, the pair and the target");
        expect(!s.toneTrigger() && s.hdr.trigger.targetWidth == 1920, "a later consumer does not move the trigger");
        expect(flatHdrLateWrites(s.hdr) == 0 && flatHdrFrameVerdict(s.hdr) == FlatHdrFrameVerdict::Trigger,
               "no write after the trigger, verdict trigger");
        const FlatMonoFrame sel = s.select();
        expect(sel.selected() && sel.color == s.sc.h && sel.hdr == s.sc.h && sel.depth == s.sc.hDepth &&
               sel.renderWidth == 3840 && sel.renderHeight == 2160 && sel.outputWidth == 3840 &&
               sel.toneSequence == s.hdr.trigger.sequence && sel.copySequence == 0 &&
               sel.sceneConstants == s.sc.b1 && sel.supportedDraws == 3,
               "the selector takes the frame: H, its depth and camera, three pool sources, the trigger's place");
    }

    // ---- rule (iv): a target at least half of H per axis, one pixel of slack for an odd extent ---------------
    {
        struct Row { uint32_t hW, hH, w, h; bool fires; };
        const Row rows[] = {
            {3840, 2160, 1920, 1080, true},   // bloom's first level, exactly half
            {3840, 2160, 1919, 1079, false},  // a pixel under half on both axes
            {3840, 2160, 1920, 1079, false},  // one axis short is enough to drop it
            {3841, 2161, 1920, 1080, true},   // an odd extent halves down: still the first level
            {3840, 2160, 960, 540, false},    // the exposure reduction's quarter
            {3840, 2160, 3840, 2160, true},   // the tone at full scale
            {3840, 2160, 3840, 1080, true},   // wider than half on both axes is enough
            {5040, 2835, 630, 354, false},    // the VR frame's eighth-size downsample
            {5040, 2835, 2520, 1418, true},   // and its half
        };
        bool all = true;
        for (const Row& r : rows) {
            Scene sc; sc.hW = r.hW; sc.hH = r.hH; sc.outW = r.hW; sc.outH = r.hH;
            Stream s(sc);
            s.sceneDraws(1, 2);
            const bool fired = s.draw(s.make(sc.half, nullptr, r.w, r.h, 26, 0xDF, 0xC1, false, false), sc.h);
            all = all && fired == r.fires;
        }
        expect(all, "rule (iv): half per axis with a pixel of slack fires, a quarter or one short axis does not");
    }

    // ---- rules (i) and (ii) and (iii) one at a time -----------------------------------------------------
    {
        Stream s;
        s.sceneDraws(1, 2);
        expect(!s.draw(s.make(s.sc.tone, s.sc.probe, 3840, 2160, 27, 1, 2, false, false), s.sc.h),
               "(i) a draw with a depth bound is not the consumer, however large and whatever it reads");
        expect(!s.draw(s.make(s.sc.h, nullptr, 3840, 2160, 26, 1, 2, false, false), s.sc.h),
               "(ii) a draw into H itself is not the consumer");
        expect(!s.draw(s.make(s.sc.tone, nullptr, 3840, 2160, 27, 1, 2, false, false), nullptr, nullptr, nullptr, nullptr),
               "(iii) a full-size pass that reads nothing of H is not the consumer");
        expect(!s.draw(s.make(s.sc.tone, nullptr, 3840, 2160, 27, 1, 2, false, false), s.sc.copyOfH, s.sc.quarter, s.sc.half),
               "(iii) a pass that reads a COPY of H (the VR frame's exposure chain) is not the consumer either");
        expect(!s.hdr.triggered, "none of the four fired");
    }
    for (uint32_t slot = 0; slot < 4; ++slot) {
        Stream s;
        s.sceneDraws(1, 2);
        const void* srv[4] = {nullptr, nullptr, nullptr, nullptr};
        srv[slot] = s.sc.h;
        const bool fired = s.draw(s.make(s.sc.tone, nullptr, 3840, 2160, 27, 1, 2, false, false), srv[0], srv[1], srv[2], srv[3]);
        expect(fired && s.hdr.trigger.srvSlot == slot, "H bound at any of t0..t3 is a consumer, and the slot is named");
    }
    {
        Stream s;
        s.sceneDraws(1, 2);
        expect(!s.draw(s.make(s.sc.tone, nullptr, 3840, 2160, 27, 1, 2, false, false)),
               "H bound past t3 (not tracked) is not a consumer the shadow can see: no trigger, no false positive");
    }

    // ---- extents: a probe target with a depth is not an H candidate ---------------------------------------
    {
        Stream s;
        s.draw(s.make(s.sc.probe, s.sc.probeDepth, 1024, 1024, 26, 1, 2, true, false));
        expect(s.hdr.candidateCount == 0, "a 1024x1024 R11G11B10F target with a depth of its own is not the scene's H");
        s.draw(s.make(s.sc.h, nullptr, 3840, 2160, 26, 1, 2, false, false));
        expect(s.hdr.candidateCount == 0, "an HDR draw with no depth bound is not a candidate");
        s.draw(s.make(s.sc.h, s.sc.hDepth, 3840, 2160, 23, 1, 2, true, false));
        expect(s.hdr.candidateCount == 0, "a format-23 scene target is not R11G11B10F: no candidate (the model's own rule)");
    }

    // ---- MUTATION: a half-size non-consumer between H writes fires early without rule (iii) ------------------
    {
        // Old trace behaviour (no SRVs recorded): a full-size draw with no depth between two H writes that reads
        // nothing of H is taken for the consumer. With the SRVs it is skipped, and the real consumer fires.
        auto run = [&](bool known, uint32_t* where, uint32_t* real) {
            Stream s;
            s.sceneDraws(2, 3);
            const uint32_t before = s.prefix->sequence;
            const auto impostor = s.make(s.sc.half, nullptr, 1920, 1080, 26, 0xE1, 0xE2, false, false);
            bool fired = known ? s.draw(impostor, s.sc.quarter, nullptr, nullptr, nullptr, true) : s.drawBlind(impostor);
            *where = fired ? s.prefix->sequence - before : 0;
            s.sceneDraws(0, 4);
            const auto tone = s.make(s.sc.tone, nullptr, 3840, 2160, 27, 0xF9, 0xFE, false, false);
            fired = known ? s.draw(tone, nullptr, s.sc.h, nullptr, nullptr, true) : s.drawBlind(tone);
            *real = (fired ? 1u : 0u) + (s.hdr.triggered ? 10u : 0u) + flatHdrLateWrites(s.hdr) * 100;
        };
        uint32_t early = 0, realEarly = 0, where = 0, realKnown = 0;
        run(false, &early, &realEarly);
        run(true, &where, &realKnown);
        expect(early == 1 && realEarly == 10 + 400, "without (iii) the impostor fires the trigger and the four later H draws count as late writes");
        expect(where == 0 && realKnown == 11, "with (iii) the impostor is skipped, the real consumer fires, and nothing is late");
    }

    // ---- MUTATION: a late H write, in each of its three forms --------------------------------------------
    {
        Stream s;
        s.write(s.sc.h);
        s.sceneDraws(2, 3);
        s.toneTrigger();
        expect(s.hdr.triggered && flatHdrLateWrites(s.hdr) == 0, "baseline: triggered, no late write");
        s.draw(s.make(s.sc.tone, nullptr, 3840, 2160, 27, 7, 8, false, false), s.sc.h);   // another read: not a write
        s.write(s.sc.copyOfH);                                                            // the game's copy: another resource
        s.dispatch(s.sc.quarter);                                                         // a dispatch elsewhere
        expect(flatHdrLateWrites(s.hdr) == 0, "a later read of H, a write to a copy and a dispatch elsewhere are not late writes");
        s.draw(s.make(s.sc.h, s.sc.hDepth, 3840, 2160, 26, 0x81, 0x82, true, false));
        expect(s.hdr.lateDraws == 1 && s.hdr.lateNamed && s.hdr.lateVs == 0x81 && s.hdr.latePs == 0x82 &&
               s.hdr.lateTargetWidth == 3840 && !s.hdr.lateNewCandidate,
               "a draw into H after the trigger is a late write, named once by its pair");
        s.draw(s.make(s.sc.h, s.sc.hDepth, 3840, 2160, 26, 0x91, 0x92, true, false));
        s.dispatch(s.sc.h);
        s.write(s.sc.h);
        expect(s.hdr.lateDraws == 2 && s.hdr.lateDispatches == 1 && s.hdr.lateExplicit == 1 &&
               flatHdrLateWrites(s.hdr) == 4 && s.hdr.lateVs == 0x81,
               "a second draw, a dispatch UAV and an explicit write are counted too; the first pair stays the named one");
        // A draw into H that is not shaped like the scene draws (no depth) is a late write all the same.
        s.draw(s.make(s.sc.h, nullptr, 3840, 2160, 26, 0xA1, 0xA2, false, false));
        expect(s.hdr.lateDraws == 3, "a depthless draw into H after the trigger is late as well");
    }

    // ---- MUTATION: two H candidates; H changing identity mid-frame -------------------------------------------
    {
        // Two scene-shaped HDR targets, no slots recorded: the consumer cannot say which it reads.
        Stream s;
        s.sceneDraws(2, 2);
        s.draw(s.make(s.sc.h2, s.sc.h2Depth, 3840, 2160, 26, 0xA3, 0xB3, true, false));
        s.drawBlind(s.make(s.sc.tone, nullptr, 3840, 2160, 27, 0xF9, 0xFE, false, false));
        expect(s.hdr.candidateCount == 2 && s.hdr.triggered && s.hdr.trigger.ambiguous,
               "two H candidates with no slots recorded: the trigger fires and is marked ambiguous");
        const FlatMonoFrame sel = s.select();
        expect(!sel.selected() && sel.reason == FlatMonoReason::ConflictingHdr &&
               flatFrameSeenFor(false, sel.reason) == FlatFrameSeen::Transient,
               "an ambiguous trigger is refused as conflicting-hdr, which is transient (the copy route serves the frame)");
    }
    {
        // With the slots recorded the consumer names the one it reads, and a bystander candidate is no ambiguity...
        Stream s;
        s.sceneDraws(2, 2);
        s.draw(s.make(s.sc.h2, s.sc.h2Depth, 3840, 2160, 26, 0xA3, 0xB3, true, false));
        const bool fired = s.toneTrigger();
        expect(fired && !s.hdr.trigger.ambiguous && s.hdr.trigger.hdr == s.sc.h,
               "with the slots recorded the consumer names the one candidate it reads");
        // ...but a consumer that reads both is.
        Stream t;
        t.sceneDraws(2, 2);
        t.draw(t.make(t.sc.h2, t.sc.h2Depth, 3840, 2160, 26, 0xA3, 0xB3, true, false));
        t.draw(t.make(t.sc.tone, nullptr, 3840, 2160, 27, 0xF9, 0xFE, false, false), t.sc.h2, t.sc.h);
        expect(t.hdr.triggered && t.hdr.trigger.ambiguous, "a consumer that reads both candidates is ambiguous");
        // A fifth candidate overflows the table and nothing can be named.
        Stream u;
        for (uintptr_t i = 0; i < 6; ++i)
            u.draw(u.make(tok(0x7000 + i * 16), tok(0x7100 + i * 16), 3840, 2160, 26, 1, 2, true, false));
        expect(u.hdr.candidateCount == kFlatHdrCandidates && u.hdr.candidateOverflow,
               "more candidates than the table holds is remembered as overflow");
        u.drawBlind(u.make(u.sc.tone, nullptr, 3840, 2160, 27, 0xF9, 0xFE, false, false));
        expect(u.hdr.triggered && u.hdr.trigger.ambiguous, "and makes the trigger ambiguous");
    }
    {
        // H changing identity AFTER the trigger: a scene-shaped draw into a target that did not exist at the trigger.
        Stream s;
        s.sceneDraws(2, 2);
        s.toneTrigger();
        s.draw(s.make(s.sc.h2, s.sc.h2Depth, 3840, 2160, 26, 0xA3, 0xB3, true, false));
        expect(s.hdr.lateDraws == 1 && s.hdr.lateNewCandidate && flatHdrLateWrites(s.hdr) == 1,
               "a scene-shaped HDR draw into a target that appeared after the trigger is a late write into a new candidate");
    }

    // ---- MUTATION: H copied mid-frame (the VR frame's CopySubresourceRegion at q 8157) -------------------------
    {
        Stream s;
        s.write(s.sc.h);
        s.sceneDraws(3, 4);
        // The copy's destination is another resource; its source is invisible to the runtime (only destinations are
        // reported). The exposure chain then reads the COPY, and so do the refraction particles.
        s.write(s.sc.copyOfH);
        expect(!s.draw(s.make(s.sc.quarter, nullptr, 960, 540, 26, 0xC1, 0xD1, false, false), s.sc.copyOfH) &&
               !s.draw(s.make(s.sc.tone, nullptr, 3840, 2160, 27, 0xA1, 0xC6, false, false), s.sc.copyOfH, s.sc.quarter),
               "a copy of H is not a consumer, and neither are the passes that read the copy");
        s.sceneDraws(0, 6);   // the late draws the copy precedes
        expect(!s.hdr.triggered && flatHdrFrameVerdict(s.hdr) == FlatHdrFrameVerdict::NoTrigger,
               "no trigger until something reads H itself");
        expect(s.toneTrigger() && flatHdrLateWrites(s.hdr) == 0, "the tone pass reading H is the trigger, with no late write");
    }

    // ---- verdicts: a 2D menu frame, a frame with no consumer, one consumer only --------------------------------
    {
        Stream menu;
        menu.draw(menu.make(menu.sc.tone, nullptr, 3840, 2160, 27, 1, 2, false, false));
        menu.draw(menu.make(menu.sc.output, nullptr, 3840, 2160, 28, 3, 4, false, false));
        expect(flatHdrFrameVerdict(menu.hdr) == FlatHdrFrameVerdict::NoHdr && menu.hdr.candidateCount == 0,
               "a 2D menu frame (no HDR target with a depth) has no H: verdict no-hdr");
        Stream lone;
        lone.sceneDraws(2, 3);
        expect(flatHdrFrameVerdict(lone.hdr) == FlatHdrFrameVerdict::NoTrigger, "an H and no consumer: verdict no-trigger");
        const FlatMonoFrame sel = lone.select();
        expect(!sel.selected() && sel.reason == FlatMonoReason::NoHdrConsumer &&
               flatFrameSeenFor(false, sel.reason) == FlatFrameSeen::Structural,
               "no consumer is the chain-shape refusal no-hdr-consumer: structural");
    }

    // ---- the selector, refusal by refusal ------------------------------------------------------------------
    {
        // R < D: upscaling keeps the copy route and its whitelist (decision (c)): a transient refusal, never structural.
        Scene lowSc; lowSc.hW = 2496; lowSc.hH = 1404;
        Stream low(lowSc); low.sceneDraws(2, 2); low.toneTrigger();
        const FlatMonoFrame lowSel = low.select();
        expect(!lowSel.selected() && lowSel.reason == FlatMonoReason::HdrExtent && !flatMonoReasonStructural(lowSel.reason) &&
               flatFrameSeenFor(false, lowSel.reason) == FlatFrameSeen::Transient,
               "R < D: hdr-route-needs-render-at-least-output, transient");
        expect(lowSel.renderWidth == 2496 && lowSel.renderHeight == 1404 && lowSel.outputWidth == 3840 &&
               lowSel.outputHeight == 2160,
               "the extent refusal carries the measured render and output sizes (the census line's last-trigger fields read them)");
        // The sizes the runtime publishes for the panel (the copy admission's scene facts, section 83) pack into one word.
        {
            const uint32_t rW = lowSel.renderWidth, rH = lowSel.renderHeight, dW = lowSel.outputWidth, dH = lowSel.outputHeight;
            uint32_t a = 0, b = 0, c = 0, d = 0;
            expect(flatHdrUnpackSizes(flatHdrPackSizes(rW, rH, dW, dH), &a, &b, &c, &d) && a == rW && b == rH && c == dW && d == dH &&
                       !flatHdrUnpackSizes(0, &a, &b, &c, &d) && flatHdrPackSizes(0, rH, dW, dH) == 0 &&
                       flatHdrPackSizes(70000, rH, dW, dH) == 0,
                   "the four sizes pack into one word and back; zero and oversized sizes publish nothing");
        }
        Scene superSc; superSc.hW = 5760; superSc.hH = 3240;   // supersampling 1.5: R > D is the route's own ground
        Stream super(superSc); super.sceneDraws(2, 2); super.toneTrigger();
        expect(super.select().selected() && super.select().renderWidth == 5760 && super.select().outputWidth == 3840,
               "R = 1.5 D is selected, with the render and output sizes of each");
        Scene hugeSc; hugeSc.hW = 7681; hugeSc.hH = 4321;      // past twice the output: no scene extent
        hugeSc.outW = 3840; hugeSc.outH = 2160;
        Stream huge(hugeSc); huge.sceneDraws(2, 2); huge.toneTrigger();
        expect(huge.hdr.candidateCount == 0 && !huge.select().selected(),
               "a target past twice the output is no candidate and no frame");
        Scene wildSc; wildSc.hW = 3840; wildSc.hH = 3000;      // the wrong aspect: no uniform scale
        Stream wild(wildSc); wild.sceneDraws(2, 2); wild.toneTrigger();
        expect(!wild.select().selected(), "a target off the output's aspect is refused");

        Stream noPool; noPool.sceneDraws(0, 4); noPool.toneTrigger();   // H without a pool-family source
        expect(noPool.select().reason == FlatMonoReason::NoSupportedSource,
               "no supported source: no-supported-motion-source-pair");
        Stream noCam;
        for (int i = 0; i < 2; ++i) noCam.draw(noCam.make(noCam.sc.h, noCam.sc.hDepth, 3840, 2160, 26, 0xA1, 0xB1, false, true));
        noCam.toneTrigger();
        expect(!noCam.select().selected() && noCam.select().reason == FlatMonoReason::NoHdrCamera,
               "H drawn with no camera at all: hdr-camera-not-observed");
        Stream uncertain; uncertain.sceneDraws(2, 2);
        uncertain.prefix->uncertain = true;
        uncertain.toneTrigger();
        expect(uncertain.select().reason == FlatMonoReason::Truncated, "an uncertain prefix (foreign work, ClearState) is truncated");
        Stream written; written.sceneDraws(2, 2);
        written.write(written.sc.h);                          // an explicit write into H after its draws: the model marks it bad
        written.toneTrigger();
        expect(written.select().reason == FlatMonoReason::ConflictingHdr,
               "an explicit write into H between its draws is a conflict the model already records");
        Stream late; late.sceneDraws(2, 2); late.toneTrigger();
        late.draw(late.make(late.sc.h, late.sc.hDepth, 3840, 2160, 26, 0xA1, 0xB1, true, true));
        expect(late.hdr.lateDraws == 1 && late.select().reason == FlatMonoReason::WrongOrder,
               "a pool source drawn after the trigger is out of order");
        // A second depth with a supported pair under the same camera: which source is the motion source is ambiguous.
        Stream twoDepths; twoDepths.sceneDraws(2, 2);
        twoDepths.draw(twoDepths.make(twoDepths.sc.h2, twoDepths.sc.h2Depth, 3840, 2160, 26, 0xA1, 0xB1, true, true));
        twoDepths.toneTrigger();
        expect(twoDepths.select().reason == FlatMonoReason::AmbiguousSource || !twoDepths.select().selected(),
               "a second supported source depth under the same camera is ambiguous");
    }

    // A protected divergent-camera draw may open a late overlay suffix, but
    // every following HDR write must remain protected. The model must retain
    // conflicts that occurred before the suffix opened.
    {
        auto late = [](Stream& s, bool protectedDraw) {
            auto d = s.make(s.sc.h, s.sc.hDepth, s.sc.hW, s.sc.hH, 26,
                            0x12345678, 0x87654321, true, false);
            d.camera[0] ^= 1;
            d.key.cameraHash = flatCameraHash(d.camera);
            d.overlayProtected = protectedDraw;
            s.draw(d);
        };
        auto target = [](const Stream& s) -> const FlatRuntimeTarget* {
            for (uint32_t i = 0; i < s.prefix->targetsUsed; ++i)
                if (s.prefix->targets[i].resource == s.sc.h) return &s.prefix->targets[i];
            return nullptr;
        };
        Stream accepted; accepted.sceneDraws(2, 2); late(accepted, true);
        const auto* a = target(accepted);
        expect(a && a->overlayOpen && a->overlayDepth == accepted.sc.hDepth && !a->hdrBad,
               "protected alternate-camera HDR draw opens a clean overlay suffix");
        late(accepted, true);
        a = target(accepted);
        expect(a && a->overlayOpen && !a->hdrBad,
               "a second protected draw may extend the same overlay suffix");
        accepted.toneTrigger();
        expect(accepted.select().selected(),
               "the real HDR consumer selector admits a protected suffix with the original world camera");

        Stream unprotected; unprotected.sceneDraws(2, 2); late(unprotected, true);
        unprotected.draw(unprotected.make(unprotected.sc.h, unprotected.sc.hDepth,
            unprotected.sc.hW, unprotected.sc.hH, 26, 0x44, 0x55, true, false));
        const auto* u = target(unprotected);
        expect(u && u->hdrBad && u->firstBad.cause == FlatRuntimeConflict::OverlaySuffix,
               "unprotected later HDR draw makes the suffix sticky-bad");
        unprotected.toneTrigger();
        expect(unprotected.select().reason == FlatMonoReason::ConflictingHdr,
               "unprotected suffix is refused by the actual HDR consumer selector");

        Stream colorWrite; colorWrite.sceneDraws(2, 2); late(colorWrite, true);
        colorWrite.write(colorWrite.sc.h);
        const auto* c = target(colorWrite);
        expect(c && c->hdrBad && c->firstBad.cause == FlatRuntimeConflict::ExplicitWrite,
               "explicit HDR copy, clear, update, or map invalidates the suffix");
        colorWrite.toneTrigger();
        expect(colorWrite.select().reason == FlatMonoReason::ConflictingHdr,
               "explicit HDR write is refused by the actual HDR consumer selector");

        Stream depthWrite; depthWrite.sceneDraws(2, 2); late(depthWrite, true);
        depthWrite.write(depthWrite.sc.hDepth);
        const auto* z = target(depthWrite);
        expect(z && z->hdrBad && z->firstBad.cause == FlatRuntimeConflict::OverlaySuffix,
               "explicit scene-depth write invalidates the suffix");
        depthWrite.toneTrigger();
        expect(depthWrite.select().reason == FlatMonoReason::ConflictingHdr,
               "scene-depth write is refused by the actual HDR consumer selector");

        Stream drawDepth; drawDepth.sceneDraws(2, 2); late(drawDepth, true);
        auto depthOnly = drawDepth.make(drawDepth.sc.h2, drawDepth.sc.hDepth,
            drawDepth.sc.hW, drawDepth.sc.hH, 26, 0x51, 0x52, false, false);
        depthOnly.effectiveDepthWrite = true;
        drawDepth.draw(depthOnly);
        z = target(drawDepth);
        expect(z && z->hdrBad && z->firstBad.cause == FlatRuntimeConflict::OverlaySuffix,
               "a different-color draw writing scene depth invalidates the suffix");
        drawDepth.toneTrigger();
        expect(drawDepth.select().reason == FlatMonoReason::ConflictingHdr,
               "different-color scene-depth writer is refused by the HDR consumer selector");

        Stream readOnlyDepth; readOnlyDepth.sceneDraws(2, 2); late(readOnlyDepth, true);
        auto depthRead = readOnlyDepth.make(readOnlyDepth.sc.h2, readOnlyDepth.sc.hDepth,
            readOnlyDepth.sc.hW, readOnlyDepth.sc.hH, 26, 0x51, 0x52, false, false);
        depthRead.effectiveDepthWrite = false;
        readOnlyDepth.draw(depthRead);
        z = target(readOnlyDepth);
        expect(z && !z->hdrBad,
               "a depth-read-only draw on another target does not invent a scene-depth write");
        readOnlyDepth.toneTrigger();
        expect(readOnlyDepth.select().selected(),
               "depth-read-only work leaves the original HDR consumer eligible");

        Stream prior; prior.sceneDraws(2, 2); late(prior, false);
        const auto* p = target(prior);
        const auto first = p ? p->firstBad.cause : FlatRuntimeConflict::None;
        late(prior, true);
        p = target(prior);
        expect(p && first == FlatRuntimeConflict::CameraChange && p->hdrBad &&
               p->firstBad.cause == first && !p->overlayOpen,
               "protection cannot erase an earlier camera conflict");
        prior.toneTrigger();
        expect(prior.select().reason == FlatMonoReason::ConflictingHdr,
               "earlier camera conflict remains refused at the HDR consumer");
    }

    // The projection proof measures the uploaded rows, independently of VS/PS
    // identity. Scale and near may differ, but both centres and the pose must
    // agree with this frame's raster phase.
    {
        constexpr uint32_t w=3840, h=2160;
        FlatProjectionJitter phase{};
        expect(flatProjectionJitter(.25f,-.25f,w,h,phase),
               "nonzero phase has a finite expected NDC shift");
        float world[6][4]{}, overlay[6][4]{};
        world[0][0]=1.2f;world[1][1]=1.5f;world[2][3]=1;
        world[2][0]=phase.ndcX;world[2][1]=phase.ndcY;world[3][2]=.025f;
        std::memcpy(overlay,world,sizeof(world));
        overlay[0][0]=1.8f;overlay[1][1]=2.1f;overlay[3][2]=.0675f;
        expect(flatCameraCenteredPairAtPhase(world,overlay,.25f,-.25f,w,h),
               "alternate FOV and near with the same nonzero phase and pose is admitted");
        overlay[2][0]+=1.e-4f;
        expect(!flatCameraCenteredPairAtPhase(world,overlay,.25f,-.25f,w,h),
               "off-centre alternate projection is refused");
        overlay[2][0]=phase.ndcX;overlay[2][1]=0;
        expect(!flatCameraCenteredPairAtPhase(world,overlay,.25f,-.25f,w,h),
               "unphased alternate projection is refused when the frame is jittered");
        overlay[2][1]=phase.ndcY;overlay[4][0]=1;
        expect(!flatCameraCenteredPairAtPhase(world,overlay,.25f,-.25f,w,h),
               "different camera pose is refused");
        overlay[4][0]=0;overlay[2][3]=0;
        expect(!flatCameraCenteredPairAtPhase(world,overlay,.25f,-.25f,w,h),
               "malformed forward row is refused");
        overlay[2][3]=1;world[2][0]=overlay[2][0]=0;world[2][1]=overlay[2][1]=0;
        expect(flatCameraCenteredPairAtPhase(world,overlay,0,0,w,h),
               "centered zero-phase alternate projection is admitted");
    }

    // ---- the latch: three treated frames with late writes turn the route off ------------------------------------
    {
        FlatHdrLatch latch;
        expect(!latch.treatedFrame(false) && !latch.treatedFrame(true) && !latch.treatedFrame(false) &&
               !latch.treatedFrame(true) && !latch.tripped && latch.frames == 2,
               "clean frames count nothing; two late-write frames do not trip it");
        expect(latch.treatedFrame(true) && latch.tripped && latch.frames == kFlatHdrLatchFrames,
               "the third treated frame with late writes trips it, once");
        expect(!latch.treatedFrame(true) && latch.tripped, "a tripped latch reports no second trip");
        latch.reset();
        expect(!latch.tripped && latch.frames == 0 && !latch.treatedFrame(true), "the key flipping off rearms it");
    }

    // ---- the census window and its lines ---------------------------------------------------------------------
    {
        FlatHdrWindow w;
        Stream menu; menu.draw(menu.make(menu.sc.tone, nullptr, 3840, 2160, 27, 1, 2, false, false));
        Stream lone; lone.sceneDraws(2, 2);
        Stream ok; ok.sceneDraws(2, 2); ok.toneTrigger();
        Stream late; late.sceneDraws(2, 2); late.toneTrigger();
        late.draw(late.make(late.sc.h, late.sc.hDepth, 3840, 2160, 26, 0x81, 0x82, true, false));
        late.dispatch(late.sc.h);
        w.noteFrame(menu.hdr); w.noteFrame(lone.hdr); w.noteFrame(ok.hdr); w.noteFrame(ok.hdr); w.noteFrame(late.hdr);
        expect(w.frames == 5 && w.hdrFrames == 4 && w.triggerFrames == 3 && w.noTriggerFrames == 1 && w.ambiguousFrames == 0 &&
               w.lateWriteFrames == 1 && w.lateWrites == 2,
               "the window counts frames, frames with an H, with a trigger and with none, and the late writes");
        char text[1024];
        const int n = flatHdrFormatWindow(text, sizeof(text), FlatHdrKey::Off, FlatHdrState::Observing, w);
        const std::string line(text);
        expect(n > 0 && n < int(sizeof(text)) &&
               line.find("flat hdr route 5s: key=off state=observing frames=5 hdr-frames=4 trigger=3 none=1 ambiguous=0 "
                         "treated=0 declined=0 late-hdr-writes=2 (in 1 frames) last=none") == 0 &&
               line.find("last-trigger=VS=") != std::string::npos && line.find("target=3840x2160 hdr=3840x2160") != std::string::npos,
               "the 5 s line: the key, the state, the trigger decision and the late-write census, in the promised order");
        const std::string none = " selection=none";
        expect(line.size() > none.size() && line.compare(line.size() - none.size(), none.size(), none) == 0 &&
               static_cast<int>(line.size()) == n,
               "a window whose selector never spoke ends with selection=none, and the return value is the line's length");
        // What the selector said at each trigger: the key off's whole question, counted by name.
        for (int i = 0; i < 4; ++i) w.noteSelection("selected");
        w.noteSelection(flatMonoReasonName(FlatMonoReason::HdrExtent));
        w.noteSelection(flatMonoReasonName(FlatMonoReason::NoSupportedSource));
        w.noteSelection("selected");
        int m = flatHdrFormatWindow(text, sizeof(text), FlatHdrKey::Off, FlatHdrState::Observing, w);
        std::string tally(text);
        const std::string tail = " selection=selected:5,hdr-route-needs-render-at-least-output:1,no-supported-motion-source-pair:1";
        expect(tally.size() > tail.size() && tally.compare(tally.size() - tail.size(), tail.size(), tail) == 0 &&
               static_cast<int>(tally.size()) == m,
               "the window names every selector verdict and how often, in the order first seen, at the end of the line");
        w.noteSelection("a1"); w.noteSelection("a2"); w.noteSelection("a3");   // the table holds six names
        w.noteSelection("a4"); w.noteSelection("a4"); w.noteSelection("selected");
        m = flatHdrFormatWindow(text, sizeof(text), FlatHdrKey::Off, FlatHdrState::Observing, w);
        tally = text;
        const std::string overflow = "selected:6,hdr-route-needs-render-at-least-output:1,no-supported-motion-source-pair:1,a1:1,a2:1,a3:1,other:2";
        expect(tally.size() > overflow.size() && tally.compare(tally.size() - overflow.size(), overflow.size(), overflow) == 0 &&
               m < int(sizeof(text)),
               "a seventh distinct verdict is counted as other, never dropped and never a write past the buffer");
        // Where the route's frames got to: the eight step counts in the order a frame meets them, between the verdict and
        // the trigger, zeros included, and gone when the window resets.
        {
            FlatHdrWindow stepped;
            stepped.steps.admitted = 7; stepped.steps.reached = 6; stepped.steps.captured = 5; stepped.steps.copied = 4;
            stepped.steps.prepped = 3; stepped.steps.backend = 2; stepped.steps.finished = 1; stepped.steps.restored = 0;
            flatHdrFormatWindow(text, sizeof(text), FlatHdrKey::Auto, FlatHdrState::Active, stepped);
            expect(std::string(text).find(" last=none steps: admitted=7 reached=6 captured=5 copied=4 prepped=3 backend=2 finished=1 restored=0 "
                                          "last-trigger=VS=") != std::string::npos,
                   "the 5 s line carries how many frames reached each step of the treatment, between the verdict and the trigger");
            stepped.reset();
            flatHdrFormatWindow(text, sizeof(text), FlatHdrKey::Auto, FlatHdrState::Active, stepped);
            expect(std::string(text).find(" steps: admitted=0 reached=0 captured=0 copied=0 prepped=0 backend=0 finished=0 restored=0 ") != std::string::npos,
                   "a window that resets starts its step counts at zero, and still prints them");
        }
        char tiny[40];
        expect(flatHdrFormatWindow(tiny, sizeof(tiny), FlatHdrKey::Off, FlatHdrState::Observing, w) >= 0 && tiny[sizeof(tiny) - 1] == 0,
               "a buffer too small for the line truncates it without a write past the end");
        w.noteSelection(nullptr);
        w.reset();
        flatHdrFormatWindow(text, sizeof(text), FlatHdrKey::Auto, FlatHdrState::Latched, w);
        expect(std::string(text).find("key=auto state=latched frames=0 hdr-frames=0 trigger=0 none=0") != std::string::npos &&
               std::string(text).find(" selection=none") != std::string::npos,
               "a window with nothing in it still prints, zeros included: an absent line is what never ran looks like");
        char first[700];
        flatHdrFormatFirstTrigger(first, sizeof(first), 123, FlatHdrKey::Off, ok.hdr);
        expect(std::string(first).find("flat hdr route: first trigger at frame=123 seq=") == 0 &&
               std::string(first).find("reads the scene HDR 3840x2160 at t1") != std::string::npos &&
               std::string(first).find("target=3840x2160 fmt=27") != std::string::npos,
               "the first-trigger line names the pair, the target, H, the slot and H's draws");
        char lateText[700];
        flatHdrFormatLateWrite(lateText, sizeof(lateText), 456, FlatHdrKey::Auto, late.hdr);
        expect(std::string(lateText).find("frame=456 wrote the scene HDR after the trigger (key=auto): 1 draw(s), 1 dispatch(es), "
                                          "0 explicit write(s); the first is seq ") != std::string::npos &&
               std::string(lateText).find("VS=0000000000000081 PS=0000000000000082") != std::string::npos,
               "the late-write line counts draws, dispatches and writes and names the first pair");
        char latched[700];
        flatHdrFormatLatched(latched, sizeof(latched), 789, late.hdr, 3);
        expect(std::string(latched).find("flat hdr route: turned off at frame=789 after 3 treated frame(s)") == 0,
               "the latch line says when, after how many frames, and what happens next");
    }

    // ---- stand-down and F8: the route's verdicts in the stand-down model ---------------------------------------------
    {
        // A chain the copy route refuses for every frame (the bloom-on star): with the route treating, the frame is
        // treatable whatever the copy stage said, so nothing stands down. Without it the same frames stand down.
        using stand_down_test::Sim;
        Sim treated;
        for (int i = 0; i < 60 * 30; ++i) {
            // The copy stage says no-known-tone-pass (structural); the route's selection says selected (treatable);
            // the frame merges to the higher.
            FlatFrameSeen seen = flatFrameSeenFor(false, FlatMonoReason::NoTonePass);
            const FlatFrameSeen route = flatFrameSeenFor(true, FlatMonoReason::Selected);
            if (route >= seen) seen = route;
            treated.frame(seen, FlatMonoReason::Selected);
        }
        expect(treated.machine.entries == 0 && treated.work == FlatWork::Full,
               "frames the route treats never stand the work down, whatever the copy stage says");
        Sim refusedCopy;
        for (int i = 0; i < 60 * 30; ++i) refusedCopy.frame(flatFrameSeenFor(false, FlatMonoReason::NoTonePass), FlatMonoReason::NoTonePass);
        expect(refusedCopy.machine.entries == 1 && refusedCopy.machine.enteredReason == FlatMonoReason::NoTonePass,
               "control: the same frames without the route stand down");
        // An H and no consumer, every frame: structural, stands down at five seconds, warns.
        Sim noConsumer;
        FlatStandDownEvent event = FlatStandDownEvent::None;
        while (event != FlatStandDownEvent::Entered)
            event = noConsumer.frame(flatFrameSeenFor(false, FlatMonoReason::NoHdrConsumer), FlatMonoReason::NoHdrConsumer);
        expect(noConsumer.machine.enteredReason == FlatMonoReason::NoHdrConsumer && noConsumer.machine.warningActive() &&
               noConsumer.now - 1000 >= kFlatStandDownTriggerMs,
               "no-hdr-consumer stands the work down after the usual five seconds and warns");
        // A probe frame that finds the consumer (selected) ends it.
        FlatStandDownEvent resumed = FlatStandDownEvent::None;
        for (int i = 0; i < 200 && resumed != FlatStandDownEvent::Resumed; ++i)
            resumed = noConsumer.frame(flatFrameSeenFor(true, FlatMonoReason::Selected), FlatMonoReason::Selected);
        expect(resumed == FlatStandDownEvent::Resumed && !noConsumer.machine.standing,
               "a probe frame the route selects ends the stand-down");
        // R < D (upscaling) is transient: never a stand-down by itself.
        Sim upscale;
        for (int i = 0; i < 60 * 30; ++i) upscale.frame(flatFrameSeenFor(false, FlatMonoReason::HdrExtent), FlatMonoReason::HdrExtent);
        expect(upscale.machine.entries == 0, "hdr-route-needs-render-at-least-output never stands the work down");

        // ---- and what the route adds to a frame's verdict, merged as the runtime merges it --------------------------
        // The route's trigger comes first in a frame, the copy stage's own verdict at the output copy after it, and the
        // higher of the two stands (None < Structural < Transient < Treatable). The route speaks only where it will
        // resolve the frame (flatHdrTriggerSeen): a refusal merged as itself would outrank the copy stage's structural
        // one, and then a chain the copy route refuses would never stand down and never warn at R < D.
        const auto merged = [&](const FlatMonoFrame& selection, FlatMonoResolveMode mode, bool routeSpeaks,
                                FlatMonoReason copyReason, FlatMonoReason* reasonOut) {
            FlatFrameSeen seen = FlatFrameSeen::None;
            FlatMonoReason reason = FlatMonoReason::NoOutputCopy;
            const FlatFrameSeen route = routeSpeaks ? flatHdrTriggerSeen(selection, mode)
                                                    : flatFrameSeenFor(selection.selected(), selection.reason);   // the old merge
            if (route != FlatFrameSeen::None && route >= seen) { seen = route; reason = selection.reason; }
            const FlatFrameSeen copy = flatFrameSeenFor(false, copyReason);
            if (copy >= seen) { seen = copy; reason = copyReason; }
            *reasonOut = reason;
            return seen;
        };
        const auto runs = [&](const FlatMonoFrame& selection, FlatMonoResolveMode mode, bool routeSpeaks, Sim* sim) {
            for (int i = 0; i < 60 * 30; ++i) {
                FlatMonoReason reason = FlatMonoReason::NoOutputCopy;
                const FlatFrameSeen seen = merged(selection, mode, routeSpeaks, FlatMonoReason::NoTonePass, &reason);
                sim->frame(seen, reason);
            }
        };
        Scene lowSc; lowSc.hW = 2496; lowSc.hH = 1404;                       // 0.65 of 3840x2160: Elite's supersampling below 1.0
        Stream low(lowSc); low.sceneDraws(2, 2); low.toneTrigger();
        const FlatMonoFrame lowFrame = low.select();
        expect(lowFrame.reason == FlatMonoReason::HdrExtent && flatHdrTriggerSeen(lowFrame, FlatMonoResolveMode::Dlss) == FlatFrameSeen::None,
               "R < D: the route adds nothing to the frame's verdict");
        Sim lowWith, lowOff;
        runs(lowFrame, FlatMonoResolveMode::Dlss, true, &lowWith);
        for (int i = 0; i < 60 * 30; ++i) lowOff.frame(flatFrameSeenFor(false, FlatMonoReason::NoTonePass), FlatMonoReason::NoTonePass);
        expect(lowWith.machine.entries == 1 && lowWith.machine.enteredReason == FlatMonoReason::NoTonePass &&
                   lowWith.machine.warningActive() && lowWith.machine.entries == lowOff.machine.entries &&
                   lowWith.machine.enteredReason == lowOff.machine.enteredReason,
               "R < D with the key auto: a chain the copy route refuses stands down and warns exactly as with the key off");
        Sim lowMasked;
        runs(lowFrame, FlatMonoResolveMode::Dlss, false, &lowMasked);
        expect(lowMasked.machine.entries == 0 && !lowMasked.machine.warningActive(),
               "control: the route's refusal merged as itself masks it (never stands down, never warns) -- the bug this pins");
        // EDVR's TAA above D: selected (R >= D) but the mode evaluates on the display grid, so the copy route keeps the frame.
        Scene superSc; superSc.hW = 5760; superSc.hH = 3240;
        Stream super(superSc); super.sceneDraws(2, 2); super.toneTrigger();
        const FlatMonoFrame superFrame = super.select();
        expect(superFrame.selected() && flatHdrTriggerSeen(superFrame, FlatMonoResolveMode::Taa) == FlatFrameSeen::None &&
                   flatHdrTriggerSeen(superFrame, FlatMonoResolveMode::Dlss) == FlatFrameSeen::Treatable &&
                   flatHdrTriggerSeen(superFrame, FlatMonoResolveMode::Dlaa) == FlatFrameSeen::Treatable &&
                   flatHdrTriggerSeen(superFrame, FlatMonoResolveMode::Fsr) == FlatFrameSeen::Treatable,
               "a selected frame is treatable to the route for DLSS, DLAA and FSR above D, and not for EDVR's TAA there");
        Sim taaAbove, dlssAbove;
        runs(superFrame, FlatMonoResolveMode::Taa, true, &taaAbove);
        runs(superFrame, FlatMonoResolveMode::Dlss, true, &dlssAbove);
        expect(taaAbove.machine.entries == 1 && dlssAbove.machine.entries == 0,
               "EDVR's TAA above D: a refused chain stands down as before; DLSS above D: the route treats it, nothing stands down");
        // A refusal for any other reason adds nothing either (the copy stage decides).
        Stream noPool; noPool.sceneDraws(0, 4); noPool.toneTrigger();
        const FlatMonoFrame refused = noPool.select();
        Sim refusedSim;
        runs(refused, FlatMonoResolveMode::Dlss, true, &refusedSim);
        expect(!refused.selected() && flatHdrTriggerSeen(refused, FlatMonoResolveMode::Dlss) == FlatFrameSeen::None &&
                   refusedSim.machine.entries == 1,
               "a selector refusal of the route's own adds nothing: the copy stage's structural verdict stands");
        // At R = D the route treats what the copy route refuses, as before: nothing stands down.
        Stream atD; atD.sceneDraws(2, 2); atD.toneTrigger();
        const FlatMonoFrame okFrame = atD.select();
        Sim treatedSim;
        runs(okFrame, FlatMonoResolveMode::Dlss, true, &treatedSim);
        expect(okFrame.selected() && treatedSim.machine.entries == 0 && treatedSim.work == FlatWork::Full,
               "R = D with the key auto: the frames the route treats never stand the work down, whatever the copy stage said");
        // A probe frame (stood down) that the route would resolve ends the stand-down; one at R < D does not.
        Sim probeAbove = lowWith;   // stood down for no-known-tone-pass at R < D
        FlatStandDownEvent lowEvent = FlatStandDownEvent::None;
        for (int i = 0; i < 400; ++i) {
            FlatMonoReason reason = FlatMonoReason::NoOutputCopy;
            const FlatFrameSeen seen = merged(lowFrame, FlatMonoResolveMode::Dlss, true, FlatMonoReason::NoTonePass, &reason);
            if (probeAbove.frame(seen, reason) == FlatStandDownEvent::Resumed) lowEvent = FlatStandDownEvent::Resumed;
        }
        expect(lowEvent == FlatStandDownEvent::None && probeAbove.machine.standing,
               "probe frames at R < D keep the stand-down (the copy route still refuses them)");
        Sim probeAtD = lowWith;
        FlatStandDownEvent atDEvent = FlatStandDownEvent::None;
        for (int i = 0; i < 400 && atDEvent != FlatStandDownEvent::Resumed; ++i) {
            FlatMonoReason reason = FlatMonoReason::NoOutputCopy;
            const FlatFrameSeen seen = merged(okFrame, FlatMonoResolveMode::Dlss, true, FlatMonoReason::NoTonePass, &reason);
            atDEvent = probeAtD.frame(seen, reason);
        }
        expect(atDEvent == FlatStandDownEvent::Resumed && !probeAtD.machine.standing,
               "a probe frame the route would resolve (R >= D) ends the stand-down");
    }

    // ---- the trace format: v4 carries the four slots and the resolve marker; v3 is still read -------------------------
    {
        auto ring = std::make_unique<FlatTraceRing>();
        Stream s;
        flatTraceBeginFrame(*ring, kFrame, s.sc.output, 3840, 2160, 28);
        const auto d1 = s.make(s.sc.h, s.sc.hDepth, 3840, 2160, 26, 0xA1, 0xB1, true, true);
        flatTraceRecord(*ring, d1, false);
        const void* srv[4] = {nullptr, s.sc.h, nullptr, s.sc.copyOfH};
        const auto d2 = s.make(s.sc.tone, nullptr, 3840, 2160, 27, 0xF9, 0xFE, false, false);
        flatTraceRecord(*ring, d2, false, srv);
        flatTraceResolve(*ring, s.sc.h, 0xF9, 0xFE, 7, uint32_t(FlatMonoReason::Selected));
        flatTraceBeginFrame(*ring, kFrame + 1, s.sc.output, 3840, 2160, 28);   // seals the slot: a dump skips the current one
        std::vector<unsigned char> bytes;
        flatTraceDump(*ring, [&](const void* data, uint32_t n) {
            const auto* p = static_cast<const unsigned char*>(data);
            bytes.insert(bytes.end(), p, p + n); return n;
        });
        expect(bytes.size() > 16 && std::memcmp(bytes.data(), "EDVRFTR4", 8) == 0, "the dump is EDVRFTR4");
        std::vector<ParsedFrame> frames;
        const bool parsed = parseTrace(bytes, &frames);
        expect(parsed && frames.size() == 1 && frames[0].events.size() == 3, "a v4 dump parses: one frame, three events");
        if (parsed && frames.size() == 1 && frames[0].events.size() == 3) {
            const auto& e = frames[0].events;
            expect(!(e[0].flags & kFlatTraceHdrSrvKnown) && e[0].hdrSrv[1] == nullptr,
                   "a draw the detector did not ask about carries no slots");
            expect((e[1].flags & kFlatTraceHdrSrvKnown) && e[1].hdrSrv[0] == nullptr && e[1].hdrSrv[1] == s.sc.h &&
                   e[1].hdrSrv[3] == s.sc.copyOfH,
                   "a candidate consumer carries its four slots, and a null slot stays null");
            expect(e[2].kind == kFlatTraceEventResolve && e[2].key.color == s.sc.h && e[2].key.vs == 0xF9 &&
                   e[2].key.ps == 0xFE && e[2].key.sequence == 7 && e[2].key.count == uint32_t(FlatMonoReason::Selected),
                   "the resolve marker names H, the trigger's pair and place, and the route's verdict");
            const FrameFacts f = replayFrame(frames[0]);
            expect(f.resolveMarkers == 1 && f.markerEvent == 2 && f.markerReason == uint32_t(FlatMonoReason::Selected) &&
                   f.triggered && f.triggerEvent == 1 && f.srvKnown && f.lateWrites == 0,
                   "a replay reads the recorded slots, pins the trigger at its event, and sees the marker without feeding it to the reducer");
        }
        // EDVRFTR3: the same bytes the corpus holds, the old event layout, widened with no slots.
        std::vector<unsigned char> v3;
        {
            FlatTraceHeader header{};
            std::memcpy(header.magic, "EDVRFTR3", 8);
            header.frameCount = 1;
            FlatTraceFrameHeader fh{};
            fh.frame = 77; fh.output = s.sc.output; fh.width = 3840; fh.height = 2160; fh.format = 28; fh.eventCount = 1;
            FlatTraceEventV3 ev{};
            ev.key = d2.key; ev.key.camera = nullptr;
            ev.flags = kFlatTraceSupported;
            ev.kind = kFlatTraceEventDraw;
            const auto* a = reinterpret_cast<const unsigned char*>(&header);
            const auto* b = reinterpret_cast<const unsigned char*>(&fh);
            const auto* c = reinterpret_cast<const unsigned char*>(&ev);
            v3.insert(v3.end(), a, a + sizeof(header)); v3.insert(v3.end(), b, b + sizeof(fh)); v3.insert(v3.end(), c, c + sizeof(ev));
        }
        std::vector<ParsedFrame> old;
        expect(parseTrace(v3, &old) && old.size() == 1 && old[0].events.size() == 1 && old[0].header.frame == 77 &&
               old[0].events[0].key.ps == 0xFE && old[0].events[0].flags == kFlatTraceSupported &&
               old[0].events[0].hdrSrv[0] == nullptr && old[0].events[0].hdrSrv[3] == nullptr,
               "an EDVRFTR3 trace still parses: the event is widened and its slots are zero");
        std::vector<unsigned char> bad = v3;
        bad[7] = '9';
        expect(!parseTrace(bad, &old), "another magic is not a trace");
        bad = v3; bad.resize(bad.size() - 3);
        expect(!parseTrace(bad, &old), "a truncated v3 trace is refused");
        expect(sizeof(FlatTraceEventV3) == 472, "the EDVRFTR3 event layout is the 472 bytes the corpus was written with");
    }

    // The live trace has to preserve the protection and effective-write
    // semantics. A replay that drops them could approve an unsafe suffix.
    {
        auto ring=std::make_unique<FlatTraceRing>();
        Stream s;
        auto recordFrame=[&](uint64_t frame,bool depthWriter) {
            flatTraceBeginFrame(*ring,frame,s.sc.output,s.sc.outW,s.sc.outH,28);
            flatTraceMark(*ring,kFlatTraceEventCameraCapture,nullptr);
            auto world=s.make(s.sc.h,s.sc.hDepth,s.sc.hW,s.sc.hH,26,0xA1,0xB1,true,true);
            world.key.writeEpoch=frame;
            flatTraceRecord(*ring,world,false);
            auto overlay=s.make(s.sc.h,s.sc.hDepth,s.sc.hW,s.sc.hH,26,0x1234,0x5678,true,false);
            overlay.camera[0]^=1;overlay.key.cameraHash=flatCameraHash(overlay.camera);
            overlay.key.writeEpoch=frame;
            overlay.overlayProtected=true;overlay.effectiveStencilWrite=true;
            flatTraceRecord(*ring,overlay,false);
            if (depthWriter) {
                auto z=s.make(s.sc.h2,s.sc.hDepth,s.sc.hW,s.sc.hH,26,0x51,0x52,false,false);
                z.effectiveDepthWrite=true;
                flatTraceRecord(*ring,z,false);
                flatTraceMark(*ring,kFlatTraceEventOverlayFailed,s.sc.h);
            } else {
                flatTraceMark(*ring,kFlatTraceEventOverlaySeal,s.sc.h);
                flatTraceMark(*ring,kFlatTraceEventWriteResource,s.sc.hDepth);
            }
        };
        recordFrame(kFrame,true);
        recordFrame(kFrame+1,false);
        flatTraceBeginFrame(*ring,kFrame+2,s.sc.output,s.sc.outW,s.sc.outH,28);
        std::vector<unsigned char> serialized;
        flatTraceDump(*ring,[&](const void* data,uint32_t n) {
            const auto* p=static_cast<const unsigned char*>(data);
            serialized.insert(serialized.end(),p,p+n);return n;
        });
        std::vector<ParsedFrame> frames;
        expect(parseTrace(serialized,&frames) && frames.size()==2,
               "overlay semantic trace frames serialize and parse");
        if (frames.size()==2) {
            for (size_t fi=0;fi<2;++fi) {
                FlatRuntimePrefix replay{};
                replay.frame=frames[fi].header.frame;replay.output=s.sc.output;
                replay.width=s.sc.outW;replay.height=s.sc.outH;replay.format=28;
                bool protectedFlag=false,depthFlag=false,stencilFlag=false;
                for (const auto& e:frames[fi].events) {
                    if (e.kind==kFlatTraceEventCameraCapture) {++replay.sequence;continue;}
                    if (e.kind==kFlatTraceEventWriteResource) {flatRuntimeWritten(replay,e.key.color);continue;}
                    if (replayOverlayMarker(replay,e)) continue;
                    auto d=flatTraceEventToDraw(e);
                    protectedFlag|=d.overlayProtected;
                    depthFlag|=d.effectiveDepthWrite;
                    stencilFlag|=d.effectiveStencilWrite;
                    flatRuntimeObserve(replay,d);
                }
                const FlatRuntimeTarget* hTarget=nullptr;
                for (uint32_t i=0;i<replay.targetsUsed;++i)
                    if(replay.targets[i].resource==s.sc.h) hTarget=&replay.targets[i];
                expect(hTarget && protectedFlag && stencilFlag &&
                       (fi==0 ? depthFlag && hTarget->hdrBad &&
                           hTarget->firstBad.cause==FlatRuntimeConflict::OverlaySuffix
                              : !depthFlag && !hTarget->hdrBad && !hTarget->overlayOpen),
                       fi==0 ? "serialized depth writer and failure marker refuse overlay replay"
                             : "serialized seal ends suffix before unrelated depth write");
            }
        }
    }

    // ---- the corpus: the detector over every captured frame, and the four captures of section 81 ----------------
    namespace fs = std::filesystem;
    const fs::path dir("tools/flat_temporal_test/traces");
    struct Pin {
        const char* file; uint64_t frame; bool h;
        uint32_t hdrDraws, hdrFirst, hdrLast, triggerEvent;
        uint64_t vs, ps; uint32_t targetW, targetH, targetFormat;
        bool copySelected;
    };
    // Derived with `flat_temporal_test --trace-chain <file>`: the events are indices in the frame's event list (camera
    // captures, writes and dispatches count), H the draws of the R11G11B10F target with its depth, "trigger" the
    // first draw after H's first write with no depth, a target that is not H and at least half of H per axis. Section
    // 81's table is the same numbers (its "+N" is trigger minus H's last event).
    const Pin pins[] = {
        // star, bloom and DoF on: the copy route refuses it (no-known-tone-pass); the trigger is bloom's first level.
        {"flat_trace_85998.bin", 85543, true, 63, 1067, 1336, 1389, 0xDFED8E1C9E191BECull, 0x143AAE0597E2F7BFull, 1920, 1080, 26, false},
        {"flat_trace_85998.bin", 85814, true, 64, 1019, 1236, 1238, 0xDFED8E1C9E191BECull, 0x143AAE0597E2F7BFull, 1920, 1080, 26, false},
        // hangar, bloom and DoF on: a 2D menu frame (no H) and the 3D hangar, whose trigger is DoF's first pass.
        {"flat_trace_87229.bin", 86701, false, 0, 0, 0, 0, 0, 0, 0, 0, 0, false},
        {"flat_trace_87229.bin", 86970, true, 27, 1123, 1213, 1215, 0x20F383BBAC05C031ull, 0xFDB74215D3E6832Dull, 3840, 2160, 9, false},
        // star, off: the trigger is the tone pass.
        {"flat_trace_45737.bin", 45734, true, 53, 742, 939, 957, 0xF9CFC798F21E9AEAull, 0xFEE777E92850B390ull, 3840, 2160, 27, true},
        {"flat_trace_45737.bin", 45735, true, 53, 1181, 1364, 1381, 0xF9CFC798F21E9AEAull, 0xFEE777E92850B390ull, 3840, 2160, 27, true},
        // hangar, off: the trigger is the game's copy of H.
        {"flat_trace_39788.bin", 39785, true, 28, 1130, 1223, 1225, 0xDEF19B035D5EDEDCull, 0xDED8796049C7BB4Aull, 3840, 2160, 26, true},
    };
    std::map<std::string, std::vector<ParsedFrame>> loaded;
    uint32_t files = 0, frames = 0, withH = 0, withTrigger = 0, late = 0, selectedFrames = 0, selectedWithTrigger = 0,
             extentRefused = 0, hashMismatch = 0, noHdr = 0;
    if (!fs::exists(dir)) {
        expect(false, "the trace corpus is readable from the repo root");
    } else {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.path().extension() != ".bin") continue;
            std::vector<unsigned char> bytes;
            std::vector<ParsedFrame>& parsedFrames = loaded[entry.path().filename().string()];
            if (!readFile(entry.path(), &bytes) || !parseTrace(bytes, &parsedFrames)) { expect(false, "a corpus trace parses"); continue; }
            ++files;
            for (const ParsedFrame& pf : parsedFrames) {
                const FrameFacts f = replayFrame(pf);
                ++frames;
                if (!f.hashMatches) ++hashMismatch;
                if (f.candidate) ++withH; else ++noHdr;
                if (f.triggered) ++withTrigger;
                if (f.lateWrites) ++late;
                if (f.copySelected) { ++selectedFrames; if (f.triggered) ++selectedWithTrigger; }
                // A frame with an H that is at least the output's size is selected; one below it is the copy route's.
                if (f.triggered) {
                    const bool atLeast = f.hdrWidth >= f.outputWidth && f.hdrHeight >= f.outputHeight;
                    if (atLeast) expect(f.selection == FlatMonoReason::Selected, "a corpus frame with R >= D is selected by the route");
                    else if (f.selection == FlatMonoReason::HdrExtent) ++extentRefused;
                    else expect(false, "a corpus frame with R < D is refused as hdr-route-needs-render-at-least-output");
                }
                expect(!f.ambiguous && f.resolveMarkers == 0, "no corpus frame has an ambiguous trigger or a resolve marker");
            }
        }
    }
    expect(files == 17 && frames == 46, "the corpus is the 13 captured traces plus the four section-81 fixtures: 46 frames");
    expect(hashMismatch == 0, "with the detector attached the replay still reproduces every recorded contract hash");
    expect(withTrigger == withH && withH == 45 && noHdr == 1,
           "a trigger in every frame that has an HDR target (45 of 45), and the one 2D menu frame has none");
    expect(late == 0, "no H write after the trigger in any corpus frame (section 81: 39 of 39; 45 of 45 with the fixtures)");
    expect(selectedFrames == 42 && selectedWithTrigger == selectedFrames,
           "a trigger in each frame the tone route selects");
    expect(extentRefused > 0, "the corpus holds render-below-output cells, which the route hands to the copy route");
    for (const Pin& p : pins) {
        auto it = loaded.find(p.file);
        const ParsedFrame* frame = nullptr;
        if (it != loaded.end())
            for (const ParsedFrame& pf : it->second) if (pf.header.frame == p.frame) frame = &pf;
        if (!frame) { expect(false, "a pinned fixture frame is in the corpus"); continue; }
        const FrameFacts f = replayFrame(*frame);
        if (!p.h) {
            expect(!f.candidate && !f.triggered && flatHdrFrameVerdict(FlatHdrFrame{}) == FlatHdrFrameVerdict::NoHdr,
                   "a 2D menu frame has no H and no trigger");
            continue;
        }
        expect(f.candidate && f.hdrWidth == f.outputWidth && f.hdrHeight == f.outputHeight && f.hdrDraws == p.hdrDraws &&
               f.hdrFirstEvent == p.hdrFirst && f.hdrLastEvent == p.hdrLast,
               "a pinned frame's H: its extent, its draw count and the events of its first and last draw");
        expect(f.triggered && f.triggerEvent == p.triggerEvent && f.triggerVs == p.vs && f.triggerPs == p.ps &&
               f.targetWidth == p.targetW && f.targetHeight == p.targetH && f.targetFormat == p.targetFormat && !f.srvKnown,
               "a pinned frame's trigger: the event, the pair, the target and its format");
        expect(f.lateWrites == 0 && !f.ambiguous && f.selection == FlatMonoReason::Selected &&
               f.copySelected == p.copySelected,
               "a pinned frame: no H write after the trigger, the route selects it, and the copy route's own verdict is as recorded");
    }

    // ---- MUTATIONS on real frames: the same four failure shapes, on captured events -------------------------
    {
        auto it = loaded.find("flat_trace_45737.bin");
        const ParsedFrame* base = nullptr;
        if (it != loaded.end())
            for (const ParsedFrame& pf : it->second) if (pf.header.frame == 45734) base = &pf;
        if (!base) {
            expect(false, "the star-off fixture frame is in the corpus");
        } else {
            const FrameFacts clean = replayFrame(*base);
            const uint32_t trigger = clean.triggerEvent, hLast = clean.hdrLastEvent;
            const FlatTraceEvent& lastHdrDraw = base->events[hLast];
            const FlatTraceEvent& triggerEvent = base->events[trigger];
            // (1) a late H draw after the trigger: counted, and named.
            {
                ParsedFrame m = *base;
                m.events.insert(m.events.begin() + trigger + 1, lastHdrDraw);
                const FrameFacts f = replayFrame(m);
                expect(f.triggered && f.triggerEvent == trigger && f.lateWrites == 1 && f.lateDraws == 1,
                       "mutation on a real frame: an H draw appended after the trigger is one late write");
            }
            // (2) a full-size pass between H writes that reads nothing of H: fires early with no slots, skipped with them.
            {
                ParsedFrame m = *base;
                FlatTraceEvent impostor = triggerEvent;       // the tone pass's own shape: no depth, full size
                impostor.key.ps = 0x1234;
                m.events.insert(m.events.begin() + hLast - 4, impostor);
                const FrameFacts blind = replayFrame(m);
                expect(blind.triggered && blind.triggerEvent == hLast - 4 && blind.lateWrites > 0,
                       "mutation: without the slots a full-size non-consumer between H writes fires early and leaves late writes");
                ParsedFrame v4 = m;
                v4.events[hLast - 4].flags |= kFlatTraceHdrSrvKnown;          // slots read: none of them is H
                FlatTraceEvent& real = v4.events[trigger + 1];                 // the real trigger moved down by the insert
                real.flags |= kFlatTraceHdrSrvKnown;
                real.hdrSrv[1] = clean.hdrResource;                            // the tone pass reads H at t1
                const FrameFacts known = replayFrame(v4);
                expect(known.triggered && known.triggerEvent == trigger + 1 && known.lateWrites == 0 && known.srvKnown &&
                       known.selection == FlatMonoReason::Selected,
                       "with the slots recorded the impostor is skipped, the real consumer fires, nothing is late");
            }
            // (3) two H candidates: a second scene-shaped target drawn before the trigger.
            {
                ParsedFrame m = *base;
                FlatTraceEvent second = lastHdrDraw;
                second.key.color = tok(0xDEAD0001);
                second.key.rtv = tok(0xDEAD0002);
                m.events.insert(m.events.begin() + hLast + 1, second);
                const FrameFacts f = replayFrame(m);
                expect(f.triggered && f.ambiguous && f.candidates == 2 && f.selection == FlatMonoReason::ConflictingHdr,
                       "mutation: a second scene-shaped HDR target before the trigger is ambiguous and refused as conflicting-hdr");
            }
            // (4) H changing identity mid-frame: after the trigger, a scene-shaped draw into a target that is new.
            {
                ParsedFrame m = *base;
                FlatTraceEvent other = lastHdrDraw;
                other.key.color = tok(0xDEAD0003);
                other.key.rtv = tok(0xDEAD0004);
                m.events.insert(m.events.begin() + trigger + 1, other);
                const FrameFacts f = replayFrame(m);
                expect(f.triggered && f.triggerEvent == trigger && f.newCandidateLate && f.lateWrites == 1,
                       "mutation: H changing identity after the trigger is a late write into a new candidate");
            }
            // (5) an explicit write and a dispatch into H after the trigger, and a write into another resource.
            {
                ParsedFrame m = *base;
                m.events.insert(m.events.begin() + trigger + 1, edvr::flatTraceEventMarker(kFlatTraceEventWriteResource, clean.hdrResource));
                m.events.insert(m.events.begin() + trigger + 1, edvr::flatTraceEventMarker(kFlatTraceEventDispatchWritten, clean.hdrResource));
                m.events.insert(m.events.begin() + trigger + 1, edvr::flatTraceEventMarker(kFlatTraceEventWriteResource, tok(0xDEAD0005)));
                const FrameFacts f = replayFrame(m);
                expect(f.lateExplicit == 1 && f.lateDispatches == 1 && f.lateWrites == 2,
                       "mutation: an explicit write and a dispatch into H after the trigger are counted, a write elsewhere is not");
            }
            // (6) H copied mid-frame: a write whose destination is another resource, between H's last draw and the trigger.
            {
                ParsedFrame m = *base;
                m.events.insert(m.events.begin() + hLast + 1, edvr::flatTraceEventMarker(kFlatTraceEventWriteResource, tok(0xDEAD0006)));
                const FrameFacts f = replayFrame(m);
                expect(f.triggered && f.triggerEvent == trigger + 1 && f.lateWrites == 0,
                       "mutation: a copy of H (a write to another resource) before the trigger moves nothing");
            }
        }
    }

    // ---- the wiring no rig can run: DLSS ---------------------------------------------------------------------
    // dlaa.cpp needs an NVIDIA GPU to make a feature, so nothing here executes it. fsr3_engine_test pins the FSR half on
    // WARP against the real port (the flags in desc.flags, a flip remaking the context). What is left for DLSS is that the
    // feature is created with the flag set hdr_backend_flags.h names, that the route's bit is in the key that decides a
    // remake, and that the evaluate call hands the route's bit down: read as source, because nothing else can see it.
    {
        const auto slurpSource = [&](const char* path) {
            std::vector<unsigned char> bytes;
            std::string text;
            if (readFile(fs::path(path), &bytes)) text.assign(bytes.begin(), bytes.end());
            return text;
        };
        const auto count = [](const std::string& text, const char* needle) {
            size_t n = 0, at = 0; const std::string s(needle);
            while ((at = text.find(s, at)) != std::string::npos) { ++n; at += s.size(); }
            return n;
        };
        const std::string dlaa = slurpSource("src/d3d11/dlaa.cpp");
        expect(!dlaa.empty() && count(dlaa, "cp.InFeatureCreateFlags = static_cast<int>(flatDlssCreateFlags(hdr));") == 1 &&
                   count(dlaa, "flatDlssCreateFlags(hdr)") == 1,
               "dlaa.cpp creates the DLSS feature with exactly the flags hdr_backend_flags.h names for the route, in one place");
        expect(count(dlaa, "f.presetGen != g_presetGen || f.hdr != hdr") == 1 && count(dlaa, "f.hdr = hdr;") == 1,
               "dlaa.cpp keys the feature on the route's bit and stores it when the feature is made, so a flip remakes it");
        expect(count(dlaa, "ensureFeature(ctx, eye, w, h, outW, outH, reason, nullptr, hdr)") == 1,
               "dlaaEvaluate hands the route's bit to the feature it asks for");
        // The resolver hands the frame's bit down to each engine, with no literal in its place.
        const std::string resolve = slurpSource("src/d3d11/flat_mono_resolve.cpp");
        expect(count(resolve, "f.jitterX,f.jitterY,reset,f.deltaMs,reason,hdr);") == 1 &&
                   count(resolve, "2*std::atan(1/sy),reason,true,hdr);") == 1,
               "the resolver's backend calls pass the frame's hdr bit to the DLSS/DLAA evaluate and to the FSR evaluate");
    }

    // ---- the wiring no rig can run: the runtime's default, merge and publication, and the panel's use of them ---------
    // The runtime and the panel need a game to run, so what the rigs above pin as functions is pinned here as calls: the
    // route's key reads auto when the file has no line (config_test holds that to the shipped ini), the route adds to the
    // stand-down verdict only through flatHdrTriggerSeen (the old merge of its own refusal, which masked a structural
    // refusal at R < D, is gone), the measured sizes are published from the selection and cleared on a flip, a resize
    // and a selection at R >= D, and the panel reads them only while frames are refused.
    {
        const auto slurpSource = [&](const char* path) {
            std::vector<unsigned char> bytes;
            std::string text;
            if (readFile(fs::path(path), &bytes)) text.assign(bytes.begin(), bytes.end());
            return text;
        };
        const auto count = [](const std::string& text, const char* needle) {
            size_t n = 0, at = 0; const std::string s(needle);
            while ((at = text.find(s, at)) != std::string::npos) { ++n; at += s.size(); }
            return n;
        };
        const std::string runtime = slurpSource("src/d3d11/flat_runtime.cpp");
        const std::string menu = slurpSource("src/d3d11/menu.cpp");
        expect(!runtime.empty() && !menu.empty(), "the runtime and menu sources are readable from the repo root");
        expect(count(runtime, "Config::get().getString(\"experimental.temporal_aa_before_post\", \"auto\")") == 1,
               "the route's key falls back to auto when the file has no line");
        expect(count(runtime, "flatHdrTriggerSeen(sel, s.engine)") == 1 && count(runtime, "flatFrameSeenFor(") == 1 &&
                   count(runtime, "if (routeSeen == FlatFrameSeen::Treatable) { s.frameSeen = FlatFrameSeen::Treatable;") == 1,
               "the route adds to a frame's stand-down verdict through flatHdrTriggerSeen only; the copy stage is the one other caller of flatFrameSeenFor");
        // The F8 supersampling advice (the game rendering below the output) is gone (section 83: below 1.0 is served by the
        // copy's admission by structure), with its published sizes and the route's eligibility word: nothing of either is left.
        expect(count(runtime, "g_hdrBelowOutput") == 0 && count(runtime, "hdrEligible") == 0 &&
                   count(runtime, "flatHdrSupersamplingAdvice") == 0 && count(menu, "flatRuntimeHdrRouteBelowOutput") == 0 &&
                   count(menu, "flatRuntimeHdrRouteActive") == 0 && count(menu, "supersamplingBelowOne") == 0,
               "the supersampling advice, its published sizes and the route's eligibility word are gone from the runtime and the panel");
        // The scene's and the output's sizes the copy stage measures are published for the panel (the admission's scene facts, or the
        // prefix model's where the admission did not look), written at every final copy that has a scene and cleared by a resize; the
        // panel reads them only while frames are refused.
        expect(count(runtime, "g_sceneSizes.store(") == 2 && count(runtime, "g_sceneSizes.store(0, std::memory_order_release);") == 1 &&
                   count(runtime, "flatHdrPackSizes(sceneW, sceneH, s.prefix.width, s.prefix.height)") == 1 &&
                   count(runtime, "uint32_t sceneW = diag.sceneWidth, sceneH = diag.sceneHeight;\n    if (!sceneW) {\n"
                                  "        const FlatSceneFacts facts = flatSceneFacts(s.prefix, s.prefix.width, s.prefix.height);\n"
                                  "        sceneW = facts.width; sceneH = facts.height;\n    }\n    if (sceneW)\n") == 1,
               "the scene's measured sizes are published at every final copy that has a scene (the admission's facts, else the prefix model's) and cleared by a resize");
        expect(count(menu, "const bool sizesKnown = refusing && flatRuntimeSceneSizes(&renderW, &renderH, &outputW, &outputH);") == 1 &&
                   count(menu, "const FlatWarningCause cause = flatWarningCause(refusing, refusing && flatRuntimeStructureAdmission(),") == 1 &&
                   count(menu, "refusing && flatRuntimeTaaAboveOutput(), renderSizeReason, sizesKnown,") == 1,
               "the panel reads the published sizes and the two bits only while frames are refused, through flatWarningCause");
        expect(count(menu, "flatSettingsWarningKey(label.c_str(), s.flatSettings.settings(), cause)") == 1 &&
                   count(menu, "flatFormatSettingsWarningLog(line, sizeof(line), was, label.c_str(), reason, standing, cause, w);") == 1 &&
                   count(menu, "&flatWarnMeasure, &ruler, &warning, s.flatWarnCause);") == 1,
               "the key, the log line and the panel's words all take the same cause");
        expect(count(menu, "static_assert(static_cast<int>(kFlatPageRowCount) + 1 + FlatSettingsWarning::kMaxLines <= kMenuMaxLines,") == 1,
               "the flat page's rows, a blank line and a full warning are held to the card's lines at compile time");
        // THE DEPTH-VALIDATED STEADY DETAIL on foot (design doc section 82): always on, with no key (retired 2026-10-01). The flat runtime
        // hands the resolver steadyDetail = true at its two treatment call sites, right after the 3D menu's own blanket policy
        // (FlatMonoResolveFrame::staticScene, from the verified menu copy), which is not this rule's and whose lines are exactly what they
        // were: four mentions of f.staticScene in the file, none of them an assignment of the steady detail. Nothing of the key that once
        // chose it is left: no reader, no state field, no key-change line, no setting that starts with the route key's name.
        expect(count(runtime, "steadyReadKey") == 0 && count(runtime, "steadyKeyRead") == 0 && count(runtime, "s.steadyDetail") == 0 &&
                   count(runtime, "bool steadyDetail") == 0 && count(runtime, "flat runtime: steady-detail is") == 0 &&
                   count(runtime, "experimental.temporal_aa_on_foot_world_") == 0,
               "the steady-detail key is gone from the flat runtime: no reader (steadyReadKey), no state field, no key-change line, no setting that starts with the route key's name");
        expect(count(runtime, "f.staticScene=flatFrameThroughMenuCopy(s.prefix,selected.hdr);\n    if(f.staticScene)++s.staticSceneFrames;\n"
                              "    f.steadyDetail=true;") == 1 &&
                   count(runtime, "f.staticScene = flatFrameThroughMenuCopy(s.prefix, selected.hdr);\n    if (f.staticScene) ++s.staticSceneFrames;\n"
                                  "    f.steadyDetail = true;") == 1 &&
                   count(runtime, "f.steadyDetail") == 2 && count(runtime, "f.staticScene") == 4,
               "both treatment call sites (the copy route and the HDR route) hand the resolver steady detail unconditionally, right after the 3D menu's blanket policy, whose lines are unchanged");
        expect(count(runtime, "flat steady detail 5s: steady-detail=on depth-check=%llu/%llu;") == 1 &&
                   count(runtime, "flat steady detail 5s: steady-detail=%s") == 0 &&
                   count(runtime, "flatMonoResolveTakeRefusalCensus()") == 1,
               "the 5 s block says steady-detail=on and the resolver's depth-check frames, zeros included, once a window");
    }

    return failures;
}

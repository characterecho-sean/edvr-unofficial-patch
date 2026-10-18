#pragma once

// The offscreen fast path (src\d3d11\flat_offscreen_fast.h). What it holds to, each a way the path could silently change a decision. Every
// check's text starts "offscreen O<n>:" and offscreen_mutants.py names the case each mutation of the header must break.
//   O1  EVERY INPUT OF THE KEY IS NEEDED: two draws differing in one input are different keys, and a key comparison that drops that input
//       matches them (fifteen inputs, each shown to be the only one that matters in its pair).
//   O2  EVERY LIVE CONDITION FORBIDS: one field flipped at a time forces the slow path and names its reason; all clear says none; two bad
//       fields answer with the earlier reason.
//   O3  EVERY DRAW CONDITION FORBIDS: the pair (each fact of it), the format, the output, the extent, the depth of the scene's size, the
//       fullscreen triangle; the draws it serves are served.
//   O4  THE MEMO: lookup says why not; a record keeps its own copy of the camera rows; a hit changes only the instance count.
//   O5  THE WIRING: the constructor asks before it builds anything, records after its last skipped block, fills each live field from the state
//       its comment names, and does what the fast path must still do, in the slow path's order; and every block the fast path skips still has
//       the gate that makes it a no-op for such a draw (a change to one fails here until the path is reviewed). Each pin has a control.
//   O6  THE LINE: the token's text, its zero form, the three largest reasons, and its length at absurd counts.
//   O7  THE DECISIVE REPLAY: every frame of the committed frame-contract corpus (the 46 recorded frames) is replayed twice through the reducer,
//       once as recorded and once with the fast path's rules (a class-4 draw that passes the live conditions and repeats the verified draw's
//       key takes the memo's record in place of its own). The two replays must leave the same contract and the same prefix, a memo hit's record
//       must EQUAL the draw's own but for the instance count, and the corpus must hold hits: a corpus with none would prove nothing.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "../../src/d3d11/flat_frame_contract.h"
#include "../../src/d3d11/flat_hdr_route.h"
#include "../../src/d3d11/flat_offscreen_fast.h"
#include "../../src/d3d11/flat_pair_facts.h"
#include "../../src/d3d11/flat_trace.h"
#include "../../src/d3d11/engine_velocity_families.h"

namespace flat_offscreen_rig {
using namespace edvr;

// ---- O7: the replay ---------------------------------------------------------------------------------------------------------------
// The reducer's state after a frame, in a form two replays can be compared by: everything the model keeps that a draw can change.
inline uint64_t mix(uint64_t h, uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); return h * 0x100000001B3ull; }
inline uint64_t recordDigest(uint64_t h, const FlatContractRecord& r) {
    const auto& k = r.key;
    h = mix(h, reinterpret_cast<uintptr_t>(k.color)); h = mix(h, reinterpret_cast<uintptr_t>(k.depth)); h = mix(h, k.vs); h = mix(h, k.ps);
    h = mix(h, k.cameraHash); h = mix(h, k.format); h = mix(h, k.width); h = mix(h, k.height); h = mix(h, static_cast<uint64_t>(k.kind));
    h = mix(h, r.draws); h = mix(h, r.first); h = mix(h, r.last); h = mix(h, r.lastInstances); h = mix(h, r.firstInstances);
    h = mix(h, r.lastWriteEpoch); h = mix(h, r.lastWriteSeq); h = mix(h, r.firstWriteEpoch); h = mix(h, r.firstWriteSeq);
    return h;
}
inline uint64_t prefixDigest(const FlatRuntimePrefix& p) {
    uint64_t h = 1469598103934665603ull;
    h = mix(h, p.sequence); h = mix(h, p.uncertain); h = mix(h, p.targetsUsed); h = mix(h, p.sourcesUsed); h = mix(h, p.copies);
    h = mix(h, p.unsupportedFamilyDraws); h = mix(h, p.firstPersonDraws); h = mix(h, p.imageCopiesAccepted); h = mix(h, p.imageCopiesRefused);
    h = mix(h, p.menuCopiesAccepted); h = mix(h, p.menuCopiesRefused);
    for (uint32_t i = 0; i < p.targetsUsed; ++i) {
        const auto& t = p.targets[i];
        h = mix(h, reinterpret_cast<uintptr_t>(t.resource)); h = recordDigest(h, t.writes); h = recordDigest(h, t.tone);
        h = mix(h, t.hdrBad); h = mix(h, t.hdrCamera); h = mix(h, t.hdrLayoutChanged); h = mix(h, t.imageSourceBad); h = mix(h, t.menuInherited);
        h = mix(h, t.tones); h = mix(h, t.overlayOpen); h = mix(h, static_cast<uint64_t>(t.firstBad.cause)); h = mix(h, t.firstBad.sequence);
    }
    for (uint32_t i = 0; i < p.sourcesUsed; ++i) h = recordDigest(h, p.sources[i]);
    return h;
}
// Two draw records are the same draw but for the instance count: the memo's record may stand in for the event's own.
inline bool sameDrawRecord(const FlatRuntimeDraw& a, const FlatRuntimeDraw& b) {
    FlatRuntimeDraw x = a, y = b;
    x.instances = y.instances = 0; x.key.camera = y.key.camera = nullptr; x.key.sequence = y.key.sequence = 0;
    for (uint32_t s = 0; s < kFlatProjectionSlots; ++s) x.key.projection[s].bytes = y.key.projection[s].bytes = nullptr;
    if (static_cast<bool>(a.key.camera) != static_cast<bool>(b.key.camera)) return false;
    return std::memcmp(&x, &y, sizeof(FlatRuntimeDraw)) == 0;
}
inline bool replayMarker(FlatRuntimePrefix& prefix, const FlatTraceEvent& e) {
    if (e.kind == kFlatTraceEventOverlayFailed) { flatRuntimeOverlayFailed(prefix, e.key.color); return true; }
    if (e.kind == kFlatTraceEventOverlaySeal) { flatRuntimeOverlaySeal(prefix, e.key.color); return true; }
    return false;
}

struct ReplayResult {
    uint32_t files = 0, frames = 0, draws = 0, candidates = 0, hits = 0, differentRecords = 0;
    std::vector<std::string> notes;
};

// One replay of the corpus. fast: the memo's rules. `dropped`: a key input the lookup ignores (the proof that the corpus itself can tell),
// or ~0u. `digests` collects one prefix digest per frame, in order; `contracts` one contract hash per frame.
inline void replayCorpus(bool fast, unsigned dropped, ReplayResult& out, std::vector<uint64_t>& digests, std::vector<uint64_t>& contracts) {
    namespace fs = std::filesystem;
    const fs::path dir("tools/flat_temporal_test/traces");
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir)) if (entry.path().extension() == ".bin") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    for (const auto& path : files) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        std::vector<unsigned char> bytes(static_cast<size_t>(file.tellg()));
        file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
        ++out.files;
        auto replayHeap = std::make_unique<FlatRuntimePrefix>();
        auto contractHeap = std::make_unique<FlatFrameContract>();
        auto memo = std::make_unique<FlatOffscreenMemo>();
        auto hdr = std::make_unique<FlatHdrFrame>();
        FlatRuntimePrefix& replay = *replayHeap;
        FlatFrameContract& rc = *contractHeap;
        FlatTraceFrameHeader cur{};
        bool open = false;
        auto finish = [&]() {
            if (!open || !cur.eventCount) return;
            ++out.frames;
            digests.push_back(prefixDigest(replay));
            contracts.push_back(rc.produced ? flatFrameContractHash(rc) : 0ull);
        };
        flatTraceParse(bytes.data(), bytes.size(),
            [&](const FlatTraceFrameHeader& h) {
                finish(); cur = h; open = true;
                { auto z = std::make_unique<FlatRuntimePrefix>(); replay = *z; }
                replay.frame = h.frame; replay.output = h.output; replay.width = h.width; replay.height = h.height; replay.format = h.format;
                { auto z = std::make_unique<FlatFrameContract>(); rc = *z; }
                memo->forget();
                flatHdrBeginFrame(*hdr, h.width, h.height);
            },
            [&](const FlatTraceEvent& e) {
                if (e.kind == kFlatTraceEventWriteResource) { flatRuntimeWritten(replay, e.key.color); flatHdrObserveExplicitWrite(*hdr, e.key.color); return; }
                if (e.kind == kFlatTraceEventDispatchWritten) { flatRuntimeDispatchObserveWritten(replay, e.key.color); flatHdrObserveDispatchWrite(*hdr, e.key.color); return; }
                if (e.kind == kFlatTraceEventMarkUncertain) { replay.uncertain = true; return; }
                if (e.kind == kFlatTraceEventCameraCapture) { ++replay.sequence; return; }
                if (replayMarker(replay, e)) return;
                FlatRuntimeDraw d = flatTraceEventToDraw(e);
                if (e.flags & kFlatTraceForeignWork) replay.uncertain = true;
                ++out.draws;
                const bool copy = d.key.vs == flat_mono_detail::kCopyVs && d.key.ps == flat_mono_detail::kCopyPs && d.key.color == replay.output;
                if (copy) { flatRuntimeObserveContract(replay, d, rc); flatHdrObserveDraw(*hdr, d.key, replay.sequence, nullptr, false); return; }
                // The fast path's rules. The trace carries no draw kind: any 3-vertex single instance counts as the fullscreen triangle.
                const FlatPairFacts pair = flatPairFactsFresh(d.key.vs, d.key.ps, true);
                FlatOffscreenArgs args; args.count = d.key.count; args.instances = d.instances; args.kind = 'D';
                const FlatOffscreenNo shape = flatOffscreenShapeVerdict(d.key, pair, replay.width, replay.height, replay.output, args);
                FlatOffscreenLive l;
                l.workFull = l.gpuFrameOpened = l.foregroundFrameReady = true;
                l.foreignWork = (e.flags & kFlatTraceForeignWork) != 0;
                for (uint32_t i = 0; i < replay.targetsUsed; ++i) l.overlayOpen = l.overlayOpen || replay.targets[i].overlayOpen;
                l.hdrCouldConsume = flatHdrCouldConsume(*hdr, d.key);
                const FlatOffscreenNo live = flatOffscreenLiveVerdict(l);
                const FlatOffscreenKey key = flatOffscreenKeyOf(d.key, 1, 1);
                const FlatRuntimeDraw* use = &d;
                bool wouldRecord = false;
                if (shape == FlatOffscreenNo::None && live == FlatOffscreenNo::None) {
                    ++out.candidates;
                    const bool hit = memo->valid() && memo->frame() == replay.frame && memo->key().same(key, dropped);
                    if (hit) {
                        if (!sameDrawRecord(memo->held(), d)) { ++out.differentRecords; if (out.notes.size() < 4) out.notes.push_back("a memo hit's record differs from the event's own"); }
                        if (fast) { use = &memo->use(d.instances); ++out.hits; }
                    } else wouldRecord = true;
                }
                const uint32_t targetsBefore = replay.targetsUsed;
                const bool uncertainBefore = replay.uncertain;
                flatRuntimeObserve(replay, *use);
                flatHdrObserveDraw(*hdr, use->key, replay.sequence, nullptr, false);
                if (fast && wouldRecord && replay.targetsUsed == targetsBefore && replay.uncertain == uncertainBefore) memo->record(replay.frame, key, d);
            });
        finish();
    }
}

// ---- helpers for the pure cases ----------------------------------------------------------------------------------------------------
inline FlatOffscreenKey baseKey() {
    FlatOffscreenKey k;
    k.color = reinterpret_cast<const void*>(0x1000); k.depth = reinterpret_cast<const void*>(0x2000);
    k.rtv = reinterpret_cast<const void*>(0x3000); k.dsv = reinterpret_cast<const void*>(0x4000); k.b1 = reinterpret_cast<const void*>(0x5000);
    k.vs = 0x1111; k.ps = 0x2222; k.cameraHash = 0xABCDEF; k.writeEpoch = 7; k.writeSeq = 9;
    k.width = 1024; k.height = 512; k.format = 28; k.depthWidth = 1024; k.depthHeight = 512; k.depthFormat = 45;
    k.viewportCount = 1; k.viewport[2] = 1024; k.viewport[3] = 512; k.viewport[5] = 1;
    k.hasCamera = true; k.rtvGen = 4; k.dsvGen = 5;
    return k;
}
// The key with exactly input `i` changed (the numbering of FlatOffscreenKey::same).
inline FlatOffscreenKey changed(FlatOffscreenKey k, unsigned i) {
    switch (i) {
    case 0: k.vs += 1; break;
    case 1: k.ps += 1; break;
    case 2: k.color = reinterpret_cast<const void*>(0x1008); break;
    case 3: k.depth = reinterpret_cast<const void*>(0x2008); break;
    case 4: k.rtv = reinterpret_cast<const void*>(0x3008); break;
    case 5: k.dsv = reinterpret_cast<const void*>(0x4008); break;
    case 6: k.b1 = reinterpret_cast<const void*>(0x5008); break;
    case 7: k.cameraHash += 1; break;
    case 8: k.writeEpoch += 1; break;
    case 9: k.writeSeq += 1; break;
    case 10: k.hasCamera = !k.hasCamera; break;
    case 11: k.height += 1; break;
    case 12: k.viewport[2] += 1; break;
    case 13: k.rtvGen += 1; break;
    case 14: k.dsvGen += 1; break;
    default: break;
    }
    return k;
}
inline FlatContractObservation shadowMap() {   // a depth-only draw into a 2048 square depth buffer
    FlatContractObservation k;
    k.depth = reinterpret_cast<const void*>(0x2000); k.dsv = reinterpret_cast<const void*>(0x4000);
    k.depthWidth = k.depthHeight = 2048; k.depthFormat = 45; k.vs = 0x1111; k.ps = 0x2222;
    k.viewportCount = 1; k.viewport[2] = k.viewport[3] = 2048; k.viewport[5] = 1;
    return k;
}
inline FlatOffscreenLive clearLive() {
    FlatOffscreenLive l; l.workFull = l.gpuFrameOpened = l.foregroundFrameReady = true; return l;
}
inline unsigned countOf(const std::string& text, const std::string& needle) {
    unsigned n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}
inline size_t at(const std::string& text, const std::string& needle, size_t from = 0) { return text.find(needle, from); }
inline std::string slurp(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace flat_offscreen_rig

inline int flatOffscreenTests() {
    using namespace edvr;
    using namespace flat_offscreen_rig;
    int failures = 0;
    auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: offscreen %s\n", name); ++failures; }
    };

    // ---- O1: every input of the key ------------------------------------------------------------------------------------------------
    {
        const FlatOffscreenKey k = baseKey();
        expect(k.same(baseKey()), "O1: identical keys are the same key");
        static const char* names[FlatOffscreenKey::kInputs] = {"vs", "ps", "color", "depth", "rtv", "dsv", "b1", "camera hash", "write epoch",
                                                                "write seq", "has camera", "extents and formats", "viewport", "rtv generation", "dsv generation"};
        for (unsigned i = 0; i < FlatOffscreenKey::kInputs; ++i) {
            const FlatOffscreenKey other = changed(k, i);
            char text[160];
            std::snprintf(text, sizeof(text), "O1: a draw that differs only in its %s is a different key", names[i]);
            expect(!k.same(other), text);
            std::snprintf(text, sizeof(text), "O1: the %s is the input that tells them apart: a comparison that ignores it matches them", names[i]);
            expect(k.same(other, i), text);
            bool others = true;
            for (unsigned j = 0; j < FlatOffscreenKey::kInputs; ++j) if (j != i && k.same(other, j)) others = false;
            std::snprintf(text, sizeof(text), "O1: and no other input does (%s)", names[i]);
            expect(others, text);
        }
        FlatContractObservation obs = shadowMap(); obs.cameraHash = 5; obs.writeEpoch = 6; obs.writeSeq = 7; obs.b1 = reinterpret_cast<const void*>(0x5000);
        const FlatOffscreenKey ko = flatOffscreenKeyOf(obs, 8, 9);
        expect(ko.depth == obs.depth && ko.dsv == obs.dsv && ko.cameraHash == 5 && ko.writeEpoch == 6 && ko.writeSeq == 7 && ko.b1 == obs.b1 &&
                   ko.rtvGen == 8 && ko.dsvGen == 9 && !ko.hasCamera && ko.viewportCount == 1 && ko.viewport[2] == 2048.0f,
               "O1: the key of an observation carries each input");
    }

    // ---- O2: every live condition ----------------------------------------------------------------------------------------------------
    {
        expect(flatOffscreenLiveVerdict(clearLive()) == FlatOffscreenNo::None, "O2: all clear: the fast path may serve");
        struct Case { const char* name; FlatOffscreenNo want; void (*flip)(FlatOffscreenLive&); };
        const Case cases[] = {
            {"the frame is not Full", FlatOffscreenNo::Work, [](FlatOffscreenLive& l) { l.workFull = false; }},
            {"the GPU span is not open yet", FlatOffscreenNo::FrameState, [](FlatOffscreenLive& l) { l.gpuFrameOpened = false; }},
            {"the foreground domain's frame reset has not run", FlatOffscreenNo::FrameState, [](FlatOffscreenLive& l) { l.foregroundFrameReady = false; }},
            {"an ingress audit is counting", FlatOffscreenNo::Audit, [](FlatOffscreenLive& l) { l.auditing = true; }},
            {"a capture is armed", FlatOffscreenNo::Diagnostics, [](FlatOffscreenLive& l) { l.diagnostics = true; }},
            {"foreign work was seen", FlatOffscreenNo::Foreign, [](FlatOffscreenLive& l) { l.foreignWork = true; }},
            {"an overlay suffix is open", FlatOffscreenNo::Overlay, [](FlatOffscreenLive& l) { l.overlayOpen = true; }},
            {"the F10 projection audit counts", FlatOffscreenNo::ProjectionAudit, [](FlatOffscreenLive& l) { l.projectionAudit = true; }},
            {"the weapon footprint probe is armed", FlatOffscreenNo::WeaponFootprint, [](FlatOffscreenLive& l) { l.weaponFootprint = true; }},
            {"the foreground probe is active and the draw has a depth", FlatOffscreenNo::Foreground, [](FlatOffscreenLive& l) { l.foregroundTouchesDepth = true; }},
            {"the HDR detector might fire", FlatOffscreenNo::HdrConsumer, [](FlatOffscreenLive& l) { l.hdrCouldConsume = true; }},
        };
        for (const Case& c : cases) {
            FlatOffscreenLive l = clearLive(); c.flip(l);
            char text[200];
            std::snprintf(text, sizeof(text), "O2: forbidden when %s, and it says why", c.name);
            expect(flatOffscreenLiveVerdict(l) == c.want, text);
        }
        FlatOffscreenLive two = clearLive(); two.hdrCouldConsume = true; two.foreignWork = true;
        expect(flatOffscreenLiveVerdict(two) == FlatOffscreenNo::Foreign, "O2: two bad fields answer with the earlier reason");
    }

    // ---- O3: every draw condition ----------------------------------------------------------------------------------------------------
    {
        const uint32_t pw = 1920, ph = 1080;
        const void* output = reinterpret_cast<const void*>(0x9000);
        FlatOffscreenArgs plain; plain.kind = 'I'; plain.count = 600; plain.instances = 1;
        const FlatPairFacts none;   // a pair the tables do not name
        auto verdict = [&](const FlatContractObservation& k, const FlatPairFacts& pair, const FlatOffscreenArgs& a) {
            return flatOffscreenShapeVerdict(k, pair, pw, ph, output, a);
        };
        FlatContractObservation shadow = shadowMap();
        expect(verdict(shadow, none, plain) == FlatOffscreenNo::None, "O3: a depth-only draw into a 2048 square shadow map is served");
        FlatContractObservation blur = shadowMap();
        blur.color = reinterpret_cast<const void*>(0x1000); blur.width = 480; blur.height = 270; blur.format = 28;
        blur.depth = nullptr; blur.dsv = nullptr; blur.depthWidth = blur.depthHeight = 0;
        expect(verdict(blur, none, plain) == FlatOffscreenNo::None, "O3: a draw into a small intermediate colour target with no depth is served");
        { FlatPairFacts p; p.supported = true; expect(verdict(shadow, p, plain) == FlatOffscreenNo::Pair, "O3: a keyed pool-family pair is refused"); }
        { FlatPairFacts p; p.toneSlot = 0; expect(verdict(shadow, p, plain) == FlatOffscreenNo::Pair, "O3: a tone pass is refused"); }
        { FlatPairFacts p; p.mapToneSlot = 1; expect(verdict(shadow, p, plain) == FlatOffscreenNo::Pair, "O3: the map's tonemap is refused"); }
        { FlatPairFacts p; p.sourceTone = true; expect(verdict(shadow, p, plain) == FlatOffscreenNo::Pair, "O3: a UI-only source tone is refused"); }
        { FlatPairFacts p; p.copyPair = true; expect(verdict(shadow, p, plain) == FlatOffscreenNo::Pair, "O3: the final copy's pair is refused"); }
        { FlatPairFacts p; p.uiMayAsk = true; expect(verdict(shadow, p, plain) == FlatOffscreenNo::Pair, "O3: a pair a UI family can name is refused"); }
        { FlatContractObservation k = shadow; k.ps = flat_mono_detail::kCopyPs; expect(verdict(k, none, plain) == FlatOffscreenNo::Pair, "O3: the copy's pixel shader under any vertex shader is refused"); }
        for (uint32_t format : {9u, 23u, 26u, 27u, 60u}) {
            FlatContractObservation k = blur; k.format = format;
            char text[120]; std::snprintf(text, sizeof(text), "O3: a colour target of format %u (a format the skipped blocks act on) is refused", format);
            expect(verdict(k, none, plain) == FlatOffscreenNo::Format, text);
        }
        { FlatContractObservation k = blur; k.color = output; expect(verdict(k, none, plain) == FlatOffscreenNo::Format, "O3: the output is refused"); }
        { FlatContractObservation k = blur; k.format = 28; k.width = 1920; k.height = 1080; k.depth = nullptr;
          // 28 is not a screen format: the contract kind is None, so this is a full-size intermediate, served; with a native-size depth it is not.
          expect(verdict(k, none, plain) == FlatOffscreenNo::None, "O3: a full-size colour target of an ordinary format and no depth is served"); }
        { FlatContractObservation k = shadow; k.depthWidth = pw; k.depthHeight = ph; expect(verdict(k, none, plain) == FlatOffscreenNo::Extent, "O3: a depth of the scene's size is refused (the foreground domain's block reads it)"); }
        { FlatOffscreenArgs a = plain; a.kind = 'D'; a.count = 3; a.instances = 1; expect(verdict(shadow, none, a) == FlatOffscreenNo::Shape, "O3: a fullscreen triangle is refused (D)");
          a.kind = 'N'; expect(verdict(shadow, none, a) == FlatOffscreenNo::Shape, "O3: a fullscreen triangle is refused (N)");
          a.kind = 'I'; expect(verdict(shadow, none, a) == FlatOffscreenNo::None, "O3: three indices is not a triangle draw (I)");
          a.kind = 'D'; a.instances = 2; expect(verdict(shadow, none, a) == FlatOffscreenNo::None, "O3: two instances is not a fullscreen triangle"); }
        expect(flatOffscreenActedOnFormat(9) && flatOffscreenActedOnFormat(26) && !flatOffscreenActedOnFormat(28) && !flatOffscreenActedOnFormat(0),
               "O3: the formats the skipped blocks act on");
    }

    // ---- O4: the memo ------------------------------------------------------------------------------------------------------------------
    {
        auto memo = std::make_unique<FlatOffscreenMemo>();
        const FlatOffscreenKey k = baseKey();
        expect(memo->lookup(5, k) == FlatOffscreenNo::NoMemo, "O4: nothing verified yet");
        FlatRuntimeDraw d{};
        d.supported = false; d.instances = 3;
        d.key.vs = 0x1111; d.key.ps = 0x2222; d.key.format = 28;
        for (uint32_t i = 0; i < kFlatCameraBytes; ++i) d.camera[i] = static_cast<unsigned char>(i + 1);
        d.key.camera = d.camera;
        memo->record(5, k, d);
        expect(memo->valid() && memo->lookup(5, k) == FlatOffscreenNo::None, "O4: a recorded key is found");
        expect(memo->held().key.camera == memo->held().camera && memo->held().key.camera != d.camera,
               "O4: the record keeps its own copy of the camera rows: its pointer is the memo's, not the source draw's");
        expect(memo->lookup(6, k) == FlatOffscreenNo::OtherFrame, "O4: another frame says so");
        expect(memo->lookup(5, changed(k, 7)) == FlatOffscreenNo::KeyDiffers, "O4: another key says so");
        // The memo's copy of the rows: the source draw goes away, the record stands.
        std::memset(d.camera, 0, sizeof(d.camera)); d.key.camera = nullptr;
        FlatRuntimeDraw& use = memo->use(11);
        expect(use.instances == 11 && use.key.camera == use.camera && use.camera[0] == 1 && use.camera[kFlatCameraBytes - 1] == kFlatCameraBytes &&
                   use.key.vs == 0x1111,
               "O4: a hit's record carries its own rows (the pointer is the memo's), the verified draw's key, and only the instance count is the draw's");
        FlatRuntimeDraw none{}; none.key.camera = nullptr;
        auto fresh = std::make_unique<FlatOffscreenMemo>();
        fresh->record(1, k, none);
        expect(fresh->use(2).key.camera == nullptr, "O4: a draw with no camera rows has none when reused");
        memo->forget();
        expect(memo->lookup(5, k) == FlatOffscreenNo::NoMemo, "O4: forgetting empties it");
        // Recording the held key again copies nothing (the rig sees it by a changed field staying).
        memo->record(5, k, d);
        const uint32_t seen = memo->held().instances;
        FlatRuntimeDraw d2 = d; d2.instances = 99;
        memo->record(5, k, d2);
        expect(memo->held().instances == seen, "O4: recording the key already held leaves the record alone");
        memo->record(6, k, d2);
        expect(memo->held().instances == 99 && memo->frame() == 6, "O4: a new frame replaces it");
    }

    // ---- O6: the line ------------------------------------------------------------------------------------------------------------------
    {
        FlatOffscreenCounts c;
        char text[320];
        flatOffscreenFormat(text, sizeof(text), c, 10.0);
        expect(std::string(text) == "offscreen fast path: 0.0 draws a frame skipped the draw scope's constructor and destructor (0.0 a frame were verified on the slow path first; "
                                    "0.0 a frame matched but had to ask why not)",
               "O6: the zero form says the block ran and did nothing");
        c.fast = 37730; c.recorded = 410; c.declined[static_cast<unsigned>(FlatOffscreenNo::Foreign)] = 120; c.declined[static_cast<unsigned>(FlatOffscreenNo::Overlay)] = 30;
        flatOffscreenFormat(text, sizeof(text), c, 10.0);
        expect(std::string(text) == "offscreen fast path: 3773.0 draws a frame skipped the draw scope's constructor and destructor (41.0 a frame were verified on the slow path "
                                    "first; 15.0 a frame matched but had to ask why not: foreign work 12.0, overlay 3.0)",
               "O6: the counts a frame, the verified ones, and the reasons largest first");
        for (unsigned i = 1; i < kFlatOffscreenReasons; ++i) c.declined[i] = 100000000000ull * i;
        c.fast = c.recorded = 99999999999999ull;
        flatOffscreenFormat(text, sizeof(text), c, 1.0);
        expect(std::strlen(text) < sizeof(text) - 1 && std::strstr(text, "and ") != nullptr && std::strstr(text, "more") != nullptr && text[std::strlen(text) - 1] == ')',
               "O6: at absurd counts it names the three largest reasons, counts the rest, and still fits the token whole");
        flatOffscreenFormat(text, sizeof(text), FlatOffscreenCounts{}, 0.0);
        expect(std::strstr(text, "0.0 draws a frame") != nullptr, "O6: a window with no frames prints zeros, not a division");
    }

    // ---- O5: the wiring ----------------------------------------------------------------------------------------------------------------
    {
        const std::string runtime = slurp("src/d3d11/flat_runtime.cpp");
        const std::string model = slurp("src/d3d11/flat_runtime_model.h");
        const std::string hdr = slurp("src/d3d11/flat_hdr_route.h");
        const std::string untrusted = slurp("src/d3d11/flat_untrusted_coverage.h");
        expect(!runtime.empty() && !model.empty() && !hdr.empty() && !untrusted.empty(), "O5: the sources are readable from the repo root");
        const size_t liveFrom = at(runtime, "FlatOffscreenLive flatOffscreenLiveOf(");
        const size_t keyFrom = at(runtime, "bool flatOffscreenKeyNow(");
        const size_t tryFrom = at(runtime, "bool flatOffscreenTry(");
        const size_t recordFrom = at(runtime, "void flatOffscreenRecord(");
        const size_t ctorFrom = at(runtime, "FlatRuntimeDrawScope::FlatRuntimeDrawScope(");
        const size_t ctorTo = at(runtime, "bool FlatRuntimeDrawScope::recover(");
        const bool found = liveFrom != std::string::npos && keyFrom > liveFrom && tryFrom > keyFrom && recordFrom > tryFrom && ctorFrom > recordFrom && ctorTo > ctorFrom;
        expect(found, "O5: the fast path's functions and the constructor can be delimited");
        if (found) {
            const std::string liveBody = runtime.substr(liveFrom, keyFrom - liveFrom), keyBody = runtime.substr(keyFrom, tryFrom - keyFrom),
                              tryBody = runtime.substr(tryFrom, recordFrom - tryFrom), recordBody = runtime.substr(recordFrom, ctorFrom - recordFrom),
                              ctor = runtime.substr(ctorFrom, ctorTo - ctorFrom);
            struct Pin { const std::string* text; const char* needle; unsigned times; const char* what; };
            // Each live field is filled from the state its comment in flat_offscreen_fast.h names.
            const Pin pins[] = {
                {&liveBody, "live.workFull = s.work == FlatWork::Full;", 1, "workFull comes from the work tier"},
                {&liveBody, "live.gpuFrameOpened = s.gpuFrameTried;", 1, "gpuFrameOpened comes from the frame's GPU span flag"},
                {&liveBody, "live.foregroundFrameReady = s.engine == FlatMonoResolveMode::Taa || s.foregroundDomainFrame == s.prefix.frame;", 1, "foregroundFrameReady comes from the domain's frame reset"},
                {&liveBody, "live.auditing = auditing;", 1, "auditing is the constructor's ingress flag"},
                {&liveBody, "live.diagnostics = flatTemporalNeedsOriginalState() || s.drawCapture.active() || s.drawPackets.armed();", 1, "diagnostics is the constructor's own physical-state definition"},
                {&liveBody, "live.foreignWork = foreignWork.load(std::memory_order_acquire);", 1, "foreignWork is the constructor's flag"},
                {&liveBody, "live.overlayOpen = overlayOpen(s) || overlaySuffixActive.load(std::memory_order_acquire);", 1, "overlayOpen is the model's open suffix or the active flag"},
                {&liveBody, "live.projectionAudit = s.projectionFrames != 0;", 1, "projectionAudit is the F10 audit's frame count"},
                {&liveBody, "live.weaponFootprint = s.weaponFootprint.active();", 1, "weaponFootprint is the probe's armed state"},
                {&liveBody, "live.foregroundTouchesDepth = s.foreground.active() && k.depth != nullptr;", 1, "foregroundTouchesDepth is the probe's active state and the draw's depth"},
                {&liveBody, "live.hdrCouldConsume = flatHdrCouldConsume(s.hdr, k);", 1, "hdrCouldConsume is the detector's own gate"},
                // The key from the binding shadow.
                {&keyBody, "now.rtv = bindingGet(BindSlot::Rtv0); now.dsv = bindingGet(BindSlot::Dsv0);", 1, "the key reads the RTV and DSV bindings"},
                {&keyBody, "now.rtvGen = bindingGeneration(BindSlot::Rtv0); now.dsvGen = bindingGeneration(BindSlot::Dsv0);", 1, "and their generations"},
                {&keyBody, "now.vs = bindingShaderHash(BindSlot::Vs); now.ps = bindingShaderHash(BindSlot::Ps);", 1, "the shader pair's hashes"},
                {&keyBody, "now.b1 = bindingGet(BindSlot::VsCb1);", 1, "the constant buffer at b1"},
                {&keyBody, "now.viewportCount = s.viewportCount;", 1, "the viewport count"},
                {&keyBody, "std::memcpy(now.viewport, &s.viewport, sizeof(now.viewport));", 1, "and the viewport"},
                {&keyBody, "rows = s.cameras.probe(now.b1, bindingGeneration(BindSlot::VsCb1), s.prefix.frame);", 1, "the camera rows come from the table's probe, as in the constructor"},
                {&keyBody, "now.cameraHash = rows->have ? rows->hash : 0; now.writeEpoch = rows->have ? rows->epoch : 0; now.writeSeq = rows->have ? rows->sequence : 0;", 1, "with their hash and write provenance"},
                // The fast path's steps.
                {&tryBody, "s.drawVs = k.vs; s.drawPs = k.ps;", 1, "the current draw's identities are stored"},
                {&tryBody, "engineVelocityFlatLazy(true);", 1, "engine motion's lazy form is set"},
                {&tryBody, "flatRuntimeSubstitution(context, FlatSubstEvent::kOtherDraw);", 1, "engine motion's state goes back, as for every draw that is not a producer"},
                {&tryBody, "flatRuntimeObserve(s.prefix, d);", 1, "the reducer runs for real"},
                {&tryBody, "flatHdrObserveDraw(s.hdr, k, s.prefix.sequence, nullptr, false);", 1, "the HDR detector observes the draw"},
                {&tryBody, "flatTraceRecord(s.traceRing, d, foreignWork.load(std::memory_order_acquire), nullptr, s.prefix.sequence);", 1, "the trace ring takes its entry"},
                {&tryBody, "if (flatUiLayerOn()) flatUiLayerNoteSceneDraw(context, a.count, a.instances, a.kind);", 1, "the UI layer's scene-draw note is made"},
                {&tryBody, "s.colors[oldTargets] = static_cast<ID3D11Resource*>(const_cast<void*>(k.color));", 1, "a target the reducer registered gets its resource bookkeeping"},
                {&tryBody, "flatOffscreenLiveVerdict(flatOffscreenLiveOf(s, auditing, memo.held().key))", 1, "the live conditions are asked of the held draw's key"},
                {&tryBody, "memo.lookup(s.prefix.frame, now) != FlatOffscreenNo::None", 1, "the memo is asked with the frame"},
                {&tryBody, "flatOffscreenFullscreenTriangle(a)", 1, "a fullscreen triangle never takes the path"},
                // The record.
                {&recordBody, "flatOffscreenShapeVerdict(k, pair, s.prefix.width, s.prefix.height, s.prefix.output, a) != FlatOffscreenNo::None", 1, "only a draw the shape verdict serves is remembered"},
                {&recordBody, "flatOffscreenLiveVerdict(flatOffscreenLiveOf(s, auditing, k)) != FlatOffscreenNo::None", 1, "and only under the live conditions"},
                {&recordBody, "if (!plain || s.prefix.targetsUsed != oldTargets || s.prefix.uncertain != uncertainBefore) return;", 1, "and only a draw that was plain and changed no target or certainty"},
                {&recordBody, "flatOffscreenKeyOf(k, bindingGeneration(BindSlot::Rtv0), bindingGeneration(BindSlot::Dsv0))", 1, "with the generations of the bindings"},
                // The constructor.
                {&ctor, "if (flatOffscreenTry(s, context, offscreenArgs, auditing)) return;", 1, "the constructor asks the fast path once"},
                {&ctor, "const FlatOffscreenArgs offscreenArgs = {kind, count, instances, startInstance};", 1, "with the draw's own arguments"},
                {&ctor, "flatOffscreenRecord(s, d, pairFact, offscreenArgs, auditing, plain, offscreenTargetsBefore, offscreenUncertainBefore);", 1, "and records after its last skipped block"},
            };
            for (const Pin& pin : pins) {
                const std::string label = std::string("O5: ") + pin.what;
                expect(countOf(*pin.text, pin.needle) == pin.times, label.c_str());
                std::string mutated = *pin.text;
                const size_t i = mutated.find(pin.needle);
                if (i != std::string::npos) mutated.erase(i, std::strlen(pin.needle));
                expect(i != std::string::npos && countOf(mutated, pin.needle) != pin.times, (std::string("O5: control: ") + pin.what + " (removed, the pin fails)").c_str());
            }
            // The order the fast path follows is the slow path's: the live verdict before anything is done; the substitution, the reducer, the
            // detector, the trace and the UI note after it, in that order.
            {
                const size_t verdict = at(tryBody, "flatOffscreenLiveVerdict("), lazy = at(tryBody, "engineVelocityFlatLazy(true);"), subst = at(tryBody, "flatRuntimeSubstitution("),
                             reduce = at(tryBody, "flatRuntimeObserve("), detector = at(tryBody, "flatHdrObserveDraw("), trace = at(tryBody, "flatTraceRecord("),
                             note = at(tryBody, "flatUiLayerNoteSceneDraw(");
                expect(verdict < lazy && lazy < subst && subst < reduce && reduce < detector && detector < trace && trace < note,
                       "O5: the fast path asks, then restores, reduces, observes, records the trace and notes the UI layer, in the slow path's order");
                const size_t sReduce = at(ctor, "flatRuntimeObserve(s.prefix, d)"), sDetect = at(ctor, "flatHdrObserveDraw(s.hdr, k, s.prefix.sequence"),
                             sTrace = at(ctor, "flatTraceRecord(s.traceRing, d,"), sNote = at(ctor, "flatUiLayerNoteSceneDraw(ctx, count, instances, kind)"),
                             sSubst = at(ctor, "if (!d.supported) flatRuntimeSubstitution(context, FlatSubstEvent::kOtherDraw);");
                expect(sSubst < sReduce && sReduce < sDetect && sDetect < sTrace && sTrace < sNote && sSubst != std::string::npos && sNote != std::string::npos,
                       "O5: and the constructor does the same five things in that order");
            }
            // Where the constructor asks: after the Paused return and the audit count, before it builds, scopes or reads anything.
            {
                const size_t paused = at(ctor, "if (s.work == FlatWork::Paused) return;"), ask = at(ctor, "if (flatOffscreenTry("), shell = at(ctor, "flatcpu::Scope shell(flatcpu::kOther);"),
                             built = at(ctor, "FlatRuntimeDraw d{}; auto& k = d.key;"), views = at(ctor, "view(BindSlot::Rtv0, 0)"), domain = at(ctor, "foregroundRoute.beginFrame(s.prefix.frame);");
                expect(paused < ask && ask < shell && ask < built && ask < views && ask < domain, "O5: the constructor asks after the Paused return and before it builds, scopes or reads anything");
                const size_t record = at(ctor, "flatOffscreenRecord("), uiNote = at(ctor, "flatUiLayerNoteSceneDraw(ctx, count, instances, kind)"), copyReturn = at(ctor, "if (s.hdrTreated) {");
                expect(uiNote < record && record < copyReturn, "O5: and records after the last block it skips (the UI layer's note) and before the copy draw's own work");
            }
            // The plain flag names every outcome of a skipped block.
            {
                const size_t from = at(ctor, "const bool plain ="), to = at(ctor, "flatOffscreenRecord(");
                const std::string plain = from == std::string::npos || to == std::string::npos ? std::string() : ctor.substr(from, to - from);
                for (const char* flag : {"!tone", "!producer", "!projection &&", "!projectionPlan", "!deferredPlan", "!drawCaptureStarted", "!drawPacket", "!weaponFootprintStarted",
                                         "!overlayPlanned", "!d.overlayProtected", "!d.alternateHdr", "!d.firstPersonCohort", "!foregroundPlanned", "!untrustedPlanned",
                                         "!domainPlanned", "!uiTake", "!uiTone", "!uiComposite", "!replaced", "!hdrTrigger", "cameraProbeAttempt == 0"})
                    expect(countOf(plain, flag) == 1, (std::string("O5: a draw is remembered only if ") + flag + " held").c_str());
            }
            // The Present drains the counts with the wrapper's, and always clears them.
            {
                const size_t wrapper = at(runtime, "s.census.noteWrapper(wrapper.stateCalls, wrapper.substitutedDraws);"), note = at(runtime, "s.census.noteOffscreen(s.offscreenCounts);"),
                             clear = at(runtime, "s.offscreenCounts = FlatOffscreenCounts{};");
                expect(wrapper != std::string::npos && wrapper < note && note < clear, "O5: the Present hands the counts to the census and clears them, after the wrapper's");
            }
            // THE GATES OF THE SKIPPED BLOCKS. Each is the condition that makes the block a no-op for a draw the shape verdict serves; the fast path is
            // sound only while they stand. A change to one fails here until the path is reviewed against it.
            {
                struct Gate { const std::string* text; const char* needle; const char* what; };
                const Gate gates[] = {
                    {&runtime, "if(k.vs!=kHdrCopyVs || k.ps!=kHdrCopyPs || k.format!=26)return false;", "verifyHdrCopy returns at once off its pair and format 26"},
                    {&runtime, "if(k.vs!=kMenuCopyVs || k.ps!=kMenuCopyPs || k.format!=26)return false;", "verifyMenuHdrCopy returns at once off its pair and format 26"},
                    {&runtime, "if(k.format!=9 || !flatRuntimeCameraIndependentImageSourcePair(k.vs,k.ps))return false;", "verifyCameraIndependentImageSource returns at once off format 9"},
                    {&runtime, "if(k.format!=kFlatDeferredHdrFormat || k.vs!=kFlatDeferredHdrVs || k.ps!=kFlatDeferredHdrPs || k.camera)return nullptr;", "verifyDeferredHdrCamera returns at once off format 26 and its pair"},
                    {&runtime, "if(!s.projectionFrames || FlatCameraProbe::pairIndex(k.vs,k.ps)==kFlatCameraProbePairCount)return 0;", "captureCameraConflict returns at once outside the F10 audit"},
                    {&runtime, "if(!s.projectionFrames || k.vs!=kHdrCopyVs || k.ps!=kHdrCopyPs) return;", "captureCopyProvenance returns at once outside the F10 audit"},
                    {&runtime, "if(!s.projectionFrames || !c.armed || c.exhausted())return;", "captureHdrProjectionDraw returns at once outside the F10 audit"},
                    {&runtime, "if(s.weaponFootprint.active() && k.vs==0x025B4B9FF54622EDull &&", "the weapon footprint acts only while armed"},
                    {&runtime, "if(d.supported && k.camera && k.depth && k.kind==kFlatContractPool && k.format==23 && weaponMotionFamilyVs(k.vs) &&", "the first-person cohort is a supported format-23 pool draw"},
                    {&runtime, "k.format==26 && k.color && k.depth && k.dsv && k.camera &&", "the overlay candidate is a format-26 draw with colour, depth and camera"},
                    {&runtime, "if(k.depth && overlaySuffixActive.load(std::memory_order_relaxed))", "the effective-depth query needs an active overlay suffix"},
                    {&runtime, "const bool sceneExtent = flatContractKind(false, k.color, k.depth, k.width, k.height, k.format==9?26:k.format, s.prefix.width, s.prefix.height, false) == kFlatContractScreen;",
                     "sceneExtent is the contract kind the shape verdict repeats"},
                    {&runtime, "const bool sourceShape=d.supported && !weaponMotionFamilyVs(k.vs) && k.camera && k.depth && sceneExtent &&", "a motion-source shape needs a supported pair"},
                    {&runtime, "!s.untrusted.finished() && !inertSource && k.format==23 && sceneExtent &&", "the unclassified pool block needs format 23 at the scene's extent"},
                    {&untrusted, "out.candidate=enabled && !namesWorldSource && k.format==23 && sceneExtent && k.color &&", "an untrusted nominee is format 23 at the scene's extent"},
                    {&runtime, "const bool foregroundCandidate=d.supported && weaponMotionFamilyVs(k.vs) && k.camera && k.depth &&", "a foreground candidate is a supported pair"},
                    {&runtime, "const bool foregroundGap=s.foreground.needsPreWorldState(", "the foreground gap is asked of the probe and the draw's depth"},
                    {&runtime, "if(s.engine!=FlatMonoResolveMode::Taa && s.work==FlatWork::Full && k.depth && (sceneExtent || nativeDepthExtent)) {", "the domain block needs a depth of the scene's size or a scene-extent target"},
                    {&runtime, "const bool nativeDepthExtent=k.depthWidth==s.prefix.width && k.depthHeight==s.prefix.height;", "nativeDepthExtent is the depth against the prefix size"},
                    {&runtime, "if(s.foreground.active() && !foregroundCandidate) {", "the foreground's after-world note needs an active probe"},
                    {&runtime, "if(s.projectionFrames)++s.projectionDraws;", "the projection draw count is the F10 audit's"},
                    {&runtime, "if(sceneExtent && k.color!=s.prefix.output && (k.format==9 || k.format==23 || k.format==26 || k.format==60)) {", "the coverage classification needs a scene-extent target of a screen format"},
                    {&runtime, "if (pairFact.uiMayAsk) {", "the UI layer's decision needs a pair a family can name"},
                    {&runtime, "else if (k.ps == flat_mono_detail::kCopyPs) {", "the UI layer's plain-copy branch needs the copy's pixel shader"},
                    {&runtime, "else if (count == 3 && instances == 1 && (kind == 'D' || kind == 'N')) {", "the UI layer's tone probes need a fullscreen triangle"},
                    {&runtime, "const bool trailDraw = (count == 3 && instances == 1 && (kind == 'D' || kind == 'N')) || tone ||", "the back-off trail notes a fullscreen triangle or a tone"},
                    {&runtime, "if (!uiTake && !copy) flatUiLayerNoteSceneDraw(ctx, count, instances, kind);", "the UI layer's scene-draw note is made for every draw it leaves"},
                    {&model, "if (!k.color) return out;", "the reducer ends a colourless draw after the suffix rule"},
                    {&model, "if (k.format == 26 && k.vs == kMenuCopyVs && k.ps == kMenuCopyPs) {", "the menu copy is format 26"},
                    {&model, "const bool imageCopy = k.format == 26 && k.vs == kHdrImageCopyVs && k.ps == kHdrImageCopyPs;", "the image copy is format 26"},
                    {&model, "if (k.format == 26) {", "the HDR camera rules are format 26"},
                    {&model, "if (k.format == 9) {", "the image intermediate rules are format 9"},
                    {&hdr, "if (!flatHdrCouldConsume(f, k)) return false;", "the detector fires only where flatHdrCouldConsume says it could"},
                    {&hdr, "if (f.triggered || !f.candidateCount || !k.color || k.depth || k.dsv) return false;", "a consumer is a colour-only draw with a candidate H"},
                };
                for (const Gate& g : gates) {
                    const std::string label = std::string("O5: gate: ") + g.what;
                    expect(countOf(*g.text, g.needle) >= 1, label.c_str());
                }
            }
            // The first lines of the constructor's skipped work are still the ones this path was reasoned against.
            expect(countOf(ctor, "if (!s.gpuFrameTried) gpuFrameOpen(s, context);") + countOf(runtime, "if (!s.gpuFrameTried) gpuFrameOpen(s, context);") >= 1,
                   "O5: gate: the frame's first draw opens the GPU span");
            expect(countOf(ctor, "s.foregroundDomainFrame!=s.prefix.frame") >= 1, "O5: gate: the foreground domain's once-a-frame reset is keyed on the frame");
            expect(countOf(ctor, "const bool diagnostics = flatTemporalNeedsOriginalState() || s.drawCapture.active() || s.drawPackets.armed();") == 1,
                   "O5: gate: the constructor's diagnostics definition is the one the live field repeats");
            expect(countOf(ctor, "if (!d.supported) flatRuntimeSubstitution(context, FlatSubstEvent::kOtherDraw);") == 1,
                   "O5: gate: a draw that is not supported sees the game's state");
        }
    }

    // ---- O7: the decisive replay ----------------------------------------------------------------------------------------------------------
    {
        ReplayResult base, fast;
        std::vector<uint64_t> baseDigest, fastDigest, baseContract, fastContract;
        replayCorpus(false, ~0u, base, baseDigest, baseContract);
        replayCorpus(true, ~0u, fast, fastDigest, fastContract);
        std::printf("offscreen fast path replay: %u file(s), %u frames, %u draws, %u class-4 candidates, %u memo hits (%.1f%% of the draws)\n", fast.files, fast.frames,
                    fast.draws, fast.candidates, fast.hits, fast.draws ? 100.0 * fast.hits / fast.draws : 0.0);
        expect(fast.frames > 0 && fast.frames == base.frames, "O7: the replay covers the corpus");
        expect(fast.hits > 100, "O7: the corpus holds draws the memo serves (without hits the replay proves nothing)");
        expect(baseContract == fastContract, "O7: every frame's contract hash is identical with the fast path on and off");
        expect(baseDigest == fastDigest, "O7: every frame's prefix (targets, sources, counters) is identical with the fast path on and off");
        expect(fast.differentRecords == 0, "O7: a memo hit's draw record equals the event's own but for the instance count");
        for (const auto& n : fast.notes) std::printf("  note: %s\n", n.c_str());
        // The corpus can itself tell some inputs apart: with one ignored by the lookup, how many hits' records differ from the events' own?
        static const char* names[FlatOffscreenKey::kInputs] = {"vs", "ps", "color", "depth", "rtv", "dsv", "b1", "camera hash", "write epoch",
                                                                "write seq", "has camera", "extents and formats", "viewport", "rtv generation", "dsv generation"};
        std::string told;
        for (unsigned i = 0; i < FlatOffscreenKey::kInputs; ++i) {
            ReplayResult r; std::vector<uint64_t> a, b;
            replayCorpus(false, i, r, a, b);
            if (r.differentRecords) { told += told.empty() ? "" : ", "; told += names[i]; }
        }
        std::printf("offscreen fast path replay: the corpus alone tells these key inputs apart (a lookup that ignores one serves a different record): %s\n",
                    told.empty() ? "(none)" : told.c_str());
    }
    return failures;
}

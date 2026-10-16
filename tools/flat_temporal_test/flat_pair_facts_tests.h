#pragma once

// The shader-pair memo (src\d3d11\flat_pair_facts.h): what the flat draw scope's constructor asks of a draw's (vs, ps) hashes, per pair.
//
// What it holds to, each a way a memo here could silently change a decision:
//   - EQUIVALENCE: over every hash the facts read (the family tables, the tone and copy pairs, the map tonemap, the UI-only source tones,
//     the UI layer's own families), unknown hashes, zero, a cross product of all of them, in both profiles, asked twice (the hit path) and
//     through a table of four slots (collisions, evictions), the memo's answer is the fresh computation's, field by field
//   - THE KEY IS COMPLETE: the memo built to ignore the vertex shader, the pixel shader or the profile, in turn, is seen to DISAGREE with
//     the fresh computation on the same stream. Each input of the key is therefore needed by a test that fails without it, and a stream
//     that could not tell would fail the corpus checks below (every fact is reached; the profile changes an answer)
//   - IT MEMOISES: a second pass over the stream misses nothing
//   - THE SOURCES THE EQUIVALENCE RESTS ON: the engine's two pair questions are the inline calls the fresh computation makes, the UI
//     layer's decision returns before any counter or log for a pair its family rule does not name, and the constructor asks the memo
//     (and nothing else) for the facts it replaced

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../../src/d3d11/flat_pair_facts.h"
#include "../../src/d3d11/ui_layer_math.h"

inline int flatPairFactsTests() {
    using namespace edvr;
    int failures = 0;
    auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: pair facts %s\n", name); ++failures; }
    };

    // ---- the corpus: every hash the facts read ---------------------------------------------------------------------------
    std::vector<uint64_t> h = {0, 1, 0xFFFFFFFFFFFFFFFFull};
    for (int i = 0; i < engine_velocity_family::kFamilyCount; ++i) {
        const auto& f = engine_velocity_family::kFamilies[i];
        h.push_back(f.vs);
        for (uint64_t p : f.ps) h.push_back(p);
        h.push_back(f.flatPs);
    }
    for (const auto& m : engine_velocity_family::kSelfMarking) { h.push_back(m.vs); h.push_back(m.ps); }
    for (uint64_t x : {flat_mono_detail::kToneVs, flat_mono_detail::kToneVsNoConst, flat_mono_detail::kToneVsCbZ, flat_mono_detail::kTonePs,
                       flat_mono_detail::kToneDofCompositePs, flat_mono_detail::kToneBloomCompositePs, flat_mono_detail::kToneEdhmGradePs,
                       flat_mono_detail::kCopyVs, flat_mono_detail::kCopyPs, kFlatUiMapToneVs, kFlatUiMapTonePs, kFlatUiSourceToneVsSteady,
                       kFlatUiSourceToneVsTransition, kFlatUiSourceTonePs, kFlatUiSourceTonePsEdhm, kFlatUiMapCanvasVs, kFlatUiMapCanvasPs,
                       kFlatUiMapSpriteVs, kFlatUiMapSpritePsA, kFlatUiMapSpritePsB, kUiVsPanel, kUiVsLoader, kUiVsHolo, kUiVsFlightHud,
                       kUiVsSprite, kUiVsGuiVector, kUiVsGuiText})
        h.push_back(x);
    for (uint64_t x : kUiPanelPs) h.push_back(x);
    for (uint64_t x : kUiLoaderPs) h.push_back(x);
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 120; ++i) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; h.push_back(rng); }

    // ---- the corpus reaches every fact, and the profile changes an answer (else the checks below are vacuous) ----------------
    unsigned supported = 0, familyVs = 0, tone = 0, mapTone = 0, sourceTone = 0, copyPair = 0, uiAsk = 0, profileDiffers = 0, unsupportedFamilyVs = 0;
    for (uint64_t vs : h)
        for (uint64_t ps : h) {
            const FlatPairFacts a = flatPairFactsFresh(vs, ps, true), b = flatPairFactsFresh(vs, ps, false);
            supported += a.supported; familyVs += a.familyVs; tone += a.toneSlot != ~0u; mapTone += a.mapToneSlot != ~0u;
            sourceTone += a.sourceTone; copyPair += a.copyPair; uiAsk += a.uiMayAsk;
            unsupportedFamilyVs += a.familyVs && !a.supported;
            profileDiffers += !(a == b);
        }
    expect(supported > 0 && familyVs > 0 && unsupportedFamilyVs > 0 && tone > 0 && mapTone > 0 && sourceTone > 0 && copyPair > 0 && uiAsk > 0,
           "the corpus reaches every fact (a keyed pair, a family VS with another PS, a tone pass, the map tonemap, a source tone, the copy, a UI family)");
    expect(profileDiffers > 0, "the profile changes an answer for some pair (a flat-only or VR-only family row), so a memo blind to it can be caught");

    // ---- equivalence and memoising ------------------------------------------------------------------------------------------
    auto compare = [&](auto& memo, unsigned* mismatches, uint64_t* queries) {
        *mismatches = 0;
        *queries = 0;
        for (int pass = 0; pass < 2; ++pass)   // the second pass is the hit path
            for (uint64_t vs : h)
                for (uint64_t ps : h)
                    for (bool flat : {true, false}) {   // both profiles of a pair back to back, so a memo blind to the profile is asked before it evicts
                        ++*queries;
                        if (!(memo.get(vs, ps, flat) == flatPairFactsFresh(vs, ps, flat))) ++*mismatches;
                    }
    };
    {
        FlatPairFactsMemoT<1024, 0>* memo = new FlatPairFactsMemoT<1024, 0>();
        unsigned bad = 0;
        uint64_t queries = 0;
        compare(*memo, &bad, &queries);
        expect(bad == 0, "the memo's answer is the fresh computation's over the whole corpus, both profiles, both passes");
        // Memoising: a stream of few distinct pairs misses once each.
        FlatPairFactsMemoT<1024, 0>* once = new FlatPairFactsMemoT<1024, 0>();
        for (int i = 0; i < 1000; ++i) once->get(flat_mono_detail::kToneVs, flat_mono_detail::kTonePs, true);
        expect(once->misses() == 1, "a pair asked a thousand times is computed once");
        delete once;
        delete memo;
    }
    {
        FlatPairFactsMemoT<4, 0>* tiny = new FlatPairFactsMemoT<4, 0>();   // four slots: nearly every ask collides and evicts
        unsigned bad = 0;
        uint64_t queries = 0;
        compare(*tiny, &bad, &queries);
        expect(bad == 0 && tiny->misses() > 1000, "through four slots (every ask a collision or an eviction) the answers are still the fresh computation's");
        delete tiny;
    }

    // ---- the key is complete: a memo missing a part is seen to fail -------------------------------------------------------
    {
        unsigned bad = 0;
        uint64_t queries = 0;
        FlatPairFactsMemoT<1024, 1>* noVs = new FlatPairFactsMemoT<1024, 1>();
        compare(*noVs, &bad, &queries);
        expect(bad > 0, "a memo that ignores the vertex shader disagrees with the fresh computation: the vertex shader is in the key");
        delete noVs;
        FlatPairFactsMemoT<1024, 2>* noPs = new FlatPairFactsMemoT<1024, 2>();
        compare(*noPs, &bad, &queries);
        expect(bad > 0, "a memo that ignores the pixel shader disagrees: the pixel shader is in the key");
        delete noPs;
        FlatPairFactsMemoT<1024, 4>* noProfile = new FlatPairFactsMemoT<1024, 4>();
        compare(*noProfile, &bad, &queries);
        expect(bad > 0, "a memo that ignores the profile disagrees: the profile is in the key");
        delete noProfile;
    }

    // ---- the sources the equivalence rests on ----------------------------------------------------------------------------------
    auto slurp = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const std::string engine = slurp("src/d3d11/engine_velocity.cpp");
    const std::string runtime = slurp("src/d3d11/flat_runtime.cpp");
    const std::string layer = slurp("src/d3d11/flat_ui_layer.cpp");
    expect(!engine.empty() && !runtime.empty() && !layer.empty(), "the sources are readable from the repo root");
    expect(engine.find("bool engineVelocityPoolFamilyPair(uint64_t vsHash, uint64_t psHash) noexcept {\n"
                       "    return keyedPs(familyForProfile(vsHash, runtimeFlatProfile()), psHash, runtimeFlatProfile());\n}") != std::string::npos &&
               engine.find("bool engineVelocityPoolFamilyVs(uint64_t vsHash) noexcept {\n"
                           "    return familyForProfile(vsHash, runtimeFlatProfile()) >= 0;\n}") != std::string::npos,
           "the engine's pair questions are the two inline family calls the fresh computation makes");
    {
        // flatUiLayerDecide: nothing before the family test counts or logs.
        const size_t from = layer.find("FlatUiLayerAsk flatUiLayerDecide(ID3D11DeviceContext* ctx, const FlatUiLayerDraw& d) {");
        const size_t gate = layer.find("if (!flatUiLayerTakesFamily(family)) return g_lastAsk;", from);
        const std::string head = from == std::string::npos || gate == std::string::npos ? "" : layer.substr(from, gate - from);
        expect(!head.empty() && head.find("++g_w") == std::string::npos && head.find("Log::") == std::string::npos &&
                   head.find("flatUiFamilyFor(d.vs, d.ps, d.mapOpen, d.hdrTarget)") != std::string::npos,
               "flatUiLayerDecide returns at its family test before any counter or log: skipping it for a pair no family names changes nothing");
    }
    {
        auto count = [](const std::string& text, const std::string& needle) {
            unsigned n = 0;
            for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
            return n;
        };
        const size_t ctor = runtime.find("FlatRuntimeDrawScope::FlatRuntimeDrawScope(");
        const size_t end = runtime.find("FlatRuntimeDrawScope::~FlatRuntimeDrawScope()");
        const std::string body = ctor == std::string::npos || end == std::string::npos ? "" : runtime.substr(ctor, end - ctor);
        expect(count(body, "s.pairFacts.get(k.vs, k.ps, runtimeFlatProfile())") == 1, "the constructor asks the memo once, with (vs, ps, profile)");
        expect(body.find("engineVelocityPoolFamilyPair(k.vs") == std::string::npos && body.find("engineVelocityPoolFamilyVs(k.vs") == std::string::npos &&
                   body.find("flatUiMapToneSlotOf(k.vs, k.ps)") == std::string::npos && body.find("flatUiSourceTonePair(k.vs, k.ps)") == std::string::npos,
               "and no draw-time site in the constructor asks the replaced questions directly any more");
        expect(count(body, "if (pairFact.uiMayAsk) {") == 1, "the UI layer's inputs are gathered and its decision asked only for a pair a family can name");
    }
    return failures;
}

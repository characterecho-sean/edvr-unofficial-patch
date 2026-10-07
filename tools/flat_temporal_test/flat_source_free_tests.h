#pragma once
// THE POOL-LESS VIEW (design section 104): a scene that draws no pool-family draw at all is selected with no motion source and treated.
//
// The selector named the scene's depth, camera and motion slots from a draw of a pool family the motion producer substitutes. A view of
// ground and sky holds none, and was refused with no-supported-motion-source-pair on the copy route and with
// conflicting-hdr-target-or-camera on the HDR route (where the weapon's glow pass, a second camera in the HDR, is admitted as an overlay only
// against a named world). Both are one fault: nothing named the world. A frame that holds no pool-family draw has nothing the producer could
// have seen move, so it is selected with its HDR's own camera, every pixel takes the camera term, and the world is named from the selection.
// A pool-family vertex shader left stock (an unkeyed pixel shader) moves with no source: a frame holding one is still refused.
//
// The pieces that run on the GPU (the engine's views from nothing, the prep with an empty slot target) are in tools\weapon_motion_test and
// tools\flat_mono_resolve_test. These are the selector's and the model's cases, the trace's flag, the 5 s counters, and the pins on the
// runtime's wiring, each with its mutation control.
inline int flatSourceFreeTests() {
    using namespace edvr;
    using namespace hdr_route_test;
    int failures = 0;
    const auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: source-free %s\n", name); ++failures; }
    };

    // -- the HDR route's selector --
    {
        Stream view; view.sceneDraws(0, 4); view.toneTrigger();
        const FlatMonoFrame a = view.select();
        expect(a.selected() && a.sourceFree && a.supportedDraws == 0 && a.sourceFirst == 0 && a.sourceLast == 0 &&
                   a.hdr == view.sc.h && a.depth == view.sc.hDepth && a.sceneConstants == view.sc.b1 &&
                   std::memcmp(a.camera, kRows, sizeof(a.camera)) == 0,
               "the HDR route selects a scene with no pool-family draw, naming H's own depth, constants and camera");
        expect(a.sourceless.records > 0 && a.sourceless.poolRecords == 0,
               "and keeps what the scene held on that depth, none of it a pool record");
        Stream sourced; sourced.sceneDraws(2, 2); sourced.toneTrigger();
        const FlatMonoFrame b = sourced.select();
        expect(b.selected() && !b.sourceFree && b.supportedDraws == 2 && b.sourceFirst != 0,
               "a scene with a supported source is selected as before, with its source, and is not source-free");
        Stream stock; stock.sceneDraws(0, 4); stock.stockFamilyDraw(); stock.toneTrigger();
        const FlatMonoFrame c = stock.select();
        expect(!c.selected() && c.reason == FlatMonoReason::NoSupportedSource && !c.sourceFree && stock.prefix->unsupportedFamilyDraws == 1,
               "a pool-family vertex shader left stock into the scene's depth keeps the frame refused: it moves and no source sees it");
        Stream both; both.sceneDraws(2, 2); both.stockFamilyDraw(); both.toneTrigger();
        const FlatMonoFrame d = both.select();
        expect(d.selected() && !d.sourceFree && d.supportedDraws == 2,
               "a stock family draw beside a supported source changes nothing: the frame has a source and is selected as it was");
        Stream late; late.sceneDraws(0, 4); late.toneTrigger(); late.stockFamilyDraw();
        expect(late.prefix->unsupportedFamilyDraws == 1,
               "the count is the prefix's, whenever the draw comes");
    }

    // -- the model's count: before the colour test, so a depth-only pre-pass counts --
    {
        Stream depthOnly; depthOnly.sceneDraws(0, 2);
        FlatRuntimeDraw d = depthOnly.make(nullptr, depthOnly.sc.hDepth, depthOnly.sc.hW, depthOnly.sc.hH, 26, 0xA1, 0xB9, true, false);
        d.poolFamilyVs = true;
        depthOnly.draw(d);
        depthOnly.draw(d);
        expect(depthOnly.prefix->unsupportedFamilyDraws == 2,
               "a pool-family draw with no colour target (a depth pre-pass) is counted, one per draw");
        Stream plain; plain.sceneDraws(2, 2);
        expect(plain.prefix->unsupportedFamilyDraws == 0, "a frame whose draws carry no flag counts none (every recorded trace of an older build)");
    }

    // -- the trace carries the flag, and an older trace has none --
    {
        Stream s;
        FlatRuntimeDraw d = s.make(s.sc.h, s.sc.hDepth, s.sc.hW, s.sc.hH, 26, 0xA1, 0xB9, true, false);
        d.poolFamilyVs = true;
        const FlatTraceEvent e = flatTraceEventFromDraw(d, false);
        const FlatRuntimeDraw back = flatTraceEventToDraw(e);
        expect((e.flags & kFlatTracePoolFamilyVs) && back.poolFamilyVs, "the stock-family flag survives the trace");
        const uint32_t others = kFlatTraceFirstPersonCohort | kFlatTraceAlternateHdr;
        expect((kFlatTracePoolFamilyVs & others) == 0 && (kFlatTracePoolFamilyVs & (kFlatTracePoolFamilyVs - 1)) == 0,
               "the flag is a bit of its own beside the cohort's and the glow pass's");
        FlatRuntimeDraw plain = s.make(s.sc.h, s.sc.hDepth, s.sc.hW, s.sc.hH, 26, 0xA1, 0xB1, true, true);
        const FlatTraceEvent ep = flatTraceEventFromDraw(plain, false);
        expect(!(ep.flags & kFlatTracePoolFamilyVs) && !flatTraceEventToDraw(ep).poolFamilyVs, "a draw without the flag records none");
        FlatTraceEvent older = e; older.flags &= ~kFlatTracePoolFamilyVs;
        expect(!flatTraceEventToDraw(older).poolFamilyVs, "an event recorded before the flag existed replays without it");
    }

    // -- the 5 s counters --
    {
        FlatSourceSpell spell;
        spell.sourceFreeFrame(true); spell.sourceFreeFrame(true); spell.sourceFreeFrame(false);
        spell.overlayUnnamed(); spell.overlayUnnamed();
        const FlatSourceSpellWindow w = spell.take();
        expect(w.sourceFreeFrames == 3 && w.sourceFreeTreated == 2 && w.overlaysUnnamed == 2,
               "source-free frames, those the resolver ran on, and overlays admitted without a named world are counted");
        const FlatSourceSpellWindow again = spell.take();
        expect(again.sourceFreeFrames == 0 && again.sourceFreeTreated == 0 && again.overlaysUnnamed == 0, "and the window is taken");
        char line[3072]{};
        const FlatMonoSourceless none{};
        const FlatSourcelessPairNote notes[4]{};
        flatSourceSpellLine(line, sizeof(line), w, none, 0, notes);
        expect(std::strstr(line, "source-free-frames=3 source-free-treated=2 overlays-without-named-world=2;") != nullptr,
               "the 5 s line prints them");
        // The runtime prints it into 3072 bytes with the four pairs of a source-less frame and every counter at its widest.
        FlatSourceSpellWindow widest;
        widest.frames = widest.noSourceFrames = widest.spells = widest.recoveries = widest.abandoned = widest.warmDone = widest.warmFrames = ~0ull;
        widest.warmMax = widest.warmAborted = widest.longestSpell = widest.openSpell = widest.sourceFreeFrames = widest.sourceFreeTreated = ~0ull;
        widest.overlaysUnnamed = ~0ull;
        FlatMonoSourceless busy;
        busy.records = busy.draws = busy.sameCameraDraws = busy.poolRecords = busy.distinctPairs = ~0u;
        busy.topCount = 4;
        for (auto& pair : busy.top) { pair.vs = pair.ps = ~0ull; pair.draws = pair.records = ~0u; pair.sameCamera = pair.pool = true; }
        FlatSourcelessPairNote loud[4];
        for (auto& note : loud) note.familyVs = note.recipe = true;
        char wide[3072]{};
        const int widest_n = flatSourceSpellLine(wide, sizeof(wide), widest, busy, ~0ull, loud);
        expect(widest_n > 0 && widest_n < int(sizeof(wide)), "and the line at its widest still fits the runtime's buffer, untruncated");
    }
    return failures;
}

inline int flatSourceFreeWiringTests() {
    int failures = 0;
    const auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: source-free wiring %s\n", name); ++failures; }
    };
    const auto slurp = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const auto compact = [](const std::string& in) {
        std::string out; out.reserve(in.size());
        for (char c : in) if (c != ' ' && c != '\r' && c != '\n' && c != '\t') out += c;
        return out;
    };
    const auto body = [](const std::string& text, const char* signature) {
        const size_t at = text.find(signature);
        if (at == std::string::npos) return std::string();
        const size_t end = text.find("\n}\n", at);
        return text.substr(at, end == std::string::npos ? std::string::npos : end + 3 - at);
    };
    const auto count = [](const std::string& text, const std::string& needle) {
        size_t n = 0, at = 0;
        while ((at = text.find(needle, at)) != std::string::npos) { ++n; at += needle.size(); }
        return n;
    };
    const auto ordered = [&](const std::string& compacted, std::initializer_list<const char*> needles) {
        size_t pos = 0;
        bool ok = !compacted.empty();
        for (const char* needle : needles) {
            const size_t at = compacted.find(compact(needle), pos);
            if (at == std::string::npos) { ok = false; break; }
            pos = at + compact(needle).size();
        }
        return ok;
    };
    const auto without = [&](std::string text, const char* needle) {
        const std::string n = compact(needle);
        const size_t at = text.find(n);
        if (at != std::string::npos) text.erase(at, n.size());
        return text;
    };
    const auto replaced = [&](std::string text, const char* from, const char* to) {
        const std::string f = compact(from);
        const size_t at = text.find(f);
        if (at != std::string::npos) text.replace(at, f.size(), compact(to));
        return text;
    };

    const std::string runtimeSource = slurp("src/d3d11/flat_runtime.cpp");
    const std::string modelSource = slurp("src/d3d11/flat_runtime_model.h");
    const std::string monoSource = slurp("src/d3d11/flat_mono_frame.h");
    const std::string hdrSource = slurp("src/d3d11/flat_hdr_route.h");
    const std::string engineSource = slurp("src/d3d11/engine_velocity.cpp");
    expect(!runtimeSource.empty() && !modelSource.empty() && !monoSource.empty() && !hdrSource.empty() && !engineSource.empty(),
           "the runtime, model, selectors and engine sources are readable from the repo root");
    const std::string runtime = compact(runtimeSource), model = compact(modelSource), mono = compact(monoSource),
                      hdr = compact(hdrSource), engine = compact(engineSource);

    // -- the naming from the selection --
    const std::string naming = compact(body(runtimeSource, "static bool nameSourceFree("));
    const auto namingValid = [&](const std::string& text) {
        return ordered(text, {"if(!sel.selected()||!sel.sourceFree)returnfalse;",
                              "if(s.namedDepth)returns.namedDepth==sel.depth&&s.namedConstants==sel.sceneConstants;",
                              "s.namedDepth=sel.depth;s.namedConstants=sel.sceneConstants;",
                              "std::memcpy(s.namedCamera,sel.camera,sizeof(sel.camera));",
                              "if(reference.valid())s.worldReference=reference;", "s.frameSourceFree=true;", "returntrue;"});
    };
    expect(namingValid(naming), "a source-free selection names the world from itself, once, and compares a world a draw already named");
    expect(!namingValid(without(naming, "if(!sel.selected()||!sel.sourceFree)returnfalse;")),
           "mutation control: a naming that takes any selection for source-free fails the wiring");
    expect(!namingValid(without(naming, "if(s.namedDepth)returns.namedDepth==sel.depth&&s.namedConstants==sel.sceneConstants;")),
           "mutation control: a naming that overwrites the world a draw named fails the wiring");
    expect(!namingValid(without(naming, "std::memcpy(s.namedCamera,sel.camera,sizeof(sel.camera));")),
           "mutation control: a naming that leaves the camera of the last world in place fails the wiring");
    expect(!namingValid(without(naming, "s.frameSourceFree=true;")), "mutation control: a naming the frame's count never hears of fails the wiring");

    // -- both routes name before they compare, and prepare the views before they ask for them --
    const std::string copyRoute = runtime;
    expect(ordered(copyRoute, {"nameSourceFree(s,selected);",
                               "if(s.treated||selected.depth!=s.namedDepth||selected.sceneConstants!=s.namedConstants){",
                               "producer-source-identity-mismatch"}),
           "the copy route names a source-free world before it compares the selection with the naming");
    expect(!ordered(replaced(copyRoute, "nameSourceFree(s,selected);if(s.treated||selected.depth!=s.namedDepth",
                             "if(s.treated||selected.depth!=s.namedDepth"),
                    {"nameSourceFree(s,selected);", "if(s.treated||selected.depth!=s.namedDepth||selected.sceneConstants!=s.namedConstants){"}),
           "mutation control: a comparison made before the naming fails the wiring");
    expect(ordered(runtime, {"nameSourceFree(s,selected);",
                             "if(selected.depth!=s.namedDepth||selected.sceneConstants!=s.namedConstants){decline(\"producer-source-identity-mismatch\");"}),
           "the HDR route's treatment names a source-free world before it compares too");
    expect(count(runtime, "nameSourceFree(s,sel);") == 1 && count(runtime, "nameSourceFree(s,selected);") == 2,
           "the world is named at the trigger's selection, at the copy route's resolve and at the HDR route's treatment, and nowhere else");
    const std::string prepare =
        "if(selected.sourceFree){floatsourceFreeRows[6][4];std::memcpy(sourceFreeRows,selected.camera,sizeof(sourceFreeRows));"
        "engineVelocityPrepareSourceFree(ctx,static_cast<ID3D11Texture2D*>(const_cast<void*>(selected.depth)),"
        "static_cast<ID3D11Buffer*>(const_cast<void*>(selected.sceneConstants)),sourceFreeRows);}"
        "if(!engineVelocitySourceViews(";
    expect(count(runtime, prepare) == 2 && count(runtime, "engineVelocitySourceViews(") == 2,
           "both routes prepare the views of a source-free selection from its camera, immediately before they ask for them");
    expect(count(without(runtime, prepare.c_str()), prepare) == 1, "mutation control: a route that asks without preparing is one prepare short");

    // -- the overlay is admitted against the HDR's own camera when no draw has named the world --
    const std::string unnamed =
        "constboolunnamedWorldDepth=!s.namedDepth&&s.prefix.sourcesUsed==0&&s.prefix.firstPersonDraws==0&&"
        "s.prefix.unsupportedFamilyDraws==0;constFlatRuntimeTarget*overlayTarget=nullptr;";
    const auto overlayValid = [&](const std::string& text) {
        return ordered(text, {unnamed.c_str(), "(k.depth==s.namedDepth||unnamedWorldDepth)&&flat_mono_detail::hdrViewport(k,k.width,k.height)",
                              "if(!s.namedDepth)s.sourceSpell.overlayUnnamed();", "if(copyWeapon()){"});
    };
    expect(overlayValid(runtime), "the weapon's glow pass is judged against the HDR's own camera when nothing has named the world, and counted");
    expect(!overlayValid(replaced(runtime, "(k.depth==s.namedDepth||unnamedWorldDepth)&&flat_mono_detail::hdrViewport",
                                  "k.depth==s.namedDepth&&flat_mono_detail::hdrViewport")),
           "mutation control: an overlay still bound to a named world fails the wiring");
    expect(!overlayValid(without(runtime, "&&s.prefix.unsupportedFamilyDraws==0;constFlatRuntimeTarget*overlayTarget=nullptr;")) &&
               !overlayValid(replaced(runtime, "&&s.prefix.unsupportedFamilyDraws==0;constFlatRuntimeTarget*overlayTarget=nullptr;",
                                      ";constFlatRuntimeTarget*overlayTarget=nullptr;")),
           "mutation control: an unnamed world that ignores a stock family draw, or a source, fails the wiring");
    expect(!overlayValid(without(runtime, "if(!s.namedDepth)s.sourceSpell.overlayUnnamed();")),
           "mutation control: an admission the line never hears of fails the wiring");

    // -- the draw scope flags the stock family draw, and the model counts it before the colour test --
    const std::string flagged =
        "d.poolFamilyVs=!d.supported&&k.depth&&engineVelocityPoolFamilyVs(k.vs)&&"
        "flatContractKind(false,k.depth,k.depth,k.depthWidth,k.depthHeight,26,s.prefix.width,s.prefix.height,false)==kFlatContractScreen;";
    expect(count(runtime, flagged) == 1, "the draw scope flags a pool-family vertex shader left stock into a scene-sized depth");
    expect(count(replaced(runtime, "d.poolFamilyVs=!d.supported&&k.depth", "d.poolFamilyVs=k.depth"), flagged) == 0,
           "mutation control: a flag that also takes the substituted draws fails the wiring");
    expect(ordered(model, {"if(d.poolFamilyVs)++p.unsupportedFamilyDraws;", "if(!k.color)returnout;"}),
           "the model counts the stock family draw before it returns for a draw with no colour target");
    expect(!ordered(replaced(model, "if(d.poolFamilyVs)++p.unsupportedFamilyDraws;if(!k.color)returnout;", "if(!k.color)returnout;if(d.poolFamilyVs)++p.unsupportedFamilyDraws;"),
                    {"if(d.poolFamilyVs)++p.unsupportedFamilyDraws;", "if(!k.color)returnout;"}),
           "mutation control: a count that a depth pre-pass never reaches fails the wiring");
    expect(count(model, "in.unsupportedFamilyDraws=p.unsupportedFamilyDraws;") == 1 && count(hdr, "in.unsupportedFamilyDraws=p.unsupportedFamilyDraws;") == 1,
           "both selectors are handed the prefix's count");

    // -- both selectors take the scene source-free only with no stock family draw --
    const std::string refuseOrFree =
        "if(in.unsupportedFamilyDraws||out.unsupportedDraws)returnrefuse(FlatMonoReason::NoSupportedSource);out.sourceFree=true;";
    expect(count(mono, refuseOrFree) == 1 && count(hdr, refuseOrFree) == 1,
           "both selectors refuse a scene with no source and a stock family draw, and take any other as source-free");
    expect(count(without(mono, "if(in.unsupportedFamilyDraws||out.unsupportedDraws)returnrefuse(FlatMonoReason::NoSupportedSource);"), refuseOrFree) == 0,
           "mutation control: a selector that takes every sourceless scene fails the wiring");

    // -- the frame's end counts it, and the next frame forgets it --
    expect(ordered(runtime, {"if(s.frameSourceFree)s.sourceSpell.sourceFreeFrame(s.treated&&SUCCEEDED(hr));", "if(!s.treated||FAILED(hr))reset();"}),
           "the Present counts a source-free frame, and whether it was treated, before the history is reset for a frame that was not");
    expect(!ordered(without(runtime, "if(s.frameSourceFree)s.sourceSpell.sourceFreeFrame(s.treated&&SUCCEEDED(hr));"),
                    {"if(s.frameSourceFree)s.sourceSpell.sourceFreeFrame(s.treated&&SUCCEEDED(hr));", "if(!s.treated||FAILED(hr))reset();"}),
           "mutation control: a frame nobody counts fails the wiring");
    expect(count(runtime, "s.treated=false;s.frameSourceFree=false;") == 1, "the flag is cleared with the frame's other naming state");

    // -- the engine's views from nothing --
    const std::string views = compact(body(engineSource, "bool engineVelocityPrepareSourceFree("));
    const auto viewsValid = [&](const std::string& text) {
        return ordered(text, {"if(e.frame==frame&&e.written)returne.depth.Get()==sceneDepth&&!e.invalid;",
                              "ensureSlots(ctx,e,kEngineVelocitySourceEye,sceneDepth)",
                              "e.sceneFrame[slot]=frame;", "e.sceneRowsKnown=true;", "e.written=true;",
                              "g_sourceDepth=sceneDepth;", "g_sourceNoted=frame;"});
    };
    expect(viewsValid(views), "the views from nothing stand down for a pool draw's, make the slot target, the constants and the pool, then name the source");
    expect(!viewsValid(without(views, "if(e.frame==frame&&e.written)returne.depth.Get()==sceneDepth&&!e.invalid;")),
           "mutation control: views that overwrite a pool draw's fail the wiring");
    expect(!viewsValid(without(views, "e.sceneFrame[slot]=frame;")),
           "mutation control: constants no frame stamp vouches for fail the wiring");
    expect(!viewsValid(without(views, "g_sourceNoted=frame;")), "mutation control: a source that is never kept alive fails the wiring");
    return failures;
}

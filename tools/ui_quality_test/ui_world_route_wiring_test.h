// Included after ui_world_route_test.h (check, the pure pieces) and ui_after_ui_test.h (squeeze, functionBody).
//
// THE VR WORLD ROUTE'S WIRING, by source scan (the rig runs from the repo root; docs/design-flat-temporal-aa-2026-09-23.md,
// section 82). The pure half decides what the layer does for the route; these scans keep the places that call it, and
// the places that must NOT, where the design put them -- the same way ui_after_ui_test.h scans uiLayerNoteOther:
//   * ui_layer.cpp: the gatherer asks the route for the 2D screen family alone; in the route's mode the decision is never a
//     take; Begin re-checks the bindings before it binds the layer and the mipped screen under VrWorldInternalScope; End
//     puts everything back and only then tells the route which eye it took (and nothing else does);
//   * vscreen.cpp: the pending re-issue is scoped to the draw, issued after the game's own draw and before the verdict's
//     undo, and the tail of the game's draw skips the per-eye screen-motion reissues for a draw the route re-issues while
//     the recognition (the naming of the world's source) still runs; for a CURVED screen the curve substitution skips its
//     own motion pass on such a draw and a swallowed draw goes to curvedScreenSwallowed, which re-issues the strip
//     (worldScreenReissueCurved: ready asked before Begin, End told what the draw did) while the flat re-issue stays exactly
//     what it was; the curved route's pins (layerCurvedPins, vscreenCurvedPins) are functions of the source text and
//     testControls runs them on copies with one edit each that must trip the pin named;
//   * native_temporal.cpp: the eye shift is advertised only while the route does not own the next frame; the layer-only
//     branch sits after the output sizing, answers S_OK and never null, and leaves the eye's history and continuity alone;
//   * native_sharpen.cpp: a layer-only eye is composited first and sharpened after; every other eye keeps today's order.
namespace worldroute {
// ------------------------------------------------------------ 6: the wiring, by source scan

std::string readText(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // Source files in this worktree may be CRLF while mutation anchors use
    // explicit LF. Normalize once so the controls test source semantics rather
    // than the checkout's line-ending convention.
    for (size_t at = 0; (at = text.find("\r\n", at)) != std::string::npos; ++at)
        text.replace(at, 2, "\n");
    return text;
}

// The squeezed text of one top-level function, or "" (after a failed check).
std::string bodyOf(const std::string& text, const char* signature, const char* what) {
    std::string body;
    char msg[240];
    std::snprintf(msg, sizeof(msg), "%s is found", what);
    if (!afterui::functionBody(text, signature, &body)) {
        check(false, msg);
        return std::string();
    }
    check(true, msg);
    return afterui::squeeze(body);
}

// True when every piece is present in order (each after the one before).
bool inOrder(const std::string& s, std::initializer_list<const char*> pieces, std::string* missing = nullptr) {
    size_t prev = 0;
    for (const char* piece : pieces) {
        const size_t at = s.find(piece, prev);
        if (at == std::string::npos) {
            if (missing) *missing = piece;
            return false;
        }
        prev = at;
    }
    return true;
}

bool has(const std::string& s, const char* piece) { return s.find(piece) != std::string::npos; }
size_t countOf(const std::string& s, const char* piece) {
    size_t n = 0;
    for (size_t at = s.find(piece); at != std::string::npos; at = s.find(piece, at + 1)) ++n;
    return n;
}

void reportOrder(bool ok, const std::string& missing, const char* what) {
    char msg[360];
    std::snprintf(msg, sizeof(msg), "%s%s%s", what, ok ? "" : " -- missing or out of order: ", ok ? "" : missing.c_str());
    check(ok, msg);
}

// ---- THE CURVED ROUTE'S PINS, HELD BY CONTROLS --------------------------------------------------------------------------------------
// A curved screen (fix.panel_curvature above 0) is no longer refused by the plan: the game's draw is the curve substitution's strip, the plan
// accepts it like a flat draw, the substitution skips its own motion pass on a frame the route owns, and the swallowed draw is re-issued as the
// same strip (vscreen.cpp curvedScreenSwallowed, worldScreenReissueCurved; panel_curve.h panelCurveReissue). Each pin below is a function of
// a source text, so the controls at the end of this section run the very same function on copies with one edit each that puts the old
// behaviour (or a likely slip) back, and require the pin they name to fail. A pin that cannot fail proves nothing.
struct WirePin { const char* id; bool ok; const char* what; };

// The squeezed text of one top-level function; "" when it is not found (the pin that reads it then fails).
std::string quietBody(const std::string& text, const char* signature) {
    std::string body;
    return afterui::functionBody(text, signature, &body) ? afterui::squeeze(body) : std::string();
}

std::vector<WirePin> layerCurvedPins(const std::string& text) {
    std::vector<WirePin> pins;
    const std::string decide = quietBody(text, "bool uiLayerDecide(ID3D11DeviceContext* ctx, int familyInt");
    const std::string plan = quietBody(text, "void worldReissuePlan(ID3D11DeviceContext* ctx, const UiLayerDrawFacts& f,");
    const std::string end = quietBody(text, "void uiLayerWorldReissueEnd(ID3D11DeviceContext* ctx, bool landed)");
    // The gatherer: in the route's mode the plan is made, never a take, and the plan is not told whether the draw was substituted.
    pins.push_back({"decide-plans",
                    inOrder(decide, {"uiLayerWorldRouteMode(family,f.worldScreen,f.worldRoute)", "uiLayerWorldCount(d,true)==UiWorldCount::kReissue",
                                     "worldReissuePlan(ctx,f,seq,jx,jy);", "returnfalse;}", "++g_win.decided[static_cast<size_t>(family)]"}) &&
                        !has(decide, "worldReissuePlan(ctx,f,substituted"),
                    "in the route's mode uiLayerDecide plans the re-issue (the plan is not told the draw was substituted: a curved screen is planned like a flat one), "
                    "returns false (never a take), and counts decisions only after"});
    // The plan: no refusal for a substituted draw; the depth and blend refusals before anything is held; the sources under the internal scope and the guard.
    pins.push_back({"plan-first-refusal",
                    plan.rfind("voidworldReissuePlan(ID3D11DeviceContext*ctx,constUiLayerDrawFacts&f,uint64_tseq,floatjx,floatjy){UiWorldRefusewhy=UiWorldRefuse::kNone;"
                               "WorldReissueplan;if(f.ds.tests()||f.ds.writes()){why=UiWorldRefuse::kDepthState;}elseif(f.blend!=UiBlendShape::kOpaque){",
                               0) == 0 &&
                        !has(plan, "substituted") &&
                        inOrder(plan, {"f.ds.tests()||f.ds.writes()", "f.blend!=UiBlendShape::kOpaque", "VrWorldInternalScopeinternal;", "guardedBudget(g_worldBudget,",
                                       "worldReissueSources(ctx,seq,&plan)", "plan.pending=true;g_reissue=plan;detail::g_uiLayerWorldReissue=true;"}),
                    "the plan takes no `substituted` and refuses a depth-tested or blended screen draw first (a curved screen is no refusal), then asks the mips module "
                    "under the internal scope and the guard, and only then holds the draw"});
    // End: everything back, the layer's own End, the fault path, then the landed test, and only then the eye counted and handed to the route.
    pins.push_back({"end-landed",
                    inOrder(end, {"VrWorldInternalScopeinternal;", "worldRestoreSources(ctx,plan);", "uiLayerEnd(ctx);", "worldReleaseSaved(plan);",
                                  "worldRefuse(plan.eye,uiWorldReasonId(UiWorldRefuse::kFault));return;}",
                                  "return;}if(!landed){worldRefuse(plan.eye,uiWorldReasonId(UiWorldRefuse::kFault));return;}++g_win.worldReissued;",
                                  "g_eye[plan.eye].worldSeq=plan.seq;", "vrWorldRouteNoteEyeTaken(static_cast<uint32_t>(plan.eye),plan.seq);"}),
                    "End(ctx, landed): the game's texture and sampler back, the layer's own End, the fault path, then a draw that did not land is a counted fault "
                    "that does not take the eye, and only then the eye counted and handed to the route"});
    return pins;
}

std::vector<WirePin> vscreenCurvedPins(const std::string& text) {
    std::vector<WirePin> pins;
    const std::string all = afterui::squeeze(text);
    const std::string fwd = quietBody(text, "void forwardWithVerdict(TracePolicy& trace, ID3D11DeviceContext* self, DrawVerdict v,");
    const std::string curved = quietBody(text, "__declspec(noinline) void worldScreenReissueCurved(TracePolicy& trace,");
    const std::string swallowed = quietBody(text, "__declspec(noinline) void curvedScreenSwallowed(TracePolicy& trace,");
    const std::string flat = quietBody(text, "__declspec(noinline) void worldScreenReissue(TracePolicy& trace,");
    // forwardWithVerdict's curve branch: the substitution skips its own per-eye motion pass exactly when the route re-issues this draw, and a
    // swallowed draw goes to curvedScreenSwallowed with that same fact and returns, before the verdict's own Begin.
    pins.push_back({"curve-branch",
                    inOrder(fwd, {"if(forwardInputs.read(forwardInputs.fact.curveThisDrawCurveGate,[&]{returng_state->curveThisDraw;},[&]{returng_state->curveThisDraw;})){g_state->curveThisDraw=false;", "constboollayered=uiLayer&&uiLayerBegin(self);",
                                  "constboolswallowed=panelCurveSubstitute(self,g_state->realDrawIndexedInstanced,!worldReissue.on);",
                                  "if(layered)uiLayerEnd(self);if(swallowed){curvedScreenSwallowed(trace,self,v,count,instances,args,worldReissue.on);return;}",
                                  "if(v!=DrawVerdict::kNone){", "forwardVerdictBegin(self,v);"}) &&
                        countOf(all, "panelCurveSubstitute(") == 1 && countOf(all, "curvedScreenSwallowed(") == 2,
                    "forwardWithVerdict's curve branch: the substitution is told to skip its motion pass when the route re-issues the draw (!worldReissue.on), and a swallowed "
                    "draw goes to curvedScreenSwallowed with that fact and returns, after the layer's End and before the verdict's Begin"});
    // The curved re-issue: the internal scope, then ready BEFORE the layer's bracket opens, the census scope, the strip through the ORIGINAL draw pointer, End told what happened.
    pins.push_back({"reissue-curved",
                    inOrder(curved, {"if(uiLayerIssueBlocked()){", "return;", "}VrWorldInternalScopeinternal;",
                                      "if(!panelCurveReissueReady())", "return;", "}constboolbegan=uiLayerWorldReissueBegin(self);", "if(began){",
                                     "GpuCensusScopecensus(self,GpuCensusSection::FrameWorldLayer);",
                                     "constbooldrawn=panelCurveReissue(self,g_state->realDrawIndexedInstanced);", "uiLayerWorldReissueEnd(self,drawn);"}) &&
                        countOf(curved, "uiLayerWorldReissueBegin(") == 1 && countOf(curved, "uiLayerWorldReissueEnd(") == 1 &&
                        countOf(all, "panelCurveReissue(") == 1 && countOf(all, "panelCurveReissueReady(") == 1,
                    "worldScreenReissueCurved: the internal scope, panelCurveReissueReady asked BEFORE uiLayerWorldReissueBegin, the FrameWorldLayer census scope, the strip drawn "
                    "through the original draw pointer, and the draw's result handed to uiLayerWorldReissueEnd"});
    // What curvedScreenSwallowed does for a swallowed draw: the footprint behind its own flag; the recognition and the re-issue only for a frame the route owns.
    pins.push_back({"swallowed-gates",
                    inOrder(swallowed, {"if(self!=g_state->ownerCtx)return;",
                                        "if(g_state->rtv0Eye&&count&&instances&&vscreenFootprintWanted())footprintEyeDraw(self,"
                                        "v==DrawVerdict::kPanel?g_state->distanceScale:1.0f,args.base,args.startInstance);",
                                        "if(!routeOwns)return;", "if(screenMotionLive())screenMotionRecognize();", "worldScreenReissueCurved(trace,self,'X',count,instances,args);"}) &&
                        countOf(swallowed, "footprintEyeDraw(") == 1 && countOf(swallowed, "screenMotionRecognize(") == 1 && countOf(swallowed, "worldScreenReissueCurved(") == 1,
                    "curvedScreenSwallowed: the owner context only; the footprint instrument only when it is armed (vscreenFootprintWanted); the recognition and the curved "
                    "re-issue only when the route owns the frame (routeOwns), in that order, once each"});
    // The flat re-issue keeps its begin/issue/end behavior, and its call site still follows the game's own issue and the crisp re-issue.
    pins.push_back({"flat-unchanged",
                    inOrder(flat, {"if(uiLayerIssueBlocked()){", "return;", "}VrWorldInternalScopeinternal;", "constboolbegan=uiLayerWorldReissueBegin(self);",
                                    "if(began){", "GpuCensusScopecensus(self,GpuCensusSection::FrameWorldLayer);",
                                    "pureDrawReissue(self,kind,count,instances,args);", "uiLayerWorldReissueEnd(self);"}) &&
                        has(fwd, "if(originalIssued&&forwardInputs.read(forwardInputs.fact.crispPendingAfterOriginal,[]{returndetail::g_uiLayerCrispPending;},[]{returnuiLayerCrispPending();})){crispHudTonemapReissue(trace,self,kind,count,instances,args);}"
                                 "if(worldReissue.on&&originalIssued){worldScreenReissue(trace,self,kind,count,instances,args);}") &&
                        countOf(all, "worldScreenReissue(") == 2 && countOf(all, "uiLayerWorldReissueEnd(self);") == 1,
                    "the flat re-issue keeps its original bracketed draw and `if (worldReissue.on && originalIssued) worldScreenReissue(trace, "
                    "kind, count, instances, args)` still follows the crisp re-issue, once"});
    // The recognition: the flat tail's, a taken 2D screen's behind the maps gate, and the curved screen's (behind routeOwns, above).
    pins.push_back({"recognition-places", countOf(all, "screenMotionRecognize()") == 3,
                    "the recognition is called in exactly three places in vscreen.cpp: the flat tail's (a draw the route re-issues), a taken 2D screen's behind uiLayerMapsOn(), "
                    "and the curved screen's after the substitution (behind routeOwns)"});
    return pins;
}

// One control: an edit of one source that must be caught by the pin it names (`layer` true: ui_layer.cpp, else vscreen.cpp).
struct WireFlip { bool layer; const char* name; const char* from; const char* to; const char* caughtBy; };
std::string flipOnce(const std::string& text, const char* from, const char* to) {
    const std::string needle(from);
    const size_t at = text.find(needle);
    if (at == std::string::npos || text.find(needle, at + needle.size()) != std::string::npos) return std::string();
    std::string out = text;
    out.replace(at, needle.size(), to);
    return out;
}
void testCurvedControls(const std::string& layerText, const std::string& vscreenText) {
    const WireFlip flips[] = {
        {true, "the plan is told the draw was substituted again", "worldReissuePlan(ctx, f, seq, jx, jy);", "worldReissuePlan(ctx, f, substituted, seq, jx, jy);", "decide-plans"},
        {true, "the plan refuses a substituted draw again",
         "    if (f.ds.tests() || f.ds.writes()) {\n        why = UiWorldRefuse::kDepthState;",
         "    if (f.substituted) {\n        why = UiWorldRefuse::kFault;\n    } else if (f.ds.tests() || f.ds.writes()) {\n        why = UiWorldRefuse::kDepthState;", "plan-first-refusal"},
        {true, "End ignores whether the draw landed", "    if (!landed) {\n", "    if (false) {\n", "end-landed"},
        {true, "End counts the eye for a draw that did not land",
         "        worldRefuse(plan.eye, uiWorldReasonId(UiWorldRefuse::kFault));\n        return;\n    }\n    ++g_win.worldReissued;",
         "        ++g_win.worldReissued;\n        return;\n    }\n    ++g_win.worldReissued;", "end-landed"},
        {false, "the substitution always runs its motion pass", "panelCurveSubstitute(self, g_state->realDrawIndexedInstanced, !worldReissue.on);",
         "panelCurveSubstitute(self, g_state->realDrawIndexedInstanced);", "curve-branch"},
        {false, "the substitution skips its motion pass on the wrong frames", "panelCurveSubstitute(self, g_state->realDrawIndexedInstanced, !worldReissue.on);",
         "panelCurveSubstitute(self, g_state->realDrawIndexedInstanced, worldReissue.on);", "curve-branch"},
        {false, "a swallowed draw no longer returns", "curvedScreenSwallowed(trace, self, v, count, instances, args, worldReissue.on);\n            return;",
         "curvedScreenSwallowed(trace, self, v, count, instances, args, worldReissue.on);", "curve-branch"},
        {false, "curvedScreenSwallowed is told the route owns every frame", "curvedScreenSwallowed(trace, self, v, count, instances, args, worldReissue.on);",
         "curvedScreenSwallowed(trace, self, v, count, instances, args, true);", "curve-branch"},
        {false, "the layer's End is not run before the swallowed draw", "        if (layered) uiLayerEnd(self);\n        if (swallowed) {\n", "        if (swallowed) {\n", "curve-branch"},
        {false, "ready is bypassed before the layer's Begin", "if (!panelCurveReissueReady()) {", "if (true) {", "reissue-curved"},
        {false, "the blocked world route falls through without returning", "            draw_ladder::ActionOutcome::Declined, kind, count, instances, args,\n            0);\n        return;\n    }\n    VrWorldInternalScope internal;",
         "            draw_ladder::ActionOutcome::Declined, kind, count, instances, args,\n            0);\n    }\n    VrWorldInternalScope internal;", "reissue-curved"},
        {false, "the ready refusal returns only after Begin", "    if (!panelCurveReissueReady()) {\n        ladderTraceAction<TracePolicy, draw_ladder::ActionId::kWorldRouteDraw>(\n            trace, draw_ladder::ActionPhase::Begin,\n            draw_ladder::ActionOutcome::Declined, kind, count, instances, args,\n            0);\n        return;\n    }\n    const bool began = uiLayerWorldReissueBegin(self);",
         "    if (!panelCurveReissueReady()) {\n        ladderTraceAction<TracePolicy, draw_ladder::ActionId::kWorldRouteDraw>(\n            trace, draw_ladder::ActionPhase::Begin,\n            draw_ladder::ActionOutcome::Declined, kind, count, instances, args,\n            0);\n    }\n    const bool began = uiLayerWorldReissueBegin(self);\n    return;", "reissue-curved"},
        {false, "End is not told what the draw did", "uiLayerWorldReissueEnd(self, drawn);", "uiLayerWorldReissueEnd(self);", "reissue-curved"},
        {false, "End is told the draw landed whatever it did",
         "const bool drawn = panelCurveReissue(self, g_state->realDrawIndexedInstanced);\n        uiLayerWorldReissueEnd(self, drawn);",
         "panelCurveReissue(self, g_state->realDrawIndexedInstanced);\n        uiLayerWorldReissueEnd(self, true);", "reissue-curved"},
        {false, "the curved re-issue runs outside the internal scope", "    VrWorldInternalScope internal;\n    if (!panelCurveReissueReady()) {", "    if (!panelCurveReissueReady()) {", "reissue-curved"},
        {false, "the curved re-issue is not on the census",
         "        GpuCensusScope census(self, GpuCensusSection::FrameWorldLayer);\n        const bool drawn", "        const bool drawn", "reissue-curved"},
        {false, "the strip is drawn through another draw pointer", "panelCurveReissue(self, g_state->realDrawIndexedInstanced);", "panelCurveReissue(self, nullptr);", "reissue-curved"},
        {false, "the recognition runs before the route's gate", "    if (!routeOwns) return;\n    if (screenMotionLive()) screenMotionRecognize();",
         "    if (screenMotionLive()) screenMotionRecognize();\n    if (!routeOwns) return;", "swallowed-gates"},
        {false, "the route's gate is gone", "    if (!routeOwns) return;\n", "", "swallowed-gates"},
        {false, "the footprint is called whether or not it is armed",
         "    if (g_state->rtv0Eye && count && instances && vscreenFootprintWanted())\n        footprintEyeDraw(",
         "    if (g_state->rtv0Eye && count && instances)\n        footprintEyeDraw(", "swallowed-gates"},
        {false, "a deferred context is measured and re-issued too", "    if (self != g_state->ownerCtx) return;\n    if (g_state->rtv0Eye && count && instances",
         "    if (g_state->rtv0Eye && count && instances", "swallowed-gates"},
        {false, "the flat re-issue's End is told something", "uiLayerWorldReissueEnd(self);",
         "uiLayerWorldReissueEnd(self, true);", "flat-unchanged"},
        {false, "the flat call site loses its originalIssued term", "    if (worldReissue.on && originalIssued) {\n        worldScreenReissue(trace, self, kind, count, instances, args);",
         "    if (worldReissue.on) {\n        worldScreenReissue(trace, self, kind, count, instances, args);", "flat-unchanged"},
        {false, "the flat call site calls the curved re-issue", "        worldScreenReissue(trace, self, kind, count, instances, args);\n    }\n", "        worldScreenReissueCurved(trace, self, kind, count, instances, args);\n    }\n", "flat-unchanged"},
        {false, "a fourth place recognises", "if (!panelCurveReissueReady()) {", "if (!panelCurveReissueReady()) { if (screenMotionLive()) screenMotionRecognize();", "recognition-places"},
    };
    for (const WireFlip& f : flips) {
        char label[360];
        const std::string mutated = flipOnce(f.layer ? layerText : vscreenText, f.from, f.to);
        std::snprintf(label, sizeof(label), "control: \"%s\" has its anchor in %s exactly once", f.name, f.layer ? "ui_layer.cpp" : "vscreen.cpp");
        check(!mutated.empty(), label);
        bool caught = false;
        if (!mutated.empty())
            for (const WirePin& p : f.layer ? layerCurvedPins(mutated) : vscreenCurvedPins(mutated)) caught = caught || (!p.ok && std::strcmp(p.id, f.caughtBy) == 0);
        std::snprintf(label, sizeof(label), "control: \"%s\" is caught by the %s pin", f.name, f.caughtBy);
        check(caught, label);
    }
}

void testWiringLayer() {
    const std::string text = readText("src/d3d11/ui_layer.cpp");
    check(!text.empty(), "src/d3d11/ui_layer.cpp is readable from the working directory");
    const std::string all = afterui::squeeze(text);
    const std::string decide = bodyOf(text, "bool uiLayerDecide(ID3D11DeviceContext* ctx, int familyInt", "the layer's gatherer (uiLayerDecide)");
    // The route is asked for the screen family alone, once, in the gatherer.
    check(has(decide, "f.worldRoute=family==UiLayerFamily::kScreen&&vrWorldRouteLayerMayTake();") &&
              countOf(all, "vrWorldRouteLayerMayTake()") == 1,
          "the gatherer asks the route (vrWorldRouteLayerMayTake) for the 2D screen family alone, and nothing else in the layer does");
    std::string missing;
    // (The plan call, the plan's first refusals and End's landed branch -- what the curved route changed -- are layerCurvedPins, below.)
    // A refusal in the route's mode is counted as a refusal and named; only a passed draw is planned.
    check(has(decide, "++g_win.decided[static_cast<size_t>(family)][static_cast<size_t>(d)];noteFamily(family,d,detail);worldRefuse(f.eye,uiWorldReasonId(d));"),
          "a refusal in the route's mode is counted as that decision, named, and logged as the route's reason");
    check(has(decide, "if(familyInt!=static_cast<int>(UiLayerFamily::kAfterUi)){g_reissue.pending=false;detail::g_uiLayerWorldReissue=false;}"),
          "a real family's decision forgets a pending re-issue; the after-UI retry's decisions leave it alone");
    // Begin: under the internal scope, validating before binding, the layer through the ordinary machinery.
    const std::string begin = bodyOf(text, "bool uiLayerWorldReissueBegin(ID3D11DeviceContext* ctx)", "uiLayerWorldReissueBegin");
    reportOrder(inOrder(begin, {"g_reissue.pending", "VrWorldInternalScopeinternal;", "g_tc.info.resource!=plan.targetRes", "res.Get()==plan.screenRes",
                                "g_draw.decided=true;", "g_draw.family=UiLayerFamily::kScreen;", "g_draw.shape=plan.shape;", "beginGuarded(ctx,0)",
                                "ctx->PSGetShaderResources(0,1,&plan.savedSrv);", "ctx->PSGetSamplers(0,1,&plan.savedSampler);",
                                "vScreenPSSetShaderResourcesRaw(ctx,0,1,&mips);", "ctx->PSSetSamplers(0,1,&sampler);"},
                        &missing),
                missing, "Begin: the internal scope, the decided draw's bindings re-checked, the layer bound by the ordinary machinery, then the mipped screen and the trilinear sampler at PS slot 0");
    check(!has(begin, "vrWorldRouteNoteEyeTaken(") && !has(begin, "returntrue;}}}"),
          "Begin never tells the route an eye was taken (only a re-issue that landed does)");
    check(has(begin, "worldReleaseSaved(plan);worldRefuse(plan.eye,uiWorldReasonId(why));returnfalse;"),
          "a refusal at Begin releases what it held, is counted by reason and named, and leaves the game's state alone");
    // End (layerCurvedPins, below): everything back first, the layer's own End, the landed test, and only then the route told of the eye.
    check(countOf(all, "vrWorldRouteNoteEyeTaken(") == 1, "vrWorldRouteNoteEyeTaken is called in exactly one place");
    // The plan (layerCurvedPins, below): the depth and blend refusals before anything is held; the sources under the internal scope, outside the guard.
    const std::string sources = bodyOf(text, "UiWorldRefuse worldReissueSources(ID3D11DeviceContext* ctx,", "worldReissueSources");
    reportOrder(inOrder(sources, {"ctx->PSGetShaderResources(0,1,&srv);", "returnUiWorldRefuse::kNoSource;", "ctx->PSGetSamplers(0,1,&smp);",
                                  "returnUiWorldRefuse::kNoSampler;", "vrWorldMipsScreen(ctx,tex.Get(),seq)", "returnUiWorldRefuse::kMipsNull;",
                                  "vrWorldMipsSampler(dev.Get(),sd)", "returnUiWorldRefuse::kSamplerNull;"},
                        &missing),
                missing, "the sources: the draw's own PS slot 0 texture and sampler, the mipped screen for this frame, a sampler made like the game's");
    // The boundary and shutdown forget a pending re-issue.
    const std::string boundary = bodyOf(text, "void uiLayerFrameBoundary(ID3D11DeviceContext* ctx)", "uiLayerFrameBoundary");
    check(has(boundary, "worldReissueReset();"), "the frame boundary drops a pending re-issue");
    // The accessors the route reads.
    check(has(all, "booluiLayerWorldScreenHeld(){returng_screenHeld==1;}") && has(all, "booluiLayerLiveForWorldRoute(){returndetail::g_uiLayerLive;}") &&
              has(all, "detail::g_uiLayerLive=uiLayerLiveFor(g_target,g_temporal,g_jitterAsShipped,g_stoodDown);"),
          "the route's accessors read the gate and the layer's liveness, and the layer's liveness is uiLayerLiveFor's");
    // What the curved route changed in the layer (held by testCurvedControls).
    for (const WirePin& p : layerCurvedPins(text)) check(p.ok, p.what);
}

void testWiringVscreen() {
    const std::string text = readText("src/d3d11/vscreen.cpp");
    check(!text.empty(), "src/d3d11/vscreen.cpp is readable from the working directory");
    std::string missing;
    const std::string reissue = bodyOf(text, "__declspec(noinline) void worldScreenReissue(TracePolicy& trace,", "worldScreenReissue");
    reportOrder(inOrder(reissue, {"if(uiLayerIssueBlocked())", "VrWorldInternalScopeinternal;", "constboolbegan=uiLayerWorldReissueBegin(self);",
                                  "if(began){", "GpuCensusScopecensus(self,GpuCensusSection::FrameWorldLayer);", "pureDrawReissue(self,kind,count,instances,args);",
                                  "uiLayerWorldReissueEnd(self);"},
                        &missing),
                missing, "worldScreenReissue: the internal scope, Begin, the census scope (FrameWorldLayer), the game's draw once more, End");
    const std::string fwd = bodyOf(text, "void forwardWithVerdict(TracePolicy& trace, ID3D11DeviceContext* self, DrawVerdict v,", "forwardWithVerdict");
    reportOrder(inOrder(fwd, {"structWorldReissueScope{boolon=false;~WorldReissueScope(){if(on)uiLayerWorldReissueAbandon();}}worldReissue;",
                              "uiLayer=uiLayerDecide(self,static_cast<int>(uiFamily)", "worldReissue.on=uiLayerWorldReissuePending();",
                              "uiLayer=uiLayerNoteOther(", "if(uiLayer&&worldReissue.on){worldReissue.on=false;uiLayerWorldReissueAbandon();}",
                              "constbooloriginalIssued=observedDraw(", "crispHudTonemapReissue(trace,self,kind,count,instances,args);",
                              "if(worldReissue.on&&originalIssued){worldScreenReissue(trace,self,kind,count,instances,args);}", "forwardVerdictEnd(self,v);"},
                        &missing),
                missing,
                "forwardWithVerdict: the pending re-issue is scoped to the call, taken from the decision, dropped if the after-UI retry took the draw, issued after the game's own draw and the crisp re-issue, before the verdict is undone");
    const std::string tail = bodyOf(text, "void STDMETHODCALLTYPE hookedDrawIndexedInstanced(",
                                    "hookedDrawIndexedInstanced");
    reportOrder(inOrder(tail, {"if(screenMotionLive()&&uiLayerWorldReissuePending())screenMotionRecognize();",
                               "if(screenMotionLive()&&!uiLayerRedirecting()&&!uiLayerWorldReissuePending()){", "screenMotionUiDraw(self,", "screenMotionDraw(self,"},
                        &missing),
                missing, "the tail of the game's draw: a draw the route re-issues runs the recognition alone, and skips the per-eye screen motion reissues (the two existing calls are untouched, behind one more term)");
    // The recognition is called in exactly three places: the route's (above, unchanged), behind the on-foot maps gate's flag, a 2D screen
    // composite the layer TOOK (design-world-camera-motion-2026-09-30.md, Phase 1: a take swallows the per-eye call, and naming stops two
    // frames after the last recognised composite), and the curved screen's after the substitution (the substitution returns before the flat
    // tail, so curvedScreenSwallowed runs it for a frame the route owns). With the key off uiLayerMapsOn() is false and the second is one load.
    check(has(afterui::squeeze(text), "if(uiLayer&&uiFamily==UiLayerFamily::kScreen&&uiLayerMapsOn()&&screenMotionLive()&&screenMotionRecognize())uiLayerMapsNoteRecognised();"),
          "a taken 2D screen's recognition sits behind uiLayerMapsOn() (the on-foot maps gate)");
    // The curved route's wiring (the curve branch, the curved re-issue, what a swallowed draw does, the flat re-issue untouched, the count of
    // recognitions: three), held by testCurvedControls.
    for (const WirePin& p : vscreenCurvedPins(text)) check(p.ok, p.what);
}

void testWiringDoor() {
    const std::string temporalText = readText("src/d3d11/native_temporal.cpp");
    check(!temporalText.empty(), "src/d3d11/native_temporal.cpp is readable from the working directory");
    std::string missing;
    const std::string begin = bodyOf(temporalText, "HRESULT WINAPI begin(void* p,", "native_temporal begin()");
    check(has(begin, "s->currentSettings.on&&!s->standDown&&s->currentSettings.jitter&&!edvr::vrWorldRouteOwnsNextFrame())for(inte=0;e<2;++e)"),
          "begin(): the eye shift is advertised only while the route does not own the next frame (the gate gains one lock-free read)");
    check(countOf(afterui::squeeze(temporalText), "vrWorldRouteOwnsNextFrame()") == 1, "...and that is the only place native_temporal asks");
    const std::string treat = bodyOf(temporalText, "HRESULT WINAPI treat(void* p,uint64_t seq,", "native_temporal treat()");
    reportOrder(inOrder(treat, {"floorOutput(*s,eye,w,h,outW,outH);", "if(layerOnlyTreat(*s,seq,eye,outW?outW:w,outH?outH:h,d.Format,b,output,outBox)){s->treated[eye]=true;returnS_OK;}",
                                "s->verdictPending[eye]", "void*raw=edvrTemporalAa("},
                        &missing),
                missing, "treat(): the layer-only branch sits after the output sizing (the served floor's cut included) and before the history and the pass, and answers S_OK");
    check(!has(treat.substr(0, treat.find("void*raw=edvrTemporalAa(")), "s->continuity[eye]=") && !has(treat.substr(0, treat.find("if(layerOnlyTreat(")), "hst.valid=true"),
          "...and leaves the eye's continuity alone (a later eye-route frame finds it broken and resets the history)");
    const std::string helper = bodyOf(temporalText, "bool layerOnlyTreat(State& s,", "layerOnlyTreat");
    reportOrder(inOrder(helper, {"if(!edvr::uiLayerDoorLayerOnly(eye,seq))returnfalse;", "blankFrame(s,eye,outW,outH,format)", "edvr::uiLayerWorldDoorGap(seq,eye,frame)",
                                 "returnfalse;}", "frame->AddRef();*output=frame;", "edvr::uiLayerNoteTemporal(seq,eye,frame);", "++s.layerOnly;", "returntrue;}"},
                        &missing),
                missing, "layerOnlyTreat: only for an eye the layer holds whole (the route's world, or a map or menu under the maps gate), the black frame, the layer's preflight (a gap leaves the eye to the pass), then the frame handed on and noted to the layer");
    check(!has(helper, "returnS_FALSE") && !has(helper, "returnE_") && !has(helper, "*output=nullptr") && !has(helper, "s.continuity") && !has(helper, "s.history"),
          "...and it has no S_FALSE, no error, no null output, and touches neither continuity nor history");
    const std::string blank = bodyOf(temporalText, "ID3D11Texture2D* blankFrame(State& s,", "blankFrame");
    check(has(blank, "BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;") && has(blank, "black[4]={0.f,0.f,0.f,1.f};edvr::vScreenClearRenderTargetViewRaw(ctx,rtv,black);") &&
              has(blank, "other.texture->AddRef();mine=other;returnmine.texture;"),
          "the black frame: made once per (size, format), cleared once to (0, 0, 0, 1) when made, shared by the eyes while they agree, never cleared again");
    check(has(afterui::squeeze(temporalText), "releaseBlank(s->blank[0]);releaseBlank(s->blank[1]);"), "...and released at close");

    const std::string sharpenText = readText("src/d3d11/native_sharpen.cpp");
    check(!sharpenText.empty(), "src/d3d11/native_sharpen.cpp is readable from the working directory");
    const std::string sharpen = bodyOf(sharpenText, "HRESULT WINAPI treat(void* p,uint64_t seq,", "native_sharpen treat()");
    const size_t only = sharpen.find("if(edvr::uiLayerDoorLayerOnly(eye,seq)){");
    const size_t ordinary = sharpen.rfind("if(s->strength<=0.f||s->stoodDown){");
    check(only != std::string::npos && ordinary != std::string::npos && only < ordinary, "sharpen: the layer-only branch precedes the ordinary path");
    if (only != std::string::npos && ordinary != std::string::npos && only < ordinary) {
        const std::string layerOnly = sharpen.substr(only, ordinary - only);
        const size_t composite = layerOnly.find("edvr::uiLayerComposite(seq,eye,source,region,layerUv)");
        const size_t rcas = layerOnly.find("edvrSharpen(");   // the FIRST RCAS call of the branch
        check(composite != std::string::npos && rcas != std::string::npos && composite < rcas && countOf(layerOnly, "edvrSharpen(") == 1 &&
                  has(layerOnly, "edvrSharpen(layered,int(eye),full,s->strength)"),
              "sharpen: for a layer-only eye the UI layer is composited FIRST and RCAS runs once, over the composited eye");
        check(has(layerOnly, "++s->layerOnlyBlack;") && has(layerOnly, "returnS_FALSE;") && has(layerOnly, "s->layerOnlyBlack<=8"),
              "sharpen: a composite that does not run for a layer-only eye is counted, and the first eight named");
        const std::string rest = sharpen.substr(ordinary);
        check(rest.find("edvrSharpen(source,int(eye),bounds,s->strength)") < rest.find("edvr::uiLayerComposite(seq,eye,result,whole,layerUv)"),
              "sharpen: every other eye keeps the ordinary order (RCAS on the frame, then the layer over it)");
    }
    check(countOf(sharpen, "uiLayerDoorLayerOnly(") == 1, "sharpen asks the layer's door predicate once");
}

void testControls() {
    const std::string layerText = readText("src/d3d11/ui_layer.cpp");
    const std::string vscreenText = readText("src/d3d11/vscreen.cpp");
    check(!layerText.empty() && !vscreenText.empty(), "the controls read ui_layer.cpp and vscreen.cpp from the working directory");
    if (layerText.empty() || vscreenText.empty()) return;
    testCurvedControls(layerText, vscreenText);
    bool pristine = true;
    for (const WirePin& p : layerCurvedPins(layerText)) pristine = pristine && p.ok;
    for (const WirePin& p : vscreenCurvedPins(vscreenText)) pristine = pristine && p.ok;
    check(pristine, "control: with no edit every curved-route pin of the two sources holds (the controls above are the only way one fails)");
}

void testWiring() {
    testWiringLayer();
    testWiringVscreen();
    testWiringDoor();
    testControls();
}

}  // namespace worldroute

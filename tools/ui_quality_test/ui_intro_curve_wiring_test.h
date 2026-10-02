// Included after ui_world_route_wiring_test.h (its helpers: worldroute::WirePin, quietBody, inOrder, has, countOf, readText, flipOnce) and
// ui_after_ui_test.h (afterui::squeeze).
//
// THE SURFACE STRIP'S WIRING INTO vscreen.cpp, by source scan (the rig runs from the repo root; docs/intro-video.md, 2026-10-01, job 3). With
// fix.panel_curvature above 0 the intro movie's composite (EDVR places its quad: intro_panel.h) and the splash's (the game's own placement,
// recognised by intro_curve.h) are drawn as the bent surface strip (panel_curve.h panelCurveSurfaceDraw) IN THE PLACE of the game's flat quad. What
// matters, and what each pin below holds:
//   * AT CURVATURE 0 EVERY CALL AND ITS ORDER IS TODAY'S. The recogniser is asked only behind introCurveWants() (one load); the strip's site is
//     two cheap tests that an ordinary draw fails, and the game's own issue (observedDraw), the dim's own draw and their order are the text they
//     were -- the only additions on that path are `if (stripIssued) return true;` in the lambda and the strip's fallback in the dim;
//   * a strip that cannot be drawn leaves the game's own draw to be issued, flat, never missing (stripIssued is only ever panelCurveSurfaceDraw's
//     answer), and `originalIssued` still means "something was issued";
//   * the strip sits AFTER the verdict's Begin (kBackdrop's slot swap is in place) and INSIDE the layer's bracket, replaces the game's issue and not
//     the dim, and the dim follows it (the same arguments, the game's draw when the strip did not draw);
//   * the movie's numbers are (introPanelStripGain(), +1, introPanelStripReverseU()), the splash's (introCurveGain(), introCurveToward(),
//     introCurveReverseU()) -- the u direction (the mirror fix) is each caller's own, read only while its strip is armed and never from the other's
//     placement -- the call, and the dim's re-issue of it, pass all three, through the thunk's REAL draw behind the same refusal observedDraw makes,
//     and there are exactly two panelCurveSurfaceDraw call sites;
//   * the splash's flag is its own (NOT curveThisDraw, whose branch returns before the verdict's Begin and the dim): cleared at the top of every
//     beginPanelOverride, set only behind introCurveWants() and right AFTER the movie's claim (a draw the movie claims never reaches the
//     recogniser), put away by the thunk after the draw and the dim;
//   * the on-foot curve's recognition (size alone) never claims the intro composite, whose size it can match under a stock vscreen_res;
//   * the draw gate lists the recogniser (a subscriber that is not listed starves when nothing else is on);
//   * the frame boundary ticks it beside the movie's, with the same scene signal, says its one line once, and the shutdown releases it.
// Each pin is a function of a source text, so testIntroCurveControls runs the very same function on copies with one edit each that puts a likely
// slip back and requires the pin it names to fail. A pin that cannot fail proves nothing. tools\vr_world_route_test\mutants.py's `wiring` rig
// runs the whole rig on edited copies of the real vscreen.cpp as well (ui_quality_test --wiring).
namespace introcurve {
using worldroute::countOf;
using worldroute::has;
using worldroute::inOrder;
using worldroute::quietBody;
using worldroute::readText;
using worldroute::WirePin;

// The squeezed statements the pins read, each the text of one statement of vscreen.cpp with its comments and whitespace out.
const char* const kClear = "s->curveThisDraw=false;s->introCurveThisDraw=false;";
const char* const kSet = "if(kind=='X'&&count==6&&introCurveWants()){s->introCurveThisDraw=introCurveOnComposite(self,kind,count,instances);}";
const char* const kEnd = "if(g_state->introCurveThisDraw){g_state->introCurveThisDraw=false;introCurveEndDraw();}";
const char* const kGuard =
    "if(panelCurveWants()&&srv0IsPanelSized(s,kind,count)&&!(kind=='X'&&count==6&&bindingShaderHash(BindSlot::Vs)==kIntroCompositeVsHash)){s->curveThisDraw=true;}";
const char* const kCandidate = "if((v==DrawVerdict::kIntroPanel||g_state->introCurveThisDraw)&&owner&&panelCurveSurfaceWanted()){";
const char* const kNumbers =
    "if(v==DrawVerdict::kIntroPanel){if(introPanelStripArmed()){stripGain=introPanelStripGain();stripToward=1;stripReverseU=introPanelStripReverseU();}}"
    "else{stripGain=introCurveGain();stripToward=introCurveToward();stripReverseU=introCurveReverseU();}";
const char* const kStripCall =
    "if(stripToward!=0&&!uiLayerIssueBlocked()){stripIssued=panelCurveSurfaceDraw(self,stripGain,stripToward,stripReverseU,g_state->realDrawIndexedInstanced);";
const char* const kObserved =
    "constbooloriginalIssued=observedDraw(alteredClass==AlteredDrawClass::Verdict?AlteredDraw(alteredClass,alteredFixOf(v)):AlteredDraw(alteredClass));";
const char* const kLambda =
    "autoobservedDraw=[&](AlteredDrawaltered){if(owner&&uiLayerIssueBlocked())returnfalse;if(stripIssued)returntrue;constboolissued=draw(altered);"
    "if(seedOutcome.on)seedOutcome.original=seedOutcome.original||issued;returnissued;};";
const char* const kDim =
    "if((v==DrawVerdict::kBackdrop||v==DrawVerdict::kIntroPanel)&&splashDimBegin(self)){if(!(stripIssued&&panelCurveSurfaceDraw(self,stripGain,stripToward,"
    "stripReverseU,g_state->realDrawIndexedInstanced))){draw(AlteredDrawClass::None);}splashDimEnd(self);}";
const char* const kTick =
    "tkIntroCurve.run([&]{constboolsceneFrame=g_state->eyeDrawsLastFrame>=kSceneEyeDraws;introCurveTick(g_state->ownerCtx,sceneFrame);"
    "if(sceneFrame)introCurveNoteRetired();});";
const char* const kRetire =
    "staticboolsaid=false;if(said)return;said=true;constuint64_tdrawn=panelCurveSurfaceInfo().drawn;if(drawn==0&&!panelCurveSurfaceWanted())return;"
    "Log::get().note(\"intro curve: %llu strip draw(s) in all (the movie's, the splash's and the splash dim's re-issues of either); the splash recogniser handed over %llu "
    "composite draw(s)\",static_cast<unsignedlonglong>(drawn),static_cast<unsignedlonglong>(introCurveInfo().armed));}";
// The end of the movie's claim -- its `return DrawVerdict::kIntroPanel;` and the two closing braces -- which the recognition follows at once.
const char* const kMovieClaimEnd = "returnDrawVerdict::kIntroPanel;}}";

// The strip's site: from its first statement to the game's own issue, squeezed (empty when either end is not found).
std::string stripBlock(const std::string& fwd) {
    const size_t from = fwd.find("floatstripGain=0.0f;");
    const size_t to = fwd.find("constbooloriginalIssued=");
    return from != std::string::npos && to != std::string::npos && from < to ? fwd.substr(from, to - from) : std::string();
}

std::vector<WirePin> wiringPins(const std::string& text) {
    std::vector<WirePin> pins;
    const std::string all = afterui::squeeze(text);
    const std::string bpo = quietBody(text, "DrawVerdict beginPanelOverride(ID3D11DeviceContext* self, char kind, UINT count,");
    const std::string fwd = quietBody(text, "void forwardWithVerdict(ID3D11DeviceContext* self, DrawVerdict v,");
    const std::string thunk = quietBody(text, "void STDMETHODCALLTYPE hookedDrawIndexedInstanced(");
    const std::string gate = quietBody(text, "bool drawGateSubscribed(State* s) {");
    const std::string frame = quietBody(text, "void vScreenFrameBoundary() {");
    const std::string shut = quietBody(text, "void shutdownVScreenFixes() {");
    const std::string note = quietBody(text, "void introCurveNoteRetired() {");
    const std::string block = stripBlock(fwd);

    // The splash's flag: its own, cleared at the top of every beginPanelOverride (before anything can set it), set in one place only, and put away
    // by the thunk after forwardWithVerdict -- the draw and the dim have used it by then -- on every way out of that call.
    pins.push_back({"flag-lifetime",
                    has(all, "boolintroCurveThisDraw=false;") && has(bpo, kClear) && has(bpo, kSet) && inOrder(bpo, {kClear, kSet}) &&
                        countOf(all, "introCurveOnComposite(") == 1 && countOf(all, "introCurveEndDraw()") == 1 &&
                        inOrder(thunk, {"forwardWithVerdict(self,v,'X',", "introPanelEndDraw(self);", kEnd}),
                    "intro curve wiring [flag-lifetime]: introCurveThisDraw is cleared right after curveThisDraw at the top of beginPanelOverride, before the one place that "
                    "sets it, and the thunk puts the numbers away (introCurveEndDraw) and the flag after forwardWithVerdict, once"});
    // The recognition: shape first, then introCurveWants() (one load at curvature 0), in the eye branch and before the eye-side verdicts it composes
    // with are returned.
    pins.push_back({"recognition",
                    has(bpo, kSet) && inOrder(bpo, {"++s->eyeDrawsThisFrame;", kSet, "returnDrawVerdict::kBackdrop;"}),
                    "intro curve wiring [recognition]: the recogniser is asked in the eye-draw branch only for a six-index X draw and only behind introCurveWants(), before the "
                    "eye-side kBackdrop is returned (a flag, so it composes with the verdict)"});
    // The recognition sits right after the movie's claim: a draw the movie claims never reaches the recogniser, so its stock pair is not learned (and
    // logged as flat) once the movie has a placement of its own; only its settle frames, which the movie does not claim, are asked.
    pins.push_back({"after-movie",
                    has(bpo, (std::string(kMovieClaimEnd) + kSet).c_str()) && countOf(bpo, "returnDrawVerdict::kIntroPanel;") == 1,
                    "intro curve wiring [after-movie]: the recognition sits right after the movie's claim (its `return DrawVerdict::kIntroPanel;` block), so a draw the movie "
                    "claims never reaches the recogniser"});
    // The on-foot curve's recognition does not claim the intro composite; the term sits behind panelCurveWants().
    pins.push_back({"guard",
                    has(bpo, kGuard) && countOf(bpo, "s->curveThisDraw=true;") == 1 && countOf(all, "kIntroCompositeVsHash") == 1,
                    "intro curve wiring [guard]: the on-foot curve's recognition (panelCurveWants() && srv0IsPanelSized) does not claim a six-index X draw whose VS is the intro "
                    "composite's, and the new term sits behind panelCurveWants()"});
    // The draw gate lists the recogniser.
    pins.push_back({"gate",
                    has(gate, "||vscreenFootprintWanted()||introCurveWants();}") && countOf(all, "introCurveWants()") == 2,
                    "intro curve wiring [gate]: drawGateSubscribed lists introCurveWants() (the recogniser acts below the gate; the on-foot strip's panelCurveWants() stands down "
                    "on its own)"});
    // The strip's site: after the verdict's Begin and the layer's Begin, before the game's own issue, the layer's End and the dim; the dim follows.
    pins.push_back({"strip-site",
                    inOrder(fwd, {"boolstripIssued=false;", "autoobservedDraw=[&](AlteredDrawaltered){", "if(v!=DrawVerdict::kNone)forwardVerdictBegin(self,v);",
                                  "constboollayered=uiLayer&&uiLayerBegin(self);", "floatstripGain=0.0f;intstripToward=0;boolstripReverseU=false;", kCandidate,
                                  "stripIssued=panelCurveSurfaceDraw(self,stripGain,stripToward,stripReverseU,g_state->realDrawIndexedInstanced);",
                                  "constbooloriginalIssued=observedDraw(", "if(layered){uiLayerEnd(self);", "if(v==DrawVerdict::kBackdrop)backdropEnd(self);",
                                  "splashDimBegin(self)", "panelCurveSurfaceDraw(self,stripGain,stripToward,stripReverseU,g_state->realDrawIndexedInstanced)",
                                  "draw(AlteredDrawClass::None);", "splashDimEnd(self);", "forwardVerdictEnd(self,v);"}) &&
                        countOf(fwd, "panelCurveSurfaceDraw(") == 2 && countOf(all, "panelCurveSurfaceDraw(") == 2,
                    "intro curve wiring [strip-site]: the strip is drawn after forwardVerdictBegin (kBackdrop's slot swap is in place) and the layer's Begin, before the game's own "
                    "issue and the layer's End, and the splash dim draws it again (the same arguments, the u direction included) before forwardVerdictEnd; "
                    "panelCurveSurfaceDraw has two call sites, and the u direction starts false"});
    // The unarmed path is today's text: the game's own issue, the lambda it goes through (one early return added), and the dim's own draw.
    pins.push_back({"unarmed-text",
                    has(fwd, kObserved) && has(fwd, kLambda) && has(fwd, kDim),
                    "intro curve wiring [unarmed-text]: `const bool originalIssued = observedDraw(...)` is the text it was, the lambda only gains `if (stripIssued) return true;` "
                    "after its refusal, and the dim still draws the game's own `draw(AlteredDrawClass::None)` when the strip did not draw"});
    // The candidate test: two cheap tests first (the verdict, the flag), the cross-TU question last, so an ordinary draw pays nothing.
    pins.push_back({"candidate", has(fwd, kCandidate),
                    "intro curve wiring [candidate]: the strip is a candidate only for the movie's verdict or the splash's flag, on the owner context, and panelCurveSurfaceWanted() "
                    "is asked last"});
    // The numbers, the refusal, the real draw pointer.
    pins.push_back({"strip-args", has(fwd, kNumbers) && has(fwd, kStripCall) && countOf(all, "introPanelStripArmed()") == 1 && countOf(all, "introPanelStripGain()") == 1 &&
                                      countOf(all, "introCurveGain()") == 1 && countOf(all, "introCurveToward()") == 1 && countOf(all, "introPanelStripReverseU()") == 1 &&
                                      countOf(all, "introCurveReverseU()") == 1,
                    "intro curve wiring [strip-args]: the movie's numbers are (introPanelStripGain(), +1, introPanelStripReverseU()) and only while introPanelStripArmed(), the splash's "
                    "(introCurveGain(), introCurveToward(), introCurveReverseU()), each u direction read once and from its own placement; the strip is asked only when a direction "
                    "is set and observedDraw's own refusal passes, through the thunk's real draw"});
    // Flat, never missing: stripIssued is the strip's own answer and nothing else.
    pins.push_back({"fallback", has(fwd, "boolstripIssued=false;") && !has(fwd, "stripIssued=true") && countOf(fwd, "stripIssued=") == 2,
                    "intro curve wiring [fallback]: stripIssued starts false and is assigned only from panelCurveSurfaceDraw's answer, so a strip that cannot be drawn leaves the "
                    "game's own draw to be issued"});
    // Not the on-foot flag, anywhere in the strip's own text.
    pins.push_back({"not-on-foot-flag",
                    !block.empty() && !has(block, "->curveThisDraw") && !has(block, "panelCurveSubstitute(") && !has(kSet, "->curveThisDraw") && !has(kEnd, "->curveThisDraw") &&
                        has(bpo, kSet) && has(thunk, kEnd),
                    "intro curve wiring [not-on-foot-flag]: the splash's strip uses introCurveThisDraw and never curveThisDraw or the on-foot substitution (that branch returns before "
                    "the verdict's Begin and the dim)"});
    // The boundary tick: declared, run once beside the movie's with the same scene signal, its one line only at the scene.
    pins.push_back({"tick",
                    has(all, "EDVR_BOUNDARY_TICK(tkIntroCurve,\"intro_curve\");") && countOf(all, "tkIntroCurve.run(") == 1 && countOf(all, "introCurveTick(") == 1 &&
                        inOrder(frame, {"tkIntroPanel.run([&]{introPanelTick(g_state->ownerCtx,g_state->eyeDrawsLastFrame>=kSceneEyeDraws);});", kTick, "tkIntroSkip.run("}),
                    "intro curve wiring [tick]: tkIntroCurve is declared and run once, right after the movie's tick and before the skip's, with the same scene signal "
                    "(eyeDrawsLastFrame >= kSceneEyeDraws), and its retirement line is called only on a scene frame"});
    pins.push_back({"shutdown", has(shut, "introPanelShutdown();introCurveShutdown();") && countOf(all, "introCurveShutdown()") == 1,
                    "intro curve wiring [shutdown]: introCurveShutdown() runs beside introPanelShutdown(), once"});
    // The one line: said once, at the first scene frame, nothing at curvature 0 unless something was drawn, both counters.
    pins.push_back({"retire-line", note == std::string("voidintroCurveNoteRetired(){") + kRetire,
                    "intro curve wiring [retire-line]: introCurveNoteRetired says `intro curve: %llu strip draw(s) in all (the movie's, the splash's and the splash dim's re-issues of "
                    "either); the splash recogniser handed over %llu composite draw(s)` once (the first scene frame), from panelCurveSurfaceInfo().drawn and introCurveInfo().armed, "
                    "and nothing at curvature 0 when nothing was drawn (at curvature above 0 it is said even with 0 and 0)"});
    return pins;
}

// One control: a chain of edits of vscreen.cpp (each anchor found exactly once in the text as it stands then) that must be caught by the pin named.
struct Flip {
    const char* name;
    std::vector<std::pair<std::string, std::string>> edits;
    const char* caughtBy;
};

// The raw text of the blocks the controls move or delete (verbatim from vscreen.cpp).
const char* const kRawClear = R"x(    s->curveThisDraw = false;
    s->introCurveThisDraw = false;
)x";
const char* const kRawSet = R"x(    if (kind == 'X' && count == 6 && introCurveWants()) {
        s->introCurveThisDraw = introCurveOnComposite(self, kind, count, instances);
    }
)x";
const char* const kRawEnd = R"x(    if (g_state->introCurveThisDraw) {
        g_state->introCurveThisDraw = false;
        introCurveEndDraw();
    }
)x";
const char* const kRawBackdrop = "        return DrawVerdict::kBackdrop;\n    }\n\n    // The FSS panel composite pair";
const char* const kRawBegin = "    if (v != DrawVerdict::kNone) forwardVerdictBegin(self, v);\n";
const char* const kRawObserved = R"x(    const bool originalIssued=observedDraw(alteredClass == AlteredDrawClass::Verdict
                                               ? AlteredDraw(alteredClass, alteredFixOf(v)) : AlteredDraw(alteredClass));
)x";
const char* const kRawStripStart = "    float stripGain = 0.0f;\n";
const char* const kRawGuard = R"x(    if (panelCurveWants() && srv0IsPanelSized(s, kind, count) &&
        !(kind == 'X' && count == 6 && bindingShaderHash(BindSlot::Vs) == kIntroCompositeVsHash)) {
)x";
const char* const kRawDim = R"x(            if (!(stripIssued && panelCurveSurfaceDraw(self, stripGain, stripToward, stripReverseU, g_state->realDrawIndexedInstanced))) {
                draw(AlteredDrawClass::None);
            }
)x";
const char* const kRawTick = R"x(        tkIntroCurve.run([&] {
            const bool sceneFrame = g_state->eyeDrawsLastFrame >= kSceneEyeDraws;
            introCurveTick(g_state->ownerCtx, sceneFrame);
            if (sceneFrame) introCurveNoteRetired();
        });
)x";
const char* const kRawSkipTick = "        tkIntroSkip.run([&] {\n";
const char* const kRawMovieComment = "    // The intro movie's panel (intro_panel.h). First thing in the eye\n";
const char* const kRawCensusLine = "    // The census line for this draw, recorded while its bindings are certainly\n";

void testIntroCurveControls(const std::string& text) {
    const std::vector<Flip> flips = {
        // ---- flag-lifetime
        {"the flag is never cleared at the top", {{kRawClear, "    s->curveThisDraw = false;\n"}}, "flag-lifetime"},
        {"the flag is cleared after it is set", {{kRawClear, "    s->curveThisDraw = false;\n"}, {kRawSet, std::string(kRawSet) + "    s->introCurveThisDraw = false;\n"}}, "flag-lifetime"},
        {"the thunk never puts the numbers away", {{kRawEnd, ""}}, "flag-lifetime"},
        {"the thunk puts them away before the draw",
         {{kRawEnd, ""}, {"    const DrawVerdict v = beginPanelOverride(self, 'X', perInstance, instances, args);\n",
                          "    const DrawVerdict v = beginPanelOverride(self, 'X', perInstance, instances, args);\n" + std::string(kRawEnd)}},
         "flag-lifetime"},
        {"the numbers are put away and the flag is left", {{"        g_state->introCurveThisDraw = false;\n        introCurveEndDraw();\n", "        introCurveEndDraw();\n"}}, "flag-lifetime"},
        {"a second place asks the recogniser",
         {{"        s->introCurveThisDraw = introCurveOnComposite(self, kind, count, instances);\n    }\n",
           "        s->introCurveThisDraw = introCurveOnComposite(self, kind, count, instances);\n    }\n    if (kind == 'X' && count == 6) {\n"
           "        s->introCurveThisDraw = introCurveOnComposite(self, kind, count, instances);\n    }\n"}},
         "flag-lifetime"},
        // ---- recognition
        {"the recogniser is asked at curvature 0 too", {{"if (kind == 'X' && count == 6 && introCurveWants()) {", "if (kind == 'X' && count == 6) {"}}, "recognition"},
        {"the recogniser is asked for every draw", {{"if (kind == 'X' && count == 6 && introCurveWants()) {", "if (introCurveWants()) {"}}, "recognition"},
        {"the flag is set whatever the recogniser said",
         {{"s->introCurveThisDraw = introCurveOnComposite(self, kind, count, instances);\n",
           "introCurveOnComposite(self, kind, count, instances);\n        s->introCurveThisDraw = true;\n"}},
         "recognition"},
        {"the recognition sits below the eye-side backdrop claim",
         {{kRawSet, ""}, {kRawBackdrop, std::string("        return DrawVerdict::kBackdrop;\n    }\n") + kRawSet + "\n    // The FSS panel composite pair"}}, "recognition"},
        // ---- after-movie
        {"the recognition is moved above the movie's claim", {{kRawSet, ""}, {kRawMovieComment, std::string(kRawSet) + "\n" + kRawMovieComment}}, "after-movie"},
        {"the recognition is moved a block away from the movie's claim", {{kRawSet, ""}, {kRawCensusLine, std::string(kRawSet) + "\n" + kRawCensusLine}}, "after-movie"},
        // ---- guard
        {"the on-foot recognition claims the intro composite again",
         {{kRawGuard, "    if (panelCurveWants() && srv0IsPanelSized(s, kind, count)) {\n"}}, "guard"},
        {"the guard is asked before panelCurveWants()",
         {{kRawGuard, "    if (!(kind == 'X' && count == 6 && bindingShaderHash(BindSlot::Vs) == kIntroCompositeVsHash) && panelCurveWants() && srv0IsPanelSized(s, kind, count)) {\n"}},
         "guard"},
        {"the guard names the pixel shader's hash",
         {{"bindingShaderHash(BindSlot::Vs) == kIntroCompositeVsHash", "bindingShaderHash(BindSlot::Ps) == kIntroCompositeVsHash"}}, "guard"},
        {"the guard forgets the shape", {{"!(kind == 'X' && count == 6 && bindingShaderHash(BindSlot::Vs) == kIntroCompositeVsHash)", "!(bindingShaderHash(BindSlot::Vs) == kIntroCompositeVsHash)"}},
         "guard"},
        // ---- gate
        {"the draw gate does not list the recogniser",
         {{"vscreenFootprintWanted() ||   // the footprint instrument (vscreen_footprint.h): it reads the 2D screen's composite\n        introCurveWants();",
           "vscreenFootprintWanted();   // the footprint instrument (vscreen_footprint.h): it reads the 2D screen's composite\n        ("}},
         "gate"},
        {"the draw gate lists the strip's own flag instead", {{"        introCurveWants();            // the splash's", "        panelCurveSurfaceWanted();    // the splash's"}}, "gate"},
        // ---- strip-site
        {"the verdict's Begin runs after the strip", {{kRawBegin, ""}, {kRawObserved, std::string(kRawBegin) + kRawObserved}}, "strip-site"},
        {"the strip is drawn after the game's own issue", {{kRawObserved, ""}, {kRawStripStart, std::string(kRawObserved) + kRawStripStart}}, "strip-site"},
        {"the strip is drawn outside the layer's bracket",
         {{"    const bool layered = uiLayer && uiLayerBegin(self);\n    if (seedOutcome.on && layered) seedOutcome.redirected = true;\n    // Which kind",
           "    const bool layeredX = uiLayer && uiLayerBegin(self);\n    if (seedOutcome.on && layeredX) seedOutcome.redirected = true;\n    // Which kind"}},
         "strip-site"},
        {"the dim draws the game's quad only", {{kRawDim, "            draw(AlteredDrawClass::None);\n"}}, "strip-site"},
        {"the dim has no fallback",
         {{kRawDim, "            if (stripIssued) panelCurveSurfaceDraw(self, stripGain, stripToward, stripReverseU, g_state->realDrawIndexedInstanced);\n"}}, "strip-site"},
        {"the dim draws the strip whatever the main draw did",
         {{"if (!(stripIssued && panelCurveSurfaceDraw(self, stripGain, stripToward, stripReverseU, g_state->realDrawIndexedInstanced))) {",
           "if (!(panelCurveSurfaceDraw(self, stripGain, stripToward, stripReverseU, g_state->realDrawIndexedInstanced))) {"}},
         "unarmed-text"},
        {"a third place draws the strip",
         {{kRawEnd, std::string(kRawEnd) + "    panelCurveSurfaceDraw(self, 1.0f, 1, false, g_state->realDrawIndexedInstanced);\n"}}, "strip-site"},
        {"the dim drops the u direction",
         {{"panelCurveSurfaceDraw(self, stripGain, stripToward, stripReverseU, g_state->realDrawIndexedInstanced))) {",
           "panelCurveSurfaceDraw(self, stripGain, stripToward, g_state->realDrawIndexedInstanced))) {"}},
         "strip-site"},
        {"the dim hard-codes the u direction",
         {{"panelCurveSurfaceDraw(self, stripGain, stripToward, stripReverseU, g_state->realDrawIndexedInstanced))) {",
           "panelCurveSurfaceDraw(self, stripGain, stripToward, false, g_state->realDrawIndexedInstanced))) {"}},
         "strip-site"},
        {"the u direction starts true", {{"bool stripReverseU = false;", "bool stripReverseU = true;"}}, "strip-site"},
        // ---- unarmed-text
        {"observedDraw's early return comes before its refusal",
         {{"        if (owner && uiLayerIssueBlocked()) return false;\n        if (stripIssued) return true;   // the strip was issued in its place: nothing more, and something was\n",
           "        if (stripIssued) return true;   // the strip was issued in its place: nothing more, and something was\n        if (owner && uiLayerIssueBlocked()) return false;\n"}},
         "unarmed-text"},
        {"the game's draw is issued a second time", {{"        if (stripIssued) return true;   // the strip was issued in its place: nothing more, and something was\n", ""}}, "unarmed-text"},
        {"originalIssued is no longer observedDraw's answer",
         {{"const bool originalIssued=observedDraw(alteredClass", "const bool originalIssued = stripIssued || observedDraw(alteredClass"}}, "unarmed-text"},
        {"the strip's early return answers false", {{"if (stripIssued) return true;   // the strip", "if (stripIssued) return false;   // the strip"}}, "unarmed-text"},
        // ---- candidate
        {"the cross-TU question is asked first",
         {{"if ((v == DrawVerdict::kIntroPanel || g_state->introCurveThisDraw) && owner && panelCurveSurfaceWanted()) {",
           "if (panelCurveSurfaceWanted() && (v == DrawVerdict::kIntroPanel || g_state->introCurveThisDraw) && owner) {"}},
         "candidate"},
        {"the owner test is gone", {{"g_state->introCurveThisDraw) && owner && panelCurveSurfaceWanted()) {", "g_state->introCurveThisDraw) && panelCurveSurfaceWanted()) {"}}, "candidate"},
        {"every draw is a candidate",
         {{"if ((v == DrawVerdict::kIntroPanel || g_state->introCurveThisDraw) && owner && panelCurveSurfaceWanted()) {", "if (owner && panelCurveSurfaceWanted()) {"}},
         "candidate"},
        // ---- strip-args
        {"the movie is drawn at the splash's gain", {{"stripGain = introPanelStripGain();", "stripGain = introCurveGain();"}}, "strip-args"},
        {"the movie bends the other way", {{"                stripToward = 1;\n", "                stripToward = -1;\n"}}, "strip-args"},
        {"the movie is drawn whether or not its strip is armed", {{"if (introPanelStripArmed()) {", "if (true) {"}}, "strip-args"},
        {"the splash is drawn at the movie's gain", {{"stripGain = introCurveGain();", "stripGain = introPanelStripGain();"}}, "strip-args"},
        {"the splash's direction is a constant", {{"stripToward = introCurveToward();", "stripToward = 1;"}}, "strip-args"},
        {"the movie passes a constant u direction", {{"stripReverseU = introPanelStripReverseU();", "stripReverseU = true;"}}, "strip-args"},
        {"the splash passes a constant u direction", {{"stripReverseU = introCurveReverseU();", "stripReverseU = true;"}}, "strip-args"},
        {"the splash passes the movie's u direction", {{"stripReverseU = introCurveReverseU();", "stripReverseU = introPanelStripReverseU();"}}, "strip-args"},
        {"the movie passes the splash's u direction", {{"stripReverseU = introPanelStripReverseU();", "stripReverseU = introCurveReverseU();"}}, "strip-args"},
        {"the movie's u direction is read before its strip is armed",
         {{"                stripReverseU = introPanelStripReverseU();\n", ""},
          {"            if (introPanelStripArmed()) {\n", "            stripReverseU = introPanelStripReverseU();\n            if (introPanelStripArmed()) {\n"}},
         "strip-args"},
        {"the strip's call drops the u direction",
         {{"stripIssued = panelCurveSurfaceDraw(self, stripGain, stripToward, stripReverseU, g_state->realDrawIndexedInstanced);",
           "stripIssued = panelCurveSurfaceDraw(self, stripGain, stripToward, g_state->realDrawIndexedInstanced);"}},
         "strip-args"},
        {"the strip's call hard-codes the u direction",
         {{"stripIssued = panelCurveSurfaceDraw(self, stripGain, stripToward, stripReverseU, g_state->realDrawIndexedInstanced);",
           "stripIssued = panelCurveSurfaceDraw(self, stripGain, stripToward, false, g_state->realDrawIndexedInstanced);"}},
         "strip-args"},
        {"the strip is drawn through no draw function",
         {{"stripIssued = panelCurveSurfaceDraw(self, stripGain, stripToward, stripReverseU, g_state->realDrawIndexedInstanced);",
           "stripIssued = panelCurveSurfaceDraw(self, stripGain, stripToward, stripReverseU, nullptr);"}},
         "strip-args"},
        {"the strip is drawn through the thunk's wrapper",
         {{"stripIssued = panelCurveSurfaceDraw(self, stripGain, stripToward, stripReverseU, g_state->realDrawIndexedInstanced);",
           "stripIssued = panelCurveSurfaceDraw(self, stripGain, stripToward, stripReverseU, draw);"}},
         "strip-args"},
        {"the strip is asked past the refusal", {{"if (stripToward != 0 && !uiLayerIssueBlocked()) {", "if (stripToward != 0) {"}}, "strip-args"},
        {"the strip is asked with no direction set", {{"if (stripToward != 0 && !uiLayerIssueBlocked()) {", "if (!uiLayerIssueBlocked()) {"}}, "strip-args"},
        // ---- fallback
        {"the strip counts as issued whatever it returned",
         {{"stripIssued = panelCurveSurfaceDraw(self, stripGain, stripToward, stripReverseU, g_state->realDrawIndexedInstanced);",
           "panelCurveSurfaceDraw(self, stripGain, stripToward, stripReverseU, g_state->realDrawIndexedInstanced);\n            stripIssued = true;"}},
         "fallback"},
        {"every draw starts as the strip's", {{"bool stripIssued = false;", "bool stripIssued = true;"}}, "fallback"},
        // ---- not-on-foot-flag
        {"the splash's strip rides the on-foot flag",
         {{"        s->introCurveThisDraw = introCurveOnComposite(self, kind, count, instances);\n", "        s->introCurveThisDraw = introCurveOnComposite(self, kind, count, instances);\n        s->curveThisDraw = s->introCurveThisDraw;\n"}},
         "not-on-foot-flag"},
        {"the strip's site reads the on-foot flag",
         {{"(v == DrawVerdict::kIntroPanel || g_state->introCurveThisDraw) && owner", "(v == DrawVerdict::kIntroPanel || g_state->introCurveThisDraw || g_state->curveThisDraw) && owner"}},
         "not-on-foot-flag"},
        // ---- tick
        {"the tick is not run", {{kRawTick, ""}}, "tick"},
        {"the tick is told no scene", {{"introCurveTick(g_state->ownerCtx, sceneFrame);", "introCurveTick(g_state->ownerCtx, false);"}}, "tick"},
        {"the tick runs before the movie's", {{kRawTick, ""}, {"        tkIntroPanel.run([&] {\n", std::string(kRawTick) + "        tkIntroPanel.run([&] {\n"}}, "tick"},
        {"the retirement line is called every frame", {{"if (sceneFrame) introCurveNoteRetired();", "introCurveNoteRetired();"}}, "tick"},
        {"the tick's scene signal is another count",
         {{"const bool sceneFrame = g_state->eyeDrawsLastFrame >= kSceneEyeDraws;", "const bool sceneFrame = g_state->eyeDrawsThisFrame >= kSceneEyeDraws;"}}, "tick"},
        // ---- shutdown
        {"the shutdown never releases the recogniser", {{"    introPanelShutdown();\n    introCurveShutdown();\n", "    introPanelShutdown();\n"}}, "shutdown"},
        // ---- retire-line
        {"the retirement line says the old text again",
         {{"\"intro curve: %llu strip draw(s) in all (the movie's, the splash's and the splash dim's re-issues of either); the splash recogniser handed over %llu composite draw(s)\",",
           "\"intro curve: strip draws %llu (the movie's and the splash's together), %llu of them the splash's\","}},
         "retire-line"},
        {"the retirement line is reworded", {{"in all (the movie's, the splash's and the splash dim's re-issues of either)", "in all (the movie's and the splash's together)"}}, "retire-line"},
        {"the retirement line is said every time", {{"    if (said) return;\n    said = true;\n", ""}}, "retire-line"},
        {"the retirement line is said at curvature 0 too", {{"    if (drawn == 0 && !panelCurveSurfaceWanted()) return;\n", ""}}, "retire-line"},
        {"the retirement line swaps its counters",
         {{"static_cast<unsigned long long>(drawn), static_cast<unsigned long long>(introCurveInfo().armed));",
           "static_cast<unsigned long long>(introCurveInfo().armed), static_cast<unsigned long long>(drawn));"}},
         "retire-line"},
        {"the retirement line is not once per session",
         {{"    said = true;\n    const uint64_t drawn", "    const uint64_t drawn"}, {"    if (drawn == 0 && !panelCurveSurfaceWanted()) return;\n", "    if (drawn == 0 && !panelCurveSurfaceWanted()) return;\n    said = true;\n"}},
         "retire-line"},
    };
    for (const Flip& f : flips) {
        char label[420];
        std::string mutated = text;
        bool anchored = true;
        for (const auto& e : f.edits) {
            mutated = worldroute::flipOnce(mutated, e.first.c_str(), e.second.c_str());
            if (mutated.empty()) {
                anchored = false;
                break;
            }
        }
        std::snprintf(label, sizeof(label), "control: \"%s\" has its anchors in vscreen.cpp exactly once", f.name);
        check(anchored, label);
        bool caught = false;
        if (anchored)
            for (const WirePin& p : wiringPins(mutated)) caught = caught || (!p.ok && std::strcmp(p.id, f.caughtBy) == 0);
        std::snprintf(label, sizeof(label), "control: \"%s\" is caught by the %s pin", f.name, f.caughtBy);
        check(caught, label);
    }
}

void testWiring() {
    const std::string text = readText("src/d3d11/vscreen.cpp");
    check(!text.empty(), "src/d3d11/vscreen.cpp is readable from the working directory (intro curve wiring)");
    if (text.empty()) return;
    bool pristine = true;
    for (const WirePin& p : wiringPins(text)) {
        check(p.ok, p.what);
        pristine = pristine && p.ok;
    }
    check(pristine, "control: with no edit every intro curve wiring pin of vscreen.cpp holds (the controls below are the only way one fails)");
    testIntroCurveControls(text);
}

}  // namespace introcurve

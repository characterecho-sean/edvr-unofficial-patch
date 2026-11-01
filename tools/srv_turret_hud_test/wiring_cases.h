// The SRV turret view's wiring pins (included by srv_turret_hud_test.cpp; run with --self-test and alone with --wiring).
//
// The production objects are tested on WARP, but the calls that put them into the game live in other files, and a hook that is never called looks like a hook that did
// nothing. Each scan reads the source from the repo root (build.bat runs the rigs there) and asks for the order the design depends on:
//   * vscreen.cpp: the scope opens in forwardWithVerdict after the skipped-draw return and BEFORE the layer's decision and every issue (so the game's own issue, the layer's and
//     the interface depth's re-issues all run inside it), is begun for the owner's draws while the pass is wanted, matches BOTH shaders of the pair, and is a term of the draw
//     gate (the AA-off fast path skips forwardWithVerdict); the boundary tick and the shutdown are there.
//   * native_on_foot.cpp: the frame tick runs just before alignTick; alignTick adds the camera term AFTER the on-foot gate and before the change test, and tells the module what
//     it published right after the one publisher's call.
//   * srv_turret.cpp: the shaders are made from the build's bytecode through shaderSwapCreateGs (one for each signature, a missing one standing down only its own families); the
//     module changes no state of the game's but through the binding class; both parts act on the turret view AND the Scorpion latch (and say the view without it); triangle lists
//     and strips are carried; the six integer keys are read with the literal ranges and defaults the settings generator reads, the other two (the depth mode, default front, and the scissor, default
//     open) as text; a bound draw's scissor rectangles are opened right after the bind and put back at the end of the scope (and settled at the boundary), and a draw is refused while they are owed; a draw is counted and bound only when its family's group has a
//     push, the gap or its own forward key (HUD forward for the panels, radar forward for the radar) above 0 (the group's constants come from one place), the pass is wanted when any of
//     the five HUD keys is above 0, the anchor is stepped by the frame tick with the head from the host's pose and cleared when the pass is not wanted, each eye's frame is built from the
//     host's pose with the tangents AND the jitter shift, and the DRAW rebuilds both eyes' frames from the pose of the frame being drawn (the frame sequence the host publishes in
//     beginFrame, after the previous frame's boundary tick), once for each new sequence.
//   * journal_watch.cpp, native_temporal.cpp: the accessors; the Scorpion latch is fed the tail's new bytes once (not the stitched head the scan reads twice), reset with the
//     file, published to its accessor, and each change said once with its event.
//   * edvr.ini: all eight keys live in [fix] with their ui: lines and shipped defaults; the flat profile's allow-list names none (VR only).
//   * the radar anchor (fix.srv_turret_radar_anchor, srv_radar_anchor.cpp): the ninth key, its publish, the billboards it leaves to the game, the hook's pins and its integer-only
//     callback, its compile-list entry and its exact-build audit entry (wiredAnchor).
//   * the shader tables (tools/temporal_shader_build): the HLSL's header is included and the eight variants are registered as gs_5_0, each with its contract row.
//   * build.bat: the module is in the DLL's compile list and this rig has its label.
// Each pin has controls: an edited copy of the real text must trip it.
#pragma once

static bool found(size_t p) { return p != std::string::npos; }

static std::string without(std::string t, const std::string& needle) {
    const size_t p = t.find(needle);
    if (p != std::string::npos) t.erase(p, needle.size());
    return t;
}
static std::string replaced(std::string t, const std::string& from, const std::string& to) {
    const size_t p = t.find(from);
    if (p != std::string::npos) t.replace(p, from.size(), to);
    return t;
}

// vscreen.cpp, forwardWithVerdict.
static bool wiredVscreenScope(const std::string& t) {
    const size_t fn = t.find("void forwardWithVerdict(");
    if (!found(fn)) return false;
    const size_t skip = t.find("if (v == DrawVerdict::kSkip) {", fn);
    const size_t scope = t.find("SrvTurretHudScope turretHud(self);", fn);
    const size_t begin = t.find("if (owner && srvTurretHudWantsDraws()) turretHud.bound = srvTurretHudBeginDraw(g_state, self);", fn);
    const size_t layerFlag = t.find("bool uiLayer = false;", fn);
    const size_t layered = t.find("const bool layered = uiLayer && uiLayerBegin(self);", fn);
    const size_t issue = t.find("const bool originalIssued=observedDraw(", fn);
    const size_t secondIssues = t.find("if (originalIssued) uiLayerSecondIssues(", fn);
    const size_t depthReissue = t.find("if (uiDepthReissueBegin(self)) pureDrawReissue(", fn);
    if (!found(skip) || !found(scope) || !found(begin) || !found(layerFlag) || !found(layered) || !found(issue) || !found(secondIssues) || !found(depthReissue)) return false;
    // after the skipped draw's return, before the layer's decision and before every issue the function makes
    return skip < scope && scope < begin && begin < layerFlag && layerFlag < layered && layered < issue && issue < secondIssues && secondIssues < depthReissue;
}
static bool wiredVscreenBegin(const std::string& t) {
    const size_t fn = t.find("bool srvTurretHudBeginDraw(State* s, ID3D11DeviceContext* self) {");
    if (!found(fn)) return false;
    const size_t vs0 = t.find("if (!srv_turret::isFamilyVs(vs)) return false;", fn);
    const size_t family = t.find("srv_turret::familyOf(vs, bindingShaderHash(BindSlot::Ps))", fn);
    const size_t eyeLayer = t.find("uiLayerEyeOfGameTarget(info->resource)", fn);
    const size_t eyeTable = t.find("uiDepthEyeOfTargetReadOnly(info->resource)", fn);
    const size_t eyeReg = t.find("uiDepthEyeOfTarget(info->resource, info->a, info->b, info->fmt)", fn);
    const size_t call = t.find("const bool bound = srvTurretHudBegin(self, family, eye);", fn);
    const size_t declined = found(call) ? t.find("if (!bound) srvTurretHudNoteRefusedDraw(family);", call) : std::string::npos;
    return found(vs0) && found(family) && found(eyeLayer) && found(eyeTable) && found(eyeReg) && found(call) && found(declined) && vs0 < family && family < eyeLayer && eyeLayer < eyeTable &&
           eyeTable < eyeReg && eyeReg < call && declined - call < 160;
}
// The unmoved family draws (srv_turret_math.h UnmovedRoute): every route a draw of a family can reach the GPU without the pass's shader bound is counted where vscreen.cpp can see it -- the four
// direct draw thunks (the owner's fast path and any second context), the indirect and auto draws (no scope exists for them), pureDrawReissue outside a scope (with its call site), the game's
// ExecuteCommandList (the draws inside cannot be seen) and the scope's own decline (wiredVscreenBegin).
static size_t occurrences(const std::string& t, const std::string& needle) {
    size_t n = 0, at = 0;
    while (found(at = t.find(needle, at))) {
        ++n;
        at += needle.size();
    }
    return n;
}
static bool wiredVscreenUnmoved(const std::string& t) {
    const size_t reissueFn = t.find("__declspec(noinline) void pureDrawReissue(ID3D11DeviceContext* self, char kind, UINT count,");
    const size_t reissue = found(reissueFn) ? t.find("if (srvTurretHudWantsDraws()) srvTurretUnmovedReissue(self, _ReturnAddress());", reissueFn) : std::string::npos;
    const size_t reissueHelper = t.find("__declspec(noinline) void srvTurretUnmovedReissue(ID3D11DeviceContext* self, const void* site) {");
    const size_t scopeCheck = found(reissueHelper) ? t.find("if (self != g_state->ownerCtx || srvTurretHudScopeActive()) return;", reissueHelper) : std::string::npos;
    const size_t executeFn = t.find("void STDMETHODCALLTYPE hookedExecuteCommandList(ID3D11DeviceContext* self,");
    const size_t execute = found(executeFn) ? t.find("if (!privateExecution && srvTurretHudWantsDraws()) srvTurretHudNoteUnmoved(-1, srv_turret::UnmovedRoute::kCommandList, nullptr);", executeFn) : std::string::npos;
    const size_t note = t.find("__declspec(noinline) void srvTurretUnmovedNote(ID3D11DeviceContext* self, srv_turret::UnmovedRoute ownerRoute, const void* site) {");
    const size_t foreign = found(note) ? t.find("self->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED ? srv_turret::UnmovedRoute::kForeignDeferred : srv_turret::UnmovedRoute::kForeignImmediate", note) : std::string::npos;
    return found(reissue) && found(reissue) && found(scopeCheck) && found(execute) && found(foreign) && reissueHelper < reissueFn &&
           occurrences(t, "srvTurretUnmovedDirect(self, fast);") == 3 && occurrences(t, "srvTurretUnmovedDirect(self, false);") == 1 &&
           occurrences(t, "srvTurretUnmovedPlain(self, srv_turret::UnmovedRoute::kIndirect);") == 2 && occurrences(t, "srvTurretUnmovedPlain(self, srv_turret::UnmovedRoute::kAuto);") == 1 &&
           found(t.find("if (srvTurretHudWantsDraws() && (fast || self != g_state->ownerCtx)) srvTurretUnmovedNote(self, srv_turret::UnmovedRoute::kFastPath, nullptr);"));
}
static bool wiredVscreenGate(const std::string& t) {
    const size_t fn = t.find("bool drawGateSubscribed(State* s) {");
    if (!found(fn)) return false;
    // the term sits inside the OR chain, after the blend probe's; the chain's tail (introCurveWants();) is pinned by other rigs and is not touched
    const size_t term = t.find("srvTurretHudWantsDraws() ||", fn);
    const size_t end = t.find("\n}\n", fn);
    return found(term) && found(end) && term < end && t.find("blendProbeWantsDraws() ||", fn) < term && t.find("introCurveWants();", fn) > term;
}
static bool wiredVscreenBoundary(const std::string& t) {
    const size_t tick = t.find("tkSrvTurretHud.run([&] { srvTurretHudFrameBoundary(g_state->ownerCtx); });");
    const size_t decl = t.find("EDVR_BOUNDARY_TICK(tkSrvTurretHud, \"srv_turret_hud\");");
    const size_t gate = t.find("drawGateSet(", found(tick) ? tick : 0);
    const size_t down = t.find("    srvTurretShutdown();\n");
    const size_t barsDown = t.find("    supercruiseBarsShutdown();\n");
    return found(tick) && found(decl) && found(down) && found(barsDown) && down > barsDown && decl < tick && (!found(gate) || tick < gate) && found(t.find("#include \"srv_turret.h\""));
}

// native_on_foot.cpp.
static bool wiredNativeOnFoot(const std::string& t) {
    const size_t boundary = t.find("void nativeOnFootFrameBoundary(uint32_t frameNo) {");
    const size_t tick = found(boundary) ? t.find("srvTurretFrameTick(frameNo);", boundary) : std::string::npos;
    const size_t align = found(tick) ? t.find("alignTick(GetTickCount64());", tick) : std::string::npos;
    if (!found(tick) || !found(align) || align - tick > 400) return false;
    const size_t fn = t.find("void alignTick(uint64_t now) noexcept {");
    const size_t term = found(fn) ? t.find("const double turretBackZ = srvTurretBackSeatedZ();", fn) : std::string::npos;
    const size_t gate = found(term) ? t.find("eye[i] = nofoot::f115::gate(onFoot, eye[i]);", term) : std::string::npos;
    const size_t add = found(gate) ? t.find("if (i == 2) eye[i] += turretBackZ;", gate) : std::string::npos;
    const size_t change = found(add) ? t.find("eyeChange = std::max(eyeChange, std::fabs(eye[i] - g_eyeAlignPub[i]));", add) : std::string::npos;
    const size_t publish = found(change) ? t.find("f89AlignPublish(f);", change) : std::string::npos;
    const size_t note = found(publish) ? t.find("srvTurretNotePublished(turretBackZ, g_align.serial);", publish) : std::string::npos;
    return found(term) && found(gate) && found(add) && found(change) && found(publish) && found(note) && add - gate < 120 && note - publish < 160 &&
           found(t.find("#include \"srv_turret.h\""));
}

// srv_turret.cpp.
static bool wiredModule(const std::string& t) {
    const size_t made = t.find("g_gs[f] = shaderSwapCreateGs(ctx, kShader[f].bytecode, kShader[f].size, kShader[f].name, \"srv turret hud\")");
    const size_t begin = t.find("g_binding.begin(ctx, twin ? g_soGs[sig] : g_gs[sig], constants, st::directSetGs, needScene ? &scene : nullptr)");
    const size_t finish = t.find("const auto done = g_binding.finish(ctx);");
    const size_t back = t.find("cfg.getIntInRange(\"fix.srv_turret_camera_back\", 0, 0, 150)");
    const size_t forward = t.find("cfg.getIntInRange(\"fix.srv_turret_hud_forward\", 70, 0, 150)");
    const size_t radarForward = t.find("cfg.getIntInRange(\"fix.srv_turret_radar_forward\", 70, 0, 150)");
    const size_t depthKey = t.find("cfg.getString(\"fix.srv_turret_hud_depth\", \"front\")");
    const size_t scissorKey = t.find("cfg.getString(\"fix.srv_turret_hud_scissor\", \"open\")");
    const size_t panels = t.find("cfg.getIntInRange(\"fix.srv_turret_panels_back\", 120, 0, 200)");
    const size_t radar = t.find("cfg.getIntInRange(\"fix.srv_turret_radar_back\", 0, 0, 200)");
    const size_t gap = t.find("cfg.getIntInRange(\"fix.srv_turret_hud_gap\", 0, 0, 200)");
    const size_t settings = t.find("\"srv turret settings: camera back %d cm, hud forward %d cm, radar forward %d cm, panels back %d cm, radar back %d cm, hud gap %d cm, hud depth %s, hud scissor %s, radar anchor %s (live; \"");
    const size_t edge = t.find("\"srv turret view: %s at frame %u (Status.json Flags 0x%08X, seat kind 0x%X)\"");
    const size_t camera = t.find("\"srv turret camera: back %.2f m published (serial %u)\"");
    const size_t cleared = t.find("\"srv turret camera: back cleared (serial %u)\"");
    const size_t line = t.find("\"srv turret hud: panels: %s; radar: %s; anchor=(%.3f,%.3f,%.3f) head_offset=%.1f cm T=(%.3f,%.3f,%.3f) Tradar=(%.3f,%.3f,%.3f) ipd_elite=%.1fmm P00[L,R]=%.3f,%.3f \"");
    const size_t lineTail = found(line) ? t.find("\"draws[L,R]=%u,%u bound=%u scissor opened %u pose refreshed at draw: %u of %u frames unmoved family draws: %s \"", line) : std::string::npos;
    // each group's constants come from one place (the push when above 0, else the pull, else the translation alone, else none), read by the draw, the 10 s line and nothing else; a draw whose
    // group is off is the game's own BEFORE it is counted, and the constants are built on the eye's published frame after it was read; the first-draw line names the direction of the move
    const size_t family = t.find("bool paramsOfGroup(st::Group group, const st::EyeFrame& frame, st::TurretParams& out) {");
    const size_t select = found(family) ? t.find("if (!st::paramsForGroup(frame, group, st::pushCmOf(group, g_panelsCm.load(std::memory_order_relaxed), g_radarCm.load(std::memory_order_relaxed)), g_gapCm.load(std::memory_order_relaxed),", family) : std::string::npos;
    // ...and the forward translation is the group's own key's: HUD forward for the panels, radar forward for the radar; the depth mode of the key is written into whatever was made
    const size_t ownForward = found(select) ? t.find("st::forwardCmOf(group, g_forwardCm.load(std::memory_order_relaxed), g_radarForwardCm.load(std::memory_order_relaxed)), out))", select) : std::string::npos;
    const size_t depthMode = found(ownForward) ? t.find("out = st::withDepth(out, static_cast<st::DepthMode>(g_depthMode.load(std::memory_order_relaxed)));", ownForward) : std::string::npos;
    const size_t drawFn = t.find("bool srvTurretHudBegin(ID3D11DeviceContext* ctx, int family, int eye) {");
    const size_t groupOff = found(drawFn) ? t.find("if (!paramsOfGroup(group, st::EyeFrame{}, unused)) return false;", drawFn) : std::string::npos;
    const size_t counted = found(groupOff) ? t.find("g_familyDraws[family].fetch_add(1, std::memory_order_relaxed);", groupOff) : std::string::npos;
    const size_t eyeRead = found(counted) ? t.find("if (!drawEyeFrame(eye, frame)) {", counted) : std::string::npos;
    const size_t built = found(eyeRead) ? t.find("if (!paramsOfGroup(group, frame, params)) return false;", eyeRead) : std::string::npos;
    const size_t uploaded = found(built) ? t.find("g_constants[eye].get(ctx, params)", built) : std::string::npos;
    // the Scorpion's hologram (signature H) needs the draw's own vertex constants at GS slot 1, read before the bind and handed to it; a draw without them is refused, counted
    const size_t sceneNeed = found(uploaded) ? t.find("const bool needScene = sig == st::kSceneSignature;", uploaded) : std::string::npos;
    const size_t sceneRead = found(sceneNeed) ? t.find("if (needScene && !st::readGameCb(ctx, st::kGameCbSlot, scene)) {", sceneNeed) : std::string::npos;
    const size_t sceneBind = found(sceneRead) ? t.find("g_binding.begin(ctx, twin ? g_soGs[sig] : g_gs[sig], constants, st::directSetGs, needScene ? &scene : nullptr)", sceneRead) : std::string::npos;
    const bool sceneOk = found(sceneNeed) && found(sceneRead) && found(sceneBind) && sceneRead - sceneNeed < 200;
    // the game's scissor rectangles: a draw is refused while they are owed back, they are opened after the bind (the key's value is whether it applies) and restored at the end of the scope, after the
    // geometry stage and the queries, and settled at the frame boundary
    const size_t owedRefuse = found(uploaded) ? t.find("if (g_binding.needsRestore() || g_scissor.owed() || !ensureGs(ctx, sig)) {") : std::string::npos;
    const size_t scissorOpen = found(sceneBind) ? t.find("const auto opened = g_scissor.open(ctx, seen, apply);", sceneBind) : std::string::npos;
    const size_t endFn2 = t.find("void srvTurretHudEnd(ID3D11DeviceContext* ctx) {");
    const size_t endFinish = found(endFn2) ? t.find("const auto done = g_binding.finish(ctx);", endFn2) : std::string::npos;
    const size_t scissorRestore = found(endFinish) ? t.find("if (!g_scissor.restore(ctx) && !g_scissorRestoreNoted) {", endFinish) : std::string::npos;
    const size_t boundaryFn = t.find("void srvTurretHudFrameBoundary(ID3D11DeviceContext* ctx) {");
    const size_t scissorSettle = found(boundaryFn) ? t.find("if (g_scissor.owed() && ctx) (void)g_scissor.settle(ctx);", boundaryFn) : std::string::npos;
    const bool scissorOk = found(owedRefuse) && found(scissorOpen) && found(scissorRestore) && found(scissorSettle) && found(scissorKey) &&
                           found(t.find("st::Scissor g_scissor;")) && found(t.find("g_scissorOpened.fetch_add(1, std::memory_order_relaxed);"));
    const size_t windowLn = t.find("groupWindow(panels, st::Group::kPanels);");
    const bool groups = found(family) && found(select) && found(ownForward) && ownForward - select < 300 && found(depthMode) && depthMode - ownForward < 300 && found(groupOff) && found(counted) && found(eyeRead) && found(built) && found(uploaded) && found(windowLn) && select - family < 300 &&
                        found(t.find("\"moves %.2f m deeper\"")) && found(t.find("\"moves %.2f m nearer\"")) && found(t.find("\"does not move radially\"")) && found(t.find("groupWindow(radar, st::Group::kRadar);")) &&
                        !found(t.find("st::paramsFor(geo"));
    // nothing of the game's state is changed but through the binding class
    const bool rawSet = found(t.find("GSSetShader(")) || found(t.find("GSSetConstantBuffers(")) || found(t.find("UpdateSubresource("));
    // the geometry comes from the host's own head and eye matrices and frustum with the tangent shift the jitter adds (the game projects with the sum), the anchor is the tracker's, and a
    // draw with none is refused, counted. Both eyes' frames are built by ONE function, from a pose and an anchor, called by the tick and by the draw
    const size_t buildFn = t.find("void buildEyeFrames(const float head[12], const float eyeToHead[2][12], bool haveAnchor, const double anchor[3], int forwardCm, int radarForwardCm, st::EyeFrame frames[2], bool ok[2]) {");
    const size_t geometry1 = found(buildFn) ? t.find("nativeTemporalEyeGeometry(e, &sequence, tan, shift)", buildFn) : std::string::npos;
    const size_t summed = found(geometry1) ? t.find("const float tangents[4] = {tan[0] + shift[0], tan[1] + shift[0], tan[2] + shift[1], tan[3] + shift[1]};", geometry1) : std::string::npos;
    const size_t framed = found(summed) ? t.find("st::eyeFrameFrom(head, eyeToHead[e], anchor, static_cast<double>(forwardCm) / 100.0, static_cast<double>(radarForwardCm) / 100.0, tangents, frames[e])", summed) : std::string::npos;
    const size_t cacheFn = t.find("void cacheGeometry(bool on, int backCm, int forwardCm, int radarForwardCm) {");
    const size_t pose = found(cacheFn) ? t.find("nativeTemporalHeadPose(&poseSequence, head, eyeToHead)", cacheFn) : std::string::npos;
    const size_t step = found(pose) ? t.find("g_anchor.step(headAt, on, backCm, forwardCm, radarForwardCm)", pose) : std::string::npos;
    const size_t said = found(step) ? t.find("\"srv turret anchor: latched at %.3f,%.3f,%.3f m seated (%s)\"", step) : std::string::npos;
    const size_t tickBuilt = found(said) ? t.find("buildEyeFrames(head, eyeToHead, haveAnchor, g_anchor.anchor(), forwardCm, radarForwardCm, frames, ok);", said) : std::string::npos;
    // the draw: the frame sequence the host published when it took the frame being drawn is compared with the one the eye frames were built from, and the frames are rebuilt from that
    // frame's pose for a new one (the tick runs at the previous boundary, before beginFrame, so its frames are a frame old); the anchor is the tick's, read from atomics
    const size_t eyeFrameFn = t.find("bool drawEyeFrame(int eye, st::EyeFrame& out) {");
    const size_t seqRead = found(eyeFrameFn) ? t.find("const uint64_t seq = nativeTemporalFrameSequence();", eyeFrameFn) : std::string::npos;
    const size_t onceFor = found(seqRead) ? t.find("if (seq != 0 && seq != g_drawSeq) {", seqRead) : std::string::npos;
    const size_t drawPose = found(onceFor) ? t.find("nativeTemporalHeadPose(&poseSequence, head, eyeToHead)", onceFor) : std::string::npos;
    const size_t drawBuilt = found(drawPose) ? t.find("buildEyeFrames(head, eyeToHead, true, anchor, g_forwardCm.load(std::memory_order_relaxed), g_radarForwardCm.load(std::memory_order_relaxed), g_drawFrames, g_drawOk);", drawPose) : std::string::npos;
    const size_t counted2 = found(onceFor) ? t.find("g_poseRefreshed.fetch_add(1, std::memory_order_relaxed);", onceFor) : std::string::npos;
    const bool geometry = found(buildFn) && found(geometry1) && found(summed) && found(framed) && found(cacheFn) && found(pose) && found(step) && found(said) && found(tickBuilt) &&
                          found(eyeFrameFn) && found(seqRead) && found(onceFor) && found(drawPose) && found(drawBuilt) && found(counted2) && found(t.find("refuse(st::Refuse::kNoGeometry)"));
    // the Scorpion only: both parts act on the turret view AND the latch (`on` is what g_backSeatedZ, the gate and the view edges are made from), and the view without it is said
    const size_t tick = t.find("void srvTurretFrameTick(uint32_t frameNo) {");
    const size_t turret = found(tick) ? t.find("const bool turret = journalSrvTurretView();", tick) : std::string::npos;
    const size_t on = found(turret) ? t.find("const bool on = turret && journalSrvScorpion();", turret) : std::string::npos;
    const size_t backZ = found(on) ? t.find("g_backSeatedZ = st::backSeatedZ(on, backCm);", on) : std::string::npos;
    const size_t wanted = found(backZ) ? t.find("const bool wanted = on && (panelsCm > 0 || radarCm > 0 || forwardCm > 0 || radarForwardCm > 0 || gapCm > 0);", backZ) : std::string::npos;
    const size_t cached = found(wanted) ? t.find("cacheGeometry(on, backCm, forwardCm, radarForwardCm);", wanted) : std::string::npos;
    const size_t cleared1 = found(cached) ? t.find("g_anchor.reset();", cached) : std::string::npos;
    const size_t ignored = found(wanted) ? t.find("\"srv turret view: not the Scorpion at frame %u (Status.json Flags 0x%08X, seat kind 0x%X): the turret view is on in an SRV the journal does not call the Scorpion, so the \"", on) : std::string::npos;
    const bool scorpion = found(turret) && found(on) && found(backZ) && found(wanted) && found(cached) && found(cleared1) && found(ignored);
    // lists and strips are carried, one signature's shader missing leaves the others' families bound
    const bool strips = found(t.find("!st::topologyCarried(static_cast<int>(topology))")) && found(t.find("bool ensureGs(ID3D11DeviceContext* ctx, uint32_t sig) {")) &&
                        found(t.find("return sig < kSigN && g_gs[sig] != nullptr;")) && found(t.find("const uint32_t sig = st::kFamilies[family].sig;"));
    return found(made) && found(begin) && found(finish) && found(back) && found(forward) && found(radarForward) && found(depthKey) && sceneOk && scissorOk && found(panels) && found(radar) && found(gap) && found(settings) && found(edge) &&
           found(camera) && found(cleared) && found(line) && found(lineTail) && !rawSet && geometry && scorpion && groups && strips && found(t.find("#include \"temporal_shader_bytecode.h\""));
}
// journal_watch.cpp / .h: the Scorpion latch is fed every byte the tail reads, once (not from the stitched head scanEvents also scans), reset with the file, published to the
// accessor, and each change is said once with the event that caused it.
static bool wiredLatch(const std::string& journal, const std::string& journalH) {
    const size_t scan = journal.find("void scanEvents(Session& s, const char* data, uint32_t len) {");
    const size_t ranges = found(scan) ? journal.find("scanRange(s, data, len);", scan) : std::string::npos;
    const size_t feed = found(ranges) ? journal.find("s.vehicle.feed(data, len, [&s](bool scorpion, journal_vehicle::Event cause) { noteVehicle(s, scorpion, cause); });", ranges) : std::string::npos;
    const size_t carry = found(feed) ? journal.find("memcpy(s.carry, data + (len - keep), keep);", feed) : std::string::npos;
    const size_t close = journal.find("void closeFile(Session& s) {");
    const size_t reset = found(close) ? journal.find("if (s.vehicle.reset()) noteVehicle(s, false, journal_vehicle::Event::kNewJournal);", close) : std::string::npos;
    const size_t note = journal.find("void noteVehicle(Session& s, bool scorpion, journal_vehicle::Event cause) {");
    const size_t say = found(note) ? journal.find("Log::get().note(\"srv turret: the vehicle is %s (event %s)\", scorpion ? \"the Scorpion\" : \"not the Scorpion\", journal_vehicle::eventName(cause));", note) : std::string::npos;
    const size_t publish = found(note) ? journal.find("s.pub.srvScorpion = scorpion;", note) : std::string::npos;
    const bool applied = found(journal.find("g_v.srvScorpion.store(p.srvScorpion, kRelaxed);"));
    const bool accessor = found(journal.find("bool journalSrvScorpion() { return !runtimeFlatProfile() && peek(g_v.srvScorpion); }")) && found(journalH.find("bool journalSrvScorpion();"));
    // the latch reads the bytes themselves: no line, no value is copied anywhere else (the one include of the pure header; the six names only live in it)
    const bool include = found(journal.find("#include \"journal_vehicle_latch.h\""));
    // each byte once: the stitched head (the carry and the next read's first bytes, which scanEvents scans again) is never fed
    const bool once = !found(journal.find("s.vehicle.feed(stitched"));
    return found(ranges) && found(feed) && found(carry) && ranges < feed && feed < carry && found(close) && found(reset) && found(note) && found(say) && found(publish) && publish < say &&
           applied && accessor && include && once;
}
// native_temporal.cpp: the frame sequence is a lock-free read published at the END of begin(), after the head, eyes, frusta and shift of that frame are copied, and cleared with the rest.
static bool wiredFrameSequence(const std::string& temporal) {
    const size_t copied = temporal.find("std::memcpy(s->head,f->head,sizeof(s->head));");
    const size_t stored = found(copied) ? temporal.find("g_frameSequence.store(f->sequence,std::memory_order_release);", copied) : std::string::npos;
    const size_t shifted = found(copied) ? temporal.find("edvr::lightGridCacheNoteJitter(s->frameCounter,jx,jy,s->shift);", copied) : std::string::npos;
    size_t cleared = 0, at = 0;
    while (found(at = temporal.find("g_recommended.store(0,std::memory_order_release);g_frameSequence.store(0,std::memory_order_release);", at))) {
        ++cleared;
        ++at;
    }
    return found(copied) && found(shifted) && found(stored) && shifted < stored && found(temporal.find("uint64_t nativeTemporalFrameSequence() { return g_frameSequence.load(std::memory_order_acquire); }")) &&
           cleared == 2;   // cleared where the rest is: invalidate and close
}
static bool wiredAccessors(const std::string& journal, const std::string& temporal, const std::string& journalH) {
    return found(journal.find("bool journalSrvTurretView() { return !runtimeFlatProfile() && srv_turret::turretViewFromFlags(peek(g_v.flags)); }")) &&
           found(journalH.find("bool journalSrvTurretView();")) && found(temporal.find("bool nativeTemporalEyeSeparation(float* metres) {")) &&
           found(temporal.find("eyeSeparationOf(current->eyes[0], current->eyes[1])")) && found(temporal.find("bool nativeTemporalHeadPose(uint64_t* sequence, float head[12], float eyeToHead[2][12]) {")) &&
           found(temporal.find("if (head) std::memcpy(head, s.head, sizeof(s.head));")) && found(temporal.find("if (eyeToHead) std::memcpy(eyeToHead, s.eyes, sizeof(s.eyes));")) && wiredFrameSequence(temporal);
}

// The GPU instrument: the issues are counted at the game's own issue and at the re-issues; the queries begin right after the bind and end right before the restore; the
// results are read once a frame at the boundary; the lines are the ones tools/edvr_log.py --health reads.
static bool wiredInstrument(const std::string& vscreen, const std::string& t) {
    const size_t reissue = vscreen.find("void pureDrawReissue(ID3D11DeviceContext* self, char kind, UINT count,");
    const size_t reissueCount = found(reissue) ? vscreen.find("srvTurretHudCountIssue(self);", reissue) : std::string::npos;
    const size_t reissueSwitch = found(reissue) ? vscreen.find("switch(kind) {", reissue) : std::string::npos;
    // the game's own issue is counted in the four draw thunks' lambdas (after the real call), not in observedDraw, whose text other rigs pin
    size_t counts = 0, at = 0;
    while (found(at = vscreen.find("srvTurretHudCountIssue(self);", at))) {
        ++counts;
        ++at;
    }
    const size_t observed = vscreen.find("auto observedDraw = [&](AlteredDraw altered) {");
    const size_t observedEnd = found(observed) ? vscreen.find("return issued;", observed) : std::string::npos;
    if (!found(reissueCount) || !found(reissueSwitch) || reissueCount > reissueSwitch || counts != 5 || !found(observed) || !found(observedEnd) ||
        found(vscreen.substr(observed, observedEnd - observed).find("srvTurretHudCountIssue"))) return false;
    // the radar disc's extras (an occlusion query around the sampled scope, two pixel-shader registers copied at its end, one line per sample) and each family's first scope saying its state
    // at every issue it makes
    const size_t endFn = t.find("void srvTurretHudEnd(ID3D11DeviceContext* ctx) {");
    const size_t noteEnd = found(endFn) ? t.find("noteEnd();", endFn) : std::string::npos;
    const size_t discBegin = t.find("ctx->Begin(p.occlusion);");
    const size_t discEnd = t.find("if (p.discArmed) ctx->End(p.occlusion);");
    const size_t discCopy = found(discEnd) ? t.find("p.discCopied[0] = probeCopyRegister(ctx, p.cbStage, kDiscAlphaSlot, kDiscAlphaRow, 0);", discEnd) : std::string::npos;
    const bool disc = found(discBegin) && found(discEnd) && found(discCopy) && found(t.find("D3D11_QUERY_OCCLUSION")) && found(t.find("kDiscAlphaSlot = 2, kDiscAlphaRow = 11, kDiscIntensitySlot = 1, kDiscIntensityRow = 90")) &&
                      found(t.find("\"srv turret hud disc: family=%s eye=%d draw=%u issues=%u samples_passed=%llu alpha=%s intensity=%s (cb2[11].w, cb1[90].y)\"")) &&
                      found(t.find("if (family == static_cast<int>(kDiscFamily)) probeReadDisc(ctx, eye);")) && found(t.find("return st::copyPsRegister(ctx, stage, slot, row, to, rawCopyRegion);"));
    // the radar disc's plane in the seated frame: eye 0's pose taken at the sampled draw, each quad of the capture read back from its PRE-move clip positions, one line apiece
    const size_t planePose = t.find("st::eyePoseOf(head, eyeToHead[0], p.poseR, p.poseE)");
    const size_t planeCall = t.find("if (static_cast<uint32_t>(family) == kDiscFamily && eye == 0 && p.planePose) logDiscPlanes(static_cast<const float*>(m.pData), vertices, fam, p);");
    const size_t planeUnmap = found(planeCall) ? t.find("ctx->Unmap(p.staging, 0);", planeCall) : std::string::npos;
    const bool planes = found(planePose) && found(planeCall) && found(planeUnmap) && planeUnmap - planeCall < 200 &&
                        found(t.find("st::quadInSeated(records, fam.strideBytes / 4, fam.preFloat, 6 * k, vertices, tangents, p.poseR, p.poseE, q)")) &&
                        found(t.find("\"srv turret hud disc plane: inst=%u centre=(%.3f,%.3f,%.3f) normal=(%.3f,%.3f,%.3f) head=(%.3f,%.3f,%.3f)\""));
    // the unmoved family draws: counted per family and route, listed in the 10 s line, the first of each route said once
    const bool unmoved = found(t.find("void srvTurretHudNoteUnmoved(int family, st::UnmovedRoute route, const void* site) {")) && found(t.find("st::unmovedList(unmoved, sizeof(unmoved), unmovedCounts);")) &&
                         found(t.find("\"srv turret hud: first unmoved family draw by route %s (family %s): %s%s\"")) && found(t.find("void srvTurretHudNoteRefusedDraw(int family) {")) &&
                         found(t.find("bool srvTurretHudScopeActive() { return g_binding.needsRestore(); }"));
    const bool notes = planes && unmoved && found(t.find("void srvTurretHudNoteIssue(ID3D11DeviceContext* ctx) {")) && found(noteEnd) && found(t.find("if (!g_stateNoted[family]) noteBegin(ctx, family, eye);")) &&
                       found(t.find("\"srv turret hud state: family=%s eye=%d issue=%u target=%s %ux%u format=%u depth_view=%s | depth: enable=%d func=%d write=%d | stencil: enable=%d ref=%u read=0x%02X \"")) &&
                       found(t.find("| raster: scissor=%d rects=%u first=[%ld,%ld,%ld,%ld] \"")) && found(t.find("\"depth_clip=%d cull=%d front_ccw=%d | viewport: x=%.0f y=%.0f w=%.0f h=%.0f\""));
    if (!disc || !notes) return false;
    const size_t begin = t.find("g_binding.begin(ctx, twin ? g_soGs[sig] : g_gs[sig], constants, st::directSetGs, needScene ? &scene : nullptr)");
    const size_t probeBegin = found(begin) ? t.find("if (sample) probeBegin(ctx, eye, family, ordinal, twin, params);", begin) : std::string::npos;
    const size_t end = t.find("void srvTurretHudEnd(ID3D11DeviceContext* ctx) {");
    const size_t probeEnd = found(end) ? t.find("probeEnd(ctx);", end) : std::string::npos;
    const size_t finish = found(probeEnd) ? t.find("const auto done = g_binding.finish(ctx);", probeEnd) : std::string::npos;
    const size_t probeFinish = found(finish) ? t.find("probeFinish(ctx);", finish) : std::string::npos;
    const size_t boundary = t.find("void srvTurretHudFrameBoundary(ID3D11DeviceContext* ctx) {");
    const size_t read = found(boundary) ? t.find("probeRead(ctx, eye, family);", boundary) : std::string::npos;
    const bool queries = found(t.find("D3D11_QUERY_PIPELINE_STATISTICS")) && found(t.find("D3D11_QUERY_SO_STATISTICS")) && found(t.find("D3D11_ASYNC_GETDATA_DONOTFLUSH")) &&
                         found(t.find("D3D11_MAP_FLAG_DO_NOT_WAIT")) && found(t.find("kProbeGiveUpFrames = 8,")) && found(t.find("CreateGeometryShaderWithStreamOutput(kShader[f].bytecode"));
    const bool lines = found(t.find("\"srv turret hud stats: eye=%d issues=%u ia_prims=%llu vs=%llu gs=%llu gs_prims=%llu c_inv=%llu c_prims=%llu ps=%llu (lost %u) draw=%u family=%s\"")) &&
                       found(t.find("\"srv turret hud capture: family=%s eye=%d draw=%u so_prims=%llu needed=%llu vertices=%u in_window=%u w=[%.3f,%.3f] ndc_x=[%.3f,%.3f] \""));
    return found(probeBegin) && found(end) && found(probeEnd) && found(finish) && found(probeFinish) && probeEnd < finish && finish < probeFinish && found(read) && queries && lines;
}

// edvr.ini and the flat allow-list.
static bool wiredKeys(const std::string& ini, const std::string& flatKeys) {
    const size_t fix = ini.find("\n[fix]\n"), visual = ini.find("\n[visual]\n");
    const size_t back = ini.find("\nsrv_turret_camera_back = 0\n"), forward = ini.find("\nsrv_turret_hud_forward = 70\n"), radarForward = ini.find("\nsrv_turret_radar_forward = 70\n"),
                 panels = ini.find("\nsrv_turret_panels_back = 120\n"), radar = ini.find("\nsrv_turret_radar_back = 0\n"), gap = ini.find("\nsrv_turret_hud_gap = 0\n"),
                 depth = ini.find("\nsrv_turret_hud_depth = front\n"), scissor = ini.find("\nsrv_turret_hud_scissor = open\n");
    if (!found(fix) || !found(visual) || !found(back) || !found(forward) || !found(radarForward) || !found(panels) || !found(radar) || !found(gap) || !found(depth) || !found(scissor)) return false;
    const bool inFix = back > fix && back < visual && forward > fix && forward < visual && radarForward > fix && radarForward < visual && panels > fix && panels < visual && radar > fix && radar < visual &&
                       gap > fix && gap < visual && depth > fix && depth < visual && scissor > fix && scissor < visual;
    const size_t backUi = ini.rfind("# ui: SRV turret camera back | range 0..150 | step 5 | unit cm | zero exact | recommended 0 | live | menu", back),
                 forwardUi = ini.rfind("# ui: SRV turret HUD forward | range 0..150 | step 5 | unit cm | zero exact | recommended 70 | live | menu", forward),
                 radarForwardUi = ini.rfind("# ui: SRV turret radar forward | range 0..150 | step 5 | unit cm | zero exact | recommended 70 | live | menu", radarForward),
                 panelsUi = ini.rfind("# ui: SRV turret panels back | range 0..200 | step 5 | unit cm | zero exact | recommended 120 | live | menu", panels),
                 radarUi = ini.rfind("# ui: SRV turret radar back | range 0..200 | step 5 | unit cm | zero exact | recommended 0 | live | menu", radar),
                 gapUi = ini.rfind("# ui: SRV turret HUD gap | range 0..200 | step 2 | unit cm | zero exact | recommended 0 | live | menu", gap),
                 depthUi = ini.rfind("# ui: SRV turret HUD depth | choices front, game, real | recommended front | live | menu", depth),
                 scissorUi = ini.rfind("# ui: SRV turret HUD screen limit | choices open, game | recommended open | live | menu", scissor);
    const bool ui = found(backUi) && back - backUi < 200 && found(forwardUi) && forward - forwardUi < 200 && found(radarForwardUi) && radarForward - radarForwardUi < 200 && found(panelsUi) && panels - panelsUi < 200 && found(radarUi) && radar - radarUi < 200 &&
                    found(gapUi) && gap - gapUi < 200 && found(depthUi) && depth - depthUi < 200 && found(scissorUi) && scissor - scissorUi < 200;
    // VR only: the flat profile's allow-list reads none of the keys
    const bool flatClean = !found(flatKeys.find("srv_turret"));
    return inFix && ui && flatClean;
}

// the shader tables and build.bat.
static bool wiredTables(const std::string& shaderBuild, const std::string& variants) {
    bool all = true;
    for (const char letter : std::string("ABCDEFGH")) {
        const std::string row = std::string("{\"kSrvTurretHudGs") + letter + "Bytecode\",\"srv turret hud pass-through " + letter + "\",\"main\",nullptr,{},false,edvr::kSrvTurretHudGs" + letter +
                                ",\"gs_5_0\"}";
        const std::string contract = std::string("{\"kSrvTurretHudGs") + letter + "Bytecode\", \"srv turret hud pass-through " + letter + "\", edvr::kSrvTurretHudGs" + letter + "}";
        all = all && found(variants.find(row)) && found(shaderBuild.find(contract));
    }
    return all && found(shaderBuild.find("#include \"fixed_srv_turret_variants.h\"")) && found(shaderBuild.find("const auto srvTurret = srvTurretHudVariants();")) &&
           found(shaderBuild.find("variants.insert(variants.end(), srvTurret.begin(), srvTurret.end());")) && found(shaderBuild.find("const TurretContract contracts[stm::kSignatureCount] = {")) &&
           found(variants.find("#include \"../../src/d3d11/srv_turret_hud_shader.h\""));
}
// The radar anchor moved in the game (fix.srv_turret_radar_anchor; srv_radar_anchor.cpp). srv_turret.cpp: the key read with its literal default and said on the settings line; the frame
// tick publishes the offset made from `on` (and from false in flat); a 5559BD94 draw the game's anchor carries is the game's own BEFORE it is counted, and its decline is group-off; the
// draw path's switch needs the hook armed, the key not off and the placement holding our offset; the shutdown shuts the gates. The hooks: build 334925's identity, both prologues and
// the vtables; the setter's callback ADDS the offset through the pure rule (srv_turret_math.h radarPlacementMove) and hands the original the sum, and no code writes the placement
// fields itself (the first build's replace dropped the radar 1.96 m); the update's callback counts only and does integer work only before the original runs (XMM1 is the game's dt).
// edvr.ini ships the key in [fix] with its menu row; build.bat compiles the file; the address audit lists it as exact-build only.
static bool wiredAnchor(const std::string& module, const std::string& hook, const std::string& ini, const std::string& build, const std::string& audit) {
    const size_t npos = std::string::npos;
    const size_t key = module.find("cfg.getString(\"fix.srv_turret_radar_anchor\", \"on\")");
    const size_t parsed = found(key) ? module.find("st::radarAnchorModeOf(anchorText.c_str())", key) : npos;
    const size_t tick = module.find("void srvTurretFrameTick(uint32_t frameNo) {");
    const size_t flat = found(tick) ? module.find("anchorTick(false, frameNo, 0);", tick) : npos;
    const size_t onAt = found(tick) ? module.find("const bool on = turret && journalSrvScorpion();", tick) : npos;
    const size_t ticked = found(onAt) ? module.find("anchorTick(on, frameNo, radarForwardCm);", onAt) : npos;
    const size_t made = module.find("w.offset = st::radarAnchorOffset(on, mode, radarForwardCm);");
    const size_t published = found(made) ? module.find("srvRadarAnchorPublish(w.offset.bits, frameNo);", made) : npos;
    const size_t drawFn = module.find("bool srvTurretHudBegin(ID3D11DeviceContext* ctx, int family, int eye) {");
    const size_t groupOff = found(drawFn) ? module.find("if (!paramsOfGroup(group, st::EyeFrame{}, unused)) return false;", drawFn) : npos;
    const size_t leaves = found(groupOff) ? module.find("if (anchorLeaves(family)) return radarCorrectionBegin(ctx, family, eye);", groupOff) : npos;
    const size_t counted = found(groupOff) ? module.find("g_familyDraws[family].fetch_add(1, std::memory_order_relaxed);", groupOff) : npos;
    const size_t refusedFn = module.find("void srvTurretHudNoteRefusedDraw(int family) {");
    const size_t refusedOff = found(refusedFn) ? module.find("&& !anchorLeaves(family);", refusedFn) : npos;
    const size_t yield = module.find("g_anchorYield.store(on && mode != st::RadarAnchorMode::kOff && a.state == RadarAnchorHookState::Armed && a.holdsOurs, std::memory_order_relaxed);");
    const size_t downFn = module.find("void srvTurretShutdown() {");
    const size_t down = found(downFn) ? module.find("srvRadarAnchorShutdown();", downFn) : npos;
    const bool moduleOk = found(parsed) && found(flat) && found(onAt) && flat < onAt && found(ticked) && found(published) && found(leaves) && found(counted) && leaves < counted &&
                          found(refusedOff) && refusedOff - refusedFn < 300 && found(yield) && found(down) && found(module.find("bool anchorLeaves(int family) { return st::radarAnchorLeavesFamily(family) && g_anchorYield.load("));
    // the update counter: integer work only, and nothing written
    const size_t fn = hook.find("uintptr_t __fastcall updateHooked(uintptr_t obj, uintptr_t b, uintptr_t c, uintptr_t d) noexcept {");
    const size_t forward = found(fn) ? hook.find("return forward(obj, b, c, d);", fn) : npos;
    const std::string body = found(fn) && found(forward) ? hook.substr(fn, forward - fn) : std::string();
    const bool integerOnly = !body.empty() && !found(body.find("float")) && !found(body.find("double")) && !found(body.find("memcpy")) && !found(body.find("Log::")) && !found(body.find("printf")) &&
                             !found(body.find("volatile uint32_t*")) && found(body.find("if (radar) s->updates.fetch_add(1, std::memory_order_relaxed);"));
    // the setter: the pure rule's sum to the original, the game's P for everything else; no store into the placement fields anywhere in the file
    const size_t setterFn = hook.find("uintptr_t __fastcall setterHooked(uintptr_t iface, uintptr_t p, uintptr_t r8, uintptr_t r9) noexcept {");
    const size_t rule = found(setterFn) ? hook.find("const st::PlacementMove r = st::radarPlacementMove(P, d, m, Q);", setterFn) : npos;
    const size_t sum = found(rule) ? hook.find("if (r == st::PlacementMove::kAdded) {", rule) : npos;
    const size_t handed = found(sum) ? hook.find("return forward(iface, reinterpret_cast<uintptr_t>(Q), r8, r9);", sum) : npos;
    const bool adds = found(rule) && found(handed) && hook.find("return", sum) == handed && !found(hook.find("0x15B0")) && !found(hook.find("0x15F0")) && !found(hook.find("volatile uint32_t*"));
    const bool pinned = found(hook.find("constexpr uintptr_t kSetterRva = 0x38563A0;")) && found(hook.find("constexpr uintptr_t kUpdateRva = 0x384A590;")) &&
                        found(hook.find("constexpr uintptr_t kObjectVtableRva = 0x5511DE8;")) && found(hook.find("constexpr uintptr_t kInterfaceVtableRva = 0x5511F18;")) &&
                        found(hook.find("constexpr uint32_t kStamp = 0x6ADA8048u, kImageSize = 0x640C000u;")) && found(hook.find("if (stamp != kStamp || size != kImageSize) {")) &&
                        found(hook.find("{0x48, 0x83, 0xEC, 0x48, 0x0F, 0x10, 0x02, 0x0F, 0x11, 0x81, 0x28, 0x15, 0x00, 0x00, 0x0F, 0x10, 0x4A, 0x10}")) &&
                        found(hook.find("{0x40, 0x53, 0x56, 0x48, 0x81, 0xEC, 0x68, 0x01, 0x00, 0x00, 0x0F, 0x29, 0xBC, 0x24, 0x30, 0x01}")) &&
                        found(hook.find("if (std::memcmp(got, prologue, n) != 0) return")) &&
                        found(hook.find("*reinterpret_cast<const uintptr_t*>(iface) != s.interfaceVtable || *reinterpret_cast<const uintptr_t*>(iface - kInterfaceAt) != s.objectVtable"));
    // the direction: eye 0's draw copies the live camera's rows 277..279 with eye 0's pose of the same frame, the boundary turns them into u and publishes it; the setter adds s * F along u in
    // the anchor's axes (the anchor's getter called only when it lies in the game's image), before the add-and-idempotence rule
    const size_t sampled = found(drawFn) && found(groupOff) ? module.find("if (eye == 0) anchorDirSample(ctx, family);", drawFn) : npos;
    const size_t sampleFn = module.find("void anchorDirSample(ID3D11DeviceContext* ctx, int family) {");
    const size_t pose0 = found(sampleFn) ? module.find("st::eyePoseOf(head, eyeToHead[0], R, E)", sampleFn) : npos;
    const size_t rows = found(pose0) ? module.find("holoCopyVsRows(ctx, g_dir.stage[k], 1, kDirFirstRow, kDirRows, 0, first, count)", pose0) : npos;
    const size_t boundaryFn = module.find("void srvTurretHudFrameBoundary(ID3D11DeviceContext* ctx) {");
    const bool direction = found(sampled) && sampled < groupOff && found(rows) && found(module.find("st::turretForwardWorld(rows, rows + 4, rows + 8, g_dir.R[k], u)")) &&
                           found(module.find("srvRadarAnchorPublishDirection(best, true, g_dir.hostSeqOf[bestSlot], false, g_dir.headPitchOf[bestSlot], g_dir.qpcOf[bestSlot]);")) && found(boundaryFn) &&
                           found(module.find("anchorDirBoundary(ctx);", boundaryFn)) && found(module.find("srvRadarAnchorPublishDirection(nullptr, false);"));
    // the setter pairs the sample with the anchor's rotation of the sample's own host frame (kept by frame) and uses it as it is: NO lead (an unled aim keeps a pairing error a smooth
    // lag, not a jitter); each call goes into the ring the draw-time correction reads, and a write shows the most recent call before it
    const size_t kept = found(setterFn) ? hook.find("s->aRing.put(nowSeq, rot);", setterFn) : npos;
    const size_t pairedK = found(kept) ? hook.find("s->aRing.find(smp.seq)", kept) : npos;
    const size_t along = found(pairedK) ? hook.find("st::radarAnchorAlong(u, ak, 1.0f, unitD, uaK)", pairedK) : npos;
    const size_t unled = found(along) ? hook.find("std::memcpy(ua, uaK, sizeof(ua));", along) : npos;
    const size_t carried = found(unled) ? hook.find("for (int i = 0; i < 3; ++i) d[i] = bitsOf(sF * ua[i]);", unled) : npos;
    const size_t ringed = found(rule) ? hook.find("putCall(*s, q, baseT, inForce ? m.d : none, A, anchor > 0, floatOf(fb[2]));", rule) : npos;
    const size_t shownBy = hook.find("if (!readCall(c, one) || one.t <= 0 || one.t >= before) continue;");
    const size_t newest = found(shownBy) ? hook.find("if (!found || one.t > out.t) {", shownBy) : npos;
    const size_t getterCheck = hook.find("if (!insideModule(s, getter)) return 0;");
    const size_t getterCall = hook.find("reinterpret_cast<MatrixFn>(getter)(anchor)");
    const bool directed = found(kept) && found(pairedK) && found(along) && found(unled) && found(carried) && found(rule) && carried < rule && found(ringed) && found(shownBy) &&
                          found(newest) && !found(hook.find("leadAim")) && !found(hook.find("extrapolateAim")) && found(getterCheck) && found(getterCall) && getterCheck < getterCall &&
                          found(hook.find("const uint64_t nowSeq = nativeTemporalFrameSequence();"));
    const size_t fix = ini.find("\n[fix]\n"), visual = ini.find("\n[visual]\n"), shipped = ini.find("\nsrv_turret_radar_anchor = on\n");
    const size_t ui = found(shipped) ? ini.rfind("# ui: SRV turret radar anchor | choices on, reversed, off | recommended on | live | menu", shipped) : npos;
    const bool iniOk = found(fix) && found(visual) && found(shipped) && shipped > fix && shipped < visual && found(ui) && shipped - ui < 200;
    const size_t source = build.find("\"src\\d3d11\\srv_radar_anchor.cpp\"");
    const bool buildOk = found(source) && source < build.find("ERROR: compile failed");
    const bool auditOk = found(audit.find("(\"d3d11/srv_radar_anchor.cpp\", \"the SRV radar anchor's exact-build hook\")"));
    return found(key) && moduleOk && integerOnly && adds && pinned && direction && directed && iniOk && buildOk && auditOk;
}
// The radar anchor's CPU camera rows: vscreen.cpp's Unmap tee hands every scene camera block write to srvTurretNoteCameraWrite (after the temporal pass's reader, before the block is
// forgotten), which keeps rows 277..279 by buffer with the host frame; the eye-0 draw looks its bound b1 up there FIRST, uses it only when written in the pose's own frame, and falls back
// to the GPU copy otherwise.
static bool wiredAnchorCpuRows(const std::string& vscreen, const std::string& module) {
    const size_t npos = std::string::npos;
    const size_t temporal = vscreen.find("temporalPassNoteSceneWrite(s->camResource, s->camData, s->camBytes);");
    const size_t tee = found(temporal) ? vscreen.find("if (srvTurretWantsCameraRows()) guardedBudget(g_srvTurretCameraBudget, [&] { srvTurretNoteCameraWrite(s->camResource, s->camData, s->camBytes); });", temporal) : npos;
    const size_t forgot = found(tee) ? vscreen.find("s->camResource = nullptr;", tee) : npos;
    const bool teeOk = found(tee) && found(forgot) && forgot - tee < 200 && found(vscreen.find("FaultBudget g_srvTurretCameraBudget("));
    const size_t noteFn = module.find("void srvTurretNoteCameraWrite(const void* res, const void* data, uint32_t bytes) {");
    const size_t copied = found(noteFn) ? module.find("std::memcpy(w->rows, static_cast<const float*>(data) + kCamRowsFloat, sizeof(w->rows));", noteFn) : npos;
    const size_t stamped = found(copied) ? module.find("w->hostSeq = nativeTemporalFrameSequence();", copied) : npos;
    const size_t sampleFn = module.find("void anchorDirSample(ID3D11DeviceContext* ctx, int family) {");
    const size_t cpuFirst = found(sampleFn) ? module.find("int miss = anchorDirCpuRows(ctx, poseSeq, cpuRows, &bytes, &writtenAt);", sampleFn) : npos;
    const size_t cpuPublished = found(cpuFirst) ? module.find("srvRadarAnchorPublishDirection(u, true, poseSeq, true, headPitch, writtenAt);", cpuFirst) : npos;
    const size_t gpuCopy = found(sampleFn) ? module.find("holoCopyVsRows(ctx, g_dir.stage[k], 1, kDirFirstRow, kDirRows, 0, first, count)", sampleFn) : npos;
    const bool moduleOk = found(copied) && found(stamped) && found(module.find("w->qpc = now.QuadPart;", stamped)) && found(module.find("kCamRowsFloat = kDirFirstRow * 4")) && found(cpuFirst) && found(cpuPublished) && found(gpuCopy) &&
                          cpuFirst < gpuCopy && found(module.find("if (w->hostSeq != hostSeq) return 3;")) && found(module.find("if (cb.ranged && cb.first != 0) return 2;"));
    return teeOk && moduleOk;
}
// The draw-time correction (srv_turret.cpp radarCorrectionBegin): reached from srvTurretHudBegin for the yielded families only; family 7 alone is moved, and only on a t4 a radar writer
// drew into; family 8 never (counted when on a radar target); the translation alone with the game's own depth; delta is paired with the call the frame's camera write shows, capped at
// 0.5 m, and put in each eye's space with that draw's own camera rows before the bind; the writers are recorded from vscreen.cpp's draw path.
static bool wiredCorrection(const std::string& vscreen, const std::string& module) {
    const size_t npos = std::string::npos;
    const size_t watch = vscreen.find("if (owner && srvTurretRadarWritersOn()) srvTurretRadarWriterDraw(self, bindingShaderHash(BindSlot::Vs), count, instances);");
    const size_t fn = module.find("bool radarCorrectionBegin(ID3D11DeviceContext* ctx, int family, int eye) {");
    const size_t fam = found(fn) ? module.find("if (family != 7) {", fn) : npos;
    const size_t fam8 = found(fam) ? module.find("if (kind) g_cc.family8.fetch_add(1, std::memory_order_relaxed);\n        return false;\n    }", fam) : npos;
    const size_t match = found(fam8) ? module.find("if (!kind) return leave(kCorrNoMatch);", fam8) : npos;
    const size_t eyeD = found(match) ? module.find("st::radarDeltaInEye(rows, g_corr.deltaW, dEye);", match) : npos;
    const size_t depth = found(eyeD) ? module.find("st::withDepth(st::translateParamsFor(ef, st::Group::kRadar), st::DepthMode::kGame)", eyeD) : npos;
    const size_t bound = found(depth) ? module.find("g_binding.begin(ctx, g_gs[st::kSceneSignature], cbuf, st::directSetGs, &camera)", depth) : npos;
    const size_t computeFn = module.find("int correctionCompute(ID3D11DeviceContext* ctx, int eye, FrameCorrection& c) {");
    const size_t written = found(computeFn) ? module.find("anchorDirCpuRows(ctx, poseSeq, rows, nullptr, &writtenAt)", computeFn) : npos;
    const size_t shown = found(written) ? module.find("srvRadarAnchorShownCall(writtenAt, call)", written) : npos;
    const size_t world = found(shown) ? module.find("st::radarCorrectionWorld(uTrue, call.A, call.sF, call.d, c.deltaW, dStar)", shown) : npos;
    const size_t capped = found(world) ? module.find("if (!(norm <= st::kCorrectionMaxM)) return kCorrOver;", world) : npos;
    const size_t kindFn = module.find("int billboardKind(ID3D11DeviceContext* ctx, bool& lastFrameOnly) {");
    const size_t t4 = found(kindFn) ? module.find("ctx->PSGetShaderResources(4, 1, &srv)", kindFn) : npos;
    // this frame's writers only move a billboard; the previous frame's are counted, never matched
    const size_t strict = found(t4) ? module.find("const int kind = res ? kindOf(g_writers, res) : 0;", t4) : npos;
    if (!found(strict) || found(module.find("&g_writersPrev}")) || found(module.find("kindOf(g_writersPrev, res) == 1"))) return false;
    const size_t writerFn = module.find("void srvTurretRadarWriterDraw(ID3D11DeviceContext* ctx, uint64_t vs, uint32_t count, uint32_t instances) {");
    const size_t icon256 = found(writerFn) ? module.find("td.Width == 256 && td.Height == 256", writerFn) : npos;
    return found(watch) && found(bound) && found(capped) && found(t4) && found(icon256) && found(module.find("constexpr uint64_t kRadarIconVs = 0x5B0068AF5630F96Bull;"));
}
// The ghost fix (flight da716942) and the counters: in radarCorrectionBegin the hologram source instrument runs first (it records the game's t1), a disc and an icon are moved at most
// once an eye a frame, and t1 is swapped for the far depth only after the bind, on the moved path (the function is called nowhere else); srvTurretHudEnd puts the game's t1 back; the far
// texture is made at the game's t1 size, read from its view; the 0.5 m bound leaves garbage to the game and over 10 cm is counted as large.
static bool wiredGhost(const std::string& module) {
    const size_t npos = std::string::npos;
    const size_t fn = module.find("bool radarCorrectionBegin(ID3D11DeviceContext* ctx, int family, int eye) {");
    const size_t holo = found(fn) ? module.find("if (family == kHoloFamilyA || family == kHoloFamilyB) holoCollect(ctx, family, eye, g_probe[eye][family].ordinal++);", fn) : npos;
    const size_t kind = found(holo) ? module.find("const int kind = billboardKind(ctx, lastFrameOnly);", holo) : npos;
    const size_t once = found(kind) ? module.find("if (g_movedAt[eye][kind - 1] == g_frame + 1u) return leave(kCorrExtra);", kind) : npos;
    const size_t bound = found(once) ? module.find("g_binding.begin(ctx, g_gs[st::kSceneSignature], cbuf, st::directSetGs, &camera)", once) : npos;
    const size_t swap = found(bound) ? module.find("    farDepthSwap(ctx);", bound) : npos;
    const size_t marked = found(swap) ? module.find("g_movedAt[eye][kind - 1] = g_frame + 1u;", swap) : npos;
    size_t calls = 0;
    for (size_t at = module.find("farDepthSwap("); found(at); at = module.find("farDepthSwap(", at + 1)) ++calls;   // the definition and the one call
    const size_t endFn = module.find("void srvTurretHudEnd(ID3D11DeviceContext* ctx) {");
    const size_t endClose = found(endFn) ? module.find("\n}\n", endFn) : npos;
    const size_t restored = found(endFn) ? module.find("farDepthRestore(ctx);", endFn) : npos;
    const size_t sized = module.find("ID3D11ShaderResourceView* farView = game && st::depthViewSize(game, w, h) ? g_farDepth.get(ctx, w, h, farDepthClear) : nullptr;");
    const size_t raw = module.find("vScreenPSSetShaderResourcesRaw(ctx, 1, 1, &farView);");
    const bool caps = found(module.find("if (!(norm <= st::kCorrectionMaxM)) return kCorrOver;")) && found(module.find("if (norm > st::kCorrectionLargeM) g_cc.large.fetch_add(1, std::memory_order_relaxed);"));
    return found(holo) && found(kind) && found(once) && found(bound) && found(swap) && found(marked) && calls == 2 && found(restored) && found(endClose) && restored < endClose && found(sized) &&
           found(raw) && caps;
}
static bool wiredBuild(const std::string& t) {
    const size_t source = t.find("\"src\\d3d11\\srv_turret.cpp\"");
    const size_t compile = t.find("ERROR: compile failed");
    return found(source) && found(compile) && source < compile && found(t.find(":rig_srv_turret_hud_test\n")) && found(t.find("srv_turret_hud_test.exe\" --self-test || exit /b 1"));
}

static void wiringCases() {
    const std::string vscreen = readText("src/d3d11/vscreen.cpp"), onFoot = readText("src/d3d11/native_on_foot.cpp"), module = readText("src/d3d11/srv_turret.cpp"),
                      journal = readText("src/d3d11/journal_watch.cpp"), journalH = readText("src/d3d11/journal_watch.h"), temporal = readText("src/d3d11/native_temporal.cpp"),
                      ini = readText("edvr.ini"), flatKeys = readText("src/common/flat_profile_keys.h"), build = readText("build.bat"),
                      shaderBuild = readText("tools/temporal_shader_build/temporal_shader_build.cpp"), variants = readText("tools/temporal_shader_build/fixed_srv_turret_variants.h"),
                      anchorHook = readText("src/d3d11/srv_radar_anchor.cpp"), audit = readText("tools/hook_signatures.py");
    check(vscreen.size() > 100000 && onFoot.size() > 100000 && module.size() > 5000 && journal.size() > 20000 && temporal.size() > 20000 && ini.size() > 20000 && flatKeys.size() > 500 &&
              build.size() > 50000 && shaderBuild.size() > 20000 && variants.size() > 100 && anchorHook.size() > 2000 && audit.size() > 50000,
          "the wiring scan read its sources (the repo root is the working directory)");

    check(wiredVscreenScope(vscreen), "vscreen.cpp: the scope opens after the skipped draw's return and before the layer's decision and every issue of the draw");
    check(!wiredVscreenScope(replaced(vscreen, "SrvTurretHudScope turretHud(self);", "")), "control: no scope trips the pin");
    check(!wiredVscreenScope(replaced(replaced(vscreen, "SrvTurretHudScope turretHud(self);\n    if (owner && srvTurretHudWantsDraws()) turretHud.bound = srvTurretHudBeginDraw(g_state, self);", ""),
                                      "const bool originalIssued=observedDraw(", "SrvTurretHudScope turretHud(self);\n    if (owner && srvTurretHudWantsDraws()) turretHud.bound = srvTurretHudBeginDraw(g_state, self);\n    const bool originalIssued=observedDraw(")),
          "control: a scope that opens after the layer's bracket (just before the game's issue) trips the pin");
    check(!wiredVscreenScope(replaced(vscreen, "if (owner && srvTurretHudWantsDraws()) turretHud.bound", "if (srvTurretHudWantsDraws()) turretHud.bound")), "control: begun on a foreign context trips the pin");
    check(wiredVscreenBegin(vscreen), "vscreen.cpp: the draw is matched on BOTH shaders of the pair, then the eye is the layer's, the table's read-only form, then the registering form");
    check(!wiredVscreenBegin(replaced(vscreen, "srv_turret::familyOf(vs, bindingShaderHash(BindSlot::Ps))", "0")), "control: the vertex shader alone (no pair match) trips the pin");
    check(!wiredVscreenBegin(replaced(vscreen, "uiLayerEyeOfGameTarget(info->resource);\n            if (eye != 0 && eye != 1) eye = uiDepthEyeOfTargetReadOnly(info->resource);", "-1;")), "control: no eye lookup trips the pin");
    check(wiredVscreenUnmoved(vscreen), "vscreen.cpp: every route a family draw can take without the pass's shader is counted (the four direct thunks, the indirect and auto draws, an unscoped reissue, a command list, the scope's decline)");
    check(!wiredVscreenUnmoved(replaced(vscreen, "    srvTurretUnmovedDirect(self, false);   // this thunk has no fast path: only a second context's draw is unscoped", "")), "control: the draw thunk that has no fast path not looking for a second context trips the pin");
    check(!wiredVscreenUnmoved(replaced(vscreen, "    srvTurretUnmovedPlain(self, srv_turret::UnmovedRoute::kAuto);\n", "")), "control: DrawAuto not counted trips the pin");
    check(!wiredVscreenUnmoved(replaced(vscreen, "    if (srvTurretHudWantsDraws()) srvTurretUnmovedReissue(self, _ReturnAddress());", "")), "control: a replay outside the scope not counted trips the pin");
    check(!wiredVscreenUnmoved(replaced(vscreen, "if (!privateExecution && srvTurretHudWantsDraws()) srvTurretHudNoteUnmoved(-1, srv_turret::UnmovedRoute::kCommandList, nullptr);", "")), "control: a command list not counted trips the pin");
    check(!wiredVscreenUnmoved(replaced(vscreen, "if (self != g_state->ownerCtx || srvTurretHudScopeActive()) return;", "if (self != g_state->ownerCtx) return;")), "control: a replay inside the scope counted as unmoved trips the pin");
    check(!wiredVscreenBegin(replaced(vscreen, "if (!bound) srvTurretHudNoteRefusedDraw(family);", "")), "control: a scope that declines a draw without counting it trips the pin");
    check(wiredVscreenGate(vscreen), "vscreen.cpp: the pass is a term of the draw gate (the AA-off fast path would otherwise skip forwardWithVerdict)");
    check(!wiredVscreenGate(replaced(vscreen, "        srvTurretHudWantsDraws() || // issue 140", "        false || // issue 140")), "control: a gate without the term trips the pin");
    check(wiredVscreenBoundary(vscreen), "vscreen.cpp: the boundary tick settles a binding, and the shutdown releases the pass after the bars'");
    check(!wiredVscreenBoundary(replaced(vscreen, "    srvTurretShutdown();\n", "")), "control: no shutdown trips the pin");

    check(wiredNativeOnFoot(onFoot), "native_on_foot.cpp: the frame tick runs just before alignTick, which adds the camera term after the on-foot gate and reports what it published");
    check(!wiredNativeOnFoot(replaced(onFoot, "srvTurretFrameTick(frameNo);", "")), "control: no frame tick trips the pin");
    check(!wiredNativeOnFoot(replaced(onFoot, "if (i == 2) eye[i] += turretBackZ;", "")), "control: no camera term trips the pin");
    check(!wiredNativeOnFoot(replaced(replaced(onFoot, "            if (i == 2) eye[i] += turretBackZ;   // issue 140: the turret view's camera, after the gate\n", ""),
                                      "            eye[i] = nofoot::f115::gate(onFoot, eye[i]);\n",
                                      "            if (i == 2) eye[i] += turretBackZ;\n            eye[i] = nofoot::f115::gate(onFoot, eye[i]);\n")),
          "control: the term before the on-foot gate (which would zero it) trips the pin");
    check(!wiredNativeOnFoot(replaced(onFoot, "srvTurretNotePublished(turretBackZ, g_align.serial);", "")), "control: nothing told to the module trips the pin");

    check(wiredModule(module), "srv_turret.cpp: the shader from the build's bytecode, the binding class alone, the literal key reads, the host's geometry, the log lines");
    check(!wiredModule(replaced(module, "shaderSwapCreateGs(ctx, kShader[f].bytecode", "shaderSwapCreateGs(ctx, kSupercruiseBarsGsBytecode")), "control: another shader's bytecode trips the pin");
    check(!wiredModule(replaced(module, "const auto begun = g_binding.begin(ctx, twin ? g_soGs[sig] : g_gs[sig], constants, st::directSetGs, needScene ? &scene : nullptr);", "ctx->GSSetShader(g_gs[sig], nullptr, 0); const auto begun = g_binding.begin(ctx, twin ? g_soGs[sig] : g_gs[sig], constants, st::directSetGs, needScene ? &scene : nullptr);")),
          "control: a raw geometry-stage set outside the binding trips the pin");
    check(!wiredModule(replaced(module, "\"fix.srv_turret_hud_gap\", 0, 0, 200", "\"fix.srv_turret_hud_gap\", 96, 0, 200")), "control: a drifted default of the gap trips the pin");
    check(!wiredModule(replaced(module, "\"fix.srv_turret_panels_back\", 120, 0, 200", "\"fix.srv_turret_panels_back\", 110, 0, 200")), "control: a drifted default of the panels' push trips the pin");
    check(!wiredModule(replaced(module, "\"fix.srv_turret_radar_back\", 0, 0, 200", "\"fix.srv_turret_radar_back\", 0, 0, 150")), "control: a drifted range of the radar's push trips the pin");
    check(!wiredModule(replaced(module, "\"fix.srv_turret_hud_forward\", 70, 0, 150", "\"fix.srv_turret_hud_forward\", 60, 0, 150")), "control: a drifted default of the HUD-forward key trips the pin");
    check(!wiredModule(replaced(module, "\"fix.srv_turret_camera_back\", 0, 0, 150", "\"fix.srv_turret_camera_back\", 60, 0, 150")), "control: the camera key back at 60 trips the pin");
    check(!wiredModule(replaced(module, "if (!paramsOfGroup(group, st::EyeFrame{}, unused)) return false;", "")), "control: a draw counted and refused before its group is known to be on trips the pin");
    check(!wiredModule(replaced(module, "if (!paramsOfGroup(group, frame, params)) return false;", "params = st::paramsFor(frame, 0.96f);")), "control: constants made from the pull alone (the push ignored) trip the pin");
    check(!wiredModule(replaced(module, "\"moves %.2f m deeper\"", "\"moves %.2f m nearer\"")), "control: a first-draw line that calls a push 'nearer' trips the pin");
    check(!wiredModule(replaced(module, "g_anchor.step(headAt, on, backCm, forwardCm, radarForwardCm)", "g_anchor.step(headAt, on, backCm, backCm, radarForwardCm)")), "control: an anchor that does not see the HUD-forward key trips the pin");
    check(!wiredModule(replaced(module, "g_anchor.step(headAt, on, backCm, forwardCm, radarForwardCm)", "g_anchor.step(headAt, on, backCm, forwardCm, forwardCm)")), "control: an anchor that does not see the radar-forward key trips the pin");
    check(!wiredModule(replaced(module, "\"fix.srv_turret_radar_forward\", 70, 0, 150", "\"fix.srv_turret_radar_forward\", 60, 0, 150")), "control: a drifted default of the radar-forward key trips the pin");
    check(!wiredModule(replaced(module, "cfg.getString(\"fix.srv_turret_hud_depth\", \"front\")", "cfg.getString(\"fix.srv_turret_hud_depth\", \"game\")")), "control: a drifted default of the depth key (front) trips the pin");
    check(!wiredModule(replaced(module, "cfg.getString(\"fix.srv_turret_hud_scissor\", \"open\")", "cfg.getString(\"fix.srv_turret_hud_scissor\", \"game\")")), "control: a drifted default of the scissor key (open) trips the pin");
    check(!wiredModule(replaced(module, "const auto opened = g_scissor.open(ctx, seen, apply);", "const auto opened = st::Scissor::Open::kOff;")), "control: a scissor that is never opened trips the pin");
    check(!wiredModule(replaced(module, "if (!g_scissor.restore(ctx) && !g_scissorRestoreNoted) {", "if (false) {")), "control: the game's scissor rectangles never put back trip the pin");
    check(!wiredModule(replaced(module, "g_scissor.owed() || !ensureGs(ctx, sig)", "!ensureGs(ctx, sig)")), "control: a draw issued while the scissor rectangles are owed back trips the pin");
    check(!wiredModule(replaced(module, "if (g_scissor.owed() && ctx) (void)g_scissor.settle(ctx);", "")), "control: owed scissor rectangles never settled at the boundary trip the pin");
    check(!wiredModule(replaced(module, "scissor opened %u pose refreshed at draw: %u of %u frames", "pose refreshed at draw: %u of %u frames")), "control: the 10 s line without the scissor count trips the pin");
    check(!wiredModule(replaced(module, "out = st::withDepth(out, static_cast<st::DepthMode>(g_depthMode.load(std::memory_order_relaxed)));", "")), "control: constants that never get the depth key's mode trip the pin");
    check(!wiredModule(replaced(module, "const bool needScene = sig == st::kSceneSignature;", "const bool needScene = false;")), "control: the Scorpion hologram's draw without the vertex constants trips the pin");
    check(!wiredModule(replaced(module, "if (needScene && !st::readGameCb(ctx, st::kGameCbSlot, scene)) {", "if (false) {")), "control: the vertex constants never read trip the pin");
    check(!wiredModule(replaced(module, ", st::directSetGs, needScene ? &scene : nullptr)", ")")), "control: the vertex constants read but never bound trip the pin");
    check(!wiredModule(replaced(module, "\"fix.srv_turret_radar_forward\", 70, 0, 150", "\"fix.srv_turret_radar_forward\", 70, 0, 100")), "control: a drifted range of the radar-forward key trips the pin");
    check(!wiredModule(replaced(module, "st::forwardCmOf(group, g_forwardCm.load(std::memory_order_relaxed), g_radarForwardCm.load(std::memory_order_relaxed)), out))", "g_forwardCm.load(std::memory_order_relaxed), out))")),
          "control: the radar group made to use the HUD-forward distance (the thin sliver) trips the pin");
    check(!wiredModule(replaced(module, "if (!drawEyeFrame(eye, frame)) {", "if (!readEye(eye, frame)) {")), "control: a draw that uses the tick's frames (a frame old) trips the pin");
    check(!wiredModule(replaced(module, "const uint64_t seq = nativeTemporalFrameSequence();", "const uint64_t seq = g_tickSeq.load(std::memory_order_acquire);")),
          "control: a draw that never sees a newer frame sequence than the tick built from trips the pin");
    check(!wiredModule(replaced(module, "if (seq != 0 && seq != g_drawSeq) {", "if (seq != 0) {")), "control: a rebuild on every draw (not once for each new sequence) trips the pin");
    check(!wiredModule(replaced(module, "g_poseRefreshed.fetch_add(1, std::memory_order_relaxed);", "")), "control: no count of the frames whose pose was refreshed at draw trips the pin");
    check(!wiredModule(replaced(module, "buildEyeFrames(head, eyeToHead, true, anchor, g_forwardCm.load(std::memory_order_relaxed), g_radarForwardCm.load(std::memory_order_relaxed), g_drawFrames, g_drawOk);", "")),
          "control: a draw that never rebuilds the eye frames trips the pin");
    check(!wiredModule(replaced(module, "pose refreshed at draw: %u of %u frames", "pose refreshed")), "control: the 10 s line without the pose-refresh count trips the pin");
    check(!wiredModule(replaced(module, "Tradar=(%.3f,%.3f,%.3f) ", "")), "control: the 10 s line without the radar group's T trips the pin");
    check(!wiredModule(replaced(module, "const float tangents[4] = {tan[0] + shift[0], tan[1] + shift[0], tan[2] + shift[1], tan[3] + shift[1]};", "const float tangents[4] = {tan[0], tan[1], tan[2], tan[3]};")),
          "control: tangents without the jitter shift (the game projects with the sum) trip the pin");
    check(!wiredModule(replaced(module, "        g_anchor.reset();", "        (void)0;")), "control: an anchor that survives the pass being off trips the pin");
    check(!wiredModule(without(module, "refuse(st::Refuse::kNoGeometry)")), "control: a draw with no geometry that is not refused trips the pin");
    check(!wiredModule(replaced(module, "const bool on = turret && journalSrvScorpion();", "const bool on = turret;")), "control: both parts acting on the turret view alone (the Scarab's too) trips the pin");
    check(!wiredModule(replaced(module, "g_backSeatedZ = st::backSeatedZ(on, backCm);", "g_backSeatedZ = st::backSeatedZ(turret, backCm);")), "control: a camera term made from the turret view, not the Scorpion's, trips the pin");
    check(!wiredModule(replaced(module, "const bool wanted = on && (panelsCm > 0 || radarCm > 0 || forwardCm > 0 || radarForwardCm > 0 || gapCm > 0);", "const bool wanted = turret && (panelsCm > 0 || radarCm > 0 || forwardCm > 0 || radarForwardCm > 0 || gapCm > 0);")),
          "control: a HUD pass made from the turret view, not the Scorpion's, trips the pin");
    check(!wiredModule(replaced(module, "const bool wanted = on && (panelsCm > 0 || radarCm > 0 || forwardCm > 0 || radarForwardCm > 0 || gapCm > 0);", "const bool wanted = on && gapCm > 0;")),
          "control: a pass wanted by the gap alone (the push keys and the forward keys cannot open it) trips the pin");
    check(!wiredModule(replaced(module, "const bool wanted = on && (panelsCm > 0 || radarCm > 0 || forwardCm > 0 || radarForwardCm > 0 || gapCm > 0);", "const bool wanted = on && (panelsCm > 0 || radarCm > 0 || forwardCm > 0 || gapCm > 0);")),
          "control: a pass the radar-forward key alone cannot open trips the pin");
    check(!wiredModule(replaced(module, "!st::topologyCarried(static_cast<int>(topology))", "topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST")), "control: lists only (a strip refused) trips the pin");
    check(!wiredModule(replaced(module, "return sig < kSigN && g_gs[sig] != nullptr;", "return g_gs[0] != nullptr && g_gs[sig] != nullptr;")), "control: every family stood down by one signature's missing shader trips the pin");
    check(wiredLatch(journal, journalH), "journal_watch: the Scorpion latch is fed the tail's bytes once, reset with the file, published, and each change is said with its event");
    check(!wiredLatch(replaced(journal, "    s.vehicle.feed(data, len, [&s](bool scorpion, journal_vehicle::Event cause) { noteVehicle(s, scorpion, cause); });\n", ""), journalH), "control: a latch that is never fed trips the pin");
    check(!wiredLatch(replaced(journal, "scanRange(s, stitched, stitchedLen);\n    scanRange(s, data, len);\n    // Issue 140", "scanRange(s, stitched, stitchedLen);\n    s.vehicle.feed(stitched, stitchedLen, [&s](bool scorpion, journal_vehicle::Event cause) { noteVehicle(s, scorpion, cause); });\n    scanRange(s, data, len);\n    // Issue 140"), journalH),
          "control: the stitched head (bytes the next read feeds again) fed to the latch trips the pin");
    check(!wiredLatch(replaced(journal, "if (s.vehicle.reset()) noteVehicle(s, false, journal_vehicle::Event::kNewJournal);", ""), journalH), "control: a latch that survives a file change trips the pin");
    check(!wiredLatch(replaced(journal, "g_v.srvScorpion.store(p.srvScorpion, kRelaxed);", ""), journalH), "control: a latch that is never published trips the pin");
    check(!wiredLatch(replaced(journal, "\"srv turret: the vehicle is %s (event %s)\"", "\"srv turret: the vehicle changed\""), journalH), "control: a change said without the event trips the pin");
    check(!wiredLatch(journal, replaced(journalH, "bool journalSrvScorpion();", "")), "control: no accessor declared trips the pin");
    check(wiredInstrument(vscreen, module), "the GPU instrument: issues counted at the game's issue and the re-issues; the queries begin after the bind and end before the restore; read once a frame; the log lines");
    check(!wiredInstrument(replaced(vscreen, "    srvTurretHudCountIssue(self);   // issue 140's GPU instrument: a re-issue", "    // issue 140's GPU instrument: a re-issue"), module), "control: a re-issue that is not counted trips the pin");
    check(!wiredInstrument(replaced(vscreen, "        srvTurretHudCountIssue(self);   // issue 140's GPU instrument: the game's own issue", "        // issue 140's GPU instrument: the game's own issue"), module), "control: a thunk that does not count the game's own issue trips the pin");
    check(!wiredInstrument(replaced(vscreen, "        const bool issued = draw(altered);\n", "        const bool issued = draw(altered);\n        srvTurretHudCountIssue(self);\n"), module),
          "control: a count inside observedDraw (whose text other rigs pin) trips the pin");
    check(!wiredInstrument(vscreen, replaced(module, "probeEnd(ctx);   // the queries end", "// the queries end")), "control: queries that never end trip the pin");
    check(!wiredInstrument(vscreen, replaced(module, "kProbeGiveUpFrames = 8,", "kProbeGiveUpFrames = 80,")), "control: a readback that never gives up trips the pin");
    check(!wiredInstrument(vscreen, replaceAll(module, "D3D11_ASYNC_GETDATA_DONOTFLUSH", "0")), "control: a readback that flushes trips the pin");
    check(!wiredInstrument(vscreen, replaced(module, "ctx->Begin(p.occlusion);", "")), "control: the disc's occlusion query that never begins trips the pin");
    check(!wiredInstrument(vscreen, replaced(module, "if (p.discArmed) ctx->End(p.occlusion);", "")), "control: the disc's occlusion query that never ends trips the pin");
    check(!wiredInstrument(vscreen, replaced(module, "kDiscAlphaSlot = 2, kDiscAlphaRow = 11, kDiscIntensitySlot = 1, kDiscIntensityRow = 90", "kDiscAlphaSlot = 2, kDiscAlphaRow = 12, kDiscIntensitySlot = 1, kDiscIntensityRow = 90")),
          "control: a register other than cb2[11] trips the pin");
    check(!wiredInstrument(vscreen, replaced(module, "if (!g_stateNoted[family]) noteBegin(ctx, family, eye);", "")), "control: a family's first scope that never says its state trips the pin");
    check(!wiredInstrument(vscreen, replaced(module, "if (family == static_cast<int>(kDiscFamily)) probeReadDisc(ctx, eye);", "")), "control: the disc's results never read trip the pin");
    check(!wiredInstrument(vscreen, replaced(module, "if (static_cast<uint32_t>(family) == kDiscFamily && eye == 0 && p.planePose) logDiscPlanes(static_cast<const float*>(m.pData), vertices, fam, p);", "")),
          "control: the disc's plane never logged trips the pin");
    check(!wiredInstrument(vscreen, replaced(module, "st::eyePoseOf(head, eyeToHead[0], p.poseR, p.poseE)", "st::eyePoseOf(head, eyeToHead[1], p.poseR, p.poseE)")), "control: the plane read with the wrong eye's pose trips the pin");
    check(!wiredInstrument(vscreen, replaced(module, "st::unmovedList(unmoved, sizeof(unmoved), unmovedCounts);", "unmoved[0] = 0;")), "control: the unmoved draws never listed trip the pin");
    check(wiredAccessors(journal, temporal, journalH), "journal_watch and native_temporal: the turret bit and the eye separation accessors");
    check(!wiredAccessors(replaced(journal, "srv_turret::turretViewFromFlags(peek(g_v.flags))", "false"), temporal, journalH), "control: an accessor that answers false trips the pin");
    check(!wiredAccessors(journal, replaced(temporal, "g_frameSequence.store(f->sequence,std::memory_order_release);", ""), journalH), "control: a frame sequence that is never published trips the pin");
    check(!wiredAccessors(journal, replaced(replaced(temporal, "  g_frameSequence.store(f->sequence,std::memory_order_release);   // last: the head, eyes, frusta and shift above are this frame's\n", ""),
                                            "  edvr::lightGridCacheNoteJitter(s->frameCounter,jx,jy,s->shift);", "  g_frameSequence.store(f->sequence,std::memory_order_release);\n  edvr::lightGridCacheNoteJitter(s->frameCounter,jx,jy,s->shift);"), journalH),
          "control: a frame sequence published before the shift is copied (a reader would see a half-built frame) trips the pin");
    check(!wiredAccessors(journal, replaced(temporal, "g_recommended.store(0,std::memory_order_release);g_frameSequence.store(0,std::memory_order_release);", "g_recommended.store(0,std::memory_order_release);"), journalH),
          "control: a frame sequence that survives an invalidate trips the pin");
    check(wiredKeys(ini, flatKeys), "edvr.ini: all seven keys in [fix] with their menu rows and shipped defaults; the flat profile's allow-list names none");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_hud_gap = 0\n", "\nsrv_turret_hud_gap = 96\n"), flatKeys), "control: a drifted shipped default of the gap trips the pin");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_panels_back = 120\n", "\nsrv_turret_panels_back = 110\n"), flatKeys), "control: a drifted shipped default of the panels' push trips the pin");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_hud_forward = 70\n", "\nsrv_turret_hud_forward = 0\n"), flatKeys), "control: a drifted shipped default of the HUD-forward key trips the pin");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_radar_forward = 70\n", "\nsrv_turret_radar_forward = 0\n"), flatKeys), "control: a drifted shipped default of the radar-forward key trips the pin");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_hud_depth = front\n", "\nsrv_turret_hud_depth = game\n"), flatKeys), "control: a drifted shipped default of the depth key trips the pin");
    check(!wiredKeys(replaced(ini, "# ui: SRV turret HUD depth | choices front, game, real | recommended front", "# ui: SRV turret HUD depth | choices front, game, real | recommended game"), flatKeys), "control: a drifted recommendation of the depth key trips the pin");
    check(!wiredKeys(ini, flatKeys + "\nsrv_turret_hud_depth\n"), "control: the depth key on the flat allow-list trips the pin");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_hud_scissor = open\n", "\nsrv_turret_hud_scissor = game\n"), flatKeys), "control: a drifted shipped default of the scissor key trips the pin");
    check(!wiredKeys(replaced(ini, "# ui: SRV turret HUD screen limit | choices open, game", "# ui: SRV turret HUD screen limit | choices open"), flatKeys), "control: a drifted menu row of the scissor key trips the pin");
    check(!wiredKeys(ini, flatKeys + "\nsrv_turret_hud_scissor\n"), "control: the scissor key on the flat allow-list trips the pin");
    check(!wiredKeys(replaced(ini, "# ui: SRV turret radar forward | range 0..150 | step 5", "# ui: SRV turret radar forward | range 0..150 | step 10"), flatKeys), "control: a drifted menu row of the radar-forward key trips the pin");
    check(!wiredKeys(ini, flatKeys + "\nsrv_turret_radar_forward\n"), "control: the radar-forward key on the flat allow-list trips the pin");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_camera_back = 0\n", "\nsrv_turret_camera_back = 60\n"), flatKeys), "control: the camera key shipped at 60 trips the pin");
    check(!wiredKeys(replaced(ini, "# ui: SRV turret radar back | range 0..200 | step 5", "# ui: SRV turret radar back | range 0..200 | step 10"), flatKeys), "control: a drifted menu row of the radar's push trips the pin");
    check(!wiredKeys(ini, flatKeys + "\nsrv_turret_hud_gap\n"), "control: the gap key on the flat allow-list trips the pin");
    check(!wiredKeys(ini, flatKeys + "\nsrv_turret_radar_back\n"), "control: the radar key on the flat allow-list trips the pin");
    check(wiredTables(shaderBuild, variants), "temporal_shader_build: the HLSL is included and the variant is registered as a gs_5_0");
    check(!wiredTables(shaderBuild, replaced(variants, "\"gs_5_0\"", "\"vs_5_0\"")), "control: a vertex-shader profile trips the pin");
    check(wiredAnchor(module, anchorHook, ini, build, audit), "the radar anchor: the key, its publish from the frame tick, the billboards left to the game before they are counted, the hooks' pins, "
                                                              "the setter's add through the pure rule, the integer-only update counter that writes nothing, the ini row, the compile list and the audit's "
                                                              "exact-only entry");
    check(!wiredAnchor(replaced(module, "cfg.getString(\"fix.srv_turret_radar_anchor\", \"on\")", "cfg.getString(\"fix.srv_turret_radar_anchor\", \"off\")"), anchorHook, ini, build, audit),
          "control: a drifted default of the radar anchor key trips the pin");
    check(!wiredAnchor(replaced(module, "anchorTick(on, frameNo, radarForwardCm);", "anchorTick(turret, frameNo, radarForwardCm);"), anchorHook, ini, build, audit),
          "control: an offset published for the turret view alone (the Scarab's too) trips the pin");
    check(!wiredAnchor(replaced(module, "anchorTick(false, frameNo, 0);", ""), anchorHook, ini, build, audit), "control: a flat profile that leaves an offset in the game trips the pin");
    check(!wiredAnchor(replaced(module, "    if (anchorLeaves(family)) return radarCorrectionBegin(ctx, family, eye);", ""), anchorHook, ini, build, audit), "control: billboards moved twice (by the game and the shader) trip the pin");
    check(!wiredAnchor(replaced(replaced(module, "    if (anchorLeaves(family)) return radarCorrectionBegin(ctx, family, eye);", ""), "g_familyDraws[family].fetch_add(1, std::memory_order_relaxed);",
                                "g_familyDraws[family].fetch_add(1, std::memory_order_relaxed);\n    if (anchorLeaves(family)) return radarCorrectionBegin(ctx, family, eye);"),
                       anchorHook, ini, build, audit),
          "control: a billboard left to the game but counted as a draw of the pass trips the pin");
    check(!wiredAnchor(replaced(module, "&& !anchorLeaves(family);", ";"), anchorHook, ini, build, audit), "control: a billboard left to the game counted as refused trips the pin");
    check(!wiredAnchor(replaced(module, " && a.holdsOurs, std::memory_order_relaxed);", ", std::memory_order_relaxed);"), anchorHook, ini, build, audit),
          "control: billboards left to the game while the placement does not hold our offset trip the pin");
    check(!wiredAnchor(replaced(module, "    srvRadarAnchorShutdown();", ""), anchorHook, ini, build, audit), "control: a gate left open at unload trips the pin");
    const std::string counted = "        if (radar) s->updates.fetch_add(1, std::memory_order_relaxed);\n";
    check(!wiredAnchor(module, replaced(anchorHook, counted, counted + "        const float scale = 1.0f;\n"), ini, build, audit),
          "control: float work in the update counter before the original runs (it would clobber the game's dt) trips the pin");
    check(!wiredAnchor(module, replaced(anchorHook, counted, counted + "        std::memcpy(&b, &c, 8);\n"), ini, build, audit), "control: a memcpy in the update counter trips the pin");
    check(!wiredAnchor(module, replaced(anchorHook, counted, counted + "        volatile uint32_t* t = reinterpret_cast<volatile uint32_t*>(obj + 0x15F0);\n        t[2] = 0;\n"), ini, build, audit),
          "control: the update writing the placement (the first build's replace, which dropped the radar) trips the pin");
    check(!wiredAnchor(module, replaced(anchorHook, "        return forward(iface, reinterpret_cast<uintptr_t>(Q), r8, r9);\n    }\n    if (r == st::PlacementMove::kRestored) {",
                                        "        return forward(iface, p, r8, r9);\n    }\n    if (r == st::PlacementMove::kRestored) {"), ini, build, audit),
          "control: an add that hands the original the game's P (the offset never reaches it) trips the pin");
    check(!wiredAnchor(module, replaced(anchorHook, "const st::PlacementMove r = st::radarPlacementMove(P, d, m, Q);", "const st::PlacementMove r = st::PlacementMove::kAdded;"), ini, build, audit),
          "control: a setter that bypasses the add-and-idempotence rule trips the pin");
    check(!wiredAnchor(module, replaced(anchorHook, "if (stamp != kStamp || size != kImageSize) {", "if (false) {"), ini, build, audit), "control: a hook that does not check the build trips the pin");
    check(!wiredAnchor(module, replaced(anchorHook, "if (std::memcmp(got, prologue, n) != 0) return", "if (false) return"), ini, build, audit), "control: a hook that does not check the prologue trips the pin");
    check(!wiredAnchor(module, replaced(anchorHook, "{0x48, 0x83, 0xEC, 0x48, 0x0F, 0x10, 0x02,", "{0x48, 0x83, 0xEC, 0x28, 0x0F, 0x10, 0x02,"), ini, build, audit),
          "control: a drifted setter prologue pin trips the pin");
    check(!wiredAnchor(replaced(module, "    if (eye == 0) anchorDirSample(ctx, family);", ""), anchorHook, ini, build, audit), "control: the live camera never sampled (d stays on the vehicle's axis) trips the pin");
    check(!wiredAnchor(replaced(module, "st::eyePoseOf(head, eyeToHead[0], R, E)", "st::eyePoseOf(head, eyeToHead[1], R, E)"), anchorHook, ini, build, audit),
          "control: eye 0's camera paired with eye 1's pose trips the pin");
    check(!wiredAnchor(replaced(module, "    anchorDirBoundary(ctx);\n", ""), anchorHook, ini, build, audit), "control: the camera copies never read back trip the pin");
    check(!wiredAnchor(replaced(module, "srvRadarAnchorPublishDirection(nullptr, false);", "(void)0;"), anchorHook, ini, build, audit), "control: a u kept from an earlier stay trips the pin");
    check(!wiredAnchor(module, replaced(anchorHook, "        if (!insideModule(s, getter)) return 0;\n", ""), ini, build, audit), "control: the anchor's getter called wherever its vtable points trips the pin");
    check(!wiredAnchor(module, replaced(anchorHook, "st::radarAnchorAlong(u, ak, 1.0f, unitD, uaK)", "false"), ini, build, audit), "control: a setter that never turns d along u trips the pin");
    check(!wiredAnchor(module, replaced(anchorHook, "s->aRing.find(smp.seq)", "s->aRing.find(nowSeq)"), ini, build, audit),
          "control: u paired with the anchor of the setter's frame, not the sample's (the vehicle's turn would lag) trips the pin");
    check(!wiredAnchor(module, replaced(anchorHook, "std::memcpy(ua, uaK, sizeof(ua));", "st::unitOfAim(leadAim(st::aimOf(uaK)), ua);"), ini, build, audit),
          "control: a setter that leads the sample again (the shake of a8465cb3) trips the pin");
    check(!wiredAnchor(module, replaced(anchorHook, "if (!found || one.t > out.t) {", "if (!found || one.t < out.t) {"), ini, build, audit),
          "control: a write paired with the oldest kept call, not the most recent before it, trips the pin");
    check(!wiredAnchor(module, replaced(anchorHook, "putCall(*s, q, baseT, inForce ? m.d : none, A, anchor > 0, floatOf(fb[2]));", "(void)inForce;"), ini, build, audit),
          "control: setter calls never put into the ring the correction reads trip the pin");
    check(wiredAnchorCpuRows(vscreen, module), "the radar anchor's camera rows: read by the Unmap tee when the game writes them, looked up first at the eye-0 draw, used only from the pose's own frame");
    check(!wiredAnchorCpuRows(replaced(vscreen, "if (srvTurretWantsCameraRows()) guardedBudget(g_srvTurretCameraBudget, [&] { srvTurretNoteCameraWrite(s->camResource, s->camData, s->camBytes); });", ""), module),
          "control: the Unmap tee never handing the rows over trips the pin");
    check(!wiredAnchorCpuRows(vscreen, replaced(module, "if (w->hostSeq != hostSeq) return 3;", "")), "control: rows written in another frame paired with this frame's pose trip the pin");
    check(!wiredAnchorCpuRows(vscreen, replaced(module, "int miss = anchorDirCpuRows(ctx, poseSeq, cpuRows, &bytes, &writtenAt);", "int miss = 1;")), "control: the CPU path never tried (the GPU readback's lag) trips the pin");
    check(!wiredAnchorCpuRows(vscreen, replaced(module, "w->hostSeq = nativeTemporalFrameSequence();", "w->hostSeq = 0;")), "control: writes never stamped with their frame trip the pin");
    check(wiredCorrection(vscreen, module), "the draw-time correction: family 7 only and on a radar writer's target, family 8 never, the game's depth, delta from the call the frame's write shows, capped, per eye");
    check(!wiredCorrection(replaced(vscreen, "if (owner && srvTurretRadarWritersOn()) srvTurretRadarWriterDraw(self, bindingShaderHash(BindSlot::Vs), count, instances);", ""), module),
          "control: no writer watch (no billboard can match) trips the pin");
    check(!wiredCorrection(vscreen, replaced(module, "if (family != 7) {", "if (family == 9) {")), "control: a correction not kept to family 7 trips the pin");
    check(!wiredCorrection(vscreen, replaced(module, "        if (kind) g_cc.family8.fetch_add(1, std::memory_order_relaxed);\n        return false;\n", "        if (kind) g_cc.family8.fetch_add(1, std::memory_order_relaxed);\n")),
          "control: family 8 falling through to the move trips the pin");
    check(!wiredCorrection(vscreen, replaced(module, "if (!kind) return leave(kCorrNoMatch);", "(void)kind;")), "control: a family 7 draw moved without a radar writer's t4 (the 512x512 panel hologram) trips the pin");
    check(!wiredCorrection(vscreen, replaced(module, "st::DepthMode::kGame)", "st::DepthMode::kFront)")), "control: a corrected draw that does not keep the game's depth trips the pin");
    check(!wiredCorrection(vscreen, replaced(module, "srvRadarAnchorShownCall(writtenAt, call)", "srvRadarAnchorShownCall(INT64_MAX, call)")),
          "control: delta paired with the newest call, not the one the frame's camera write shows, trips the pin");
    check(!wiredCorrection(vscreen, replaced(module, "if (!(norm <= st::kCorrectionMaxM)) return kCorrOver;", "(void)norm;")), "control: an uncapped correction trips the pin");
    check(wiredGhost(module), "the ghost fix: the holo source first, one disc and one icon an eye a frame, t1 swapped for the far depth only after the bind and put back in End, the far texture at the game's t1 size, the 0.5 m bound");
    check(!wiredGhost(replaced(module, "    farDepthSwap(ctx);   // THE GHOST", "    // THE GHOST")), "control: a moved billboard that keeps the game's t1 (the ghost) trips the pin");
    check(!wiredGhost(replaced(module, "    farDepthRestore(ctx);   // the game's t1", "    // the game's t1")), "control: the game's t1 never put back trips the pin");
    check(!wiredGhost(replaced(module, "    if (family == kHoloFamilyA || family == kHoloFamilyB) holoCollect(ctx, family, eye, g_probe[eye][family].ordinal++);\n", "")),
          "control: the holo source rounds stopped while the correction is on trip the pin");
    check(!wiredGhost(replaced(replaced(module, "    farDepthSwap(ctx);   // THE GHOST", "    // THE GHOST"), "    if (lastFrameOnly) g_cc.lastFrameOnly", "    farDepthSwap(ctx);\n    if (lastFrameOnly) g_cc.lastFrameOnly")),
          "control: t1 swapped before the bind (on a draw that may stay the game's own) trips the pin");
    check(!wiredGhost(replaced(module, "if (g_movedAt[eye][kind - 1] == g_frame + 1u) return leave(kCorrExtra);", "(void)0;")), "control: more than one disc or icon an eye a frame moved trips the pin");
    check(!wiredGhost(replaced(module, "st::depthViewSize(game, w, h) ? g_farDepth.get(ctx, w, h, farDepthClear)", "st::depthViewSize(game, w, h) ? g_farDepth.get(ctx, 2016, 2017, farDepthClear)")),
          "control: a far texture not at the game's t1 size trips the pin");
    check(!wiredGhost(replaced(module, "if (norm > st::kCorrectionLargeM) g_cc.large.fetch_add(1, std::memory_order_relaxed);", "(void)0;")), "control: large corrections never counted trip the pin");
    check(!wiredAnchor(module, anchorHook, replaced(ini, "\nsrv_turret_radar_anchor = on\n", "\nsrv_turret_radar_anchor = off\n"), build, audit), "control: a drifted shipped default of the radar anchor key trips the pin");
    check(!wiredAnchor(module, anchorHook, replaced(ini, "# ui: SRV turret radar anchor | choices on, reversed, off", "# ui: SRV turret radar anchor | choices on, off"), build, audit),
          "control: a drifted menu row of the radar anchor key trips the pin");
    check(!wiredAnchor(module, anchorHook, ini, replaced(build, "\"src\\d3d11\\srv_radar_anchor.cpp\"", ""), audit), "control: the hook outside the compile list trips the pin");
    check(!wiredAnchor(module, anchorHook, ini, build, replaced(audit, "(\"d3d11/srv_radar_anchor.cpp\", \"the SRV radar anchor's exact-build hook\")", "")), "control: the hook not listed as exact-build only trips the pin");
    check(!wiredKeys(ini, flatKeys + "\nsrv_turret_radar_anchor\n"), "control: the radar anchor key on the flat allow-list trips the pin");
    check(wiredBuild(build), "build.bat: the module is in the DLL's compile list and this rig has its label and its run");
    check(!wiredBuild(replaced(build, "\"src\\d3d11\\srv_turret.cpp\"", "")), "control: a module outside the compile list trips the pin");
}

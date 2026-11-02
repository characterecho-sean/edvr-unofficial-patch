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
//     and strips are carried; the six integer keys are read with the literal ranges and defaults the settings generator reads, the other two (the depth mode, default game, and the scissor, default
//     open) as text; a bound draw's scissor rectangles are opened right after the bind and put back at the end of the scope (and settled at the boundary), and a draw is refused while they are owed; a draw is counted and bound only when its family's group has a
//     push, the gap or its own forward key (HUD forward for the panels, radar forward for the radar) above 0 (the group's constants come from one place), the pass is wanted when any of
//     the five HUD keys is above 0, the anchor is stepped by the frame tick with the head from the host's pose and cleared when the pass is not wanted, each eye's frame is built from the
//     host's pose with the tangents AND the jitter shift, and the DRAW rebuilds both eyes' frames from the pose of the frame being drawn (the frame sequence the host publishes in
//     beginFrame, after the previous frame's boundary tick), once for each new sequence.
//   * journal_watch.cpp, native_temporal.cpp: the accessors; the Scorpion latch is fed the tail's new bytes once (not the stitched head the scan reads twice), reset with the
//     file, published to its accessor, and each change said once with its event.
//   * edvr.ini: all ten keys live in [experimental] (demoted from [fix] on 2026-11-01) with their ui: lines, no menu token, and the flown defaults; the flat profile's allow-list names none (VR only).
//   * the radar anchor (experimental.srv_turret_radar_anchor, srv_radar_anchor.cpp): the ninth key, its publish, the billboards it leaves to the game, the setter hook's pins, its compile-list entry and its
//     exact-build audit entry (wiredAnchor); camera mode's slot hook and the radar picture's writers (wiredSlotCamera, wiredWriters); the camera rows the Unmap tee hands the module (wiredAnchorCpuRows).
//   * the first scope's state note: the issues counted at the game's issue and the re-issues, each family's first scope saying its state (wiredStateNote).
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
static std::string replacedLast(std::string t, const std::string& from, const std::string& to) {
    const size_t p = t.rfind(from);
    if (p != std::string::npos) t.replace(p, from.size(), to);
    return t;
}

// vscreen.cpp, forwardWithVerdict.
static bool wiredVscreenScope(const std::string& t) {
    const size_t fn = t.find("void forwardWithVerdict(");
    if (!found(fn)) return false;
    const size_t skip = t.find("if (v == DrawVerdict::kSkip) {", fn);
    const size_t scope = t.find("SrvTurretHudScope turretHud(self);", fn);
    const size_t begin = t.find("if (owner && srvTurretHudWantsDraws()) turretHud.bound = srvTurretHudBeginDraw(g_state, self, kind, count);", fn);
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
    const size_t fn = t.find("bool srvTurretHudBeginDraw(State* s, ID3D11DeviceContext* self, char kind, UINT indexCount) {");
    if (!found(fn)) return false;
    const size_t vs0 = t.find("if (!srv_turret::isFamilyVs(vs)) return false;", fn);
    const size_t family = t.find("srv_turret::familyOf(vs, bindingShaderHash(BindSlot::Ps))", fn);
    const size_t eyeLayer = t.find("uiLayerEyeOfGameTarget(info->resource)", fn);
    const size_t eyeTable = t.find("uiDepthEyeOfTargetReadOnly(info->resource)", fn);
    const size_t eyeReg = t.find("uiDepthEyeOfTarget(info->resource, info->a, info->b, info->fmt)", fn);
    const size_t call = t.find("return srvTurretHudBegin(self, family, eye);", fn);
    return found(vs0) && found(family) && found(eyeLayer) && found(eyeTable) && found(eyeReg) && found(call) &&
           vs0 < family && family < eyeLayer && eyeLayer < eyeTable && eyeTable < eyeReg && eyeReg < call;
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
    const size_t begin = t.find("g_binding.begin(ctx, g_gs[sig], constants, st::directSetGs, needScene ? &scene : nullptr)");
    const size_t finish = t.find("const auto done = g_binding.finish(ctx);");
    const size_t back = t.find("cfg.getIntInRange(\"experimental.srv_turret_camera_back\", 80, 0, 150)");
    const size_t forward = t.find("cfg.getIntInRange(\"experimental.srv_turret_hud_forward\", 0, 0, 150)");
    const size_t radarForward = t.find("cfg.getIntInRange(\"experimental.srv_turret_radar_forward\", 0, 0, 150)");
    const size_t depthKey = t.find("cfg.getString(\"experimental.srv_turret_hud_depth\", \"game\")");
    const size_t scissorKey = t.find("cfg.getString(\"experimental.srv_turret_hud_scissor\", \"game\")");
    const size_t panels = t.find("cfg.getIntInRange(\"experimental.srv_turret_panels_back\", 0, 0, 200)");
    const size_t radar = t.find("cfg.getIntInRange(\"experimental.srv_turret_radar_back\", 0, 0, 200)");
    const size_t gap = t.find("cfg.getIntInRange(\"experimental.srv_turret_hud_gap\", 0, 0, 200)");
    const size_t systems = t.find("cfg.getIntInRange(\"experimental.srv_turret_systems_near\", 70, 0, 200)");
    const size_t hudUp = t.find("cfg.getIntInRange(\"experimental.srv_turret_hud_up\", 10, 0, 100)");
    const size_t gunnerUp = t.find("cfg.getIntInRange(\"experimental.srv_turret_gunner_up\", 0, 0, 100)");
    const size_t settings = t.find("\"srv turret settings: camera back %d cm, hud forward %d cm, radar forward %d cm, panels back %d cm, radar back %d cm, hud gap %d cm, hud depth %s, hud scissor %s, radar anchor %s, systems near %d cm, hud up %d cm, gunner up %d cm (live; \"");
    const size_t edge = t.find("\"srv turret view: %s at frame %u (Status.json Flags 0x%08X, seat kind 0x%X)\"");
    const size_t camera = t.find("\"srv turret camera: back %.2f m published (serial %u)\"");
    const size_t cleared = t.find("\"srv turret camera: back cleared (serial %u)\"");
    const size_t line = t.find("\"srv turret hud: panels: %s; radar: %s; anchor=(%.3f,%.3f,%.3f) head_offset=%.1f cm T=(%.3f,%.3f,%.3f) Tradar=(%.3f,%.3f,%.3f) ipd_elite=%.1fmm P00[L,R]=%.3f,%.3f \"");
    const size_t lineTail = found(line) ? t.find("\"draws[L,R]=%u,%u bound=%u scissor opened %u pose refreshed at draw: %u of %u frames \"", line) : std::string::npos;
    // each group's constants come from one place (the push when above 0, else the pull, else the translation alone, else none), read by the draw, the 10 s line and nothing else; a draw whose
    // group is off is the game's own BEFORE it is counted, and the constants are built on the eye's published frame after it was read; the first-draw line names the direction of the move
    const size_t family = t.find("bool paramsOfGroup(st::Group group, const st::EyeFrame& frame, st::TurretParams& out) {");
    const size_t select = found(family) ? t.find("if (!st::paramsForGroup(frame, group, st::pushCmOf(group, g_panelsCm.load(std::memory_order_relaxed), g_radarCm.load(std::memory_order_relaxed)), g_gapCm.load(std::memory_order_relaxed),", family) : std::string::npos;
    // ...and the forward translation is the group's own key's: HUD forward for the panels, radar forward for the radar; the depth mode of the key is written into whatever was made
    const size_t ownForward = found(select) ? t.find("st::forwardCmOf(group, g_forwardCm.load(std::memory_order_relaxed), g_radarForwardCm.load(std::memory_order_relaxed)), out, group == st::Group::kPanels ? g_systemsNearCm.load(std::memory_order_relaxed) : 0, group == st::Group::kPanels ? g_hudUpCm.load(std::memory_order_relaxed) : 0)", select) : std::string::npos;
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
    const size_t sceneBind = found(sceneRead) ? t.find("g_binding.begin(ctx, g_gs[sig], constants, st::directSetGs, needScene ? &scene : nullptr)", sceneRead) : std::string::npos;
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
    const bool groups = found(family) && found(select) && found(ownForward) && ownForward - select < 300 && found(depthMode) && depthMode - ownForward < 400 && found(groupOff) && found(counted) && found(eyeRead) && found(built) && found(uploaded) && found(windowLn) && select - family < 300 &&
                        found(t.find("\"moves %.2f m deeper\"")) && found(t.find("\"moves %.2f m nearer\"")) && found(t.find("\"does not move radially\"")) && found(t.find("groupWindow(radar, st::Group::kRadar);")) &&
                        !found(t.find("st::paramsFor(geo"));
    // nothing of the game's state is changed but through the binding class
    const bool rawSet = found(t.find("GSSetShader(")) || found(t.find("GSSetConstantBuffers(")) || found(t.find("UpdateSubresource("));
    // the geometry comes from the host's own head and eye matrices and frustum with the tangent shift the jitter adds (the game projects with the sum), the anchor is the tracker's, and a
    // draw with none is refused, counted. Both eyes' frames are built by ONE function, from a pose and an anchor, called by the tick and by the draw
    const size_t buildFn = t.find("void buildEyeFrames(const float head[12], const float eyeToHead[2][12], bool haveAnchor, const double anchor[3], int forwardCm, int radarForwardCm, int nearCm, int hudUpCm, int gunnerUpCm, st::EyeFrame frames[2], bool ok[2]) {");
    const size_t geometry1 = found(buildFn) ? t.find("nativeTemporalEyeGeometry(e, &sequence, tan, shift)", buildFn) : std::string::npos;
    const size_t summed = found(geometry1) ? t.find("const float tangents[4] = {tan[0] + shift[0], tan[1] + shift[0], tan[2] + shift[1], tan[3] + shift[1]};", geometry1) : std::string::npos;
    const size_t framed = found(summed) ? t.find("st::eyeFrameFrom(head, eyeToHead[e], anchor, static_cast<double>(forwardCm) / 100.0, static_cast<double>(radarForwardCm) / 100.0, tangents, frames[e], static_cast<double>(nearCm) / 100.0, static_cast<double>(hudUpCm) / 100.0, static_cast<double>(gunnerUpCm) / 100.0)", summed) : std::string::npos;
    const size_t cacheFn = t.find("void cacheGeometry(bool on, int backCm, int forwardCm, int radarForwardCm) {");
    const size_t pose = found(cacheFn) ? t.find("nativeTemporalHeadPose(&poseSequence, head, eyeToHead)", cacheFn) : std::string::npos;
    const size_t step = found(pose) ? t.find("g_anchor.step(headAt, on, backCm, forwardCm, radarForwardCm)", pose) : std::string::npos;
    const size_t said = found(step) ? t.find("\"srv turret anchor: latched at %.3f,%.3f,%.3f m seated (%s)\"", step) : std::string::npos;
    const size_t tickBuilt = found(said) ? t.find("buildEyeFrames(head, eyeToHead, haveAnchor, g_anchor.anchor(), forwardCm, radarForwardCm, g_systemsNearCm.load(std::memory_order_relaxed), g_hudUpCm.load(std::memory_order_relaxed), g_gunnerUpCm.load(std::memory_order_relaxed), frames, ok);", said) : std::string::npos;
    // the draw: the frame sequence the host published when it took the frame being drawn is compared with the one the eye frames were built from, and the frames are rebuilt from that
    // frame's pose for a new one (the tick runs at the previous boundary, before beginFrame, so its frames are a frame old); the anchor is the tick's, read from atomics
    const size_t eyeFrameFn = t.find("bool drawEyeFrame(int eye, st::EyeFrame& out) {");
    const size_t seqRead = found(eyeFrameFn) ? t.find("const uint64_t seq = nativeTemporalFrameSequence();", eyeFrameFn) : std::string::npos;
    const size_t onceFor = found(seqRead) ? t.find("if (seq != 0 && seq != g_drawSeq) {", seqRead) : std::string::npos;
    const size_t drawPose = found(onceFor) ? t.find("nativeTemporalHeadPose(&poseSequence, head, eyeToHead)", onceFor) : std::string::npos;
    const size_t drawBuilt = found(drawPose) ? t.find("buildEyeFrames(head, eyeToHead, true, anchor, g_forwardCm.load(std::memory_order_relaxed), g_radarForwardCm.load(std::memory_order_relaxed), g_systemsNearCm.load(std::memory_order_relaxed), g_hudUpCm.load(std::memory_order_relaxed), g_gunnerUpCm.load(std::memory_order_relaxed), g_drawFrames, g_drawOk);", drawPose) : std::string::npos;
    const size_t counted2 = found(onceFor) ? t.find("g_poseRefreshed.fetch_add(1, std::memory_order_relaxed);", onceFor) : std::string::npos;
    const bool geometry = found(buildFn) && found(geometry1) && found(summed) && found(framed) && found(cacheFn) && found(pose) && found(step) && found(said) && found(tickBuilt) &&
                          found(eyeFrameFn) && found(seqRead) && found(onceFor) && found(drawPose) && found(drawBuilt) && found(counted2) && found(t.find("refuse(st::Refuse::kNoGeometry)"));
    // the Scorpion only: both parts act on the turret view AND the latch (`on` is what g_backSeatedZ, the gate and the view edges are made from), and the view without it is said
    const size_t tick = t.find("void srvTurretFrameTick(uint32_t frameNo) {");
    const size_t turret = found(tick) ? t.find("const bool turret = journalSrvTurretView();", tick) : std::string::npos;
    const size_t on = found(turret) ? t.find("const bool on = turret && journalSrvScorpion();", turret) : std::string::npos;
    const size_t backZ = found(on) ? t.find("g_backSeatedZ = st::backSeatedZ(on, backCm);", on) : std::string::npos;
    const size_t wanted = found(backZ) ? t.find("const bool wanted = on && (panelsCm > 0 || radarCm > 0 || forwardCm > 0 || radarForwardCm > 0 || gapCm > 0 || systemsNearCm > 0 || hudUpCm > 0 || gunnerUpCm > 0);", backZ) : std::string::npos;
    const size_t cached = found(wanted) ? t.find("cacheGeometry(on, backCm, forwardCm, radarForwardCm);", wanted) : std::string::npos;
    const size_t cleared1 = found(cached) ? t.find("g_anchor.reset();", cached) : std::string::npos;
    const size_t ignored = found(wanted) ? t.find("\"srv turret view: not the Scorpion at frame %u (Status.json Flags 0x%08X, seat kind 0x%X): the turret view is on in an SRV the journal does not call the Scorpion, so the \"", on) : std::string::npos;
    const bool scorpion = found(turret) && found(on) && found(backZ) && found(wanted) && found(cached) && found(cleared1) && found(ignored);
    // lists and strips are carried, one signature's shader missing leaves the others' families bound
    const bool strips = found(t.find("!st::topologyCarried(static_cast<int>(topology))")) && found(t.find("bool ensureGs(ID3D11DeviceContext* ctx, uint32_t sig) {")) &&
                        found(t.find("return sig < kSigN && g_gs[sig] != nullptr;")) && found(t.find("const uint32_t sig = st::kFamilies[family].sig;"));
    return found(made) && found(begin) && found(finish) && found(back) && found(forward) && found(radarForward) && found(depthKey) && sceneOk && scissorOk && found(panels) && found(radar) && found(gap) && found(systems) && found(hudUp) && found(gunnerUp) && found(settings) && found(edge) &&
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

// The first scope's state note (srv_turret.h srvTurretHudCountIssue): the issues are counted at the game's own issue and at the re-issues, and each family's first scope says its state at every issue
// it makes; the line is the one tools/edvr_log.py --health reads.
static bool wiredStateNote(const std::string& vscreen, const std::string& t) {
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
    // each family's first scope says its state at every issue it makes
    const size_t endFn = t.find("void srvTurretHudEnd(ID3D11DeviceContext* ctx) {");
    const size_t noteEnd = found(endFn) ? t.find("noteEnd();", endFn) : std::string::npos;
    return found(t.find("void srvTurretHudNoteIssue(ID3D11DeviceContext* ctx) {")) && found(noteEnd) && found(t.find("if (!g_stateNoted[family]) noteBegin(ctx, family, eye);")) &&
           found(t.find("\"srv turret hud state: family=%s eye=%d issue=%u target=%s %ux%u format=%u depth_view=%s | depth: enable=%d func=%d write=%d | stencil: enable=%d ref=%u read=0x%02X \"")) &&
           found(t.find("| raster: scissor=%d rects=%u first=[%ld,%ld,%ld,%ld] \"")) && found(t.find("\"depth_clip=%d cull=%d front_ccw=%d | viewport: x=%.0f y=%.0f w=%.0f h=%.0f\""));
}

// edvr.ini and the flat allow-list.
static bool wiredKeys(const std::string& ini, const std::string& flatKeys) {
    // [experimental] since 2026-11-01 (demoted from [fix]): the last section of the file, so a key is in it when it lies after the header and before any later one. The generator refuses a
    // `menu` token on an [experimental] key, and each ui: line below is matched through its newline so that one cannot hide after `live`.
    const size_t experimental = ini.find("\n[experimental]\n");
    if (!found(experimental)) return false;
    const size_t nextSection = ini.find("\n[", experimental + 1), end = found(nextSection) ? nextSection : ini.size();
    const size_t back = ini.find("\nsrv_turret_camera_back = 80\n"), forward = ini.find("\nsrv_turret_hud_forward = 0\n"), radarForward = ini.find("\nsrv_turret_radar_forward = 0\n"),
                 panels = ini.find("\nsrv_turret_panels_back = 0\n"), radar = ini.find("\nsrv_turret_radar_back = 0\n"), gap = ini.find("\nsrv_turret_hud_gap = 0\n"),
                 systems = ini.find("\nsrv_turret_systems_near = 70\n"), hudUp = ini.find("\nsrv_turret_hud_up = 10\n"), gunnerUp = ini.find("\nsrv_turret_gunner_up = 0\n"),
                 depth = ini.find("\nsrv_turret_hud_depth = game\n"), scissor = ini.find("\nsrv_turret_hud_scissor = game\n");
    if (!found(back) || !found(forward) || !found(radarForward) || !found(panels) || !found(radar) || !found(gap) || !found(systems) || !found(hudUp) || !found(gunnerUp) || !found(depth) || !found(scissor)) return false;
    const bool inExperimental = back > experimental && back < end && forward > experimental && forward < end && radarForward > experimental && radarForward < end && panels > experimental && panels < end &&
                                radar > experimental && radar < end && gap > experimental && gap < end && systems > experimental && systems < end && hudUp > experimental && hudUp < end && gunnerUp > experimental && gunnerUp < end && depth > experimental && depth < end && scissor > experimental && scissor < end;
    const size_t backUi = ini.rfind("# ui: SRV turret camera back | range 0..150 | step 5 | unit cm | zero exact | recommended 80 | live\n", back),
                 forwardUi = ini.rfind("# ui: SRV turret HUD forward | range 0..150 | step 5 | unit cm | zero exact | recommended 0 | live\n", forward),
                 radarForwardUi = ini.rfind("# ui: SRV turret radar forward | range 0..150 | step 5 | unit cm | zero exact | recommended 0 | live\n", radarForward),
                 panelsUi = ini.rfind("# ui: SRV turret panels back | range 0..200 | step 5 | unit cm | zero exact | recommended 0 | live\n", panels),
                 radarUi = ini.rfind("# ui: SRV turret radar back | range 0..200 | step 5 | unit cm | zero exact | recommended 0 | live\n", radar),
                 gapUi = ini.rfind("# ui: SRV turret HUD gap | range 0..200 | step 2 | unit cm | zero exact | recommended 0 | live\n", gap),
                 systemsUi = ini.rfind("# ui: SRV turret systems near | range 0..200 | step 5 | unit cm | zero exact | recommended 70 | live\n", systems),
                 hudUpUi = ini.rfind("# ui: SRV turret weapon panel up | range 0..100 | step 5 | unit cm | zero exact | recommended 10 | live\n", hudUp),
                 gunnerUpUi = ini.rfind("# ui: SRV turret gunner panel up | range 0..100 | step 5 | unit cm | zero exact | recommended 0 | live\n", gunnerUp),
                 depthUi = ini.rfind("# ui: SRV turret HUD depth | choices front, game, real | recommended game | live\n", depth),
                 scissorUi = ini.rfind("# ui: SRV turret HUD screen limit | choices open, game | recommended game | live\n", scissor);
    const bool ui = found(backUi) && back - backUi < 200 && found(forwardUi) && forward - forwardUi < 200 && found(radarForwardUi) && radarForward - radarForwardUi < 200 && found(panelsUi) && panels - panelsUi < 200 && found(radarUi) && radar - radarUi < 200 &&
                    found(gapUi) && gap - gapUi < 200 && found(systemsUi) && systems - systemsUi < 200 && found(hudUpUi) && hudUp - hudUpUi < 200 && found(gunnerUpUi) && gunnerUp - gunnerUpUi < 200 && found(depthUi) && depth - depthUi < 200 && found(scissorUi) && scissor - scissorUi < 200;
    // VR only: the flat profile's allow-list reads none of the keys
    const bool flatClean = !found(flatKeys.find("srv_turret"));
    return inExperimental && ui && flatClean;
}

// the shader tables and build.bat.
static bool wiredTables(const std::string& shaderBuild, const std::string& variants) {
    bool all = true;
    for (const char letter : std::string("ABCDEFGHI")) {
        const std::string row = std::string("{\"kSrvTurretHudGs") + letter + "Bytecode\",\"srv turret hud pass-through " + letter + "\",\"main\",nullptr,{},false,edvr::kSrvTurretHudGs" + letter +
                                ",\"gs_5_0\"}";
        const std::string contract = std::string("{\"kSrvTurretHudGs") + letter + "Bytecode\", \"srv turret hud pass-through " + letter + "\", edvr::kSrvTurretHudGs" + letter + "}";
        all = all && found(variants.find(row)) && found(shaderBuild.find(contract));
    }
    return all && found(shaderBuild.find("#include \"fixed_srv_turret_variants.h\"")) && found(shaderBuild.find("const auto srvTurret = srvTurretHudVariants();")) &&
           found(shaderBuild.find("variants.insert(variants.end(), srvTurret.begin(), srvTurret.end());")) && found(shaderBuild.find("const TurretContract contracts[stm::kSignatureCount] = {")) &&
           found(variants.find("#include \"../../src/d3d11/srv_turret_hud_shader.h\""));
}
// The radar anchor moved in the game (experimental.srv_turret_radar_anchor; srv_radar_anchor.cpp). srv_turret.cpp: the key read with its literal default and said on the settings line; the frame
// tick publishes the offset made from `on` (and from false in flat); a 5559BD94 draw the game's anchor carries is the game's own BEFORE it is counted; the draw path's switch needs the hook armed, the mode on or
// reversed and the placement holding our offset; the shutdown shuts the gates. The setter hook: build 334925's identity, its prologue and the vtables; its callback ADDS the offset through the pure rule
// (srv_turret_math.h radarPlacementMove) and hands the original the sum, and no code writes the placement fields itself (the first build's replace dropped the radar 1.96 m).
// edvr.ini ships the key in [experimental] with its ui: line (no menu token), default camera; build.bat compiles the file; the address audit lists it as exact-build only.
static bool wiredAnchor(const std::string& module, const std::string& hook, const std::string& ini, const std::string& build, const std::string& audit) {
    const size_t npos = std::string::npos;
    const size_t key = module.find("cfg.getString(\"experimental.srv_turret_radar_anchor\", \"off\")");
    const size_t parsed = found(key) ? module.find("st::radarAnchorModeOf(anchorText.c_str())", key) : npos;
    const size_t tick = module.find("void srvTurretFrameTick(uint32_t frameNo) {");
    const size_t flat = found(tick) ? module.find("anchorTick(false, frameNo, 0);", tick) : npos;
    const size_t onAt = found(tick) ? module.find("const bool on = turret && journalSrvScorpion();", tick) : npos;
    const size_t ticked = found(onAt) ? module.find("anchorTick(on, frameNo, radarForwardCm);", onAt) : npos;
    const size_t made = module.find("w.offset = st::radarAnchorOffset(on, mode, radarForwardCm);");
    const size_t published = found(made) ? module.find("srvRadarAnchorPublish(w.offset.bits, frameNo);", made) : npos;
    const size_t drawFn = module.find("bool srvTurretHudBegin(ID3D11DeviceContext* ctx, int family, int eye) {");
    const size_t groupOff = found(drawFn) ? module.find("if (!paramsOfGroup(group, st::EyeFrame{}, unused)) return false;", drawFn) : npos;
    const size_t leaves = found(groupOff) ? module.find("if (anchorLeaves(family)) return false;", groupOff) : npos;
    const size_t counted = found(groupOff) ? module.find("g_familyDraws[family].fetch_add(1, std::memory_order_relaxed);", groupOff) : npos;
    const size_t yield = module.find("g_anchorYield.store(on && setterMode && a.state == RadarAnchorHookState::Armed && a.holdsOurs, std::memory_order_relaxed);");
    const size_t downFn = module.find("void srvTurretShutdown() {");
    const size_t down = found(downFn) ? module.find("srvRadarAnchorShutdown();", downFn) : npos;
    const bool moduleOk = found(parsed) && found(flat) && found(onAt) && flat < onAt && found(ticked) && found(published) && found(leaves) && found(counted) && leaves < counted && found(yield) && found(down) &&
                          found(module.find("bool anchorLeaves(int family) { return st::radarAnchorLeavesFamily(family) && g_anchorYield.load("));
    // the setter: the pure rule's sum to the original, the game's P for everything else; no store into the placement fields anywhere in the file
    const size_t setterFn = hook.find("uintptr_t __fastcall setterHooked(uintptr_t iface, uintptr_t p, uintptr_t r8, uintptr_t r9) noexcept {");
    const size_t rule = found(setterFn) ? hook.find("const st::PlacementMove r = st::radarPlacementMove(P, d, m, Q);", setterFn) : npos;
    const size_t sum = found(rule) ? hook.find("if (r == st::PlacementMove::kAdded) {", rule) : npos;
    const size_t handed = found(sum) ? hook.find("return forward(iface, reinterpret_cast<uintptr_t>(Q), r8, r9);", sum) : npos;
    const bool adds = found(rule) && found(handed) && hook.find("return", sum) == handed && !found(hook.find("0x15B0")) && !found(hook.find("0x15F0")) && !found(hook.find("volatile uint32_t*"));
    const bool pinned = found(hook.find("constexpr uintptr_t kSetterRva = 0x38563A0;")) && found(hook.find("constexpr uintptr_t kObjectVtableRva = 0x5511DE8;")) &&
                        found(hook.find("constexpr uintptr_t kInterfaceVtableRva = 0x5511F18;")) && found(hook.find("constexpr uint32_t kStamp = 0x6ADA8048u, kImageSize = 0x640C000u;")) &&
                        hook.find("if (stamp != kStamp || size != kImageSize) {", hook.find("RadarAnchorHookState srvRadarAnchorArm(char* whyOut, size_t cap) {")) <
                            hook.find("RadarAnchorHookState srvRadarAnchorState() {") &&   // the setter's own arm checks the build (the slot camera's arm has its own check)
                        found(hook.find("{0x48, 0x83, 0xEC, 0x48, 0x0F, 0x10, 0x02, 0x0F, 0x11, 0x81, 0x28, 0x15, 0x00, 0x00, 0x0F, 0x10, 0x4A, 0x10}")) &&
                        found(hook.find("if (std::memcmp(got, prologue, n) != 0) return")) &&
                        found(hook.find("*reinterpret_cast<const uintptr_t*>(iface) != s.interfaceVtable || *reinterpret_cast<const uintptr_t*>(iface - kInterfaceAt) != s.objectVtable"));
    // the direction: eye 0's draw copies the live camera's rows 277..279 with eye 0's pose of the same frame, the boundary turns them into u and publishes it; the setter adds s * F along u in
    // the anchor's axes (the anchor's getter called only when it lies in the game's image), before the add-and-idempotence rule
    const size_t sampled = found(drawFn) && found(groupOff) ? module.find("if (eye == 0) anchorDirSample(ctx, family);", drawFn) : npos;
    const size_t sampleFn = module.find("void anchorDirSample(ID3D11DeviceContext* ctx, int family) {");
    const size_t pose0 = found(sampleFn) ? module.find("st::eyePoseOf(head, eyeToHead[0], R, E)", sampleFn) : npos;
    const size_t rows = found(pose0) ? module.find("copyVsRows(ctx, g_dir.stage[k], 1, kDirFirstRow, kDirRows, 0, first, count)", pose0) : npos;
    const size_t boundaryFn = module.find("void srvTurretHudFrameBoundary(ID3D11DeviceContext* ctx) {");
    const bool direction = found(sampled) && sampled < groupOff && found(rows) && found(module.find("st::turretForwardWorld(rows, rows + 4, rows + 8, g_dir.R[k], u)")) &&
                           found(module.find("srvRadarAnchorPublishDirection(best, true, g_dir.hostSeqOf[bestSlot], false, g_dir.headPitchOf[bestSlot], g_dir.qpcOf[bestSlot]);")) && found(boundaryFn) &&
                           found(module.find("anchorDirBoundary(ctx);", boundaryFn)) && found(module.find("srvRadarAnchorPublishDirection(nullptr, false);"));
    // the setter pairs the sample with the anchor's rotation of the sample's own host frame (kept by frame) and uses it as it is: NO lead (an unled aim keeps a pairing error a smooth
    // lag, not a jitter)
    const size_t kept = found(setterFn) ? hook.find("s->aRing.put(nowSeq, rot);", setterFn) : npos;
    const size_t pairedK = found(kept) ? hook.find("s->aRing.find(smp.seq)", kept) : npos;
    const size_t along = found(pairedK) ? hook.find("st::radarAnchorAlong(u, ak, 1.0f, unitD, uaK)", pairedK) : npos;
    const size_t unled = found(along) ? hook.find("std::memcpy(ua, uaK, sizeof(ua));", along) : npos;
    const size_t carried = found(unled) ? hook.find("for (int i = 0; i < 3; ++i) d[i] = bitsOf(sF * ua[i]);", unled) : npos;
    const size_t getterCheck = hook.find("if (!insideModule(s, getter)) return 0;");
    const size_t getterCall = hook.find("reinterpret_cast<MatrixFn>(getter)(anchor)");
    const bool directed = found(kept) && found(pairedK) && found(along) && found(unled) && found(carried) && found(rule) && carried < rule && !found(hook.find("leadAim")) &&
                          !found(hook.find("extrapolateAim")) && found(getterCheck) && found(getterCall) && getterCheck < getterCall &&
                          found(hook.find("const uint64_t nowSeq = nativeTemporalFrameSequence();"));
    const size_t experimental = ini.find("\n[experimental]\n"), shipped = ini.find("\nsrv_turret_radar_anchor = off\n");
    const size_t nextSection = found(experimental) ? ini.find("\n[", experimental + 1) : npos, end = found(nextSection) ? nextSection : ini.size();
    const size_t ui = found(shipped) ? ini.rfind("# ui: SRV turret radar anchor | choices on, reversed, off, camera | recommended off | live\n", shipped) : npos;
    const bool iniOk = found(experimental) && found(shipped) && shipped > experimental && shipped < end && found(ui) && shipped - ui < 200;
    const size_t source = build.find("\"src\\d3d11\\srv_radar_anchor.cpp\"");
    const bool buildOk = found(source) && source < build.find("ERROR: compile failed");
    const bool auditOk = found(audit.find("(\"d3d11/srv_radar_anchor.cpp\", \"the SRV radar anchor's exact-build hooks: the placement setter, the slot camera\")"));
    return found(key) && moduleOk && adds && pinned && direction && directed && iniOk && buildOk && auditOk;
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
    const size_t cpuFirst = found(sampleFn) ? module.find("if (anchorDirCpuRows(ctx, poseSeq, cpuRows, &writtenAt)) {", sampleFn) : npos;
    const size_t cpuPublished = found(cpuFirst) ? module.find("srvRadarAnchorPublishDirection(u, true, poseSeq, true, headPitch, writtenAt);", cpuFirst) : npos;
    const size_t gpuCopy = found(sampleFn) ? module.find("copyVsRows(ctx, g_dir.stage[k], 1, kDirFirstRow, kDirRows, 0, first, count)", sampleFn) : npos;
    const bool moduleOk = found(copied) && found(stamped) && found(module.find("w->qpc = now.QuadPart;", stamped)) && found(module.find("kCamRowsFloat = kDirFirstRow * 4")) && found(cpuFirst) && found(cpuPublished) && found(gpuCopy) &&
                          cpuFirst < gpuCopy && found(module.find("if (w->hostSeq != hostSeq) return false;")) && found(module.find("if (cb.ranged && cb.first != 0) return false;"));
    return teeOk && moduleOk;
}
// The radar picture's writers (camera mode's billboard match): vscreen.cpp's draw path hands every owner-context draw to srvTurretRadarWriterDraw while the writers are watched (camera mode), the module
// records the terrain schematic's and the Scorpion icon's (a 256x256 target) render targets for the frame, and billboardKind matches a billboard's t4 against this frame's writers alone.
static bool wiredWriters(const std::string& vscreen, const std::string& module) {
    const size_t npos = std::string::npos;
    const size_t watch = vscreen.find("if (owner && srvTurretRadarWritersOn()) srvTurretRadarWriterDraw(self, bindingShaderHash(BindSlot::Vs));");
    const size_t kindFn = module.find("int billboardKind(ID3D11DeviceContext* ctx) {");
    const size_t t4 = found(kindFn) ? module.find("ctx->PSGetShaderResources(4, 1, &srv)", kindFn) : npos;
    // this frame's writers only match a billboard
    const size_t strict = found(t4) ? module.find("const int kind = res ? kindOf(g_writers, res) : 0;", t4) : npos;
    if (!found(strict) || found(module.find("g_writersPrev"))) return false;
    const size_t writerFn = module.find("void srvTurretRadarWriterDraw(ID3D11DeviceContext* ctx, uint64_t vs) {");
    const size_t icon256 = found(writerFn) ? module.find("td.Width == 256 && td.Height == 256", writerFn) : npos;
    return found(watch) && found(t4) && found(icon256) && found(module.find("constexpr uint64_t kRadarIconVs = 0x5B0068AF5630F96Bull;")) &&
           found(module.find("constexpr uint64_t kSchematicTerrainVs = 0x4EDE4BC9003BE352ull;"));
}
// CAMERA MODE (flight fe0b2560; srv_turret_math.h THE RADAR'S OWN CAMERA): the mode publishes a zero setter offset (the restore gives the game its placement back) and no yield (the setter's yield is for
// on and reversed); the slot hook is pinned to build 334925 (the stamp and size, the RVA, the slot vtable, the 21 prologue bytes) and records only (no logging, no allocation); it
// hands the original the moved copy only for a radar slot (st::radarSlotTest) and the game's camera otherwise; the copy is rebased, then moved, with VP marked dirty; the frame tick
// publishes D from the radar forward key and turns the writer watch on (camera mode alone); the draw path sends the radar's own picture billboards through the regular radar path and leaves the rest.
static bool wiredSlotCamera(const std::string& module, const std::string& hook, const std::string& math) {
    const size_t npos = std::string::npos;
    const bool zero = found(math.find("if (!on || mode == RadarAnchorMode::kOff || mode == RadarAnchorMode::kCamera || radarForwardCm <= 0) return o;")) &&
                      found(math.find("if (text && !std::strcmp(text, \"camera\")) return RadarAnchorMode::kCamera;"));
    const bool dirty = found(math.find("inline constexpr uint32_t kCamVpDirty = 0x8;")) && found(math.find("    flags |= kCamVpDirty;\n")) &&
                       found(math.find("view[12 + j] -= d[0] * view[j] + d[1] * view[4 + j] + d[2] * view[8 + j];"));
    const size_t armFn = hook.find("RadarAnchorHookState srvRadarSlotArm() {");
    const size_t stamp = found(armFn) ? hook.find("if (stamp != kStamp || size != kImageSize) {", armFn) : npos;
    const size_t patched = found(stamp) ? hook.find("installHook(s->slot, target, kSlotPrologue, kSlotPrologueBytes, reinterpret_cast<const void*>(&slotHooked), \"srv-radar-slot\");", stamp) : npos;
    const bool pinned = found(hook.find("constexpr uintptr_t kSlotRva = 0x36BF3C0;")) && found(hook.find("constexpr uintptr_t kSlotVtableRva = 0x54FB7F8;")) &&
                        found(hook.find("{0x40, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x10, 0xF9, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xF0, 0x07, 0x00, 0x00}")) && found(patched) &&
                        found(hook.find("s->slotVtable = base + kSlotVtableRva;"));
    const size_t fn = hook.find("uintptr_t __fastcall slotHooked(uintptr_t slot, uintptr_t cam, uintptr_t r8, uintptr_t r9) noexcept {");
    const size_t tested = found(fn) ? hook.find("const int radar = st::radarSlotTest(slot, s->slotVtable, s->interfaceVtable, &sehRead, nullptr, hit);", fn) : npos;
    const size_t notRadar = found(tested) ? hook.find("if (radar <= 0) return forward(slot, cam, r8, r9);", tested) : npos;
    const size_t copied = found(notRadar) ? hook.find("if (!guardedRead(cam, copy, sizeof(copy))) {", notRadar) : npos;
    const size_t rebased = found(copied) ? hook.find("st::cameraRebase(copy, cam);", copied) : npos;
    const size_t moved = found(rebased) ? hook.find("st::cameraMoveBy(copy, d);", rebased) : npos;
    const size_t handed = found(moved) ? hook.find("return forward(slot, reinterpret_cast<uintptr_t>(copy), r8, r9);", moved) : npos;
    size_t hands = 0;
    for (size_t at = hook.find("reinterpret_cast<uintptr_t>(copy)"); found(at); at = hook.find("reinterpret_cast<uintptr_t>(copy)", at + 1)) ++hands;
    const std::string body = found(fn) && found(handed) ? hook.substr(fn, handed - fn) : std::string();
    const bool quiet = !body.empty() && !found(body.find("Log::")) && !found(body.find("printf")) && !found(body.find("new ")) && !found(body.find("malloc"));
    const bool ticked = found(module.find("srvRadarSlotPublish(st::radarCameraBackM(on, mode, radarForwardCm));")) &&
                        found(module.find("detail::g_srvTurretRadarWritersOn.store(cameraMode, std::memory_order_relaxed);")) &&
                        found(module.find("g_anchorYield.store(on && setterMode && "));
    const size_t routed = module.find("    if (anchorLeaves(family)) return false;\n    if (cameraLeaves(ctx, family)) return false;");
    const bool regular = found(routed) && found(module.find("    if (family == kRadarFamily && billboardKind(ctx) != 0) return false;\n")) &&
                         found(module.find("if (!st::radarAnchorLeavesFamily(family) || !g_cameraBillboards.load(std::memory_order_relaxed)) return false;"));
    return zero && dirty && pinned && found(handed) && hands == 1 && quiet && ticked && regular;
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
    const std::string mathText = readText("src/d3d11/srv_turret_math.h");
    check(vscreen.size() > 100000 && onFoot.size() > 100000 && module.size() > 5000 && journal.size() > 20000 && temporal.size() > 20000 && ini.size() > 20000 && flatKeys.size() > 500 &&
              build.size() > 50000 && shaderBuild.size() > 20000 && variants.size() > 100 && anchorHook.size() > 2000 && audit.size() > 50000,
          "the wiring scan read its sources (the repo root is the working directory)");

    check(wiredVscreenScope(vscreen), "vscreen.cpp: the scope opens after the skipped draw's return and before the layer's decision and every issue of the draw");
    check(!wiredVscreenScope(replaced(vscreen, "SrvTurretHudScope turretHud(self);", "")), "control: no scope trips the pin");
    check(!wiredVscreenScope(replaced(replaced(vscreen, "SrvTurretHudScope turretHud(self);\n    if (owner && srvTurretHudWantsDraws()) turretHud.bound = srvTurretHudBeginDraw(g_state, self, kind, count);", ""),
                                      "const bool originalIssued=observedDraw(", "SrvTurretHudScope turretHud(self);\n    if (owner && srvTurretHudWantsDraws()) turretHud.bound = srvTurretHudBeginDraw(g_state, self, kind, count);\n    const bool originalIssued=observedDraw(")),
          "control: a scope that opens after the layer's bracket (just before the game's issue) trips the pin");
    check(!wiredVscreenScope(replaced(vscreen, "if (owner && srvTurretHudWantsDraws()) turretHud.bound", "if (srvTurretHudWantsDraws()) turretHud.bound")), "control: begun on a foreign context trips the pin");
    check(wiredVscreenBegin(vscreen), "vscreen.cpp: the draw is matched on BOTH shaders of the pair, then the eye is the layer's, the table's read-only form, then the registering form");
    check(!wiredVscreenBegin(replaced(vscreen, "srv_turret::familyOf(vs, bindingShaderHash(BindSlot::Ps))", "0")), "control: the vertex shader alone (no pair match) trips the pin");
    check(!wiredVscreenBegin(replaced(vscreen, "uiLayerEyeOfGameTarget(info->resource);\n            if (eye != 0 && eye != 1) eye = uiDepthEyeOfTargetReadOnly(info->resource);", "-1;")), "control: no eye lookup trips the pin");
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
    check(!wiredModule(replaced(module, "const auto begun = g_binding.begin(ctx, g_gs[sig], constants, st::directSetGs, needScene ? &scene : nullptr);", "ctx->GSSetShader(g_gs[sig], nullptr, 0); const auto begun = g_binding.begin(ctx, g_gs[sig], constants, st::directSetGs, needScene ? &scene : nullptr);")),
          "control: a raw geometry-stage set outside the binding trips the pin");
    check(!wiredModule(replaced(module, "\"experimental.srv_turret_hud_gap\", 0, 0, 200", "\"experimental.srv_turret_hud_gap\", 96, 0, 200")), "control: a drifted default of the gap trips the pin");
    check(!wiredModule(replaced(module, "\"experimental.srv_turret_panels_back\", 0, 0, 200", "\"experimental.srv_turret_panels_back\", 100, 0, 200")), "control: a drifted default of the panels' push trips the pin");
    check(!wiredModule(replaced(module, "\"experimental.srv_turret_radar_back\", 0, 0, 200", "\"experimental.srv_turret_radar_back\", 0, 0, 150")), "control: a drifted range of the radar's push trips the pin");
    check(!wiredModule(replaced(module, "\"experimental.srv_turret_hud_forward\", 0, 0, 150", "\"experimental.srv_turret_hud_forward\", 60, 0, 150")), "control: a drifted default of the HUD-forward key trips the pin");
    check(!wiredModule(replaced(module, "\"experimental.srv_turret_camera_back\", 80, 0, 150", "\"experimental.srv_turret_camera_back\", 60, 0, 150")), "control: the camera key back at 60 trips the pin");
    check(!wiredModule(replaced(module, "if (!paramsOfGroup(group, st::EyeFrame{}, unused)) return false;", "")), "control: a draw counted and refused before its group is known to be on trips the pin");
    check(!wiredModule(replaced(module, "if (!paramsOfGroup(group, frame, params)) return false;", "params = st::paramsFor(frame, 0.96f);")), "control: constants made from the pull alone (the push ignored) trip the pin");
    check(!wiredModule(replaced(module, "\"moves %.2f m deeper\"", "\"moves %.2f m nearer\"")), "control: a first-draw line that calls a push 'nearer' trips the pin");
    check(!wiredModule(replaced(module, "g_anchor.step(headAt, on, backCm, forwardCm, radarForwardCm)", "g_anchor.step(headAt, on, backCm, backCm, radarForwardCm)")), "control: an anchor that does not see the HUD-forward key trips the pin");
    check(!wiredModule(replaced(module, "g_anchor.step(headAt, on, backCm, forwardCm, radarForwardCm)", "g_anchor.step(headAt, on, backCm, forwardCm, forwardCm)")), "control: an anchor that does not see the radar-forward key trips the pin");
    check(!wiredModule(replaced(module, "\"experimental.srv_turret_radar_forward\", 0, 0, 150", "\"experimental.srv_turret_radar_forward\", 60, 0, 150")), "control: a drifted default of the radar-forward key trips the pin");
    check(!wiredModule(replaced(module, "cfg.getString(\"experimental.srv_turret_hud_depth\", \"game\")", "cfg.getString(\"experimental.srv_turret_hud_depth\", \"front\")")), "control: a drifted default of the depth key (game) trips the pin");
    check(!wiredModule(replaced(module, "cfg.getString(\"experimental.srv_turret_hud_scissor\", \"game\")", "cfg.getString(\"experimental.srv_turret_hud_scissor\", \"open\")")), "control: a drifted default of the scissor key (game) trips the pin");
    check(!wiredModule(replaced(module, "const auto opened = g_scissor.open(ctx, seen, apply);", "const auto opened = st::Scissor::Open::kOff;")), "control: a scissor that is never opened trips the pin");
    check(!wiredModule(replaced(module, "if (!g_scissor.restore(ctx) && !g_scissorRestoreNoted) {", "if (false) {")), "control: the game's scissor rectangles never put back trip the pin");
    check(!wiredModule(replaced(module, "g_scissor.owed() || !ensureGs(ctx, sig)", "!ensureGs(ctx, sig)")), "control: a draw issued while the scissor rectangles are owed back trips the pin");
    check(!wiredModule(replaced(module, "if (g_scissor.owed() && ctx) (void)g_scissor.settle(ctx);", "")), "control: owed scissor rectangles never settled at the boundary trip the pin");
    check(!wiredModule(replaced(module, "scissor opened %u pose refreshed at draw: %u of %u frames", "pose refreshed at draw: %u of %u frames")), "control: the 10 s line without the scissor count trips the pin");
    check(!wiredModule(replaced(module, "out = st::withDepth(out, static_cast<st::DepthMode>(g_depthMode.load(std::memory_order_relaxed)));", "")), "control: constants that never get the depth key's mode trip the pin");
    check(!wiredModule(replaced(module, "const bool needScene = sig == st::kSceneSignature;", "const bool needScene = false;")), "control: the Scorpion hologram's draw without the vertex constants trips the pin");
    check(!wiredModule(replaced(module, "if (needScene && !st::readGameCb(ctx, st::kGameCbSlot, scene)) {", "if (false) {")), "control: the vertex constants never read trip the pin");
    check(!wiredModule(replaced(module, ", st::directSetGs, needScene ? &scene : nullptr)", ")")), "control: the vertex constants read but never bound trip the pin");
    check(!wiredModule(replaced(module, "\"experimental.srv_turret_radar_forward\", 0, 0, 150", "\"experimental.srv_turret_radar_forward\", 0, 0, 100")), "control: a drifted range of the radar-forward key trips the pin");
    check(!wiredModule(replaced(module, "\"experimental.srv_turret_systems_near\", 70, 0, 200", "\"experimental.srv_turret_systems_near\", 40, 0, 200")), "control: a drifted default of the systems-near key trips the pin");
    check(!wiredModule(replaced(module, "\"experimental.srv_turret_hud_up\", 10, 0, 100", "\"experimental.srv_turret_hud_up\", 20, 0, 100")), "control: a drifted default of the weapon-panel height key trips the pin");
    check(!wiredModule(replaced(module, "\"experimental.srv_turret_gunner_up\", 0, 0, 100", "\"experimental.srv_turret_gunner_up\", 20, 0, 100")), "control: a drifted default of the gunner-panel height key trips the pin");
    check(!wiredModule(replaced(module, "st::forwardCmOf(group, g_forwardCm.load(std::memory_order_relaxed), g_radarForwardCm.load(std::memory_order_relaxed)), out, group == st::Group::kPanels ? g_systemsNearCm.load(std::memory_order_relaxed) : 0, group == st::Group::kPanels ? g_hudUpCm.load(std::memory_order_relaxed) : 0)", "g_forwardCm.load(std::memory_order_relaxed), out)")),
          "control: the radar group made to use the HUD-forward distance (the thin sliver) trips the pin");
    check(!wiredModule(replaced(module, "if (!drawEyeFrame(eye, frame)) {", "if (!readEye(eye, frame)) {")), "control: a draw that uses the tick's frames (a frame old) trips the pin");
    check(!wiredModule(replaced(module, "const uint64_t seq = nativeTemporalFrameSequence();", "const uint64_t seq = g_tickSeq.load(std::memory_order_acquire);")),
          "control: a draw that never sees a newer frame sequence than the tick built from trips the pin");
    check(!wiredModule(replaced(module, "if (seq != 0 && seq != g_drawSeq) {", "if (seq != 0) {")), "control: a rebuild on every draw (not once for each new sequence) trips the pin");
    check(!wiredModule(replaced(module, "g_poseRefreshed.fetch_add(1, std::memory_order_relaxed);", "")), "control: no count of the frames whose pose was refreshed at draw trips the pin");
    check(!wiredModule(replaced(module, "buildEyeFrames(head, eyeToHead, true, anchor, g_forwardCm.load(std::memory_order_relaxed), g_radarForwardCm.load(std::memory_order_relaxed), g_systemsNearCm.load(std::memory_order_relaxed), g_hudUpCm.load(std::memory_order_relaxed), g_gunnerUpCm.load(std::memory_order_relaxed), g_drawFrames, g_drawOk);", "")),
          "control: a draw that never rebuilds the eye frames trips the pin");
    check(!wiredModule(replaced(module, "pose refreshed at draw: %u of %u frames", "pose refreshed")), "control: the 10 s line without the pose-refresh count trips the pin");
    check(!wiredModule(replaced(module, "Tradar=(%.3f,%.3f,%.3f) ", "")), "control: the 10 s line without the radar group's T trips the pin");
    check(!wiredModule(replaced(module, "const float tangents[4] = {tan[0] + shift[0], tan[1] + shift[0], tan[2] + shift[1], tan[3] + shift[1]};", "const float tangents[4] = {tan[0], tan[1], tan[2], tan[3]};")),
          "control: tangents without the jitter shift (the game projects with the sum) trip the pin");
    check(!wiredModule(replaced(module, "        g_anchor.reset();", "        (void)0;")), "control: an anchor that survives the pass being off trips the pin");
    check(!wiredModule(without(module, "refuse(st::Refuse::kNoGeometry)")), "control: a draw with no geometry that is not refused trips the pin");
    check(!wiredModule(replaced(module, "const bool on = turret && journalSrvScorpion();", "const bool on = turret;")), "control: both parts acting on the turret view alone (the Scarab's too) trips the pin");
    check(!wiredModule(replaced(module, "g_backSeatedZ = st::backSeatedZ(on, backCm);", "g_backSeatedZ = st::backSeatedZ(turret, backCm);")), "control: a camera term made from the turret view, not the Scorpion's, trips the pin");
    check(!wiredModule(replaced(module, "const bool wanted = on && (panelsCm > 0 || radarCm > 0 || forwardCm > 0 || radarForwardCm > 0 || gapCm > 0 || systemsNearCm > 0 || hudUpCm > 0 || gunnerUpCm > 0);", "const bool wanted = turret && (panelsCm > 0 || radarCm > 0 || forwardCm > 0 || radarForwardCm > 0 || gapCm > 0 || systemsNearCm > 0);")),
          "control: a HUD pass made from the turret view, not the Scorpion's, trips the pin");
    check(!wiredModule(replaced(module, "const bool wanted = on && (panelsCm > 0 || radarCm > 0 || forwardCm > 0 || radarForwardCm > 0 || gapCm > 0 || systemsNearCm > 0 || hudUpCm > 0 || gunnerUpCm > 0);", "const bool wanted = on && gapCm > 0;")),
          "control: a pass wanted by the gap alone (the push keys and the forward keys cannot open it) trips the pin");
    check(!wiredModule(replaced(module, "const bool wanted = on && (panelsCm > 0 || radarCm > 0 || forwardCm > 0 || radarForwardCm > 0 || gapCm > 0 || systemsNearCm > 0 || hudUpCm > 0 || gunnerUpCm > 0);", "const bool wanted = on && (panelsCm > 0 || radarCm > 0 || forwardCm > 0 || gapCm > 0 || systemsNearCm > 0);")),
          "control: a pass the radar-forward key alone cannot open trips the pin");
    check(!wiredModule(replaced(module, "const bool wanted = on && (panelsCm > 0 || radarCm > 0 || forwardCm > 0 || radarForwardCm > 0 || gapCm > 0 || systemsNearCm > 0 || hudUpCm > 0 || gunnerUpCm > 0);", "const bool wanted = on && (panelsCm > 0 || radarCm > 0 || forwardCm > 0 || radarForwardCm > 0 || gapCm > 0);")),
          "control: a pass the systems-near key alone cannot open trips the pin");
    check(!wiredModule(replaced(module, "const bool wanted = on && (panelsCm > 0 || radarCm > 0 || forwardCm > 0 || radarForwardCm > 0 || gapCm > 0 || systemsNearCm > 0 || hudUpCm > 0 || gunnerUpCm > 0);", "const bool wanted = on && (panelsCm > 0 || radarCm > 0 || forwardCm > 0 || radarForwardCm > 0 || gapCm > 0 || systemsNearCm > 0);")),
          "control: a pass the height keys alone cannot open trips the pin");
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
    check(wiredStateNote(vscreen, module), "the first scope's state note: issues counted at the game's issue and the re-issues, each family's first scope says its state at every issue it makes");
    check(!wiredStateNote(replaced(vscreen, "    srvTurretHudCountIssue(self);   // issue 140: a re-issue inside a family's first scope is an issue of the matched draw, and its state is said (one relaxed load otherwise)",
                                   "    // issue 140: a re-issue inside a family's first scope is an issue of the matched draw"),
                          module),
          "control: a re-issue that is not counted trips the pin");
    check(!wiredStateNote(replaced(vscreen, "        srvTurretHudCountIssue(self);   // issue 140: the game's own issue; a family's first scope says its state (one relaxed load otherwise)",
                                   "        // issue 140: the game's own issue"),
                          module),
          "control: a thunk that does not count the game's own issue trips the pin");
    check(!wiredStateNote(replaced(vscreen, "        const bool issued = draw(altered);\n", "        const bool issued = draw(altered);\n        srvTurretHudCountIssue(self);\n"), module),
          "control: a count inside observedDraw (whose text other rigs pin) trips the pin");
    check(!wiredStateNote(vscreen, replaced(module, "if (!g_stateNoted[family]) noteBegin(ctx, family, eye);", "")), "control: a family's first scope that never says its state trips the pin");
    check(wiredAccessors(journal, temporal, journalH), "journal_watch and native_temporal: the turret bit and the eye separation accessors");
    check(!wiredAccessors(replaced(journal, "srv_turret::turretViewFromFlags(peek(g_v.flags))", "false"), temporal, journalH), "control: an accessor that answers false trips the pin");
    check(!wiredAccessors(journal, replaced(temporal, "g_frameSequence.store(f->sequence,std::memory_order_release);", ""), journalH), "control: a frame sequence that is never published trips the pin");
    check(!wiredAccessors(journal, replaced(replaced(temporal, "  g_frameSequence.store(f->sequence,std::memory_order_release);   // last: the head, eyes, frusta and shift above are this frame's\n", ""),
                                            "  edvr::lightGridCacheNoteJitter(s->frameCounter,jx,jy,s->shift);", "  g_frameSequence.store(f->sequence,std::memory_order_release);\n  edvr::lightGridCacheNoteJitter(s->frameCounter,jx,jy,s->shift);"), journalH),
          "control: a frame sequence published before the shift is copied (a reader would see a half-built frame) trips the pin");
    check(!wiredAccessors(journal, replaced(temporal, "g_recommended.store(0,std::memory_order_release);g_frameSequence.store(0,std::memory_order_release);", "g_recommended.store(0,std::memory_order_release);"), journalH),
          "control: a frame sequence that survives an invalidate trips the pin");
    check(wiredKeys(ini, flatKeys), "edvr.ini: the ten keys in [experimental] with their ui: lines (no menu token) and the flown defaults; the flat profile's allow-list names none");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_hud_gap = 0\n", "\nsrv_turret_hud_gap = 96\n"), flatKeys), "control: a drifted shipped default of the gap trips the pin");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_panels_back = 0\n", "\nsrv_turret_panels_back = 100\n"), flatKeys), "control: a drifted shipped default of the panels' push trips the pin");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_hud_forward = 0\n", "\nsrv_turret_hud_forward = 60\n"), flatKeys), "control: a drifted shipped default of the HUD-forward key trips the pin");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_radar_forward = 0\n", "\nsrv_turret_radar_forward = 60\n"), flatKeys), "control: a drifted shipped default of the radar-forward key trips the pin");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_camera_back = 80\n", "\nsrv_turret_camera_back = 0\n"), flatKeys), "control: a drifted shipped default of the camera key trips the pin");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_systems_near = 70\n", "\nsrv_turret_systems_near = 40\n"), flatKeys), "control: a drifted shipped default of the systems-near key trips the pin");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_hud_up = 10\n", "\nsrv_turret_hud_up = 20\n"), flatKeys), "control: a drifted shipped default of the weapon-panel height key trips the pin");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_gunner_up = 0\n", "\nsrv_turret_gunner_up = 20\n"), flatKeys), "control: a drifted shipped default of the gunner-panel height key trips the pin");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_hud_depth = game\n", "\nsrv_turret_hud_depth = front\n"), flatKeys), "control: a drifted shipped default of the depth key trips the pin");
    check(!wiredKeys(replaced(ini, "# ui: SRV turret HUD depth | choices front, game, real | recommended game", "# ui: SRV turret HUD depth | choices front, game, real | recommended front"), flatKeys), "control: a drifted recommendation of the depth key trips the pin");
    check(!wiredKeys(replaced(ini, "recommended 80 | live\n", "recommended 80 | live | menu\n"), flatKeys), "control: a menu token on an [experimental] key (the generator refuses it) trips the pin");
    check(!wiredKeys(replaced(ini, "\n[experimental]\n", "\n[fix]\n"), flatKeys), "control: the keys outside [experimental] trip the pin");
    check(!wiredKeys(ini, flatKeys + "\nsrv_turret_hud_depth\n"), "control: the depth key on the flat allow-list trips the pin");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_hud_scissor = game\n", "\nsrv_turret_hud_scissor = open\n"), flatKeys), "control: a drifted shipped default of the scissor key trips the pin");
    check(!wiredKeys(replaced(ini, "# ui: SRV turret HUD screen limit | choices open, game", "# ui: SRV turret HUD screen limit | choices open"), flatKeys), "control: a drifted menu row of the scissor key trips the pin");
    check(!wiredKeys(ini, flatKeys + "\nsrv_turret_hud_scissor\n"), "control: the scissor key on the flat allow-list trips the pin");
    check(!wiredKeys(replaced(ini, "# ui: SRV turret radar forward | range 0..150 | step 5", "# ui: SRV turret radar forward | range 0..150 | step 10"), flatKeys), "control: a drifted menu row of the radar-forward key trips the pin");
    check(!wiredKeys(ini, flatKeys + "\nsrv_turret_radar_forward\n"), "control: the radar-forward key on the flat allow-list trips the pin");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_camera_back = 80\n", "\nsrv_turret_camera_back = 60\n"), flatKeys), "control: the camera key shipped at 60 trips the pin");
    check(!wiredKeys(ini, flatKeys + "\nsrv_turret_systems_near\n"), "control: the systems-near key on the flat allow-list trips the pin");
    check(!wiredKeys(replaced(ini, "# ui: SRV turret radar back | range 0..200 | step 5", "# ui: SRV turret radar back | range 0..200 | step 10"), flatKeys), "control: a drifted menu row of the radar's push trips the pin");
    check(!wiredKeys(ini, flatKeys + "\nsrv_turret_hud_gap\n"), "control: the gap key on the flat allow-list trips the pin");
    check(!wiredKeys(ini, flatKeys + "\nsrv_turret_radar_back\n"), "control: the radar key on the flat allow-list trips the pin");
    check(wiredTables(shaderBuild, variants), "temporal_shader_build: the HLSL is included and the variant is registered as a gs_5_0");
    check(!wiredTables(shaderBuild, replaced(variants, "\"gs_5_0\"", "\"vs_5_0\"")), "control: a vertex-shader profile trips the pin");
    check(wiredAnchor(module, anchorHook, ini, build, audit), "the radar anchor: the key, its publish from the frame tick, the billboards left to the game before they are counted, the hooks' pins, "
                                                              "the setter's add through the pure rule, the ini row, the compile list and the audit's exact-only entry");
    check(!wiredAnchor(replaced(module, "cfg.getString(\"experimental.srv_turret_radar_anchor\", \"off\")", "cfg.getString(\"experimental.srv_turret_radar_anchor\", \"camera\")"), anchorHook, ini, build, audit),
          "control: a drifted default of the radar anchor key trips the pin");
    check(!wiredAnchor(replaced(module, "anchorTick(on, frameNo, radarForwardCm);", "anchorTick(turret, frameNo, radarForwardCm);"), anchorHook, ini, build, audit),
          "control: an offset published for the turret view alone (the Scarab's too) trips the pin");
    check(!wiredAnchor(replaced(module, "anchorTick(false, frameNo, 0);", ""), anchorHook, ini, build, audit), "control: a flat profile that leaves an offset in the game trips the pin");
    check(!wiredAnchor(replaced(module, "    if (anchorLeaves(family)) return false;", ""), anchorHook, ini, build, audit), "control: billboards moved twice (by the game and the shader) trip the pin");
    check(!wiredAnchor(replaced(replaced(module, "    if (anchorLeaves(family)) return false;", ""), "g_familyDraws[family].fetch_add(1, std::memory_order_relaxed);",
                                "g_familyDraws[family].fetch_add(1, std::memory_order_relaxed);\n    if (anchorLeaves(family)) return false;"),
                       anchorHook, ini, build, audit),
          "control: a billboard left to the game but counted as a draw of the pass trips the pin");
    check(!wiredAnchor(replaced(module, " && a.holdsOurs, std::memory_order_relaxed);", ", std::memory_order_relaxed);"), anchorHook, ini, build, audit),
          "control: billboards left to the game while the placement does not hold our offset trip the pin");
    check(!wiredAnchor(replaced(module, "    srvRadarAnchorShutdown();", ""), anchorHook, ini, build, audit), "control: a gate left open at unload trips the pin");
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
    check(!wiredAnchor(module, replaced(anchorHook, "std::memcpy(ua, uaK, sizeof(ua));", "std::memcpy(ua, uaK, sizeof(ua)); leadAim(ua);"), ini, build, audit),
          "control: a setter that leads the sample again (the shake of a8465cb3) trips the pin");
    check(wiredAnchorCpuRows(vscreen, module), "the radar anchor's camera rows: read by the Unmap tee when the game writes them, looked up first at the eye-0 draw, used only from the pose's own frame");
    check(!wiredAnchorCpuRows(replaced(vscreen, "if (srvTurretWantsCameraRows()) guardedBudget(g_srvTurretCameraBudget, [&] { srvTurretNoteCameraWrite(s->camResource, s->camData, s->camBytes); });", ""), module),
          "control: the Unmap tee never handing the rows over trips the pin");
    check(!wiredAnchorCpuRows(vscreen, replaced(module, "if (w->hostSeq != hostSeq) return false;", "")), "control: rows written in another frame paired with this frame's pose trip the pin");
    check(!wiredAnchorCpuRows(vscreen, replaced(module, "if (anchorDirCpuRows(ctx, poseSeq, cpuRows, &writtenAt)) {", "if (false) {")), "control: the CPU path never tried (the GPU readback's lag) trips the pin");
    check(!wiredAnchorCpuRows(vscreen, replaced(module, "w->hostSeq = nativeTemporalFrameSequence();", "w->hostSeq = 0;")), "control: writes never stamped with their frame trip the pin");
    check(wiredWriters(vscreen, module), "the radar picture's writers: the terrain schematic's and the Scorpion icon's render targets recorded from the draw path, a billboard matched on this frame's alone");
    check(!wiredWriters(replaced(vscreen, "if (owner && srvTurretRadarWritersOn()) srvTurretRadarWriterDraw(self, bindingShaderHash(BindSlot::Vs));", ""), module),
          "control: no writer watch (no billboard can match) trips the pin");
    check(wiredSlotCamera(module, anchorHook, mathText), "camera mode: a zero setter offset and no yield, the slot hook pinned to build 334925 and quiet, the moved copy for a radar slot only, rebased then moved with VP dirty, D published, the radar's own billboards on the regular path");
    check(!wiredSlotCamera(module, anchorHook, replaced(mathText, "mode == RadarAnchorMode::kCamera || radarForwardCm <= 0) return o;", "radarForwardCm <= 0) return o;")), "control: camera mode that still moves the placement trips the pin");
    check(!wiredSlotCamera(replaced(module, "on && setterMode && a.state", "on && a.state"), anchorHook, mathText), "control: camera mode with the anchor yield trips the pin");
    check(!wiredSlotCamera(module, replaced(anchorHook, "if (radar <= 0) return forward(slot, cam, r8, r9);", "(void)radar;"), mathText), "control: the moved camera handed to a slot that is not the radar's trips the pin");
    check(!wiredSlotCamera(module, anchorHook, replaced(mathText, "    flags |= kCamVpDirty;\n", "")), "control: the moved camera without VP marked dirty trips the pin");
    check(!wiredSlotCamera(module, replaced(anchorHook, "    st::cameraRebase(copy, cam);\n", ""), mathText), "control: a copy whose plane pointers still point into the game's camera trips the pin");
    check(!wiredSlotCamera(module, replaced(anchorHook, "0x48, 0x81, 0xEC, 0xF0, 0x07, 0x00, 0x00}", "0x48, 0x81, 0xEC, 0xF0, 0x08, 0x00, 0x00}"), mathText), "control: a drifted slot prologue trips the pin");
    check(!wiredSlotCamera(module, replaced(anchorHook, "constexpr uintptr_t kSlotRva = 0x36BF3C0;", "constexpr uintptr_t kSlotRva = 0x36BF3D0;"), mathText), "control: a drifted slot address trips the pin");
    check(!wiredSlotCamera(module, replacedLast(anchorHook, "if (stamp != kStamp || size != kImageSize) {", "if (false) {"), mathText), "control: a slot hook armed without the build check trips the pin");
    check(!wiredSlotCamera(module, replaced(anchorHook, "    const float back = floatOf(s->slotBack.load(std::memory_order_relaxed));\n", "    const float back = floatOf(s->slotBack.load(std::memory_order_relaxed));\n    Log::get().note(\"slot\");\n"), mathText),
          "control: a slot hook that logs on the game thread trips the pin");
    check(!wiredSlotCamera(replaced(module, "    if (family == kRadarFamily && billboardKind(ctx) != 0) return false;\n", ""), anchorHook, mathText), "control: camera mode leaving the radar's own billboards to the game trips the pin");
    check(!wiredSlotCamera(replaced(module, "srvRadarSlotPublish(st::radarCameraBackM(on, mode, radarForwardCm));", "srvRadarSlotPublish(0.0f);"), anchorHook, mathText), "control: no back distance published trips the pin");
    check(!wiredAnchor(module, anchorHook, replaced(ini, "\nsrv_turret_radar_anchor = off\n", "\nsrv_turret_radar_anchor = camera\n"), build, audit), "control: a drifted shipped default of the radar anchor key trips the pin");
    check(!wiredAnchor(module, anchorHook, replaced(ini, "# ui: SRV turret radar anchor | choices on, reversed, off, camera", "# ui: SRV turret radar anchor | choices on, reversed, off"), build, audit),
          "control: a drifted menu row of the radar anchor key trips the pin");
    check(!wiredAnchor(module, anchorHook, replaced(ini, "recommended off | live\n", "recommended on | live\n"), build, audit), "control: a drifted recommendation of the radar anchor key trips the pin");
    check(!wiredAnchor(module, anchorHook, replaced(ini, "recommended off | live\n", "recommended off | live | menu\n"), build, audit), "control: a menu token on the radar anchor key (the generator refuses it) trips the pin");
    check(!wiredAnchor(module, anchorHook, replaced(ini, "\n[experimental]\n", "\n[fix]\n"), build, audit), "control: the radar anchor key outside [experimental] trips the pin");
    check(!wiredAnchor(module, anchorHook, ini, replaced(build, "\"src\\d3d11\\srv_radar_anchor.cpp\"", ""), audit), "control: the hook outside the compile list trips the pin");
    check(!wiredAnchor(module, anchorHook, ini, build, replaced(audit, "(\"d3d11/srv_radar_anchor.cpp\", \"the SRV radar anchor's exact-build hooks: the placement setter, the slot camera\")", "")), "control: the hook not listed as exact-build only trips the pin");
    check(!wiredKeys(ini, flatKeys + "\nsrv_turret_radar_anchor\n"), "control: the radar anchor key on the flat allow-list trips the pin");
    check(wiredBuild(build), "build.bat: the module is in the DLL's compile list and this rig has its label and its run");
    check(!wiredBuild(replaced(build, "\"src\\d3d11\\srv_turret.cpp\"", "")), "control: a module outside the compile list trips the pin");
}

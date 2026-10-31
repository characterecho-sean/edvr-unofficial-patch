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
//     and strips are carried; the six keys are read with the literal ranges and defaults the settings generator reads; a draw is counted and bound only when its family's group has a
//     push, the gap or its own forward key (HUD forward for the panels, radar forward for the radar) above 0 (the group's constants come from one place), the pass is wanted when any of
//     the five HUD keys is above 0, the anchor is stepped by the frame tick with the head from the host's pose and cleared when the pass is not wanted, each eye's frame is built from the
//     host's pose with the tangents AND the jitter shift, and the DRAW rebuilds both eyes' frames from the pose of the frame being drawn (the frame sequence the host publishes in
//     beginFrame, after the previous frame's boundary tick), once for each new sequence.
//   * journal_watch.cpp, native_temporal.cpp: the accessors; the Scorpion latch is fed the tail's new bytes once (not the stitched head the scan reads twice), reset with the
//     file, published to its accessor, and each change said once with its event.
//   * edvr.ini: all six keys live in [fix] with their ui: lines and shipped defaults; the flat profile's allow-list names none (VR only).
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
    const size_t call = t.find("return srvTurretHudBegin(self, family, eye);", fn);
    return found(vs0) && found(family) && found(eyeLayer) && found(eyeTable) && found(eyeReg) && found(call) && vs0 < family && family < eyeLayer && eyeLayer < eyeTable &&
           eyeTable < eyeReg && eyeReg < call;
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
    const size_t begin = t.find("g_binding.begin(ctx, twin ? g_soGs[sig] : g_gs[sig], constants)");
    const size_t finish = t.find("const auto done = g_binding.finish(ctx);");
    const size_t back = t.find("cfg.getIntInRange(\"fix.srv_turret_camera_back\", 0, 0, 150)");
    const size_t forward = t.find("cfg.getIntInRange(\"fix.srv_turret_hud_forward\", 70, 0, 150)");
    const size_t radarForward = t.find("cfg.getIntInRange(\"fix.srv_turret_radar_forward\", 70, 0, 150)");
    const size_t panels = t.find("cfg.getIntInRange(\"fix.srv_turret_panels_back\", 120, 0, 200)");
    const size_t radar = t.find("cfg.getIntInRange(\"fix.srv_turret_radar_back\", 0, 0, 200)");
    const size_t gap = t.find("cfg.getIntInRange(\"fix.srv_turret_hud_gap\", 0, 0, 200)");
    const size_t settings = t.find("\"srv turret settings: camera back %d cm, hud forward %d cm, radar forward %d cm, panels back %d cm, radar back %d cm, hud gap %d cm (live; fix.srv_turret_camera_back, \"");
    const size_t edge = t.find("\"srv turret view: %s at frame %u (Status.json Flags 0x%08X, seat kind 0x%X)\"");
    const size_t camera = t.find("\"srv turret camera: back %.2f m published (serial %u)\"");
    const size_t cleared = t.find("\"srv turret camera: back cleared (serial %u)\"");
    const size_t line = t.find("\"srv turret hud: panels: %s; radar: %s; anchor=(%.3f,%.3f,%.3f) head_offset=%.1f cm T=(%.3f,%.3f,%.3f) Tradar=(%.3f,%.3f,%.3f) ipd_elite=%.1fmm P00[L,R]=%.3f,%.3f \"");
    const size_t lineTail = found(line) ? t.find("\"draws[L,R]=%u,%u bound=%u pose refreshed at draw: %u of %u frames \"", line) : std::string::npos;
    // each group's constants come from one place (the push when above 0, else the pull, else the translation alone, else none), read by the draw, the 10 s line and nothing else; a draw whose
    // group is off is the game's own BEFORE it is counted, and the constants are built on the eye's published frame after it was read; the first-draw line names the direction of the move
    const size_t family = t.find("bool paramsOfGroup(st::Group group, const st::EyeFrame& frame, st::TurretParams& out) {");
    const size_t select = found(family) ? t.find("return st::paramsForGroup(frame, group, st::pushCmOf(group, g_panelsCm.load(std::memory_order_relaxed), g_radarCm.load(std::memory_order_relaxed)), g_gapCm.load(std::memory_order_relaxed),", family) : std::string::npos;
    // ...and the forward translation is the group's own key's: HUD forward for the panels, radar forward for the radar
    const size_t ownForward = found(select) ? t.find("st::forwardCmOf(group, g_forwardCm.load(std::memory_order_relaxed), g_radarForwardCm.load(std::memory_order_relaxed)), out);", select) : std::string::npos;
    const size_t drawFn = t.find("bool srvTurretHudBegin(ID3D11DeviceContext* ctx, int family, int eye) {");
    const size_t groupOff = found(drawFn) ? t.find("if (!paramsOfGroup(group, st::EyeFrame{}, unused)) return false;", drawFn) : std::string::npos;
    const size_t counted = found(groupOff) ? t.find("g_familyDraws[family].fetch_add(1, std::memory_order_relaxed);", groupOff) : std::string::npos;
    const size_t eyeRead = found(counted) ? t.find("if (!drawEyeFrame(eye, frame)) {", counted) : std::string::npos;
    const size_t built = found(eyeRead) ? t.find("if (!paramsOfGroup(group, frame, params)) return false;", eyeRead) : std::string::npos;
    const size_t uploaded = found(built) ? t.find("g_constants[eye].get(ctx, params)", built) : std::string::npos;
    const size_t windowLn = t.find("groupWindow(panels, st::Group::kPanels);");
    const bool groups = found(family) && found(select) && found(ownForward) && ownForward - select < 300 && found(groupOff) && found(counted) && found(eyeRead) && found(built) && found(uploaded) && found(windowLn) && select - family < 300 &&
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
    return found(made) && found(begin) && found(finish) && found(back) && found(forward) && found(radarForward) && found(panels) && found(radar) && found(gap) && found(settings) && found(edge) &&
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
    const size_t reissueCount = found(reissue) ? vscreen.find("srvTurretHudCountIssue();", reissue) : std::string::npos;
    const size_t reissueSwitch = found(reissue) ? vscreen.find("switch(kind) {", reissue) : std::string::npos;
    // the game's own issue is counted in the four draw thunks' lambdas (after the real call), not in observedDraw, whose text other rigs pin
    size_t counts = 0, at = 0;
    while (found(at = vscreen.find("srvTurretHudCountIssue();", at))) {
        ++counts;
        ++at;
    }
    const size_t observed = vscreen.find("auto observedDraw = [&](AlteredDraw altered) {");
    const size_t observedEnd = found(observed) ? vscreen.find("return issued;", observed) : std::string::npos;
    if (!found(reissueCount) || !found(reissueSwitch) || reissueCount > reissueSwitch || counts != 5 || !found(observed) || !found(observedEnd) ||
        found(vscreen.substr(observed, observedEnd - observed).find("srvTurretHudCountIssue"))) return false;
    const size_t begin = t.find("g_binding.begin(ctx, twin ? g_soGs[sig] : g_gs[sig], constants)");
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
                 panels = ini.find("\nsrv_turret_panels_back = 120\n"), radar = ini.find("\nsrv_turret_radar_back = 0\n"), gap = ini.find("\nsrv_turret_hud_gap = 0\n");
    if (!found(fix) || !found(visual) || !found(back) || !found(forward) || !found(radarForward) || !found(panels) || !found(radar) || !found(gap)) return false;
    const bool inFix = back > fix && back < visual && forward > fix && forward < visual && radarForward > fix && radarForward < visual && panels > fix && panels < visual && radar > fix && radar < visual &&
                       gap > fix && gap < visual;
    const size_t backUi = ini.rfind("# ui: SRV turret camera back | range 0..150 | step 5 | unit cm | zero exact | recommended 0 | live | menu", back),
                 forwardUi = ini.rfind("# ui: SRV turret HUD forward | range 0..150 | step 5 | unit cm | zero exact | recommended 70 | live | menu", forward),
                 radarForwardUi = ini.rfind("# ui: SRV turret radar forward | range 0..150 | step 5 | unit cm | zero exact | recommended 70 | live | menu", radarForward),
                 panelsUi = ini.rfind("# ui: SRV turret panels back | range 0..200 | step 5 | unit cm | zero exact | recommended 120 | live | menu", panels),
                 radarUi = ini.rfind("# ui: SRV turret radar back | range 0..200 | step 5 | unit cm | zero exact | recommended 0 | live | menu", radar),
                 gapUi = ini.rfind("# ui: SRV turret HUD gap | range 0..200 | step 2 | unit cm | zero exact | recommended 0 | live | menu", gap);
    const bool ui = found(backUi) && back - backUi < 200 && found(forwardUi) && forward - forwardUi < 200 && found(radarForwardUi) && radarForward - radarForwardUi < 200 && found(panelsUi) && panels - panelsUi < 200 && found(radarUi) && radar - radarUi < 200 &&
                    found(gapUi) && gap - gapUi < 200;
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
static bool wiredBuild(const std::string& t) {
    const size_t source = t.find("\"src\\d3d11\\srv_turret.cpp\"");
    const size_t compile = t.find("ERROR: compile failed");
    return found(source) && found(compile) && source < compile && found(t.find(":rig_srv_turret_hud_test\n")) && found(t.find("srv_turret_hud_test.exe\" --self-test || exit /b 1"));
}

static void wiringCases() {
    const std::string vscreen = readText("src/d3d11/vscreen.cpp"), onFoot = readText("src/d3d11/native_on_foot.cpp"), module = readText("src/d3d11/srv_turret.cpp"),
                      journal = readText("src/d3d11/journal_watch.cpp"), journalH = readText("src/d3d11/journal_watch.h"), temporal = readText("src/d3d11/native_temporal.cpp"),
                      ini = readText("edvr.ini"), flatKeys = readText("src/common/flat_profile_keys.h"), build = readText("build.bat"),
                      shaderBuild = readText("tools/temporal_shader_build/temporal_shader_build.cpp"), variants = readText("tools/temporal_shader_build/fixed_srv_turret_variants.h");
    check(vscreen.size() > 100000 && onFoot.size() > 100000 && module.size() > 5000 && journal.size() > 20000 && temporal.size() > 20000 && ini.size() > 20000 && flatKeys.size() > 500 &&
              build.size() > 50000 && shaderBuild.size() > 20000 && variants.size() > 100,
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
    check(!wiredModule(replaced(module, "const auto begun = g_binding.begin(ctx, twin ? g_soGs[sig] : g_gs[sig], constants);", "ctx->GSSetShader(g_gs[sig], nullptr, 0); const auto begun = g_binding.begin(ctx, twin ? g_soGs[sig] : g_gs[sig], constants);")),
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
    check(!wiredModule(replaced(module, "\"fix.srv_turret_radar_forward\", 70, 0, 150", "\"fix.srv_turret_radar_forward\", 70, 0, 100")), "control: a drifted range of the radar-forward key trips the pin");
    check(!wiredModule(replaced(module, "st::forwardCmOf(group, g_forwardCm.load(std::memory_order_relaxed), g_radarForwardCm.load(std::memory_order_relaxed)), out);", "g_forwardCm.load(std::memory_order_relaxed), out);")),
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
    check(!wiredInstrument(replaced(vscreen, "    srvTurretHudCountIssue();   // issue 140's GPU instrument: a re-issue", "    // issue 140's GPU instrument: a re-issue"), module), "control: a re-issue that is not counted trips the pin");
    check(!wiredInstrument(replaced(vscreen, "        srvTurretHudCountIssue();   // issue 140's GPU instrument: the game's own issue", "        // issue 140's GPU instrument: the game's own issue"), module), "control: a thunk that does not count the game's own issue trips the pin");
    check(!wiredInstrument(replaced(vscreen, "        const bool issued = draw(altered);\n", "        const bool issued = draw(altered);\n        srvTurretHudCountIssue();\n"), module),
          "control: a count inside observedDraw (whose text other rigs pin) trips the pin");
    check(!wiredInstrument(vscreen, replaced(module, "probeEnd(ctx);   // the queries end", "// the queries end")), "control: queries that never end trip the pin");
    check(!wiredInstrument(vscreen, replaced(module, "kProbeGiveUpFrames = 8,", "kProbeGiveUpFrames = 80,")), "control: a readback that never gives up trips the pin");
    check(!wiredInstrument(vscreen, replaceAll(module, "D3D11_ASYNC_GETDATA_DONOTFLUSH", "0")), "control: a readback that flushes trips the pin");
    check(wiredAccessors(journal, temporal, journalH), "journal_watch and native_temporal: the turret bit and the eye separation accessors");
    check(!wiredAccessors(replaced(journal, "srv_turret::turretViewFromFlags(peek(g_v.flags))", "false"), temporal, journalH), "control: an accessor that answers false trips the pin");
    check(!wiredAccessors(journal, replaced(temporal, "g_frameSequence.store(f->sequence,std::memory_order_release);", ""), journalH), "control: a frame sequence that is never published trips the pin");
    check(!wiredAccessors(journal, replaced(replaced(temporal, "  g_frameSequence.store(f->sequence,std::memory_order_release);   // last: the head, eyes, frusta and shift above are this frame's\n", ""),
                                            "  edvr::lightGridCacheNoteJitter(s->frameCounter,jx,jy,s->shift);", "  g_frameSequence.store(f->sequence,std::memory_order_release);\n  edvr::lightGridCacheNoteJitter(s->frameCounter,jx,jy,s->shift);"), journalH),
          "control: a frame sequence published before the shift is copied (a reader would see a half-built frame) trips the pin");
    check(!wiredAccessors(journal, replaced(temporal, "g_recommended.store(0,std::memory_order_release);g_frameSequence.store(0,std::memory_order_release);", "g_recommended.store(0,std::memory_order_release);"), journalH),
          "control: a frame sequence that survives an invalidate trips the pin");
    check(wiredKeys(ini, flatKeys), "edvr.ini: all six keys in [fix] with their menu rows and shipped defaults; the flat profile's allow-list names none");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_hud_gap = 0\n", "\nsrv_turret_hud_gap = 96\n"), flatKeys), "control: a drifted shipped default of the gap trips the pin");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_panels_back = 120\n", "\nsrv_turret_panels_back = 110\n"), flatKeys), "control: a drifted shipped default of the panels' push trips the pin");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_hud_forward = 70\n", "\nsrv_turret_hud_forward = 0\n"), flatKeys), "control: a drifted shipped default of the HUD-forward key trips the pin");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_radar_forward = 70\n", "\nsrv_turret_radar_forward = 0\n"), flatKeys), "control: a drifted shipped default of the radar-forward key trips the pin");
    check(!wiredKeys(replaced(ini, "# ui: SRV turret radar forward | range 0..150 | step 5", "# ui: SRV turret radar forward | range 0..150 | step 10"), flatKeys), "control: a drifted menu row of the radar-forward key trips the pin");
    check(!wiredKeys(ini, flatKeys + "\nsrv_turret_radar_forward\n"), "control: the radar-forward key on the flat allow-list trips the pin");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_camera_back = 0\n", "\nsrv_turret_camera_back = 60\n"), flatKeys), "control: the camera key shipped at 60 trips the pin");
    check(!wiredKeys(replaced(ini, "# ui: SRV turret radar back | range 0..200 | step 5", "# ui: SRV turret radar back | range 0..200 | step 10"), flatKeys), "control: a drifted menu row of the radar's push trips the pin");
    check(!wiredKeys(ini, flatKeys + "\nsrv_turret_hud_gap\n"), "control: the gap key on the flat allow-list trips the pin");
    check(!wiredKeys(ini, flatKeys + "\nsrv_turret_radar_back\n"), "control: the radar key on the flat allow-list trips the pin");
    check(wiredTables(shaderBuild, variants), "temporal_shader_build: the HLSL is included and the variant is registered as a gs_5_0");
    check(!wiredTables(shaderBuild, replaced(variants, "\"gs_5_0\"", "\"vs_5_0\"")), "control: a vertex-shader profile trips the pin");
    check(wiredBuild(build), "build.bat: the module is in the DLL's compile list and this rig has its label and its run");
    check(!wiredBuild(replaced(build, "\"src\\d3d11\\srv_turret.cpp\"", "")), "control: a module outside the compile list trips the pin");
}

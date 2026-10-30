// The SRV turret view's wiring pins (included by srv_turret_hud_test.cpp; run with --self-test and alone with --wiring).
//
// The production objects are tested on WARP, but the calls that put them into the game live in other files, and a hook that is never called looks like a hook that did
// nothing. Each scan reads the source from the repo root (build.bat runs the rigs there) and asks for the order the design depends on:
//   * vscreen.cpp: the scope opens in forwardWithVerdict after the skipped-draw return and BEFORE the layer's decision and every issue (so the game's own issue, the layer's and
//     the interface depth's re-issues all run inside it), is begun for the owner's draws while the pass is wanted, matches BOTH shaders of the pair, and is a term of the draw
//     gate (the AA-off fast path skips forwardWithVerdict); the boundary tick and the shutdown are there.
//   * native_on_foot.cpp: the frame tick runs just before alignTick; alignTick adds the camera term AFTER the on-foot gate and before the change test, and tells the module what
//     it published right after the one publisher's call.
//   * srv_turret.cpp: the shader is made from the build's bytecode through shaderSwapCreateGs; the module changes no state of the game's but through the binding class; the two
//     keys are read with the literal ranges and defaults the settings generator reads.
//   * journal_watch.cpp, native_temporal.cpp: the accessors.
//   * edvr.ini: both keys live in [fix] with their ui: lines; the flat profile's allow-list names neither (VR only).
//   * the shader tables (tools/temporal_shader_build): the HLSL's header is included and the variant is registered as a gs_5_0.
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
    const size_t vs = t.find("bindingShaderHash(BindSlot::Vs) != srv_turret::kSpriteVs", fn);
    const size_t ps = t.find("bindingShaderHash(BindSlot::Ps) != srv_turret::kSpritePs", fn);
    const size_t eyeLayer = t.find("uiLayerEyeOfGameTarget(info->resource)", fn);
    const size_t eyeTable = t.find("uiDepthEyeOfTargetReadOnly(info->resource)", fn);
    const size_t eyeReg = t.find("uiDepthEyeOfTarget(info->resource, info->a, info->b, info->fmt)", fn);
    const size_t call = t.find("return srvTurretHudBegin(self, eye);", fn);
    return found(vs) && found(ps) && found(eyeLayer) && found(eyeTable) && found(eyeReg) && found(call) && vs < ps && ps < eyeLayer && eyeLayer < eyeTable && eyeTable < eyeReg && eyeReg < call;
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
    const size_t made = t.find("g_gs = shaderSwapCreateGs(ctx, kSrvTurretHudGsBytecode, sizeof(kSrvTurretHudGsBytecode),");
    const size_t begin = t.find("g_binding.begin(ctx, g_gs, constants)");
    const size_t finish = t.find("const auto done = g_binding.finish(ctx);");
    const size_t back = t.find("cfg.getIntInRange(\"fix.srv_turret_camera_back\", 60, 0, 150)");
    const size_t gap = t.find("cfg.getIntInRange(\"fix.srv_turret_hud_gap\", 102, 0, 200)");
    const size_t edge = t.find("\"srv turret view: %s at frame %u (Status.json Flags 0x%08X, seat kind 0x%X)\"");
    const size_t camera = t.find("\"srv turret camera: back %.2f m published (serial %u)\"");
    const size_t cleared = t.find("\"srv turret camera: back cleared (serial %u)\"");
    const size_t line = t.find("\"srv turret hud: gap=%.2f m wMin=%.2f wMax=%.2f s[L,R]=%+.4f,%+.4f ipd_elite=%.1fmm P00[L,R]=%.3f,%.3f draws[L,R]=%u,%u bound=%u \"");
    // nothing of the game's state is changed but through the binding class
    const bool rawSet = found(t.find("GSSetShader(")) || found(t.find("GSSetConstantBuffers(")) || found(t.find("UpdateSubresource("));
    // the geometry comes from the host's own frustum and eye pair, and a draw with none is refused, counted
    const bool geometry = found(t.find("nativeTemporalEyeSeparation(&ipd)")) && found(t.find("nativeTemporalEyeGeometry(e, &sequence, tan, nullptr)")) && found(t.find("refuse(st::Refuse::kNoGeometry)"));
    return found(made) && found(begin) && found(finish) && found(back) && found(gap) && found(edge) && found(camera) && found(cleared) && found(line) && !rawSet && geometry &&
           found(t.find("#include \"temporal_shader_bytecode.h\""));
}
static bool wiredAccessors(const std::string& journal, const std::string& temporal, const std::string& journalH) {
    return found(journal.find("bool journalSrvTurretView() { return !runtimeFlatProfile() && srv_turret::turretViewFromFlags(peek(g_v.flags)); }")) &&
           found(journalH.find("bool journalSrvTurretView();")) && found(temporal.find("bool nativeTemporalEyeSeparation(float* metres) {")) &&
           found(temporal.find("eyeSeparationOf(current->eyes[0], current->eyes[1])"));
}

// edvr.ini and the flat allow-list.
static bool wiredKeys(const std::string& ini, const std::string& flatKeys) {
    const size_t fix = ini.find("\n[fix]\n"), visual = ini.find("\n[visual]\n");
    const size_t back = ini.find("\nsrv_turret_camera_back = 60\n"), gap = ini.find("\nsrv_turret_hud_gap = 102\n");
    if (!found(fix) || !found(visual) || !found(back) || !found(gap)) return false;
    const bool inFix = back > fix && back < visual && gap > fix && gap < visual;
    const size_t backUi = ini.rfind("# ui: SRV turret camera back |", back), gapUi = ini.rfind("# ui: SRV turret HUD gap |", gap);
    const bool ui = found(backUi) && back - backUi < 200 && found(gapUi) && gap - gapUi < 200;
    // VR only: the flat profile's allow-list reads neither key
    const bool flatClean = !found(flatKeys.find("srv_turret"));
    return inFix && ui && flatClean;
}

// the shader tables and build.bat.
static bool wiredTables(const std::string& shaderBuild, const std::string& variants) {
    return found(shaderBuild.find("#include \"fixed_srv_turret_variants.h\"")) && found(shaderBuild.find("const auto srvTurret = srvTurretHudVariants();")) &&
           found(shaderBuild.find("variants.insert(variants.end(), srvTurret.begin(), srvTurret.end());")) &&
           found(variants.find("{\"kSrvTurretHudGsBytecode\",\"srv turret hud pass-through\",\"main\",nullptr,{},false,edvr::kSrvTurretHudGs,\"gs_5_0\"}")) &&
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
    check(!wiredVscreenBegin(replaced(vscreen, "|| bindingShaderHash(BindSlot::Ps) != srv_turret::kSpritePs", "")), "control: the vertex shader alone trips the pin");
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
    check(!wiredModule(replaced(module, "shaderSwapCreateGs(ctx, kSrvTurretHudGsBytecode", "shaderSwapCreateGs(ctx, kSupercruiseBarsGsBytecode")), "control: another shader's bytecode trips the pin");
    check(!wiredModule(replaced(module, "const auto begun = g_binding.begin(ctx, g_gs, constants);", "ctx->GSSetShader(g_gs, nullptr, 0); const auto begun = g_binding.begin(ctx, g_gs, constants);")),
          "control: a raw geometry-stage set outside the binding trips the pin");
    check(!wiredModule(replaced(module, "\"fix.srv_turret_hud_gap\", 102, 0, 200", "\"fix.srv_turret_hud_gap\", 100, 0, 200")), "control: a drifted default trips the pin");
    check(!wiredModule(without(module, "refuse(st::Refuse::kNoGeometry)")), "control: a draw with no geometry that is not refused trips the pin");
    check(wiredAccessors(journal, temporal, journalH), "journal_watch and native_temporal: the turret bit and the eye separation accessors");
    check(!wiredAccessors(replaced(journal, "srv_turret::turretViewFromFlags(peek(g_v.flags))", "false"), temporal, journalH), "control: an accessor that answers false trips the pin");
    check(wiredKeys(ini, flatKeys), "edvr.ini: both keys in [fix] with their menu rows; the flat profile's allow-list names neither");
    check(!wiredKeys(replaced(ini, "\nsrv_turret_hud_gap = 102\n", "\nsrv_turret_hud_gap = 100\n"), flatKeys), "control: a drifted shipped default trips the pin");
    check(!wiredKeys(ini, flatKeys + "\nsrv_turret_hud_gap\n"), "control: the key on the flat allow-list trips the pin");
    check(wiredTables(shaderBuild, variants), "temporal_shader_build: the HLSL is included and the variant is registered as a gs_5_0");
    check(!wiredTables(shaderBuild, replaced(variants, "\"gs_5_0\"", "\"vs_5_0\"")), "control: a vertex-shader profile trips the pin");
    check(wiredBuild(build), "build.bat: the module is in the DLL's compile list and this rig has its label and its run");
    check(!wiredBuild(replaced(build, "\"src\\d3d11\\srv_turret.cpp\"", "")), "control: a module outside the compile list trips the pin");
}

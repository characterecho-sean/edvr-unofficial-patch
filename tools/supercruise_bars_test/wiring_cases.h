// The supercruise bars' wiring pins (included by supercruise_bars_test.cpp; run with --self-test and alone with --wiring).
//
// The production objects are tested on WARP, but the calls that put them around the game's own draw live in other files, and a hook that is never
// called looks like a hook that did nothing. Each scan reads the source from the repo root (build.bat runs the rigs there) and asks for the order
// the design depends on:
//   * vscreen.cpp: the family flag is set from the family rule; Prepare (the game's own viewport) comes BEFORE the layer's bracket and only for a
//     draw the layer decided to take; Begin comes after the bracket (the layer's remapped viewport is what the tent is made for) and before the
//     game's own issue, for a draw the layer actually bracketed; End follows the issue and comes BEFORE the layer's own restore; the family rule's
//     gatherer reads the pixel shader for the two line vertex shaders (the pair is the key); the boundary tick and the shutdown are there.
//   * ui_layer.cpp: the bars' 30 s line follows the orbit lines'.
//   * object_probe.cpp: the aux vertex capture watches the bars' draw whatever its instance count, with a copy cap that holds the 140 vertices and a
//     line that names the offset.
//   * supercruise_bars.cpp: the shader is made from the build's bytecode through shaderSwapCreateGs; the module changes no state of the game's but
//     through the binding class.
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
static bool wiredVscreenBars(const std::string& t) {
    const size_t fn = t.find("void forwardWithVerdict(");
    if (!found(fn)) return false;
    const size_t flag = t.find("barsFamily = uiFamily == UiLayerFamily::kSupercruiseBars;", fn);
    const size_t decided = t.find("const bool barsDecided = barsFamily && uiLayer;", fn);
    const size_t prepare = t.find("if (barsDecided) supercruiseBarsPrepare(self);", fn);
    const size_t layerBegin = found(prepare) ? t.find("const bool layered = uiLayer && uiLayerBegin(self);", prepare) : std::string::npos;
    const size_t begin = t.find("const bool barsBound = layered && barsDecided && supercruiseBarsBegin(self, count);", fn);
    const size_t issue = t.find("const bool originalIssued=observedDraw(", fn);
    const size_t end = t.find("if (barsBound) supercruiseBarsEnd(self);", fn);
    const size_t layerEnd = t.find("        uiLayerEnd(self);\n        if (originalIssued) uiLayerSecondIssues(", fn);
    if (!found(flag) || !found(decided) || !found(prepare) || !found(layerBegin) || !found(begin) || !found(issue) || !found(end) || !found(layerEnd)) return false;
    // the flag is made from the family rule; Prepare (the game's viewport) is before the layer's bracket; Begin (the layer's) is after it and before the game's issue; End is after the issue and before the layer's restore
    if (!(flag < decided && decided < prepare && prepare < layerBegin && layerBegin < begin && begin < issue && issue < end && end < layerEnd)) return false;
    // Begin only for a draw the layer bracketed AND decided to take as the bars
    return found(t.substr(begin, 120).find("layered && barsDecided"));
}
static bool wiredGatherer(const std::string& t) {
    const size_t fn = t.find("UiLayerFamily uiLayerFamilyOf(State* s, char kind, UINT count) {");
    if (!found(fn)) return false;
    const size_t read = t.find("if (f.targetKind == 1 && (f.vs == kHoloTargetSphere || f.vs == kUiVsOrbitLines || f.vs == kUiVsSupercruiseBars || f.vs == kUiVsSpaceDust))", fn);
    const size_t ps = t.find("f.ps = bindingShaderHash(BindSlot::Ps);", fn);
    return found(read) && found(ps) && ps > read && ps - read < 250;
}
static bool wiredBoundary(const std::string& t) {
    const size_t layerTick = t.find("tkUiLayer.run(");
    const size_t orbitTick = t.find("tkOrbitalWidth.run(");
    const size_t barsTick = t.find("tkSupercruiseBars.run([&] { supercruiseBarsFrameBoundary(g_state->ownerCtx); });");
    if (!found(layerTick) || !found(orbitTick) || !found(barsTick) || !(layerTick < barsTick)) return false;
    if (!found(t.find("EDVR_BOUNDARY_TICK(tkSupercruiseBars, \"supercruise_bars\");"))) return false;
    const size_t layerDown = t.find("    uiLayerShutdown();\n");
    const size_t barsDown = t.find("    supercruiseBarsShutdown();\n");
    return found(layerDown) && found(barsDown) && barsDown > layerDown;
}
static bool wiredLayerLog(const std::string& t) {
    const size_t orbit = t.find("    orbitalWidthLog(sceneLineLayerText(UiLayerFamily::kOrbitLines, frames).c_str());");
    const size_t bars = t.find("    supercruiseBarsLog(sceneLineLayerText(UiLayerFamily::kSupercruiseBars, frames).c_str());");
    const size_t dust = t.find("    Log::get().note(\"ui quality: space dust: %s.\", sceneLineLayerText(UiLayerFamily::kSpaceDust, frames).c_str());");
    return found(orbit) && found(bars) && found(dust) && bars > orbit && bars - orbit < 300 && dust > bars && dust - bars < 700 && found(t.find("#include \"supercruise_bars.h\""));
}
static bool wiredProbe(const std::string& t) {
    const size_t cond = t.find("if (instances >= kLedgerAuxMin || d.vs==kOrbitalLineVs || d.vs==kUiVsSupercruiseBars || d.vs==kUiVsSpaceDust) auxCapture(ctx, d.vs, count, instances);");
    const size_t cap = t.find("const uint32_t cap=vs==kOrbitalLineVs ? 256*1024 : (vs==kUiVsSupercruiseBars || vs==kUiVsSpaceDust) ? 1024*1024 : kLedgerAuxBytes;");
    const size_t note = t.find("if(vs==kUiVsSupercruiseBars) Log::get().note(\"eye supercruise bars inputs: frame %u vb%d offset %u stride %u bytes %u (copy cap %u).\"");
    const size_t dustNote = t.find("if(vs==kUiVsSpaceDust) Log::get().note(\"eye space dust inputs: frame %u vb%d offset %u stride %u bytes %u (copy cap %u).\"");
    return found(cond) && found(cap) && found(note) && found(dustNote) && cap < note && note < dustNote && found(t.find("#include \"supercruise_lines.h\""));
}
// eye_draw_snapshot.h: the drawstate watch list names the bars' and the dust's vertex shaders (their b0/b1/b2 go in the dump).
static bool wiredWatch(const std::string& t) {
    const size_t fn = t.find("static bool watches(uint64_t vs) {");
    if (!found(fn)) return false;
    const size_t bars = t.find("case 0xA47A3315FFF5E2E4ull:", fn);
    const size_t dust = t.find("case 0x9BFC7FD232328391ull:", fn);
    // the cases fall through to the list's one `return true;` (the first one in the function is the solar draws' early answer)
    const size_t ret = found(dust) ? t.find("return true;", dust) : std::string::npos;
    const size_t end = t.find("default: return false;", fn);
    return found(bars) && found(dust) && found(ret) && found(end) && bars < ret && dust < ret && ret - dust < 400 && ret < end;
}
static bool wiredModule(const std::string& t) {
    const size_t made = t.find("g_gs = shaderSwapCreateGs(ctx, kSupercruiseBarsGsBytecode, sizeof(kSupercruiseBarsGsBytecode),");
    const size_t begin = t.find("g_binding.begin(ctx, g_gs, constants,");
    const size_t finish = t.find("const auto done = g_binding.finish(ctx);");
    // nothing of the game's state is changed but through the binding class
    const bool rawSet = found(t.find("GSSetShader(")) || found(t.find("GSSetConstantBuffers(")) || found(t.find("RSSetState("));
    return found(made) && found(begin) && found(finish) && !rawSet && found(t.find("#include \"temporal_shader_bytecode.h\""));
}
static bool wiredTables(const std::string& build, const std::string& variants, const std::string& shaderSwap, const std::string& shaderSwapHeader) {
    return found(build.find("#include \"../../src/d3d11/supercruise_bars_shader.h\"")) &&
           found(build.find("const auto supercruise = supercruiseVariants();")) && found(build.find("variants.insert(variants.end(), supercruise.begin(), supercruise.end());")) &&
           found(variants.find("{\"kSupercruiseBarsGsBytecode\", \"supercruise bars strip\", \"main\", nullptr, {}, false, edvr::kSupercruiseBarsGs, \"gs_5_0\"}")) &&
           found(shaderSwap.find("ID3D11GeometryShader* shaderSwapCreateGs(ID3D11DeviceContext* ctx,")) && found(shaderSwap.find("dev->CreateGeometryShader(bytecode, bytecodeLen, nullptr, &out)")) &&
           found(shaderSwapHeader.find("ID3D11GeometryShader* shaderSwapCreateGs(ID3D11DeviceContext* ctx,"));
}
static bool wiredBuild(const std::string& t) {
    const size_t source = t.find("\"src\\d3d11\\supercruise_bars.cpp\"");
    const size_t orbit = t.find("\"src\\d3d11\\orbital_width.cpp\"");
    const size_t compile = t.find("ERROR: compile failed");
    return found(source) && found(orbit) && found(compile) && source > orbit && source - orbit < 80 && source < compile && found(t.find(":rig_supercruise_bars_test\n"));
}

static void wiringCases() {
    const std::string vscreen = readText("src/d3d11/vscreen.cpp"), layer = readText("src/d3d11/ui_layer.cpp"), probe = readText("src/d3d11/object_probe.cpp"),
                      module = readText("src/d3d11/supercruise_bars.cpp"), build = readText("build.bat"),
                      shaderBuild = readText("tools/temporal_shader_build/temporal_shader_build.cpp"),
                      variants = readText("tools/temporal_shader_build/fixed_core_shader_variants.h"), swapCpp = readText("src/d3d11/shader_swap.cpp"),
                      swapH = readText("src/d3d11/shader_swap.h");
    check(vscreen.size() > 100000 && layer.size() > 100000 && probe.size() > 50000 && module.size() > 5000 && build.size() > 50000 && shaderBuild.size() > 20000 && variants.size() > 3000 &&
              swapCpp.size() > 3000 && swapH.size() > 1000,
          "the sources are read from the repo root");

    check(wiredVscreenBars(vscreen), "vscreen.cpp: the family flag, Prepare before the layer's bracket, Begin after it and before the game's issue (for a bracketed bars draw), End after the issue and before the layer's restore");
    check(!wiredVscreenBars(without(vscreen, "if (barsDecided) supercruiseBarsPrepare(self);")), "control: no Prepare trips the pin");
    check(!wiredVscreenBars(without(vscreen, "if (barsBound) supercruiseBarsEnd(self);")), "control: no End after the game's issue trips the pin");
    check(!wiredVscreenBars(replaced(vscreen, "const bool barsBound = layered && barsDecided && supercruiseBarsBegin(self, count);", "const bool barsBound = barsDecided && supercruiseBarsBegin(self, count);")),
          "control: a Begin that does not ask whether the layer bracketed the draw trips the pin");
    check(!wiredVscreenBars(replaced(vscreen, "const bool barsBound = layered && barsDecided && supercruiseBarsBegin(self, count);", "const bool barsBound = layered && barsFamily && supercruiseBarsBegin(self, count);")),
          "control: a Begin for every bars draw, taken or not, trips the pin");
    check(!wiredVscreenBars(replaced(replaced(vscreen, "    if (barsBound) supercruiseBarsEnd(self);", ""), "        if (originalIssued) uiLayerSecondIssues(self, kind, count, instances, args);\n    }\n",
                                     "        if (originalIssued) uiLayerSecondIssues(self, kind, count, instances, args);\n    }\n    if (barsBound) supercruiseBarsEnd(self);\n")),
          "control: End after the layer's own restore trips the pin");
    check(!wiredVscreenBars(replaced(vscreen, "barsFamily = uiFamily == UiLayerFamily::kSupercruiseBars;", "barsFamily = uiFamily == UiLayerFamily::kOrbitLines;")),
          "control: a flag made from the wrong family trips the pin");

    check(wiredGatherer(vscreen), "vscreen.cpp: the family rule's gatherer reads the pixel shader for the two line vertex shaders (the pair is the key)");
    check(!wiredGatherer(replaced(vscreen, "f.vs == kHoloTargetSphere || f.vs == kUiVsOrbitLines || f.vs == kUiVsSupercruiseBars || f.vs == kUiVsSpaceDust", "f.vs == kHoloTargetSphere || f.vs == kUiVsOrbitLines || f.vs == kUiVsSpaceDust")),
          "control: a gatherer that never reads the bars' pixel shader trips the pin");
    check(!wiredGatherer(replaced(vscreen, "f.vs == kHoloTargetSphere || f.vs == kUiVsOrbitLines || f.vs == kUiVsSupercruiseBars || f.vs == kUiVsSpaceDust", "f.vs == kHoloTargetSphere || f.vs == kUiVsOrbitLines || f.vs == kUiVsSupercruiseBars")),
          "control: ...nor the space dust's");
    check(wiredBoundary(vscreen), "vscreen.cpp: the boundary tick follows the layer's, and the shutdown follows the layer's");
    check(!wiredBoundary(without(vscreen, "tkSupercruiseBars.run([&] { supercruiseBarsFrameBoundary(g_state->ownerCtx); });")), "control: no boundary tick trips the pin");
    check(!wiredBoundary(without(vscreen, "    supercruiseBarsShutdown();\n")), "control: no shutdown trips the pin");

    check(wiredLayerLog(layer), "ui_layer.cpp: the bars' 30 s line follows the orbit lines' and carries the layer's half");
    check(!wiredLayerLog(without(layer, "    supercruiseBarsLog(sceneLineLayerText(UiLayerFamily::kSupercruiseBars, frames).c_str());")), "control: no bars log call trips the pin");
    check(!wiredLayerLog(without(layer, "    Log::get().note(\"ui quality: space dust: %s.\", sceneLineLayerText(UiLayerFamily::kSpaceDust, frames).c_str());")), "control: no space-dust line trips the pin");

    check(wiredProbe(probe), "object_probe.cpp: the aux vertex capture watches the bars' draw whatever its instance count, with a cap that holds its 140 vertices and a line naming the offset");
    check(!wiredProbe(replaced(probe, "|| d.vs==kUiVsSupercruiseBars || d.vs==kUiVsSpaceDust) auxCapture", "|| d.vs==kUiVsSpaceDust) auxCapture")), "control: a capture that does not watch the bars trips the pin");
    check(!wiredProbe(replaced(probe, "|| d.vs==kUiVsSupercruiseBars || d.vs==kUiVsSpaceDust) auxCapture", "|| d.vs==kUiVsSupercruiseBars) auxCapture")),
          "control: a capture that does not watch the space dust (one instance, under the 50 the aux capture asks for) trips the pin");
    check(!wiredProbe(replaced(probe, "(vs==kUiVsSupercruiseBars || vs==kUiVsSpaceDust) ? 1024*1024 : kLedgerAuxBytes", "kLedgerAuxBytes")), "control: a copy cap of 64 KB (the vertices may sit past it) trips the pin");
    check(!wiredProbe(without(probe, "if(vs==kUiVsSupercruiseBars) Log::get().note(\"eye supercruise bars inputs")), "control: no offset line trips the pin");
    check(!wiredProbe(without(probe, "if(vs==kUiVsSpaceDust) Log::get().note(\"eye space dust inputs")), "control: no space-dust offset line trips the pin");

    const std::string snapshot = readText("src/d3d11/eye_draw_snapshot.h");
    check(snapshot.size() > 50000, "eye_draw_snapshot.h is read from the repo root");
    check(wiredWatch(snapshot), "eye_draw_snapshot.h: the drawstate watch list names the bars' (A47A3315FFF5E2E4) and the space dust's (9BFC7FD232328391) vertex shaders");
    check(!wiredWatch(without(snapshot, "case 0xA47A3315FFF5E2E4ull:")), "control: a watch list without the bars trips the pin");
    check(!wiredWatch(without(snapshot, "case 0x9BFC7FD232328391ull:")), "control: a watch list without the space dust trips the pin");

    check(wiredModule(module), "supercruise_bars.cpp: the shader from the build's bytecode, the binding's Begin and End, and no state set but through the binding class");
    check(!wiredModule(replaced(module, "g_gs = shaderSwapCreateGs(ctx, kSupercruiseBarsGsBytecode", "g_gs = shaderSwapCreateVs(ctx, kSupercruiseBarsGsBytecode")), "control: a shader made another way trips the pin");
    check(!wiredModule(replaced(module, "const auto done = g_binding.finish(ctx);", "const auto done = g_binding.finish(ctx);ctx->GSSetShader(g_gs, nullptr, 0);")),
          "control: a raw geometry-stage set outside the binding trips the pin");
    check(!wiredModule(replaced(module, "const auto done = g_binding.finish(ctx);", "const auto done = g_binding.finish(ctx);ctx->RSSetState(nullptr);")), "control: a raw rasterizer-state set trips the pin");

    check(wiredTables(shaderBuild, variants, swapCpp, swapH), "the shader tables: the HLSL header is included, the variant is a gs_5_0 appended to the fixed list, and shaderSwapCreateGs is defined and declared");
    check(!wiredTables(without(shaderBuild, "variants.insert(variants.end(), supercruise.begin(), supercruise.end());"), variants, swapCpp, swapH), "control: a variant never appended trips the pin");
    check(!wiredTables(shaderBuild, replaced(variants, "edvr::kSupercruiseBarsGs, \"gs_5_0\"", "edvr::kSupercruiseBarsGs, \"vs_5_0\""), swapCpp, swapH), "control: another profile trips the pin");
    check(!wiredTables(shaderBuild, variants, replaced(swapCpp, "dev->CreateGeometryShader(bytecode, bytecodeLen, nullptr, &out)", "dev->CreateVertexShader(bytecode, bytecodeLen, nullptr, nullptr)"), swapH), "control: a creation call that makes another stage trips the pin");

    check(wiredBuild(build), "build.bat: the module is in the DLL's compile list beside the orbit lines' and this rig has its label");
    check(!wiredBuild(without(build, "\"src\\d3d11\\supercruise_bars.cpp\"")), "control: a module left out of the DLL trips the pin");
    check(!wiredBuild(replaced(build, ":rig_supercruise_bars_test\n", ":rig_other\n")), "control: a rig without its label trips the pin");
}

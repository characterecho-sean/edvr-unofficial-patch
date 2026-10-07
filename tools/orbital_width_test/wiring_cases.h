// The orbit lines' wiring pins (included by orbital_width_test.cpp; run with --self-test and alone with --wiring).
//
// The production objects above are tested on WARP, but the calls that put them around the game's own draw live in four
// other files, and a hook that is never called looks like a hook that did nothing. Each scan below reads the source from
// the repo root (build.bat runs the rigs there) and asks for the order the design depends on:
//   * vscreen.cpp: Begin is asked before the game's own issue and only for the owner's draw into an eye target whose
//     bound vertex shader is the orbit-line shader; End follows the issue and comes BEFORE the coverage twin binds its
//     own shader (the twin saves the shader that is bound: it must be the game's); the frame boundary's tick follows the
//     layer's (the panel patch's factor is settled by then); the shutdown rolls it up.
//   * device_hook.cpp: the creation hook offers every new vertex shader's bytes to the module, after the shader is
//     registered, saying whether it was linked.
//   * ui_depth.cpp: the twin binds its private b13 (made from the same factor) wherever it binds its shader, saves the
//     slot first and gives it back where it gives the shader back; a missing buffer declines the twin.
//   * ui_layer.cpp: the module's 30 s line follows the panels'.
//   * stellar_coverage.h: the twin reads its half-width from b13.
//   * build.bat: the module is in the DLL's compile list and this rig has its label.
// Each pin has controls: an edited copy of the real text must trip it.
#pragma once

static std::string readText(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    t.erase(std::remove(t.begin(), t.end(), '\r'), t.end());  // build.bat is CRLF on disk, the sources LF
    return t;
}

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
// The first `from` at or after `anchor` (an edit of one statement, not of an earlier look-alike).
static std::string replacedAfter(std::string t, const std::string& anchor, const std::string& from, const std::string& to) {
    const size_t a = t.find(anchor);
    if (a == std::string::npos) return t;
    const size_t p = t.find(from, a);
    if (p != std::string::npos) t.replace(p, from.size(), to);
    return t;
}
// The text from `from` up to (not including) `to`, cut out and put straight after `after`.
static std::string movedAfter(std::string t, const std::string& from, const std::string& to, const std::string& after) {
    const size_t a = t.find(from);
    if (a == std::string::npos) return t;
    const size_t b = t.find(to, a);
    const size_t c = t.find(after, a);
    if (b == std::string::npos || c == std::string::npos) return t;
    const std::string piece = t.substr(a, b - a);
    t.erase(a, b - a);
    const size_t c2 = t.find(after, a);
    t.insert(c2 + after.size(), piece);
    return t;
}

static bool wiredVscreen(const std::string& t) {
    const size_t fn = t.find("void forwardWithVerdict(");
    if (!found(fn)) return false;
    const size_t gate = t.find("const bool orbitScaled = ", fn);
    const size_t begin = t.find("orbitalWidthBegin(self, instances, args.startInstance)", fn);
    const size_t issue = t.find("const bool originalIssued=observedDraw(", fn);
    const size_t end = t.find("if (orbitScaled) orbitalWidthEnd(self);", fn);
    const size_t twin = t.find("uiDepthReissueBegin(self)", fn);
    if (!found(gate) || !found(begin) || !found(issue) || !found(end) || !found(twin)) return false;
    if (!(gate < begin && begin < issue && issue < end && end < twin)) return false;
    const std::string condition = t.substr(gate, begin - gate);
    if (!found(condition.find("owner")) || !found(condition.find("g_state->rtv0Eye")) || !found(condition.find("bindingShaderHash(BindSlot::Vs)")) ||
        !found(condition.find("orbital_width::kVs")))
        return false;
    const size_t layerTick = t.find("tkUiLayer.run(");
    const size_t orbitTick = t.find("tkOrbitalWidth.run([&] { orbitalWidthFrameBoundary(g_state->ownerCtx); });");
    if (!found(layerTick) || !found(orbitTick) || orbitTick < layerTick) return false;
    if (!found(t.find("EDVR_BOUNDARY_TICK(tkOrbitalWidth, \"orbital_width\");"))) return false;
    const size_t layerDown = t.find("    uiLayerShutdown();\n");
    const size_t orbitDown = t.find("    orbitalWidthShutdown();\n");
    return found(layerDown) && found(orbitDown) && orbitDown > layerDown;
}

static bool wiredDeviceHook(const std::string& t) {
    const size_t fn = t.find("HRESULT STDMETHODCALLTYPE hookedCreateVS(");
    if (!found(fn)) return false;
    const size_t next = t.find("HRESULT packetCreateShader(", fn);
    const size_t registered = t.find("registerShaderHash(*out, hash);", fn);
    const size_t call = t.find("orbitalWidthRememberVs(static_cast<ID3D11VertexShader*>(*out),hash,bytecode,static_cast<size_t>(len),linkage!=nullptr);", fn);
    return found(next) && found(registered) && found(call) && registered < call && call < next;
}

static bool wiredUiDepth(const std::string& t) {
    const size_t begin = t.find("bool uiDepthReissueBegin(ID3D11DeviceContext* ctx) {");
    const size_t end = t.find("void uiDepthReissueEnd(ID3D11DeviceContext* ctx) {");
    const size_t shutdown = t.find("void uiDepthShutdown() {");
    if (!found(begin) || !found(end) || !found(shutdown) || !(begin < end && end < shutdown)) return false;
    const size_t made = t.find("g_orbitalWidthCb.get(ctx,orbital_width::factor())", begin);
    const size_t gate = t.find("(orbital && g_orbitalVs && orbitalWidth)", begin);
    const size_t prepared = t.find("holo=g_holoMotion[g_drawEye].prepare(ctx,scene,g_holoDraw,g_cockpitMetres,ring?1:orbital?2:sprite?3:0,shader->slot);", begin);
    const size_t saved = t.find("ctx->VSGetConstantBuffers(orbital_width::kSlot,1,&g_savedOrbitalWidth);", begin);
    const size_t bindVs = t.find("ctx->VSSetShader(g_orbitalVs.Get(),nullptr,0);", begin);
    const size_t bindCb = t.find("ctx->VSSetConstantBuffers(orbital_width::kSlot,1,&orbitalWidth);", begin);
    const size_t backVs = t.find("ctx->VSSetShader(g_savedOrbitalVs.Get(),g_savedOrbitalClasses,g_savedOrbitalClassCount);", end);
    const size_t backCb = t.find("ctx->VSSetConstantBuffers(orbital_width::kSlot,1,g_savedOrbitalWidth.GetAddressOf());", end);
    const size_t dropped = t.find("g_savedOrbitalWidth.Reset();g_orbitalBound=false;", end);
    const size_t reset = t.find("g_orbitalWidthCb.reset();", shutdown);
    if (!found(made) || !found(gate) || !found(prepared) || !found(saved) || !found(bindVs) || !found(bindCb) || !found(backVs) || !found(backCb) ||
        !found(dropped) || !found(reset))
        return false;
    if (!(made < gate && gate < prepared && prepared < saved && saved < bindVs && bindVs < bindCb && bindCb < end)) return false;
    return end < backVs && backVs < backCb && backCb < dropped && dropped < shutdown && shutdown < reset;
}

static bool wiredLayer(const std::string& t) {
    const size_t panels = t.find("    uiPanelScaleLog();");
    const size_t orbit = t.find("    orbitalWidthLog();");
    return found(panels) && found(orbit) && orbit > panels && orbit - panels < 200 && found(t.find("#include \"orbital_width.h\""));
}

static bool wiredCoverage(const std::string& t) {
    const size_t vs = t.find("constexpr char kOrbitalCoverageVs[]");
    const size_t ps = t.find("constexpr char kOrbitalCoveragePs[]");
    if (!found(vs) || !found(ps) || vs > ps) return false;
    const std::string twin = t.substr(vs, ps - vs);
    return found(twin.find("cbuffer Width:register(b13){float4 width;}")) && found(twin.find("scene[332].zw*width.x*p.w")) &&
           !found(twin.find("scene[332].zw*2*p.w"));
}

static bool wiredBuild(const std::string& t) {
    const size_t source = t.find("\"src\\d3d11\\orbital_width.cpp\"");
    const size_t compile = t.find("ERROR: compile failed");
    const size_t panel = t.find("\"src\\d3d11\\ui_panel_scale.cpp\"");
    return found(source) && found(compile) && found(panel) && std::abs(static_cast<long long>(source) - static_cast<long long>(panel)) < 120 &&
           source < compile && found(t.find(":rig_orbital_width_test\n"));
}

static void wiringCases() {
    const std::string vscreen = readText("src/d3d11/vscreen.cpp"), hook = readText("src/d3d11/device_hook.cpp"),
                      depth = readText("src/d3d11/ui_depth.cpp"), layer = readText("src/d3d11/ui_layer.cpp"),
                      coverage = readText("src/d3d11/stellar_coverage.h"), build = readText("build.bat");
    check(vscreen.size() > 100000 && hook.size() > 50000 && depth.size() > 100000 && layer.size() > 100000 && coverage.size() > 1000 && build.size() > 50000,
          "the sources are read from the repo root");

    check(wiredVscreen(vscreen), "vscreen.cpp: Begin gates on the owner, an eye target and the shader hash, and runs before the game's issue; End follows it, before the twin; tick and shutdown are there");
    check(!wiredVscreen(without(vscreen, "if (orbitScaled) orbitalWidthEnd(self);")), "control: no End after the game's issue trips the pin");
    check(!wiredVscreen(replaced(without(vscreen, "if (orbitScaled) orbitalWidthEnd(self);"), "uiDepthReissueBegin(self)",
                                 "uiDepthReissueBegin(self);if (orbitScaled) orbitalWidthEnd(self);//")),
          "control: End after the coverage twin binds trips the pin");
    const std::string gate = "const bool orbitScaled = ";
    check(!wiredVscreen(replacedAfter(vscreen, gate, "g_state->rtv0Eye && ", "")), "control: a Begin not gated on an eye target trips the pin");
    check(!wiredVscreen(replacedAfter(vscreen, gate, "owner && ", "")), "control: a Begin not gated on the owner context trips the pin");
    check(!wiredVscreen(replacedAfter(vscreen, gate, "bindingShaderHash(BindSlot::Vs) == orbital_width::kVs", "true")), "control: a Begin not gated on the shader hash trips the pin");
    check(!wiredVscreen(movedAfter(vscreen, "const bool orbitScaled = ", "    // The layer's bracket goes innermost", "    if (orbitScaled) orbitalWidthEnd(self);")),
          "control: a Begin after the game's issue trips the pin");
    check(!wiredVscreen(without(vscreen, "tkOrbitalWidth.run([&] { orbitalWidthFrameBoundary(g_state->ownerCtx); });")), "control: no frame boundary tick trips the pin");
    check(!wiredVscreen(without(vscreen, "    orbitalWidthShutdown();\n")), "control: no shutdown trips the pin");

    check(wiredDeviceHook(hook), "device_hook.cpp: the creation hook offers the shader's bytes to the module after registering it");
    check(!wiredDeviceHook(without(hook, "orbitalWidthRememberVs(static_cast<ID3D11VertexShader*>(*out),hash,bytecode,static_cast<size_t>(len),linkage!=nullptr);")),
          "control: no capture call trips the pin");
    check(!wiredDeviceHook(replaced(hook, "len),linkage!=nullptr);   // fix.ui_quality", "len),false);   // fix.ui_quality")),
          "control: a capture that never says whether the creation was linked trips the pin");

    check(wiredUiDepth(depth), "ui_depth.cpp: the twin makes, saves, binds and gives back its private b13 beside its shader, and a missing buffer declines it");
    check(!wiredUiDepth(without(depth, "ctx->VSSetConstantBuffers(orbital_width::kSlot,1,&orbitalWidth);")), "control: a twin that never binds b13 trips the pin");
    check(!wiredUiDepth(without(depth, "ctx->VSGetConstantBuffers(orbital_width::kSlot,1,&g_savedOrbitalWidth);")), "control: a twin that never saves slot 13 trips the pin");
    check(!wiredUiDepth(without(depth, "ctx->VSSetConstantBuffers(orbital_width::kSlot,1,g_savedOrbitalWidth.GetAddressOf());")), "control: a twin that never gives slot 13 back trips the pin");
    check(!wiredUiDepth(replaced(depth, "(orbital && g_orbitalVs && orbitalWidth)", "(orbital && g_orbitalVs)")), "control: a twin that runs without its buffer trips the pin");
    check(!wiredUiDepth(replaced(depth, "g_orbitalWidthCb.get(ctx,orbital_width::factor())", "g_orbitalWidthCb.get(ctx,1.0)")), "control: a twin that ignores the factor trips the pin");
    check(!wiredUiDepth(without(depth, "g_orbitalWidthCb.reset();")), "control: a shutdown that keeps the twin's buffer trips the pin");

    check(wiredLayer(layer), "ui_layer.cpp: the orbit lines' 30 s line follows the panels'");
    check(!wiredLayer(without(layer, "    orbitalWidthLog();")), "control: no orbit-line log call trips the pin");

    check(wiredCoverage(coverage), "stellar_coverage.h: the twin's half-width is the factor in b13, not the literal 2");
    check(!wiredCoverage(replaced(coverage, "scene[332].zw*width.x*p.w", "scene[332].zw*2*p.w")), "control: a twin with the literal 2 back trips the pin");
    check(!wiredCoverage(replaced(coverage, "cbuffer Width:register(b13){float4 width;}", "")), "control: a twin without its b13 trips the pin");

    check(wiredBuild(build), "build.bat: the module is in the DLL's compile list and this rig has its label");
    check(!wiredBuild(without(build, "\"src\\d3d11\\orbital_width.cpp\"")), "control: a module left out of the DLL trips the pin");
    check(!wiredBuild(replaced(build, ":rig_orbital_width_test\n", ":rig_other\n")), "control: a rig without its label trips the pin");
}

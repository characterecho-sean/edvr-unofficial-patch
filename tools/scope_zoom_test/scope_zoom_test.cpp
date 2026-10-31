// The scope lens, in the build's gate (docs/design-vr-scope-magnification-2026-10-28.md, section 8).
//
//   THE WIRING (source scans, each with a control). The lens at the door right after the shield ripple and before the weapons/HUD composite; the temporal pass clearing and setting
//   the eye's depth around the view it makes for the upscaler; the frame thread's tick after S0's and the publish; the two keys read by their literal names; the ini rows.
//   THE PASSES (WARP, the debug layer on). The production door (src/d3d11/scope_zoom.cpp linked as shipped, the build's bytecode, the production parameter blocks) on a synthetic
//   frame whose depth is a near hexagonal ring about c, far inside and out: inside the ring the frame is sampled at c + d / M (Catmull-Rom, within one code of the reference
//   built from scope_zoom_s1.h's own functions); the ring and everything outside it are not touched, byte for byte; the mask's edge lies on the ring's inner edge to 4 output px; a
//   ring with a two-ray gap still has a lens; an open ring (no frame) and a frame whose format is not R8G8B8A8 or B8G8R8A8 UNORM are left alone and counted; a frame EDVR may
//   not write comes back as the lens's own copy with the original untouched; a stale c, no depth, a picture that is not the upscaler's, a gate verdict that is not a lens and
//   M = 1 are each counted under their own reason and leave the frame alone. The rays read back give the hole the instrument reports, which agrees with the reference march.
//   Mutants of the comparator (M off by 20 %, c off by 4 px, the hole 20 % small) each fail the check that passes the real thing.
//   B3 (the left eye drawn as the zoomed view; docs section 11). THE WIRING: the left door before the left eye's UI composite, the draw hook's count, the provider's five words, the runtime's
//   narrowing and the raw getter's true answer, the temporal pass's announced frusta and reset edge, the key and its ini rows (default off). THE PASS: a synthetic world (a textured plane at
//   several distances, and a sky) seen by two eyes 6.3 cm apart, the left one drawn zoomed; the production left door copies its picture and the production door takes the lens from it, each
//   pixel at the place its world point is drawn at in the left eye: the lens shows the plane's texture at every distance within a few codes of the ideal, and the same pixels sampled without
//   the parallax removed do not; the lens's source is counted as zoomed, and a frame not asked for, with the left eye not zoomed, with no copy, with a copy of another frame, with no
//   matrices, or with the left eye behind falls back to the plain lens for the reason named.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <windows.h>

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>

#include "../../src/d3d11/gpu_census.h"
#include "../../src/d3d11/gpu_timing.h"
#include "../../src/d3d11/scope_zoom.h"
#include "../../src/d3d11/scope_zoom_s1.h"
#include "../../src/d3d11/scope_zoom_shaders.h"
#include "../../src/d3d11/shader_swap.h"
#include "../native_frame_test/b3_checks.h"   // B3: the synthetic pinhole eye (makeCam) the pure checks use

using namespace edvr;
using Microsoft::WRL::ComPtr;
namespace sz = edvr::scopezoom;

static unsigned g_checks = 0;
static void check(bool ok, const char* why) {
    ++g_checks;
    if (!ok) throw std::runtime_error(why);
}
static void hr(HRESULT r, const char* what) { check(SUCCEEDED(r), what); }
static std::string readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ============================================================================================ production's neighbours, stubbed (as the UI layer rigs stub theirs)
namespace edvr {
void breadcrumb(const char*) {}   // production guard.cpp's crash-channel dependency
bool gpuCensusBegin(ID3D11DeviceContext*, GpuCensusSection) noexcept { return false; }
void gpuCensusEnd(ID3D11DeviceContext*, GpuCensusSection) noexcept {}
bool gpuTimingBind(ID3D11Device*, ID3D11DeviceContext*, const GpuSpanD3D11Ops&) noexcept { return false; }
bool gpuTimingAccepts(ID3D11DeviceContext*) noexcept { return false; }
bool gpuTimingOwns(ID3D11DeviceContext*) noexcept { return false; }
bool GpuTimer::begin(ID3D11Device*, ID3D11DeviceContext*) noexcept { return false; }
bool GpuTimer::beginBorrowedFrame(ID3D11Device*, ID3D11DeviceContext*) noexcept { return false; }
bool GpuTimer::end(ID3D11DeviceContext*) noexcept { return false; }
// B3: the temporal pass's word on eye 0 (native_temporal.cpp), driven by the rig: on, and the window the frame was told.
struct StubZoom {
    bool on = false;
    float zoomed[4] = {}, pre[4] = {};
} g_stubZoom;
bool nativeTemporalZoom(uint64_t, float zoomed[4], float pre[4]) {
    if (!g_stubZoom.on) return false;
    if (zoomed) std::memcpy(zoomed, g_stubZoom.zoomed, sizeof(g_stubZoom.zoomed));
    if (pre) std::memcpy(pre, g_stubZoom.pre, sizeof(g_stubZoom.pre));
    return true;
}
GpuTimerPoll GpuTimer::poll(ID3D11DeviceContext*, double&) noexcept { return GpuTimerPoll::Pending; }
void GpuTimer::reset(ID3D11DeviceContext*) noexcept {}
ID3D11ComputeShader* shaderSwapCreateCs(ID3D11DeviceContext* ctx, const void* bytes, size_t n, const char*, const char*) {
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    ID3D11ComputeShader* s = nullptr;
    return dev && SUCCEEDED(dev->CreateComputeShader(bytes, n, nullptr, &s)) ? s : nullptr;
}
}  // namespace edvr

// ============================================================================================ the wiring
static bool has(const std::string& t, const std::string& s) { return t.find(s) != std::string::npos; }
static bool before(const std::string& t, const std::string& a, const std::string& b) {
    const size_t pa = t.find(a), pb = t.find(b);
    return pa != std::string::npos && pb != std::string::npos && pa < pb;
}
static std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const size_t at = text.find(from);
    check(at != std::string::npos, "a mutation's text is in the source it mutates");
    text.replace(at, from.size(), to);
    return text;
}
// The door: inside uiLayerComposite, the lens (scopeZoomDoor) after the ripple's door and before the weapons/HUD composite's first return, for the right eye only.
static bool wiredDoor(const std::string& layer) {
    const size_t fn = layer.find("ID3D11Texture2D* uiLayerComposite(uint64_t sequence, uint32_t eye");
    if (fn == std::string::npos) return false;
    const std::string body = layer.substr(fn, 8900);   // the function is about 8600 bytes with B3's left door; a margin for a line or two, so a window can't reach into the next function and satisfy a pin there
    return before(body, "rippleDoor(e, eye, sequence", "scopeZoomDoor(lensArgs") && before(body, "scopeZoomDoor(lensArgs", "if (!e.srv || !e.rtv || e.compositedSeq == sequence) return handOn();") &&
           before(body, "scopeZoomDoor(lensArgs", "compositeInner(e, eye, frame") && has(body, "if (eye == 1 && scopeZoomWanted())") && has(body, "lensArgs.resolved = e.doorFromPass;");
}
// B3: the left eye's door copies the zoomed picture's window after the ripple and before the left eye's own UI composite, and before the right eye's lens reads it.
static bool wiredLeftDoor(const std::string& layer) {
    const size_t fn = layer.find("ID3D11Texture2D* uiLayerComposite(uint64_t sequence, uint32_t eye");
    if (fn == std::string::npos) return false;
    const std::string body = layer.substr(fn, 8900);
    return before(body, "rippleDoor(e, eye, sequence", "scopeZoomLeftDoor(leftArgs)") && before(body, "scopeZoomLeftDoor(leftArgs)", "scopeZoomDoor(lensArgs") &&
           before(body, "scopeZoomLeftDoor(leftArgs)", "if (!e.srv || !e.rtv || e.compositedSeq == sequence) return handOn();") && has(body, "if (eye == 0 && scopeZoomWanted())") &&
           has(body, "leftArgs.resolved = e.doorFromPass;") && has(body, "leftArgs.frame = frame;");
}
static bool wiredTemporal(const std::string& t) {
    return before(t, "scopeZoomDepthClear(eye);", "depthProbeSceneDepth(sd.Width, sd.Height, eye, &dtex)") && before(t, "depthProbeSceneDepth(sd.Width, sd.Height, eye, &dtex)", "scopeZoomDepthSet(eye, depthSrv") &&
           before(t, "scopeZoomDepthSet(eye, depthSrv", "uiDepthSmokeDepth(sd.Width, sd.Height, eye, &smokeSrv)");
}
static bool wiredFrame(const std::string& n) {
    return before(n, "s0::vrTick(GetTickCount64());", "s1::tick(GetTickCount64());") && has(n, "scopeZoomPublish(out);") && has(n, "scopeZoomPressBegin(s.press);") && has(n, "scopeZoomPressTake(&pl.d);") &&
           has(n, "cfg.getString(\"fix.native_on_foot_ads_scope_zoom\", \"on\")") && has(n, "std::atomic<bool> g_scopeZoomKey{true};") && has(n, "cfg.getFloat(\"experimental.native_on_foot_ads_scope_zoom_fov\"") &&
           has(n, "sz::lensGate(gi)") && has(n, "nativeTemporalDrawJitter(static_cast<uint32_t>(e), &tseq, &jx, &jy, &w, &h)") &&
           has(n, "if (g_scopeZoomKey.load(std::memory_order_relaxed) && (e == 1 || g_scopeZoomFullKey.load(std::memory_order_relaxed))) eyeJitterPublish(e, frame);") &&
           has(n, "nofoot::sightPointNdc(g.rows[1], od, S, dir, kFarM, ndcFar)") && has(n, "const double dir[3] = {S[0] - v.Epos[0], S[1] - v.Epos[1], S[2] - v.Epos[2]};") &&
           has(n, "if (s.open) endPress(s);");
}
// B3: the key (read by its literal name, default off, live), the tick's step before the publish, the window handed the provider each tick, the press line from every module's half, the
// strip still published outside the sights, the draw path's count, the readout pin taking the right eye alone, the shadow constants, and the runtime's export.
static bool wiredB3Frame(const std::string& n) {
    return has(n, "cfg.getString(\"experimental.native_on_foot_ads_scope_zoom_full_detail\", \"off\") == \"on\"") && has(n, "std::atomic<bool> g_scopeZoomFullKey{false};") &&
           before(n, "b3Tick(s, gi, verdict, g, dt, nowMs, out);", "    scopeZoomPublish(out);\n}\n}  // namespace s1") && has(n, "scopewin::publish(so.request, nowMs);") && has(n, "const sz::B3StepOut so = sz::b3Step(b.loop, si);") &&
           has(n, "scopewin::publish(sz::b3IdleStep(g_b3s.loop, dt), nowMs);") && has(n, "scopewin::publish(sz::b3OffStep(g_b3s.loop), nowMs);") && has(n, "if (!so.frameAsked) return;") &&
           has(n, "p.temporalKnown = sz::b3Since(b.temporalBase, temporalNow,") && has(n, "p.lightGridKnown = sz::b3Since(true, true, b.lgCalls0, calls, &p.lgCalls)") &&
           has(n, "void endPress(State& s) noexcept {\n    endB3Press(s);") && has(n, "sz::formatB3Press(line, sizeof(line), p);") && has(n, "edvr::scopezoom::formatB3Runtime(line, sizeof(line), *r);") &&
           has(n, "scopeZoomB3Take(&door);") && has(n, "nativeTemporalZoomCounts(&zf, rs, &edge)") && has(n, "lightGridCacheZoomCounts(&calls, &bypassed);") && has(n, "        b3Idle(dt, nowMs);\n        return;") &&
           has(n, "edvr::scopezoom::b3PinAction(nativeTemporalZoomLatest(nullptr, nullptr, nullptr), e);") && has(n, "g_f85Latch.putSolo(1, frame, s.K, s.oh, s.ow)") &&
           has(n, "b3ShadowNote(e, frame, cb1);") && has(n, "void nativeOnFootB3NoteEyeDraw() {") && has(n, "scopewin::clear();") && has(n, "extern \"C\" HRESULT WINAPI edvrNoteNativeScopeZoom(") &&
           has(n, "sz::b3Matrices(g.rows[1], o1, jn[1], g.rows[0], o0, jn[0], out.b3.T, out.b3.u)") && has(n, "sz::b3WindowMatches(zt, lt, 3.0e-3)") && has(n, "const bool keyB3 = g_scopeZoomFullKey.load(std::memory_order_relaxed) && gi.keyOn;");
}
// B3: the provider hands the window to the runtime in a version 11 answer only; the runtime narrows eye 0's told frustum where the trim sets the frame's frusta, keeps the true frustum for the
// raw getter, the hidden-area mesh and the temporal pass's announcements, and tells the strip to the stereo renderer; the temporal pass resets eye 0's history at the window's edges.
static bool wiredB3Halves(const std::string& frame, const std::string& host, const std::string& temporal, const std::string& stereo, const std::string& vscreen, const std::string& system) {
    return has(frame, "if (carried) edvr::scopewin::toWords(edvr::scopewin::read(GetTickCount64()), result.reservedTerrainGuard);") &&
           before(host, "zoomApplied=applyScopeZoom(gameGeometry,zoomPreRaw,zoomToldRaw);", "geometry.publish(gameGeometry,false,false,frameTangentShift,hiddenMeshCompatible()&&!zoomApplied,zoomApplied?zoomToldRaw:nullptr)") &&
           has(host, "zoomApplied?&zoomPreRaw:nullptr);") && has(host, "locatedHeadPose(),leftShare,&hidden,zoomStripNow,zoomAppliedNow,&frameOrigin);") && has(host, "leftShare,&hidden,zoomStripNow,previousZoomed,&previousOrigin):") && has(host, "previousZoomed=zoomAppliedNow;") &&
           has(host, "const ScopeZoomEvents ev=zoomRun.frame(out,frameCycleUs());") && has(host, "if(raw)zoomCallers.note(caller);") && has(host, "zoomCallers.top(press.callerRva,press.callerReads,press.callerOther);") &&
           has(system, "source_.noteProjectionRead(eye,true,s.toldRawSet&&liveGeometry,rawSpanV(out),frameRva(exe_,reinterpret_cast<uintptr_t>(_ReturnAddress())));") &&
           has(system, "source_.noteProjectionRead(eye,false,s.toldRawSet&&liveGeometry,accepted?matrixSpanV(out):0.0f,frameRva(exe_,reinterpret_cast<uintptr_t>(_ReturnAddress())));") &&
           has(host, "in.shareDrawn=facts.drawn;in.shareRefused=facts.refused;") &&
           has(host, "const bool carries=features.acquired()&&featureFrameKnown&&featureFrame.version>=EDVR_NATIVE_FRAME_VERSION_11;") && has(host, "game.views[0].fov=out.fov;") &&
           has(system, "const RawFov base=liveGeometry?(s.toldRawSet?s.toldRaw[eye]:s.geometry.raw[eye]):s.optics.raw[eye];") &&
           has(temporal, "edvr::terrainCullCoverNoteFrusta(s->announced);") && has(temporal, "const float outer=-s->announced[0][0], inner=s->announced[0][1];") &&
           has(temporal, "const bool zoomMoved=s->history[eye].valid&&edvr::scopezoom::b3ResetEdge(s->history[eye].zoomed,zoomedHere);") && has(temporal, "separationMoved||zoomMoved;") &&
           has(temporal, "const bool zoomedHere=eye==0&&s->zoomedNow;") && has(temporal, "hst.zoomed=zoomedHere;") &&
           has(stereo, "share.band[2]=leftStripBlack;") && has(stereo, "share.band[3]=leftZoomed?1.f:0.f;") && has(vscreen, "if (nativeOnFootB3Counting()) nativeOnFootB3NoteEyeDraw();");
}
static bool wiredIni(const std::string& ini) {
    const size_t fixAt = ini.find("native_on_foot_ads_scope_zoom = on"), expAt = ini.find("native_on_foot_ads_scope_zoom_fov = 56.3");
    return fixAt != std::string::npos && expAt != std::string::npos && fixAt < expAt && has(ini, "# ui: ADS: scope zoom | choices on, off | recommended on | live | menu native_on_foot") &&
           has(ini, "# ui: ADS: scope zoom FOV (experimental) | range 30..90 | step 0.5 | unit deg | recommended 56.3 | live") && before(ini, "[experimental]", "native_on_foot_ads_scope_zoom_fov = 56.3");
}
static bool wiredMark(const std::string& layer, const std::string& vscreen) {
    const size_t fn = vscreen.find("UiLayerFamily uiLayerFamilyOf(State* s, char kind, UINT count, UINT instances) {");
    if (fn == std::string::npos) return false;
    const size_t weapons = vscreen.find("else if (f.targetKind == 1 && (weaponsFireVs(f.vs) || shieldRefractVs(f.vs)))", fn);
    const size_t mark = vscreen.find("else if (f.targetKind == 1 && f.vs == kUiVsScopeMark) {", fn);
    const size_t facts = vscreen.find("f.scopeMark = uiScopeMarkFacts(", fn);
    const size_t tail = vscreen.find("if (f.targetKind == 2) {", fn);
    // The rule itself (the shape, the arming, the pixel shader and the atlas, each alone refusing) is driven behaviourally by sharp_weapons_test; here the hook is only shown to hand the
    // rule the real reads, the pixel shader's hash into f.ps, and to count and blend as the rule's neighbours do.
    return weapons != std::string::npos && mark != std::string::npos && facts != std::string::npos && tail != std::string::npos && weapons < mark && mark < facts && facts < tail &&
           has(vscreen, "uiLayerFamilyOf(g_state, kind, count, instances)") && has(vscreen, "f.vs, kind, count, instances, [] { return scopeZoomMarkArmed() || scopeZoomCrispArmed(); }, [] { return bindingShaderHash(BindSlot::Ps); }, "
                                                                                              "[] { return uiDepthSampledSurfaceSlot() >= 0; }, &f.ps);") &&
           has(layer, "scopeZoomNoteMark(static_cast<uint32_t>(g_draw.eye), g_draw.seq, g_draw.markPath);") && has(layer, "f.blend = shapeOf(bs, &game, destAlphaOne, &gameBlend);") &&
           has(layer, "const UiBlendShape shape = shapeOf(bs, &game, g_draw.destAlphaOne);") && has(layer, "const bool destAlphaOne = markFamily && !uiLayerFormatHasAlpha(g_tc.info.fmt);") &&
           has(layer, "describeBlend(gameBlend, bl, sizeof(bl));") &&
           has(layer, "dsReproducible(ctx, dsv, g_tc.info.a, g_tc.info.b, dsWhy, sizeof(dsWhy), markFamily && uiScopeMarkWritesNothing(dsState));") &&
           has(layer, "markDepth = uiScopeMarkDepth(f.ds, dsState, f.dsReproducible);") && has(layer, "if (markDepth == UiMarkDepth::kTestsOff) f.ds.depthTest = f.ds.stencilTest = false;") &&
           has(layer, "if (family == UiLayerFamily::kScopeMark && familyLineWanted(family, d))") &&
           // A3: the mark rides the weapons layer. It is counted there per eye-frame at the door, however uiLayerComposite returns, as the re-issue that carried it (the tonemap
           // difference or the plain add) or none, and the layer is composited after the lens in the same call (wiredDoor).
           has(layer, "g_draw.weapons = uiLayerFamilyUsesWeaponsLayer(family);") && has(layer, "} markFrame{e, sequence};") && has(layer, "markFrame.composited = result != nullptr;") &&
           has(layer, "e.wPubMatched = plan.sourcePass;") && has(layer, "scopeZoomNoteMarkComposite(!composited || e.wPubSeq != sequence ? scopezoom::kMarkCompositeNone");
}
static void wiringTests() {
    const std::string layer = readFile("src/d3d11/ui_layer.cpp"), temporal = readFile("src/d3d11/temporal_pass.cpp"), native = readFile("src/d3d11/native_on_foot.cpp"), ini = readFile("edvr.ini");
    check(!layer.empty() && !temporal.empty() && !native.empty() && !ini.empty(), "the sources the wiring scans read are readable from the repository root");
    check(wiredDoor(layer), "the lens is at the door, after the ripple, before the weapons/HUD composite, for the right eye, over the upscaler's picture");
    check(!wiredDoor(replaced(layer, "if (eye == 1 && scopeZoomWanted())", "if (eye == 0 && scopeZoomWanted())")), "control: the lens in the left eye is not the wiring");
    check(!wiredDoor(replaced(layer, "lensArgs.resolved = e.doorFromPass;", "lensArgs.resolved = true;")), "control: a lens over a picture that is not the upscaler's is not the wiring");
    check(!wiredDoor(replaced(layer, "scopeZoomDoor(lensArgs, &lensCopy);", "scopeZoomDoorOff(a, &lensCopy);")), "control: a door call to something else is not the wiring");
    check(wiredTemporal(temporal), "the temporal pass clears the eye's depth slot at the top of its depth step and sets it from the view it made for the upscaler");
    check(!wiredTemporal(replaced(temporal, "scopeZoomDepthClear(eye);", "")), "control: no clear is not the wiring");
    check(wiredFrame(native), "the frame tick follows S0's, publishes, takes the door's counters at the press's end, and the two keys are read by their literal names");
    check(!wiredFrame(replaced(native, "s1::tick(GetTickCount64());", "")), "control: no tick is not the wiring");
    check(!wiredFrame(replaced(native, "nofoot::sightPointNdc(g.rows[1], od, S, dir, kFarM, ndcFar)", "nofoot::sightPointNdc(g.rows[1], od, S, dir, 0.0, ndcFar)")), "control: c taken at S itself, not at the far point along the line E -> S, is not the wiring");
    check(!wiredFrame(replaced(native, "if (s.open) endPress(s);", "")), "control: a press left open by the flat profile or an unarmed tick with no line is not the wiring");
    check(!wiredFrame(replaced(native, "cfg.getString(\"fix.native_on_foot_ads_scope_zoom\", \"on\")", "cfg.getString(\"fix.native_on_foot_ads_zoom\", \"on\")")), "control: a key read by another name is not the wiring");
    check(!wiredFrame(replaced(native, "cfg.getString(\"fix.native_on_foot_ads_scope_zoom\", \"on\")", "cfg.getString(\"fix.native_on_foot_ads_scope_zoom\", \"off\")")), "control: the reader's fallback off is not the shipped default");
    check(!wiredFrame(replaced(native, "std::atomic<bool> g_scopeZoomKey{true};", "std::atomic<bool> g_scopeZoomKey{false};")), "control: the atomic's start value off is not the shipped default");
    check(wiredIni(ini), "the ini states both keys with their rows: the user key on the Native on foot page, the experimental one in the developer tier");
    // ---- B3 ----
    check(wiredLeftDoor(layer), "B3: the left eye's door copies the zoomed picture after the ripple, before the left eye's UI composite and the right eye's lens");
    check(!wiredLeftDoor(replaced(layer, "if (eye == 0 && scopeZoomWanted())", "if (eye == 1 && scopeZoomWanted())")), "control: a left door asked for the right eye is not the wiring");
    check(!wiredLeftDoor(replaced(layer, "leftArgs.resolved = e.doorFromPass;", "leftArgs.resolved = true;")), "control: a left copy of a picture that is not the upscaler's is not the wiring");
    check(has(native, "scopeZoomSetTemporalZoom(&nativeTemporalZoom);"), "B3: the tick hands the doors the temporal pass's word on eye 0");
    check(wiredB3Frame(native), "B3: the key by its literal name, off by default; the tick's step before the publish; the window handed the provider; the press line from every half; the pin on one eye; the export");
    check(!wiredB3Frame(replaced(native, "std::atomic<bool> g_scopeZoomFullKey{false};", "std::atomic<bool> g_scopeZoomFullKey{true};")), "control: the full-detail key on at the start is not the shipped default");
    check(!wiredB3Frame(replaced(native, "cfg.getString(\"experimental.native_on_foot_ads_scope_zoom_full_detail\", \"off\") == \"on\"", "cfg.getString(\"experimental.native_on_foot_ads_scope_zoom_full_detail\", \"on\") == \"on\"")),
          "control: the reader's fallback on is not the shipped default");
    check(!wiredB3Frame(replaced(native, "b3Tick(s, gi, verdict, g, dt, nowMs, out);", "")), "control: no B3 step in the tick is not the wiring");
    check(!wiredB3Frame(replaced(native, "        b3Idle(dt, nowMs);\n        return;", "        return;")), "control: a strip that is not published once the sights are down (it would stay black) is not the wiring");
    check(!wiredB3Frame(replaced(native, "    endB3Press(s);\n", "")), "control: a press that does not write its B3 line is not the wiring");
    check(!wiredB3Frame(replaced(native, "g_f85Latch.putSolo(1, frame, s.K, s.oh, s.ow)", "false")), "control: a readout pin that does not take the right eye alone while the left is zoomed is not the wiring");
    check(!wiredB3Frame(replaced(native, "edvr::scopezoom::b3PinAction(nativeTemporalZoomLatest(nullptr, nullptr, nullptr), e);", "edvr::scopezoom::b3PinAction(false, e);")),
          "control: a pin that never knows eye 0 is zoomed is not the wiring");
    check(!wiredB3Frame(replaced(native, "if (!so.frameAsked) return;", "if (!qualifies) return;")), "control: a frame the doors are told is asked before the window latched is not the wiring");
    check(!wiredB3Frame(replaced(native, "p.temporalKnown = sz::b3Since(b.temporalBase, temporalNow,", "p.temporalKnown = (true || temporalNow || b.temporalBase) && sz::b3Since(true, true,")),
          "control: temporal counts taken as differences of totals whatever the press's baseline or reading was are not the wiring");
    check(!wiredB3Frame(replaced(native, "sz::b3WindowMatches(zt, lt, 3.0e-3)", "true")), "control: matrices made from rows that do not describe the window the runtime told the game are not the wiring");
    {
        const std::string frameCpp = readFile("src/d3d11/native_frame.cpp"), host = readFile("src/openxr/native_runtime_host.h"), tcpp = readFile("src/d3d11/native_temporal.cpp"),
                          stereo = readFile("src/openxr/d3d11_stereo.cpp"), system = readFile("src/openxr/openvr_system.cpp"), vs = readFile("src/d3d11/vscreen.cpp");
        check(!frameCpp.empty() && !host.empty() && !tcpp.empty() && !stereo.empty() && !system.empty(), "B3: the sources the halves' wiring scans read are readable");
        check(wiredB3Halves(frameCpp, host, tcpp, stereo, vs, system), "B3: the provider's words, the runtime's narrowing (before the publish; the raw getter, mesh and announcements kept true), the strip, the draw count, the reset edge");
        check(!wiredB3Halves(replaced(frameCpp, "if (carried) edvr::scopewin::toWords(", "if (false) edvr::scopewin::toWords("), host, tcpp, stereo, vs, system), "control: words the provider never fills are not the wiring");
        check(!wiredB3Halves(frameCpp, replaced(host, "hiddenMeshCompatible()&&!zoomApplied,zoomApplied?zoomToldRaw:nullptr", "hiddenMeshCompatible(),nullptr"), tcpp, stereo, vs, system),
              "control: a zoom whose hidden-area mesh stays and whose raw getter is not held back is not the wiring");
        check(!wiredB3Halves(frameCpp, replaced(host, "zoomApplied?&zoomPreRaw:nullptr);", "nullptr);"), tcpp, stereo, vs, system), "control: a temporal pass not told the frustum without the zoom is not the wiring");
        check(!wiredB3Halves(frameCpp, replaced(host, "in.shareDrawn=facts.drawn;in.shareRefused=facts.refused;", ""), tcpp, stereo, vs, system), "control: a zoom that does not look at whether the share was drawn is not the wiring");
        check(!wiredB3Halves(frameCpp, host, replaced(tcpp, "edvr::terrainCullCoverNoteFrusta(s->announced);", "edvr::terrainCullCoverNoteFrusta(s->frusta);"), stereo, vs, system), "control: announced frusta that follow the zoom are not the wiring");
        check(!wiredB3Halves(frameCpp, host, replaced(tcpp, "separationMoved||zoomMoved;", "separationMoved;"), stereo, vs, system), "control: no history reset at the window's edges is not the wiring");
        check(!wiredB3Halves(frameCpp, host, tcpp, replaced(stereo, "share.band[2]=leftStripBlack;", ""), vs, system), "control: a strip that never reaches the shader is not the wiring");
        check(!wiredB3Halves(frameCpp, host, tcpp, replaced(stereo, "share.band[3]=leftZoomed?1.f:0.f;", ""), vs, system), "control: a narrowed frame that never reaches the shader is not the wiring");
        check(!wiredB3Halves(frameCpp, replaced(host, "previousZoomed=zoomAppliedNow;", ""), tcpp, stereo, vs, system), "control: a replayed pair that does not remember it was narrowed is not the wiring");
        check(!wiredB3Halves(frameCpp, replaced(host, "if(raw)zoomCallers.note(caller);", ""), tcpp, stereo, vs, system), "control: raw getter reads that name no caller are not the wiring");
        check(!wiredB3Halves(frameCpp, host, tcpp, stereo, vs, replaced(system, "rawSpanV(out),frameRva(exe_,", "0.0f,frameRva(exe_,")), "control: a raw getter that does not report what it answered is not the wiring");
        {   // the client sends a frame with no zoom at the size from before B3 (the key-off wire form is the parent's)
            const std::string client = readFile("src/openxr/native_temporal_client.h");
            const std::string sentAt = "EdvrNativeTemporalFrame frame{temporalFrameSize(preZoomEye0 != nullptr), EDVR_NATIVE_TEMPORAL_VERSION_1};";
            check(has(client, sentAt) && has(client, "return zoomed ? static_cast<uint32_t>(sizeof(EdvrNativeTemporalFrame)) : EDVR_NATIVE_TEMPORAL_FRAME_SIZE_NO_ZOOM;"), "B3: a frame with no zoom is sent at the size from before B3");
            check(!has(replaced(client, sentAt, "EdvrNativeTemporalFrame frame{sizeof(frame), EDVR_NATIVE_TEMPORAL_VERSION_1};"), sentAt), "control: a client that always sends the whole struct is not the wiring");
        }
        check(!wiredB3Halves(frameCpp, host, tcpp, stereo, replaced(vs, "if (nativeOnFootB3Counting()) nativeOnFootB3NoteEyeDraw();", ""), system), "control: draws that are not counted are not the wiring");
        check(!wiredB3Halves(frameCpp, host, tcpp, stereo, vs, replaced(system, "(s.toldRawSet?s.toldRaw[eye]:s.geometry.raw[eye])", "s.geometry.raw[eye]")), "control: a raw getter that follows the zoom (the HUD would grow) is not the wiring");
    }
    {
        const char* key = "native_on_foot_ads_scope_zoom_full_detail = off";
        const size_t at = ini.find(key), fovAt = ini.find("native_on_foot_ads_scope_zoom_fov = 56.3");
        check(at != std::string::npos && fovAt != std::string::npos && fovAt < at && before(ini, "[experimental]", key) && has(ini, "# ui: ADS: scope zoom full detail (experimental) | choices on, off | recommended off | live") &&
                  has(ini, "# ADS: scope zoom full detail: your choice of how the scope zoom") && !has(ini, "TEMPORARY (B3)") && !has(ini, "it goes when the lens has been judged"),
              "B3: the ini states the key off, in the experimental section after the reference FOV, as the user's choice and not as a temporary key, with its row");
        check(!(ini.find("native_on_foot_ads_scope_zoom_full_detail = on") != std::string::npos), "B3: the key is not on in the shipped file");
    }
    {   // B3-3: black is the behaviour. No edge key (its reader, atomic, ini entry and wire word are gone); a narrowed frame with no share has the left eye cleared; the right eye's mirror of the
        // strip is drawn after the share, only with a share drawn and the strip up or the frame narrowed, at the left strip's weight (full on a narrowed frame); the host tallies it and the press
        // takes it; the door estimates the lens outside the copy
        const std::string stereo2 = readFile("src/openxr/d3d11_stereo.cpp"), host2 = readFile("src/openxr/native_runtime_host.h"), door2 = readFile("src/d3d11/scope_zoom.cpp"),
                          window2 = readFile("src/common/scope_zoom_window.h"), b3h = readFile("src/d3d11/scope_zoom_b3.h"), shader2 = readFile("src/openxr/stereo_shader_source.h");
        auto noEdge = [&](const std::string& nat, const std::string& ini2, const std::string& win, const std::string& b3) {
            return !has(nat, "full_detail_edge") && !has(ini2, "full_detail_edge") && !has(win, "kEdge") && !has(win, "edgeName") && !has(b3, "kEdge") && !has(nat, "g_scopeZoomEdgeKey") &&
                   !has(nat, "B3 (live, TEMPORARY)") && has(nat, "announceB3(s, g_scopeZoomFullKey.load(std::memory_order_relaxed));") &&
                   has(nat, "Log::get().note(\"scope zoom (B3): full detail %s; for the table's") && has(nat, "p.outN = door.outN;") && has(nat, "out.b3.tcNow[0] = leftOk ?");
        };
        check(noEdge(native, ini, window2, b3h), "B3-3: the edge key is gone: no reader, atomic, ini entry, wire word or mode name, and the announce and the press line carry the full-detail key alone");
        check(!noEdge(native + "\nreadEdge(\"experimental.native_on_foot_ads_scope_zoom_full_detail_edge\");", ini, window2, b3h), "control: a source that reads the edge key again is not the wiring");
        check(!noEdge(native, ini + "\nnative_on_foot_ads_scope_zoom_full_detail_edge = world\n", window2, b3h), "control: an ini that carries the edge key again is not the wiring");
        check(!noEdge(native, ini, window2 + "\nconstexpr uint32_t kEdgeWorld = 1;", b3h), "control: a wire word that carries a mode again is not the wiring");
        auto wiredMirror = [&](const std::string& st, const std::string& ho, const std::string& dr, const std::string& sh) {
            return has(st, "const bool blackLeft=leftZoomed&&!shareOn&&fade<1.f;") && has(st, "if(i==0&&blackLeft) {") && has(st, "if(shareOn&&(leftStripBlack>0.f||leftZoomed)) {") &&
                   has(st, "const float weight=leftZoomed?1.f:leftStripBlack;") && has(st, "mirror.band[2]=weight;") && has(st, "mirror.misc[3]=1.f;") && has(st, "if(i==1&&mirrorOn) {") &&
                   has(st, "shareFacts_.mirrorDrawn=true;") && before(st, "if(i==0&&shareOn) {", "if(i==1&&mirrorOn) {") && before(st, "if(i==1&&mirrorOn) {", "if(fade>0.f) {") &&
                   has(sh, "if(misc.w>0.5) {") && has(sh, "return float4(0,0,0,mdim);") &&
                   has(ho, "zoomMirror.note(zoomStripNow,frameWithheld?previousZoomed:zoomAppliedNow,mirrorFacts.mirrorDrawn,mirrorFacts.mirrorWidth);") &&
                   has(ho, "in.standing=zoomRun.standing();in.notDrawnRun=zoomRun.notDrawnRun;") && has(ho, "zoomStripNow=out.strip;") && has(ho, "press.takeMirror(zoomMirror);") &&
                   has(dr, "if (s.outside && settled && !sz::lensOpenSight(rb.escaped)) {") && has(dr, "sz::b3OutsideFraction(rb.rDeg, s.m, s.tcNow, s.lo, s.hi, &frac)");
        };
        check(wiredMirror(stereo2, host2, door2, shader2), "B3-3: the renderer clears the left eye of a narrowed frame with no share and draws the right eye's mirror strip after the share, the host tallies it and the press takes it, the door estimates the lens outside the copy");
        check(!wiredMirror(replaced(stereo2, "if(shareOn&&(leftStripBlack>0.f||leftZoomed)) {", "if(true) {"), host2, door2, shader2), "control: a mirror drawn with no strip and no narrowing is not the wiring");
        check(!wiredMirror(replaced(stereo2, "const float weight=leftZoomed?1.f:leftStripBlack;", "const float weight=leftStripBlack;"), host2, door2, shader2), "control: a mirror that is not full on a narrowed frame is not the wiring");
        check(!wiredMirror(replaced(stereo2, "mirror.misc[3]=1.f;", ""), host2, door2, shader2), "control: a mirror that never asks the shader for its mode is not the wiring");
        check(!wiredMirror(replaced(stereo2, "if(i==1&&mirrorOn) {", "if(false) {"), host2, door2, shader2), "control: a mirror that is never drawn is not the wiring");
        check(!wiredMirror(replaced(stereo2, "if(i==0&&blackLeft) {", "if(false) {"), host2, door2, shader2), "control: a narrowed frame with no share whose left eye is drawn, not cleared, is not the wiring");
        check(!wiredMirror(stereo2, replaced(host2, "zoomMirror.note(zoomStripNow,frameWithheld?previousZoomed:zoomAppliedNow,mirrorFacts.mirrorDrawn,mirrorFacts.mirrorWidth);", ""), door2, shader2), "control: a host that does not count the mirror strip is not the wiring");
        check(!wiredMirror(stereo2, replaced(host2, "frameWithheld?previousZoomed:zoomAppliedNow,mirrorFacts", "zoomAppliedNow,mirrorFacts"), door2, shader2), "control: a replayed pair counted by the frame it replays, not the one it shows, is not the wiring");
        check(!wiredMirror(stereo2, replaced(host2, "in.standing=zoomRun.standing();in.notDrawnRun=zoomRun.notDrawnRun;", ""), door2, shader2), "control: a decision that is not told whether a zoom stands is not the wiring");
        check(!wiredMirror(stereo2, replaced(host2, "press.takeMirror(zoomMirror);", ""), door2, shader2), "control: a press that does not take the mirror strip's tally is not the wiring");
        check(!wiredMirror(stereo2, host2, replaced(door2, "if (s.outside && settled && !sz::lensOpenSight(rb.escaped)) {", "if (false) {"), shader2), "control: a door that never estimates the lens outside the copy is not the wiring");
    }
    check(!wiredIni(replaced(ini, "native_on_foot_ads_scope_zoom = on", "native_on_foot_ads_scope_zoom = off")), "control: the user key off by default is not the shipped default");

    // the scope's gunsight mark: named in the draw hook only for the pair's 6 indices in 1 instance while the lens is armed and a learned surface is sampled, counted where the layer takes
    // it, and blended with destination alpha read as 1 at the decision and at the issue alike
    const std::string vscreen = readFile("src/d3d11/vscreen.cpp");
    check(vscreen.size() > 100000, "vscreen.cpp is readable from the repository root");
    check(wiredMark(layer, vscreen), "the mark is named for the pair's shape, only while armed, with the atlas test, after the weapons-fire read; the layer counts it and reads its blend with destination alpha as 1 at both places");
    check(!wiredMark(layer, replaced(vscreen, "[] { return scopeZoomMarkArmed() || scopeZoomCrispArmed(); }", "[] { return true; }")), "control: a mark named whether or not the lens is armed is not the wiring");
    check(!wiredMark(layer, replaced(vscreen, "[] { return uiDepthSampledSurfaceSlot() >= 0; }", "[] { return true; }")), "control: a mark named without the atlas bound is not the wiring");
    check(!wiredMark(layer, replaced(vscreen, "[] { return bindingShaderHash(BindSlot::Ps); }", "[&] { return f.ps; }")), "control: a pixel shader not read from the binding is not the wiring");
    check(!wiredMark(replaced(layer, "const UiBlendShape shape = shapeOf(bs, &game, g_draw.destAlphaOne);", "const UiBlendShape shape = shapeOf(bs, &game);"), vscreen), "control: a blend read differently at the issue than at the decision is not the wiring");
    check(!wiredMark(replaced(layer, "describeBlend(gameBlend, bl, sizeof(bl));", "describeBlend(game, bl, sizeof(bl));"), vscreen), "control: a decline that prints the rewritten blend as the game's is not the wiring");
    check(!wiredMark(replaced(layer, "scopeZoomNoteMark(static_cast<uint32_t>(g_draw.eye), g_draw.seq, g_draw.markPath);", ""), vscreen), "control: marks not counted at the layer are not the wiring");
    check(!wiredMark(replaced(layer, "g_draw.weapons = uiLayerFamilyUsesWeaponsLayer(family);", "g_draw.weapons = family == UiLayerFamily::kWeaponsFire;"), vscreen), "control: a mark that is not drawn into the weapons layer is not the wiring");
    check(!wiredMark(replaced(layer, "markFrame.composited = result != nullptr;", ""), vscreen), "control: a composite the door does not report to the mark's count is not the wiring");
    check(!wiredMark(replaced(layer, "e.wPubMatched = plan.sourcePass;", ""), vscreen), "control: a re-issue that does not say whether it composited by the tonemap difference is not the wiring");
    check(!wiredMark(replaced(layer, "dsWhy, sizeof(dsWhy), markFamily && uiScopeMarkWritesNothing(dsState));", "dsWhy, sizeof(dsWhy), markFamily);"), vscreen),
          "control: a read-only view accepted for the mark whatever the draw writes is not the wiring");
    check(!wiredMark(replaced(layer, "if (markDepth == UiMarkDepth::kTestsOff) f.ds.depthTest = f.ds.stencilTest = false;", ""), vscreen), "control: a mark whose unreproducible tests are not dropped is not the wiring");
    // the lens declines for the left aiming eye (F162) before the gate: the tick hands the gate the key's eye
    check(before(native, "gi.leftAim = adsAimEye() == 0;", "sz::lensGate(gi)") && has(native, "case sz::LensVerdict::kLeftAim: ++s.ts.leftAim; break;"), "the tick hands the gate the aiming eye (F162) and counts its left aiming eye verdict");
    check(!(before(replaced(native, "gi.leftAim = adsAimEye() == 0;", "gi.leftAim = false;"), "gi.leftAim = adsAimEye() == 0;", "sz::lensGate(gi)")), "control: a gate that is not given the aiming eye is not the wiring");
    // A3: the S0 census arms its pixel probe only for a press of an optical sight (the S1 table's verdict, at the reference FOV); any other press says so on a skip line, once for each item
    // and scope a session, and RETURNS before the probe. The guard's block is read whole: it must hold the skip line and end in the return, in that order, with the probe's configure and
    // arm both after it. (An order of strings alone passes a guard that logs and falls through to the arm.)
    const auto wiredCensus = [](const std::string& n) {
        const std::string guard = "if (!sz::lensOpticalSight(v.item.name, v.item.scope, static_cast<double>(g_scopeZoomRefDeg.load(std::memory_order_relaxed)))) {";
        const size_t at = n.find(guard);
        const size_t next = at == std::string::npos ? at : n.find("    const uint32_t pe = kProbeGeometryEye;", at);   // the first statement after the guard
        if (at == std::string::npos || next == std::string::npos) return false;
        const std::string block = n.substr(at, next - at);
        const size_t skip = block.find("sz::formatCensusNotOptical(line, sizeof(line), v.press, v.item.name, loadout::scopeName(v.item.scope));");
        const size_t gate = block.find("v.notOpticalSaid.first(v.item.name, v.item.scope)");
        const size_t ret = block.rfind("        return;\n    }\n");
        return skip != std::string::npos && gate != std::string::npos && ret != std::string::npos && gate < skip && skip < ret && ret + std::strlen("        return;\n    }\n") == block.size() &&
               block.find("pixelProbeArm();") == std::string::npos && block.find("pixelProbeConfigure(") == std::string::npos && before(n, guard, "pixelProbeConfigure(set.pt, set.n);") &&
               before(n, "pixelProbeConfigure(set.pt, set.n);", "pixelProbeArm();");
    };
    check(wiredCensus(native), "the S0 census asks the S1 table's verdict (lensOpticalSight at the reference FOV); a press of any other sight is a skip line, once for each item and scope, and the guard returns before it configures or arms the pixel probe");
    check(!wiredCensus(replaced(native, "if (!sz::lensOpticalSight(v.item.name, v.item.scope, static_cast<double>(g_scopeZoomRefDeg.load(std::memory_order_relaxed)))) {", "if (false) {")),
          "control: a census that asks no table is not the wiring");
    check(!wiredCensus(replaced(native, "sz::formatCensusNotOptical(line, sizeof(line), v.press, v.item.name, loadout::scopeName(v.item.scope));", "")), "control: a press that is declined without its skip line is not the wiring");
    check(!wiredCensus(replaced(native, "        return;\n    }\n    const uint32_t pe = kProbeGeometryEye;", "    }\n    const uint32_t pe = kProbeGeometryEye;")),
          "control: a guard that logs the skip line and falls through to the probe is not the wiring");
    check(!wiredCensus(replaced(native, "if (v.press <= kPressLineCap && v.notOpticalSaid.first(v.item.name, v.item.scope)) {", "if (v.press <= kPressLineCap) {")), "control: a skip line written on every press is not the wiring");
    check(!wiredCensus(replaced(native, "        return;\n    }\n    const uint32_t pe = kProbeGeometryEye;", "        pixelProbeArm();\n        return;\n    }\n    const uint32_t pe = kProbeGeometryEye;")),
          "control: a guard that arms the probe before it returns is not the wiring");
}

// ============================================================================================ the scene
namespace {
constexpr uint32_t kRenW = 256, kRenH = 256, kOutW = 512, kOutH = 512;
constexpr float kTan4[4] = {-0.5f, 0.5f, -0.5f, 0.5f};     // 256 render px a unit of tangent: 12 degrees is 54 render px
constexpr double kNearDepth = 0.0417, kNearD = 0.14, kFarD = 0.00122;
constexpr double kCRen[2] = {70.3, 65.6};                   // c in render px, as the game drew it (the jitter in)
constexpr double kJitterPx[2] = {0.25, -0.125};            // the draw's jitter in render px
constexpr double kScale = 2.0;                              // output px per render px

struct Hex {
    double hx, hy, a, w, rot;
    double metric(double x, double y) const {
        double m = -1.0e30;
        for (int i = 0; i < 6; ++i) {
            const double ang = rot + i * (sz::kPi / 3.0);
            m = std::fmax(m, (x - hx) * std::cos(ang) + (y - hy) * std::sin(ang));
        }
        return m;
    }
    bool frame(double x, double y) const { const double m = metric(x, y); return m >= a && m <= a + w; }
    double cast(double cx, double cy, double dx, double dy) const {
        double t = 1.0e30;
        for (int i = 0; i < 6; ++i) {
            const double ang = rot + i * (sz::kPi / 3.0);
            const double nx = std::cos(ang), ny = std::sin(ang);
            const double along = nx * dx + ny * dy;
            if (along <= 1.0e-12) continue;
            t = std::fmin(t, (a - ((cx - hx) * nx + (cy - hy) * ny)) / along);
        }
        return t;
    }
};
const Hex kHex{73.0, 63.0, 32.0, 7.0, 0.2};

struct Rgba { uint8_t r, g, b, a; };
Rgba rampAt(int x, int y) { return Rgba{static_cast<uint8_t>(x & 255), static_cast<uint8_t>(y & 255), static_cast<uint8_t>(((x >> 8) * 80 + (y >> 8) * 40) & 255), 255}; }
struct Image {
    uint32_t w = 0, h = 0;
    std::vector<Rgba> px;   // semantic RGBA, whatever the texture's byte order
    Rgba& at(int x, int y) { return px[static_cast<size_t>(y) * w + x]; }
    const Rgba& at(int x, int y) const { return px[static_cast<size_t>(y) * w + x]; }
};
Image makeOrig() {
    Image im;
    im.w = kOutW;
    im.h = kOutH;
    im.px.resize(static_cast<size_t>(kOutW) * kOutH);
    for (uint32_t y = 0; y < kOutH; ++y)
        for (uint32_t x = 0; x < kOutW; ++x) im.at(static_cast<int>(x), static_cast<int>(y)) = rampAt(static_cast<int>(x), static_cast<int>(y));
    return im;
}

struct Scene {
    bool ring = true;
    double cRen[2] = {kCRen[0], kCRen[1]};     // c in render px (the scene's rays start here)
    int gapRay = -1;                           // two rays wide: the frame is cut away between rays gapRay and gapRay + 1
    std::vector<float> depth = std::vector<float>(static_cast<size_t>(kRenW) * kRenH);
    double depthAt(int x, int y) const { return depth[static_cast<size_t>(y) * kRenW + x]; }
};
Scene makeScene(bool ring, int gapRay) {
    Scene s;
    s.ring = ring;
    s.gapRay = gapRay;
    const double gapLo = gapRay >= 0 ? (gapRay * 2.0 * sz::kPi / sz::kLensRays - 1.0 / sz::kDeg) : 0.0, gapHi = gapRay >= 0 ? ((gapRay + 1) * 2.0 * sz::kPi / sz::kLensRays + 1.0 / sz::kDeg) : 0.0;
    for (int y = 0; y < static_cast<int>(kRenH); ++y)
        for (int x = 0; x < static_cast<int>(kRenW); ++x) {
            bool isNear = ring && kHex.frame(x + 0.5, y + 0.5);
            if (isNear && gapRay >= 0) {
                double ang = std::atan2(y + 0.5 - kCRen[1], x + 0.5 - kCRen[0]);
                if (ang < 0.0) ang += 2.0 * sz::kPi;
                if (ang >= gapLo && ang <= gapHi) isNear = false;
            }
            s.depth[static_cast<size_t>(y) * kRenW + x] = static_cast<float>(isNear ? kNearD : kFarD);
        }
    return s;
}

// The reference image of the lens, from scope_zoom_s1.h's own functions over the reference march: the picture in `orig`, magnified by m about c (output px), inside r(theta).
struct Expected {
    Image img;
    std::vector<double> edge;   // output px inside the hole's edge (negative outside), per pixel
};
Rgba catmullAt(const Image& im, double sx, double sy) {
    const double fx = sx - 0.5, fy = sy - 0.5;
    const int bx = static_cast<int>(std::floor(fx)), by = static_cast<int>(std::floor(fy));
    double wx[4], wy[4];
    sz::lensCatmullRom(fx - bx, wx);
    sz::lensCatmullRom(fy - by, wy);
    double acc[3] = {0, 0, 0};
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) {
            const int x = std::min(std::max(bx - 1 + i, 0), static_cast<int>(im.w) - 1), y = std::min(std::max(by - 1 + j, 0), static_cast<int>(im.h) - 1);
            const Rgba p = im.at(x, y);
            const double w = wx[i] * wy[j];
            acc[0] += w * p.r;
            acc[1] += w * p.g;
            acc[2] += w * p.b;
        }
    auto q = [](double v) { return static_cast<uint8_t>(std::min(std::max(std::lround(v), 0L), 255L)); };
    return Rgba{q(acc[0]), q(acc[1]), q(acc[2]), 255};
}
Expected expectedLens(const Scene& sc, const Image& orig, double m, double cx, double cy, double holeScale, double crOffX) {
    Expected e;
    e.img = orig;
    e.edge.assign(orig.px.size(), -1.0e9);
    sz::LensRay rays[sz::kLensRays];
    for (uint32_t k = 0; k < sz::kLensRays; ++k)
        rays[k] = sz::lensMarchRef([&](int x, int y) { return sc.depthAt(x, y); }, kRenW, kRenH, kCRen[0], kCRen[1], k, kTan4, kNearDepth);
    float r[sz::kLensRays];
    sz::lensClampEscaped(rays, r);
    for (uint32_t k = 0; k < sz::kLensRays; ++k) r[k] = static_cast<float>(r[k] * holeScale);
    for (int y = 0; y < static_cast<int>(orig.h); ++y)
        for (int x = 0; x < static_cast<int>(orig.w); ++x) {
            const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
            const double dRx = dx / kScale, dRy = dy / kScale;
            const double rr = std::sqrt(dRx * dRx + dRy * dRy);
            if (rr > 120.0) continue;
            const double inside = (sz::lensRadiusAt(r, std::atan2(dRy, dRx)) - rr) * kScale;
            e.edge[static_cast<size_t>(y) * orig.w + x] = inside;
            const double w = sz::lensWeight(inside);
            if (w <= 0.0) continue;
            const double c2[2] = {cx + crOffX, cy}, p[2] = {x + 0.5, y + 0.5};
            double s[2];
            sz::lensSourceOf(c2, m, p, s);
            const Rgba mag = catmullAt(orig, s[0], s[1]), o = orig.at(x, y);
            auto mix = [&](uint8_t a, uint8_t b) { return static_cast<uint8_t>(std::lround(a + (b - a) * w)); };
            e.img.at(x, y) = Rgba{mix(o.r, mag.r), mix(o.g, mag.g), mix(o.b, mag.b), 255};
        }
    return e;
}
// The comparator: pixels well inside the hole within `tol` codes of the reference, pixels outside it (and the ring) identical to the original. Counts the pixels that disagree.
struct Verdict { unsigned inside = 0, outside = 0, insideBad = 0, outsideBad = 0, insideChanged = 0; };
Verdict compare(const Image& got, const Image& orig, const Expected& e, int tol) {
    Verdict v;
    for (size_t i = 0; i < got.px.size(); ++i) {
        const double edge = e.edge[i];
        const Rgba g = got.px[i], o = orig.px[i], x = e.img.px[i];
        if (edge >= 4.0) {
            ++v.inside;
            if (std::abs(g.r - x.r) > tol || std::abs(g.g - x.g) > tol || std::abs(g.b - x.b) > tol) ++v.insideBad;
            if (g.r != o.r || g.g != o.g || g.b != o.b) ++v.insideChanged;
        } else if (edge <= -4.0) {
            ++v.outside;
            if (g.r != o.r || g.g != o.g || g.b != o.b || g.a != o.a) ++v.outsideBad;
        }
    }
    return v;
}

struct Fx {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11InfoQueue> info;
    bool debugLayer = false;
    bool bgraStore = false;
    explicit Fx(bool hardware) {
        D3D_FEATURE_LEVEL level;
        const D3D_DRIVER_TYPE type = hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;
        UINT flags = hardware ? 0 : D3D11_CREATE_DEVICE_DEBUG;
        HRESULT r = D3D11CreateDevice(nullptr, type, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx);
        if (FAILED(r)) {
            flags = 0;
            r = D3D11CreateDevice(nullptr, type, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx);
        }
        hr(r, "a WARP device");
        debugLayer = (flags & D3D11_CREATE_DEVICE_DEBUG) != 0;
        if (debugLayer) dev.As(&info);
        UINT s = 0;
        bgraStore = SUCCEEDED(dev->CheckFormatSupport(DXGI_FORMAT_B8G8R8A8_UNORM, &s)) && (s & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW);
        UINT s2 = 0;
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 fs2{DXGI_FORMAT_B8G8R8A8_UNORM, 0};
        if (bgraStore && SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &fs2, sizeof(fs2)))) {
            s2 = fs2.OutFormatSupport2;
            bgraStore = (s2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
        }
    }
    ComPtr<ID3D11Texture2D> frameTex(DXGI_FORMAT fmt, const Image& im, UINT bind) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = kOutW; d.Height = kOutH; d.MipLevels = d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
        ComPtr<ID3D11Texture2D> t;
        hr(dev->CreateTexture2D(&d, nullptr, &t), "the rig's frame");
        upload(t.Get(), fmt, im);
        return t;
    }
    void upload(ID3D11Texture2D* t, DXGI_FORMAT fmt, const Image& im) {
        std::vector<uint8_t> bytes(static_cast<size_t>(im.w) * im.h * 4);
        for (size_t i = 0; i < im.px.size(); ++i) {
            const Rgba p = im.px[i];
            const bool bgra = fmt == DXGI_FORMAT_B8G8R8A8_UNORM;
            bytes[4 * i + 0] = bgra ? p.b : p.r;
            bytes[4 * i + 1] = p.g;
            bytes[4 * i + 2] = bgra ? p.r : p.b;
            bytes[4 * i + 3] = p.a;
        }
        ctx->UpdateSubresource(t, 0, nullptr, bytes.data(), im.w * 4, 0);
    }
    Image read(ID3D11Texture2D* t, DXGI_FORMAT fmt) {
        D3D11_TEXTURE2D_DESC d{};
        t->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> stage;
        hr(dev->CreateTexture2D(&d, nullptr, &stage), "staging");
        ctx->CopyResource(stage.Get(), t);
        D3D11_MAPPED_SUBRESOURCE m{};
        hr(ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "map");
        Image im;
        im.w = d.Width;
        im.h = d.Height;
        im.px.resize(static_cast<size_t>(im.w) * im.h);
        for (uint32_t y = 0; y < im.h; ++y)
            for (uint32_t x = 0; x < im.w; ++x) {
                const uint8_t* p = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch + 4 * x;
                const bool bgra = fmt == DXGI_FORMAT_B8G8R8A8_UNORM;
                im.at(static_cast<int>(x), static_cast<int>(y)) = Rgba{bgra ? p[2] : p[0], p[1], bgra ? p[0] : p[2], p[3]};
            }
        ctx->Unmap(stage.Get(), 0);
        return im;
    }
    // The scene's depth as the game's own texture: the depth channel of R32G8X24_TYPELESS, viewed R32_FLOAT_X8X24_TYPELESS (as temporal_pass.cpp makes it).
    ComPtr<ID3D11ShaderResourceView> depthSrv(const Scene& sc, ComPtr<ID3D11Texture2D>* keep) {
        std::vector<uint32_t> raw(static_cast<size_t>(kRenW) * kRenH * 2, 0);
        for (size_t i = 0; i < sc.depth.size(); ++i) std::memcpy(&raw[2 * i], &sc.depth[i], 4);
        D3D11_TEXTURE2D_DESC d{};
        d.Width = kRenW; d.Height = kRenH; d.MipLevels = d.ArraySize = 1; d.Format = DXGI_FORMAT_R32G8X24_TYPELESS; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{raw.data(), kRenW * 8, 0};
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateTexture2D(&d, &sd, keep->ReleaseAndGetAddressOf()))) {   // the plain float depth view reads the same bytes the shader sees
            d.Format = DXGI_FORMAT_R32_FLOAT;
            v.Format = DXGI_FORMAT_R32_FLOAT;
            D3D11_SUBRESOURCE_DATA sd2{sc.depth.data(), kRenW * 4, 0};
            hr(dev->CreateTexture2D(&d, &sd2, keep->ReleaseAndGetAddressOf()), "the depth texture (R32_FLOAT)");
            std::puts("scope_zoom_test: this adapter would not make an R32G8X24_TYPELESS texture with initial data: the depth is R32_FLOAT here");
        }
        ComPtr<ID3D11ShaderResourceView> s;
        hr(dev->CreateShaderResourceView(keep->Get(), &v, &s), "the depth view");
        return s;
    }
};

struct Run {
    Image image;                  // the frame the lens left (or the copy it handed back)
    Image original;               // the frame texture after the lens (to see it was not written when it must not be)
    bool gotCopy = false;
    sz::LensDoorStats stats;
};
struct RunIn {
    DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool inPlace = true, resolved = true, hasDepth = true, covers = true;
    uint32_t regionTrim = 0;       // the region handed to the door is this many px narrower than the texture
    int jitterSeqDelta = 0;        // the record's jitter sequence differs from the door's sequence by this
    sz::LensVerdict verdict = sz::LensVerdict::kQualifies;
    double m = 2.5;
    uint32_t sleepMs = 0;          // between the publish and the door: a c this old is stale
};
// One press as the production pieces see it: publish, door (with the frame re-uploaded before each call), the counters taken. A lens that runs is called until its rays have been
// read back (they come a few calls later); the picture compared is the last call's, which starts from the original like every other.
Run runDoor(Fx& f, const Scene& sc, const Image& orig, const RunIn& in) {
    ComPtr<ID3D11Texture2D> depthTex;
    ComPtr<ID3D11ShaderResourceView> srv = f.depthSrv(sc, &depthTex);
    const UINT bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> frame = f.frameTex(in.fmt, orig, bind);
    scopeZoomDepthClear(1);
    if (in.hasDepth) scopeZoomDepthSet(1, srv.Get(), kRenW, kRenH, static_cast<float>(kNearDepth), in.covers);
    static uint32_t press = 0;   // a press of its own each run: the rays still in flight from the run before belong to that one and are dropped
    ++press;
    scopeZoomPressBegin(press);
    ScopeZoomFrame rec;
    rec.verdict = static_cast<uint32_t>(in.verdict);
    rec.press = press;
    rec.m = static_cast<float>(in.m);
    rec.mTarget = static_cast<float>(in.m);
    rec.uv[0] = static_cast<float>(sc.cRen[0] / kRenW);
    rec.uv[1] = static_cast<float>(sc.cRen[1] / kRenH);
    rec.jitterUv[0] = static_cast<float>(kJitterPx[0] / kRenW);
    rec.jitterUv[1] = static_cast<float>(kJitterPx[1] / kRenH);
    rec.jitterSeq = static_cast<uint64_t>(100 + in.jitterSeqDelta);
    std::memcpy(rec.tan4, kTan4, sizeof(kTan4));
    scopeZoomPublish(rec);
    if (in.sleepMs) Sleep(in.sleepMs);
    const bool wait = in.hasDepth && in.covers && in.verdict == sz::LensVerdict::kQualifies && in.m >= 1.001 && !in.sleepMs && in.resolved && !in.regionTrim;
    const uint32_t region[4] = {0, 0, kOutW - in.regionTrim, kOutH};
    ComPtr<ID3D11Texture2D> copy;
    Run out;
    uint32_t dispatchedBefore = 0;
    for (uint32_t i = 0; i < (wait ? 400u : 1u); ++i) {
        f.upload(frame.Get(), in.fmt, orig);
        if (i) {   // published right before the door: c must be under 50 ms old when it is read
            rec.jitterSeq = static_cast<uint64_t>(100 + i + in.jitterSeqDelta);
            scopeZoomPublish(rec);
        }
        ScopeZoomDoorArgs a;
        a.eye = 1;
        a.sequence = 100 + i;
        a.frame = frame.Get();
        a.region = region;
        a.inPlace = in.inPlace;
        a.resolved = in.resolved;
        copy.Reset();
        scopeZoomDoor(a, &copy);
        f.ctx->Flush();
        if (!wait) break;
        sz::LensDoorStats peek;
        scopeZoomPressTake(&peek);
        // A machine that held the door past c's 50 ms (the rigs run in parallel, CI runners are shared) makes that frame stale: it wrote nothing, and it is no part of this press. The press
        // starts again under a new id, so what is counted and compared is the frames the lens ran on (#150).
        if (peek.staleC) {
            ++press;
            scopeZoomPressBegin(press);
            rec.press = press;
            dispatchedBefore = 0;
            continue;
        }
        // The door also polls the rays before it checks c's age, so a stale frame still counts a read: stop on a frame the door wrote.
        const bool wrote = peek.dispatched > dispatchedBefore;
        dispatchedBefore = peek.dispatched;
        if (peek.raysRead >= 1 && wrote) break;
        Sleep(1);
    }
    out.gotCopy = copy != nullptr;
    out.image = f.read(copy ? copy.Get() : frame.Get(), in.fmt);
    out.original = f.read(frame.Get(), in.fmt);
    scopeZoomPressTake(&out.stats);
    scopeZoomDepthClear(1);
    return out;
}
double cOutX() { return (kCRen[0] / kRenW - kJitterPx[0] / kRenW) * kOutW; }
double cOutY() { return (kCRen[1] / kRenH - kJitterPx[1] / kRenH) * kOutH; }
bool sameImage(const Image& a, const Image& b) { return a.w == b.w && a.h == b.h && a.px.size() == b.px.size() && std::memcmp(a.px.data(), b.px.data(), a.px.size() * sizeof(Rgba)) == 0; }

void lensTests(Fx& f) {
    const Image orig = makeOrig();
    const Scene ring = makeScene(true, -1);
    const double cx = cOutX(), cy = cOutY();
    check(std::fabs(cx - 140.1) < 1e-3 && std::fabs(cy - 131.45) < 1e-3, "the rig's c in output px is c less the jitter, scaled (140.10, 131.45)");

    // ---- the real thing, in place, RGBA and BGRA ----
    for (int pass = 0; pass < 2; ++pass) {
        const DXGI_FORMAT fmt = pass == 0 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
        if (pass == 1 && !f.bgraStore) {
            std::puts("scope_zoom_test: this adapter has no typed UAV store for B8G8R8A8_UNORM: that format's cases are skipped");
            continue;
        }
        RunIn in;
        in.fmt = fmt;
        const Run r = runDoor(f, ring, orig, in);
        const Expected e = expectedLens(ring, orig, in.m, cx, cy, 1.0, 0.0);
        const Verdict v = compare(r.image, orig, e, 1);
        std::printf("lens %s in place: %u px inside the hole (%u bad, %u changed), %u px outside it (%u bad)\n", pass == 0 ? "RGBA" : "BGRA", v.inside, v.insideBad, v.insideChanged, v.outside, v.outsideBad);
        check(!r.gotCopy && v.inside > 2000 && v.insideBad == 0 && v.outsideBad == 0, "inside the ring the frame is sampled at c + d / M (Catmull-Rom, within one code of the reference); the ring and everything outside are untouched");
        check(v.insideChanged * 10 > v.inside * 9, "and the lens really changed the picture over (nearly) all of the hole");
        check(r.stats.dispatched >= 1 && r.stats.noop == 0 && r.stats.staleC == 0 && r.stats.depthUnavailable == 0 && r.stats.formatRefused == 0 && r.stats.noS == 0, "the qualifying frames were dispatched and none was declined");
        // the mask's edge on the ring's inner edge: scan inward from outside along 24 directions for the first pixel the lens changed
        double worst = 0.0;
        for (int k = 0; k < 24; ++k) {
            const double phi = k * 2.0 * sz::kPi / 24.0 + 0.04, dx = std::cos(phi), dy = std::sin(phi);
            double found = -1.0;
            for (double t = 160.0; t > 6.0; t -= 0.5) {
                const int x = static_cast<int>(std::floor(cx + dx * t)), y = static_cast<int>(std::floor(cy + dy * t));
                if (x < 0 || y < 0 || x >= static_cast<int>(kOutW) || y >= static_cast<int>(kOutH)) continue;
                const Rgba g = r.image.at(x, y), o = orig.at(x, y);
                if (std::abs(g.r - o.r) + std::abs(g.g - o.g) >= 6) { found = t; break; }
            }
            const double exact = kHex.cast(kCRen[0], kCRen[1], dx, dy) * kScale;   // render px to output px
            check(found > 0.0, "the scan found the lens's edge");
            worst = std::fmax(worst, std::fabs(found - exact));
        }
        std::printf("lens edge: worst distance from the ring's inner edge %.2f output px\n", worst);
        check(worst <= 4.0, "the lens ends at the ring's inner edge, to 4 output px, in every direction scanned");
        if (pass == 0) {
            // the comparator's mutants: each must fail the check that passes the real thing
            const Verdict wrongM = compare(r.image, orig, expectedLens(ring, orig, in.m * 1.2, cx, cy, 1.0, 0.0), 1);
            const Verdict wrongC = compare(r.image, orig, expectedLens(ring, orig, in.m, cx, cy, 1.0, 4.0), 1);
            const Verdict tiny = compare(r.image, orig, expectedLens(ring, orig, in.m, cx, cy, 0.8, 0.0), 1);
            check(wrongM.insideBad > 100, "mutant: M off by 20 percent is caught");
            check(wrongC.insideBad > 100, "mutant: the source taken 4 px off c is caught");
            check(tiny.insideBad + tiny.outsideBad > 100, "mutant: the hole 20 percent too small is caught");
            // the rays read back: the hole the instrument reports against the reference march
            check(r.stats.raysRead >= 1, "the rays were read back");
            sz::LensRay rays[sz::kLensRays];
            sz::LensReadback ref;
            for (uint32_t k = 0; k < sz::kLensRays; ++k) {
                rays[k] = sz::lensMarchRef([&](int x, int y) { return ring.depthAt(x, y); }, kRenW, kRenH, kCRen[0], kCRen[1], k, kTan4, kNearDepth);
                ref.rDeg[k] = static_cast<float>(rays[k].rDeg);
            }
            ref.escaped = 0;
            double rmin, rmed, rmax;
            check(sz::lensHoleStats(ref, &rmin, &rmed, &rmax), "the reference march has a hole");
            std::printf("hole: GPU r min %.3f median %.3f max %.3f deg, %u escaped; reference %.3f %.3f %.3f\n", r.stats.rMinDeg, r.stats.rMedianDeg, r.stats.rMaxDeg, r.stats.holeEscaped, rmin, rmed, rmax);
            check(r.stats.holeKnown && r.stats.holeEscaped == 0 && std::fabs(r.stats.rMinDeg - rmin) < 0.02 && std::fabs(r.stats.rMedianDeg - rmed) < 0.02 && std::fabs(r.stats.rMaxDeg - rmax) < 0.02,
                  "the hole the GPU's rays give (min, median, max in degrees, none escaped) is the reference march's, to 0.02 degrees");
            check(std::fabs(r.stats.depthAtC - kFarD) < 1e-6 && std::fabs(r.stats.depthNear - kNearDepth) < 1e-6, "the depth at c (far, 0.00122) and the near line are what the line reports");
            check(r.stats.cN >= 1 && std::fabs(r.stats.cSumX / r.stats.cN - cx) < 1e-3 && std::fabs(r.stats.cSumY / r.stats.cN - cy) < 1e-3, "c's counted place is c in output px");
        }
    }

    // ---- a frame EDVR may not write: the lens's own copy comes back, the original is untouched ----
    {
        RunIn in;
        in.inPlace = false;
        const Run r = runDoor(f, ring, orig, in);
        const Verdict v = compare(r.image, orig, expectedLens(ring, orig, in.m, cx, cy, 1.0, 0.0), 1);
        check(r.gotCopy && v.insideBad == 0 && v.outsideBad == 0 && v.insideChanged * 10 > v.inside * 9, "a frame that is not EDVR's to rewrite comes back as the lens's copy, magnified as in place");
        check(sameImage(r.original, orig), "and the original frame is not written");
    }

    // ---- a ring with a two-ray gap still has a lens, the gap clamped to its neighbours ----
    {
        const Scene gap = makeScene(true, 20);
        uint32_t esc = 0;
        for (uint32_t k = 0; k < sz::kLensRays; ++k) esc += sz::lensMarchRef([&](int x, int y) { return gap.depthAt(x, y); }, kRenW, kRenH, kCRen[0], kCRen[1], k, kTan4, kNearDepth).escaped ? 1u : 0u;
        check(esc >= 1 && esc <= 2, "the reference march sees the gap as one or two escaped rays, not an open sight");
        RunIn in;
        const Run r = runDoor(f, gap, orig, in);
        const Verdict v = compare(r.image, orig, expectedLens(gap, orig, in.m, cx, cy, 1.0, 0.0), 1);
        std::printf("lens with a gap (%u rays escaped): %u px inside (%u bad), %u outside (%u bad); GPU escaped %u\n", esc, v.inside, v.insideBad, v.outside, v.outsideBad, r.stats.holeEscaped);
        check(v.insideBad == 0 && v.outsideBad == 0 && v.insideChanged * 10 > v.inside * 9, "a gap in the ring still has its lens, to the reference built with the escaped rays clamped to their neighbours");
        check(r.stats.holeKnown && r.stats.holeEscaped == esc && r.stats.openSight == 0 && r.stats.applied >= 1, "and the instrument counts the escaped rays and no open sight");
    }

    // ---- an open ring (no frame): the frame is left alone and the sight is counted open ----
    {
        const Scene open = makeScene(false, -1);
        RunIn in;
        const Run r = runDoor(f, open, orig, in);
        check(sameImage(r.image, orig) && !r.gotCopy, "an open sight (no frame in the depth) leaves the picture as it was");
        check(r.stats.dispatched >= 1 && r.stats.raysRead >= 1 && r.stats.openSight == r.stats.raysRead && r.stats.applied + r.stats.openSight >= r.stats.dispatched && !r.stats.holeKnown,
              "and the rays read back say open sight, with no hole to report");
    }

    // ---- an open sight with c near the picture's edge: the rays that leave the image are escapes (the sentinel), not hits in the hole's statistics ----
    {
        Scene edge = makeScene(false, -1);
        edge.cRen[0] = 20.0;
        edge.cRen[1] = 128.0;
        RunIn in;
        const Run r = runDoor(f, edge, orig, in);
        check(sameImage(r.image, orig) && r.stats.raysRead >= 1 && r.stats.openSight == r.stats.raysRead && !r.stats.holeKnown,
              "an open sight whose rays leave the image on one side reports no hole: an off-image ray is an escape, not a hit with an angle under 12 degrees");
    }

    // ---- a c from the draws of another frame than the picture is counted, and the lens still applies ----
    {
        RunIn in;
        in.jitterSeqDelta = 1;
        const Run r = runDoor(f, ring, orig, in);
        const Verdict v = compare(r.image, orig, expectedLens(ring, orig, in.m, cx, cy, 1.0, 0.0), 1);
        check(sz::lensOtherFrames(r.stats) >= 1 && sz::lensOtherFrames(r.stats) == r.stats.dispatched && r.stats.seqGap[0] == r.stats.dispatched && v.insideBad == 0 && v.outsideBad == 0 &&
                  v.insideChanged * 10 > v.inside * 9,
              "a c whose jitter sequence is one after the picture's is counted in the -1 bucket and the lens still runs on it");
        const Run same = runDoor(f, ring, orig, RunIn{});
        check(sz::lensOtherFrames(same.stats) == 0 && same.stats.seqGap[1] == same.stats.dispatched && same.stats.dispatched >= 1, "a c from the picture's own frame is counted in the 0 bucket and not as another frame");
        // the other buckets, one frame each (a lens at M 1 counts the gap too: the reading is what the bucket is about)
        const struct { int delta; uint32_t bucket; } gaps[] = {{1, 0}, {0, 1}, {-1, 2}, {-2, 3}, {-3, 4}, {7, 4}};
        for (const auto& g : gaps) {
            RunIn gi;
            gi.m = 1.0;
            gi.jitterSeqDelta = g.delta;
            const Run gr = runDoor(f, ring, orig, gi);
            uint32_t total = 0;
            for (uint32_t k = 0; k < sz::kLensGapBuckets; ++k) total += gr.stats.seqGap[k];
            check(total == 1 && gr.stats.seqGap[g.bucket] == 1, "the picture's sequence less c's lands in its bucket: -1, 0, +1, +2, other");
        }
    }

    // ---- the scope's gunsight mark: armed only for a qualifying fresh frame; marks per eye; armed frames with and without a mark draw in the right eye ----
    {
        ScopeZoomFrame rec;
        rec.m = rec.mTarget = 2.5f;
        rec.verdict = static_cast<uint32_t>(sz::LensVerdict::kQualifies);
        scopeZoomPublish(rec);
        check(scopeZoomMarkArmed(), "the mark is armed while the last published frame qualifies for the lens");
        rec.verdict = static_cast<uint32_t>(sz::LensVerdict::kNotOptical);
        scopeZoomPublish(rec);
        check(!scopeZoomMarkArmed(), "a weapon that is not an optical sight does not arm the mark");
        rec.verdict = static_cast<uint32_t>(sz::LensVerdict::kNoS);
        scopeZoomPublish(rec);
        check(!scopeZoomMarkArmed(), "a frame with no S does not arm the mark");
        rec.verdict = static_cast<uint32_t>(sz::LensVerdict::kIdle);
        scopeZoomPublish(rec);
        check(!scopeZoomMarkArmed(), "an idle frame (the key off, not in the sights) does not arm the mark");
        rec.verdict = static_cast<uint32_t>(sz::LensVerdict::kLeftAim);
        scopeZoomPublish(rec);
        check(!scopeZoomMarkArmed(), "a frame declined for the left aiming eye does not arm the mark (the lens is the right eye's)");
        rec.verdict = static_cast<uint32_t>(sz::LensVerdict::kQualifies);
        scopeZoomPublish(rec);
        Sleep(120);
        check(!scopeZoomMarkArmed(), "a qualifying frame older than the door takes a c does not arm the mark");
        // the door counts a right-eye frame as armed, and as without a mark draw unless the right eye's mark was redirected in the picture's own sequence (runDoor's first sequence is 100)
        RunIn in;
        in.m = 1.0;
        scopeZoomNoteMark(1, 100, sz::kMarkPathSeeded);
        const Run drawn = runDoor(f, ring, orig, in);
        check(drawn.stats.markArmedFrames == 1 && drawn.stats.markNoDraw == 0, "an armed right-eye frame whose mark draw was redirected in its own sequence has a mark draw");
        scopeZoomNoteMark(1, 99, sz::kMarkPathSeeded);
        const Run missed = runDoor(f, ring, orig, in);
        check(missed.stats.markArmedFrames == 1 && missed.stats.markNoDraw == 1, "an armed right-eye frame whose last mark draw is from another sequence is counted with no mark draw");
        scopeZoomNoteMark(1, 0, sz::kMarkPathSeeded);
        in.verdict = sz::LensVerdict::kNotOptical;
        const Run unarmed = runDoor(f, ring, orig, in);
        check(unarmed.stats.markArmedFrames == 0 && unarmed.stats.markNoDraw == 0, "a frame the gate declined is not an armed frame");
        in.verdict = sz::LensVerdict::kLeftAim;
        const Run leftAim = runDoor(f, ring, orig, in);
        check(leftAim.stats.leftAim == 1 && leftAim.stats.qualifying == 0 && leftAim.stats.markArmedFrames == 0 && leftAim.stats.markNoDraw == 0 && sameImage(leftAim.image, orig) && !leftAim.gotCopy,
              "a frame declined for the left aiming eye is counted under its own reason, asks nothing of the passes, leaves the frame alone and is not an armed frame");
        // a qualifying frame whose c is stale is the door's decline (stale c) and is not an armed frame either: the family is armed for fresh frames, and an armed frame with no
        // mark draw is a frame the mark could have been taken from
        in.verdict = sz::LensVerdict::kQualifies;
        in.sleepMs = 150;
        const Run stale = runDoor(f, ring, orig, in);
        check(stale.stats.staleC == 1 && stale.stats.markArmedFrames == 0 && stale.stats.markNoDraw == 0, "a qualifying frame with a stale c is counted stale and is neither an armed frame nor a frame with no mark draw");
        in.sleepMs = 0;
        // marks per eye, counted at the draw, and by the path the layer took their depth and stencil tests
        scopeZoomPressBegin(77);
        scopeZoomNoteMark(0, 5, sz::kMarkPathSeeded);
        scopeZoomNoteMark(1, 5, sz::kMarkPathSeededReadOnly);
        scopeZoomNoteMark(1, 6, sz::kMarkPathTestsOff);
        scopeZoomNoteMark(1, 6, sz::kMarkPathUntested);
        scopeZoomNoteMark(2, 7, sz::kMarkPathSeeded);
        scopeZoomNoteMark(1, 8, sz::kMarkPaths);   // a path that is none is not counted by path
        sz::LensDoorStats marks;
        scopeZoomPressTake(&marks);
        check(marks.markRedirected[0] == 1 && marks.markRedirected[1] == 4, "marks redirected are counted per eye (an eye that is neither is ignored)");
        check(marks.markPath[sz::kMarkPathSeeded] == 1 && marks.markPath[sz::kMarkPathSeededReadOnly] == 1 && marks.markPath[sz::kMarkPathTestsOff] == 1 && marks.markPath[sz::kMarkPathUntested] == 1,
              "marks redirected are counted by the path their depth and stencil tests took (a path that is none is not counted)");
        // the weapons layer's composite of an eye-frame that held a mark (A3), counted by how it served the frame: the tonemap difference, the plain add, or not at all
        scopeZoomPressBegin(78);
        scopeZoomNoteMarkComposite(sz::kMarkCompositeMatched);
        scopeZoomNoteMarkComposite(sz::kMarkCompositeMatched);
        scopeZoomNoteMarkComposite(sz::kMarkCompositePlain);
        scopeZoomNoteMarkComposite(sz::kMarkCompositeNone);
        scopeZoomNoteMarkComposite(sz::kMarkComposites);   // a kind that is none is not counted
        sz::LensDoorStats comps;
        scopeZoomPressTake(&comps);
        check(comps.markComposite[sz::kMarkCompositeMatched] == 2 && comps.markComposite[sz::kMarkCompositePlain] == 1 && comps.markComposite[sz::kMarkCompositeNone] == 1,
              "mark composites are counted by kind (the tonemap difference, the plain add, none), a kind that is none is not counted");
        scopeZoomPressBegin(79);
        sz::LensDoorStats fresh;
        scopeZoomPressTake(&fresh);
        check(fresh.markComposite[0] == 0 && fresh.markComposite[1] == 0 && fresh.markComposite[2] == 0, "a press begins with no mark composite counted");
    }

    // ---- the reasons that decline a frame, each counted and each leaving the frame alone ----
    {
        RunIn in;
        in.fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
        // a format the lens refuses: the frame is built by hand (the rig's pixel helpers are 8-bit)
        ComPtr<ID3D11Texture2D> depthTex;
        ComPtr<ID3D11ShaderResourceView> srv = f.depthSrv(ring, &depthTex);
        D3D11_TEXTURE2D_DESC d{};
        d.Width = kOutW; d.Height = kOutH; d.MipLevels = d.ArraySize = 1; d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> half;
        hr(f.dev->CreateTexture2D(&d, nullptr, &half), "a half-float frame");
        scopeZoomDepthSet(1, srv.Get(), kRenW, kRenH, static_cast<float>(kNearDepth), true);
        scopeZoomPressBegin(1);
        ScopeZoomFrame rec;
        rec.verdict = static_cast<uint32_t>(sz::LensVerdict::kQualifies);
        rec.m = rec.mTarget = 2.5f;
        rec.uv[0] = static_cast<float>(kCRen[0] / kRenW);
        rec.uv[1] = static_cast<float>(kCRen[1] / kRenH);
        std::memcpy(rec.tan4, kTan4, sizeof(kTan4));
        scopeZoomPublish(rec);
        const uint32_t region[4] = {0, 0, kOutW, kOutH};
        ScopeZoomDoorArgs a;
        a.eye = 1;
        a.frame = half.Get();
        a.region = region;
        a.inPlace = true;
        a.resolved = true;
        ComPtr<ID3D11Texture2D> copy;
        scopeZoomDoor(a, &copy);
        sz::LensDoorStats s;
        scopeZoomPressTake(&s);
        check(!copy && s.formatRefused == 1 && s.dispatched == 0 && s.qualifying == 1, "a frame that is not R8G8B8A8 or B8G8R8A8 UNORM is refused, counted as format refused and handed back as it was");
        scopeZoomDepthClear(1);
    }
    {
        const struct { const char* what; RunIn in; } cases[] = {
            {"a c older than 50 ms", [] { RunIn i; i.sleepMs = 150; return i; }()},
            {"no depth for the eye", [] { RunIn i; i.hasDepth = false; return i; }()},
            {"a picture that is not the upscaler's", [] { RunIn i; i.resolved = false; return i; }()},
            {"the gate's verdict no S", [] { RunIn i; i.verdict = sz::LensVerdict::kNoS; return i; }()},
            {"the gate's verdict not an optical sight", [] { RunIn i; i.verdict = sz::LensVerdict::kNotOptical; return i; }()},
            {"the gate's verdict left aiming eye", [] { RunIn i; i.verdict = sz::LensVerdict::kLeftAim; return i; }()},
            {"the idle verdict", [] { RunIn i; i.verdict = sz::LensVerdict::kIdle; return i; }()},
            {"M at 1 (the first frame of the ease)", [] { RunIn i; i.m = 1.0; return i; }()},
            {"no depth for the eye on the first frame of the ease", [] { RunIn i; i.hasDepth = false; i.m = 1.0; return i; }()},
            {"a picture that is not the upscaler's on the first frame of the ease", [] { RunIn i; i.resolved = false; i.m = 1.0; return i; }()},
            {"a depth that does not cover the render", [] { RunIn i; i.covers = false; return i; }()},
            {"a region that is not the whole texture, in place", [] { RunIn i; i.regionTrim = 8; return i; }()},
            {"a region that is not the whole texture, on the copy route", [] { RunIn i; i.regionTrim = 8; i.inPlace = false; return i; }()},
        };
        for (const auto& c : cases) {
            const Run r = runDoor(f, ring, orig, c.in);
            check(sameImage(r.image, orig) && !r.gotCopy, c.what);
            const sz::LensDoorStats& s = r.stats;
            const std::string what = c.what;
            if (what == "a c older than 50 ms") check(s.staleC == 1 && s.dispatched == 0, "a c older than 50 ms is counted stale and leaves the frame alone");
            else if (what == "no depth for the eye") check(s.depthUnavailable == 1 && s.dispatched == 0, "no depth is counted depth unavailable and leaves the frame alone");
            else if (what == "a picture that is not the upscaler's") check(s.formatRefused == 1 && s.dispatched == 0, "a picture that is not the upscaler's is refused and left alone");
            else if (what == "the gate's verdict no S") check(s.noS == 1 && s.qualifying == 0, "the gate's no S is counted and asks nothing of the passes");
            else if (what == "the gate's verdict not an optical sight") check(s.notOptical == 1 && s.qualifying == 0, "the gate's not an optical sight is counted and asks nothing of the passes");
            else if (what == "the gate's verdict left aiming eye") check(s.leftAim == 1 && s.qualifying == 0 && s.dispatched == 0, "the gate's left aiming eye is counted and asks nothing of the passes");
            else if (what == "the idle verdict") check(s.noS + s.notOptical + s.leftAim + s.staleC + s.dispatched + s.noop + s.formatRefused + s.depthUnavailable == 0, "the idle verdict counts nothing");
            else if (what == "no depth for the eye on the first frame of the ease") check(s.depthUnavailable == 1 && s.noop == 0 && s.applied == 0 && s.qualifying == 1, "the first frame at M 1 with no depth is depth unavailable, not a frame with nothing to do");
            else if (what == "a picture that is not the upscaler's on the first frame of the ease") check(s.formatRefused == 1 && s.noop == 0 && s.applied == 0 && s.qualifying == 1, "the first frame at M 1 over a picture that is not the upscaler's is refused, not a frame with nothing to do");
            else if (what == "a depth that does not cover the render") check(s.depthUnavailable == 1 && s.dispatched == 0 && s.applied == 0, "a depth the picture's uv does not map onto is counted depth unavailable and the frame is left alone");
            else if (what == "a region that is not the whole texture, in place") check(s.formatRefused == 1 && s.dispatched == 0 && s.applied == 0, "a region that is not the whole texture is refused in place (the mapping of c needs the whole picture)");
            else if (what == "a region that is not the whole texture, on the copy route") check(s.formatRefused == 1 && s.dispatched == 0 && !r.gotCopy, "a region that is not the whole texture is refused on the copy route too, and no copy is handed back");
            else check(s.noop == 1 && s.dispatched == 0 && s.applied == 0 && s.qualifying == 1, "M at 1 is counted as qualifying, nothing is issued and it is not applied");
        }
    }
    // the eye that is not the aiming one is not called, and a call with no frame does nothing
    {
        ScopeZoomDoorArgs a;
        a.eye = 0;
        ComPtr<ID3D11Texture2D> copy;
        scopeZoomDoor(a, &copy);
        a.eye = 1;
        scopeZoomDoor(a, &copy);
        check(!copy, "a door call for the left eye, or with no frame, does nothing");
    }
}

// ============================================================================================ B3: the left eye drawn as the zoomed view
// A synthetic world seen by two eyes 6.3 cm apart. The right eye (the aiming one) looks down +z with the rig's tangents; the left eye is drawn zoomed: M = 4, a window about its direction to the
// far point c. A textured plane at distance Z (or a sky, a function of the direction alone) fills the scope's hole. The right picture is a ramp (unrelated to the world, so a lens that did
// not take its picture from the left eye is seen at once); the left picture is the world as the left eye draws it. The ideal lens shows, at each pixel p of the hole, the world point the
// right eye sees at q = c + (p - c) / M.
using b3test::Cam;
using b3test::makeCam;
constexpr double kEyeR[3] = {0.0315, 0.0, 0.0}, kEyeL[3] = {-0.0315, 0.0, 0.0};
constexpr double kNearPlane = 0.025;   // reverse-Z depth = n / z: the depth of 0.6 m is kNearDepth (0.0417)
constexpr uint32_t kLeftW = 512, kLeftH = 512;
constexpr double kWinM = 4.0;

Rgba worldTex(double a, double b) {   // a, b: the plane's coordinates over its distance (or the sky's tangents); a period of 0.0133
    auto q = [](double v) { return static_cast<uint8_t>(std::min(std::max(std::lround(v), 0L), 255L)); };
    const double u = a / 0.0133, v = b / 0.0133;
    return Rgba{q(128.0 + 90.0 * std::sin(2.0 * sz::kPi * u + 0.3)), q(128.0 + 90.0 * std::cos(2.0 * sz::kPi * v)), q(128.0 + 60.0 * std::sin(2.0 * sz::kPi * (u + v) * 0.5)), 255};
}
struct B3Geom {
    double trueTan[4] = {-0.5, 0.5, -0.5, 0.5};
    double tc[2] = {0.0, 0.0};
    double win[4] = {};
    B3Geom() {
        // c in the right eye (unjittered): the rig's c, as a tangent; the left eye's direction to the far point is the same ray 6.3 cm over
        const double u = cOutX() / kOutW, v = cOutY() / kOutH;
        tc[0] = trueTan[0] + u * (trueTan[1] - trueTan[0]) + 0.063 / 100.0;
        tc[1] = trueTan[3] - v * (trueTan[3] - trueTan[2]);
        sz::b3Window(trueTan, tc, kWinM, win);
    }
};
// The world's colour at the right eye's pixel q (output px), for a plane at Z (Z <= 0: the sky).
Rgba worldAtRight(const B3Geom& g, double Z, double qx, double qy) {
    const double tx = g.trueTan[0] + qx / kOutW * (g.trueTan[1] - g.trueTan[0]), ty = g.trueTan[3] - qy / kOutH * (g.trueTan[3] - g.trueTan[2]);
    return Z > 0.0 ? worldTex(tx + kEyeR[0] / Z, ty) : worldTex(tx, ty);
}
Image makeLeft(const B3Geom& g, double Z) {
    Image im;
    im.w = kLeftW;
    im.h = kLeftH;
    im.px.resize(static_cast<size_t>(kLeftW) * kLeftH);
    for (uint32_t y = 0; y < kLeftH; ++y)
        for (uint32_t x = 0; x < kLeftW; ++x) {
            const double tx = g.win[0] + (x + 0.5) / kLeftW * (g.win[1] - g.win[0]), ty = g.win[3] - (y + 0.5) / kLeftH * (g.win[3] - g.win[2]);
            im.at(static_cast<int>(x), static_cast<int>(y)) = Z > 0.0 ? worldTex(tx + kEyeL[0] / Z, ty) : worldTex(tx, ty);
        }
    return im;
}
Scene makeScenePlane(double Z) {
    Scene s = makeScene(true, -1);
    for (float& d : s.depth)
        if (d == static_cast<float>(kFarD)) d = Z > 0.0 ? static_cast<float>(kNearPlane / Z) : 0.0f;
    return s;
}
struct B3In {
    double Z = 3.0;
    bool asked = true, matOk = true, zoomedFrame = true, makeLeftCopy = true, leftStaleSeq = false, leftBehind = false, shaderless = false;
    double m = 2.5;
};
struct B3Run {
    Image image;
    sz::LensDoorStats stats;
    sz::B3Press b3;
    bool gotCopy = false;
};
B3Run runLensB3(Fx& f, const B3In& in) {
    const B3Geom g;
    const Scene sc = makeScenePlane(in.Z);
    const Image orig = makeOrig(), left = makeLeft(g, in.Z);
    ComPtr<ID3D11Texture2D> depthTex;
    ComPtr<ID3D11ShaderResourceView> srv = f.depthSrv(sc, &depthTex);
    const UINT bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> frame = f.frameTex(DXGI_FORMAT_R8G8B8A8_UNORM, orig, bind);
    ComPtr<ID3D11Texture2D> leftTex;
    {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = kLeftW; d.Height = kLeftH; d.MipLevels = d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr(f.dev->CreateTexture2D(&d, nullptr, &leftTex), "the rig's left picture");
        f.upload(leftTex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, left);
    }
    // the two eyes' matrices, from rows of one frame (the right eye's unjittered rows, the left eye's with the window)
    const Cam camR = makeCam(kEyeR, g.trueTan, kNearPlane), camL = makeCam(in.leftBehind ? std::array<double, 3>{-0.0315, 0.0, 2.0}.data() : kEyeL, g.win, kNearPlane);
    const double zero2[2] = {0.0, 0.0};
    ScopeZoomFrame rec;
    check(sz::b3Matrices(camR.rows, camR.o, zero2, camL.rows, camL.o, zero2, rec.b3.T, rec.b3.u), "B3 rig: the two eyes' matrices");
    scopeZoomDepthClear(1);
    scopeZoomDepthSet(1, srv.Get(), kRenW, kRenH, static_cast<float>(kNearDepth), true);
    static uint32_t press = 5000;
    ++press;
    scopeZoomPressBegin(press);
    rec.verdict = static_cast<uint32_t>(sz::LensVerdict::kQualifies);
    rec.press = press;
    rec.m = rec.mTarget = static_cast<float>(in.m);
    rec.uv[0] = static_cast<float>(kCRen[0] / kRenW);
    rec.uv[1] = static_cast<float>(kCRen[1] / kRenH);
    rec.jitterUv[0] = static_cast<float>(kJitterPx[0] / kRenW);
    rec.jitterUv[1] = static_cast<float>(kJitterPx[1] / kRenH);
    rec.jitterSeq = 100;
    std::memcpy(rec.tan4, kTan4, sizeof(kTan4));
    rec.b3.asked = in.asked ? 1u : 0u;
    rec.b3.matOk = in.matOk ? 1u : 0u;
    rec.b3.tc[0] = static_cast<float>(g.tc[0]);
    rec.b3.tc[1] = static_cast<float>(g.tc[1]);
    g_stubZoom.on = in.zoomedFrame;
    for (int i = 0; i < 4; ++i) {
        g_stubZoom.zoomed[i] = static_cast<float>(g.win[i]);
        g_stubZoom.pre[i] = static_cast<float>(g.trueTan[i]);
    }
    scopeZoomPublish(rec);
    const uint32_t region[4] = {0, 0, kOutW, kOutH}, leftRegion[4] = {0, 0, kLeftW, kLeftH};
    B3Run out;
    ComPtr<ID3D11Texture2D> copy;
    uint32_t dispatchedBefore = 0;
    for (uint32_t i = 0; i < 400u; ++i) {
        f.upload(frame.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, orig);
        if (in.makeLeftCopy) {
            ScopeZoomLeftArgs la;
            la.sequence = 100 + i + (in.leftStaleSeq ? 7u : 0u);
            la.frame = leftTex.Get();
            la.region = leftRegion;
            la.resolved = true;
            scopeZoomLeftDoor(la);
        }
        if (i) {   // published right before the door: c must be under 50 ms old when it is read
            rec.jitterSeq = static_cast<uint64_t>(100 + i);
            scopeZoomPublish(rec);
        }
        ScopeZoomDoorArgs a;
        a.eye = 1;
        a.sequence = 100 + i;
        a.frame = frame.Get();
        a.region = region;
        a.inPlace = true;
        a.resolved = true;
        copy.Reset();
        scopeZoomDoor(a, &copy);
        f.ctx->Flush();
        sz::LensDoorStats peek;
        scopeZoomPressTake(&peek);
        if (peek.staleC) {   // see runDoor: a stale frame writes nothing; the press starts again under a new id
            ++press;
            scopeZoomPressBegin(press);
            rec.press = press;
            dispatchedBefore = 0;
            continue;
        }
        const bool wrote = peek.dispatched > dispatchedBefore;
        dispatchedBefore = peek.dispatched;
        if (peek.raysRead >= 2 && wrote) break;
        Sleep(1);
    }
    out.gotCopy = copy != nullptr;
    out.image = f.read(copy ? copy.Get() : frame.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    scopeZoomPressTake(&out.stats);
    scopeZoomB3Take(&out.b3);
    scopeZoomDepthClear(1);
    g_stubZoom.on = false;
    return out;
}
// What the lens should show where the hole is: the world at q = c + (p - c) / M. naive: the left picture taken at the same direction, with no parallax removed (the mutant).
struct B3Verdict { unsigned inside = 0, bad = 0, worst = 0; };
B3Verdict compareB3(const Image& got, double Z, double m, const Expected& e, int tol, double mOff = 0.0) {
    const B3Geom g;
    B3Verdict v;
    const double cx = cOutX(), cy = cOutY();
    for (size_t i = 0; i < got.px.size(); ++i) {
        if (e.edge[i] < 4.0) continue;
        const int x = static_cast<int>(i % got.w), y = static_cast<int>(i / got.w);
        const double mm = m + mOff;
        const double qx = cx + (x + 0.5 - cx) / mm, qy = cy + (y + 0.5 - cy) / mm;
        const Rgba want = worldAtRight(g, Z, qx, qy), have = got.px[i];
        const unsigned d = static_cast<unsigned>(std::max({std::abs(have.r - want.r), std::abs(have.g - want.g), std::abs(have.b - want.b)}));
        ++v.inside;
        if (static_cast<int>(d) > tol) ++v.bad;
        v.worst = std::max(v.worst, d);
    }
    return v;
}
// The same, for the left picture taken straight (the direction q has in the right eye, read at that direction in the left eye): what a lens without the parallax removed would show.
B3Verdict compareNaive(const Image& got, double Z, double m, const Expected& e, int tol) {
    const B3Geom g;
    B3Verdict v;
    const Image left = makeLeft(g, Z);
    const double cx = cOutX(), cy = cOutY();
    for (size_t i = 0; i < got.px.size(); ++i) {
        if (e.edge[i] < 4.0) continue;
        const int x = static_cast<int>(i % got.w), y = static_cast<int>(i / got.w);
        const double qx = cx + (x + 0.5 - cx) / m, qy = cy + (y + 0.5 - cy) / m;
        const double tx = g.trueTan[0] + qx / kOutW * (g.trueTan[1] - g.trueTan[0]), ty = g.trueTan[3] - qy / kOutH * (g.trueTan[3] - g.trueTan[2]);
        const double lx = (tx - g.win[0]) / (g.win[1] - g.win[0]) * kLeftW, ly = (g.win[3] - ty) / (g.win[3] - g.win[2]) * kLeftH;
        const Rgba want = catmullAt(left, lx, ly), have = got.px[i];
        const unsigned d = static_cast<unsigned>(std::max({std::abs(have.r - want.r), std::abs(have.g - want.g), std::abs(have.b - want.b)}));
        ++v.inside;
        if (static_cast<int>(d) > tol) ++v.bad;
        v.worst = std::max(v.worst, d);
    }
    return v;
}
unsigned diffCount(const Image& a, const Image& b) {
    unsigned n = 0;
    for (size_t i = 0; i < a.px.size() && i < b.px.size(); ++i) n += (a.px[i].r != b.px[i].r || a.px[i].g != b.px[i].g || a.px[i].b != b.px[i].b) ? 1u : 0u;
    return n;
}

void b3Tests(Fx& f) {
    const Image orig = makeOrig();
    const Scene ring = makeScene(true, -1);
    const double cx = cOutX(), cy = cOutY();
    const Expected e = expectedLens(ring, orig, 2.5, cx, cy, 1.0, 0.0);
    const uint32_t zoomedIdx = static_cast<uint32_t>(sz::B3Source::kZoomed);
    // ---- the lens takes its picture from the left eye, at every distance and for the sky ----
    const double planes[5] = {1.5, 3.0, 10.0, 100.0, 0.0};
    for (double Z : planes) {
        B3In in;
        in.Z = Z;
        const B3Run r = runLensB3(f, in);
        const B3Verdict v = compareB3(r.image, Z, in.m, e, 4);
        std::printf("B3 plane %.1f m: %u px in the hole, %u off by more than 4 codes (worst %u); source zoomed %u, parallax mean %.1f px\n", Z, v.inside, v.bad, v.worst, r.b3.source[zoomedIdx],
                    r.b3.parN ? r.b3.parSumPx / r.b3.parN : -1.0);
        check(v.inside > 2000 && v.bad * 100 < v.inside, Z > 0.0 ? "B3: the lens shows the world as the right eye sees it at c + (p - c) / M, taken from the left eye's zoomed picture, at this distance"
                                                                  : "B3: the lens shows the sky by its direction, taken from the left eye's zoomed picture");
        check(r.b3.source[zoomedIdx] >= 1 && r.b3.copies >= 1 && r.stats.dispatched >= 1 && !r.gotCopy, "B3: the frames were counted as taken from the zoomed picture, the left door copied it, and the frame was written in place");
        check(r.b3.source[static_cast<uint32_t>(sz::B3Source::kNotZoomed)] + r.b3.source[static_cast<uint32_t>(sz::B3Source::kNoCopy)] + r.b3.source[static_cast<uint32_t>(sz::B3Source::kStale)] +
                      r.b3.source[static_cast<uint32_t>(sz::B3Source::kNoMatrices)] + r.b3.source[static_cast<uint32_t>(sz::B3Source::kNoShader)] ==
                  0u,
              "B3: and none of them fell back");
        if (Z == 3.0 || Z == 1.5) {
            check(r.b3.parN >= 1 && std::fabs(r.b3.parSumPx / r.b3.parN - 0.063 / Z * (kLeftW / (B3Geom().win[1] - B3Geom().win[0]))) < 0.05 * 0.063 / Z * (kLeftW / (B3Geom().win[1] - B3Geom().win[0])),
                  "B3: the parallax at c the press line reports is 6.3 cm over the distance in the left picture's pixels (within 5 %)");
            const B3Verdict naive = compareNaive(r.image, Z, in.m, e, 8);
            check(naive.bad * 4 > naive.inside, "B3: control: the left picture read at the same direction, with the parallax left in, is not what the lens shows (a quarter of the hole disagrees)");
        }
        if (Z == 3.0) {
            const B3Verdict wrongM = compareB3(r.image, Z, in.m, e, 4, 0.5);
            check(wrongM.bad * 4 > wrongM.inside, "B3: control: a lens magnified 20 % less than the game's zoom is not what the lens shows");
        }
    }
    // ---- each reason the lens falls back to the plain lens, counted, and the plain lens is what it shows ----
    {
        B3In base;
        base.Z = 3.0;
        const Image a = runDoor(f, ring, orig, RunIn{}).image;   // the plain lens, A, for the same frame
        struct Case { const char* name; B3In in; sz::B3Source want; };
        Case cases[6];
        cases[0] = {"not asked", base, sz::B3Source::kNotAsked};
        cases[0].in.asked = false;
        cases[1] = {"the left eye not zoomed in this frame", base, sz::B3Source::kNotZoomed};
        cases[1].in.zoomedFrame = false;
        cases[2] = {"no copy of the left picture", base, sz::B3Source::kNoCopy};
        cases[2].in.makeLeftCopy = false;
        cases[3] = {"a copy of another frame", base, sz::B3Source::kStale};
        cases[3].in.leftStaleSeq = true;
        cases[4] = {"no usable matrices", base, sz::B3Source::kNoMatrices};
        cases[4].in.matOk = false;
        cases[5] = {"the left eye behind the point (each pixel falls back)", base, sz::B3Source::kZoomed};
        cases[5].in.leftBehind = true;
        for (const Case& c : cases) {
            const B3Run r = runLensB3(f, c.in);
            const uint32_t want = static_cast<uint32_t>(c.want);
            uint32_t others = 0;
            for (uint32_t i = 0; i < sz::kB3Sources; ++i)
                if (i != want) others += r.b3.source[i];
            const unsigned differs = diffCount(r.image, a);
            std::printf("B3 fall-back, %s: counted %u under its reason, %u elsewhere, %u px differ from the plain lens\n", c.name, r.b3.source[want], others, differs);
            check(r.b3.source[want] >= 1 && others == 0, "B3: the frames are counted under the reason the lens fell back, and under no other");
            check(differs * 200 < static_cast<unsigned>(a.px.size()) || differs == 0u, "B3: and the picture is the plain lens's (the same frame, magnified from the right eye's own picture)");
            check(r.stats.dispatched >= 1, "B3: the fall-back is a lens, not a declined frame");
        }
    }
}

void infoCheck(Fx& f) {
    if (!f.info) {
        std::puts("scope_zoom_test: no debug layer on this machine (the passes ran without it)");
        return;
    }
    const UINT64 count = f.info->GetNumStoredMessages();
    unsigned bad = 0;
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T size = 0;
        if (FAILED(f.info->GetMessage(i, nullptr, &size)) || !size) continue;
        std::vector<uint8_t> raw(size);
        auto* msg = reinterpret_cast<D3D11_MESSAGE*>(raw.data());
        if (FAILED(f.info->GetMessage(i, msg, &size))) continue;
        if (msg->Severity == D3D11_MESSAGE_SEVERITY_ERROR || msg->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION) {
            ++bad;
            std::fprintf(stderr, "debug layer: %s\n", msg->pDescription);
        }
    }
    check(bad == 0, "the debug layer reports no error or corruption");
}
}  // namespace

#include "crisp_sights_cases.h"   // C1: the crisp reticle's gate and wiring, the draw census' line budgets

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("scope_zoom_test: dry-run (no device, no files)");
        return 0;
    }
    const bool hardware = argc > 1 && std::strcmp(argv[1], "--hardware-self-test") == 0;
    if (argc > 1 && std::strcmp(argv[1], "--self-test") != 0 && !hardware) {
        std::fprintf(stderr, "usage: --self-test | --dry-run | --hardware-self-test (the machine's own adapter; not a gate)\n");
        return 2;
    }
    try {
        wiringTests();
        c1cases::gateTests();
        c1cases::lineTests();
        c1cases::doorTests();
        c1cases::wiringTests();
        scopeZoomSetTemporalZoom(&nativeTemporalZoom);
        Fx f(hardware);
        lensTests(f);
        b3Tests(f);
        infoCheck(f);
        scopeZoomRelease();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "scope_zoom_test: FAIL: %s (after %u checks)\n", e.what(), g_checks);
        return 1;
    }
    std::printf("scope_zoom_test: %u checks passed.\n", g_checks);
    return 0;
}

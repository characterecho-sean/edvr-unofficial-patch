#pragma once
#include "../../src/d3d11/flat_discovery_policy.h"
#include "../../src/d3d11/flat_offscreen_fast.h"
#include <fstream>
#include <iterator>
#include <string>

namespace flat_discovery_policy_tests {
// These checks execute the shared policy and offscreen live gate. Source checks
// below only connect those tested decisions to observer ordering/lifecycle;
// they do not execute the discovery collector or prove resolver/history parity.
template<class Check> void run(Check check) {
    using namespace edvr;
    struct RouteCase {
        bool requested, flat, keyboard, shift, ctrl;
        FlatDrawCaptureRoute expected;
        const char* name;
    };
    const RouteCase routes[] = {
        {false,true,true,false,true,FlatDrawCaptureRoute::None,"unpressed mapped key does not request capture"},
        {false,false,true,true,true,FlatDrawCaptureRoute::None,"unpressed VR key does not request capture"},
        {true,false,true,false,false,FlatDrawCaptureRoute::Vr,"plain VR key keeps VR diagnostics"},
        {true,false,true,true,true,FlatDrawCaptureRoute::Vr,"VR modifiers cannot select flat capture"},
        {true,true,true,false,false,FlatDrawCaptureRoute::General,"plain mapped key selects General"},
        {true,true,true,true,false,FlatDrawCaptureRoute::Full,"Shift selects Full"},
        {true,true,true,false,true,FlatDrawCaptureRoute::Passive,"Ctrl plus mapped keyboard key selects passive rearm"},
        {true,true,true,true,true,FlatDrawCaptureRoute::Full,"Shift takes precedence over Ctrl"},
        {true,true,false,false,true,FlatDrawCaptureRoute::General,"pad/joystick or Ctrl-required key stays General"},
        {true,true,false,true,true,FlatDrawCaptureRoute::Full,"Shift on a non-keyboard binding stays Full"}
    };
    for(const auto& c:routes)
        check(flatDrawCaptureRoute(c.requested,c.flat,c.keyboard,c.shift,c.ctrl)==c.expected,c.name);
    FlatCaptureRequest pendingAudit;
    pendingAudit.request(false);
    check(pendingAudit.pending()==FlatCaptureTier::General && pendingAudit.take()==FlatCaptureTier::General,
          "discovery policy: a busy check observes a pending General audit without consuming it");
    pendingAudit.request(true);
    check(pendingAudit.pending()==FlatCaptureTier::Full && pendingAudit.take()==FlatCaptureTier::Full,
          "discovery policy: a busy check observes a pending Full audit without consuming it");
    unsigned combinations=0;
    for(unsigned observing=0;observing<2;++observing)
    for(unsigned manual=0;manual<2;++manual)
    for(unsigned candidate=0;candidate<2;++candidate)
    for(unsigned drawCapture=0;drawCapture<2;++drawCapture)
    for(unsigned packets=0;packets<2;++packets) {
        const bool demand=flatDiscoveryNeedsOriginalState(observing!=0,manual!=0,candidate!=0);
        check(demand==(observing && (manual || candidate)),"discovery policy: manual and independent candidate demand only while observing");
        FlatOffscreenLive live{};live.workFull=true;live.gpuFrameOpened=true;live.foregroundFrameReady=true;
        live.diagnostics=demand || drawCapture || packets;
        check(flatOffscreenLiveVerdict(live)==((demand || drawCapture || packets)?FlatOffscreenNo::Diagnostics:FlatOffscreenNo::None),
              "discovery policy: passive offscreen path stays eligible; manual/candidate/other capture keeps slow diagnostics");
        live.foreignWork=true;
        check(flatOffscreenLiveVerdict(live)==(live.diagnostics?FlatOffscreenNo::Diagnostics:FlatOffscreenNo::Foreign),
              "discovery policy: permitting passive capture cannot bypass foreign-work guard");
        ++combinations;
    }
    const auto read=[](const char* path) {std::ifstream f(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(f),{});};
    const auto body=[](const std::string& source,const std::string& signature) {
        const auto at=source.find(signature);if(at==std::string::npos)return std::string();
        const auto end=source.find("\n}",at);return end==std::string::npos?std::string():source.substr(at,end+2-at);
    };
    const auto before=[](const std::string& source,const char* a,const char* b) {
        const auto x=source.find(a),y=source.find(b);return x!=std::string::npos&&y!=std::string::npos&&x<y;
    };
    const auto temporal=read("src/d3d11/flat_temporal.cpp"),runtime=read("src/d3d11/flat_runtime.cpp"),
               hooks=read("src/d3d11/vscreen.cpp"),deviceHook=read("src/d3d11/device_hook.cpp"),
               compute=read("src/d3d11/flat_compute_capture.cpp");
    check(!temporal.empty()&&!runtime.empty()&&!hooks.empty()&&!deviceHook.empty()&&!compute.empty(),"discovery wiring: source inputs are readable");
    const auto keyStart=deviceHook.find("if (g_state->censusKey.pressed()) {");
    const auto keyEnd=deviceHook.find("if (g_state->gpuPassKey.pressed())",keyStart);
    const auto key=(keyStart!=std::string::npos && keyEnd!=std::string::npos)?deviceHook.substr(keyStart,keyEnd-keyStart):std::string();
    check(key.find("flatDrawCaptureRoute(")!=std::string::npos &&
          key.find("g_state->censusKey.kind() == HotkeyKind::Key")!=std::string::npos &&
          key.find("g_state->censusKey.mods() & kHotkeyCtrl")!=std::string::npos &&
          key.find("GetAsyncKeyState(VK_SHIFT)")!=std::string::npos &&
          key.find("GetAsyncKeyState(VK_CONTROL)")!=std::string::npos,
          "discovery key wiring: the configured focused key supplies binding kind and physical modifiers to the tested route");
    const auto passive=key.find("if (route == FlatDrawCaptureRoute::Passive)"),
               ordinary=key.find("else if (route == FlatDrawCaptureRoute::General || route == FlatDrawCaptureRoute::Full)"),
               vr=key.find("else if (route == FlatDrawCaptureRoute::Vr)");
    check(passive!=std::string::npos && ordinary!=std::string::npos && vr!=std::string::npos && passive<ordinary && ordinary<vr &&
          key.substr(passive,ordinary-passive).find("flatTemporalArmPassive(g_state->device);")!=std::string::npos &&
          key.substr(passive,ordinary-passive).find("drawCensusFlatRequest(")==std::string::npos &&
          key.substr(ordinary,vr-ordinary).find("flatTemporalArm(route == FlatDrawCaptureRoute::Full);")!=std::string::npos &&
          key.substr(ordinary,vr-ordinary).find("drawCensusFlatRequest();")!=std::string::npos &&
          key.substr(vr).find("drawCensusRequest();")!=std::string::npos &&
          key.substr(vr).find("quadProbeRequest();")!=std::string::npos,
          "discovery key wiring: passive skips the two-frame census while General, Full and VR keep their existing arms");
    const auto rearm=body(temporal,"void flatTemporalArmPassive(");
    const auto reject=rearm.find("if (reason) {"), restart=rearm.find("flatTemporalStart(device);");
    check(!rearm.empty() && reject!=std::string::npos && restart!=std::string::npos && reject<restart &&
          before(rearm,"g_passiveRearmAvailable.load(","device != g.device") &&
          before(rearm,"else if (!owner)","device != g.device") &&
          before(rearm,"owner != thread","device != g.device") &&
          before(rearm,"owner != thread","flatRuntimePassiveRearmBusy()") &&
          before(rearm,"flatRuntimePassiveRearmBusy()","if (reason) {") &&
          before(rearm,"if (reason) {","if (rearmed) report(0, \"rearmed\");") &&
          before(rearm,"if (rearmed) report(0, \"rearmed\");","flatTemporalStart(device);") &&
          rearm.substr(reject,restart-reject).find("return;")!=std::string::npos,
          "discovery passive rearm: owner and busy guards reject before reporting or resetting the existing window");
    const auto guardPrefix=reject!=std::string::npos?rearm.substr(0,reject):std::string();
    for(const char* guard:{"not-flat-profile","device-mismatch","stopped","owner-not-established","foreign-owner","paused",
                            "internal","manual-capture-active","compute-capture-active","runtime-diagnostic-active"})
        check(guardPrefix.find(guard)!=std::string::npos,
              "discovery passive rearm: each unsafe lifecycle has an explicit rejection reason");
    check(rearm.find("flat temporal: passive rearm rejected reason=%s; existing capture unchanged")!=std::string::npos &&
          before(rearm,"flatTemporalStart(device);","flat temporal: passive rearm accepted previous=%s") &&
          rearm.find("g_flatTemporalManual.store(true")==std::string::npos &&
          rearm.find("flatComputeArm(")==std::string::npos &&
          rearm.find("flatRuntimeArmProjectionAudit(")==std::string::npos,
          "discovery passive rearm: distinct rejected/accepted logs and no manual, compute or projection arm");
    const auto runtimeBusy=body(runtime,"bool flatRuntimePassiveRearmBusy(");
    check(runtimeBusy.find("if (!owner()) return true;")!=std::string::npos &&
          runtimeBusy.find("projectionAuditRequested.pending()")!=std::string::npos &&
          runtimeBusy.find("s.projectionFrames != 0")!=std::string::npos &&
          runtimeBusy.find("s.drawCapture.active()")!=std::string::npos &&
          runtimeBusy.find("s.drawPackets.armed()")!=std::string::npos &&
          runtimeBusy.find(".take()") == std::string::npos,
          "discovery passive rearm: runtime busy query covers pending audit and active exporters without consuming requests");
    const auto computeBusy=body(compute,"bool flatComputeBusy(");
    check(computeBusy.find("initializedState.load(")!=std::string::npos &&
          computeBusy.find("active.load(")!=std::string::npos &&
          computeBusy.find("flatComputeReadbackPending()")!=std::string::npos &&
          computeBusy.find("s->attempts.armed")!=std::string::npos &&
          computeBusy.find("s->attempts.frameOpen")!=std::string::npos &&
          computeBusy.find("state()") == std::string::npos &&
          computeBusy.find("manual") == std::string::npos,
          "discovery passive rearm: compute busy query covers live attempts and readback without allocating state or blocking on stale Full history");
    const auto start=body(temporal,"void flatTemporalStart("),owned=body(temporal,"void flatTemporalBeforePresent("),
               sampled=body(temporal,"void flatTemporalAfterPresent("),finished=body(temporal,"void finishDiscovery(");
    check(start.find("g_flatTemporalManual.store(false")!=std::string::npos &&
          start.find("g_flatTemporalOwnerThread.store(0")!=std::string::npos &&
          start.find("g.captureBudget = flatCaptureBudget(FlatCaptureTier::Full);")!=std::string::npos &&
          start.find("g_passiveRearmAvailable.store(true")!=std::string::npos &&
          start.find("g_waitingForPresent.store(true")!=std::string::npos &&
          before(owned,"flatOwnerClaimOnPresent(","first owned Present on thread") &&
          before(owned,"first owned Present on thread","g_flatTemporalCapturing.store(true") &&
          before(sampled,"if (frameDraws) ++g.usefulFrames;","report(frame, \"sample\");") &&
          finished.find("report(frame, \"final\");")!=std::string::npos,
          "discovery passive rearm: restart resets manual demand and 120-second/12000-frame budget, then first owned Present activates distinct sample/final reports");
    const auto stop=body(temporal,"void flatTemporalStop(");
    check(stop.find("g_passiveRearmAvailable.store(false")!=std::string::npos &&
          stop.find("g_waitingForPresent.store(false")!=std::string::npos,
          "discovery passive rearm: stop revokes restart and owner claim");
    const auto draw=body(temporal,"void flatTemporalDraw("),dispatch=body(temporal,"void flatTemporalDispatch(");
    check(before(draw,"if (!flatTemporalCapturing()) return;","flatRuntimeSubstitution(ctx, FlatSubstEvent::kOtherDraw)")&&
          before(draw,"flatTemporalNeedsOriginalState()","flatRuntimeSubstitution(ctx, FlatSubstEvent::kOtherDraw)")&&
          before(draw,"flatRuntimeSubstitution(ctx, FlatSubstEvent::kOtherDraw)","flatComputeProbeDraw(")&&
          before(draw,"flatRuntimeSubstitution(ctx, FlatSubstEvent::kOtherDraw)","captureMenuCopy(ctx)"),
          "discovery wiring: draw preflight follows observer gate and precedes physical candidate/menu readers");
    check(before(dispatch,"if (!flatTemporalCapturing()) return;","flatRuntimeSubstitution(ctx, FlatSubstEvent::kDispatch)")&&
          before(dispatch,"flatTemporalNeedsOriginalState()","flatRuntimeSubstitution(ctx, FlatSubstEvent::kDispatch)")&&
          before(dispatch,"flatRuntimeSubstitution(ctx, FlatSubstEvent::kDispatch)","flatComputeDispatch("),
          "discovery wiring: dispatch preflight follows observer gate and precedes physical candidate reader");
    std::string removed=draw;const auto restore=removed.find("flatRuntimeSubstitution(");
    if(restore!=std::string::npos)removed.erase(restore,removed.find(';',restore)+1-restore);
    check(!before(removed,"flatRuntimeSubstitution(ctx, FlatSubstEvent::kOtherDraw)","flatComputeProbeDraw("),
          "discovery wiring control: losing restore is detected");
    const auto arm=body(temporal,"void flatTemporalArm(");
    check(before(arm,"flatTemporalStart(device);","g_flatTemporalManual.store(true")&&
          arm.find("g.captureBudget = flatCaptureBudget(full ? FlatCaptureTier::Full : FlatCaptureTier::General);")!=std::string::npos&&
          arm.find("g.projectionManual = full;")!=std::string::npos,
          "discovery wiring: General and Full both request original state without changing their capture tiers");
    for(const char* function:{"void flatTemporalStart(","void flatTemporalStop(","void finishDiscovery("})
        check(body(temporal,function).find("g_flatTemporalManual.store(false")!=std::string::npos,
              "discovery wiring: startup, stop and completion clear manual demand");
    const char* hooksToCheck[]={"hookedDrawIndexedInstancedIndirect","hookedDrawInstancedIndirect","hookedDraw","hookedDrawAuto",
                               "hookedDrawIndexed","hookedDrawInstanced","hookedDrawIndexedInstanced"};
    for(const char* hook:hooksToCheck) {
        const auto part=body(hooks,std::string("void STDMETHODCALLTYPE ")+hook+"(");
        check(before(part,"flatTemporalDraw(","FlatRuntimeDrawScope flatDraw("),
              "discovery wiring: every flat draw observes before constructing slow/offscreen scope");
    }
    const auto live=body(runtime,"FlatOffscreenLive flatOffscreenLiveOf("),ctor=body(runtime,"FlatRuntimeDrawScope::FlatRuntimeDrawScope(");
    // Existing O5 tests also pin the source expressions and exercise reducer
    // parity on the committed corpus; this checks their shared new policy.
    check(live.find("flatTemporalNeedsOriginalState() || s.drawCapture.active() || s.drawPackets.armed()")!=std::string::npos&&
          ctor.find("flatTemporalNeedsOriginalState() || s.drawCapture.active() || s.drawPackets.armed()")!=std::string::npos,
          "discovery wiring: slow and offscreen constructors use identical physical-state demand");
    std::printf("  discovery policy: %u live-policy combinations; limited observer-order/lifecycle wiring checks\n",combinations);
}
} // namespace flat_discovery_policy_tests

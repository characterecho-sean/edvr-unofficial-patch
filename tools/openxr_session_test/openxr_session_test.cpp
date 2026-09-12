#include "../../src/openxr/session_state.h"
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include <type_traits>
using namespace edvr::openxr;
namespace {
unsigned checks=0, failures=0;
void check(bool value,const char* message) { ++checks; if(!value) { ++failures; std::printf("FAIL: %s\n",message); } }
const auto instance = reinterpret_cast<XrInstance>(1);
const auto session = reinterpret_cast<XrSession>(2);
enum Call { Poll, Start, Stop, Wait, Begin, End, Count };
struct Fake {
  std::vector<std::string> trace;
  std::deque<XrEventDataBuffer> events;
  XrResult results[Count]{};
  XrSessionState state=XR_SESSION_STATE_UNKNOWN;
  bool running=false, waited=false, opened=false, render=true;
  XrTime nextTime=100, waitedTime=0, endedTime=0;
  XrDuration period=11;
  uint32_t endedLayers=0;
  const XrCompositionLayerBaseHeader* const* layerPointer=nullptr;
  XrEnvironmentBlendMode endedBlend=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
};
Fake* f=nullptr;
bool success(XrResult r) {return r==XR_SUCCESS || r==XR_SESSION_LOSS_PENDING || r==XR_FRAME_DISCARDED;}
XrResult XRAPI_PTR poll(XrInstance i,XrEventDataBuffer* b) {
  f->trace.emplace_back("poll");check(i==instance && b->type==XR_TYPE_EVENT_DATA_BUFFER && !b->next,"poll ABI");
  if(f->results[Poll]!=XR_SUCCESS) return f->results[Poll];
  if(f->events.empty()) return XR_EVENT_UNAVAILABLE;
  *b=f->events.front();f->events.pop_front();
  if(b->type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
    auto& e=*reinterpret_cast<XrEventDataSessionStateChanged*>(b);
    if(e.session==session) f->state=e.state;
  }
  return XR_SUCCESS;
}
XrResult XRAPI_PTR start(XrSession s,const XrSessionBeginInfo* b) {
  f->trace.emplace_back("start");
  check(s==session && b->type==XR_TYPE_SESSION_BEGIN_INFO && !b->next &&
        b->primaryViewConfigurationType==XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,"begin session ABI");
  check(!f->running && f->state==XR_SESSION_STATE_READY,"begin session runtime order");
  if(success(f->results[Start])) {f->running=true;f->waited=f->opened=false;}
  return f->results[Start];
}
XrResult XRAPI_PTR stop(XrSession s) {
  f->trace.emplace_back("stop");check(s==session && f->running && f->state==XR_SESSION_STATE_STOPPING &&
                                    !f->opened && !f->waited,"end session runtime order");
  if(success(f->results[Stop])) f->running=false;
  return f->results[Stop];
}
XrResult XRAPI_PTR waitFrame(XrSession s,const XrFrameWaitInfo* b,XrFrameState* out) {
  f->trace.emplace_back("wait");check(s==session && b->type==XR_TYPE_FRAME_WAIT_INFO && !b->next &&
      out->type==XR_TYPE_FRAME_STATE && !out->next,"wait ABI");
  check(f->running && !f->waited && !f->opened,"serial wait runtime order");
  if(success(f->results[Wait])) {
    f->waited=true;f->waitedTime=f->nextTime;f->nextTime+=f->period;
    out->predictedDisplayTime=f->waitedTime;out->predictedDisplayPeriod=f->period;out->shouldRender=f->render?XR_TRUE:XR_FALSE;
  }
  return f->results[Wait];
}
XrResult XRAPI_PTR beginFrame(XrSession s,const XrFrameBeginInfo* b) {
  f->trace.emplace_back("begin");check(s==session && b->type==XR_TYPE_FRAME_BEGIN_INFO && !b->next,"begin frame ABI");
  check(f->running && f->waited && !f->opened,"begin frame runtime order");
  if(success(f->results[Begin])) {f->waited=false;f->opened=true;}
  return f->results[Begin];
}
XrResult XRAPI_PTR endFrame(XrSession s,const XrFrameEndInfo* b) {
  f->trace.emplace_back("end");check(s==session && b->type==XR_TYPE_FRAME_END_INFO,"end frame ABI");
  check(f->running && f->opened && !f->waited,"end frame runtime order");
  check(b->displayTime==f->waitedTime,"end uses exact waited display time");
  check(!b->layerCount || b->layers,"end layer pointer");
  check(b->environmentBlendMode==XR_ENVIRONMENT_BLEND_MODE_ADDITIVE,"caller-selected blend retained");
  f->endedTime=b->displayTime;f->endedLayers=b->layerCount;f->layerPointer=b->layers;f->endedBlend=b->environmentBlendMode;
  f->opened=false;return f->results[End];
}
Dispatch api() {return {poll,start,stop,waitFrame,beginFrame,endFrame};}
void event(XrSessionState value,XrSession target=session) {
  XrEventDataSessionStateChanged e{XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};e.session=target;e.state=value;
  XrEventDataBuffer b{};std::memcpy(&b,&e,sizeof(e));f->events.push_back(b);
}
struct Fixture {
  Fake fake;
  SessionState core{api(),instance,session,XR_ENVIRONMENT_BLEND_MODE_ADDITIVE};
  Fixture() {f=&fake;}
  void ready() {
    event(XR_SESSION_STATE_READY);check(core.pollEvents()==XR_SUCCESS,"ready event");
    check(core.startIfReady()==XR_SUCCESS,"start ready session");
  }
  void stopping() {event(XR_SESSION_STATE_STOPPING);check(core.pollEvents()==XR_SUCCESS,"stopping event");}
  void destroy() {fake.running=fake.waited=fake.opened=false;fake.events.clear();fake.state=XR_SESSION_STATE_UNKNOWN;core.abandonAfterOwnerDestruction();}
};
XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
const XrCompositionLayerBaseHeader* layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
XrFrameEndInfo supplied() {
  XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};info.displayTime=99999;info.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  info.layerCount=1;info.layers=&layer;return info;
}
void basic() {
  Fixture t;Frame frame;
  check(t.core.waitAndBegin(frame)==XR_ERROR_CALL_ORDER_INVALID && t.fake.trace.empty(),"no wait before start");
  check(t.core.startIfReady()==XR_ERROR_CALL_ORDER_INVALID && t.fake.trace.empty(),"no start before READY");
  t.ready();const auto before=t.fake.trace.size();
  check(t.core.startIfReady()==XR_ERROR_CALL_ORDER_INVALID && t.fake.trace.size()==before,"no repeated start");
  check(t.core.reset(api(),instance,session,XR_ENVIRONMENT_BLEND_MODE_ADDITIVE)==XR_ERROR_CALL_ORDER_INVALID,"reset running rejected");
  event(XR_SESSION_STATE_FOCUSED);t.core.pollEvents();check(t.core.lifecycle()==Lifecycle::Focused,"focused");
  event(XR_SESSION_STATE_VISIBLE);t.core.pollEvents();check(t.core.lifecycle()==Lifecycle::Visible,"focus loss updates visibility");
  event(XR_SESSION_STATE_SYNCHRONIZED);t.core.pollEvents();check(t.core.lifecycle()==Lifecycle::Synchronized,"visibility loss updates state");
  t.fake.trace.clear();check(t.core.waitAndBegin(frame)==XR_SUCCESS,"open frame");
  check(frame.predictedDisplayTime==100 && frame.predictedDisplayPeriod==11 && frame.shouldRender,"runtime frame metadata");
  const Frame original=frame;
  check(t.core.waitAndBegin(frame)==XR_ERROR_CALL_ORDER_INVALID && frame.sequence==original.sequence,"nested call preserves active token");
  check(t.core.reset(api(),instance,session,XR_ENVIRONMENT_BLEND_MODE_ADDITIVE)==XR_ERROR_CALL_ORDER_INVALID,"reset open rejected");
  frame.predictedDisplayTime=-100;frame.shouldRender=false;
  check(t.core.end(frame,supplied())==XR_SUCCESS && t.fake.endedLayers==1 && t.fake.layerPointer==&layer,"stored state overrides public metadata");
  check(frame.status==FrameStatus::Ended && !t.core.frameOpen() && t.core.predictedDisplayTime()==0,"end clears active metadata");
  check(t.core.end(frame,supplied())==XR_ERROR_CALL_ORDER_INVALID,"double end rejected");
  check(t.fake.trace==std::vector<std::string>({"wait","begin","end"}),"exact successful frame trace");
  Frame next;t.core.waitAndBegin(next);auto bad=supplied();bad.layers=nullptr;
  const auto n=t.fake.trace.size();check(t.core.end(next,bad)==XR_ERROR_VALIDATION_FAILURE && t.fake.trace.size()==n,"invalid layer list does not consume frame");
  t.core.end(next,supplied());
}
void noRender() {
  for(unsigned mode=0;mode<3;++mode) {
    Fixture t;t.ready();t.fake.render=mode!=0;Frame frame;t.core.waitAndBegin(frame);
    frame.shouldRender=true;RenderReady ready{mode!=1,mode!=2};
    check(t.core.end(frame,supplied(),ready)==XR_SUCCESS && t.fake.endedLayers==0 && !t.fake.layerPointer,"no-render/invalid pose/unready renderer closes zero layers");
  }
}
void restart() {
  Fixture t;t.ready();Frame old;t.core.waitAndBegin(old);t.stopping();
  const auto n=t.fake.trace.size();check(t.core.pollEvents()==XR_SUCCESS && t.fake.trace.size()==n,"STOPPING not overwritten before stop");
  check(t.core.waitAndBegin(old)==XR_ERROR_CALL_ORDER_INVALID,"no wait in STOPPING");
  t.fake.trace.clear();check(t.core.stop()==XR_SUCCESS,"stop with open frame");
  check(t.fake.trace==std::vector<std::string>({"end","stop"}) && t.fake.endedLayers==0,"zero-layer end before endSession");
  check(!t.core.running() && !t.core.terminal(),"stopped session can later restart");
  check(t.core.startIfReady()==XR_ERROR_CALL_ORDER_INVALID,"restart requires new READY");
  event(XR_SESSION_STATE_IDLE);t.core.pollEvents();check(t.core.lifecycle()==Lifecycle::Idle,"IDLE after stop");
  t.ready();Frame fresh;t.core.waitAndBegin(fresh);const auto at=t.fake.trace.size();
  check(t.core.end(old,supplied())==XR_ERROR_CALL_ORDER_INVALID && t.fake.trace.size()==at,"stale generation rejected after restart");
  check(fresh.generation!=old.generation,"restart changes generation");t.core.end(fresh,supplied());t.stopping();t.core.stop();
  check(t.core.reset(api(),instance,session,XR_ENVIRONMENT_BLEND_MODE_ADDITIVE)==XR_SUCCESS,"stopped reset");
  check(t.core.predictedDisplayTime()==0 && !t.core.running(),"reset metadata empty");
}
void failureCases() {
  for(auto point:{Poll,Start,Wait,Begin,End,Stop}) {
    Fixture t;Frame frame;t.fake.results[point]=XR_ERROR_RUNTIME_FAILURE;
    XrResult r=XR_SUCCESS;
    if(point==Poll) r=t.core.pollEvents();
    else if(point==Start) {event(XR_SESSION_STATE_READY);t.core.pollEvents();r=t.core.startIfReady();}
    else {t.ready();if(point==Stop){t.stopping();r=t.core.stop();}
      else {r=t.core.waitAndBegin(frame);if(point==End)r=t.core.end(frame,supplied());}}
    check(r==XR_ERROR_RUNTIME_FAILURE && t.core.terminal() && t.core.lifecycle()==Lifecycle::Failed,"injected dispatch failure is terminal");
    const auto size=t.fake.trace.size();t.core.waitAndBegin(frame);t.core.startIfReady();t.core.pollEvents();t.core.end(frame,supplied());t.core.stop();
    check(t.fake.trace.size()==size,"no dispatch after uncertain failure");
    t.destroy();check(t.core.reset(api(),instance,session,XR_ENVIRONMENT_BLEND_MODE_ADDITIVE)==XR_SUCCESS,"owner destruction permits rebind");
    t.fake.results[point]=XR_SUCCESS;t.ready();Frame rebound;t.core.waitAndBegin(rebound);t.core.end(rebound,supplied());
  }
  for(auto error:{XR_ERROR_SESSION_LOST,XR_ERROR_INSTANCE_LOST}) {
    Fixture t;t.ready();Frame frame;t.core.waitAndBegin(frame);t.fake.results[Poll]=error;
    check(t.core.pollEvents()==error && t.core.lifecycle()==(error==XR_ERROR_SESSION_LOST?Lifecycle::SessionLost:Lifecycle::InstanceLost),"session/instance lost remain distinct");
    const auto size=t.fake.trace.size();t.core.end(frame,supplied());check(t.fake.trace.size()==size,"no end on lost handle");
    t.destroy();check(!t.core.frameOpen() && !t.core.running() && t.core.predictedDisplayTime()==0,"destroy invalidates outstanding frame");
    check(t.core.reset(api(),instance,session,XR_ENVIRONMENT_BLEND_MODE_ADDITIVE)==XR_SUCCESS,"rebind after lost resource teardown");
  }
  Fixture t;t.ready();Frame frame;t.core.waitAndBegin(frame);t.stopping();t.fake.results[End]=XR_ERROR_RUNTIME_FAILURE;
  t.fake.trace.clear();check(t.core.stop()==XR_ERROR_RUNTIME_FAILURE && t.fake.trace==std::vector<std::string>({"end"}),"failed stop-frame closure blocks endSession");
  t.core.waitAndBegin(frame);check(t.fake.trace.size()==1,"failed closure blocks future wait");
}
void positiveCodes() {
  for(auto point:{Start,Wait,Begin,End,Stop}) {
    Fixture t;t.fake.results[point]=XR_SESSION_LOSS_PENDING;Frame frame;XrResult r;
    if(point==Start) {event(XR_SESSION_STATE_READY);t.core.pollEvents();r=t.core.startIfReady();}
    else {t.ready();if(point==Stop) {t.stopping();r=t.core.stop();}
      else {r=t.core.waitAndBegin(frame);if(point==End)r=t.core.end(frame,supplied());}}
    check(r==XR_SESSION_LOSS_PENDING && t.core.lifecycle()==Lifecycle::LossPending,"positive loss stays explicit");
    if(t.core.frameOpen()) {check(t.core.end(frame,supplied())==XR_SESSION_LOSS_PENDING && t.fake.endedLayers==0,"pending-loss frame closes zero without erasing loss");}
    const auto size=t.fake.trace.size();t.core.waitAndBegin(frame);check(size==t.fake.trace.size(),"no next wait after pending loss");
  }
  for(bool loss:{false,true}) {
    Fixture t;t.ready();t.fake.results[Begin]=XR_FRAME_DISCARDED;t.fake.results[Wait]=loss?XR_SESSION_LOSS_PENDING:XR_SUCCESS;
    Frame frame;t.fake.trace.clear();const auto r=t.core.waitAndBegin(frame);
    check(r==(loss?XR_SESSION_LOSS_PENDING:XR_FRAME_DISCARDED) && frame.discarded && t.core.frameOpen(),"discarded begin succeeds and retains earlier loss");
    check(t.fake.trace==std::vector<std::string>({"wait","begin"}),"successful loss wait still begins");
    t.core.end(frame,supplied());check(t.fake.endedLayers==(loss?0u:1u),"discarded frame has an end obligation");
  }
}
void eventsAndOwners() {
  Fixture t;
  unsigned delivered=0;
  t.core.setUnhandledEventSink([](const XrEventDataBuffer& b,void* context) noexcept {
    check(b.type==XR_TYPE_EVENT_DATA_EVENTS_LOST || b.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED,
          "unhandled event retains its type");
    ++*static_cast<unsigned*>(context);
  }, &delivered);
  for(unsigned i=0;i<100;++i) {XrEventDataBuffer b{};b.type=XR_TYPE_EVENT_DATA_EVENTS_LOST;t.fake.events.push_back(b);}
  t.core.pollEvents(0);check(t.fake.trace.empty(),"zero event budget");t.core.pollEvents(1000);
  check(t.fake.trace.size()==SessionState::kMaxEvents,"hard event bound");t.fake.events.clear();
  check(delivered==SessionState::kMaxEvents,"other events forwarded within budget");
  event(XR_SESSION_STATE_READY,reinterpret_cast<XrSession>(99));t.core.pollEvents();
  check(t.core.lifecycle()==Lifecycle::Uninitialized && delivered==SessionState::kMaxEvents+1,
        "other session event routed without altering this session");t.ready();
  Frame old;t.core.waitAndBegin(old);t.core.end(old,supplied());
  Fixture other;other.ready();Frame frame;other.core.waitAndBegin(frame);const auto size=other.fake.trace.size();
  check(other.core.end(old,supplied())==XR_ERROR_CALL_ORDER_INVALID && other.fake.trace.size()==size,"different policy owner token rejected");other.core.end(frame,supplied());
  for(auto state:{XR_SESSION_STATE_EXITING,XR_SESSION_STATE_LOSS_PENDING}) {
    Fixture terminal;event(state);terminal.core.pollEvents();
    check(terminal.core.terminal() && terminal.core.lifecycle()==(state==XR_SESSION_STATE_EXITING?Lifecycle::Exiting:Lifecycle::LossPending),"terminal event has explicit outcome");
    const auto n=terminal.fake.trace.size();terminal.core.startIfReady();terminal.core.waitAndBegin(frame);check(n==terminal.fake.trace.size(),"terminal event blocks new work");
  }
  Fixture loss;loss.ready();loss.core.waitAndBegin(frame);XrEventDataBuffer b{};b.type=XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING;loss.fake.events.push_back(b);
  loss.core.pollEvents();check(loss.core.lifecycle()==Lifecycle::InstanceLossPending,"instance loss pending distinct");
  loss.core.end(frame,supplied());check(loss.fake.endedLayers==0 && loss.core.lifecycle()==Lifecycle::InstanceLossPending,"instance loss survives empty closure");
  SessionState empty;check(empty.reset(api(),XR_NULL_HANDLE,session,XR_ENVIRONMENT_BLEND_MODE_ADDITIVE)==XR_ERROR_HANDLE_INVALID,"null handle rejected");
  auto missing=api();missing.endFrame=nullptr;check(empty.reset(missing,instance,session,XR_ENVIRONMENT_BLEND_MODE_ADDITIVE)==XR_ERROR_FUNCTION_UNSUPPORTED,"incomplete dispatch rejected");
}
}
static_assert(!std::is_copy_constructible_v<SessionState> && !std::is_move_constructible_v<SessionState>,"one frame owner");
int main(int argc,char** argv) {
  if(argc==2 && !std::strcmp(argv[1],"--dry-run")) {std::puts("Would drive OpenXR lifecycle with injected dispatch; no runtime or files.");return 0;}
  if(argc!=2 || std::strcmp(argv[1],"--self-test")) {std::puts("usage: openxr_session_test --self-test | --dry-run");return 2;}
  basic();noRender();restart();failureCases();positiveCodes();eventsAndOwners();
  std::printf("openxr_session_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}

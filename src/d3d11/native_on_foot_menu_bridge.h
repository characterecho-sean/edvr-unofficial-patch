#pragma once
#include "native_on_foot_f114b.h"
#include <atomic>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <cstring>

namespace edvr::nofoot::menu_input {
struct BridgeMode {
    uint64_t object=0,vtable=0,observedFunctionAddress=0,valueAddress=0;
    uint32_t status=0;int32_t value=-1;uint8_t code[21]{};
};
template<class Read> BridgeMode bridgeMode(uintptr_t ctl,Read read) {
    BridgeMode m;uint64_t objectAfter=0,vtAfter=0,fnAfter=0,valueAfter=0;
    if(!read(ctl+f114b::kOffMode,&m.object,8)||!m.object||!read(m.object,&m.vtable,8)||!m.vtable||
       !read(m.vtable+f114b::kModeGetter,&m.observedFunctionAddress,8)||!m.observedFunctionAddress||
       !read(m.observedFunctionAddress,m.code,sizeof(m.code)))return m;
    // Build334925's observed accessor: optional pointer at +218 -> dword, fallback2. No game function is called.
    static constexpr uint8_t expected[21]={0x48,0x8b,0x81,0x18,0x02,0,0,0x48,0x85,0xc0,0x74,3,0x8b,0,0xc3,0xb8,2,0,0,0,0xc3};
    m.status=1;if(std::memcmp(m.code,expected,sizeof(expected)))return m;
    m.status=2;if(!read(m.object+0x218,&m.valueAddress,8))return m;
    if(m.valueAddress){if(!read(m.valueAddress,&m.value,4))return m;}else m.value=2;
    if(!read(ctl+f114b::kOffMode,&objectAfter,8)||!read(m.object,&vtAfter,8)||!read(m.vtable+f114b::kModeGetter,&fnAfter,8)||
       !read(m.object+0x218,&valueAfter,8)||objectAfter!=m.object||vtAfter!=m.vtable||fnAfter!=m.observedFunctionAddress||valueAfter!=m.valueAddress)return m;
    m.status=3;return m;
}
struct BridgeUpdate {
    uint32_t generation=0,sequence=0,ledgerFrame=0,renderFrame=0,endRenderFrame=0,thread=0;
    uint64_t ctl=0,beforeQpc=0,afterQpc=0;BridgeMode before,after;bool completed=false,returned=false;
};
struct BridgePlacement {
    uint32_t generation=0,ledgerFrame=0,renderFrame=0,thread=0,triangles=0,readFlags=0,meshStatus=0;
    uint64_t ctl=0,panel=0,list=0,owner=0,ownerVtable=0,observedOwnerFunctionAddress=0,triangleAddress=0,beforeQpc=0,afterQpc=0;
    BridgeMode mode;
};
// Hooks use try-lock; draw reads copy numeric snapshots only. No previously observed game pointer is dereferenced at draw time.
class BridgeCapture {
    struct Slot {uint64_t ctl=0;bool hasUpdate=false,hasPlacement=false,hasCandidate=false;BridgeUpdate update;BridgePlacement placement,candidate;};
    std::mutex mutex;std::atomic<bool> enabled{false};std::atomic<uint32_t> first{0},last{0};uint32_t epoch=0,selected=0,sequence=0,pending=0;
    Slot slots[4]{};std::vector<BridgeUpdate> updates;
    uint32_t slotOverflow=0,updateOverflow=0;std::atomic<uint32_t> contention{0};
    uint32_t gateFrame=0;uint64_t gateQpc=0;
    Slot* slot(uint64_t ctl){for(auto& s:slots)if(s.ctl==ctl)return &s;for(auto& s:slots)if(!s.ctl){s.ctl=ctl;return &s;}++slotOverflow;return nullptr;}
    static void modeJson(std::ostream& s,const BridgeMode& m){s<<"{\"object\":"<<m.object<<",\"vtable\":"<<m.vtable<<",\"observed_function_address\":"<<m.observedFunctionAddress<<",\"value_address\":"<<m.valueAddress<<",\"status\":"<<m.status<<",\"value\":"<<m.value<<",\"code\":[";for(unsigned i=0;i<21;++i){if(i)s<<',';s<<unsigned(m.code[i]);}s<<"]}";}
    static void updateJson(std::ostream& s,const BridgeUpdate& r){s<<"{\"sequence\":"<<r.sequence<<",\"ledger_frame\":"<<r.ledgerFrame<<",\"render_frame\":"<<r.renderFrame<<",\"end_render_frame\":"<<r.endRenderFrame<<",\"thread\":"<<r.thread<<",\"ctl\":"<<r.ctl<<",\"before_qpc\":"<<r.beforeQpc<<",\"after_qpc\":"<<r.afterQpc<<",\"completed\":"<<(r.completed?"true":"false")<<",\"original_returned\":"<<(r.returned?"true":"false")<<",\"before_mode\":";modeJson(s,r.before);s<<",\"after_mode\":";modeJson(s,r.after);s<<'}';}
    static void placementJson(std::ostream& s,const BridgePlacement& r){s<<"{\"ledger_frame\":"<<r.ledgerFrame<<",\"render_frame\":"<<r.renderFrame<<",\"thread\":"<<r.thread<<",\"ctl\":"<<r.ctl<<",\"panel\":"<<r.panel<<",\"list\":"<<r.list<<",\"owner\":"<<r.owner<<",\"owner_vtable\":"<<r.ownerVtable<<",\"observed_owner_function_address\":"<<r.observedOwnerFunctionAddress<<",\"triangle_address\":"<<r.triangleAddress<<",\"triangles\":"<<r.triangles<<",\"read_flags\":"<<r.readFlags<<",\"mesh_status\":"<<r.meshStatus<<",\"before_qpc\":"<<r.beforeQpc<<",\"after_qpc\":"<<r.afterQpc<<",\"mode\":";modeJson(s,r.mode);s<<'}';}
public:
    static constexpr unsigned kUpdates=64;
    void arm(uint32_t begin,uint32_t end,uint32_t sharedGeneration=0){enabled.store(false);std::lock_guard<std::mutex> l(mutex);epoch=sharedGeneration?sharedGeneration:epoch+1;first=begin;last=end;selected=begin+1;sequence=pending=slotOverflow=updateOverflow=gateFrame=0;gateQpc=0;contention.store(0);updates.clear();for(auto& s:slots)s=Slot{};enabled.store(true);}
    void discard(){enabled.store(false);std::lock_guard<std::mutex> l(mutex);++epoch;updates.clear();for(auto& s:slots)s=Slot{};}
    bool active(uint32_t frame) const noexcept{return enabled.load(std::memory_order_acquire)&&frame>=first&&frame<=last;}
    void gateOpened(uint32_t frame,uint64_t qpc){std::unique_lock<std::mutex> l(mutex,std::try_to_lock);if(!l){contention.fetch_add(1);return;}if(enabled.load()&&!gateQpc){gateFrame=frame;gateQpc=qpc;}}
    template<class Read> bool begin(BridgeUpdate& r,uintptr_t ctl,uint32_t frame,uint32_t render,uint32_t thread,uint64_t qpc,Read read){
        std::unique_lock<std::mutex> l(mutex,std::try_to_lock);if(!l){contention.fetch_add(1);return false;}if(!active(frame)||!ctl)return false;
        if(updates.size()+pending>=kUpdates){++updateOverflow;return false;}Slot* s=slot(ctl);if(!s)return false;
        r.generation=epoch;r.sequence=++sequence;r.ctl=ctl;r.ledgerFrame=frame;r.renderFrame=render;r.thread=thread;r.beforeQpc=qpc;r.before=bridgeMode(ctl,read);
        s->update=r;s->hasUpdate=true;++pending;return true;
    }
    template<class Read> void finish(BridgeUpdate& r,uint32_t render,uint64_t qpc,Read read){
        std::unique_lock<std::mutex> l(mutex,std::try_to_lock);if(!l){contention.fetch_add(1);return;}if(r.generation!=epoch)return;if(pending)--pending;if(!enabled.load())return;
        r.endRenderFrame=render;r.afterQpc=qpc;r.after=bridgeMode(r.ctl,read);r.completed=true;updates.push_back(r);
        for(auto& s:slots)if(s.ctl==r.ctl&&s.update.sequence==r.sequence){s.update=r;break;}
    }
    template<class Read> void placement(const BridgePlacement& input,Read read){
        std::unique_lock<std::mutex> l(mutex,std::try_to_lock);if(!l){contention.fetch_add(1);return;}if(input.generation!=epoch||!active(input.ledgerFrame))return;auto* s=slot(input.ctl);if(!s)return;
        BridgePlacement r=input;r.mode=bridgeMode(r.ctl,read);s->placement=r;s->hasPlacement=true;if(r.triangles==120){s->candidate=r;s->hasCandidate=true;}
    }
    std::string draw(uint32_t frame,uint32_t render,uint64_t drawMarkerQpc,const BridgeUpdate* enclosing,uint64_t(*clock)()=nullptr){
        std::unique_lock<std::mutex> l(mutex,std::try_to_lock);if(!l){contention.fetch_add(1);return "{\"status\":\"contended\"}";}
        if(!active(frame)||frame!=selected)return "{\"status\":\"outside_selected_frame\"}";
        const uint64_t qpc=clock?clock():drawMarkerQpc;
        std::ostringstream s;s<<"{\"status\":\"sampled\",\"ledger_frame\":"<<frame<<",\"render_frame\":"<<render<<",\"qpc\":"<<qpc<<",\"draw_marker_qpc\":"<<drawMarkerQpc<<",\"snapshot_qpc\":"<<qpc<<",\"gpu_owner_proven\":false,\"pointer_rereads\":0,\"enclosing_update\":";
        if(enclosing&&enclosing->generation==epoch)updateJson(s,*enclosing);else s<<"null";
        s<<",\"controllers\":[";bool comma=false;for(const auto& a:slots)if(a.ctl){if(comma)s<<',';comma=true;s<<"{\"ctl\":"<<a.ctl<<",\"latest_update\":";if(a.hasUpdate)updateJson(s,a.update);else s<<"null";
            s<<",\"prior_placement_only\":";if(a.hasPlacement)placementJson(s,a.placement);else s<<"null";s<<",\"prior_120_triangle_candidate_only\":";if(a.hasCandidate)placementJson(s,a.candidate);else s<<"null";s<<'}';}s<<"]}";return s.str();
    }
    std::string close(){enabled.store(false);std::lock_guard<std::mutex> l(mutex);std::ostringstream s;s<<"{\"schema\":\"edvr_menu_input_bridge_v1\",\"selected_frame\":"<<selected<<",\"first_frame\":"<<first<<",\"last_frame\":"<<last<<",\"update_gate_frame\":"<<gateFrame<<",\"update_gate_qpc\":"<<gateQpc<<",\"contended\":"<<contention.load()<<",\"slot_overflow\":"<<slotOverflow<<",\"update_overflow\":"<<updateOverflow<<",\"in_flight_upper_bound\":"<<pending<<",\"getter_calls\":0,\"legacy_update_instrument_enabled_by_capture\":false,\"status\":\""<<(updates.empty()?(pending?"in_flight_only":gateQpc?"hook_open_no_update_callbacks":"hook_not_opened"):"observed")<<"\",\"updates\":[";
        for(size_t i=0;i<updates.size();++i){if(i)s<<',';updateJson(s,updates[i]);}s<<"],\"association\":\"timing_and_callback_identity_only_not_GPU_owner_proof\"}";updates.clear();for(auto& a:slots)a=Slot{};return s.str();}
};
template<class Original,class Before,class After> auto observedUpdate(Original original,Before before,After after){before();const auto result=original();after();return result;}
template<class Original,class After,class Legacy> auto dispatchUpdate(bool instruments,Original original,After after,Legacy legacy){if(instruments)return legacy();return observedUpdate(original,[](){},after);}
inline std::string attachBridge(std::string input,const std::string& bridge){const auto at=input.find("\"update_capture_gate\":false");if(at!=std::string::npos)input.replace(at,27,"\"update_capture_gate\":true");if(!input.empty())input.pop_back();return input+",\"update_bridge\":"+bridge+"}";}
} // namespace edvr::nofoot::menu_input

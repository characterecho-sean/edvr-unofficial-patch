#pragma once
#include "native_on_foot_f114b.h"
#include "native_on_foot_f117.h"
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <cstring>

namespace edvr::nofoot::menu_input {
// These gates expose calls the game already makes. The update hook can call a camera getter for its old instrument, so capture never opens it.
inline bool hookGate(unsigned hook,bool instruments,bool placement,bool observer) noexcept {
    return hook==0?instruments:hook==1?(placement||observer):(instruments||observer);
}
template<class Original,class After> auto afterOriginal(Original original,After after) {
    const auto result=original();after(result);return result;
}
struct Row {
    uint32_t generation=0,ledgerFrame=0,renderFrame=0,thread=0,reads=0,rayCalls=0,windowCalls=0,ownerCalls=0,acceptedRay=0;
    uint32_t mesh=UINT32_MAX,meshStatus=0,triCount=0,source=0,hit=0,buttonValid=0,observedOwnerMixed=0;
    uint64_t beforeQpc=0,afterQpc=0,ctl=0,panel=0,list=0,owner=0,ownerVtable=0,ownerFn=0,triangleAddress=0,mode=0,modeVt=0,modeFn=0,sink=0,observedOwner=0;
    int32_t modeKind=0,modeArg=0,modeValue=-1;
    uint8_t modeCode[8]{},buttons[6]{};
    float mouse[2]{},other[2]{},dir[3]{},uv[2]{},rayOrigin[3]{},rayDir[3]{},rayT=0,sprite[3]{},ownerWorld[16]{},ownerOut[16]{};
};
struct Mesh {uint64_t panel=0,list=0,owner=0,triangleAddress=0,qpc=0;uint32_t ledgerFrame=0,renderFrame=0;std::vector<f117::Tri> triangles;};
template<class Read> void observeRay(Row& r,uintptr_t origin,uintptr_t dir,uintptr_t u,uintptr_t v,uintptr_t t,uint64_t result,Read read) {
    ++r.rayCalls;r.reads&=~(128u|256u|512u);float rayOrigin[3]{},rayDir[3]{},uv[2]{},distance=0;
    if(read(origin,rayOrigin,sizeof(rayOrigin))&&read(dir,rayDir,sizeof(rayDir))) {
        std::memcpy(r.rayOrigin,rayOrigin,sizeof(rayOrigin));std::memcpy(r.rayDir,rayDir,sizeof(rayDir));r.reads|=128;
    }
    if(result&0xff){++r.acceptedRay;if(read(u,&uv[0],4)&&read(v,&uv[1],4)&&read(t,&distance,4)) {
        std::memcpy(r.uv,uv,sizeof(uv));r.rayT=distance;r.reads|=256;
    }}
}
template<class Read> void observeWindow(Row& r,uintptr_t out,Read read) {
    ++r.windowCalls;r.reads&=~(256u|512u);float uv[2]{};
    if(read(out,uv,sizeof(uv))){std::memcpy(r.uv,uv,sizeof(uv));r.reads|=512;}
}
template<class Read> void observeOwner(Row& r,uintptr_t owner,uintptr_t out,Read read) {
    ++r.ownerCalls;if(r.observedOwner&&r.observedOwner!=owner)r.observedOwnerMixed=1;r.observedOwner=owner;
    r.reads&=~1024u;float world[16]{},result[16]{};
    if(read(owner+f114b::kOffOwnerRows,world,sizeof(world))&&read(out+f114b::kOutRows,result,sizeof(result))) {
        std::memcpy(r.ownerWorld,world,sizeof(world));std::memcpy(r.ownerOut,result,sizeof(result));r.reads|=1024;
    }
}
class Capture {
public:
    static constexpr uint32_t kRowsPerKind=80,kMeshesPerKind=4,kMaxTris=256;
    std::atomic<bool> armed{false};std::atomic<uint32_t> frame{0},first{0},last{0},generation{0},contended{0},begun{0},pending{0};
    std::mutex mutex;std::string stamp;std::vector<Row> rows;std::vector<Mesh> meshes;
    uint32_t rowCount[2]{},meshCount[2]{},rowQuota=0,meshQuota=0,readFaults=0;
    bool active() const noexcept {const uint32_t f=frame.load(std::memory_order_relaxed);return armed.load(std::memory_order_acquire)&&f>=first.load()&&f<=last.load();}
    void discard(){armed.store(false,std::memory_order_release);std::lock_guard<std::mutex> lock(mutex);generation.fetch_add(1);rows.clear();meshes.clear();}
    void arm(const wchar_t* id,uint32_t begin,uint32_t end) {
        armed.store(false,std::memory_order_release);std::lock_guard<std::mutex> lock(mutex);
        rows.clear();meshes.clear();stamp.clear();for(const wchar_t* p=id;p&&*p;++p)if(*p>=L'0'&&*p<=L'9')stamp+=char(*p);
        rowCount[0]=rowCount[1]=meshCount[0]=meshCount[1]=rowQuota=meshQuota=readFaults=0;contended.store(0);begun.store(0);pending.store(0);
        first.store(begin);last.store(end);frame.store(begin);generation.fetch_add(1);armed.store(true,std::memory_order_release);
    }
    template<class Read> bool before(Row& r,uintptr_t ctl,uintptr_t panel,uint32_t render,uint32_t thread,uint64_t qpc,Read read) {
        std::unique_lock<std::mutex> lock(mutex,std::try_to_lock);if(!lock.owns_lock()){contended.fetch_add(1);return false;}
        if(!active())return false;
        r.generation=generation.load();r.ledgerFrame=frame.load();r.renderFrame=render;r.thread=thread;r.beforeQpc=qpc;r.ctl=ctl;r.panel=panel;
        begun.fetch_add(1);pending.fetch_add(1);
        if(read(ctl+f111::kOffMouse,r.mouse,sizeof(r.mouse)))r.reads|=1;
        if(read(ctl+f111::kOffOther,r.other,sizeof(r.other)))r.reads|=2;
        if(read(ctl+f111::kOffDir,r.dir,sizeof(r.dir)))r.reads|=4;
        uint8_t source=0;if(read(ctl+f111::kOffSource,&source,1)){r.source=source;r.reads|=8;}
        if(read(ctl+f114b::kOffMode,&r.mode,8)&&r.mode&&read(r.mode,&r.modeVt,8)&&r.modeVt&&
           read(r.modeVt+f114b::kModeGetter,&r.modeFn,8)&&r.modeFn&&read(r.modeFn,r.modeCode,sizeof(r.modeCode))) {
            const auto kind=f114b::decodeModeGetter(r.modeCode,sizeof(r.modeCode),&r.modeArg);r.modeKind=int(kind);
            if(kind==f114b::Getter::kConst){r.modeValue=r.modeArg;r.reads|=16;}
            else if(kind==f114b::Getter::kByteField){uint8_t value=0;if(read(r.mode+intptr_t(r.modeArg),&value,1)){r.modeValue=value;r.reads|=16;}}
            else if(kind==f114b::Getter::kDwordField&&read(r.mode+intptr_t(r.modeArg),&r.modeValue,4))r.reads|=16;
        }
        read(ctl+f114b::kOffSink,&r.sink,8);
        for(unsigned i=0;i<3;++i){uint64_t button=0;if(read(ctl+f114b::kOffButtons+8*i,&button,8)&&button&&
            read(button+f114b::kOffButtonNow,&r.buttons[2*i],1)&&read(button+f114b::kOffButtonBefore,&r.buttons[2*i+1],1))r.buttonValid|=1u<<i;}
        return true;
    }
    template<class Read> void finish(Row& r,uint64_t qpc,Read read) {
        std::unique_lock<std::mutex> lock(mutex,std::try_to_lock);if(!lock.owns_lock()){contended.fetch_add(1);return;}
        if(r.generation!=generation.load())return;
        pending.fetch_sub(1);
        if(!armed.load())return;
        r.afterQpc=qpc;uint8_t hit=0;if(read(r.ctl+f111::kOffHit,&hit,1)){r.hit=hit;r.reads|=32;}
        if(read(r.ctl+f111::kOffPos,r.sprite,sizeof(r.sprite)))r.reads|=64;
        uint64_t tris=0,vt=0,count=0;
        if(read(r.panel+f117::kOffList,&r.list,8)&&r.list&&read(r.list+f117::kListCount,&count,8)&&count<=UINT32_MAX&&
           read(r.list+f117::kListTris,&tris,8))r.triCount=static_cast<uint32_t>(count);
        else r.meshStatus=3;
        if(r.list&&read(r.list,&r.owner,8)&&r.owner&&read(r.owner,&vt,8)&&vt)read(vt+0x10,&r.ownerFn,8);
        r.ownerVtable=vt;r.triangleAddress=tris;
        if(r.meshStatus==3)++readFaults;
        const unsigned kind=r.triCount==120?0u:1u;
        if(rowCount[kind]>=kRowsPerKind){++rowQuota;return;}++rowCount[kind];
        // Only the exact menu-selected complete ledger frame gets triangle payloads; later records explicitly refer to no fresh mesh.
        if(!r.meshStatus && r.ledgerFrame!=first.load()+1)r.meshStatus=4;
        if(!r.meshStatus && (!r.triCount||!tris))r.meshStatus=1;
        if(!r.meshStatus && r.triCount>kMaxTris)r.meshStatus=2;
        if(!r.meshStatus) {
            // The game rebuilds the list during placement. Equal owner/count alone
            // cannot identify its current triangles, even within one ledger frame.
            std::vector<f117::Tri> current(r.triCount),verify(r.triCount);
            uint64_t verifyTris=0,verifyCount=0;
            const size_t bytes=current.size()*sizeof(f117::Tri);
            if(!read(tris,current.data(),bytes)||!read(tris,verify.data(),bytes)||
               std::memcmp(current.data(),verify.data(),bytes)!=0||
               !read(r.list+f117::kListTris,&verifyTris,8)||!read(r.list+f117::kListCount,&verifyCount,8)||
               verifyTris!=tris||verifyCount!=count){r.meshStatus=3;++readFaults;}
            if(!r.meshStatus)for(uint32_t i=0;i<meshes.size();++i) {
                const auto& old=meshes[i];
                if(old.panel==r.panel&&old.list==r.list&&old.owner==r.owner&&old.triangleAddress==tris&&
                   old.ledgerFrame==r.ledgerFrame&&old.triangles.size()==r.triCount&&
                   std::memcmp(old.triangles.data(),current.data(),bytes)==0){r.mesh=i;r.meshStatus=6;break;}
            }
            if(!r.meshStatus && meshCount[kind]>=kMeshesPerKind){r.meshStatus=5;++meshQuota;}
            if(!r.meshStatus){
                Mesh m;m.panel=r.panel;m.list=r.list;m.owner=r.owner;m.triangleAddress=tris;m.qpc=qpc;
                m.ledgerFrame=r.ledgerFrame;m.renderFrame=r.renderFrame;m.triangles=std::move(current);
                r.mesh=static_cast<uint32_t>(meshes.size());r.meshStatus=7;meshes.push_back(std::move(m));++meshCount[kind];
            }
        }
        rows.push_back(r);
    }
    std::string close(uint32_t hooks,uint64_t frequency,uint64_t gameBase=0) {
        armed.store(false,std::memory_order_release);std::lock_guard<std::mutex> lock(mutex);std::ostringstream s;s<<std::setprecision(9);
        s<<"{\"schema\":\"edvr_menu_input_v1\",\"capture_id\":\""<<stamp<<"\",\"first_ledger_frame\":"<<first.load()<<",\"last_ledger_frame\":"<<last.load()
         <<",\"hooks_armed\":"<<hooks<<",\"qpc_frequency\":"<<frequency<<",\"row_quota\":"<<rowQuota<<",\"mesh_quota\":"<<meshQuota<<",\"read_faults\":"<<readFaults
         <<",\"contended\":"<<contended.load()<<",\"placements_begun\":"<<begun.load()<<",\"in_flight_at_close\":"<<pending.load()<<",\"in_flight_is_upper_bound\":true,\"game_image_base\":"<<gameBase<<",\"getter_calls\":0,\"update_capture_gate\":false,\"phase\":\"mode_and_mouse_before_original; branch_results_and_cached_mesh_after_original_before_cursor_hide\",\"association\":\"time_and_geometry_only_not_proven_GPU_owner\",\"rows\":[";
        const auto floats=[&](const float* p,size_t n){s<<'[';for(size_t i=0;i<n;++i){if(i)s<<',';if(std::isfinite(p[i]))s<<p[i];else s<<"null";}s<<']';};
        for(size_t i=0;i<rows.size();++i){if(i)s<<',';const auto&r=rows[i];s<<"{\"ledger_frame\":"<<r.ledgerFrame<<",\"render_frame\":"<<r.renderFrame<<",\"thread\":"<<r.thread<<",\"before_qpc\":"<<r.beforeQpc<<",\"after_qpc\":"<<r.afterQpc
            <<",\"ctl\":"<<r.ctl<<",\"panel\":"<<r.panel<<",\"list\":"<<r.list<<",\"owner\":"<<r.owner<<",\"owner_fn\":"<<r.ownerFn<<",\"mode\":"<<r.mode<<",\"mode_vtable\":"<<r.modeVt<<",\"mode_fn\":"<<r.modeFn
            <<",\"mode_kind\":"<<r.modeKind<<",\"mode_arg\":"<<r.modeArg<<",\"mode_value\":"<<r.modeValue<<",\"read_flags\":"<<r.reads<<",\"source\":"<<r.source<<",\"hit\":"<<r.hit<<",\"sink\":"<<r.sink<<",\"button_valid\":"<<r.buttonValid
            <<",\"ray_calls\":"<<r.rayCalls<<",\"window_calls\":"<<r.windowCalls<<",\"owner_calls\":"<<r.ownerCalls<<",\"observed_owner\":"<<r.observedOwner<<",\"observed_owner_mixed\":"<<r.observedOwnerMixed<<",\"accepted_ray\":"<<r.acceptedRay<<",\"mesh\":"<<r.mesh<<",\"mesh_status\":"<<r.meshStatus<<",\"triangle_address\":"<<r.triangleAddress<<",\"triangle_count\":"<<r.triCount<<",\"mouse\":";
            floats(r.mouse,2);s<<",\"other\":";floats(r.other,2);s<<",\"direction\":";floats(r.dir,3);s<<",\"uv\":";floats(r.uv,2);s<<",\"ray_origin\":";floats(r.rayOrigin,3);s<<",\"ray_direction\":";floats(r.rayDir,3);s<<",\"ray_t\":";floats(&r.rayT,1);s<<",\"sprite\":";floats(r.sprite,3);s<<",\"owner_world\":";floats(r.ownerWorld,16);s<<",\"owner_output\":";floats(r.ownerOut,16);s<<",\"buttons\":[";
            for(unsigned b=0;b<6;++b){if(b)s<<',';s<<unsigned(r.buttons[b]);}s<<"],\"mode_code\":[";for(unsigned b=0;b<8;++b){if(b)s<<',';s<<unsigned(r.modeCode[b]);}s<<"]}";
        }
        s<<"],\"meshes\":[";for(size_t i=0;i<meshes.size();++i){if(i)s<<',';const auto&m=meshes[i];s<<"{\"panel\":"<<m.panel<<",\"list\":"<<m.list<<",\"owner\":"<<m.owner<<",\"triangle_address\":"<<m.triangleAddress<<",\"ledger_frame\":"<<m.ledgerFrame<<",\"render_frame\":"<<m.renderFrame<<",\"qpc\":"<<m.qpc<<",\"space\":\"cached_pick_camera_frame\",\"triangles\":[";
            for(size_t j=0;j<m.triangles.size();++j){if(j)s<<',';const auto&t=m.triangles[j];s<<'[';for(unsigned v=0;v<3;++v){if(v)s<<',';floats(t.v[v],3);}for(unsigned v=0;v<3;++v){s<<',';floats(t.uv[v],2);}s<<']';}s<<"]}";}
        s<<"]}";rows.clear();meshes.clear();return s.str();
    }
};
} // namespace edvr::nofoot::menu_input

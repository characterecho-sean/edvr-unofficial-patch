#pragma once
#include "native_on_foot_menu_input.h"
#include <algorithm>
#include <cmath>

namespace edvr::nofoot::menu_input {
constexpr uint32_t kSurfaceModelVtableRva=0x55999C0;
struct SurfaceBindings {
    uint64_t ownerVtable=0,fullVtable=0,ownerFunction=0,modelVtable=0;bool knownLayout=false;
    bool ready() const noexcept{return knownLayout&&ownerVtable&&fullVtable&&ownerFunction&&modelVtable;}
};
// Read only inside the existing, pinned owner-transform callback. In this build the
// visual and pick providers select the same {model, CPU mesh} pair. A pair identity
// is a construction association, not proof that a particular GPU draw used it.
struct SurfaceMesh {
    uint64_t address=0,verticesAddress=0,indicesAddress=0;uint32_t status=0,rawHeader0=0;
    std::vector<float> vertices;std::vector<uint8_t> indices;
};
struct SurfacePair {
    uint64_t handle=0,handleVtable=0,getter=0,pair=0,model=0,modelVtable=0,renderResource=0,source=0;
    uint32_t status=0,modelStatus=0,keyStatus=0,resourceKey=0;float basis[16]{},scale=0;SurfaceMesh mesh;
};
struct SurfaceRow {
    uint32_t ledgerFrame=0,renderFrame=0,placementRenderFrame=0,thread=0,path=0,status=0,entryFlag=0,mapSteps=0,mapKey=0,mapStatus=0;
    uint64_t qpc=0,ctl=0,panel=0,list=0,owner=0,fullPanel=0,entry=0,definition=0,mapHead=0,count=0;
    uint64_t preRebuildTriangleCount=0;uint32_t preRebuildCountRead=0;
    float ownerWorld[16]{},ownerOutput[16]{};uint32_t transformValid=0;SurfacePair pairs[2];
};
class SurfaceCapture {
public:
    static constexpr uint32_t kMaxOwners=4,kMaxWalk=32;
    std::mutex mutex;std::atomic<uint32_t> contended{0};bool armed=false;uint32_t generation=0,selected=0;
    uint32_t callbacks=0,typeDeclined=0,nonMenu=0,quota=0,duplicate=0,crossed=0,layoutDeclined=0;std::vector<SurfaceRow> rows;SurfaceBindings bindings;
    void arm(uint32_t epoch,uint32_t frame,const SurfaceBindings& b){std::lock_guard<std::mutex> lock(mutex);generation=epoch;selected=frame;armed=true;bindings=b;
        rows.clear();callbacks=typeDeclined=nonMenu=quota=duplicate=crossed=layoutDeclined=0;contended.store(0);}
    void discard(){std::lock_guard<std::mutex> lock(mutex);armed=false;++generation;rows.clear();}
    template<class Read> static bool same(Read read,uintptr_t address,uint64_t expected){uint64_t value=0;return read(address,&value,8)&&value==expected;}
    template<class Read> static void mesh(SurfaceMesh& m,Read read){
        if(!m.address){m.status=1;return;}uint8_t header[2]{};uint64_t v=0,i=0;
        if(!read(m.address,header,2)||!read(m.address+8,&v,8)||!read(m.address+16,&i,8)){m.status=2;return;}
        m.rawHeader0=header[0];m.verticesAddress=v;m.indicesAddress=i;
        if(!header[1]||!v||!i){m.status=1;return;}
        m.indices.resize(size_t(header[1])*3);
        if(!read(i,m.indices.data(),m.indices.size())){m.status=2;m.indices.clear();return;}
        const auto vertexCount=1u+*std::max_element(m.indices.begin(),m.indices.end());
        m.vertices.resize(size_t(vertexCount)*5);
        if(!read(v,m.vertices.data(),m.vertices.size()*sizeof(float))){m.status=2;m.vertices.clear();m.indices.clear();return;}
        uint8_t verify[2]{};
        if(!read(m.address,verify,2)||std::memcmp(header,verify,2)||!same(read,m.address+8,v)||!same(read,m.address+16,i))m.status=3;
        else m.status=4;
        if(m.status!=4){m.vertices.clear();m.indices.clear();}
    }
    template<class Read> static void pair(SurfacePair& p,uint64_t modelVtable,Read read){
        if(!p.pair){p.status=1;return;}
        if(!read(p.pair,&p.model,8)||!read(p.pair+8,&p.mesh.address,8)){p.status=2;return;}
        mesh(p.mesh,read);p.status=4;
        if(!p.model)p.modelStatus=1;
        else if(!read(p.model,&p.modelVtable,8))p.modelStatus=2;
        else if(p.modelVtable!=modelVtable)p.modelStatus=5;
        else if(!read(p.model+0x10,p.basis,sizeof(p.basis))||
            !read(p.model+0x50,&p.scale,4)||!read(p.model+0xf0,&p.renderResource,8)||!read(p.model+0x150,&p.source,8))p.modelStatus=2;
        else {float verify[16]{},scale=0;
            p.modelStatus=(same(read,p.model,p.modelVtable)&&same(read,p.model+0xf0,p.renderResource)&&same(read,p.model+0x150,p.source)&&
                read(p.model+0x10,verify,sizeof(verify))&&!std::memcmp(verify,p.basis,sizeof(verify))&&
                read(p.model+0x50,&scale,4)&&!std::memcmp(&scale,&p.scale,4))?4u:3u;}
        if(!same(read,p.pair,p.model)||!same(read,p.pair+8,p.mesh.address))p.status=3;
    }
    template<class Read> static void handle(SurfacePair& p,uint64_t modelVtable,Read read){
        if(!p.handle){p.status=1;return;}uint8_t code[5]{};constexpr uint8_t pin[]={0x48,0x8b,0x41,0x08,0xc3};
        if(!read(p.handle,&p.handleVtable,8)||!p.handleVtable||!read(p.handleVtable+8,&p.getter,8)||!p.getter||
            !read(p.getter,code,5)||std::memcmp(code,pin,5)){p.status=5;return;}
        if(!read(p.handle+8,&p.pair,8)){p.status=2;return;}pair(p,modelVtable,read);
        if(!same(read,p.handle,p.handleVtable)||!same(read,p.handleVtable+8,p.getter)||!same(read,p.handle+8,p.pair))p.status=3;
    }
    template<class Read> static void keys(SurfaceRow& r,Read read){
        uint64_t pending[kMaxWalk]{},seen[kMaxWalk]{};unsigned top=0,n=0;
        if(r.mapHead)pending[top++]=r.mapHead;r.mapStatus=4;
        while(top){
            const uint64_t node=pending[--top];
            if(n==kMaxWalk||std::find(seen,seen+n,node)!=seen+n){r.mapStatus=5;break;}
            seen[n++]=node;uint64_t left=0,right=0,value=0;uint16_t key=0;
            if(!read(node,&left,8)||!read(node+8,&right,8)||!read(node+0x20,&key,2)||!read(node+0x28,&value,8)){r.mapStatus=2;break;}
            for(auto& p:r.pairs)if(p.pair&&value==p.pair){if(p.keyStatus==4)p.keyStatus=3;else if(!p.keyStatus){p.keyStatus=4;p.resourceKey=key;}}
            uint16_t verify=0;
            if(!same(read,node,left)||!same(read,node+8,right)||!same(read,node+0x28,value)||!read(node+0x20,&verify,2)||verify!=key){r.mapStatus=3;break;}
            for(uint64_t child:{left,right})if(child){if(top==kMaxWalk){r.mapStatus=5;break;}pending[top++]=child;}
            if(r.mapStatus!=4)break;
        }
        r.mapSteps=n;
        for(auto& p:r.pairs)if(p.pair){if(r.mapStatus!=4)p.keyStatus=r.mapStatus;else if(!p.keyStatus)p.keyStatus=1;}
    }
    template<class Read> void observe(const Row& scope,uintptr_t owner,uint64_t qpc,uint32_t ledger,uint32_t render,Read read){
        std::unique_lock<std::mutex> lock(mutex,std::try_to_lock);if(!lock.owns_lock()){contended.fetch_add(1);return;}
        if(!armed||scope.generation!=generation||scope.ledgerFrame!=selected)return;
        ++callbacks;if(ledger!=selected){++crossed;return;}if(!bindings.ready()){++layoutDeclined;return;}
        SurfaceRow r;r.ledgerFrame=ledger;r.renderFrame=render;r.placementRenderFrame=scope.renderFrame;r.thread=scope.thread;r.qpc=qpc;
        r.ctl=scope.ctl;r.panel=scope.panel;r.owner=owner;if(owner<16){++typeDeclined;return;}r.fullPanel=owner-16;
        const uint64_t ownerVtable=bindings.ownerVtable,fullVtable=bindings.fullVtable;
        uint64_t fn=0;
        if(!same(read,owner,ownerVtable)||!same(read,r.fullPanel,fullVtable)||!read(ownerVtable+16,&fn,8)||fn!=bindings.ownerFunction||
           !read(r.panel+f117::kOffList,&r.list,8)||!r.list||!same(read,r.list,owner)){++typeDeclined;return;}
        for(const auto& old:rows)if(old.owner==owner){++duplicate;return;}
        if(rows.size()>=kMaxOwners){++quota;return;}
        // The game's mesh refresh clears this count before calling the owner's
        // transform and populates its triangles after this callback returns.
        // It is callback-time evidence, never a surface identity gate.
        if(read(r.list+f117::kListCount,&r.preRebuildTriangleCount,8))r.preRebuildCountRead=1;
        if(!read(r.fullPanel+0x828,&r.count,8)||!read(r.fullPanel+0x830,&r.mapHead,8)||
           !read(r.fullPanel+0x860,&r.definition,8)||!read(r.fullPanel+0x868,&r.entry,8)){r.status=2;rows.push_back(std::move(r));return;}
        uint64_t fallbackNodes[kMaxWalk]{},fallbackLeft[kMaxWalk]{};unsigned fallbackCount=0;
        if(r.entry){
            r.path=1;uint64_t back=0;
            if(!read(r.entry,&r.entryFlag,4)||!read(r.entry+24,&back,8)||back!=r.fullPanel||
               !read(r.entry+8,&r.pairs[0].handle,8)||!read(r.entry+16,&r.pairs[1].handle,8))r.status=2;
            else {handle(r.pairs[0],bindings.modelVtable,read);handle(r.pairs[1],bindings.modelVtable,read);r.status=4;}
        }else if(r.count&&r.mapHead){
            r.path=2;uint64_t node=r.mapHead,seen[kMaxWalk]{};
            while(node){
                if(r.mapSteps==kMaxWalk||std::find(seen,seen+r.mapSteps,node)!=seen+r.mapSteps){r.status=5;break;}
                seen[r.mapSteps++]=node;uint64_t left=0;if(!read(node,&left,8)){r.status=2;break;}
                fallbackNodes[fallbackCount]=node;fallbackLeft[fallbackCount++]=left;
                if(left){node=left;continue;}
                uint16_t key=0;if(!read(node+0x20,&key,2)||!read(node+0x28,&r.pairs[0].pair,8)){r.status=2;break;}
                r.mapKey=key;pair(r.pairs[0],bindings.modelVtable,read);r.status=4;break;
            }
            if(!node&&!r.status)r.status=1;
        }else {r.path=3;r.status=1;}
        if(r.status==4)keys(r,read);
        // The bounded key walk is a separate observation. Recheck the exact
        // fallback selection path too, so a tree change cannot relabel an old
        // leftmost pair as the current fallback merely because it still exists.
        if(r.path==2&&r.status==4){
            for(unsigned i=0;i<fallbackCount;++i)if(!same(read,fallbackNodes[i],fallbackLeft[i]))r.status=3;
            uint16_t key=0;const auto leaf=fallbackNodes[fallbackCount-1];
            if(!same(read,leaf+0x28,r.pairs[0].pair)||!read(leaf+0x20,&key,2)||key!=r.mapKey)r.status=3;
        }
        if((scope.reads&1024)&&scope.observedOwner==owner&&!scope.observedOwnerMixed){r.transformValid=1;
            std::memcpy(r.ownerWorld,scope.ownerWorld,sizeof(r.ownerWorld));std::memcpy(r.ownerOutput,scope.ownerOut,sizeof(r.ownerOutput));}
        if(!same(read,owner,ownerVtable)||!same(read,r.fullPanel,fullVtable)||!same(read,ownerVtable+16,fn)||!same(read,r.panel+f117::kOffList,r.list)||!same(read,r.list,owner)||
            !same(read,r.fullPanel+0x828,r.count)||!same(read,r.fullPanel+0x830,r.mapHead)||!same(read,r.fullPanel+0x860,r.definition)||!same(read,r.fullPanel+0x868,r.entry)||
            (r.preRebuildCountRead&&!same(read,r.list+f117::kListCount,r.preRebuildTriangleCount)))r.status=3;
        if(r.path==1&&r.status==4){uint32_t flag=0;
            if(!read(r.entry,&flag,4)||flag!=r.entryFlag||!same(read,r.entry+24,r.fullPanel)||
               !same(read,r.entry+8,r.pairs[0].handle)||!same(read,r.entry+16,r.pairs[1].handle))r.status=3;}
        rows.push_back(std::move(r));
    }
    std::string close(){std::lock_guard<std::mutex> lock(mutex);armed=false;std::ostringstream s;s<<std::setprecision(9);
        const auto floats=[&](const float* p,size_t n){s<<'[';for(size_t i=0;i<n;++i){if(i)s<<',';if(std::isfinite(p[i]))s<<p[i];else s<<"null";}s<<']';};
        s<<"{\"schema\":\"edvr_menu_surface_v1\",\"selected_frame\":"<<selected<<",\"known_layout\":"<<(bindings.ready()?"true":"false")<<",\"expected_model_vtable\":"<<bindings.modelVtable
            <<",\"getter_calls\":0,\"gpu_draw_owner_proven\":false,\"association\":\"shared_model_pick_pair_construction_not_actual_GPU_draw_identity\",\"max_owners\":4,\"max_triangles\":255,\"max_vertices\":256,\"max_map_nodes\":32,\"callbacks\":"<<callbacks<<",\"type_declined\":"<<typeDeclined<<",\"non_menu\":"<<nonMenu<<",\"quota\":"<<quota<<",\"duplicate\":"<<duplicate<<",\"crossed_frame\":"<<crossed<<",\"layout_declined\":"<<layoutDeclined<<",\"contended\":"<<contended.load()<<",\"rows\":[";
        for(size_t j=0;j<rows.size();++j){if(j)s<<',';const auto&r=rows[j];
            s<<"{\"ledger_frame\":"<<r.ledgerFrame<<",\"render_frame\":"<<r.renderFrame<<",\"placement_render_frame\":"<<r.placementRenderFrame<<",\"thread\":"<<r.thread<<",\"qpc\":"<<r.qpc<<",\"ctl\":"<<r.ctl<<",\"panel\":"<<r.panel<<",\"list\":"<<r.list<<",\"owner\":"<<r.owner<<",\"full_panel\":"<<r.fullPanel<<",\"pre_rebuild_count_read\":"<<(r.preRebuildCountRead?"true":"false")<<",\"pre_rebuild_triangle_count\":"<<r.preRebuildTriangleCount<<",\"path\":"<<r.path<<",\"status\":"<<r.status<<",\"entry\":"<<r.entry<<",\"entry_flag\":"<<r.entryFlag<<",\"definition\":"<<r.definition<<",\"map_head\":"<<r.mapHead<<",\"map_count\":"<<r.count<<",\"map_steps\":"<<r.mapSteps<<",\"map_status\":"<<r.mapStatus<<",\"map_key\":"<<r.mapKey<<",\"transform_valid\":"<<r.transformValid<<",\"owner_world\":";floats(r.ownerWorld,16);s<<",\"owner_output\":";floats(r.ownerOutput,16);s<<",\"pairs\":[";
            for(unsigned k=0;k<2;++k){if(k)s<<',';const auto&p=r.pairs[k];const auto&m=p.mesh;
                s<<"{\"status\":"<<p.status<<",\"handle\":"<<p.handle<<",\"handle_vtable\":"<<p.handleVtable<<",\"observed_getter_address\":"<<p.getter<<",\"pair\":"<<p.pair<<",\"model\":"<<p.model<<",\"model_vtable\":"<<p.modelVtable<<",\"model_status\":"<<p.modelStatus<<",\"key_status\":"<<p.keyStatus<<",\"resource_key\":"<<p.resourceKey<<",\"constructor_resource_f0\":"<<p.renderResource<<",\"constructor_source_150\":"<<p.source<<",\"model_basis\":";floats(p.basis,16);s<<",\"scale\":";floats(&p.scale,1);
                s<<",\"mesh\":{\"status\":"<<m.status<<",\"address\":"<<m.address<<",\"vertices_address\":"<<m.verticesAddress<<",\"indices_address\":"<<m.indicesAddress<<",\"raw_header0\":"<<m.rawHeader0<<",\"space\":\"model_local_xyz_uv\",\"vertices\":";floats(m.vertices.data(),m.vertices.size());s<<",\"indices\":[";
                for(size_t n=0;n<m.indices.size();++n){if(n)s<<',';s<<unsigned(m.indices[n]);}s<<"]}}";}
            s<<"]}";
        }s<<"]}";rows.clear();return s.str();
    }
};
inline std::string attachSurface(std::string input,const std::string& surface){if(!input.empty()&&input.back()=='}'){input.pop_back();input+=",\"surface_evidence\":"+surface+"}";}return input;}
} // namespace edvr::nofoot::menu_input

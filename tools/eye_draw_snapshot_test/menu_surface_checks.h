#pragma once
#include "../../src/d3d11/native_on_foot_menu_surface.h"
#include <array>

inline std::string menuSurfaceChecks() {
    using namespace edvr::nofoot::menu_input;
    using namespace edvr::nofoot;
    // An address arena makes every read bounded and lets the fixture detect reads
    // that a rejected type or stale callback must never attempt.
    std::vector<uint8_t> memory(0x20000);constexpr uintptr_t base=0x100000;
    const auto put=[&](uintptr_t at,const auto& value){std::memcpy(memory.data()+at-base,&value,sizeof(value));};
    unsigned reads=0;
    const auto read=[&](uintptr_t at,void* out,size_t bytes){++reads;if(at<base||bytes>memory.size()||at-base>memory.size()-bytes)return false;
        std::memcpy(out,memory.data()+at-base,bytes);return true;};
    const uint64_t ov=base+0x100,fv=base+0x200,fn=base+0x300,mv=base+0x400;
    SurfaceBindings bindings{ov,fv,fn,mv,true};put(ov+16,fn);
    const uint64_t hv=base+0x500,getter=base+0x600;put(hv+8,getter);
    const std::array<uint8_t,5> pin{{0x48,0x8b,0x41,0x08,0xc3}};put(getter,pin);
    const uint64_t root=base+0x700,left=base+0x800,right=base+0x900;
    put(root,left);put(root+8,right);put(root+0x20,uint16_t(20));
    const auto owner=[&](unsigned index){
        const uint64_t full=base+0x1000+index*0x1000,panel=base+0x7000+index*0x1000,list=base+0xc000+index*0x100;
        put(full,fv);put(full+16,ov);put(panel+pickmesh::kOffList,list);put(list,full+16);put(list+pickmesh::kListCount,uint64_t(120));
        put(full+0x828,uint64_t(3));put(full+0x830,root);put(full+0x860,base+0x1f000);put(full+0x868,base+0xd000);
        Row scope;scope.generation=7;scope.ledgerFrame=801;scope.renderFrame=900;scope.panel=panel;scope.ctl=42;scope.thread=9;
        scope.reads=1024;scope.observedOwner=full+16;scope.ownerWorld[0]=scope.ownerOut[0]=1;return scope;};
    const uint64_t entry=base+0xd000;put(entry,uint32_t(1));put(entry+24,base+0x1000);
    for(unsigned i=0;i<2;++i){
        const uint64_t handle=base+0xd100+i*0x100,pair=base+0xd300+i*0x100,model=base+0xe000+i*0x1000;
        const uint64_t mesh=base+0x11000+i*0x100,vertices=base+0x12000+i*0x100,indices=base+0x13000+i*0x100;
        put(entry+8+i*8,handle);put(handle,hv);put(handle+8,pair);put(pair,model);put(pair+8,mesh);put(model,mv);
        std::array<float,16> pose{};pose[0]=pose[5]=pose[10]=1;pose[12]=float(i);put(model+0x10,pose);put(model+0x50,1.0f);
        put(model+0xf0,base+0x19000+i*0x100);put(model+0x150,base+0x1a000+i*0x100);
        put(mesh,std::array<uint8_t,2>{{0,1}});put(mesh+8,vertices);put(mesh+16,indices);
        put(vertices,std::array<float,15>{{0,0,0,0,0,1,0,0,1,0,0,1,float(i),0,1}});put(indices,std::array<uint8_t,3>{{0,1,2}});
        const uint64_t node=i?right:left;put(node+0x20,uint16_t(10+i*20));put(node+0x28,pair);
    }
    SurfaceCapture capture;auto scope=owner(0);
    Capture placement;placement.arm(L"menu",800,819);placement.frame.store(801);Row placed;
    check(placement.before(placed,scope.ctl,scope.panel,scope.renderFrame,scope.thread,990,read),"placement fixture begins before mesh rebuild");
    capture.arm(placed.generation,801,bindings);
    const uint64_t pickList=base+0xc000,pickTris=base+0x15000;
    put(pickTris,std::array<pickmesh::Tri,120>{});put(pickList+pickmesh::kListTris,pickTris);
    unsigned originals=0;
    afterOriginal([&](){
        ++originals;put(pickList+pickmesh::kListCount,uint64_t(0));
        observeOwner(placed,base+0x1010,base+0x18000,read);
        capture.observe(placed,base+0x1010,1000,801,901,read);
        put(pickList+pickmesh::kListCount,uint64_t(120));return 17ull;
    },[&](uint64_t){placement.finish(placed,1010,read);});
    check(originals==1&&capture.rows.size()==1&&capture.rows[0].status==4,"surface observer forwards original once and captures paired owner");
    check(capture.rows[0].preRebuildCountRead==1&&capture.rows[0].preRebuildTriangleCount==0&&
          capture.rows[0].pairs[0].mesh.status==4&&capture.rows[0].pairs[1].mesh.status==4,
          "reset-zero owner callback copies both authored pairs before pick triangles are populated");
    check(placement.rows.size()==1&&placement.rows[0].triCount==120&&placement.rows[0].owner==capture.rows[0].owner&&
          placement.rows[0].observedOwner==capture.rows[0].owner&&placement.rows[0].meshStatus==7,
          "post-original placement independently joins 120 cached triangles to the same owner");
    const auto& row=capture.rows[0];
    check(row.mapStatus==4&&row.mapSteps==3&&row.pairs[0].resourceKey==10&&row.pairs[1].resourceKey==30,"bounded key walk visits both tree branches");
    check(row.pairs[0].mesh.vertices.size()==15&&row.pairs[1].mesh.vertices[12]==1&&row.pairs[0].modelStatus==4&&row.pairs[1].modelStatus==4,"both typed authored meshes and model bases survive independent copies");
    check(row.renderFrame==901&&row.placementRenderFrame==900&&row.transformValid==1,"surface stamps identify actual callback separately from placement");
    capture.observe(placed,base+0x1010,1001,801,901,read);check(capture.duplicate==1,"same owner deduplicated before payload reads");
    const std::string fixture=capture.close();
    capture.arm(7,801,bindings);put(base+0x1868,uint64_t(0));put(base+0x1830,left);put(base+0x1828,uint64_t(1));
    capture.observe(scope,base+0x1010,1010,801,901,read);
    check(capture.rows[0].path==2&&capture.rows[0].status==4&&capture.rows[0].pairs[0].resourceKey==10,"fallback starts at actual root and handles root without a left child");
    capture.arm(7,801,bindings);
    const auto changedFallback=[&](uintptr_t at,void* out,size_t bytes){const bool ok=read(at,out,bytes);if(at==base+0x12000)put(left,right);return ok;};
    capture.observe(scope,base+0x1010,1011,801,901,changedFallback);
    check(capture.rows[0].status==3,"changed leftmost selection is rejected even when the former pair remains in the map");put(left,uint64_t(0));
    scope=owner(0);capture.arm(7,801,bindings);put(base+0xe000,uint64_t(99));
    unsigned typedReads=0;const auto wrongType=[&](uintptr_t at,void* out,size_t bytes){if(at>=base+0xe010&&at<=base+0xe150)++typedReads;return read(at,out,bytes);};
    capture.observe(scope,base+0x1010,1020,801,901,wrongType);
    check(capture.rows[0].pairs[0].modelStatus==5&&typedReads==0,"unknown model type never admits typed fields");put(base+0xe000,mv);
    capture.arm(7,801,bindings);put(getter,uint8_t(0));capture.observe(scope,base+0x1010,1030,801,901,read);
    check(capture.rows[0].pairs[0].status==5&&capture.nonMenu==0,"unsupported getter is unavailable evidence, not a non-menu claim");put(getter,pin);
    SurfaceRow walk;walk.mapHead=base+0x14000;walk.pairs[0].pair=base+0xd300;
    for(unsigned i=0;i<33;++i){const auto n=walk.mapHead+i*0x40;put(n+8,i<32?n+0x40:uint64_t(0));}
    SurfaceCapture::keys(walk,read);check(walk.mapStatus==5&&walk.mapSteps==32&&walk.pairs[0].keyStatus==5,"tree cap never claims an unmatched pair absent");
    walk={};walk.mapHead=left;walk.pairs[0].pair=base+0xd300;unsigned keyReads=0;
    const auto changed=[&](uintptr_t at,void* out,size_t bytes){const bool ok=read(at,out,bytes);if(at==left+0x20&&++keyReads==2)*static_cast<uint16_t*>(out)=99;return ok;};
    SurfaceCapture::keys(walk,changed);check(walk.mapStatus==3&&walk.pairs[0].keyStatus==3,"changed tree identity invalidates key attribution");
    capture.arm(7,801,bindings);for(unsigned i=0;i<5;++i){auto s=owner(i);put(entry+24,base+0x1000+i*0x1000);
        put(base+0xc000+i*0x100+pickmesh::kListCount,uint64_t(i==0?2:i==1?0:120));
        capture.observe(s,base+0x1010+i*0x1000,1040+i,801,901,read);}
    check(capture.rows.size()==4&&capture.quota==1&&capture.rows[0].preRebuildTriangleCount==2&&
          capture.rows[1].preRebuildTriangleCount==0&&capture.rows[2].preRebuildTriangleCount==120,
          "typed quad and pre-rebuild-zero owners are captured with the unchanged four-owner quota");
    scope=owner(0);put(entry+24,base+0x1000);capture.arm(7,801,bindings);put(base+0xc000+pickmesh::kListCount,uint64_t(2));capture.observe(scope,base+0x1010,1050,801,901,read);
    check(capture.rows.size()==1&&capture.rows[0].preRebuildTriangleCount==2&&capture.nonMenu==0,
          "ordinary cached quad count is evidence, not a callback-time identity claim");
    capture.arm(7,801,bindings);
    const auto unreadCount=[&](uintptr_t at,void* out,size_t bytes){return at!=base+0xc000+pickmesh::kListCount&&read(at,out,bytes);};
    capture.observe(scope,base+0x1010,1051,801,901,unreadCount);
    check(capture.rows.size()==1&&capture.rows[0].status==4&&!capture.rows[0].preRebuildCountRead&&
          capture.rows[0].pairs[0].mesh.status==4,"unreadable pre-rebuild count does not suppress typed authored evidence");
    capture.arm(8,801,bindings);reads=0;capture.observe(scope,base+0x1010,1060,801,901,read);check(reads==0&&capture.callbacks==0,"old generation performs no reads after rearm");
    capture.arm(7,801,bindings);capture.observe(scope,base+0x1010,1070,802,902,read);check(reads==0&&capture.crossed==1,"callback crossing selected frame is explicit and performs no game reads");
    capture.arm(7,801,{});capture.observe(scope,base+0x1010,1080,801,901,read);check(reads==0&&capture.layoutDeclined==1,"unknown build layout is unavailable without pointer reads");
    return fixture;
}

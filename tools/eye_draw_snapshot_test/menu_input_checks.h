#pragma once
#include "../../src/d3d11/native_on_foot_menu_input.h"
#include <array>
#include <thread>
inline std::string menuInputChecks() {
    using namespace edvr::nofoot::menu_input;
    check(!hookGate(0,false,true,true)&&hookGate(1,false,true,true)&&hookGate(2,false,true,true)&&hookGate(3,false,true,true),
          "passive input capture opens placement/ray/window but never the update/camera-getter hook with cursor drawing off");
    for(bool capture:{false,true}) {
        unsigned originals=0,observations=0;
        for(unsigned i=0;i<4;++i){const auto result=afterOriginal([&](){++originals;return 0x12345678ull;},[&](uint64_t result){if(capture){check(result==0x12345678ull,"observer sees unchanged original result");++observations;}});
            check(result==0x12345678ull,"passive forwarding preserves result for every existing callback");}
        check(originals==4&&observations==(capture?4u:0u),"capture on/off forwards each original exactly once and changes no callback count");
    }
    Row ownerRow;unsigned ownerReads=0;
    const auto ownerRead=[&](uintptr_t,void* dst,size_t bytes){++ownerReads;std::memset(dst,int(ownerReads),bytes);return ownerReads!=4;};
    observeOwner(ownerRow,100,200,ownerRead);float committed[16];std::memcpy(committed,ownerRow.ownerWorld,sizeof(committed));
    check((ownerRow.reads&1024)!=0,"successful owner matrix pair is valid");
    observeOwner(ownerRow,100,200,ownerRead);
    check(!(ownerRow.reads&1024)&&!std::memcmp(committed,ownerRow.ownerWorld,sizeof(committed))&&ownerRow.ownerCalls==2,
          "failed second owner read clears validity and never commits a partially replaced matrix pair");
    Row rayRow;const auto goodRead=[](uintptr_t,void* dst,size_t bytes){std::memset(dst,0,bytes);return true;};
    const auto badRead=[](uintptr_t,void*,size_t){return false;};
    observeRay(rayRow,1,2,3,4,5,1,goodRead);check((rayRow.reads&384)==384,"accepted ray payload is valid");
    observeRay(rayRow,1,2,3,4,5,0,goodRead);check(!(rayRow.reads&256)&&rayRow.rayCalls==2,"rejected second ray clears earlier accepted UV validity");
    observeWindow(rayRow,1,goodRead);observeWindow(rayRow,1,badRead);
    check(!(rayRow.reads&(256|512))&&rayRow.windowCalls==2,"failed repeated window clears its earlier UV validity");
    std::array<uint8_t,0x200> ctl{};std::array<uint8_t,0x500> panel{};std::array<uint8_t,0x40> list{},mode{};
    std::array<uint8_t,0x80> vt{};std::array<uint8_t,8> code{0xB0,0,0xC3};std::vector<edvr::nofoot::f117::Tri> triangles(120);
    const auto put=[](uint8_t* dst,uintptr_t value){std::memcpy(dst,&value,8);};
    put(ctl.data()+edvr::nofoot::f114b::kOffMode,reinterpret_cast<uintptr_t>(mode.data()));put(mode.data(),reinterpret_cast<uintptr_t>(vt.data()));
    put(vt.data()+edvr::nofoot::f114b::kModeGetter,reinterpret_cast<uintptr_t>(code.data()));
    put(panel.data()+edvr::nofoot::f117::kOffList,reinterpret_cast<uintptr_t>(list.data()));
    put(list.data()+edvr::nofoot::f117::kListTris,reinterpret_cast<uintptr_t>(triangles.data()));put(list.data()+edvr::nofoot::f117::kListCount,120);
    struct Region{uintptr_t start;size_t size;};
    const Region regions[]={{reinterpret_cast<uintptr_t>(ctl.data()),ctl.size()},{reinterpret_cast<uintptr_t>(panel.data()),panel.size()},
        {reinterpret_cast<uintptr_t>(list.data()),list.size()},{reinterpret_cast<uintptr_t>(mode.data()),mode.size()},{reinterpret_cast<uintptr_t>(vt.data()),vt.size()},
        {reinterpret_cast<uintptr_t>(code.data()),code.size()},{reinterpret_cast<uintptr_t>(triangles.data()),triangles.size()*sizeof(triangles[0])}};
    const auto read=[&](uintptr_t address,void* dst,size_t bytes){for(const auto& region:regions)if(address>=region.start&&address-region.start<=region.size&&bytes<=region.size-(address-region.start)){std::memcpy(dst,reinterpret_cast<void*>(address),bytes);return true;}return false;};
    Capture c;c.arm(L"123456789001",800,819);c.frame.store(801);Row r;
    c.before(r,reinterpret_cast<uintptr_t>(ctl.data()),reinterpret_cast<uintptr_t>(panel.data()),0,9,100,read);r.windowCalls=1;r.reads|=512;r.uv[0]=.25f;r.uv[1]=.75f;
    c.finish(r,110,read);
    check(c.rows.size()==1&&c.meshes.size()==1&&c.rows[0].modeValue==0&&c.rows[0].meshStatus==7&&c.meshes[0].triangles.size()==120,
          "existing plain mode accessor is decoded without calling it; selected-frame cached curved input triangles and UV retain");
    Capture changed;changed.arm(L"123456789099",800,819);changed.frame.store(801);Row firstMesh,changedMesh;
    changed.before(firstMesh,reinterpret_cast<uintptr_t>(ctl.data()),reinterpret_cast<uintptr_t>(panel.data()),0,9,100,read);changed.finish(firstMesh,101,read);
    put(list.data()+edvr::nofoot::f117::kListCount,6);
    changed.before(changedMesh,reinterpret_cast<uintptr_t>(ctl.data()),reinterpret_cast<uintptr_t>(panel.data()),0,9,102,read);changed.finish(changedMesh,103,read);
    check(changed.meshes.size()==2&&changed.rows.back().meshStatus==7&&changed.meshes.back().triangles.size()==6,
          "same-frame list count changes never reuse a mismatching earlier triangle payload");
    put(list.data()+edvr::nofoot::f117::kListCount,120);
    Row inFlight;c.before(inFlight,reinterpret_cast<uintptr_t>(ctl.data()),reinterpret_cast<uintptr_t>(panel.data()),0,9,111,read);
    const std::string fixture=c.close(31,1000);c.finish(inFlight,112,read);
    check(fixture.find("\"in_flight_at_close\":1")!=std::string::npos&&fixture.find("\"getter_calls\":0")!=std::string::npos&&c.rows.empty(),
          "closing a run reports unfinished original callbacks and never waits for or admits their late completion");
    c.arm(L"123456789002",900,919);c.frame.store(901);Row old;c.before(old,reinterpret_cast<uintptr_t>(ctl.data()),reinterpret_cast<uintptr_t>(panel.data()),0,9,200,read);
    c.discard();c.arm(L"123456789003",1000,1019);c.finish(old,210,read);
    check(c.rows.empty()&&c.pending.load()==0,"a scope from a discarded/replaced generation cannot enter the new capture or underflow its pending count");
    c.frame.store(1020);check(!c.active(),"input observer closes at its bounded ledger window, before the later GPU readback write");c.discard();
    c.arm(L"123456789004",1100,1119);c.frame.store(1101);
    put(list.data()+edvr::nofoot::f117::kListCount,257);
    for(unsigned i=0;i<81;++i){Row capped;c.before(capped,reinterpret_cast<uintptr_t>(ctl.data()),reinterpret_cast<uintptr_t>(panel.data()),0,9,300+i,read);c.finish(capped,400+i,read);}
    check(c.rows.size()==80&&c.rows[0].meshStatus==2&&c.meshes.empty()&&c.rowQuota==1,"oversize input triangles and non-anchor row quota refuse explicitly without copying beyond the cap");
    put(list.data()+edvr::nofoot::f117::kListCount,120);Row anchor;c.before(anchor,reinterpret_cast<uintptr_t>(ctl.data()),reinterpret_cast<uintptr_t>(panel.data()),0,9,500,read);c.finish(anchor,501,read);
    check(c.rows.size()==81&&c.meshes.size()==1,"other-panel saturation cannot consume the independently reserved curved-menu input rows/meshes");
    c.mutex.lock();bool admitted=true;std::thread concurrent([&](){Row denied;admitted=c.before(denied,reinterpret_cast<uintptr_t>(ctl.data()),reinterpret_cast<uintptr_t>(panel.data()),0,9,600,read);});concurrent.join();c.mutex.unlock();
    check(!admitted&&c.contended.load()==1,"hook-thread admission never waits on the capture mutex and records contention");
    c.discard();
    return fixture;
}

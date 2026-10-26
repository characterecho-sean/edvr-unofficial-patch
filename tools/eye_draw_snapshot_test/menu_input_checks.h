#pragma once
#include "../../src/d3d11/native_on_foot_menu_input.h"
#include "../../src/d3d11/native_on_foot_menu_bridge.h"
#include <array>
#include <thread>
#include <condition_variable>
inline std::string menuInputDrawBridgeFixture;
inline std::string menuInputChecks() {
    using namespace edvr::nofoot::menu_input;
    check(!hookGate(0,false,true,true)&&hookGate(1,false,true,true)&&hookGate(2,false,true,true)&&hookGate(3,false,true,true),
          "legacy instrument gate policy leaves update closed; the separate manual observer dispatch does not activate it");
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
    std::array<uint8_t,0x240> indirectMode{};std::array<uint8_t,21> indirectCode{0x48,0x8b,0x81,0x18,2,0,0,0x48,0x85,0xc0,0x74,3,0x8b,0,0xc3,0xb8,2,0,0,0,0xc3};int32_t modeValue=0;
    put(ctl.data()+edvr::nofoot::f114b::kOffMode,reinterpret_cast<uintptr_t>(indirectMode.data()));put(indirectMode.data(),reinterpret_cast<uintptr_t>(vt.data()));
    put(vt.data()+edvr::nofoot::f114b::kModeGetter,reinterpret_cast<uintptr_t>(indirectCode.data()));put(indirectMode.data()+0x218,reinterpret_cast<uintptr_t>(&modeValue));
    const Region bridgeRegions[]={{reinterpret_cast<uintptr_t>(ctl.data()),ctl.size()},{reinterpret_cast<uintptr_t>(indirectMode.data()),indirectMode.size()},
        {reinterpret_cast<uintptr_t>(vt.data()),vt.size()},{reinterpret_cast<uintptr_t>(indirectCode.data()),indirectCode.size()},{reinterpret_cast<uintptr_t>(&modeValue),4}};
    const auto bridgeRead=[&](uintptr_t address,void* dst,size_t bytes){for(const auto& a:bridgeRegions)if(address>=a.start&&address-a.start<=a.size&&bytes<=a.size-(address-a.start)){std::memcpy(dst,reinterpret_cast<void*>(address),bytes);return true;}return false;};
    const auto ctlAddress=reinterpret_cast<uintptr_t>(ctl.data());
    check(bridgeMode(ctlAddress,bridgeRead).status==3&&bridgeMode(ctlAddress,bridgeRead).value==0,"verified indirect mode accessor reads2D value without executing game code");
    modeValue=1;check(bridgeMode(ctlAddress,bridgeRead).value==1,"same verified accessor exposes ray mode rather than assuming on-foot always2D");modeValue=0;
    for(bool instruments:{false,true}){unsigned originalCalls=0,legacyCalls=0,observed=0;
        const auto result=dispatchUpdate(instruments,[&](){++originalCalls;return 42ull;},[&](){++observed;},[&](){++legacyCalls;++originalCalls;return 42ull;});
        check(result==42&&originalCalls==1&&legacyCalls==(instruments?1u:0u)&&observed==(instruments?0u:1u),"actual update dispatch callsoriginalonce and observer-only never calls legacy APIs regardless of live mouse key");}
    unsigned throws=0;volatile bool throwNow=true;try{dispatchUpdate(false,[&]()->uint64_t{++throws;if(throwNow)throw 7;return 1;},[](){},[](){return 0ull;});check(false,"exception must propagate");}catch(int n){check(n==7&&throws==1,"update dispatch preserves original exceptions and call count");}
    BridgeCapture bridge;bridge.arm(800,819,77);bridge.gateOpened(800,90);BridgeUpdate update;
    check(bridge.begin(update,ctlAddress,801,901,9,100,bridgeRead),"selected-frame update admitted");
    const auto nested=bridge.draw(801,901,101,&update);
    check(nested.find("\"completed\":false")!=std::string::npos&&nested.find("\"enclosing_update\":null")==std::string::npos,"drawnested insidecurrentTLSupdate has true lifetime witness and coherent inflight snapshot");
    update.returned=true;bridge.finish(update,901,110,bridgeRead);
    BridgePlacement placement;placement.generation=77;placement.ctl=ctlAddress;placement.ledgerFrame=801;placement.renderFrame=901;placement.thread=9;placement.triangles=120;placement.panel=22;placement.list=33;placement.owner=44;placement.ownerVtable=55;placement.observedOwnerFunctionAddress=66;placement.triangleAddress=77;placement.beforeQpc=111;placement.afterQpc=112;
    bridge.placement(placement,bridgeRead);menuInputDrawBridgeFixture=bridge.draw(801,901,100,nullptr,[](){return uint64_t(120);});
    check(menuInputDrawBridgeFixture.find("\"draw_marker_qpc\":100,\"snapshot_qpc\":120")!=std::string::npos,
          "callback completion during GPUstaging is prior to numeric snapshot but explicitly after the original draw marker");
    const std::string bridgeFixture=bridge.close();
    check(bridgeFixture.find("\"update_gate_qpc\":90")!=std::string::npos&&bridgeFixture.find("\"status\":\"observed\"")!=std::string::npos,"closed bridge retains gate timing and completed update");
    bridge.arm(800,819,78);bridge.placement(placement,bridgeRead);
    check(bridge.draw(801,901,130,nullptr).find("\"controllers\":[]")!=std::string::npos,"old placement cannot contaminate rearm even after old capture completion");
    BridgeUpdate oldUpdate;bridge.begin(oldUpdate,ctlAddress,801,901,9,140,bridgeRead);bridge.discard();bridge.arm(800,819,80);bridge.finish(oldUpdate,901,150,bridgeRead);
    check(bridge.draw(801,901,160,&oldUpdate).find("\"enclosing_update\":null")!=std::string::npos&&bridge.close().find("\"in_flight_upper_bound\":0")!=std::string::npos,"old update/TLS witness cannot contaminate replacement run or underflow pending");
    bridge.arm(800,819,81);for(unsigned i=0;i<5;++i){BridgeUpdate u;if(bridge.begin(u,ctlAddress+i*16,801,901,9,170+i,bridgeRead)){u.returned=true;bridge.finish(u,901,180+i,bridgeRead);}}
    check(bridge.close().find("\"slot_overflow\":1")!=std::string::npos,"fifth controller is explicit overflow rather than silently replacing another identity");
    bridge.arm(800,819,82);for(unsigned i=0;i<65;++i){BridgeUpdate u;if(bridge.begin(u,ctlAddress,801,901,9,200+i*2,bridgeRead)){u.returned=true;bridge.finish(u,901,201+i*2,bridgeRead);}}
    check(bridge.close().find("\"update_overflow\":1")!=std::string::npos,"whole-run update rows are bounded64");
    bridge.arm(800,819,83);std::mutex barrier;std::condition_variable cv;bool inside=false,release=false;
    const auto blockedRead=[&](uintptr_t address,void* dst,size_t bytes){{std::unique_lock<std::mutex> l(barrier);if(!inside){inside=true;cv.notify_one();cv.wait(l,[&](){return release;});}}return bridgeRead(address,dst,bytes);};
    std::thread producer([&](){BridgeUpdate u;bridge.begin(u,ctlAddress,801,901,9,400,blockedRead);});
    {std::unique_lock<std::mutex> l(barrier);cv.wait(l,[&](){return inside;});}
    check(bridge.draw(801,901,401,nullptr)=="{\"status\":\"contended\"}","render read never waits/spins on callback publication");
    {std::lock_guard<std::mutex> l(barrier);release=true;}cv.notify_one();producer.join();
    check(bridge.close().find("\"in_flight_upper_bound\":1")!=std::string::npos,"close explicitly reports incomplete update callbacks");
    return attachBridge(fixture,bridgeFixture);
}

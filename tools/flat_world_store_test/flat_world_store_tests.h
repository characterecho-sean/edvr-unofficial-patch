#pragma once
#include "../../src/d3d11/flat_world_store.h"

// Call from the established optimized_station rig so the PE/pin decision and
// the passive dispatch/guarded field reader share one focused gate.
inline void flatWorldStoreTests(void (*check)(bool,const char*)) {
    using namespace edvr;
    alignas(8) uint8_t record[0x300]{},bucket[0x20]{},node[0x100]{},bounds[0x80]{};
    alignas(8) uint8_t builder[0x100]{},storePose[0x80]{},matrix[0x40]{},name[96]{};
    auto put=[](uint8_t* p,size_t offset,const void* source,size_t n){std::memcpy(p+offset,source,n);};
    uintptr_t nodePtr=reinterpret_cast<uintptr_t>(node),boundsPtr=reinterpret_cast<uintptr_t>(bounds);
    uintptr_t builderPtr=reinterpret_cast<uintptr_t>(builder),storePtr=reinterpret_cast<uintptr_t>(storePose);
    uintptr_t matrixPtr=reinterpret_cast<uintptr_t>(matrix),namePtr=reinterpret_cast<uintptr_t>(name);
    uintptr_t owner=0x123456781000ull;
    put(bucket,0x10,&owner,sizeof(owner));put(record,0x18,&nodePtr,sizeof(nodePtr));
    put(node,0x08,&boundsPtr,sizeof(boundsPtr));put(node,0,&namePtr,sizeof(namePtr));
    put(record,0x290,&builderPtr,sizeof(builderPtr));put(record,0x190,&storePtr,sizeof(storePtr));
    put(storePose,0x50,&matrixPtr,sizeof(matrixPtr));
    const float q[4]={0,0,0,1},t[4]={100,200,300,0},c[4]={3,4,5,0};
    put(builder,0x10,q,sizeof(q));put(builder,0x20,t,sizeof(t));put(builder,0x80,c,sizeof(c));
    const float local[3]={7,8,9},radius=6,raw[3]={60,70,80};
    put(bounds,0x20,local,sizeof(local));put(bounds,0x2C,&radius,sizeof(radius));
    put(record,0x240,raw,sizeof(raw));put(record,0x280,&radius,sizeof(radius));
    float A[15]{};A[0]=A[5]=A[10]=1;A[12]=11;A[13]=13;A[14]=17;
    float B[16]{};B[0]=B[5]=B[10]=B[15]=1;B[12]=19;B[13]=23;B[14]=29;
    put(storePose,0,A,sizeof(A));put(matrix,0,B,sizeof(B));
    const uint64_t mask=0xA5A5B6B6CCDD1234ull;const uint8_t admitted=1;
    put(record,0x208,&mask,sizeof(mask));put(record,0x234,&admitted,sizeof(admitted));
    const char* label="RockFeature_17";std::memcpy(name,label,std::strlen(label)+1);
    FlatWorldWitnessInput s{};
    const uintptr_t rec=reinterpret_cast<uintptr_t>(record),b=reinterpret_cast<uintptr_t>(bucket);
    check(flatWorldStoreRead(b,rec,71,&s)==0&&s.frame==71&&s.record==rec&&s.owner==owner,
          "flat world store: guarded selected record and bucket owner");
    check(s.source==1&&s.center[0]==103&&s.center[1]==204&&s.center[2]==305&&s.radius==6,
          "flat world store: builder pose takes established first priority");
    check((s.validMask&(flatworldstore::poseBit|flatworldstore::storeBit|flatworldstore::rawBit|
         flatworldstore::radiusBit|flatworldstore::maskBit|flatworldstore::admittedBit|
         flatworldstore::nodeBit|flatworldstore::nameBit))==0xff&&
         s.storeCenter[0]==37&&s.storeCenter[1]==44&&s.storeCenter[2]==55&&s.rawCenter[0]==60,
          "flat world store: all three non-producer centre sources retained");
    check(s.viewMask==mask&&s.admitted==1&&s.node==nodePtr&&s.nodeValidator!=0&&
         std::strcmp(s.name,label)==0&&s.nameClass==static_cast<uint8_t>(sealed::NameClass::kOther),
          "flat world store: post-store admission and validated raw node name");
    const uintptr_t poison=1;put(record,0x290,&poison,sizeof(poison));
    check(flatWorldStoreRead(b,rec,72,&s)==0&&s.source==2&&s.center[0]==37&&
          !(s.validMask&flatworldstore::poseBit),"flat world store: unreadable builder falls to store-style world sphere");
    put(record,0x190,&poison,sizeof(poison));
    check(flatWorldStoreRead(b,rec,73,&s)==0&&s.source==3&&s.center[1]==70&&
          !(s.validMask&flatworldstore::storeBit),"flat world store: unreadable store pose falls to usable raw sphere");
    const float zero[3]{};put(record,0x240,zero,sizeof(zero));
    check(flatWorldStoreRead(b,rec,74,&s)==0&&s.source==4&&!(s.validMask&flatworldstore::radiusBit),
          "flat world store: zero raw centre is not an origin witness");
    put(record,0x290,&builderPtr,sizeof(builderPtr));put(record,0x190,&storePtr,sizeof(storePtr));
    const float invalidRadius=-1;put(record,0x280,&invalidRadius,sizeof(invalidRadius));
    check(flatWorldStoreRead(b,rec,75,&s)==0&&s.source==4&&(s.validMask&flatworldstore::poseBit)&&
          (s.validMask&flatworldstore::storeBit)&&s.center[0]==0,
          "flat world store: readable pose with invalid radius does not silently choose another source");
    put(node,0,&poison,sizeof(poison));
    check(flatWorldStoreRead(b,rec,76,&s)==0&&!(s.validMask&flatworldstore::nameBit)&&s.nodeValidator==0,
          "flat world store: unreadable name text is an observed unknown");
    check(flatWorldStoreRead(b,1,77,&s)==10&&flatWorldStoreRead(b,0,77,&s)==10,
          "flat world store: unreadable or null record is a terminal guarded miss");
    int stationCalls=0,flatCalls=0,forwardCalls=0;uintptr_t got[5]{};int order=0;bool stationOnly=false;
    auto forward=[&](uintptr_t a,uintptr_t m,uintptr_t o,uintptr_t l,uintptr_t r) noexcept -> uintptr_t {
        ++forwardCalls;order=1;got[0]=a;got[1]=m;got[2]=o;got[3]=l;got[4]=r;return 0xCAFE9876u;};
    auto station=[&](uintptr_t,uintptr_t) noexcept {++stationCalls;check(order==2||stationOnly,"flat world store: station follows passive raw observation");order=3;};
    auto flat=[&](uintptr_t,uintptr_t) noexcept {check(order==1,"flat world store: passive call follows original");++flatCalls;order=2;};
    const uintptr_t returned=flatWorldStorePostForward(11,22,33,44,55,false,true,forward,station,flat);
    check(returned==0xCAFE9876u&&stationCalls==0&&flatCalls==1&&got[0]==11&&got[1]==22&&
          got[2]==33&&got[3]==44&&got[4]==55,
          "flat world store: flat-only observer forwards five arguments and return without station mutation");
    stationOnly=true;flatWorldStorePostForward(11,22,33,44,55,true,false,forward,station,flat);stationOnly=false;
    check(stationCalls==1&&flatCalls==1,"flat world store: station-only path preserved");
    flatWorldStorePostForward(11,22,33,44,55,true,true,forward,station,flat);
    check(stationCalls==2&&flatCalls==2&&order==3,
          "flat world store: with both consumers, passive reads before station mutation");
    check(!flatWorldStoreGateWanted(false,true,false)&&flatWorldStoreGateWanted(false,true,true)&&
          !flatWorldStoreGateWanted(false,false,true),
          "flat world store: wrong-PE/unready or no-consumer relay remains closed");
}

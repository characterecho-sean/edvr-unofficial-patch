#pragma once
// Passive, guarded reads after the pinned admission store returns. Offsets and
// calculations are the established optimized_station.cpp worldOfRecord path;
// no record, node, or pose field is read until the witness selects an attempt.
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "flat_world_witness.h"
#include "optimized_station_core.h"
#include "station_lockfree_core.h"

namespace edvr {
template<class Attempt>
inline void flatWorldStoreInstallGuard(std::mutex& mutex,std::atomic<bool>& ready,
    std::atomic<bool>& refused,Attempt&& attempt) noexcept {
    if(ready.load(std::memory_order_acquire)||refused.load(std::memory_order_acquire))return;
    std::lock_guard<std::mutex> lock(mutex);
    if(ready.load(std::memory_order_acquire)||refused.load(std::memory_order_acquire))return;
    attempt();
}
inline bool flatWorldStoreGateWanted(bool stationGathering,bool flatActive,bool hookReady) noexcept {
    return hookReady&&(stationGathering||flatActive);
}
inline bool flatWorldStorePublish(bool enabled,uint32_t frame,std::atomic<bool>& active,
    std::atomic<uint32_t>& currentFrame) noexcept {
    if(!enabled){active.store(false,std::memory_order_release);return false;}
    currentFrame.store(frame,std::memory_order_release);
    return !active.exchange(true,std::memory_order_acq_rel);
}
template<class Forward,class Station,class Flat>
inline uintptr_t flatWorldStorePostForward(uintptr_t bucket,uintptr_t mask,uintptr_t output,
    uintptr_t lod,uintptr_t record,bool stationGathering,bool flatActive,
    Forward&& forward,Station&& station,Flat&& flat) noexcept {
    const uintptr_t result=forward(bucket,mask,output,lod,record);
    // Read the store's raw post-forward admission before the station's
    // optional culling can change the mask or admitted byte.
    if(flatActive)flat(bucket,record);
    if(stationGathering)station(bucket,record);
    return result;
}
template<class Forward,class Station,class Flat>
inline uintptr_t flatWorldStoreStampedPostForward(uintptr_t bucket,uintptr_t mask,uintptr_t output,
    uintptr_t lod,uintptr_t record,bool stationGathering,const std::atomic<bool>& active,
    const std::atomic<uint32_t>& currentFrame,Forward&& forward,Station&& station,Flat&& flat) noexcept {
    const uint32_t frame=currentFrame.load(std::memory_order_acquire);
    const bool enabled=active.load(std::memory_order_acquire);
    const bool consistent=frame==currentFrame.load(std::memory_order_acquire);
    return flatWorldStorePostForward(bucket,mask,output,lod,record,stationGathering,enabled&&consistent,forward,station,
        [&](uintptr_t b,uintptr_t r) noexcept {flat(b,r,frame);});
}
namespace flatworldstore {
constexpr uint8_t poseBit=1u<<0, storeBit=1u<<1, rawBit=1u<<2, radiusBit=1u<<3;
constexpr uint8_t maskBit=1u<<4, admittedBit=1u<<5, nodeBit=1u<<6, nameBit=1u<<7;

inline bool read(void* dst, uintptr_t src, size_t n) noexcept {
    if(src<0x10000 || n>UINTPTR_MAX-src) return false;
    __try { std::memcpy(dst,reinterpret_cast<const void*>(src),n);return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> inline bool value(T* dst,uintptr_t src) noexcept {return read(dst,src,sizeof(T));}
inline bool plus(uintptr_t base,uintptr_t offset,uintptr_t* out) noexcept {
    if(base>UINTPTR_MAX-offset)return false;
    *out=base+offset;return true;
}
template<class T> inline bool field(T* dst,uintptr_t base,uintptr_t offset) noexcept {
    uintptr_t address=0;return plus(base,offset,&address)&&value(dst,address);
}
inline bool finiteRadius(float r) noexcept {return std::isfinite(r)&&r>0.0f;}

inline bool pose(uintptr_t rec,float out[3]) noexcept {
    uintptr_t p=0;
    if(!field(&p,rec,0x290)||!p)return false;
    float q[4]{},t[4]{},c[4]{},world[4]{};
    if(!field(&q,p,0x10)||!field(&t,p,0x20)||!field(&c,p,0x80))return false;
    sealed::builderCentre(q,t,c,world);
    for(unsigned i=0;i<3;++i)out[i]=world[i];
    return sealed::finite3(out);
}
inline bool sphere(uintptr_t rec,uintptr_t node,float out[3],float* radius) noexcept {
    uintptr_t block=0,p=0,m=0;
    float L[3]{},A[15]{},B[16]{},r=0;
    if(!node||!field(&block,node,0x08)||!block||!field(&L,block,0x20)||
       !field(&r,block,0x2C)||!field(&p,rec,0x190)||!p||!read(A,p,sizeof(A))||
       !field(&m,p,0x50)||!m||!read(B,m,sizeof(B))||!finiteRadius(r)||
       !sealed::storeWorldSphere(L,A,B,out))return false;
    *radius=r;return true;
}
inline bool name(uintptr_t node,uint64_t* validator,char out[49]) noexcept {
    uintptr_t text=0;uint8_t first[16]{},bytes[sealed::kNameBytes]{};
    if(!node||!value(&text,node)||!text||!read(first,text,sizeof(first)))return false;
    *validator=sealed::nodeValidator(static_cast<uint64_t>(text),first);
    if(!read(bytes,text,sizeof(bytes)))return false;
    // The station's textName accepts ASCII or UTF-16LE in the same allowed
    // alphabet, 4..48 chars and NUL inside 96 bytes. Parse into fixed storage.
    for(unsigned wide=0;wide<2;++wide) {
        char parsed[sealed::kNameMax+1]{};unsigned length=0;
        const unsigned step=wide?2u:1u;
        bool valid=false;
        for(unsigned i=0;i+step<=sizeof(bytes);i+=step) {
            const uint8_t c=bytes[i];
            if(wide&&bytes[i+1]!=0)break;
            if(!c){valid=length>=4&&length<=sealed::kNameMax;break;}
            if(!sealed::nameChar(c)||length>=sealed::kNameMax)break;
            parsed[length++]=static_cast<char>(c);
        }
        if(valid){std::snprintf(out,49,"%s",parsed);return true;}
    }
    return false;
}
} // namespace flatworldstore

// 0: observation can be submitted (including source=none with partial
// alternatives). 10: no safe field could be read; report one terminal miss.
inline uint8_t flatWorldStoreRead(uintptr_t bucket,uintptr_t record,uint32_t frame,
                                  FlatWorldWitnessInput* out) noexcept {
    if(!out||!record)return 10;
    FlatWorldWitnessInput s{};s.record=record;s.frame=frame;s.source=4;s.bucket=bucket;
    s.ownerReadValid=flatworldstore::field(&s.owner,bucket,0x10); // existing job-path bucket owner, optional
    if(flatworldstore::field(&s.node,record,0x18)&&s.node)s.validMask|=flatworldstore::nodeBit;
    if(flatworldstore::field(&s.viewMask,record,0x208))s.validMask|=flatworldstore::maskBit;
    if(flatworldstore::field(&s.admitted,record,0x234))s.validMask|=flatworldstore::admittedBit;
    if(flatworldstore::pose(record,s.poseCenter))s.validMask|=flatworldstore::poseBit;
    float storeRadius=0;
    if(flatworldstore::sphere(record,s.node,s.storeCenter,&storeRadius))s.validMask|=flatworldstore::storeBit;
    s.sourceRadius[1]=storeRadius;
    float rawRadius=0;
    if(flatworldstore::field(&s.rawCenter,record,0x240)&&
       flatworldstore::field(&rawRadius,record,0x280)&&sealed::rawCentreUsable(s.rawCenter,rawRadius))
        s.validMask|=flatworldstore::rawBit;
    s.sourceRadius[2]=rawRadius;
    // Store and raw bits already include their own radius validation. A pose
    // center can be readable while its independent primary radius is invalid.
    s.sourceValidMask=s.validMask&(flatworldstore::storeBit|flatworldstore::rawBit);
    // Mirror worldOfRecord's non-producer priority, including its early
    // return when the pose exists but its radius is invalid.
    if(s.validMask&flatworldstore::poseBit) {
        float poseRadius=0;
        if(flatworldstore::field(&poseRadius,record,0x280)&&flatworldstore::finiteRadius(poseRadius)) {
            s.sourceValidMask|=flatworldstore::poseBit;
            s.source=1;s.radius=poseRadius;std::memcpy(s.center,s.poseCenter,sizeof(s.center));
        }
        s.sourceRadius[0]=poseRadius;
    } else if(s.validMask&flatworldstore::storeBit) {
        s.source=2;s.radius=storeRadius;std::memcpy(s.center,s.storeCenter,sizeof(s.center));
    } else if(s.validMask&flatworldstore::rawBit) {
        s.source=3;s.radius=rawRadius;std::memcpy(s.center,s.rawCenter,sizeof(s.center));
    }
    if(s.source!=4)s.validMask|=flatworldstore::radiusBit;
    if(s.node&&flatworldstore::name(s.node,&s.nodeValidator,s.name)) {
        s.validMask|=flatworldstore::nameBit;
        s.nameClass=static_cast<uint8_t>(sealed::classifyName(true,s.name));
    } else s.nameClass=static_cast<uint8_t>(sealed::NameClass::kUnnamed);
    if(!s.validMask)return 10;
    *out=s;return 0;
}
} // namespace edvr

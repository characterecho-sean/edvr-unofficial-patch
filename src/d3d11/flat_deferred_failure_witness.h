#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "flat_deferred_camera.h"

namespace edvr {
// Passive same-call data. Pointers are read only by the admitted failure report;
// copied=false never permits reading original, including a zero-filled scratch.
struct FlatDeferredFailureWitness {
    uint64_t frame=0,calls=0;uint32_t q=0;const void* hdr=nullptr;const void* depth=nullptr;
    uint64_t vs=0,ps=0;float phaseX=0,phaseY=0;uint64_t applied=0;
    FlatDeferredCameraMatch relation=FlatDeferredCameraMatch::Unavailable;
    bool contextAvailable=false,range=false,tracked=false,shadow=false,mapped=false,pending=false,current=false,copied=false;
    const void* buffer=nullptr;uint32_t first=0,count=0,width=0;
    uint64_t generation=0,mutation=0,writeGeneration=0,bank=0,writeFrame=0;uint32_t writeQ=0;
    const unsigned char* original=nullptr;
    const void* donorColor=nullptr;const void* donorB1=nullptr;
    uint64_t donorWriteFrame=0,donorHash=0;uint32_t donorWriteQ=0,donorFirst=0,donorLast=0;
    const unsigned char* donorCamera=nullptr;
};
// Existing ownership-report budget: first failure and at most once per 5s.
inline bool flatDeferredFailureWitnessDue(uint64_t calls,uint64_t now,uint64_t last) {
    return calls==1 || now-last>=5000;
}
inline void flatDeferredFailureHex(const unsigned char* bytes,unsigned count,char* text) {
    static constexpr char digits[]="0123456789ABCDEF";
    for(unsigned i=0;i<count;++i){text[i*2]=digits[bytes[i]>>4];text[i*2+1]=digits[bytes[i]&15];}
    text[count*2]=0;
}
// Fixed output; no heap, clock, D3D queries, mutation, or float formatting.
// Phase bits are the chosen pre-failure float values, not a post-fail sample.
inline bool flatFormatDeferredFailureWitness(const FlatDeferredFailureWitness& w,char (&lines)[4][1000]) {
    char original[225]="unavailable",donor[193]="unavailable";
    const bool haveOriginal=w.contextAvailable&&w.copied&&w.original;
    const bool haveDonor=w.donorCamera!=nullptr;
    if(haveOriginal)flatDeferredFailureHex(w.original,112,original);
    if(haveDonor)flatDeferredFailureHex(w.donorCamera,96,donor);
    uint32_t px=0,py=0;std::memcpy(&px,&w.phaseX,4);std::memcpy(&py,&w.phaseY,4);
    const auto ull=[](uint64_t x){return static_cast<unsigned long long>(x);};
    const int n0=std::snprintf(lines[0],sizeof(lines[0]),
        "flat deferred failure witness: v=1 frame=%llu q=%u H=%p depth=%p VS=%016llX PS=%016llX phase-bits=%08X/%08X applied=%llu relation=%s same-call=%u calls=%llu budget=first-or-5000ms donor-color=%p donor-b1=%p donor-first=%u donor-last=%u",
        ull(w.frame),w.q,w.hdr,w.depth,ull(w.vs),ull(w.ps),px,py,ull(w.applied),flatDeferredCameraMatchName(w.relation),
        w.contextAvailable?1u:0u,ull(w.calls),w.donorColor,w.donorB1,w.donorFirst,w.donorLast);
    const int n1=std::snprintf(lines[1],sizeof(lines[1]),
        "flat deferred failure original stamp: v=1 frame=%llu q=%u H=%p CB2=%p first=%u count=%u width=%u generation=%llu mutation=%llu write-generation=%llu bank=%llu write-frame=%llu write-q=%u range=%u tracked=%u shadow=%u mapped=%u pending=%u current=%u copied=%u available=%u",
        ull(w.frame),w.q,w.hdr,w.buffer,w.first,w.count,w.width,ull(w.generation),ull(w.mutation),ull(w.writeGeneration),ull(w.bank),ull(w.writeFrame),w.writeQ,
        w.range?1u:0u,w.tracked?1u:0u,w.shadow?1u:0u,w.mapped?1u:0u,w.pending?1u:0u,w.current?1u:0u,w.copied?1u:0u,haveOriginal?1u:0u);
    const int n2=std::snprintf(lines[2],sizeof(lines[2]),
        "flat deferred failure original bytes: v=1 frame=%llu q=%u H=%p CB2=%p row=10 rows=7 bytes=%u hex=%s",
        ull(w.frame),w.q,w.hdr,w.buffer,haveOriginal?112u:0u,original);
    const int n3=std::snprintf(lines[3],sizeof(lines[3]),
        "flat deferred failure donor bytes: v=1 frame=%llu q=%u H=%p b1=%p write-frame=%llu write-q=%u camera-hash=%016llX bytes=%u hex=%s",
        ull(w.frame),w.q,w.hdr,w.donorB1,ull(w.donorWriteFrame),w.donorWriteQ,ull(w.donorHash),haveDonor?96u:0u,donor);
    return n0>=0&&n0<1000&&n1>=0&&n1<1000&&n2>=0&&n2<1000&&n3>=0&&n3<1000;
}
}

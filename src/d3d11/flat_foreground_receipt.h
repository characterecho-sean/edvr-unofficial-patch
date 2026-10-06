#pragma once
#include <cstdint>

// Bounded, value-only evidence from the original draw. Zero is a valid state;
// valid distinguishes a captured zero from a failure before observation.
struct EdvrFlatForegroundStateReceipt {
    uint32_t valid=0,boundColors=0,independentBlend=0,alphaToCoverage=0,sampleMask=0;
    uint32_t depthEnable=0,depthWriteMask=0,depthFunc=0;
    uint32_t stencilEnable=0,stencilReadMask=0,stencilWriteMask=0,stencilRef=0;
    struct Face {uint32_t func=0,fail=0,depthFail=0,pass=0;} front{},back{};
    uint32_t dsvFlagsValid=0,dsvFlags=0;
    uint32_t foreign=0,hdr=0,worldNamed=0,sameWorld=0,namedWorldQ=0;
    struct Slot {
        uint32_t componentMask=0,effectiveWriteMask=0,blendEnable=0;
        uint32_t viewFormatValid=0,viewFormat=0;
        uint32_t src=0,dst=0,op=0,srcAlpha=0,dstAlpha=0,opAlpha=0;
    } slot[6]{};
};

struct EdvrFlatForegroundBudgetReceipt {
    uint32_t valid=0,requestedBytes=0,recordLimitHit=0,byteLimitHit=0;
    uint32_t records=0,bytes=0,invalid=0,pending=0,current=0,prior=0,older=0;
    uint32_t reclaimedRecords=0,reclaimedBytes=0;
    uint32_t currentDraws=0,previousDraws=0;
    uint32_t beforeWorldCurrent=0,beforeWorldPrevious=0;
    uint32_t knownMutations=0,unknownMutations=0;
};

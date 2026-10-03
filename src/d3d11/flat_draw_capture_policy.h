#pragma once
// Pure F10 admission policy. No D3D state reads or allocation outside an arm.
#include <cstdint>

namespace edvr {
struct FlatDrawAdmission {
    static constexpr unsigned kQuota=256, kStride=64, kInitial=8;
    unsigned sceneEligible=0, motionEligible=0, sceneRecorded=0, motionRecorded=0;
    unsigned sceneSkipped=0, motionSkipped=0, sceneQuotaSkipped=0, motionQuotaSkipped=0;
    unsigned poolCandidates[2]{},poolSelected[2]{},poolSkipped[2]{},poolQuotaSkipped[2]{};

    bool consider(bool motion,uint64_t frame) {
        unsigned& eligible=motion?motionEligible:sceneEligible;
        unsigned& recorded=motion?motionRecorded:sceneRecorded;
        unsigned& skipped=motion?motionSkipped:sceneSkipped;
        unsigned& quotaSkipped=motion?motionQuotaSkipped:sceneQuotaSkipped;
        const unsigned ordinal=++eligible;
        const unsigned phase=(frame&1)?0:kStride/2;
        if(ordinal>kInitial&&(ordinal+phase)%kStride!=0){++skipped;return false;}
        if(recorded>=kQuota){++quotaSkipped;return false;}
        ++recorded;return true;
    }
    bool considerPool(unsigned family) {
        const unsigned ordinal=++poolCandidates[family];
        if(ordinal!=1&&ordinal%32!=0){++poolSkipped[family];return false;}
        if(poolSelected[family]>=8){++poolQuotaSkipped[family];return false;}
        ++poolSelected[family];return true;
    }
};
}

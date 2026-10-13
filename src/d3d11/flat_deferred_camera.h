#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <initializer_list>
#include "flat_projection_math.h"

namespace edvr {
// The one physically certified deferred HDR pair and its CB2 projection span.
// These identify a recipe; the caller still proves target, donor and current bytes.
constexpr uint64_t kFlatDeferredHdrVs = 0x7E38A6AA1269C901ull;
constexpr uint64_t kFlatDeferredHdrPs = 0x7CECABDE34FFBE9Eull;
constexpr uint32_t kFlatDeferredHdrFormat = 26;
constexpr uint32_t kFlatDeferredCameraSlot = 2;
constexpr uint32_t kFlatDeferredCameraFirstRow = 10;
constexpr uint32_t kFlatDeferredCameraRows = 7;
constexpr uint32_t kFlatDeferredInverseFirstRow = 14;
constexpr uint32_t kFlatDeferredInverseOffsetRows =
    kFlatDeferredInverseFirstRow - kFlatDeferredCameraFirstRow;
constexpr uint32_t kFlatDeferredCameraBytes = kFlatDeferredCameraRows * 16;
constexpr uint32_t kFlatDeferredCameraMinWidth =
    (kFlatDeferredCameraFirstRow + kFlatDeferredCameraRows) * 16;
enum class FlatDeferredPhaseDomain { Outside, KnownHdr, SceneDepthExtent };
// Ownership is independent of camera/projection proof: broken evidence on the
// selected target must still fail closed. A named HDR excludes other targets,
// even when they share its depth. Before HDR is named, depth and extent jointly
// protect the pending scene candidate (including the LDR continuation route).
inline FlatDeferredPhaseDomain flatDeferredPhaseDomain(const void* color,const void* depth,
        unsigned width,unsigned height,const void* phaseHdr,const void* phaseDepth,
        const void* namedDepth,unsigned phaseWidth,unsigned phaseHeight) {
    if(phaseHdr)return color==phaseHdr?FlatDeferredPhaseDomain::KnownHdr:FlatDeferredPhaseDomain::Outside;
    if(depth && (depth==phaseDepth || depth==namedDepth) && phaseWidth && phaseHeight &&
       width==phaseWidth && height==phaseHeight)return FlatDeferredPhaseDomain::SceneDepthExtent;
    return FlatDeferredPhaseDomain::Outside;
}
inline bool flatDeferredVerifyInPhase(bool nonzero,FlatDeferredPhaseDomain domain) {
    // Zero phase can discover a new scene without inheriting the previous
    // render extent. Its original donor/matrix proof remains mandatory.
    return !nonzero || domain!=FlatDeferredPhaseDomain::Outside;
}
enum class FlatDeferredCameraMatch { Match, Unavailable, Forward, Translation, Inverse };
inline const char* flatDeferredCameraMatchName(FlatDeferredCameraMatch match) {
    switch(match) {
    case FlatDeferredCameraMatch::Match:return "match";
    case FlatDeferredCameraMatch::Unavailable:return "unavailable";
    case FlatDeferredCameraMatch::Forward:return "forward-mismatch";
    case FlatDeferredCameraMatch::Translation:return "translation-mismatch";
    case FlatDeferredCameraMatch::Inverse:return "inverse-mismatch";
    }
    return "unavailable";
}
// gamma(n) bounds n rounded float32 operations. No scene-tuned tolerance.
inline double flatDeferredFloatBound(unsigned operations,double magnitude) {
    const double u=std::numeric_limits<float>::epsilon()/2.0;
    return operations*u/(1.0-operations*u)*magnitude +
        operations*static_cast<double>(std::numeric_limits<float>::denorm_min());
}
inline FlatDeferredCameraMatch flatMatchDeferredCamera(const unsigned char* cameraBytes,
                                                       const unsigned char* cb2Bytes) {
    if(!cameraBytes || !cb2Bytes)return FlatDeferredCameraMatch::Unavailable;
    float camera[6][4],rows[kFlatDeferredCameraRows][4];
    std::memcpy(camera,cameraBytes,sizeof(camera));std::memcpy(rows,cb2Bytes,sizeof(rows));
    for(const auto& r:camera)for(float x:r)if(!std::isfinite(x))return FlatDeferredCameraMatch::Unavailable;
    for(const auto& r:rows)for(float x:r)if(!std::isfinite(x))return FlatDeferredCameraMatch::Unavailable;
    if(camera[0][2]!=0 || camera[1][2]!=0 || camera[2][2]!=0 ||
       camera[3][0]!=0 || camera[3][1]!=0 || camera[3][3]!=0 || camera[3][2]<=0 ||
       camera[4][0]!=camera[0][3] || camera[4][1]!=camera[1][3] || camera[4][2]!=camera[2][3])
        return FlatDeferredCameraMatch::Unavailable;
    // Each forward encoding may incur a multiply/add plus jitter multiply/add.
    // gamma(8) covers both encodings; translation adds three products and three
    // additions to the independently rounded stored translation. Near stays exact.
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<3;++c) {
        const double a=rows[r][c],b=camera[c][r];
        if(std::abs(a-b)>flatDeferredFloatBound(8,std::abs(a)+std::abs(b)))return FlatDeferredCameraMatch::Forward;
    }
    if(rows[2][3]!=camera[3][2])return FlatDeferredCameraMatch::Forward;
    for(unsigned r:{0u,1u,3u}) {
        double sum=rows[r][3],magnitude=std::abs(sum);
        for(unsigned c=0;c<3;++c){const double term=double(camera[c][r])*camera[5][c];sum+=term;magnitude+=std::abs(term);}
        if(std::abs(sum)>flatDeferredFloatBound(8,magnitude))return FlatDeferredCameraMatch::Translation;
    }
    // The exact VS consumes (u,v,1) through b2[14..16].xyw. A maps world rays
    // into clip x/y/w; their product must implement (2u-1,1-2v,1).
    const unsigned clipRows[]={0,1,3},rayColumns[]={0,1,3};
    const double expected[3][3]={{2,0,-1},{0,-2,1},{0,0,1}};
    double residual[3][3]{},bounds[3][3]{};
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c) {
        double sum=0,magnitude=std::abs(expected[r][c]);
        for(unsigned i=0;i<3;++i){const double term=double(rows[clipRows[r]][i])*rows[kFlatDeferredInverseOffsetRows+i][rayColumns[c]];sum+=term;magnitude+=std::abs(term);}
        // Three products/two sums plus independently rounded forward/inverse
        // coefficients and their jitter updates are enclosed by gamma(12).
        residual[r][c]=sum-expected[r][c];bounds[r][c]=flatDeferredFloatBound(12,magnitude);
        if(std::abs(residual[r][c])>bounds[r][c])return FlatDeferredCameraMatch::Inverse;
    }
    // T is the exact UV-to-clip matrix above. Certify ||(AB-T)T^-1||inf < 1,
    // including arithmetic uncertainty. The Neumann-series criterion proves AB
    // (and therefore A) nonsingular; large cancellation cannot certify ownership.
    const double inverseT[3][3]={{.5,0,.5},{0,-.5,.5},{0,0,1}};
    for(unsigned r=0;r<3;++r){double norm=0;
        for(unsigned c=0;c<3;++c){double error=0,bound=0;
            for(unsigned i=0;i<3;++i){error+=residual[r][i]*inverseT[i][c];bound+=bounds[r][i]*std::abs(inverseT[i][c]);}
            norm+=std::abs(error)+bound;
        }
        if(!(norm<1))return FlatDeferredCameraMatch::Unavailable;
    }
    return FlatDeferredCameraMatch::Match;
}
inline bool flatDeferredExecutionCoherent(bool qualified,FlatDeferredCameraMatch original,
                                          bool prepared) {
    return qualified && (original==FlatDeferredCameraMatch::Match || prepared);
}
// Certify the measured omission, never an arbitrary inverse mismatch. Both
// dephased source and corrected result retain the nonsingularity certificate.
inline bool flatRepairDeferredMissingPhase(const unsigned char* cameraBytes,
        const unsigned char* original, const FlatProjectionJitter& jitter,
        unsigned char* corrected) {
    if(!corrected || !flat_projection_detail::finite(jitter) ||
       (jitter.ndcX==0 && jitter.ndcY==0) ||
       flatMatchDeferredCamera(cameraBytes,original)!=FlatDeferredCameraMatch::Inverse)return false;
    float camera[6][4],rows[kFlatDeferredCameraRows][4];
    std::memcpy(camera,cameraBytes,sizeof(camera));std::memcpy(rows,original,sizeof(rows));
    FlatProjectionJitter undo=jitter;
    undo.ndcX=-undo.ndcX;undo.ndcY=-undo.ndcY;undo.uvX=-undo.uvX;undo.uvY=-undo.uvY;
    float columns[4][4],forward[4][4];
    std::memcpy(columns,camera,sizeof(columns));std::memcpy(forward,rows,sizeof(forward));
    if(!flatJitterForwardColumns(columns,undo) || !flatJitterForwardDp4(forward,undo))return false;
    std::memcpy(camera,columns,sizeof(columns));std::memcpy(rows,forward,sizeof(forward));
    if(flatMatchDeferredCamera(reinterpret_cast<unsigned char*>(camera),
                               reinterpret_cast<unsigned char*>(rows))!=FlatDeferredCameraMatch::Match)return false;
    std::memcpy(rows,original,sizeof(rows));
    float rays[3][4];std::memcpy(rays,rows+kFlatDeferredInverseOffsetRows,sizeof(rays));
    if(!flatJitterInverseUvRay(rays,jitter))return false;
    std::memcpy(rows+kFlatDeferredInverseOffsetRows,rays,sizeof(rays));
    if(flatMatchDeferredCamera(cameraBytes,reinterpret_cast<unsigned char*>(rows))!=FlatDeferredCameraMatch::Match)return false;
    std::memcpy(corrected,rows,sizeof(rows));return true;
}

}

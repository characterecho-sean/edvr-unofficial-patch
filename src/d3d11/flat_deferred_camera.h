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
// The clip x/y coefficients are sums of products of a rotation row and the frustum scale,
// so a coefficient that cancels to a small result still carries rounding of the scale's size.
// Rotation rows have unit length: by Cauchy-Schwarz the terms of any coefficient in clip row r
// sum to at most the length of that row's xyz, which is the frustum scale (the horizontal scale
// for x, the vertical for y). Only a camera and CB2 finalized separately need this allowance.
inline double flatDeferredFrustumScale(const float rows[][4],unsigned r) {
    if(r>=2)return 0;
    return std::sqrt(double(rows[r][0])*rows[r][0]+double(rows[r][1])*rows[r][1]+double(rows[r][2])*rows[r][2]);
}
// frustumScaleForward: the camera was finalized from the phased frustum and the CB2 from an
// unphased one (the whole-camera omission), so the forward comparison carries that allowance.
inline FlatDeferredCameraMatch flatMatchDeferredCamera(const unsigned char* cameraBytes,
                                                       const unsigned char* cb2Bytes,
                                                       bool frustumScaleForward=false) {
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
        const double scale=frustumScaleForward?flatDeferredFrustumScale(rows,r):0;
        if(std::abs(a-b)>flatDeferredFloatBound(8,std::abs(a)+std::abs(b)+2*scale))return FlatDeferredCameraMatch::Forward;
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
// g = stored - phase*w reverses one float multiply and one float add.
// Cancellation leaves a tiny stored result, so gamma(2) is charged on
// |stored| + 2*|phase*w|, which keeps the removed term in the bound.
inline double flatDeferredReverseDelta(double stored,double phase,double homogeneous) {
    if(phase==0)return 0;
    return flatDeferredFloatBound(2,std::abs(stored)+2*std::abs(phase)*std::abs(homogeneous));
}
// Removing the requested phase must leave this inverse. The bound carries the
// reversed madd's pre-cancellation magnitude; a float undo measured after
// cancellation rejects a real omission. The corrected upload is certified
// separately by the strict matcher, including its nonsingularity test.
// The camera always carries the requested phase. The CB2 forward rows carry it
// when only the inverse rays omit it, and carry none when the whole camera
// omits it; then the rows are compared raw, charge no reversal, and, being
// finalized apart from the camera, carry the frustum-scale allowance.
inline bool flatDeferredDephasedOmission(const float camera[6][4],const float rows[7][4],
        double ndcX,double ndcY,bool forwardCarriesPhase=true) {
    const double rowX=forwardCarriesPhase?ndcX:0,rowY=forwardCarriesPhase?ndcY:0;
    double cam[4][4],cb[4][4];
    for(unsigned i=0;i<4;++i)for(unsigned k=0;k<4;++k){cam[i][k]=camera[i][k];cb[k][i]=rows[k][i];}
    for(unsigned i=0;i<4;++i){
        cam[i][0]-=ndcX*cam[i][3];cam[i][1]-=ndcY*cam[i][3];
        cb[0][i]-=rowX*cb[3][i];cb[1][i]-=rowY*cb[3][i];
    }
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<3;++c){
        double delta=0;
        if(r==0)delta=flatDeferredReverseDelta(rows[0][c],rowX,rows[3][c])+flatDeferredReverseDelta(camera[c][0],ndcX,camera[c][3]);
        if(r==1)delta=flatDeferredReverseDelta(rows[1][c],rowY,rows[3][c])+flatDeferredReverseDelta(camera[c][1],ndcY,camera[c][3]);
        const double scale=forwardCarriesPhase?0:flatDeferredFrustumScale(rows,r);
        const double magnitude=std::abs(cb[r][c])+std::abs(cam[c][r])+2*scale;
        const double bound=delta+flatDeferredFloatBound(8,magnitude);
        if(!(std::abs(cb[r][c]-cam[c][r])<=bound))return false;
    }
    if(rows[2][3]!=camera[3][2])return false;
    const double world[]={camera[5][0],camera[5][1],camera[5][2]};
    for(unsigned r:{0u,1u,3u}){
        double sum=cb[r][3],magnitude=std::abs(sum),delta=0;
        if(r==0)delta+=flatDeferredReverseDelta(rows[0][3],rowX,rows[3][3]);
        if(r==1)delta+=flatDeferredReverseDelta(rows[1][3],rowY,rows[3][3]);
        for(unsigned c=0;c<3;++c){
            const double term=cam[c][r]*world[c];sum+=term;magnitude+=std::abs(term);
            if(r==0)delta+=flatDeferredReverseDelta(camera[c][0],ndcX,camera[c][3])*std::abs(world[c]);
            if(r==1)delta+=flatDeferredReverseDelta(camera[c][1],ndcY,camera[c][3])*std::abs(world[c]);
        }
        if(!(std::abs(sum)<=flatDeferredFloatBound(8,magnitude)+delta))return false;
    }
    const unsigned clipRows[]={0,1,3},rayColumns[]={0,1,3};
    const double expected[3][3]={{2,0,-1},{0,-2,1},{0,0,1}};
    double residual[3][3]{},bounds[3][3]{};
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c){
        double sum=0,magnitude=std::abs(expected[r][c]),delta=0;
        const unsigned clip=clipRows[r];
        for(unsigned i=0;i<3;++i){
            const double ray=rows[kFlatDeferredInverseOffsetRows+i][rayColumns[c]];
            const double term=cb[clip][i]*ray;sum+=term;magnitude+=std::abs(term);
            if(clip==0)delta+=flatDeferredReverseDelta(rows[0][i],rowX,rows[3][i])*std::abs(ray);
            if(clip==1)delta+=flatDeferredReverseDelta(rows[1][i],rowY,rows[3][i])*std::abs(ray);
        }
        residual[r][c]=sum-expected[r][c];bounds[r][c]=flatDeferredFloatBound(12,magnitude)+delta;
        if(!(std::abs(residual[r][c])<=bounds[r][c]))return false;
    }
    const double inverseT[3][3]={{.5,0,.5},{0,-.5,.5},{0,0,1}};
    for(unsigned r=0;r<3;++r){double norm=0;
        for(unsigned c=0;c<3;++c){double error=0,bound=0;
            for(unsigned i=0;i<3;++i){error+=residual[r][i]*inverseT[i][c];bound+=bounds[r][i]*std::abs(inverseT[i][c]);}
            norm+=std::abs(error)+bound;
        }
        if(!(norm<1))return false;
    }
    return true;
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
    if(!flatDeferredDephasedOmission(camera,rows,jitter.ndcX,jitter.ndcY))return false;
    float rays[3][4];std::memcpy(rays,rows+kFlatDeferredInverseOffsetRows,sizeof(rays));
    if(!flatJitterInverseUvRay(rays,jitter))return false;
    std::memcpy(rows+kFlatDeferredInverseOffsetRows,rays,sizeof(rays));
    if(flatMatchDeferredCamera(cameraBytes,reinterpret_cast<unsigned char*>(rows))!=FlatDeferredCameraMatch::Match)return false;
    std::memcpy(corrected,rows,sizeof(rows));return true;
}
// The pass can also be fed a CB2 with no phase in any row (supercruise and
// planetary flight) while the donor camera carries it: forward and inverse
// rows agree with each other but equal the donor with the requested phase
// removed. Certify exactly that, then rebuild the span with the layouts the
// projection patches use (forward dp4 on rows 0..3, inverse rays on rows 4..6)
// so the bytes equal the uploaded clone. The result passes the strict matcher
// with the same frustum-scale allowance on the forward rows.
inline bool flatRepairDeferredWholeOmission(const unsigned char* cameraBytes,
        const unsigned char* original, const FlatProjectionJitter& jitter,
        unsigned char* corrected) {
    if(!corrected || !flat_projection_detail::finite(jitter) ||
       (jitter.ndcX==0 && jitter.ndcY==0) ||
       flatMatchDeferredCamera(cameraBytes,original)!=FlatDeferredCameraMatch::Forward)return false;
    float camera[6][4],rows[kFlatDeferredCameraRows][4];
    std::memcpy(camera,cameraBytes,sizeof(camera));std::memcpy(rows,original,sizeof(rows));
    if(!flatDeferredDephasedOmission(camera,rows,jitter.ndcX,jitter.ndcY,false))return false;
    float forward[4][4],rays[3][4];
    std::memcpy(forward,rows,sizeof(forward));std::memcpy(rays,rows+kFlatDeferredInverseOffsetRows,sizeof(rays));
    if(!flatJitterForwardDp4(forward,jitter) || !flatJitterInverseUvRay(rays,jitter))return false;
    std::memcpy(rows,forward,sizeof(forward));std::memcpy(rows+kFlatDeferredInverseOffsetRows,rays,sizeof(rays));
    if(flatMatchDeferredCamera(cameraBytes,reinterpret_cast<unsigned char*>(rows),true)!=FlatDeferredCameraMatch::Match)return false;
    std::memcpy(corrected,rows,sizeof(rows));return true;
}

}

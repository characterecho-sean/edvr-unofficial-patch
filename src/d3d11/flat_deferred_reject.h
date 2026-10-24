#pragma once
// Log-only: which check refuses the whole-camera omission repair, and by how much.
//
// flatRepairDeferredWholeOmission (flat_deferred_camera.h) answers yes or no. A flight on 2026-10-24
// (v0.21.0, 2560x1440 DLAA) showed it accepting four of the eight jitter phases and refusing the other
// four with no line saying which comparison failed. flatExplainWholeOmission walks the same checks in
// the same order and names the first one that fails, with its residual and bound. It is called only
// from a frame the repair has already refused; it never decides anything, and the repair is untouched.
// tools/flat_temporal_test/flat_deferred_reject_tests.h pins that the two agree on every capture and
// on every negative the repair's own rig uses.
#include <cmath>
#include <cstring>
#include "flat_deferred_camera.h"

namespace edvr {
struct FlatDeferredReject {
    const char* stage = "accepted";   // gate, match, dephase, forward-patch, inverse-patch, rematch, accepted
    const char* check = "";           // the comparison inside the stage that failed
    unsigned row = 0, col = 0;        // which coefficient, where the check has one
    double residual = 0, bound = 0;   // the failing comparison: residual > bound refuses
};

namespace flat_deferred_reject_detail {
inline void fail(FlatDeferredReject& why, const char* stage, const char* check, unsigned row, unsigned col,
                 double residual, double bound) {
    why.stage = stage; why.check = check; why.row = row; why.col = col; why.residual = residual; why.bound = bound;
}
}

// flatMatchDeferredCamera, naming the check that decided the answer. Same checks, same order, same bounds.
inline FlatDeferredCameraMatch flatExplainDeferredMatch(const unsigned char* cameraBytes, const unsigned char* cb2Bytes,
                                                        bool frustumScaleForward, const char* stage, FlatDeferredReject& why) {
    using flat_deferred_reject_detail::fail;
    if(!cameraBytes || !cb2Bytes) { fail(why, stage, "no-bytes", 0, 0, 0, 0); return FlatDeferredCameraMatch::Unavailable; }
    float camera[6][4], rows[kFlatDeferredCameraRows][4];
    std::memcpy(camera, cameraBytes, sizeof(camera)); std::memcpy(rows, cb2Bytes, sizeof(rows));
    for(const auto& r : camera) for(float x : r) if(!std::isfinite(x)) { fail(why, stage, "camera-not-finite", 0, 0, 0, 0); return FlatDeferredCameraMatch::Unavailable; }
    for(const auto& r : rows) for(float x : r) if(!std::isfinite(x)) { fail(why, stage, "cb2-not-finite", 0, 0, 0, 0); return FlatDeferredCameraMatch::Unavailable; }
    if(camera[0][2]!=0 || camera[1][2]!=0 || camera[2][2]!=0 ||
       camera[3][0]!=0 || camera[3][1]!=0 || camera[3][3]!=0 || camera[3][2]<=0 ||
       camera[4][0]!=camera[0][3] || camera[4][1]!=camera[1][3] || camera[4][2]!=camera[2][3]) {
        fail(why, stage, "camera-shape", 0, 0, 0, 0); return FlatDeferredCameraMatch::Unavailable;
    }
    for(unsigned r=0;r<4;++r) for(unsigned c=0;c<3;++c) {
        const double a=rows[r][c], b=camera[c][r];
        const double scale=frustumScaleForward?flatDeferredFrustumScale(rows,r):0;
        const double bound=flatDeferredFloatBound(8,std::abs(a)+std::abs(b)+2*scale);
        if(std::abs(a-b)>bound) { fail(why, stage, "forward-coefficient", r, c, std::abs(a-b), bound); return FlatDeferredCameraMatch::Forward; }
    }
    if(rows[2][3]!=camera[3][2]) { fail(why, stage, "near-plane", 2, 3, std::abs(double(rows[2][3])-camera[3][2]), 0); return FlatDeferredCameraMatch::Forward; }
    for(unsigned r:{0u,1u,3u}) {
        double sum=rows[r][3], magnitude=std::abs(sum);
        for(unsigned c=0;c<3;++c) { const double term=double(camera[c][r])*camera[5][c]; sum+=term; magnitude+=std::abs(term); }
        const double allowance=frustumScaleForward&&r<2?
            flatDeferredWorldAllowance(flatDeferredFrustumScale(rows,r),camera[5][0],camera[5][1],camera[5][2]):0;
        const double bound=flatDeferredFloatBound(8,magnitude)+allowance;
        if(std::abs(sum)>bound) { fail(why, stage, "translation", r, 3, std::abs(sum), bound); return FlatDeferredCameraMatch::Translation; }
    }
    const unsigned clipRows[]={0,1,3}, rayColumns[]={0,1,3};
    const double expected[3][3]={{2,0,-1},{0,-2,1},{0,0,1}};
    double residual[3][3]{}, bounds[3][3]{};
    for(unsigned r=0;r<3;++r) for(unsigned c=0;c<3;++c) {
        double sum=0, magnitude=std::abs(expected[r][c]);
        for(unsigned i=0;i<3;++i) { const double term=double(rows[clipRows[r]][i])*rows[kFlatDeferredInverseOffsetRows+i][rayColumns[c]]; sum+=term; magnitude+=std::abs(term); }
        residual[r][c]=sum-expected[r][c]; bounds[r][c]=flatDeferredFloatBound(12,magnitude);
        if(std::abs(residual[r][c])>bounds[r][c]) { fail(why, stage, "inverse", r, c, std::abs(residual[r][c]), bounds[r][c]); return FlatDeferredCameraMatch::Inverse; }
    }
    const double inverseT[3][3]={{.5,0,.5},{0,-.5,.5},{0,0,1}};
    for(unsigned r=0;r<3;++r) {
        double norm=0;
        for(unsigned c=0;c<3;++c) {
            double error=0, bound=0;
            for(unsigned i=0;i<3;++i) { error+=residual[r][i]*inverseT[i][c]; bound+=bounds[r][i]*std::abs(inverseT[i][c]); }
            norm+=std::abs(error)+bound;
        }
        if(!(norm<1)) { fail(why, stage, "nonsingular", r, 0, norm, 1); return FlatDeferredCameraMatch::Unavailable; }
    }
    return FlatDeferredCameraMatch::Match;
}

// flatDeferredDephasedOmission(camera, rows, ndcX, ndcY, forwardCarriesPhase), naming the check that fails.
inline bool flatExplainDephasedOmission(const float camera[6][4], const float rows[7][4], double ndcX, double ndcY,
                                        bool forwardCarriesPhase, FlatDeferredReject& why) {
    using flat_deferred_reject_detail::fail;
    const double rowX=forwardCarriesPhase?ndcX:0, rowY=forwardCarriesPhase?ndcY:0;
    double cam[4][4], cb[4][4];
    for(unsigned i=0;i<4;++i) for(unsigned k=0;k<4;++k) { cam[i][k]=camera[i][k]; cb[k][i]=rows[k][i]; }
    for(unsigned i=0;i<4;++i) {
        cam[i][0]-=ndcX*cam[i][3]; cam[i][1]-=ndcY*cam[i][3];
        cb[0][i]-=rowX*cb[3][i]; cb[1][i]-=rowY*cb[3][i];
    }
    for(unsigned r=0;r<4;++r) for(unsigned c=0;c<3;++c) {
        double delta=0;
        if(r==0) delta=flatDeferredReverseDelta(rows[0][c],rowX,rows[3][c])+flatDeferredReverseDelta(camera[c][0],ndcX,camera[c][3]);
        if(r==1) delta=flatDeferredReverseDelta(rows[1][c],rowY,rows[3][c])+flatDeferredReverseDelta(camera[c][1],ndcY,camera[c][3]);
        const double scale=forwardCarriesPhase?0:flatDeferredFrustumScale(rows,r);
        const double magnitude=std::abs(cb[r][c])+std::abs(cam[c][r])+2*scale;
        const double bound=delta+flatDeferredFloatBound(8,magnitude);
        if(!(std::abs(cb[r][c]-cam[c][r])<=bound)) { fail(why, "dephase", "forward-coefficient", r, c, std::abs(cb[r][c]-cam[c][r]), bound); return false; }
    }
    if(rows[2][3]!=camera[3][2]) { fail(why, "dephase", "near-plane", 2, 3, std::abs(double(rows[2][3])-camera[3][2]), 0); return false; }
    const double world[]={camera[5][0],camera[5][1],camera[5][2]};
    for(unsigned r:{0u,1u,3u}) {
        double sum=cb[r][3], magnitude=std::abs(sum), delta=0;
        if(r==0) delta+=flatDeferredReverseDelta(rows[0][3],rowX,rows[3][3]);
        if(r==1) delta+=flatDeferredReverseDelta(rows[1][3],rowY,rows[3][3]);
        for(unsigned c=0;c<3;++c) {
            const double term=cam[c][r]*world[c]; sum+=term; magnitude+=std::abs(term);
            if(r==0) delta+=flatDeferredReverseDelta(camera[c][0],ndcX,camera[c][3])*std::abs(world[c]);
            if(r==1) delta+=flatDeferredReverseDelta(camera[c][1],ndcY,camera[c][3])*std::abs(world[c]);
        }
        const double allowance=!forwardCarriesPhase&&r<2?
            flatDeferredWorldAllowance(flatDeferredFrustumScale(rows,r),world[0],world[1],world[2]):0;
        const double bound=flatDeferredFloatBound(8,magnitude)+delta+allowance;
        if(!(std::abs(sum)<=bound)) { fail(why, "dephase", "translation", r, 3, std::abs(sum), bound); return false; }
    }
    const unsigned clipRows[]={0,1,3}, rayColumns[]={0,1,3};
    const double expected[3][3]={{2,0,-1},{0,-2,1},{0,0,1}};
    double residual[3][3]{}, bounds[3][3]{};
    for(unsigned r=0;r<3;++r) for(unsigned c=0;c<3;++c) {
        double sum=0, magnitude=std::abs(expected[r][c]), delta=0;
        const unsigned clip=clipRows[r];
        for(unsigned i=0;i<3;++i) {
            const double ray=rows[kFlatDeferredInverseOffsetRows+i][rayColumns[c]];
            const double term=cb[clip][i]*ray; sum+=term; magnitude+=std::abs(term);
            if(clip==0) delta+=flatDeferredReverseDelta(rows[0][i],rowX,rows[3][i])*std::abs(ray);
            if(clip==1) delta+=flatDeferredReverseDelta(rows[1][i],rowY,rows[3][i])*std::abs(ray);
        }
        residual[r][c]=sum-expected[r][c]; bounds[r][c]=flatDeferredFloatBound(12,magnitude)+delta;
        if(!(std::abs(residual[r][c])<=bounds[r][c])) { fail(why, "dephase", "inverse", r, c, std::abs(residual[r][c]), bounds[r][c]); return false; }
    }
    const double inverseT[3][3]={{.5,0,.5},{0,-.5,.5},{0,0,1}};
    for(unsigned r=0;r<3;++r) {
        double norm=0;
        for(unsigned c=0;c<3;++c) {
            double error=0, bound=0;
            for(unsigned i=0;i<3;++i) { error+=residual[r][i]*inverseT[i][c]; bound+=bounds[r][i]*std::abs(inverseT[i][c]); }
            norm+=std::abs(error)+bound;
        }
        if(!(norm<1)) { fail(why, "dephase", "nonsingular", r, 0, norm, 1); return false; }
    }
    return true;
}

// flatRepairDeferredWholeOmission, stage by stage. True exactly when the repair would accept; otherwise
// `why` names the first stage and check that refused.
inline bool flatExplainWholeOmission(const unsigned char* cameraBytes, const unsigned char* original,
                                     const FlatProjectionJitter& jitter, FlatDeferredReject& why) {
    using flat_deferred_reject_detail::fail;
    why = FlatDeferredReject{};
    if(!flat_projection_detail::finite(jitter)) { fail(why, "gate", "jitter-not-finite", 0, 0, 0, 0); return false; }
    if(jitter.ndcX==0 && jitter.ndcY==0) { fail(why, "gate", "zero-phase", 0, 0, 0, 0); return false; }
    const auto first=flatExplainDeferredMatch(cameraBytes, original, false, "match", why);
    if(first==FlatDeferredCameraMatch::Match) { fail(why, "match", "already-matches", 0, 0, 0, 0); return false; }
    if(first!=FlatDeferredCameraMatch::Forward) return false;   // the matcher named the check that decided it
    why = FlatDeferredReject{};
    float camera[6][4], rows[kFlatDeferredCameraRows][4];
    std::memcpy(camera, cameraBytes, sizeof(camera)); std::memcpy(rows, original, sizeof(rows));
    if(!flatExplainDephasedOmission(camera, rows, jitter.ndcX, jitter.ndcY, false, why)) return false;
    float forward[4][4], rays[3][4];
    std::memcpy(forward, rows, sizeof(forward)); std::memcpy(rays, rows+kFlatDeferredInverseOffsetRows, sizeof(rays));
    if(!flatJitterForwardDp4(forward, jitter)) { fail(why, "forward-patch", "refused", 0, 0, 0, 0); return false; }
    if(!flatJitterInverseUvRay(rays, jitter)) { fail(why, "inverse-patch", "refused", 0, 0, 0, 0); return false; }
    std::memcpy(rows, forward, sizeof(forward)); std::memcpy(rows+kFlatDeferredInverseOffsetRows, rays, sizeof(rays));
    if(flatExplainDeferredMatch(cameraBytes, reinterpret_cast<unsigned char*>(rows), true, "rematch", why)!=FlatDeferredCameraMatch::Match) return false;
    why = FlatDeferredReject{};
    return true;
}

// Log only. A deferred pass with no source record whose witness entry raised conflict=second-camera: two writers on the depth bound
// different camera rows, and the witness keeps hashes, not bytes. This answers the three questions that decide what the second camera is,
// from the 96 bytes of each and the pass's own CB2 (rows 10..16): which of them the CB2 matches under the strict relation (plain, and with
// the frustum allowance the whole-camera repair's rematch uses), and how the two cameras differ (a handful of last-bit rounding differences
// of the landed phase, or a different view). Nothing here is read by a decision.
struct FlatSecondCameraReport {
    FlatDeferredCameraMatch first = FlatDeferredCameraMatch::Unavailable, offender = FlatDeferredCameraMatch::Unavailable;
    FlatDeferredCameraMatch firstScaled = FlatDeferredCameraMatch::Unavailable, offenderScaled = FlatDeferredCameraMatch::Unavailable;
    FlatDeferredReject firstWhy, offenderWhy;   // the plain match's deciding check for each
    bool compared = false;                      // both cameras present
    unsigned differing = 0;                     // floats whose bits differ
    unsigned firstDiff = 0;                     // index (0..23) of the first differing float
    double maxAbs = 0, maxRel = 0;              // over the differing floats; rel = |a-b| / max(|a|,|b|)
};
// The jitter phase a camera's 96 bytes carry, as the game's projection builds it: clip x = x0 + ndcX * w, clip y = y0 + ndcY * w, where the
// w row (camera[c][3]) is the view axis and x0, y0 are orthogonal to it. So ndcX = (x row . w row) / (w row . w row), likewise y, and the
// scale is what is left of the row. Log only: it says whether the draws of a camera were jittered by the phase EDVR requested.
struct FlatCameraPhase {
    bool valid = false;
    double ndcX = 0, ndcY = 0, scaleX = 0, scaleY = 0, nearPlane = 0;
};
inline FlatCameraPhase flatEstimateCameraPhase(const unsigned char* cameraBytes) {
    FlatCameraPhase r;
    if(!cameraBytes) return r;
    float camera[6][4]; std::memcpy(camera, cameraBytes, sizeof(camera));
    for(unsigned c=0;c<4;++c) for(unsigned i=0;i<4;++i) if(!std::isfinite(camera[c][i])) return r;
    double ww=0, xw=0, yw=0;
    for(unsigned c=0;c<3;++c) { ww+=double(camera[c][3])*camera[c][3]; xw+=double(camera[c][0])*camera[c][3]; yw+=double(camera[c][1])*camera[c][3]; }
    if(!(ww>0)) return r;
    r.ndcX=xw/ww; r.ndcY=yw/ww;
    double sx=0, sy=0;
    for(unsigned c=0;c<3;++c) {
        const double x=camera[c][0]-r.ndcX*camera[c][3], y=camera[c][1]-r.ndcY*camera[c][3];
        sx+=x*x; sy+=y*y;
    }
    r.scaleX=std::sqrt(sx); r.scaleY=std::sqrt(sy); r.nearPlane=camera[3][2]; r.valid=true;
    return r;
}
inline FlatSecondCameraReport flatExplainSecondCamera(const unsigned char* first, const unsigned char* offender,
                                                      const unsigned char* cb2) {
    FlatSecondCameraReport r;
    r.first=flatExplainDeferredMatch(first, cb2, false, "first", r.firstWhy);
    r.offender=flatExplainDeferredMatch(offender, cb2, false, "offender", r.offenderWhy);
    FlatDeferredReject scratch;
    r.firstScaled=flatExplainDeferredMatch(first, cb2, true, "first-scaled", scratch);
    r.offenderScaled=flatExplainDeferredMatch(offender, cb2, true, "offender-scaled", scratch);
    if(first && offender) {
        r.compared=true;
        float a[24], b[24]; std::memcpy(a, first, sizeof(a)); std::memcpy(b, offender, sizeof(b));
        for(unsigned i=0;i<24;++i) {
            if(std::memcmp(&a[i], &b[i], 4)==0) continue;
            if(!r.differing) r.firstDiff=i;
            ++r.differing;
            const double d=std::abs(double(a[i])-double(b[i])), m=std::fmax(std::abs(double(a[i])), std::abs(double(b[i])));
            if(std::isfinite(d)) { if(d>r.maxAbs) r.maxAbs=d; if(m>0 && d/m>r.maxRel) r.maxRel=d/m; }
        }
    }
    return r;
}
}

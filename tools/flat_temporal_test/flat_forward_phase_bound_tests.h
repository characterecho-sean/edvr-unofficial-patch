#pragma once
#include "../../src/d3d11/flat_forward_light.h"
#include "../../src/d3d11/flat_phase_failure_capture.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace flat_forward_phase_bound_test {
using namespace edvr;
using namespace edvr::forward_light;

// Volatile stores model the observed scalar SSE32 instruction boundaries.
// This oracle constructs engine endpoints and rows; it never calls a jitter
// patch helper, phaseAxis, or a rearrangement of the certificate inequality.
inline float stored(float v) { volatile float result=v;return result; }
inline float add32(float a,float b){return stored(a+b);}
inline float sub32(float a,float b){return stored(a-b);}
inline float mul32(float a,float b){return stored(a*b);}
inline float div32(float a,float b){return stored(a/b);}
inline float bitsFloat(uint32_t bits){float result;std::memcpy(&result,&bits,sizeof(result));return result;}
inline uint32_t floatBits(float value){uint32_t result;std::memcpy(&result,&value,sizeof(result));return result;}
struct AxisProjection {float scale=0,center=0;};
inline AxisProjection engineAxis(float halfExtent,float injectedBound) {
    const float span=sub32(halfExtent,-halfExtent);
    const float displacement=mul32(span,injectedBound);
    const float lowerEndpoint=sub32(-halfExtent,displacement);
    const float upperEndpoint=sub32(halfExtent,displacement);
    return {div32(2.f,sub32(upperEndpoint,lowerEndpoint)),
            div32(add32(upperEndpoint,lowerEndpoint),sub32(lowerEndpoint,upperEndpoint))};
}
inline FlatProjectionJitter requested(float pixelX,float pixelY,uint32_t width,uint32_t height) {
    const float dx=static_cast<float>(2.*double(pixelX)/width);
    const float dy=static_cast<float>(-2.*double(pixelY)/height);
    return {dx,dy,dx*.5f,-dy*.5f};
}
struct Camera {
    float donor[6][4]{},constants[47][4]{};
    Reason result(const FlatProjectionJitter& expected={})const {
        return match(reinterpret_cast<const unsigned char*>(donor),
                     reinterpret_cast<const unsigned char*>(constants),expected);
    }
};
inline Camera camera(float halfHeight,float aspect,float boundX,float boundY,unsigned orientation=0) {
    Camera out{};
    // Rz(yaw)*Ry(pitch) gives stored, approximately orthonormal rows. Include
    // both a general rotation and a near-axis rotation, not only exact axes.
    const double angles[3][2]={{.73,-.41},{1.0471975511965976,.000017},{-.29,1.31}};
    const double cy=std::cos(angles[orientation][0]),sy=std::sin(angles[orientation][0]);
    const double cp=std::cos(angles[orientation][1]),sp=std::sin(angles[orientation][1]);
    const double rotation[3][3]={{cy*cp,-sy,cy*sp},{sy*cp,cy,sy*sp},{-sp,0,cp}};
    const float origin[3]={3.25f,-2.125f,7.5f};
    for(unsigned row=0;row<3;++row){float translation=0;
        for(unsigned col=0;col<3;++col){out.constants[2+row][col]=static_cast<float>(rotation[row][col]);
            translation=sub32(translation,mul32(out.constants[2+row][col],origin[col]));}
        out.constants[2+row][3]=translation;out.donor[5][row]=origin[row];
    }
    const AxisProjection projection[2]={engineAxis(mul32(halfHeight,aspect),boundX),engineAxis(halfHeight,boundY)};
    for(unsigned row=0;row<2;++row)for(unsigned col=0;col<4;++col)
        out.constants[10+row][col]=add32(mul32(projection[row].scale,out.constants[2+row][col]),
                                            mul32(projection[row].center,out.constants[4][col]));
    out.constants[12][3]=out.donor[3][2]=.025f;
    for(unsigned col=0;col<4;++col)out.constants[13][col]=out.constants[4][col];
    for(unsigned row=0;row<4;++row)for(unsigned col=0;col<3;++col)out.donor[col][row]=out.constants[10+row][col];
    for(unsigned col=0;col<3;++col)out.donor[4][col]=out.constants[4][col];
    return out;
}
inline Camera captured(bool withPhase) {
    // 2d25730e verified paired receipt, frames14352/14353. Values are the
    // round-trip float bits from the sanctioned decoder's consumed rows.
    const uint32_t donor[]={0xBF09DE2Au,0xBFD442E1u,0,0xB6CEA54Bu,0xBF6ECB3Du,0x3F75192Eu,0,0xB77599F5u,0x378E5E82u,0xB67F368Au,0,0xBF800000u,0,0,0x3CCCCCCDu,0,0xB6CEA54Bu,0xB77599F5u,0xBF800000u,0,0xC070D320u,0x400ABF2Au,0xC18DD14Bu,0};
    const uint32_t view[]={0xBF000006u,0xBF5DB3D6u,0x37842DE3u,0xBB720BAAu,0xBF5DB3D6u,0x3F000006u,0xB605484Au,0xC08AF7CEu,0xB6CEA54Bu,0xB77599F5u,0xBF800000u,0xC18DD147u};
    const uint32_t forward[]={0xBF09DE2Au,0xBF6ECB3Du,0x378E5E82u,0xBB825A46u,0xBFD442E1u,0x3F75192Eu,0xB67F368Au,0xC1050CCDu,0,0,0,0x3CCCCCCDu,0xB6CEA54Bu,0xB77599F5u,0xBF800000u,0xC18DD147u};
    Camera out{};std::memcpy(out.donor,donor,sizeof(donor));std::memcpy(out.constants+2,view,sizeof(view));std::memcpy(out.constants+10,forward,sizeof(forward));
    if(withPhase){const uint32_t phasedY[]={0xBFD442E1u,0x3F75192Eu,0xB925CA72u,0xC1051801u};
        std::memcpy(out.constants[11],phasedY,sizeof(phasedY));for(unsigned col=0;col<3;++col)out.donor[col][1]=out.constants[11][col];}
    return out;
}

inline int run() {
    unsigned checks=0,builders=0;int failures=0;
    const auto expect=[&](bool ok,const char* message){++checks;if(!ok){++failures;if(failures<=20)std::printf("FAIL: forward phase bound %s\n",message);}};
    const float nativeHalfHeight=bitsFloat(0x3F05B180u),captureNdc=bitsFloat(0x3921D139u);
    const auto bootstrap=captured(false),rejected=captured(true);
    const auto native=engineAxis(nativeHalfHeight,stored(captureNdc*.5f));
    expect(floatBits(native.scale)==0x3FF51923u,"engine scalar replay recovers captured native projection scale");
    expect(floatBits(native.center)==0x3921CD98u,"engine scalar replay recovers captured center independently of expected shift");
    const auto centered=engineAxis(nativeHalfHeight,0);
    for(unsigned col=0;col<4;++col){
        expect(floatBits(mul32(centered.scale,bootstrap.constants[3][col]))==floatBits(bootstrap.constants[11][col]),"native builder reproduces each bootstrap Y component bit for bit");
        const float row=add32(mul32(native.scale,rejected.constants[3][col]),mul32(native.center,rejected.constants[4][col]));
        expect(floatBits(row)==floatBits(rejected.constants[11][col]),"native builder reproduces each rejected Y component bit for bit");
    }
    const FlatProjectionJitter capturedPhase={0,captureNdc,0,-captureNdc*.5f};
    expect(bootstrap.result()==Reason::None,"actual bootstrap is accepted with zero phase");
    expect(rejected.result(capturedPhase)==Reason::None,"actual correctly phased candidate passes engine-construction bound");
    expect(bootstrap.result(capturedPhase)==Reason::Phase&&rejected.result()==Reason::Phase,"identical bound policy distinguishes actual zero and nonzero captured phases");
    // The known old failure was Y/column2, not the large translation. Confirm
    // the fixture exercises that counterexample rather than merely a fit.
    double oldScale=0;for(unsigned col=0;col<3;++col)oldScale+=(double(rejected.constants[11][col])-double(captureNdc)*rejected.constants[13][col])*rejected.constants[3][col];
    const double oldResidual=double(rejected.constants[11][2])-double(captureNdc)*rejected.constants[13][2]-oldScale*rejected.constants[3][2];
    const double oldBound=flatDeferredFloatBound(24,std::abs(double(rejected.constants[11][2]))+std::abs(double(captureNdc)*rejected.constants[13][2])+std::abs(oldScale*rejected.constants[3][2]));
    expect(std::abs(oldResidual)>20*oldBound,"captured fixture independently reproduces old false rejection at Y/column2");
    const auto report=phase_failure::analyze(reinterpret_cast<const unsigned char*>(rejected.donor),reinterpret_cast<const unsigned char*>(rejected.constants),capturedPhase);
    expect(report.reason==Reason::None&&report.stage==phase_failure::Stage::Accepted,"diagnostic uses the same accepted construction contract");

    const float halfHeights[]={.0625f,.25f,nativeHalfHeight,1.f,4.f,16.f};
    const float aspects[]={.625f,1.f,16.f/9.f,32.f/9.f};
    const uint32_t extents[][2]={{960,540},{1920,1080},{3840,2160},{1248,780}};
    const float pixels[][2]={{0,0},{.375f,-.25f},{-.375f,.25f},{0,-.166666657f},{.125f,0},{-.4375f,.388888896f},{.25f,-.388888896f}};
    for(float h:halfHeights)for(float aspect:aspects)for(unsigned orient=0;orient<3;++orient)
    for(const auto& extent:extents)for(const auto& pixel:pixels){
        const auto expected=requested(pixel[0],pixel[1],extent[0],extent[1]);
        const float bx=div32(pixel[0],static_cast<float>(extent[0]));
        const float by=div32(-pixel[1],static_cast<float>(extent[1]));
        const auto actual=camera(h,aspect,bx,by,orient);++builders;
        expect(actual.result(expected)==Reason::None,"independent centered float builder accepts across FOV/aspect/rotation/extents/signed axes");
        for(unsigned axis=0;axis<2;++axis){PhaseConstruction envelope;const double shift=axis?expected.ndcY:expected.ndcX;
            const auto projection=engineAxis(axis?h:mul32(h,aspect),axis?by:bx);
            expect(phaseConstructionBound(shift,envelope)&&std::abs(double(projection.center)-shift)<=envelope.bound,
                   "coefficient envelope contains actual float endpoint construction and injector rounding");
        }
        if(pixel[0]!=0||pixel[1]!=0){
            expect(actual.result()!=Reason::None,"a real nonzero phase cannot certify zero on rotated stored views");
            const auto unphased=camera(h,aspect,0,0,orient);
            expect(unphased.result(expected)!=Reason::None,"nonzero requested phase cannot admit unphased builder");
        }
    }
    const auto expected=requested(.375f,-.25f,3840,2160);
    const float bx=div32(.375f,3840.f),by=div32(.25f,2160.f);
    const auto good=camera(nativeHalfHeight,16.f/9.f,bx,by);
    const float wrongBounds[][2]={{0,0},{-bx,-by},{2*bx,2*by},{bx,0},{0,by},{div32(-.125f,3840.f),div32(.277777791f,2160.f)}};
    for(const auto& wrong:wrongBounds){
        const auto wrongCamera=camera(nativeHalfHeight,16.f/9.f,wrong[0],wrong[1]);
        const auto before=wrongCamera;
        expect(wrongCamera.result(expected)==Reason::Phase,"missing reversed doubled single-axis and prior-frame phases refuse");
        const auto analysis=phase_failure::analyze(reinterpret_cast<const unsigned char*>(wrongCamera.donor),reinterpret_cast<const unsigned char*>(wrongCamera.constants),expected);
        expect(analysis.reason==Reason::Phase&&analysis.stage==phase_failure::Stage::PhaseResidual&&analysis.axis>=0&&analysis.axis<2,"diagnostic names actual failed phase axis");
        if(analysis.axis>=0&&analysis.axis<2){
            const auto axis=phaseAxis(wrongCamera.constants,static_cast<unsigned>(analysis.axis),analysis.axis?expected.ndcY:expected.ndcX);
            expect(analysis.column==axis.column&&analysis.bound==axis.bound&&analysis.residual==axis.residual&&analysis.lhs==axis.lhs&&analysis.rhs==axis.rhs,"diagnostic and admission share every failed component and bound exactly");
        }
        expect(!std::memcmp(&before,&wrongCamera,sizeof(before)),"phase analysis preserves actual stored input bytes");
    }
    for(unsigned axis=0;axis<2;++axis)for(unsigned col=0;col<4;++col){auto bad=good;bad.constants[10+axis][col]+=.01f;
        expect(phaseAxis(bad.constants,axis,axis?expected.ndcY:expected.ndcX).reason!=Reason::None,"every spatial/translation component participates in phase certification");}
    auto bad=good;bad.donor[5][0]+=.25f;expect(bad.result(expected)==Reason::Origin,"construction envelope cannot relax geometry origin authority");
    bad=good;bad.constants[3][0]+=.05f;expect(bad.result(expected)!=Reason::None,"construction envelope cannot relax used-view authority");
    bad=good;bad.donor[2][0]+=.01f;expect(bad.result(expected)==Reason::Forward,"construction envelope cannot relax donor forward agreement");

    // Numeric phase agreement supplies no source or producer lineage.
    Surface depth{};depth.resource=101;depth.width=3840;depth.height=2160;depth.slices=depth.samples=1;depth.known=true;
    auto linear=depth;linear.resource=102;
    Source source{};source.buffer=103;source.generation=4;source.bank=2;source.writeGeneration=8;source.frame=90;source.writeQ=10;source.count=4096;
    Ledger ledger;ledger.begin(source.frame);
    ledger.observe(depth,Role::Origin,true,true,source,reinterpret_cast<const unsigned char*>(good.donor),11);
    const auto token=ledger.publish(depth,linear,12);ledger.issued(token);ledger.returned(token);
    Reason lineage=Reason::None;
    expect(good.result(expected)==Reason::None&&ledger.find(depth,linear,13,lineage)!=nullptr&&ledger.domains[0].failure==Reason::None,"phase acceptance and independent current lineage can coexist");
    auto stale=source;--stale.frame;
    ledger.observe(depth,Role::Origin,true,true,stale,reinterpret_cast<const unsigned char*>(good.donor),14);
    expect(ledger.domains[0].failure==Reason::StaleDonor,"correct phase cannot revive stale origin source");
    ledger.begin(source.frame+1);
    expect(!ledger.find(depth,linear,13,lineage),"correct phase cannot revive prior-frame converter cohort");

    PhaseConstruction envelope;
    expect(phaseConstructionBound(0,envelope)&&envelope.bound==0,"exact centered zero construction adds no absolute tolerance");
    for(double unsupported:{std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),1.,-1.,double((std::numeric_limits<float>::denorm_min)())})
        expect(!phaseConstructionBound(unsupported,envelope),"unsupported coefficient regimes refuse explicitly");
    const float tiny=1.e-8f;
    const auto invisible=camera(nativeHalfHeight,1,0,tiny*.5f);
    const FlatProjectionJitter tinyPhase={0,tiny,0,-tiny*.5f};
    expect(invisible.result(tinyPhase)!=Reason::None,"enlarged coefficient uncertainty cannot certify a phase erased by endpoint rounding");
    expect(invisible.result()==Reason::None,"same actual erased-phase rows still constitute a zero bootstrap");
    for(float unsupportedHalf:{(std::numeric_limits<float>::denorm_min)(),(std::numeric_limits<float>::min)(),(std::numeric_limits<float>::max)()})
        expect(camera(unsupportedHalf,1,bx,by).result(expected)!=Reason::None,"subnormal underflow overflow frustum constructions refuse");
    auto lost=camera(1.e-20f,1,bx,by);expect(lost.result(expected)!=Reason::None,"huge projection scale cannot hide required phase inside float matrix roundoff");
    const auto cropped=camera(nativeHalfHeight,16.f/9.f,bx+.01f,by);
    expect(cropped.result(expected)==Reason::Phase,"unmodelled off-center crop outside construction envelope refuses");
    std::printf("forward phase bound: %u checks, %u independent engine cameras, %d failed\n",checks,builders,failures);
    return failures;
}
} // namespace flat_forward_phase_bound_test
inline int flatForwardPhaseBoundTests(){return flat_forward_phase_bound_test::run();}

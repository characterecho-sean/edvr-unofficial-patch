#pragma once
// Cameras built the way the game builds them, rather than copied from a flight. The game's off-centre perspective
// (tools/c2_derive_test/c2_derive_model.h, the trigonometric branch) forms the window edges in float32 and takes
// p8 = (R+L)/(L-R), so the phase it lands is the request rounded at the step of the frustum, not the request.
// A camera is that projection times a view; the lighting pass's CB2 is the same camera built without the phase.
// The 2560x1440 flights (flat_deferred_landed_phase_captures.h) are one point of this space; the supercruise captures
// (960x600, world terms in the thousands) are another. This runs the repair across the space: field of view, aspect,
// the eight phases, a camera tilt, and positions from the world origin to a hundred metres out.
#include <cmath>
#include <cstdio>
#include <cstring>
#include "../../src/d3d11/flat_deferred_reject.h"
namespace deferred_generated {
using namespace edvr;
struct Scene { float t, aspect; double yaw, pitch; float world[3]; };
struct Pair { unsigned char camera[96], cb2[112]; float carried[2], nominal[2]; };
inline float tanHalf(float fov) { return static_cast<float>(std::tan(static_cast<double>(fov*0.5f))); }
// The window edges and the projection terms for bounds bx, by (the game's c.F(0x26C), c.F(0x270)).
struct Proj { float p0, p5, p8, p9; };
inline Proj projection(float t, float aspect, float bx, float by) {
    float l=-t;
    float fy=(t-l)*by;
    float fx=(t*aspect-l*aspect)*bx;
    const float bottom=l-fy, top=t-fy;
    const float left=l*aspect-fx, right=t*aspect-fx;
    Proj p;
    p.p0=2.0f/(right-left); p.p5=2.0f/(top-bottom);
    p.p8=(right+left)/(left-right); p.p9=(top+bottom)/(bottom-top);
    return p;
}
inline void putRow(float (&dst)[4], float a, float b, float c, float d) { dst[0]=a; dst[1]=b; dst[2]=c; dst[3]=d; }
// camera bytes: the phased projection. cb2 bytes: the unphased one. Both finalized in float32.
inline bool build(const Scene& s, float bx, float by, float ndcX, float ndcY, Pair& out) {
    const Proj ph=projection(s.t,s.aspect,bx,by), un=projection(s.t,s.aspect,0.f,0.f);
    // Orthonormal view frame: x, y (down), z = x cross y, tilted by yaw then pitch.
    double R[3][3]={{1,0,0},{0,-1,0},{0,0,-1}};
    const double cy=std::cos(s.yaw),sy=std::sin(s.yaw),cp=std::cos(s.pitch),sp=std::sin(s.pitch);
    const double Ry[3][3]={{cy,0,sy},{0,1,0},{-sy,0,cy}}, Rx[3][3]={{1,0,0},{0,cp,-sp},{0,sp,cp}};
    double T1[3][3],T2[3][3];
    for(int i=0;i<3;++i)for(int j=0;j<3;++j){T1[i][j]=0;for(int k=0;k<3;++k)T1[i][j]+=R[i][k]*Ry[k][j];}
    for(int i=0;i<3;++i)for(int j=0;j<3;++j){T2[i][j]=0;for(int k=0;k<3;++k)T2[i][j]+=T1[i][k]*Rx[k][j];}
    float xr[3],yr[3],zr[3];
    for(int c=0;c<3;++c){xr[c]=static_cast<float>(T2[0][c]);yr[c]=static_cast<float>(T2[1][c]);zr[c]=static_cast<float>(T2[2][c]);}
    float Mp[4][3],Mu[4][3];   // clip row r, world column c
    for(int c=0;c<3;++c){
        Mu[0][c]=un.p0*xr[c]; Mu[1][c]=un.p5*yr[c]; Mu[2][c]=0; Mu[3][c]=zr[c];
        Mp[0][c]=ph.p0*xr[c]+ph.p8*zr[c]; Mp[1][c]=ph.p5*yr[c]+ph.p9*zr[c]; Mp[2][c]=0; Mp[3][c]=zr[c];
    }
    const float nearPlane=0.025f;
    float camera[6][4]{},rows[7][4]{};
    for(int c=0;c<3;++c)for(int r=0;r<4;++r)camera[c][r]=Mp[r][c];
    putRow(camera[3],0,0,nearPlane,0);
    putRow(camera[4],camera[0][3],camera[1][3],camera[2][3],0);
    putRow(camera[5],s.world[0],s.world[1],s.world[2],0);
    for(int r=0;r<4;++r){
        for(int c=0;c<3;++c)rows[r][c]=Mu[r][c];
        float sum=0;for(int c=0;c<3;++c)sum+=Mu[r][c]*s.world[c];
        rows[r][3]=r==2?nearPlane:-sum;
    }
    // Inverse rays: the 3x3 A of clip rows 0,1,3 times B must equal T = [[2,0,-1],[0,-2,1],[0,0,1]].
    const int clip[3]={0,1,3};
    double A[3][3];for(int r=0;r<3;++r)for(int i=0;i<3;++i)A[r][i]=rows[clip[r]][i];
    const double det=A[0][0]*(A[1][1]*A[2][2]-A[1][2]*A[2][1])-A[0][1]*(A[1][0]*A[2][2]-A[1][2]*A[2][0])+A[0][2]*(A[1][0]*A[2][1]-A[1][1]*A[2][0]);
    if(!(std::abs(det)>1e-9))return false;
    double Inv[3][3]={{A[1][1]*A[2][2]-A[1][2]*A[2][1],A[0][2]*A[2][1]-A[0][1]*A[2][2],A[0][1]*A[1][2]-A[0][2]*A[1][1]},
                      {A[1][2]*A[2][0]-A[1][0]*A[2][2],A[0][0]*A[2][2]-A[0][2]*A[2][0],A[0][2]*A[1][0]-A[0][0]*A[1][2]},
                      {A[1][0]*A[2][1]-A[1][1]*A[2][0],A[0][1]*A[2][0]-A[0][0]*A[2][1],A[0][0]*A[1][1]-A[0][1]*A[1][0]}};
    const double Tm[3][3]={{2,0,-1},{0,-2,1},{0,0,1}};
    const int rayCol[3]={0,1,3};
    for(int i=0;i<3;++i)for(int c=0;c<3;++c){double sum=0;for(int k=0;k<3;++k)sum+=Inv[i][k]/det*Tm[k][c];rows[4+i][rayCol[c]]=static_cast<float>(sum);}
    std::memcpy(out.camera,camera,sizeof(camera));std::memcpy(out.cb2,rows,sizeof(rows));
    // How much phase the camera landed against what was requested: the x-row z element, phased less unphased.
    out.carried[0]=Mp[0][2]-Mu[0][2]; out.nominal[0]=ndcX*zr[2];
    out.carried[1]=Mp[1][2]-Mu[1][2]; out.nominal[1]=ndcY*zr[2];
    return true;
}
inline unsigned run() {
    using namespace edvr;
    unsigned failures=0; char name[200];
    auto expect=[&](bool ok,const char* what){if(!ok){std::printf("FAIL: deferred generated %s\n",what);++failures;}};
    const float phaseX[8]={0.f,-.25f,.25f,-.375f,.125f,-.125f,.375f,-.4375f};
    const float phaseY[8]={-1.f/6,1.f/6,-.3888889f,-.0555556f,.2777778f,-.2777778f,.0555556f,.3888889f};
    // Fidelity to the flights: the 2560x1440 rig of the probe logs lands the phase at 1.0001x for most phases and 1.0000084x for x -0.4375.
    {
        Scene s{1.0f/1.87090755f,16.0f/9.0f,3e-4,2e-4,{0.3127f,-0.18f,-11.155f}};
        unsigned landedNear=0;
        for(int i=0;i<8;++i){
            if(phaseX[i]==0.f)continue;
            FlatProjectionJitter j{};flatProjectionJitter(phaseX[i],phaseY[i],2560,1440,j);
            Pair p;if(!build(s,phaseX[i]/2560.f,-phaseY[i]/1440.f,j.ndcX,j.ndcY,p)){expect(false,"the flight-like scene builds");continue;}
            const double ratio=static_cast<double>(p.carried[0])/p.nominal[0];
            if(ratio>1.00005&&ratio<1.00015)++landedNear;
        }
        expect(landedNear>=5,"the generator lands the phase near 1.0001x, as the 2560x1440 flights did, for most x phases");
    }
    const float fovT[]={0.4f,0.5345f,0.7f,1.0f};             // tan(fov/2): about 44, 56, 70 and 90 degrees of vertical field
    const float aspects[]={16.0f/9.0f,16.0f/10.0f,21.0f/9.0f,4.0f/3.0f};
    const struct { unsigned w,h; } sizes[]={{2560,1440},{1920,1200},{3440,1440},{1600,1200},{960,600}};
    const double tilts[][2]={{0,0},{3e-4,2e-4},{0.01,-0.02},{0.4,0.25}};
    const float worlds[][3]={{0,0,-11.155f},{0.3127f,-0.18f,-11.155f},{1e-4f,0.3f,-2.f},{0,0,-30.f},{5.f,3.f,-100.f},{0.f,0.f,-0.5f}};
    unsigned built=0,accepted=0,refusedMoved=0,movedCases=0;
    for(unsigned ti=0;ti<4;++ti)for(unsigned ai=0;ai<4;++ai)for(unsigned wi=0;wi<6;++wi)for(unsigned li=0;li<4;++li){
        Scene s{fovT[ti],aspects[ai],tilts[li][0],tilts[li][1],{worlds[wi][0],worlds[wi][1],worlds[wi][2]}};
        const auto size=sizes[ai];   // the size follows the aspect
        for(unsigned pi=1;pi<8;++pi){
            FlatProjectionJitter j{};if(!flatProjectionJitter(phaseX[pi],phaseY[pi],size.w,size.h,j))continue;
            Pair p;if(!build(s,phaseX[pi]/size.w,-phaseY[pi]/size.h,j.ndcX,j.ndcY,p))continue;
            ++built;
            std::snprintf(name,sizeof(name),"t=%.3f aspect=%.3f tilt=%u world=%u phase=%u: ",s.t,s.aspect,li,wi,pi);
            // Without a phase the generated pair is a camera the strict matcher certifies: the generator is faithful.
            Pair none;
            if(build(s,0.f,0.f,0.f,0.f,none)){
                char n2[240];std::snprintf(n2,sizeof(n2),"%szero phase matches",name);
                expect(flatMatchDeferredCamera(none.camera,none.cb2)==FlatDeferredCameraMatch::Match,n2);
            }
            unsigned char repaired[112];FlatDeferredReject why;
            const bool ok=flatRepairDeferredWholeOmission(p.camera,p.cb2,j,repaired);
            const bool explained=flatExplainWholeOmission(p.camera,p.cb2,j,why);
            char n3[260];std::snprintf(n3,sizeof(n3),"%sthe repair accepts the game-built whole-camera omission (refused at %s/%s residual %.3g bound %.3g)",name,why.stage,why.check,why.residual,why.bound);
            expect(ok,n3);
            std::snprintf(n3,sizeof(n3),"%sthe explanation agrees with the repair",name);expect(ok==explained,n3);
            if(ok){
                ++accepted;
                std::snprintf(n3,sizeof(n3),"%scorrected rows strictly match",name);
                expect(flatMatchDeferredCamera(p.camera,repaired,true)==FlatDeferredCameraMatch::Match,n3);
                // A camera not where its CB2 was built must still be refused. 2e-4 m sideways is far outside the allowance's
                // few-ulp window at these scales (the largest allowance here is about 3e-5 m), and 1e-3 m along the view hits the w row.
                unsigned char moved[96],scratch[112];
                for(int axis=0;axis<3;++axis){
                    std::memcpy(moved,p.camera,96);float w;std::memcpy(&w,moved+(20+axis)*4,4);w+=axis==2?1e-3f:2e-4f;std::memcpy(moved+(20+axis)*4,&w,4);
                    ++movedCases;
                    if(!flatRepairDeferredWholeOmission(moved,p.cb2,j,scratch))++refusedMoved;
                }
            }
        }
    }
    expect(built>=2500,"the grid built a few thousand pairs");
    expect(accepted==built,"every game-built pair repairs");
    expect(movedCases>0&&refusedMoved==movedCases,"a camera displaced from its CB2 is refused wherever it sits");
    std::printf("flat deferred generated: %u pairs, %u repaired, %u of %u displaced cameras refused, %u failure(s)\n",built,accepted,refusedMoved,movedCases,failures);
    return failures;
}
}

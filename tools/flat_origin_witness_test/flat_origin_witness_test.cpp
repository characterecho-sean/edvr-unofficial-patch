// Match the resolver's Windows SDK include order. In particular, leave the
// SDK's near/far macros intact so this rig catches accidental identifier use.
#include <windows.h>
#include <d3d11_1.h>
#if !defined(near) || !defined(far)
#error The Windows SDK near/far macros must remain defined for this regression.
#endif
#include "../../src/d3d11/flat_origin_witness.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

static std::vector<std::string> lines;
namespace edvr {
Log& Log::get(){static auto* log=new Log;return *log;}
void Log::note(const char* fmt,...){char line[2048]{};va_list ap;va_start(ap,fmt);
    std::vsnprintf(line,sizeof(line),fmt,ap);va_end(ap);lines.emplace_back(line);}
}
using edvr::FlatOriginWitnessCore;
using edvr::FlatOriginWitnessReport;

static void check(bool p,const char* message){if(!p){std::fprintf(stderr,"FAIL: %s\n",message);std::abort();}}
static void pose(float x,float y,float z,uint32_t out[5]){
    std::memcpy(out,&x,4);std::memcpy(out+1,&y,4);std::memcpy(out+2,&z,4);
    out[3]=0x80008000u;out[4]=0xFFFF8000u;
}
static uint64_t identityHash(unsigned id){
    uint64_t x=(0x10000ull+id*0x1000ull)^((0x20000ull+id*0x2000ull)*0x9E3779B97F4A7C15ull);
    x^=x>>33;x*=0xff51afd7ed558ccdull;x^=x>>33;x*=0xc4ceb9fe1a85ec53ull;return x^(x>>33);
}
static void add(FlatOriginWitnessCore& c,unsigned frame,unsigned id,float x,uintptr_t node=0){
    uint32_t p[5];pose(x,float(id)*2.0f,100.0f,p);
    c.observe(0x10000u+id*0x1000u,node?node:0x20000u+id*0x2000u,0x30000u+id*0x3000u,p,frame,true,true);
}
static FlatOriginWitnessReport runCase(bool shift,bool outlier=false){
    FlatOriginWitnessCore c;
    for(unsigned frame=97;frame<=102;++frame)for(unsigned id=1;id<=5;++id){
        float x=float(id)*100.0f;
        if(frame>=100 && shift)x-=60.0f;
        if(frame>=100 && outlier && id==1)x+=15.0f;
        add(c,frame,id,x);
    }
    const float now[3]={20,0,0},previous[3]={80,0,0};
    check(c.event(100,100,now,previous,16.0f,true,true,false),"candidate retained");
    FlatOriginWitnessReport r;
    check(!c.complete(102,r),"wait until +2 frame has ended");
    check(c.complete(103,r),"event completes at +3 boundary");
    check(!c.complete(103,r),"event completes exactly once");
    check(r.clockMatch && r.sampleFrame==100 && r.hadFrame[0] && r.hadFrame[5],"six aligned frames and clocks");
    check(r.preStable==5 && r.owners==5 && r.edgeCount[2]==5,"stable diverse cohort through event");
    check(r.farOwners==2 && r.farEdgeCount[2]==2,"far owners have their own event-edge evidence");
    check(r.rows[0].present==0x3F,"raw witness includes all six positions");
    return r;
}
static void selfTest(){
    using namespace edvr;
    check(!flatOriginCaptureEnabled(), "origin capture starts off before initialization");
    const float offNow[3]={20,0,0},offOld[3]={80,0,0};const uint32_t offPose[5]{};
    flatOriginWitnessFrameBoundary(1,true,true);
    flatOriginWitnessObserve(1,2,3,offPose,1,true,true);
    flatOriginWitnessEvent(1,offNow,offOld,16,true,true,false);
    flatOriginWitnessUnavailable(1,offNow,offOld,16,"off-fixture");
    flatOriginWitnessShutdown();
    check(lines.empty() && !flat_origin_witness_runtime::active.load() &&
          !flat_origin_witness_runtime::callbacks.load() && !flat_origin_witness_runtime::core.events(),
          "default off blocks every origin entry without logs, reads or samples");
    flatOriginCaptureInitialize(true);
    check(flatOriginCaptureEnabled(), "origin witness rig explicitly opts in");
    auto inward=runCase(false);
    check(inward.verdict==FlatOriginWitnessReport::Verdict::UnchangedRecords,"80 to 20 inward camera with static game records");
    check(inward.worldEdgeValid && inward.cameraMinusWorldValid,"measured zero world edge remains valid");
    check(inward.cameraDelta[0]==-60 && inward.edgeMedian[2][0]==0 && inward.cameraMinusMedian[0]==-60,
          "camera -60 and world zero remain separate raw measurements");
    auto reanchor=runCase(true);
    check(reanchor.verdict==FlatOriginWitnessReport::Verdict::CoherentRecordShift && reanchor.edgeMedian[2][0]==-60,
          "five game-owned current poses share -60 on reanchor");
    auto oneMover=runCase(true,true);
    check(oneMover.verdict==FlatOriginWitnessReport::Verdict::CoherentRecordShift && oneMover.edgeMedian[2][0]==-60 &&
          oneMover.edgeCoherent[2]==4 && oneMover.movingOutliers==1,"one moving outlier cannot move the cohort median");
    {
        FlatOriginWitnessCore c;
        for(unsigned f=97;f<=102;++f)for(unsigned id=1;id<=6;++id){
            const float baseline=id<=4?90.0f+id*10.0f:400.0f+id*100.0f;
            add(c,f,id,baseline+(f>=100 && id<=4?-60.0f:0.0f));
        }
        const float now[3]={20,0,0},old[3]={80,0,0};c.event(100,100,now,old,16,true,true,false);
        FlatOriginWitnessReport r;check(c.complete(103,r),"mixed near and far cohort completes");
        check(r.edgeMedian[2][0]==-60 && r.farEdgeMedian[2][0]==0 &&
            r.verdict==FlatOriginWitnessReport::Verdict::Inconclusive,"far static records veto moving near cohort");
    }
    {
        FlatOriginWitnessCore c;
        for(unsigned f=97;f<=102;++f)for(unsigned id=1;id<=5;++id)
            add(c,f,id,id*100.0f+(f>=99?-60.0f:0.0f));
        const float now[3]={20,0,0},old[3]={80,0,0};c.event(100,100,now,old,16,true,true,false);
        FlatOriginWitnessReport r;check(c.complete(103,r),"early producer-shift completes");
        check(r.edgeMedian[1][0]==-60 && r.edgeMedian[2][0]==0 &&
            r.verdict==FlatOriginWitnessReport::Verdict::Inconclusive,"neighbor-edge motion remains timing inconclusive");
    }
    {
        FlatOriginWitnessCore c;
        for(unsigned f=97;f<=102;++f)for(unsigned id=1;id<=5;++id)add(c,f,id,id*100.0f);
        const float now[3]={20,0,0},old[3]={80,0,0};c.event(100,101,now,old,16,true,true,false);
        FlatOriginWitnessReport r;check(c.complete(103,r),"clock-mismatch event completes");
        check(!r.clockMatch && r.worldEdgeValid && !r.cameraMinusWorldValid &&
            r.verdict==FlatOriginWitnessReport::Verdict::Inconclusive,"clock mismatch measures edge but not aligned camera residual");
    }
    {
        FlatOriginWitnessCore c;
        const float now[3]={20,0,0},old[3]={80,0,0};c.event(100,100,now,old,16,true,true,false);
        FlatOriginWitnessReport r;check(c.complete(103,r),"no-record event completes");
        check(!r.worldEdgeValid && !r.cameraMinusWorldValid && r.edgeCount[2]==0,
            "absent native edge and camera residual remain explicitly invalid");
        lines.clear();edvr::flat_origin_witness_runtime::print(r);
        check(!lines.empty() && lines[0].find("world-edge-valid=0")!=std::string::npos &&
            lines[0].find("camera-minus-world-valid=0")!=std::string::npos,"invalidity reaches actual summary log");
    }
    {
        FlatOriginWitnessCore c;
        for(unsigned f=97;f<=102;++f)for(unsigned id=1;id<=4;++id)
            add(c,f,id,float(id)*100.0f,f==100 && id==4?0x999999u:0);
        const float now[3]={20,0,0},old[3]={80,0,0};
        c.event(100,100,now,old,16,true,true,false);
        FlatOriginWitnessReport r;check(c.complete(103,r),"reuse event completes");
        check(r.preStable==4 && r.missingAt==1 && r.edgeCount[2]==3,"node reuse is a missing identity, not object motion");
    }
    {
        unsigned busy=0,lateStable=0;
        for(unsigned a=20;a<4000 && !busy;++a)for(unsigned b=a+1;b<4000;++b){
            const auto ha=identityHash(a),hb=identityHash(b);
            if((ha%FlatOriginWitnessCore::kSlots)==(hb%FlatOriginWitnessCore::kSlots)){
                busy=ha>hb?a:b;lateStable=ha>hb?b:a;break;
            }
        }
        check(busy && lateStable,"collision fixture found");
        FlatOriginWitnessCore c;
        for(unsigned f=97;f<=102;++f){
            add(c,f,busy,1000.0f+float(f));
            add(c,f,lateStable,700.0f); // lower hash rank replaces an early busy occupant
            for(unsigned id=1;id<=4;++id)add(c,f,id,id*100.0f);
        }
        const float now[3]={20,0,0},old[3]={80,0,0};c.event(100,100,now,old,16,true,true,false);
        FlatOriginWitnessReport r;check(c.complete(103,r),"collision event completes");
        bool retained=false;for(unsigned i=0;i<r.rowCount;++i)
            if(r.rows[i].record==0x10000u+lateStable*0x1000u && r.rows[i].present==0x3F)retained=true;
        check(retained && r.collisions[3]>0,"late stable witness turns over busy colliding slot");
    }
    {
        FlatOriginWitnessCore c;
        for(unsigned f=98;f<=102;++f)for(unsigned id=1;id<=4;++id)add(c,f,id,float(id)*100.0f);
        const float now[3]={20,0,0},old[3]={80,0,0};c.event(100,100,now,old,16,true,true,false);
        FlatOriginWitnessReport r;check(c.complete(103,r),"gap event completes");
        check(!r.hadFrame[0] && r.preStable==4 && r.edgeCount[2]==4,"missing -3 retains explicit presence and -2/-1 fallback");
    }
    {
        FlatOriginWitnessCore c;
        for(unsigned f=97;f<=102;++f)for(unsigned id=1;id<=4;++id){
            uint32_t p[5];pose(float(id)*100,0,0,p);c.observe(0x10000+id*0x1000,0x20000+id*0x1000,0x30000,p,f,true,true);
        }
        const float now[3]={20,0,0},old[3]={80,0,0};c.event(100,100,now,old,16,true,true,false);
        FlatOriginWitnessReport r;check(c.complete(103,r),"single-owner event completes");
        check(r.owners==1 && r.worldEdgeValid && r.cameraMinusWorldValid &&
            r.verdict==FlatOriginWitnessReport::Verdict::Inconclusive,"measured single-owner edge stays valid but static-world inference is inconclusive");
    }
    {
        FlatOriginWitnessCore c;
        for(unsigned f=97;f<=102;++f)for(unsigned id=1;id<=5;++id)add(c,f,id,80.0f+id*10.0f);
        const float now[3]={20,0,0},old[3]={80,0,0};c.event(100,100,now,old,16,true,true,false);
        FlatOriginWitnessReport r;check(c.complete(103,r),"near-only multi-owner event completes");
        check(r.owners==5 && r.farRecords==0 && r.edgeCount[2]==5 && r.edgeMedian[2][0]==0 &&
            r.worldEdgeValid && r.cameraMinusWorldValid && r.cameraMinusMedian[0]==-60 &&
            r.verdict==FlatOriginWitnessReport::Verdict::Inconclusive,
            "near-only measured zero edge and camera residual remain valid while static-world inference is inconclusive");
    }
    {
        FlatOriginWitnessCore c;
        const float shortNow[3]={20,0,0},nearPrev[3]={80,0,0};
        const float longNow[3]={20,0,0},farPrev[3]={5020,0,0};
        for(unsigned i=0;i<100;++i)c.event(i,100+i,longNow,farPrev,16,true,true,false);
        check(c.droppedLongToward()>0,"periodic long reanchors hit only long budget");
        check(c.event(1000,1000,shortNow,nearPrev,16,true,true,false),"later short inward candidate has reserved budget");
        FlatOriginWitnessReport r;check(c.complete(1003,r,true),"pending budget events flush");
    }
    {
        FlatOriginWitnessCore c;
        const float shortNow[3]={20,0,0},nearPrev[3]={80,0,0};
        for(unsigned i=0;i<20;++i)c.event(i,i,shortNow,nearPrev,16,false,false,false,"missing-resource");
        check(c.droppedUnavailableShort()>0,"unavailable-short budget reports loss");
        check(c.event(1000,1000,shortNow,nearPrev,16,true,true,false),"unavailable candidates do not starve accepted inward candidate");
    }
    {
        lines.clear();
        const float now[3]={20,0,0},old[3]={80,0,0};
        edvr::flatOriginWitnessEvent(200,now,old,16,true,true,false);
        check(!lines.empty() && lines.back().find("candidate inactive")!=std::string::npos,"inactive hook logs candidate");
        edvr::flatOriginWitnessFrameBoundary(200,true,false);
        for(unsigned f=201;f<500;++f)edvr::flatOriginWitnessFrameBoundary(f,true,false);
        bool noHook=false;for(const auto& line:lines)if(line.find("heartbeat")!=std::string::npos &&
            line.find("emit-hook=0")!=std::string::npos && line.find("callbacks=0")!=std::string::npos)noHook=true;
        check(noHook,"unconditional frame clock reports no emit callback");
        edvr::flatOriginWitnessShutdown();
    }
    {
        lines.clear();auto report=runCase(true);edvr::flat_origin_witness_runtime::print(report);
        bool stage=false,farEdges=false,valid=false;for(const auto& line:lines){
            check(line.size()<1150,"witness log fits the production 1200-byte line buffer");
            if(line.find("decision-stage=pre-backend")!=std::string::npos)stage=true;
            if(line.find("far edges:")!=std::string::npos)farEdges=true;
            if(line.find("world-edge-valid=1")!=std::string::npos &&
                line.find("camera-minus-world-valid=1")!=std::string::npos)valid=true;
        }
        check(stage && farEdges && valid,"decision stage, measured validity and far cohort printed");
    }
    {
        FlatOriginWitnessReport r;r.rowCount=1;r.rows[0].present=0x3F;
        for(unsigned i=0;i<6;++i)for(unsigned c=0;c<3;++c)r.rows[0].pos[i][c]=float(10*(i+1)+c+1);
        lines.clear();edvr::flat_origin_witness_runtime::print(r);
        bool found=false;for(const auto& line:lines)if(line.find("flat origin witness record:")!=std::string::npos)
            found=line.find("pos(-3,-2,-1,0,+1,+2)=(11,12,13)/(21,22,23)/(31,32,33)/(41,42,43)/(51,52,53)/(61,62,63)")!=std::string::npos;
        check(found,"all six distinct triplets survive the formatted record line");
    }
    {
        lines.clear();FlatOriginWitnessReport r;
        r.resolverFrame=~0ull;r.engineAtEvent=r.sampleFrame=r.skippedLock=~0u;
        std::memset(r.reason,'r',sizeof(r.reason)-1);
        const float extreme=-std::numeric_limits<float>::max();
        for(unsigned c=0;c<3;++c){r.now[c]=r.previous[c]=r.cameraDelta[c]=r.medianDelta[c]=
            r.cameraMinusMedian[c]=extreme;}
        for(unsigned e=0;e<5;++e){r.edgeCount[e]=r.edgeCoherent[e]=r.farEdgeCount[e]=r.farEdgeCoherent[e]=65535;
            for(unsigned c=0;c<3;++c)r.edgeMedian[e][c]=r.farEdgeMedian[e][c]=extreme;}
        for(unsigned i=0;i<6;++i)r.observed[i]=r.occupied[i]=r.collisions[i]=~0u;
        r.rowCount=8;for(auto& row:r.rows){row.record=row.node=row.owner=~uintptr_t(0);row.distanceM=extreme;
            row.present=row.proven=row.quatChanged=row.primary=255;
            for(auto& p:row.pos)for(float& v:p)v=extreme;}
        edvr::flat_origin_witness_runtime::print(r);
        for(const auto& line:lines)check(line.size()<1150,"worst-case witness line fits production log buffer");
    }
    std::printf("flat origin witness: self-test passed\n");
}
int main(int argc,char** argv){if(argc==2 && std::strcmp(argv[1],"--self-test")==0){selfTest();return 0;}
    if(argc==2 && std::strcmp(argv[1],"--dry-run")==0)return 0;
    std::fprintf(stderr,"usage: flat_origin_witness_test --self-test|--dry-run\n");return 2;}

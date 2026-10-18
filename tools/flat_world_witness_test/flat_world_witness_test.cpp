#include <windows.h>
#include <d3d11_1.h>
#if !defined(near) || !defined(far)
#error Keep the SDK near/far macros enabled to catch header identifier collisions.
#endif
#include "../../src/d3d11/flat_world_witness.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

static std::vector<std::string> lines;
static size_t maxLogBytes=0;
namespace edvr {
Log& Log::get(){static auto* log=new Log;return *log;}
void Log::note(const char* fmt,...){char line[1200]{};va_list ap;va_start(ap,fmt);
    const int n=std::vsnprintf(line,sizeof(line),fmt,ap);va_end(ap);
    if(n<0||n>1149){std::fprintf(stderr,"log line overflow %d: %s\n",n,line);std::abort();}
    if(static_cast<size_t>(n)>maxLogBytes)maxLogBytes=static_cast<size_t>(n);
    lines.emplace_back(line);
}
}
using namespace edvr;
static void check(bool yes,const char* message){if(!yes){std::fprintf(stderr,"FAIL %s\n",message);std::abort();}}
static FlatWorldWitnessInput sample(uint32_t frame,uintptr_t record,float x,float y,float z,
                                    uint8_t source=1,uintptr_t owner=0x3000) {
    FlatWorldWitnessInput s{};s.frame=frame;s.record=record;s.node=record+0x10000;s.owner=owner;
    s.nodeValidator=record+0x555;s.source=source;s.radius=2;s.viewMask=0xff;s.admitted=1;
    s.nameClass=2;std::strcpy(s.name,"far-world-observation");
    s.validMask=kRadius|kViewMask|kAdmitted|kNode|kName|kPoseCenter|kStoreCenter|kRawCenter;
    s.sourceValidMask=kPoseCenter|kStoreCenter|kRawCenter;
    s.bucket=0x7000;s.ownerReadValid=true;s.sourceRadius[0]=2;s.sourceRadius[1]=3;s.sourceRadius[2]=4;
    s.center[0]=x;s.center[1]=y;s.center[2]=z;
    for(unsigned j=0;j<3;++j){s.poseCenter[j]=s.center[j];s.storeCenter[j]=s.center[j]+10;s.rawCenter[j]=s.center[j]+20;}
    return s;
}
static FlatWorldWitnessReport complete(FlatWorldWitnessCore& c,uint32_t frame){
    FlatWorldWitnessReport r{};check(c.complete(frame,r),"expected completed event");return r;
}
static std::vector<uintptr_t> distinctRecords(unsigned count,bool sampled){
    std::vector<uintptr_t> records;bool used[256]{};
    for(uintptr_t id=0x10000;records.size()<count;++id){
        if(sampled&&(flat_world_witness_detail::hash(id)&63u))continue;
        const unsigned slot=flat_world_witness_detail::slotIndex(id);
        if(!used[slot]){used[slot]=true;records.push_back(id);}
    }
    return records;
}
static void sampledCoverage(){
    const auto records=distinctRecords(44,true);
    flatWorldWitnessFrameBoundary(2997,true,true,true);
    const float previous[3]={80,0,0},now[3]={20,0,0};
    for(uint32_t f=2997;f<=3002;++f){
        if(f!=2997)flatWorldWitnessFrameBoundary(f,true,true,true);
        for(auto id:records){check(flatWorldWitnessSelect(id,f),"distinct sampled record selected");
            flatWorldWitnessObserve(sample(f,id,500+float(id%100),0,0));}
        if(f==3000)flatWorldWitnessEvent(3000,now,previous,16,true,true,false);
    }
    auto r=complete(flat_world_witness_runtime::core,3003);
    for(unsigned t=0;t<6;++t)check(r.occupied[t]==44&&r.observed[t]==44,"seed32 plus hashed tail records reach diverse collector slots");
    check(r.matchedEdge[2]==44&&r.preStableFarEdge[2]==44,"real select/observe/event keeps diverse aligned cohort");
    check(flat_world_witness_runtime::seedSelected.load()==192&&flat_world_witness_runtime::tailSelected.load()==72,
        "twelve tail identities per frame survive after unfiltered prefix");
    flatWorldWitnessShutdown();
}
static void stableFarIntersection(){
    FlatWorldWitnessCore c;const auto records=distinctRecords(5,false);
    for(uint32_t f=3997;f<=4002;++f)for(unsigned i=0;i<records.size();++i){
        const float motion=i<2?0.f:float(f-3997)*60.f;
        auto s=sample(f,records[i],500+float(i)*100+motion,0,0);
        if(i==0)s.nodeValidator=0; // raw identity weakness stays visible.
        c.observe(s);
    }
    const float previous[3]={80,0,0},now[3]={20,0,0};
    check(c.event(4000,4000,now,previous,16,true,true,false),"stable far event");
    auto r=complete(c,4003);
    check(r.farEdge[2]==5&&r.farMedianEdge[2][0]==60,"moving distant records dominate raw far median");
    check(r.preStableFarEdge[2]==2&&r.preStableFarEdgeValid[2]&&r.preStableFarMedianEdge[2][0]==0,
        "observationally pre-stable distant intersection remains separately measurable");
    check(r.weakIdentity==1&&r.preStableFarWeakEdge[2]==1,"weak generation identity membership visible in intersection");
}
static void sparsePrefix(){
    flatWorldWitnessFrameBoundary(4500,true,true,true);unsigned reads=0;
    std::vector<uintptr_t> records;
    for(uintptr_t id=0x10000;records.size()<12;++id)if(flat_world_witness_detail::hash(id)&63u)records.push_back(id);
    for(auto id:records)for(unsigned repeat=0;repeat<50;++repeat)if(flatWorldWitnessSelect(id,4500))++reads;
    check(reads==12&&flat_world_witness_runtime::seedSelected.load()==12&&flat_world_witness_runtime::tailSelected.load()==0,
        "sparse non-hash records read once each despite repeated hot records");
    check(flat_world_witness_runtime::seedClaims.load()==12,"duplicate calls cannot exhaust seed prefix");
    flatWorldWitnessShutdown();
}
static void metadataRoundTrip(){
    FlatWorldWitnessCore c;
    for(unsigned t=0;t<6;++t){auto s=sample(4997+t,0x1000,500,0,0);
        s.bucket=0x7000+t*0x100;s.ownerReadValid=(t&1)!=0;
        s.viewMask=0xFEDCBA9800000000ull+t;s.radius=2.f+t;
        s.sourceValidMask=uint8_t(kPoseCenter|((t&1)?kStoreCenter:kRawCenter));
        for(unsigned a=0;a<3;++a)s.sourceRadius[a]=10.f*float(a+1)+t;
        c.observe(s);
    }
    const float previous[3]={80,0,0},now[3]={20,0,0};
    check(c.event(5000,5000,now,previous,16,true,true,false),"metadata event retained");
    auto r=complete(c,5003);check(r.rowCount==1,"metadata row retained");const auto& row=r.rows[0];
    std::string masks,buckets;
    for(unsigned t=0;t<6;++t){
        check(row.bucket[t]==0x7000+t*0x100&&row.viewMask[t]==0xFEDCBA9800000000ull+t&&row.radius[t]==2.f+t&&
            row.ownerReadValid[t]==((t&1)!=0)&&row.sourceValidMask[t]==uint8_t(kPoseCenter|((t&1)?kStoreCenter:kRawCenter)),
            "all six metadata samples preserve exact masks, radii, bucket and validity");
        char text[40]{};std::snprintf(text,sizeof(text),"%016llX",static_cast<unsigned long long>(row.viewMask[t]));
        if(t){masks+=',';buckets+=',';}masks+=text;
        std::snprintf(text,sizeof(text),"%p",reinterpret_cast<void*>(row.bucket[t]));buckets+=text;
    }
    check(row.alternativeRadius[0][0]==12&&row.alternativeRadius[1][0]==13&&
        row.alternativeRadius[0][1]==22&&row.alternativeRadius[1][1]==23&&
        row.alternativeRadius[0][2]==32&&row.alternativeRadius[1][2]==33,"source radius pairs survive independently of validity");
    lines.clear();flat_world_witness_runtime::report(r);bool found=false;
    for(const auto& line:lines)if(line.find("record metadata:")!=std::string::npos){
        found=line.find("view-mask(hex)="+masks)!=std::string::npos&&line.find("bucket="+buckets)!=std::string::npos&&
            line.find("owner-read-valid=0,1,0,1,0,1")!=std::string::npos&&line.find("selected-radius=2,3,4,5,6,7")!=std::string::npos&&
            line.find("source-valid(pose=1,store=2,raw=4)=05,03,05,03,05,03")!=std::string::npos&&
            line.find("radius(-1>0,pose/store/raw)=(12>13)/(22>23)/(32>33)")!=std::string::npos;
    }
    check(found,"formatter serializes all metadata values and explicit source/owner validity");
}
static void staticShiftAndInward(){
    FlatWorldWitnessCore c;
    for(uint32_t f=97;f<=102;++f){
        c.observe(sample(f,0x1000,f>=100?560.f:500.f,0,0,1,0x3000));
        c.observe(sample(f,0x2000,f>=100?660.f:600.f,10,0,2,0x4000));
        // Deliberately moving camera-attached observation: still retained as raw evidence.
        c.observe(sample(f,0x3000,f>=100?20.f:80.f,0,0,3,0x5000));
    }
    const float previous[3]={80,0,0},now[3]={20,0,0};
    check(c.event(100,100,now,previous,16,true,true,false),"event accepted");
    FlatWorldWitnessReport r=complete(c,103);
    check(r.clockMatch&&r.complete&&r.preStable==3,"phase and stable baseline");
    check(r.matchedEdge[2]==3&&r.farEdge[2]==2,"far cohort and attached retained");
    check(r.edgeValid[2]&&r.farEdgeValid[2],"measured edges valid");
    check(r.farMedianEdge[2][0]==60,"far common shift");
    check(r.alternativeCount[0]==3&&r.alternativeCount[1]==3&&r.alternativeCount[2]==3,"all source edges");
    check(r.rowCount==3&&r.rows[0].farFromCamera,"rows rank far observations");
    check(r.rows[0].present==0x3f,"six raw frame positions retained");
    check(r.farOwners==2,"distinct owner observations");
}
static void unchangedAndMissing(){
    FlatWorldWitnessCore c;
    for(uint32_t f=197;f<=202;++f)c.observe(sample(f,0x1000,500,0,0));
    const float previous[3]={80,0,0},now[3]={20,0,0};
    check(c.event(200,200,now,previous,16,true,false,false),"unchanged event");
    auto r=complete(c,203);check(r.edgeValid[2]&&r.medianEdge[2][0]==0,"measured zero remains valid");
    c.clear();c.observe(sample(199,0x1000,500,0,0));
    check(c.event(200,201,now,previous,16,false,false,true),"mismatch event");
    r=complete(c,203);check(!r.clockMatch&&!r.edgeValid[2]&&r.complete,"missing edge is invalid");
}
static void reuseAndSourceChange(){
    FlatWorldWitnessCore c;
    auto a=sample(299,0x1000,500,0,0);c.observe(a);
    auto b=sample(300,0x1000,550,0,0);b.nodeValidator++;c.observe(b);
    const float previous[3]={80,0,0},now[3]={20,0,0};
    check(c.event(300,300,now,previous,16,true,true,false),"reuse event");
    auto r=complete(c,303);check(!r.edgeValid[2]&&r.identityTurnover>0,"validator reuse excludes edge");
    c.clear();a=sample(399,0x1000,500,0,0,1);b=sample(400,0x1000,550,0,0,2);
    c.observe(a);c.observe(b);check(c.event(400,400,now,previous,16,true,true,false),"source event");
    r=complete(c,403);check(!r.edgeValid[2]&&r.sourceChanges>0,"source changes exclude selected edge");
    check(r.alternativeCount[0]==1&&r.alternativeMedian[0][0]==50,"same raw source still comparable");
    c.clear();a=sample(499,0x1000,500,0,0,4);b=sample(500,0x1000,550,0,0,4);
    a.validMask&=~kRadius;b.validMask&=~kRadius;a.sourceValidMask=b.sourceValidMask=0;c.observe(a);c.observe(b);
    check(c.event(500,500,now,previous,16,false,false,false),"partial source event");
    r=complete(c,503);check(!r.edgeValid[2]&&r.alternativeCount[0]==0&&r.noCenter[2]==1,
        "partial centers without radius are retained but not measured edges");
}
static void quotasAndGaps(){
    FlatWorldWitnessCore c;const float previous[3]={80,0,0},now[3]={20,0,0};
    const float longPrevious[3]={5080,0,0};
    for(unsigned i=0;i<100;++i)c.event(500+i,500+i,now,longPrevious,16,true,true,false);
    check(c.dropped(1)>0,"long inward budget exhausted");
    check(c.event(700,700,now,previous,16,true,true,false),"short inward quota reserved");
    auto r=complete(c,1000);check(!r.hadFrame[0]&&!r.edgeValid[2],"frame gaps explicit");
    c.clear();check(c.event(200,200,now,previous,16,false,false,false,"refused"),"unavailable candidate");
    r=complete(c,203);check(r.unavailable&&std::strcmp(r.reason,"refused")==0,"unavailable reason");
}
static void runtimeAndLog(){
    lines.clear();check(!flatWorldWitnessSelect(0x1000,999),"inactive hook selects nothing");
    flatWorldWitnessFrameBoundary(1000,true,false,false);
    check(!flatWorldWitnessSelect(0x1000,999),"frame mismatch rejected before read");
    uintptr_t eligible=0;
    for(uintptr_t id=0x10000;id<0x20000;++id)if((edvr::flat_world_witness_detail::hash(id)&63u)==0){eligible=id;break;}
    check(eligible&&flatWorldWitnessSelect(eligible,1000),"sampled first record");
    check(!flatWorldWitnessSelect(eligible,1000),"selected record deduplicated before read");
    unsigned guardedReads=1;
    for(unsigned i=0;i<30000;++i)if(flatWorldWitnessSelect(0x10000+i*16,1000))++guardedReads;
    check(guardedReads==128,"duplicate and cap rejection perform no guarded field read");
    check(edvr::flat_world_witness_runtime::selected.load()==128,"read selection bounded per frame");
    check(edvr::flat_world_witness_runtime::capSuppressed.load()>0,"cap pressure counted");
    check(edvr::flat_world_witness_runtime::duplicate.load()>0,"duplicate pressure counted");
    flatWorldWitnessFrameBoundary(1001,true,false,false);
    flatWorldWitnessObserve(sample(1000,eligible,1,2,3));
    check(edvr::flat_world_witness_runtime::observeStale.load()>0,"stale completed read rejected");
    for(unsigned f=1002;f<=1300;++f)flatWorldWitnessFrameBoundary(f,true,false,false);
    bool armed=false,heartbeat=false;
    for(const auto& s:lines){armed|=s.find("armed engine-frame=")!=std::string::npos;
        heartbeat|=s.find("heartbeat engine-frame=")!=std::string::npos;}
    check(armed&&heartbeat,"armed and no-hook heartbeat");
    flatWorldWitnessShutdown();
    check(!flatWorldWitnessSelect(eligible,1300),"shutdown closes selection");
    FlatWorldWitnessCore c;
    for(unsigned f=1997;f<=2002;++f){auto s=sample(f,0x1000,float(f),10000,10000);
        std::strcpy(s.name,"maximum-name-record-012345678901234567890123456789");c.observe(s);}
    const float previous[3]={80,0,0},now[3]={20,0,0};
    check(c.event(2000,2000,now,previous,16,true,true,false),"formatted event");
    auto r=complete(c,2003);
    lines.clear();edvr::flat_world_witness_runtime::report(r);
    bool six=false,alt=false,valid=false;
    for(const auto& line:lines){
        if(line.find("record: resolver-frame=")!=std::string::npos){
            six=line.find("pos(-3,-2,-1,0,+1,+2)=")!=std::string::npos;
            for(unsigned f=1997;f<=2002;++f){char triplet[100]{};std::snprintf(triplet,sizeof(triplet),"(%.9g,10000,10000)",double(f));
                six=six&&line.find(triplet)!=std::string::npos;}
        }
        alt|=line.find("record alternatives:")!=std::string::npos;
        valid|=line.find("world-edge-valid=1")!=std::string::npos;
    }
    check(six&&alt&&valid,"all six triplets, alternatives and validity formatted");
    FlatWorldWitnessReport worst{};worst.resolverFrame=UINT64_MAX;worst.engineAtEvent=UINT32_MAX;
    worst.sampleFrame=UINT32_MAX;worst.rowCount=1;worst.rows[0].record=UINTPTR_MAX;
    worst.rows[0].node=UINTPTR_MAX;worst.rows[0].owner=UINTPTR_MAX;
    worst.rows[0].nodeValidator=UINT64_MAX;worst.rows[0].distanceM=std::numeric_limits<float>::max();
    worst.rows[0].present=0x3f;std::memset(worst.rows[0].name,'X',48);worst.rows[0].name[48]=0;
    std::memset(worst.reason,'R',47);worst.reason[47]=0;
    worst.deltaMs=std::numeric_limits<float>::max();
    for(unsigned j=0;j<3;++j)worst.now[j]=worst.previous[j]=worst.cameraDelta[j]=std::numeric_limits<float>::max();
    for(unsigned t=0;t<6;++t)for(unsigned j=0;j<3;++j)worst.rows[0].pos[t][j]=std::numeric_limits<float>::max();
    for(unsigned i=0;i<5;++i){worst.matchedEdge[i]=worst.preStableEdge[i]=worst.farEdge[i]=worst.preStableFarEdge[i]=UINT16_MAX;
        worst.preStableFarEdgeValid[i]=true;worst.preStableFarWeakEdge[i]=UINT16_MAX;
        for(unsigned j=0;j<3;++j){worst.medianEdge[i][j]=std::numeric_limits<float>::max();worst.farMedianEdge[i][j]=std::numeric_limits<float>::max();
            worst.preStableFarMedianEdge[i][j]=-std::numeric_limits<float>::max();}}
    for(unsigned i=0;i<3;++i){worst.alternativeCount[i]=UINT16_MAX;
        for(unsigned j=0;j<3;++j)worst.alternativeMedian[i][j]=std::numeric_limits<float>::max();}
    worst.callbacks=worst.selected=worst.attempted=worst.duplicate=worst.claimSaturated=UINT32_MAX;
    worst.seedSelected=worst.tailSelected=UINT32_MAX;
    worst.hashSuppressed=worst.capSuppressed=worst.frameMismatch=worst.readFailures=worst.lockSkipped=worst.observeStale=UINT32_MAX;
    worst.preStable=worst.farRecords=worst.farOwners=worst.sourceChanges=worst.metadataChanges=worst.identityTurnover=worst.weakIdentity=UINT16_MAX;
    for(unsigned t=0;t<6;++t){worst.observed[t]=worst.occupied[t]=worst.collision[t]=worst.noCenter[t]=UINT32_MAX;
        worst.rows[0].bucket[t]=UINTPTR_MAX;worst.rows[0].viewMask[t]=UINT64_MAX;
        worst.rows[0].radius[t]=-std::numeric_limits<float>::max();worst.rows[0].sourceValidMask[t]=UINT8_MAX;
        worst.rows[0].ownerReadValid[t]=true;
        worst.rows[0].source[t]=worst.rows[0].validMask[t]=worst.rows[0].admitted[t]=worst.rows[0].nameClass[t]=UINT8_MAX;}
    for(auto& pair:worst.rows[0].alternativeRadius)for(float& radius:pair)radius=-std::numeric_limits<float>::max();
    for(unsigned t=0;t<5;++t)worst.sourceCount[t]=UINT32_MAX;
    for(unsigned t=0;t<8;++t)worst.fieldPresent[t]=UINT32_MAX;
    for(unsigned t=0;t<11;++t)worst.failedReason[t]=UINT32_MAX;
    lines.clear();edvr::flat_world_witness_runtime::report(worst);
}
int main(int argc,char** argv){
    if(argc==2&&std::strcmp(argv[1],"--dry-run")==0)return 0;
    if(argc!=2||std::strcmp(argv[1],"--self-test")!=0){std::fprintf(stderr,"usage: flat_world_witness_test --self-test|--dry-run\n");return 2;}
    using namespace edvr;
    check(!flatOriginCaptureEnabled(), "world capture starts off before initialization");
    const float offNow[3]={20,0,0},offOld[3]={80,0,0};
    flatWorldWitnessFrameBoundary(1,true,true,true);
    check(!flatWorldWitnessSelect(1,1), "default off refuses before world record read");
    flatWorldWitnessObserve(sample(1,1,0,0,0));
    flatWorldWitnessReadFailure(1,1);
    flatWorldWitnessEvent(1,offNow,offOld,16,true,true,false);
    flatWorldWitnessUnavailable(1,offNow,offOld,16,"off-fixture");
    flatWorldWitnessShutdown();
    check(lines.empty() && !flat_world_witness_runtime::active.load() &&
          !flat_world_witness_runtime::callbacks.load() && !flat_world_witness_runtime::core.events(),
          "default off blocks every world entry and shutdown logs");
    flatOriginCaptureInitialize(true);
    check(flatOriginCaptureEnabled(), "world witness rig explicitly opts in");
    staticShiftAndInward();unchangedAndMissing();reuseAndSourceChange();quotasAndGaps();runtimeAndLog();sampledCoverage();stableFarIntersection();sparsePrefix();metadataRoundTrip();
    std::printf("flat_world_witness_test: 9 cases passed; maximum log line %zu bytes\n",maxLogBytes);return 0;
}

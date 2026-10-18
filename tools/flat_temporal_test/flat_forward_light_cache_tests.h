#pragma once
#include "../../src/d3d11/flat_forward_light.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

namespace forward_light_cache_test {
// A COM-like handle with visible AddRef/Release counts. Keeping one reference
// to a view is what makes pointer identity safe across a later allocator reuse.
struct Object { int refs=0; };
struct Hold {
    Object* p=nullptr;
    Hold()=default;
    explicit Hold(Object* q):p(q){if(p)++p->refs;}
    Hold(const Hold& h):Hold(h.p){}
    Hold& operator=(const Hold& h){
        if(this!=&h){Object* next=h.p;if(next)++next->refs;Reset();p=next;}
        return *this;
    }
    ~Hold(){Reset();}
    Object* Get()const{return p;}
    void Reset(){if(p){--p->refs;p=nullptr;}}
};
struct Description { int resource=0, flags=0; };

inline int run() {
    unsigned checks=0;int failures=0;
    const auto expect=[&](bool ok,const char* why){++checks;if(!ok){++failures;std::printf("FAIL: forward cache %s\n",why);}};
    using Cache=edvr::forward_light::ImmutableDescriptionCache<Hold,Description>;
    Object a{},b{},depth{};
    Hold first(&a),second(&b),depthState(&depth),missing;
    Cache dsv,depthDesc;
    Description scratch{};bool hit=true;int loads=0;
    const auto loadFirst=[&](Description& value){++loads;value={11,0};return true;};
    expect(!dsv.describe(missing,scratch,hit,loadFirst)&&!hit&&!loads,
           "null binding cannot use a cache entry or invoke the metadata loader");
    const auto* initial=dsv.describe(first,scratch,hit,loadFirst);
    expect(initial&&initial->resource==11&&!hit&&loads==1&&a.refs==2,
           "first description load retains the actual view identity");
    for(int i=0;i<20;++i) {
        const auto* again=dsv.describe(first,scratch,hit,loadFirst);
        expect(again&&again->resource==11&&hit,"repeated actual view uses retained immutable description");
    }
    expect(loads==1,"production-used helper collapses repeated metadata loads");
    first.Reset();
    expect(a.refs==1&&dsv.held.Get()==&a,"cached view remains alive after getter's temporary reference is released");
    Hold firstAgain(&a);
    const auto* same=dsv.describe(firstAgain,scratch,hit,loadFirst);
    expect(same&&hit&&loads==1,"new getter reference to the same live view still hits");
    const auto loadSecond=[&](Description& value){++loads;value={22,1};return true;};
    const auto* switched=dsv.describe(second,scratch,hit,loadSecond);
    expect(switched&&switched->flags==1&&!hit&&loads==2&&a.refs==1&&b.refs==2,
           "different read-only view misses and evicts prior retained identity");
    firstAgain.Reset();
    expect(a.refs==0&&b.refs==2,"evicted address can become reusable only after all references leave");
    Hold firstReturned(&a);
    const auto* reloaded=dsv.describe(firstReturned,scratch,hit,loadFirst);
    expect(reloaded&&reloaded->flags==0&&!hit&&loads==3&&b.refs==1,
           "one-entry eviction forces a fresh description on return to the old identity");
    dsv.clear();
    const auto badLoad=[&](Description& value){++loads;value={0,9};return false;};
    for(int i=0;i<2;++i) {
        const auto* bad=dsv.describe(second,scratch,hit,badLoad);
        expect(bad==&scratch&&bad->resource==0&&!hit&&!dsv.valid,
               "failed metadata remains an uncached observation and retries on next use");
    }
    expect(loads==5&&b.refs==1,"failed observations invoke loader again without retaining the view");
    int depthLoads=0;
    const auto depthLoad=[&](Description& value){++depthLoads;value={1,0};return true;};
    Description depthScratch{};
    expect(depthDesc.describe(depthState,depthScratch,hit,depthLoad)&&!hit&&depthLoads==1&&depth.refs==2,
           "independent depth-state description cache retains its state identity");
    expect(depthDesc.describe(depthState,depthScratch,hit,depthLoad)&&hit&&depthLoads==1,
           "repeated depth state skips its GetDesc loader");
    expect(!depthDesc.describe(missing,depthScratch,hit,depthLoad)&&!hit&&depthLoads==1,
           "null depth state cannot alias a retained non-null description");
    dsv.clear();depthDesc.clear();
    expect(!dsv.find(firstReturned)&&!depthDesc.find(depthState)&&a.refs==1&&depth.refs==1,
           "work/frame reset releases both cached COM identities and descriptions");

    // The pure cache cannot prove that the production adapter asks D3D for
    // current mutable state. Pin that independent boundary at the call site.
    std::ifstream input("src/d3d11/flat_forward_light_runtime.inl",std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(input)),{});
    const auto begin=source.find("bool forwardDsvActual(State& s,");
    const auto end=source.find("bool forwardDsv(State& s,",begin);
    const std::string body=begin!=std::string::npos&&end!=std::string::npos?source.substr(begin,end-begin):std::string{};
    const auto inOrder=[&](const char* a,const char* b){const auto x=body.find(a),y=body.find(b);return x!=std::string::npos&&y!=std::string::npos&&x<y;};
    expect(inOrder("OMGetRenderTargets(0,nullptr,&current)","cache.describe(dsv,fresh,dsvHit")&&
           inOrder("cache.describe(dsv,fresh,dsvHit","dsv->GetResource(&value.resource)")&&
           body.find("return value.resource&&value.surface.known;")!=std::string::npos,
           "actual OM DSV query precedes production helper and failed metadata is not retained");
    expect(inOrder("OMGetDepthStencilState(&state,&ref)","depthCache.describe(state,freshDepth,depthHit")&&
           body.find("else {counts.add(flatcpu::kCopyRawDepthDefault);s.forwardLight.depthDescription.clear();}")!=std::string::npos&&
           body.find("dsvObservationMatches(actualProof,expected,writes)")!=std::string::npos,
           "actual depth state is queried each draw; null/default and read-only write proof remain live");
    const auto wrapperEnd=source.find("bool forwardViewport(",end);
    const std::string wrapper=end!=std::string::npos&&wrapperEnd!=std::string::npos?source.substr(end,wrapperEnd-end):std::string{};
    expect(wrapper.find("forwardDsvActual(s,k,out,dsv,resource,writes)&&dsv.Get()==k.dsv")!=std::string::npos,
           "ordinary draw observation still checks the actual DSV against its recorded binding");
    const auto issueBegin=source.find("bool forwardIssueValid(State& s,");
    const auto issueEnd=source.find("void forwardBeginIssue(",issueBegin);
    const std::string issue=issueBegin!=std::string::npos&&issueEnd!=std::string::npos?source.substr(issueBegin,issueEnd-issueBegin):std::string{};
    const auto count=[&](const std::string& haystack,const char* needle){
        unsigned n=0;for(auto at=haystack.find(needle);at!=std::string::npos;at=haystack.find(needle,at+1))++n;return n;
    };
    expect(count(body,"OMGetRenderTargets(0,nullptr,&current)")==1&&
           count(body,"observeCurrentDsv(dsv,")==1&&
           count(issue,"forwardDsvActual(s,key,observed,dsv,depth,writes)")==2&&
           count(issue,"OMGetRenderTargets(0,nullptr,&dsv)")==0&&
           issue.find("!owner()||s.prefix.uncertain||foreignWork.load")!=std::string::npos&&
           count(issue,"!plane(observed,issue.raw)")==2&&
           count(issue,"issue.phaseX!=s.phase.currentX||issue.phaseY!=s.phase.currentY")==2,
           "both issue paths keep owner, foreign, plane, phase and one fresh actual DSV query in their shared core");
    expect(source.find("forwardIssueValid(s,issue)",issueEnd)!=std::string::npos&&
           count(source,"forwardIssueValid(s,issue)")>=2&&
           count(issue,"kCopyIssueGeometryDsvGet")==1&&count(issue,"kCopyIssueLightDsvGet")==1,
           "pre- and post-draw issue checks independently re-read and count the actual DSV");
    std::ifstream cbInput("src/d3d11/flat_light_payload_runtime.inl",std::ios::binary);
    const std::string cbSource((std::istreambuf_iterator<char>(cbInput)),{});
    const auto cbBegin=cbSource.find("void lightBoundCb(State& s,");
    const auto cbEnd=cbSource.find("void lightDepth(",cbBegin);
    const std::string cbBody=cbBegin!=std::string::npos&&cbEnd!=std::string::npos?
        cbSource.substr(cbBegin,cbEnd-cbBegin):std::string{};
    expect(cbBody.find("VSGetConstantBuffers1(slot,1,&buffer,&first,&count)")!=std::string::npos&&
           cbBody.find("VSGetConstantBuffers(slot,1,&buffer)")!=std::string::npos&&
           cbBody.find("buffer->GetDesc(&desc)")!=std::string::npos&&
           cbBody.find("range(first,count,out.width,row,bytes/16)")!=std::string::npos&&
           cbBody.find("snapshotTrackedConstants(")!=std::string::npos&&
           cbBody.find("finish(out,snapshot.copied,bytes,")!=std::string::npos&&
           cbBody.find("constantsMetadata(buffer.Get())")!=std::string::npos,
           "each Geometry proof re-reads actual VS CB/range/width; diagnostic path remains available");
    expect(count(issue,"lightBoundCb(s,s.context.Get(),0,1,270,96,cb,0,true)")==1&&
           count(issue,"flatcpu::Scope shaderCost(flatcpu::kIssueShader)")==1&&
           count(issue,"flatcpu::Scope depthCost(flatcpu::kIssueDsv)")==1&&
           count(issue,"flatcpu::Scope constantsCost(flatcpu::kIssueConstants)")==1&&
           count(issue,"forwardIssueValid(s,issue)")==0,
           "only Geometry selects the tracked snapshot and exclusive callback children");
    const auto reuseAt=issue.find("geometryIssueReuse(");
    const auto shaderAt=issue.find("flatcpu::kIssueShader");
    const auto constantsAt=issue.find("flatcpu::kIssueConstants");
    const auto stampAt=issue.find("geometryCertStamp(");
    expect(reuseAt!=std::string::npos&&shaderAt!=std::string::npos&&constantsAt!=std::string::npos&&
           stampAt!=std::string::npos&&reuseAt<shaderAt&&constantsAt<stampAt,
           "reuse is decided before the shader queries and the stamp follows the constant query");
    std::ifstream runtimeInput("src/d3d11/flat_projection_runtime.cpp",std::ios::binary);
    const std::string runtimeSource((std::istreambuf_iterator<char>(runtimeInput)),{});
    const auto snapBegin=runtimeSource.find("FlatProjectionTrackedSnapshot FlatProjectionRuntime::snapshotTrackedConstants(");
    const auto snapEnd=runtimeSource.find("bool FlatProjectionRuntime::sameRecipe(",snapBegin);
    const std::string snapshot=snapBegin!=std::string::npos&&snapEnd!=std::string::npos?
        runtimeSource.substr(snapBegin,snapEnd-snapBegin):std::string{};
    expect(count(snapshot,"find(buffer)")==1&&count(snapshot,"shadows_.lookup(")==1&&
           count(snapshot,"std::memcpy(")==1&&snapshot.find("if (!owner()) return result;")!=std::string::npos&&
           snapshot.find("entry->mapped")!=std::string::npos&&snapshot.find("entry->pending")!=std::string::npos,
           "production snapshot uses one owner-guarded tracker/shadow lookup and bounded byte copy");
    using namespace edvr::forward_light;
    int actualView=0,gets=0,inspections=0;
    const int changedViews[]={3,4};
    for(int stage=0;stage<2;++stage) {
        const bool accepted=observeCurrentDsv(actualView,[&](int& bound){++gets;bound=changedViews[stage];},
            [&](const int& bound){++inspections;return bound==changedViews[stage];});
        expect(accepted&&actualView==changedViews[stage]&&gets==stage+1&&inspections==stage+1,
               "pre- and post-draw observations each acquire and inspect their own current view once");
    }
    const bool nullAccepted=observeCurrentDsv(actualView,[&](int& bound){++gets;bound=0;},
        [&](const int&){++inspections;return true;});
    expect(!nullAccepted&&gets==3&&inspections==2,
           "a null actual DSV rejects after one getter without inspecting stale metadata");
    Surface raw{};raw.resource=11;raw.width=1920;raw.height=1080;raw.slices=raw.samples=1;raw.known=true;
    DsvObservationProof proof{raw,11,3,4,5,true,true,false};
    const DsvRequirement required{11,1920,1080,3,4,5};
    bool writes=false;
    expect(dsvObservationMatches(proof,required,writes)&&writes,
           "matching current resource, format, extent and default writable depth certify the observation");
    proof.readOnlyDepth=true;
    expect(dsvObservationMatches(proof,required,writes)&&!writes,
           "a read-only view remains structurally valid but cannot claim a depth write");
    proof.readOnlyDepth=false;proof.depthEnabled=false;
    expect(dsvObservationMatches(proof,required,writes)&&!writes,
           "a bound depth state with depth disabled cannot claim a write");
    proof.depthEnabled=true;proof.resource=12;
    expect(!dsvObservationMatches(proof,required,writes),
           "an aliased view of another resource fails current raw-resource identity");
    proof.resource=11;proof.viewFormat=6;
    expect(!dsvObservationMatches(proof,required,writes),"current DSV format mismatch fails closed");
    proof.viewFormat=5;proof.surface.width=1919;
    expect(!dsvObservationMatches(proof,required,writes),"current extent mismatch fails closed");
    proof.surface.width=1920;proof.surface.known=false;
    expect(!dsvObservationMatches(proof,required,writes),"unknown texture description fails closed");
    proof.surface=raw;proof.surface.mip=1;
    expect(dsvObservationMatches(proof,required,writes)&&!plane(proof.surface,raw),
           "issue plane check independently rejects a changed raw subresource");
    const auto reset=source.find("void forwardReset(State& s,");
    expect(reset!=std::string::npos&&source.find("f.dsvDescription.clear();f.depthDescription.clear();",reset)!=std::string::npos,
           "forward lifecycle reset releases both caches");
    Surface surface{};
    surface.resource=11;surface.width=1920;surface.height=1080;surface.slices=1;surface.samples=1;surface.known=true;
    ConstantIdentity id{};
    id.buffer=0x1000;id.generation=3;id.mutation=4;id.writeGeneration=5;id.bank=6;id.writeFrame=7;
    id.first=0;id.count=8;id.width=256;id.writeQ=9;id.bytes=96;id.hash=0xABCDu;
    unsigned char camera[96]{};
    for(int i=0;i<96;++i)camera[i]=static_cast<unsigned char>(i);
    Issue matched{};
    matched.kind=Issue::Geometry;matched.frame=7;matched.vs=11;matched.ps=22;matched.depthWrite=true;
    matched.raw=surface;matched.constants[0]=id;matched.phaseX=0.25f;matched.phaseY=-0.5f;
    std::memcpy(matched.camera,camera,96);
    GeometryReuseProof reuseProof{};
    geometryCertStamp(reuseProof,matched,40,99);
    expect(geometryIssueReuse(reuseProof,matched,40,7,0.25f,-0.5f,false),"matching geometry proof reuses");
    expect(!geometryIssueReuse(reuseProof,matched,40,7,0.25f,-0.5f,true),"frozen proof never reuses");
    expect(!geometryIssueReuse(reuseProof,matched,41,7,0.25f,-0.5f,false),"epoch mismatch refuses reuse");
    matched.camera[95]^=1;
    expect(!geometryIssueReuse(reuseProof,matched,40,7,0.25f,-0.5f,false),"one camera byte refuses reuse");
    matched.camera[95]^=1;
    matched.depthWrite=false;
    expect(!geometryIssueReuse(reuseProof,matched,40,7,0.25f,-0.5f,false),"depth write flip refuses reuse");
    matched.depthWrite=true;
    matched.raw.resource=12;
    expect(!geometryIssueReuse(reuseProof,matched,40,7,0.25f,-0.5f,false),"plane mismatch refuses reuse");
    matched.raw.resource=11;
    matched.constants[0].hash^=1u;
    expect(!geometryIssueReuse(reuseProof,matched,40,7,0.25f,-0.5f,false),"constant identity mismatch refuses reuse");
    matched.constants[0].hash^=1u;
    expect(geometryObserveReuse(reuseProof,40,7,11,22,11,99,0x1000,0.25f,-0.5f,camera,false),
           "observe reuse accepts the proof's depth view, resource and buffer");
    expect(!geometryObserveReuse(reuseProof,40,7,11,22,11,98,0x1000,0.25f,-0.5f,camera,false),
           "observe reuse refuses a different depth view");
    expect(!geometryObserveReuse(reuseProof,40,7,11,22,12,99,0x1000,0.25f,-0.5f,camera,false),
           "observe reuse refuses a different depth resource");
    expect(!geometryObserveReuse(reuseProof,40,7,11,22,11,99,0x1001,0.25f,-0.5f,camera,false),
           "observe reuse refuses a different constant buffer");
    std::printf("forward light cache: %u checks, %d failed\n",checks,failures);
    return failures;
}
} // namespace forward_light_cache_test

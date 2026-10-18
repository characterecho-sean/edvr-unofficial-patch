#pragma once
#include "flat_deferred_camera.h"
#include <cstring>

namespace edvr { namespace forward_light {
constexpr uint64_t kVs=0x8289669D93A18C1Dull,kPs=0xC6E6E419DA9F6FADull;
constexpr uint64_t kDepthVs=0xDEF19B035D5EDEDCull,kDepthPs=0xCB95394B50D737D6ull;
constexpr unsigned kBytes=752,kDomains=8,kPublications=8;
enum class Reason:uint32_t {None,Never,Ownership,Uncertain,Overflow,UnknownWriter,MissingDonor,StaleDonor,
    Ambiguous,MissingProducer,PendingProducer,MutatedProducer,Lineage,Binding,Range,OriginalMissing,
    OriginalStale,OriginalChanged,Forward,Origin,View,Phase,Issue,Abandoned,Count};
inline const char* name(Reason r) {
    const char* const text[]={"accepted","never-called","upstream-ownership-required","uncertain","capacity-overflow",
        "unknown-depth-writer","missing-origin-donor","stale-donor","ambiguous-camera","missing-depth-producer",
        "producer-not-returned","producer-mutated","depth-lineage","actual-binding","constant-range",
        "original-unavailable","original-stale","original-changed","forward-mismatch","origin-mismatch",
        "used-view-mismatch","phase-mismatch","actual-issue-mismatch","abandoned-issue"};
    const auto i=static_cast<unsigned>(r);return i<static_cast<unsigned>(Reason::Count)?text[i]:"invalid";
}
// These roles certify only camera/depth coordinates. They add no motion-source support.
enum class Role:uint32_t {Unknown,Origin,ProjectionOnly};
inline Role typedRole(uint64_t vs,uint64_t ps) {
    if(vs==0x84F6596FAF22CCFAull&&!ps)return Role::ProjectionOnly;
    // Captured pool-family PS has only colour outputs; unregistered pixel
    // shaders are not generically certified by their shared vertex family.
    if(vs==0xEB5234DB6ADB491Dull&&ps==0xB7D50283329322C3ull)return Role::Origin;
    if((vs==0x989E043933A369ABull&&ps==0xBD43EB1E0AAEFC7Aull)||
       (vs==0x7B0DC42D383F694Cull&&ps==0x0DF03E64DF9DBEF1ull))return Role::Origin;
    return Role::Unknown;
}
struct Surface {
    uint64_t resource=0;uint32_t width=0,height=0,mip=0,slice=0,slices=0,samples=0;
    bool known=false;
};
// Keep the actual binding acquisition adjacent to its inspection. Each call
// observes one current view; callers must call again on the other side of Draw.
template<class View,class Get,class Inspect>
inline bool observeCurrentDsv(View& view,Get&& get,Inspect&& inspect) {
    get(view);
    return !view?false:inspect(view);
}
struct DsvObservationProof {
    Surface surface{};uint64_t resource=0;
    uint32_t viewDimension=0,resourceFormat=0,viewFormat=0;
    bool depthEnabled=false,writeAll=false,readOnlyDepth=false;
};
struct DsvRequirement {
    uint64_t resource=0;uint32_t width=0,height=0;
    uint32_t viewDimension=0,resourceFormat=0,viewFormat=0;
};
inline bool dsvObservationMatches(const DsvObservationProof& actual,const DsvRequirement& expected,bool& writes) {
    writes=actual.depthEnabled&&actual.writeAll&&!actual.readOnlyDepth;
    return actual.surface.known&&actual.viewDimension==expected.viewDimension&&
        actual.resourceFormat==expected.resourceFormat&&actual.viewFormat==expected.viewFormat&&
        actual.resource==expected.resource&&actual.surface.width==expected.width&&actual.surface.height==expected.height;
}
// One immutable resource description supplies both the structural surface
// and its format. The caller still reads actual mutable bindings/state.
struct TextureDescription {
    uint32_t width=0,height=0,mipLevels=0,arraySize=0,samples=0,format=0;
};
struct TextureObservation { Surface surface{};uint32_t format=0; };
// The caller owns the actual binding returned by a D3D getter on every use.
// Hold retains its COM identity, so a recycled address cannot become a hit.
// Only immutable descriptions belong here; mutable bindings and ledger work
// must still be observed for each draw.
template<class Hold,class Description>
struct ImmutableDescriptionCache {
    Hold held{};
    Description description{};
    bool valid=false;
    const Description* find(const Hold& actual) const {
        return valid&&actual.Get()&&held.Get()==actual.Get()?&description:nullptr;
    }
    void remember(const Hold& actual,const Description& value) {
        if(!actual.Get()){clear();return;}
        held=actual;description=value;valid=true;
    }
    template<class Load>
    const Description* describe(const Hold& actual,Description& scratch,bool& hit,Load&& load) {
        hit=false;
        if(!actual.Get())return nullptr;
        if(const auto* kept=find(actual)){hit=true;return kept;}
        if(load(scratch)){remember(actual,scratch);return &description;}
        return &scratch; // Uncertain metadata is usable only for this observation; retry next draw.
    }
    void clear(){held.Reset();description={};valid=false;}
};
template<class Describe>
inline TextureObservation describeTexture(uint64_t resource,unsigned mip,unsigned slice,unsigned slices,Describe&& describe) {
    TextureDescription d{};const bool texture=resource&&describe(d);
    if(!texture)d={};
    TextureObservation out;out.surface.resource=resource;out.surface.mip=mip;out.surface.slice=slice;out.surface.slices=slices;
    out.surface.width=d.width;out.surface.height=d.height;out.surface.samples=d.samples;out.format=d.format;
    out.surface.known=texture&&d.arraySize==1&&d.mipLevels==1&&d.samples==1&&mip==0&&slice==0&&slices==1;
    return out;
}
inline bool plane(const Surface& a,const Surface& b) {
    return a.known&&b.known&&a.resource&&a.resource==b.resource&&a.width==b.width&&a.height==b.height&&
        a.mip==b.mip&&a.slice==b.slice&&a.slices==b.slices&&a.samples==b.samples;
}
struct Source {
    uint64_t buffer=0,generation=0,bank=0,writeGeneration=0,frame=0;
    uint32_t writeQ=0,first=0,count=0;
};
inline bool sourceIdentity(const Source& a,const Source& b) {
    return a.buffer&&a.buffer==b.buffer&&a.generation==b.generation&&a.bank==b.bank&&a.first==b.first&&a.count==b.count;
}
struct Domain {
    Surface raw{};Source source{};unsigned char camera[96]{};
    uint32_t origins=0,projections=0,versions=0,lastQ=0;
    Reason failure=Reason::None;bool hasProjection=false;
};
// Returned exact-shader CPU draw input/output association, not a receipt of
// GPU execution, full-texel refresh or linear-depth metric equivalence.
struct Publication {
    Surface raw{},linear{};uint64_t token=0,frame=0;uint32_t q=0;
    bool live=false,issued=false,returned=false;
};
struct Ledger {
    uint64_t frame=0,serial=0;Domain domains[kDomains]{};Publication publications[kPublications]{};
    uint32_t used=0;bool overflow=false;
    void reset(uint64_t f=0){for(auto& d:domains)d={};for(auto& p:publications)p={};used=0;overflow=false;frame=f;}
    void begin(uint64_t f){if(frame!=f)reset(f);}
    Domain* domain(const Surface& raw) {
        for(unsigned i=0;i<used;++i)if(domains[i].raw.resource==raw.resource){
            if(!plane(domains[i].raw,raw))domains[i].failure=Reason::Lineage;return &domains[i];}
        if(!raw.known||!raw.resource)return nullptr;
        if(used==kDomains){overflow=true;return nullptr;}auto& d=domains[used++];d.raw=raw;return &d;
    }
    void observe(const Surface& raw,Role role,bool depthWrite,bool current,const Source& s,
                 const unsigned char* camera,uint32_t q) {
        auto* d=domain(raw);if(!d)return;
        if(role==Role::Unknown){if(depthWrite)d->failure=Reason::UnknownWriter;return;}
        if(!current||!camera||!s.buffer||!s.generation||s.frame!=frame||!s.writeQ||s.writeQ>q){d->failure=Reason::StaleDonor;return;}
        if(d->hasProjection&&(!sourceIdentity(d->source,s)||std::memcmp(d->camera,camera,64)))d->failure=Reason::Ambiguous;
        if(d->origins&&role==Role::Origin&&std::memcmp(d->camera,camera,96))d->failure=Reason::Ambiguous;
        if(!d->hasProjection){std::memcpy(d->camera,camera,96);d->source=s;d->hasProjection=true;}
        if(role==Role::Origin){if(!d->origins)std::memcpy(d->camera,camera,96);++d->origins;}
        else ++d->projections;
        d->source=s;d->lastQ=q;if(++d->versions==UINT32_MAX){overflow=true;d->failure=Reason::Overflow;}
    }
    void mutation(uint64_t resource,bool forgetSources=true) {
        if(forgetSources)for(auto& d:domains)if(d.hasProjection&&(!resource||d.raw.resource==resource))d.failure=Reason::StaleDonor;
        for(auto& p:publications)if(!resource||p.raw.resource==resource||p.linear.resource==resource)p.live=false;
    }
    uint64_t publish(const Surface& raw,const Surface& linear,uint32_t q) {
        mutation(linear.resource);Publication* slot=nullptr;
        for(auto& p:publications)if(p.linear.resource==linear.resource){slot=&p;break;}
        if(!slot)for(auto& p:publications)if(!p.token){slot=&p;break;}
        if(!slot){overflow=true;return 0;}*slot={raw,linear,++serial,frame,q,true,false,false};return serial;
    }
    void issued(uint64_t token){for(auto& p:publications)if(p.token==token&&p.live)p.issued=true;}
    void returned(uint64_t token){for(auto& p:publications)if(p.token==token&&p.live&&p.issued)p.returned=true;}
    void abandon(uint64_t token){for(auto& p:publications)if(p.token==token)p.live=false;}
    const Publication* find(const Surface& raw,const Surface& linear,uint32_t q,Reason& why)const {
        if(overflow){why=Reason::Overflow;return nullptr;}
        for(const auto& p:publications)if(plane(p.raw,raw)&&plane(p.linear,linear)) {
            if(!p.live){why=Reason::MutatedProducer;return nullptr;}
            if(p.frame!=frame||!p.q||p.q>=q){why=Reason::Lineage;return nullptr;}
            if(!p.returned){why=Reason::PendingProducer;return nullptr;}why=Reason::None;return &p;
        }why=Reason::MissingProducer;return nullptr;
    }
};
inline bool phaseReceipt(bool upstream,float x,float y,uint64_t applied) {
    return upstream&&std::isfinite(x)&&std::isfinite(y)&&((x==0&&y==0)||applied!=0);
}
struct ConstantIdentity {
    uint64_t buffer=0,generation=0,mutation=0,writeGeneration=0,bank=0,writeFrame=0;
    uint32_t first=0,count=0,width=0,writeQ=0,bytes=0,hash=0;
};
inline Reason constantReady(const ConstantIdentity& id,bool tracked,bool shadow,bool mapped,bool pending,
                            uint64_t frame,uint32_t q,unsigned required) {
    if(!id.buffer)return Reason::Binding;
    if(!required||required%16||uint64_t(required/16)>id.count||
       (uint64_t(id.first)*16+required)>id.width)return Reason::Range;
    if(!tracked||!shadow||mapped||pending||!id.generation)return Reason::OriginalMissing;
    if(id.writeFrame!=frame||!id.writeQ||id.writeQ>=q)return Reason::OriginalStale;
    return Reason::None;
}
inline bool same(const ConstantIdentity& a,const ConstantIdentity& b) {
    return a.buffer&&a.buffer==b.buffer&&a.generation==b.generation&&a.mutation==b.mutation&&
        a.writeGeneration==b.writeGeneration&&a.bank==b.bank&&a.writeFrame==b.writeFrame&&
        a.first==b.first&&a.count==b.count&&a.width==b.width&&a.writeQ==b.writeQ&&a.bytes==b.bytes&&a.hash==b.hash;
}
struct ConverterRaster {
    uint32_t topology=0,fill=0,cull=0,width=0,height=0,scissors=0;
    int32_t bias=0,left=0,top=0,right=0,bottom=0;
    float clamp=0,slope=0;
    bool clip=false,scissor=false,multisample=false,antialiasedLine=false,depth=false,stencil=false;
};
inline bool converterRaster(const ConverterRaster& r) {
    // D3D11 triangle list/strip, solid raster; this certifies observed draw
    // state/resource lineage, never that every GPU texel was executed.
    return (r.topology==4||r.topology==5)&&r.fill==3&&(r.cull>=1&&r.cull<=3)&&r.width&&r.height&&
        !r.bias&&r.clamp==0&&r.slope==0&&r.clip&&!r.multisample&&!r.antialiasedLine&&!r.depth&&!r.stencil&&
        (!r.scissor||(r.scissors==1&&r.left<=0&&r.top<=0&&int64_t(r.right)>=r.width&&int64_t(r.bottom)>=r.height));
}
struct CloneReceipt {
    uint64_t context=0,owner=0,originalVs=0,originalPs=0,shadowVs=0,shadowPs=0,actualVs=0,actualPs=0,patchedVs=0,patchedPs=0;
    uint32_t vsGeneration=0,psGeneration=0,currentVsGeneration=0,currentPsGeneration=0;
};
inline bool cloneReceipt(const CloneReceipt& r) {
    return r.context&&r.context==r.owner&&r.originalVs&&r.originalVs==r.shadowVs&&r.originalPs==r.shadowPs&&
        r.vsGeneration==r.currentVsGeneration&&r.psGeneration==r.currentPsGeneration&&
        r.actualVs==(r.patchedVs?r.patchedVs:r.originalVs)&&r.actualPs==(r.patchedPs?r.patchedPs:r.originalPs);
}
struct Authority {
    uint64_t frame=0,hdr=0,raw=0,linear=0,token=0;bool live=false;
    bool affected(uint64_t current,uint64_t resource)const {
        return live&&frame==current&&(!resource||resource==raw||resource==linear);
    }
};
// Output mutation demand, evaluated afresh at the current owner draw seam.
// A failed domain cannot become healthy before reset; a new domain/publication
// makes the next observation watch again. ALL live publications are retained,
// including pending/failed-domain/overflow publications. Live authority also
// watches after overflow or publication invalidation, until actually revoked.
//
// Known held issues cannot resurrect unrepresented usable state: Converter's
// issued/returned only update a still-live publication; valid Geometry return
// does not heal its domain; Light validation requires a live returned token.
// Invalid/abandoned issues only invalidate or refuse. This is a representation
// proof, NOT an assertion that callbacks cannot nest. Re-audit these callbacks
// if any Issue kind gains a state-creating/healing return path.
// Skipping a UAV mutation may retain a different NONZERO failed-domain reason;
// admission/revocation are preserved, not byte-identical refusal diagnostics.
enum class OutputWatchReason:uint32_t {UnknownState,LiveAuthority,LivePublication,HealthyRaw,NoUsableState,Count};
struct OutputWatchDecision {
    OutputWatchReason reason=OutputWatchReason::UnknownState;
    bool needed()const {return reason!=OutputWatchReason::NoUsableState;}
};
inline OutputWatchDecision outputWatch(const Ledger& ledger,const Authority (&authority)[kPublications],
        uint64_t currentFrame,bool anyLivePublication,bool stateKnown,bool unrepresentedPendingIssue) {
    // anyLivePublication is the result of the existing complete publication
    // walk immediately before this call, with no intervening ledger mutation.
    // Unknown ownership/context, an unrepresented issue or unestablished epoch
    // always asks for the actual pipeline. No binding-state cache is involved.
    if(!stateKnown||unrepresentedPendingIssue||!currentFrame||ledger.frame!=currentFrame||ledger.used>kDomains)
        return {OutputWatchReason::UnknownState};
    for(const auto& a:authority)if(a.live) {
        if(a.frame!=currentFrame)return {OutputWatchReason::UnknownState};
        return {OutputWatchReason::LiveAuthority};
    }
    if(anyLivePublication)return {OutputWatchReason::LivePublication};
    for(unsigned i=0;i<ledger.used;++i) {
        const auto& d=ledger.domains[i];
        if(!d.raw.known||!d.raw.resource||!d.raw.width||!d.raw.height||!d.raw.slices||!d.raw.samples||
           static_cast<unsigned>(d.failure)>=static_cast<unsigned>(Reason::Count))
            return {OutputWatchReason::UnknownState};
        if(d.failure==Reason::None)return {OutputWatchReason::HealthyRaw};
    }
    return {OutputWatchReason::NoUsableState};
}
struct Issue {
    enum Kind:uint32_t {None,Converter,Light,Geometry,Count};Kind kind=None;
    uint64_t token=0,frame=0,color=0,vs=0,ps=0;uint32_t q=0,linearSlot=0;
    Surface raw{},linear{};ConstantIdentity constants[2]{};
    float phaseX=0,phaseY=0;unsigned char camera[96]{};bool depthWrite=false,donated=false,issued=false,returned=false;
};
// What one fresh Geometry validation proved about the bound state. Reuse is
// allowed only when the caller still holds that epoch and this issue names
// the same shaders, depth plane, write bit, camera bytes, constant identity
// and phase. Frozen means a writer could not be hooked.
struct GeometryReuseProof {
    uint64_t epoch=0,frame=0,vs=0,ps=0,dsv=0;
    Surface raw{};ConstantIdentity constants{};
    float phaseX=0,phaseY=0;unsigned char camera[96]{};
    bool depthWrite=false,valid=false;
};
inline void geometryCertStamp(GeometryReuseProof& proof,const Issue& issue,uint64_t epoch,uint64_t dsv) {
    proof.epoch=epoch;proof.frame=issue.frame;proof.vs=issue.vs;proof.ps=issue.ps;proof.dsv=dsv;
    proof.raw=issue.raw;proof.constants=issue.constants[0];
    proof.phaseX=issue.phaseX;proof.phaseY=issue.phaseY;proof.depthWrite=issue.depthWrite;
    std::memcpy(proof.camera,issue.camera,96);proof.valid=true;
}
inline bool geometryIssueReuse(const GeometryReuseProof& proof,const Issue& issue,uint64_t epoch,uint64_t frame,
                               float phaseX,float phaseY,bool frozen) {
    if(frozen||!proof.valid||proof.epoch!=epoch||proof.frame!=frame||issue.frame!=frame||issue.kind!=Issue::Geometry)return false;
    if(proof.vs!=issue.vs||proof.ps!=issue.ps||proof.depthWrite!=issue.depthWrite)return false;
    if(proof.phaseX!=phaseX||proof.phaseY!=phaseY||issue.phaseX!=phaseX||issue.phaseY!=phaseY)return false;
    if(!plane(proof.raw,issue.raw)||!same(proof.constants,issue.constants[0]))return false;
    return std::memcmp(proof.camera,issue.camera,96)==0;
}
// Domain observation may skip its driver queries on the same proof. The draw
// key's depth view, resource and b1 have to name what the proof measured,
// because that observation runs before substitution rebinds anything.
inline bool geometryObserveReuse(const GeometryReuseProof& proof,uint64_t epoch,uint64_t frame,uint64_t vs,uint64_t ps,
                                 uint64_t depth,uint64_t dsv,uint64_t buffer,float phaseX,float phaseY,
                                 const unsigned char* camera,bool frozen) {
    if(frozen||!proof.valid||!camera||proof.epoch!=epoch||proof.frame!=frame)return false;
    if(proof.vs!=vs||proof.ps!=ps||proof.dsv!=dsv||proof.raw.resource!=depth||proof.constants.buffer!=buffer)return false;
    if(proof.phaseX!=phaseX||proof.phaseY!=phaseY)return false;
    return std::memcmp(proof.camera,camera,96)==0;
}
inline bool close(double a,double b,unsigned n=12) {
    return std::abs(a-b)<=flatDeferredFloatBound(n,std::abs(a)+std::abs(b));
}
// The engine's centered perspective builder rounds shifted +/-frustum
// endpoints BEFORE it sums their nearly cancelling center (4F3389..4F3462).
// This certifies compatibility of the USED net transform, not which camera
// branch ran. Crop/alternate effects outside this envelope remain refused.
inline double phaseUp(double x){return std::nextafter(x,std::numeric_limits<double>::infinity());}
inline double phaseDown(double x){return std::nextafter(x,-std::numeric_limits<double>::infinity());}
struct PhaseConstruction {
    double bound=0,denominatorError=0,shiftRatio=0,numeratorError=0,inputBound=0,inputBoundLow=0;
};
inline bool phaseConstructionBound(double expected,PhaseConstruction& out) {
    out={};if(!std::isfinite(expected))return false;
    if(expected==0)return true; // +/-h, zero center are exact, including signed zero.
    const double u=std::numeric_limits<float>::epsilon()/2.0;
    // Injection divides float pixel/float extent; the expected NDC helper
    // divides in double then rounds to float. D3D11 render extents are exactly
    // representable as float. Include both division roundings.
    const double input=phaseUp((2*u*std::abs(expected))/(1-u));
    const double b=phaseUp(phaseUp(std::abs(expected)+input)/2);
    if(!(b>=std::numeric_limits<float>::min()&&b<=std::numeric_limits<float>::max()))return false;
    const double ds=phaseUp(2*u*b),a=phaseUp(2*b*(1+u));
    // Sterbenz must hold AFTER endpoint rounding, not just for ideal h +/-s.
    if(!(phaseUp(phaseUp(1+a)*(1+u))<=phaseDown(2*phaseDown(1-a)*(1-u))))return false;
    const double e=phaseUp(2*u*phaseUp(1+a)),dd=phaseUp(e+phaseUp(u*phaseUp(2+e)));
    if(!(dd<2))return false;
    const double numeratorError=phaseUp(2*ds+e);
    const double quotientNumerator=phaseUp(phaseUp(numeratorError+phaseUp(2*b*dd))+
        phaseUp(u*phaseUp(4*b+numeratorError)));
    const double error=phaseUp(quotientNumerator/phaseDown(2-dd));
    out.bound=phaseUp(error+input);
    out.denominatorError=dd;out.shiftRatio=a;out.numeratorError=numeratorError;
    out.inputBound=b;out.inputBoundLow=input<std::abs(expected)?phaseDown(phaseDown(std::abs(expected)-input)/2):0;
    return std::isfinite(out.bound);
}
enum class PhaseAxisStage:uint32_t {Accepted,Condition,Unresolved,Residual};
struct PhaseAxis {
    Reason reason=Reason::View;PhaseAxisStage stage=PhaseAxisStage::Condition;
    double scale=0,coefficientBound=0,lhs=0,rhs=0,residual=0,bound=0;
    double nativeScaleLow=0,nativeScaleHigh=0,halfHeightLow=0,halfHeightHigh=0;
    bool resolved=false;int column=-1;
};
inline double phaseComponentBound(double construction,double magnitude) {
    return phaseUp(construction+phaseUp(flatDeferredFloatBound(24,magnitude)));
}
inline PhaseAxis phaseAxis(const float (&cb)[47][4],unsigned axis,double expected) {
    PhaseAxis out;if(axis>=2||!std::isfinite(expected))return out;
    double normLow=0,normHigh=0,dotMagnitude=0,composition=0,viewMagnitude=0;
    for(unsigned j=0;j<3;++j){const double r=cb[2+axis][j],w=cb[13][j],f=cb[10+axis][j];
        if(!std::isfinite(r)||!std::isfinite(w)||!std::isfinite(f))return out;
        out.scale+=(f-expected*w)*r;normLow=phaseDown(normLow+r*r);normHigh=phaseUp(normHigh+r*r);
        dotMagnitude=phaseUp(dotMagnitude+std::abs(w*r));viewMagnitude=phaseUp(viewMagnitude+std::abs(r));
        composition=phaseUp(composition+phaseUp(std::abs(f*r)+phaseUp(std::abs(expected)*std::abs(w*r))));}
    const double fmin=std::numeric_limits<float>::min(),fmax=std::numeric_limits<float>::max();
    if(!(out.scale>=fmin&&out.scale<=fmax)||!std::isfinite(out.scale)||!(normLow>0))return out;
    const float native=static_cast<float>(out.scale),reciprocal=1.0f/native;
    if(!std::isnormal(native)||!std::isnormal(reciprocal))return out;
    PhaseConstruction construction;if(!phaseConstructionBound(expected,construction))return out;
    out.coefficientBound=construction.bound;
    const double u=std::numeric_limits<float>::epsilon()/2.0,g=flatDeferredFloatBound(24,1);
    // Outward compatible native-scale interval. Stored R is only approximately
    // unit: fit=S*||R||^2 + delta*dot(W,R) + composition error, not simply S.
    const double du=std::numeric_limits<double>::epsilon()/2;
    const double fitRound=phaseUp((8*du/(1-8*du))*composition);
    const double compositionUnderflow=phaseUp((24*static_cast<double>(std::numeric_limits<float>::denorm_min()))*viewMagnitude);
    const double uncertainty=phaseUp(phaseUp(phaseUp(out.coefficientBound*dotMagnitude)+phaseUp(g*composition))+
        phaseUp(compositionUnderflow+fitRound));
    out.nativeScaleLow=phaseDown(phaseDown(out.scale-uncertainty)/phaseUp(normHigh*(1+g)));
    out.nativeScaleHigh=phaseUp(phaseUp(out.scale+uncertainty)/phaseDown(normLow*(1-g)));
    if(!(out.nativeScaleLow>=fmin&&out.nativeScaleHigh<=fmax))return out;
    const double dd=construction.denominatorError;
    out.halfHeightLow=phaseDown(phaseDown(2*(1-u)/out.nativeScaleHigh)/phaseUp(2+dd));
    out.halfHeightHigh=phaseUp(phaseUp(2*(1+u)/out.nativeScaleLow)/phaseDown(2-dd));
    const double lo=out.halfHeightLow,hi=out.halfHeightHigh,a=construction.shiftRatio;
    // Refuse unless the WHOLE compatible interval keeps the modeled native
    // reciprocal, doubled half-height and rounded endpoints normal/finite.
    if(!(phaseDown(phaseDown(lo*phaseDown(1-a))*(1-u))>=fmin&&
         phaseUp(phaseUp(hi*phaseUp(1+a))*(1+u))<=fmax&&phaseUp(2*hi)<=fmax))return out;
    if(expected!=0){
        const double b=construction.inputBound,bLow=construction.inputBoundLow;
        // Shift, cancelled numerator and quotient must remain in the normal
        // regime used by the relative-error proof. No underflow guesswork.
        if(!(phaseDown(phaseDown(lo*(2*bLow))*(1-u))>=fmin&&phaseUp(phaseUp(hi*(2*b))*(1+u))<=fmax&&
             phaseDown(lo*phaseDown(4*bLow-construction.numeratorError))>=fmin&&
             phaseDown(std::abs(expected)-out.coefficientBound)>=fmin))return out;
    }
    out.reason=Reason::Phase;out.stage=PhaseAxisStage::Unresolved;out.resolved=expected==0;
    double extra[4]{};
    for(unsigned c=0;c<4;++c){
        if(!std::isfinite(cb[10+axis][c])||!std::isfinite(cb[13][c])||!std::isfinite(cb[2+axis][c]))return out;
        // The fitted scale also contains delta*dot(W,R); propagate that
        // contamination as well as the direct coefficient error, all4columns.
        extra[c]=expected==0?0:phaseUp(out.coefficientBound*phaseUp(std::abs(double(cb[13][c]))+
            phaseUp(std::abs(double(cb[2+axis][c]))*dotMagnitude)));
        if(c<3&&std::abs(expected*cb[13][c])>phaseComponentBound(extra[c],
            std::abs(double(cb[10+axis][c]))+std::abs(out.scale*cb[2+axis][c])))out.resolved=true;
    }
    if(!out.resolved)return out;
    out.stage=PhaseAxisStage::Residual;
    for(unsigned c=0;c<4;++c){const double lhs=double(cb[10+axis][c])-expected*cb[13][c],rhs=out.scale*cb[2+axis][c];
        const double bound=phaseComponentBound(extra[c],std::abs(double(cb[10+axis][c]))+std::abs(expected*cb[13][c])+std::abs(rhs));
        if(std::abs(lhs-rhs)>bound){out.column=static_cast<int>(c);out.lhs=lhs;out.rhs=rhs;out.residual=lhs-rhs;out.bound=bound;return out;}}
    out.reason=Reason::None;out.stage=PhaseAxisStage::Accepted;return out;
}
// Used view2..4, forward10..13 only. Unused inverse-view6..9 supplies no authority.
// Scope: centered net-transform compatibility. Crop/alternate effects outside
// the proven arithmetic envelope refuse; no selected camera branch is inferred.
// The original raster matrix must carry exactly this frame's upstream phase.
inline Reason match(const unsigned char* cameraBytes,const unsigned char* cbBytes,const FlatProjectionJitter& phase) {
    if(!cameraBytes||!cbBytes||!flat_projection_detail::finite(phase))return Reason::OriginalMissing;
    float camera[6][4],cb[47][4];std::memcpy(camera,cameraBytes,sizeof(camera));std::memcpy(cb,cbBytes,sizeof(cb));
    for(const auto& row:camera)for(float x:row)if(!std::isfinite(x))return Reason::OriginalMissing;
    for(unsigned r:{2u,3u,4u,10u,11u,12u,13u})for(float x:cb[r])if(!std::isfinite(x))return Reason::OriginalMissing;
    if(camera[0][2]!=0||camera[1][2]!=0||camera[2][2]!=0||camera[3][0]!=0||camera[3][1]!=0||
       camera[3][3]!=0||camera[3][2]<=0||camera[4][0]!=camera[0][3]||camera[4][1]!=camera[1][3]||camera[4][2]!=camera[2][3])return Reason::Forward;
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<3;++c)if(!close(cb[10+r][c],camera[c][r],8))return Reason::Forward;
    if(cb[12][3]!=camera[3][2])return Reason::Forward;
    for(unsigned r:{0u,1u,3u}) {
        double residual=cb[10+r][3],mag=std::abs(residual);
        for(unsigned c=0;c<3;++c){const double t=double(camera[c][r])*camera[5][c];residual+=t;mag+=std::abs(t);}
        if(std::abs(residual)>flatDeferredFloatBound(8,mag))return Reason::Origin;
    }
    for(unsigned r=0;r<3;++r) {
        double center=cb[2+r][3],mag=std::abs(center);
        for(unsigned c=0;c<3;++c){const double t=double(cb[2+r][c])*camera[5][c];center+=t;mag+=std::abs(t);}
        if(std::abs(center)>flatDeferredFloatBound(12,mag))return Reason::Origin;
        for(unsigned j=0;j<3;++j){double dot=0,terms=1;
            for(unsigned c=0;c<3;++c){double v=double(cb[2+r][c])*cb[2+j][c];dot+=v;terms+=std::abs(v);}
            if(std::abs(dot-(r==j?1:0))>flatDeferredFloatBound(16,terms))return Reason::View;}
        if(!close(cb[13][r],cb[4][r]))return Reason::View;
    }
    if(!close(cb[13][3],cb[4][3]))return Reason::View;
    const double determinant=double(cb[2][0])*(double(cb[3][1])*cb[4][2]-double(cb[3][2])*cb[4][1])-
        double(cb[2][1])*(double(cb[3][0])*cb[4][2]-double(cb[3][2])*cb[4][0])+
        double(cb[2][2])*(double(cb[3][0])*cb[4][1]-double(cb[3][1])*cb[4][0]);
    if(!close(determinant,1,32))return Reason::View;
    const double shift[]={phase.ndcX,phase.ndcY};
    for(unsigned r=0;r<2;++r){const auto axis=phaseAxis(cb,r,shift[r]);if(axis.reason!=Reason::None)return axis.reason;}
    return Reason::None;
}
} } // namespace edvr::forward_light

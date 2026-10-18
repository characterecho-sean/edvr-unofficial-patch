#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <type_traits>

namespace edvr { namespace lightaudit {
constexpr uint32_t kVersion=1,kDeclaredBytes=47*16,kCameraBytes=96;
constexpr uint32_t kDonors=64,kInputs=5,kWriters=128,kAttempts=4;
constexpr uint64_t kSpacing=91,kLifetime=900,kVs=0x8289669D93A18C1Dull,kPs=0xC6E6E419DA9F6FADull;
enum Status:uint32_t { Missing,RangeInvalid,Untracked,Mapped,Pending,NoShadow,CopyFailed,Changed,Full };
enum Freshness:uint32_t { Unobserved,Current,Older,Later };
struct Stamp {
    uint64_t generation=0,mutation=0,writeGeneration=0,bank=0,writeFrame=0;
    uint32_t writeQ=0,width=0,tracked=0,mapped=0,pending=0,shadow=0;
};
inline bool same(const Stamp& a,const Stamp& b) {
    return a.generation==b.generation&&a.mutation==b.mutation&&a.writeGeneration==b.writeGeneration&&
        a.bank==b.bank&&a.writeFrame==b.writeFrame&&a.writeQ==b.writeQ&&a.width==b.width&&
        a.tracked==b.tracked&&a.mapped==b.mapped&&a.pending==b.pending&&a.shadow==b.shadow;
}
inline Freshness freshness(const Stamp& s,uint64_t frame,uint32_t q) {
    if(!s.writeFrame||!s.writeQ)return Unobserved;
    if(s.writeFrame>frame||(s.writeFrame==frame&&s.writeQ>q))return Later;
    return s.writeFrame==frame?Current:Older;
}
inline bool range(uint32_t first,uint32_t count,uint32_t width,uint32_t row,uint32_t rows) {
    const uint64_t end=(uint64_t(first)+row+rows)*16;
    return rows&&uint64_t(row)+rows<=count&&end<=width&&end<=UINT32_MAX;
}
struct Cb {
    uint64_t buffer=0,context=0,bindingGeneration=0;
    uint32_t stage=0,slot=0,first=0,count=0,width=0,rangeKnown=0,row=0;
    Stamp before{},after{};
    uint32_t status=Missing,fresh=Unobserved,bytes=0,byteOffset=0;
    unsigned char payload[kDeclaredBytes]{};
};
inline void finish(Cb& c,bool copied,uint32_t requested,uint64_t frame,uint32_t q) {
    c.fresh=freshness(c.before,frame,q);c.bytes=0;
    c.status=!c.buffer?Missing:!c.rangeKnown||!requested||requested%16||requested>kDeclaredBytes||!range(c.first,c.count,c.width,c.row,requested/16)?RangeInvalid:
        !c.before.tracked?Untracked:c.before.mapped?Mapped:c.before.pending?Pending:
        !c.before.shadow?NoShadow:c.before.width!=c.width?Changed:!copied?CopyFailed:!same(c.before,c.after)?Changed:Full;
    if(c.status==Full)c.bytes=requested;
}
struct Effective {
    Cb observed{}; // pre-forward CPU-known bytes, never GPU readback
    uint64_t planOriginal=0,planReplacement=0,planRevision=0,preparedRevision=0;
    uint32_t planFirst=0,planCount=0,planReady=0,association=0,expectedBytes=0;
    unsigned char expected[kDeclaredBytes]{};
};
struct DepthSurface {
    uint64_t resource=0,view=0,color=0,rtv=0;
    uint32_t viewDimension=0,viewFormat=0,flags=0,mip=0,firstSlice=0,slices=0;
    uint32_t width=0,height=0,format=0,mips=0,array=0,samples=0,quality=0,known=0,bindingMatches=0;
};
struct Donor {
    uint64_t frame=0,color=0,depth=0,dsv=0,rtv=0,b1=0,vs=0,ps=0,cameraHash=0;
    uint64_t uploadFrame=0;
    uint32_t q=0,uploadQ=0,kind=0,width=0,height=0,format=0,depthFormat=0;
    uint32_t supported=0,poolFamily=0,current=0,rangeMatches=0;
    unsigned char camera[kCameraBytes]{};
    DepthSurface depthSurface{};
    Cb original{}; // first six rows of payload are frozen original camera rows
};
inline bool donorSame(const Donor& a,const Donor& b) {
    return a.color==b.color&&a.depth==b.depth&&a.dsv==b.dsv&&a.b1==b.b1&&a.vs==b.vs&&a.ps==b.ps&&
        a.cameraHash==b.cameraHash&&a.uploadFrame==b.uploadFrame&&a.uploadQ==b.uploadQ&&
        a.original.first==b.original.first&&a.original.count==b.original.count&&same(a.original.before,b.original.before);
}
struct Producer {
    uint64_t resource=0,depth=0,dsv=0,vs=0,ps=0,frame=0,view=0;
    uint32_t firstQ=0,lastQ=0,draws=0,format=0,width=0,height=0,bad=0,depthWrite=0;
    uint32_t targetSlot=0,viewDimension=0,mip=0,firstSlice=0,slices=0,rangeKnown=0;
};
struct Input {
    uint64_t view=0,resource=0;
    uint32_t stage=0,slot=0,dimension=0,format=0,first=0,count=0,mips=0,slices=0;
    uint32_t width=0,height=0,samples=0,array=0,stride=0,contentsCaptured=0;
    uint32_t bound=0,descriptorKnown=0,rangeKnown=0,resourceAlias=0,subresourceAlias=0,producerKnown=0;
    Producer producer{};
    uint64_t ledgerFrame=0;
    uint32_t ledgerQ=0,ledgerKnown=0;
};
struct Writer { uint64_t vs=0,ps=0,depth=0,dsv=0;uint32_t q=0,format=0; };
struct Sample {
    uint64_t generation=0,frame=0,context=0,h=0,depth=0,dsv=0,rtv=0;
    uint32_t q=0,width=0,height=0,priorWrites=0,actualShaders=0,uncertain=0;
    uint32_t drawKind=0,count=0,start=0,instances=0,startInstance=0;
    int32_t base=0;
    uint32_t indirect=0,indirectKnown=0,preForward=0,drawReturned=0,abandoned=0;
    uint64_t preForwardVs=0,preForwardPs=0;
    uint32_t upstreamOwnerBefore=0,upstreamOwnerPreForward=0;
    float phase[2]{};DepthSurface depthSurface{};
    Cb original[2]{};Effective effective[2]{};
    uint32_t donorCount=0,donorOverflow=0,inputCount=kInputs,writerCount=0,writerOverflow=0;
    uint32_t noCameraSameDepth=0,mutation=0,handoff=0,handoffQ=0,lateWrites=0;
    uint32_t producerOverflow=0,writeEvictions=0;
    Donor donors[kDonors]{};Input inputs[kInputs]{};Writer writers[kWriters]{};
};
inline bool bytesComplete(const Sample& s) {
    return s.actualShaders&&s.original[0].bytes==kDeclaredBytes&&s.original[1].bytes==kDeclaredBytes;
}
// Record completeness is useful offline coverage, never camera ownership or admission proof.
inline bool recordComplete(const Sample& s) {
    if(!bytesComplete(s)||s.priorWrites||s.uncertain||s.donorOverflow||s.writerOverflow||s.producerOverflow||s.writeEvictions||s.mutation||
       !s.preForward||s.preForwardVs!=kVs||s.preForwardPs!=kPs||!s.drawReturned||s.abandoned||!s.handoff||s.lateWrites)return false;
    unsigned candidates=0;
    for(unsigned i=0;i<s.donorCount;++i)if(s.donors[i].current&&(s.donors[i].supported||s.donors[i].poolFamily)) {
        ++candidates;if(!s.donors[i].rangeMatches||s.donors[i].original.bytes!=kCameraBytes||
            !s.donors[i].depthSurface.known||!s.donors[i].depthSurface.bindingMatches)return false;
    }
    if(!candidates||!s.depthSurface.known||!s.depthSurface.bindingMatches)return false; // all versions remain serialized; no numeric or unique-donor choice
    for(const auto& cb:s.original)if(cb.fresh!=Current)return false;
    for(const auto& cb:s.effective)if(!cb.association||
        (cb.observed.bytes!=kDeclaredBytes&&!(cb.association==2&&cb.expectedBytes==kDeclaredBytes)))return false;
    // All declared shader inputs must be observed. Buffer contents and execution remain uncaptured.
    for(const auto& input:s.inputs) {
        if(!input.bound||!input.descriptorKnown||!input.rangeKnown||!input.resource)return false;
        if(input.stage==0) {if(input.stride!=48)return false;continue;}
        if(!input.producerKnown||!input.producer.rangeKnown||input.producer.frame!=s.frame||input.producer.lastQ>=s.q||input.producer.bad)return false;
        if(input.ledgerKnown&&(input.ledgerFrame>input.producer.frame||
            (input.ledgerFrame==input.producer.frame&&input.ledgerQ>=input.producer.lastQ)))return false;
        if(input.resourceAlias&&(!input.subresourceAlias||!input.producer.depthWrite))return false;
        if(!input.resourceAlias&&(!input.producer.rangeKnown||input.first!=input.producer.mip||input.count!=1||
            input.mips!=input.producer.firstSlice||input.slices!=input.producer.slices))return false;
    }
    return true;
}
struct Collector {
    bool armed=false,open=false,pending=false,done=false;
    uint64_t generation=0,armFrame=0,historyFrame=0,lastAttempt=0;
    uint32_t attempts=0,good=0,written=0,writeFailures=0,historyCount=0,historyOverflow=0;
    Donor history[kDonors]{};Sample sample{};
    void arm(uint64_t frame,uint64_t gen) { *this={};armed=true;generation=gen;armFrame=frame; }
    bool expired(uint64_t frame)const {return armed&&frame>=armFrame&&frame-armFrame>=kLifetime;}
    bool active()const {return armed&&!done&&attempts<kAttempts&&good<2;}
    void frame(uint64_t f) { if(historyFrame!=f){historyFrame=f;historyCount=historyOverflow=0;} }
    bool donorNeeded(const Donor& d) const {
        for(unsigned i=0;i<historyCount;++i)if(donorSame(history[i],d))return false;
        return true;
    }
    void donor(const Donor& d) {
        if(!active())return;frame(d.frame);if(!donorNeeded(d))return;
        if(historyCount==kDonors){++historyOverflow;return;}history[historyCount++]=d;
    }
    bool begin(uint64_t f,uint32_t q,uint64_t h,uint64_t depth) {
        if(!active()||open||pending||!h||(attempts&&(f<=lastAttempt||f-lastAttempt<kSpacing)))return false;
        frame(f);sample={};sample.generation=generation;sample.frame=f;sample.q=q;sample.h=h;sample.depth=depth;
        sample.donorOverflow=historyOverflow;
        for(unsigned i=0;i<historyCount;++i)if(history[i].depth==depth)sample.donors[sample.donorCount++]=history[i];
        open=true;lastAttempt=f;return true;
    }
    void writer(uint64_t frame,uint64_t h,const Writer& w) {
        if(!open||sample.frame!=frame||sample.h!=h)return;
        if(sample.handoff&&w.q>sample.handoffQ)++sample.lateWrites;
        if(sample.writerCount==kWriters){++sample.writerOverflow;return;}sample.writers[sample.writerCount++]=w;
    }
    void writtenResource(uint64_t resource) {if(open&&(sample.h==resource||sample.depth==resource))sample.mutation=1;}
    void finishFrame() {if(!open)return;open=false;pending=true;++attempts;}
    void saved(bool ok) {if(!pending)return;pending=false;if(ok){++written;if(recordComplete(sample))++good;}else++writeFailures;if(!active())done=true;}
};
struct WireHeader {char magic[8]={'E','D','V','R','L','C','A','1'};uint32_t version=kVersion,bytes=sizeof(Sample);uint64_t checksum=0;};
static_assert(std::is_trivially_copyable<Sample>::value,"wire snapshot must own all payload bytes");
inline uint64_t hash(const void* data,size_t bytes) {
    const auto* p=static_cast<const unsigned char*>(data);uint64_t h=14695981039346656037ull;
    for(size_t i=0;i<bytes;++i)h=(h^p[i])*1099511628211ull;return h;
}
inline std::vector<unsigned char> encode(const Sample& s) {
    WireHeader h;h.checksum=hash(&s,sizeof(s));std::vector<unsigned char> out(sizeof(h)+sizeof(s));
    std::memcpy(out.data(),&h,sizeof(h));std::memcpy(out.data()+sizeof(h),&s,sizeof(s));return out;
}
inline bool decode(const void* bytes,size_t count,Sample& out) {
    if(!bytes||count!=sizeof(WireHeader)+sizeof(Sample))return false;WireHeader h;
    std::memcpy(&h,bytes,sizeof(h));const auto* p=static_cast<const unsigned char*>(bytes)+sizeof(h);
    if(std::memcmp(h.magic,"EDVRLCA1",8)||h.version!=kVersion||h.bytes!=sizeof(Sample)||h.checksum!=hash(p,sizeof(Sample)))return false;
    Sample s;std::memcpy(&s,p,sizeof(s));
    if(s.donorCount>kDonors||s.inputCount!=kInputs||s.writerCount>kWriters)return false;
    const auto valid=[](const Cb& c,unsigned full){return c.status<=Full&&c.fresh<=Later&&
        ((c.status==Full&&c.bytes==full)||(c.status!=Full&&!c.bytes));};
    for(const auto& c:s.original)if(!valid(c,kDeclaredBytes))return false;
    for(unsigned i=0;i<s.donorCount;++i)if(!valid(s.donors[i].original,kCameraBytes))return false;
    for(const auto& c:s.effective)if(!valid(c.observed,kDeclaredBytes)||(c.expectedBytes&&c.expectedBytes!=kDeclaredBytes)||c.association>2)return false;
    out=s;return true;
}
} } // namespace edvr::lightaudit

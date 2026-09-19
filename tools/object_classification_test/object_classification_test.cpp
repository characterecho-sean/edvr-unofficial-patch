#include "../../src/d3d11/object_classification_probe.h"
#include "../../src/d3d11/object_record_writer_hook.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0;
void check(bool ok,const char* what){++checks;if(!ok){std::printf("FAIL: %s\n",what);std::exit(1);}}
void hr(HRESULT value,const char* what){if(FAILED(value)){std::printf("FAIL: %s (0x%08lX)\n",what,(unsigned long)value);std::exit(1);}}
struct Device {
    ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> c;
    Device(){D3D_FEATURE_LEVEL level{};hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,&level,&c),"WARP device");}
    ComPtr<ID3D11Buffer> buffer(UINT bytes,UINT bind,D3D11_USAGE usage,const void* data=nullptr,UINT stride=0){
        D3D11_BUFFER_DESC x{};x.ByteWidth=bytes;x.BindFlags=bind;x.Usage=usage;x.StructureByteStride=stride;
        if(stride)x.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        if(usage==D3D11_USAGE_DYNAMIC)x.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        if(usage==D3D11_USAGE_STAGING){x.BindFlags=0;x.CPUAccessFlags=D3D11_CPU_ACCESS_READ;}
        D3D11_SUBRESOURCE_DATA s{};s.pSysMem=data;ComPtr<ID3D11Buffer> out;
        hr(d->CreateBuffer(&x,data&&usage!=D3D11_USAGE_STAGING?&s:nullptr,&out),"buffer");return out;
    }
    ComPtr<ID3D11Texture2D> texture(UINT w,UINT h,DXGI_FORMAT format,const void* data,UINT pitch,UINT bind=D3D11_BIND_SHADER_RESOURCE){
        D3D11_TEXTURE2D_DESC x{};x.Width=w;x.Height=h;x.MipLevels=x.ArraySize=x.SampleDesc.Count=1;x.Format=format;x.BindFlags=bind;
        D3D11_SUBRESOURCE_DATA s{};s.pSysMem=data;s.SysMemPitch=pitch;ComPtr<ID3D11Texture2D> out;
        hr(d->CreateTexture2D(&x,data?&s:nullptr,&out),"texture");return out;
    }
};
template<class T>std::vector<uint8_t> bytes(const std::vector<T>& v){const auto* p=reinterpret_cast<const uint8_t*>(v.data());return {p,p+v.size()*sizeof(T)};}
void put32(std::vector<uint8_t>& v,size_t at,uint32_t value){check(at+4<=v.size(),"fixture put32 range");memcpy(v.data()+at,&value,4);}
void put64(std::vector<uint8_t>& v,size_t at,uint64_t value){check(at+8<=v.size(),"fixture put64 range");memcpy(v.data()+at,&value,8);}
std::array<uint32_t,16> key(uint32_t base){std::array<uint32_t,16> x{};for(uint32_t i=0;i<16;++i)x[i]=base+i;return x;}
void makeRecord(uint8_t* record,const std::array<uint32_t,16>& k,const std::vector<uint8_t>& pool,size_t poolAt,uint32_t firstRecord,uint32_t instances,uint32_t poolIndex){
    memset(record,0,240);memcpy(record,k.data(),64);memcpy(record+64,pool.data()+poolAt+28,4);
    memcpy(record+68,pool.data()+poolAt+320,4);memcpy(record+80,pool.data()+poolAt+4,24);
    memcpy(record+116,&firstRecord,4);memcpy(record+120,&instances,4);memcpy(record+124,&poolIndex,4);
}
std::vector<uint8_t> readFile(const std::wstring& p){FILE* f=nullptr;check(_wfopen_s(&f,p.c_str(),L"rb")==0&&f,"open generated file");fseek(f,0,SEEK_END);long n=ftell(f);check(n>=0,"generated file size");rewind(f);std::vector<uint8_t> out(static_cast<size_t>(n));check(fread(out.data(),1,out.size(),f)==out.size(),"read generated file");check(fclose(f)==0,"close generated file");return out;}
std::wstring joined(const wchar_t* dir,const wchar_t* file){std::wstring p=dir;if(!p.empty()&&p.back()!=L'\\'&&p.back()!=L'/')p+=L'\\';p+=file;return p;}

extern "C" void sourceOwnerUnwindStub(uintptr_t owner);
extern "C" void sourceOwnerUnwindResume();
extern "C" void recordWriterUnwindStub(uintptr_t dictionary,uintptr_t key,const void* record);
extern "C" void recordWriterUnwindResume();
extern "C" void kinematicOwnerDirectUnwindStub(uintptr_t outer,uintptr_t dictionary,uintptr_t key,const void* record);
extern "C" void kinematicOwnerVirtualUnwindStub(uintptr_t outer,uintptr_t dictionary,uintptr_t key,const void* record);
extern "C" void recordWriterOwnershipUnwindResume();
extern "C" void kinematicOwnerDirectUnwindResume();
extern "C" void kinematicOwnerVirtualUnwindResume();
uintptr_t unwindOwner=0;uint32_t unwindFrames=0;std::string unwindStatus;
extern "C" __declspec(noinline) void sourceOwnerTestCapture(){
    edvr::ObjectSourceOwnerProbe::recoverOwnerAt(reinterpret_cast<uintptr_t>(&sourceOwnerUnwindResume),unwindOwner,unwindFrames,unwindStatus);
}

edvr::ObjectRecordWriterProbe* writerUnwindProbe=nullptr;
edvr::ObjectRecordWriterProbe::Pending writerUnwindPending{};
uintptr_t writerOwnershipFixtureReturn=0;uint64_t writerOwnershipGameReturn=0;
extern "C" __declspec(noinline) void recordWriterUnwindCapture(uintptr_t dictionary,uintptr_t key){
    CONTEXT context{};RtlCaptureContext(&context);
    writerUnwindPending=writerUnwindProbe->beginLookupUnwindRvaForTest(
        reinterpret_cast<uintptr_t>(&recordWriterUnwindResume),0x369CE91,dictionary,key,context);
}
extern "C" __declspec(noinline) void recordWriterOwnershipUnwindCapture(uintptr_t dictionary,uintptr_t key){
    CONTEXT context{};RtlCaptureContext(&context);
    writerUnwindPending=writerUnwindProbe->beginLookupOwnershipUnwindRvaForTest(
        reinterpret_cast<uintptr_t>(&recordWriterOwnershipUnwindResume),0x43130AA,
        writerOwnershipFixtureReturn,writerOwnershipGameReturn,dictionary,key,context);
}

struct KinematicFixture {
    std::vector<uint8_t> outer=std::vector<uint8_t>(0x460),collection=std::vector<uint8_t>(0x700);
    std::vector<uint8_t> context=std::vector<uint8_t>(0xC0),records=std::vector<uint8_t>(0x2F0);
    std::vector<uint8_t> gameObject=std::vector<uint8_t>(0x80),descriptor=std::vector<uint8_t>(0x80);
    std::vector<uint8_t> provider=std::vector<uint8_t>(0x80),parent=std::vector<uint8_t>(0x80);
    uintptr_t registry=0;
    explicit KinematicFixture(uint32_t parentToken=UINT32_MAX,bool parentPresent=true){
        const uintptr_t module=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        registry=reinterpret_cast<uintptr_t>(collection.data()+0x80);
        put64(outer,0,module+0x1000);put64(gameObject,0,module+0x1010);put64(descriptor,0,module+0x1020);
        put64(provider,0,module+0x1030);put64(parent,0,module+0x1040);
        put64(outer,0x20,reinterpret_cast<uintptr_t>(gameObject.data()));
        put64(outer,0x50,reinterpret_cast<uintptr_t>(descriptor.data()));
        put64(outer,0x180,reinterpret_cast<uintptr_t>(provider.data()));
        put32(outer,0x1B8,parentToken);put64(outer,0x1C0,parentPresent?reinterpret_cast<uintptr_t>(parent.data()):0);
        put64(outer,0x348,reinterpret_cast<uintptr_t>(collection.data()));
        put64(collection,0x280,reinterpret_cast<uintptr_t>(records.data()));put64(collection,0x298,1);
        put64(collection,0x2E0,registry);put64(context,0x30,registry);put64(context,0xB0,0xB0B0B0B0ull);
        put64(records,0x290,reinterpret_cast<uintptr_t>(context.data()));
    }
    uintptr_t directOwner() const{return reinterpret_cast<uintptr_t>(collection.data()+0x300);}
    uintptr_t directDictionary() const{return directOwner()+0x260;}
    uintptr_t directKey() const{return reinterpret_cast<uintptr_t>(records.data()+0x250);}
    uintptr_t inlineOwner() const{return registry+0x78;}
    uintptr_t inlineDictionary() const{return inlineOwner()+0x260;}
};

struct OwnerGraph {
    std::vector<uint8_t> nested=std::vector<uint8_t>(0x1A8),wrapper=std::vector<uint8_t>(0x148),strideOwner=std::vector<uint8_t>(0x18),group=std::vector<uint8_t>(0x150),leaf=std::vector<uint8_t>(0xD8),link=std::vector<uint8_t>(0x18);
    std::vector<uint64_t> groups{1},leaves{1};
    std::vector<uint8_t> first,second,source=std::vector<uint8_t>(336);
    explicit OwnerGraph(ID3D11Resource* resource,uint32_t firstCount=1,bool secondEntry=false):
        first(size_t(firstCount)*0x48),second(secondEntry?0x50:0) {
        put64(nested,0x100,uint64_t(reinterpret_cast<uintptr_t>(strideOwner.data())));
        put64(nested,0x130,0x130130130ull);put64(nested,0x138,0x138138138ull);
        put64(nested,0x140,uint64_t(reinterpret_cast<uintptr_t>(wrapper.data())));
        put64(nested,0x198,1);groups[0]=uint64_t(reinterpret_cast<uintptr_t>(group.data()));
        put64(nested,0x1A0,uint64_t(reinterpret_cast<uintptr_t>(groups.data())));
        put64(wrapper,0x140,uint64_t(reinterpret_cast<uintptr_t>(resource)));put32(strideOwner,0x10,336);
        put64(group,0x140,1);leaves[0]=uint64_t(reinterpret_cast<uintptr_t>(leaf.data()));put64(group,0x148,uint64_t(reinterpret_cast<uintptr_t>(leaves.data())));
        put64(leaf,0xC0,0);put64(leaf,0xC8,1);put64(leaf,0xD0,uint64_t(reinterpret_cast<uintptr_t>(link.data())));put64(link,0x10,uint64_t(reinterpret_cast<uintptr_t>(nested.data())));
        put64(leaf,0x8,firstCount);put64(leaf,0x10,uint64_t(reinterpret_cast<uintptr_t>(first.data())));
        for(uint32_t i=0;i<firstCount;++i){put64(first,size_t(i)*0x48+8,uint64_t(reinterpret_cast<uintptr_t>(source.data())));put32(first,size_t(i)*0x48+0x38,0);put32(first,size_t(i)*0x48+0x3C,i?0:1);}
        if(secondEntry){put64(leaf,0x20,1);put64(leaf,0x28,uint64_t(reinterpret_cast<uintptr_t>(second.data())));put64(second,8,uint64_t(reinterpret_cast<uintptr_t>(source.data())));}
        for(size_t i=0;i<source.size();++i)source[i]=uint8_t(i*13+7);
    }
};

void recordWriterTests(){
    check(edvr::objectRecordWriterHookSelfTest()==0,"record-writer relay publishes original before gate and forwards exactly once");
    edvr::ObjectRecordWriterProbe p;p.armForTest(77);
    std::vector<uint8_t> owner(0x300),keyBytes(0x60),stack(0x400),object(0x1A0),entry(0x38);
    for(size_t i=0;i<keyBytes.size();++i)keyBytes[i]=uint8_t(0x20+i);
    for(size_t i=0;i<object.size();++i)object[i]=uint8_t(0x80+i);
    for(size_t i=0;i<entry.size();++i)entry[i]=uint8_t(0xE0+i);
    auto fillRecord=[](uint8_t* at,uint8_t seed){for(size_t i=0;i<edvr::ObjectRecordWriterProbe::kRecordBytes;++i)at[i]=uint8_t(seed+i*3);};
    const uintptr_t dictionary=reinterpret_cast<uintptr_t>(owner.data()+0x260);
    CONTEXT c{};
    std::vector<uint8_t> unwindRecord(0x150);fillRecord(unwindRecord.data(),0x71);writerUnwindProbe=&p;writerUnwindPending={};
    recordWriterUnwindStub(dictionary,reinterpret_cast<uintptr_t>(keyBytes.data()),unwindRecord.data());
    check(writerUnwindPending.record!=edvr::ObjectRecordWriterProbe::kNone&&p.recordsForTest().back().record.data==unwindRecord,"real unwind stops at writer caller and captures its stack record");
    p.completeLookup(writerUnwindPending,reinterpret_cast<uintptr_t>(entry.data()));
    fillRecord(stack.data()+0x50,1);
    auto a=p.beginLookupRvaForTest(0x369CE91,reinterpret_cast<uintptr_t>(stack.data()),dictionary,reinterpret_cast<uintptr_t>(keyBytes.data()),c);
    p.completeLookup(a,reinterpret_cast<uintptr_t>(entry.data()));
    check(p.recordsForTest().back().writer=="direct_369ce91"&&p.recordsForTest().back().record.data[7]==uint8_t(1+7*3),"direct writer captures caller-stack record");
    std::vector<uint8_t> primaryStack(0x300),builder(0x60),pose(0xC0);fillRecord(primaryStack.data()+0x30,2);c.R12=reinterpret_cast<DWORD64>(pose.data());
    auto b=p.beginLookupRvaForTest(0x42B42EF,reinterpret_cast<uintptr_t>(primaryStack.data()),dictionary,reinterpret_cast<uintptr_t>(builder.data()+0x40),c);p.completeLookup(b,reinterpret_cast<uintptr_t>(entry.data()));
    check(p.recordsForTest().back().builder==reinterpret_cast<uintptr_t>(builder.data())&&p.recordsForTest().back().builderSnapshot.data.size()==0x60&&p.recordsForTest().back().objectSnapshot.data.size()==0xC0,"primary writer retains builder and complete context allocation");
    std::vector<uint8_t> inlineFrame(0x400);uintptr_t rbp=reinterpret_cast<uintptr_t>(inlineFrame.data()+0x80);fillRecord(reinterpret_cast<uint8_t*>(rbp+0x1C0),3);put64(inlineFrame,0x18,reinterpret_cast<uintptr_t>(pose.data()));c.Rbp=rbp;
    auto d=p.beginLookupRvaForTest(0x42B4ED6,0,dictionary,reinterpret_cast<uintptr_t>(keyBytes.data()),c);p.completeLookup(d,reinterpret_cast<uintptr_t>(entry.data()));
    check(p.recordsForTest().back().object==reinterpret_cast<uintptr_t>(pose.data())&&p.recordsForTest().back().record.data[0]==3,"inline writer reads spilled outer input and RBP record");
    std::vector<uint8_t> directStack(0x300);fillRecord(directStack.data()+0x60,4);
    auto e=p.beginLookupRvaForTest(0x43130AA,reinterpret_cast<uintptr_t>(directStack.data()),dictionary,reinterpret_cast<uintptr_t>(keyBytes.data()),c);p.completeLookup(e,reinterpret_cast<uintptr_t>(entry.data()));
    std::vector<uint8_t> helperRecord(0x150);fillRecord(helperRecord.data(),5);c.Rbx=reinterpret_cast<DWORD64>(helperRecord.data());c.Rdi=reinterpret_cast<DWORD64>(object.data());
    auto f=p.beginLookupRvaForTest(0x434D149,0,dictionary,reinterpret_cast<uintptr_t>(keyBytes.data()),c);p.completeLookup(f,reinterpret_cast<uintptr_t>(entry.data()));
    check(p.recordsForTest().back().objectSnapshot.data.size()==0x1A0&&p.recordsForTest().back().lookupStatus=="complete","helper writer retains bounded input and completed entry");
    p.beginLookupRvaForTest(0x434E316,0,dictionary,reinterpret_cast<uintptr_t>(keyBytes.data()),c);p.beginLookupRvaForTest(0x123456,0,dictionary,reinterpret_cast<uintptr_t>(keyBytes.data()),c);
    const uint64_t uploadCutoff=p.sampleUploadCutoff();
    std::vector<uint8_t> lateStack(0x200);fillRecord(lateStack.data()+0x50,8);auto late=p.beginLookupRvaForTest(0x369CE91,reinterpret_cast<uintptr_t>(lateStack.data()),dictionary,reinterpret_cast<uintptr_t>(keyBytes.data()),c);p.completeLookup(late,reinterpret_cast<uintptr_t>(entry.data()));
    p.noteUpload(3,4,5,uploadCutoff);const auto s=p.summary();check(s.observed==7&&s.stored==7&&s.completed==7&&s.declinedManagement==1&&s.declinedUnknown==1,"all five writer recipes accepted and management/unknown callers declined");
    check(p.uploadsForTest().back().cutoff==12&&p.recordsForTest().back().completionSequence>p.uploadsForTest().back().cutoff,"Map-entry cutoff excludes a writer completed during source-owner traversal");
    std::vector<uint8_t> pendingStack(0x200);fillRecord(pendingStack.data()+0x50,9);auto pending=p.beginLookupRvaForTest(0x369CE91,reinterpret_cast<uintptr_t>(pendingStack.data()),dictionary,reinterpret_cast<uintptr_t>(keyBytes.data()),c);
    p.finish();check(p.recordsForTest().back().lookupStatus=="pending"&&p.summary().completed+1==p.summary().stored,"finish preserves an explicit incomplete lookup");
    p.armForTest(78);p.completeLookup(pending,reinterpret_cast<uintptr_t>(entry.data()));check(p.summary().completionFailures==1&&p.summary().stored==0,"stale pending completion is rejected by epoch");p.finish();

    edvr::ObjectRecordWriterProbe ownershipProbe;ownershipProbe.armForTest(90);writerUnwindProbe=&ownershipProbe;
    KinematicFixture rig;std::vector<uint8_t> ownershipRecord(0x150);fillRecord(ownershipRecord.data(),0x51);
    writerOwnershipFixtureReturn=reinterpret_cast<uintptr_t>(&kinematicOwnerDirectUnwindResume);writerOwnershipGameReturn=0x431B212;
    kinematicOwnerDirectUnwindStub(reinterpret_cast<uintptr_t>(rig.outer.data()),rig.directDictionary(),rig.directKey(),ownershipRecord.data());
    check(writerUnwindPending.record!=edvr::ObjectRecordWriterProbe::kNone&&ownershipProbe.recordsForTest().back().ownershipStatus=="linked","real nested unwind restores direct ancestor RBX/RDI and links ownership");
    ownershipProbe.completeLookup(writerUnwindPending,reinterpret_cast<uintptr_t>(entry.data()));
    check(ownershipProbe.ownershipsForTest().back().branch=="direct_4321940"&&ownershipProbe.ownershipsForTest().back().ancestorReturnRva==0x431B212,"direct ancestor return identifies 4321940 branch");
    writerOwnershipFixtureReturn=reinterpret_cast<uintptr_t>(&kinematicOwnerVirtualUnwindResume);writerOwnershipGameReturn=0x431B21F;
    kinematicOwnerVirtualUnwindStub(reinterpret_cast<uintptr_t>(rig.outer.data()),rig.directDictionary(),rig.directKey(),ownershipRecord.data());
    ownershipProbe.completeLookup(writerUnwindPending,reinterpret_cast<uintptr_t>(entry.data()));
    check(ownershipProbe.ownershipsForTest().back().branch=="virtual_50"&&ownershipProbe.ownershipsForTest().back().ancestorReturnRva==0x431B21F,"virtual ancestor return identifies slot-50 branch");

    std::vector<uint8_t> ownedInlineFrame(0x400);uintptr_t ownedRbp=reinterpret_cast<uintptr_t>(ownedInlineFrame.data()+0x80);
    fillRecord(reinterpret_cast<uint8_t*>(ownedRbp+0x1C0),0x61);put64(ownedInlineFrame,0x18,reinterpret_cast<uintptr_t>(rig.context.data()));
    CONTEXT inlineCaller{};inlineCaller.Rbp=ownedRbp;CONTEXT ownerContext{};ownerContext.Rdi=reinterpret_cast<DWORD64>(rig.outer.data());ownerContext.Rbx=reinterpret_cast<DWORD64>(rig.collection.data());
    auto ownedInline=ownershipProbe.beginLookupOwnershipRvaForTest(0x42B4ED6,0,rig.inlineDictionary(),rig.directKey(),inlineCaller,0x431B21F,ownerContext,3);
    ownershipProbe.completeLookup(ownedInline,reinterpret_cast<uintptr_t>(entry.data()));const auto inlineOwnershipId=ownershipProbe.recordsForTest().back().ownershipId;
    check(ownershipProbe.recordsForTest().back().ownershipStatus=="linked"&&ownershipProbe.ownershipsForTest()[inlineOwnershipId].contextSnapshot.data.size()==0xC0,"inline ownership proves registry join and captures context tail");
    auto duplicate=ownershipProbe.beginLookupOwnershipRvaForTest(0x42B4ED6,0,rig.inlineDictionary(),rig.directKey(),inlineCaller,0x431B21F,ownerContext,3);
    check(ownershipProbe.recordsForTest().back().ownershipId==inlineOwnershipId&&ownershipProbe.ownershipSummary().deduplicated==1,"identical ownership provenance deduplicates");
    ownershipProbe.completeLookup(duplicate,reinterpret_cast<uintptr_t>(entry.data()));
    std::vector<uint8_t> secondContext(0xC0);put64(secondContext,0x30,rig.registry);put64(ownedInlineFrame,0x18,reinterpret_cast<uintptr_t>(secondContext.data()));
    auto distinct=ownershipProbe.beginLookupOwnershipRvaForTest(0x42B4ED6,0,rig.inlineDictionary(),rig.directKey(),inlineCaller,0x431B21F,ownerContext,3);
    check(ownershipProbe.recordsForTest().back().ownershipId!=inlineOwnershipId,"same owner and frame keeps distinct inline contexts");ownershipProbe.completeLookup(distinct,reinterpret_cast<uintptr_t>(entry.data()));
    put64(ownedInlineFrame,0x18,reinterpret_cast<uintptr_t>(rig.context.data()));
    std::vector<uint8_t> provider2(0x80);put64(provider2,0,reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+0x1050);put64(rig.outer,0x180,reinterpret_cast<uintptr_t>(provider2.data()));
    auto metadataChanged=ownershipProbe.beginLookupOwnershipRvaForTest(0x42B4ED6,0,rig.inlineDictionary(),rig.directKey(),inlineCaller,0x431B21F,ownerContext,3);
    check(ownershipProbe.recordsForTest().back().ownershipId!=inlineOwnershipId,"changed outer metadata cannot reuse first ownership snapshot");ownershipProbe.completeLookup(metadataChanged,reinterpret_cast<uintptr_t>(entry.data()));
    put64(rig.outer,0x180,reinterpret_cast<uintptr_t>(rig.provider.data()));
    put32(rig.outer,0x1B8,UINT32_MAX);put64(rig.outer,0x1C0,0);auto absent=ownershipProbe.beginLookupOwnershipRvaForTest(0x42B4ED6,0,rig.inlineDictionary(),rig.directKey(),inlineCaller,0x431B21F,ownerContext,3);
    check(ownershipProbe.ownershipsForTest()[ownershipProbe.recordsForTest().back().ownershipId].parentStatus=="resolved_absent","resolved absent parent is explicit");ownershipProbe.completeLookup(absent,reinterpret_cast<uintptr_t>(entry.data()));
    put32(rig.outer,0x1B8,7);put64(rig.outer,0x1C0,reinterpret_cast<uintptr_t>(rig.parent.data()));auto unresolved=ownershipProbe.beginLookupOwnershipRvaForTest(0x42B4ED6,0,rig.inlineDictionary(),rig.directKey(),inlineCaller,0x431B21F,ownerContext,3);
    check(ownershipProbe.ownershipsForTest()[ownershipProbe.recordsForTest().back().ownershipId].parentStatus=="unresolved","non-sentinel parent token is unresolved");ownershipProbe.completeLookup(unresolved,reinterpret_cast<uintptr_t>(entry.data()));
    put64(rig.records,0x290,0);CONTEXT directCaller{};auto nullContext=ownershipProbe.beginLookupOwnershipRvaForTest(0x43130AA,reinterpret_cast<uintptr_t>(directStack.data()),rig.directDictionary(),rig.directKey(),directCaller,0x431B212,ownerContext,2);
    const auto& nullOwner=ownershipProbe.ownershipsForTest()[ownershipProbe.recordsForTest().back().ownershipId];check(nullOwner.context==0&&nullOwner.contextSnapshot.status=="null_pointer","direct record without optional inline context retains outer ownership");ownershipProbe.completeLookup(nullContext,reinterpret_cast<uintptr_t>(entry.data()));
    put64(rig.records,0x290,reinterpret_cast<uintptr_t>(rig.context.data()));
    const auto beforeFault=ownershipProbe.ownershipSummary();CONTEXT badOwner=ownerContext;badOwner.Rdi=1;
    ownershipProbe.beginLookupOwnershipRvaForTest(0x42B4ED6,0,rig.inlineDictionary(),rig.directKey(),inlineCaller,0x431B21F,badOwner,1);
    put64(rig.collection,0x280,reinterpret_cast<uintptr_t>(rig.records.data()+0x10));ownershipProbe.beginLookupOwnershipRvaForTest(0x43130AA,reinterpret_cast<uintptr_t>(directStack.data()),rig.directDictionary(),rig.directKey(),directCaller,0x431B212,ownerContext,2);
    check(ownershipProbe.ownershipSummary().tupleReadFault==beforeFault.tupleReadFault+1&&ownershipProbe.ownershipSummary().recordRangeMismatch==beforeFault.recordRangeMismatch+1,"ownership faults and direct range refusal are explicit");
    ownershipProbe.beginLookupRvaForTest(0x434D149,0,dictionary,reinterpret_cast<uintptr_t>(keyBytes.data()),c);check(ownershipProbe.recordsForTest().back().ownershipStatus=="unsupported_writer","unsupported ownership recipe preserves writer record");
    ownershipProbe.finish();writerUnwindProbe=nullptr;

    edvr::ObjectRecordWriterProbe capProbe;capProbe.armForTest(1);put64(rig.collection,0x280,reinterpret_cast<uintptr_t>(rig.records.data()));
    for(uint32_t i=0;i<=edvr::ObjectRecordWriterProbe::kOwnershipCap;++i){capProbe.setFrame(i);capProbe.beginLookupOwnershipRvaForTest(0x42B4ED6,0,rig.inlineDictionary(),rig.directKey(),inlineCaller,0x431B21F,ownerContext,1);}
    check(capProbe.ownershipSummary().stored==edvr::ObjectRecordWriterProbe::kOwnershipCap&&capProbe.ownershipSummary().recordOverflow==1,"ownership record cap is explicit");capProbe.finish();
}

void sourceOwnerTests(Device& x,const wchar_t* directory){
    const uintptr_t sentinel=sizeof(uintptr_t)==8?uintptr_t(0x123456789ABCDEF0ull):uintptr_t(0x12345678u);
    sourceOwnerUnwindStub(sentinel);check(unwindStatus=="matched"&&unwindOwner==sentinel&&unwindFrames>0,"real unwind recovers target-frame RBX before caller unwind");
    check(!edvr::ObjectSourceOwnerProbe::opcodeMatchesAtBaseForTest(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))),"wrong callsite opcode rejected");

    std::vector<uint8_t> poolData(672,0x44),idsData(8,0);auto pool=x.buffer(672,D3D11_BIND_SHADER_RESOURCE,D3D11_USAGE_DYNAMIC,poolData.data(),336);auto ids=x.buffer(8,D3D11_BIND_VERTEX_BUFFER,D3D11_USAGE_DYNAMIC,idsData.data());
    edvr::ObjectClassificationProbe identity;identity.arm(1);identity.noteDraw(x.c.Get(),1,0,0,1,pool.Get(),ids.Get(),0,nullptr);D3D11_MAPPED_SUBRESOURCE rejectMap{};hr(x.c->Map(pool.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&rejectMap),"identity fixture map");identity.noteMap(x.c.Get(),pool.Get(),0,D3D11_MAP_WRITE_DISCARD,S_OK,true);check(identity.sourceOwnerForTest().attempts().size()==1&&identity.sourceOwnerForTest().attempts()[0].status=="identity_mismatch"&&identity.sourceOwnerForTest().summary().readFaults==0,"wrong executable identity fails before owner pointer reads");x.c->Unmap(pool.Get(),0);identity.noteUnmap(x.c.Get(),pool.Get(),0);

    edvr::ObjectClassificationProbe p;p.arm(100);edvr::objectRecordWriterProbe.armForTest(100);auto drawKey=key(0x5150);p.noteDraw(x.c.Get(),100,0,0,1,pool.Get(),ids.Get(),0,drawKey.data(),0xEB5234DB6ADB491Dull);
    p.setFrame(101);D3D11_MAPPED_SUBRESOURCE m{};hr(x.c->Map(pool.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&m),"owner fixture pool map");OwnerGraph graph(pool.Get());
    std::vector<uint8_t> writerStack(0x400),writerEntry(0x38);uintptr_t writerRbp=reinterpret_cast<uintptr_t>(writerStack.data()+0x80);memcpy(reinterpret_cast<void*>(writerRbp+0x1C0),graph.source.data(),graph.source.size());
    KinematicFixture writerRig;put64(writerStack,0x18,reinterpret_cast<uintptr_t>(writerRig.context.data()));CONTEXT writerContext{};writerContext.Rbp=writerRbp;CONTEXT writerOwner{};writerOwner.Rdi=reinterpret_cast<DWORD64>(writerRig.outer.data());writerOwner.Rbx=reinterpret_cast<DWORD64>(writerRig.collection.data());
    auto writerPending=edvr::objectRecordWriterProbe.beginLookupOwnershipRvaForTest(0x42B4ED6,0,writerRig.inlineDictionary(),writerRig.directKey(),writerContext,0x431B21F,writerOwner,3);
    edvr::objectRecordWriterProbe.completeLookup(writerPending,reinterpret_cast<uintptr_t>(writerEntry.data()));
    const uint32_t synthetic=p.noteMapSourceOwnerForTest(x.c.Get(),pool.Get(),0,D3D11_MAP_WRITE_DISCARD,reinterpret_cast<uintptr_t>(graph.nested.data()));check(synthetic==0,"one-to-one synthetic Map owner capture");
    memcpy(poolData.data(),graph.source.data(),graph.source.size());memcpy(m.pData,poolData.data(),poolData.size());x.c->Unmap(pool.Get(),0);p.noteUnmap(x.c.Get(),pool.Get(),0);
    const auto& success=p.sourceOwnerForTest().attempts()[synthetic];check(success.status=="captured"&&success.descriptors.size()==1&&success.descriptors[0].status=="captured","synthetic graph captures one source descriptor");
    const auto& sourceBlob=p.sourceOwnerForTest().blobs()[success.descriptors[0].payloadBlob];check(sourceBlob.data==graph.source,"captured CPU source bytes are exact");
    p.noteDraw(x.c.Get(),101,0,0,1,pool.Get(),ids.Get(),0,drawKey.data(),0xEB5234DB6ADB491Dull);p.noteDraw(x.c.Get(),102,0,0,1,pool.Get(),ids.Get(),0,drawKey.data(),0xEB5234DB6ADB491Dull);p.noteDraw(x.c.Get(),103,0,0,1,pool.Get(),ids.Get(),0,drawKey.data(),0xEB5234DB6ADB491Dull);
    std::vector<float> scene(1,0.5f);auto sceneTex=x.texture(1,1,DXGI_FORMAT_R32_FLOAT,scene.data(),4);struct Float2{float x,y;};Float2 coverage{1,0.5f};auto coverageTex=x.texture(1,1,DXGI_FORMAT_R32G32_FLOAT,&coverage,8,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE);
    std::vector<uint8_t> mesh(240);makeRecord(mesh.data(),drawKey,poolData,0,0,1,0);auto meshBuffer=x.buffer(UINT(mesh.size()),D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,D3D11_USAGE_DEFAULT,mesh.data(),240);uint32_t colour=0xFF406080;auto colourTex=x.texture(1,1,DXGI_FORMAT_R8G8B8A8_UNORM,&colour,4);
    p.stage(x.c.Get(),103,0,701,sceneTex.Get(),coverageTex.Get(),meshBuffer.Get(),1,colourTex.Get());check(p.sealed(),"source-owner fixture stages selected visible record");p.useExpectedExecutableIdentityForTest();check(p.write(x.c.Get(),directory,L"source_fixture"),"write source-owner fixture");
    const auto sourceJson=readFile(joined(directory,L"classification_source_fixture.json"));const std::string sourceText(sourceJson.begin(),sourceJson.end());check(sourceText.find("edvr_object_classification_v2")!=std::string::npos&&sourceText.find("nested_owner_field_130")!=std::string::npos&&sourceText.find("inline_42b4ed6")!=std::string::npos&&sourceText.find("\"ownerships\":[{")!=std::string::npos&&sourceText.find("\"ownership_status\":\"linked\"")!=std::string::npos,"source-owner fixture publishes kinematic ownership and exact record-writer join");

    edvr::ObjectSourceOwnerProbe bad;OwnerGraph wrong(reinterpret_cast<ID3D11Resource*>(uintptr_t(0x1234)));
    bad.captureSynthetic(0,0,1,1,0,pool.Get(),672,reinterpret_cast<uintptr_t>(wrong.nested.data()));check(bad.attempts().back().status=="resource_mismatch","wrong wrapped native resource rejected");
    bad.captureSynthetic(1,0,1,1,0,pool.Get(),672,1);check(bad.attempts().back().status=="nested_unreadable","malformed owner pointer guarded");
    OwnerGraph empty(pool.Get());put64(empty.leaf,0xC8,0);put64(empty.leaf,0xD0,1);bad.captureSynthetic(2,0,1,1,0,pool.Get(),672,reinterpret_cast<uintptr_t>(empty.nested.data()));check(bad.attempts().back().status=="captured"&&bad.attempts().back().leaves.size()==1,"empty leaf skips unused owner link and descriptors");
    OwnerGraph range(pool.Get());put32(range.first,0x3C,3);bad.captureSynthetic(3,0,1,1,0,pool.Get(),672,reinterpret_cast<uintptr_t>(range.nested.data()));check(bad.attempts().back().status=="partial"&&bad.attempts().back().descriptors[0].status=="range_overflow","destination range outside leaf is explicit partial");
    OwnerGraph capped(pool.Get(),edvr::ObjectSourceOwnerProbe::kDescriptorCap,true);bad.captureSynthetic(4,0,1,1,0,pool.Get(),672,reinterpret_cast<uintptr_t>(capped.nested.data()));const auto& cap=bad.attempts().back();check(cap.status=="partial"&&cap.descriptorCount==edvr::ObjectSourceOwnerProbe::kDescriptorCap+1ull&&cap.descriptorsScanned==edvr::ObjectSourceOwnerProbe::kDescriptorCap&&bad.summary().descriptorOverflow==1,"descriptor cap reports omitted later list");
    for(uint32_t i=bad.summary().attemptsStored;i<edvr::ObjectSourceOwnerProbe::kAttemptCap+1;++i)bad.captureSynthetic(10+i,0,1,1,0,pool.Get(),672,1);
    check(bad.summary().attemptsStored==edvr::ObjectSourceOwnerProbe::kAttemptCap&&bad.summary().attemptOverflow==1,"attempt cap is explicit");
}

void stateTests(Device& x){
    std::vector<uint8_t> poolData(336,0x11),idData(16,0x22);auto pool=x.buffer(UINT(poolData.size()),D3D11_BIND_SHADER_RESOURCE,D3D11_USAGE_DYNAMIC,poolData.data(),336);auto ids=x.buffer(UINT(idData.size()),D3D11_BIND_VERTEX_BUFFER,D3D11_USAGE_DYNAMIC,idData.data());
    edvr::ObjectClassificationProbe p;p.arm(0);p.noteDraw(x.c.Get(),0,0,0,1,pool.Get(),ids.Get(),0,key(1).data());
    check(p.discoveryFrame()==0&&p.active(),"frame zero is valid discovery");
    D3D11_MAPPED_SUBRESOURCE mapped{};hr(x.c->Map(pool.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"write map");p.setFrame(1);p.noteMap(x.c.Get(),pool.Get(),0,D3D11_MAP_WRITE_DISCARD,S_OK,mapped.pData!=nullptr);memset(mapped.pData,0x33,poolData.size());x.c->Unmap(pool.Get(),0);p.noteUnmap(x.c.Get(),pool.Get(),0);
    check(p.observedWriteCount()==1&&p.writeCount()==1,"writable Map and Unmap form one complete epoch");
    p.noteWrite(x.c.Get(),pool.Get(),edvr::ObjectClassificationProbe::Update);p.noteWrite(x.c.Get(),pool.Get(),edvr::ObjectClassificationProbe::CopyResource,ids.Get());
    check(p.observedWriteCount()==3,"update and copy each advance generation");
    const auto nonempty=p.observedWriteCount();p.noteWrite(x.c.Get(),pool.Get(),edvr::ObjectClassificationProbe::Update,nullptr,0,12,12);check(p.observedWriteCount()==nonempty,"known empty writes do not advance generation");
    p.setFrame(3);p.noteDraw(x.c.Get(),3,1,0,1,pool.Get(),ids.Get(),0,key(2).data());check(p.drawCount()==0,"eye one cannot select");
    p.noteDraw(x.c.Get(),3,0,0,1,pool.Get(),ids.Get(),0,key(2).data());check(p.selectedFrame()==3&&p.drawCount()==1,"three discovery frames precede selection");
    const auto before=p.summary();p.unknownWrites();p.noteDraw(x.c.Get(),3,0,1,1,pool.Get(),ids.Get(),0,key(3).data());const auto after=p.summary();
    check(after.snapshots>=before.snapshots+2,"unknown writes force distinct resource snapshots");
    check(after.writes==before.writes+2,"unknown writes recorded for both nominated resources");
    p.foreignWrite();p.noteDraw(x.c.Get(),3,0,2,1,pool.Get(),ids.Get(),0,key(4).data());check(p.summary().foreignWrites==1&&p.summary().snapshots>=after.snapshots+2,"foreign epoch prevents snapshot reuse");
    p.stage(x.c.Get(),2,0,7,nullptr,nullptr,nullptr,0);p.stage(x.c.Get(),3,1,7,nullptr,nullptr,nullptr,0);check(p.summary().stageRejects==2,"wrong frame and eye stage rejected");
    const ULONG heldBefore=pool->AddRef();pool->Release();p.reset();const ULONG heldAfter=pool->AddRef();pool->Release();check(heldAfter<heldBefore,"reset releases retained resources");

    auto staging=x.buffer(64,0,D3D11_USAGE_STAGING);edvr::ObjectClassificationProbe read;read.arm(0);read.noteDraw(x.c.Get(),0,0,0,1,staging.Get(),ids.Get(),0,nullptr);
    hr(x.c->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"read map");read.noteMap(x.c.Get(),staging.Get(),0,D3D11_MAP_READ,S_OK,mapped.pData!=nullptr);x.c->Unmap(staging.Get(),0);read.noteUnmap(x.c.Get(),staging.Get(),0);
    check(read.observedWriteCount()==0,"read-only Map and Unmap do not advance generation");

    edvr::ObjectClassificationProbe open;open.arm(0);open.noteDraw(x.c.Get(),0,0,0,1,pool.Get(),ids.Get(),0,nullptr);
    hr(x.c->Map(pool.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"open write map");open.noteMap(x.c.Get(),pool.Get(),0,D3D11_MAP_WRITE_DISCARD,S_OK,mapped.pData!=nullptr);open.noteDraw(x.c.Get(),3,0,0,1,pool.Get(),ids.Get(),0,nullptr);check(open.summary().openMapSnapshots==1,"open writable Map refuses source snapshot");x.c->Unmap(pool.Get(),0);open.noteUnmap(x.c.Get(),pool.Get(),0);check(open.summary().stackSamples==2,"Map and matching Unmap retain both caller stacks");
    const auto unavailableSnapshots=open.summary().snapshots;open.noteDraw(x.c.Get(),3,0,1,1,pool.Get(),ids.Get(),0,nullptr);
    check(open.summary().snapshots==unavailableSnapshots+1&&open.summary().openMapSnapshots==1,"completed Map retries its unavailable snapshot without duplicating unchanged IDs");
    open.noteUnmap(x.c.Get(),pool.Get(),0);check(open.summary().unmatchedUnmaps==1&&open.observedWriteCount()==2,"unmatched Unmap conservatively invalidates generation");

    edvr::ObjectClassificationProbe duplicate;duplicate.arm(0);duplicate.noteDraw(x.c.Get(),0,0,0,1,pool.Get(),ids.Get(),0,nullptr);duplicate.noteMap(x.c.Get(),pool.Get(),0,D3D11_MAP_WRITE_DISCARD,S_OK,true);duplicate.noteMap(x.c.Get(),pool.Get(),0,D3D11_MAP_WRITE_DISCARD,S_OK,true);check(duplicate.summary().duplicateMaps==1&&duplicate.observedWriteCount()==2,"duplicate outstanding Map is explicit and unmatched");

    D3D11_BUFFER_DESC unsafeDesc{};unsafeDesc.ByteWidth=336;unsafeDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;unsafeDesc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;unsafeDesc.StructureByteStride=336;ComPtr<ID3D11Buffer> unsafe;hr(x.d->CreateBuffer(&unsafeDesc,nullptr,&unsafe),"unsafe buffer");
    edvr::ObjectClassificationProbe unsafeProbe;unsafeProbe.arm(0);unsafeProbe.noteDraw(x.c.Get(),0,0,0,1,unsafe.Get(),ids.Get(),0,nullptr);unsafeProbe.noteDraw(x.c.Get(),3,0,0,1,unsafe.Get(),ids.Get(),0,nullptr);check(unsafeProbe.summary().unsafeAssociations==1,"UAV nominated source is unavailable");

    edvr::ObjectClassificationProbe cap;cap.arm(0);cap.noteDraw(x.c.Get(),0,0,0,1,pool.Get(),ids.Get(),0,nullptr);for(unsigned i=0;i<520;++i)cap.noteWrite(x.c.Get(),pool.Get(),edvr::ObjectClassificationProbe::Update);auto capped=cap.summary();check(capped.writes==512&&capped.observedWrites==520&&capped.writeOverflow==8&&capped.unmatchedWrites>0,"event cap never freezes generation");
    cap.setFrame(3);for(unsigned i=0;i<520;++i)cap.noteDraw(x.c.Get(),3,0,i,1,pool.Get(),ids.Get(),0,nullptr);check(cap.summary().draws==512&&cap.summary().drawOverflow==8,"draw cap explicit");

    edvr::ObjectClassificationProbe resources;resources.arm(0);std::vector<ComPtr<ID3D11Buffer>> many;for(unsigned i=0;i<66;++i){many.push_back(x.buffer(16,D3D11_BIND_VERTEX_BUFFER,D3D11_USAGE_DEFAULT));resources.noteDraw(x.c.Get(),0,0,0,1,many.back().Get(),ids.Get(),0,nullptr);}check(resources.summary().resources==64&&resources.summary().resourceOverflow>0,"resource cap explicit");

    auto copySource=x.buffer(UINT(poolData.size()),D3D11_BIND_SHADER_RESOURCE,D3D11_USAGE_DYNAMIC,poolData.data(),336);edvr::ObjectClassificationProbe chain;chain.arm(0);chain.noteDraw(x.c.Get(),0,0,0,1,pool.Get(),ids.Get(),0,nullptr);chain.noteWrite(x.c.Get(),pool.Get(),edvr::ObjectClassificationProbe::CopyResource,copySource.Get());
    hr(x.c->Map(copySource.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"copy source map");chain.noteMap(x.c.Get(),copySource.Get(),0,D3D11_MAP_WRITE_DISCARD,S_OK,mapped.pData!=nullptr);x.c->Unmap(copySource.Get(),0);chain.noteUnmap(x.c.Get(),copySource.Get(),0);check(chain.observedWriteCount()==2&&chain.summary().stackSamples==3,"buffer copy source acquires subsequent Map and Unmap route");const auto chainWrites=chain.observedWriteCount();chain.unknownWrites();check(chain.observedWriteCount()==chainWrites+3,"unknown writes invalidate pool, IDs, and watched copy source");
}

void fixture(Device& x,const wchar_t* directory){
    CreateDirectoryW(directory,nullptr);check(GetLastError()==ERROR_ALREADY_EXISTS||GetFileAttributesW(directory)!=INVALID_FILE_ATTRIBUTES,"fixture output directory");
    std::vector<uint8_t> poolA(672),poolB(336),idsA(24),idsB(8);
    for(size_t i=0;i<poolA.size();++i)poolA[i]=uint8_t((i*7+3)&255);for(size_t i=0;i<poolB.size();++i)poolB[i]=uint8_t((i*11+5)&255);
    put32(poolA,28,0xA0A00028);put32(poolA,320,0xA0A00140);put32(poolA,336+28,0xB0B00028);put32(poolA,336+320,0xB0B00140);put32(poolB,28,0xC0C00028);put32(poolB,320,0xC0C00140);
    uint32_t idWordsA[]={0,0x11111111,1,0x22222222,0,0x33333333};memcpy(idsA.data(),idWordsA,sizeof(idWordsA));uint32_t idWordsB[]={0,0x44444444};memcpy(idsB.data(),idWordsB,sizeof(idWordsB));
    auto pool=x.buffer(UINT(poolA.size()),D3D11_BIND_SHADER_RESOURCE,D3D11_USAGE_DEFAULT,poolA.data(),336);auto ids=x.buffer(UINT(idsA.size()),D3D11_BIND_VERTEX_BUFFER,D3D11_USAGE_DYNAMIC,idsA.data());
    auto missingPool=x.buffer(UINT(poolB.size()),D3D11_BIND_SHADER_RESOURCE,D3D11_USAGE_DEFAULT,poolB.data(),336);auto missingIds=x.buffer(UINT(idsB.size()),D3D11_BIND_VERTEX_BUFFER,D3D11_USAGE_DEFAULT,idsB.data());
    auto source=x.buffer(UINT(poolA.size()),D3D11_BIND_SHADER_RESOURCE,D3D11_USAGE_DEFAULT,poolA.data(),336);
    auto k0=key(0x1000),k1=key(0x2000),k2=key(0x3000);auto& p=edvr::objectClassificationProbe;p.arm(20);
    p.noteDraw(x.c.Get(),20,0,0,1,pool.Get(),ids.Get(),0,k0.data(),0xEB5234DB6ADB491Dull);
    p.setFrame(21);D3D11_MAPPED_SUBRESOURCE m{};hr(x.c->Map(ids.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&m),"fixture ids map");p.noteMap(x.c.Get(),ids.Get(),0,D3D11_MAP_WRITE_DISCARD,S_OK,m.pData!=nullptr);memcpy(m.pData,idsA.data(),idsA.size());x.c->Unmap(ids.Get(),0);p.noteUnmap(x.c.Get(),ids.Get(),0);
    p.noteWrite(x.c.Get(),pool.Get(),edvr::ObjectClassificationProbe::CopyResource,source.Get());x.c->CopyResource(pool.Get(),source.Get());
    p.noteDraw(x.c.Get(),21,0,0,1,pool.Get(),ids.Get(),0,k0.data(),0xEB5234DB6ADB491Dull);p.noteDraw(x.c.Get(),22,0,0,1,pool.Get(),ids.Get(),0,k0.data(),0xEB5234DB6ADB491Dull);
    p.setFrame(23);p.noteDraw(x.c.Get(),23,0,0,1,pool.Get(),ids.Get(),0,k0.data(),0xEB5234DB6ADB491Dull);
    std::vector<uint8_t> poolSecond=poolA;for(size_t i=0;i<336;++i)poolSecond[i]=uint8_t(0xD0+(i&15));put32(poolSecond,336+28,0xB0B00028);put32(poolSecond,336+320,0xB0B00140);
    x.c->UpdateSubresource(pool.Get(),0,nullptr,poolSecond.data(),0,0);p.noteWrite(x.c.Get(),pool.Get(),edvr::ObjectClassificationProbe::Update);
    p.noteDraw(x.c.Get(),23,0,1,1,pool.Get(),ids.Get(),8,k1.data(),0xDE545DC8EE4FBB87ull);
    p.noteDraw(x.c.Get(),23,0,2,1,missingPool.Get(),missingIds.Get(),0,k2.data(),0x61AE8EB05FDC18DDull);
    check(p.summary().snapshots==5&&p.summary().unobservedSources==2,"fixture includes rewritten and missing provenance");
    std::vector<float> scene(16,0.5f);scene[5]=0.75f;auto sceneTex=x.texture(4,4,DXGI_FORMAT_R32_FLOAT,scene.data(),16);
    struct Float2{float x,y;};std::vector<Float2> coverage(16,{0,0});coverage[5]={1,0.25f};coverage[6]={2,0.5f};auto coverageTex=x.texture(4,4,DXGI_FORMAT_R32G32_FLOAT,coverage.data(),32,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE);
    std::vector<uint8_t> mesh(3*240);makeRecord(mesh.data(),k0,poolA,0,0,1,0);makeRecord(mesh.data()+240,k1,poolSecond,336,1,1,1);makeRecord(mesh.data()+480,k2,poolB,0,2,1,0);
    auto meshBuffer=x.buffer(UINT(mesh.size()),D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,D3D11_USAGE_DEFAULT,mesh.data(),240);
    std::vector<uint32_t> colour(16,0xFF203040);colour[5]=0xFF00A0E0;auto colourTex=x.texture(4,4,DXGI_FORMAT_R8G8B8A8_UNORM,colour.data(),16);
    const auto writesBefore=p.writeCount();p.stage(x.c.Get(),22,0,900,sceneTex.Get(),coverageTex.Get(),meshBuffer.Get(),3,colourTex.Get());check(!p.sealed(),"wrong-frame fixture stage rejected");
    D3D11_QUERY_DESC queryDesc{D3D11_QUERY_OCCLUSION_PREDICATE,0};ComPtr<ID3D11Predicate> predicate;hr(x.d->CreatePredicate(&queryDesc,&predicate),"fixture predicate");x.c->Begin(predicate.Get());x.c->End(predicate.Get());x.c->SetPredication(predicate.Get(),FALSE);
    p.stage(x.c.Get(),23,0,900,sceneTex.Get(),coverageTex.Get(),meshBuffer.Get(),3,colourTex.Get());check(p.sealed()&&!p.active()&&p.writeCount()==writesBefore,"own GPU copies do not enter write ledger");
    ComPtr<ID3D11Predicate> restored;BOOL predicateValue=TRUE;x.c->GetPredication(&restored,&predicateValue);check(restored==predicate&&!predicateValue,"stage copies restore caller predication");x.c->SetPredication(nullptr,FALSE);
    check(p.write(x.c.Get(),directory,L"fixture"),"write fixture JSON and BIN");
    const auto json=readFile(joined(directory,L"classification_fixture.json"));const std::string text(json.begin(),json.end());check(text.find("edvr_object_classification_v2")!=std::string::npos&&text.find("scene_colour")!=std::string::npos,"fixture JSON schema and colour blob");
    const auto bin=readFile(joined(directory,L"classification_fixture.bin"));check(bin.size()==2688,"fixture binary exact size");check(memcmp(bin.data(),poolA.data(),poolA.size())==0,"first draw retains original pool bytes");check(memcmp(bin.data()+696,poolSecond.data(),poolSecond.size())==0,"rewrite produces a distinct pool snapshot");
}
}

int wmain(int argc,wchar_t** argv){if(argc!=2){std::puts("usage: object_classification_test.exe <output-directory>");return 2;}Device device;recordWriterTests();stateTests(device);sourceOwnerTests(device,argv[1]);fixture(device,argv[1]);std::printf("object classification: %u checks passed; fixtures written\n",checks);return 0;}

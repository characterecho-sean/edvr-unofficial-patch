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
void put32(std::vector<uint8_t>& v,size_t at,uint32_t value){check(at+4<=v.size(),"fixture put32 range");memcpy(v.data()+at,&value,4);}
void put64(std::vector<uint8_t>& v,size_t at,uint64_t value){check(at+8<=v.size(),"fixture put64 range");memcpy(v.data()+at,&value,8);}
std::vector<uint8_t> readFile(const std::wstring& p){FILE* f=nullptr;check(_wfopen_s(&f,p.c_str(),L"rb")==0&&f,"open generated file");fseek(f,0,SEEK_END);long n=ftell(f);check(n>=0,"generated file size");rewind(f);std::vector<uint8_t> out(static_cast<size_t>(n));check(fread(out.data(),1,out.size(),f)==out.size(),"read generated file");check(fclose(f)==0,"close generated file");return out;}
std::wstring joined(const wchar_t* dir,const wchar_t* file){std::wstring p=dir;if(!p.empty()&&p.back()!=L'\\'&&p.back()!=L'/')p+=L'\\';p+=file;return p;}
void realExecutableOpcodeTest(const wchar_t* path){
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    check(file!=INVALID_HANDLE_VALUE,"open exact Elite executable for opcode validation");
    HANDLE mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY|SEC_IMAGE_NO_EXECUTE,0,0,nullptr);
    check(mapping!=nullptr,"map exact Elite executable as image");
    const void* image=MapViewOfFile(mapping,FILE_MAP_READ,0,0,0);check(image!=nullptr,"view exact Elite executable image");
    edvr::ObjectRecordWriterProbe probe;
    check(probe.executableOpcodesMatchAtBaseForTest(reinterpret_cast<uintptr_t>(image)),"exact Elite executable passes all writer and ownership opcode guards");
    check(UnmapViewOfFile(image)!=FALSE,"unmap exact Elite executable image");
    check(CloseHandle(mapping)!=FALSE,"close exact Elite executable mapping");
    check(CloseHandle(file)!=FALSE,"close exact Elite executable");
}

extern "C" void recordWriterUnwindStub(uintptr_t dictionary,uintptr_t key,const void* record);
extern "C" void recordWriterUnwindResume();
extern "C" void kinematicOwnerDirectUnwindStub(uintptr_t outer,uintptr_t dictionary,uintptr_t key,const void* record);
extern "C" void kinematicOwnerVirtualUnwindStub(uintptr_t outer,uintptr_t dictionary,uintptr_t key,const void* record);
extern "C" void recordWriterOwnershipUnwindResume();
extern "C" void kinematicOwnerDirectUnwindResume();
extern "C" void kinematicOwnerVirtualUnwindResume();
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

void recordWriterTests(){
    check(edvr::objectRecordWriterHookSelfTest()==0,"record-writer relay publishes original before gate and forwards exactly once");
    constexpr std::array<uint8_t,40> ownershipWindow={
        0x00,0x00,0x01,0x48,0x85,0xC9,0x75,0x0C,0x48,0x8D,0x4C,0x24,0x20,0xE8,0x2E,0x67,
        0x00,0x00,0xEB,0x31,0x48,0x8B,0x01,0x48,0x8D,0x54,0x24,0x20,0xFF,0x50,0x50,0xEB,
        0x24,0x48,0x8D,0x8B,0x00,0x03,0x00,0x00};
    check(edvr::ObjectRecordWriterProbe::ownershipOpcodeWindowMatchesForTest(0x431B200,ownershipWindow.data(),ownershipWindow.size()),"exact Elite ownership dispatch window passes production guard");
    int32_t directDisplacement=0;memcpy(&directDisplacement,ownershipWindow.data()+14,sizeof(directDisplacement));
    check(int64_t(0x431B212)+directDisplacement==int64_t(0x4321940),"exact direct rel32 independently resolves to 4321940");
    auto wrongTarget=ownershipWindow;wrongTarget[15]=0xF7;
    check(!edvr::ObjectRecordWriterProbe::ownershipOpcodeWindowMatchesForTest(0x431B200,wrongTarget.data(),wrongTarget.size()),"stale direct rel32 target is rejected");
    check(!edvr::ObjectRecordWriterProbe::ownershipOpcodeWindowMatchesForTest(0x431B1FF,ownershipWindow.data(),ownershipWindow.size()),"neighboring ownership instruction placement is rejected");
    auto wrongVirtual=ownershipWindow;wrongVirtual[30]=0x58;
    check(!edvr::ObjectRecordWriterProbe::ownershipOpcodeWindowMatchesForTest(0x431B200,wrongVirtual.data(),wrongVirtual.size()),"wrong virtual slot is rejected");
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
    std::vector<uint8_t> lateStack(0x200);fillRecord(lateStack.data()+0x50,8);auto late=p.beginLookupRvaForTest(0x369CE91,reinterpret_cast<uintptr_t>(lateStack.data()),dictionary,reinterpret_cast<uintptr_t>(keyBytes.data()),c);p.completeLookup(late,reinterpret_cast<uintptr_t>(entry.data()));
    const auto s=p.summary();check(s.observed==7&&s.stored==7&&s.completed==7&&s.declinedManagement==1&&s.declinedUnknown==1,"all five writer recipes accepted and management/unknown callers declined");
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

    edvr::ObjectRecordWriterProbe refusedProbe;refusedProbe.armForTest(91);refusedProbe.setOwnershipOpcodesValidForTest(false);writerUnwindProbe=&refusedProbe;
    writerOwnershipFixtureReturn=reinterpret_cast<uintptr_t>(&kinematicOwnerDirectUnwindResume);writerOwnershipGameReturn=0x431B212;
    kinematicOwnerDirectUnwindStub(reinterpret_cast<uintptr_t>(rig.outer.data()),rig.directDictionary(),rig.directKey(),ownershipRecord.data());
    refusedProbe.completeLookup(writerUnwindPending,reinterpret_cast<uintptr_t>(entry.data()));
    const auto refusedSummary=refusedProbe.summary();const auto refusedOwnership=refusedProbe.ownershipSummary();
    check(refusedSummary.observed==1&&refusedSummary.stored==1&&refusedSummary.completed==1&&
          refusedOwnership.attempted==1&&refusedOwnership.opcodeMismatch==1&&refusedOwnership.linked==0&&
          refusedProbe.recordsForTest().back().ownershipStatus=="opcode_mismatch",
          "ownership opcode refusal preserves completed writer diagnostics");
    refusedProbe.finish();writerUnwindProbe=&ownershipProbe;

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

// The v3 capture end to end: armed as the eye run arms it, one
// ownership-linked record through the writer's test entry, finished and
// written. build.bat then has tools/object_classification.py load the file,
// so the C++ writer and the Python reader cannot drift apart unnoticed.
void captureFixture(const wchar_t* directory){
    CreateDirectoryW(directory,nullptr);check(GetLastError()==ERROR_ALREADY_EXISTS||GetFileAttributesW(directory)!=INVALID_FILE_ATTRIBUTES,"fixture output directory");
    auto& p=edvr::objectClassificationProbe;p.arm(100);edvr::objectRecordWriterProbe.armForTest(100);
    std::vector<uint8_t> source(336);for(size_t i=0;i<source.size();++i)source[i]=uint8_t(i*13+7);
    std::vector<uint8_t> writerStack(0x400),writerEntry(0x38);uintptr_t writerRbp=reinterpret_cast<uintptr_t>(writerStack.data()+0x80);memcpy(reinterpret_cast<void*>(writerRbp+0x1C0),source.data(),source.size());
    KinematicFixture writerRig;put64(writerStack,0x18,reinterpret_cast<uintptr_t>(writerRig.context.data()));CONTEXT writerContext{};writerContext.Rbp=writerRbp;CONTEXT writerOwner{};writerOwner.Rdi=reinterpret_cast<DWORD64>(writerRig.outer.data());writerOwner.Rbx=reinterpret_cast<DWORD64>(writerRig.collection.data());
    auto writerPending=edvr::objectRecordWriterProbe.beginLookupOwnershipRvaForTest(0x42B4ED6,0,writerRig.inlineDictionary(),writerRig.directKey(),writerContext,0x431B21F,writerOwner,3);
    edvr::objectRecordWriterProbe.completeLookup(writerPending,reinterpret_cast<uintptr_t>(writerEntry.data()));
    p.finish();p.useExpectedExecutableIdentityForTest();check(p.write(directory,L"fixture"),"write v3 classification fixture");
    const auto json=readFile(joined(directory,L"classification_fixture.json"));const std::string text(json.begin(),json.end());
    check(text.find("edvr_object_classification_v3")!=std::string::npos&&text.find("inline_42b4ed6")!=std::string::npos&&
          text.find("\"ownerships\":[{")!=std::string::npos&&text.find("\"ownership_status\":\"linked\"")!=std::string::npos,
          "v3 fixture publishes the record writer's linked ownership");
    check(text.find("\"draws\"")==std::string::npos&&text.find("\"source_owner\"")==std::string::npos&&text.find("\"resources\"")==std::string::npos,
          "v3 fixture carries no draw/mesh sections");
    check(text.find("\"record_writers\":{\"version\":3,")!=std::string::npos&&text.find("\"uploads\"")==std::string::npos,
          "the record-writer section is version 3, without the upload join");
    p.reset();
}
}

int wmain(int argc,wchar_t** argv){if(argc<2||argc>3){std::puts("usage: object_classification_test.exe <output-directory> [exact-Elite-executable]");return 2;}if(argc==3)realExecutableOpcodeTest(argv[2]);recordWriterTests();captureFixture(argv[1]);std::printf("object classification: %u checks passed; fixtures written\n",checks);return 0;}

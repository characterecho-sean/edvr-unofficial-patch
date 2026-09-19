#pragma once
// Build-specific, bounded CPU provenance capture for AtlasModel t33 uploads.
// This diagnostic only runs while ObjectClassificationProbe is armed and only
// for a nominated pool Map.  Every pointer read is guarded; no captured value
// affects rendering.
#include <d3d11.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace edvr {

class ObjectSourceOwnerProbe {
public:
    static constexpr uint32_t kExpectedTimestamp=1788384820u;
    static constexpr uint32_t kExpectedImageSize=104894464u;
    static constexpr uintptr_t kReturnRva=0x4C821B3u;
    static constexpr uint32_t kRecordStride=0x150u;
    static constexpr uint32_t kAttemptCap=16u,kUnwindCap=32u,kGroupCap=64u;
    static constexpr uint32_t kLeafCap=512u,kDescriptorCap=4096u;
    static constexpr uint64_t kCpuByteCap=32ull*1024*1024;
    static constexpr uint64_t kAttemptByteCap=8ull*1024*1024;
    static constexpr uint64_t kSourceByteCap=8ull*1024*1024;
    static constexpr uint32_t kNone=std::numeric_limits<uint32_t>::max();

    struct Summary {
        uint32_t mapsConsidered=0,identityRejects=0,opcodeRejects=0;
        uint32_t unwindAttempts=0,unwindFailures=0,callsiteMatches=0,resourceMatches=0;
        uint32_t attemptsStored=0,attemptOverflow=0,completeAttempts=0,partialAttempts=0,readFaults=0,metadataChanges=0;
        uint64_t groupOverflow=0,leafOverflow=0,descriptorOverflow=0,cpuByteDeclines=0;
    };
    struct CpuBlob {
        uint32_t attempt=kNone,descriptor=kNone;
        std::string kind,status;
        uint64_t address=0,fileOffset=0,fileBytes=0;
        std::vector<uint8_t> data;
    };
    struct Group {uint64_t address=0,leafCount=0,leavesScanned=0;};
    struct Leaf {
        uint32_t group=0;uint64_t address=0,ownerLink=0,baseRecord=0,recordCount=0;
    };
    struct Descriptor {
        uint32_t leaf=0,list=0,entry=0,entryStride=0;
        uint64_t address=0,sourceAddress=0,destinationRelativeRecord=0,recordCount=0;
        uint64_t destinationFirstRecord=0,destinationEndRecord=0;
        uint32_t rawBlob=kNone,payloadBlob=kNone;
        std::string status="not_run";
    };
    struct Attempt {
        uint32_t id=0,event=0,resource=0;uint64_t generation=0,foreignEpoch=0;
        uint32_t meshFrame=0,unwindFrames=0;uint64_t returnRva=0;
        std::string status="not_run",unwindStatus="not_run";
        uint64_t nestedOwner=0,nestedOwnerField130=0,nestedOwnerField138=0;
        uint64_t wrapper=0,nativeResource=0;uint32_t stride=0;
        uint64_t groupCount=0,groupsScanned=0,leafCount=0,leavesScanned=0;
        uint64_t descriptorCount=0,descriptorsScanned=0,retainedBytes=0;
        std::vector<Group> groups;std::vector<Leaf> leaves;std::vector<Descriptor> descriptors;
    };

private:
    Summary summary_{};std::vector<Attempt> attempts_;std::vector<CpuBlob> blobs_;
    uint64_t retainedBytes_=0;bool syntheticFixture_=false;

    template<class T> static bool readValue(uintptr_t address,T& value) noexcept {
        if(!address || address>UINTPTR_MAX-sizeof(T))return false;
        __try {std::memcpy(&value,reinterpret_cast<const void*>(address),sizeof(T));return true;}
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    static bool readBytes(uintptr_t address,void* output,size_t bytes) noexcept {
        if(!bytes)return true;if(!address||!output||address>UINTPTR_MAX-bytes)return false;
        __try {std::memcpy(output,reinterpret_cast<const void*>(address),bytes);return true;}
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    static uint64_t addSaturated(uint64_t a,uint64_t b) noexcept {
        return b>UINT64_MAX-a?UINT64_MAX:a+b;
    }
    static const char* topStatus(const Summary& s,const std::vector<Attempt>& a) noexcept {
        if(!s.mapsConsidered)return "not_run";
        if(s.identityRejects==s.mapsConsidered)return "identity_mismatch";
        if(s.callsiteMatches==0)return "no_callsite";
        if(s.resourceMatches==0)return "no_resource_match";
        for(const auto& x:a)if(x.status=="partial")return "partial";
        return s.completeAttempts?"captured":"partial";
    }
    static bool executableIdentity(uintptr_t& base,uint32_t& timestamp,uint32_t& imageSize) noexcept {
        base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));timestamp=imageSize=0;
        IMAGE_DOS_HEADER dos{};if(!readValue(base,dos)||dos.e_magic!=IMAGE_DOS_SIGNATURE)return false;
        IMAGE_NT_HEADERS nt{};if(dos.e_lfanew<=0||!readValue(base+uintptr_t(dos.e_lfanew),nt)||nt.Signature!=IMAGE_NT_SIGNATURE)return false;
        timestamp=nt.FileHeader.TimeDateStamp;imageSize=nt.OptionalHeader.SizeOfImage;return true;
    }
    static bool callsiteOpcodeMatches(uintptr_t base) noexcept {
        // call 0x51E0C0; then stores beginning at the exact return PC.
        static const uint8_t before[]={0xE8,0x0D,0xBF,0x89,0xFB};
        static const uint8_t after[]={0x8B,0x45,0xE0,0x48,0x8B,0x8B,0x68,0x01,0x00,0x00};
        uint8_t gotBefore[sizeof(before)]{},gotAfter[sizeof(after)]{};
        return readBytes(base+kReturnRva-sizeof(before),gotBefore,sizeof(gotBefore)) &&
            readBytes(base+kReturnRva,gotAfter,sizeof(gotAfter)) &&
            std::memcmp(before,gotBefore,sizeof(before))==0 && std::memcmp(after,gotAfter,sizeof(after))==0;
    }
    static bool unwindOwnerAt(uintptr_t target,uintptr_t& owner,uint32_t& frames,
                              std::string& status) noexcept {
        CONTEXT c{};RtlCaptureContext(&c);
        for(frames=0;frames<kUnwindCap;++frames) {
            // The register state belongs to this return PC.  Comparing before
            // its unwind is essential: unwinding the target caller would
            // restore the RBX it saved before installing the owner value.
            if(c.Rip==target){owner=uintptr_t(c.Rbx);status="matched";return true;}
            const DWORD64 oldRip=c.Rip,oldRsp=c.Rsp;DWORD64 imageBase=0;PRUNTIME_FUNCTION function=nullptr;
            __try {function=RtlLookupFunctionEntry(c.Rip,&imageBase,nullptr);}
            __except(EXCEPTION_EXECUTE_HANDLER){status="metadata_unreadable";return false;}
            if(function) {
                DWORD64 establisher=0;PVOID handlerData=nullptr;
                __try {RtlVirtualUnwind(UNW_FLAG_NHANDLER,imageBase,c.Rip,function,&c,&handlerData,&establisher,nullptr);}
                __except(EXCEPTION_EXECUTE_HANDLER){status="frame_unreadable";return false;}
            } else {
                uintptr_t next=0;if(!readValue(uintptr_t(c.Rsp),next)){status="leaf_unreadable";return false;}
                c.Rip=next;c.Rsp+=sizeof(uintptr_t);
            }
            if(c.Rsp<=oldRsp||c.Rip==oldRip){status="no_progress";return false;}
        }
        status="frame_limit";return false;
    }
    uint32_t addBlob(uint32_t attempt,uint32_t descriptor,const char* kind,
                     uintptr_t address,size_t bytes,const char* unavailable,
                     uint64_t& attemptBytes) {
        CpuBlob b;b.attempt=attempt;b.descriptor=descriptor;b.kind=kind;b.address=address;
        if(unavailable)b.status=unavailable;
        else if(bytes>kSourceByteCap && std::strcmp(kind,"source_records")==0)b.status="source_too_large";
        else if(bytes>kCpuByteCap-retainedBytes_||bytes>kAttemptByteCap-attemptBytes){b.status="byte_budget";++summary_.cpuByteDeclines;}
        else {
            // Charge the attempted allocation even if the guarded read faults,
            // so repeated bad pointers cannot bypass the memory/read budget.
            retainedBytes_+=bytes;attemptBytes+=bytes;
            b.data.resize(bytes);
            if(readBytes(address,b.data.data(),bytes))b.status="available";
            else {std::vector<uint8_t>().swap(b.data);b.status="read_fault";++summary_.readFaults;}
        }
        blobs_.push_back(std::move(b));return uint32_t(blobs_.size()-1);
    }
    uint32_t addCapturedBlob(uint32_t attempt,uint32_t descriptor,const char* kind,
                             uintptr_t address,const void* bytes,size_t size,uint64_t& attemptBytes) {
        CpuBlob b;b.attempt=attempt;b.descriptor=descriptor;b.kind=kind;b.address=address;
        if(size>kCpuByteCap-retainedBytes_||size>kAttemptByteCap-attemptBytes){b.status="byte_budget";++summary_.cpuByteDeclines;}
        else {retainedBytes_+=size;attemptBytes+=size;b.data.resize(size);std::memcpy(b.data.data(),bytes,size);b.status="available";}
        blobs_.push_back(std::move(b));return uint32_t(blobs_.size()-1);
    }
    static bool plus(uintptr_t base,uint64_t offset,uintptr_t& out) noexcept {
        if(offset>UINTPTR_MAX||base>UINTPTR_MAX-uintptr_t(offset))return false;out=base+uintptr_t(offset);return true;
    }
    bool scanOwner(Attempt& a,ID3D11Resource* resource,uint32_t poolBytes) {
        bool partial=false;uintptr_t p=0;
        if(!plus(uintptr_t(a.nestedOwner),0x140,p)||!readValue(p,a.wrapper)||!a.wrapper ||
           !plus(uintptr_t(a.wrapper),0x140,p)||!readValue(p,a.nativeResource)) {
            ++summary_.readFaults;a.status="nested_unreadable";return false;
        }
        if(a.nativeResource!=uint64_t(reinterpret_cast<uintptr_t>(resource))){a.status="resource_mismatch";return false;}
        ++summary_.resourceMatches;
        // These two factory-populated qwords are retained as opaque ownership
        // evidence.  Their semantic meaning is deliberately not inferred.
        if(!plus(uintptr_t(a.nestedOwner),0x130,p)||!readValue(p,a.nestedOwnerField130)||
           !plus(uintptr_t(a.nestedOwner),0x138,p)||!readValue(p,a.nestedOwnerField138)){
            ++summary_.readFaults;partial=true;
        }
        uint64_t strideOwner=0;if(!plus(uintptr_t(a.nestedOwner),0x100,p)||!readValue(p,strideOwner)||!strideOwner ||
            !plus(uintptr_t(strideOwner),0x10,p)||!readValue(p,a.stride)){
            ++summary_.readFaults;a.status="layout_invalid";return false;}
        if(a.stride!=kRecordStride||poolBytes<kRecordStride||poolBytes%kRecordStride){a.status="layout_invalid";return false;}
        uint64_t groupArray=0;if(!plus(uintptr_t(a.nestedOwner),0x198,p)||!readValue(p,a.groupCount)||
            !plus(uintptr_t(a.nestedOwner),0x1A0,p)||!readValue(p,groupArray)||(a.groupCount&&!groupArray)){
            ++summary_.readFaults;a.status="layout_invalid";return false;}
        const uint64_t groupScan=std::min<uint64_t>(a.groupCount,kGroupCap);
        if(a.groupCount>groupScan){summary_.groupOverflow=addSaturated(summary_.groupOverflow,a.groupCount-groupScan);partial=true;}
        a.groups.reserve(size_t(groupScan));a.leaves.reserve(std::min<uint64_t>(kLeafCap,groupScan*8));
        for(uint64_t gi=0;gi<groupScan;++gi) {
            uint64_t groupAddress=0;uintptr_t at=0;if(!plus(uintptr_t(groupArray),gi*8,at)||!readValue(at,groupAddress)){
                ++summary_.readFaults;partial=true;break;}
            Group g;g.address=groupAddress;
            uint64_t leafArray=0;if(!groupAddress||!plus(uintptr_t(groupAddress),0x140,at)||!readValue(at,g.leafCount)||
                !plus(uintptr_t(groupAddress),0x148,at)||!readValue(at,leafArray)||(g.leafCount&&!leafArray)){
                ++summary_.readFaults;partial=true;a.groups.push_back(g);continue;}
            a.leafCount=addSaturated(a.leafCount,g.leafCount);
            uint64_t room=kLeafCap-a.leaves.size(),leafScan=std::min<uint64_t>(g.leafCount,room);
            if(g.leafCount>leafScan){summary_.leafOverflow=addSaturated(summary_.leafOverflow,g.leafCount-leafScan);partial=true;}
            for(uint64_t li=0;li<leafScan;++li) {
                uint64_t leafAddress=0;if(!plus(uintptr_t(leafArray),li*8,at)||!readValue(at,leafAddress)||!leafAddress){++summary_.readFaults;partial=true;continue;}
                uint64_t link=0,back=0,baseRecord=0,recordCount=0;
                if(!plus(uintptr_t(leafAddress),0xC0,at)||!readValue(at,baseRecord)||
                    !plus(uintptr_t(leafAddress),0xC8,at)||!readValue(at,recordCount)){
                    ++summary_.readFaults;partial=true;continue;}
                // 4C81E80 skips leaves with C8 == 0, so their unused D0 and
                // descriptor storage must not manufacture read failures.
                if(recordCount && (!plus(uintptr_t(leafAddress),0xD0,at)||!readValue(at,link)||!link||
                    !plus(uintptr_t(link),0x10,at)||!readValue(at,back)||back!=a.nestedOwner)){
                    ++summary_.readFaults;partial=true;continue;}
                const uint32_t leafIndex=uint32_t(a.leaves.size());a.leaves.push_back({uint32_t(gi),leafAddress,link,baseRecord,recordCount});++g.leavesScanned;
                if(!recordCount){uint64_t again=~0ull;if(!plus(uintptr_t(leafAddress),0xC8,at)||!readValue(at,again)){++summary_.readFaults;partial=true;}else if(again!=0){++summary_.metadataChanges;partial=true;}continue;}
                for(uint32_t list=0;list<8;++list) {
                    const uint64_t listOffset=list?0x20ull+uint64_t(list-1)*0x18ull:0x8ull;
                    const uint32_t entryStride=list?0x50u:0x48u;uint64_t count=0,array=0;
                    if(!plus(uintptr_t(leafAddress),listOffset,at)||!readValue(at,count)||
                        !plus(uintptr_t(leafAddress),listOffset+8,at)||!readValue(at,array)||(count&&!array)){
                        ++summary_.readFaults;partial=true;continue;}
                    a.descriptorCount=addSaturated(a.descriptorCount,count);
                    uint64_t descRoom=kDescriptorCap-a.descriptors.size(),descScan=std::min<uint64_t>(count,descRoom);
                    if(count>descScan){summary_.descriptorOverflow=addSaturated(summary_.descriptorOverflow,count-descScan);partial=true;}
                    for(uint64_t di=0;di<descScan;++di) {
                        uintptr_t descAddress=0;if(!plus(uintptr_t(array),di*entryStride,descAddress)){partial=true;continue;}
                        Descriptor d;d.leaf=leafIndex;d.list=list;d.entry=uint32_t(di);d.entryStride=entryStride;d.address=descAddress;
                        std::array<uint8_t,0x50> raw{},verify{};
                        if(!readBytes(descAddress,raw.data(),entryStride)){d.status="descriptor_fault";++summary_.readFaults;partial=true;}
                        else {
                            std::memcpy(&d.sourceAddress,raw.data()+8,8);uint32_t rel=0,n=0;std::memcpy(&rel,raw.data()+0x38,4);std::memcpy(&n,raw.data()+0x3C,4);
                            d.destinationRelativeRecord=rel;d.recordCount=n;
                            d.destinationFirstRecord=addSaturated(baseRecord,rel);d.destinationEndRecord=addSaturated(d.destinationFirstRecord,n);
                            // Preserve the exact first descriptor image from
                            // which the interpreted fields above were read.
                            d.rawBlob=addCapturedBlob(a.id,uint32_t(a.descriptors.size()),"descriptor",descAddress,raw.data(),entryStride,a.retainedBytes);
                            const uint64_t byteCount=uint64_t(n)*kRecordStride,poolRecords=poolBytes/kRecordStride;
                            const char* unavailable=nullptr;
                            if(!n)unavailable="zero_count";else if(!d.sourceAddress)unavailable="null_source";
                            else if(d.destinationFirstRecord<baseRecord||d.destinationEndRecord<d.destinationFirstRecord||
                                d.destinationEndRecord>addSaturated(baseRecord,recordCount)||d.destinationEndRecord>poolRecords)unavailable="range_overflow";
                            d.payloadBlob=addBlob(a.id,uint32_t(a.descriptors.size()),"source_records",uintptr_t(d.sourceAddress),size_t(byteCount),unavailable,a.retainedBytes);
                            if(!readBytes(descAddress,verify.data(),entryStride)){d.status="descriptor_fault";++summary_.readFaults;partial=true;}
                            else if(std::memcmp(raw.data(),verify.data(),entryStride)!=0){d.status="descriptor_changed";partial=true;}
                            else if(unavailable){d.status=unavailable;if(std::strcmp(unavailable,"zero_count")!=0)partial=true;}
                            else if(blobs_[d.rawBlob].status!="available"||blobs_[d.payloadBlob].status!="available"){
                                d.status=blobs_[d.payloadBlob].status=="available"?blobs_[d.rawBlob].status:blobs_[d.payloadBlob].status;partial=true;
                            } else d.status="captured";
                        }
                        a.descriptors.push_back(std::move(d));
                    }
                    uint64_t countAgain=0,arrayAgain=0;
                    if(!plus(uintptr_t(leafAddress),listOffset,at)||!readValue(at,countAgain)||
                        !plus(uintptr_t(leafAddress),listOffset+8,at)||!readValue(at,arrayAgain)){++summary_.readFaults;partial=true;}
                    else if(countAgain!=count||arrayAgain!=array){++summary_.metadataChanges;partial=true;}
                }
                uint64_t baseAgain=0,countAgain=0,linkAgain=0,backAgain=0;
                if(!plus(uintptr_t(leafAddress),0xC0,at)||!readValue(at,baseAgain)||
                    !plus(uintptr_t(leafAddress),0xC8,at)||!readValue(at,countAgain)||
                    !plus(uintptr_t(leafAddress),0xD0,at)||!readValue(at,linkAgain)||!linkAgain||
                    !plus(uintptr_t(linkAgain),0x10,at)||!readValue(at,backAgain)){++summary_.readFaults;partial=true;}
                else if(baseAgain!=baseRecord||countAgain!=recordCount||linkAgain!=link||backAgain!=a.nestedOwner){++summary_.metadataChanges;partial=true;}
            }
            uint64_t leafCountAgain=0,leafArrayAgain=0;
            if(!plus(uintptr_t(groupAddress),0x140,at)||!readValue(at,leafCountAgain)||
                !plus(uintptr_t(groupAddress),0x148,at)||!readValue(at,leafArrayAgain)){++summary_.readFaults;partial=true;}
            else if(leafCountAgain!=g.leafCount||leafArrayAgain!=leafArray){++summary_.metadataChanges;partial=true;}
            a.groups.push_back(g);
        }
        uint64_t groupCountAgain=0,groupArrayAgain=0;
        if(!plus(uintptr_t(a.nestedOwner),0x198,p)||!readValue(p,groupCountAgain)||
            !plus(uintptr_t(a.nestedOwner),0x1A0,p)||!readValue(p,groupArrayAgain)){++summary_.readFaults;partial=true;}
        else if(groupCountAgain!=a.groupCount||groupArrayAgain!=groupArray){++summary_.metadataChanges;partial=true;}
        a.groupsScanned=a.groups.size();a.leavesScanned=a.leaves.size();a.descriptorsScanned=a.descriptors.size();
        a.status=partial?"partial":"captured";if(partial){++summary_.partialAttempts;}else ++summary_.completeAttempts;return true;
    }

public:
    void reset(){summary_=Summary{};attempts_.clear();blobs_.clear();retainedBytes_=0;syntheticFixture_=false;}
    void markSyntheticFixture() noexcept{syntheticFixture_=true;}
    const Summary& summary() const noexcept{return summary_;}
    const std::vector<Attempt>& attempts() const noexcept{return attempts_;}
    const std::vector<CpuBlob>& blobs() const noexcept{return blobs_;}
    static bool recoverOwnerAt(uintptr_t target,uintptr_t& owner,uint32_t& frames,
                               std::string& status) noexcept {
        return unwindOwnerAt(target,owner,frames,status);
    }
    static bool opcodeMatchesAtBaseForTest(uintptr_t base) noexcept{return callsiteOpcodeMatches(base);}
    uint32_t captureMap(uint32_t event,uint32_t resource,uint64_t generation,uint32_t meshFrame,
                        uint64_t foreignEpoch,ID3D11Resource* native,uint32_t poolBytes) {
        ++summary_.mapsConsidered;if(attempts_.size()>=kAttemptCap){++summary_.attemptOverflow;return kNone;}
        Attempt a;a.id=uint32_t(attempts_.size());a.event=event;a.resource=resource;a.generation=generation;a.meshFrame=meshFrame;a.foreignEpoch=foreignEpoch;
        uintptr_t base=0;uint32_t timestamp=0,imageSize=0;
        if(!executableIdentity(base,timestamp,imageSize)||timestamp!=kExpectedTimestamp||imageSize!=kExpectedImageSize){a.status="identity_mismatch";++summary_.identityRejects;}
        else if(!callsiteOpcodeMatches(base)){a.status="opcode_mismatch";++summary_.opcodeRejects;}
        else {
            ++summary_.unwindAttempts;uintptr_t owner=0;
            if(!unwindOwnerAt(base+kReturnRva,owner,a.unwindFrames,a.unwindStatus)){a.status="unwind_not_found";++summary_.unwindFailures;}
            else {a.returnRva=kReturnRva;++summary_.callsiteMatches;a.nestedOwner=owner;scanOwner(a,native,poolBytes);}
        }
        attempts_.push_back(std::move(a));summary_.attemptsStored=uint32_t(attempts_.size());return uint32_t(attempts_.size()-1);
    }
    uint32_t captureSynthetic(uint32_t event,uint32_t resource,uint64_t generation,uint32_t meshFrame,
                              uint64_t foreignEpoch,ID3D11Resource* native,uint32_t poolBytes,
                              uintptr_t nestedOwner) {
        ++summary_.mapsConsidered;if(attempts_.size()>=kAttemptCap){++summary_.attemptOverflow;return kNone;}
        Attempt a;a.id=uint32_t(attempts_.size());a.event=event;a.resource=resource;a.generation=generation;a.meshFrame=meshFrame;a.foreignEpoch=foreignEpoch;
        a.unwindStatus="matched";a.returnRva=kReturnRva;a.nestedOwner=nestedOwner;++summary_.callsiteMatches;
        scanOwner(a,native,poolBytes);attempts_.push_back(std::move(a));summary_.attemptsStored=uint32_t(attempts_.size());return uint32_t(attempts_.size()-1);
    }
    bool writeBinary(FILE* file,uint64_t& offset,bool& binOk) {
        for(auto& b:blobs_) {
            b.fileOffset=offset;b.fileBytes=0;if(b.status!="available")continue;
            if(!file||!binOk){b.status=file?"bin_write_failed":"bin_open_failed";continue;}
            if(fwrite(b.data.data(),1,b.data.size(),file)!=b.data.size()){b.status="bin_write_failed";binOk=false;continue;}
            b.fileBytes=b.data.size();offset+=b.fileBytes;
        }
        return binOk;
    }
    void writeJson(std::ostringstream& j) const {
        const char* status=topStatus(summary_,attempts_);
        j<<"  \"source_owner\":{\"status\":\""<<status<<"\",\"synthetic_fixture\":"<<(syntheticFixture_?"true":"false")<<",\"expected_pe_timestamp\":"<<kExpectedTimestamp
         <<",\"expected_image_size\":"<<kExpectedImageSize<<",\"callsite_rva\":"<<uint64_t(kReturnRva)
         <<",\"limits\":{\"attempts\":"<<kAttemptCap<<",\"unwind_frames\":"<<kUnwindCap<<",\"groups\":"<<kGroupCap
         <<",\"leaves\":"<<kLeafCap<<",\"descriptors\":"<<kDescriptorCap<<",\"cpu_bytes\":"<<kCpuByteCap
         <<",\"attempt_cpu_bytes\":"<<kAttemptByteCap<<",\"single_source_bytes\":"<<kSourceByteCap<<"},\"summary\":{"
         <<"\"maps_considered\":"<<summary_.mapsConsidered<<",\"identity_rejects\":"<<summary_.identityRejects
         <<",\"opcode_rejects\":"<<summary_.opcodeRejects<<",\"unwind_attempts\":"<<summary_.unwindAttempts
         <<",\"unwind_failures\":"<<summary_.unwindFailures<<",\"callsite_matches\":"<<summary_.callsiteMatches
         <<",\"resource_matches\":"<<summary_.resourceMatches<<",\"attempts_stored\":"<<summary_.attemptsStored
         <<",\"attempt_overflow\":"<<summary_.attemptOverflow
         <<",\"complete_attempts\":"<<summary_.completeAttempts<<",\"partial_attempts\":"<<summary_.partialAttempts
         <<",\"read_faults\":"<<summary_.readFaults<<",\"metadata_changes\":"<<summary_.metadataChanges<<",\"group_overflow\":"<<summary_.groupOverflow
         <<",\"leaf_overflow\":"<<summary_.leafOverflow<<",\"descriptor_overflow\":"<<summary_.descriptorOverflow
         <<",\"cpu_byte_declines\":"<<summary_.cpuByteDeclines<<"},\"attempts\":[";
        for(size_t i=0;i<attempts_.size();++i){if(i)j<<',';const auto& a=attempts_[i];j<<"{\"id\":"<<a.id<<",\"event\":"<<a.event<<",\"resource\":"<<a.resource
            <<",\"generation\":"<<a.generation<<",\"mesh_frame\":"<<a.meshFrame<<",\"foreign_epoch\":"<<a.foreignEpoch<<",\"status\":\""<<a.status
            <<"\",\"unwind_status\":\""<<a.unwindStatus<<"\",\"unwind_frames\":"<<a.unwindFrames<<",\"return_rva\":"<<a.returnRva
            <<",\"nested_owner\":\"0x"<<std::hex<<a.nestedOwner<<"\",\"nested_owner_field_130\":\"0x"<<a.nestedOwnerField130<<"\",\"nested_owner_field_138\":\"0x"<<a.nestedOwnerField138
            <<"\",\"wrapper\":\"0x"<<a.wrapper<<"\",\"native_resource\":\"0x"<<a.nativeResource<<std::dec
            <<"\",\"stride\":"<<a.stride<<",\"group_count\":"<<a.groupCount<<",\"groups_scanned\":"<<a.groupsScanned
            <<",\"leaf_count\":"<<a.leafCount<<",\"leaves_scanned\":"<<a.leavesScanned<<",\"descriptor_count\":"<<a.descriptorCount
            <<",\"descriptors_scanned\":"<<a.descriptorsScanned<<",\"retained_bytes\":"<<a.retainedBytes<<",\"groups\":[";
            for(size_t n=0;n<a.groups.size();++n){if(n)j<<',';const auto& g=a.groups[n];j<<"{\"address\":\"0x"<<std::hex<<g.address<<std::dec<<"\",\"leaf_count\":"<<g.leafCount<<",\"leaves_scanned\":"<<g.leavesScanned<<'}';}j<<"],\"leaves\":[";
            for(size_t n=0;n<a.leaves.size();++n){if(n)j<<',';const auto& l=a.leaves[n];j<<"{\"group\":"<<l.group<<",\"address\":\"0x"<<std::hex<<l.address<<"\",\"owner_link\":\"0x"<<l.ownerLink<<std::dec<<"\",\"base_record\":"<<l.baseRecord<<",\"record_count\":"<<l.recordCount<<'}';}j<<"],\"descriptors\":[";
            for(size_t n=0;n<a.descriptors.size();++n){if(n)j<<',';const auto& d=a.descriptors[n];j<<"{\"leaf\":"<<d.leaf<<",\"list\":"<<d.list<<",\"entry\":"<<d.entry<<",\"entry_stride\":"<<d.entryStride
                <<",\"address\":\"0x"<<std::hex<<d.address<<"\",\"source_address\":\"0x"<<d.sourceAddress<<std::dec<<"\",\"destination_relative_record\":"<<d.destinationRelativeRecord
                <<",\"record_count\":"<<d.recordCount<<",\"destination_first_record\":"<<d.destinationFirstRecord<<",\"destination_end_record\":"<<d.destinationEndRecord
                <<",\"raw_blob\":";if(d.rawBlob==kNone)j<<"null";else j<<d.rawBlob;j<<",\"payload_blob\":";if(d.payloadBlob==kNone)j<<"null";else j<<d.payloadBlob;j<<",\"status\":\""<<d.status<<"\"}";}j<<"]}";
        }
        j<<"]},\n  \"cpu_blobs\":[";
        for(size_t i=0;i<blobs_.size();++i){if(i)j<<',';const auto& b=blobs_[i];j<<"{\"id\":"<<i<<",\"kind\":\""<<b.kind<<"\",\"attempt\":"<<b.attempt<<",\"descriptor\":"<<b.descriptor
            <<",\"address\":\"0x"<<std::hex<<b.address<<std::dec<<"\",\"offset\":"<<b.fileOffset<<",\"bytes\":"<<b.fileBytes<<",\"status\":\""<<b.status<<"\"}";}
        j<<"]";
    }
};

} // namespace edvr

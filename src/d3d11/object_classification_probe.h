#pragma once
// The eye run's classification capture: what the CPU-side game-code probes
// saw during the run, written beside its drawstate files as
// classification_<stamp>.json and .bin. It never changes a live binding or
// draw.
//
// Arming it arms the record-writer probe (the 336-byte producer records and
// their KinematicRig ownership, object_record_writer_probe.h) and the
// kinematic eval probe (kinematic_eval_probe.h); their own game-code hooks
// fill their sections.
//
// The draw/mesh half retired 2026-09-23: noteDraw's nomination of the t33
// pool and instance-ID buffers, stage()'s scene, coverage and mesh-record
// copies, the resource ledger that followed nominated buffers through
// Map/Unmap/Update/Copy, and the CPU source-owner probe it fed. Nothing had
// called noteDraw or stage() since the mesh records retired (319c1c5a), so
// every capture after that left those sections empty. Captures are
// edvr_object_classification_v3 from here: executable identity, binary_ok
// and the two probes' sections. tools/object_classification.py reads v3 and
// the older v1/v2 captures alike.
#include "object_record_writer_probe.h"
#include "kinematic_eval_probe.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

namespace edvr {

class ObjectClassificationProbe {
    static std::string quote(const std::string& v) {
        std::string out="\"";char tmp[7]{};
        for(unsigned char c:v){if(c=='\"'||c=='\\')out+='\\';if(c<32){sprintf_s(tmp,"\\u%04x",unsigned(c));out+=tmp;}else out+=char(c);}
        out+='\"';return out;
    }
    static std::string narrow(const wchar_t* value) {
        if(!value)return {};
        const int n=WideCharToMultiByte(CP_UTF8,0,value,-1,nullptr,0,nullptr,nullptr);
        if(n<=1)return {};
        std::string out(size_t(n),'\0');WideCharToMultiByte(CP_UTF8,0,value,-1,&out[0],n,nullptr,nullptr);out.pop_back();return out;
    }
    static std::wstring path(const wchar_t* dir,const wchar_t* stem,const wchar_t* stamp,const wchar_t* ext) {
        std::wstring out=dir?dir:L"";if(!out.empty()&&out.back()!=L'\\'&&out.back()!=L'/')out+=L'\\';
        out+=stem;out+=stamp?stamp:L"";out+=ext;return out;
    }
    static void executableIdentity(uint32_t& timestamp,uint32_t& imageSize) noexcept {
        timestamp=imageSize=0;const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));if(!base)return;
        const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);if(dos->e_magic!=IMAGE_DOS_SIGNATURE)return;
        const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(base+dos->e_lfanew);if(nt->Signature!=IMAGE_NT_SIGNATURE)return;
        timestamp=nt->FileHeader.TimeDateStamp;imageSize=nt->OptionalHeader.SizeOfImage;
    }
    bool testIdentityOverride_=false;

public:
    // clock: the run's starting frame count, handed to both probes.
    void arm(uint32_t clock){objectRecordWriterProbe.arm(clock);kinematicEvalProbe.arm(clock);testIdentityOverride_=false;}
    void reset(){objectRecordWriterProbe.reset();kinematicEvalProbe.reset();testIdentityOverride_=false;}
    void finish(){objectRecordWriterProbe.finish();kinematicEvalProbe.finish();}
    // A fixture runs on a machine whose executable is not Elite's: publish the
    // identity the record writer expects, so the reader checks the fixture
    // the way it checks a real capture.
    void useExpectedExecutableIdentityForTest(){testIdentityOverride_=true;}

    bool write(const wchar_t* directory,const wchar_t* stamp) {
        if(!directory||!stamp)return false;
        const std::wstring binPath=path(directory,L"classification_",stamp,L".bin");
        const std::wstring jsonPath=path(directory,L"classification_",stamp,L".json");
        const std::string binName="classification_"+narrow(stamp)+".bin";
        // JSON is the publication marker.  Retire a previous marker before
        // touching its BIN so a failed rewrite cannot leave stale success
        // metadata pointing at new or partial bytes.
        if(!DeleteFileW(jsonPath.c_str())&&GetLastError()!=ERROR_FILE_NOT_FOUND)return false;
        FILE* bin=nullptr;bool binOk=_wfopen_s(&bin,binPath.c_str(),L"wb")==0&&bin;uint64_t offset=0;
        objectRecordWriterProbe.writeBinary(bin,offset,binOk);
        if(bin && fclose(bin)!=0)binOk=false;
        uint32_t peTimestamp=0,peImageSize=0;executableIdentity(peTimestamp,peImageSize);
        if(testIdentityOverride_){peTimestamp=ObjectRecordWriterProbe::kExpectedTimestamp;peImageSize=ObjectRecordWriterProbe::kExpectedImageSize;}
        std::ostringstream j;
        j<<"{\n  \"schema\":\"edvr_object_classification_v3\",\n  \"binary\":"<<quote(binName)
         <<",\n  \"executable\":{\"pe_timestamp\":"<<peTimestamp<<",\"image_size\":"<<peImageSize<<"},\n"
         <<"  \"summary\":{\"binary_ok\":"<<(binOk?"true":"false")<<"},\n";
        objectRecordWriterProbe.writeJson(j);j<<",\n";kinematicEvalProbe.writeJson(j);j<<"\n}\n";
        FILE* json=nullptr;bool jsonOk=_wfopen_s(&json,jsonPath.c_str(),L"wb")==0&&json;
        const std::string body=j.str();if(jsonOk)jsonOk=fwrite(body.data(),1,body.size(),json)==body.size();
        if(json&&fclose(json)!=0)jsonOk=false;return jsonOk&&binOk;
    }
};

inline ObjectClassificationProbe objectClassificationProbe;

} // namespace edvr

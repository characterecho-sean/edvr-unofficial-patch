// The real production boundary and CodeHook install against executable owned
// pages placed more than 2 GiB from the production callbacks.
#include <windows.h>
#include <cstdint>
static HMODULE fixtureModule=nullptr;
static HMODULE fixtureGetModule(const wchar_t*) {return fixtureModule;}
#define GetModuleHandleW fixtureGetModule
#include "../../src/d3d11/native_preview_camera.cpp"
#undef GetModuleHandleW
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>

namespace edvr {
bool fixtureKnown=true;uintptr_t fixtureOffsets[4]={0x100,0x200,0x300,0x400};
unsigned resolverCalls=0,guardFaults=0;std::string messages;
Log& Log::get(){static Log log;return log;}Log::~Log()=default;
void Log::note(const char* fmt,...){char text[1400];va_list args;va_start(args,fmt);std::vsnprintf(text,sizeof(text),fmt,args);va_end(args);messages+=text;messages+='\n';}
int guardFilter(unsigned long,const char*){++guardFaults;return EXCEPTION_EXECUTE_HANDLER;}
bool gameBuildKnown() noexcept{return fixtureKnown;}
const char* gameBuildText() noexcept{return "fixture334925";}
uintptr_t gameRva(uint32_t rva) noexcept{++resolverCalls;for(unsigned i=0;i<4;++i)if(rva==kHooks[i])return fixtureOffsets[i];return 0x1000;}
bool gameRvasFound(const uint32_t*,size_t,uint32_t*) noexcept{++resolverCalls;return true;}
bool gamePinMatches(uint32_t,const uint8_t* actual,const uint8_t* expected,size_t bytes) noexcept{return !std::memcmp(actual,expected,bytes);}
}
namespace {
unsigned checks=0,failures=0;
void check(bool value,const char* name){++checks;if(!value){++failures;std::printf("FAIL: %s\n",name);}}
uint8_t* allocateFar() {
    const auto callback=reinterpret_cast<uintptr_t>(&edvr::previewHmdHook);
    for(uintptr_t candidate=0x10000000;candidate<0x70000000;candidate+=0x1000000) {
        const uintptr_t difference=callback>candidate?callback-candidate:candidate-callback;
        if(difference<=0x90000000)continue;
        auto* p=static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate),65536,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        if(p)return p;
    }
    for(uintptr_t candidate=0x10000000000;candidate<0x10100000000;candidate+=0x1000000) {
        const uintptr_t difference=callback>candidate?callback-candidate:candidate-callback;
        if(difference<=0x90000000)continue;
        auto* p=static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate),65536,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        if(p)return p;
    }
    return nullptr;
}
void functions(uint8_t* memory) {
    using namespace edvr;
    const std::vector<std::vector<uint8_t>> epilogues={
        {0x41,0x5e,0x5f,0x5d}, // HMD: r14, rdi, rbp
        {0x48,0x83,0xc4,0x20,0x5d},
        {0x48,0x81,0xc4,0x18,0x02,0,0,0x41,0x5c,0x5f,0x5b,0x5d},
        {0x48,0x83,0xc4,0x48}};
    for(unsigned i=0;i<4;++i) {
        uint8_t* p=memory+fixtureOffsets[i];size_t at=0;
        std::memcpy(p,kPins[i],kLengths[i]);at+=kLengths[i];
        // Increment a counter in the same allocation without changing args.
        p[at++]=0xff;p[at++]=0x05;const int32_t delta=static_cast<int32_t>((memory+0x8000+i*4)-(p+at+4));
        std::memcpy(p+at,&delta,4);at+=4;
        std::memcpy(p+at,epilogues[i].data(),epilogues[i].size());at+=epilogues[i].size();
        p[at++]=0xb8;const uint32_t result=0x12340000+i;std::memcpy(p+at,&result,4);at+=4;p[at]=0xc3;
    }
    DWORD ignored=0;check(VirtualProtect(memory,4096,PAGE_EXECUTE_READ,&ignored)!=0,"synthetic pinned entries sealed executable");
    FlushInstructionCache(GetCurrentProcess(),memory,4096);
}
bool executable(void* pointer) {
    MEMORY_BASIC_INFORMATION info{};return VirtualQuery(pointer,&info,sizeof(info))&&info.State==MEM_COMMIT&&info.Protect==PAGE_EXECUTE_READ;
}
void cleanup(uint8_t* memory) {
    using namespace edvr;
    // Fixture is single-threaded and every call has returned. Production never
    // frees a successful relay, including after partial installation or stop.
    nativePreviewCameraStop();for(unsigned i=0;i<4;++i){g_previewHooks[i].uninstall();
        if(g_previewRelays[i])VirtualFree(g_previewRelays[i],0,MEM_RELEASE);g_previewRelays[i]=nullptr;g_previewOriginal[i].store(nullptr);}
    if(memory)VirtualFree(memory,0,MEM_RELEASE);
    fixtureModule=nullptr;g_previewAttempted=g_previewInstalled=g_previewLastActive=false;g_previewReportAt=g_previewLastApplied=0;
    for(unsigned i=0;i<4;++i)fixtureOffsets[i]=0x100+i*0x100;
}
uintptr_t call(uint8_t* memory,unsigned i) {
    using namespace edvr;
    if(i==1)return reinterpret_cast<Two>(memory+fixtureOffsets[i])(0,0);
    return reinterpret_cast<One>(memory+fixtureOffsets[i])(0);
}
}
int main(int argc,char** argv) {
    if(argc!=2||std::strcmp(argv[1],"--self-test"))return 2;
    using namespace edvr;
    fixtureKnown=false;nativePreviewCameraBoundary(true,true);
    check(!g_previewInstalled&&resolverCalls==0,"unknown build refuses at real boundary before resolver/install");cleanup(nullptr);fixtureKnown=true;
    nativePreviewCameraBoundary(true,true);
    check(!g_previewInstalled&&messages.find("game module not loaded")!=std::string::npos,"missing module has explicit unavailable reason");cleanup(nullptr);
    for(unsigned scenario=0;scenario<3;++scenario) {
        auto* memory=allocateFar();check(memory!=nullptr,"owned entries allocated more than 2GiB from callbacks");if(!memory)return 1;
        fixtureModule=reinterpret_cast<HMODULE>(memory);
        if(scenario==1)++fixtureOffsets[2]; // Pin passes, real CodeHook rejects unaligned third entry.
        functions(memory);
        if(scenario==2){DWORD old=0;VirtualProtect(memory,4096,PAGE_READWRITE,&old);memory[fixtureOffsets[3]]=0x90;VirtualProtect(memory,4096,PAGE_EXECUTE_READ,&old);}
        nativePreviewCameraBoundary(false,true);check(!g_previewAttempted,"wanted=false never installs");
        nativePreviewCameraBoundary(true,true);
        const unsigned installed=scenario==0?4:scenario==1?2:0;
        check(g_previewInstalled==(scenario==0),"only complete installation enables correction");
        for(unsigned i=0;i<4;++i)check(g_previewHooks[i].installed()==(i<installed),"actual installed count matches complete/partial/pin refusal");
        if(scenario==0){
            for(unsigned i=0;i<4;++i){
                check(executable(g_previewRelays[i]),"successful relay is executable read-only");
                const intptr_t distance=reinterpret_cast<intptr_t>(g_previewRelays[i])-(reinterpret_cast<intptr_t>(memory+fixtureOffsets[i])+5);
                check(distance>=INT32_MIN&&distance<=INT32_MAX,"all four patched entries reach nearby relays");
                check(call(memory,i)==0x12340000+i,"far relay reaches actual wrapper and preserves original result");
                check(reinterpret_cast<uint32_t*>(memory+0x8000)[i]==1,"each installed wrapper calls original exactly once");
            }
            nativePreviewCameraBoundary(true,false);nativePreviewCameraStop();
            for(unsigned i=0;i<4;++i){check(executable(g_previewRelays[i]),"stop retains successful executable relay");
                check(call(memory,i)==0x12340000+i&&reinterpret_cast<uint32_t*>(memory+0x8000)[i]==2,"inactive wrapper remains original-once forwarder");}
        }else if(scenario==1){
            check(!g_previewEngine.current()&&g_previewEngine.applied.load()==0,"partial install cannot enable camera writes");
            for(unsigned i=0;i<2;++i)check(executable(g_previewRelays[i])&&call(memory,i)==0x12340000+i&&reinterpret_cast<uint32_t*>(memory+0x8000)[i]==1,"partial success retains live original-once passive relay");
            check(!g_previewRelays[2]&&!g_previewOriginal[2].load()&&!g_previewRelays[3],"failed/unattempted entry retains no executable relay or dangling forward");
            check(messages.find("eight-byte aligned")!=std::string::npos,"actual CodeHook refusal preserved in diagnostics");
        }else check(messages.find("entry pin")!=std::string::npos,"all entry pins validated before publishing any hook");
        const auto attempts=resolverCalls;nativePreviewCameraBoundary(true,true);check(resolverCalls==attempts,"boundary never silently retries or duplicates installation");
        cleanup(memory);
    }
    check(guardFaults==0,"actual installer and original forwarding raise no guarded faults");
    std::printf("native_preview_install_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}

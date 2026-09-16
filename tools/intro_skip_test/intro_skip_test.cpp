// Exercise the production hooks through Windows' actual DirectShow file
// source, not only by calling the refusal wrapper with a synthetic path.
#include "../../src/d3d11/intro_skip.cpp"
#include <dshow.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
unsigned checks=0;
void check(bool ok,const char* label){++checks;if(!ok){std::printf("FAIL: %s\n",label);std::exit(1);}}
namespace edvr {
std::string testMode="screen";
bool testProbe=false;      // advanced.intro_probe, the watch's switch
bool testDevice=false;     // whether the probe's device stamp exists
std::vector<std::string> messages;
Config& Config::get(){static Config cfg;return cfg;}
std::string Config::getString(const char* key,const char* fallback)const{
    check(!strcmp(key,"fix.intro_video") && !strcmp(fallback,"screen"),"existing intro setting and default retained");return testMode;
}
bool Config::getBool(const char* key,bool fallback)const{
    check(!strcmp(key,"advanced.intro_probe") && !fallback,"the watch reads the probe's own key, default off");return testProbe;
}
// The probe's clock, stood in for: the watch line must read against the
// device when there is one and say so when there is not.
bool introProbeSinceDevice(double* seconds){if(!testDevice||!seconds)return false;*seconds=1.234;return true;}
Log& Log::get(){static Log log;return log;}
Log::~Log()=default;
void Log::note(const char* format,...){char text[4096]{};va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);messages.emplace_back(text);}
}
using namespace edvr;
struct Files {
    std::wstring dir,movies,ident,temporary,loop,story;
    Files(){
        wchar_t temp[MAX_PATH]{},name[MAX_PATH]{};
        check(GetTempPathW(MAX_PATH,temp)!=0 && GetTempFileNameW(temp,L"edv",0,name)!=0,"unique temporary fixture path");
        check(DeleteFileW(name)!=0 && CreateDirectoryW(name,nullptr)!=0,"own fixture directory created");
        dir=name;movies=dir+L"\\Movies";check(CreateDirectoryW(movies.c_str(),nullptr)!=0,"own Movies directory created");
        ident=movies+L"\\Ident_Frontier_EliteNeutral.webm";temporary=movies+L"\\intro_temp.webm";
        loop=movies+L"\\FrontEnd0.webm";story=movies+L"\\SalvationFinale.webm";
        for(const auto* p:{&ident,&temporary,&loop,&story}){
            HANDLE h=CreateFileW(p->c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            check(h!=INVALID_HANDLE_VALUE,"fixture created before hooks");
            const unsigned char data[4096]={0x1a,0x45,0xdf,0xa3};DWORD written=0;
            check(WriteFile(h,data,sizeof(data),&written,nullptr)!=0 && written==sizeof(data),"fixture contents written");CloseHandle(h);
        }
    }
    ~Files(){for(const auto* p:{&ident,&temporary,&loop,&story})DeleteFileW(p->c_str());RemoveDirectoryW(movies.c_str());RemoveDirectoryW(dir.c_str());}
};
HRESULT readerLoad(const std::wstring& path){
    ComPtr<IFileSourceFilter> reader;
    HRESULT hr=CoCreateInstance(CLSID_AsyncReader,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&reader));
    if(FAILED(hr))return hr;
    AM_MEDIA_TYPE type{};type.majortype=MEDIATYPE_Stream;
    return reader->Load(path.c_str(),&type);
}
void test(){
    check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)),"DirectShow COM initialized");
    Files files;
    introSkipConfigure(Config::get());check(!g_installTried && !g_reader,"screen default installs no file hooks");
    check(SUCCEEDED(readerLoad(files.ident)),"DirectShow can load the unmodified fixture");
    // The reported failure: even when armed, the executable's IAT never
    // sees the source filter's file opens. This assertion fails if the test
    // accidentally calls the EXE wrapper rather than the real reader.
    check(iatHookInstall("kernel32.dll","CreateFileW",reinterpret_cast<void*>(&hookCreateFileW),&g_createW),"EXE-only reproduction hook installed");
    g_armed.store(true);const auto before=g_refused.load();
    check(SUCCEEDED(readerLoad(files.ident)) && g_refused.load()==before,"EXE-only hook reproduces the missed DirectShow open");
    g_armed.store(false);iatHookUninstall(&g_createW);
    testMode="skip";introSkipConfigure(Config::get());
    check(g_armed.load() && g_readerCreateW.applied && g_reader,"skip owns the system reader import and module reference");
    check(readerLoad(files.ident)==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),"actual DirectShow ident load is refused");
    check(readerLoad(files.temporary)==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),"actual alternate intro load is refused");
    check(SUCCEEDED(readerLoad(files.loop)),"front-end video loop still loads through the same reader");
    check(SUCCEEDED(readerLoad(files.story)),"other story videos still load");
    check(g_refused.load()==before+2 && g_otherMovieOpens.load()>0,"live reader refusals and allowed movies are counted");
    introSkipTick(true);bool worked=false,readerSaid=false;
    for(const auto& line:messages){worked|=line.find("intro skip: WORKED")!=std::string::npos;readerSaid|=line.find("DirectShow CreateFileW")!=std::string::npos;}
    check(worked && readerSaid,"actual reader calls reach the success verdict and diagnostic");
    testMode="stock";introSkipConfigure(Config::get());
    check(SUCCEEDED(readerLoad(files.ident)) && g_refused.load()==before+2,"live stock forwards without replacing files");
    testMode="skip";introSkipConfigure(Config::get());
    check(readerLoad(files.ident)==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),"rearming the existing reader hook works");
    void** slot=g_readerCreateW.slot;void* original=g_readerCreateW.original;
    HMODULE held=nullptr;check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(slot),&held)!=0,"test holds reader while inspecting its restored import");
    introSkipShutdown();check(*slot==original && !g_reader,"shutdown restores the reader import before releasing its module reference");
    FreeLibrary(held);
    check(SUCCEEDED(readerLoad(files.ident)),"intro file remains loadable after shutdown");
    WIN32_FILE_ATTRIBUTE_DATA info{};
    check(GetFileAttributesExW(files.ident.c_str(),GetFileExInfoStandard,&info) && info.nFileSizeLow==4096,"intro file on disk was never changed");
    // The probe's WATCH through the actual reader: the same hooks, installed
    // forwarding, count the ident open and time it, and refuse nothing.
    auto said=[&](const char* text){for(const auto& line:messages)if(line.find(text)!=std::string::npos)return true;return false;};
    const auto refusedBefore=g_refused.load();messages.clear();g_identOpens.store(0);
    // A watch asked for once the scene has passed (the skip's tick above) is
    // declined with its own line: hooks installed now would report NO open
    // about an open they were not there for.
    testMode="screen";testProbe=true;testDevice=true;introSkipConfigure(Config::get());
    check(!g_watching.load() && said("intro probe: the movie's open watch was asked for after the first rendered scene"),"a watch asked for after the scene is declined and says so");
    testProbe=false;introSkipConfigure(Config::get());testProbe=true;introSkipConfigure(Config::get());
    check(!g_watching.load() && !said("no longer watched"),"a declined watch never stood up, so nothing stands down");
    auto lateLines=[&]{unsigned n=0;for(const auto& line:messages)if(line.find("was asked for after")!=std::string::npos)++n;return n;};
    check(lateLines()==1,"the late-watch line is said once");
    // This launch's scene has not arrived: the watch installs at startup.
    g_sceneSeen=false;messages.clear();introSkipConfigure(Config::get());
    check(g_watching.load() && !g_armed.load() && g_readerCreateW.applied && g_reader,"the watch owns the reader import without arming the skip");
    check(said("intro probe: watching the movie's open"),"the watch announces its install before any open");
    auto openLines=[&]{unsigned n=0;for(const auto& line:messages)if(line.find("intro probe: the game opened ")!=std::string::npos)++n;return n;};
    // N5 (2026-09-15): the once-guard is per API family, so an existence
    // check on the ident must not swallow the line the real open earns.
    // Through this executable's own patched import, never the hook function
    // directly: a hook forwards through the slot's saved original, which is
    // only set for imports this process actually has (GetFileAttributesW is
    // not one of them here; GetFileAttributesExW, called below, is).
    WIN32_FILE_ATTRIBUTE_DATA probeInfo{};
    check(g_attrExW.applied && GetFileAttributesExW(files.ident.c_str(),GetFileExInfoStandard,&probeInfo) && probeInfo.nFileSizeLow==4096,"a watched existence check is forwarded, not refused");
    check(openLines()==1 && said("through GetFileAttributesExW)"),"the existence check gets its own once-only open line");
    check(SUCCEEDED(readerLoad(files.ident)) && g_identOpens.load()>=2 && g_refused.load()==refusedBefore,"a watched ident open is counted, timed and forwarded");
    const auto firstOpens=g_identOpens.load();
    check(openLines()==2 && said("intro probe: the game opened Ident_Frontier_EliteNeutral.webm at +1.234 s after the device") && said("through DirectShow CreateFileW)"),"the CreateFile open still gets its own line, not swallowed by the existence check's guard");
    check(SUCCEEDED(readerLoad(files.temporary)) && g_identOpens.load()>firstOpens && openLines()==2,"a second ident open in the same family counts without a second line");
    check(SUCCEEDED(readerLoad(files.loop)) && g_refused.load()==refusedBefore,"the front end's loop is forwarded and never refused");
    introSkipTick(true);
    check(said("intro probe: the movie's open watch retires at the first rendered scene -- ") && said(" ident open(s) and "),"the watch's account prints at the scene edge");
    g_identOpens.store(0);g_identOpenNotedCreate.store(false);g_identOpenNotedAttr.store(false);testDevice=false;
    check(SUCCEEDED(readerLoad(files.ident)) && said("before any D3D11 device existed"),"with no device stamp the open line says so instead of printing zero");
    testProbe=false;introSkipConfigure(Config::get());
    check(!g_watching.load() && said("intro probe: the movie's open is no longer watched"),"the watch stands down when the probe goes off");
    introSkipShutdown();
    check(SUCCEEDED(readerLoad(files.ident)),"intro file remains loadable after the watch's shutdown");
    check(edvrIntroSkipSelftest()==7,"existing path, direct-refusal and watch regressions still pass");
    CoUninitialize();
}
int main(int argc,char** argv){
    if(argc!=2 || strcmp(argv[1],"--self-test")){std::puts("Usage: intro_skip_test --self-test");return 2;}
    test();std::printf("PASS: DirectShow intro skip (%u checks)\n",checks);
}

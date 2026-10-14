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
std::vector<std::string> messages;
Config& Config::get(){static Config cfg;return cfg;}
std::string Config::getString(const char* key,const char* fallback)const{
    check(!strcmp(key,"fix.intro_video") && !strcmp(fallback,"screen"),"existing intro setting and default retained");return testMode;
}
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
    check(edvrIntroSkipSelftest()==3,"existing path and direct-refusal regressions still pass");

    // ---- the flat profile (2026-10-14): the same key, the same hooks, a verdict that does not need eye draws -------------------
    // The parse: in flat only `skip` does anything. screen (the default), stock and the dev values play the game's own movie, so
    // the VR panel and resample read through introVideoParseFor and stay off however the key is spelled.
    for(const char* spelling:{"screen","stock","head","sharp","off","on","fsr","","SCREEN"}){
        const IntroVideoMode f=introVideoParseFor(spelling,true);
        check(!f.skip && !f.screen && !f.worldLock && f.upscale==0,"flat: every spelling but skip leaves the game's own movie");
    }
    for(const char* spelling:{"skip","SKIP","none"}){
        const IntroVideoMode f=introVideoParseFor(spelling,true),v=introVideoParseFor(spelling,false);
        check(f.skip && !f.screen && !f.worldLock && f.upscale==0,"flat: skip skips and keeps the panel and resample off");
        check(v.skip && v.screen && v.worldLock && v.upscale==2,"VR: skip is unchanged (the panel stays armed as the fallback)");
    }
    check(!introVideoParseFor("banana",true).recognised && introVideoParseFor("skip",true).recognised,"flat: an unknown spelling is still reported");
    check(introVideoParseFor("screen",false).screen && introVideoParseFor("screen",false).upscale==2 &&
          !introVideoParseFor("stock",false).screen,"VR: screen and stock are what they were");
    // The flat verdict's clock: refusals then four quiet seconds, or ninety seconds of nothing.
    check(!introFlatVerdictDue(1,500,0) && !introFlatVerdictDue(3,9000,kIntroFlatSettleMs-1) && introFlatVerdictDue(3,9000,kIntroFlatSettleMs),
          "flat: with refusals the verdict waits for the game to stop asking");
    check(!introFlatVerdictDue(0,kIntroFlatGiveUpMs-1,0) && introFlatVerdictDue(0,kIntroFlatGiveUpMs,0) && !introFlatVerdictDue(0,10000,99999),
          "flat: with none refused the verdict waits for the give-up time, whatever the quiet clock says");

    runtimeProfileSet(RuntimeProfile::Flat);
    check(runtimeFlatProfile(),"the rig is in the flat profile");
    g_armedSaid=false;g_verdictSaid=false;g_refused.store(0);g_refusalLines.store(0);g_otherMovieOpens.store(0);g_movieDrew.store(0);g_lastRefusalMs.store(0);
    messages.clear();
    testMode="screen";introSkipConfigure(Config::get());
    check(!g_armed.load() && !g_installTried && !g_reader,"flat: the default (screen) installs nothing and refuses nothing");
    testMode="stock";introSkipConfigure(Config::get());
    check(!g_armed.load() && !g_installTried,"flat: stock installs nothing either");
    testMode="skip";introSkipConfigure(Config::get());
    check(g_armed.load() && g_readerCreateW.applied && g_reader,"flat: skip arms the same DirectShow reader hook");
    bool armedFlat=false;
    for(const auto& line:messages) armedFlat|=line.find("intro skip: ARMED (flat)")!=std::string::npos;
    check(armedFlat,"flat: the ARMED line names the flat profile");
    check(readerLoad(files.ident)==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),"flat: the actual DirectShow ident load is refused");
    check(SUCCEEDED(readerLoad(files.loop)) && SUCCEEDED(readerLoad(files.story)),"flat: the front end's loops and story videos load");
    auto sawFlat=[&](const char* what){for(const auto& line:messages) if(line.find(what)!=std::string::npos) return true;return false;};
    introSkipTickFlat();
    check(!sawFlat("WORKED (flat)"),"flat: no verdict the moment after a refusal");
    introSkipTick(true);
    check(!sawFlat("intro skip: WORKED") && !sawFlat("did not take"),"flat: the eye-draw signal of the VR verdict closes nothing");
    g_lastRefusalMs.store(nowMs()-kIntroFlatSettleMs-1);
    introSkipTickFlat();
    check(sawFlat("intro skip: WORKED (flat) -- 1 refusal(s)"),"flat: four quiet seconds after the refusal the log says WORKED (flat)");
    const size_t msgCount=messages.size();
    introSkipTickFlat();
    check(messages.size()==msgCount,"flat: the verdict is said once");
    // Nothing was ever asked for: the give-up line, not a success.
    introSkipShutdown();
    g_verdictSaid=false;g_refused.store(0);g_otherMovieOpens.store(0);g_lastRefusalMs.store(0);messages.clear();
    testMode="skip";introSkipConfigure(Config::get());
    g_armedAtMs=nowMs()-kIntroFlatGiveUpMs-1;
    introSkipTickFlat();
    check(sawFlat("intro skip: no ident was asked for (flat)") && !sawFlat("WORKED"),"flat: nothing refused for ninety seconds is reported as such");
    // The VR profile is untouched by all of it: its tick still closes on a scene frame.
    introSkipShutdown();
    runtimeProfileSet(RuntimeProfile::LegacyVr);
    g_verdictSaid=false;g_refused.store(0);g_movieDrew.store(0);messages.clear();
    testMode="skip";introSkipConfigure(Config::get());
    introSkipTickFlat();
    check(!g_verdictSaid && !sawFlat("(flat)"),"VR: the flat tick does nothing outside the flat profile");
    introSkipTick(true);
    check(g_verdictSaid && sawFlat("intro skip: no ident was asked for through"),"VR: the scene-frame verdict still closes it");
    introSkipShutdown();
    CoUninitialize();
}
int main(int argc,char** argv){
    if(argc!=2 || strcmp(argv[1],"--self-test")){std::puts("Usage: intro_skip_test --self-test");return 2;}
    test();std::printf("PASS: DirectShow intro skip (%u checks)\n",checks);
}

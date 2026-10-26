// Exercise the production capture/writer without installing or invoking a game hook.
// Installation/runtime stubs count unexpected calls: this fixture must never
// install a hook or consult game/runtime producers to serialize captured data.
#include "../../src/d3d11/flat_stationary_camera_capture.cpp"
#include <string>
#include <thread>
#include <limits>

namespace edvr {
unsigned unexpectedProducerCalls=0;
Log& Log::get() { static Log log; return log; }
Log::~Log() = default;
void Log::note(const char*, ...) {}
void CodeHook::uninstall() {}
uint32_t nativeOnFootRenderFrame() { return 77; }
const char* flatCameraInjectObserveStatus() { return "fixture-installed"; }
bool CodeHook::install(void*,void*,void**,const char*,Prepare,void*) {++unexpectedProducerCalls;return false;}
bool gameBuildIdentity(uint32_t*,uint32_t*) noexcept {++unexpectedProducerCalls;return false;}
bool gameBuildKnown() noexcept {++unexpectedProducerCalls;return false;}
const char* gameBuildText() noexcept {++unexpectedProducerCalls;return "fixture";}
uintptr_t gameRva(uint32_t) noexcept {++unexpectedProducerCalls;return 0;}
bool gameRvasFound(const uint32_t*,size_t,uint32_t*) noexcept {++unexpectedProducerCalls;return false;}
bool gamePinMatches(uint32_t,const uint8_t*,const uint8_t*,size_t) noexcept {++unexpectedProducerCalls;return false;}
bool uiLayerWorldScreenHeld() {++unexpectedProducerCalls;return false;}
bool nativeFrameNowPose(EdvrNativeNowPose&) {++unexpectedProducerCalls;return false;}
bool journalGuiFocus(uint32_t*) {++unexpectedProducerCalls;return false;}
JournalRawStatus journalRawStatus() {++unexpectedProducerCalls;return {};}
bool journalStatusDockedKnown() {++unexpectedProducerCalls;return false;}
bool journalStatusDocked() {++unexpectedProducerCalls;return false;}
bool journalStatusSeatedKnown() {++unexpectedProducerCalls;return false;}
bool journalStatusSeated() {++unexpectedProducerCalls;return false;}
namespace detail {uint64_t g_uiLayerGateFrame=0,g_uiLayerNamedAt=0;}
}

// Strict syntax validation, including escapes, finite JSON numbers and complete
// consumption. Schema assertions below operate on the same parsed document text.
struct Json {
    const std::string& s; size_t at=0;
    void space(){while(at<s.size() && (s[at]==' '||s[at]=='\n'||s[at]=='\r'||s[at]=='\t'))++at;}
    bool take(char c){space();if(at<s.size()&&s[at]==c){++at;return true;}return false;}
    bool str(){if(!take('"'))return false;while(at<s.size()) {unsigned char c=s[at++];if(c=='"')return true;if(c<32)return false;
        if(c=='\\'){if(at>=s.size())return false;char e=s[at++];if(e=='u'){for(int i=0;i<4;++i){if(at>=s.size())return false;char h=s[at++];if(!((h>='0'&&h<='9')||(h>='a'&&h<='f')||(h>='A'&&h<='F')))return false;}}
            else if(std::strchr("\"\\/bfnrt",e)==nullptr)return false;}}
        return false;}
    bool number(){space();if(at<s.size()&&s[at]=='-')++at;if(at>=s.size())return false;
        if(s[at]=='0')++at;else {if(s[at]<'1'||s[at]>'9')return false;while(at<s.size()&&s[at]>='0'&&s[at]<='9')++at;}
        if(at<s.size()&&s[at]=='.'){++at;const auto start=at;while(at<s.size()&&s[at]>='0'&&s[at]<='9')++at;if(at==start)return false;}
        if(at<s.size()&&(s[at]=='e'||s[at]=='E')){++at;if(at<s.size()&&(s[at]=='+'||s[at]=='-'))++at;const auto start=at;while(at<s.size()&&s[at]>='0'&&s[at]<='9')++at;if(at==start)return false;}return true;}
    bool value(unsigned depth=0){space();if(depth>64||at>=s.size())return false;
        if(s[at]=='"')return str();
        if(take('{')){if(take('}'))return true;do{if(!str()||!take(':')||!value(depth+1))return false;}while(take(','));return take('}');}
        if(take('[')){if(take(']'))return true;do{if(!value(depth+1))return false;}while(take(','));return take(']');}
        for(const auto* word:{"null","true","false"}){const auto n=std::strlen(word);if(s.compare(at,n,word)==0){at+=n;return true;}}
        return number();}
    bool valid(){if(!value())return false;space();return at==s.size();}
};
std::string read(const wchar_t* path){FILE* f=nullptr;if(_wfopen_s(&f,path,L"rb")||!f)return {};std::string out;char b[4096];size_t n;while((n=fread(b,1,sizeof(b),f)))out.append(b,n);fclose(f);_wremove(path);return out;}
bool has(const std::string& s,const char* text){return s.find(text)!=std::string::npos;}
unsigned originalCalls=0;
void original(uintptr_t,uintptr_t,uintptr_t){++originalCalls;}
int main(int argc,char** argv){
    if(argc!=2||strcmp(argv[1],"--self-test"))return 2;
    using namespace edvr;
    if(Json{std::string("{\"a\":[null,-1.25e+2,\"\\\"\\\\\"]}")}.valid()==false ||
       Json{std::string("{\"a\":[1,]}")}.valid() || Json{std::string("[NaN]")}.valid())return 1;
    wchar_t dir[MAX_PATH],path[MAX_PATH];if(!GetTempPathW(MAX_PATH,dir)||!GetTempFileNameW(dir,L"ecj",0,path))return 1;
    flatStationaryCameraEyeArm(42,L"empty");
    if(!flatStationaryCameraEyeWrite(path))return 1;
    const auto empty=read(path);
    if(!Json{empty}.valid()||!has(empty,"\"capture_id\":\"empty\"")||!has(empty,"\"sampled\":0")||!has(empty,"\"empty\":1"))return 1;
    flatStationaryCameraEyeArm(43,L"capped");
    auto& s=*stateOrNull();auto& e=s.eye;e.ledgerFrame=43;e.nativeFrame=77;e.ownerThread=GetCurrentThreadId();e.window.boundary(43);e.live.store(e.epoch);
    s.forward.store(reinterpret_cast<uintptr_t>(&original));
    alignas(16) uint8_t camera[0x2B0]{};
    for(unsigned i=0;i<eyecamera::kSources+5;++i)rendererDetour(0,reinterpret_cast<uintptr_t>(camera),reinterpret_cast<uintptr_t>(camera));
    if(originalCalls!=eyecamera::kSources+5)return 1;
    for(unsigned i=0;i<eyecamera::kRefresh+3;++i)flatStationaryCameraEyeRefreshPre(0,reinterpret_cast<uintptr_t>(camera),11,22,i+1,false);
    flatStationaryCameraEyeRefreshPost(reinterpret_cast<uintptr_t>(camera),1,false);
    e.source[0].snapshot.observedCallerOffset=0x1234;
    e.refresh[0].observedCallerOffset=0x5678;
    std::thread foreign([&]{flatStationaryCameraEyeRefreshPre(0,reinterpret_cast<uintptr_t>(camera),11,22,9999,false);});foreign.join();
    for(auto& r:e.source){auto& snap=r.snapshot;snap.stageCount=stationcam::kStages;snap.placedCount=stationcam::kPlacedDetails;
        for(auto& a:snap.stages){a.activityCount=stationcam::kActivities;a.nameRead=true;strcpy_s(a.stackName,"quoted\"\\name");a.baseRead=true;a.basePose[0]=std::numeric_limits<float>::quiet_NaN();
            for(auto& b:a.activities){b.nameRead=true;strcpy_s(b.name,"quoted\"\\activity");b.poseRead=true;b.weightKnown=true;b.weight=std::numeric_limits<float>::infinity();}}}
    if(!flatStationaryCameraEyeWrite(path))return 1;
    const auto capped=read(path);
    if(!Json{capped}.valid()||capped.size()>16*1024*1024||!has(capped,"\"source_overflow\":5")||!has(capped,"\"refresh_overflow\":3")||
       !has(capped,"\"foreign_refresh_unavailable\":1")||!has(capped,"\"post_seen\":1")||!has(capped,"\"post_seen\":0")||
       !has(capped,"\"stages_written\":192,\"activities_written\":1024,\"placed_written\":64")||!has(capped,"\"empty\":0")||
       !has(capped,"quoted\\\"\\\\name")||!has(capped,"\"caller_rva\":4660")||!has(capped,"\"caller_rva\":22136")||has(capped,":nan")||has(capped,":inf"))return 1;
    flatStationaryCameraEyeArm(44,L"replacement");
    if(!flatStationaryCameraEyeWrite(path))return 1;const auto replacement=read(path);
    if(!Json{replacement}.valid()||!has(replacement,"\"source_calls\":0")||!has(replacement,"\"refresh_calls\":0")||!has(replacement,"\"empty\":1")||unexpectedProducerCalls)return 1;
    std::printf("eye camera writer: PASS (empty, capped, replacement JSON; source original once; refresh post/foreign/overflow; serialization quotas)\n");return 0;
}

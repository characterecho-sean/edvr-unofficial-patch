#include "../../src/common/native_render_settings.h"
#include "../../src/common/openxr_resolution_entries.h"
#include "../../src/d3d11/native_render_labels.h"
#include "../../src/common/config.h"

#include <cstdio>
#include <cwchar>
#include <cstring>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace edvr::native_render;

namespace {
unsigned checks=0, failures=0;
void check(bool value,const char* message) {
    ++checks;
    if(!value) { ++failures; std::printf("FAIL: %s\n",message); }
}

EdvrNativeRenderSizing snapshot(uint64_t generation,float requested) {
    EdvrNativeRenderSizing result{sizeof(result),EDVR_NATIVE_RENDER_SIZING_VERSION_1,generation};
    result.eyes[0]={1001,777,2000,2000};
    result.eyes[1]={997,781,1800,1600};
    result.requestedScale=requested;
    result.effectiveScale=edvr::native_render::effectiveScale(requested,result.eyes,2);
    for(unsigned eye=0;eye<2;++eye) {
        result.activeWidth[eye]=edvr::native_render::scaledDimension(
            result.eyes[eye].originalWidth,result.eyes[eye].maxWidth,result.effectiveScale);
        result.activeHeight[eye]=edvr::native_render::scaledDimension(
            result.eyes[eye].originalHeight,result.eyes[eye].maxHeight,result.effectiveScale);
    }
    result.valid=1;
    return result;
}

bool same(const EdvrNativeRenderSizing& a,const EdvrNativeRenderSizing& b) {
    return !std::memcmp(&a,&b,sizeof(a));
}

// The recorded runtime strings and sizes of 2026-09-14; the system strings
// are fixtures, not claims about any runtime (the first flight records them).
// The Oculus and VDXR maxima are assumed; Tall-cap is synthetic, the one
// fixture whose height binds before its width (8000 wide resolves to 7282).
struct Fixture { const char* name; const char* runtime; const char* system; uint32_t w,h,max; };
const Fixture kQ3Oculus{"Q3-Oculus","Oculus","Meta Quest 3",1824,1968,16384};
const Fixture kQ3Vdxr{"Q3-VDXR","VirtualDesktopXR","Meta Quest 3",3072,3264,16384};
const Fixture kQ3SteamLink{"Q3-SteamLink","SteamVR/OpenXR","Quest 3 (Steam Link)",2528,2704,8192};
const Fixture kPimaxSteamA{"Pimax-SteamVR-a","SteamVR/OpenXR","Pimax Crystal Super",4068,4016,8192};
const Fixture kPimaxSteamB{"Pimax-SteamVR-b","SteamVR/OpenXR","Pimax Crystal Super",4980,4916,8192};
const Fixture kPimaxPi{"Pimax-PiOpenXR","Pimax OpenXR","Pimax Crystal Super",5424,5356,16384};
const Fixture kTallCap{"Tall-cap","SteamVR/OpenXR","Tall Fixture",4000,4500,8192};
const Fixture* const kFixtures[]={&kQ3Oculus,&kQ3Vdxr,&kQ3SteamLink,&kPimaxSteamA,&kPimaxSteamB,&kPimaxPi,&kTallCap};

void bounds(const Fixture& f,EdvrNativeRenderViewBounds eyes[2]) {
    eyes[0]={f.w,f.h,f.max,f.max};
    eyes[1]={f.w,f.h,f.max,f.max};
}

EdvrNativeRenderSettings request(const Fixture& f) {
    EdvrNativeRenderViewBounds eyes[2]; bounds(f,eyes);
    return buildRenderSettingsRequest(eyes,f.runtime,f.system);
}

// The v2 export, in-process, with the fixture's bounds and names.
EdvrNativeRenderSettings query(const Fixture& f,BOOL* answered=nullptr) {
    EdvrNativeRenderSettings answer=request(f);
    const BOOL ok=edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_2,sizeof(answer),&answer);
    if(answered) *answered=ok;
    return answer;
}

struct Active { uint32_t w,h; };
// What the host builds from the answer: effectiveScale/scaledDimension on
// the fixture's bounds, so every assertion below is stated in pixels.
Active active(const Fixture& f,const EdvrNativeRenderSettings& answer) {
    EdvrNativeRenderViewBounds eyes[2]; bounds(f,eyes);
    const float effective=effectiveScale(answer.openxrRenderScale,eyes,2);
    return {scaledDimension(f.w,f.max,effective),scaledDimension(f.h,f.max,effective)};
}

void setValue(const char* value) { edvr::Config::get().set("fix.openxr_resolution",value); }

bool queryGives(const Fixture& f,const char* value,uint32_t w,uint32_t h,uint32_t matched,uint32_t entries) {
    setValue(value);
    BOOL ok=FALSE;
    const auto answer=query(f,&ok);
    const Active a=active(f,answer);
    if(ok!=TRUE || a.w!=w || a.h!=h || answer.matchedEntry!=matched || answer.entryCount!=entries) {
        std::printf("  %s value=\"%s\": got %ux%u matched=%u entries=%u, wanted %ux%u matched=%u entries=%u\n",
            f.name,value,a.w,a.h,answer.matchedEntry,answer.entryCount,w,h,matched,entries);
        return false;
    }
    return true;
}

uint32_t bare(const char* value,const Fixture& f) {
    EdvrNativeRenderViewBounds eyes[2]; bounds(f,eyes);
    return bareNumberToWidth(value,eyes);
}

size_t parse(const char* value,ResolutionEntry out[kResolutionEntryMax],std::vector<std::string>* skipped=nullptr) {
    return parseResolutionEntries(value,out,skipped);
}

bool token(const char* raw,const char* expected,const char* message) {
    const std::string got=headsetToken(raw,std::strlen(raw));
    const bool ok=got==expected && headsetToken(got)==got;
    if(!ok) std::printf("  headsetToken(\"%s\") = \"%s\", wanted \"%s\"\n",raw,got.c_str(),expected);
    check(ok,message);
    return ok;
}
}

int wmain(int argc,wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    if(argc==2 && !std::wcscmp(argv[1],L"--dry-run")) {
        std::puts("native_render_settings_test: dry-run (no runtime, device or files)");
        return 0;
    }
    if(argc!=2 || std::wcscmp(argv[1],L"--self-test")) return 2;

    check(sizeof(EdvrNativeRenderSettings)==184,"settings POD ABI");
    check(sizeof(EdvrNativeRenderSizing)==80,"published sizing POD ABI");
    check(edvr::native_render::clampScale(.5f)==.5f,"ordinary scale preserved");
    check(edvr::native_render::clampScale(.1f)==.25f,"low scale clamps to minimum");
    check(edvr::native_render::clampScale(3.f)==2.f,"high scale clamps to maximum");
    check(edvr::native_render::clampScale(std::numeric_limits<float>::quiet_NaN())==1.f,
          "NaN scale defaults to runtime recommendation");
    check(edvr::native_render::clampScale(std::numeric_limits<float>::infinity())==1.f,
          "infinite scale defaults to runtime recommendation");

    EdvrNativeRenderViewBounds asymmetric[2]={{1001,777,2000,2000},{997,781,1800,1600}};
    check(edvr::native_render::effectiveScale(1.f,asymmetric,2)==1.f,
          "100 percent is identity for odd asymmetric recommendations");
    check(edvr::native_render::scaledDimension(1001,2000,1.f)==1001 &&
          edvr::native_render::scaledDimension(777,2000,1.f)==777,
          "100 percent preserves odd dimensions");
    check(edvr::native_render::effectiveScale(.5f,asymmetric,2)==.5f,
          "one factor applies to both eyes");
    check(edvr::native_render::scaledDimension(1001,2000,.5f)==501 &&
          edvr::native_render::scaledDimension(777,2000,.5f)==389,
          "width and height scale independently with stable rounding");
    asymmetric[1].maxHeight=1000;
    const float limited=edvr::native_render::effectiveScale(2.f,asymmetric,2);
    check(limited>1.280f && limited<1.281f,"shared factor respects tightest asymmetric limit");
    check(edvr::native_render::scaledDimension(1001,2000,limited)<=2000 &&
          edvr::native_render::scaledDimension(781,1000,limited)<=1000,
          "scaled asymmetric eye sizes remain within maxima");
    check(edvr::native_render::effectiveScale(2.f,nullptr,0)==2.f,
          "empty diagnostics retain requested bounded scale");

    // ---- the sanitiser ---------------------------------------------------
    token("SteamVR/OpenXR","steamvr-openxr","SteamVR/OpenXR sanitises to steamvr-openxr");
    token("Oculus","oculus","Oculus sanitises to oculus");
    token("VirtualDesktopXR","virtualdesktopxr","VirtualDesktopXR sanitises to virtualdesktopxr");
    token("Pimax OpenXR","pimax-openxr","Pimax OpenXR sanitises to pimax-openxr");
    token("  Meta   Quest 3 ","meta-quest-3","surrounding and repeated spaces collapse");
    token("Quest 3 (Steam Link)","quest-3-steam-link","parentheses become separators");
    token("--Quest--","quest","leading and trailing dashes are trimmed");
    token("Quest, 3","quest-3","the entry separator cannot appear in a token");
    token("Quest\xC3\xA9 3","quest-3","bytes above 0x7F collapse to one dash");
    token("","","the empty string is empty");
    token("---/ ( ) ,","","a string of only punctuation is empty");
    token("aaaaaaaaaa bbbbbbbbbb ccccccc dddddddddd eeeeeeeeee fffffffffff",
          "aaaaaaaaaa-bbbbbbbbbb-ccccccc","a 63-byte name truncates to 30 with no trailing dash");
    token("SteamVR/OpenXR : lighthouse","steamvr-openxr-lighthouse","a driver-shaped system name keeps its runtime prefix");
    check(headsetToken("Meta Quest 3 is a long name",4)=="meta","maxBytes bounds the read");
    check(headsetToken("Oculus\0hidden",13)=="oculus","the first NUL ends the name");
    check(headsetKey("oculus","meta-quest-3")=="oculus/meta-quest-3" && headsetKey("oculus","")=="oculus",
          "headsetKey joins with a slash or stands alone");

    // ---- the graphics DLL's v2 export, in pixels --------------------------
    NativeRenderLabels labels{};
    check(!nativeRenderLabels(&labels) && labels.valid==0,"labels read invalid before any query");
    for(const Fixture* f:kFixtures)
        check(queryGives(*f,"",f->w,f->h,0,0),"an empty value is 100% of the recommendation everywhere");
    {
        setValue("");
        const auto answer=query(kQ3Oculus);
        check(answer.openxrRenderScale==1.f,"an empty value answers scale 1.0");
        check(nativeRenderLabels(&labels) && labels.valid==1 &&
              !std::strcmp(labels.runtimeName,"Oculus") && !std::strcmp(labels.systemName,"Meta Quest 3") &&
              !std::strcmp(labels.runtimeToken,"oculus") && !std::strcmp(labels.systemToken,"meta-quest-3"),
              "labels after a query carry the names sent and headsetToken of each");
    }
    check(queryGives(kQ3Oculus,"oculus/meta-quest-3:3283",3283,3542,1,1),"the Oculus entry gives 3283x3542 on the Quest 3");
    check(queryGives(kQ3Vdxr,"oculus/meta-quest-3:3283",3072,3264,0,1),
          "a width saved for Oculus does not reach VirtualDesktopXR");
    {
        setValue("oculus/meta-quest-3:3283");
        const Active a=active(kQ3Vdxr,query(kQ3Vdxr));
        check(!(a.w==5530 && a.h==5875),"the 20:05 incident (5530x5875 on VDXR) cannot recur");
    }
    check(queryGives(kQ3SteamLink,"oculus/meta-quest-3:3283",2528,2704,0,1) &&
          queryGives(kPimaxSteamA,"oculus/meta-quest-3:3283",4068,4016,0,1) &&
          queryGives(kPimaxSteamB,"oculus/meta-quest-3:3283",4980,4916,0,1) &&
          queryGives(kPimaxPi,"oculus/meta-quest-3:3283",5424,5356,0,1),
          "a width saved for Oculus does not reach SteamVR/OpenXR or Pimax OpenXR");
    check(queryGives(kPimaxSteamB,"steamvr-openxr/pimax-crystal-super:4980",4980,4916,1,1),
          "the Pimax entry at the 20:50 base is 100%");
    {
        setValue("steamvr-openxr/pimax-crystal-super:4980");
        check(query(kPimaxSteamB).openxrRenderScale==1.f,"4980 on a 4980 base is scale exactly 1.0");
    }
    check(queryGives(kPimaxSteamA,"steamvr-openxr/pimax-crystal-super:4980",4980,4916,1,1),
          "the entry survives the 4068 -> 4980 base change");
    check(queryGives(kQ3SteamLink,"steamvr-openxr/pimax-crystal-super:4980",2528,2704,0,1),
          "a width saved for one SteamVR/OpenXR system does not reach the other");
    check(queryGives(kPimaxSteamB,"steamvr-openxr:4000, steamvr-openxr/pimax-crystal-super:4980",4980,4916,1,2),
          "the specific entry wins though it is second");
    check(queryGives(kQ3SteamLink,"steamvr-openxr:4000, steamvr-openxr/pimax-crystal-super:4980",4000,4278,2,2),
          "a runtime-only entry is the fallback for an unlisted system on that runtime");
    check(queryGives(kPimaxSteamB,"steamvr-openxr/pimax-crystal-super:4980, steamvr-openxr:4000",4980,4916,1,2) &&
          queryGives(kQ3SteamLink,"steamvr-openxr/pimax-crystal-super:4980, steamvr-openxr:4000",4000,4278,2,2),
          "the same list in the other order gives the same answers");
    check(queryGives(kQ3Oculus,"Oculus/Meta-Quest-3:3283",3283,3542,1,1),
          "the entry's own tokens are sanitised before comparison");
    for(const char* legacy:{"1.8","1.0","0.90","180","3283"}) {
        for(const Fixture* f:kFixtures)
            check(queryGives(*f,legacy,f->w,f->h,0,0),"first-flight disaster: a bare number cannot reach any headset");
    }
    check(bare("1.8",kQ3Oculus)==3283 && bare("1.0",kQ3Oculus)==1824 && bare("0.90",kQ3Oculus)==1642 &&
          bare("180",kQ3Oculus)==3283 && bare("3283",kQ3Oculus)==3283,
          "bareNumberToWidth converts a fraction, a percent and a width on the Quest 3");
    check(bare("0.90",kPimaxSteamB)==4482,"bareNumberToWidth of 0.90 on the Pimax base is 4482");
    check(bare("3",kQ3Oculus)==0 && bare("24",kQ3Oculus)==0 && bare("201",kQ3Oculus)==0 && bare("455",kQ3Oculus)==0,
          "a bare 3..24 or 201..455 on a 1824 base is out of range, not a width below the clamp");
    check(bare("456",kQ3Oculus)==456,"456 is exactly a quarter of 1824 and is offered");
    check(bare("9000",kPimaxSteamB)==0,"a bare width above the effective cap is out of range");
    {
        EdvrNativeRenderViewBounds eyes[2]; bounds(kPimaxSteamB,eyes);
        bool aboveCap=false;
        check(bareNumberToWidth("9000",eyes,&aboveCap)==0 && aboveCap,"9000 on the Pimax is refused by the cap alone, and says so");
        check(bareNumberToWidth("12000",eyes,&aboveCap)==0 && !aboveCap,"12000 on the Pimax is outside the band, not merely capped");
        check(bareNumberToWidth("1e30",eyes,&aboveCap)==0 && bareNumberToWidth("99999999999",eyes,nullptr)==0 &&
              bareNumberToWidth("16384.4",eyes,nullptr)==0,
              "a whole number past uint32 is refused before any cast (no undefined behaviour)");
    }
    check(bare("abc",kQ3Oculus)==0 && bare("",kQ3Oculus)==0 && bare("1.8x",kQ3Oculus)==0,
          "bareNumberToWidth refuses anything that is not a whole number");
    {
        double n=0;
        check(parseBareNumber("0.90",&n) && n==0.9 && !parseBareNumber("abc",nullptr) &&
              !parseBareNumber("",nullptr) && !parseBareNumber("oculus/meta-quest-3:3283",nullptr),
              "parseBareNumber takes the whole string or nothing");
    }
    check(queryGives(kQ3Oculus,"oculus/meta-quest-3:4000",3648,3936,1,1),"a width above 2x is clamped to 200%");
    {
        setValue("oculus/meta-quest-3:4000");
        check(query(kQ3Oculus).openxrRenderScale==2.f,"4000 on 1824 answers scale 2.0");
        setValue("oculus/meta-quest-3:400");
        check(query(kQ3Oculus).openxrRenderScale==.25f,"400 on 1824 answers scale 0.25");
    }
    check(queryGives(kQ3Oculus,"oculus/meta-quest-3:400",456,492,1,1),"a width below a quarter is floored at 25%");
    check(queryGives(kPimaxSteamB,"steamvr-openxr/pimax-crystal-super:9000",8192,8087,1,1),
          "the runtime maximum caps 9000 on the Pimax at 8192x8087");
    {
        setValue("steamvr-openxr/pimax-crystal-super:9000");
        EdvrNativeRenderViewBounds eyes[2]; bounds(kPimaxSteamB,eyes);
        const float effective=effectiveScale(query(kPimaxSteamB).openxrRenderScale,eyes,2);
        check(effective>1.64497f && effective<1.64499f,"the Pimax cap is effective 1.644980");
    }
    check(queryGives(kTallCap,"steamvr-openxr/tall-fixture:8000",7282,8192,1,1),"the height binds first on the tall fixture");
    check(queryGives(kPimaxPi,"pimax-openxr/pimax-crystal-super:3283",3283,3242,1,1),
          "the Quest 3's width is 60.5% on the Crystal Super");
    // Round-trip: an in-range width at or under the effective cap comes back
    // exactly through the float scale; above the cap it comes back as the cap.
    {
        bool roundTrips=true;
        for(const Fixture* f:kFixtures) {
            EdvrNativeRenderViewBounds eyes[2]; bounds(*f,eyes);
            const uint32_t cap=effectiveWidthCap(eyes);
            const uint32_t widths[]={f->w,3283,4980,f->w*2,(f->w+3)/4,8191,7282};
            for(uint32_t width:widths) {
                if(width*4<f->w || width>f->w*2) continue;
                const std::string value=std::string(headsetToken(f->runtime,64))+"/"+headsetToken(f->system,64)+":"+std::to_string(width);
                setValue(value.c_str());
                const auto answer=query(*f);
                const Active a=active(*f,answer);
                const uint32_t expected=width<=cap ? width : cap;
                if(answer.matchedEntry!=1 || a.w!=expected) {
                    std::printf("  %s width %u: active %u, wanted %u (cap %u)\n",f->name,width,a.w,expected,cap);
                    roundTrips=false;
                }
            }
        }
        check(roundTrips,"every fixture width round-trips through the float scale or lands on the cap");
        EdvrNativeRenderViewBounds tall[2]; bounds(kTallCap,tall);
        EdvrNativeRenderViewBounds pimax[2]; bounds(kPimaxSteamB,pimax);
        EdvrNativeRenderViewBounds quest[2]; bounds(kQ3Oculus,quest);
        check(effectiveWidthCap(tall)==7282 && effectiveWidthCap(pimax)==8192 && effectiveWidthCap(quest)==15185,
              "the effective cap is the tighter of the width and height axes");
    }
    // Malformed tokens and the split rule.
    {
        ResolutionEntry entries[kResolutionEntryMax]; std::vector<std::string> skipped;
        check(parse("abc, oculus/meta-quest-3:3283",entries,&skipped)==1 && entries[0].width==3283 &&
              skipped.size()==1 && skipped[0]=="abc","abc is skipped and the entry after it kept");
        check(queryGives(kQ3Oculus,"abc, oculus/meta-quest-3:3283",3283,3542,1,1),"a skipped token does not spoil the list");
        skipped.clear();
        check(parse("oculus/meta-quest-3:62,5",entries,&skipped)==1 && entries[0].width==62 &&
              skipped.size()==1 && skipped[0]=="5","62 is a width and 5 a malformed token (no decimal comma)");
        for(const char* bad:{"oculus/meta-quest-3:0",":3283","oculus/:3283","oculus/meta-quest-3:20000",
                             "oculus/meta-quest-3:1824x1968","oculus/---:3283","oculus","3283","oculus/meta-quest-3:"}) {
            skipped.clear();
            const size_t n=parse(bad,entries,&skipped);
            if(n!=0 || skipped.size()!=1) std::printf("  \"%s\": %u entries, %u skipped\n",bad,unsigned(n),unsigned(skipped.size()));
            check(n==0 && skipped.size()==1,"a malformed entry is skipped and named");
        }
        check(parse("SteamVR/OpenXR/Pimax Crystal Super:4980",entries)==1 && entries[0].runtime=="steamvr" &&
              entries[0].system=="openxr-pimax-crystal-super","the raw runtime name typed by hand splits at the first slash");
        for(const Fixture* f:kFixtures)
            check(queryGives(*f,"SteamVR/OpenXR/Pimax Crystal Super:4980",f->w,f->h,0,1),
                  "the hand-typed raw name matches no fixture (copy the key from the log)");
        check(parse(" a:1 , b / c : 2 ,, d:3, ",entries)==3 && entries[0].runtime=="a" && entries[1].runtime=="b" &&
              entries[1].system=="c" && entries[1].width==2 && entries[2].runtime=="d","whitespace around / : and , is allowed");
        skipped.clear();
        check(parse("a:1,b:2,c:3,d:4,e:5,f:6,g:7,h:8,i:9",entries,&skipped)==8 && entries[7].runtime=="h" &&
              skipped.size()==1 && skipped[0]=="i:9","nine entries -> eight kept, the ninth named");
        check(queryGives(kQ3Oculus,"oculus/meta-quest-3:3283, oculus/meta-quest-3:2000",3283,3542,1,2),"a duplicate key: first wins");
        check(parse(nullptr,entries)==0 && parse("",entries)==0 && parse("   ",entries)==0,"null, empty and blank values are zero entries");
    }
    // The ABI's refusals.
    {
        setValue("oculus/meta-quest-3:3283");
        EdvrNativeRenderSettings settings=request(kQ3Oculus);
        check(edvrQueryNativeRenderSettings(1,sizeof(settings),&settings)==FALSE,"getter rejects version 1 without running");
        check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_2,16,&settings)==FALSE,"getter rejects size 16");
        check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_2,sizeof(settings)-1,&settings)==FALSE,"getter rejects short ABI");
        check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_2,sizeof(settings),nullptr)==FALSE,"getter rejects null output");
        check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_2,sizeof(settings),reinterpret_cast<void*>(1))==FALSE,
              "getter rejects bad output pointer");
        settings=request(kQ3Oculus); settings.reserved=1;
        check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_2,sizeof(settings),&settings)==FALSE,"getter rejects reserved = 1");
        settings=request(kQ3Oculus); settings.version=1;
        check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_2,sizeof(settings),&settings)==FALSE,"getter rejects an input header of version 1");
        settings=request(kQ3Oculus); settings.size=16;
        check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_2,sizeof(settings),&settings)==FALSE,"getter rejects an input header of size 16");
        settings=request(kQ3Oculus); settings.eyes[1].originalHeight=0;
        check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_2,sizeof(settings),&settings)==FALSE,"getter rejects a zero bound");
        settings=request(kQ3Oculus); settings.eyes[0].maxWidth=1000;
        check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_2,sizeof(settings),&settings)==FALSE,"getter rejects original > max");
        settings=request(kQ3Oculus); std::memset(settings.systemName,'x',sizeof(settings.systemName));
        check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_2,sizeof(settings),&settings)==FALSE,"getter rejects an unterminated name");
        settings=request(kQ3Oculus); std::memset(settings.runtimeName,'x',sizeof(settings.runtimeName));
        check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_2,sizeof(settings),&settings)==FALSE,"getter rejects an unterminated runtime name");
    }
    // Empty system token: the worn key is the runtime alone and only a
    // runtime-only entry can match.
    {
        const Fixture nameless{"nameless","SteamVR/OpenXR","---",2528,2704,8192};
        check(queryGives(nameless,"steamvr-openxr:4000",4000,4278,2,1),"an empty system token falls back to the runtime-only entry");
        check(queryGives(nameless,"steamvr-openxr/x:4000",2528,2704,0,1),"an empty system token never matches a runtime/system entry");
        check(nativeRenderLabels(&labels) && !std::strcmp(labels.systemToken,"") && !std::strcmp(labels.runtimeToken,"steamvr-openxr"),
              "the labels carry the empty system token");
        setValue("steamvr-openxr:4000");
        OpenxrResolutionReport report;
        report.runtimeName="SteamVR/OpenXR"; report.systemName="---";
        report.runtimeToken="steamvr-openxr"; report.systemToken="";
        report.value="steamvr-openxr:4000";
        ResolutionEntry entries[kResolutionEntryMax];
        report.entryCount=parse("steamvr-openxr:4000",entries); report.entries=entries;
        report.matched=2; report.width=4000;
        EdvrNativeRenderViewBounds eyes[2]; bounds(nameless,eyes); report.eyes=eyes;
        const std::string line=formatOpenxrResolutionReport(report);
        check(line.find("(SteamVR/OpenXR, runtime reported no system name)")!=std::string::npos &&
              line.find("from the runtime-only entry steamvr-openxr:4000 = 4000x4278 per eye (17.1 MP, 158.2% of the runtime's 2528x2704)")!=std::string::npos,
              "the matched=2 line names the runtime-only entry and the missing system name");
    }
    // The graphics-log line's wording and its bound.
    {
        EdvrNativeRenderViewBounds eyes[2]; bounds(kQ3Oculus,eyes);
        ResolutionEntry entries[kResolutionEntryMax];
        OpenxrResolutionReport report;
        report.runtimeName="Oculus"; report.systemName="Meta Quest 3";
        report.runtimeToken="oculus"; report.systemToken="meta-quest-3";
        report.value="oculus/meta-quest-3:3283, steamvr-openxr/pimax-crystal-super:4980";
        report.entryCount=parse(report.value.c_str(),entries); report.entries=entries;
        report.matched=1; report.width=3283; report.eyes=eyes;
        check(formatOpenxrResolutionReport(report)==
              "openxr resolution: this headset is oculus/meta-quest-3 (Oculus, \"Meta Quest 3\"); edvr.ini sets it to 3283 wide = 3283x3542 per eye "
              "(11.6 MP, 180% of the runtime's 1824x1968). Saved: oculus/meta-quest-3:3283, steamvr-openxr/pimax-crystal-super:4980. "
              "Elite's HMD Quality multiplies this.","the matched line reads as the design states it");
        EdvrNativeRenderViewBounds vdxr[2]; bounds(kQ3Vdxr,vdxr);
        report.runtimeName="VirtualDesktopXR"; report.runtimeToken="virtualdesktopxr";
        report.value="oculus/meta-quest-3:3283";
        report.entryCount=parse(report.value.c_str(),entries);
        report.matched=0; report.width=0; report.eyes=vdxr;
        check(formatOpenxrResolutionReport(report)==
              "openxr resolution: this headset is virtualdesktopxr/meta-quest-3 (VirtualDesktopXR, \"Meta Quest 3\"); edvr.ini has no entry for it, "
              "so it runs at 100% = 3072x3264 per eye (10.0 MP). Saved: oculus/meta-quest-3:3283. Set it in F8 > Performance with this headset on, "
              "or add \"virtualdesktopxr/meta-quest-3:<width>\" to fix.openxr_resolution (3072 is 100%).","the no-entry line offers the key as a template");
        EdvrNativeRenderViewBounds pimax[2]; bounds(kPimaxSteamB,pimax);
        report.runtimeName="SteamVR/OpenXR"; report.systemName="Pimax Crystal Super";
        report.runtimeToken="steamvr-openxr"; report.systemToken="pimax-crystal-super";
        report.value="0.90"; report.entryCount=0; report.eyes=pimax;
        check(formatOpenxrResolutionReport(report)==
              "openxr resolution: fix.openxr_resolution = \"0.90\" is a bare number and names no headset; this headset (steamvr-openxr/pimax-crystal-super, "
              "4980x4916) runs at 100% = 4980x4916 per eye (24.5 MP). Set it in F8 > Performance with this headset on, or replace the value with "
              "\"steamvr-openxr/pimax-crystal-super:4482\" for 4482 wide (90%).","the bare-number line offers the width it would have meant");
        report.value="1.0";
        check(formatOpenxrResolutionReport(report).find("or add \"steamvr-openxr/pimax-crystal-super:<width>\" to fix.openxr_resolution (4980 is 100%).")!=std::string::npos,
              "a bare 1.0 gets the 100% template");
        // 9000 is within a quarter to twice 4980; only the 8192 cap refuses
        // it, so the tail names the cap, not the band it is inside.
        report.value="9000";
        check(formatOpenxrResolutionReport(report).find("; \"9000\" is more than this runtime can build (8192 wide at most for this headset).")!=std::string::npos,
              "a bare number above the runtime cap names the cap");
        report.value="12000";
        check(formatOpenxrResolutionReport(report).find("; \"12000\" is out of range for this headset (a quarter to twice 4980 wide).")!=std::string::npos,
              "a bare number out of range is named as such");
        report.value="steamvr-openxr/pimax-crystal-super:9000";
        report.entryCount=parse(report.value.c_str(),entries); report.matched=1; report.width=9000;
        // 8192 x 8087 = 66,248,704: 66.2 MP, not the design doc's hand-computed 66.3.
        check(formatOpenxrResolutionReport(report).find("9000 wide = 8192x8087 per eye (66.2 MP, 164.5% of the runtime's 4980x4916, runtime cap 164.5%)")!=std::string::npos,
              "the runtime cap is named in the log");
        // Both clamps bind (12000 on 4980 is 2.41x): the runtime cap is the
        // one named, beside its own percent, as the menu names it.
        report.value="steamvr-openxr/pimax-crystal-super:12000";
        report.entryCount=parse(report.value.c_str(),entries); report.width=12000;
        check(formatOpenxrResolutionReport(report).find("12000 wide = 8192x8087 per eye (66.2 MP, 164.5% of the runtime's 4980x4916, runtime cap 164.5%)")!=std::string::npos,
              "with both clamps binding the log names the runtime cap, not 200%");
        report.value="oculus/meta-quest-3:4000"; report.runtimeToken="oculus"; report.systemToken="meta-quest-3";
        report.entryCount=parse(report.value.c_str(),entries); report.width=4000; report.eyes=eyes;
        check(formatOpenxrResolutionReport(report).find("4000 wide = 3648x3936 per eye (14.4 MP, 200% of the runtime's 1824x1968, capped at 200%)")!=std::string::npos,
              "the 2x clamp is named in the log");
        // Eight maximal entries: 30-byte tokens, five-digit widths, 63-byte
        // raw names. Under 1100 so Log::note's 1200-byte buffer holds it whole.
        std::string maximal;
        for(unsigned i=0;i<8;++i) {
            if(i) maximal+=", ";
            maximal+=std::string(29,'r')+char('a'+i)+"/"+std::string(29,'s')+char('a'+i)+":16384";
        }
        report.entryCount=parse(maximal.c_str(),entries);
        check(report.entryCount==8,"eight maximal entries parse");
        const std::string longName(63,'n');
        report.runtimeName=longName.c_str(); report.systemName=longName.c_str();
        report.runtimeToken=std::string(29,'r')+"a"; report.systemToken=std::string(29,'s')+"a";
        report.value=maximal; report.matched=1; report.width=16384; report.eyes=eyes;
        const std::string longest=formatOpenxrResolutionReport(report);
        check(longest.size()<1100,"the graphics-log line for eight maximal entries is under 1100 bytes");
        report.matched=0; report.width=0;
        check(formatOpenxrResolutionReport(report).size()<1100,"the no-entry line for eight maximal entries is under 1100 bytes");
        report.matched=2; report.width=16384;
        check(formatOpenxrResolutionReport(report).size()<1100,"the runtime-only line for eight maximal entries is under 1100 bytes");
    }
    // Echo rule.
    {
        setValue("oculus/meta-quest-3:3283");
        const EdvrNativeRenderSettings sent=request(kQ3Oculus);
        EdvrNativeRenderSettings answer=sent;
        check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_2,sizeof(answer),&answer)==TRUE &&
              validateRenderSettingsAnswer(sent,answer) && !std::memcmp(answer.eyes,sent.eyes,sizeof(sent.eyes)) &&
              !std::memcmp(answer.runtimeName,sent.runtimeName,64) && !std::memcmp(answer.systemName,sent.systemName,64) &&
              answer.size==184 && answer.version==2 && answer.reserved==0,"every input field comes back byte-equal");
        EdvrNativeRenderSettings corrupted=answer; corrupted.systemName[40]^=1;
        check(!validateRenderSettingsAnswer(sent,corrupted),"a corrupted echoed byte after the NUL is rejected");
        corrupted=answer; corrupted.runtimeName[0]^=1;
        check(!validateRenderSettingsAnswer(sent,corrupted),"a corrupted echoed runtime byte is rejected");
        corrupted=answer; corrupted.eyes[1].maxHeight^=1;
        check(!validateRenderSettingsAnswer(sent,corrupted),"a corrupted echoed bound is rejected");
        corrupted=answer; corrupted.matchedEntry=3;
        check(!validateRenderSettingsAnswer(sent,corrupted),"matchedEntry 3 is rejected");
        corrupted=answer; corrupted.entryCount=9;
        check(!validateRenderSettingsAnswer(sent,corrupted),"entryCount 9 is rejected");
        EdvrNativeRenderViewBounds eyes[2]; bounds(kQ3Oculus,eyes);
        const EdvrNativeRenderSettings built=buildRenderSettingsRequest(eyes,"0123456789","012345678901");
        bool zeroed=true;
        for(size_t i=10;i<64;++i) zeroed=zeroed && built.runtimeName[i]==0;
        for(size_t i=12;i<64;++i) zeroed=zeroed && built.systemName[i]==0;
        check(zeroed && !std::strcmp(built.runtimeName,"0123456789") && !std::strcmp(built.systemName,"012345678901") &&
              built.size==184 && built.version==2 && built.openxrRenderScale==0.f && built.matchedEntry==0 &&
              built.entryCount==0 && built.reserved==0 && built.eyes[0].originalWidth==1824 && built.eyes[1].maxHeight==16384,
              "buildRenderSettingsRequest zeroes every byte after each NUL and the outputs");
    }
    // Header round-trips.
    {
        ResolutionEntry entries[kResolutionEntryMax];
        const char* canonical="oculus/meta-quest-3:3283, steamvr-openxr:4000, pimax-openxr/pimax-crystal-super:5424";
        const size_t n=parse(canonical,entries);
        check(n==3 && formatResolutionEntries(entries,n)==canonical,"parse(format(x)) == x");
        check(formatResolutionEntries(entries,0).empty() && formatResolutionEntries(nullptr,3).empty(),"formatting nothing is empty");
        std::string out;
        check(mergeResolutionEntry("oculus/meta-quest-3:3283, steamvr-openxr/pimax-crystal-super:4980","oculus","meta-quest-3",3320,&out) &&
              out=="oculus/meta-quest-3:3320, steamvr-openxr/pimax-crystal-super:4980","merge rewrites in place preserving order");
        check(mergeResolutionEntry("oculus/meta-quest-3:3283, steamvr-openxr:4000, oculus/meta-quest-3:2000","oculus","meta-quest-3",3320,&out) &&
              out=="oculus/meta-quest-3:3320, steamvr-openxr:4000","merge drops later duplicates of the key");
        check(mergeResolutionEntry("oculus/meta-quest-3:3283","steamvr-openxr","pimax-crystal-super",4980,&out) &&
              out=="oculus/meta-quest-3:3283, steamvr-openxr/pimax-crystal-super:4980","merge appends when absent");
        check(mergeResolutionEntry("  Oculus / Meta-Quest-3 : 3283 ,abc,steamvr-openxr:4000","oculus","meta-quest-3",3283,&out) &&
              out=="oculus/meta-quest-3:3283, steamvr-openxr:4000","merge canonicalises spacing and drops malformed tokens");
        check(mergeResolutionEntry("","oculus","meta-quest-3",3283,&out) && out=="oculus/meta-quest-3:3283","merge into an empty list");
        out="untouched";
        check(!mergeResolutionEntry("a:1,b:2,c:3,d:4,e:5,f:6,g:7,h:8","i","",9,&out) && out=="untouched","merge refuses a ninth key and writes nothing");
        check(mergeResolutionEntry("a:1,b:2,c:3,d:4,e:5,f:6,g:7,h:8","h","",9,&out) && out=="a:1, b:2, c:3, d:4, e:5, f:6, g:7, h:9",
              "merge rewrites the eighth key in place");
        check(mergeResolutionEntry("oculus/meta-quest-3:3283","steamvr-openxr","",4000,&out) &&
              out=="oculus/meta-quest-3:3283, steamvr-openxr:4000","merge writes a runtime-only entry only for an empty system token");
        check(!mergeResolutionEntry("","","meta-quest-3",3283,&out) && !mergeResolutionEntry("","oculus","x",0,&out) &&
              !mergeResolutionEntry("","oculus","x",20000,&out),"merge refuses an empty runtime and a width out of range");
        check(removeResolutionEntry("oculus/meta-quest-3:3283, steamvr-openxr:4000, steamvr-openxr/pimax-crystal-super:4980",
                                    "steamvr-openxr","pimax-crystal-super")=="oculus/meta-quest-3:3283, steamvr-openxr:4000",
              "remove takes the exact key and leaves the runtime-only entry alone");
        check(removeResolutionEntry("oculus/meta-quest-3:3283","oculus","meta-quest-3").empty(),"removing the only entry empties the list");
        check(removeResolutionEntry("oculus/meta-quest-3:3283","oculus","")=="oculus/meta-quest-3:3283","removing an absent key changes nothing");
        check(formatPercent(106.9f)=="106.9" && formatPercent(180.f)=="180" && formatPercent(100.f)=="100" &&
              formatPercent(164.498f)=="164.5" && formatPercent(0.25f*100.f)=="25","formatPercent rounds to 0.1 and drops a zero decimal");
        check(std::fabs(megapixels(3283,3542)-11.63)<0.01 && std::fabs(megapixels(5530,5875)-32.49)<0.01,"megapixels");
        check(widthToScale(3283,1824)>1.7998f && widthToScale(3283,1824)<1.8f && widthToScale(0,1824)==1.f && widthToScale(3283,0)==1.f,
              "widthToScale divides, and is 1.0 when either side is 0");
        uint32_t matched=9;
        check(resolveResolutionWidth(entries,n,"oculus","meta-quest-3",&matched)==3283 && matched==1 &&
              resolveResolutionWidth(entries,n,"steamvr-openxr","anything",&matched)==4000 && matched==2 &&
              resolveResolutionWidth(entries,n,"pimax-openxr","other",&matched)==0 && matched==0 &&
              resolveResolutionWidth(entries,n,"","meta-quest-3",&matched)==0 && matched==0,
              "resolve: runtime/system beats runtime-only beats none");
    }

    // ---- sizing v1, unchanged ---------------------------------------------
    auto first=snapshot(1,.5f);
    check(edvrPublishNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(first),&first)==TRUE,"publish coherent sizing");
    EdvrNativeRenderSizing observed{};
    check(edvrQueryNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(observed),&observed)==TRUE && same(first,observed),
        "query returns the coherent published sizing");
    setValue("oculus/meta-quest-3:2500");
    check(query(kQ3Oculus).openxrRenderScale>1.37f && query(kQ3Oculus).openxrRenderScale<1.371f &&
        edvrQueryNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(observed),&observed)==TRUE && same(first,observed),
        "saved setting changes leave the active sizing unchanged until restart");
    auto invalid=first;
    invalid.eyes[0].maxWidth=1000;
    check(edvrPublishNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(invalid),&invalid)==FALSE,"publish rejects original beyond maximum");
    invalid=first; invalid.activeHeight[1]=0;
    check(edvrPublishNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(invalid),&invalid)==FALSE,"publish rejects zero active dimension");
    invalid=first; invalid.activeWidth[0]++;
    check(edvrPublishNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(invalid),&invalid)==FALSE,"publish rejects incoherent active size");
    invalid=first; invalid.valid=2;
    check(edvrPublishNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(invalid),&invalid)==FALSE,"publish rejects invalid validity flag");
    check(edvrPublishNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(first)-1,&first)==FALSE,"publish rejects short ABI");
    check(edvrPublishNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(first),reinterpret_cast<const void*>(1))==FALSE,
        "publish rejects bad input pointer");
    check(edvrPublishNativeRenderSizing(0,sizeof(first),&first)==FALSE,
        "publish rejects version");
    check(edvrQueryNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(observed),&observed)==TRUE && same(first,observed),
        "invalid publication leaves prior snapshot unchanged");

    auto newer=snapshot(2,.75f);
    check(edvrPublishNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(newer),&newer)==TRUE,"publish newer generation");
    auto stale=first;
    stale.generation=1;
    stale.valid=0;
    check(edvrPublishNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(stale),&stale)==TRUE,"stale clear is harmless");
    check(edvrQueryNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(observed),&observed)==TRUE && same(newer,observed),
        "stale clear cannot erase newer snapshot");
    check(nativeRenderLabels(&labels) && labels.valid==1,"a stale clear leaves the labels alone too");
    auto matchingClear=newer;
    matchingClear.valid=0;
    check(edvrPublishNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(matchingClear),&matchingClear)==TRUE,"matching clear accepted");
    check(edvrQueryNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(observed),&observed)==TRUE && observed.valid==0 && observed.generation==0,
        "matching clear invalidates snapshot");
    check(!nativeRenderLabels(&labels) && labels.valid==0 && !labels.runtimeToken[0],
        "matching clear invalidates the labels on the same condition");
    check(edvrQueryNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(observed)-1,&observed)==FALSE,"sizing query rejects short ABI");
    check(edvrQueryNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(observed),reinterpret_cast<void*>(1))==FALSE,"sizing query rejects bad pointer");

    std::printf("native_render_settings_test: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}

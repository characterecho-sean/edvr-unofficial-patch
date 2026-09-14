#include "../../src/common/native_render_settings.h"
#include "../../src/common/config.h"

#include <cstdio>
#include <cwchar>
#include <cstring>
#include <limits>

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
}

int wmain(int argc,wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    if(argc==2 && !std::wcscmp(argv[1],L"--dry-run")) {
        std::puts("native_render_settings_test: dry-run (no runtime, device or files)");
        return 0;
    }
    if(argc!=2 || std::wcscmp(argv[1],L"--self-test")) return 2;

    check(sizeof(EdvrNativeRenderSettings)==16,"settings POD ABI");
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

    EdvrNativeRenderSettings settings{sizeof(settings),EDVR_NATIVE_RENDER_SETTINGS_VERSION_1,0,0};
    check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_1,
        sizeof(settings),&settings)==TRUE && settings.openxrRenderScale==1.f,
        "provider getter defaults to runtime recommendation");
    edvr::Config::get().set("fix.openxr_render_scale","0.5");
    check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_1,
        sizeof(settings),&settings)==TRUE && settings.openxrRenderScale==.5f,
        "provider getter reads Config on the CPU path");
    edvr::Config::get().set("fix.openxr_render_scale","0.5junk");
    check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_1,
        sizeof(settings),&settings)==TRUE && settings.openxrRenderScale==.5f,
        "existing config numeric prefixes retain their interpreted scale");
    edvr::Config::get().set("fix.openxr_render_scale","3");
    check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_1,
        sizeof(settings),&settings)==TRUE && settings.openxrRenderScale==2.f,
        "provider getter clamps high Config values");
    edvr::Config::get().set("fix.openxr_render_scale","nan");
    check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_1,
        sizeof(settings),&settings)==TRUE && settings.openxrRenderScale==1.f,
        "provider getter defaults nonfinite Config values");
    check(edvrQueryNativeRenderSettings(0,sizeof(settings),&settings)==FALSE,
          "getter rejects version without running");
    check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_1,
        sizeof(settings)-1,&settings)==FALSE,"getter rejects short ABI");
    check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_1,
        sizeof(settings),nullptr)==FALSE,"getter rejects null output");
    check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_1,
        sizeof(settings),reinterpret_cast<void*>(1))==FALSE,"getter rejects bad output pointer");

    auto first=snapshot(1,.5f);
    check(edvrPublishNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(first),&first)==TRUE,"publish coherent sizing");
    EdvrNativeRenderSizing observed{};
    check(edvrQueryNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(observed),&observed)==TRUE && same(first,observed),
        "query returns the coherent published sizing");
    edvr::Config::get().set("fix.openxr_render_scale","0.75");
    check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_1,
        sizeof(settings),&settings)==TRUE && settings.openxrRenderScale==.75f &&
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
    auto matchingClear=newer;
    matchingClear.valid=0;
    check(edvrPublishNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(matchingClear),&matchingClear)==TRUE,"matching clear accepted");
    check(edvrQueryNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(observed),&observed)==TRUE && observed.valid==0 && observed.generation==0,
        "matching clear invalidates snapshot");
    check(edvrQueryNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(observed)-1,&observed)==FALSE,"sizing query rejects short ABI");
    check(edvrQueryNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
        sizeof(observed),reinterpret_cast<void*>(1))==FALSE,"sizing query rejects bad pointer");

    std::printf("native_render_settings_test: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}

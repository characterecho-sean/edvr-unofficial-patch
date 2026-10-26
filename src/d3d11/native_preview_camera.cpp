#include "native_preview_camera.h"
#include "native_preview_camera_core.h"
#include <windows.h>
#include <iterator>
#include "../common/code_hook.h"
#include "../common/game_build.h"
#include "../common/log.h"

namespace edvr { namespace {
// Build334925: the manager's synchronous priority-group dispatch joins camera
// producers before each stage finalizer. HMD update stores its exact service
// pose at state+C0. CustomisationVisual's task copies its selected preview
// pose unless mode==2. The strict first-maximum finalizer otherwise selects
// the earlier HMD activity when both weights are one, discarding the preview.
// These ordinary entry hooks observe producer completion; only finalization
// changes the primary XYZ rows. No getter, task, or projection API is called.
constexpr uint32_t kManagerRva=0x106D8E0,kHmdRva=0x1074140,kPreviewRva=0x1261680,kFinalizerRva=0x1070980;
constexpr uint32_t kHooks[]={kHmdRva,kPreviewRva,kFinalizerRva,kManagerRva};
constexpr uint8_t kHmdPin[]={0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x55,0x57,0x41,0x56};
constexpr uint8_t kPreviewPin[]={0x48,0x89,0x5C,0x24,0x18,0x55,0x48,0x83,0xEC,0x20};
constexpr uint8_t kFinalizerPin[]={0x48,0x8B,0xC4,0x55,0x53,0x57,0x41,0x54,0x48,0x8D,0xA8,0xC8,0xFE,0xFF,0xFF,0x48,0x81,0xEC,0x18,0x02,0x00,0x00};
constexpr uint8_t kManagerPin[]={0x48,0x83,0xEC,0x48,0x48,0x89,0x5C,0x24,0x58};
const uint8_t* const kPins[]={kHmdPin,kPreviewPin,kFinalizerPin,kManagerPin};
constexpr size_t kLengths[]={sizeof(kHmdPin),sizeof(kPreviewPin),sizeof(kFinalizerPin),sizeof(kManagerPin)};
using One=uintptr_t (*)(uintptr_t);
using Two=uintptr_t (*)(uintptr_t,uintptr_t);
previewcam::Engine g_previewEngine;
previewcam::Game g_previewGame;
CodeHook g_previewHooks[4];
std::atomic<void*> g_previewOriginal[4]{};
bool g_previewAttempted=false,g_previewInstalled=false,g_previewLastActive=false;
uint64_t g_previewReportAt=0,g_previewLastApplied=0;

bool previewRead(void*,uintptr_t at,void* out,size_t n) noexcept {
    __try {std::memcpy(out,reinterpret_cast<const void*>(at),n);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool previewWrite(void*,uintptr_t at,const void* in,size_t n) noexcept {
    if(n!=sizeof(previewcam::Pose))return false;
    __try {
        for(unsigned row=0;row<4;++row)
            std::memcpy(reinterpret_cast<void*>(at+row*16),static_cast<const uint8_t*>(in)+row*16,12);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
const previewcam::Memory g_previewMemory{previewRead,previewWrite,nullptr};
uintptr_t previewManagerHook(uintptr_t object) {
    const auto original=reinterpret_cast<One>(g_previewOriginal[3].load(std::memory_order_acquire));
    return previewcam::manager(g_previewEngine,[&]{return original(object);});
}
uintptr_t previewHmdHook(uintptr_t object) {
    const auto original=reinterpret_cast<One>(g_previewOriginal[0].load(std::memory_order_acquire));
    return previewcam::hmd(g_previewEngine,g_previewMemory,g_previewGame,object,[&]{return original(object);});
}
uintptr_t previewSourceHook(uintptr_t context,uintptr_t subscriber) {
    const auto original=reinterpret_cast<Two>(g_previewOriginal[1].load(std::memory_order_acquire));
    return previewcam::preview(g_previewEngine,g_previewMemory,g_previewGame,subscriber,[&]{return original(context,subscriber);});
}
uintptr_t previewFinalizerHook(uintptr_t child) {
    const auto original=reinterpret_cast<One>(g_previewOriginal[2].load(std::memory_order_acquire));
    return previewcam::finalizer(g_previewEngine,g_previewMemory,g_previewGame,child,[&]{return original(child);});
}
bool previewPrepare(void* trampoline,void* context) noexcept {
    static_cast<std::atomic<void*>*>(context)->store(trampoline,std::memory_order_release);return true;
}
bool previewInstall() {
    // The resolver carries addresses across builds; it does not establish the
    // CameraState offsets or scheduler ownership contract on a new layout.
    if(!gameBuildKnown()) {
        Log::get().note("native preview camera: unavailable; CameraState/task layout is verified only on build 334925 (%s)",gameBuildText());
        return false;
    }
    const auto module=GetModuleHandleW(L"EliteDangerous64.exe");if(!module)return false;
    uint32_t missing=0;
    if(!gameRvasFound(kHooks,std::size(kHooks),&missing)||
       !gameRvasFound(previewcam::kRvas,previewcam::AddressCount,&missing)) {
        Log::get().note("native preview camera: unavailable; binding +0x%X not found on %s",missing,gameBuildText());return false;
    }
    const uintptr_t base=reinterpret_cast<uintptr_t>(module);uintptr_t targets[4]{};
    for(unsigned i=0;i<4;++i) {
        targets[i]=base+gameRva(kHooks[i]);uint8_t bytes[32]{};
        if(!previewRead(nullptr,targets[i],bytes,kLengths[i])||!gamePinMatches(kHooks[i],bytes,kPins[i],kLengths[i])) {
            Log::get().note("native preview camera: unavailable; entry pin +0x%X failed on %s",kHooks[i],gameBuildText());return false;
        }
    }
    for(unsigned i=0;i<previewcam::AddressCount;++i)g_previewGame.at[i]=base+gameRva(previewcam::kRvas[i]);
    void* replacements[]={reinterpret_cast<void*>(&previewHmdHook),reinterpret_cast<void*>(&previewSourceHook),
        reinterpret_cast<void*>(&previewFinalizerHook),reinterpret_cast<void*>(&previewManagerHook)};
    const char* names[]={"native-preview-hmd","native-preview-source","native-preview-finalizer","native-preview-manager"};
    for(unsigned i=0;i<4;++i) {
        if(!g_previewHooks[i].install(reinterpret_cast<void*>(targets[i]),replacements[i],nullptr,names[i],
                                     previewPrepare,&g_previewOriginal[i])) {
            // Already-installed forwarders remain passive. Never tear down a
            // trampoline while a scheduler thread may still be inside it.
            Log::get().note("native preview camera: unavailable; %s install refused; no camera writes enabled",names[i]);return false;
        }
    }
    Log::get().note("native preview camera: armed on %s; four pinned callbacks, 16 child slots per synchronous evaluation; "
        "exact HMD/CustomisationVisual Location pair only, exact game head pose, primary XYZ only; overflow or observed overlap before commit refuses",gameBuildText());
    return true;
}
} // namespace

void nativePreviewCameraBoundary(bool wanted,bool active) {
    if(wanted&&!g_previewAttempted){g_previewAttempted=true;g_previewInstalled=previewInstall();}
    const bool enabled=wanted&&active&&g_previewInstalled;
    g_previewEngine.enable(enabled);
    const uint64_t now=GetTickCount64(),applied=g_previewEngine.applied.load();
    const bool firstApply=applied&&!g_previewLastApplied;
    if(g_previewAttempted&&(enabled!=g_previewLastActive||firstApply||!g_previewReportAt||now-g_previewReportAt>=10000)) {
        Log::get().note("native preview camera: active=%u installed=%u evaluations=%llu HMD-calls=%llu preview-calls=%llu finalizer-calls=%llu "
            "HMD-qualified=%llu preview-qualified=%llu applied=%llu refused=%llu; totals, zero callbacks means never ran",
            unsigned(enabled),unsigned(g_previewInstalled),g_previewEngine.evaluations.load(),g_previewEngine.hmdCalls.load(),
            g_previewEngine.previewCalls.load(),g_previewEngine.finalizerCalls.load(),g_previewEngine.hmdQualified.load(),
            g_previewEngine.previewQualified.load(),applied,g_previewEngine.refused.load());
        g_previewReportAt=now;
    }
    g_previewLastActive=enabled;g_previewLastApplied=applied;
}
void nativePreviewCameraStop() noexcept {g_previewEngine.enable(false);}
} // namespace edvr

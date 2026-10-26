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
constexpr uint32_t kManagerRva=0x106D8E0,kHmdRva=0x1074140,kPreviewRva=0x1261680,kFinalizerRva=0x1070980,kEyeRva=0x283FA20;
constexpr uint32_t kHooks[]={kHmdRva,kPreviewRva,kFinalizerRva,kManagerRva,kEyeRva};
constexpr uint8_t kHmdPin[]={0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x55,0x57,0x41,0x56};
constexpr uint8_t kPreviewPin[]={0x48,0x89,0x5C,0x24,0x18,0x55,0x48,0x83,0xEC,0x20};
constexpr uint8_t kFinalizerPin[]={0x48,0x8B,0xC4,0x55,0x53,0x57,0x41,0x54,0x48,0x8D,0xA8,0xC8,0xFE,0xFF,0xFF,0x48,0x81,0xEC,0x18,0x02,0x00,0x00};
constexpr uint8_t kManagerPin[]={0x48,0x83,0xEC,0x48,0x48,0x89,0x5C,0x24,0x58};
constexpr uint8_t kEyePin[]={0x48,0x8B,0xC4,0x48,0x89,0x58,0x10};
const uint8_t* const kPins[]={kHmdPin,kPreviewPin,kFinalizerPin,kManagerPin,kEyePin};
constexpr size_t kLengths[]={sizeof(kHmdPin),sizeof(kPreviewPin),sizeof(kFinalizerPin),sizeof(kManagerPin),sizeof(kEyePin)};
using One=uintptr_t (*)(uintptr_t);
using Two=uintptr_t (*)(uintptr_t,uintptr_t);
previewcam::Engine g_previewEngine;
previewcam::Game g_previewGame;
CodeHook g_previewHooks[5];
std::atomic<void*> g_previewOriginal[5]{};
uint8_t* g_previewRelays[5]{};
void* g_eyeTrampolineSlot=nullptr;
bool g_previewAttempted=false,g_previewInstalled=false,g_previewLastActive=false;
uint64_t g_previewReportAt=0,g_previewLastApplied=0;
#if defined(EDVR_PREVIEW_TEST_READ_PROBE)
void (*g_previewReadProbe)(uintptr_t,size_t) noexcept=nullptr;
#endif

bool previewRead(void*,uintptr_t at,void* out,size_t n) noexcept {
    __try {std::memcpy(out,reinterpret_cast<const void*>(at),n);}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
#if defined(EDVR_PREVIEW_TEST_READ_PROBE)
    if(g_previewReadProbe)g_previewReadProbe(at,n);
#endif
    return true;
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
uint8_t* previewRelay(uintptr_t target,void* callback) noexcept {
    // Game entries and this DLL can be farther apart than rel32. Keep the
    // atomic five-byte entry patch; an adjacent, immutable indirect jump
    // reaches the ordinary callback without changing registers or the stack.
    constexpr uintptr_t granularity=64*1024,span=0x7FFE0000;
    const uintptr_t aligned=target&~(granularity-1);
    uint8_t* relay=nullptr;
    for(uintptr_t distance=granularity;distance<=span&&!relay;distance+=granularity) {
        const uintptr_t candidates[]={aligned>=distance?aligned-distance:0,
            aligned<=UINTPTR_MAX-distance?aligned+distance:0};
        for(auto candidate:candidates)if(candidate) {
            relay=static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate),4096,
                MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
            if(relay)break;
        }
    }
    if(!relay)return nullptr;
    const uint8_t jump[]={0xFF,0x25,0,0,0,0};
    const uintptr_t destination=reinterpret_cast<uintptr_t>(callback);
    std::memcpy(relay,jump,sizeof(jump));std::memcpy(relay+6,&destination,sizeof(destination));
    DWORD oldProtect=0;
    if(!VirtualProtect(relay,4096,PAGE_EXECUTE_READ,&oldProtect)||
       !FlushInstructionCache(GetCurrentProcess(),relay,14)) {
        VirtualFree(relay,0,MEM_RELEASE);return nullptr;
    }
    return relay;
}
__declspec(noinline) bool eyeCommit(uintptr_t pose,const previewcam::EyeSnapshot& snapshot) noexcept {
    previewcam::Pose current;
    if(!previewcam::get(g_previewMemory,pose,current)||!previewcam::finite(current))return false;
    const float dx=current.v[12]-snapshot.raw[0],dy=current.v[13]-snapshot.raw[1],dz=current.v[14]-snapshot.raw[2];
    if(dx*dx+dy*dy+dz*dz>0.25f)return false;
    if(!g_previewEngine.eyeSnapshotValid(snapshot))return false;
    return g_previewMemory.write(g_previewMemory.context,pose,&snapshot.composed,sizeof(snapshot.composed));
}
void eyeApply(uintptr_t pose) noexcept {
    // The builder is entered with whatever rdx the caller left. Do not read
    // that pointer unless this evaluation published a head.
    if(!pose)return;
    previewcam::EyeSnapshot snapshot;
    if(!g_previewEngine.loadEye(snapshot))return;
    eyeCommit(pose,snapshot);
}
// Entered by a jump from the patched eye-producer entry, so the game's return
// address stays on the stack and RSP is 8 mod 16. Eight pushes keep that
// residue. sub rsp,0x88 then leaves RSP 0 mod 16, which is what a call
// requires, and [rsp, rsp+0x20) is eyeApply's shadow space. Another sub 8
// would enter eyeApply at 0 mod 16; its /O2 movaps spill faults once a head
// is published. Volatile registers are restored and the original body is
// jumped to; only the pose memory rdx points at may change.
constexpr unsigned kEyeApplyImm=59,kEyeTrampImm=120,kEyeStubBytes=130;
uint8_t* eyeStub(uintptr_t target) noexcept {
    uint8_t* page=nullptr;
    constexpr uintptr_t granularity=64*1024,span=0x7FFE0000;
    const uintptr_t aligned=target&~(granularity-1);
    for(uintptr_t distance=granularity;distance<=span&&!page;distance+=granularity) {
        const uintptr_t candidates[]={aligned>=distance?aligned-distance:0,
            aligned<=UINTPTR_MAX-distance?aligned+distance:0};
        for(auto candidate:candidates)if(candidate) {
            page=static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate),4096,
                MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
            if(page)break;
        }
    }
    if(!page)return nullptr;
    const uint8_t code[]={
        0x50,0x51,0x52,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,0x9C,
        0x48,0x81,0xEC,0x88,0x00,0x00,0x00,
        0x0F,0x29,0x44,0x24,0x20,0x0F,0x29,0x4C,0x24,0x30,0x0F,0x29,0x54,0x24,0x40,
        0x0F,0x29,0x5C,0x24,0x50,0x0F,0x29,0x64,0x24,0x60,0x0F,0x29,0x6C,0x24,0x70,
        0x48,0x8B,0x8C,0x24,0xB0,0x00,0x00,0x00,
        0x48,0xB8,0,0,0,0,0,0,0,0,
        0xFF,0xD0,
        0x0F,0x28,0x44,0x24,0x20,0x0F,0x28,0x4C,0x24,0x30,0x0F,0x28,0x54,0x24,0x40,
        0x0F,0x28,0x5C,0x24,0x50,0x0F,0x28,0x64,0x24,0x60,0x0F,0x28,0x6C,0x24,0x70,
        0x48,0x81,0xC4,0x88,0x00,0x00,0x00,
        0x9D,0x41,0x5B,0x41,0x5A,0x41,0x59,0x41,0x58,0x5A,0x59,0x58,
        0x48,0xB8,0,0,0,0,0,0,0,0,
        0xFF,0xE0};
    static_assert(sizeof(code)==kEyeStubBytes,"eye stub size");
    std::memcpy(page,code,sizeof(code));
    const auto apply=reinterpret_cast<uintptr_t>(&eyeApply);
    std::memcpy(page+kEyeApplyImm,&apply,sizeof(apply));
    g_eyeTrampolineSlot=page+kEyeTrampImm;
    return page;
}
bool eyePrepare(void* trampoline,void*) noexcept {
    if(!g_eyeTrampolineSlot||!trampoline)return false;
    std::memcpy(g_eyeTrampolineSlot,&trampoline,sizeof(trampoline));
    DWORD oldProtect=0;
    auto* page=static_cast<uint8_t*>(g_eyeTrampolineSlot)-kEyeTrampImm;
    if(!VirtualProtect(page,4096,PAGE_EXECUTE_READ,&oldProtect)||
       !FlushInstructionCache(GetCurrentProcess(),page,kEyeStubBytes)) return false;
    g_previewOriginal[4].store(trampoline,std::memory_order_release);
    return true;
}
bool previewInstall() {
    // The resolver carries addresses across builds; it does not establish the
    // CameraState offsets or scheduler ownership contract on a new layout.
    if(!gameBuildKnown()) {
        Log::get().note("native preview camera: unavailable; CameraState/task layout is verified only on build 334925 (%s)",gameBuildText());
        return false;
    }
    const auto module=GetModuleHandleW(L"EliteDangerous64.exe");
    if(!module){Log::get().note("native preview camera: unavailable; game module not loaded");return false;}
    uint32_t missing=0;
    if(!gameRvasFound(kHooks,std::size(kHooks),&missing)||
       !gameRvasFound(previewcam::kRvas,previewcam::AddressCount,&missing)) {
        Log::get().note("native preview camera: unavailable; binding +0x%X not found on %s",missing,gameBuildText());return false;
    }
    const uintptr_t base=reinterpret_cast<uintptr_t>(module);uintptr_t targets[5]{};
    for(unsigned i=0;i<5;++i) {
        targets[i]=base+gameRva(kHooks[i]);uint8_t bytes[32]{};
        if(!previewRead(nullptr,targets[i],bytes,kLengths[i])||!gamePinMatches(kHooks[i],bytes,kPins[i],kLengths[i])) {
            Log::get().note("native preview camera: unavailable; entry pin +0x%X failed on %s",kHooks[i],gameBuildText());return false;
        }
    }
    for(unsigned i=0;i<previewcam::AddressCount;++i)g_previewGame.at[i]=base+gameRva(previewcam::kRvas[i]);
    void* replacements[]={reinterpret_cast<void*>(&previewHmdHook),reinterpret_cast<void*>(&previewSourceHook),
        reinterpret_cast<void*>(&previewFinalizerHook),reinterpret_cast<void*>(&previewManagerHook),nullptr};
    const char* names[]={"native-preview-hmd","native-preview-source","native-preview-finalizer","native-preview-manager","native-preview-eye"};
    for(unsigned i=0;i<5;++i) {
        const bool eye=i==4;
        g_previewRelays[i]=eye?eyeStub(targets[i]):previewRelay(targets[i],replacements[i]);
        if(!g_previewRelays[i]) {
            Log::get().note("native preview camera: unavailable; %s near relay allocation/seal failed; no camera writes enabled",names[i]);return false;
        }
        if(!g_previewHooks[i].install(reinterpret_cast<void*>(targets[i]),g_previewRelays[i],nullptr,names[i],
                                     eye?eyePrepare:previewPrepare,eye?nullptr:&g_previewOriginal[i])) {
            // This entry was not published. Previous successful entries and
            // their relays remain process-lifetime passive forwarders.
            VirtualFree(g_previewRelays[i],0,MEM_RELEASE);g_previewRelays[i]=nullptr;
            g_previewOriginal[i].store(nullptr,std::memory_order_release);
            // Already-installed forwarders remain passive. Never tear down a
            // trampoline while a scheduler thread may still be inside it.
            Log::get().note("native preview camera: unavailable; %s install refused; no camera writes enabled; %s",names[i],g_previewHooks[i].refusal());return false;
        }
    }
    Log::get().note("native preview camera: armed on %s; five pinned callbacks, 16 child slots per synchronous evaluation; "
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

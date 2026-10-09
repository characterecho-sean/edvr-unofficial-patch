// fix.ui_quality -- the engine-side panel sizing. ui_panel_scale.h says what
// and why; ui_sizing_math.h holds the arithmetic and build 332841's bytes,
// shared with tools/ui_quality_test.
#include "ui_panel_scale.h"

#include "ui_quality_math.h"  // uiQualityFovTangent, uiQualityInternalDim, uiQualityRecommendedFromEyes
#include "ui_sizing_math.h"
#include "ui_surfaces.h"      // native temporal's lock-free accessors, uiSurfacesHmdQuality/Supersampling/DisplayWidth
#include "vscreen_res.h"      // vscreenModeAppliedWidth: one of the widths the panel budget starts from

#include "../common/log.h"
#include "../common/native_render_settings.h"  // edvrQueryNativeRenderSizing: W_out
#include "../common/runtime_profile.h"         // runtimeFlatProfile: the flat profile's inputs

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {

// flat_runtime.h's: the scene's and the output's sizes as the flat runtime's final copy last measured them. Any thread.
bool flatRuntimeSceneSizes(uint32_t* renderWidth, uint32_t* renderHeight, uint32_t* outputWidth, uint32_t* outputHeight);

namespace {

enum PatchState : uint8_t { kUntried = 0, kApplied = 1, kStoodDown = 2, kApplying = 3 };
std::atomic<uint8_t> g_patch{kUntried};
std::atomic<float> g_target{0.0f};
std::atomic<bool> g_live{false};
std::atomic<uint64_t> g_factorBits{0};  // the written factor's double bits (0: 1.0)
std::atomic<uint64_t> g_lineBits{0};    // the factor without the budget (uiPanelScaleLineFactor): the orbit lines' (0: 1.0)
std::atomic<uint64_t> g_ssBits{0};      // max(Supersampling, 1) in the written factor (0: 1.0)

volatile float* g_floats = nullptr;  // [0] 1080 x f, [1] 1920 x f, in a page within rel32
uint8_t* g_operand[4] = {};          // site 0's 1080 and 1920 disp32s, then site 1's
int32_t g_origDisp[4] = {};
char g_why[240] = "";

// The render thread's own (uiPanelScaleFrameBoundary).
double g_written = 1.0;
double g_pending = -1.0;
uint32_t g_settle = 0;
constexpr uint32_t kSettleFrames = kUiPanelSettleFrames;  // the inputs steady this long before a write
uint32_t g_frame = 0, g_liveSince = 0, g_writes = 0;
UiPanelInputs g_lastInputs;
UiPanelPlan g_lastPlan;
double g_lastSsEff = 1.0;  // max(Supersampling, 1) in the last write: a move of it is written past the settle

// ------------------------------------------- the game's live Supersampling (2026-10-08)
// ui_sizing_math.h ("the game's LIVE Supersampling") says what is read and why. The two virtual slots are
// replaced by thunks that forward untouched; they remember the render context the interface object holds, and
// the setter's moves the factor to the new Supersampling BEFORE the original runs (the game's reconfigure,
// and the panels it recreates, follow the setter). Everything the thunks touch is atomic or under g_floatLock.
SRWLOCK g_floatLock = SRWLOCK_INIT;  // the float page's protection toggle and stores: the render thread and the thunk
constexpr int kCtxSlots = 4;
std::atomic<uintptr_t> g_ctx[kCtxSlots];            // distinct render contexts a hooked slot has been called on
std::atomic<uint32_t> g_setterCalls{0}, g_preWrites{0};
std::atomic<bool> g_getterSeen{false};
std::atomic<uint64_t> g_pubFormulaBits{0}, g_pubBaseBits{0};  // the render thread's last plan, for the setter thunk
std::atomic<bool> g_pubReady{false};
std::atomic<uint32_t> g_chosenSsBits{0};            // the Supersampling the factor was last made from
std::atomic<uint8_t> g_chosenSource{0};             // ...and its UiSsSource
enum HookState : uint8_t { kHookNone = 0, kHookInstalled = 1, kHookRefused = 2 };
uint8_t g_hookState = kHookNone;
char g_hookWhy[200] = "";
uint8_t* g_slotAt[2] = {};                          // setter's slot, getter's slot (in the game's .rdata)
uint64_t g_slotOrig[2] = {};
// The render thread's log state for the source (uiPanelScaleFrameBoundary).
bool g_srcKnown = false, g_liveUsedOnce = false;
UiSsSource g_srcLogged = UiSsSource::kNone;
UiSsWhy g_whyLogged = UiSsWhy::kNone;
UiSsPick g_pick;                                    // the last choice, for the 30 s line
float g_liveCur = 0.0f, g_liveLo = 0.0f, g_liveHi = 0.0f, g_fxcfgSs = 0.0f;

// ------------------------------------------- the flat profile (2026-10-09; ui_sizing_math.h's uiFlatPanelPlanFor)
std::atomic<bool> g_flatTemporal{false};      // the flat profile's anti-aliasing is on (uiPanelScaleSetFlatTemporal)
std::atomic<bool> g_pubFlat{false};           // the published plan is the flat one: the setter thunk scales it by the move
std::atomic<uint32_t> g_pubFlatSsBits{0};     // ...from the live Supersampling it was made beside
std::atomic<uint64_t> g_pubFlatDims{0};       // ...and the scene size it was made beside (render W << 32 | H)
// A move of the factor the setter thunk made while the plan is flat: the number of them, and the scene's size when the last one ran. The boundary holds the thunk's factor
// until the scene's size differs from it (UiFlatPanelSettle). Written by the thunk, read by the render thread.
std::atomic<uint32_t> g_flatSetterEpoch{0};
std::atomic<uint64_t> g_flatSetterAt{0};
// The render thread's own.
UiFlatPanelSettle g_flatSettle;               // the plan's settle and the setter's held factor (ui_sizing_math.h)
uint64_t g_flatHeldFrames = 0, g_flatArrivals = 0, g_flatTimeouts = 0;   // boundaries a setter's factor was held / sizes that followed / waits that gave up (all time)
UiFlatPanelInputs g_flatLastIn;               // the inputs of the last write
UiFlatPanelRefuse g_flatRefuse = UiFlatPanelRefuse::kUnknown;  // this frame's refusal (kNone: the inputs make a plan)
uint64_t g_flatRefusedFrames = 0;             // frames the inputs were refused, this 30 s window
bool g_flatAaOff = false;                     // the factor is held at 1 because the anti-aliasing is off

// ------------------------------------------------------------ the checks

// No destructors in the guarded readers: __try is illegal where unwinding is
// needed, and reading the game's image is where a guard belongs.
bool readGame(const uint8_t* at, void* out, size_t n) {
    __try {
        std::memcpy(out, at, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool imageStamp(const uint8_t* base, uint32_t* stamp, uint32_t* size) {
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        *stamp = nt->FileHeader.TimeDateStamp;
        *size = nt->OptionalHeader.SizeOfImage;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// How often the shape occurs in .text: the version-free fact the stand-down
// line gives for the next build (~0xFFFFFFFF when the scan faulted).
uint32_t shapeCount(const uint8_t* base) {
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const auto* sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (std::memcmp(sec->Name, ".text", 5) != 0) continue;
            return uiPanelScan(base + sec->VirtualAddress, sec->Misc.VirtualSize, sec->VirtualAddress,
                               nullptr, 0);
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xFFFFFFFFu;
    }
}

void hexOf(const uint8_t* p, size_t n, char* out, size_t cap) {
    size_t used = 0;
    out[0] = '\0';
    for (size_t i = 0; i < n && used + 4 < cap; ++i) {
        const int m = std::snprintf(out + used, cap - used, "%s%02X", i ? " " : "", p[i]);
        if (m < 0) break;
        used += static_cast<size_t>(m);
    }
}

// Build 332841, exactly: the stamp and size, both sites' bytes and the
// constants they read, the two functions' prologues. False, with g_why,
// on the first mismatch.
bool checkBuild(const uint8_t* base) {
    uint32_t stamp = 0, size = 0;
    if (!imageStamp(base, &stamp, &size)) {
        std::snprintf(g_why, sizeof(g_why), "the game's PE headers could not be read");
        return false;
    }
    if (stamp != kUiPanelStamp || size != kUiPanelImageSize) {
        std::snprintf(g_why, sizeof(g_why),
                      "the game is not build 332841 (PE stamp %u, image %u bytes; 332841's are %u, %u)",
                      stamp, size, kUiPanelStamp, kUiPanelImageSize);
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        uint8_t got[kUiPanelShapeBytes] = {};
        if (!readGame(base + kUiPanelSiteRva[i], got, sizeof(got))) {
            std::snprintf(g_why, sizeof(g_why), "the site at 0x%X could not be read", kUiPanelSiteRva[i]);
            return false;
        }
        if (std::memcmp(got, kUiPanelSiteBytes[i], sizeof(got)) != 0) {
            char hex[120];
            hexOf(got, sizeof(got), hex, sizeof(hex));
            std::snprintf(g_why, sizeof(g_why), "the bytes at 0x%X are not build 332841's: %s",
                          kUiPanelSiteRva[i], hex);
            return false;
        }
        const UiPanelTargets t = uiPanelTargets(got, kUiPanelSiteRva[i]);
        if (t.aspect != kUiPanelAspectRva || t.d1080 != kUiPanel1080Rva || t.d1920 != kUiPanel1920Rva) {
            std::snprintf(g_why, sizeof(g_why), "the site at 0x%X reads 0x%X/0x%X/0x%X, not the constants",
                          kUiPanelSiteRva[i], t.aspect, t.d1080, t.d1920);
            return false;
        }
    }
    uint8_t p0[sizeof(kUiPanelProlog0)] = {}, p1[sizeof(kUiPanelProlog1)] = {};
    if (!readGame(base + kUiPanelFuncRva[0], p0, sizeof(p0)) ||
        !readGame(base + kUiPanelFuncRva[1], p1, sizeof(p1)) ||
        std::memcmp(p0, kUiPanelProlog0, sizeof(p0)) != 0 || std::memcmp(p1, kUiPanelProlog1, sizeof(p1)) != 0) {
        std::snprintf(g_why, sizeof(g_why), "the prologue of FUN_144570500 or FUN_144571180 differs");
        return false;
    }
    float aspect = 0.0f, d1080 = 0.0f, d1920 = 0.0f;
    if (!readGame(base + kUiPanelAspectRva, &aspect, 4) || !readGame(base + kUiPanel1080Rva, &d1080, 4) ||
        !readGame(base + kUiPanel1920Rva, &d1920, 4) || aspect != 16.0f / 9.0f || d1080 != 1080.0f ||
        d1920 != 1920.0f) {
        std::snprintf(g_why, sizeof(g_why), "the constants read %.7g, %.7g, %.7g (not 16/9, 1080, 1920)",
                      static_cast<double>(aspect), static_cast<double>(d1080), static_cast<double>(d1920));
        return false;
    }
    return true;
}

// ------------------------------------------------------------ the writes

// A page within rel32 of `target` (kinematic_eval_hook.cpp's allocateRelay,
// for data: read-write while filled, read-only after, never executable).
uint8_t* allocateNear(uintptr_t target) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const uintptr_t granularity = info.dwAllocationGranularity;
    const uintptr_t floor = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const uintptr_t ceiling = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    const uintptr_t distance = uintptr_t(INT32_MAX) - 0x10000u;
    uintptr_t at = target > distance ? target - distance : floor;
    if (at < floor) at = floor;
    const uintptr_t limit = target > ceiling - distance ? ceiling : target + distance;
    while (at < limit) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(at), &region, sizeof(region))) break;
        const uintptr_t start = reinterpret_cast<uintptr_t>(region.BaseAddress);
        if (region.RegionSize > UINTPTR_MAX - start) break;
        const uintptr_t end = start + region.RegionSize;
        if (region.State == MEM_FREE) {
            uintptr_t candidate = at > start ? at : start;
            if (candidate > UINTPTR_MAX - (granularity - 1)) break;
            candidate = (candidate + granularity - 1) & ~(granularity - 1);
            if (candidate < limit && candidate < end && end - candidate >= 4096) {
                void* p = VirtualAlloc(reinterpret_cast<void*>(candidate), 4096, MEM_RESERVE | MEM_COMMIT,
                                       PAGE_READWRITE);
                if (p) return static_cast<uint8_t*>(p);
            }
        }
        if (end <= at) break;
        at = end;
    }
    return nullptr;
}

// One disp32 in the game's code (vscreen_res.cpp's writeImm).
bool writeDisp(uint8_t* at, int32_t disp) {
    DWORD prot = 0;
    if (!VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &prot)) return false;
    std::memcpy(at, &disp, 4);
    DWORD ignored = 0;
    VirtualProtect(at, 4, prot, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, 4);
    return true;
}

uint64_t doubleBitsOrZero(double v) {
    if (v == 1.0) return 0;
    uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

double doubleFromBitsOrOne(uint64_t bits) {
    if (!bits) return 1.0;
    double v = 1.0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

// The two floats: data stores, aligned, each one atomic for the game's reads. `lineF` is the factor without the
// budget and `ss` the Supersampling in it, for the readers beside the panels (the orbit lines, the chain line).
bool writeFloats(double f, double lineF = 1.0, double ss = 1.0) {
    if (!g_floats) return false;
    float d1080 = 1080.0f, d1920 = 1920.0f;
    uiPanelDivisors(f, &d1080, &d1920);
    // The render thread and the setter thunk both write: one at a time, or one's READONLY lands under the other's store.
    AcquireSRWLockExclusive(&g_floatLock);
    DWORD prot = 0;
    const bool ok = VirtualProtect(const_cast<float*>(g_floats), 8, PAGE_READWRITE, &prot) != 0;
    if (ok) {
        g_floats[0] = d1080;
        g_floats[1] = d1920;
        DWORD ignored = 0;
        VirtualProtect(const_cast<float*>(g_floats), 8, PAGE_READONLY, &ignored);
        g_lineBits.store(doubleBitsOrZero(lineF), std::memory_order_release);
        g_ssBits.store(doubleBitsOrZero(ss), std::memory_order_release);
        g_factorBits.store(doubleBitsOrZero(f), std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_floatLock);
    return ok;
}

// ---------------------------------------------------- the live Supersampling: the thunks

void noteCtx(uintptr_t ctx) {
    if (!ctx) return;
    for (int i = 0; i < kCtxSlots; ++i) {
        uintptr_t seen = g_ctx[i].load(std::memory_order_acquire);
        if (seen == ctx) return;
        if (!seen) {
            uintptr_t none = 0;
            if (g_ctx[i].compare_exchange_strong(none, ctx, std::memory_order_acq_rel)) return;
            if (none == ctx) return;
        }
    }
}

bool readFloatAt(uintptr_t at, float* out) { return readGame(reinterpret_cast<const uint8_t*>(at), out, sizeof(float)); }

// The factor for Supersampling `ss` from what the render thread last published, written when it is not the one the
// floats hold. No log here (a game thread): the frame boundary says what it sees.
void moveFactorTo(float ss) {
    if (!g_pubReady.load(std::memory_order_acquire)) return;
    const uint64_t fb = g_pubFormulaBits.load(std::memory_order_acquire), bb = g_pubBaseBits.load(std::memory_order_acquire);
    double formula = 0.0, base = 0.0;
    std::memcpy(&formula, &fb, sizeof(formula));
    std::memcpy(&base, &bb, sizeof(base));
    if (!(formula > 0.0) || !(base > 0.0)) return;
    UiPanelPlan p;
    if (g_pubFlat.load(std::memory_order_acquire)) {
        // Flat: R carries the Supersampling, so the plan scales by its move (ui_sizing_math.h's uiFlatPanelMove).
        const uint32_t sb = g_pubFlatSsBits.load(std::memory_order_acquire);
        float from = 0.0f;
        std::memcpy(&from, &sb, sizeof(from));
        if (!uiFlatPanelMove(formula, base, from, ss, &p)) return;
    } else {
        uiPanelSolve(formula, base, UiPanelBase::kObserved, ss, &p);
    }
    if (std::fabs(p.f - uiPanelScaleFactor()) <= 1e-9 && std::fabs(p.lineF - uiPanelScaleLineFactor()) <= 1e-9) return;
    if (writeFloats(p.f, p.lineF, p.ss)) {
        g_preWrites.fetch_add(1, std::memory_order_relaxed);
        if (g_pubFlat.load(std::memory_order_acquire)) {
            // Flat: the factor the thunk wrote is for the size the game is about to reconfigure to. The boundary holds it until the scene's size has followed, so the
            // size it has now is recorded (the plan's own when the runtime cannot say), then the count that tells the boundary there is something to hold.
            uint32_t w = 0, h = 0, ow = 0, oh = 0;
            uint64_t at = g_pubFlatDims.load(std::memory_order_acquire);
            if (flatRuntimeSceneSizes(&w, &h, &ow, &oh) && w && h) at = (static_cast<uint64_t>(w) << 32) | h;
            g_flatSetterAt.store(at, std::memory_order_release);
            g_flatSetterEpoch.fetch_add(1, std::memory_order_acq_rel);
        }
    }
}

// Before the game's own setter runs: the context, and the factor for the value it is about to store.
void setterBeforeImpl(void* self, float x) {
    uintptr_t ctx = 0;
    if (!readGame(reinterpret_cast<const uint8_t*>(self) + kUiSsCtxFromThis, &ctx, sizeof(ctx)) || !ctx) return;
    noteCtx(ctx);
    g_setterCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_patch.load(std::memory_order_acquire) != kApplied || !g_live.load(std::memory_order_acquire) ||
        !(g_target.load(std::memory_order_acquire) > 0.0f))
        return;
    float lo = 0.0f, hi = 0.0f;
    if (!readFloatAt(ctx + kUiSsOffMin, &lo) || !readFloatAt(ctx + kUiSsOffMax, &hi)) return;
    // FUN_1428767D0: x below the minimum is the minimum, else the smaller of the maximum and x.
    const float stored = (lo > x) ? lo : ((hi < x) ? hi : x);
    if (!uiLiveSupersamplingValid(stored, lo, hi, nullptr)) return;
    moveFactorTo(stored);
}

// After it: what the game really stored (the same unless the model of its clamp is off), and the factor for it.
void setterAfterImpl(void* self) {
    uintptr_t ctx = 0;
    float cur = 0.0f, lo = 0.0f, hi = 0.0f;
    if (!readGame(reinterpret_cast<const uint8_t*>(self) + kUiSsCtxFromThis, &ctx, sizeof(ctx)) || !ctx ||
        !readFloatAt(ctx + kUiSsOffCur, &cur) || !readFloatAt(ctx + kUiSsOffMin, &lo) ||
        !readFloatAt(ctx + kUiSsOffMax, &hi) || !uiLiveSupersamplingValid(cur, lo, hi, nullptr))
        return;
    if (g_patch.load(std::memory_order_acquire) == kApplied && g_live.load(std::memory_order_acquire) &&
        g_target.load(std::memory_order_acquire) > 0.0f)
        moveFactorTo(cur);
}

void getterNoteImpl(void* self) {
    uintptr_t ctx = 0;
    if (readGame(reinterpret_cast<const uint8_t*>(self) + kUiSsCtxFromThis, &ctx, sizeof(ctx))) noteCtx(ctx);
    g_getterSeen.store(true, std::memory_order_relaxed);
}

// SEH around each (no destructors in any of them; a fault in the game's memory must not become the game's crash).
void guardedRun(void (*fn)(void*, float), void* self, float x) {
    __try {
        fn(self, x);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

using SetterFn = void(__fastcall*)(void*, float);
using GetterFn = float(__fastcall*)(void*);
std::atomic<SetterFn> g_origSetter{nullptr};
std::atomic<GetterFn> g_origGetter{nullptr};

void setterBeforeThunk(void* self, float x) { setterBeforeImpl(self, x); }
void setterAfterThunk(void* self, float) { setterAfterImpl(self); }
void getterNoteThunk(void* self, float) { getterNoteImpl(self); }

void __fastcall hookedSetter(void* self, float x) {
    guardedRun(setterBeforeThunk, self, x);
    const SetterFn orig = g_origSetter.load(std::memory_order_acquire);
    if (orig) orig(self, x);
    guardedRun(setterAfterThunk, self, x);
}

float __fastcall hookedGetter(void* self) {
    guardedRun(getterNoteThunk, self, 0.0f);
    return g_origGetter.load(std::memory_order_acquire)(self);
}

bool writeSlot(uint8_t* slot, uint64_t value) {
    DWORD prot = 0;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &prot)) return false;
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(slot), static_cast<LONG64>(value));
    DWORD ignored = 0;
    VirtualProtect(slot, 8, prot, &ignored);
    return true;
}

// Called by apply() once the build is checked: both functions' bytes and both slots' values must be build 332841's.
void installLiveHooks(const uint8_t* base) {
    const uint8_t* const fn[2] = {base + kUiSsSetterRva, base + kUiSsGetterRva};
    const uint8_t* const want[2] = {kUiSsSetterBytes, kUiSsGetterBytes};
    const size_t len[2] = {sizeof(kUiSsSetterBytes), sizeof(kUiSsGetterBytes)};
    const uint32_t slotRva[2] = {kUiSsSetterSlotRva, kUiSsGetterSlotRva};
    const char* const name[2] = {"setter", "getter"};
    for (int i = 0; i < 2; ++i) {
        uint8_t got[64] = {};
        if (!readGame(fn[i], got, len[i]) || std::memcmp(got, want[i], len[i]) != 0) {
            std::snprintf(g_hookWhy, sizeof(g_hookWhy), "the Supersampling %s's bytes at 0x%X are not build 332841's",
                          name[i], i == 0 ? kUiSsSetterRva : kUiSsGetterRva);
            g_hookState = kHookRefused;
            return;
        }
        uint64_t value = 0;
        if (!readGame(base + slotRva[i], &value, sizeof(value)) || value != reinterpret_cast<uint64_t>(fn[i])) {
            std::snprintf(g_hookWhy, sizeof(g_hookWhy), "the %s's virtual slot at 0x%X does not hold the function",
                          name[i], slotRva[i]);
            g_hookState = kHookRefused;
            return;
        }
    }
    g_origSetter.store(reinterpret_cast<SetterFn>(const_cast<uint8_t*>(fn[0])), std::memory_order_release);
    g_origGetter.store(reinterpret_cast<GetterFn>(const_cast<uint8_t*>(fn[1])), std::memory_order_release);
    for (int i = 0; i < 2; ++i) {
        g_slotAt[i] = const_cast<uint8_t*>(base) + slotRva[i];
        g_slotOrig[i] = reinterpret_cast<uint64_t>(fn[i]);
    }
    if (!writeSlot(g_slotAt[0], reinterpret_cast<uint64_t>(&hookedSetter))) {
        std::snprintf(g_hookWhy, sizeof(g_hookWhy), "the setter's slot could not be written");
        g_hookState = kHookRefused;
        return;
    }
    if (!writeSlot(g_slotAt[1], reinterpret_cast<uint64_t>(&hookedGetter))) {
        writeSlot(g_slotAt[0], g_slotOrig[0]);
        std::snprintf(g_hookWhy, sizeof(g_hookWhy), "the getter's slot could not be written; the setter's put back");
        g_hookState = kHookRefused;
        return;
    }
    g_hookState = kHookInstalled;
}

// The render thread, once a frame: every captured context's entry, the believable largest, or why not.
UiSsRead readLiveSupersampling(float* curOut, float* loOut, float* hiOut) {
    UiSsRead state = UiSsRead::kNone;
    bool haveValid = false, haveBad = false;
    float bc = 0.0f, bl = 0.0f, bh = 0.0f, xc = 0.0f, xl = 0.0f, xh = 0.0f;
    for (int i = 0; i < kCtxSlots; ++i) {
        const uintptr_t ctx = g_ctx[i].load(std::memory_order_acquire);
        if (!ctx) continue;
        float c = 0.0f, l = 0.0f, h = 0.0f;
        if (!readFloatAt(ctx + kUiSsOffCur, &c) || !readFloatAt(ctx + kUiSsOffMin, &l) ||
            !readFloatAt(ctx + kUiSsOffMax, &h)) {
            if (state == UiSsRead::kNone) state = UiSsRead::kFault;
            continue;
        }
        state = UiSsRead::kOk;
        if (uiLiveSupersamplingValid(c, l, h, nullptr)) {
            if (!haveValid || c > bc) {
                bc = c;
                bl = l;
                bh = h;
            }
            haveValid = true;
        } else if (!haveBad) {
            xc = c;
            xl = l;
            xh = h;
            haveBad = true;
        }
    }
    if (haveValid) {
        *curOut = bc;
        *loOut = bl;
        *hiOut = bh;
    } else if (haveBad) {
        *curOut = xc;
        *loOut = xl;
        *hiOut = xh;
    }
    return state;
}

void standDown(const uint8_t* base) {
    g_patch.store(kStoodDown, std::memory_order_release);
    const uint32_t shapes = base ? shapeCount(base) : 0;
    Log::get().note("ui quality: panels: STANDING DOWN, nothing written -- %s (the panel formula's shape "
                    "occurs %u time(s) in the game's code). The panels stay at the game's own size "
                    "until the patch is re-keyed for this build; the sizing chains below are the "
                    "record to re-key it against.",
                    g_why, shapes);
}

// The first time the key is on: check the build, place the floats (the
// game's own values), swap the four operands -- all or none.
void apply() {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (!base || !checkBuild(base)) {
        if (!base) std::snprintf(g_why, sizeof(g_why), "the game module could not be found");
        standDown(base);
        return;
    }
    uint8_t* page = allocateNear(reinterpret_cast<uintptr_t>(base) + kUiPanelSiteRva[0]);
    if (!page) {
        std::snprintf(g_why, sizeof(g_why), "no free page within rel32 of the sites");
        standDown(base);
        return;
    }
    g_floats = reinterpret_cast<volatile float*>(page);
    g_floats[0] = 1080.0f;
    g_floats[1] = 1920.0f;
    DWORD old = 0;
    VirtualProtect(page, 4096, PAGE_READONLY, &old);
    int32_t newDisp[4] = {};
    for (int i = 0; i < 2; ++i) {
        uint8_t* site = const_cast<uint8_t*>(base) + kUiPanelSiteRva[i];
        g_operand[2 * i] = site + kUiPanel1080Disp;
        g_operand[2 * i + 1] = site + kUiPanel1920Disp;
        g_origDisp[2 * i] = uiReadDisp(site + kUiPanel1080Disp);
        g_origDisp[2 * i + 1] = uiReadDisp(site + kUiPanel1920Disp);
        if (!uiPanelDisp(reinterpret_cast<uint64_t>(site + kUiPanel1080Next),
                         reinterpret_cast<uint64_t>(&g_floats[0]), &newDisp[2 * i]) ||
            !uiPanelDisp(reinterpret_cast<uint64_t>(site + kUiPanel1920Next),
                         reinterpret_cast<uint64_t>(&g_floats[1]), &newDisp[2 * i + 1])) {
            std::snprintf(g_why, sizeof(g_why), "the float page at %p is out of rel32 range of the sites",
                          static_cast<void*>(page));
            standDown(base);
            return;
        }
    }
    int done = 0;
    for (; done < 4; ++done)
        if (!writeDisp(g_operand[done], newDisp[done])) break;
    if (done != 4) {
        for (int i = 0; i < done; ++i) writeDisp(g_operand[i], g_origDisp[i]);
        std::snprintf(g_why, sizeof(g_why), "a write failed at operand %d of 4; all restored", done + 1);
        standDown(base);
        return;
    }
    g_patch.store(kApplied, std::memory_order_release);
    Log::get().note("ui quality: panels: the panel formula's four divisions (init 0x%X and 0x%X, view "
                    "change 0x%X and 0x%X) now read EDVR's floats at %p -- 1080 and 1920, the game's own, "
                    "until the factor's inputs are known. Build 332841 checked: the stamp, both sites, "
                    "the prologues and the constants. The original operands are put back when EDVR "
                    "unloads.",
                    kUiPanelSiteRva[0] + kUiPanel1080Disp, kUiPanelSiteRva[0] + kUiPanel1920Disp,
                    kUiPanelSiteRva[1] + kUiPanel1080Disp, kUiPanelSiteRva[1] + kUiPanel1920Disp,
                    static_cast<void*>(page));
    // The live Supersampling (2026-10-08): two virtual slots of the render context's interface class.
    installLiveHooks(base);
    if (g_hookState == kHookInstalled)
        Log::get().note("ui quality: panels: the game's Supersampling setter (virtual slot 0x%X, function 0x%X) and "
                        "its getter of the scale the last configure used (slot 0x%X, function 0x%X) now run through "
                        "EDVR's thunks, which forward untouched and remember the render context they were called on; "
                        "the setter's also moves the panel factor to the new value before the game's own setter "
                        "runs. Until one of them is called the Supersampling comes from the .fxcfg. Both slots are "
                        "put back when EDVR unloads.",
                        kUiSsSetterSlotRva, kUiSsSetterRva, kUiSsGetterSlotRva, kUiSsGetterRva);
    else
        Log::get().note("ui quality: panels: the game's Supersampling is NOT read live -- %s. The .fxcfg's value, "
                        "read every 5 s, is what the panel factor uses.",
                        g_hookWhy);
}

// The choice between the live value and the .fxcfg's, made each frame the factor is made, and said when it changes
// (a source or a reason): the first live use, each fallback with why, a return to live. One line each, never
// per frame.
UiSsPick chooseSupersampling(float fxcfg) {
    float cur = 0.0f, lo = 0.0f, hi = 0.0f;
    const UiSsRead read = readLiveSupersampling(&cur, &lo, &hi);
    const UiSsPick pick = uiPickSupersampling(read, cur, lo, hi, fxcfg);
    g_liveCur = cur;
    g_liveLo = lo;
    g_liveHi = hi;
    g_fxcfgSs = fxcfg;
    uint32_t bits = 0;
    std::memcpy(&bits, &pick.ss, sizeof(bits));
    g_chosenSsBits.store(bits, std::memory_order_release);
    g_chosenSource.store(static_cast<uint8_t>(pick.source), std::memory_order_release);
    if (g_srcKnown && pick.source == g_srcLogged && pick.why == g_whyLogged) return pick;
    g_srcKnown = true;
    g_srcLogged = pick.source;
    g_whyLogged = pick.why;
    if (pick.source == UiSsSource::kLive) {
        Log::get().note(
            "ui quality: panels: Supersampling is %s from the game's render context (ctx+0x3564 = %.3f, inside the "
            "game's own range %.2f to %.2f; the .fxcfg says %.3f), read every frame: a change in the game's "
            "graphics menu moves the panel factor within a frame, before the game's reconfigure.",
            g_liveUsedOnce ? "read live again" : "now read live", static_cast<double>(cur), static_cast<double>(lo),
            static_cast<double>(hi), static_cast<double>(fxcfg));
        g_liveUsedOnce = true;
    } else if (pick.source == UiSsSource::kFxcfg) {
        char detail[120] = "";
        if (read == UiSsRead::kOk)
            std::snprintf(detail, sizeof(detail), " (it read %.3f in a range of %.2f to %.2f)", static_cast<double>(cur),
                          static_cast<double>(lo), static_cast<double>(hi));
        Log::get().note(
            "ui quality: panels: Supersampling %.3f is read from the .fxcfg (every 5 s), not live from the game: %s%s%s%s.",
            static_cast<double>(pick.ss), uiSsWhyName(pick.why), detail,
            pick.why == UiSsWhy::kNotCaptured && g_hookState == kHookRefused ? "; the live source is not hooked: " : "",
            pick.why == UiSsWhy::kNotCaptured && g_hookState == kHookRefused ? g_hookWhy : "");
    } else {
        Log::get().note(
            "ui quality: panels: Supersampling is unknown (the live read: %s; no .fxcfg value): no panel factor is "
            "made until one of them is there, the panels stay as the floats hold them.",
            uiSsWhyName(pick.why));
    }
    return pick;
}

// This frame's inputs, as EDVR has them; false while any is unknown.
bool gatherInputs(UiPanelInputs* in, uint32_t* askW, uint32_t* askH, float* hmd, float* up, float* down,
                  float* trueUp, float* trueDown) {
    *hmd = uiSurfacesHmdQuality();
    if (!(*hmd > 0.0f)) return false;
    if (!nativeTemporalAsked(askW, askH) && !nativeTemporalRecommended(askW, askH)) return false;
    if (!nativeTemporalVerticalTangents(up, down)) return false;
    if (!nativeTemporalTrueVerticalTangents(trueUp, trueDown)) return false;
    EdvrNativeRenderSizing s{};
    uint32_t outW = 0, outH = 0;
    if (!edvrQueryNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1, sizeof(s), &s) || s.valid != 1)
        return false;
    uiQualityRecommendedFromEyes(s.activeWidth, s.activeHeight, &outW, &outH);
    in->renderW = uiQualityInternalDim(*askW, *hmd);
    in->fovTangent = uiQualityFovTangent(*up, *down);
    in->outputW = outW;
    in->trueTangent = uiQualityFovTangent(*trueUp, *trueDown);
    in->target = g_target.load(std::memory_order_acquire);
    // The size budget's inputs: the .fxcfg's Supersampling as cached (the fallback; the boundary replaces it with the
    // live one when that is believable, and makes no factor when neither is there), and the display and the 2D
    // screen's forced width, which only widen the base.
    in->supersampling = uiSurfacesSupersampling();
    in->displayW = uiSurfacesDisplayWidth();
    in->screenW = vscreenModeAppliedWidth();
    return in->renderW && in->outputW && in->fovTangent > 0.0f && in->trueTangent > 0.0f;
}

// The flat profile's frame boundary (defined after uiPanelScaleFrameBoundary, which calls it with the key on).
void flatFrameBoundary(float target);

}  // namespace

void uiPanelScaleSetTarget(float target) {
    g_target.store(target, std::memory_order_release);
    uint8_t untried = kUntried;
    // Once: the first configure with the key on checks and swaps (apply sets
    // kApplied or kStoodDown); a concurrent configure sees kApplying and
    // leaves it alone.
    if (target > 0.0f && g_patch.compare_exchange_strong(untried, kApplying, std::memory_order_acq_rel))
        apply();
}

bool uiPanelScaleLive() { return g_live.load(std::memory_order_acquire); }

double uiPanelScaleFactor() { return doubleFromBitsOrOne(g_factorBits.load(std::memory_order_acquire)); }

double uiPanelScaleLineFactor() { return doubleFromBitsOrOne(g_lineBits.load(std::memory_order_acquire)); }

double uiPanelScaleSupersampling() { return doubleFromBitsOrOne(g_ssBits.load(std::memory_order_acquire)); }

void uiPanelScaleFrameBoundary() {
    ++g_frame;
    if (g_patch.load(std::memory_order_acquire) != kApplied) return;
    const float target = g_target.load(std::memory_order_acquire);
    if (!(target > 0.0f)) {
        // Key off: the game's own values.
        if (g_live.load(std::memory_order_acquire) || g_written != 1.0) {
            writeFloats(1.0);
            g_written = 1.0;
            g_lastSsEff = 1.0;
            g_live.store(false, std::memory_order_release);
            Log::get().note("ui quality: panels: the key is off -- the floats read 1080 and 1920 again, "
                            "the game's own sizes from the next panel init or view change.");
        }
        g_pending = -1.0;
        g_settle = 0;
        g_flatSettle.forget();
        return;
    }
    // The flat profile has its own inputs (the render and display sizes; no HMD, frustum or .fxcfg).
    if (runtimeFlatProfile()) {
        flatFrameBoundary(target);
        return;
    }
    UiPanelInputs in;
    uint32_t askW = 0, askH = 0;
    float hmd = 0.0f, up = 0.0f, down = 0.0f, trueUp = 0.0f, trueDown = 0.0f;
    UiPanelPlan plan;
    if (!gatherInputs(&in, &askW, &askH, &hmd, &up, &down, &trueUp, &trueDown)) {
        g_settle = 0;
        return;
    }
    // The Supersampling, every frame: the game's own live value when it can be read and is believable, the
    // .fxcfg's (in.supersampling as gathered: read every 5 s) when not. Neither: no factor.
    const UiSsPick pick = chooseSupersampling(in.supersampling);
    in.supersampling = pick.ss;
    if (!(pick.ss > 0.0f) || !uiPanelPlanFor(in, &plan)) {
        g_settle = 0;
        return;
    }
    // What the setter thunk makes a factor from when the game's menu moves the Supersampling.
    {
        uint64_t fb = 0, bb = 0;
        std::memcpy(&fb, &plan.formula, sizeof(fb));
        std::memcpy(&bb, &plan.base, sizeof(bb));
        g_pubFormulaBits.store(fb, std::memory_order_release);
        g_pubBaseBits.store(bb, std::memory_order_release);
        g_pubReady.store(true, std::memory_order_release);
    }
    const double f = plan.f;
    const UiPanelClamp clamp = plan.clamp;
    // A move of the Supersampling is written NOW (the game's reconfigure is at most a frame behind its setter);
    // anything else only once the inputs have settled on a new value: never per frame, so the two runs of one
    // view change read one factor.
    const bool ssMoved = g_live.load(std::memory_order_acquire) && uiPanelSsMoved(g_lastSsEff, plan.ss);
    if (ssMoved) {
        g_pending = f;
        g_settle = kSettleFrames;
    } else {
        if (std::fabs(f - g_pending) > 1e-6) {
            g_pending = f;
            g_settle = 0;
            return;
        }
        if (++g_settle < kSettleFrames) return;
        // Against what the floats hold (the setter thunk may have moved them), not against this thread's last write.
        if (g_live.load(std::memory_order_acquire) && std::fabs(f / uiPanelScaleFactor() - 1.0) <= 0.001) return;
    }
    if (!writeFloats(f, plan.lineF, plan.ss)) return;
    g_written = f;
    g_lastSsEff = plan.ss;
    g_lastInputs = in;
    g_lastPlan = plan;
    g_pick = pick;
    ++g_writes;
    if (!g_live.exchange(true, std::memory_order_acq_rel)) g_liveSince = g_frame;
    float d1080 = 0.0f, d1920 = 0.0f;
    uiPanelDivisors(f, &d1080, &d1920);
    const double k = uiSizingK(in.fovTangent), kOut = uiSizingK(in.trueTangent);
    char ssText[48] = "";
    if (plan.ss > 1.0) std::snprintf(ssText, sizeof(ssText), " x Supersampling %.2f", plan.ss);
    Log::get().note(
        "ui quality: panels: the engine now sizes every render-to-texture panel x%.4f (the four "
        "operands at 0x%X/0x%X and 0x%X/0x%X read 1080 -> %.2f, 1920 -> %.2f): f %.4f = (W_ui %u x k "
        "%.4f) / (W_out %u x k_out %.4f) / target %.0f%%%s%s -- HMD Quality %.2f, Elite's Supersampling "
        "%.2f, the game told %ux%u, its vertical FOV %.1f degrees, the headset's %.1f. From the next "
        "panel init or view change.",
        1.0 / f, kUiPanelSiteRva[0] + kUiPanel1080Disp, kUiPanelSiteRva[0] + kUiPanel1920Disp,
        kUiPanelSiteRva[1] + kUiPanel1080Disp, kUiPanelSiteRva[1] + kUiPanel1920Disp,
        static_cast<double>(d1080), static_cast<double>(d1920), f, in.renderW, k, in.outputW, kOut,
        static_cast<double>(in.target) * 100.0, ssText,
        clamp == UiPanelClamp::kCap      ? " (capped: no panel above four times its game size)"
        : clamp == UiPanelClamp::kFloor  ? " (at 1: the game's own panels are at or above the target already)"
        : clamp == UiPanelClamp::kBudget ? " (raised by the size budget, next line)"
                                         : "",
        static_cast<double>(hmd), static_cast<double>(in.supersampling), askW, askH,
        (std::atan(up) + std::atan(down)) * 57.29577951308232,
        (std::atan(trueUp) + std::atan(trueDown)) * 57.29577951308232);
    // The line that says the Supersampling term or the size budget changed the factor (2026-10-07): what was
    // seen, the factor before and after, and the widest panel the formula could then ask for. Not written when
    // neither acts, so its absence at Supersampling 1 is the same as before.
    if (plan.ssActs || plan.budgetActs) {
        char budgetText[160] = "";
        if (plan.budgetActs)
            std::snprintf(budgetText, sizeof(budgetText),
                          ", then f %.4f (x%.4f) from the size budget (%.0f px at f %.4f would be over it)",
                          plan.f, 1.0 / plan.f, plan.ss * plan.base / plan.lineF, plan.lineF);
        Log::get().note(
            "ui quality: panels: factor adjusted -- Elite's Supersampling is %.2f (%s), the game's own panels "
            "already %.2fx wider than HMD Quality makes them: f %.4f "
            "(x%.4f) without that term, f %.4f (x%.4f) with it%s. The widest panel the formula could ask for "
            "is %.0f px at the written f %.4f (%.0f px at f %.4f), from a %.0f px base (%s), against "
            "D3D11's %.0f px limit and EDVR's %.0f px budget.",
            plan.ss,
            pick.source == UiSsSource::kLive ? "live, the game's render context" : "the .fxcfg's SSAAMultiplier",
            plan.ss, plan.beforeF, 1.0 / plan.beforeF, plan.lineF, 1.0 / plan.lineF, budgetText,
            plan.largest, plan.f, plan.largestBefore, plan.beforeF, plan.base, uiPanelBaseName(plan.baseFrom),
            kUiPanelTextureLimit, kUiPanelBudget);
    }
}

void uiPanelScaleSetFlatTemporal(bool on) { g_flatTemporal.store(on, std::memory_order_release); }

namespace {

const char* flatClampText(UiPanelClamp clamp) {
    return clamp == UiPanelClamp::kCap     ? " (capped: no panel above four times its game size)"
           : clamp == UiPanelClamp::kFloor ? " (at 1: the render is at or above the display times the target, so the "
                                             "game's own panels are that size already)"
           : clamp == UiPanelClamp::kBudget ? " (raised by the size budget)"
                                            : "";
}

// The render thread, once a frame with the key on, in the flat profile. The same settle and the same floats as VR's;
// the inputs are R and D (ui_sizing_math.h's uiFlatPanelPlanFor).
void flatFrameBoundary(float target) {
    if (!g_flatTemporal.load(std::memory_order_acquire)) {
        // Anti-aliasing off: the game's own sizes (ui_panel_scale.h says why).
        g_pubReady.store(false, std::memory_order_release);
        g_flatAaOff = true;
        if (g_live.load(std::memory_order_acquire) || g_written != 1.0) {
            writeFloats(1.0);
            g_written = 1.0;
            g_live.store(false, std::memory_order_release);
            Log::get().note("ui quality: panels (flat): anti-aliasing is off -- the floats read 1080 and 1920 again, "
                            "the game's own panel sizes from the next panel init or view change.");
        }
        g_pending = -1.0;
        g_settle = 0;
        g_flatSettle.forget();
        return;
    }
    g_flatAaOff = false;
    UiFlatPanelInputs in;
    if (!flatRuntimeSceneSizes(&in.renderW, &in.renderH, &in.outputW, &in.outputH)) in = UiFlatPanelInputs{};
    in.target = target;
    UiPanelPlan plan;
    g_flatRefuse = uiFlatPanelPlanFor(in, &plan);
    if (g_flatRefuse != UiFlatPanelRefuse::kNone) {
        // A frame with no scene (a loading screen) or one unlike the screen (a 512x512 preview): the floats hold.
        ++g_flatRefusedFrames;
        g_flatSettle.unsettle();
        return;
    }
    // Only a value the inputs have held for kSettleFrames is written (the two runs of one view change read one factor) -- and a factor the game's Supersampling setter wrote
    // is held, not written over by the old size's plan, until the scene's size has followed it; the new size's plan is then accepted at once (ui_sizing_math.h's UiFlatPanelSettle).
    const uint32_t setterEpoch = g_flatSetterEpoch.load(std::memory_order_acquire);
    const uint64_t setterAt = g_flatSetterAt.load(std::memory_order_acquire);
    const UiFlatPanelSettle::Step step =
        g_flatSettle.step(plan.f, g_live.load(std::memory_order_acquire), uiPanelScaleFactor(), setterEpoch, static_cast<uint32_t>(setterAt >> 32),
                          static_cast<uint32_t>(setterAt & 0xFFFFFFFFu), in.renderW, in.renderH);
    if (step.arrived) {
        ++g_flatArrivals;
        Log::get().note("ui quality: panels (flat): the scene is now %ux%u, %u boundary(ies) after the game's Supersampling setter moved the factor to x%.4f; "
                        "the plan for it is f %.4f, accepted without settling.",
                        in.renderW, in.renderH, step.waited, 1.0 / uiPanelScaleFactor(), plan.f);
    }
    if (step.timedOut) {
        ++g_flatTimeouts;
        Log::get().note("ui quality: panels (flat): the scene stayed %ux%u for %u boundaries after the game's Supersampling setter moved the factor to x%.4f; "
                        "the plan for that size (f %.4f) stands.",
                        in.renderW, in.renderH, step.waited, 1.0 / uiPanelScaleFactor(), plan.f);
    }
    if (step.act == UiFlatPanelSettle::Act::kHold) {
        ++g_flatHeldFrames;
        return;
    }
    if (step.act == UiFlatPanelSettle::Act::kWait) return;
    // Settled: what the setter thunk scales when the game's menu moves the Supersampling before R follows. Published only
    // beside a believable live value (the scale needs the value the plan was made at); without one the thunk does nothing.
    {
        float cur = 0.0f, lo = 0.0f, hi = 0.0f;
        if (readLiveSupersampling(&cur, &lo, &hi) == UiSsRead::kOk && uiLiveSupersamplingValid(cur, lo, hi, nullptr)) {
            uint64_t fb = 0, bb = 0;
            uint32_t sb = 0;
            std::memcpy(&fb, &plan.formula, sizeof(fb));
            std::memcpy(&bb, &plan.base, sizeof(bb));
            std::memcpy(&sb, &cur, sizeof(sb));
            g_pubReady.store(false, std::memory_order_release);
            g_pubFormulaBits.store(fb, std::memory_order_release);
            g_pubBaseBits.store(bb, std::memory_order_release);
            g_pubFlatSsBits.store(sb, std::memory_order_release);
            g_pubFlatDims.store((static_cast<uint64_t>(in.renderW) << 32) | in.renderH, std::memory_order_release);
            g_pubFlat.store(true, std::memory_order_release);
            g_pubReady.store(true, std::memory_order_release);
        } else {
            g_pubReady.store(false, std::memory_order_release);
        }
    }
    if (step.act == UiFlatPanelSettle::Act::kKeep) return;
    if (!writeFloats(plan.f, plan.lineF, 1.0)) return;
    g_written = plan.f;
    g_lastSsEff = 1.0;
    g_flatLastIn = in;
    g_lastPlan = plan;
    ++g_writes;
    if (!g_live.exchange(true, std::memory_order_acq_rel)) g_liveSince = g_frame;
    float d1080 = 0.0f, d1920 = 0.0f;
    uiPanelDivisors(plan.f, &d1080, &d1920);
    const bool heightAxis = uiFlatPanelHeightAxis(in.renderW, in.renderH);
    Log::get().note(
        "ui quality: panels (flat): the engine now sizes every render-to-texture panel x%.4f (the four operands at "
        "0x%X/0x%X and 0x%X/0x%X read 1080 -> %.2f, 1920 -> %.2f): f %.4f = render %ux%u / display %ux%u on the %s "
        "axis / target %.0f%%%s. A panel the game made WxH at the game's own divisors is now made about %.2fW x %.2fH; "
        "the widest the formula can ask for is %.0f px (D3D11's limit %.0f, EDVR's budget %.0f). From the next panel "
        "init or view change.",
        1.0 / plan.f, kUiPanelSiteRva[0] + kUiPanel1080Disp, kUiPanelSiteRva[0] + kUiPanel1920Disp,
        kUiPanelSiteRva[1] + kUiPanel1080Disp, kUiPanelSiteRva[1] + kUiPanel1920Disp, static_cast<double>(d1080),
        static_cast<double>(d1920), plan.f, in.renderW, in.renderH, in.outputW, in.outputH,
        heightAxis ? "height (1080)" : "width (1920)", static_cast<double>(target) * 100.0, flatClampText(plan.clamp),
        1.0 / plan.f, 1.0 / plan.f, plan.largest, kUiPanelTextureLimit, kUiPanelBudget);
}

// The flat profile's 30 s line.
void flatLog(const char* net) {
    const uint64_t refused = g_flatRefusedFrames;
    g_flatRefusedFrames = 0;
    // The game's Supersampling setter, through which a menu change moves the factor before R follows.
    char live[360];
    if (g_hookState != kHookInstalled)
        std::snprintf(live, sizeof(live), "not hooked (%s): a change follows R once it holds",
                      g_hookState == kHookRefused ? g_hookWhy : "not installed");
    else
        std::snprintf(live, sizeof(live),
                      "hooked, called %u time(s), %u factor move(s) made in it, the plan %s published to it; a moved factor held for %llu boundary(ies) "
                      "until the scene's size followed (%llu time(s)) or gave up waiting (%llu time(s))",
                      g_setterCalls.load(std::memory_order_relaxed), g_preWrites.load(std::memory_order_relaxed),
                      g_pubReady.load(std::memory_order_acquire) && g_pubFlat.load(std::memory_order_acquire) ? "is" : "is not",
                      static_cast<unsigned long long>(g_flatHeldFrames), static_cast<unsigned long long>(g_flatArrivals),
                      static_cast<unsigned long long>(g_flatTimeouts));
    if (!g_live.load(std::memory_order_acquire)) {
        const char* why = g_target.load(std::memory_order_acquire) <= 0.0f ? "the key is off: the game's own sizes"
                          : g_flatAaOff ? "anti-aliasing is off: the game's own sizes until it is on"
                          : g_flatRefuse != UiFlatPanelRefuse::kNone
                              ? uiFlatPanelRefuseName(g_flatRefuse)
                              : "waiting for the render size to hold for ten frames";
        Log::get().note("ui quality: panels (flat): patched, not sizing -- %s; %llu frame(s) this window refused the "
                        "inputs. The game's Supersampling setter: %s. %s.",
                        why, static_cast<unsigned long long>(refused), live, net);
        return;
    }
    const UiFlatPanelInputs& in = g_flatLastIn;
    Log::get().note("ui quality: panels (flat): the engine sizes panels x%.4f since frame %u (%u writes; f %.4f = "
                    "render %ux%u / display %ux%u / target %.0f%%; the widest panel the formula could ask for is %.0f "
                    "px, D3D11's limit %.0f); %llu frame(s) this window refused the inputs (now: %s). The game's Supersampling setter: "
                    "%s. %s.",
                    1.0 / uiPanelScaleFactor(), g_liveSince, g_writes, uiPanelScaleFactor(), in.renderW, in.renderH,
                    in.outputW, in.outputH, static_cast<double>(in.target) * 100.0, g_lastPlan.largest,
                    kUiPanelTextureLimit, static_cast<unsigned long long>(refused), uiFlatPanelRefuseName(g_flatRefuse),
                    live, net);
}

}  // namespace

double uiPanelScaleChosenSupersampling(const char** source) {
    if (source) {
        const uint8_t s = g_chosenSource.load(std::memory_order_acquire);
        *source = uiSsSourceName(static_cast<UiSsSource>(s));
    }
    const uint32_t bits = g_chosenSsBits.load(std::memory_order_acquire);
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return static_cast<double>(v);
}

void uiPanelScaleShutdown() {
    if (g_patch.load(std::memory_order_acquire) != kApplied) return;
    for (int i = 0; i < 4; ++i)
        if (g_operand[i]) writeDisp(g_operand[i], g_origDisp[i]);
    if (g_hookState == kHookInstalled) {
        for (int i = 0; i < 2; ++i)
            if (g_slotAt[i]) writeSlot(g_slotAt[i], g_slotOrig[i]);
        g_hookState = kHookNone;
    }
    g_patch.store(kStoodDown, std::memory_order_release);
    g_live.store(false, std::memory_order_release);
    Log::get().note("ui quality: panels: the panel formula's four original operands, and the Supersampling "
                    "setter's and getter's virtual slots, written back.");
}

namespace {

// Where the Supersampling comes from, and how often the game has talked to the thunks, for the 30 s line.
void liveStatusText(char* out, size_t n) {
    if (g_hookState != kHookInstalled) {
        std::snprintf(out, n, "not live (%s): the .fxcfg's %.3f, read every 5 s", g_hookState == kHookRefused ? g_hookWhy : "not hooked",
                      static_cast<double>(g_fxcfgSs));
    } else if (g_pick.source == UiSsSource::kLive) {
        std::snprintf(out, n, "live %.3f from ctx+0x3564 (the game's range %.2f to %.2f; the .fxcfg says %.3f), setter called %u time(s), "
                      "%u factor move(s) made before it, getter %s",
                      static_cast<double>(g_liveCur), static_cast<double>(g_liveLo), static_cast<double>(g_liveHi),
                      static_cast<double>(g_fxcfgSs), g_setterCalls.load(std::memory_order_relaxed),
                      g_preWrites.load(std::memory_order_relaxed), g_getterSeen.load(std::memory_order_relaxed) ? "seen" : "not seen");
    } else if (g_pick.source == UiSsSource::kFxcfg) {
        std::snprintf(out, n, "the .fxcfg's %.3f (%s), setter called %u time(s), getter %s", static_cast<double>(g_pick.ss),
                      uiSsWhyName(g_pick.why), g_setterCalls.load(std::memory_order_relaxed),
                      g_getterSeen.load(std::memory_order_relaxed) ? "seen" : "not seen");
    } else {
        std::snprintf(out, n, "unknown");
    }
}

// The net's count, on every state of the line: the panels' create that D3D11 would have refused, shrunk.
void netText(char* out, size_t n) {
    uint32_t fired = 0, distinct = 0;
    uiSurfacesPanelNetCounts(&fired, &distinct);
    if (!fired)
        std::snprintf(out, n, "panel net: 0 create(s) over %u a side shrunk", kUiPanelNetLimit);
    else
        std::snprintf(out, n, "panel net: %u create(s) over %u a side shrunk, %u distinct size(s) (each named on its own line)",
                      fired, kUiPanelNetLimit, distinct);
}

}  // namespace

void uiPanelScaleLog() {
    const uint8_t state = g_patch.load(std::memory_order_acquire);
    if (state == kUntried || state == kApplying) return;
    char net[160];
    netText(net, sizeof(net));
    if (state == kStoodDown) {
        Log::get().note("ui quality: panels: standing down -- %s. %s.", g_why[0] ? g_why : "EDVR is unloading", net);
        return;
    }
    char live[400];
    liveStatusText(live, sizeof(live));
    if (runtimeFlatProfile()) {
        flatLog(net);
        return;
    }
    if (!g_live.load(std::memory_order_acquire)) {
        Log::get().note("ui quality: panels: patched, not sizing -- %s. Supersampling: %s. %s.",
                        g_target.load(std::memory_order_acquire) > 0.0f
                            ? "the factor's inputs are not all known yet (HMD Quality, Elite's "
                              "Supersampling, the frame's frustum, the headset's, the runtime's size); "
                              "until they are the panels stay at the game's own size"
                            : "the key is off: the game's own sizes",
                        live, net);
        return;
    }
    const UiPanelInputs& in = g_lastInputs;
    const UiPanelPlan& plan = g_lastPlan;
    Log::get().note("ui quality: panels: the engine sizes panels x%.4f since frame %u (%u writes; f %.4f "
                    "= (W_ui %u x k %.4f) / (W_out %u x k_out %.4f) / target %.0f%% x Supersampling %.2f; "
                    "the widest panel the formula could ask for is %.0f px, D3D11's limit %.0f). "
                    "Supersampling: %s. %s.",
                    1.0 / uiPanelScaleFactor(), g_liveSince, g_writes, uiPanelScaleFactor(), in.renderW,
                    uiSizingK(in.fovTangent), in.outputW, uiSizingK(in.trueTangent),
                    static_cast<double>(in.target) * 100.0, plan.ss, plan.largest, kUiPanelTextureLimit, live, net);
}

}  // namespace edvr

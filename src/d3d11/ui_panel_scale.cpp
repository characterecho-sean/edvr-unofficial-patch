// fix.ui_quality -- the engine-side panel sizing. ui_panel_scale.h says what
// and why; ui_sizing_math.h holds the arithmetic and build 332841's bytes,
// shared with tools/ui_quality_test.
#include "ui_panel_scale.h"

#include "ui_quality_math.h"  // uiQualityFovTangent, uiQualityInternalDim, uiQualityRecommendedFromEyes
#include "ui_sizing_math.h"
#include "ui_surfaces.h"      // native temporal's lock-free accessors, uiSurfacesHmdQuality

#include "../common/log.h"
#include "../common/native_render_settings.h"  // edvrQueryNativeRenderSizing: W_out

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {

namespace {

enum PatchState : uint8_t { kUntried = 0, kApplied = 1, kStoodDown = 2, kApplying = 3 };
std::atomic<uint8_t> g_patch{kUntried};
std::atomic<float> g_target{0.0f};
std::atomic<bool> g_live{false};
std::atomic<uint64_t> g_factorBits{0};  // the written factor's double bits (0: 1.0)

volatile float* g_floats = nullptr;  // [0] 1080 x f, [1] 1920 x f, in a page within rel32
uint8_t* g_operand[4] = {};          // site 0's 1080 and 1920 disp32s, then site 1's
int32_t g_origDisp[4] = {};
char g_why[240] = "";

// The render thread's own (uiPanelScaleFrameBoundary).
double g_written = 1.0;
double g_pending = -1.0;
uint32_t g_settle = 0;
constexpr uint32_t kSettleFrames = 10;  // the inputs steady this long before a write
uint32_t g_frame = 0, g_liveSince = 0, g_writes = 0;
UiPanelInputs g_lastInputs;

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

// The two floats: data stores, aligned, each one atomic for the game's reads.
bool writeFloats(double f) {
    if (!g_floats) return false;
    float d1080 = 1080.0f, d1920 = 1920.0f;
    uiPanelDivisors(f, &d1080, &d1920);
    DWORD prot = 0;
    if (!VirtualProtect(const_cast<float*>(g_floats), 8, PAGE_READWRITE, &prot)) return false;
    g_floats[0] = d1080;
    g_floats[1] = d1920;
    DWORD ignored = 0;
    VirtualProtect(const_cast<float*>(g_floats), 8, PAGE_READONLY, &ignored);
    uint64_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    g_factorBits.store(f == 1.0 ? 0 : bits, std::memory_order_release);
    return true;
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
    return in->renderW && in->outputW && in->fovTangent > 0.0f && in->trueTangent > 0.0f;
}

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

double uiPanelScaleFactor() {
    const uint64_t bits = g_factorBits.load(std::memory_order_acquire);
    if (!bits) return 1.0;
    double f = 1.0;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

void uiPanelScaleFrameBoundary() {
    ++g_frame;
    if (g_patch.load(std::memory_order_acquire) != kApplied) return;
    const float target = g_target.load(std::memory_order_acquire);
    if (!(target > 0.0f)) {
        // Key off: the game's own values.
        if (g_live.load(std::memory_order_acquire) || g_written != 1.0) {
            writeFloats(1.0);
            g_written = 1.0;
            g_live.store(false, std::memory_order_release);
            Log::get().note("ui quality: panels: the key is off -- the floats read 1080 and 1920 again, "
                            "the game's own sizes from the next panel init or view change.");
        }
        g_pending = -1.0;
        g_settle = 0;
        return;
    }
    UiPanelInputs in;
    uint32_t askW = 0, askH = 0;
    float hmd = 0.0f, up = 0.0f, down = 0.0f, trueUp = 0.0f, trueDown = 0.0f;
    double f = 0.0;
    UiPanelClamp clamp = UiPanelClamp::kNone;
    if (!gatherInputs(&in, &askW, &askH, &hmd, &up, &down, &trueUp, &trueDown) ||
        !uiPanelFactor(in, &f, &clamp)) {
        g_settle = 0;
        return;
    }
    // Written only once the inputs have settled on a new value: never per
    // frame, so the two runs of one view change read one factor.
    if (std::fabs(f - g_pending) > 1e-6) {
        g_pending = f;
        g_settle = 0;
        return;
    }
    if (++g_settle < kSettleFrames) return;
    if (g_live.load(std::memory_order_acquire) && std::fabs(f / g_written - 1.0) <= 0.001) return;
    if (!writeFloats(f)) return;
    g_written = f;
    g_lastInputs = in;
    ++g_writes;
    if (!g_live.exchange(true, std::memory_order_acq_rel)) g_liveSince = g_frame;
    float d1080 = 0.0f, d1920 = 0.0f;
    uiPanelDivisors(f, &d1080, &d1920);
    const double k = uiSizingK(in.fovTangent), kOut = uiSizingK(in.trueTangent);
    Log::get().note(
        "ui quality: panels: the engine now sizes every render-to-texture panel x%.4f (the four "
        "operands at 0x%X/0x%X and 0x%X/0x%X read 1080 -> %.2f, 1920 -> %.2f): f %.4f = (W_ui %u x k "
        "%.4f) / (W_out %u x k_out %.4f) / target %.0f%%%s -- HMD Quality %.2f, the game told %ux%u, its "
        "vertical FOV %.1f degrees, the headset's %.1f. From the next panel init or view change.",
        1.0 / f, kUiPanelSiteRva[0] + kUiPanel1080Disp, kUiPanelSiteRva[0] + kUiPanel1920Disp,
        kUiPanelSiteRva[1] + kUiPanel1080Disp, kUiPanelSiteRva[1] + kUiPanel1920Disp,
        static_cast<double>(d1080), static_cast<double>(d1920), f, in.renderW, k, in.outputW, kOut,
        static_cast<double>(in.target) * 100.0,
        clamp == UiPanelClamp::kCap     ? " (capped: no panel above four times its game size)"
        : clamp == UiPanelClamp::kFloor ? " (at 1: HMD Quality is at or above the target)"
                                        : "",
        static_cast<double>(hmd), askW, askH,
        (std::atan(up) + std::atan(down)) * 57.29577951308232,
        (std::atan(trueUp) + std::atan(trueDown)) * 57.29577951308232);
}

void uiPanelScaleShutdown() {
    if (g_patch.load(std::memory_order_acquire) != kApplied) return;
    for (int i = 0; i < 4; ++i)
        if (g_operand[i]) writeDisp(g_operand[i], g_origDisp[i]);
    g_patch.store(kStoodDown, std::memory_order_release);
    g_live.store(false, std::memory_order_release);
    Log::get().note("ui quality: panels: the panel formula's four original operands written back.");
}

void uiPanelScaleLog() {
    const uint8_t state = g_patch.load(std::memory_order_acquire);
    if (state == kUntried || state == kApplying) return;
    if (state == kStoodDown) {
        Log::get().note("ui quality: panels: standing down -- %s.", g_why[0] ? g_why : "EDVR is unloading");
        return;
    }
    if (!g_live.load(std::memory_order_acquire)) {
        Log::get().note("ui quality: panels: patched, not sizing -- %s.",
                        g_target.load(std::memory_order_acquire) > 0.0f
                            ? "the factor's inputs are not all known yet (HMD Quality, the frame's "
                              "frustum, the headset's, the runtime's size)"
                            : "the key is off: the game's own sizes");
        return;
    }
    const UiPanelInputs& in = g_lastInputs;
    Log::get().note("ui quality: panels: the engine sizes panels x%.4f since frame %u (%u writes; f %.4f "
                    "= (W_ui %u x k %.4f) / (W_out %u x k_out %.4f) / target %.0f%%).",
                    1.0 / g_written, g_liveSince, g_writes, g_written, in.renderW,
                    uiSizingK(in.fovTangent), in.outputW, uiSizingK(in.trueTangent),
                    static_cast<double>(in.target) * 100.0);
}

}  // namespace edvr

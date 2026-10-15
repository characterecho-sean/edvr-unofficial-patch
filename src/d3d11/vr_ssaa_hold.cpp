// The VR Supersampling hold (vr_ssaa_hold.h says what and why). The decision is vr_ssaa_hold_math.h's; this file is the game's side.
#include "vr_ssaa_hold.h"

#include "vr_ssaa_gate.h"       // vrSsaaGateFrame: the frame number the instrument line carries
#include "vr_ssaa_hold_math.h"
#include "ui_panel_scale.h"  // uiPanelScaleEarlyHooks: the setter and getter hooks, installed at DLL load
#include "ui_sizing_math.h"  // kUiPanelStamp, kUiPanelImageSize: build 332841

#include "../common/elite_graphics_folder.h"       // the Options\Graphics folder under %LOCALAPPDATA%
#include "../common/log.h"
#include "../common/native_render_settings.h"      // edvrQueryNativeRenderSizing: the runtime's live VR session (eye sizes valid)
#include "../common/runtime_profile.h"             // runtimeFlatProfile, runtimeFeaturesAllowed

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// d3d11_proxy.cpp: the graphics DLL's one-time setup (config, log, module pin), the same work the first device creation does.
// The loader can run before that, so the hold's loader thunk asks for it.
void edvrGraphicsEnsureInitialised();

namespace edvr {
namespace {

// Build 332841's addresses (RVAs). The wrapper calls the fxcfg loader with the loader's object in r8; the slot holds the wrapper.
constexpr uint32_t kLoaderSlotRva = 0x52E8368;
constexpr uint32_t kWrapperRva = 0x2862780;
constexpr uint32_t kLoaderRva = 0x2855A50;
constexpr uint32_t kSsFieldOff = 0x13C;     // the loader object's SS field (a stack temporary the game copies from)
constexpr uintptr_t kCtxSsOff = 0x3564;     // the render context's Supersampling field, the one every sizing consumer reads
constexpr uint32_t kNoneBits = 0xFFFFFFFFu;  // "no value yet" in the float-bits atomics below
// The wrapper's bytes: sub rsp,0x28; mov rcx,r8; call rel32 (the loader); mov al,1; add rsp,0x28; ret.
constexpr uint8_t kWrapperBytes[19] = {0x48, 0x83, 0xEC, 0x28, 0x49, 0x8B, 0xC8, 0xE8, 0xC4, 0x32, 0xFF, 0xFF,
                                       0xB0, 0x01, 0x48, 0x83, 0xC4, 0x28, 0xC3};

using WrapperFn = bool (*)(void*, void*, void*);

std::atomic<WrapperFn> g_wrapperOrig{nullptr};
std::atomic<uint8_t> g_slotState{0};      // 0 not tried, 1 installed, 2 refused
char g_slotWhy[240] = "not tried";
bool g_slotWritten = false;
bool g_setterIn = false;                  // the setter and getter hooks (ui_panel_scale.cpp) are in
char g_setterWhy[200] = "";
DWORD g_slotErr = 0;
std::atomic<bool> g_holdAllowed{false};   // the VR profile: set when the loader runs (config is up by then)
std::atomic<bool> g_arriving{false};      // VR went live under a flat profile: a 3D-on apply ahead of the profile switch (armed on the rise)
std::atomic<uint32_t> g_copyCalls{0};     // the settings copy relay's calls (the instrument)
std::atomic<uint32_t> g_copyLines{0};     // the lines it logged (capped)
std::atomic<uint32_t> g_copyLastKey{0xFFFFFFFFu};  // its last logged (allowed, live, arriving, wrote) and copied bits: a change logs
std::atomic<uint32_t> g_copyLastBits{kNoneBits};
std::atomic<bool> g_modeKnown{false};     // Settings.xml's mode, read at the loader only (startup)
std::atomic<int> g_mode{-1};
std::atomic<bool> g_held{false};          // the field is held at 1.0 right now; the panel factor reads 1.0 then
std::atomic<bool> g_vrLive{false};        // the runtime's live VR session: the hold's decision (edvrQueryNativeRenderSizing valid)
std::atomic<uint32_t> g_requestedBits{kNoneBits};  // the game's value while held (the notice)
std::atomic<uint32_t> g_gameBits{kNoneBits};       // the game's own last value (set or copied): restored on release
std::atomic<uintptr_t> g_ctx{0};          // the render context (from the setter, the getter or the copy)
std::atomic<uintptr_t> g_loaderObj{0};    // the loader's object (a stack temporary; logged only)
std::atomic<bool> g_startupSaid{false};
std::atomic<uint32_t> g_loggedBits{kNoneBits};   // the requested value the last menu-hold line said
std::atomic<uint32_t> g_noticedBits{kNoneBits};  // the value the last toast was queued for
std::atomic<bool> g_copyHeld{false};             // the copy's hold is on (its last line)
std::atomic<uint32_t> g_copyBits{kNoneBits};     // the copied value its last line said
std::atomic<uint32_t> g_loaderRuns{0};
std::atomic<uint32_t> g_loaderLastReq{0};
std::atomic<int> g_loaderLastMode{-2};

uint32_t bitsOf(float v) {
    uint32_t b = 0;
    std::memcpy(&b, &v, sizeof(b));
    return b;
}

float floatOf(uint32_t b) {
    float v = 0.0f;
    std::memcpy(&v, &b, sizeof(v));
    return v;
}

// No destructors in the guarded readers: __try is illegal where unwinding is needed.
bool readGame(const uint8_t* at, void* out, size_t n) {
    __try {
        std::memcpy(out, at, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool writeFloatGuarded(uintptr_t at, float v) {
    __try {
        *reinterpret_cast<volatile float*>(at) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readFloatGuarded(uintptr_t at, float* out) { return readGame(reinterpret_cast<const uint8_t*>(at), out, sizeof(float)); }

// The game's PE header: build 332841's stamp and image size (the gate ui_panel_scale.cpp applies).
bool buildChecked(const uint8_t* base, char* why, size_t whyLen) {
    uint32_t stamp = 0, size = 0;
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            std::snprintf(why, whyLen, "the game's PE headers are not readable");
            return false;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            std::snprintf(why, whyLen, "the game's PE signature is not readable");
            return false;
        }
        stamp = nt->FileHeader.TimeDateStamp;
        size = nt->OptionalHeader.SizeOfImage;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::snprintf(why, whyLen, "the game's PE headers faulted");
        return false;
    }
    if (stamp != kUiPanelStamp || size != kUiPanelImageSize) {
        std::snprintf(why, whyLen, "game build not checked (PE stamp %u, image %u bytes; build 332841's are %u, %u)", stamp,
                      size, kUiPanelStamp, kUiPanelImageSize);
        return false;
    }
    return true;
}

// Writes one 8-byte slot in the game's .rdata (ui_panel_scale.cpp's writeSlot). False, with GetLastError in g_slotErr, when the page
// cannot be made writable: nothing was written then, and the caller keeps its state as it was.
bool writeSlot(uint8_t* slot, uint64_t value) {
    DWORD prot = 0;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &prot)) {
        g_slotErr = GetLastError();
        return false;
    }
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(slot), static_cast<LONG64>(value));
    DWORD ignored = 0;
    VirtualProtect(slot, 8, prot, &ignored);
    return true;
}

// Settings.xml's text, read whole (small; a size over 1 MB is refused).
bool readSettingsText(std::string* out) {
    out->clear();
    wchar_t appdata[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    const std::wstring path = eliteGraphicsFolderUnder(appdata) + L"\\Settings.xml";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const DWORD size = GetFileSize(f, nullptr);
    bool ok = size != INVALID_FILE_SIZE && size <= (1u << 20);
    if (ok) {
        std::string buf(size, '\0');
        DWORD got = 0;
        ok = size == 0 || ReadFile(f, &buf[0], size, &got, nullptr) != 0;
        if (ok) {
            buf.resize(got);
            *out = buf;
        }
    }
    CloseHandle(f);
    return ok;
}

// The Settings.xml mode (startup only: the loader's hold). Not used for the live decision.
bool readMode(int* mode, char* raw, size_t rawLen) {
    std::string xml;
    if (!readSettingsText(&xml)) {
        std::snprintf(raw, rawLen, "unreadable");
        return false;
    }
    if (ssaahold::parseStereoscopicMode(xml.c_str(), mode)) {
        std::snprintf(raw, rawLen, "%d", *mode);
        return true;
    }
    std::snprintf(raw, rawLen, "%s", xml.find("<StereoscopicMode>") == std::string::npos ? "absent" : "unparsed");
    return false;
}

// The loader's own thunk: the game's wrapper, then the startup hold's write on the loader's temporary.
void afterLoader(uintptr_t obj);

// The VR profile allows the hold: never under the flat profile, and only where the runtime's features are on.
bool holdAllowedNow() { return !runtimeFlatProfile() && runtimeFeaturesAllowed(); }

bool loaderHoldThunk(void* a, void* b, void* c) {
    WrapperFn orig = g_wrapperOrig.load(std::memory_order_acquire);
    const bool r = orig ? orig(a, b, c) : true;
    afterLoader(reinterpret_cast<uintptr_t>(c));
    return r;
}

// The startup decision: after the loader wrote the .fxcfg's values into its temporary, the SS entry is set to 1.0 when the mode in
// Settings.xml is on. The game copies the temporary afterwards, so this write reaches the render context's copy (log shows which).
void afterLoader(uintptr_t obj) {
    if (!obj) return;
    g_loaderObj.store(obj, std::memory_order_release);
    edvrGraphicsEnsureInitialised();  // the config and the log, the same as the first device creation sets up (a no-op after it)
    const bool allowed = holdAllowedNow();
    g_holdAllowed.store(allowed, std::memory_order_release);
    int mode = -1;
    char raw[24] = {};
    const bool known = readMode(&mode, raw, sizeof(raw));
    g_mode.store(mode, std::memory_order_release);
    g_modeKnown.store(known, std::memory_order_release);
    float requested = 0.0f;
    if (!readFloatGuarded(obj + kSsFieldOff, &requested)) {
        Log::get().note("vr ssaa gate: startup loader ran but its Supersampling field at 0x%llX could not be read; not held",
                        static_cast<unsigned long long>(obj + kSsFieldOff));
        return;
    }
    const ssaahold::Decision d = ssaahold::decide(allowed, known, mode, requested);
    if (d.hold) {
        if (!writeFloatGuarded(obj + kSsFieldOff, d.value)) {
            Log::get().note("vr ssaa gate: holding Supersampling at 1.0 failed: the field at 0x%llX could not be written",
                            static_cast<unsigned long long>(obj + kSsFieldOff));
            return;
        }
        // The loader runs once per preset and once for Custom: a run logs only when its request or the mode changes.
        const uint32_t run = g_loaderRuns.fetch_add(1, std::memory_order_relaxed) + 1;
        const uint32_t bits = bitsOf(requested);
        if (run == 1 || bits != g_loaderLastReq.load(std::memory_order_relaxed) || mode != g_loaderLastMode.load(std::memory_order_relaxed)) {
            g_loaderLastReq.store(bits, std::memory_order_relaxed);
            g_loaderLastMode.store(mode, std::memory_order_relaxed);
            Log::get().note("vr ssaa gate: holding Supersampling at 1.0 (requested %.4f, 3D mode %d, at startup); loader object "
                            "0x%llX, field 0x%llX; loader run %u",
                            static_cast<double>(requested), mode, static_cast<unsigned long long>(obj),
                            static_cast<unsigned long long>(obj + kSsFieldOff), run);
        }
    } else if (!g_startupSaid.exchange(true)) {
        Log::get().note("vr ssaa gate: startup not held (%s); the game's Supersampling is %.4f (Settings.xml StereoscopicMode=%s)",
                        !allowed ? "flat profile" : !known ? "3D mode unknown" : "3D mode 0", static_cast<double>(requested), raw);
    }
}

}  // namespace

void vrSsaaHoldEarlyInstall() {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) {
        std::snprintf(g_slotWhy, sizeof(g_slotWhy), "the game module could not be found");
        g_slotState.store(2, std::memory_order_release);
        return;
    }
    if (!buildChecked(base, g_slotWhy, sizeof(g_slotWhy))) {
        g_slotState.store(2, std::memory_order_release);
        return;
    }
    // The wrapper: its bytes, and that its call lands on the loader.
    uint8_t got[sizeof(kWrapperBytes)] = {};
    if (!readGame(base + kWrapperRva, got, sizeof(got)) || std::memcmp(got, kWrapperBytes, sizeof(got)) != 0) {
        std::snprintf(g_slotWhy, sizeof(g_slotWhy), "the wrapper at 0x%X is not build 332841's", kWrapperRva);
        g_slotState.store(2, std::memory_order_release);
        return;
    }
    int32_t rel = 0;
    std::memcpy(&rel, got + 8, 4);
    if (base + kWrapperRva + 12 + rel != base + kLoaderRva) {
        std::snprintf(g_slotWhy, sizeof(g_slotWhy), "the wrapper's call does not land on the loader at 0x%X", kLoaderRva);
        g_slotState.store(2, std::memory_order_release);
        return;
    }
    // The slot: it must hold the wrapper now, before anything has swapped it.
    uint64_t slot = 0;
    if (!readGame(base + kLoaderSlotRva, &slot, sizeof(slot)) || slot != reinterpret_cast<uint64_t>(base + kWrapperRva)) {
        std::snprintf(g_slotWhy, sizeof(g_slotWhy), "the slot at 0x%X does not hold the wrapper", kLoaderSlotRva);
        g_slotState.store(2, std::memory_order_release);
        return;
    }
    // The loader's hold: the replacement is written only once the page can be made writable; on failure nothing is installed.
    if (!writeSlot(const_cast<uint8_t*>(base) + kLoaderSlotRva, reinterpret_cast<uint64_t>(&loaderHoldThunk))) {
        std::snprintf(g_slotWhy, sizeof(g_slotWhy), "VirtualProtect failed (%lu)", static_cast<unsigned long>(g_slotErr));
        g_slotState.store(2, std::memory_order_release);
        return;
    }
    g_wrapperOrig.store(reinterpret_cast<WrapperFn>(const_cast<uint8_t*>(base + kWrapperRva)), std::memory_order_release);
    g_slotWritten = true;
    g_slotState.store(1, std::memory_order_release);
    // The setter and getter hooks: the same build gate and the same slots ui_panel_scale.cpp uses, once and for all.
    char why[200] = {};
    g_setterIn = uiPanelScaleEarlyHooks(why, sizeof(why));
    if (!g_setterIn) std::snprintf(g_setterWhy, sizeof(g_setterWhy), "%s", why);
}

void vrSsaaHoldReport() {
    if (g_slotState.load(std::memory_order_acquire) == 1) {
        if (g_setterIn)
            Log::get().note("vr ssaa gate: hold installed at DLL load: loader wrapper 0x%X (slot 0x%X) and the Supersampling setter, "
                            "build 332841 checked",
                            kWrapperRva, kLoaderSlotRva);
        else
            Log::get().note("vr ssaa gate: hold installed at DLL load: loader wrapper 0x%X (slot 0x%X); the setter hook refused, "
                            "so menu changes are not held: %s",
                            kWrapperRva, kLoaderSlotRva, g_setterWhy);
    } else {
        Log::get().note("vr ssaa gate: hold refused, nothing written: %s; the game's Supersampling is not held", g_slotWhy);
    }
}

void vrSsaaHoldShutdown() {
    if (!g_slotWritten) return;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    // The slot goes back only if the write lands; if it does not, the flag stays set (the game's own slot is still ours).
    if (base && writeSlot(const_cast<uint8_t*>(base) + kLoaderSlotRva, reinterpret_cast<uint64_t>(base + kWrapperRva)))
        g_slotWritten = false;
}

// The setter's value. The game's own value is remembered (restored on release); while the runtime's VR session is live, the value
// passed on is 1.0 and the hold is on; otherwise the game's value passes unchanged.
float vrSsaaHoldSetterValue(float requested) {
    g_gameBits.store(bitsOf(requested), std::memory_order_release);
    if (g_slotState.load(std::memory_order_acquire) != 1 || !g_holdAllowed.load(std::memory_order_acquire)) return requested;
    if (!g_vrLive.load(std::memory_order_acquire)) return requested;
    const uint32_t bits = bitsOf(requested);
    g_held.store(true, std::memory_order_release);
    g_requestedBits.store(bits, std::memory_order_release);
    if (g_loggedBits.load(std::memory_order_acquire) != bits) {
        g_loggedBits.store(bits, std::memory_order_release);
        Log::get().note("vr ssaa gate: holding Supersampling at 1.0 (requested %.4f, VR live 1, at menu)",
                        static_cast<double>(requested));
    }
    return ssaahold::kHeldValue;
}

bool vrSsaaHoldActive() { return g_held.load(std::memory_order_acquire); }

bool vrSsaaHoldVrLive() { return g_vrLive.load(std::memory_order_acquire); }

// The render thread, once a frame. The live VR decision comes from the runtime's render sizing record (valid while an OpenXR session
// has published its eye sizes; the runtime clears it at teardown): VR active -> the field is set to 1.0 at once; VR inactive -> the
// game's own value is restored, so flat uses the user's Supersampling and the panels follow it.
// The live-rise path: VR is live and the active profile allows the hold, so the field goes to 1.0 at once. Shared by the frame
// boundary and vrSsaaHoldProfileChanged.
static void holdEngageLive() {
    if (!g_holdAllowed.load(std::memory_order_acquire)) return;
    const uintptr_t ctx = g_ctx.load(std::memory_order_acquire);
    const uint32_t gameBits = g_gameBits.load(std::memory_order_acquire);
    char gameText[24] = "unknown";
    if (gameBits != kNoneBits) std::snprintf(gameText, sizeof(gameText), "%.4f", static_cast<double>(floatOf(gameBits)));
    if (ctx && writeFloatGuarded(ctx + kCtxSsOff, ssaahold::kHeldValue)) {
        g_held.store(true, std::memory_order_release);
        g_requestedBits.store(gameBits, std::memory_order_release);
        Log::get().note("vr ssaa gate: VR active -> holding at 1.0 (game's %s%s)", gameText,
                        g_arriving.load(std::memory_order_acquire) ? "; ahead of the profile switch, 3D turning on" : "");
    } else {
        Log::get().note("vr ssaa gate: VR active -> not held yet (game's %s; no render context or write failed)", gameText);
    }
}

// The release path: the game's own value goes back into the field, if the hold was on. Shared by the frame boundary (VR went
// inactive) and vrSsaaHoldProfileChanged (the profile no longer allows the hold).
static void holdRelease() {
    if (!g_held.exchange(false, std::memory_order_acq_rel)) return;
    const uintptr_t ctx = g_ctx.load(std::memory_order_acquire);
    const uint32_t gameBits = g_gameBits.load(std::memory_order_acquire);
    if (ctx && gameBits != kNoneBits && writeFloatGuarded(ctx + kCtxSsOff, floatOf(gameBits)))
        Log::get().note("vr ssaa gate: VR inactive -> released: the game's %.4f restored", static_cast<double>(floatOf(gameBits)));
    else
        Log::get().note("vr ssaa gate: VR inactive -> released; the game's value could not be restored (no render context or value)");
}

void vrSsaaHoldFrameBoundary() {
    if (g_slotState.load(std::memory_order_acquire) != 1) return;
    EdvrNativeRenderSizing sizing{};
    const bool live = edvrQueryNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1, sizeof(sizing), &sizing) && sizing.valid == 1;
    const bool was = g_vrLive.exchange(live, std::memory_order_acq_rel);
    if (live == was) return;
    const bool profileAllows = holdAllowedNow();
    if (live) {
        // A rise under a flat profile is a 3D-on whose apply runs before the profile switch commits (~0.5 s later in the 2026-10-14
        // flight): the hold is armed now so the next sizing pass reads 1.0, not the flat 0.65. The profile switch clears the latch.
        const bool arriving = ssaahold::arrivingAtRise(profileAllows, runtimeScopeIncludesVr(), runtimeFeaturesAllowed());
        g_arriving.store(arriving, std::memory_order_release);
        g_holdAllowed.store(ssaahold::holdAllowedFor(profileAllows, arriving), std::memory_order_release);
        if (arriving)
            Log::get().note("vr ssaa gate: VR session live under the %s profile (3D turning on): the hold is armed ahead of the profile "
                            "switch; settings copies so far %u",
                            runtimeProfileName(), g_copyCalls.load(std::memory_order_relaxed));
        holdEngageLive();
    } else {
        g_arriving.store(false, std::memory_order_release);
        g_holdAllowed.store(profileAllows, std::memory_order_release);
        holdRelease();
    }
}

// Called at a profile switch, render thread, frame boundary (vr_ssaa_hold.h). The hold follows the active profile as well as the live
// session: g_holdAllowed is recomputed, and the field is released or engaged to match by the two paths above. Does nothing until
// the loader hook is installed; safe to call twice (the second call finds the field already where it should be).
void vrSsaaHoldProfileChanged() {
    if (g_slotState.load(std::memory_order_acquire) != 1) return;
    g_arriving.store(false, std::memory_order_release);  // the profile now decides
    g_holdAllowed.store(holdAllowedNow(), std::memory_order_release);
    switch (ssaahold::profileChangeAction(g_holdAllowed.load(std::memory_order_acquire), g_vrLive.load(std::memory_order_acquire),
                                          g_held.load(std::memory_order_acquire))) {
        case ssaahold::ProfileAction::Release: holdRelease(); break;
        case ssaahold::ProfileAction::Engage: holdEngageLive(); break;
        case ssaahold::ProfileAction::None: break;
    }
}

// The copy's source block, for the instrument only (log): W copies 0x160 bytes of the active preset entry, [[ctx+0x33C8]+0x230] +
// index*0x178 + 0x14 (index [obj+0x258], count [obj+0x228]), to ctx+0x3428, and a small header of the object (+0x30, +0x58..+0x80) to
// ctx+0x33D0.. (read from 0x281C0D0..0x281C3EB, build 332841). Nothing in it is proven to hold the 3D mode; this logs which dwords change
// between copies, so a 3D-on apply shows the mode field if it is there. Never written.
constexpr uintptr_t kCtxSettingsObjOff = 0x33C8;
constexpr uint32_t kBlockBytes = 0x160, kHeadFrom = 0x30, kHeadBytes = 0x54;  // object +0x30 .. +0x84
constexpr uint32_t kBlockDwords = (kBlockBytes + kHeadBytes) / 4;
static uint32_t g_blockPrev[kBlockDwords] = {};
static bool g_blockHavePrev = false;
static uint32_t g_blockLines = 0;
static std::atomic_flag g_blockBusy = ATOMIC_FLAG_INIT;

// Reads the block into out (kBlockDwords dwords) and the preset index and count. False when any pointer does not read.
static bool readCopyBlock(uintptr_t ctx, uint32_t* out, uint32_t* index, uint32_t* count) {
    uint64_t obj = 0, table = 0;
    if (!readGame(reinterpret_cast<const uint8_t*>(ctx + kCtxSettingsObjOff), &obj, 8) || !obj) return false;
    if (!readGame(reinterpret_cast<const uint8_t*>(obj + 0x258), index, 4)) return false;
    if (!readGame(reinterpret_cast<const uint8_t*>(obj + 0x228), count, 4)) return false;
    if (!readGame(reinterpret_cast<const uint8_t*>(obj + 0x230), &table, 8) || !table || *index >= *count) return false;
    if (!readGame(reinterpret_cast<const uint8_t*>(table + static_cast<uint64_t>(*index) * 0x178 + 0x14), out, kBlockBytes)) return false;
    return readGame(reinterpret_cast<const uint8_t*>(obj + kHeadFrom), out + kBlockBytes / 4, kHeadBytes);
}

// One line per copy that changed any dword (first 10 changes named), capped at 40 lines: "e+0x<off>" is the preset entry, "o+0x<off>" the
// object header. The entry's offset 0x13C is the Supersampling field.
static void logCopyBlockChange(uintptr_t ctx, uint32_t call) {
    if (g_blockBusy.test_and_set(std::memory_order_acquire)) return;  // another thread is in it: this copy is simply not compared
    uint32_t cur[kBlockDwords] = {};
    uint32_t index = 0, count = 0;
    if (!readCopyBlock(ctx, cur, &index, &count)) {
        g_blockBusy.clear(std::memory_order_release);
        return;
    }
    if (g_blockHavePrev && g_blockLines < 40) {
        char list[400] = {};
        size_t at = 0;
        uint32_t changed = 0;
        for (uint32_t i = 0; i < kBlockDwords; ++i) {
            if (cur[i] == g_blockPrev[i]) continue;
            if (++changed > 10) continue;
            const bool entry = i < kBlockBytes / 4;
            const uint32_t off = entry ? i * 4 : kHeadFrom + (i - kBlockBytes / 4) * 4;
            const int w = std::snprintf(list + at, sizeof(list) - at, " %c+0x%X %u->%u;", entry ? 'e' : 'o', off, g_blockPrev[i], cur[i]);
            if (w > 0 && at + static_cast<size_t>(w) < sizeof(list)) at += static_cast<size_t>(w);
        }
        if (changed) {
            ++g_blockLines;
            Log::get().note("vr ssaa gate: copy call %u: preset %u of %u; %u dword(s) of the copied block changed since the last copy:%s%s",
                            call, index, count, changed, list, changed > 10 ? " (more)" : "");
        }
    }
    std::memcpy(g_blockPrev, cur, sizeof(cur));
    g_blockHavePrev = true;
    g_blockBusy.clear(std::memory_order_release);
}

// The game's 3D mode in the settings object (+0x30, the dword W copies to ctx+0x33D0): Settings.xml's StereoscopicMode, 0 when 3D is off.
// False when the object or the dword does not read, or the value is not 0..6.
static bool readBlockMode(uintptr_t ctx, unsigned* mode) {
    uint64_t obj = 0;
    uint32_t raw = 0;
    if (!readGame(reinterpret_cast<const uint8_t*>(ctx + kCtxSettingsObjOff), &obj, 8) || !obj) return false;
    if (!readGame(reinterpret_cast<const uint8_t*>(obj + kHeadFrom), &raw, 4) || !ssaahold::blockModeKnown(raw)) return false;
    *mode = raw;
    return true;
}

static std::atomic<int> g_lastBlockMode{-1};  // the previous copy's block mode (-1 none yet): a rise from 0 is a 3D-on apply

// The settings copy's hold (the W relay in vr_window_trampolines.cpp): after the game copies its settings into the render context,
// the copied value is the game's own, and when the copy says 3D is on the field is set to 1.0 (the size writer reads it next). The source
// block is not written. The decision is ssaahold::copyHolds: the block's own 3D mode when it reads (any profile, any live state, since the
// apply runs 0.7 s before the profile switch and the runtime session never drops on a live switch), else the VR profile or an arriving
// 3D-on with a live session. Every call is counted; a line is logged on the first calls and whenever the inputs, the result or the copied
// value change (capped at 64), so the next log says what each sizing pass saw.
void vrSsaaHoldAfterCopy(uintptr_t ctx) {
    if (!ctx || g_slotState.load(std::memory_order_acquire) != 1) return;
    const uint32_t call = g_copyCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool live = g_vrLive.load(std::memory_order_acquire);
    const bool arriving = g_arriving.load(std::memory_order_acquire);
    const bool profileAllows = holdAllowedNow();
    g_ctx.store(ctx, std::memory_order_release);
    float copied = 0.0f;
    const bool haveCopied = readFloatGuarded(ctx + kCtxSsOff, &copied);
    if (haveCopied) g_gameBits.store(bitsOf(copied), std::memory_order_release);  // the copy is the game's own value in every profile
    unsigned blockMode = 0;
    const bool modeKnown = readBlockMode(ctx, &blockMode);
    const int lastMode = g_lastBlockMode.exchange(modeKnown ? static_cast<int>(blockMode) : -1, std::memory_order_acq_rel);
    const bool modeRose = modeKnown && lastMode == 0 && blockMode != 0;
    const bool holds = ssaahold::copyHolds(modeKnown, blockMode, modeRose, runtimeScopeIncludesVr(), runtimeFeaturesAllowed(), profileAllows,
                                           live, arriving);
    const bool allowed = holds;  // the instrument line's "hold allowed" is the decision
    int wrote = 0;  // 0 not written, 1 written 1.0, -1 write failed
    if (holds) {
        if (writeFloatGuarded(ctx + kCtxSsOff, ssaahold::kHeldValue)) {
            wrote = 1;
            g_held.store(true, std::memory_order_release);
            g_requestedBits.store(haveCopied ? bitsOf(copied) : kNoneBits, std::memory_order_release);
        } else {
            wrote = -1;
        }
    } else {
        g_copyHeld.store(false, std::memory_order_release);
    }
    // The instrument line.
    const uint32_t bits = haveCopied ? bitsOf(copied) : kNoneBits;
    const uint32_t key = (allowed ? 1u : 0u) | (live ? 2u : 0u) | (arriving ? 4u : 0u) | (wrote == 1 ? 8u : wrote < 0 ? 16u : 0u) |
                         (profileAllows ? 32u : 0u) | (modeKnown ? 64u : 0u) | (modeRose ? 128u : 0u) | ((blockMode & 0x7u) << 8);
    if (g_copyLines.load(std::memory_order_relaxed) < 64 &&
        (call <= 6 || key != g_copyLastKey.load(std::memory_order_relaxed) || bits != g_copyLastBits.load(std::memory_order_relaxed))) {
        g_copyLastKey.store(key, std::memory_order_relaxed);
        g_copyLastBits.store(bits, std::memory_order_relaxed);
        g_copyLines.fetch_add(1, std::memory_order_relaxed);
        char copiedText[24] = "unknown";
        if (haveCopied) std::snprintf(copiedText, sizeof(copiedText), "%.4f", static_cast<double>(copied));
        char modeText[24] = "unreadable";
        if (modeKnown) std::snprintf(modeText, sizeof(modeText), "%u%s", blockMode, modeRose ? " (rose from 0)" : "");
        Log::get().note("vr ssaa gate: copy call %u (W 0x281C0D0), frame %u: profile %s, block 3D mode %s, hold %d (VR profile %d, arriving "
                        "%d), VR live %d; copied %s, %s; ctx 0x%llX",
                        call, vrSsaaGateFrame(), runtimeProfileName(), modeText, allowed ? 1 : 0, profileAllows ? 1 : 0, arriving ? 1 : 0,
                        live ? 1 : 0, copiedText,
                        wrote == 1 ? "wrote 1.0" : wrote < 0 ? "write FAILED" : "wrote nothing (the sizing pass reads the copied value)",
                        static_cast<unsigned long long>(ctx));
    }
    logCopyBlockChange(ctx, call);
    if (wrote < 0) {
        Log::get().note("vr ssaa gate: holding Supersampling at 1.0 after the settings copy failed: the field at 0x%llX could not be "
                        "written",
                        static_cast<unsigned long long>(ctx + kCtxSsOff));
        return;
    }
    if (wrote == 1 && (!g_copyHeld.load(std::memory_order_acquire) || bits != g_copyBits.load(std::memory_order_acquire))) {
        g_copyHeld.store(true, std::memory_order_release);
        g_copyBits.store(bits, std::memory_order_release);
        Log::get().note("vr ssaa gate: holding Supersampling at 1.0 after the settings copy (W 0x281C0D0): copied %s, held 1.0, "
                        "VR live 1",
                        haveCopied ? "(see the copy call line)" : "unknown");
    }
}

void vrSsaaHoldNoteContext(uintptr_t ctx) {
    if (ctx) g_ctx.store(ctx, std::memory_order_release);
}

bool vrSsaaHoldNoticeDue(float* requested) {
    if (!g_held.load(std::memory_order_acquire)) return false;
    const uint32_t bits = g_requestedBits.load(std::memory_order_acquire);
    if (bits == kNoneBits) return false;
    const float value = floatOf(bits);
    if (value == ssaahold::kHeldValue) return false;
    if (g_noticedBits.load(std::memory_order_acquire) == bits) return false;
    g_noticedBits.store(bits, std::memory_order_release);
    if (requested) *requested = value;
    return true;
}

bool vrSsaaHoldReadSettings(int* mode, char* raw, size_t rawLen) {
    if (!mode || !raw || !rawLen) return false;
    return readMode(mode, raw, rawLen);
}

unsigned long long vrSsaaHoldLoaderObject() {
    return static_cast<unsigned long long>(g_loaderObj.load(std::memory_order_acquire));
}

int vrSsaaHoldLastMode(bool* known) {
    if (known) *known = g_modeKnown.load(std::memory_order_acquire);
    return g_mode.load(std::memory_order_acquire);
}

}  // namespace edvr

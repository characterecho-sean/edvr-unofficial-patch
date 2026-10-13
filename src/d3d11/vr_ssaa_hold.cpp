// The VR Supersampling hold (vr_ssaa_hold.h says what and why). The decision is vr_ssaa_hold_math.h's; this file is the game's side.
#include "vr_ssaa_hold.h"

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
        Log::get().note("vr ssaa gate: VR active -> holding at 1.0 (game's %s)", gameText);
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
    if (live) holdEngageLive();
    else holdRelease();
}

// Called at a profile switch, render thread, frame boundary (vr_ssaa_hold.h). The hold follows the active profile as well as the live
// session: g_holdAllowed is recomputed, and the field is released or engaged to match by the two paths above. Does nothing until
// the loader hook is installed; safe to call twice (the second call finds the field already where it should be).
void vrSsaaHoldProfileChanged() {
    if (g_slotState.load(std::memory_order_acquire) != 1) return;
    g_holdAllowed.store(holdAllowedNow(), std::memory_order_release);
    switch (ssaahold::profileChangeAction(g_holdAllowed.load(std::memory_order_acquire), g_vrLive.load(std::memory_order_acquire),
                                          g_held.load(std::memory_order_acquire))) {
        case ssaahold::ProfileAction::Release: holdRelease(); break;
        case ssaahold::ProfileAction::Engage: holdEngageLive(); break;
        case ssaahold::ProfileAction::None: break;
    }
}

// The settings copy's hold (the W relay in vr_window_trampolines.cpp): after the game copies its settings into the render context,
// the copied value is the game's own, and while VR is live the field is set to 1.0 (the size writer reads it next). The source block
// is not written. Writes nothing when VR is not live.
void vrSsaaHoldAfterCopy(uintptr_t ctx) {
    if (!ctx || g_slotState.load(std::memory_order_acquire) != 1 || !g_holdAllowed.load(std::memory_order_acquire)) return;
    g_ctx.store(ctx, std::memory_order_release);
    float copied = 0.0f;
    const bool haveCopied = readFloatGuarded(ctx + kCtxSsOff, &copied);
    if (haveCopied) g_gameBits.store(bitsOf(copied), std::memory_order_release);
    if (!g_vrLive.load(std::memory_order_acquire)) {
        g_copyHeld.store(false, std::memory_order_release);
        return;
    }
    if (!writeFloatGuarded(ctx + kCtxSsOff, ssaahold::kHeldValue)) {
        Log::get().note("vr ssaa gate: holding Supersampling at 1.0 after the settings copy failed: the field at 0x%llX could not be "
                        "written",
                        static_cast<unsigned long long>(ctx + kCtxSsOff));
        return;
    }
    g_held.store(true, std::memory_order_release);
    const uint32_t bits = haveCopied ? bitsOf(copied) : kNoneBits;
    g_requestedBits.store(bits, std::memory_order_release);
    if (!g_copyHeld.load(std::memory_order_acquire) || bits != g_copyBits.load(std::memory_order_acquire)) {
        g_copyHeld.store(true, std::memory_order_release);
        g_copyBits.store(bits, std::memory_order_release);
        char copiedText[24] = "unknown";
        if (haveCopied) std::snprintf(copiedText, sizeof(copiedText), "%.4f", static_cast<double>(copied));
        Log::get().note("vr ssaa gate: holding Supersampling at 1.0 after the settings copy (W 0x281C0D0): copied %s, held 1.0, "
                        "VR live 1",
                        copiedText);
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

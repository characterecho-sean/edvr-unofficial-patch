// The VR Supersampling hold (vr_ssaa_hold.h says what and why). The decision is vr_ssaa_hold_math.h's; this file is the game's side.
#include "vr_ssaa_hold.h"

#include "vr_ssaa_hold_math.h"
#include "vr_display_observer.h"  // vrDisplayCallerText: the sizing watch's callers
#include "vr_context_watch.h"     // the hardware write watch on the context's field (armed from the first context)
#include "ui_panel_scale.h"  // uiPanelScaleEarlyHooks: the setter and getter hooks, installed at DLL load
#include "ui_sizing_math.h"  // kUiPanelStamp, kUiPanelImageSize: build 332841

#include "../common/elite_graphics_folder.h"  // the Options\Graphics folder under %LOCALAPPDATA%
#include "../common/log.h"
#include "../common/runtime_profile.h"        // runtimeFlatProfile, runtimeFeaturesAllowed

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
constexpr uint32_t kSsFieldOff = 0x13C;     // the loader object's SS field: render context +0x3564
constexpr double kRereadSeconds = 1.5;     // Settings.xml is re-read this long after a setter call
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
std::atomic<bool> g_holdAllowed{false};   // the VR profile: set when the loader runs (config is up by then)
std::atomic<bool> g_modeKnown{false};
std::atomic<int> g_mode{-1};
std::atomic<bool> g_held{false};
std::atomic<uint32_t> g_requestedBits{0};  // the game's requested value (float bits), while held
std::atomic<uint32_t> g_loggedBits{0};     // the requested value the last held line said (float bits)
std::atomic<bool> g_loggedAny{false};
std::atomic<uint32_t> g_noticedBits{0};
std::atomic<bool> g_noticedAny{false};
std::atomic<uint64_t> g_rereadAt{0};       // QPC ticks; 0 when no re-read is pending
std::atomic<uintptr_t> g_loaderObj{0};
std::atomic<bool> g_startupSaid{false};
// The render context the setter was last called on (vrSsaaHoldNoteContext): the field the hold re-applies to on a 0 -> on change.
std::atomic<uintptr_t> g_ctx{0};
constexpr uintptr_t kCtxSsOff = 0x3564;  // render context +0x3564: the Supersampling field every sizing consumer reads
// The sizing watch (H7): for 3 s after a setter call or a mode change, the game's render and depth targets are logged with their callers.
std::atomic<uint64_t> g_watchUntil{0};   // QPC ticks; 0 when not armed
std::atomic<int64_t> g_watchArmed{0};    // QPC ticks at arming
std::atomic<uint8_t> g_watchWhy{0};      // 1 a setter call, 2 a mode change
std::atomic<uint32_t> g_watchLines{0};
constexpr uint32_t kWatchLines = 96;
constexpr double kWatchSeconds = 3.0;
// The settings watch (read-only, H8): the loader object's SS field and the render context's, once per Present.
std::atomic<int64_t> g_lastSetterTicks{0};  // QPC ticks of the last setter call (0: none yet)
uint32_t g_watchFieldBits = 0;
bool g_watchSeen = false;
std::atomic<uint32_t> g_watchWatchLines{0};
constexpr uint32_t kWatchWatchCap = 128;
// The loader's repeated runs (the presets, then Custom): a run logs only when its request or mode changes, and counts the rest.
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

int64_t holdTicksNow() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

int64_t holdTicksPerSecond() {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}

// Arms the sizing watch for kWatchSeconds from now: why is 1 (a setter call) or 2 (a mode change).
void armWatch(uint8_t why) {
    const int64_t now = holdTicksNow();
    g_watchArmed.store(now, std::memory_order_relaxed);
    g_watchWhy.store(why, std::memory_order_relaxed);
    g_watchUntil.store(static_cast<uint64_t>(now + static_cast<int64_t>(kWatchSeconds * static_cast<double>(holdTicksPerSecond()))),
                       std::memory_order_release);
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
DWORD g_slotErr = 0;
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

// The render thread's and the game thread's shared settings read (the loader and the re-read): the mode and what raw says.
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

// The loader's own thunk: the game's wrapper, then the hold's decision on the loader's object.
void afterLoader(uintptr_t obj);

bool loaderHoldThunk(void* a, void* b, void* c) {
    WrapperFn orig = g_wrapperOrig.load(std::memory_order_acquire);
    const bool r = orig ? orig(a, b, c) : true;
    afterLoader(reinterpret_cast<uintptr_t>(c));
    return r;
}

// The startup decision: after the loader wrote the .fxcfg's values into the object, the field is held at 1.0 when the mode is on.
void afterLoader(uintptr_t obj) {
    if (!obj) return;
    g_loaderObj.store(obj, std::memory_order_release);
    // The config and the log, the same as the first device creation sets up (a no-op after the first).
    edvrGraphicsEnsureInitialised();
    const bool allowed = !runtimeFlatProfile() && runtimeFeaturesAllowed();
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
        g_held.store(true, std::memory_order_release);
        g_requestedBits.store(bitsOf(requested), std::memory_order_release);
        g_loggedBits.store(bitsOf(requested), std::memory_order_release);
        g_loggedAny.store(true, std::memory_order_release);
        // The loader runs once per preset and once for Custom: a run logs only when its request or the mode changes, and carries
        // the run number, so the count of the rest is in the line that follows them.
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
    } else {
        g_held.store(false, std::memory_order_release);
        if (!g_startupSaid.exchange(true))
            Log::get().note("vr ssaa gate: startup not held (%s); the game's Supersampling is %.4f (Settings.xml "
                            "StereoscopicMode=%s)",
                            !allowed ? "flat profile" : !known ? "3D mode unknown" : "3D mode 0", static_cast<double>(requested),
                            raw);
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
    if (!writeSlot(const_cast<uint8_t*>(base) + kLoaderSlotRva, reinterpret_cast<uint64_t>(&loaderHoldThunk))) {
        // Nothing was written: the slot still holds the wrapper, so the hold stays off and the state says so.
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
            Log::get().note("vr ssaa gate: hold installed at DLL load: loader wrapper 0x%X (slot 0x%X) and the Supersampling "
                            "setter, build 332841 checked",
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
    vrContextWatchShutdown();  // the write watch's debug registers and handler, if armed
    if (!g_slotWritten) return;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    // The slot goes back only if the write lands; if it does not, the flag stays set (the game's own slot is still ours).
    if (base && writeSlot(const_cast<uint8_t*>(base) + kLoaderSlotRva, reinterpret_cast<uint64_t>(base + kWrapperRva)))
        g_slotWritten = false;
}

int vrSsaaHoldLastMode(bool* known) {
    if (known) *known = g_modeKnown.load(std::memory_order_acquire);
    return g_mode.load(std::memory_order_acquire);
}

float vrSsaaHoldSetterValue(float requested) {
    g_lastSetterTicks.store(holdTicksNow(), std::memory_order_release);  // the settings watch's "after the last setter call"
    if (g_slotState.load(std::memory_order_acquire) != 1 || !g_holdAllowed.load(std::memory_order_acquire)) return requested;
    const ssaahold::Decision d = ssaahold::decide(true, g_modeKnown.load(std::memory_order_acquire),
                                                  g_mode.load(std::memory_order_acquire), requested);
    // A setter call asks for a mode re-read: the game rewrites Settings.xml on apply, just after this.
    g_rereadAt.store(static_cast<uint64_t>(holdTicksNow() + static_cast<int64_t>(kRereadSeconds * static_cast<double>(holdTicksPerSecond()))),
                     std::memory_order_release);
    armWatch(1);  // the sizing watch (H7): the targets created in the next seconds, with their callers
    const bool wasHeld = g_held.exchange(d.hold, std::memory_order_acq_rel);
    const uint32_t bits = bitsOf(requested);
    if (d.hold) {
        g_requestedBits.store(bits, std::memory_order_release);
        if (!wasHeld || !g_loggedAny.load(std::memory_order_acquire) || bits != g_loggedBits.load(std::memory_order_acquire)) {
            g_loggedBits.store(bits, std::memory_order_release);
            g_loggedAny.store(true, std::memory_order_release);
            Log::get().note("vr ssaa gate: holding Supersampling at 1.0 (requested %.4f, 3D mode %d, at menu)",
                            static_cast<double>(requested), g_mode.load(std::memory_order_acquire));
        }
    } else if (wasHeld) {
        Log::get().note("vr ssaa gate: released: the game's Supersampling %.4f passes again (3D mode %d)",
                        static_cast<double>(requested), g_mode.load(std::memory_order_acquire));
    }
    return d.value;
}

bool vrSsaaHoldActive() { return g_held.load(std::memory_order_acquire); }

void vrSsaaHoldFrameBoundary() {
    const uint64_t due = g_rereadAt.load(std::memory_order_acquire);
    if (!due || static_cast<uint64_t>(holdTicksNow()) < due) return;
    g_rereadAt.store(0, std::memory_order_release);
    int mode = -1;
    char raw[24] = {};
    const bool known = readMode(&mode, raw, sizeof(raw));
    const bool oldKnown = g_modeKnown.load(std::memory_order_acquire);
    const int old = g_mode.load(std::memory_order_acquire);
    g_mode.store(mode, std::memory_order_release);
    g_modeKnown.store(known, std::memory_order_release);
    if (known == oldKnown && mode == old) return;
    Log::get().note("vr ssaa gate: 3D mode %s -> %s (Settings.xml, re-read %.1f s after a Supersampling call)",
                    oldKnown ? std::to_string(old).c_str() : "unknown", known ? raw : "unknown", kRereadSeconds);
    if (oldKnown && old != 0 && known && mode == 0 && g_held.load(std::memory_order_acquire))
        Log::get().note("vr ssaa gate: 3D mode is now 0; the held Supersampling stays at 1.0 until the next apply or restart "
                        "(not chased)");
    armWatch(2);  // the sizing watch (H7): the targets the game makes after a mode change, with their callers
    // GAP 2: 0 (or unknown) -> on re-holds at once, on the render context the setter last named. The game's apply after the switch
    // has already passed the setter, so waiting for the next apply would leave the field at the game's value until then.
    const bool turnedOn = known && mode != 0 && (!oldKnown || old == 0);
    if (turnedOn && g_holdAllowed.load(std::memory_order_acquire) && !g_held.load(std::memory_order_acquire)) {
        const uintptr_t ctx = g_ctx.load(std::memory_order_acquire);
        float cur = 0.0f;
        if (ctx && readFloatGuarded(ctx + kCtxSsOff, &cur) && writeFloatGuarded(ctx + kCtxSsOff, ssaahold::kHeldValue)) {
            g_held.store(true, std::memory_order_release);
            g_requestedBits.store(bitsOf(cur), std::memory_order_release);
            g_loggedBits.store(bitsOf(cur), std::memory_order_release);
            g_loggedAny.store(true, std::memory_order_release);
            Log::get().note("vr ssaa gate: holding Supersampling at 1.0 (requested %.4f, 3D mode %d, at mode change); context "
                            "0x%llX",
                            static_cast<double>(cur), mode, static_cast<unsigned long long>(ctx));
        }
    }
}

void vrSsaaHoldNoteContext(uintptr_t ctx) {
    if (!ctx) return;
    // The first context arms the write watch on its Supersampling field (H7/H8); later calls only store the pointer.
    if (g_ctx.exchange(ctx, std::memory_order_acq_rel) == 0) vrContextWatchArm(ctx + kCtxSsOff);
}

// The settings watch (H8): read-only, once per Present, on the render thread. Logs a change of the loader object's SS field
// (+0x13C) or of the render context's (+0x3564), with the time since the last setter call. Writes nothing.
// Only the render context's field: the loader's object is a stack temporary that the game copies from afterwards, so reads of it after
// the loader returns are dead stack (H8). The change test compares the bits this watch last saw, and a read that fails is "unknown",
// never a value.
void vrSsaaHoldWatchSettings(uint32_t frame) {
    if (g_slotState.load(std::memory_order_acquire) != 1) return;
    const uintptr_t ctx = g_ctx.load(std::memory_order_acquire);
    if (!ctx) return;
    float field = 0.0f;
    const bool haveF = readFloatGuarded(ctx + kCtxSsOff, &field);
    const uint32_t fb = haveF ? bitsOf(field) : 0xFFFFFFFFu;
    if (g_watchSeen && fb == g_watchFieldBits) return;
    const bool first = !g_watchSeen;
    const uint32_t prevFb = g_watchFieldBits;
    g_watchSeen = true;
    g_watchFieldBits = fb;
    const uint32_t lines = g_watchWatchLines.fetch_add(1, std::memory_order_relaxed);
    if (lines >= kWatchWatchCap) return;
    char oldF[24] = "unknown", newF[24] = "unknown", since[32] = "no setter call yet";
    if (haveF) std::snprintf(newF, sizeof(newF), "%.4f", static_cast<double>(field));
    if (!first && prevFb != 0xFFFFFFFFu) std::snprintf(oldF, sizeof(oldF), "%.4f", static_cast<double>(floatOf(prevFb)));
    const int64_t last = g_lastSetterTicks.load(std::memory_order_acquire);
    if (last)
        std::snprintf(since, sizeof(since), "%.1f ms", 1000.0 * static_cast<double>(holdTicksNow() - last) /
                                                          static_cast<double>(holdTicksPerSecond()));
    Log::get().note("vr ssaa gate: watch: ctx+0x3564 %s -> %s, frame %u, %s after the last setter call",
                    first ? "first read" : oldF, newF, frame, since);
}

void vrSizingWatchTexture(uint32_t w, uint32_t h, uint32_t format, uint32_t bind) {
    const uint64_t until = g_watchUntil.load(std::memory_order_acquire);
    if (!until) return;
    const int64_t now = holdTicksNow();
    if (static_cast<uint64_t>(now) >= until) {
        g_watchUntil.store(0, std::memory_order_release);
        return;
    }
    constexpr uint32_t kRenderTarget = 0x20u, kDepthStencil = 0x40u;  // D3D11_BIND_RENDER_TARGET, D3D11_BIND_DEPTH_STENCIL
    if (!(bind & (kRenderTarget | kDepthStencil))) return;
    if (g_watchLines.fetch_add(1, std::memory_order_relaxed) >= kWatchLines) return;
    void* frames[6] = {};
    const USHORT got = CaptureStackBackTrace(1, 6, frames, nullptr);  // frame 0: the create hook; then the game's callers
    char chain[400];
    chain[0] = '\0';
    size_t used = 0;
    for (USHORT i = 0; i < got && used + 2 < sizeof(chain); ++i) {
        char one[96];
        vrDisplayCallerText(frames[i], one, sizeof(one));
        const int m = std::snprintf(chain + used, sizeof(chain) - used, "%s%s", used ? " <- " : "", one);
        if (m < 0) break;
        used += static_cast<size_t>(m);
    }
    const double secs = static_cast<double>(now - g_watchArmed.load(std::memory_order_relaxed)) /
                        static_cast<double>(holdTicksPerSecond());
    const char* kind = (bind & kRenderTarget) && (bind & kDepthStencil) ? "render and depth-stencil" : (bind & kRenderTarget) ? "render target" : "depth-stencil";
    Log::get().note("vr sizing: %s %ux%u format %u bind 0x%X created %.2f s after a %s; callers %s", kind, w, h, format, bind, secs,
                    g_watchWhy.load(std::memory_order_relaxed) == 1 ? "Supersampling setter call" : "3D mode change", chain);
}

bool vrSsaaHoldNoticeDue(float* requested) {
    if (!g_held.load(std::memory_order_acquire)) return false;
    const uint32_t bits = g_requestedBits.load(std::memory_order_acquire);
    const float value = floatOf(bits);
    if (value == ssaahold::kHeldValue) return false;
    if (g_noticedAny.load(std::memory_order_acquire) && g_noticedBits.load(std::memory_order_acquire) == bits) return false;
    g_noticedBits.store(bits, std::memory_order_release);
    g_noticedAny.store(true, std::memory_order_release);
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

}  // namespace edvr

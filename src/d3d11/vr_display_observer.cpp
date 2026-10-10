// The display observer (vr_display_observer.h says what and why). Log only.
#include "vr_display_observer.h"

#include "vr_ssaa_gate.h"  // vrSsaaGateFrame: the frame number the lines carry
#include "vr_ssaa_hold.h"  // vrSsaaHoldLastMode: the Settings.xml mode as the hold last knew it

#include "../common/log.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <wchar.h>

namespace edvr {
namespace {

constexpr uint32_t kFirstLines = 64;   // every call logs while its count is at or under this
constexpr uint32_t kHardCap = 512;     // change-only lines stop here

struct Cap {
    std::atomic<uint32_t> seen{0};
    std::atomic<uint64_t> lastKey{0};
};
Cap g_fullscreen, g_resizeTarget, g_resizeBuffers;

// The first kFirstLines calls log; past that only a call whose key differs from the last logged one, up to kHardCap.
bool capAllow(Cap& c, uint64_t key) {
    const uint32_t n = c.seen.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= kFirstLines) {
        c.lastKey.store(key, std::memory_order_relaxed);
        return true;
    }
    if (n > kHardCap || c.lastKey.load(std::memory_order_relaxed) == key) return false;
    c.lastKey.store(key, std::memory_order_relaxed);
    return true;
}

// The caller as an RVA into the game's image when the return address lies there, else module name + offset, else the raw address.
void callerText(void* ret, char* out, size_t n) {
    HMODULE mod = nullptr;
    if (!ret || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(ret), &mod) || !mod) {
        std::snprintf(out, n, "unknown 0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ret)));
        return;
    }
    const uintptr_t off = reinterpret_cast<uintptr_t>(ret) - reinterpret_cast<uintptr_t>(mod);
    if (mod == GetModuleHandleW(nullptr)) {
        std::snprintf(out, n, "game RVA 0x%llX", static_cast<unsigned long long>(off));
        return;
    }
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(mod, path, MAX_PATH);
    const wchar_t* leaf = wcsrchr(path, L'\\');
    std::snprintf(out, n, "%ls+0x%llX", leaf ? leaf + 1 : path, static_cast<unsigned long long>(off));
}

void modeText(char* out, size_t n) {
    bool known = false;
    const int mode = vrSsaaHoldLastMode(&known);
    if (known) std::snprintf(out, n, "%d", mode);
    else std::snprintf(out, n, "unknown");
}

// The game window: the swap chain's output window, set once at hookSwapChain. Render thread only, after the store.
std::atomic<void*> g_hwnd{nullptr};
std::atomic<bool> g_windowSeen{false};
LONG_PTR g_style = 0;
LONG g_cw = 0, g_ch = 0;

}  // namespace

void vrDisplayObserveInstalled(bool setFullscreenState, bool resizeTarget, const char* why) {
    if (setFullscreenState && resizeTarget) {
        Log::get().note("vr ssaa gate: display observer installed (SetFullscreenState, ResizeTarget)");
        return;
    }
    Log::get().note("vr ssaa gate: display observer not installed: SetFullscreenState %s, ResizeTarget %s; %s",
                    setFullscreenState ? "ok" : "no", resizeTarget ? "ok" : "no", why ? why : "the vtable hook refused");
}

void vrDisplayObserveWindow(void* hwnd) {
    g_windowSeen.store(false, std::memory_order_release);
    g_hwnd.store(hwnd, std::memory_order_release);
}

void vrDisplayObserveFrame() {
    void* h = g_hwnd.load(std::memory_order_acquire);
    if (!h) return;
    const HWND hwnd = static_cast<HWND>(h);
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const LONG cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    const bool seen = g_windowSeen.load(std::memory_order_acquire);
    if (seen && style == g_style && cw == g_cw && ch == g_ch) return;
    const uint32_t frame = vrSsaaGateFrame();
    if (seen)
        Log::get().note("vr display: game window style 0x%llX client %ldx%ld (was 0x%llX %ldx%ld), frame %u",
                        static_cast<unsigned long long>(style), cw, ch, static_cast<unsigned long long>(g_style), g_cw, g_ch,
                        frame);
    else
        Log::get().note("vr display: game window at the first frame: style 0x%llX client %ldx%ld, frame %u",
                        static_cast<unsigned long long>(style), cw, ch, frame);
    g_style = style;
    g_cw = cw;
    g_ch = ch;
    g_windowSeen.store(true, std::memory_order_release);
}

void vrDisplayNoteFullscreen(int fullscreen, bool targetGiven, void* ret, long hr, bool ours) {
    const uint64_t key = (fullscreen ? 1u : 0u) | (targetGiven ? 2u : 0u) | (static_cast<uint64_t>(static_cast<uint32_t>(hr)) << 8) |
                         (ours ? 1ull << 40 : 0u);
    if (!capAllow(g_fullscreen, key)) return;
    char where[96], mode[16];
    callerText(ret, where, sizeof(where));
    modeText(mode, sizeof(mode));
    Log::get().note("vr display: SetFullscreenState(%s, target %s) from %s -> hr 0x%08lX; %s swap chain; frame %u; "
                    "StereoscopicMode %s",
                    fullscreen ? "TRUE" : "FALSE", targetGiven ? "given" : "null", where, static_cast<unsigned long>(hr),
                    ours ? "the game's" : "another", vrSsaaGateFrame(), mode);
}

void vrDisplayNoteResizeTarget(uint32_t width, uint32_t height, uint32_t refreshNum, uint32_t refreshDen, uint32_t format,
                               uint32_t scaling, void* ret, long hr, bool ours) {
    const uint64_t key = (static_cast<uint64_t>(width) * 131u + height) * 1000003ull + refreshNum * 7919ull + refreshDen * 104729ull +
                         format * 31ull + scaling * 17ull + static_cast<uint64_t>(static_cast<uint32_t>(hr)) * 3ull +
                         (ours ? 1ull << 50 : 0u);
    if (!capAllow(g_resizeTarget, key)) return;
    char where[96], mode[16];
    callerText(ret, where, sizeof(where));
    modeText(mode, sizeof(mode));
    Log::get().note("vr display: ResizeTarget %ux%u refresh %u/%u format %u scaling %u from %s -> hr 0x%08lX; %s swap chain; "
                    "frame %u; StereoscopicMode %s",
                    width, height, refreshNum, refreshDen, format, scaling, where, static_cast<unsigned long>(hr),
                    ours ? "the game's" : "another", vrSsaaGateFrame(), mode);
}

void vrDisplayNoteResizeBuffers(const char* api, uint32_t width, uint32_t height, uint32_t format, uint32_t flags, void* ret,
                                long hr, bool ours) {
    const uint64_t key = (static_cast<uint64_t>(width) * 131u + height) * 1000003ull + format * 31ull + flags * 17ull +
                         static_cast<uint64_t>(static_cast<uint32_t>(hr)) * 3ull + (ours ? 1ull << 50 : 0u);
    if (!capAllow(g_resizeBuffers, key)) return;
    char where[96];
    callerText(ret, where, sizeof(where));
    Log::get().note("vr display: %s %ux%u format %u flags 0x%X from %s -> hr 0x%08lX; %s swap chain; frame %u",
                    api ? api : "ResizeBuffers", width, height, format, flags, where, static_cast<unsigned long>(hr),
                    ours ? "the game's" : "another", vrSsaaGateFrame());
}

}  // namespace edvr

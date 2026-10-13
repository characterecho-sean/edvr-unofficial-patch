// The game window's state (vr_display_observer.h says what). Read only.
#include "vr_display_observer.h"

#include "vr_ssaa_gate.h"  // vrSsaaGateFrame: the frame number the line carries
#include "mode_watch.h"    // modeWatchHint: a window change is a hint for the 3D mode watch

#include "../common/log.h"

#include <windows.h>

#include <atomic>

namespace edvr {
namespace {

// The window: set once at hookSwapChain. The frame check runs on the render thread only, after the store.
std::atomic<void*> g_hwnd{nullptr};
std::atomic<bool> g_windowSeen{false};
LONG_PTR g_style = 0;
LONG g_cw = 0, g_ch = 0;

}  // namespace

void vrDisplayObserveWindow(void* hwnd) {
    g_windowSeen.store(false, std::memory_order_release);
    g_hwnd.store(hwnd, std::memory_order_release);
}

void* vrDisplayGameWindow() { return g_hwnd.load(std::memory_order_acquire); }

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
    // A change, not the first look: the 3D mode watch re-reads Settings.xml now (mode_watch.h).
    if (seen) modeWatchHint(ModeHint::Window);
    g_style = style;
    g_cw = cw;
    g_ch = ch;
    g_windowSeen.store(true, std::memory_order_release);
}

}  // namespace edvr

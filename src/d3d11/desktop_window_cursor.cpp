// visual.desktop_window, round 3: see desktop_window_cursor.h for what this is for and what it changes (ClipCursor with a rectangle, while the window is active, and nothing else).
#include "desktop_window_cursor.h"

#include <atomic>
#include <cstdint>
#include <cstdio>

#include "../common/desktop_window_math.h"
#include "../common/iat_hook.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"

namespace edvr {
namespace {

using ClipFn = BOOL(WINAPI*)(const RECT*);
using SetPosFn = BOOL(WINAPI*)(int, int);
using GetPosFn = BOOL(WINAPI*)(LPPOINT);
using ShowFn = int(WINAPI*)(BOOL);

// Set while an activation is committed. Read by every hook on any thread.
std::atomic<bool> g_active{false};
// The client centre in screen coordinates, published by the render thread: bit 63 valid, then x and y as 31-bit offsets of 2^30 (screen coordinates can be negative).
std::atomic<uint64_t> g_centre{0};
constexpr uint64_t kCentreValid = 1ull << 63;
constexpr int kCentreBias = 1 << 30;

// This window's counters (the 10 s line takes and resets them).
std::atomic<uint32_t> g_clipReleased{0}, g_clipPassed{0}, g_setPos{0}, g_setPosCentre{0}, g_getPos{0}, g_showCalls{0};
std::atomic<int32_t> g_lastRect[4] = {};   // width, height, x, y of the last rectangle released
std::atomic<bool> g_haveRect{false};
std::atomic<int32_t> g_lastShow{-1};       // -1 none, 0 hide, 1 show
std::atomic<int32_t> g_lastDisplay{0};     // the display count that call returned

// The real functions: what each slot held before the patch (so a tool ahead of us in the chain is still called), or user32's own until then.
ClipFn g_realClip = nullptr;
SetPosFn g_realSetPos = nullptr;
GetPosFn g_realGetPos = nullptr;
ShowFn g_realShow = nullptr;
IatPatch g_patchClip, g_patchSetPos, g_patchGetPos, g_patchShow;
bool g_installTried = false;

BOOL WINAPI hookClipCursor(const RECT* rect) {
    switch (desktopWindowClipVerdict(g_active.load(std::memory_order_relaxed), rect != nullptr)) {
        case DwClipVerdict::Release:
            // The game asked to confine the cursor to a rectangle while the desktop window is up: it is let go instead, and the game is told it worked.
            g_lastRect[0].store(rect->right - rect->left, std::memory_order_relaxed);
            g_lastRect[1].store(rect->bottom - rect->top, std::memory_order_relaxed);
            g_lastRect[2].store(rect->left, std::memory_order_relaxed);
            g_lastRect[3].store(rect->top, std::memory_order_relaxed);
            g_haveRect.store(true, std::memory_order_relaxed);
            g_clipReleased.fetch_add(1, std::memory_order_relaxed);
            g_realClip(nullptr);
            return TRUE;
        case DwClipVerdict::PassCounted:
            g_clipPassed.fetch_add(1, std::memory_order_relaxed);
            return g_realClip(rect);
        default:
            return g_realClip(rect);
    }
}

BOOL WINAPI hookSetCursorPos(int x, int y) {
    if (g_active.load(std::memory_order_relaxed)) {   // counted only
        g_setPos.fetch_add(1, std::memory_order_relaxed);
        const uint64_t c = g_centre.load(std::memory_order_relaxed);
        if ((c & kCentreValid) &&
            desktopWindowAtCentre(x, y, static_cast<int>((c >> 31) & 0x7FFFFFFFu) - kCentreBias, static_cast<int>(c & 0x7FFFFFFFu) - kCentreBias))
            g_setPosCentre.fetch_add(1, std::memory_order_relaxed);
    }
    return g_realSetPos(x, y);
}

BOOL WINAPI hookGetCursorPos(LPPOINT point) {
    if (g_active.load(std::memory_order_relaxed)) g_getPos.fetch_add(1, std::memory_order_relaxed);   // counted only
    return g_realGetPos(point);
}

int WINAPI hookShowCursor(BOOL show) {
    const int count = g_realShow(show);
    if (g_active.load(std::memory_order_relaxed)) {   // counted only
        g_showCalls.fetch_add(1, std::memory_order_relaxed);
        g_lastShow.store(show ? 1 : 0, std::memory_order_relaxed);
        g_lastDisplay.store(count, std::memory_order_relaxed);
    }
    return count;
}

template <class F>
F userFunction(const char* name) {
    const HMODULE user = GetModuleHandleW(L"user32.dll");
    return user ? reinterpret_cast<F>(GetProcAddress(user, name)) : nullptr;
}

}  // namespace

void desktopWindowCursorInstall() {
    if (g_installTried) return;
    g_installTried = true;
    // user32's own until the patch says what the slot held: a call that reaches a hook in between forwards to the same function.
    g_realClip = userFunction<ClipFn>("ClipCursor");
    g_realSetPos = userFunction<SetPosFn>("SetCursorPos");
    g_realGetPos = userFunction<GetPosFn>("GetCursorPos");
    g_realShow = userFunction<ShowFn>("ShowCursor");
    if (!g_realClip || !g_realSetPos || !g_realGetPos || !g_realShow) {
        Log::get().note("desktop window: cursor hooks not installed -- user32.dll did not give ClipCursor, SetCursorPos, GetCursorPos and ShowCursor");
        return;
    }
    if (iatHookInstall("user32.dll", "ClipCursor", reinterpret_cast<void*>(&hookClipCursor), &g_patchClip)) g_realClip = reinterpret_cast<ClipFn>(g_patchClip.original);
    if (iatHookInstall("user32.dll", "SetCursorPos", reinterpret_cast<void*>(&hookSetCursorPos), &g_patchSetPos)) g_realSetPos = reinterpret_cast<SetPosFn>(g_patchSetPos.original);
    if (iatHookInstall("user32.dll", "GetCursorPos", reinterpret_cast<void*>(&hookGetCursorPos), &g_patchGetPos)) g_realGetPos = reinterpret_cast<GetPosFn>(g_patchGetPos.original);
    if (iatHookInstall("user32.dll", "ShowCursor", reinterpret_cast<void*>(&hookShowCursor), &g_patchShow)) g_realShow = reinterpret_cast<ShowFn>(g_patchShow.original);
    Log::get().note("desktop window: cursor hooks in the executable's import table -- ClipCursor %s, SetCursorPos %s, GetCursorPos %s, ShowCursor %s (they pass through; "
                    "only ClipCursor with a rectangle is changed, and only while the desktop window is active)",
                    g_patchClip.applied ? "patched" : "NOT FOUND", g_patchSetPos.applied ? "patched" : "NOT FOUND", g_patchGetPos.applied ? "patched" : "NOT FOUND",
                    g_patchShow.applied ? "patched" : "NOT FOUND");
}

void desktopWindowCursorUninstall() {
    g_active.store(false, std::memory_order_relaxed);
    iatHookUninstall(&g_patchClip);
    iatHookUninstall(&g_patchSetPos);
    iatHookUninstall(&g_patchGetPos);
    iatHookUninstall(&g_patchShow);
}

void desktopWindowCursorSetActive(bool active) {
    const bool was = g_active.exchange(active, std::memory_order_relaxed);
    if (active && !was) {
        // The clip Elite already holds goes at the start of the activation (ClipCursor sends no window message, so this is safe in the Present hook). Elite may set it
        // again; the hook then releases that, and the line says how often.
        ClipCursor(nullptr);
        g_centre.store(0, std::memory_order_relaxed);
    }
    if (!active) g_centre.store(0, std::memory_order_relaxed);
}

void desktopWindowCursorPublishCentre(int x, int y) {
    g_centre.store(kCentreValid | (static_cast<uint64_t>(static_cast<uint32_t>(x + kCentreBias) & 0x7FFFFFFFu) << 31) | (static_cast<uint32_t>(y + kCentreBias) & 0x7FFFFFFFu),
                   std::memory_order_relaxed);
}

int desktopWindowCursorTakeLine(char* out, size_t size) {
    const unsigned released = g_clipReleased.exchange(0, std::memory_order_relaxed);
    const unsigned passed = g_clipPassed.exchange(0, std::memory_order_relaxed);
    const unsigned setPos = g_setPos.exchange(0, std::memory_order_relaxed);
    const unsigned setPosCentre = g_setPosCentre.exchange(0, std::memory_order_relaxed);
    const unsigned getPos = g_getPos.exchange(0, std::memory_order_relaxed);
    const unsigned showCalls = g_showCalls.exchange(0, std::memory_order_relaxed);
    char rect[64] = "none";
    if (g_haveRect.exchange(false, std::memory_order_relaxed))
        std::snprintf(rect, sizeof rect, "%dx%d at %d,%d", g_lastRect[0].load(std::memory_order_relaxed), g_lastRect[1].load(std::memory_order_relaxed),
                      g_lastRect[2].load(std::memory_order_relaxed), g_lastRect[3].load(std::memory_order_relaxed));
    char show[64] = "none";
    const int lastShow = g_lastShow.exchange(-1, std::memory_order_relaxed);
    if (lastShow >= 0) std::snprintf(show, sizeof show, "%s, display count %d", lastShow ? "show" : "hide", g_lastDisplay.load(std::memory_order_relaxed));
    return desktopWindowCursorLine(out, size, released, rect, passed, setPos, setPosCentre, getPos, showCalls, show);
}

void desktopWindowNoteMouseCooperative(DWORD flags, long hr) {
    if (runtimeFlatProfile()) return;   // the feature is VR's, and the flat profile says nothing about it
    static std::atomic<DWORD> seen[4] = {};
    static std::atomic<unsigned> count{0};
    const unsigned n = count.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < n && i < 4; ++i) {
        if (seen[i].load(std::memory_order_relaxed) == flags) return;
    }
    if (n >= 4) return;
    seen[n].store(flags, std::memory_order_relaxed);
    count.store(n + 1, std::memory_order_relaxed);
    char words[96];
    desktopWindowCooperativeWords(words, sizeof words, flags);
    Log::get().note("desktop window: the game's DirectInput mouse cooperative level 0x%08X (%s; the call returned 0x%08lX)", flags, words, static_cast<unsigned long>(hr));
}

}  // namespace edvr

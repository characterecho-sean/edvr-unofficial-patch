// visual.desktop_window, the Windows cursor: see desktop_window_cursor.h for what this is for and what it changes (ShowCursor's count while the cursor is hidden, and nothing else).
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
std::atomic<uint32_t> g_setPos{0}, g_setPosCentre{0}, g_getPos{0}, g_showCalls{0};
std::atomic<int32_t> g_lastShow{-1};       // -1 none, 0 hide, 1 show
std::atomic<int32_t> g_lastDisplay{0};     // the display count that call returned

// The real functions: what each slot held before the patch (so a tool ahead of us in the chain is still called), or user32's own until then.
SetPosFn g_realSetPos = nullptr;
GetPosFn g_realGetPos = nullptr;
ShowFn g_realShow = nullptr;
IatPatch g_patchSetPos, g_patchGetPos, g_patchShow;
bool g_installTried = false;

// The hide (round 4). The wanted state is decided by the render thread at each Present and read by the ShowCursor hook, which acts only on the thread that owns the window
// (g_hideThread, set by the Present hook when it runs on it), so g_count is touched by that one thread only.
std::atomic<bool> g_hideWanted{false};
std::atomic<DWORD> g_hideThread{0};
DesktopWindowShowCount g_count;
std::atomic<uint32_t> g_hiddenPresents{0}, g_decidedPresents{0};
bool g_noteThread = false, g_noteHook = false;   // said once; the render thread

int realShow(bool visible) { return g_realShow(visible ? TRUE : FALSE); }

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
    const int real = g_realShow(show);
    int count = real;
    // On the window's thread only: while the cursor is wanted hidden the count is pushed below 0 again, and the game is given the count it would have seen without EDVR.
    if (g_hideThread.load(std::memory_order_relaxed) == GetCurrentThreadId())
        count = g_count.afterGameCall(real, g_hideWanted.load(std::memory_order_relaxed), realShow);
    if (g_active.load(std::memory_order_relaxed)) {   // counted
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
    g_realSetPos = userFunction<SetPosFn>("SetCursorPos");
    g_realGetPos = userFunction<GetPosFn>("GetCursorPos");
    g_realShow = userFunction<ShowFn>("ShowCursor");
    if (!g_realSetPos || !g_realGetPos || !g_realShow) {
        Log::get().note("desktop window: cursor hooks not installed -- user32.dll did not give SetCursorPos, GetCursorPos and ShowCursor");
        return;
    }
    if (iatHookInstall("user32.dll", "SetCursorPos", reinterpret_cast<void*>(&hookSetCursorPos), &g_patchSetPos)) g_realSetPos = reinterpret_cast<SetPosFn>(g_patchSetPos.original);
    if (iatHookInstall("user32.dll", "GetCursorPos", reinterpret_cast<void*>(&hookGetCursorPos), &g_patchGetPos)) g_realGetPos = reinterpret_cast<GetPosFn>(g_patchGetPos.original);
    if (iatHookInstall("user32.dll", "ShowCursor", reinterpret_cast<void*>(&hookShowCursor), &g_patchShow)) g_realShow = reinterpret_cast<ShowFn>(g_patchShow.original);
    Log::get().note("desktop window: cursor hooks in the executable's import table -- SetCursorPos %s, GetCursorPos %s, ShowCursor %s (they pass through; "
                    "only ShowCursor's count is changed, and only while the cursor is hidden)",
                    g_patchSetPos.applied ? "patched" : "NOT FOUND", g_patchGetPos.applied ? "patched" : "NOT FOUND", g_patchShow.applied ? "patched" : "NOT FOUND");
}

void desktopWindowCursorUninstall() {
    g_active.store(false, std::memory_order_relaxed);
    g_hideWanted.store(false, std::memory_order_relaxed);
    if (g_hideThread.load(std::memory_order_relaxed) == GetCurrentThreadId() && g_realShow) g_count.unhide(realShow);   // give back the hides, if this is the thread that made them
    iatHookUninstall(&g_patchSetPos);
    iatHookUninstall(&g_patchGetPos);
    iatHookUninstall(&g_patchShow);
}

void desktopWindowCursorSetActive(bool active) {
    const bool was = g_active.exchange(active, std::memory_order_relaxed);
    if (active && !was) g_centre.store(0, std::memory_order_relaxed);   // the centre is published from the first Present on
    if (!active) {
        g_centre.store(0, std::memory_order_relaxed);
        g_hideWanted.store(false, std::memory_order_relaxed);
        if (g_realShow) g_count.unhide(realShow);   // the cursor comes back: every hide EDVR made is given back (a no-op at offset 0; the offset exists on the Present thread only)
    }
}

void desktopWindowCursorHide(HWND hwnd) {
    if (!g_active.load(std::memory_order_relaxed) || !hwnd) return;
    g_decidedPresents.fetch_add(1, std::memory_order_relaxed);
    const DWORD here = GetCurrentThreadId(), owner = GetWindowThreadProcessId(hwnd, nullptr);
    if (owner != here) {   // the show count is the window's thread's: from another thread this would hide nothing, or the wrong thread's cursor
        g_hideWanted.store(false, std::memory_order_relaxed);
        if (!g_noteThread) {
            g_noteThread = true;
            Log::get().note("desktop window: the cursor is not hidden -- the window belongs to thread %lu, Present runs on %lu", static_cast<unsigned long>(owner),
                            static_cast<unsigned long>(here));
        }
        return;
    }
    if (!g_patchShow.applied) {   // without the ShowCursor hook the game's own calls would show it again between Presents
        g_hideWanted.store(false, std::memory_order_relaxed);
        if (!g_noteHook) {
            g_noteHook = true;
            Log::get().note("desktop window: the cursor is not hidden -- the ShowCursor hook is not installed");
        }
        return;
    }
    g_hideThread.store(here, std::memory_order_relaxed);
    // EDVR's own calls, through EDVR's own import table (the patches are in the executable's): the cursor in screen coordinates against the client area in screen coordinates.
    POINT cursor{}, origin{0, 0};
    RECT client{};
    const bool inside = GetCursorPos(&cursor) && GetClientRect(hwnd, &client) && ClientToScreen(hwnd, &origin) &&
                        desktopWindowPointInRect(cursor.x, cursor.y, origin.x, origin.y, origin.x + client.right, origin.y + client.bottom);
    const bool hide = desktopWindowHideCursor(true, GetForegroundWindow() == hwnd, inside);
    g_hideWanted.store(hide, std::memory_order_relaxed);
    if (hide) {
        g_hiddenPresents.fetch_add(1, std::memory_order_relaxed);
        g_count.hide(realShow);
    } else {
        g_count.unhide(realShow);
    }
}

void desktopWindowCursorPublishCentre(int x, int y) {
    g_centre.store(kCentreValid | (static_cast<uint64_t>(static_cast<uint32_t>(x + kCentreBias) & 0x7FFFFFFFu) << 31) | (static_cast<uint32_t>(y + kCentreBias) & 0x7FFFFFFFu),
                   std::memory_order_relaxed);
}

int desktopWindowCursorTakeLine(char* out, size_t size) {
    const unsigned setPos = g_setPos.exchange(0, std::memory_order_relaxed);
    const unsigned setPosCentre = g_setPosCentre.exchange(0, std::memory_order_relaxed);
    const unsigned getPos = g_getPos.exchange(0, std::memory_order_relaxed);
    const unsigned showCalls = g_showCalls.exchange(0, std::memory_order_relaxed);
    char show[64] = "none";
    const int lastShow = g_lastShow.exchange(-1, std::memory_order_relaxed);
    if (lastShow >= 0) std::snprintf(show, sizeof show, "%s, display count %d", lastShow ? "show" : "hide", g_lastDisplay.load(std::memory_order_relaxed));
    const unsigned hidden = g_hiddenPresents.exchange(0, std::memory_order_relaxed), decided = g_decidedPresents.exchange(0, std::memory_order_relaxed);
    return desktopWindowCursorLine(out, size, setPos, setPosCentre, getPos, showCalls, show, hidden, decided, g_count.offset(), g_count.takePushed());
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

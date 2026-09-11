#include "input_gate.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <utility>

#include <windows.h>

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/hotkey.h"
#include "../common/iat_hook.h"
#include "../common/log.h"
#include "../common/timing.h"
#include "../common/vtable_hook.h"

namespace edvr {
namespace {

// dinput.h declares these GUIDs extern and expects dxguid.lib to define
// them. Spelled out here instead -- the values are the public ones -- so the
// DLL links against nothing new.
const GUID kIidDirectInput8A = {0xBF798030, 0x483A, 0x4DA2,
                                {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
const GUID kIidDirectInput8W = {0xBF798031, 0x483A, 0x4DA2,
                                {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
const GUID kGuidSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF,
                               {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

// IDirectInputDevice8's vtable, counted from dinput.h's declaration order:
// IUnknown 0-2, GetCapabilities 3, EnumObjects 4, GetProperty 5,
// SetProperty 6, Acquire 7, Unacquire 8, GetDeviceState 9, GetDeviceData
// 10, SetDataFormat 11, SetEventNotification 12, SetCooperativeLevel 13,
// GetObjectInfo 14, GetDeviceInfo 15.
constexpr size_t kSlotGetDeviceState = 9;
constexpr size_t kSlotGetDeviceData = 10;
constexpr size_t kSlotGetDeviceInfo = 15;

typedef HRESULT(WINAPI* PFN_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDevice)(void*, REFGUID, void**, LPUNKNOWN);
typedef HRESULT(STDMETHODCALLTYPE* PFN_GetDeviceState)(void*, DWORD, LPVOID);
typedef HRESULT(STDMETHODCALLTYPE* PFN_GetDeviceData)(void*, DWORD, LPDIDEVICEOBJECTDATA,
                                                      LPDWORD, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_GetDeviceInfoW)(void*, LPDIDEVICEINSTANCEW);
typedef HRESULT(STDMETHODCALLTYPE* PFN_GetDeviceInfoA)(void*, LPDIDEVICEINSTANCEA);

typedef SHORT(WINAPI* PFN_GetAsyncKeyState)(int);
typedef SHORT(WINAPI* PFN_GetKeyState)(int);
typedef BOOL(WINAPI* PFN_GetKeyboardState)(PBYTE);
typedef BOOL(WINAPI* PFN_PeekMessageA)(LPMSG, HWND, UINT, UINT, UINT);

// The flag every door consults. Relaxed loads are enough: a door that
// reads the old value for one call more is one more call of the state the
// player was already in.
std::atomic<int> g_private{0};

// The summon key, packed so a live rebind cannot tear it: bits 0-7 the
// virtual key, 8-15 the DirectInput scan code, 16-18 the modifier bits,
// bit 31 valid.
std::atomic<uint32_t> g_summon{0};

inline void unpackSummon(uint32_t p, int* vk, uint8_t* dik, uint32_t* mods) {
    *vk = static_cast<int>(p & 0xFFu);
    *dik = static_cast<uint8_t>((p >> 8) & 0xFFu);
    *mods = (p >> 16) & 0x7u;
}

// One DirectInput door: a table (the A or the W interface's), its hook,
// the originals, and the probe's counters.
struct DiDoor {
    const char* name = "";
    void*      dummy = nullptr;      // our own keyboard device, held for the session
    VTableHook hook;
    PFN_GetDeviceState origState = nullptr;
    PFN_GetDeviceData  origData = nullptr;
    std::atomic<bool> installed{false};
    bool  retired = false;           // a fault took it out for the session
    bool  gameDevice = false;        // observed at the game's CreateDevice return
    // Counters, relaxed: evidence for the probe and the reclaim vouch.
    std::atomic<uint32_t> stateCalls{0};
    std::atomic<uint32_t> stateForeign{0};   // a `this` that is not our dummy
    std::atomic<uint32_t> stateKeyboard{0};
    std::atomic<uint32_t> dataCalls{0};
    std::atomic<uint32_t> dataKeyboard{0};
    std::atomic<uint32_t> swallowed{0};
    std::atomic<uint32_t> zeroed{0};
    uint32_t stateSeen = 0;          // for the reclaim vouch
    uint32_t dataSeen = 0;
    uint32_t quietSeconds = 0;
    // Where the patched table lives and where its GetDeviceState pointed
    // before the patch, for the blindness line below (2026-09-08).
    char tableModule[64] = "?";
    char entryModule[64] = "?";
};
DiDoor g_diA;
DiDoor g_diW;

// A wrapper can give each device a different table. Capture the returned
// objects before Elite receives them, and keep one reference per patched
// table so both the table and our restore target stay alive until shutdown.
constexpr size_t kGameDeviceDoors = 16;
constexpr size_t kFactoryDoors = 8;
DiDoor g_gameDi[kGameDeviceDoors];
struct FactoryDoor {
    void* owner = nullptr;
    VTableHook hook;
    PFN_CreateDevice original = nullptr;
};
FactoryDoor g_factories[kFactoryDoors];
SRWLOCK g_captureLock = SRWLOCK_INIT;
IatPatch g_createImport;
std::atomic<uint64_t> g_gameKeyboardCalls{0};

// Closing with Escape (or another key still held) must not turn that same
// press into an Elite action. Only keys held at close wait for release;
// a fresh press after release works normally. EDVR reads its own imports.
std::atomic<bool> g_releaseTail{false};
std::atomic<bool> g_heldVk[256]{};
std::atomic<bool> g_heldDik[256]{};

void captureReleaseTail(PFN_GetAsyncKeyState readKey) {
    for (int k = 0; k < 256; ++k) g_heldDik[k].store(false);
    bool any = false;
    for (int vk = 0; vk < 256; ++vk) {
        const bool down = vk > VK_XBUTTON2 && (readKey(vk) & 0x8000) != 0;
        g_heldVk[vk].store(down);
        if (down) {
            any = true;
            const uint8_t dik = inputGateDikOf(vk);
            if (dik) g_heldDik[dik].store(true);
        }
    }
    g_releaseTail.store(any);
}

void refreshReleaseTail(PFN_GetAsyncKeyState readKey) {
    if (!g_releaseTail.load()) return;
    bool any = false;
    for (int vk = 0; vk < 256; ++vk) {
        if (!g_heldVk[vk].load()) continue;
        if (readKey(vk) & 0x8000) { any = true; continue; }
        g_heldVk[vk].store(false);
    }
    // Rebuild after releases: generic/left/right modifier VKs can share a DIK.
    bool held[256]{};
    for (int vk = 0; vk < 256; ++vk) if (g_heldVk[vk].load()) {
        const uint8_t dik = inputGateDikOf(vk);
        if (dik) held[dik] = true;
    }
    for (int k = 0; k < 256; ++k) g_heldDik[k].store(held[k]);
    g_releaseTail.store(any);
}

bool releaseTailVk(int vk) {
    return g_releaseTail.load(std::memory_order_relaxed) && vk >= 0 && vk < 256 &&
           g_heldVk[vk].load(std::memory_order_relaxed);
}

// The door's blindness, said once. The DirectInput door patches OUR dummy
// device's table and reaches the game's device only if that table is the
// shared one. On the Steam copy (2026-09-08) the entries pointed into
// gameoverlayrenderer64.dll before the patch: the overlay wraps devices,
// and if it hands each one a private copy of the table, the door sits on
// our copy alone -- the menu reports "keys private" while Tab boosts the
// ship. Counted per frame while the keys are private; the verdict prints
// once the menu has had them for over a second with no keyboard but our
// own having reached the door.
uint32_t g_privateTicks = 0;
bool     g_unreachedNoted = false;
constexpr uint32_t kUnreachedAfterTicks = 120;

struct UserDoor {
    IatPatch asyncKey;
    IatPatch keyState;
    IatPatch keyboardState;
    IatPatch peek;
    PFN_GetAsyncKeyState origAsync = nullptr;
    PFN_GetKeyState origKeyState = nullptr;
    PFN_GetKeyboardState origKeyboardState = nullptr;
    PFN_PeekMessageA origPeek = nullptr;
    bool retired2 = false;   // door 2 (the trio)
    bool retired3 = false;   // door 3 (the pump)
    std::atomic<uint32_t> asyncCalls{0};
    std::atomic<uint32_t> keyStateCalls{0};
    std::atomic<uint32_t> keyboardStateCalls{0};
    std::atomic<uint32_t> peekKeyMessages{0};
    std::atomic<uint32_t> peekInputMessages{0};   // WM_INPUT: Raw Input after all
    std::atomic<uint32_t> nulled{0};
    std::atomic<uint32_t> swallowed{0};
};
UserDoor g_user;

bool g_installTried = false;
bool g_probe = false;
bool g_privateWanted = true;   // menu.keyboard = private
uint64_t g_probeMs = 0;
uint64_t g_reclaimMs = 0;
constexpr uint64_t kProbeEveryMs = 5000;
constexpr uint64_t kReclaimEveryMs = 1000;

FaultBudget g_budgetDi("inputGate.dinput", 4);
FaultBudget g_budgetUser("inputGate.user32", 4);
FaultBudget g_budgetPump("inputGate.pump", 4);

// Which modifiers a 256-byte DirectInput state says are down.
uint32_t modsInState(const uint8_t* st) {
    uint32_t m = 0;
    if ((st[DIK_LCONTROL] | st[DIK_RCONTROL]) & 0x80) m |= kHotkeyCtrl;
    if ((st[DIK_LMENU] | st[DIK_RMENU]) & 0x80) m |= kHotkeyAlt;
    if ((st[DIK_LSHIFT] | st[DIK_RSHIFT]) & 0x80) m |= kHotkeyShift;
    return m;
}

// Which modifiers Windows says are down, read through OUR import (never
// the game's patched slot).
uint32_t modsNow() {
    uint32_t m = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= kHotkeyCtrl;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) m |= kHotkeyAlt;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) m |= kHotkeyShift;
    return m;
}

bool summonModsHeldNow(uint32_t mods) { return (mods & ~modsNow()) == 0; }

// GetCapabilities has the same layout on A and W. The retained keyboard
// is known; other devices sharing its table are checked without caching a
// raw pointer that a released device could reuse for a joystick later.
template <bool Wide>
bool isKeyboard(DiDoor& d, void* self) {
    if (self == d.dummy) return true;
    void** vt = *reinterpret_cast<void***>(self);
    if (!vt || !vt[3]) return false;
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    typedef HRESULT(STDMETHODCALLTYPE* GetCaps)(void*, LPDIDEVCAPS);
    return SUCCEEDED(reinterpret_cast<GetCaps>(vt[3])(self, &caps)) &&
           GET_DIDEVICE_TYPE(caps.dwDevType) == DI8DEVTYPE_KEYBOARD;
}

template <bool Wide>
HRESULT filterDeviceState(DiDoor& d, void* self, DWORD cb, LPVOID data) {
    const HRESULT hr = d.origState(self, cb, data);
    d.stateCalls.fetch_add(1, std::memory_order_relaxed);
    if (self != d.dummy) d.stateForeign.fetch_add(1, std::memory_order_relaxed);
    if (d.retired || FAILED(hr) || !data) return hr;
    guardedBudget(g_budgetDi, [&] {
        if (!isKeyboard<Wide>(d, self)) return;
        d.stateKeyboard.fetch_add(1, std::memory_order_relaxed);
        if (d.gameDevice) g_gameKeyboardCalls.fetch_add(1, std::memory_order_relaxed);
        const bool priv = g_private.load(std::memory_order_relaxed) != 0;
        int vk = 0;
        uint8_t dik = 0;
        uint32_t mods = 0;
        const uint32_t packed = g_summon.load(std::memory_order_relaxed);
        if (packed & 0x80000000u) unpackSummon(packed, &vk, &dik, &mods);
        if (priv) {
            memset(data, 0, cb);
            d.zeroed.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // The swallow needs the standard 256-byte format to know where the
        // key lives; a custom format passes untouched.
        if (dik && cb == 256) {
            uint8_t* st = static_cast<uint8_t*>(data);
            if (st[dik] & 0x80) {
                inputGateFilterState(st, false, dik, mods);
                if (!(st[dik] & 0x80)) d.swallowed.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (cb == 256 && g_releaseTail.load(std::memory_order_relaxed)) {
            auto* st = static_cast<uint8_t*>(data);
            for (int k = 0; k < 256; ++k) if (g_heldDik[k].load()) st[k] = 0;
        }
    });
    if (!g_budgetDi.shouldRun() && !d.retired) {
        d.retired = true;
        Log::get().note("keyboard gate: the DirectInput door (%s) faulted repeatedly and "
                        "is pass-through for the rest of this session; keys are SHARED "
                        "with the game while the menu is open.",
                        d.name);
    }
    return hr;
}

template <bool Wide>
HRESULT filterDeviceData(DiDoor& d, void* self, DWORD cbObj, LPDIDEVICEOBJECTDATA rgdod,
                         LPDWORD inOut, DWORD flags) {
    const HRESULT hr = d.origData(self, cbObj, rgdod, inOut, flags);
    d.dataCalls.fetch_add(1, std::memory_order_relaxed);
    if (d.retired || FAILED(hr) || !rgdod || !inOut || *inOut == 0) return hr;
    if (cbObj != sizeof(DIDEVICEOBJECTDATA)) return hr;   // a layout this was not written for
    guardedBudget(g_budgetDi, [&] {
        if (!isKeyboard<Wide>(d, self)) return;
        d.dataKeyboard.fetch_add(1, std::memory_order_relaxed);
        if (d.gameDevice) g_gameKeyboardCalls.fetch_add(1, std::memory_order_relaxed);
        const bool priv = g_private.load(std::memory_order_relaxed) != 0;
        int vk = 0;
        uint8_t dik = 0;
        uint32_t mods = 0;
        const uint32_t packed = g_summon.load(std::memory_order_relaxed);
        if (packed & 0x80000000u) unpackSummon(packed, &vk, &dik, &mods);
        const bool swallow = dik != 0 && summonModsHeldNow(mods);
        if (!priv && !swallow && !g_releaseTail.load()) return;
        static_assert(sizeof(DiObjectData) == sizeof(DIDEVICEOBJECTDATA),
                      "DiObjectData mirrors DIDEVICEOBJECTDATA");
        const uint32_t before = *inOut;
        uint32_t kept = inputGateFilterData(reinterpret_cast<DiObjectData*>(rgdod),
                                                  before, priv, dik, swallow);
        if (g_releaseTail.load()) {
            uint32_t out = 0;
            for (uint32_t i = 0; i < kept; ++i) {
                const auto& event = rgdod[i];
                if ((event.dwData & 0x80) && event.dwOfs < 256 &&
                    g_heldDik[event.dwOfs].load()) continue;
                rgdod[out++] = event;
            }
            kept = out;
        }
        *inOut = kept;
        if (kept < before) {
            (priv ? d.zeroed : d.swallowed).fetch_add(before - kept, std::memory_order_relaxed);
        }
    });
    return hr;
}

template <DiDoor* Door, bool Wide>
HRESULT STDMETHODCALLTYPE hookGetDeviceState(void* self, DWORD cb, LPVOID data) {
    return filterDeviceState<Wide>(*Door, self, cb, data);
}

template <DiDoor* Door, bool Wide>
HRESULT STDMETHODCALLTYPE hookGetDeviceData(void* self, DWORD cb, LPDIDEVICEOBJECTDATA data,
                                            LPDWORD count, DWORD flags) {
    return filterDeviceData<Wide>(*Door, self, cb, data, count, flags);
}

SHORT WINAPI hookGetAsyncKeyState(int vk) {
    g_user.asyncCalls.fetch_add(1, std::memory_order_relaxed);
    if (!g_user.retired2) {
        if (g_private.load(std::memory_order_relaxed)) return 0;
        if (releaseTailVk(vk)) return 0;
        const uint32_t packed = g_summon.load(std::memory_order_relaxed);
        if (packed & 0x80000000u) {
            int svk = 0;
            uint8_t dik = 0;
            uint32_t mods = 0;
            unpackSummon(packed, &svk, &dik, &mods);
            if (vk == svk && summonModsHeldNow(mods)) {
                g_user.swallowed.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
        }
    }
    return g_user.origAsync(vk);
}

SHORT WINAPI hookGetKeyState(int vk) {
    g_user.keyStateCalls.fetch_add(1, std::memory_order_relaxed);
    if (!g_user.retired2) {
        if (g_private.load(std::memory_order_relaxed)) return 0;
        if (releaseTailVk(vk)) return 0;
        const uint32_t packed = g_summon.load(std::memory_order_relaxed);
        if (packed & 0x80000000u) {
            int svk = 0;
            uint8_t dik = 0;
            uint32_t mods = 0;
            unpackSummon(packed, &svk, &dik, &mods);
            if (vk == svk && summonModsHeldNow(mods)) {
                g_user.swallowed.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
        }
    }
    return g_user.origKeyState(vk);
}

BOOL WINAPI hookGetKeyboardState(PBYTE state) {
    const BOOL r = g_user.origKeyboardState(state);
    g_user.keyboardStateCalls.fetch_add(1, std::memory_order_relaxed);
    if (!r || !state || g_user.retired2) return r;
    guardedBudget(g_budgetUser, [&] {
        if (g_private.load(std::memory_order_relaxed)) {
            memset(state, 0, 256);
            return;
        }
        if (g_releaseTail.load()) {
            for (int vk = 0; vk < 256; ++vk) if (g_heldVk[vk].load()) state[vk] = 0;
        }
        const uint32_t packed = g_summon.load(std::memory_order_relaxed);
        if (packed & 0x80000000u) {
            int svk = 0;
            uint8_t dik = 0;
            uint32_t mods = 0;
            unpackSummon(packed, &svk, &dik, &mods);
            if (svk > 0 && svk < 256 && summonModsHeldNow(mods)) state[svk] = 0;
        }
    });
    if (!g_budgetUser.shouldRun() && !g_user.retired2) {
        g_user.retired2 = true;
        Log::get().note("keyboard gate: the key-state door faulted repeatedly and is "
                        "pass-through for the rest of this session.");
    }
    return r;
}

bool isKeyboardMessage(UINT m) {
    switch (m) {
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        case WM_CHAR: case WM_SYSCHAR: case WM_DEADCHAR: case WM_SYSDEADCHAR:
        case WM_UNICHAR:
            return true;
        default:
            return false;
    }
}

BOOL WINAPI hookPeekMessageA(LPMSG msg, HWND hwnd, UINT lo, UINT hi, UINT remove) {
    const BOOL r = g_user.origPeek(msg, hwnd, lo, hi, remove);
    if (!r || !msg || g_user.retired3) return r;
    guardedBudget(g_budgetPump, [&] {
        const UINT m = msg->message;
        if (m == WM_INPUT) {
            g_user.peekInputMessages.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!isKeyboardMessage(m)) return;
        g_user.peekKeyMessages.fetch_add(1, std::memory_order_relaxed);
        if (g_private.load(std::memory_order_relaxed)) {
            msg->message = WM_NULL;
            g_user.nulled.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (g_releaseTail.load()) {
            const bool key = m == WM_KEYDOWN || m == WM_KEYUP ||
                             m == WM_SYSKEYDOWN || m == WM_SYSKEYUP;
            const unsigned dik = ((msg->lParam >> 16) & 0x7f) |
                                 ((msg->lParam & (1 << 24)) ? 0x80 : 0);
            if ((key && releaseTailVk(static_cast<int>(msg->wParam))) ||
                (!key && g_heldDik[dik].load())) {
                msg->message = WM_NULL;
                g_user.nulled.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        const uint32_t packed = g_summon.load(std::memory_order_relaxed);
        if ((packed & 0x80000000u) &&
            (m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP)) {
            int svk = 0;
            uint8_t dik = 0;
            uint32_t mods = 0;
            unpackSummon(packed, &svk, &dik, &mods);
            if (static_cast<int>(msg->wParam) == svk && summonModsHeldNow(mods)) {
                msg->message = WM_NULL;
                g_user.swallowed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    if (!g_budgetPump.shouldRun() && !g_user.retired3) {
        g_user.retired3 = true;
        Log::get().note("keyboard gate: the message-pump door faulted repeatedly and is "
                        "pass-through for the rest of this session.");
    }
    return r;
}

template <size_t Index>
HRESULT STDMETHODCALLTYPE gameDeviceState(void* self, DWORD cb, LPVOID data) {
    return filterDeviceState<false>(g_gameDi[Index], self, cb, data);
}

template <size_t Index>
HRESULT STDMETHODCALLTYPE gameDeviceData(void* self, DWORD cb, LPDIDEVICEOBJECTDATA data,
                                         LPDWORD count, DWORD flags) {
    return filterDeviceData<false>(g_gameDi[Index], self, cb, data, count, flags);
}

struct CaptureLock {
    CaptureLock() { AcquireSRWLockExclusive(&g_captureLock); }
    ~CaptureLock() { ReleaseSRWLockExclusive(&g_captureLock); }
};

template <size_t... I>
void captureKeyboard(void* device, std::index_sequence<I...>) {
    DiDoor unknown;
    if (!isKeyboard<false>(unknown, device)) return;
    static const PFN_GetDeviceState stateHooks[] = {&gameDeviceState<I>...};
    static const PFN_GetDeviceData dataHooks[] = {&gameDeviceData<I>...};
    CaptureLock lock;
    void** table = *reinterpret_cast<void***>(device);
    // A shared system table may already have a fallback hook. Do not stack
    // another gate on it. Private overlay tables each get their own original.
    for (DiDoor* d : {&g_diW, &g_diA}) {
        if (d->installed && d->hook.originalVTable() == table) return;
    }
    for (auto& d : g_gameDi) {
        if (d.installed && d.hook.originalVTable() == table) return;
    }
    for (size_t i = 0; i < kGameDeviceDoors; ++i) {
        auto& d = g_gameDi[i];
        if (d.installed) continue;
        if (!d.hook.attach(device, 32) || d.hook.executablePrefix() <= kSlotGetDeviceData) {
            d.hook.uninstall();
            return;
        }
        d.name = "game keyboard";
        d.gameDevice = true;
        d.dummy = device;
        reinterpret_cast<IUnknown*>(device)->AddRef();
        d.hook.setMode(HookMode::InPlace);
        const bool staged =
            d.hook.replace(kSlotGetDeviceState, reinterpret_cast<void*>(stateHooks[i]),
                           reinterpret_cast<void**>(&d.origState)) &&
            d.hook.replace(kSlotGetDeviceData, reinterpret_cast<void*>(dataHooks[i]),
                           reinterpret_cast<void**>(&d.origData));
        if (!staged || !d.hook.commit()) {
            d.hook.uninstall();
            reinterpret_cast<IUnknown*>(device)->Release();
            d.dummy = nullptr;
            return;
        }
        d.installed = true;
        iatHookEntryModule(table, d.tableModule, sizeof(d.tableModule));
        iatHookEntryModule(reinterpret_cast<void*>(d.origState), d.entryModule,
                          sizeof(d.entryModule));
        Log::get().note("keyboard gate: captured the game's keyboard at CreateDevice; "
                        "table %zu in %s, forwarding through %s. Private overlay tables "
                        "are covered; the device's identity is unchanged.",
                        i, d.tableModule, d.entryModule);
        return;
    }
    Log::get().note("keyboard gate: all %zu keyboard tables are occupied; a new table "
                    "is left untouched. Please report this log.", kGameDeviceDoors);
}

template <size_t Index>
HRESULT STDMETHODCALLTYPE factoryCreateDevice(void* self, REFGUID guid, void** device,
                                               LPUNKNOWN outer) {
    const HRESULT hr = g_factories[Index].original(self, guid, device, outer);
    if (SUCCEEDED(hr) && device && *device && !outer) {
        guardedBudget(g_budgetDi, [&] {
            captureKeyboard(*device, std::make_index_sequence<kGameDeviceDoors>{});
        });
    }
    return hr;
}

template <size_t... I>
void captureFactory(void* factory, std::index_sequence<I...>) {
    static const PFN_CreateDevice hooks[] = {&factoryCreateDevice<I>...};
    CaptureLock lock;
    void** table = *reinterpret_cast<void***>(factory);
    for (const auto& f : g_factories) {
        if (f.owner && f.hook.originalVTable() == table) return;
    }
    for (size_t i = 0; i < kFactoryDoors; ++i) {
        auto& f = g_factories[i];
        if (f.owner) continue;
        if (!f.hook.attach(factory, 11) || f.hook.executablePrefix() <= 3) {
            f.hook.uninstall();
            return;
        }
        f.owner = factory;
        reinterpret_cast<IUnknown*>(factory)->AddRef();
        f.hook.setMode(HookMode::InPlace);
        if (!f.hook.replace(3, reinterpret_cast<void*>(hooks[i]),
                            reinterpret_cast<void**>(&f.original)) || !f.hook.commit()) {
            f.hook.uninstall();
            f.owner = nullptr;
            reinterpret_cast<IUnknown*>(factory)->Release();
        }
        return;
    }
    Log::get().note("keyboard gate: all %zu DirectInput factory tables are occupied; "
                    "a new factory is left untouched.", kFactoryDoors);
}

HRESULT WINAPI hookDirectInput8Create(HINSTANCE instance, DWORD version, REFIID iid,
                                      LPVOID* out, LPUNKNOWN outer) {
    const auto original = reinterpret_cast<PFN_DirectInput8Create>(g_createImport.original);
    const HRESULT hr = original(instance, version, iid, out, outer);
    if (SUCCEEDED(hr) && out && *out && !outer &&
        (IsEqualGUID(iid, kIidDirectInput8A) || IsEqualGUID(iid, kIidDirectInput8W))) {
        guardedBudget(g_budgetDi, [&] {
            captureFactory(*out, std::make_index_sequence<kFactoryDoors>{});
        });
    }
    return hr;
}

// Fallback for a runtime without an observed factory: make a dummy device
// and patch the shared table, preserving the path that works without overlays.
template <DiDoor* Door, bool Wide>
void installDiDoor(PFN_DirectInput8Create create, HINSTANCE inst, const GUID& iid,
                   const char* name, void*** sharedTableOut) {
    DiDoor& d = *Door;
    d.name = name;
    void* di = nullptr;
    HRESULT hr = create(inst, DIRECTINPUT_VERSION, iid, &di, nullptr);
    if (FAILED(hr) || !di) {
        Log::get().note("keyboard gate: DirectInput8Create (%s) refused (0x%08lX); that door "
                        "is not installed.",
                        name, static_cast<unsigned long>(hr));
        return;
    }
    // IDirectInput8::CreateDevice is slot 3 on both interfaces.
    typedef HRESULT(STDMETHODCALLTYPE * PFN_CreateDevice)(void*, REFGUID, void**, LPUNKNOWN);
    void** dvt = *reinterpret_cast<void***>(di);
    void* dev = nullptr;
    hr = reinterpret_cast<PFN_CreateDevice>(dvt[3])(di, kGuidSysKeyboard, &dev, nullptr);
    if (FAILED(hr) || !dev) {
        Log::get().note("keyboard gate: CreateDevice(SysKeyboard) on %s refused (0x%08lX); "
                        "that door is not installed.",
                        name, static_cast<unsigned long>(hr));
        reinterpret_cast<IUnknown*>(di)->Release();
        return;
    }
    // The IDirectInput8 object itself is not needed past this point; the
    // device keeps its own reference to what it needs.
    reinterpret_cast<IUnknown*>(di)->Release();
    d.dummy = dev;

    void** table = *reinterpret_cast<void***>(dev);
    if (*sharedTableOut && *sharedTableOut == table) {
        // The two interfaces share one table on this dinput8: patching it
        // twice would chain the second hook to the first and loop.
        Log::get().note("keyboard gate: %s dispatches through the same table as the "
                        "door already installed; one hook covers both.",
                        name);
        return;
    }
    if (!d.hook.attach(dev) || d.hook.executablePrefix() <= kSlotGetDeviceInfo) {
        Log::get().note("keyboard gate: the %s keyboard device's vtable does not read as "
                        "one this build can patch (%zu plausible entries); that door is "
                        "not installed.",
                        name, d.hook.executablePrefix());
        d.hook.uninstall();
        return;
    }
    char modState[64], modData[64];
    iatHookEntryModule(table[kSlotGetDeviceState], modState, sizeof(modState));
    iatHookEntryModule(table[kSlotGetDeviceData], modData, sizeof(modData));
    d.hook.setMode(HookMode::InPlace);
    d.hook.replace(kSlotGetDeviceState, reinterpret_cast<void*>(&hookGetDeviceState<Door, Wide>),
                   reinterpret_cast<void**>(&d.origState));
    d.hook.replace(kSlotGetDeviceData, reinterpret_cast<void*>(&hookGetDeviceData<Door, Wide>),
                   reinterpret_cast<void**>(&d.origData));
    if (!d.hook.commit()) {
        Log::get().note("keyboard gate: patching the %s keyboard table failed; that door "
                        "is not installed.",
                        name);
        d.hook.uninstall();
        return;
    }
    d.installed = true;
    *sharedTableOut = table;
    // Whose memory the table is in: dinput8.dll's own image is the class's
    // shared table, which every keyboard device dispatches through; a
    // table anywhere else (a tool's module, or no module at all -- a heap
    // copy) was made for our dummy alone, and the game's device has one
    // of its own that this patch never touches.
    iatHookEntryModule(table, d.tableModule, sizeof(d.tableModule));
    strncpy_s(d.entryModule, sizeof(d.entryModule), modState, _TRUNCATE);
    Log::get().note(
        "keyboard gate: the DirectInput door is installed on %s (GetDeviceState pointed "
        "into %s, GetDeviceData into %s before the patch -- dinput8.dll is the runtime's "
        "own, anything else is a tool ahead of EDVR in the chain, chained through). The "
        "table itself lives in %s (dinput8.dll's own image is the shared one; anywhere "
        "else is a copy made for our device alone). The game's own keyboard device "
        "reaches the door only if the table is shared; the probe line says whether it "
        "does, and a line prints once if it has not by the time the menu has held the "
        "keys a second.",
        name, modState, modData, d.tableModule);
}

void installUserDoors() {
    char mod[64];
    if (iatHookInstall("user32.dll", "GetAsyncKeyState",
                       reinterpret_cast<void*>(&hookGetAsyncKeyState), &g_user.asyncKey)) {
        g_user.origAsync = reinterpret_cast<PFN_GetAsyncKeyState>(g_user.asyncKey.original);
    }
    if (iatHookInstall("user32.dll", "GetKeyState",
                       reinterpret_cast<void*>(&hookGetKeyState), &g_user.keyState)) {
        g_user.origKeyState = reinterpret_cast<PFN_GetKeyState>(g_user.keyState.original);
    }
    if (iatHookInstall("user32.dll", "GetKeyboardState",
                       reinterpret_cast<void*>(&hookGetKeyboardState), &g_user.keyboardState)) {
        g_user.origKeyboardState =
            reinterpret_cast<PFN_GetKeyboardState>(g_user.keyboardState.original);
    }
    if (iatHookInstall("user32.dll", "PeekMessageA",
                       reinterpret_cast<void*>(&hookPeekMessageA), &g_user.peek)) {
        g_user.origPeek = reinterpret_cast<PFN_PeekMessageA>(g_user.peek.original);
    }
    iatHookEntryModule(g_user.asyncKey.original, mod, sizeof(mod));
    Log::get().note(
        "keyboard gate: the executable's import table -- GetAsyncKeyState %s, GetKeyState "
        "%s, GetKeyboardState %s, PeekMessageA %s (the first pointed into %s before the "
        "patch). EDVR's own modules import these through their own tables and keep "
        "reading the real keyboard.",
        g_user.asyncKey.applied ? "patched" : "NOT FOUND",
        g_user.keyState.applied ? "patched" : "NOT FOUND",
        g_user.keyboardState.applied ? "patched" : "NOT FOUND",
        g_user.peek.applied ? "patched" : "NOT FOUND", mod);
}

void probeLine() {
    Log::get().note(
        "input probe (5 s): dinput %s state %u (foreign %u, keyboard %u) data %u "
        "(keyboard %u) zeroed %u swallowed %u; dinput %s state %u (foreign %u, keyboard "
        "%u) data %u (keyboard %u); user32 GetAsyncKeyState %u GetKeyState %u "
        "GetKeyboardState %u; pump keyboard messages %u, WM_INPUT %u, nulled %u, "
        "swallowed %u; keys %s.",
        g_diW.name, g_diW.stateCalls.exchange(0), g_diW.stateForeign.exchange(0),
        g_diW.stateKeyboard.exchange(0), g_diW.dataCalls.exchange(0),
        g_diW.dataKeyboard.exchange(0), g_diW.zeroed.exchange(0), g_diW.swallowed.exchange(0),
        g_diA.name, g_diA.stateCalls.exchange(0), g_diA.stateForeign.exchange(0),
        g_diA.stateKeyboard.exchange(0), g_diA.dataCalls.exchange(0),
        g_diA.dataKeyboard.exchange(0), g_user.asyncCalls.exchange(0),
        g_user.keyStateCalls.exchange(0), g_user.keyboardStateCalls.exchange(0),
        g_user.peekKeyMessages.exchange(0), g_user.peekInputMessages.exchange(0),
        g_user.nulled.exchange(0), g_user.swallowed.exchange(0),
        g_private.load() ? "PRIVATE" : "shared");
}

// The DirectInput door's evidence, the two facts the Status page prints and
// the menu's alias predicate consults: is a door installed and not retired,
// and has the game's keyboard been seen reaching one. A retired door does
// not clear g_private (inputGateSetPrivate only follows the menu's wish),
// so the flag alone cannot say the ship is deaf to a key.
void doorEvidence(bool* di, bool* reached) {
    bool any = (g_diW.installed && !g_diW.retired) || (g_diA.installed && !g_diA.retired);
    for (const auto& d : g_gameDi) any |= d.installed && !d.retired;
    // The budget is what actually decides pass-through: once it is spent
    // guardedBudget skips every door's lambda, and only the door whose next
    // GetDeviceState observes that marks itself retired -- a door the game
    // reads through GetDeviceData alone never would. So a spent budget is
    // no live door, whatever the per-door flags say.
    if (!g_budgetDi.shouldRun()) any = false;
    *di = any;
    *reached = g_diW.stateForeign.load() + g_diA.stateForeign.load() + g_diW.dataKeyboard.load() +
                       g_diA.dataKeyboard.load() >
                   0 ||
               g_gameKeyboardCalls.load() != 0;
}

}  // namespace

uint8_t inputGateDikOf(int vk) {
    if (vk <= 0 || vk > 255) return 0;
    if (vk == VK_PAUSE) return DIK_PAUSE;   // E1-prefixed; MapVirtualKey gives NumLock's code
    const UINT sc = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    if (sc == 0 || sc > 0x7F) return 0;
    // The extended keys: DirectInput names them scan code | 0x80.
    switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT: case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_DIVIDE: case VK_RCONTROL: case VK_RMENU: case VK_LWIN: case VK_RWIN:
        case VK_APPS: case VK_SNAPSHOT:
            return static_cast<uint8_t>(sc | 0x80);
        default:
            return static_cast<uint8_t>(sc);
    }
}

void inputGateFilterState(uint8_t* st, bool priv, uint8_t summonDik, uint32_t summonMods) {
    if (!st) return;
    if (priv) {
        memset(st, 0, 256);
        return;
    }
    if (summonDik && (summonMods & ~modsInState(st)) == 0) st[summonDik] = 0;
}

uint32_t inputGateFilterData(DiObjectData* data, uint32_t count, bool priv, uint8_t summonDik,
                             bool summonModsHeld) {
    if (!data) return count;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const bool down = (data[i].dwData & 0x80) != 0;
        bool keep = true;
        if (priv && down) keep = false;
        if (summonDik && summonModsHeld && data[i].dwOfs == summonDik) keep = false;
        if (keep) {
            if (kept != i) data[kept] = data[i];
            ++kept;
        }
    }
    return kept;
}

void inputGateConfigure(Config& cfg) {
    const std::string kb = cfg.getString("menu.keyboard", "private");
    g_privateWanted = _stricmp(kb.c_str(), "shared") != 0;
    g_probe = cfg.getBool("advanced.input_probe", false);
    const std::string key = cfg.getString("hotkey.menu", "F8");
    uint32_t mods = 0;
    const int vk = key.empty() ? 0 : virtualKeyFromName(key.c_str(), &mods);
    uint32_t packed = 0;
    if (vk > 0 && vk < 256) {
        const uint8_t dik = inputGateDikOf(vk);
        packed = 0x80000000u | static_cast<uint32_t>(vk) | (static_cast<uint32_t>(dik) << 8) |
                 ((mods & 7u) << 16);
        static uint32_t lastNoted = 0xFFFFFFFFu;
        if (packed != lastNoted) {
            lastNoted = packed;
            if (dik) {
                Log::get().note("keyboard gate: the menu key %s (vk 0x%02X, DirectInput 0x%02X) is "
                                "swallowed at every door while its chord is held, so the "
                                "game never sees the press that opens the menu.",
                                key.c_str(), vk, dik);
            } else {
                Log::get().note("keyboard gate: the menu key %s (vk 0x%02X) has no DirectInput "
                                "scan code this build can name, so the DirectInput door "
                                "cannot swallow it -- the game will see that press too. "
                                "Prefer an F-key.",
                                key.c_str(), vk);
            }
        }
    }
    g_summon.store(packed);
    if (!g_privateWanted) {
        g_private.store(0);
        g_releaseTail.store(false);
    }
}

void inputGateInstall() {
    if (g_installTried) return;
    g_installTried = true;
    guarded("inputGate/install", [&] {
        bool captured = false;
        for (const auto& d : g_gameDi) captured |= d.installed;
        HMODULE di = GetModuleHandleW(L"dinput8.dll");
        if (!di) di = LoadLibraryW(L"dinput8.dll");
        PFN_DirectInput8Create create =
            di ? reinterpret_cast<PFN_DirectInput8Create>(GetProcAddress(di, "DirectInput8Create"))
               : nullptr;
        if (captured) {
            Log::get().note("keyboard gate: using the keyboard device captured from the game.");
        } else if (!create) {
            Log::get().note("keyboard gate: dinput8.dll or DirectInput8Create is missing; the "
                            "DirectInput door is not installed and bound keys reach the game.");
        } else {
            void** shared = nullptr;
            const HINSTANCE inst = GetModuleHandleW(nullptr);
            installDiDoor<&g_diW, true>(create, inst, kIidDirectInput8W, "IDirectInput8W",
                                        &shared);
            installDiDoor<&g_diA, false>(create, inst, kIidDirectInput8A, "IDirectInput8A",
                                         &shared);
        }
        installUserDoors();
    });
}

void inputGateSetPrivate(bool priv) {
    const int want = (priv && g_privateWanted) ? 1 : 0;
    if (g_private.load() != want) {
        if (!want) captureReleaseTail(&GetAsyncKeyState);
        else g_releaseTail.store(false);
        g_private.store(want);
    } else if (!want) {
        // The menu's fault path still calls SetPrivate(false) each frame,
        // even when its normal tick has retired. Never strand held keys there.
        refreshReleaseTail(&GetAsyncKeyState);
    }
}

bool inputGatePrivate() { return g_private.load() != 0; }

bool inputGateHoldsGameKeyboard() {
    if (g_private.load() == 0) return false;
    bool di = false, reached = false;
    doorEvidence(&di, &reached);
    return di && reached;
}

bool inputGateGameKeyboardSeen() {
    bool di = false, reached = false;
    doorEvidence(&di, &reached);
    return di && reached;
}

void inputGateTick() {
    if (!g_installTried) return;
    refreshReleaseTail(&GetAsyncKeyState);
    static bool gameReachedNoted = false;
    if (!gameReachedNoted && g_gameKeyboardCalls.load() != 0) {
        gameReachedNoted = true;
        Log::get().note("keyboard gate: the game's captured keyboard is reaching the gate; "
                        "state and buffered reads follow menu privacy.");
    }
    if (dueMs(g_reclaimMs, kReclaimEveryMs)) {
        g_reclaimMs = stampMs();
        // Vouch only for slots the game has been reaching and that have gone
        // quiet for several seconds while frames flow -- the same evidence
        // rule vScreen applies. A slot never reached is never vouched.
        DiDoor* doors[kGameDeviceDoors + 2] = {&g_diW, &g_diA};
        for (size_t i = 0; i < kGameDeviceDoors; ++i) doors[i + 2] = &g_gameDi[i];
        for (DiDoor* d : doors) {
            if (!d->installed) continue;
            const uint32_t st = d->stateCalls.load(std::memory_order_relaxed);
            const uint32_t da = d->dataCalls.load(std::memory_order_relaxed);
            const bool moved = st != d->stateSeen || da != d->dataSeen;
            d->stateSeen = st;
            d->dataSeen = da;
            d->quietSeconds = moved ? 0 : d->quietSeconds + 1;
            if (d->quietSeconds >= 5 && d->stateForeign.load(std::memory_order_relaxed) > 0) {
                const size_t slots[2] = {kSlotGetDeviceState, kSlotGetDeviceData};
                d->hook.reclaim(d->name, slots, 2);
            } else {
                d->hook.reclaim(d->name);
            }
        }
    }
    if (g_probe && dueMs(g_probeMs, kProbeEveryMs)) {
        g_probeMs = stampMs();
        probeLine();
    }
    // The blindness verdict (the note at g_privateTicks says why). A
    // keyboard call counted at either door is any keyboard device's -- the
    // dummy is held, never polled -- so zero after a second of private
    // keys means the game's device dispatches through a table the door is
    // not on, and DirectInput keys are reaching the ship.
    if (g_private.load(std::memory_order_relaxed) != 0) {
        if (g_privateTicks < kUnreachedAfterTicks) ++g_privateTicks;
        if (!g_unreachedNoted && g_privateTicks == kUnreachedAfterTicks) {
            const DiDoor* live = (g_diW.installed && !g_diW.retired) ? &g_diW
                               : (g_diA.installed && !g_diA.retired) ? &g_diA : nullptr;
            const uint32_t kbd = g_diW.stateKeyboard.load() + g_diA.stateKeyboard.load() +
                                 g_diW.dataKeyboard.load() + g_diA.dataKeyboard.load();
            if (live && kbd == 0 && g_gameKeyboardCalls.load() == 0) {
                g_unreachedNoted = true;
                Log::get().note(
                    "keyboard gate: the menu has held the keys for %u frames and no keyboard "
                    "device but our own has reached the DirectInput door -- the game's device "
                    "dispatches through a table the door is not on (ours lives in %s; its "
                    "GetDeviceState pointed into %s, a tool ahead in the chain that hands each "
                    "device a table of its own), so DirectInput keys are REACHING THE GAME while "
                    "the menu is open: Tab boosts, Space and Enter act in the ship. PageUp/"
                    "PageDown change the page and the arrows move the highlight, and are safe "
                    "where Elite has nothing bound to them. The Status page's doors line says "
                    "'not reached yet' for the same reason.",
                    kUnreachedAfterTicks, live->tableModule, live->entryModule);
            }
        }
    } else {
        g_privateTicks = 0;
    }
}

void inputGateStatusLine(char* buf, size_t bufLen) {
    if (!buf || !bufLen) return;
    bool di = false, reached = false;
    doorEvidence(&di, &reached);
    const bool trio = g_user.asyncKey.applied && !g_user.retired2;
    const bool pump = g_user.peek.applied && !g_user.retired3;
    snprintf(buf, bufLen, "keys %s -- doors: dinput %s%s, key-state %s, pump %s",
             g_private.load() ? "PRIVATE" : (g_privateWanted ? "private when open" : "shared"),
             di ? "on" : "off", di ? (reached ? " (game reaches it)" : " (not reached yet)") : "",
             trio ? "on" : "off", pump ? "on" : "off");
    buf[bufLen - 1] = 0;
}

void inputGateShutdown() {
    g_private.store(0);
    g_summon.store(0);
    g_releaseTail.store(false);
    iatHookUninstall(&g_createImport);
    iatHookUninstall(&g_user.peek);
    iatHookUninstall(&g_user.keyboardState);
    iatHookUninstall(&g_user.keyState);
    iatHookUninstall(&g_user.asyncKey);
    for (DiDoor* d : {&g_diA, &g_diW}) {
        if (d->installed) d->hook.uninstall();
        d->installed = false;
        if (d->dummy) {
            reinterpret_cast<IUnknown*>(d->dummy)->Release();
            d->dummy = nullptr;
        }
    }
    for (auto& d : g_gameDi) {
        d.hook.uninstall();
        d.installed = false;
        if (d.dummy) reinterpret_cast<IUnknown*>(d.dummy)->Release();
        d.dummy = nullptr;
    }
    for (auto& f : g_factories) {
        f.hook.uninstall();
        if (f.owner) reinterpret_cast<IUnknown*>(f.owner)->Release();
        f.owner = nullptr;
    }
}

void inputGateInstallEarly() {
    // Called during loader attach: only inspect the already-mapped EXE's
    // imports and exchange one pointer. No DirectInput creation, loading,
    // logging or config access here. Actual devices arrive after startup.
    iatHookInstall("dinput8.dll", "DirectInput8Create",
                   reinterpret_cast<void*>(&hookDirectInput8Create), &g_createImport);
}

}  // namespace edvr

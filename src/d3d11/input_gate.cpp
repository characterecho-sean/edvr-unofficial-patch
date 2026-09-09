#include "input_gate.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>

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
    bool  installed = false;
    bool  retired = false;           // a fault took it out for the session
    // The per-object keyboard verdicts. Fixed and small: a game has a
    // handful of devices. An unknown object past the table passes through.
    struct Verdict { void* self; bool keyboard; };
    Verdict verdicts[16] = {};
    volatile LONG verdictCount = 0;
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
};
DiDoor g_diA;
DiDoor g_diW;

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

// Is this object a keyboard? Asked once per object through the table's
// own, unpatched GetDeviceInfo; remembered by pointer. `Wide` picks the
// instance struct the table speaks.
template <bool Wide>
bool isKeyboard(DiDoor& d, void* self) {
    if (self == d.dummy) return true;
    const LONG n = d.verdictCount;
    for (LONG i = 0; i < n; ++i) {
        if (d.verdicts[i].self == self) return d.verdicts[i].keyboard;
    }
    void** vt = *reinterpret_cast<void***>(self);
    bool keyboard = false;
    if (vt && vt[kSlotGetDeviceInfo]) {
        if (Wide) {
            DIDEVICEINSTANCEW di{};
            di.dwSize = sizeof(di);
            if (SUCCEEDED(reinterpret_cast<PFN_GetDeviceInfoW>(vt[kSlotGetDeviceInfo])(self, &di))) {
                keyboard = GET_DIDEVICE_TYPE(di.dwDevType) == DI8DEVTYPE_KEYBOARD;
            }
        } else {
            DIDEVICEINSTANCEA di{};
            di.dwSize = sizeof(di);
            if (SUCCEEDED(reinterpret_cast<PFN_GetDeviceInfoA>(vt[kSlotGetDeviceInfo])(self, &di))) {
                keyboard = GET_DIDEVICE_TYPE(di.dwDevType) == DI8DEVTYPE_KEYBOARD;
            }
        }
    }
    // Remember it, if there is room. A table full of sixteen objects is a
    // game this was not written for; the seventeenth is asked every call,
    // which is slower and still correct.
    if (n < 16) {
        d.verdicts[n].self = self;
        d.verdicts[n].keyboard = keyboard;
        InterlockedIncrement(&d.verdictCount);
    }
    return keyboard;
}

template <DiDoor* Door, bool Wide>
HRESULT STDMETHODCALLTYPE hookGetDeviceState(void* self, DWORD cb, LPVOID data) {
    DiDoor& d = *Door;
    const HRESULT hr = d.origState(self, cb, data);
    d.stateCalls.fetch_add(1, std::memory_order_relaxed);
    if (self != d.dummy) d.stateForeign.fetch_add(1, std::memory_order_relaxed);
    if (d.retired || FAILED(hr) || !data) return hr;
    guardedBudget(g_budgetDi, [&] {
        if (!isKeyboard<Wide>(d, self)) return;
        d.stateKeyboard.fetch_add(1, std::memory_order_relaxed);
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

template <DiDoor* Door, bool Wide>
HRESULT STDMETHODCALLTYPE hookGetDeviceData(void* self, DWORD cbObj, LPDIDEVICEOBJECTDATA rgdod,
                                            LPDWORD inOut, DWORD flags) {
    DiDoor& d = *Door;
    const HRESULT hr = d.origData(self, cbObj, rgdod, inOut, flags);
    d.dataCalls.fetch_add(1, std::memory_order_relaxed);
    if (d.retired || FAILED(hr) || !rgdod || !inOut || *inOut == 0) return hr;
    if (cbObj != sizeof(DIDEVICEOBJECTDATA)) return hr;   // a layout this was not written for
    guardedBudget(g_budgetDi, [&] {
        if (!isKeyboard<Wide>(d, self)) return;
        d.dataKeyboard.fetch_add(1, std::memory_order_relaxed);
        const bool priv = g_private.load(std::memory_order_relaxed) != 0;
        int vk = 0;
        uint8_t dik = 0;
        uint32_t mods = 0;
        const uint32_t packed = g_summon.load(std::memory_order_relaxed);
        if (packed & 0x80000000u) unpackSummon(packed, &vk, &dik, &mods);
        const bool swallow = dik != 0 && summonModsHeldNow(mods);
        if (!priv && !swallow) return;
        static_assert(sizeof(DiObjectData) == sizeof(DIDEVICEOBJECTDATA),
                      "DiObjectData mirrors DIDEVICEOBJECTDATA");
        const uint32_t before = *inOut;
        const uint32_t kept = inputGateFilterData(reinterpret_cast<DiObjectData*>(rgdod),
                                                  before, priv, dik, swallow);
        *inOut = kept;
        if (kept < before) {
            (priv ? d.zeroed : d.swallowed).fetch_add(before - kept, std::memory_order_relaxed);
        }
    });
    return hr;
}

SHORT WINAPI hookGetAsyncKeyState(int vk) {
    g_user.asyncCalls.fetch_add(1, std::memory_order_relaxed);
    if (!g_user.retired2) {
        if (g_private.load(std::memory_order_relaxed)) return 0;
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

// Install one DirectInput door: make the dummy device, patch its table.
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
    Log::get().note(
        "keyboard gate: the DirectInput door is installed on %s (GetDeviceState pointed "
        "into %s, GetDeviceData into %s before the patch -- dinput8.dll is the runtime's "
        "own, anything else is a tool ahead of EDVR in the chain, chained through). The "
        "game's own keyboard device reaches it if the table is shared; the probe line "
        "says whether it does.",
        name, modState, modData);
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
    if (!g_privateWanted && g_private.load()) g_private.store(0);
}

void inputGateInstall() {
    if (g_installTried) return;
    g_installTried = true;
    guarded("inputGate/install", [&] {
        HMODULE di = GetModuleHandleW(L"dinput8.dll");
        if (!di) di = LoadLibraryW(L"dinput8.dll");
        PFN_DirectInput8Create create =
            di ? reinterpret_cast<PFN_DirectInput8Create>(GetProcAddress(di, "DirectInput8Create"))
               : nullptr;
        if (!create) {
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
    if (g_private.load() != want) g_private.store(want);
}

bool inputGatePrivate() { return g_private.load() != 0; }

void inputGateTick() {
    if (!g_installTried) return;
    if (dueMs(g_reclaimMs, kReclaimEveryMs)) {
        g_reclaimMs = stampMs();
        // Vouch only for slots the game has been reaching and that have gone
        // quiet for several seconds while frames flow -- the same evidence
        // rule vScreen applies. A slot never reached is never vouched.
        for (DiDoor* d : {&g_diW, &g_diA}) {
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
}

void inputGateStatusLine(char* buf, size_t bufLen) {
    if (!buf || !bufLen) return;
    const bool di = (g_diW.installed && !g_diW.retired) || (g_diA.installed && !g_diA.retired);
    const bool reached = g_diW.stateForeign.load() + g_diA.stateForeign.load() +
                             g_diW.dataKeyboard.load() + g_diA.dataKeyboard.load() >
                         0;
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
}

}  // namespace edvr

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
#include "joy_watch.h"

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
const GUID kGuidSysKeyboardEm = {0x6F1D2B82, 0xD5A0, 0x11CF,
                                 {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID kGuidSysKeyboardEm2 = {0x6F1D2B83, 0xD5A0, 0x11CF,
                                  {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

inline bool isKeyboardGuid(REFGUID guid) {
    return IsEqualGUID(guid, kGuidSysKeyboard) ||
           IsEqualGUID(guid, kGuidSysKeyboardEm) ||
           IsEqualGUID(guid, kGuidSysKeyboardEm2);
}

const GUID kGuidSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF,
                            {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID kGuidSysMouseEm = {0x6F1D2B80, 0xD5A0, 0x11CF,
                              {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID kGuidSysMouseEm2 = {0x6F1D2B81, 0xD5A0, 0x11CF,
                               {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

inline bool isMouseGuid(REFGUID guid) {
    return IsEqualGUID(guid, kGuidSysMouse) || IsEqualGUID(guid, kGuidSysMouseEm) ||
           IsEqualGUID(guid, kGuidSysMouseEm2);
}

// dinput.h's device-type values the joystick watch relies on (joy_watch.cpp
// spells them as numbers so it needs no <dinput.h>).
static_assert(DI8DEVTYPE_DEVICE == 0x11 && DI8DEVTYPE_JOYSTICK == 0x14 &&
                  DI8DEVTYPE_GAMEPAD == 0x15 && DI8DEVTYPE_DRIVING == 0x16 &&
                  DI8DEVTYPE_FLIGHT == 0x17 && DI8DEVTYPE_1STPERSON == 0x18 &&
                  DI8DEVTYPE_SUPPLEMENTAL == 0x1C,
              "joy_watch.cpp's controller device types");
static_assert(sizeof(DIJOYSTATE) == kJoyStateSize && sizeof(DIJOYSTATE2) == kJoyState2Size,
              "the layouts joy_watch.cpp reads");
static_assert(DIJOFS_POV(0) == kJoyOfsPov && DIJOFS_BUTTON(0) == kJoyOfsButtons,
              "the offsets joy_watch.cpp reads");

// IDirectInputDevice8's vtable, counted from dinput.h's declaration order:
// IUnknown 0-2, GetCapabilities 3, EnumObjects 4, GetProperty 5,
// SetProperty 6, Acquire 7, Unacquire 8, GetDeviceState 9, GetDeviceData
// 10, SetDataFormat 11, SetEventNotification 12, SetCooperativeLevel 13,
// GetObjectInfo 14, GetDeviceInfo 15.
constexpr size_t kSlotGetDeviceState = 9;
constexpr size_t kSlotGetDeviceData = 10;
constexpr size_t kSlotGetDeviceInfo = 15;   // checked for plausibility only; nothing calls it

// IDirectInput8's vtable: IUnknown 0-2, CreateDevice 3, EnumDevices 4 (A and W alike,
// the callback type aside).
constexpr size_t kSlotEnumDevices = 4;

typedef HRESULT(WINAPI* PFN_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDevice)(void*, REFGUID, void**, LPUNKNOWN);
typedef HRESULT(STDMETHODCALLTYPE* PFN_EnumDevices)(void*, DWORD, LPVOID, LPVOID, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_GetDeviceState)(void*, DWORD, LPVOID);
typedef HRESULT(STDMETHODCALLTYPE* PFN_GetDeviceData)(void*, DWORD, LPDIDEVICEOBJECTDATA,
                                                      LPDWORD, DWORD);

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
    PFN_EnumDevices enumOriginal = nullptr;   // the game's EnumDevices, the same slot in the A and W tables
    bool wide = false;                        // IDirectInput8W: the enumeration callback is the W kind
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
// The joystick watch (joy_watch.h) has a budget of its own: a fault while
// copying a joystick's buttons retires the WATCH, never a keyboard door.
FaultBudget g_budgetJoy("inputGate.joystick", 4);

// ---------------------------------------------------------------------------
// The joystick watch's side of the gate (docs/settings-menu.md, "Hotkeys page").
//
// READ-ONLY, and it calls nothing on a controller. The invariant, stated once:
// EDVR calls nothing on a non-keyboard device beyond holding a reference (the AddRef
// at capture, released at shutdown). A controller's type and name come from the
// game's OWN IDirectInput8::EnumDevices, recorded by the factory door as the game
// receives each record. Its reads are copies taken AFTER the game's own GetDeviceState
// or GetDeviceData returns, into joy_watch's table. The rig pins this with a fake device
// whose every method counts its calls. Issue 45: a wheel's force-feedback driver stalled
// the render thread inside DirectInput, and new traffic there is the risk.
//   * the game's buffer is never written;
//   * a device the game never enumerated is not registered, not captured and not called;
//   * anything it does not recognise -- a device that is not a game controller, a buffer
//     of another size, a row at an offset that is not a button -- is ignored and counted,
//     and the call goes on untouched.

// The game's own enumeration is the only source of a controller's type and identity.
// The factory door records every device record the game receives from EnumDevices
// (below); a device the game then creates is looked up by its instance GUID.
struct EnumRecord {
    bool     used = false;
    GUID     instance{};
    uint32_t devType = 0;      // dwDevType, as the game was handed it
    uint32_t id = 0;           // the id Elite writes: vendor << 16 | product
    char     name[48] = "";    // the product name, UTF-8, for the log
};
constexpr size_t kEnumRecords = 32;
EnumRecord g_enumRecords[kEnumRecords];
size_t     g_enumNext = 0;     // the slot a new record takes when it is not already here
SRWLOCK    g_enumLock = SRWLOCK_INIT;

// A game controller the game enumerated and then created. The flags say which read
// paths have been named in the log, once per device.
struct DeviceEntry {
    void*    device = nullptr;
    uint32_t type = 0;         // GET_DIDEVICE_TYPE(dwDevType)
    uint32_t id = 0;
    char     name[48] = "";
    uint64_t seq = 0;          // registration order: the oldest is the one evicted
    bool     notedState = false;
    bool     notedData = false;
    bool     notedSize = false;
};
constexpr size_t kWatchedDevices = 16;
DeviceEntry g_devices[kWatchedDevices];
uint64_t    g_devSeq = 0;
SRWLOCK     g_devicesLock = SRWLOCK_INIT;
std::atomic<bool> g_neverEnumeratedNoted{false};

// The registry entry for `self`, copied out under the lock. False for a device the
// watch does not know: it is then ignored, and nothing is called on it.
bool watchedEntry(const void* self, DeviceEntry* out) {
    AcquireSRWLockShared(&g_devicesLock);
    bool found = false;
    for (const DeviceEntry& e : g_devices) {
        if (e.device == self) {
            *out = e;
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_devicesLock);
    return found;
}

// An observer-only door on a joystick's own table: the private-table case, where
// the overlay hands each device a table of its own and the keyboard's door never
// sees the stick (the same reason the keyboard is captured at CreateDevice).
constexpr size_t kJoyDoors = 8;
struct JoyDoor {
    void*      held = nullptr;   // one reference, so the table and our restore target live
    VTableHook hook;
    PFN_GetDeviceState origState = nullptr;
    PFN_GetDeviceData  origData = nullptr;
    bool       installed = false;
};
JoyDoor g_joyDoors[kJoyDoors];

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

// GET_DIDEVICE_TYPE of what GetCapabilities says, 0 when the device would not say.
// Only the keyboard door asks, and only for a device that is neither its dummy nor a
// controller the game enumerated (see deviceTypeOf): the one call EDVR still makes on
// a non-keyboard device, and it predates the joystick watch.
uint32_t capsTypeOf(void* self) {
    void** vt = *reinterpret_cast<void***>(self);
    if (!vt || !vt[3]) return 0;
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    typedef HRESULT(STDMETHODCALLTYPE* GetCaps)(void*, LPDIDEVCAPS);
    if (FAILED(reinterpret_cast<GetCaps>(vt[3])(self, &caps))) return 0;
    return GET_DIDEVICE_TYPE(caps.dwDevType);
}

// The type the keyboard door reads a device as. Its dummy is the keyboard; a controller
// the game enumerated is answered from the registry, with no call on it.
template <bool Wide>
uint32_t deviceTypeOf(DiDoor& d, void* self) {
    if (self == d.dummy) return DI8DEVTYPE_KEYBOARD;
    DeviceEntry e;
    if (watchedEntry(self, &e)) return e.type;
    return capsTypeOf(self);
}

template <bool Wide>
bool isKeyboard(DiDoor& d, void* self) {
    return deviceTypeOf<Wide>(d, self) == DI8DEVTYPE_KEYBOARD;
}

// ---- the joystick watch ---------------------------------------------------

// One record the game was handed by EnumDevices. A record for an instance already
// here overwrites its own entry; a new one takes the next slot, the oldest.
void recordEnumerated(REFGUID instance, DWORD devType, REFGUID product, const char* name) {
    EnumRecord r;
    r.used = true;
    r.instance = instance;
    r.devType = devType;
    r.id = joyDeviceIdFromProduct(product.Data1);
    snprintf(r.name, sizeof(r.name), "%s", name);
    AcquireSRWLockExclusive(&g_enumLock);
    EnumRecord* slot = nullptr;
    for (EnumRecord& e : g_enumRecords) {
        if (e.used && IsEqualGUID(e.instance, instance)) {
            slot = &e;
            break;
        }
    }
    if (!slot) {
        slot = &g_enumRecords[g_enumNext];
        g_enumNext = (g_enumNext + 1) % kEnumRecords;
    }
    *slot = r;
    ReleaseSRWLockExclusive(&g_enumLock);
}

// The game's own record for an instance GUID, if it enumerated one.
bool enumeratedRecord(REFGUID instance, EnumRecord* out) {
    AcquireSRWLockShared(&g_enumLock);
    bool found = false;
    for (const EnumRecord& e : g_enumRecords) {
        if (e.used && IsEqualGUID(e.instance, instance)) {
            *out = e;
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_enumLock);
    return found;
}

// A device the game has just created. If the game enumerated its instance as a game
// controller, the device is registered with the type and name from that record, and
// true comes back: the caller may then capture its table. A device the game never
// enumerated is neither registered nor watched, and nothing is called on it.
bool registerCreated(void* device, REFGUID guid) {
    EnumRecord rec;
    if (!enumeratedRecord(guid, &rec)) {
        if (!g_neverEnumeratedNoted.exchange(true)) {
            Log::get().note("joystick watch: the game created a device it never enumerated; it is not watched.");
        }
        return false;
    }
    const uint32_t type = GET_DIDEVICE_TYPE(rec.devType);
    if (!joyDeviceTypeIsController(type)) return false;
    DeviceEntry e;
    e.device = device;
    e.type = type;
    e.id = rec.id;
    snprintf(e.name, sizeof(e.name), "%s", rec.name);
    AcquireSRWLockExclusive(&g_devicesLock);
    // The same address again (a new object where an old one was) takes the entry
    // over; otherwise the first free slot, else the one registered longest ago.
    DeviceEntry* slot = nullptr;
    for (DeviceEntry& d : g_devices) {
        if (d.device == device) {
            slot = &d;
            break;
        }
    }
    if (!slot) {
        for (DeviceEntry& d : g_devices) {
            if (!d.device) {
                slot = &d;
                break;
            }
        }
    }
    if (!slot) {
        slot = &g_devices[0];
        for (DeviceEntry& d : g_devices) {
            if (d.seq < slot->seq) slot = &d;
        }
    }
    e.seq = ++g_devSeq;
    *slot = e;
    ReleaseSRWLockExclusive(&g_devicesLock);
    char dev[9];
    hotkeyJoyDeviceText(rec.id, dev);
    Log::get().note("joystick watch: the game enumerated %s \"%s\" as a game controller; its reads are copied "
                    "after the game's own calls.", dev, e.name);
    return true;
}

// The game created a device at an address that may have held a controller before. The
// old identity is not the new object's, so it is dropped (before the new one is classified).
void watchForget(void* device) {
    AcquireSRWLockExclusive(&g_devicesLock);
    for (DeviceEntry& d : g_devices) {
        if (d.device == device) d = DeviceEntry();
    }
    ReleaseSRWLockExclusive(&g_devicesLock);
}

// Said once per device and per path: how the game reads its joystick. This is
// the line a flight reads to learn whether Elite uses GetDeviceState,
// GetDeviceData or both, and with which buffer. The flags live in the registry
// entry; the text is built outside the lock.
void joyNote(void* self, int what, uint32_t detail) {
    uint32_t id = 0;
    char name[48] = "";
    bool first = false;
    AcquireSRWLockExclusive(&g_devicesLock);
    for (DeviceEntry& e : g_devices) {
        if (e.device != self) continue;
        bool* flag = what == 0 ? &e.notedState : what == 1 ? &e.notedData : &e.notedSize;
        if (!*flag) {
            *flag = true;
            first = true;
            id = e.id;
            snprintf(name, sizeof(name), "%s", e.name);
        }
        break;
    }
    ReleaseSRWLockExclusive(&g_devicesLock);
    if (!first) return;
    char dev[9];
    hotkeyJoyDeviceText(id, dev);
    char text[260] = "";
    if (what == 0)
        snprintf(text, sizeof(text),
                 "joystick watch: %s \"%s\" is read by the game with GetDeviceState (%u bytes); its buttons are "
                 "watched.", dev, name, detail);
    else if (what == 1)
        snprintf(text, sizeof(text),
                 "joystick watch: %s \"%s\" is read by the game with GetDeviceData; its buttons are watched.", dev,
                 name);
    else
        snprintf(text, sizeof(text),
                 "joystick watch: %s \"%s\" is read with a %u-byte data format this build does not read (it "
                 "understands DIJOYSTATE, 80, and DIJOYSTATE2, 272), so its buttons cannot be watched. Please "
                 "report this line.", dev, name, detail);
    Log::get().note("%s", text);
}

// The game's GetDeviceState just returned `data` (cb bytes) for `self`. A copy, nothing
// more, and only for a device the registry holds.
void observeControllerState(void* self, DWORD cb, const void* data) {
    DeviceEntry e;
    if (!watchedEntry(self, &e)) return;
    const bool sized = cb == kJoyStateSize || cb == kJoyState2Size;
    if (joyWatchObserveState(self, e.id, data, cb, stampMs())) joyNote(self, 0, cb);
    else if (!sized) joyNote(self, 2, cb);
}

// The game's GetDeviceData just returned `count` rows for `self`. Also a copy.
void observeControllerData(void* self, const DIDEVICEOBJECTDATA* rows, DWORD count) {
    DeviceEntry e;
    if (!watchedEntry(self, &e)) return;
    if (joyWatchObserveData(self, e.id, rows, count, sizeof(DIDEVICEOBJECTDATA), stampMs())) joyNote(self, 1, 0);
}

template <bool Wide>
HRESULT filterDeviceState(DiDoor& d, void* self, DWORD cb, LPVOID data) {
    const HRESULT hr = d.origState(self, cb, data);
    d.stateCalls.fetch_add(1, std::memory_order_relaxed);
    if (self != d.dummy) d.stateForeign.fetch_add(1, std::memory_order_relaxed);
    if (d.retired || FAILED(hr) || !data) return hr;
    bool other = false;   // a device that is not the keyboard
    guardedBudget(g_budgetDi, [&] {
        if (deviceTypeOf<Wide>(d, self) != DI8DEVTYPE_KEYBOARD) {
            other = true;
            return;
        }
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
    // A joystick or HOTAS: copy what the game just read, after its call, into
    // the watch. Not the keyboard's business and not its budget's.
    if (other) {
        guardedBudget(g_budgetJoy, [&] { observeControllerState(self, cb, data); });
    }
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
    if (d.retired || FAILED(hr) || !rgdod || !inOut) return hr;
    if (cbObj != sizeof(DIDEVICEOBJECTDATA)) return hr;   // a layout this was not written for
    bool other = false;   // a device that is not the keyboard
    const DWORD returned = *inOut;
    guardedBudget(g_budgetDi, [&] {
        if (deviceTypeOf<Wide>(d, self) != DI8DEVTYPE_KEYBOARD) {
            other = true;
            return;
        }
        // An empty read is nothing to filter (the check that used to sit above
        // the type test, which now has to run for a joystick's empty read too:
        // a call that returns nothing still proves the game is reading it).
        if (*inOut == 0) return;
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
    // A joystick or HOTAS: the rows the game's read just returned, copied and
    // untouched (an empty read too: it shows the game is polling the device).
    if (other) {
        guardedBudget(g_budgetJoy, [&] { observeControllerData(self, rgdod, returned); });
    }
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
    SHORT result = 0;
    guarded("inputGate/origAsyncKeyState", [&] { result = g_user.origAsync(vk); });
    return result;
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
    SHORT result = 0;
    guarded("inputGate/origKeyState", [&] { result = g_user.origKeyState(vk); });
    return result;
}

BOOL WINAPI hookGetKeyboardState(PBYTE state) {
    BOOL r = FALSE;
    if (!guarded("inputGate/origKeyboardState", [&] { r = g_user.origKeyboardState(state); })) {
        return FALSE;
    }
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
    BOOL r = FALSE;
    if (!guarded("inputGate/origPeekMessageA",
                 [&] { r = g_user.origPeek(msg, hwnd, lo, hi, remove); })) {
        return FALSE;
    }
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

// ---- the joystick doors: observation only ---------------------------------

template <size_t Index>
HRESULT STDMETHODCALLTYPE joyDeviceState(void* self, DWORD cb, LPVOID data) {
    JoyDoor& j = g_joyDoors[Index];
    const HRESULT hr = j.origState(self, cb, data);
    if (SUCCEEDED(hr) && data) {
        guardedBudget(g_budgetJoy, [&] { observeControllerState(self, cb, data); });
    }
    return hr;
}

template <size_t Index>
HRESULT STDMETHODCALLTYPE joyDeviceData(void* self, DWORD cb, LPDIDEVICEOBJECTDATA data, LPDWORD count,
                                        DWORD flags) {
    JoyDoor& j = g_joyDoors[Index];
    const HRESULT hr = j.origData(self, cb, data, count, flags);
    if (SUCCEEDED(hr) && data && count && cb == sizeof(DIDEVICEOBJECTDATA)) {
        const DWORD returned = *count;
        guardedBudget(g_budgetJoy, [&] { observeControllerData(self, data, returned); });
    }
    return hr;
}

// What the factory door hands the game's own EnumDevices callback in place of the
// callback: the game's callback and its pvRef, carried for this one call on the stack.
struct EnumCall {
    void* callback;   // the game's LPDIENUMDEVICESCALLBACKA, or W for the W interface
    void* pvRef;      // the game's pvRef
};

// The thunks copy the record the game is being handed, then pass the SAME record on
// to the game's callback with the game's own pvRef, and return that callback's answer
// unchanged (DIENUM_STOP included). The lock is never held across the game's callback.
BOOL CALLBACK enumThunkA(LPCDIDEVICEINSTANCEA inst, LPVOID ref) {
    const EnumCall* call = static_cast<const EnumCall*>(ref);
    if (inst) {
        guardedBudget(g_budgetJoy, [&] {
            recordEnumerated(inst->guidInstance, inst->dwDevType, inst->guidProduct, inst->tszProductName);
        });
    }
    return reinterpret_cast<LPDIENUMDEVICESCALLBACKA>(call->callback)(inst, call->pvRef);
}

BOOL CALLBACK enumThunkW(LPCDIDEVICEINSTANCEW inst, LPVOID ref) {
    const EnumCall* call = static_cast<const EnumCall*>(ref);
    if (inst) {
        guardedBudget(g_budgetJoy, [&] {
            char name[260] = "";
            WideCharToMultiByte(CP_UTF8, 0, inst->tszProductName, -1, name, static_cast<int>(sizeof(name)),
                                nullptr, nullptr);
            name[sizeof(name) - 1] = 0;
            recordEnumerated(inst->guidInstance, inst->dwDevType, inst->guidProduct, name);
        });
    }
    return reinterpret_cast<LPDIENUMDEVICESCALLBACKW>(call->callback)(inst, call->pvRef);
}

// A registered controller (registerCreated said so) gets its table captured for the
// observers. A table a keyboard door already holds is watched by that door's wrapper
// (it sees every device on the table), so only a table of its own gets a door here --
// the private overlay table. Nothing is called on the device: the hooks only copy, and
// the AddRef below is the one reference, which keeps the table alive for the reclaim.
template <size_t... I>
void captureController(void* device, std::index_sequence<I...>) {
    static const PFN_GetDeviceState stateHooks[] = {&joyDeviceState<I>...};
    static const PFN_GetDeviceData dataHooks[] = {&joyDeviceData<I>...};
    CaptureLock lock;
    void** table = *reinterpret_cast<void***>(device);
    for (DiDoor* d : {&g_diW, &g_diA}) {
        if (d->installed && d->hook.originalVTable() == table) return;
    }
    for (auto& d : g_gameDi) {
        if (d.installed && d.hook.originalVTable() == table) return;
    }
    for (auto& j : g_joyDoors) {
        if (j.installed && j.hook.originalVTable() == table) return;
    }
    for (size_t i = 0; i < kJoyDoors; ++i) {
        JoyDoor& j = g_joyDoors[i];
        if (j.installed) continue;
        if (!j.hook.attach(device, 32) || j.hook.executablePrefix() <= kSlotGetDeviceData) {
            j.hook.uninstall();
            return;
        }
        reinterpret_cast<IUnknown*>(device)->AddRef();
        j.held = device;
        j.hook.setMode(HookMode::InPlace);
        const bool staged =
            j.hook.replace(kSlotGetDeviceState, reinterpret_cast<void*>(stateHooks[i]),
                           reinterpret_cast<void**>(&j.origState)) &&
            j.hook.replace(kSlotGetDeviceData, reinterpret_cast<void*>(dataHooks[i]),
                           reinterpret_cast<void**>(&j.origData));
        if (!staged || !j.hook.commit()) {
            j.hook.uninstall();
            reinterpret_cast<IUnknown*>(device)->Release();
            j.held = nullptr;
            return;
        }
        j.installed = true;
        char tableModule[64] = "?";
        iatHookEntryModule(table, tableModule, sizeof(tableModule));
        Log::get().note("joystick watch: captured a game controller at CreateDevice; table %zu in %s. Its "
                        "reads are copied after the game's own call returns; nothing else on the device is "
                        "touched.", i, tableModule);
        return;
    }
    Log::get().note("joystick watch: all %zu controller tables are occupied; a new one is left untouched.",
                    kJoyDoors);
}

// The game's IDirectInput8::EnumDevices, wrapped. The game's callback is replaced by the
// thunk for the interface this factory belongs to (A or W), with the game's callback and
// pvRef carried on the stack for this one call.
template <size_t Index>
HRESULT STDMETHODCALLTYPE factoryEnumDevices(void* self, DWORD devType, LPVOID callback, LPVOID pvRef,
                                             DWORD flags) {
    const FactoryDoor& f = g_factories[Index];
    if (!callback) return f.enumOriginal(self, devType, callback, pvRef, flags);
    EnumCall call{callback, pvRef};
    const LPVOID thunk = f.wide ? reinterpret_cast<LPVOID>(&enumThunkW) : reinterpret_cast<LPVOID>(&enumThunkA);
    return f.enumOriginal(self, devType, thunk, &call, flags);
}

template <size_t Index>
HRESULT STDMETHODCALLTYPE factoryCreateDevice(void* self, REFGUID guid, void** device,
                                               LPUNKNOWN outer) {
    const HRESULT hr = g_factories[Index].original(self, guid, device, outer);
    if (SUCCEEDED(hr) && device && *device && !outer) {
        watchForget(*device);   // whatever this object is, an old controller at its address is not its identity
        if (isKeyboardGuid(guid)) {
            guardedBudget(g_budgetDi, [&] {
                captureKeyboard(*device, std::make_index_sequence<kGameDeviceDoors>{});
            });
        } else if (!isMouseGuid(guid)) {
            // A controller the game enumerated is registered BEFORE the table is looked at:
            // a shared table that is already patched returns early in captureController,
            // and the device must be registered either way.
            guardedBudget(g_budgetJoy, [&] {
                if (registerCreated(*device, guid)) {
                    captureController(*device, std::make_index_sequence<kJoyDoors>{});
                }
            });
        }
    }
    return hr;
}

template <size_t... I>
void captureFactory(void* factory, bool wide, std::index_sequence<I...>) {
    static const PFN_CreateDevice hooks[] = {&factoryCreateDevice<I>...};
    static const PFN_EnumDevices enums[] = {&factoryEnumDevices<I>...};
    CaptureLock lock;
    void** table = *reinterpret_cast<void***>(factory);
    for (const auto& f : g_factories) {
        if (f.owner && f.hook.originalVTable() == table) return;
    }
    for (size_t i = 0; i < kFactoryDoors; ++i) {
        auto& f = g_factories[i];
        if (f.owner) continue;
        if (!f.hook.attach(factory, 11) || f.hook.executablePrefix() <= kSlotEnumDevices) {
            f.hook.uninstall();
            return;
        }
        f.owner = factory;
        f.wide = wide;
        reinterpret_cast<IUnknown*>(factory)->AddRef();
        f.hook.setMode(HookMode::InPlace);
        if (!f.hook.replace(3, reinterpret_cast<void*>(hooks[i]), reinterpret_cast<void**>(&f.original)) ||
            !f.hook.replace(kSlotEnumDevices, reinterpret_cast<void*>(enums[i]),
                            reinterpret_cast<void**>(&f.enumOriginal)) ||
            !f.hook.commit()) {
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
    if (!Config::get().getBool("advanced.input_gate", true)) {
        return hr;
    }
    const bool wide = IsEqualGUID(iid, kIidDirectInput8W);
    if (SUCCEEDED(hr) && out && *out && !outer && (wide || IsEqualGUID(iid, kIidDirectInput8A))) {
        guardedBudget(g_budgetDi, [&] {
            captureFactory(*out, wide, std::make_index_sequence<kFactoryDoors>{});
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
    JoyWatchStats js;
    joyWatchStats(&js);
    Log::get().note(
        "input probe (5 s): joystick watch %d device%s, GetDeviceState %llu (%llu in a size not read, last %u), "
        "GetDeviceData %llu (%llu rows used, %llu at an offset that is not a button or hat, last 0x%X), table full %llu; "
        "%s.",
        js.devices, js.devices == 1 ? "" : "s", static_cast<unsigned long long>(js.stateCalls),
        static_cast<unsigned long long>(js.statesIgnored), js.lastIgnoredSize,
        static_cast<unsigned long long>(js.dataCalls), static_cast<unsigned long long>(js.rowsUsed),
        static_cast<unsigned long long>(js.rowsIgnored), js.lastIgnoredOfs,
        static_cast<unsigned long long>(js.tableFull), g_budgetJoy.shouldRun() ? "running" : "RETIRED by faults");
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
    if (!Config::get().getBool("advanced.input_gate", true)) {
        Log::get().note("keyboard gate: advanced.input_gate is off; input doors are not installed.");
        return;
    }
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
        if (!want) {
            guarded("inputGate/captureReleaseTail",
                    [&] { captureReleaseTail(&GetAsyncKeyState); });
        } else {
            g_releaseTail.store(false);
        }
        g_private.store(want);
    } else if (!want) {
        // The menu's fault path still calls SetPrivate(false) each frame,
        // even when its normal tick has retired. Never strand held keys there.
        guarded("inputGate/refreshReleaseTail-setPrivate",
                [&] { refreshReleaseTail(&GetAsyncKeyState); });
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
    guarded("inputGate/refreshReleaseTail-tick", [&] { refreshReleaseTail(&GetAsyncKeyState); });
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
        // The joystick doors only look: a table another tool re-points is
        // named in the log, never patched back (observation is not worth a
        // fight over a shared slot).
        for (JoyDoor& j : g_joyDoors) {
            if (j.installed) j.hook.reclaim("joystick watch");
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
    for (auto& j : g_joyDoors) {
        j.hook.uninstall();
        j.installed = false;
        if (j.held) reinterpret_cast<IUnknown*>(j.held)->Release();
        j.held = nullptr;
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

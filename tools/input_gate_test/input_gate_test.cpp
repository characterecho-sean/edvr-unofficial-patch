// Exercise the production hooks with distinct overlay-style COM tables,
// then the executable's real DirectInput import. No keys are injected.
#include <windows.h>
static HWND testPadForeground = nullptr;
static HWND WINAPI testGetForegroundWindow() { return testPadForeground; }
#define GetForegroundWindow testGetForegroundWindow
#include "../../src/d3d11/input_gate.cpp"
#undef GetForegroundWindow

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

#include "../../src/d3d11/loaded_module_match.h"

using namespace edvr;

namespace {
int failures = 0;
void check(bool value, const char* name) {
    printf("  %s  %s\n", value ? "ok  " : "FAIL", name);
    if (!value) ++failures;
}
XINPUT_STATE testPadState{};
DWORD WINAPI testPadOriginal(DWORD index, XINPUT_STATE* state) {
    if (index != 0 || !state) return ERROR_DEVICE_NOT_CONNECTED;
    *state = testPadState;
    return ERROR_SUCCESS;
}
void padHookCases() {
    HWND ownWindow = CreateWindowExW(0, L"STATIC", L"EDVR pad gate rig", 0,
                                    0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(ownWindow != nullptr, "pad hook rig creates a same-process foreground window");
    if (!ownWindow) return;
    g_padImport.original = reinterpret_cast<void*>(&testPadOriginal);
    g_padInstalled.store(true);
    g_padRetired.store(false);
    testPadForeground = ownWindow;
    testPadState.dwPacketNumber = 1;
    testPadState.Gamepad.wButtons = 0x1000;
    XINPUT_STATE seen{};
    check(hookPadGetState(0, &seen) == ERROR_SUCCESS && seen.Gamepad.wButtons == 0x1000 &&
          !inputGatePadPrivate(0), "observed pad alone is not private before the panel requests it");
    inputGateSetPadPrivate(true, 0);
    check(hookPadGetState(0, &seen) == ERROR_SUCCESS && seen.Gamepad.wButtons == 0 &&
          inputGatePadPrivate(0), "the actual hook masks A after a focused request and proves the slot private");
    testPadForeground = nullptr;
    check(hookPadGetState(0, &seen) == ERROR_SUCCESS && seen.Gamepad.wButtons == 0 &&
          !inputGatePadPrivate(0), "actual foreground loss revokes the request and hides the held release tail");
    testPadForeground = ownWindow;
    testPadState.Gamepad.wButtons = 0;
    hookPadGetState(0, &seen);
    testPadState.Gamepad.wButtons = 0x1000;
    check(hookPadGetState(0, &seen) == ERROR_SUCCESS && seen.Gamepad.wButtons == 0x1000,
          "after release, an unowned fresh A press reaches the game");
    inputGateSetPadPrivate(false, -1);
    g_padInstalled.store(false);
    g_padImport.original = nullptr;
    DestroyWindow(ownWindow);
}
HRESULT STDMETHODCALLTYPE unused() { return E_NOTIMPL; }

struct Device {
    void** table;
    ULONG refs = 1;
    DWORD type = DI8DEVTYPE_KEYBOARD;
    HRESULT result = DI_OK;
    unsigned calls = 0;
    unsigned capsCalls = 0;
    BYTE keys[256]{};
    DIDEVICEOBJECTDATA events[4]{};
    DWORD count = 0;
};
ULONG STDMETHODCALLTYPE retainDevice(Device* d) { return ++d->refs; }
ULONG STDMETHODCALLTYPE releaseDevice(Device* d) { return --d->refs; }
HRESULT STDMETHODCALLTYPE caps(Device* d, DIDEVCAPS* c) {
    ++d->capsCalls;
    if (!c || c->dwSize != sizeof(*c)) return DIERR_INVALIDPARAM;
    c->dwDevType = d->type;
    return DI_OK;
}
HRESULT STDMETHODCALLTYPE state(Device* d, DWORD bytes, void* out) {
    ++d->calls;
    if (FAILED(d->result)) return d->result;
    if (bytes != 256 || !out) return DIERR_INVALIDPARAM;
    memcpy(out, d->keys, 256);
    return d->result;
}
HRESULT STDMETHODCALLTYPE data(Device* d, DWORD bytes, DIDEVICEOBJECTDATA* out,
                                DWORD* count, DWORD flags) {
    ++d->calls;
    if (FAILED(d->result)) return d->result;
    if (!count || bytes != sizeof(*out)) return DIERR_INVALIDPARAM;
    const DWORD n = *count < d->count ? *count : d->count;
    if (out) memcpy(out, d->events, n * sizeof(*out));
    *count = n;
    if (!(flags & DIGDD_PEEK) && out) d->count = 0;
    return d->result;
}
std::array<void*, 32> deviceTable() {
    std::array<void*, 32> t;
    t.fill(reinterpret_cast<void*>(&unused));
    t[1] = reinterpret_cast<void*>(&retainDevice);
    t[2] = reinterpret_cast<void*>(&releaseDevice);
    t[3] = reinterpret_cast<void*>(&caps);
    t[9] = reinterpret_cast<void*>(&state);
    t[10] = reinterpret_cast<void*>(&data);
    return t;
}
struct Factory {
    void** table;
    ULONG refs = 1;
    Device* next;
    HRESULT result = DI_OK;
    unsigned calls = 0;
    // The game's enumeration, as the fake factory answers EnumDevices: one device, the
    // record it is handed (A or W by `wide`), and what the game's callback answered.
    bool     wide = false;
    GUID     enumGuid{};
    DWORD    enumType = DI8DEVTYPE_JOYSTICK;
    DWORD    enumProduct = 0x0200231D;
    unsigned enumCalls = 0;
    HRESULT  enumResult = DI_OK;
    BOOL     callbackResult = DIENUM_CONTINUE;
};
ULONG STDMETHODCALLTYPE retainFactory(Factory* f) { return ++f->refs; }
ULONG STDMETHODCALLTYPE releaseFactory(Factory* f) { return --f->refs; }
HRESULT STDMETHODCALLTYPE createDevice(Factory* f, REFGUID, void** out, LPUNKNOWN) {
    ++f->calls;
    if (FAILED(f->result)) return f->result;
    *out = f->next;
    return f->result;
}
HRESULT STDMETHODCALLTYPE enumDevices(Factory* f, DWORD, LPVOID callback, LPVOID pvRef, DWORD) {
    ++f->enumCalls;
    if (FAILED(f->enumResult)) return f->enumResult;
    if (!callback) return DIERR_INVALIDPARAM;
    if (f->wide) {
        DIDEVICEINSTANCEW inst{};
        inst.dwSize = sizeof(inst);
        inst.guidInstance = f->enumGuid;
        inst.guidProduct = GUID{f->enumProduct, 0, 0, {0, 0, 'P', 'I', 'D', 'V', 'I', 'D'}};
        inst.dwDevType = f->enumType;
        wcscpy_s(inst.tszProductName, L"Test Stick");
        f->callbackResult = reinterpret_cast<LPDIENUMDEVICESCALLBACKW>(callback)(&inst, pvRef);
    } else {
        DIDEVICEINSTANCEA inst{};
        inst.dwSize = sizeof(inst);
        inst.guidInstance = f->enumGuid;
        inst.guidProduct = GUID{f->enumProduct, 0, 0, {0, 0, 'P', 'I', 'D', 'V', 'I', 'D'}};
        inst.dwDevType = f->enumType;
        strcpy_s(inst.tszProductName, "Test Stick");
        f->callbackResult = reinterpret_cast<LPDIENUMDEVICESCALLBACKA>(callback)(&inst, pvRef);
    }
    return DI_OK;
}
std::array<void*, 11> factoryTable() {
    std::array<void*, 11> t;
    t.fill(reinterpret_cast<void*>(&unused));
    t[1] = reinterpret_cast<void*>(&retainFactory);
    t[2] = reinterpret_cast<void*>(&releaseFactory);
    t[3] = reinterpret_cast<void*>(&createDevice);
    t[kSlotEnumDevices] = reinterpret_cast<void*>(&enumDevices);
    return t;
}
// The game's own callback, A or W: it records its pvRef and the record it was handed, and
// answers `answer` (DIENUM_STOP passes through the thunks unchanged).
struct GameEnum {
    unsigned calls = 0;
    void*    seenRef = nullptr;
    DWORD    seenType = 0;
    BOOL     answer = DIENUM_CONTINUE;
};
BOOL CALLBACK gameEnumW(LPCDIDEVICEINSTANCEW inst, LPVOID ref) {
    auto* g = static_cast<GameEnum*>(ref);
    ++g->calls;
    g->seenRef = ref;
    g->seenType = inst->dwDevType;
    return g->answer;
}
BOOL CALLBACK gameEnumA(LPCDIDEVICEINSTANCEA inst, LPVOID ref) {
    auto* g = static_cast<GameEnum*>(ref);
    ++g->calls;
    g->seenRef = ref;
    g->seenType = inst->dwDevType;
    return g->answer;
}
// The game enumerates through the factory's own slot 4 with its own callback and pvRef.
HRESULT enumerateThrough(Factory& f, GameEnum& g) {
    const LPVOID callback = f.wide ? reinterpret_cast<LPVOID>(&gameEnumW) : reinterpret_cast<LPVOID>(&gameEnumA);
    return reinterpret_cast<PFN_EnumDevices>(f.table[kSlotEnumDevices])(&f, DI8DEVCLASS_ALL, callback, &g, 0);
}
// Enumerate `guid` as `type` with product `product`, the record the game would hand out.
void enumerate(Factory& f, const GUID& guid, DWORD type, DWORD product) {
    f.enumGuid = guid;
    f.enumType = type;
    f.enumProduct = product;
    GameEnum g;
    enumerateThrough(f, g);
}
HRESULT createThrough(Factory& f, void** out) {
    return reinterpret_cast<PFN_CreateDevice>(f.table[3])(&f, kGuidSysKeyboard, out, nullptr);
}
HRESULT readState(Device& d, BYTE (&out)[256]) {
    return reinterpret_cast<PFN_GetDeviceState>(d.table[9])(&d, sizeof(out), out);
}
HRESULT readData(Device& d, DIDEVICEOBJECTDATA (&out)[4], DWORD& count, DWORD flags = 0) {
    count = 4;
    return reinterpret_cast<PFN_GetDeviceData>(d.table[10])(&d, sizeof(out[0]), out, &count, flags);
}
bool released(const BYTE (&bytes)[256]) {
    for (BYTE b : bytes) if (b) return false;
    return true;
}
bool physical[256]{};
SHORT WINAPI physicalKey(int vk) { return vk >= 0 && vk < 256 && physical[vk] ? SHORT(-32768) : 0; }
// A tail reader that stops inside the release scan until the rig lets it go, so another thread can poll the doors
// while the scan is in progress (PR #92 review).
HANDLE g_scanEntered = nullptr;
HANDLE g_scanMayContinue = nullptr;
bool g_pauseArmed = false;
// The pause itself. Both handles are copied before signalling entry: once the rig sees the entry it may point the
// globals at the next worker's events, and a worker that read g_scanMayContinue after that would wait on the wrong
// event and fall through on the timeout instead of in the order the case sets (PR #92 review).
void pauseInScan() {
    const HANDLE entered = g_scanEntered;
    const HANDLE mayContinue = g_scanMayContinue;
    SetEvent(entered);
    WaitForSingleObject(mayContinue, 10000);
}
SHORT WINAPI pausingKey(int vk) {
    if (g_pauseArmed) {
        g_pauseArmed = false;
        pauseInScan();
    }
    return physicalKey(vk);
}
// A refresh's reader that reports when the refresh first reaches its physical scan.
HANDLE g_refreshEntered = nullptr;
SHORT WINAPI refreshProbeKey(int vk) {
    if (g_refreshEntered) SetEvent(g_refreshEntered);
    return physicalKey(vk);
}
// Like pausingKey, but the reading is taken before the pause, so the resumed scan reports what was true when it began.
SHORT WINAPI sampleThenPauseKey(int vk) {
    const SHORT sample = physicalKey(vk);
    if (g_pauseArmed) {
        g_pauseArmed = false;
        pauseInScan();
    }
    return sample;
}
BOOL WINAPI physicalState(PBYTE out) {
    for (int vk = 0; vk < 256; ++vk) out[vk] = physical[vk] ? 0x80 : 0;
    return TRUE;
}
MSG queued{};
BOOL WINAPI nextMessage(LPMSG out, HWND, UINT, UINT, UINT) { *out = queued; return TRUE; }

// ---- fake joysticks, for the HOTAS watch (joy_watch.h) ---------------------------
//
// Every method of a fake joystick that the watch has no business calling counts a
// call in g_trapCalls: the promise is that the game's own read is copied after it
// returns and that is all -- no Acquire, no Poll, no force-feedback command, no
// device of EDVR's own.
unsigned g_trapCalls = 0;
HRESULT STDMETHODCALLTYPE trapSlot() { ++g_trapCalls; return E_NOTIMPL; }

struct Stick {
    void**   table;
    ULONG    refs = 1;
    DWORD    type = DI8DEVTYPE_JOYSTICK;
    uint8_t  state[kJoyState2Size]{};       // what the game's read returns
    DWORD    stateBytes = kJoyState2Size;   // how much of it
    DIDEVICEOBJECTDATA rows[6]{};           // what the game's buffered read returns
    DWORD    rowCount = 0;
    HRESULT  result = DI_OK;
    // A read whose out page the fake makes unreadable after its own write: the watch's
    // copy of the game's buffer then faults inside its own guard (the game's call returns).
    bool     faultRead = false;
    DWORD    nButtons = 32, nAxes = 6, nPovs = 1;   // the shape GetCapabilities reports
    unsigned stateCalls = 0, dataCalls = 0, capsCalls = 0, infoCalls = 0;
    void button(int n, bool down) { state[kJoyOfsButtons + n] = down ? 0x80 : 0; }
    void hat(int n, uint32_t v) { memcpy(state + kJoyOfsPov + 4 * n, &v, 4); }
};
ULONG STDMETHODCALLTYPE stickRetain(Stick* s) { return ++s->refs; }
ULONG STDMETHODCALLTYPE stickRelease(Stick* s) { return --s->refs; }
HRESULT STDMETHODCALLTYPE stickCaps(Stick* s, DIDEVCAPS* c) {
    ++s->capsCalls;
    if (!c || c->dwSize != sizeof(*c)) return DIERR_INVALIDPARAM;
    c->dwDevType = s->type;
    c->dwButtons = s->nButtons;
    c->dwAxes = s->nAxes;
    c->dwPOVs = s->nPovs;
    return DI_OK;
}
HRESULT STDMETHODCALLTYPE stickState(Stick* s, DWORD bytes, void* out) {
    ++s->stateCalls;
    if (FAILED(s->result)) return s->result;
    if (!out) return DIERR_INVALIDPARAM;
    DWORD old = 0;
    if (s->faultRead) VirtualProtect(out, bytes, PAGE_READWRITE, &old);   // a page a previous read left unreadable
    memcpy(out, s->state, bytes < sizeof(s->state) ? bytes : sizeof(s->state));
    if (s->faultRead) VirtualProtect(out, bytes, PAGE_NOACCESS, &old);
    return s->result;
}
HRESULT STDMETHODCALLTYPE stickData(Stick* s, DWORD bytes, DIDEVICEOBJECTDATA* out, DWORD* count, DWORD flags) {
    ++s->dataCalls;
    if (FAILED(s->result)) return s->result;
    if (!count || bytes != sizeof(*out)) return DIERR_INVALIDPARAM;
    const DWORD n = *count < s->rowCount ? *count : s->rowCount;
    if (out) memcpy(out, s->rows, n * sizeof(*out));
    *count = n;
    if (!(flags & DIGDD_PEEK) && out) s->rowCount = 0;
    return s->result;
}
// GetDeviceInfo: nothing may call it on a controller any more. It is counted, and it
// answers as a device that will not say.
HRESULT STDMETHODCALLTYPE stickInfo(Stick* s, void*) {
    ++s->infoCalls;
    return E_NOTIMPL;
}
std::array<void*, 32> stickTable() {
    std::array<void*, 32> t;
    t.fill(reinterpret_cast<void*>(&trapSlot));
    t[0] = reinterpret_cast<void*>(&trapSlot);
    t[1] = reinterpret_cast<void*>(&stickRetain);
    t[2] = reinterpret_cast<void*>(&stickRelease);
    t[3] = reinterpret_cast<void*>(&stickCaps);
    t[9] = reinterpret_cast<void*>(&stickState);
    t[10] = reinterpret_cast<void*>(&stickData);
    t[15] = reinterpret_cast<void*>(&stickInfo);
    return t;
}
const GUID kGuidSomeStick = {0xAAAA0001, 0x1111, 0x2222, {1, 2, 3, 4, 5, 6, 7, 8}};
const GUID kGuidGhost = {0xAAAA0002, 0x1111, 0x2222, {1, 2, 3, 4, 5, 6, 7, 8}};   // never enumerated
const GUID kGuidSharedGhost = {0xAAAA0003, 0x1111, 0x2222, {1, 2, 3, 4, 5, 6, 7, 8}};   // never enumerated, on the keyboard's table
HRESULT createAs(Factory& f, const GUID& guid, void** out) {
    return reinterpret_cast<PFN_CreateDevice>(f.table[3])(&f, guid, out, nullptr);
}
HRESULT createStick(Factory& f, void** out) { return createAs(f, kGuidSomeStick, out); }
HRESULT readStick(Stick& s, DWORD bytes, uint8_t* out) {
    return reinterpret_cast<PFN_GetDeviceState>(s.table[9])(&s, bytes, out);
}
HRESULT readStickRows(Stick& s, DIDEVICEOBJECTDATA (&out)[6], DWORD& count, DWORD flags = 0) {
    count = 6;
    return reinterpret_cast<PFN_GetDeviceData>(s.table[10])(&s, sizeof(out[0]), out, &count, flags);
}
}

// The LoadLibrary shortcut's matching decision (loaded_module_match.h) against a fake module list that answers like
// GetModuleHandleEx(0): a bare name matches any loaded file of that name, a path matches only that exact file.
struct FakeModule {
    const wchar_t* path;
    HMODULE handle;
};
const FakeModule kFakeModules[] = {
    {L"c:\\a\\same_name.dll", reinterpret_cast<HMODULE>(0x1000)},
    {L"c:\\windows\\system32\\advapi32.dll", reinterpret_cast<HMODULE>(0x2000)},
};
HMODULE fakeLookupW(const wchar_t* name) {
    std::wstring want(name);
    const bool bare = want.find_first_of(L"\\/") == std::wstring::npos;
    for (const FakeModule& m : kFakeModules) {
        std::wstring have(m.path);
        if (bare) have = have.substr(have.find_last_of(L'\\') + 1);
        if (_wcsicmp(have.c_str(), want.c_str()) == 0) return m.handle;
    }
    return nullptr;
}
HMODULE fakeLookupA(const char* name) {
    return fakeLookupW(std::wstring(name, name + strlen(name)).c_str());
}
void loadedModuleMatchCases() {
    const HMODULE a = kFakeModules[0].handle, adv = kFakeModules[1].handle;
    check(matchLoadedModule(L"advapi32.dll", fakeLookupW) == adv, "wide: a bare basename returns the preloaded module");
    check(matchLoadedModule(L"advapi32", fakeLookupW) == adv, "wide: a bare name without an extension still matches");
    check(matchLoadedModule(L"c:\\a\\same_name.dll", fakeLookupW) == a, "wide: a qualified path that is loaded matches");
    check(matchLoadedModule(L"c:\\b\\same_name.dll", fakeLookupW) == nullptr,
          "wide: a qualified miss with a basename collision does not match the other module");
    check(matchLoadedModule(L"c:/b/same_name.dll", fakeLookupW) == nullptr,
          "wide: a forward-slash qualified miss does not match either");
    check(matchLoadedModule(L"b\\same_name", fakeLookupW) == nullptr,
          "wide: a relative qualified miss without an extension does not match");
    check(matchLoadedModule("advapi32.dll", fakeLookupA) == adv, "ansi: a bare basename returns the preloaded module");
    check(matchLoadedModule("c:\\a\\same_name.dll", fakeLookupA) == a, "ansi: a qualified path that is loaded matches");
    check(matchLoadedModule("c:\\b\\same_name.dll", fakeLookupA) == nullptr,
          "ansi: a qualified miss with a basename collision does not match the other module");
    check(matchLoadedModule("", fakeLookupA) == nullptr, "ansi: an empty request matches nothing");
}

// The executable's own import of XInputGetState from xinput9_1_0.dll, which the controller door patches in the real game. Taking the
// address keeps the linker from dropping the import.
DWORD (WINAPI* volatile keepPadImport)(DWORD, XINPUT_STATE*) = &XInputGetState;

static void pluginReleaseScanCases(Device& keyboard) {
    // PR #92 review: there is no moment during a plugin's release when a key it was holding reads down. The tail is
    // captured while the block is still up and the block cleared last. Another thread polls GetAsyncKeyState and the
    // DirectInput state while the release's key scan is paused inside the tail reader; every release path is checked.
        const PFN_GetAsyncKeyState savedReader = g_tailReader;
        auto releaseWhilePolling = [&](const char* path, const std::function<void()>& release) {
            physical['A'] = true;
            keyboard.keys[DIK_A] = 0x80;
            refreshReleaseTail(&physicalKey);
            inputGateSetPluginBlock(true);
            g_scanEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_scanMayContinue = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_tailReader = &pausingKey;
            g_pauseArmed = true;
            SHORT duringScan = 1;
            BYTE duringKeys[256]{};
            bool polledDuringScan = false;
            std::thread poller([&] {
                if (WaitForSingleObject(g_scanEntered, 10000) == WAIT_OBJECT_0) {
                    polledDuringScan = true;
                    duringScan = hookGetAsyncKeyState('A');
                    readState(keyboard, duringKeys);
                }
                SetEvent(g_scanMayContinue);
            });
            release();
            poller.join();
            char name[160];
            snprintf(name, sizeof(name), "%s: the poll that lands during the release's key scan was made", path);
            check(polledDuringScan, name);
            snprintf(name, sizeof(name), "%s: a key the plugin held reads up in GetAsyncKeyState during the scan", path);
            check(duringScan == 0, name);
            snprintf(name, sizeof(name), "%s: ...and in DirectInput state", path);
            check(duringKeys[DIK_A] == 0, name);
            snprintf(name, sizeof(name), "%s: ...and still up once the release is done", path);
            check(!g_pluginBlock.load() && hookGetAsyncKeyState('A') == 0, name);
            CloseHandle(g_scanEntered);
            CloseHandle(g_scanMayContinue);
            g_scanEntered = g_scanMayContinue = nullptr;
            physical['A'] = false;
            keyboard.keys[DIK_A] = 0;
            refreshReleaseTail(&physicalKey);
            g_tailReader = savedReader;
        };
        releaseWhilePolling("explicit clear (a fault or a profile switch)", [&] { inputGateSetPluginBlock(false); });
        releaseWhilePolling("lease expiry", [&] {
            g_pluginLease.store(1);
            inputGatePluginLeaseTick();
        });
        // A renewal that lands between the tick spending the lease and the release keeps the block.
        inputGateSetPluginBlock(true);
        g_pluginLease.store(kPluginLeaseFrames);
        releasePluginBlock("lease expired", true);
        check(g_pluginBlock.load(), "a renewed lease is not released by a tick that saw it spent");
        inputGateSetPluginBlock(false);
        check(!g_pluginBlock.load(), "(and an explicit clear still releases it)");

        // PR #92 review: a Present-thread refresh paused mid-scan must not publish its stale result over a newer
        // capture. The tail holds B (now released); the refresh scans past A and pauses on B; the owner thread
        // captures the physically held A and clears the plugin block; the refresh resumes.
        {
            const PFN_GetAsyncKeyState saved = g_tailReader;
            g_tailReader = &physicalKey;
            physical['B'] = true;
            physical['A'] = false;
            captureReleaseTail(&physicalKey);   // tail = {B}
            g_scanEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_scanMayContinue = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_pauseArmed = true;
            std::thread refresher([&] { refreshReleaseTail(&pausingKey); });
            const bool entered = WaitForSingleObject(g_scanEntered, 10000) == WAIT_OBJECT_0;
            check(entered, "stale refresh: the refresh reached its scan of B");
            physical['B'] = false;   // the refresh will see B up and conclude the tail is empty
            physical['A'] = true;
            inputGateSetPluginBlock(true);
            inputGateSetPluginBlock(false);   // captures A, clears the block
            SetEvent(g_scanMayContinue);
            refresher.join();
            check(releaseTailVk('A'), "stale refresh: the tail still covers A after the paused refresh publishes");
            check(hookGetAsyncKeyState('A') == 0, "stale refresh: the game still reads the held A as up");
            CloseHandle(g_scanEntered);
            CloseHandle(g_scanMayContinue);
            g_scanEntered = g_scanMayContinue = nullptr;
            physical['A'] = false;
            refreshReleaseTail(&physicalKey);
            g_tailReader = saved;
        }

        // PR #92 review, the reverse ordering: a refresh that starts while a capture is mid-write must not read the new
        // generation with the old held keys. The tail holds B (now released); A is physically held; a plugin release
        // captures on the owner thread and pauses before reading A; the refresh starts meanwhile. With the snapshot
        // under the shared lock the refresh waits for the capture; every wait here is bounded.
        {
            const PFN_GetAsyncKeyState saved = g_tailReader;
            g_tailReader = &physicalKey;
            physical['B'] = true;
            physical['A'] = false;
            captureReleaseTail(&physicalKey);   // tail = {B}
            physical['B'] = false;
            physical['A'] = true;
            inputGateSetPluginBlock(true);
            g_scanEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_scanMayContinue = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_refreshEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_tailReader = &pausingKey;
            g_pauseArmed = true;
            std::thread owner([&] { inputGateSetPluginBlock(false); });   // captures A, clears the block
            const bool paused = WaitForSingleObject(g_scanEntered, 5000) == WAIT_OBJECT_0;
            check(paused, "refresh during capture: the capture paused inside its key scan");
            std::thread refresher([&] { refreshReleaseTail(&refreshProbeKey); });
            WaitForSingleObject(g_refreshEntered, 300);   // fixed: times out, the refresh waits for the capture
            SetEvent(g_scanMayContinue);
            owner.join();
            refresher.join();
            check(releaseTailVk('A'), "refresh during capture: the tail covers A after both finish");
            check(hookGetAsyncKeyState('A') == 0, "refresh during capture: the game reads the held A as up");
            check(!g_pluginBlock.load(), "refresh during capture: the plugin block is cleared");
            CloseHandle(g_scanEntered);
            CloseHandle(g_scanMayContinue);
            CloseHandle(g_refreshEntered);
            g_scanEntered = g_scanMayContinue = g_refreshEntered = nullptr;
            physical['A'] = false;
            refreshReleaseTail(&physicalKey);
            g_tailReader = saved;
        }

        // PR #92 review: two refreshes from the same generation must not publish in reverse order. The tail covers A.
        // Refresh 1 samples A held and pauses before it publishes; refresh 2 sees A released and retires the tail; A is
        // pressed afresh; refresh 1 resumes. It must not restore the held-key bit refresh 2 removed.
        {
            const PFN_GetAsyncKeyState saved = g_tailReader;
            g_tailReader = &physicalKey;
            physical['A'] = true;
            captureReleaseTail(&physicalKey);   // tail = {A}
            g_scanEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_scanMayContinue = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_pauseArmed = true;
            std::thread first([&] { refreshReleaseTail(&sampleThenPauseKey); });
            const bool entered = WaitForSingleObject(g_scanEntered, 5000) == WAIT_OBJECT_0;
            check(entered, "two refreshes: the first reached its scan of A");
            physical['A'] = false;
            refreshReleaseTail(&physicalKey);   // the second refresh: A is up, the tail retires
            check(!releaseTailVk('A'), "two refreshes: the second refresh retired the tail");
            physical['A'] = true;               // a fresh press
            SetEvent(g_scanMayContinue);        // the first refresh resumes with its stale sample of A held
            first.join();
            check(!releaseTailVk('A'), "two refreshes: the older refresh did not resurrect the retired tail");
            check(hookGetAsyncKeyState('A') != 0, "two refreshes: the game reads the fresh A press");
            CloseHandle(g_scanEntered);
            CloseHandle(g_scanMayContinue);
            g_scanEntered = g_scanMayContinue = nullptr;
            physical['A'] = false;
            refreshReleaseTail(&physicalKey);
            g_tailReader = saved;
        }

        // PR #92 review, the other order: the stale refresh publishes FIRST. Both refreshes snapshot the same generation
        // with the tail {A}. Refresh 1 samples A held and pauses; A is released; refresh 2 samples A up and pauses;
        // refresh 1 publishes (a no-op), then refresh 2 publishes. The observed release must not be discarded.
        {
            const PFN_GetAsyncKeyState saved = g_tailReader;
            g_tailReader = &physicalKey;
            physical['A'] = true;
            captureReleaseTail(&physicalKey);   // tail = {A}
            HANDLE entered1 = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            HANDLE go1 = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            HANDLE entered2 = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            HANDLE go2 = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_scanEntered = entered1;
            g_scanMayContinue = go1;
            g_pauseArmed = true;
            std::thread first([&] { refreshReleaseTail(&sampleThenPauseKey); });
            const bool in1 = WaitForSingleObject(entered1, 5000) == WAIT_OBJECT_0;
            check(in1, "stale publishes first: refresh 1 reached its scan of A");
            physical['A'] = false;
            g_scanEntered = entered2;
            g_scanMayContinue = go2;
            g_pauseArmed = true;
            std::thread second([&] { refreshReleaseTail(&sampleThenPauseKey); });
            const bool in2 = WaitForSingleObject(entered2, 5000) == WAIT_OBJECT_0;
            check(in2, "stale publishes first: refresh 2 reached its scan of A");
            const ULONGLONG resumed1 = GetTickCount64();
            SetEvent(go1);
            first.join();                        // publishes its stale sample of A held
            // Refresh 1 must finish on go1, not on its reader's 10 s timeout: a timeout would mean it waited on the
            // wrong event and the publish order this case sets was never enforced.
            check(GetTickCount64() - resumed1 < 5000, "stale publishes first: refresh 1 resumed on its own event");
            SetEvent(go2);
            second.join();                       // publishes the observed release
            check(!releaseTailVk('A'), "stale publishes first: the observed release retired the tail");
            physical['A'] = true;                // a fresh press
            check(hookGetAsyncKeyState('A') != 0, "stale publishes first: a fresh A press reaches the game");
            CloseHandle(entered1); CloseHandle(go1); CloseHandle(entered2); CloseHandle(go2);
            g_scanEntered = g_scanMayContinue = nullptr;
            physical['A'] = false;
            refreshReleaseTail(&physicalKey);
            g_tailReader = saved;
        }

        // PR #92 review: a refresh must not resurrect a tail a cancel retired. Both drive the production cancelReleaseTail, so
        // this cannot drift from what the menu's cancel does. First the scan-paused ordering: the refresh has taken its
        // snapshot (tail {A}, A sampled held) and is parked in its scan with no lock held; the cancel lands; the refresh then
        // resumes and tries to publish its stale "A still held" result.
        {
            const PFN_GetAsyncKeyState saved = g_tailReader;
            g_tailReader = &physicalKey;
            physical['A'] = true;
            captureReleaseTail(&physicalKey);   // tail = {A}
            g_scanEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_scanMayContinue = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_pauseArmed = true;
            std::thread refresher([&] { refreshReleaseTail(&sampleThenPauseKey); });
            const bool entered = WaitForSingleObject(g_scanEntered, 5000) == WAIT_OBJECT_0;
            check(entered, "refresh racing cancel: the refresh reached its scan of A");
            cancelReleaseTail();                // the real cancel, while the refresh is parked mid-scan
            SetEvent(g_scanMayContinue);
            refresher.join();
            check(!g_releaseTail.load(), "refresh racing cancel: the cancelled tail is not resurrected by a paused scan");
            check(!releaseTailVk('A'), "refresh racing cancel: A is not covered after the cancel");
            CloseHandle(g_scanEntered);
            CloseHandle(g_scanMayContinue);
            g_scanEntered = g_scanMayContinue = nullptr;
            physical['A'] = false;
            g_tailReader = saved;
        }
        // Then the lock-parked ordering: the refresh waits on the tail lock (held here) and the real cancel queues on it too, so
        // whichever the lock admits first, the tail must end retired: the refresh either sees no tail under the lock or loses the
        // generation check.
        {
            const PFN_GetAsyncKeyState saved = g_tailReader;
            g_tailReader = &physicalKey;
            physical['A'] = true;
            captureReleaseTail(&physicalKey);   // tail = {A}
            AcquireSRWLockExclusive(&g_tailLock);
            std::thread refresher([&] { refreshReleaseTail(&physicalKey); });
            Sleep(300);                         // bounded: the refresh is now waiting on the lock
            std::thread canceller([&] { cancelReleaseTail(); });
            Sleep(300);                         // ...and so is the cancel
            ReleaseSRWLockExclusive(&g_tailLock);
            refresher.join();
            canceller.join();
            check(!g_releaseTail.load(), "refresh and cancel both waiting on the lock: the tail ends retired");
            check(!releaseTailVk('A'), "refresh and cancel both waiting on the lock: A is not covered");
            physical['A'] = false;
            g_tailReader = saved;
        }
}

static void releaseLogCases();

int main() {
    (void)keepPadImport;
    padHookCases();
    loadedModuleMatchCases();
    auto dummyTable = deviceTable(), table1 = deviceTable(), table2 = deviceTable();
    auto ftable1 = factoryTable(), ftable2 = factoryTable();
    Device dummy{dummyTable.data()}, keyboard{table1.data()}, second{table2.data()};
    Device joystick{table1.data()};
    joystick.type = DI8DEVTYPE_JOYSTICK;
    Factory factory{ftable1.data(), 1, &keyboard}, otherFactory{ftable2.data(), 1, &second};
    void* returned = nullptr;
    BYTE keys[256]{};
    DIDEVICEOBJECTDATA events[4]{};
    DWORD count = 0;

    // The old dummy's table is deliberately different from BOTH returned
    // keyboards, exactly the case that bypassed the gate under Steam overlay.
    captureFactory(&factory, true, std::make_index_sequence<kFactoryDoors>{});
    check(createThrough(factory, &returned) == DI_OK && returned == &keyboard,
          "the original factory runs and returns the same object identity");
    check(keyboard.table == table1.data() && keyboard.refs == 2 && factory.refs == 2,
          "the overlay's vptr stays intact and restore targets are retained");
    captureFactory(&otherFactory, false, std::make_index_sequence<kFactoryDoors>{});
    check(createThrough(otherFactory, &returned) == DI_OK && returned == &second,
          "a second private factory and keyboard table are captured");
    const auto hooked = keyboard.table[9];
    captureFactory(&factory, true, std::make_index_sequence<kFactoryDoors>{});
    createThrough(factory, &returned);
    check(keyboard.table[9] == hooked && keyboard.refs == 2 && factory.refs == 2,
          "observing the same tables twice neither chains to itself nor leaks references");

    dummy.keys[DIK_TAB] = keyboard.keys[DIK_TAB] = second.keys[DIK_RETURN] = 0x80;
    joystick.keys[5] = 0x80;
    inputGateSetPrivate(true);
    check(readState(keyboard, keys) == DI_OK && released(keys), "private keyboard state releases all keys");
    check(readState(second, keys) == DI_OK && released(keys), "both private device tables are filtered");
    check(readState(dummy, keys) == DI_OK && keys[DIK_TAB] == 0x80, "an unrelated dummy keeps its original table");
    check(readState(joystick, keys) == DI_OK && keys[5] == 0x80,
          "a joystick sharing the patched table retains its state");
    factory.next = &joystick;
    createThrough(factory, &returned);
    check(joystick.refs == 1, "joystick creation adds no keyboard ownership");
    const GUID kGuidMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    auto mouseTable = deviceTable();
    Device mouse{mouseTable.data()};
    mouse.type = DI8DEVTYPE_MOUSE;
    factory.next = &mouse;
    reinterpret_cast<PFN_CreateDevice>(factory.table[3])(&factory, kGuidMouse, &returned, nullptr);
    check(mouse.refs == 1 && mouse.capsCalls == 0,
          "non-keyboard GUID creation bypasses keyboard capture without querying capabilities");
    factory.next = &keyboard;

    const RuntimeProfile savedProfile = g_runtimeProfile;
    g_runtimeProfile = RuntimeProfile::Flat;
    check(runtimeProfileAllowsKey("advanced.input_gate"),
          "flat profile allowlist includes advanced.input_gate");
    check(Config::get().getBool("advanced.input_gate", true),
          "flat profile preserves default input_gate enablement");
    g_runtimeProfile = savedProfile;

    keyboard.events[0] = {DIK_TAB, 0x80, 10, 1, 0};
    keyboard.events[1] = {DIK_A, 0, 11, 2, 0};
    keyboard.events[2] = {DIK_RETURN, 0x80, 12, 3, 0};
    keyboard.count = 3;
    check(readData(keyboard, events, count, DIGDD_PEEK) == DI_OK && count == 1 &&
          events[0].dwOfs == DIK_A && events[0].dwData == 0 && keyboard.count == 3,
          "buffered peek drops downs, keeps releases and leaves the queue intact");
    check(readData(keyboard, events, count) == DI_OK && count == 1 && keyboard.count == 0,
          "buffered reads consume the original queue without leaking menu presses");
    keyboard.result = DIERR_INPUTLOST;
    memset(keys, 0x5a, sizeof(keys));
    check(readState(keyboard, keys) == DIERR_INPUTLOST && keys[0] == 0x5a,
          "input-lost results and buffers are forwarded without fabricated success");
    keyboard.result = DI_OK;
    factory.result = E_FAIL;
    returned = nullptr;
    check(createThrough(factory, &returned) == E_FAIL && returned == nullptr,
          "failed device creation is forwarded unchanged");
    factory.result = DI_OK;

    g_user.origAsync = &physicalKey;
    g_user.origKeyState = &physicalKey;
    g_user.origKeyboardState = &physicalState;
    g_user.origPeek = &nextMessage;
    physical[VK_ESCAPE] = physical[VK_TAB] = true;
    check(hookGetAsyncKeyState(VK_ESCAPE) == 0 && hookGetKeyState(VK_TAB) == 0,
          "user32 polling is private while the menu is open");
    hookGetKeyboardState(keys);
    check(released(keys), "the user32 full keyboard state is private");
    MSG msg{};
    queued.message = WM_KEYDOWN; queued.wParam = VK_ESCAPE;
    hookPeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE);
    check(msg.message == WM_NULL, "the game message pump cannot see menu keydowns");

    inputGateSetPrivate(false);
    captureReleaseTail(&physicalKey);
    keyboard.keys[DIK_ESCAPE] = 0x80;
    keyboard.keys[DIK_A] = 0x80;
    check(readState(keyboard, keys) == DI_OK && keys[DIK_ESCAPE] == 0 &&
          keys[DIK_TAB] == 0 && keys[DIK_A] == 0x80,
          "after closing, held menu keys wait for release while fresh keys work");
    check(hookGetAsyncKeyState(VK_ESCAPE) == 0 && hookGetKeyState(VK_TAB) == 0,
          "held closing keys also remain suppressed in user32");
    hookGetKeyboardState(keys);
    check(keys[VK_ESCAPE] == 0 && keys[VK_TAB] == 0, "full state respects the closing-key latch");
    queued.message = WM_CHAR; queued.wParam = 9; queued.lParam = DIK_TAB << 16;
    hookPeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE);
    check(msg.message == WM_NULL, "queued characters from a held menu key remain suppressed");
    keyboard.events[0] = {DIK_ESCAPE, 0x80, 20, 4, 0};
    keyboard.events[1] = {DIK_ESCAPE, 0, 21, 5, 0};
    keyboard.events[2] = {DIK_A, 0x80, 22, 6, 0};
    keyboard.count = 3;
    readData(keyboard, events, count);
    check(count == 2 && events[0].dwData == 0 && events[1].dwOfs == DIK_A,
          "buffered closing keys are suppressed but releases and fresh presses survive");
    physical[VK_ESCAPE] = physical[VK_TAB] = false;
    refreshReleaseTail(&physicalKey);
    physical[VK_ESCAPE] = true;
    check(hookGetAsyncKeyState(VK_ESCAPE) != 0 && readState(keyboard, keys) == DI_OK &&
          keys[DIK_ESCAPE] != 0, "release followed by a fresh press returns control to Elite");
    g_summon.store(0x80000000u | VK_F8 | (uint32_t(DIK_F8) << 8));
    keyboard.keys[DIK_F8] = 0x80;
    check(readState(keyboard, keys) == DI_OK && keys[DIK_F8] == 0 && keys[DIK_A] != 0,
          "the closed-menu summon key is still swallowed through the actual device");
    g_summon.store(0);
    check(g_gameKeyboardCalls.load() != 0, "status evidence counts actual game keyboard reads");

    // PR #92 review: a key held across a plugin's release stays up until it is physically released and pressed
    // again, exactly as after the menu closes. A key pressed after the release is reported normally.
    {
        const PFN_GetAsyncKeyState savedReader = g_tailReader;
        g_tailReader = &physicalKey;
        physical[VK_ESCAPE] = false;
        keyboard.keys[DIK_ESCAPE] = 0;
        refreshReleaseTail(&physicalKey);
        physical['A'] = true;
        keyboard.keys[DIK_A] = 0x80;
        inputGateSetPluginBlock(true);
        check(readState(keyboard, keys) == DI_OK && keys[DIK_A] == 0, "(a plugin owns input: the held key reads up)");
        inputGateSetPluginBlock(false);
        check(readState(keyboard, keys) == DI_OK && keys[DIK_A] == 0,
              "a key held across the plugin's release is still up in DirectInput state");
        check(hookGetAsyncKeyState('A') == 0 && hookGetKeyState('A') == 0,
              "...and in the user32 polls");
        hookGetKeyboardState(keys);
        check(keys['A'] == 0, "...and in the full user32 keyboard state");
        physical['B'] = true;
        keyboard.keys[DIK_B] = 0x80;
        check(readState(keyboard, keys) == DI_OK && keys[DIK_B] == 0x80 && hookGetAsyncKeyState('B') != 0,
              "a key pressed after the release is reported normally");
        physical['A'] = false;
        keyboard.keys[DIK_A] = 0;
        refreshReleaseTail(&physicalKey);
        physical['A'] = true;
        keyboard.keys[DIK_A] = 0x80;
        check(readState(keyboard, keys) == DI_OK && keys[DIK_A] == 0x80 && hookGetAsyncKeyState('A') != 0,
              "the held key reads down again once it has been released and pressed again");
        physical['A'] = physical['B'] = false;
        keyboard.keys[DIK_A] = keyboard.keys[DIK_B] = 0;
        refreshReleaseTail(&physicalKey);
        g_tailReader = savedReader;
    }

    pluginReleaseScanCases(keyboard);

    // PR #92 review: while a plugin owns input a buffered hat event is dropped unless it re-centres the hat, in
    // both representations of centred (0xFFFFFFFF, or a low word of 0xFFFF). Buttons: presses dropped, releases pass.
    auto povUnderPluginBlock = [&](Stick& js, const char* where) {
        DIDEVICEOBJECTDATA r[6]{};
        DWORD n = 0;
        g_pluginBlock.store(true);
        js.rows[0] = {kJoyOfsPov, 9000, 1, 1, 0};
        js.rows[1] = {kJoyOfsPov, 0xFFFFFFFFu, 2, 2, 0};
        js.rows[2] = {kJoyOfsPov + 4, 18000, 3, 3, 0};
        js.rows[3] = {kJoyOfsPov + 4, 0x0000FFFFu, 4, 4, 0};
        js.rows[4] = {kJoyOfsButtons + 3, 0x80, 5, 5, 0};
        js.rows[5] = {kJoyOfsButtons + 3, 0, 6, 6, 0};
        js.rowCount = 6;
        const HRESULT hr = readStickRows(js, r, n);
        g_pluginBlock.store(false);
        char name[160];
        snprintf(name, sizeof(name), "%s: only the two centred hat events and the button release survive a plugin block", where);
        check(hr == DI_OK && n == 3 && r[0].dwOfs == kJoyOfsPov && r[0].dwData == 0xFFFFFFFFu &&
                  r[1].dwOfs == kJoyOfsPov + 4 && r[1].dwData == 0x0000FFFFu &&
                  r[2].dwOfs == kJoyOfsButtons + 3 && r[2].dwData == 0,
              name);
    };

    // ---- HOTAS / joystick buttons, watched and never captured -------------------------
    //
    // Three paths to a joystick's reads: its own table (the overlay's private table, a door
    // made at CreateDevice), a table a keyboard door already holds (the shared case: the
    // keyboard door's wrapper sees every device on it), and both interface encodings of
    // GetDeviceInfo. Each is read-only: the game gets the bytes it would have got, and the
    // only calls on the device are GetCapabilities and, once a second, GetDeviceInfo.
    auto stickTable1 = stickTable();
    Stick stick{stickTable1.data()};
    auto ansiTable = stickTable();
    Stick ansi{ansiTable.data()};
    // (Each fixture lives at main's scope, at an address of its own: the watch caches a device's
    // identity by address for a second, and a block-scoped stick reusing a dead one's stack slot
    // would meet that cache.)
    auto badTable = stickTable();
    Stick bad{badTable.data()};
    // The shared-table fixtures: a keyboard and two sticks on ONE table. They outlive the block
    // that uses them because the doors made on that table are undone at shutdown, which writes
    // to it.
    auto sharedTable = stickTable();
    Stick kb3{sharedTable.data()};
    Stick js3{sharedTable.data()};
    Stick js4{sharedTable.data()};
    Stick kb4{sharedTable.data()};        // a second keyboard, made through the factory on that table
    Stick sharedGhost{sharedTable.data()};   // a controller the game never enumerated, on the same table
    // The factories live at main's scope too: their doors are undone at shutdown, which writes
    // to their tables.
    auto stickFactoryTable = factoryTable();
    Factory stickFactory{stickFactoryTable.data(), 1, nullptr};
    auto ansiFactoryTable = factoryTable();
    Factory ansiFactory{ansiFactoryTable.data(), 1, nullptr};
    {
        joyWatchReset();
        g_trapCalls = 0;
        stickFactory.wide = true;
        stickFactory.next = reinterpret_cast<Device*>(&stick);
        captureFactory(&stickFactory, true, std::make_index_sequence<kFactoryDoors>{});
        // The game enumerates the stick through the factory's own slot 4, with its own callback,
        // pvRef and answer, and then creates it. The record is the game's own; EDVR asks the stick
        // nothing.
        GameEnum seen;
        seen.answer = DIENUM_STOP;
        enumerate(stickFactory, kGuidSomeStick, DI8DEVTYPE_JOYSTICK, 0x0200231D);
        check(enumerateThrough(stickFactory, seen) == DI_OK && seen.calls == 1 && seen.seenRef == &seen,
              "the game's EnumDevices reaches its own callback, with its own pvRef");
        check(stickFactory.callbackResult == DIENUM_STOP,
              "...and the callback's DIENUM_STOP comes back to the game unchanged");
        void* made = nullptr;
        check(createStick(stickFactory, &made) == DI_OK && made == &stick, "a joystick is created through the factory unchanged");
        check(stick.table == stickTable1.data() && stick.refs == 2,
              "its private table is captured (one reference kept) and its vptr is not replaced");
        check(stickTable1[9] != reinterpret_cast<void*>(&stickState) && stickTable1[10] != reinterpret_cast<void*>(&stickData),
              "...GetDeviceState and GetDeviceData are the two entries patched");
        size_t patched = 0;
        auto original = stickTable();
        for (size_t i = 0; i < 32; ++i) patched += stickTable1[i] != original[i];
        check(patched == 2, "...and only those two: no other method of the stick is touched");

        // The game reads: byte for byte what it would have got, and Joy_12 is watched.
        uint8_t buf[kJoyState2Size];
        uint8_t expect[kJoyState2Size];
        stick.button(11, false);
        check(readStick(stick, kJoyState2Size, buf) == DI_OK, "the game's state read succeeds");
        stick.button(11, true);
        stick.hat(0, 9000);
        memcpy(expect, stick.state, sizeof(expect));
        check(readStick(stick, kJoyState2Size, buf) == DI_OK && memcmp(buf, expect, sizeof(buf)) == 0,
              "...and gets exactly the bytes the device returned");
        const uint64_t now = stampMs();
        check(joyWatchHeld(0x231D0200, 11, now), "Joy_12 of 231D0200 is watched");
        check(joyWatchHeld(0x231D0200, kJoyPovBase + 1, now), "...and the hat pointing right");
        check(!joyWatchHeld(0x231D0200, 12, now), "...and not its neighbour");
        check(stick.infoCalls == 0 && stick.capsCalls == 0,
              "the device is never asked who it is or what type it is: the game's enumeration said so");
        for (int i = 0; i < 50; ++i) readStick(stick, kJoyState2Size, buf);
        check(stick.infoCalls == 0 && stick.capsCalls == 0, "...and not on each read either");
        // The same address holding another device (a released stick whose address a new one reused):
        // the game enumerates the new object and creates it at that address. The registry takes the
        // new record over, and still nothing is called on the object.
        enumerate(stickFactory, kGuidSomeStick, DI8DEVTYPE_JOYSTICK, 0x012D231D);
        void* again = nullptr;
        check(createStick(stickFactory, &again) == DI_OK && again == &stick,
              "the same address is created again: the new record replaces the old identity");
        readStick(stick, kJoyState2Size, buf);
        check(stick.infoCalls == 0 && stick.capsCalls == 0, "...and the new object is not asked either");
        stick.button(20, true);
        readStick(stick, kJoyState2Size, buf);
        check(joyWatchHeld(0x231D012D, 20, stampMs()) && !joyWatchHeld(0x231D0200, 20, stampMs()),
              "...and its buttons are the new device's");
        stick.button(20, false);
        enumerate(stickFactory, kGuidSomeStick, DI8DEVTYPE_JOYSTICK, 0x0200231D);
        createStick(stickFactory, &again);
        readStick(stick, kJoyState2Size, buf);
        readStick(stick, kJoyState2Size, buf);
        check(g_trapCalls == 0, "nothing else on the device was called (no Acquire, no Poll, no force feedback)");

        // The game's buffer is read-only to the watch: a read into a page that faults on a write.
        {
            uint8_t* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            readStick(stick, kJoyState2Size, page);
            // (the game wrote it; from here the watch alone sees it again)
            DWORD old;
            VirtualProtect(page, 4096, PAGE_READONLY, &old);
            bool faulted = false;
            __try {
                joyWatchObserveState(&stick, 0x231D0200, page, kJoyState2Size, stampMs());
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                faulted = true;
            }
            check(!faulted, "observing a state buffer never writes to it");
            VirtualFree(page, 0, MEM_RELEASE);
        }

        // A format this build does not read: forwarded untouched, ignored, counted.
        JoyWatchStats st0, st1;
        joyWatchStats(&st0);
        uint8_t tiny[128];
        check(readStick(stick, 100, tiny) == DI_OK && memcmp(tiny, expect, 100) == 0,
              "a 100-byte custom format is forwarded as it is");
        joyWatchStats(&st1);
        check(st1.statesIgnored == st0.statesIgnored + 1 && st1.lastIgnoredSize == 100, "...and counted as a size not read");

        // Buffered reads: GetDeviceData rows are copied; the game's array is returned unchanged.
        stick.rows[0] = {kJoyOfsButtons + 4, 0x80, 1, 1, 0};
        stick.rows[1] = {kJoyOfsPov + 4, 18000, 2, 2, 0};
        stick.rows[2] = {0, 77, 3, 3, 0};   // an axis
        stick.rowCount = 3;
        DIDEVICEOBJECTDATA rows[6]{};
        DWORD nrows = 0;
        check(readStickRows(stick, rows, nrows) == DI_OK && nrows == 3 && rows[0].dwOfs == kJoyOfsButtons + 4 &&
                  rows[1].dwData == 18000 && rows[2].dwData == 77,
              "the game gets all three buffered rows, untouched");
        check(joyWatchHeld(0x231D0200, 4, stampMs()) && joyWatchHeld(0x231D0200, kJoyPovBase + 4 + 2, stampMs()),
              "...and Joy_5 and hat 2 down are watched from them");
        check(readStickRows(stick, rows, nrows) == DI_OK && nrows == 0, "an empty buffered read is forwarded as empty");
        stick.rows[0] = {kJoyOfsButtons + 4, 0, 4, 4, 0};
        stick.rowCount = 1;
        readStickRows(stick, rows, nrows, DIGDD_PEEK);
        check(!joyWatchHeld(0x231D0200, 4, stampMs()) && stick.rowCount == 1, "a peek is observed too, and leaves the queue as it was");

        // A failed read is not observed.
        stick.result = DIERR_INPUTLOST;
        stick.button(30, true);
        readStick(stick, kJoyState2Size, buf);
        check(!joyWatchHeld(0x231D0200, 30, stampMs()), "a lost-input read changes nothing in the table");
        stick.result = DI_OK;
        stick.button(30, false);

        // The menu being open touches the KEYBOARD only: a stick keeps flying.
        inputGateSetPrivate(true);
        stick.button(2, true);
        readStick(stick, kJoyState2Size, buf);
        check(buf[kJoyOfsButtons + 2] == 0x80 && joyWatchHeld(0x231D0200, 2, stampMs()),
              "with the keyboard private the stick's state is untouched and still watched");
        inputGateSetPrivate(false);
        povUnderPluginBlock(stick, "a stick on its own table");

        // The interface encoding: an A factory, so the game's enumeration is the ANSI record and
        // the A thunk is the one that records it.
        {
            captureFactory(&ansiFactory, false, std::make_index_sequence<kFactoryDoors>{});
            enumerate(ansiFactory, kGuidSomeStick, DI8DEVTYPE_JOYSTICK, 0x012D231D);   // 231D012D, the other of Sean's two
            ansiFactory.next = reinterpret_cast<Device*>(&ansi);
            void* m2 = nullptr;
            createAs(ansiFactory, kGuidSomeStick, &m2);
            ansi.button(41, true);
            readStick(ansi, kJoyState2Size, buf);
            ansi.button(0, true);
            readStick(ansi, kJoyState2Size, buf);
            check(joyWatchHeld(0x231D012D, 0, stampMs()) && joyWatchHeld(0x231D012D, 41, stampMs()) == false,
                  "an ANSI-interface stick is identified (231D012D); a button down at the first read is parked");
            check(joyWatchHeld(0x231D0200, 0, stampMs()) == false, "...and is not mistaken for the other stick");
        }

        // A device that is not a game controller is never observed, whatever it is read with.
        {
            auto t3 = stickTable();
            Stick odd{t3.data()};
            odd.type = DI8DEVTYPE_MOUSE;
            odd.button(9, true);
            enumerate(stickFactory, kGuidSomeStick, DI8DEVTYPE_MOUSE, 0x0200231D);   // the game's record says mouse
            stickFactory.next = reinterpret_cast<Device*>(&odd);
            void* m3 = nullptr;
            createStick(stickFactory, &m3);
            check(odd.table[9] == reinterpret_cast<void*>(&stickState), "a mouse made through the factory is not hooked");
            readStick(odd, kJoyState2Size, buf);
            readStick(odd, kJoyState2Size, buf);
            check(!joyWatchHeld(0x231D0200, 9, stampMs()), "...and its bytes never reach the table");
        }

        // A device the game never enumerated: not registered, not captured, and nothing is called
        // on it, whatever it does. The game's own reads still reach its table untouched.
        {
            auto ghostTable1 = stickTable();
            Stick ghost{ghostTable1.data()};
            stickFactory.next = reinterpret_cast<Device*>(&ghost);
            void* m7 = nullptr;
            check(createAs(stickFactory, kGuidGhost, &m7) == DI_OK && m7 == &ghost,
                  "a device the game never enumerated is created through the factory");
            check(ghost.table == ghostTable1.data() && ghostTable1[9] == reinterpret_cast<void*>(&stickState) &&
                      ghost.refs == 1,
                  "...its table is left alone: no capture, no reference kept");
            ghost.button(3, true);
            readStick(ghost, kJoyState2Size, buf);
            readStick(ghost, kJoyState2Size, buf);
            check(ghost.capsCalls == 0 && ghost.infoCalls == 0 && g_trapCalls == 0,
                  "...and nothing is called on it");
            check(!joyWatchHeld(0x231D0200, 3, stampMs()), "...and its buttons are not watched");
        }

        // The shared-table case: a stick on a table a keyboard door holds is watched by that
        // door's wrapper, and no second door is made for it.
        {
            kb3.type = DI8DEVTYPE_KEYBOARD;
            stickFactory.next = reinterpret_cast<Device*>(&kb3);
            void* m4 = nullptr;
            createThrough(stickFactory, &m4);   // the keyboard first: the door goes on the shared table
            size_t joyDoorsBefore = 0;
            for (const auto& j : g_joyDoors) joyDoorsBefore += j.installed;
            enumerate(stickFactory, kGuidSomeStick, DI8DEVTYPE_JOYSTICK, 0x0200231D);
            stickFactory.next = reinterpret_cast<Device*>(&js3);
            createStick(stickFactory, &m4);
            size_t joyDoorsAfter = 0;
            for (const auto& j : g_joyDoors) joyDoorsAfter += j.installed;
            check(joyDoorsAfter == joyDoorsBefore, "a stick on a table the keyboard door holds gets no door of its own");
            joyWatchReset();
            js3.button(6, false);
            readStick(js3, kJoyState2Size, buf);
            js3.button(6, true);
            readStick(js3, kJoyState2Size, buf);
            check(joyWatchHeld(0x231D0200, 6, stampMs()), "...and is watched through the keyboard door's wrapper");
            inputGateSetPrivate(true);
            kb3.state[DIK_A] = 0x80;
            uint8_t kbuf[kJoyState2Size];
            readStick(kb3, 256, kbuf);
            check(kbuf[DIK_A] == 0 && g_trapCalls == 0, "...while the keyboard on the same table is still released");
            inputGateSetPrivate(false);
            check(g_trapCalls == 0, "still no call on any device beyond caps and info");
            povUnderPluginBlock(js3, "a stick on a table the keyboard door holds");

            // PR #92 review: on the shared table the watch sees only the rows the game sees. Two presses are swallowed
            // and compacted away, a release passes; the stale tail slots left behind by the compaction must not be
            // watched as a press.
            joyWatchReset();
            readStick(js3, kJoyState2Size, buf);   // an ordinary read first, so the next one is not the device's first sight
            DIDEVICEOBJECTDATA sr[6]{};
            DWORD sn = 0;
            js3.rows[0] = {kJoyOfsButtons + 5, 0x80, 1, 1, 0};
            js3.rows[1] = {kJoyOfsButtons + 7, 0x80, 2, 2, 0};
            js3.rows[2] = {kJoyOfsButtons + 3, 0, 3, 3, 0};
            js3.rowCount = 3;
            g_pluginBlock.store(true);
            const HRESULT shr = readStickRows(js3, sr, sn);
            g_pluginBlock.store(false);
            check(shr == DI_OK && sn == 1 && sr[0].dwOfs == kJoyOfsButtons + 3,
                  "shared table: a plugin block drops the two presses and passes the release");
            check(!joyWatchHeld(0x231D0200, 5, stampMs()) && !joyWatchHeld(0x231D0200, 7, stampMs()),
                  "shared table: the watch sees only the rows the game sees, not the swallowed presses");

            // A short-format state read under a plugin block keeps its axes; only buttons are zeroed and hats centred.
            uint8_t shortBuf[64];
            memset(shortBuf, 0xCC, sizeof(shortBuf));
            const int32_t axis = 1234;
            memcpy(js3.state, &axis, 4);
            js3.button(2, true);
            js3.hat(0, 9000);
            g_pluginBlock.store(true);
            const HRESULT sthr = readStick(js3, 64, shortBuf);
            g_pluginBlock.store(false);
            int32_t gotAxis = 0;
            memcpy(&gotAxis, shortBuf, 4);
            uint32_t gotPov = 0;
            memcpy(&gotPov, shortBuf + kJoyOfsPov, 4);
            check(sthr == DI_OK && gotAxis == 1234, "shared table: a short-format state read keeps its axes under a plugin block");
            check(shortBuf[kJoyOfsButtons + 2] == 0 && gotPov == 0xFFFFFFFFu,
                  "shared table: ...with the buttons zeroed and the hat centred");
            js3.button(2, false);
            js3.hat(0, 0xFFFFFFFFu);
            memset(js3.state, 0, 4);
        }

        // The shared-vtable ghost (the review of 0.19.1): a controller the game never enumerated,
        // on the keyboard door's own table, with a GUID that is not a keyboard's. The door's read
        // wrapper reaches it. It is neither a keyboard nor a controller, so the gate does not act on
        // it, and nothing is asked of it: no GetCapabilities, no GetDeviceInfo, on any read.
        {
            stickFactory.next = reinterpret_cast<Device*>(&sharedGhost);
            void* m8 = nullptr;
            check(createAs(stickFactory, kGuidSharedGhost, &m8) == DI_OK && m8 == &sharedGhost,
                  "a never-enumerated controller is created on the keyboard door's shared table");
            sharedGhost.button(4, true);
            uint8_t gbuf[kJoyState2Size];
            readStick(sharedGhost, kJoyState2Size, gbuf);
            readStick(sharedGhost, kJoyState2Size, gbuf);
            check(sharedGhost.capsCalls == 0 && sharedGhost.infoCalls == 0,
                  "two state reads make ZERO capability or info calls on the shared ghost");
            inputGateSetPrivate(true);
            readStick(sharedGhost, kJoyState2Size, gbuf);
            inputGateSetPrivate(false);
            check(gbuf[kJoyOfsButtons + 4] == 0x80 && memcmp(gbuf, sharedGhost.state, kJoyState2Size) == 0,
                  "...the gate does not act on it, even with the menu open: its bytes reach the game as they are");
            check(sharedGhost.capsCalls == 0 && sharedGhost.infoCalls == 0,
                  "...and still no call on it after the menu closes");
        }

        // The keyboard case of the same table: a second keyboard made through the keyboard GUID is
        // still a keyboard. Its one GetCapabilities call is at CreateDevice; its reads make none, and
        // its state is released while the menu is open.
        {
            kb4.type = DI8DEVTYPE_KEYBOARD;
            stickFactory.next = reinterpret_cast<Device*>(&kb4);
            void* m9 = nullptr;
            createThrough(stickFactory, &m9);
            const unsigned kb4CapsAtCreate = kb4.capsCalls;
            kb4.state[DIK_A] = 0x80;
            uint8_t kb4buf[kJoyState2Size];
            inputGateSetPrivate(true);
            readStick(kb4, 256, kb4buf);
            readStick(kb4, 256, kb4buf);
            inputGateSetPrivate(false);
            check(kb4buf[DIK_A] == 0 && kb4.capsCalls == kb4CapsAtCreate && kb4CapsAtCreate == 1,
                  "a second keyboard on the shared table is still released; asked once, at CreateDevice, never on a read");
        }

        // Faults: the watch's copy of the game's buffer faults when the buffer's page cannot be read
        // (the fake makes it unreadable after the game's own write). That is contained on BOTH paths
        // a stick can be read by -- the wrapper of a keyboard door on a shared table (js4) and the
        // observer-only door on a table of its own (bad) -- and each is charged to the joystick
        // watch's own budget. Two faults on each path spend it (four), which only happens if both
        // paths charge it and neither charges the keyboard door's. The game's reads go on throughout.
        {
            uint8_t* faultPage = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            js4.faultRead = true;
            bad.faultRead = true;
            // Both registered through the factory, as the game creates them: js4 on the shared table
            // (no second capture there, but it is still registered), bad on its own.
            enumerate(stickFactory, kGuidSomeStick, DI8DEVTYPE_JOYSTICK, 0x0200231D);
            stickFactory.next = reinterpret_cast<Device*>(&js4);
            void* m6 = nullptr;
            createStick(stickFactory, &m6);
            stickFactory.next = reinterpret_cast<Device*>(&bad);
            void* m5 = nullptr;
            createStick(stickFactory, &m5);
            joyWatchReset();
            check(g_budgetJoy.shouldRun(), "(the joystick watch's budget is whole before the faults)");
            for (int i = 0; i < 2; ++i) {
                check(readStick(js4, kJoyState2Size, faultPage) == DI_OK, "a faulting copy on the shared path never breaks the game's read");
            }
            check(g_budgetJoy.shouldRun(), "...two faults leave the budget standing");
            for (int i = 0; i < 2; ++i) {
                check(readStick(bad, kJoyState2Size, faultPage) == DI_OK, "...nor on the private path");
            }
            check(!g_budgetJoy.shouldRun(), "...four, split across both paths, spend the joystick watch's budget");
            check(g_budgetDi.shouldRun(), "...and the keyboard door's is untouched");
            js4.faultRead = false;
            bad.faultRead = false;
            VirtualFree(faultPage, 0, MEM_RELEASE);
            bad.button(8, true);
            readStick(bad, kJoyState2Size, buf);
            check(!joyWatchHeld(0x231D0200, 8, stampMs()), "a retired watch observes nothing more");
            keyboard.keys[DIK_B] = 0x80;
            inputGateSetPrivate(true);
            check(readState(keyboard, keys) == DI_OK && released(keys), "...and the keyboard gate is still private");
            inputGateSetPrivate(false);
        }
    }
    // A real executable IAT and both DirectInput8 interface encodings. This
    // verifies the early callback reaches CreateDevice without a dummy hook.
    inputGateInstallEarly();
    check(g_createImport.applied, "early bootstrap patches this executable's DirectInput import");
    IDirectInput8W* realW = nullptr;
    HRESULT hr = DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                                   kIidDirectInput8W, reinterpret_cast<void**>(&realW), nullptr);
    check(SUCCEEDED(hr) && realW, "the real Unicode factory passes through the import hook");
    IDirectInputDevice8W* realKeyboard = nullptr;
    if (realW) {
        hr = realW->CreateDevice(kGuidSysKeyboard, &realKeyboard, nullptr);
        check(SUCCEEDED(hr) && realKeyboard, "the real Unicode keyboard is created normally");
        bool found = false;
        for (const auto& d : g_gameDi) found |= d.installed && d.dummy == realKeyboard;
        check(found, "the returned runtime keyboard is captured before reaching the caller");
        // The real EnumDevices: the game's own pvRef and callback answer come back through the thunk.
        GameEnum real;
        hr = realW->EnumDevices(DI8DEVCLASS_ALL, reinterpret_cast<LPDIENUMDEVICESCALLBACKW>(&gameEnumW), &real,
                                DIEDFL_ALLDEVICES);
        check(SUCCEEDED(hr) && (real.calls == 0 || real.seenRef == &real),
              "the real Unicode EnumDevices hands the game's own pvRef through the thunk");
        if (realKeyboard) realKeyboard->Release();
        realW->Release();
    }
    IDirectInput8A* realA = nullptr;
    hr = DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                           kIidDirectInput8A, reinterpret_cast<void**>(&realA), nullptr);
    check(SUCCEEDED(hr) && realA, "the real ANSI factory also passes through the import hook");
    if (realA) {
        IDirectInputDevice8A* dev = nullptr;
        hr = realA->CreateDevice(kGuidSysKeyboard, &dev, nullptr);
        check(SUCCEEDED(hr) && dev, "the real ANSI keyboard is created normally");
        if (dev) dev->Release();
        realA->Release();
    }
    inputGateShutdown();
    check(table1[9] == reinterpret_cast<void*>(&state) && table2[10] == reinterpret_cast<void*>(&data),
          "shutdown restores all device tables");
    check(ftable1[3] == reinterpret_cast<void*>(&createDevice) && ftable1[kSlotEnumDevices] == reinterpret_cast<void*>(&enumDevices) &&
          factory.refs == 1 &&
          keyboard.refs == 1 && second.refs == 1, "shutdown restores factories and balances retained references");
    check(!g_createImport.applied, "shutdown restores the executable import");
    {
        size_t differs = 0;
        auto original = stickTable();
        for (size_t i = 0; i < 32; ++i) {
            differs += stickTable1[i] != original[i] || ansiTable[i] != original[i] || badTable[i] != original[i] ||
                       sharedTable[i] != original[i];
        }
        check(differs == 0 && stick.refs == 1 && ansi.refs == 1 && bad.refs == 1 && kb3.refs == 1,
              "shutdown restores the joystick tables and gives back the references it held");
    }
    // PR #92 review: an input-filtering plugin gets the user32 doors without the menu ever opening; with no
    // plugin they stay out until it does.
    {
        inputGateInstallEarly();
        check(!g_user.asyncKey.applied && !g_installTried,
              "with no input-filtering plugin and F8 never opened the user32 doors are not installed");
        inputGateInstallForPlugin();   // what the plugin manager calls when an addon with a filter initialises
        check(g_user.asyncKey.applied && !g_installTried,
              "an input-filtering plugin installs the user32 doors with the menu still never opened");
        void* const asyncOriginal = g_user.asyncKey.original;
        inputGateInstallForPlugin();
        check(g_user.asyncKey.original == asyncOriginal, "...and a second plugin does not install them again");
        g_user.origAsync = &physicalKey;
        physical[VK_F9] = true;
        check((GetAsyncKeyState(VK_F9) & 0x8000) != 0, "(the installed door passes a held key while no plugin owns input)");
        const uint32_t before = g_user.asyncCalls.load();
        inputGateSetPluginBlock(true);
        check(GetAsyncKeyState(VK_F9) == 0 && g_user.asyncCalls.load() == before + 1,
              "the executable's own GetAsyncKeyState call reaches the door and reads a held key up while a plugin owns input");
        inputGateSetPluginBlock(false);
        physical[VK_F9] = false;
        inputGateShutdown();
        check(!g_user.asyncKey.applied, "(shutdown takes the plugin-installed doors out)");
    }
    // Review finding 7, through the real setup order: the loader attach, the VR menu's first inputGateInstall, then the flat menu's. The
    // controller door is the executable's real XINPUT9_1_0 import (the rig links it for this), so "installed" is observable.
    {
        const RuntimeProfile before = g_runtimeProfile;
        check(!g_installTried && !g_padTried && !g_padInstalled.load(), "(nothing has run the menu's gate setup yet)");
        inputGateInstallEarly();
        g_runtimeProfile = RuntimeProfile::Vr;
        inputGateInstall();
        check(g_installTried && g_gateEnabled && !g_padTried && !g_padInstalled.load() && !g_padImport.applied,
              "a VR menu opening installs the keyboard doors and leaves the controller door alone");
        const bool diW = g_diW.installed, diA = g_diA.installed;
        void* const asyncKey = g_user.asyncKey.original;
        void* const peek = g_user.peek.original;
        void* const create = g_createImport.original;
        g_runtimeProfile = RuntimeProfile::Flat;
        inputGateInstall();
        check(g_padTried && g_padInstalled.load() && g_padImport.applied,
              "the flat menu opened after a VR one still installs the controller door");
        check(g_diW.installed == diW && g_diA.installed == diA && g_user.asyncKey.original == asyncKey &&
              g_user.peek.original == peek && g_createImport.original == create,
              "and does not install the keyboard doors a second time");
        const void* const padOriginal = g_padImport.original;
        inputGateInstall();
        check(g_padImport.original == padOriginal && g_padInstalled.load(),
              "a second flat opening does not install the controller door again");
        inputGateShutdown();
        check(!g_padImport.applied && !g_padInstalled.load(), "shutdown takes the controller door out");
        g_runtimeProfile = before;
    }
    // PR #92 review: a plugin's ownership of input is a lease. Renewing it each frame keeps it; a plugin
    // that stops renewing (no render pass, flat profile) loses it after one lease period.
    {
        inputGateSetPluginBlock(true);
        check(g_pluginBlock.load(), "(a plugin takes input)");
        for (int frame = 0; frame < kPluginLeaseFrames * 3; ++frame) {
            inputGatePluginLeaseTick();
            inputGateSetPluginBlock(true);   // renewed by the plugin's filter call this frame
        }
        check(g_pluginBlock.load(), "a plugin that renews every frame keeps input past several lease periods");
        for (int frame = 0; frame < kPluginLeaseFrames - 1; ++frame) inputGatePluginLeaseTick();
        check(g_pluginBlock.load(), "an unrenewed lease still holds until its period is spent");
        inputGatePluginLeaseTick();
        check(!g_pluginBlock.load(), "the gate is clear once a lease period passes with no renewal");
        inputGateSetPluginBlock(true);
        inputGateSetPluginBlock(false);
        check(!g_pluginBlock.load(), "an explicit release clears the gate at once");
        inputGateSetPluginBlock(true);
        inputGateShutdown();
        check(!g_pluginBlock.load(), "shutdown clears a block still held");
    }
    releaseLogCases();
    printf("\nINPUT GATE TEST %s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}

// PR #92 review follow-up: the release line is logged for the first five releases only, and the shutdown says the total.
// (Its own function: main holds __try frames, which cannot share a function with objects that unwind.)
static void releaseLogCases() {
    {
        inputGateShutdown();   // earlier cases' releases are summarised (and the count reset) before this log opens
        wchar_t tmp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tmp);
        const std::filesystem::path dir = std::filesystem::path(tmp) / (L"edvr_igt_log_" + std::to_wstring(GetCurrentProcessId()));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        check(Log::get().open(dir.wstring(), L"igt"), "(the rig's log opens to capture the release lines)");
        for (int i = 0; i < 7; ++i) {
            inputGateSetPluginBlock(true);
            inputGateSetPluginBlock(false);
        }
        inputGateSetPluginBlock(false);   // not a transition: no line, no count
        inputGateShutdown();
        Log::get().close();
        int releaseLines = 0, summaryLines = 0;
        bool summaryIsSeven = false;
        for (const auto& f : std::filesystem::directory_iterator(dir)) {
            std::ifstream in(f.path());
            std::string line;
            while (std::getline(in, line)) {
                if (line.find("keyboard gate: plugin input ownership released (") != std::string::npos) ++releaseLines;
                if (line.find("keyboard gate: plugin input ownership released 7 times this session.") != std::string::npos) summaryIsSeven = true;
                if (line.find("times this session") != std::string::npos) ++summaryLines;
            }
        }
        check(releaseLines == 5, "seven releases log five lines, then stay silent");
        check(summaryLines == 1 && summaryIsSeven, "shutdown logs one summary line with the total of seven");
        std::filesystem::remove_all(dir, ec);
    }
}

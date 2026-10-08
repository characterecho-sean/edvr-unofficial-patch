// Exercise the production hooks with distinct overlay-style COM tables,
// then the executable's real DirectInput import. No keys are injected.
#include "../../src/d3d11/input_gate.cpp"

#include <array>

using namespace edvr;

namespace {
int failures = 0;
void check(bool value, const char* name) {
    printf("  %s  %s\n", value ? "ok  " : "FAIL", name);
    if (!value) ++failures;
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
};
ULONG STDMETHODCALLTYPE retainFactory(Factory* f) { return ++f->refs; }
ULONG STDMETHODCALLTYPE releaseFactory(Factory* f) { return --f->refs; }
HRESULT STDMETHODCALLTYPE createDevice(Factory* f, REFGUID, void** out, LPUNKNOWN) {
    ++f->calls;
    if (FAILED(f->result)) return f->result;
    *out = f->next;
    return f->result;
}
std::array<void*, 11> factoryTable() {
    std::array<void*, 11> t;
    t.fill(reinterpret_cast<void*>(&unused));
    t[1] = reinterpret_cast<void*>(&retainFactory);
    t[2] = reinterpret_cast<void*>(&releaseFactory);
    t[3] = reinterpret_cast<void*>(&createDevice);
    return t;
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
    DWORD    infoSize = sizeof(DIDEVICEINSTANCEW);   // the interface this object is (W, or A)
    DWORD    product = 0x0200231D;          // Data1 of the product GUID: 231D0200
    bool     infoFaults = false;
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
    memcpy(out, s->state, bytes < sizeof(s->state) ? bytes : sizeof(s->state));
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
// GetDeviceInfo, A or W by the size it is handed: the field the watch reads is at the
// same offset in both (dwSize, guidInstance, guidProduct).
HRESULT STDMETHODCALLTYPE stickInfo(Stick* s, void* info) {
    ++s->infoCalls;
    if (s->infoFaults) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    DWORD size = 0;
    memcpy(&size, info, sizeof(size));
    if (size != s->infoSize) return DIERR_INVALIDPARAM;
    GUID product = {s->product, 0, 0, {0, 0, 'P', 'I', 'D', 'V', 'I', 'D'}};
    memcpy(static_cast<uint8_t*>(info) + 4 + sizeof(GUID), &product, sizeof(GUID));
    if (size == sizeof(DIDEVICEINSTANCEW)) {
        wcscpy_s(static_cast<DIDEVICEINSTANCEW*>(info)->tszProductName, L"Test Stick");
    } else {
        strcpy_s(static_cast<DIDEVICEINSTANCEA*>(info)->tszProductName, "Test Stick");
    }
    return DI_OK;
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
HRESULT createStick(Factory& f, void** out) {
    return reinterpret_cast<PFN_CreateDevice>(f.table[3])(&f, kGuidSomeStick, out, nullptr);
}
HRESULT readStick(Stick& s, DWORD bytes, uint8_t* out) {
    return reinterpret_cast<PFN_GetDeviceState>(s.table[9])(&s, bytes, out);
}
HRESULT readStickRows(Stick& s, DIDEVICEOBJECTDATA (&out)[6], DWORD& count, DWORD flags = 0) {
    count = 6;
    return reinterpret_cast<PFN_GetDeviceData>(s.table[10])(&s, sizeof(out[0]), out, &count, flags);
}
}

int main() {
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
    captureFactory(&factory, std::make_index_sequence<kFactoryDoors>{});
    check(createThrough(factory, &returned) == DI_OK && returned == &keyboard,
          "the original factory runs and returns the same object identity");
    check(keyboard.table == table1.data() && keyboard.refs == 2 && factory.refs == 2,
          "the overlay's vptr stays intact and restore targets are retained");
    captureFactory(&otherFactory, std::make_index_sequence<kFactoryDoors>{});
    check(createThrough(otherFactory, &returned) == DI_OK && returned == &second,
          "a second private factory and keyboard table are captured");
    const auto hooked = keyboard.table[9];
    captureFactory(&factory, std::make_index_sequence<kFactoryDoors>{});
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

    const auto savedProfile = g_runtimeProfile;
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
    {
        joyWatchReset();
        g_trapCalls = 0;
        auto stickFactoryTable = factoryTable();
        Factory stickFactory{stickFactoryTable.data(), 1, nullptr};
        stickFactory.next = reinterpret_cast<Device*>(&stick);
        captureFactory(&stickFactory, std::make_index_sequence<kFactoryDoors>{});
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
        check(stick.infoCalls <= 2 && stick.capsCalls >= 2,
              "the device was asked who it is (once, W then A at worst) and what type it is");
        const unsigned infoBefore = stick.infoCalls;
        for (int i = 0; i < 50; ++i) readStick(stick, kJoyState2Size, buf);
        check(stick.infoCalls == infoBefore, "...and not again on each read");
        // The same address holding another device (a released stick whose address a new one reused):
        // it reports another shape, so it is asked who it is again.
        stick.nButtons = 24;
        stick.product = 0x012D231D;
        readStick(stick, kJoyState2Size, buf);
        check(stick.infoCalls == infoBefore + 1, "an address that now reports another shape is identified again");
        stick.button(20, true);
        readStick(stick, kJoyState2Size, buf);
        check(joyWatchHeld(0x231D012D, 20, stampMs()) && !joyWatchHeld(0x231D0200, 20, stampMs()),
              "...and its buttons are the new device's");
        stick.button(20, false);
        stick.nButtons = 32;
        stick.product = 0x0200231D;
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

        // The interface encoding: an object that answers only the ANSI size.
        {
            ansi.infoSize = sizeof(DIDEVICEINSTANCEA);
            ansi.product = 0x012D231D;   // 231D012D, the other of Sean's two
            stickFactory.next = reinterpret_cast<Device*>(&ansi);
            void* m2 = nullptr;
            createStick(stickFactory, &m2);
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
            stickFactory.next = reinterpret_cast<Device*>(&odd);
            void* m3 = nullptr;
            createStick(stickFactory, &m3);
            check(odd.table[9] == reinterpret_cast<void*>(&stickState), "a mouse made through the factory is not hooked");
            readStick(odd, kJoyState2Size, buf);
            readStick(odd, kJoyState2Size, buf);
            check(!joyWatchHeld(0x231D0200, 9, stampMs()), "...and its bytes never reach the table");
        }

        // The shared-table case: a stick on a table a keyboard door holds is watched by that
        // door's wrapper, and no second door is made for it.
        {
            kb3.type = DI8DEVTYPE_KEYBOARD;
            js3.product = 0x0200231D;
            stickFactory.next = reinterpret_cast<Device*>(&kb3);
            void* m4 = nullptr;
            createThrough(stickFactory, &m4);   // the keyboard first: the door goes on the shared table
            size_t joyDoorsBefore = 0;
            for (const auto& j : g_joyDoors) joyDoorsBefore += j.installed;
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
        }

        // Faults: a GetDeviceInfo that crashes is contained on BOTH paths a stick can be read by --
        // the wrapper of a keyboard door on a shared table (js4) and the observer-only door on a
        // table of its own (bad) -- and each is charged to the joystick watch's own budget. Two
        // faults on each path spend it (four), which only happens if both paths charge it and
        // neither charges the keyboard door's. The game's reads go on throughout.
        {
            js4.infoFaults = true;
            js4.product = 0x0200231D;
            bad.infoFaults = true;
            bad.product = 0x0200231D;
            stickFactory.next = reinterpret_cast<Device*>(&bad);
            void* m5 = nullptr;
            createStick(stickFactory, &m5);
            joyWatchReset();
            check(g_budgetJoy.shouldRun(), "(the joystick watch's budget is whole before the faults)");
            for (int i = 0; i < 2; ++i) {
                check(readStick(js4, kJoyState2Size, buf) == DI_OK, "a faulting identity query on the shared path never breaks the game's read");
            }
            check(g_budgetJoy.shouldRun(), "...two faults leave the budget standing");
            for (int i = 0; i < 2; ++i) {
                check(readStick(bad, kJoyState2Size, buf) == DI_OK, "...nor on the private path");
            }
            check(!g_budgetJoy.shouldRun(), "...four, split across both paths, spend the joystick watch's budget");
            check(g_budgetDi.shouldRun(), "...and the keyboard door's is untouched");
            js4.infoFaults = false;
            bad.infoFaults = false;
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
    check(ftable1[3] == reinterpret_cast<void*>(&createDevice) && factory.refs == 1 &&
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
    printf("\nINPUT GATE TEST %s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}

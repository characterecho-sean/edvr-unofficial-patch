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
    BYTE keys[256]{};
    DIDEVICEOBJECTDATA events[4]{};
    DWORD count = 0;
};
ULONG STDMETHODCALLTYPE retainDevice(Device* d) { return ++d->refs; }
ULONG STDMETHODCALLTYPE releaseDevice(Device* d) { return --d->refs; }
HRESULT STDMETHODCALLTYPE caps(Device* d, DIDEVCAPS* c) {
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
    factory.next = &keyboard;

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
    printf("\nINPUT GATE TEST %s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}

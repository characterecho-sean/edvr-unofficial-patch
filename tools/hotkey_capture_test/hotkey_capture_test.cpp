// The settings menu's Hotkeys page, below the menu: the value formats a hotkey can
// have and what they do to edvr.ini, the pad/joystick edge, the joystick
// observation table, the capture state machine, the checks on a captured binding,
// Elite's bindings read for the same three kinds of device, and which generated
// rows are on the page in which tier. Everything here is pure or synthetic: no
// keyboard is read, no device is opened, no game is touched.
//
//   hotkey_capture_test.exe [<repo root>]
//
// The generated row table (build\gen\menu_schema.inc) is read from the include
// path build.bat gives the compiler, so the rows checked are the rows shipped.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/common/hotkey.h"
#include "../../src/common/iniedit.h"
#include "../../src/common/pad_names.h"
#include "../../src/d3d11/elite_binds.h"
#include "../../src/d3d11/hotkey_capture.h"
#include "../../src/d3d11/joy_watch.h"
#include "../../src/d3d11/menu_schema.h"

namespace edvr {
#include "menu_schema.inc"
}

using namespace edvr;

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const char* name) {
    ++checks;
    if (!ok) {
        ++failures;
        printf("  FAIL  %s\n", name);
    }
}

void checkf(bool ok, const char* fmt, const char* a = "", const char* b = "") {
    char name[400];
    snprintf(name, sizeof(name), fmt, a, b);
    check(ok, name);
}

HotkeyBinding parse(const char* text, bool* ok = nullptr) {
    HotkeyBinding b;
    const bool r = hotkeyParseBinding(text, &b, /*quiet=*/true);
    if (ok) *ok = r;
    return b;
}

std::string format(const HotkeyBinding& b) {
    char out[64];
    hotkeyFormatBinding(b, out, sizeof(out));
    return out;
}

HotkeyBinding key(int vk, uint32_t mods = 0) {
    HotkeyBinding b;
    b.kind = HotkeyKind::Key;
    b.vk = vk;
    b.mods = mods;
    return b;
}
HotkeyBinding pad(uint16_t buttons, uint8_t trigger = 0) {
    HotkeyBinding b;
    b.kind = HotkeyKind::Pad;
    b.padButtons = buttons;
    b.padTrigger = trigger;
    return b;
}
HotkeyBinding joy(uint32_t device, uint16_t input) {
    HotkeyBinding b;
    b.kind = HotkeyKind::Joy;
    b.joyDevice = device;
    b.joyInput = input;
    return b;
}

constexpr uint32_t kGladiator = 0x231D0200;   // Sean's two sticks, as Elite writes them
constexpr uint32_t kGunfighter = 0x231D012D;

// ---------------------------------------------------------------------------
// 1. The three value formats: parse, format, and the round trip through the ini

void testFormats() {
    printf("value formats\n");
    // Keyboard: unchanged, and canonical on the way out.
    struct { const char* in; const char* out; } keyboard[] = {
        {"F5", "F5"},
        {"f5", "F5"},
        {"CTRL+SHIFT+F9", "CTRL+SHIFT+F9"},
        {"shift + ctrl + f9", "CTRL+SHIFT+F9"},
        {"ALT+CTRL+SHIFT+SPACE", "CTRL+ALT+SHIFT+SPACE"},
        {"SCROLLLOCK", "SCROLLLOCK"},
        {"PAUSE", "PAUSE"},
        {"INSERT", "INSERT"},
        {"NUMPAD5", "NUMPAD5"},
        {"PAGEDOWN", "PAGEDOWN"},
        {"RIGHT", "RIGHT"},
        {"7", "7"},
        {"m", "M"},
        {"0x91", "SCROLLLOCK"},
    };
    for (const auto& k : keyboard) {
        bool ok = false;
        const HotkeyBinding b = parse(k.in, &ok);
        checkf(ok && b.kind == HotkeyKind::Key, "keyboard \"%s\" parses", k.in);
        checkf(format(b) == k.out, "keyboard \"%s\" is written canonically", k.in);
    }
    // Every key the capture can produce, with every modifier combination, survives
    // a write into the ini and a read back to the same key. This is the check
    // behind "the next key pressed becomes the binding": a name the parser cannot
    // read back would be a hotkey that silently binds nothing.
    int roundTripped = 0, badKey = 0;
    for (int vk = 1; vk < 256; ++vk) {
        if (!hotkeyCaptureKeyEligible(vk)) continue;
        for (uint32_t mods = 0; mods < 8; ++mods) {
            const HotkeyBinding want = key(vk, mods);
            const std::string text = format(want);
            bool ok = false;
            const HotkeyBinding got = parse(text.c_str(), &ok);
            if (ok && hotkeyBindingsEqual(want, got)) {
                ++roundTripped;
            } else {
                ++badKey;
                printf("    key 0x%02X mods %u wrote \"%s\" which reads back as vk 0x%02X mods %u\n", vk, mods,
                       text.c_str(), got.vk, got.mods);
            }
        }
    }
    check(badKey == 0 && roundTripped > 8 * 90, "every capturable key, with every modifier set, survives the ini");

    // Pad: Elite's own names; the aliases read to the canonical button.
    for (const PadName& n : kPadNames) {
        bool ok = false;
        const HotkeyBinding b = parse(n.elite, &ok);
        checkf(ok && b.kind == HotkeyKind::Pad && b.padButtons == n.buttons && b.padTrigger == n.trigger,
               "pad \"%s\" parses", n.elite);
        const std::string text = format(b);
        const char* canonical = padNameOf(n.buttons, n.trigger);
        checkf(canonical && text == canonical, "pad \"%s\" is written under its canonical name", n.elite);
        checkf(hotkeyBindingsEqual(parse(text.c_str()), b), "pad \"%s\" round-trips", n.elite);
    }
    check(parse("gamepad_back").padButtons == kPadBack, "a pad name is not case sensitive");
    check(format(parse("GamePad_A")) == "GamePad_FaceDown", "the face-button aliases write the position names");

    // Joystick: the device as Elite writes it, and Joy_N counted from 1.
    check(format(joy(kGladiator, 11)) == "231D0200:Joy_12", "joystick button 12 is 231D0200:Joy_12");
    check(format(joy(kGunfighter, 41)) == "231D012D:Joy_42", "joystick button 42 (past 32) is written Joy_42");
    check(format(joy(kGladiator, 0)) == "231D0200:Joy_1", "the first button is Joy_1, not Joy_0");
    check(format(joy(kGladiator, 127)) == "231D0200:Joy_128", "the last button is Joy_128");
    check(format(joy(kGladiator, kJoyPovBase)) == "231D0200:Joy_POV1Up", "hat 1 up");
    check(format(joy(kGladiator, kJoyPovBase + 1)) == "231D0200:Joy_POV1Right", "hat 1 right");
    check(format(joy(kGladiator, kJoyPovBase + 2)) == "231D0200:Joy_POV1Down", "hat 1 down");
    check(format(joy(kGladiator, kJoyPovBase + 3)) == "231D0200:Joy_POV1Left", "hat 1 left");
    check(format(joy(kGladiator, kJoyPovBase + 4 * 3 + 3)) == "231D0200:Joy_POV4Left", "hat 4 left");
    int joyBad = 0;
    for (uint32_t device : {kGladiator, kGunfighter, 0x00000001u, 0xFFFFFFFFu, 0x1234BEADu}) {
        for (uint16_t input = 0; input < kJoyInputCount; ++input) {
            const std::string text = format(joy(device, input));
            bool ok = false;
            const HotkeyBinding got = parse(text.c_str(), &ok);
            if (!ok || got.kind != HotkeyKind::Joy || got.joyDevice != device || got.joyInput != input) {
                ++joyBad;
                printf("    \"%s\" does not round-trip\n", text.c_str());
            }
        }
    }
    check(joyBad == 0, "every joystick button and hat direction, on five devices, round-trips");
    check(parse("231d0200:joy_12").joyInput == 11 && parse("231d0200:joy_12").joyDevice == kGladiator,
          "a joystick value is not case sensitive");
    check(format(parse("231d0200:joy_12")) == "231D0200:Joy_12", "...and is written with the device in capitals");
    check(parse(" 231D0200:Joy_12 ").kind == HotkeyKind::Joy, "spaces around a joystick value are ignored");

    // Things that name nothing are refused, not bound to something else.
    for (const char* bad : {"GamePad_Nope", "GamePad_", "231D0200:Joy_0", "231D0200:Joy_129", "231D0200:Joy_XAxis",
                            "231D0200:Joy_RZAxis", "231D0200:Joy_POV5Up", "231D0200:Joy_POV1Middle",
                            "231D0200:Joy_012", "231D0200:Joy_1x", "231D0200:", "231D020:Joy_1", "WOMBAT",
                            "CTRL+", "CTRL+WOMBAT+F5"}) {
        bool ok = true;
        const HotkeyBinding b = parse(bad, &ok);
        checkf(!ok && b.kind == HotkeyKind::None, "\"%s\" is refused and binds nothing", bad);
    }
    bool ok = false;
    check(parse("", &ok).kind == HotkeyKind::None && ok, "an empty value is a choice: unbound, and fine");
    check(parse(nullptr, &ok).kind == HotkeyKind::None && ok, "a missing value is unbound too");

    // The keyboard parser must not take a pad or joystick spelling for a key (and
    // must stay quiet about it: the caller asked for a keyboard key, it is not one).
    check(virtualKeyFromName("GamePad_Back") == 0, "virtualKeyFromName says no key for a pad button");
    check(virtualKeyFromName("231D0200:Joy_12") == 0, "virtualKeyFromName says no key for a joystick button");
    check(hotkeyLooksNonKeyboard("GamePad_Back") && hotkeyLooksNonKeyboard("231D0200:Joy_3") &&
              !hotkeyLooksNonKeyboard("F5") && !hotkeyLooksNonKeyboard("CTRL+F5") &&
              !hotkeyLooksNonKeyboard("DEADBEEF") && !hotkeyLooksNonKeyboard(""),
          "hotkeyLooksNonKeyboard tells the three apart");
}

// ---------------------------------------------------------------------------
// 2. The ini round trip: the menu writes a value, the reader reads it back

const char* kSampleIni =
    "[fix]\r\n"
    "explorer_cam = on\r\n"
    "\r\n"
    "[hotkey]\r\n"
    "# the menu\r\n"
    "menu = F8\r\n"
    "explorer_cam = F5\r\n"
    "toggle_exposure = SCROLLLOCK\r\n"
    "dump_camera = PAUSE\r\n"
    "# Developer instrument. Empty is off.\r\n"
    "#dump_draws =\r\n"
    "#dump_eyes =\r\n"
    "read_game_bindings = 1\r\n"
    "\r\n"
    "[menu]\r\n"
    "developer = off\r\n";

void testIniRoundTrip() {
    printf("ini round trip\n");
    struct Case { const char* key; HotkeyBinding value; };
    const Case cases[] = {
        {"menu", key(VK_F9)},
        {"menu", key(VK_F9, kHotkeyCtrl | kHotkeyShift)},
        {"explorer_cam", pad(kPadBack)},
        {"explorer_cam", pad(0, 2)},
        {"explorer_cam", joy(kGladiator, 11)},
        {"explorer_cam", joy(kGunfighter, 41)},
        {"toggle_exposure", joy(kGladiator, kJoyPovBase + 1)},
        {"dump_camera", key(VK_F12, kHotkeyAlt)},
        {"dump_draws", key(VK_NUMLOCK)},          // a commented-out key becomes a live one
        {"dump_draws", joy(kGladiator, 0)},
        {"dump_eyes", pad(kPadLeftShoulder)},
        {"dump_eyes", key(0xBA)},                  // ';': the one the ini's own comment rule would eat
    };
    for (const Case& c : cases) {
        const std::string text = format(c.value);
        std::string source = kSampleIni;
        MergeReport report;
        const std::string updated = mergeIni(source, source, &source, {{std::string("hotkey.") + c.key, text}}, &report);
        const std::string read = iniValue(updated, std::string("hotkey.") + c.key, "<unset>");
        checkf(read == text, "hotkey.%s keeps its text through the ini (%s)", c.key, text.c_str());
        const HotkeyBinding back = parse(read.c_str());
        checkf(hotkeyBindingsEqual(back, c.value), "hotkey.%s reads back as the binding that was written", c.key);
        // Nothing else in the file moved: the other hotkeys read as they did.
        checkf(iniValue(updated, "hotkey.read_game_bindings", "<unset>") == "1", "the neighbouring key is untouched (%s)", c.key);
    }
    // Clearing writes an empty value, which reads back as unbound.
    {
        std::string source = kSampleIni;
        MergeReport report;
        const std::string updated = mergeIni(source, source, &source, {{"hotkey.explorer_cam", ""}}, &report);
        bool ok = false;
        check(parse(iniValue(updated, "hotkey.explorer_cam", "<unset>").c_str(), &ok).kind == HotkeyKind::None && ok,
              "a cleared hotkey reads back as unbound");
    }
}

// ---------------------------------------------------------------------------
// 3. Hotkey: the edge for a pad and a joystick, and the registry

bool g_held = false;
int g_readerCalls = 0;
bool fakeReader(const HotkeyBinding&) {
    ++g_readerCalls;
    return g_held;
}

void testHotkeyEdge() {
    printf("hotkey edge\n");
    hotkeySetNonKeyboardReader(&fakeReader);
    for (const char* text : {"231D0200:Joy_12", "GamePad_Back", "231D0200:Joy_POV1Up"}) {
        Hotkey h;
        h.setGameMirrored(true);   // no window to ask in a console rig
        g_held = false;
        h.setBinding(text);
        checkf(h.bound() && h.key() == 0, "\"%s\" is bound, and is not a keyboard key", text);
        check(!h.pressed(), "nothing held, nothing fires");
        g_held = true;
        check(h.pressed(), "the press fires once");
        check(!h.pressed(), "...and not again while held");
        g_held = false;
        check(!h.pressed(), "the release fires nothing");
        g_held = true;
        check(h.pressed(), "a second press fires again");
        g_held = false;
        h.pressed();
        // A new binding starts with its latch at what is held now: the button used to
        // pick it is still down when the ini reloads, and that press must not fire it.
        g_held = true;
        h.setBinding("231D012D:Joy_3");
        check(!h.pressed(), "a binding made while its button is held does not fire on that press");
        g_held = false;
        h.pressed();
        g_held = true;
        check(h.pressed(), "...but the next press does");
        g_held = false;
        h.pressed();
        // The same text again is not a change: the latch is not reset to what is held.
        g_held = false;
        h.pressed();
        g_held = true;
        h.setBinding("231D012D:Joy_3");
        check(h.pressed(), "setting the same binding again does not swallow a press");
    }
    // Focus is the keyboard's rule: a pad or joystick press with another window up is
    // missed, said once, and not fired when focus returns.
    {
        Hotkey h;
        g_held = false;
        h.setBinding("231D0200:Joy_12");
        check(!h.pressedWith(false, 0, false), "released, unfocused: nothing");
        check(!h.pressedWith(true, 0, false), "held while another window has focus: no fire");
        check(h.takeMissedWhileUnfocused(), "...but it is remembered, to be said once");
        check(!h.takeMissedWhileUnfocused(), "...once");
        check(!h.pressedWith(true, 0, true), "focus returning with the button still held does not mint a press");
        check(!h.pressedWith(false, 0, true) && h.pressedWith(true, 0, true), "the next real press fires");
    }
    // Without a reader a non-keyboard binding never fires (a rig, or the early window).
    {
        hotkeySetNonKeyboardReader(nullptr);
        Hotkey h;
        h.setGameMirrored(true);
        g_held = true;
        h.setBinding("GamePad_Back");
        check(!h.pressed(), "no reader installed: a pad binding does not fire");
        hotkeySetNonKeyboardReader(&fakeReader);
    }
    // Keyboard semantics are untouched by all of it.
    {
        Hotkey h;
        h.setBinding("CTRL+F5");
        check(!h.pressedWith(true, 0, true), "CTRL+F5 needs Ctrl");
        check(h.pressedWith(true, kHotkeyCtrl, true) == false, "...but the latch (key held without Ctrl) holds");
        check(!h.pressedWith(false, 0, true) && h.pressedWith(true, kHotkeyCtrl | kHotkeyShift, true),
              "extra modifiers held are allowed");
    }
    // Suspension: while the menu waits for a key to bind, no hotkey reports a press, and a
    // press that began meanwhile is not one afterwards.
    {
        Hotkey joyKey, padKey, kbKey;
        joyKey.setGameMirrored(true);
        padKey.setGameMirrored(true);
        kbKey.setGameMirrored(true);
        hotkeySetKeyboardReaderForTest([](int vk) { return g_held && vk == VK_F20; });
        g_held = false;
        joyKey.setBinding("231D0200:Joy_12");
        padKey.setBinding("GamePad_Back");
        kbKey.setBinding("F20");
        hotkeysSuspend(true);
        check(hotkeysSuspended(), "suspension is on");
        g_held = true;
        check(!joyKey.pressed() && !padKey.pressed(), "suspended: a pad or HOTAS press fires nothing");
        check(!kbKey.pressed(), "suspended: a key press fires nothing");
        hotkeysSuspend(false);
        check(!hotkeysSuspended(), "suspension is off");
        check(!joyKey.pressed() && !padKey.pressed() && !kbKey.pressed(),
              "...and a press that began while suspended is not a press when it ends");
        g_held = false;
        joyKey.pressed();
        padKey.pressed();
        kbKey.pressed();
        g_held = true;
        check(joyKey.pressed() && padKey.pressed() && kbKey.pressed(), "...the next press fires");
        g_held = false;
        joyKey.pressed();
        padKey.pressed();
        kbKey.pressed();
        hotkeySetKeyboardReaderForTest(nullptr);
    }
    // The registry counts live hotkeys.
    {
        int vks[16];
        auto has = [&](int vk) {
            const int n = hotkeyRegisteredKeys(vks, 16);
            for (int i = 0; i < n; ++i) if (vks[i] == vk) return true;
            return false;
        };
        const int before = hotkeyRegisteredKeys(vks, 16);
        {
            Hotkey a, b;
            a.setBinding("F20");
            b.setBinding("F20");
            check(has(VK_F20), "a bound key is in the registry");
            check(hotkeyRegisteredKeys(vks, 16) == before + 1, "two hotkeys on one key are one entry");
            a.setBinding("F21");
            check(has(VK_F20) && has(VK_F21), "...a rebind of one leaves the other's entry");
            b.setBinding("F22");
            check(!has(VK_F20), "a key nobody is bound to any more is out of the registry");
            Hotkey c = a;
            check(has(VK_F21), "a copy holds the entry too");
            a.setBinding("");
            check(has(VK_F21), "...and outlives the original's rebind");
        }
        check(hotkeyRegisteredKeys(vks, 16) == before, "a Hotkey that goes away gives its entries back");
        for (int i = 0; i < 40; ++i) {
            Hotkey h;
            char t[16];
            snprintf(t, sizeof(t), "F%d", 13 + i % 12);
            h.setBinding(t);
        }
        check(hotkeyRegisteredKeys(vks, 16) == before, "forty rebinds leave nothing behind");
    }
}

// ---------------------------------------------------------------------------
// 4. The joystick observation table, against synthetic DirectInput buffers

// A page the table is not allowed to write to: any store into it faults, so a
// "read-only" claim that is not is caught here and not in the game.
struct ReadOnlyPage {
    uint8_t* p = nullptr;
    ReadOnlyPage(const void* src, size_t n) {
        p = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        memcpy(p, src, n);
        DWORD old;
        VirtualProtect(p, 4096, PAGE_READONLY, &old);
    }
    ~ReadOnlyPage() { VirtualFree(p, 0, MEM_RELEASE); }
};

struct State2 {   // DIJOYSTATE2's front: axes, sliders, hats, buttons, then the rest
    uint8_t bytes[kJoyState2Size] = {};
    void button(int n, bool down) { bytes[kJoyOfsButtons + n] = down ? 0x80 : 0; }
    void hat(int n, uint32_t v) { memcpy(bytes + kJoyOfsPov + 4 * n, &v, 4); }
    State2() { for (int i = 0; i < 4; ++i) hat(i, 0xFFFFFFFFu); }
};

// Runs f() and says whether it faulted. Holds no object needing unwinding, so __try may live here.
template <class F>
bool runsWithoutFault(F&& f) {
    __try {
        f();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool observeState(const void* dev, uint32_t id, const State2& s, uint32_t cb, uint64_t now) {
    ReadOnlyPage page(s.bytes, sizeof(s.bytes));
    bool ok = false;
    if (!runsWithoutFault([&] { ok = joyWatchObserveState(dev, id, page.p, cb, now); }))
        check(false, "the observation wrote into the game's buffer");
    return ok;
}

struct Row { uint32_t ofs, data, ts, seq; uint64_t app; };   // DIDEVICEOBJECTDATA's shape

bool observeRows(const void* dev, uint32_t id, std::vector<Row> rows, uint64_t now) {
    rows.resize(rows.size() + 1);   // keep the pointer valid for an empty list
    ReadOnlyPage page(rows.data(), rows.size() * sizeof(Row));
    bool ok = false;
    if (!runsWithoutFault([&] {
            ok = joyWatchObserveData(dev, id, page.p, static_cast<uint32_t>(rows.size() - 1), sizeof(Row), now);
        }))
        check(false, "the observation wrote into the game's rows");
    return ok;
}

void testJoyWatch() {
    printf("joystick table\n");
    // The id Elite writes: vendor then product, from a DirectInput product GUID
    // whose Data1 is product << 16 | vendor.
    check(joyDeviceIdFromProduct(0x0200231D) == 0x231D0200, "Data1 0x0200231D is device 231D0200");
    check(joyDeviceIdFromProduct(0x012D231D) == 0x231D012D, "Data1 0x012D231D is device 231D012D");
    check(joyDeviceIdFromProduct(0xB10A044F) == 0x044FB10A, "a T16000M (044F, B10A) is 044FB10A");
    {
        char text[9];
        hotkeyJoyDeviceText(joyDeviceIdFromProduct(0x0200231D), text);
        check(strcmp(text, "231D0200") == 0, "the id is written as eight capital hex digits, as in the .binds");
        hotkeyJoyDeviceText(joyDeviceIdFromProduct(0x0402044F), text);
        check(strcmp(text, "044F0402") == 0, "a vendor id keeps its leading zero (a Warthog is 044F0402)");
        uint32_t back = 0;
        check(hotkeyJoyDeviceFromText("231D0200", &back) && back == kGladiator, "and read back");
        check(!hotkeyJoyDeviceFromText("231D020", &back) && !hotkeyJoyDeviceFromText("231D02000", &back) &&
                  !hotkeyJoyDeviceFromText("231D020G", &back),
              "a device that is not eight hex digits is refused");
    }
    check(joyDeviceTypeIsController(0x14) && joyDeviceTypeIsController(0x15) && joyDeviceTypeIsController(0x16) &&
              joyDeviceTypeIsController(0x17) && joyDeviceTypeIsController(0x18) && joyDeviceTypeIsController(0x1C) &&
              joyDeviceTypeIsController(0x11) && !joyDeviceTypeIsController(0x12) && !joyDeviceTypeIsController(0x13) &&
              !joyDeviceTypeIsController(0) && !joyDeviceTypeIsController(0x19),
          "controllers are joysticks, pads, wheels, flight sticks, supplemental devices; a mouse and a keyboard are not");

    int a = 0, b = 0;   // two device objects
    const void* stickA = &a;
    const void* stickB = &b;
    uint64_t t = 1000;

    // DIJOYSTATE2: Joy_12 and a hat.
    joyWatchReset();
    State2 s;
    check(observeState(stickA, kGladiator, s, kJoyState2Size, t), "a 272-byte state is read");
    check(!joyWatchHeld(kGladiator, 11, t), "nothing pressed");
    s.button(11, true);
    t += 11;
    observeState(stickA, kGladiator, s, kJoyState2Size, t);
    check(joyWatchHeld(kGladiator, 11, t), "byte 48 + 11 with the high bit is Joy_12 (input 11)");
    check(!joyWatchHeld(kGladiator, 10, t) && !joyWatchHeld(kGladiator, 12, t), "...and no neighbour");
    check(!joyWatchHeld(kGunfighter, 11, t), "...and not another device's Joy_12");
    s.button(11, false);
    t += 11;
    observeState(stickA, kGladiator, s, kJoyState2Size, t);
    check(!joyWatchHeld(kGladiator, 11, t), "released");
    // The last button of the 128 and a button past 32.
    s.button(127, true);
    s.button(41, true);
    t += 11;
    observeState(stickA, kGladiator, s, kJoyState2Size, t);
    check(joyWatchHeld(kGladiator, 127, t) && joyWatchHeld(kGladiator, 41, t), "Joy_128 and Joy_42");
    s.button(127, false);
    s.button(41, false);
    t += 11;
    observeState(stickA, kGladiator, s, kJoyState2Size, t);

    // Hats: a hundredth of a degree, 0xFFFF centred; a diagonal is both neighbours.
    s.hat(0, 9000);
    t += 11;
    observeState(stickA, kGladiator, s, kJoyState2Size, t);
    check(joyWatchHeld(kGladiator, kJoyPovBase + 1, t), "hat 1 at 9000 is Joy_POV1Right");
    check(!joyWatchHeld(kGladiator, kJoyPovBase + 0, t) && !joyWatchHeld(kGladiator, kJoyPovBase + 2, t) &&
              !joyWatchHeld(kGladiator, kJoyPovBase + 3, t),
          "...and no other direction");
    s.hat(0, 4500);
    t += 11;
    observeState(stickA, kGladiator, s, kJoyState2Size, t);
    check(joyWatchHeld(kGladiator, kJoyPovBase + 0, t) && joyWatchHeld(kGladiator, kJoyPovBase + 1, t) &&
              !joyWatchHeld(kGladiator, kJoyPovBase + 2, t),
          "the up-right diagonal is up and right");
    s.hat(0, 31500);
    t += 11;
    observeState(stickA, kGladiator, s, kJoyState2Size, t);
    check(joyWatchHeld(kGladiator, kJoyPovBase + 0, t) && joyWatchHeld(kGladiator, kJoyPovBase + 3, t),
          "the up-left diagonal is up and left (wraps through 0)");
    s.hat(0, 0);
    s.hat(2, 18000);
    t += 11;
    observeState(stickA, kGladiator, s, kJoyState2Size, t);
    check(joyWatchHeld(kGladiator, kJoyPovBase + 0, t) && joyWatchHeld(kGladiator, kJoyPovBase + 8 + 2, t),
          "hat 1 up and hat 3 down at once");
    s.hat(0, 0xFFFFFFFFu);
    s.hat(2, 0xFFFFFFFFu);
    s.hat(1, 0xFFFF);   // only the low word says centred
    s.hat(3, 40000);    // out of range reads as centred
    t += 11;
    observeState(stickA, kGladiator, s, kJoyState2Size, t);
    bool anyHat = false;
    for (uint16_t i = kJoyPovBase; i < kJoyInputCount; ++i) anyHat |= joyWatchHeld(kGladiator, i, t);
    check(!anyHat, "a centred hat (or a value out of range) is no direction");
    check(joyPovMatches(0, 0) && joyPovMatches(35999, 0) && joyPovMatches(4500, 0) && !joyPovMatches(4501, 0) &&
              joyPovMatches(13500, 1) && joyPovMatches(13500, 2) && !joyPovMatches(0xFFFF, 0),
          "joyPovMatches: the boundaries of a quarter turn");

    // DIJOYSTATE, 80 bytes: 32 buttons; the rest of the buffer is not there.
    joyWatchReset();
    State2 s80;
    t = 5000;
    observeState(stickA, kGladiator, s80, kJoyStateSize, t);
    s80.button(31, true);
    s80.button(40, true);   // past the 80-byte state's 32 buttons: not read
    t += 11;
    observeState(stickA, kGladiator, s80, kJoyStateSize, t);
    check(joyWatchHeld(kGladiator, 31, t), "an 80-byte state's Joy_32");
    check(!joyWatchHeld(kGladiator, 40, t), "...and byte 48 + 40 is past its end, so not a button");

    // A buffer of another size is ignored, counted, and changes nothing.
    {
        joyWatchReset();
        State2 junk;
        junk.button(3, true);
        JoyWatchStats before, after;
        joyWatchStats(&before);
        check(!observeState(stickA, kGladiator, junk, 100, t), "a 100-byte buffer is not read");
        check(!observeState(stickA, kGladiator, junk, 16, t), "...nor a mouse's 16");
        check(!observeState(stickA, kGladiator, junk, 256, t), "...nor a keyboard's 256");
        joyWatchStats(&after);
        check(after.statesIgnored == before.statesIgnored + 3 && after.lastIgnoredSize == 256, "...and each is counted");
        check(!joyWatchHeld(kGladiator, 3, t) && after.devices == 0, "...and no device appears for them");
    }

    // The first sample arms what is already down: a button held as the game starts
    // reading the device is not a press until it has been released.
    joyWatchReset();
    State2 held;
    held.button(5, true);
    t = 20000;
    observeState(stickA, kGladiator, held, kJoyState2Size, t);
    check(!joyWatchHeld(kGladiator, 5, t), "a button already down at the first read is not held yet");
    t += 11;
    observeState(stickA, kGladiator, held, kJoyState2Size, t);
    check(!joyWatchHeld(kGladiator, 5, t), "...still not, while it stays down");
    held.button(5, false);
    t += 11;
    observeState(stickA, kGladiator, held, kJoyState2Size, t);
    check(!joyWatchHeld(kGladiator, 5, t), "...released");
    held.button(5, true);
    t += 11;
    observeState(stickA, kGladiator, held, kJoyState2Size, t);
    check(joyWatchHeld(kGladiator, 5, t), "...and the next press counts");

    // A device nobody has read for a while is not held; when the game starts
    // reading it again, what is down is armed again.
    t += 11;
    check(joyWatchHeld(kGladiator, 5, t), "(still held, still read)");
    t += 3000;
    check(!joyWatchHeld(kGladiator, 5, t), "no read for three seconds: up");
    observeState(stickA, kGladiator, held, kJoyState2Size, t);
    check(!joyWatchHeld(kGladiator, 5, t), "reading resumes with the button down: armed, not a press");
    held.button(5, false);
    t += 11;
    observeState(stickA, kGladiator, held, kJoyState2Size, t);
    held.button(5, true);
    t += 11;
    observeState(stickA, kGladiator, held, kJoyState2Size, t);
    check(joyWatchHeld(kGladiator, 5, t), "a fresh press after the pause counts");

    // GetDeviceData rows: buttons at 48 + n, hats at 32 + 4n; axes and the rest.
    joyWatchReset();
    t = 40000;
    observeRows(stickA, kGladiator, {}, t);
    check(true, "an empty read is accepted (it shows the game is polling)");
    observeRows(stickA, kGladiator, {{kJoyOfsButtons + 11, 0x80, 0, 0, 0}}, t);
    check(joyWatchHeld(kGladiator, 11, t), "a row at 48 + 11 with the high bit is Joy_12 down");
    t += 11;
    observeRows(stickA, kGladiator, {{kJoyOfsButtons + 11, 0x00, 0, 0, 0}}, t);
    check(!joyWatchHeld(kGladiator, 11, t), "...and the row that clears it is up");
    t += 11;
    observeRows(stickA, kGladiator, {{kJoyOfsButtons + 1, 0x80, 0, 0, 0}, {kJoyOfsButtons + 1, 0x00, 0, 0, 0}}, t);
    check(!joyWatchHeld(kGladiator, 1, t), "a press and release in one read leaves it up");
    t += 11;
    observeRows(stickA, kGladiator, {{kJoyOfsPov, 18000, 0, 0, 0}, {kJoyOfsPov + 4, 9000, 0, 0, 0}}, t);
    check(joyWatchHeld(kGladiator, kJoyPovBase + 2, t) && joyWatchHeld(kGladiator, kJoyPovBase + 4 + 1, t),
          "hat rows: hat 1 down and hat 2 right");
    t += 11;
    observeRows(stickA, kGladiator, {{kJoyOfsPov, 0xFFFF, 0, 0, 0}}, t);
    check(!joyWatchHeld(kGladiator, kJoyPovBase + 2, t) && joyWatchHeld(kGladiator, kJoyPovBase + 4 + 1, t),
          "a hat returning to centre releases only its own directions");
    {
        JoyWatchStats before, after;
        joyWatchStats(&before);
        t += 11;
        observeRows(stickA, kGladiator, {{0, 12345, 0, 0, 0}, {4, 1, 0, 0, 0}, {24, 7, 0, 0, 0}}, t);   // axes and a slider
        joyWatchStats(&after);
        check(after.rowsIgnored == before.rowsIgnored, "axis rows are normal traffic and are not counted as ignored");
        observeRows(stickA, kGladiator, {{176, 0x80, 0, 0, 0}, {36 + 1, 0x80, 0, 0, 0}, {900, 0x80, 0, 0, 0}}, t);
        joyWatchStats(&after);
        check(after.rowsIgnored == before.rowsIgnored + 3 && after.lastIgnoredOfs == 900,
              "rows past the buttons, or between the hats, are counted and change nothing");
        for (uint16_t i = 0; i < kJoyInputCount; ++i) {
            if (i == 4 + kJoyPovBase + 1) continue;
            if (joyWatchHeld(kGladiator, i, t)) check(false, "an ignored row set an input");
        }
    }

    // Two units of one model share an id, and their buttons merge.
    joyWatchReset();
    t = 60000;
    State2 left, right;
    observeState(stickA, kGladiator, left, kJoyState2Size, t);
    observeState(stickB, kGladiator, right, kJoyState2Size, t);
    right.button(2, true);
    t += 11;
    observeState(stickB, kGladiator, right, kJoyState2Size, t);
    check(joyWatchHeld(kGladiator, 2, t), "a button on either unit of one model is that model's button");
    {
        JoyWatchStats st;
        joyWatchStats(&st);
        check(st.devices == 2, "...though they are two devices in the table");
    }

    // The table is small and fixed; a ninth device is turned away, not wedged in.
    joyWatchReset();
    t = 70000;
    int objs[12];
    State2 quiet;
    for (int i = 0; i < 12; ++i) observeState(&objs[i], 0x10000000u + i, quiet, kJoyState2Size, t);
    {
        JoyWatchStats st;
        joyWatchStats(&st);
        check(st.devices == kJoyWatchSlots && st.tableFull == 12 - kJoyWatchSlots, "more devices than slots: the extras are counted");
    }
    t += 6000;   // the first eight have not been read for six seconds
    observeState(&objs[11], 0x1000000Bu, quiet, kJoyState2Size, t);
    {
        JoyWatchStats st;
        joyWatchStats(&st);
        check(st.devices == kJoyWatchSlots && st.tableFull == 12 - kJoyWatchSlots, "...and a slot nobody has read for a while is given up");
    }

    // The capture's view: a snapshot, and the first input that is new.
    joyWatchReset();
    t = 80000;
    State2 base;
    observeState(stickA, kGladiator, base, kJoyState2Size, t);
    JoySnapshot before, after;
    joyWatchSnapshot(&before, t);
    base.button(7, true);
    base.hat(0, 27000);
    t += 11;
    observeState(stickA, kGladiator, base, kJoyState2Size, t);
    joyWatchSnapshot(&after, t);
    uint32_t id = 0;
    uint16_t input = 0;
    check(joyFirstNew(before, after, &id, &input) && id == kGladiator && input == 7, "the first new input is the lowest: Joy_8");
    check(!joyFirstNew(after, after, &id, &input), "nothing is new against itself");
    check(joyInputIn(after.dev[0], 7) && joyInputIn(after.dev[0], kJoyPovBase + 3) && !joyInputIn(after.dev[0], 8),
          "the snapshot holds the button and the hat's left");
}

// ---------------------------------------------------------------------------
// 5. The capture state machine

CaptureSnapshot snap() { return CaptureSnapshot(); }

void testCapture() {
    printf("capture\n");
    auto begin = [](HotkeyCapture& c, const CaptureSnapshot& s) { c.begin(s); };

    {   // a plain key
        HotkeyCapture c;
        begin(c, snap());
        check(c.step(snap(), true).kind == CaptureKind::Waiting, "nothing pressed: waiting");
        CaptureSnapshot s = snap();
        s.keyDown[VK_F9] = 1;
        const CaptureStep st = c.step(s, true);
        check(st.kind == CaptureKind::Captured && st.binding.kind == HotkeyKind::Key && st.binding.vk == VK_F9 &&
                  st.binding.mods == 0,
              "F9 is captured as F9");
    }
    {   // a chord: the modifiers held when the key goes down
        HotkeyCapture c;
        begin(c, snap());
        CaptureSnapshot s = snap();
        s.mods = kHotkeyCtrl | kHotkeyShift;
        check(c.step(s, true).kind == CaptureKind::Waiting, "Ctrl+Shift alone are not a binding");
        s.keyDown['K'] = 1;
        const CaptureStep st = c.step(s, true);
        check(st.kind == CaptureKind::Captured && st.binding.vk == 'K' && st.binding.mods == (kHotkeyCtrl | kHotkeyShift),
              "Ctrl+Shift+K is captured with both modifiers");
        HotkeyCapture alt;
        begin(alt, snap());
        CaptureSnapshot a = snap();
        a.mods = kHotkeyAlt;
        a.keyDown[VK_NUMPAD5] = 1;
        const CaptureStep as = alt.step(a, true);
        check(as.kind == CaptureKind::Captured && as.binding.vk == VK_NUMPAD5 && as.binding.mods == kHotkeyAlt,
              "Alt+Numpad5");
    }
    {   // keys that are not main keys: modifiers, the Windows keys, the mouse
        HotkeyCapture c;
        begin(c, snap());
        CaptureSnapshot s = snap();
        for (int vk : {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LSHIFT, VK_RCONTROL, VK_LMENU, VK_LWIN, VK_RWIN, VK_LBUTTON,
                       VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_VOLUME_UP, VK_BROWSER_BACK, VK_PACKET, 0xFF, 0})
            s.keyDown[vk] = 1;
        check(c.step(s, true).kind == CaptureKind::Waiting, "a modifier, a Windows key or a mouse button is no main key");
    }
    {   // what is held when the capture starts is parked until released: the Enter that opened it
        HotkeyCapture c;
        CaptureSnapshot s = snap();
        s.keyDown[VK_RETURN] = 1;
        begin(c, s);
        check(c.step(s, true).kind == CaptureKind::Waiting, "the Enter that started the capture is not the binding");
        s.keyDown[VK_RETURN] = 0;
        check(c.step(s, true).kind == CaptureKind::Waiting, "...nor its release");
        s.keyDown[VK_RETURN] = 1;
        const CaptureStep st = c.step(s, true);
        check(st.kind == CaptureKind::Captured && st.binding.vk == VK_RETURN, "...but pressing it again afterwards binds it");
    }
    {   // Esc cancels, Delete and Backspace clear, bare only
        HotkeyCapture c;
        begin(c, snap());
        CaptureSnapshot s = snap();
        s.keyDown[VK_ESCAPE] = 1;
        check(c.step(s, true).kind == CaptureKind::Cancelled, "a bare Esc cancels");
        HotkeyCapture d;
        begin(d, snap());
        CaptureSnapshot del = snap();
        del.keyDown[VK_DELETE] = 1;
        check(d.step(del, true).kind == CaptureKind::Cleared, "a bare Delete clears");
        HotkeyCapture e;
        begin(e, snap());
        CaptureSnapshot bs = snap();
        bs.keyDown[VK_BACK] = 1;
        check(e.step(bs, true).kind == CaptureKind::Cleared, "a bare Backspace clears");
        HotkeyCapture f;
        begin(f, snap());
        check(f.step(del, false).kind == CaptureKind::ClearRefused, "Delete on the menu key is refused (it cannot be cleared)");
        HotkeyCapture g;
        begin(g, snap());
        check(g.step(bs, false).kind == CaptureKind::ClearRefused, "...and so is Backspace");
        // with a modifier they are keys like any other
        HotkeyCapture h;
        begin(h, snap());
        CaptureSnapshot sd = snap();
        sd.mods = kHotkeyShift;
        sd.keyDown[VK_DELETE] = 1;
        const CaptureStep st = h.step(sd, true);
        check(st.kind == CaptureKind::Captured && st.binding.vk == VK_DELETE && st.binding.mods == kHotkeyShift,
              "Shift+Delete is a chord, not a clear");
        HotkeyCapture i;
        begin(i, snap());
        CaptureSnapshot ce = snap();
        ce.mods = kHotkeyCtrl;
        ce.keyDown[VK_ESCAPE] = 1;
        check(i.step(ce, true).kind == CaptureKind::Captured, "Ctrl+Esc is a chord, not a cancel");
    }
    {   // the summon key and the navigation keys are capturable here (the menu decides what to do with them)
        HotkeyCapture c;
        begin(c, snap());
        CaptureSnapshot s = snap();
        s.keyDown[VK_F8] = 1;
        check(c.step(s, true).binding.vk == VK_F8, "F8, the menu key, is capturable");
    }
    {   // a pad: the first button, else a trigger
        HotkeyCapture c;
        begin(c, snap());
        CaptureSnapshot s = snap();
        s.padButtons = kPadBack;
        const CaptureStep st = c.step(s, true);
        check(st.kind == CaptureKind::Captured && st.binding.kind == HotkeyKind::Pad && st.binding.padButtons == kPadBack &&
                  st.binding.padTrigger == 0,
              "a pad's Back is captured");
        HotkeyCapture t;
        begin(t, snap());
        CaptureSnapshot ts = snap();
        ts.padTriggers = 2;
        const CaptureStep tt = t.step(ts, true);
        check(tt.binding.kind == HotkeyKind::Pad && tt.binding.padButtons == 0 && tt.binding.padTrigger == 2,
              "the right trigger");
        HotkeyCapture two;
        begin(two, snap());
        CaptureSnapshot both = snap();
        both.padButtons = static_cast<uint16_t>(kPadA | kPadX);
        const CaptureStep tw = two.step(both, true);
        check(tw.binding.padButtons == kPadA, "two pad buttons at once: the lowest bit, one button, never a chord");
        HotkeyCapture held;
        CaptureSnapshot hs = snap();
        hs.padButtons = kPadStart;
        begin(held, hs);
        check(held.step(hs, true).kind == CaptureKind::Waiting, "a pad button held at the start is parked");
    }
    {   // a joystick: a button on a named device, and a hat
        HotkeyCapture c;
        begin(c, snap());
        CaptureSnapshot s = snap();
        s.joy.count = 1;
        s.joy.dev[0].id = kGladiator;
        s.joy.dev[0].bits[0] = 1ull << 11;
        const CaptureStep st = c.step(s, true);
        check(st.kind == CaptureKind::Captured && st.binding.kind == HotkeyKind::Joy && st.binding.joyDevice == kGladiator &&
                  st.binding.joyInput == 11 && format(st.binding) == "231D0200:Joy_12",
              "a HOTAS button is captured as device:Joy_N");
        HotkeyCapture h;
        begin(h, snap());
        CaptureSnapshot hs = snap();
        hs.joy.count = 1;
        hs.joy.dev[0].id = kGunfighter;
        hs.joy.dev[0].bits[2] = 1ull << 1;   // hat 1 right
        const CaptureStep ht = h.step(hs, true);
        check(ht.binding.joyDevice == kGunfighter && format(ht.binding) == "231D012D:Joy_POV1Right", "a hat direction");
        HotkeyCapture p;
        CaptureSnapshot ps = snap();
        ps.joy.count = 1;
        ps.joy.dev[0].id = kGladiator;
        ps.joy.dev[0].bits[0] = 1ull << 3;
        begin(p, ps);
        check(p.step(ps, true).kind == CaptureKind::Waiting, "a joystick button held at the start is parked");
        ps.joy.dev[0].bits[0] = 0;
        p.step(ps, true);
        ps.joy.dev[0].bits[0] = 1ull << 3;
        check(p.step(ps, true).kind == CaptureKind::Captured, "...and counts after a release");
    }
    {   // the keyboard wins a tie; a second input in the same look is not lost to the first
        HotkeyCapture c;
        begin(c, snap());
        CaptureSnapshot s = snap();
        s.keyDown[VK_F3] = 1;
        s.padButtons = kPadA;
        check(c.step(s, true).binding.kind == HotkeyKind::Key, "key and pad in one look: the key");
        const CaptureStep next = c.step(s, true);
        check(next.kind == CaptureKind::Waiting, "...and the pad button that came with it was already seen (one capture, one input)");
    }
    {   // an inactive capture reads nothing
        HotkeyCapture c;
        CaptureSnapshot s = snap();
        s.keyDown[VK_F3] = 1;
        check(c.step(s, true).kind == CaptureKind::Waiting && !c.active(), "no capture begun: nothing");
        begin(c, snap());
        c.end();
        check(c.step(s, true).kind == CaptureKind::Waiting, "an ended capture reads nothing");
    }
}

// ---------------------------------------------------------------------------
// 6. The checks on a captured binding

EliteBindUse use(const char* element, const HotkeyBinding& b) {
    EliteBindUse u;
    memset(&u, 0, sizeof(u));
    snprintf(u.element, sizeof(u.element), "%s", element);
    u.binding = b;
    return u;
}

void testChecks() {
    printf("checks\n");
    const HotkeyOther others[] = {
        {"hotkey.menu", key(VK_F8)},
        {"hotkey.toggle_exposure", key(VK_SCROLL)},
        {"hotkey.dump_camera", pad(kPadBack)},
        {"hotkey.dump_draws", joy(kGladiator, 3)},
        {"hotkey.dump_eyes", HotkeyBinding()},
    };
    const int n = static_cast<int>(sizeof(others) / sizeof(others[0]));

    // Exact duplicates of another EDVR hotkey are refused, naming which.
    BindCheck c = hotkeyCheckBinding(key(VK_F8), others, n, nullptr, 0, ClashScope::AnyContext);
    check(c.verdict == BindVerdict::Duplicate && strcmp(c.duplicateOf, "hotkey.menu") == 0, "F8 is the menu key: refused, and says so");
    c = hotkeyCheckBinding(key(VK_SCROLL), others, n, nullptr, 0, ClashScope::OnFoot);
    check(c.verdict == BindVerdict::Duplicate && strcmp(c.duplicateOf, "hotkey.toggle_exposure") == 0, "Scroll Lock: refused, names the exposure toggle");
    c = hotkeyCheckBinding(pad(kPadBack), others, n, nullptr, 0, ClashScope::AnyContext);
    check(c.verdict == BindVerdict::Duplicate && strcmp(c.duplicateOf, "hotkey.dump_camera") == 0, "a pad button already used: refused");
    c = hotkeyCheckBinding(joy(kGladiator, 3), others, n, nullptr, 0, ClashScope::AnyContext);
    check(c.verdict == BindVerdict::Duplicate && strcmp(c.duplicateOf, "hotkey.dump_draws") == 0, "a HOTAS button already used: refused");
    check(hotkeyCheckBinding(joy(kGunfighter, 3), others, n, nullptr, 0, ClashScope::AnyContext).verdict == BindVerdict::Ok,
          "the same button number on the other stick is a different button");
    check(hotkeyCheckBinding(key(VK_F8, kHotkeyCtrl), others, n, nullptr, 0, ClashScope::AnyContext).verdict == BindVerdict::Ok,
          "CTRL+F8 is not F8: an exact duplicate only");
    check(hotkeyCheckBinding(key(VK_F9), others, n, nullptr, 0, ClashScope::AnyContext).verdict == BindVerdict::Ok, "a free key is fine");
    check(hotkeyCheckBinding(HotkeyBinding(), others, n, nullptr, 0, ClashScope::AnyContext).verdict == BindVerdict::Ok,
          "an empty binding is never a duplicate of another empty one");

    // The keys the menu navigates with are not available to a hotkey.
    for (int vk : {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_SPACE, VK_TAB, VK_PRIOR, VK_NEXT, VK_HOME, VK_END,
                   VK_ESCAPE, static_cast<int>('R')}) {
        c = hotkeyCheckBinding(key(vk), others, n, nullptr, 0, ClashScope::AnyContext);
        char name[48];
        snprintf(name, sizeof(name), "vk 0x%02X is the menu's own key: refused", vk);
        check(c.verdict == BindVerdict::Reserved, name);
    }
    check(hotkeyCheckBinding(key(VK_TAB, kHotkeyShift), others, n, nullptr, 0, ClashScope::AnyContext).verdict == BindVerdict::Reserved,
          "Shift+Tab is the menu's previous page: refused");
    // ...WHATEVER the modifiers: the menu reads these keys raw and never asks what else is held, so a
    // chord on one is still that key to the menu, and the hotkey would fire with it.
    {
        int bad = 0;
        const int mains[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_SPACE, VK_TAB, VK_PRIOR, VK_NEXT, VK_HOME,
                             VK_END, VK_ESCAPE, 'R'};
        for (int vk : mains) {
            for (uint32_t mods = 0; mods < 8; ++mods) {
                if (hotkeyCheckBinding(key(vk, mods), others, n, nullptr, 0, ClashScope::AnyContext).verdict != BindVerdict::Reserved) {
                    ++bad;
                    printf("    vk 0x%02X with mods %u is not refused\n", vk, mods);
                }
            }
        }
        check(bad == 0, "every menu navigation or control key is refused with ANY of the eight modifier sets");
    }
    check(hotkeyCheckBinding(key(VK_RETURN, kHotkeyCtrl), others, n, nullptr, 0, ClashScope::AnyContext).verdict == BindVerdict::Reserved,
          "CTRL+ENTER (it is also the menu's Enter) is refused");
    check(hotkeyCheckBinding(key(VK_ESCAPE, kHotkeyCtrl | kHotkeyAlt), others, n, nullptr, 0, ClashScope::AnyContext).verdict ==
              BindVerdict::Reserved,
          "CTRL+ALT+ESCAPE (it would open the panel and close it in the same tick) is refused");
    check(hotkeyCheckBinding(key(VK_UP, kHotkeyCtrl), others, n, nullptr, 0, ClashScope::AnyContext).verdict == BindVerdict::Reserved,
          "CTRL+UP (it also moves the selection) is refused");
    check(hotkeyCheckBinding(key(VK_F9, kHotkeyCtrl | kHotkeyShift), others, n, nullptr, 0, ClashScope::AnyContext).verdict ==
              BindVerdict::Ok,
          "CTRL+SHIFT+F9 still binds");
    check(hotkeyCheckBinding(key(VK_BACK, kHotkeyCtrl), others, n, nullptr, 0, ClashScope::AnyContext).verdict == BindVerdict::Ok &&
              hotkeyCheckBinding(key(VK_F12, kHotkeyAlt), others, n, nullptr, 0, ClashScope::AnyContext).verdict == BindVerdict::Ok,
          "...and so do the chords on keys the menu does not read");
    check(hotkeyCheckBinding(pad(kPadA), others, n, nullptr, 0, ClashScope::AnyContext).verdict == BindVerdict::Ok &&
              hotkeyCheckBinding(joy(kGladiator, 0), others, n, nullptr, 0, ClashScope::AnyContext).verdict == BindVerdict::Ok,
          "a pad or HOTAS button is never the menu's key (the menu is navigated by the keyboard)");

    // Elite's own bindings WARN: the key is watched, not captured, so the game sees it too.
    const EliteBindUse uses[] = {
        use("HumanoidJumpButton", key(VK_SPACE)),
        use("HumanoidSprintButton", key(VK_F6)),
        use("ToggleFreeCam", key(VK_F9)),
        use("CycleFireGroupNext", key(VK_F9, kHotkeyCtrl)),
        use("PrimaryFire", joy(kGladiator, 0)),
        use("HumanoidPrimaryFireButton", joy(kGladiator, 0)),
        use("UI_Select", pad(kPadA)),
        use("HumanoidReloadButton", pad(kPadBack)),
        use("Hyperspace", key(VK_F5, kHotkeyShift)),
        use("HumanoidThrowGrenadeButton", key(VK_F5, kHotkeyCtrl)),
        use("HumanoidSecondary", key(VK_F5)),
        use("HumanoidSecondary", key(VK_F5)),   // both slots of one element: named once
    };
    const int nu = static_cast<int>(sizeof(uses) / sizeof(uses[0]));
    c = hotkeyCheckBinding(key(VK_F9), others, n, uses, nu, ClashScope::AnyContext);
    check(c.verdict == BindVerdict::Ok && c.clashCount == 2 && strstr(c.clashList, "ToggleFreeCam") &&
              strstr(c.clashList, "CycleFireGroupNext"),
          "F9 is warned about: ToggleFreeCam, and CTRL+F9 (the same key, a modifier the game wants)");
    check(!strstr(c.clashList, "HumanoidJump"), "...and not about an unrelated binding");
    c = hotkeyCheckBinding(key(VK_F9), others, n, uses, nu, ClashScope::OnFoot);
    check(c.clashCount == 1 && strstr(c.clashList, "ToggleFreeCam"),
          "on foot, a ship control that shares the key does not count (CycleFireGroupNext), a free-camera one does");
    c = hotkeyCheckBinding(key(VK_F5), others, n, uses, nu, ClashScope::OnFoot);
    check(c.verdict == BindVerdict::Ok && c.clashCount == 2 && !strstr(c.clashList, "Hyperspace"), "F5 on foot: Ctrl+F5 and F5 clash (a modifier on top of the key still presses the key); the ship's Shift+F5 does not count");
    c = hotkeyCheckBinding(key(VK_F5, kHotkeyShift), others, n, uses, nu, ClashScope::AnyContext);
    check(c.clashCount == 2, "Shift+F5 clashes with Shift+F5 and bare F5, not with Ctrl+F5");
    c = hotkeyCheckBinding(key(VK_F5), others, n, uses, nu, ClashScope::OnFoot);
    check(strstr(c.clashList, "HumanoidSecondary") != nullptr, "the on-foot element is named");
    {
        int named = 0;
        for (const char* p = c.clashList; (p = strstr(p, "HumanoidSecondary")); p += 5) ++named;
        check(named == 1, "an element on the key in both slots is named once");
    }
    c = hotkeyCheckBinding(joy(kGladiator, 0), others, n, uses, nu, ClashScope::AnyContext);
    check(c.verdict == BindVerdict::Ok && c.clashCount == 2, "a HOTAS button: PrimaryFire and HumanoidPrimaryFireButton");
    c = hotkeyCheckBinding(joy(kGladiator, 0), others, n, uses, nu, ClashScope::OnFoot);
    check(c.clashCount == 1 && strstr(c.clashList, "HumanoidPrimaryFireButton"), "...on foot, only the on-foot one");
    check(hotkeyCheckBinding(joy(kGunfighter, 0), others, n, uses, nu, ClashScope::AnyContext).clashCount == 0,
          "the same button on another stick is clear");
    c = hotkeyCheckBinding(pad(kPadBack), others, n, uses, nu, ClashScope::AnyContext);
    check(c.verdict == BindVerdict::Duplicate, "(the duplicate check comes first)");
    {
        const HotkeyOther none[1] = {{"x", HotkeyBinding()}};
        c = hotkeyCheckBinding(pad(kPadBack), none, 0, uses, nu, ClashScope::AnyContext);
        check(c.verdict == BindVerdict::Ok && c.clashCount == 1 && strstr(c.clashList, "HumanoidReloadButton"),
              "a pad button Elite uses is warned about, not refused");
    }
    // The list is bounded: a key with many uses names three and counts the rest.
    {
        std::vector<EliteBindUse> many;
        for (int i = 0; i < 9; ++i) {
            char name[40];
            snprintf(name, sizeof(name), "HumanoidThing%d", i);
            many.push_back(use(name, key(VK_F11)));
        }
        c = hotkeyCheckBinding(key(VK_F11), nullptr, 0, many.data(), static_cast<int>(many.size()), ClashScope::OnFoot);
        check(c.clashCount == 9 && strstr(c.clashList, "(+6 more)") && strstr(c.clashList, "HumanoidThing2") &&
                  !strstr(c.clashList, "HumanoidThing3"),
              "nine uses: three named and \"+6 more\"");
        check(strlen(c.clashList) < sizeof(c.clashList) - 1, "...inside the buffer");
    }
    check(eliteElementIsOnFoot("HumanoidJumpButton") && eliteElementIsOnFoot("SystemMapOpen_Humanoid") &&
              eliteElementIsOnFoot("UI_Up") && eliteElementIsOnFoot("PhotoCameraToggle") &&
              eliteElementIsOnFoot("ToggleFreeCam") && !eliteElementIsOnFoot("PrimaryFire") &&
              !eliteElementIsOnFoot("IncreaseEnginesPower_Buggy") && !eliteElementIsOnFoot("") &&
              !eliteElementIsOnFoot(nullptr),
          "which Elite elements count as on foot");
    check(hotkeyClashes(key(VK_F5), key(VK_F5)) && hotkeyClashes(key(VK_F5), key(VK_F5, kHotkeyCtrl)) &&
              hotkeyClashes(key(VK_F5, kHotkeyCtrl | kHotkeyShift), key(VK_F5, kHotkeyCtrl)) &&
              !hotkeyClashes(key(VK_F5, kHotkeyShift), key(VK_F5, kHotkeyCtrl)) && !hotkeyClashes(key(VK_F5), key(VK_F6)) &&
              !hotkeyClashes(key(VK_F5), pad(kPadBack)) && !hotkeyClashes(HotkeyBinding(), HotkeyBinding()),
          "hotkeyClashes: the modifier rule, and kinds never clash across");
}

// ---------------------------------------------------------------------------
// 6b. What a capture step does to a row, and the Explorer Cam lock

// The session flag the Explorer Cam side will own (explorerCamSessionActive()); a stub here.
bool g_session = false;
bool sessionStub() { return g_session; }

// A step, made by driving the real state machine with one new input.
CaptureStep stepFor(const CaptureSnapshot& s, bool canClear = true) {
    HotkeyCapture c;
    c.begin(CaptureSnapshot());
    return c.step(s, canClear);
}
CaptureSnapshot pressing(int vk, uint32_t mods = 0) {
    CaptureSnapshot s;
    s.keyDown[vk] = 1;
    s.mods = mods;
    return s;
}

void testDecide() {
    printf("decisions\n");
    HotkeyOther others[] = {
        {"hotkey.menu", key(VK_F8)},
        {"hotkey.toggle_exposure", key(VK_SCROLL)},
        {"hotkey.dump_camera", key(VK_PAUSE)},
        {"hotkey.dump_draws", HotkeyBinding()},
        {"hotkey.dump_eyes", HotkeyBinding()},
    };
    EliteBindUse uses[] = {use("HumanoidJumpButton", key(VK_F9)), use("HumanoidSprintButton", pad(kPadBack)),
                           use("Hyperspace", key(VK_F7))};
    // The glue lists every hotkey row but the one being captured; so does this.
    auto ctxFor = [&](const char* row, const char* current, ClashScope scope) {
        static HotkeyOther pool[16][5];   // one list per call: a context outlives the next call
        static int nextPool = 0;
        HotkeyOther* mine = pool[nextPool++ % 16];
        int n = 0;
        char self[64];
        snprintf(self, sizeof(self), "hotkey.%s", row);
        for (const HotkeyOther& o : others) {
            if (strcmp(o.dotted, self) != 0) mine[n++] = o;
        }
        HotkeyRowContext c;
        c.rowKey = row;
        c.currentText = current;
        c.others = mine;
        c.nOthers = n;
        c.uses = uses;
        c.nUses = 3;
        c.scope = scope;
        c.explorerCamSession = sessionStub();
        return c;
    };

    // Explorer Cam's key while NO session is on: free to change and to clear.
    g_session = false;
    {
        const HotkeyRowContext c = ctxFor("explorer_cam", "F5", ClashScope::OnFoot);
        check(hotkeyCaptureMayBegin(c), "no session: Enter on the Explorer Cam key row starts a capture");
        CaptureDecision d = hotkeyDecide(stepFor(pressing(VK_F10)), c);
        check(d.outcome == CaptureOutcome::Bind && d.binding.vk == VK_F10, "...F10 is written");
        d = hotkeyDecide(stepFor(pressing(VK_DELETE)), c);
        check(d.outcome == CaptureOutcome::Clear, "...Delete clears it (that turns Explorer Cam off)");
        d = hotkeyDecide(stepFor(pressing(VK_F9)), c);
        check(d.outcome == CaptureOutcome::Bind && d.check.clashCount == 1 && strstr(d.check.clashList, "HumanoidJumpButton"),
              "...a key Elite also uses is written, with the warning");
        d = hotkeyDecide(stepFor(pressing(VK_F7)), c);
        check(d.outcome == CaptureOutcome::Bind && d.check.clashCount == 0, "...a ship control on the key is no warning on foot");
    }
    // The same row WHILE a session is on: locked -- clear and change both refused, whatever the key.
    g_session = true;
    {
        const HotkeyRowContext c = ctxFor("explorer_cam", "F5", ClashScope::OnFoot);
        check(!hotkeyCaptureMayBegin(c), "session on: Enter on the Explorer Cam key row starts nothing");
        check(hotkeyDecide(stepFor(pressing(VK_F10)), c).outcome == CaptureOutcome::Locked, "...a change is refused");
        check(hotkeyDecide(stepFor(pressing(VK_DELETE)), c).outcome == CaptureOutcome::Locked, "...a clear is refused");
        check(hotkeyDecide(stepFor(pressing(VK_BACK)), c).outcome == CaptureOutcome::Locked, "...Backspace too");
        check(hotkeyDecide(stepFor(pressing(VK_F8)), c).outcome == CaptureOutcome::Locked,
              "...and the lock is the answer even for a key that would also be a duplicate");
        check(hotkeyDecide(stepFor(pressing(VK_TAB)), c).outcome == CaptureOutcome::Locked,
              "...or a reserved one");
        check(hotkeyDecide(stepFor(pressing(VK_ESCAPE)), c).outcome == CaptureOutcome::Cancelled,
              "...but Esc, which changes nothing, is just a cancel");
        check(hotkeyDecide(CaptureStep(), c).outcome == CaptureOutcome::Waiting, "...and nothing pressed is nothing");
        char words[96];
        hotkeyLockedText("F5", words, sizeof(words));
        check(strcmp(words, "Leave Explorer Cam (F5) to change its key") == 0, "the words are \"Leave Explorer Cam (F5) to change its key\"");
        hotkeyLockedText("231D0200:Joy_12", words, sizeof(words));
        check(strcmp(words, "Leave Explorer Cam (231D0200:Joy_12) to change its key") == 0, "...with the key the row holds, a HOTAS button too");
        hotkeyLockedText("", words, sizeof(words));
        check(strcmp(words, "Leave Explorer Cam (its key) to change its key") == 0, "...and a fallback for none");
        check(hotkeyRowLocked("explorer_cam", true) && !hotkeyRowLocked("explorer_cam", false), "the row is locked only while the session is");
        // No other hotkey row is ever locked by it.
        for (const char* other : {"menu", "toggle_exposure", "dump_camera", "dump_draws", "dump_eyes"}) {
            const HotkeyRowContext oc = ctxFor(other, "", ClashScope::AnyContext);
            checkf(hotkeyCaptureMayBegin(oc) && !hotkeyRowLocked(other, true), "hotkey.%s is not locked by a session", other);
        }
        const HotkeyRowContext toggle = ctxFor("toggle_exposure", "SCROLLLOCK", ClashScope::AnyContext);
        check(hotkeyDecide(stepFor(pressing(VK_F11)), toggle).outcome == CaptureOutcome::Bind,
              "...so the exposure key is still written during a session");
    }
    // And unlocked again once the session ends: the stub flips, the row follows.
    g_session = false;
    {
        const HotkeyRowContext c = ctxFor("explorer_cam", "F5", ClashScope::OnFoot);
        check(hotkeyCaptureMayBegin(c) && hotkeyDecide(stepFor(pressing(VK_F10)), c).outcome == CaptureOutcome::Bind,
              "session over: the row takes a change again");
    }

    // The menu key: changed, never cleared.
    {
        const HotkeyRowContext c = ctxFor("menu", "F8", ClashScope::AnyContext);
        CaptureDecision d = hotkeyDecide(stepFor(pressing(VK_DELETE), /*canClear=*/false), c);
        check(d.outcome == CaptureOutcome::ClearRefusedMenu, "Delete on the menu key: refused");
        d = hotkeyDecide(stepFor(pressing(VK_BACK), false), c);
        check(d.outcome == CaptureOutcome::ClearRefusedMenu, "Backspace on the menu key: refused");
        // Even a step that says "cleared" (a caller that forgot canClear) cannot clear the menu row.
        CaptureStep forced;
        forced.kind = CaptureKind::Cleared;
        check(hotkeyDecide(forced, c).outcome == CaptureOutcome::ClearRefusedMenu, "...whatever the step says");
        d = hotkeyDecide(stepFor(pressing(VK_F12, kHotkeyCtrl)), c);
        check(d.outcome == CaptureOutcome::Bind && d.binding.mods == kHotkeyCtrl, "the menu key can be changed (to Ctrl+F12)");
        d = hotkeyDecide(stepFor(pressing(VK_F9, kHotkeyCtrl | kHotkeyShift)), c);
        check(d.outcome == CaptureOutcome::Bind && format(d.binding) == "CTRL+SHIFT+F9", "...and to Ctrl+Shift+F9");
        d = hotkeyDecide(stepFor(pressing(VK_ESCAPE, kHotkeyCtrl | kHotkeyAlt)), c);
        check(d.outcome == CaptureOutcome::Reserved, "the menu key cannot become CTRL+ALT+ESCAPE: refused as a menu key");
        d = hotkeyDecide(stepFor(pressing(VK_RETURN, kHotkeyCtrl)), c);
        check(d.outcome == CaptureOutcome::Reserved, "...nor CTRL+ENTER");
        const HotkeyRowContext toggleCtx = ctxFor("toggle_exposure", "SCROLLLOCK", ClashScope::AnyContext);
        d = hotkeyDecide(stepFor(pressing(VK_RETURN, kHotkeyCtrl)), toggleCtx);
        check(d.outcome == CaptureOutcome::Reserved, "a diagnostic hotkey cannot become CTRL+ENTER either");
        d = hotkeyDecide(stepFor(pressing(VK_F9, kHotkeyCtrl | kHotkeyShift)), toggleCtx);
        check(d.outcome == CaptureOutcome::Bind, "...but CTRL+SHIFT+F9 binds");
        d = hotkeyDecide(stepFor(pressing(VK_F8)), c);
        check(d.outcome == CaptureOutcome::Unchanged, "F8 on the menu key is what it already is");
        d = hotkeyDecide(stepFor(pressing(VK_SCROLL)), c);
        check(d.outcome == CaptureOutcome::Duplicate && strcmp(d.check.duplicateOf, "hotkey.toggle_exposure") == 0,
              "a key another hotkey has: refused, and it says which");
        d = hotkeyDecide(stepFor(pressing(VK_UP)), c);
        check(d.outcome == CaptureOutcome::Reserved, "a navigation key: refused");
        d = hotkeyDecide(stepFor(pressing(VK_F9)), c);
        check(d.outcome == CaptureOutcome::Bind && d.check.clashCount == 1, "a key Elite uses: written, with the warning");
        CaptureSnapshot padPress;
        padPress.padButtons = kPadBack;
        d = hotkeyDecide(stepFor(padPress), c);
        check(d.outcome == CaptureOutcome::Bind && d.binding.kind == HotkeyKind::Pad && d.check.clashCount == 1,
              "a pad button: written, and Elite's use of it is the warning");
        CaptureSnapshot stick;
        stick.joy.count = 1;
        stick.joy.dev[0].id = kGladiator;
        stick.joy.dev[0].bits[0] = 1ull << 11;
        d = hotkeyDecide(stepFor(stick), c);
        check(d.outcome == CaptureOutcome::Bind && format(d.binding) == "231D0200:Joy_12" && d.check.clashCount == 0,
              "a HOTAS button: written as 231D0200:Joy_12");
    }
    // An empty row: a clear has nothing to do.
    {
        const HotkeyRowContext c = ctxFor("dump_draws", "", ClashScope::AnyContext);
        check(hotkeyDecide(stepFor(pressing(VK_DELETE)), c).outcome == CaptureOutcome::ClearAlready, "Delete on an empty row: already empty");
        const HotkeyRowContext spelled = ctxFor("explorer_cam", "f5", ClashScope::OnFoot);
        check(hotkeyDecide(stepFor(pressing(VK_F5)), spelled).outcome == CaptureOutcome::Unchanged,
              "\"f5\" in the ini and F5 pressed are the same key: unchanged");
    }
    g_session = false;
}

// ---------------------------------------------------------------------------
// 6c. The capture-ending press must not reach a hotkey polled LATER in the same frame
//
// The review of 2026-10-08, finding 1. device_hook runs the menu BEFORE vScreenFrameBoundary,
// which polls Explorer Cam's F5 latch; that latch had not seen the press that ended a capture,
// and suspension was already lifted, so capturing F5 on another row (refused as a duplicate)
// entered Explorer Cam. These cells run the REAL Hotkey, the real state machine and the real
// decision in the frame's real order -- earlier poll, menu tick (step, decide, end the capture),
// later poll -- against a keyboard, a pad and a HOTAS the cell controls.

struct World {
    bool     keys[256] = {};
    uint16_t padButtons = 0;
    uint8_t  padTriggers = 0;
    uint32_t joyDevice = 0;
    int      joyInput = -1;
} g_world;

bool worldKey(int vk) { return vk > 0 && vk < 256 && g_world.keys[vk]; }
bool worldHeld(const HotkeyBinding& b) {
    if (b.kind == HotkeyKind::Pad) {
        if (b.padButtons) return (g_world.padButtons & b.padButtons) == b.padButtons;
        return (g_world.padTriggers & (b.padTrigger == 1 ? 1 : 2)) != 0;
    }
    if (b.kind == HotkeyKind::Joy) return g_world.joyInput == b.joyInput && g_world.joyDevice == b.joyDevice;
    return false;
}

void setHeld(const HotkeyBinding& b, bool down) {
    switch (b.kind) {
        case HotkeyKind::Key:
            g_world.keys[b.vk] = down;
            if (b.mods & kHotkeyCtrl) g_world.keys[VK_CONTROL] = down;
            if (b.mods & kHotkeyAlt) g_world.keys[VK_MENU] = down;
            if (b.mods & kHotkeyShift) g_world.keys[VK_SHIFT] = down;
            break;
        case HotkeyKind::Pad:
            if (b.padButtons) {
                if (down) g_world.padButtons = static_cast<uint16_t>(g_world.padButtons | b.padButtons);
                else g_world.padButtons = static_cast<uint16_t>(g_world.padButtons & ~b.padButtons);
            } else {
                const uint8_t bit = b.padTrigger == 1 ? 1 : 2;
                g_world.padTriggers = static_cast<uint8_t>(down ? (g_world.padTriggers | bit) : (g_world.padTriggers & ~bit));
            }
            break;
        case HotkeyKind::Joy:
            g_world.joyDevice = down ? b.joyDevice : 0;
            g_world.joyInput = down ? b.joyInput : -1;
            break;
        default:
            break;
    }
}

CaptureSnapshot snapshotOfWorld() {
    CaptureSnapshot s;
    for (int vk = 1; vk < 256; ++vk) {
        if (hotkeyCaptureKeyEligible(vk) && g_world.keys[vk]) s.keyDown[vk] = 1;
    }
    if (g_world.keys[VK_CONTROL]) s.mods |= kHotkeyCtrl;
    if (g_world.keys[VK_MENU]) s.mods |= kHotkeyAlt;
    if (g_world.keys[VK_SHIFT]) s.mods |= kHotkeyShift;
    s.padButtons = g_world.padButtons;
    s.padTriggers = g_world.padTriggers;
    if (g_world.joyInput >= 0) {
        s.joy.count = 1;
        s.joy.dev[0].id = g_world.joyDevice;
        if (g_world.joyInput < kJoyPovBase) s.joy.dev[0].bits[g_world.joyInput >> 6] |= 1ull << (g_world.joyInput & 63);
        else s.joy.dev[0].bits[2] |= 1ull << (g_world.joyInput - kJoyPovBase);
    }
    return s;
}

struct LeakCell {
    const char*   name;
    const char*   explorer;      // hotkey.explorer_cam, as the ini holds it
    const char*   rowKey;        // the row being captured
    const char*   rowCurrent;    // ...and what it holds
    HotkeyBinding pressed;       // the input that ends the capture
    CaptureOutcome expect;
};

void runLeakCell(const LeakCell& c) {
    g_world = World();
    hotkeySetKeyboardReaderForTest(&worldKey);
    hotkeySetNonKeyboardReader(&worldHeld);
    HotkeyBinding e;
    hotkeyParseBinding(c.explorer, &e, true);

    Hotkey early;      // a diagnostic key, polled BEFORE the menu in the frame
    Hotkey explorer;   // Explorer Cam's F5, polled AFTER it (vScreenFrameBoundary)
    early.setGameMirrored(true);
    explorer.setGameMirrored(true);
    early.setBinding(c.explorer);
    explorer.setBinding(c.explorer);

    // Frame 0: the capture begins (Enter on the row).
    early.pressed();
    explorer.pressed();
    HotkeyCapture capture;
    capture.begin(snapshotOfWorld());
    hotkeysSuspend(true);

    // Frame 1: the press lands.
    setHeld(c.pressed, true);
    check(!early.pressed(), "(earlier poll, capture waiting: nothing fires)");
    const CaptureStep step = capture.step(snapshotOfWorld(), strcmp(c.rowKey, "menu") != 0);
    HotkeyOther others[2];
    int nOthers = 0;
    const bool isExplorerRow = strcmp(c.rowKey, "explorer_cam") == 0;
    if (!isExplorerRow) others[nOthers++] = HotkeyOther{"hotkey.explorer_cam", e};
    HotkeyRowContext ctx;
    ctx.rowKey = c.rowKey;
    ctx.currentText = c.rowCurrent;
    ctx.others = others;
    ctx.nOthers = nOthers;
    ctx.scope = isExplorerRow ? ClashScope::OnFoot : ClashScope::AnyContext;
    const CaptureDecision dec = hotkeyDecide(step, ctx);
    capture.end();
    hotkeysSuspend(false);   // endCapture(): the capture is over, EDVR's hotkeys work again
    char what[240];
    snprintf(what, sizeof(what), "%s: the capture ends as expected", c.name);
    check(dec.outcome == c.expect, what);
    const bool leaked = explorer.pressed();   // the later poll in the same frame
    snprintf(what, sizeof(what), "%s: the capture-ending press does not fire Explorer Cam's latch in that frame", c.name);
    check(!leaked, what);
    snprintf(what, sizeof(what), "%s: ...nor the earlier one's, a frame later", c.name);
    check(!early.pressed(), what);
    // Frame 2 and on, the press still held, then released.
    snprintf(what, sizeof(what), "%s: ...nor while it stays held", c.name);
    check(!explorer.pressed() && !explorer.pressed() && !early.pressed(), what);
    setHeld(c.pressed, false);
    check(!explorer.pressed() && !early.pressed(), "(released: nothing)");
    // The next fresh press of Explorer Cam's own input still works, once.
    setHeld(e, true);
    snprintf(what, sizeof(what), "%s: a fresh press afterwards still fires", c.name);
    check(explorer.pressed() && early.pressed(), what);
    check(!explorer.pressed() && !early.pressed(), "...once");
    setHeld(e, false);
    explorer.pressed();
    early.pressed();
    hotkeySetKeyboardReaderForTest(nullptr);
    hotkeySetNonKeyboardReader(&fakeReader);
}

void testCaptureLeak() {
    printf("capture-ending press\n");
    const char* explorers[3] = {"F5", "GamePad_Back", "231D0200:Joy_12"};
    const char* kinds[3] = {"keyboard", "pad", "HOTAS"};
    for (int k = 0; k < 3; ++k) {
        HotkeyBinding e;
        hotkeyParseBinding(explorers[k], &e, true);
        char name[96];
        // The press is the Explorer binding itself, captured on ANOTHER row: refused as a duplicate,
        // and it must not also be Explorer Cam's key press.
        snprintf(name, sizeof(name), "Duplicate (%s)", kinds[k]);
        runLeakCell({name, explorers[k], "toggle_exposure", "SCROLLLOCK", e, CaptureOutcome::Duplicate});
        // ...captured on the Explorer row itself: unchanged.
        snprintf(name, sizeof(name), "Unchanged (%s)", kinds[k]);
        runLeakCell({name, explorers[k], "explorer_cam", explorers[k], e, CaptureOutcome::Unchanged});
        // Esc cancels. (The cell for a keyboard Explorer key that IS Esc is below.)
        snprintf(name, sizeof(name), "Cancelled (%s)", kinds[k]);
        runLeakCell({name, explorers[k], "toggle_exposure", "SCROLLLOCK", key(VK_ESCAPE), CaptureOutcome::Cancelled});
    }
    // A successful Bind that is a different press of the same key: Explorer = F5 is a subset of CTRL+F5.
    runLeakCell({"Bind (keyboard, CTRL+F5 over an F5 Explorer key)", "F5", "toggle_exposure", "SCROLLLOCK", key(VK_F5, kHotkeyCtrl),
                 CaptureOutcome::Bind});
    runLeakCell({"Bind (pad, Start over a Back Explorer key)", "GamePad_Back", "toggle_exposure", "SCROLLLOCK", pad(kPadStart),
                 CaptureOutcome::Bind});
    runLeakCell({"Bind (HOTAS, Joy_3 over a Joy_12 Explorer key)", "231D0200:Joy_12", "toggle_exposure", "SCROLLLOCK",
                 joy(kGladiator, 2), CaptureOutcome::Bind});
    runLeakCell({"Bind (HOTAS, a hat over a Joy_12 Explorer key)", "231D0200:Joy_12", "dump_camera", "PAUSE",
                 joy(kGladiator, kJoyPovBase), CaptureOutcome::Bind});
    // The same press ending a capture on the MENU row.
    runLeakCell({"Unchanged (keyboard, menu row)", "F5", "menu", "F8", key(VK_F8), CaptureOutcome::Unchanged});
    // An Explorer key that is itself the cancelling key (an ini edit): the Esc that cancels is not a press of it.
    runLeakCell({"Cancelled (keyboard Explorer key IS Esc)", "ESCAPE", "toggle_exposure", "SCROLLLOCK", key(VK_ESCAPE),
                 CaptureOutcome::Cancelled});

    // The registry behind it: every Hotkey is in it, wherever it lives, and copies and
    // assignments do not double or lose an entry.
    {
        const int before = hotkeysLiveCount();
        {
            Hotkey a, b(VK_F19);
            Hotkey c = a;
            a = b;
            b = Hotkey();
            check(hotkeysLiveCount() == before + 3, "three live Hotkeys are three registry entries (construct, int, copy)");
        }
        check(hotkeysLiveCount() == before, "...and none is left behind");
        static Hotkey staticLike;   // a file-scope Hotkey, like Explorer Cam's
        check(hotkeysLiveCount() == before + 1, "...a Hotkey with static storage registers itself too");
    }
    // Priming reaches a pad and a HOTAS latch, and a key latch, whichever binding they hold, and
    // lifting a suspension is what primes: a latch lifted past with the key held does not fire.
    {
        g_world = World();
        hotkeySetKeyboardReaderForTest(&worldKey);
        hotkeySetNonKeyboardReader(&worldHeld);
        Hotkey k, p, j;
        k.setGameMirrored(true);
        p.setGameMirrored(true);
        j.setGameMirrored(true);
        k.setBinding("F6");
        p.setBinding("GamePad_Start");
        j.setBinding("231D0200:Joy_POV1Left");
        hotkeysSuspend(true);
        setHeld(key(VK_F6), true);
        setHeld(pad(kPadStart), true);
        setHeld(joy(kGladiator, kJoyPovBase + 3), true);
        check(!k.pressed() && !p.pressed() && !j.pressed(), "(suspended: nothing fires)");
        setHeld(key(VK_F6), false);
        setHeld(pad(kPadStart), false);
        setHeld(joy(kGladiator, kJoyPovBase + 3), false);
        // Everything released and re-pressed WHILE suspended and held at the lift:
        setHeld(key(VK_F6), true);
        setHeld(pad(kPadStart), true);
        setHeld(joy(kGladiator, kJoyPovBase + 3), true);
        hotkeysSuspend(false);
        check(!k.pressed() && !p.pressed() && !j.pressed(), "held at the lift: no latch, key, pad or HOTAS, makes an edge of it");
        setHeld(key(VK_F6), false);
        setHeld(pad(kPadStart), false);
        setHeld(joy(kGladiator, kJoyPovBase + 3), false);
        k.pressed();
        p.pressed();
        j.pressed();
        setHeld(key(VK_F6), true);
        setHeld(pad(kPadStart), true);
        setHeld(joy(kGladiator, kJoyPovBase + 3), true);
        check(k.pressed() && p.pressed() && j.pressed(), "...and the next fresh press of each fires");
        g_world = World();
        hotkeySetKeyboardReaderForTest(nullptr);
        hotkeySetNonKeyboardReader(&fakeReader);
    }
}

// The wiring the cells above assume, pinned in the source: the menu lifts the suspension when a
// capture ends (which is what primes), and decides BEFORE it ends the capture and writes.
void testCaptureWiringPins(const char* root) {
    printf("capture wiring\n");
    std::string text;
    {
        const std::string path = std::string(root) + "\\src\\d3d11\\menu.cpp";
        FILE* f = fopen(path.c_str(), "rb");
        if (f) {
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            fclose(f);
        }
    }
    if (text.empty()) {
        printf("    (no menu.cpp at \"%s\": the source pins are skipped)\n", root);
        return;
    }
    auto bodyOf = [&](const char* signature) {
        const size_t at = text.find(signature);
        if (at == std::string::npos) return std::string();
        const size_t open = text.find('{', at);
        int depth = 0;
        for (size_t i = open; i < text.size(); ++i) {
            if (text[i] == '{') ++depth;
            else if (text[i] == '}' && --depth == 0) return text.substr(open, i - open + 1);
        }
        return std::string();
    };
    const std::string end = bodyOf("void endCapture() {");
    check(!end.empty() && end.find("hotkeysSuspend(false)") != std::string::npos,
          "menu.cpp: endCapture lifts the suspension (hotkeysSuspend(false), which primes every latch)");
    const std::string handle = bodyOf("void handleCapture(uint64_t now) {");
    const size_t decide = handle.find("hotkeyDecide(");
    const size_t ending = decide == std::string::npos ? std::string::npos : handle.find("endCapture();", decide);
    const size_t applying = handle.find("applyDecision(");
    check(decide != std::string::npos && ending != std::string::npos && applying != std::string::npos &&
              decide < ending && ending < applying,
          "menu.cpp: handleCapture decides, then ends the capture, then applies the decision");
    const std::string start = bodyOf("void startCapture(int entryIndex, int defIndex) {");
    check(start.find("hotkeysSuspend(true)") != std::string::npos, "menu.cpp: a capture that starts suspends the hotkeys");

    // The Explorer Cam page (2026-10-08): built for everyone -- ahead of, and outside, the developer pages -- from the `explorer_cam` rows, and its numbers
    // are shown and stepped by menu_schema.h's helpers (the ones testExplorerPage holds to "+0.15 m" and 0.01).
    {
        const std::string pages = bodyOf("void buildPages() {");
        const size_t at = pages.find("p.name = \"Explorer Cam\";");
        const size_t rows = at == std::string::npos ? at : pages.find("addSettingRows(p, MenuTier::Fix, \"explorer_cam\", false);", at);
        const size_t developer = pages.find("if (s.developer) {");
        check(at != std::string::npos && rows != std::string::npos && rows - at < 200,
              "menu.cpp: buildPages makes a page named Explorer Cam from the Fix-tier rows of the explorer_cam page");
        check(at != std::string::npos && developer != std::string::npos && at < developer,
              "...for everyone: it is built before, and outside, the developer-mode pages");
        check(bodyOf("std::string displayValue(const MenuRowDef& d, const std::string& v) {").find("menuFormatNumber(d, atof(v.c_str()))") !=
                  std::string::npos,
              "menu.cpp: a number row is shown by menuFormatNumber (\"+0.15 m\", \"0 ms (exact)\")");
        check(bodyOf("void stepRow(int defIndex, int dir, int mult) {").find("menuSteppedNumber(d, ") != std::string::npos,
              "menu.cpp: Left and Right step a number row with menuSteppedNumber (0.01 m, 10 ms)");
        check(bodyOf("void stepRow(int defIndex, int dir, int mult) {").find("d.shipped") != std::string::npos,
              "menu.cpp: a row that is empty steps from its shipped value (R writes d.shipped)");
    }
}
// ---------------------------------------------------------------------------
// 7. Elite's bindings, read for keyboard, pad and joystick

std::wstring makeTempDir() {
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    wchar_t dir[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%sedvr-hotkey-test-%lu", base, GetCurrentProcessId());
    CreateDirectoryW(dir, nullptr);
    return dir;
}

void writeFile(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
    CloseHandle(f);
}

void testEliteBinds(const char* root) {
    printf("elite bindings\n");
    const std::wstring dir = makeTempDir();
    writeFile(dir + L"\\StartPreset.4.start", "Custom\r\n");
    // The shapes of Sean's file: a keyboard slot with a modifier, a pad slot, a
    // stick on two devices (buttons past 32, a hat), an axis, a mouse slot, an empty slot.
    const std::string binds =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
        "<Root PresetName=\"Custom\" MajorVersion=\"4\" MinorVersion=\"2\">\n"
        "\t<KeyboardLayout>en-US</KeyboardLayout>\n"
        "\t<HumanoidJumpButton>\n"
        "\t\t<Primary Device=\"Keyboard\" Key=\"Key_Space\" />\n"
        "\t\t<Secondary Device=\"231D0200\" Key=\"Joy_12\" />\n"
        "\t</HumanoidJumpButton>\n"
        "\t<HumanoidReloadButton>\n"
        "\t\t<Primary Device=\"GamePad\" Key=\"GamePad_Back\" />\n"
        "\t\t<Secondary Device=\"Keyboard\" Key=\"Key_R\">\n"
        "\t\t\t<Modifier Device=\"Keyboard\" Key=\"Key_LeftControl\" />\n"
        "\t\t</Secondary>\n"
        "\t</HumanoidReloadButton>\n"
        "\t<TargetNextRouteSystem>\n"
        "\t\t<Primary Device=\"{NoDevice}\" Key=\"\" />\n"
        "\t\t<Secondary Device=\"231D012D\" Key=\"Joy_42\" />\n"
        "\t</TargetNextRouteSystem>\n"
        "\t<LeftThrustButton>\n"
        "\t\t<Primary Device=\"231D0200\" Key=\"Joy_POV1Left\" />\n"
        "\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n"
        "\t</LeftThrustButton>\n"
        "\t<YawAxisRaw>\n"
        "\t\t<Binding Device=\"231D0200\" Key=\"Joy_RZAxis\" />\n"
        "\t\t<Inverted Value=\"0\" />\n"
        "\t</YawAxisRaw>\n"
        "\t<PrimaryFire>\n"
        "\t\t<Primary Device=\"Mouse\" Key=\"Mouse_1\" />\n"
        "\t\t<Secondary Device=\"231D0200\" Key=\"Joy_1\" />\n"
        "\t</PrimaryFire>\n"
        "\t<UnknownPad>\n"
        "\t\t<Primary Device=\"GamePad\" Key=\"GamePad_Mystery\" />\n"
        "\t\t<Secondary Device=\"Keyboard\" Key=\"Key_F6\" />\n"
        "\t</UnknownPad>\n"
        "\t<ThrottleAxis>\n"
        "\t\t<Primary Device=\"GamePad\" Key=\"Pos_GamePad_RTrigger\" />\n"
        "\t\t<Secondary Device=\"Keyboard\" Key=\"Key_NotAKeyWeKnow\" />\n"
        "\t</ThrottleAxis>\n"
        "</Root>\n";
    writeFile(dir + L"\\Custom.4.2.binds", binds);

    static EliteBindUse uses[64];
    char file[64] = "";
    const int count = eliteBindsAllUsesDir(dir.c_str(), uses, 64, file, sizeof(file));
    check(count > 0 && strcmp(file, "Custom.4.2.binds") == 0, "the maintained file is read");
    auto find = [&](const char* element, const HotkeyBinding& b) {
        for (int i = 0; i < count; ++i) {
            if (strcmp(uses[i].element, element) == 0 && hotkeyBindingsEqual(uses[i].binding, b)) return true;
        }
        return false;
    };
    check(find("HumanoidJumpButton", key(VK_SPACE)), "a keyboard slot");
    check(find("HumanoidJumpButton", joy(kGladiator, 11)), "...and the joystick slot beside it: 231D0200 Joy_12");
    check(find("HumanoidReloadButton", pad(kPadBack)), "a GamePad slot");
    check(find("HumanoidReloadButton", key('R', kHotkeyCtrl)), "a keyboard slot with its modifier");
    check(find("TargetNextRouteSystem", joy(kGunfighter, 41)), "a button past 32 on the second stick (Joy_42)");
    check(find("LeftThrustButton", joy(kGladiator, kJoyPovBase + 3)), "a hat direction");
    check(find("PrimaryFire", joy(kGladiator, 0)), "Joy_1 is input 0");
    check(find("ThrottleAxis", pad(0, 2)), "an axis direction prefix on a pad trigger is the trigger");
    int axes = 0, mouse = 0, noDevice = 0;
    for (int i = 0; i < count; ++i) {
        if (strcmp(uses[i].element, "YawAxisRaw") == 0) ++axes;
        if (strcmp(uses[i].element, "PrimaryFire") == 0 && uses[i].binding.kind != HotkeyKind::Joy) ++mouse;
        if (uses[i].binding.kind == HotkeyKind::None) ++noDevice;
    }
    check(axes == 0, "a joystick axis is not a button and is left out");
    check(mouse == 0, "the mouse is left out");
    check(noDevice == 0, "an empty slot, and a key this build has no name for, are left out");
    check(count == 9, "nine uses in all (the unknown pad key is left out, its keyboard slot is not)");
    check(find("UnknownPad", key(VK_F6)), "an element with a pad key this build has no name for still reports its keyboard slot");

    // The keyboard-only lookup Explorer Cam's launch check uses is unchanged by all this.
    static EliteKeyboardUse kb[64];
    const int kn = eliteBindsKeyboardUsesDir(dir.c_str(), kb, 64, nullptr, 0);
    bool sawSpace = false, sawCtrlR = false, leaked = false;
    for (int i = 0; i < kn; ++i) {
        if (strcmp(kb[i].binding, "SPACE") == 0) sawSpace = true;
        if (strcmp(kb[i].binding, "CTRL+R") == 0) sawCtrlR = true;
        if (strncmp(kb[i].binding, "GamePad", 7) == 0 || strchr(kb[i].binding, ':')) leaked = true;
    }
    check(kn == 3 && sawSpace && sawCtrlR && !leaked, "eliteBindsKeyboardUses still answers keyboard bindings only");

    // The same walk over the real file, when this machine has one (Sean's rig does).
    wchar_t local[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
        const std::wstring real = std::wstring(local) + L"\\Frontier Developments\\Elite Dangerous\\Options\\Bindings";
        if (GetFileAttributesW(real.c_str()) != INVALID_FILE_ATTRIBUTES) {
            static EliteBindUse realUses[1024];
            char realFile[64] = "";
            const int rn = eliteBindsAllUsesDir(real.c_str(), realUses, 1024, realFile, sizeof(realFile));
            int sticks = 0, pads = 0, keys = 0;
            for (int i = 0; i < rn; ++i) {
                if (realUses[i].binding.kind == HotkeyKind::Joy) ++sticks;
                if (realUses[i].binding.kind == HotkeyKind::Pad) ++pads;
                if (realUses[i].binding.kind == HotkeyKind::Key) ++keys;
            }
            printf("    (this machine's %s: %d uses, %d keyboard, %d pad, %d joystick)\n", realFile, rn, keys, pads, sticks);
            check(rn != 0 || strcmp(realFile, "") == 0, "the real bindings file parses");
            // The device ids this build writes are the ones the file itself spells: each joystick
            // use, formatted, is a Device="..." that is in the file's text.
            if (sticks > 0) {
                std::string text;
                const std::wstring path = real + L"\\" + std::wstring(realFile, realFile + strlen(realFile));
                HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f != INVALID_HANDLE_VALUE) {
                    const DWORD size = GetFileSize(f, nullptr);
                    text.resize(size);
                    DWORD got = 0;
                    ReadFile(f, &text[0], size, &got, nullptr);
                    CloseHandle(f);
                }
                int unmatched = 0;
                for (int i = 0; i < rn; ++i) {
                    if (realUses[i].binding.kind != HotkeyKind::Joy) continue;
                    char dev[9];
                    hotkeyJoyDeviceText(realUses[i].binding.joyDevice, dev);
                    if (text.find(std::string("Device=\"") + dev + "\"") == std::string::npos) ++unmatched;
                }
                check(!text.empty() && unmatched == 0,
                      "every joystick device id read from the real file is written back exactly as the file spells it");
            }
            // For the reader: what the shipped defaults would be told on this machine's bindings.
            struct { const char* what; HotkeyBinding b; ClashScope scope; } defaults[] = {
                {"menu F8", key(VK_F8), ClashScope::AnyContext},
                {"explorer_cam F5 (on foot)", key(VK_F5), ClashScope::OnFoot},
                {"toggle_exposure SCROLLLOCK", key(VK_SCROLL), ClashScope::AnyContext},
                {"dump_camera PAUSE", key(VK_PAUSE), ClashScope::AnyContext},
            };
            for (const auto& d : defaults) {
                const BindCheck c = hotkeyCheckBinding(d.b, nullptr, 0, realUses, rn, d.scope);
                printf("    %s: %d clash%s%s%s\n", d.what, c.clashCount, c.clashCount == 1 ? "" : "es",
                       c.clashCount ? " -- " : "", c.clashList);
            }
        }
    }
    (void)root;
    DeleteFileW((dir + L"\\StartPreset.4.start").c_str());
    DeleteFileW((dir + L"\\Custom.4.2.binds").c_str());
    RemoveDirectoryW(dir.c_str());
}

// ---------------------------------------------------------------------------
// 8b. The Explorer Cam page (2026-10-08): the generated rows, how they step, and what they show

void testExplorerPage() {
    printf("explorer cam page\n");
    constexpr int kCount = static_cast<int>(sizeof(kMenuRows) / sizeof(kMenuRows[0]));
    auto find = [&](const char* section, const char* key) {
        for (int i = 0; i < kCount; ++i) {
            if (strcmp(kMenuRows[i].section, section) == 0 && strcmp(kMenuRows[i].key, key) == 0) return i;
        }
        return -1;
    };
    // The page: exactly four rows, in the ini's order, for everyone (the Fix tier), live, numbers.
    const char* want[4] = {"explorer_cam_eye_trim_up", "explorer_cam_eye_trim_forward", "explorer_cam_eye_trim_right",
                           "explorer_cam_follow_smoothing_ms"};
    int onPage[16];
    int n = 0;
    for (int i = 0; i < kCount; ++i) {
        if (strcmp(kMenuRows[i].page, "explorer_cam") == 0 && n < 16) onPage[n++] = i;
    }
    check(n == 4, "the Explorer Cam page has four rows");
    for (int k = 0; k < 4 && k < n; ++k) {
        const MenuRowDef& d = kMenuRows[onPage[k]];
        checkf(strcmp(d.key, want[k]) == 0, "row %s is in the ini's order", want[k]);
        checkf(strcmp(d.section, "fix") == 0 && d.tier == MenuTier::Fix, "%s is a [fix] row of the Fix tier (everyone, not the developer pages)", want[k]);
        checkf(d.kind == MenuKind::Number && d.applies == 1, "%s is a LIVE number row", want[k]);
    }
    check(n == 4 && strcmp(kMenuRows[onPage[0]].label, "Eye height") == 0 && strcmp(kMenuRows[onPage[1]].label, "Eye forward") == 0 &&
              strcmp(kMenuRows[onPage[2]].label, "Eye sideways") == 0 && strcmp(kMenuRows[onPage[3]].label, "Head-follow smoothing") == 0,
          "the labels: Eye height, Eye forward, Eye sideways, Head-follow smoothing");
    // The absolute fallback keys have no row anywhere, and neither does the Explorer Cam key outside its own page.
    for (const char* hidden : {"explorer_cam_eye_up", "explorer_cam_eye_forward", "explorer_cam_eye_right"}) {
        checkf(find("fix", hidden) < 0, "the fallback key fix.%s has no menu row (ini-only)", hidden);
    }
    check(find("fix", "explorer_cam") < 0, "no [fix] explorer_cam row (the key is hotkey.explorer_cam)");
    const int key = find("hotkey", "explorer_cam");
    check(key >= 0 && strcmp(kMenuRows[key].page, "hotkeys") == 0, "the Explorer Cam KEY stays on the Hotkeys page only (not duplicated on this one)");

    // Shipped values, bounds and steps.
    if (n == 4) {
        const MenuRowDef& up = kMenuRows[onPage[0]];
        const MenuRowDef& fwd = kMenuRows[onPage[1]];
        const MenuRowDef& right = kMenuRows[onPage[2]];
        const MenuRowDef& smooth = kMenuRows[onPage[3]];
        check(atof(up.shipped) == 0.15 && atof(fwd.shipped) == -0.08 && atof(right.shipped) == 0.0 && atof(smooth.shipped) == 0.0,
              "shipped: 0.15 up, -0.08 forward, 0.0 sideways, smoothing 0 (Sean's tuning)");
        for (int k = 0; k < 3; ++k) {
            const MenuRowDef& d = kMenuRows[onPage[k]];
            checkf(atof(d.lo) == -0.5 && atof(d.hi) == 0.5 && d.precision == 2, "%s is -0.5..0.5 m at two decimals", d.key);
            checkf(menuStepOf(d) == 0.01, "%s steps by 0.01 m", d.key);
        }
        check(atof(smooth.lo) == 0.0 && atof(smooth.hi) == 200.0 && smooth.precision == 0 && menuStepOf(smooth) == 10.0,
              "the smoothing is 0..200 ms, whole numbers, in steps of 10");
        // What the rows show.
        check(menuFormatNumber(up, 0.15) == "+0.15 m", "Eye height 0.15 reads \"+0.15 m\"");
        check(menuFormatNumber(fwd, -0.08) == "-0.08 m", "Eye forward -0.08 reads \"-0.08 m\"");
        check(menuFormatNumber(right, 0.0) == "0.00 m", "Eye sideways 0 reads \"0.00 m\" (no sign on a zero)");
        check(menuFormatNumber(up, 0.1) == "+0.10 m", "a trailing zero stays on a metre row: \"+0.10 m\"");
        check(menuFormatNumber(up, -0.004) == "0.00 m", "-0.004 rounds to a plain zero, not \"-0.00 m\"");
        check(menuFormatNumber(up, 0.5) == "+0.50 m" && menuFormatNumber(up, -0.5) == "-0.50 m", "the limits read \"+0.50 m\" and \"-0.50 m\"");
        check(menuFormatNumber(smooth, 0.0) == "0 ms (exact)", "smoothing 0 reads \"0 ms (exact)\"");
        check(menuFormatNumber(smooth, 50.0) == "50 ms" && menuFormatNumber(smooth, 200.0) == "200 ms", "smoothing 50 reads \"50 ms\"");
        // The arrow keys: a step, the grid, the bounds; Shift is five steps; R is the shipped value.
        double v = atof(up.shipped);
        for (int i = 0; i < 10; ++i) v = menuSteppedNumber(up, v, +1, 1);
        check(menuFileNumber(v, up.precision) == "0.25", "ten presses of Right from 0.15 write 0.25 (the grid, not 0.25000000000000006)");
        for (int i = 0; i < 40; ++i) v = menuSteppedNumber(up, v, +1, 1);
        check(menuFileNumber(v, up.precision) == "0.5", "...and the row stops at +0.5");
        v = menuSteppedNumber(up, 0.5, +1, 1);
        check(menuFileNumber(v, up.precision) == "0.5", "(one more Right at the limit stays)");
        for (int i = 0; i < 120; ++i) v = menuSteppedNumber(fwd, v, -1, 1);
        check(menuFileNumber(v, fwd.precision) == "-0.5", "Left runs down to -0.5 and stops");
        check(menuFileNumber(menuSteppedNumber(up, 0.15, +1, 5), 2) == "0.2" && menuFileNumber(menuSteppedNumber(up, 0.15, -1, 1), 2) == "0.14",
              "Shift+Right is five steps (0.2), Left one step down (0.14)");
        check(menuFileNumber(menuSteppedNumber(fwd, -0.08, +1, 1), 2) == "-0.07", "-0.08 + one step is -0.07");
        double s = 0.0;
        s = menuSteppedNumber(smooth, s, -1, 1);
        check(s == 0.0, "smoothing: Left at 0 stays at 0");
        for (int i = 0; i < 3; ++i) s = menuSteppedNumber(smooth, s, +1, 1);
        check(menuFileNumber(s, smooth.precision) == "30", "...three presses of Right are 30");
        check(menuFileNumber(menuSteppedNumber(smooth, 0.0, +1, 5), smooth.precision) == "50", "...Shift+Right from 0 is 50 (five steps)");
        for (int i = 0; i < 30; ++i) s = menuSteppedNumber(smooth, s, +1, 1);
        check(menuFileNumber(s, smooth.precision) == "200", "...and the row stops at 200 ms (the file may say more; the menu offers 200)");
        // A value that is off the grid (a hand edit) lands back on it: the snap that also keeps 0.3 from becoming 0.30000001 over a run of presses.
        check(menuFileNumber(menuSteppedNumber(smooth, 24.0, +1, 1), smooth.precision) == "30" &&
                  menuFileNumber(menuSteppedNumber(up, 0.12, +1, 5), 2) == "0.15",
              "an off-grid value lands on the step grid (24 ms + one step is 30; 0.12 m + five steps is 0.15)");
        // R resets to the shipped text, which the file already holds in the form it reads.
        check(strcmp(up.shipped, "0.15") == 0 && strcmp(fwd.shipped, "-0.08") == 0 && strcmp(smooth.shipped, "0") == 0, "R writes \"0.15\", \"-0.08\" and \"0\"");
    }
    // The rows every OTHER page already had keep their look: the plain rule trims trailing zeros, a percentage scales and gets its sign.
    {
        MenuRowDef plain = {};
        plain.lo = "";
        plain.hi = "";
        plain.step = "";
        plain.unit = "";
        plain.zero = "";
        plain.precision = 2;
        check(menuFormatNumber(plain, 0.30) == "0.3" && menuFormatNumber(plain, 1.0) == "1.0" && menuFormatNumber(plain, 0.25) == "0.25" &&
                  menuFormatNumber(plain, -0.5) == "-0.5",
              "an unshaped decimal row keeps the older rule (0.30 reads 0.3, 1.00 reads 1.0)");
        MenuRowDef whole = plain;
        whole.precision = 0;
        check(menuFormatNumber(whole, 7.0) == "7", "an unshaped whole-number row reads as a whole number");
        MenuRowDef pct = plain;
        pct.percent = true;
        check(menuFormatNumber(pct, 0.3) == "30%" && menuFormatNumber(pct, 1.0) == "100%", "a percent row reads 30%");
        check(menuStepOf(plain) == 0.1 && menuStepOf(whole) == 1.0, "an unbounded row steps by a tenth, or by one");
        MenuRowDef ranged = plain;
        ranged.lo = "0";
        ranged.hi = "1";
        check(menuStepOf(ranged) == 0.05, "a 0..1 decimal row steps by a twentieth (the older rule)");
    }
}
// ---------------------------------------------------------------------------
// 8. The generated rows: which are on the Hotkeys page, in which tier

void testRows() {
    printf("rows\n");
    constexpr int kCount = static_cast<int>(sizeof(kMenuRows) / sizeof(kMenuRows[0]));
    auto rowIndex = [&](const char* key) {
        for (int i = 0; i < kCount; ++i) {
            if (strcmp(kMenuRows[i].section, "hotkey") == 0 && strcmp(kMenuRows[i].key, key) == 0) return i;
        }
        return -1;
    };
    int all[16], dev[16];
    const int nEveryone = hotkeyPageRows(kMenuRows, kCount, false, all, 16);
    const int nDev = hotkeyPageRows(kMenuRows, kCount, true, dev, 16);
    auto contains = [](const int* v, int n, int i) {
        for (int k = 0; k < n; ++k) if (v[k] == i) return true;
        return false;
    };
    for (const char* key : {"menu", "explorer_cam", "toggle_exposure", "dump_camera"}) {
        const int i = rowIndex(key);
        checkf(i >= 0, "hotkey.%s has a generated row", key);
        checkf(i >= 0 && kMenuRows[i].kind == MenuKind::Hotkey && kMenuRows[i].tier == MenuTier::Fix &&
                   strcmp(kMenuRows[i].page, "hotkeys") == 0,
               "hotkey.%s is a Hotkey row of the Fix tier on the hotkeys page", key);
        checkf(contains(all, nEveryone, i), "hotkey.%s is on the page with developer mode off", key);
        checkf(contains(dev, nDev, i), "hotkey.%s is on the page with developer mode on", key);
    }
    for (const char* key : {"dump_draws", "dump_eyes"}) {
        const int i = rowIndex(key);
        checkf(i >= 0, "hotkey.%s has a generated row", key);
        checkf(i >= 0 && kMenuRows[i].kind == MenuKind::Hotkey && kMenuRows[i].tier == MenuTier::Advanced &&
                   strcmp(kMenuRows[i].page, "hotkeys") == 0,
               "hotkey.%s is a Hotkey row of the developer tier on the hotkeys page", key);
        checkf(!contains(all, nEveryone, i), "hotkey.%s is NOT on the page with developer mode off", key);
        checkf(contains(dev, nDev, i), "hotkey.%s is on the page with developer mode on", key);
    }
    check(nEveryone == 4 && nDev == 6, "four rows for everyone, six in developer mode");
    check(rowIndex("read_game_bindings") < 0, "a plain [hotkey] setting is not a row");
    // Table order is the page's order: the menu key first.
    check(nEveryone > 0 && strcmp(kMenuRows[all[0]].key, "menu") == 0, "the menu key leads the page");
    // No other row is a Hotkey row, and none of these sits on another page.
    int stray = 0;
    for (int i = 0; i < kCount; ++i) {
        if ((kMenuRows[i].kind == MenuKind::Hotkey) != (strcmp(kMenuRows[i].page, "hotkeys") == 0)) ++stray;
    }
    check(stray == 0, "a Hotkey row is on the hotkeys page and nothing else is");
    // The developer pages must not list a hotkey row twice: their rows are
    // selected by the section name in `page`, which a hotkey row does not carry.
    int onAdvancedPage = 0;
    for (int i = 0; i < kCount; ++i) {
        if (kMenuRows[i].tier == MenuTier::Advanced && strcmp(kMenuRows[i].page, "advanced") == 0 &&
            strcmp(kMenuRows[i].section, "hotkey") == 0) ++onAdvancedPage;
    }
    check(onAdvancedPage == 0, "a developer hotkey row is not also on the Advanced page");
    // The ini says what the menu says: every shipped hotkey value parses.
    for (int i = 0; i < kCount; ++i) {
        if (kMenuRows[i].kind != MenuKind::Hotkey) continue;
        bool ok = false;
        parse(kMenuRows[i].shipped, &ok);
        checkf(ok, "the shipped value of hotkey.%s parses", kMenuRows[i].key);
    }
}

}  // namespace

int main(int argc, char** argv) {
    testFormats();
    testIniRoundTrip();
    testHotkeyEdge();
    testJoyWatch();
    testCapture();
    testChecks();
    testDecide();
    testCaptureLeak();
    testCaptureWiringPins(argc > 1 ? argv[1] : "");
    testEliteBinds(argc > 1 ? argv[1] : "");
    testRows();
    testExplorerPage();
    if (failures) {
        printf("hotkey_capture_test: %d of %d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("hotkey_capture_test: %d checks passed\n", checks);
    return 0;
}

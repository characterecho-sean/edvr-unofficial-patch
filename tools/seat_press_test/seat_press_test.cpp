// The HMD recentre key as the seat memory reads it (src/d3d11/elite_binds.cpp eliteBindsHmdReset*): Elite's HMDReset action out of .binds
// text, slot by slot, with what watching each takes. Keyboard (with modifiers), XInput pad and HOTAS/joystick slots are observable; the
// mouse, an axis and an unnamed key are not. Nothing is opened: no device, no window, no game; the folder cases write to a scratch folder.
//   seat_press_test --self-test | --dry-run
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/d3d11/elite_binds.h"
#include "../../src/common/hotkey.h"

using namespace edvr;

namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char* text) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", text); }
}

// A .binds file with only the HMDReset element's slots varying; the rest is the neighbours a real file has.
std::string binds(const std::string& hmd) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<Root PresetName=\"Custom\" MajorVersion=\"4\" MinorVersion=\"2\">\n"
           "\t<ResetPowerDistribution>\n\t\t<Primary Device=\"Keyboard\" Key=\"Key_F11\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</ResetPowerDistribution>\n" +
           hmd +
           "\t<ToggleCargoScoop>\n\t\t<Primary Device=\"Keyboard\" Key=\"Key_Home\" />\n\t\t<Secondary Device=\"231D012D\" Key=\"Joy_33\" />\n\t\t<ToggleOn Value=\"1\" />\n\t</ToggleCargoScoop>\n</Root>\n";
}
bool parse(const std::string& hmd, EliteResetBind* out) {
    const std::string text = binds(hmd);
    return eliteBindsHmdResetIn(text.data(), text.size(), out);
}

void seansBind() {
    // Sean's real file (Custom.4.2.binds, 2026-10-14): F12 on the keyboard, nothing on the secondary.
    EliteResetBind b;
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"Keyboard\" Key=\"Key_F12\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", &b),
          "Sean's HMDReset is found");
    check(b.present && b.count == 1, "one bound slot (the {NoDevice} secondary has no key)");
    check(b.slot[0].observable && b.slot[0].binding.kind == HotkeyKind::Key && b.slot[0].binding.vk == VK_F12 && b.slot[0].binding.mods == 0,
          "the keyboard slot is F12, observable, no modifier");
    check(std::strcmp(b.slot[0].label, "Keyboard:Key_F12") == 0, "its label names the device and the key as Elite wrote them");
    char text[64];
    check(hotkeyFormatBinding(b.slot[0].binding, text, sizeof text) > 0 && std::strcmp(text, "F12") == 0, "the binding formats as the hotkey machinery's F12");
}

void keyboardAndHotas() {
    EliteResetBind b;
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"231D0200\" Key=\"Joy_33\" />\n"
                "\t\t<Secondary Device=\"Keyboard\" Key=\"Key_F12\">\n\t\t\t<Modifier Device=\"Keyboard\" Key=\"Key_LeftControl\" />\n\t\t</Secondary>\n\t</HMDReset>\n", &b),
          "a HOTAS primary with a chorded keyboard secondary is found");
    check(b.count == 2, "two slots");
    check(b.slot[0].observable && b.slot[0].binding.kind == HotkeyKind::Joy && b.slot[0].binding.joyDevice == 0x231D0200u && b.slot[0].binding.joyInput == 32,
          "the HOTAS slot is device 231D0200, Joy_33 (input 32)");
    check(std::strcmp(b.slot[0].label, "231D0200:Joy_33") == 0, "its label is the device id and the key");
    check(b.slot[1].observable && b.slot[1].binding.kind == HotkeyKind::Key && b.slot[1].binding.vk == VK_F12 && (b.slot[1].binding.mods & kHotkeyCtrl),
          "the keyboard slot is F12 with Ctrl");
    check(std::strcmp(b.slot[1].label, "Keyboard:Key_LeftControl+Key_F12") == 0 || std::strncmp(b.slot[1].label, "Keyboard:", 9) == 0, "its label names the keyboard");

    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"231D0200\" Key=\"Joy_POV1Up\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", &b) &&
          b.slot[0].observable && b.slot[0].binding.kind == HotkeyKind::Joy && b.slot[0].binding.joyInput == kJoyPovBase,
          "a POV hat direction is a joystick input too");
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"GamePad\" Key=\"GamePad_Back\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", &b) &&
          b.slot[0].observable && b.slot[0].binding.kind == HotkeyKind::Pad && b.slot[0].binding.padButtons != 0,
          "an XInput pad button is observable");
}

void unobservable() {
    EliteResetBind b;
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"Mouse\" Key=\"Mouse_9\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", &b) &&
          b.count == 1 && !b.slot[0].observable && std::strcmp(b.slot[0].label, "Mouse:Mouse_9") == 0,
          "a mouse button past the fifth is bound and is NOT observable");
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"Mouse\" Key=\"Mouse_XAxis\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", &b) &&
          b.count == 1 && !b.slot[0].observable, "a mouse axis is bound and is NOT observable");
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"231D0200\" Key=\"Joy_XAxis\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", &b) &&
          b.count == 1 && !b.slot[0].observable, "a joystick axis is bound and is NOT observable");
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"Keyboard\" Key=\"Key_F12\" />\n\t\t<Secondary Device=\"231D0200\" Key=\"Joy_XAxis\" />\n\t</HMDReset>\n", &b) &&
          b.count == 2 && b.slot[0].observable && !b.slot[1].observable,
          "a key plus a joystick axis: the axis slot is the one nothing sees (the status is then unobservable for the pair)");
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"Keyboard\" Key=\"Key_NoSuchKeyAnywhere\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", &b) &&
          b.count == 1 && !b.slot[0].observable, "a key this build has no name for is bound and not observable");
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"{NoDevice}\" Key=\"\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", &b) && b.present && b.count == 0,
          "an unbound HMDReset is present with no slots");
}

void mouse() {
    EliteResetBind b;
    const int expect[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    for (int n = 1; n <= 5; ++n) {
        char slot[200];
        std::snprintf(slot, sizeof slot, "\t<HMDReset>\n\t\t<Primary Device=\"Mouse\" Key=\"Mouse_%d\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", n);
        check(parse(slot, &b) && b.count == 1 && b.slot[0].observable && b.slot[0].binding.kind == HotkeyKind::Key && b.slot[0].binding.vk == expect[n - 1],
              "Mouse_1 to Mouse_5 are the left, right, middle and two side buttons as GetAsyncKeyState names them");
        check(b.slot[0].needsProof, "...and are trusted only after a first press has been seen");
        char text[64];
        HotkeyBinding back;
        check(hotkeyFormatBinding(b.slot[0].binding, text, sizeof text) > 0 && hotkeyParseBinding(text, &back, true) && hotkeyBindingsEqual(back, b.slot[0].binding),
              "...and the hotkey machinery reads the binding back as the same mouse button");
    }
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"Mouse\" Key=\"Mouse_01\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", &b) && !b.slot[0].observable,
          "a spelling that is not exactly Mouse_N is not guessed at");
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"Keyboard\" Key=\"Key_F12\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", &b) && !b.slot[0].needsProof,
          "a keyboard key needs no proof");
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"231D0200\" Key=\"Joy_5\" />\n\t\t<Secondary Device=\"GamePad\" Key=\"GamePad_Back\" />\n\t</HMDReset>\n", &b) &&
          b.slot[0].needsProof && b.slot[1].needsProof, "a joystick and a pad button are trusted after a first press");
}

void absentAndNeighbours() {
    EliteResetBind b;
    check(!parse("", &b) && !b.present && b.count == 0, "no HMDReset element: not present");
    // The neighbours carry keys and joystick bindings of their own and must not be read as the HMD key.
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"Keyboard\" Key=\"Key_F12\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", &b) &&
          b.count == 1 && b.slot[0].binding.vk == VK_F12, "ResetPowerDistribution (F11) and ToggleCargoScoop (a joystick secondary) are not the HMD key");
    EliteResetBind nul;
    check(!eliteBindsHmdResetIn(nullptr, 0, &nul) && !nul.present, "no text: not present");
    check(!eliteBindsHmdResetIn("x", 1, nullptr), "no output: false");
}

std::wstring scratchDir() {
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    wchar_t name[64];
    std::swprintf(name, 64, L"edvr_seat_press_%lu_%llu", GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    return std::wstring(tmp) + name;
}
void writeFile(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &put, nullptr);
    CloseHandle(f);
}

void folder() {
    const std::wstring dir = scratchDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    EliteResetBind b;
    check(!eliteBindsHmdResetDir(dir.c_str(), &b) && b.filesSeen == 0, "no preset and no .binds: not found, no file seen");
    writeFile(dir + L"\\StartPreset.4.start", "Custom\r\n");
    check(!eliteBindsHmdResetDir(dir.c_str(), &b) && b.filesSeen == 0, "a preset naming files that are not there: no file seen");
    writeFile(dir + L"\\Custom.4.2.binds", binds("\t<HMDReset>\n\t\t<Primary Device=\"Keyboard\" Key=\"Key_F12\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n"));
    check(eliteBindsHmdResetDir(dir.c_str(), &b) && b.present && b.count == 1 && b.filesSeen == 1 && std::strcmp(b.file, "Custom.4.2.binds") == 0 &&
          b.slot[0].binding.vk == VK_F12, "the active preset's file answers, and names itself");
    // The newest active file answers alone, even if an older one would have said something else.
    Sleep(30);
    writeFile(dir + L"\\Custom.4.3.binds", binds("\t<HMDReset>\n\t\t<Primary Device=\"231D0200\" Key=\"Joy_12\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n"));
    check(eliteBindsHmdResetDir(dir.c_str(), &b) && std::strcmp(b.file, "Custom.4.3.binds") == 0 && b.slot[0].binding.kind == HotkeyKind::Joy,
          "the newest active file wins (a rebind to a HOTAS button)");
    // A file for another preset is not consulted.
    Sleep(30);
    writeFile(dir + L"\\Other.4.2.binds", binds("\t<HMDReset>\n\t\t<Primary Device=\"Keyboard\" Key=\"Key_F1\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n"));
    check(eliteBindsHmdResetDir(dir.c_str(), &b) && std::strcmp(b.file, "Custom.4.3.binds") == 0, "a newer file of another preset is not the active one");
    // A file without the element: the next active file is consulted (first that CONTAINS it answers).
    Sleep(30);
    writeFile(dir + L"\\Custom.4.4.binds", binds(""));
    check(eliteBindsHmdResetDir(dir.c_str(), &b) && b.filesSeen == 2 && std::strcmp(b.file, "Custom.4.3.binds") == 0,
          "the newest file without the element is passed over for the next that has it");
    DeleteFileW((dir + L"\\StartPreset.4.start").c_str());
    DeleteFileW((dir + L"\\Custom.4.2.binds").c_str());
    DeleteFileW((dir + L"\\Custom.4.3.binds").c_str());
    DeleteFileW((dir + L"\\Custom.4.4.binds").c_str());
    DeleteFileW((dir + L"\\Other.4.2.binds").c_str());
    RemoveDirectoryW(dir.c_str());
    check(GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES, "the scratch folder is gone");
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && !wcscmp(argv[1], L"--dry-run")) {
        std::puts("seat_press_test: dry-run (no files, no devices)");
        return 0;
    }
    if (argc != 2 || wcscmp(argv[1], L"--self-test")) {
        std::fputs("usage: --self-test | --dry-run\n", stderr);
        return 2;
    }
    seansBind();
    keyboardAndHotas();
    unobservable();
    mouse();
    absentAndNeighbours();
    folder();
    std::printf("seat_press_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

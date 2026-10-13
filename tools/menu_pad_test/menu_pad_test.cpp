// menu_pad_test: pure regression rig for the XInput panel gate and navigation.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "../../src/d3d11/menu_pad.h"

using namespace edvr;
namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", name); }
}
MenuPadSample sample(uint32_t packet, uint16_t buttons = 0) {
    MenuPadSample s; s.packet = packet; s.buttons = buttons; return s;
}
void ownershipCases() {
    check(menuPadMayOwn(3, 3, 500, 600, true, false, true),
          "ownership survives a frame over 100ms for slot 3");
    check(menuPadMayOwn(3, 3, 500, 5500, true, false, true),
          "ownership survives a blocking five-second backend creation");
    check(!menuPadMayOwn(3, 3, 500, 5501, true, false, true),
          "stale request expires beyond the bounded lease");
    check(!menuPadMayOwn(3, 3, 500, 600, true, false, false),
          "focus loss revokes ownership before lease expiry");
    check(!menuPadMayOwn(-1, -1, 500, 500, true, false, true),
          "negative requested controller slot is invalid");
    check(!menuPadMayOwn(4, 4, 500, 500, true, false, true),
          "controller slot 4 is outside valid 0 through 3 range");
    check(!menuPadMayOwn(0, -1, 500, 500, true, false, true),
          "not-drawn request sentinel does not grant ownership");
    check(!menuPadMayOwn(0, 0, 500, 500, false, false, true),
          "missing installed door cannot grant ownership");
    check(!menuPadMayOwn(0, 0, 500, 500, true, true, true),
          "retired door cannot grant ownership");
    check(!menuPadMayOwn(0, 0, 501, 500, true, false, true),
          "future request timestamp cannot grant ownership");
    check(!menuPadMayOwn(0, 0, 0, 0, true, false, true),
          "zero request timestamp cannot grant ownership");
    check(menuPadOwnsButton(0x1000, 0) && menuPadOwnsButton(0x0008, 0) &&
          !menuPadOwnsButton(0x4000, 0) && !menuPadOwnsButton(0x1000, 1),
          "panel owns only its button set, not X/Y or trigger chords");
}
void gateCases() {
    MenuPadGate longFrame;
    longFrame.filter(sample(1, 0), menuPadMayOwn(0, 0, 100, 100, true, false, true));
    check(longFrame.filter(sample(2, 0x1000),
                                menuPadMayOwn(0, 0, 100, 1200, true, false, true)).buttons == 0,
          "fresh A during a 1.1-second blocked Present remains private");
    check(longFrame.filter(sample(3, 0x1000),
                                menuPadMayOwn(0, 0, 100, 1201, true, false, false)).buttons == 0,
          "focus loss keeps a held panel button hidden as a release tail");
    longFrame.filter(sample(4, 0), false);
    check(longFrame.filter(sample(5, 0x1000), false).buttons == 0x1000,
          "fresh press after focus loss and release reaches Elite");

    MenuPadGate packetModel;
    MenuPadSample largeInitial = packetModel.filter(sample(0xDEADBEEFu, 0x0000), false);
    check(largeInitial.packet == 0xDEADBEEFu,
          "initial unowned sample preserves large raw packet number");
    packetModel.filter(sample(0xDEADBEEFu, 0x1000), false);
    MenuPadSample firstMasked = packetModel.filter(sample(0xDEADBEEFu, 0x1000), true);
    check(firstMasked.buttons == 0 && firstMasked.packet == 0xDEADBEF0u,
          "first masking transition bumps packet after passthrough at same raw packet");

    // D-pad 0x000F, LB/RB 0x0300, A/B 0x3000 are consumed.
    MenuPadGate stable;
    MenuPadSample raw = sample(10, 0x1001);
    MenuPadSample first = stable.filter(raw, true);
    MenuPadSample changedRawPacket = sample(91, 0x1001);
    MenuPadSample second = stable.filter(changedRawPacket, true);
    check(first.buttons == 0 && second.buttons == 0 && second.packet == first.packet,
          "owned masked button raw packet change leaves visible packet stable");

    MenuPadGate transition;
    MenuPadSample pass = transition.filter(sample(4, 0x1000), false);
    MenuPadSample masked = transition.filter(sample(4, 0x1000), true);
    check(pass.buttons == 0x1000 && masked.buttons == 0 && masked.packet == pass.packet + 1,
          "ownership mask transition increments packet for same raw packet");
    transition.filter(sample(4, 0x1000), false); // held close tail remains hidden
    transition.filter(sample(4, 0), false);       // physical release clears the tail
    MenuPadSample passAgain = transition.filter(sample(4, 0x1000), false);
    check(passAgain.buttons == 0x1000 && passAgain.packet == masked.packet + 1,
          "ownership release permits fresh press and increments packet for same raw packet");

    MenuPadGate preserve;
    MenuPadSample controls = sample(1, 0xFFFF);
    controls.leftTrigger = 9; controls.rightTrigger = 227;
    controls.leftX = -1234; controls.leftY = 2345; controls.rightX = -3276; controls.rightY = 3275;
    MenuPadSample out = preserve.filter(controls, true);
    check(out.buttons == 0xCCF0 && out.leftTrigger == 9 && out.rightTrigger == 227 &&
          out.leftX == -1234 && out.leftY == 2345 && out.rightX == -3276 && out.rightY == 3275,
          "owned output preserves analog and unconsumed buttons with independent mask expectation");

    MenuPadGate tail;
    check(tail.filter(sample(1, 0x2000), true).buttons == 0, "owned close starts hidden B press");
    check(tail.filter(sample(2, 0x2000), false).buttons == 0, "closed held B tail remains hidden");
    check(tail.filter(sample(3, 0), false).buttons == 0, "released tail clears");
    check(tail.filter(sample(4, 0x2000), false).buttons == 0x2000, "fresh B press passes after release");

    // Focus loss and a panel that is not drawn both mean the game owns input.
    MenuPadGate ownership;
    check(ownership.filter(sample(1, 0x0002), false).buttons == 0x0002,
          "ownership false passes controls after focus loss or hidden panel");

    MenuPadGate disconnected;
    disconnected.filter(sample(1, 0x0001), true);
    disconnected.filter(sample(2, 0x0001), false);
    disconnected.disconnect();
    MenuPadSample after = disconnected.filter(sample(40, 0x0001), false);
    check(after.buttons == 0x0001 && after.packet == 40,
          "disconnect clears tail and packet initialization");

    MenuPadGate controller0, controller1;
    controller0.filter(sample(1, 0x0004), true);
    check(controller1.filter(sample(1, 0x0004), false).buttons == 0x0004,
          "independent controller gate never inherits another controller tail");
}
void navigationCases() {
    // Expected XInput ABI values and action order, specified independently of
    // the production tables so a reordered or mistyped mapping fails here.
    const uint16_t expectedMasks[kMenuPadCount] = {
        0x0001, 0x0002, 0x0004, 0x0008, 0x1000, 0x2000, 0x0100, 0x0200};
    const MenuNav expectedNavs[kMenuPadCount] = {
        kNavUp, kNavDown, kNavLeft, kNavRight, kNavSelect, kNavBack,
        kNavPagePrev, kNavPageNext};
    bool mappingMatches = kMenuPadMask == 0x330F;
    for (int i = 0; i < kMenuPadCount; ++i)
        mappingMatches = mappingMatches && kMenuPadButtons[i] == expectedMasks[i] &&
                         kMenuPadNavs[i] == expectedNavs[i];
    check(mappingMatches, "XInput masks and navigation order match independent expectations");

    MenuPadNavigation nav;
    check(nav.step(0, 0x0001, 1000, true) == 1, "up emits initial press");
    check(nav.step(0, 0x0001, 1399, true) == 0, "up waits before 400ms initial delay");
    check(nav.step(0, 0x0001, 1400, true) == 1, "up repeats at 400ms");
    check(nav.step(0, 0x0001, 1482, true) == 0, "up waits before 83ms repeat cadence");
    check(nav.step(0, 0x0001, 1483, true) == 1, "up repeats at 83ms cadence");

    check(nav.step(4, 0x1000, 2000, true) == 1, "A select emits initial edge");
    check(nav.step(4, 0x1000, 2400, true) == 0, "held A select is edge-only");
    check(nav.step(5, 0x2000, 2000, true) == 1, "B back emits initial edge");
    check(nav.step(5, 0x2000, 2400, true) == 0, "held B back is edge-only");
    check(nav.step(6, 0x0100, 2000, true) == 1, "LB page emits initial edge");
    check(nav.step(6, 0x0100, 2400, true) == 0, "held LB page is edge-only");
    check(nav.step(7, 0x0200, 2000, true) == 1, "RB page emits initial edge");
    check(nav.step(7, 0x0200, 2400, true) == 0, "held RB page is edge-only");

    MenuPadNavigation primed;
    primed.prime(0x1000);
    check(primed.step(4, 0x1000, 3000, true) == 0, "open-held A is blocked by prime");
    check(primed.step(4, 0, 3001, true) == 0 && primed.step(4, 0x1000, 3002, true) == 1,
          "primed A acts after release and fresh press");

    MenuPadNavigation inactive;
    check(inactive.step(0, 0x0001, 4000, false) == 0, "inactive ownership swallows up press");
    check(inactive.step(0, 0x0001, 5000, true) == 0, "held inactive up stays parked when ownership returns");
    check(inactive.step(0, 0, 5001, true) == 0 && inactive.step(0, 0x0001, 5002, true) == 1,
          "parked up needs release and fresh press");

    MenuPadNavigation edit;
    check(!edit.stepHeld(), "edit navigation initially has no held horizontal direction");
    edit.step(2, 0x0004, 6000, true);
    check(edit.stepHeld(), "held left is detected for numeric edit");
    edit.step(2, 0, 6001, true);
    edit.step(3, 0x0008, 6002, true);
    check(edit.stepHeld(), "held right is detected for numeric edit");
    edit.step(3, 0, 6003, true);
    check(!edit.stepHeld(), "horizontal hold clears on release");
}
}
int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("menu_pad_test: dry-run (no checks or files)");
        return 0;
    }
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "--self-test") != 0)) {
        std::fputs("usage: menu_pad_test [--self-test | --dry-run]\n", stderr);
        return 2;
    }
    ownershipCases();
    gateCases();
    navigationCases();
    std::printf("menu_pad_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

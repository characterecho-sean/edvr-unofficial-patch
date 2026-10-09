// The on-foot maps gate's rig (docs/design-world-camera-motion-2026-09-30.md, Phase 1).
//
// Two halves, both with no D3D:
//   R1..R9   the pure half, src\d3d11\ui_maps_math.h, run on its own: the key, the step (2 named frames hold the panel as the
//            world, 3 unnamed release it), flight 1's runs replayed through it, the gate's combine with the key off held to a frozen
//            copy of today's expression for every input, the carry, the door's predicate and the text of every line the feature
//            logs. tools\on_foot_maps_test\mutants.py compiles this rig against a copy of the header with ONE rule flipped and
//            requires the rig to fail on the case that belongs to the rule (the label of its first FAIL starts with the mutation's
//            label prefix); that is why every check below carries a label "R<n><letter>: ...".
//   P1..P8   the pins, by source scan from the repo root: the places that call the pure half stay where the design put them, and
//            the key-off contract stays true -- with the key off nothing new is read, counted, logged or issued; the 5 s line's
//            screen-draws= is counted in the one place (P8, with controls that make it fail)
//            (--self-test <repo root>; skipped when no root is given, and never run by the mutation tool).
//
// Usage: --self-test [<repo root>] [--only R1,R5,...]   |   --dry-run (no checks run)
#include "ui_maps_math.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace edvr;

// ---- the harness ---------------------------------------------------------------------------------------------------------
unsigned g_checks = 0;
void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) throw std::runtime_error(label);
}
bool contains(const char* text, const char* piece) { return std::strstr(text, piece) != nullptr; }

// ---- R1: the key ----------------------------------------------------------------------------------------------------------
void caseR1() {
    check(uiMapsKeyFromText("on") == UiMapsKey::On, "R1a: \"on\" reads as on");
    check(uiMapsKeyFromText("ON") == UiMapsKey::On && uiMapsKeyFromText("On") == UiMapsKey::On && uiMapsKeyFromText("oN") == UiMapsKey::On,
          "R1b: the value is read without regard to case");
    check(uiMapsKeyFromText("off") == UiMapsKey::Off, "R1c: \"off\" reads as off");
    const char* typos[] = {"", " ", "o", "onn", "of", " on", "on ", "1", "true", "yes", "auto", "enabled", "0", "no", "on\n"};
    bool allOff = true;
    for (const char* t : typos) allOff = allOff && uiMapsKeyFromText(t) == UiMapsKey::Off;
    check(allOff, "R1d: a value that is present and is not \"on\" reads as off, so a typo never switches the gate on");
    check(uiMapsKeyFromText(nullptr) == UiMapsKey::Off, "R1e: no value reads as off");
    check(std::strcmp(uiMapsKeyName(UiMapsKey::On), "on") == 0 && std::strcmp(uiMapsKeyName(UiMapsKey::Off), "off") == 0,
          "R1f: the key's names are on and off");
}

// ---- R2: the step ---------------------------------------------------------------------------------------------------------
void caseR2() {
    {
        UiMapsGate g;
        check(!g.world && g.run == 0, "R2a: a fresh gate is not the world");
        check(uiMapsStep(g, true) == UiMapsEdge::None && !g.world, "R2b: one named frame holds nothing");
        check(uiMapsStep(g, true) == UiMapsEdge::Hold && g.world && g.run == 0, "R2c: the second named frame in a row holds the panel as the world (an edge), and the hold leaves no run counting");
        check(uiMapsStep(g, true) == UiMapsEdge::None && g.world && g.run == 0, "R2d: a held world stays held on named frames, with no edge and no run");
    }
    {
        UiMapsGate g;
        uiMapsStep(g, true);
        check(uiMapsStep(g, false) == UiMapsEdge::None && !g.world && g.run == 0, "R2e: an unnamed frame between two named ones restarts the run toward holding");
        check(uiMapsStep(g, true) == UiMapsEdge::None && !g.world, "R2f: ... so the next named frame is the first of a new run");
        check(uiMapsStep(g, true) == UiMapsEdge::Hold && g.world, "R2g: ... and the one after it holds");
    }
    {
        UiMapsGate g;
        uiMapsStep(g, true);
        uiMapsStep(g, true);
        check(g.world, "R2h: (setup) held");
        check(uiMapsStep(g, false) == UiMapsEdge::None && g.world, "R2i: one unnamed frame releases nothing");
        check(uiMapsStep(g, false) == UiMapsEdge::None && g.world, "R2j: two unnamed frames release nothing");
        check(uiMapsStep(g, false) == UiMapsEdge::Release && !g.world && g.run == 0, "R2k: the third unnamed frame in a row releases the panel (an edge)");
        check(uiMapsStep(g, false) == UiMapsEdge::None && !g.world, "R2l: a released panel stays released on unnamed frames, with no edge");
    }
    {
        UiMapsGate g;
        uiMapsStep(g, true);
        uiMapsStep(g, true);
        uiMapsStep(g, false);
        uiMapsStep(g, false);
        check(uiMapsStep(g, true) == UiMapsEdge::None && g.world && g.run == 0, "R2m: a named frame inside an unnamed run restarts the run toward releasing");
        check(uiMapsStep(g, false) == UiMapsEdge::None && uiMapsStep(g, false) == UiMapsEdge::None && g.world, "R2n: ... so two more unnamed frames still hold");
        check(uiMapsStep(g, false) == UiMapsEdge::Release && !g.world, "R2o: ... and the third releases");
    }
    {
        // Every edge is reported exactly once: a long alternating script has as many edges as the gate flipped times.
        UiMapsGate g;
        unsigned edges = 0, flips = 0;
        bool before = g.world;
        for (unsigned i = 0; i < 4000; ++i) {
            const bool named = (i / 7) % 2 == 0;   // runs of 7 frames: every run is long enough to flip the gate
            const UiMapsEdge e = uiMapsStep(g, named);
            if (e != UiMapsEdge::None) ++edges;
            if (g.world != before) ++flips;
            before = g.world;
        }
        check(edges == flips && edges > 100, "R2p: every flip of the gate is one edge, and no edge is reported without a flip");
    }
}

// ---- R3: the constants ------------------------------------------------------------------------------------------------------
void caseR3() {
    check(kUiMapsHoldFrames == 2, "R3a: two named frames hold the panel as the world (the design's count)");
    check(kUiMapsReleaseFrames == 3, "R3b: three unnamed frames release it (the VR world route's grace, kVrWorldGraceFrames)");
    check(kUiMapsReleaseFrames > kUiMapsHoldFrames, "R3c: letting go is slower than taking hold, so one stray frame is not a flap");
}

// ---- R4: flight 1's runs ----------------------------------------------------------------------------------------------------
// edvr_gfx_20260930_161545.log (Frontier, v0.18.0-rc.4-104-gbde47f81): the world was named for 13,044 frames, a map showed for 1,597
// unnamed frames, the world came back for 98, a map showed for 929, and the world was named again: "a hold and four flips".
void caseR4() {
    struct Run { bool named; uint32_t frames; };
    const Run runs[] = {{true, 13044}, {false, 1597}, {true, 98}, {false, 929}, {true, 6}};
    UiMapsGate g;
    std::vector<uint64_t> edgeFrame;
    std::vector<UiMapsEdge> edgeKind;
    uint64_t frame = 0;
    for (const Run& r : runs)
        for (uint32_t i = 0; i < r.frames; ++i, ++frame) {
            const UiMapsEdge e = uiMapsStep(g, r.named);
            if (e != UiMapsEdge::None) {
                edgeFrame.push_back(frame);
                edgeKind.push_back(e);
            }
        }
    check(edgeKind.size() == 5, "R4a: the replay makes five edges -- the first hold, then four flips");
    if (edgeKind.size() != 5) return;
    check(edgeKind[0] == UiMapsEdge::Hold && edgeKind[1] == UiMapsEdge::Release && edgeKind[2] == UiMapsEdge::Hold &&
              edgeKind[3] == UiMapsEdge::Release && edgeKind[4] == UiMapsEdge::Hold,
          "R4b: hold, release, hold, release, hold, in that order");
    check(edgeFrame[0] == 1, "R4c: the world is held on its second named frame");
    check(edgeFrame[1] == 13044 + 2, "R4d: the first map is released on its third unnamed frame (frame 13,046)");
    check(edgeFrame[2] == 13044 + 1597 + 1, "R4e: the world is held again on its second named frame");
    check(edgeFrame[3] == 13044 + 1597 + 98 + 2, "R4f: the second map is released on its third unnamed frame");
    check(edgeFrame[4] == 13044 + 1597 + 98 + 929 + 1, "R4g: the world is held again on the second named frame after it");
    check(g.world, "R4h: the replay ends with the world held");
}

// ---- R5: the combine, and the key-off contract -----------------------------------------------------------------------------
void caseR5() {
    // The frozen copy of today's expression (ui_layer.cpp onFootGateTick before the maps gate: held = byJournal || byDepth).
    auto today = [](bool byJournal, bool byDepth) { return byJournal || byDepth; };
    bool offEqualsToday = true, onFollowsNaming = true;
    unsigned cases = 0;
    for (int world = 0; world < 2; ++world)
        for (uint32_t run = 0; run < 4; ++run)
            for (int j = 0; j < 2; ++j)
                for (int d = 0; d < 2; ++d) {
                    UiMapsGate g;
                    g.world = world != 0;
                    g.run = run;
                    offEqualsToday = offEqualsToday && uiMapsGateHeld(false, g, j != 0, d != 0) == today(j != 0, d != 0);
                    onFollowsNaming = onFollowsNaming && uiMapsGateHeld(true, g, j != 0, d != 0) == (world != 0);
                    ++cases;
                }
    check(cases == 2 * 4 * 2 * 2, "R5a: (setup) every input of the combine was tried");
    check(offEqualsToday, "R5b: with the gate not decided by naming (the key off, the layer or screen motion not live) the combine is today's journal OR depth, for every input");
    check(onFollowsNaming, "R5c: decided by naming, the combine is the step's verdict alone and the journal and the depth are not asked");
}

// ---- R6: the carry ----------------------------------------------------------------------------------------------------------
void caseR6() {
    UiMapsGate g;
    g.world = false;
    g.run = 1;
    uiMapsCarry(g, true);
    check(g.world && g.run == 0, "R6a: a switch in while today's gate holds the world starts the step held, with no run");
    g.run = 2;
    uiMapsCarry(g, false);
    check(!g.world && g.run == 0, "R6b: a switch in while today's gate does not hold the world starts it released, with no run");
    // A held world carried in stays held through two unnamed frames (a map that opened just before the switch is released in the usual three).
    UiMapsGate h;
    uiMapsCarry(h, true);
    uiMapsStep(h, false);
    uiMapsStep(h, false);
    check(h.world, "R6c: a carried world is not released by fewer than three unnamed frames");
    check(uiMapsStep(h, false) == UiMapsEdge::Release, "R6d: ... and is by the third");
}

// ---- R7: the door -----------------------------------------------------------------------------------------------------------
void caseR7() {
    check(uiMapsDoor(0, 0, 2, 2) == UiMapsDoor::No, "R7a: sequence 0 is never a take (a zero-initialised mark cannot match the first frame)");
    check(uiMapsDoor(41, 42, 2, 2) == UiMapsDoor::No && uiMapsDoor(43, 42, 2, 2) == UiMapsDoor::No,
          "R7b: a take marked in another sequence is not this sequence's");
    check(uiMapsDoor(42, 42, 2, 2) == UiMapsDoor::Yes, "R7c: the take in this sequence with every eye draw taken holds the eye's whole picture");
    check(uiMapsDoor(42, 42, 3, 2) == UiMapsDoor::NotEmpty, "R7d: one eye draw the layer did not take leaves the eye with something else in it");
    check(uiMapsDoor(42, 42, 2, 3) == UiMapsDoor::Yes && uiMapsDoor(42, 42, 1, 1) == UiMapsDoor::Yes,
          "R7e: taken draws counted beyond the game's (the layer's own) never make an eye not empty");
    check(uiMapsDoor(42, 42, 4000, 2) == UiMapsDoor::NotEmpty, "R7f: a cockpit's thousands of eye draws are a scene in the eye");
    check(uiMapsDoor(42, 43, 0, 0) == UiMapsDoor::No, "R7g: no take, no door, whatever the counts");
    check(uiMapsDoor(~0ull, ~0ull, 2, 2) == UiMapsDoor::Yes, "R7h: the largest sequence is a sequence");
}

// ---- R8: the lines ----------------------------------------------------------------------------------------------------------
void caseR8() {
    char line[1400];
    int n = uiMapsFormatOn(line, sizeof(line), 4242, true, "on foot");
    check(n > 0 && n < 900 && contains(line, "on foot maps sharp: ON at frame=4242 ") && contains(line, "(the maps gate is on)") &&
              contains(line, "2 frames in a row hold it, 3 release it") && contains(line, "the world (the journal: on foot)"),
          "R8a: the ON line says the frame, the key, the counts and the gate it starts as");
    n = uiMapsFormatOn(line, sizeof(line), 7, false, "aboard");
    check(n > 0 && contains(line, "not the world (the journal: aboard)"), "R8b: ... and, when today's gate does not hold the world, says that");
    n = uiMapsFormatOff(line, sizeof(line), 99, "the key went off", true);
    check(n > 0 && n < 700 && contains(line, "on foot maps sharp: OFF at frame=99 (the key went off)") && contains(line, "the layer held a panel at that moment"),
          "R8c: the OFF line names the reason and says when the layer held a panel");
    n = uiMapsFormatOff(line, sizeof(line), 99, "the UI layer is not live", false);
    check(n > 0 && !contains(line, "the layer held a panel"), "R8d: ... and says nothing about a panel when it held the world");
    n = uiMapsFormatTake(line, sizeof(line), 13046, 3, 13046, 144.9, "on foot");
    check(n > 0 && n < 700 && contains(line, "on foot maps sharp: the layer TAKES the 2D screen at frame=13046: no world camera named its source for 3 frames in a row") &&
              contains(line, "after 13046 world frames, 144.9 s") && contains(line, "the journal: on foot"),
          "R8e: the TAKES line says the frame, the unnamed run, how long the world had run and what the journal says");
    char why[128];
    uiMapsFormatNamedWhy(why, sizeof(why), 2);
    check(std::strcmp(why, "a world camera named the screen's source for 2 frames in a row") == 0, "R8f: the hand-back reason for a hold says the named run");
    n = uiMapsFormatHandBack(line, sizeof(line), 14645, 1599, 17.8, 3190, 2, why);
    check(n > 0 && n < 700 && contains(line, "on foot maps sharp: the layer HANDS BACK the 2D screen at frame=14645 after 1599 panel frames (17.8 s;") &&
              contains(line, "3190 eyes through the layer-only door, 2 kept the upscaler") && contains(line, "a world camera named the screen's source for 2 frames in a row."),
          "R8g: the HANDS BACK line says the frame, the length of the panel period, what the door did and the reason");
    n = uiMapsFormatNotLive(line, sizeof(line), "fix.ui_quality is off");
    check(n > 0 && contains(line, "on foot maps sharp: the maps gate is on but") && contains(line, "fix.ui_quality is off."),
          "R8h: the not-live line says the key is on and why nothing changes");
    n = uiMapsFormatNotEmpty(line, sizeof(line), 1, 8279, 5, 2);
    check(n > 0 && contains(line, "for eye 1 (sequence 8279)") && contains(line, "drew 5 draw(s) into eye-sized targets this frame and the layer took 2") &&
              contains(line, "the eye keeps the upscaler"),
          "R8i: the not-empty line names the eye, the sequence and both counts");
    // Every line, at its longest, stays inside the log's 1169 characters.
    char longest[1400];
    const int a = uiMapsFormatOn(longest, sizeof(longest), ~0ull, false, "no Flags2 in Status.json (a menu, or no file yet)");
    const int b = uiMapsFormatOff(longest, sizeof(longest), ~0ull, "screen motion is not live (fix.temporal_aa is off, or it stood down)", true);
    const int c = uiMapsFormatTake(longest, sizeof(longest), ~0ull, 3, ~0ull, 99999999.9, "no Flags2 in Status.json (a menu, or no file yet)");
    const int d = uiMapsFormatHandBack(longest, sizeof(longest), ~0ull, ~0ull, 99999999.9, ~0ull, ~0ull, why);
    const int e = uiMapsFormatNotLive(longest, sizeof(longest), "the eye jitter is not as shipped");
    check(a < 1100 && b < 1100 && c < 1100 && d < 1100 && e < 1100, "R8j: no line is longer than the log can carry");
}

// ---- R9: the 5 s window -----------------------------------------------------------------------------------------------------
void caseR9() {
    UiMapsWindow w;
    char line[1000];
    int n = uiMapsFormatWindow(line, sizeof(line), 5.0, "naming", true, w);
    check(n > 0 && contains(line, "on foot maps sharp 5s: key=on 5 s mode=naming gate=world frames=0 named=0 unnamed=0 world-frames=0 panel-frames=0 holds=0 releases=0 "
                                   "screen-takes=0 recognised=0 door-layer-only=0 door-not-empty=0 not-live-frames=0 screen-draws=0"),
          "R9a: a window of zeros is still a line, with every counter in it (screen-draws last), so an absent line means the code never ran");
    w.frames = 450; w.named = 12; w.unnamed = 438; w.worldFrames = 14; w.panelFrames = 436; w.holds = 1; w.releases = 2; w.screenTakes = 872;
    w.recognised = 871; w.doorLayerOnly = 870; w.doorNotEmpty = 3; w.notLive = 4; w.screenDraws = 900;
    n = uiMapsFormatWindow(line, sizeof(line), 5.0, "fallback", false, w);
    check(n > 0 && n < 600 && contains(line, "mode=fallback gate=panel frames=450 named=12 unnamed=438 world-frames=14 panel-frames=436 holds=1 releases=2 screen-takes=872 "
                                             "recognised=871 door-layer-only=870 door-not-empty=3 not-live-frames=4 screen-draws=900"),
          "R9b: every counter is printed under its name, and the gate and the mode with them");
    w.reset();
    check(w.frames == 0 && w.named == 0 && w.unnamed == 0 && w.worldFrames == 0 && w.panelFrames == 0 && w.holds == 0 && w.releases == 0 && w.screenTakes == 0 &&
              w.recognised == 0 && w.doorLayerOnly == 0 && w.doorNotEmpty == 0 && w.notLive == 0 && w.screenDraws == 0,
          "R9c: a window's reset zeroes every counter");
}

// ---- the reader's fixture ---------------------------------------------------------------------------------------------------
// tools\maps_sharp_fixture.log is the synthetic flight the reader's self-test (tools\edvr_log.py --maps-sharp) parses: every
// "on foot maps sharp" line in it is the formatters' own output for the numbers below (the rig compares the file to this text, byte
// for byte, so a formatter that changes fails HERE and the fixture is regenerated: on_foot_maps_test --emit-fixture >
// tools\maps_sharp_fixture.log); the lines of other modules are literal. The story: the world for 2:30, a map opened for 17.8 s (the VR
// world route released on the same boundary and owned again 144 ms after the hand-back), a 4-frame blip, a menu over the next
// ten seconds, the key turned off.
std::string fixtureText() {
    std::string out;
    auto add = [&](const char* ts, const char* text) {
        out += "[";
        out += ts;
        out += "] ";
        out += text;
        out += "\n";
    };
    char line[1400];
    char why[128];
    uiMapsFormatNamedWhy(why, sizeof(why), kUiMapsHoldFrames);
    add("16:15:45.128", "version v0.0.0-fixture (build 00000000) -- this DLL was linked 2026-10-01 00:00:00 UTC");
    uiMapsFormatOn(line, sizeof(line), 900, true, "on foot");
    add("16:20:10.100", line);
    UiMapsWindow w;
    // screen-draws is two composites a frame (one per eye) in every window of a flight on foot, whoever drew them: the world's are
    // re-issued or left in the game's frame, a map's are taken. A cockpit window would say 0 (see the reader's boarding case).
    w.frames = 450; w.named = 450; w.worldFrames = 450; w.screenDraws = 900;
    uiMapsFormatWindow(line, sizeof(line), 5.0, "naming", true, w);
    add("16:20:15.101", line);
    add("16:22:40.100", line);
    uiMapsFormatTake(line, sizeof(line), 27844, kUiMapsReleaseFrames, 26944, 149.9, "on foot");
    add("16:22:41.915", line);
    add("16:22:41.915", "vr world route: RELEASED the world at frame=26844 (on-foot-gate-lost) after 13044 owned frame(s); the eye shift is back on and the eye route serves the eyes");
    w = UiMapsWindow{};
    w.frames = 450; w.named = 43; w.unnamed = 407; w.worldFrames = 45; w.panelFrames = 405; w.releases = 1; w.screenTakes = 810; w.recognised = 810; w.doorLayerOnly = 810;
    w.screenDraws = 900;
    uiMapsFormatWindow(line, sizeof(line), 5.0, "naming", false, w);
    add("16:22:45.100", line);
    uiMapsFormatNotEmpty(line, sizeof(line), 0, 31200, 3, 2);
    add("16:22:47.300", line);
    w = UiMapsWindow{};
    w.frames = 450; w.unnamed = 450; w.panelFrames = 450; w.screenTakes = 900; w.recognised = 900; w.doorLayerOnly = 898; w.doorNotEmpty = 2;
    w.screenDraws = 900;
    uiMapsFormatWindow(line, sizeof(line), 5.0, "naming", false, w);
    add("16:22:50.100", line);
    add("16:22:55.100", line);
    uiMapsFormatHandBack(line, sizeof(line), 29443, 1599, 17.8, 3190, 2, why);
    add("16:22:59.982", line);
    w = UiMapsWindow{};
    w.frames = 450; w.named = 9; w.unnamed = 441; w.worldFrames = 6; w.panelFrames = 444; w.holds = 1; w.screenTakes = 888; w.recognised = 888; w.doorLayerOnly = 888;
    w.screenDraws = 900;
    uiMapsFormatWindow(line, sizeof(line), 5.0, "naming", true, w);
    add("16:23:00.100", line);
    add("16:23:00.126", "vr world route: OWNS the world from frame=29452 after 8 treated frames in a row; the eye shift is off and the layer takes the screen draw on every frame the route treats (the eye route serves the rest)");
    w = UiMapsWindow{};
    w.frames = 450; w.named = 450; w.worldFrames = 450; w.screenDraws = 900;
    uiMapsFormatWindow(line, sizeof(line), 5.0, "naming", true, w);
    add("16:23:05.100", line);
    uiMapsFormatTake(line, sizeof(line), 30000, kUiMapsReleaseFrames, 800, 8.9, "on foot");
    add("16:23:20.000", line);
    uiMapsFormatHandBack(line, sizeof(line), 30004, 4, 0.0, 8, 0, why);
    add("16:23:20.050", line);
    uiMapsFormatTake(line, sizeof(line), 31000, kUiMapsReleaseFrames, 996, 11.1, "on foot");
    add("16:23:30.000", line);
    uiMapsFormatHandBack(line, sizeof(line), 31900, 900, 10.0, 1800, 0, why);
    add("16:23:40.000", line);
    uiMapsFormatOff(line, sizeof(line), 32500, "the key went off", false);
    add("16:23:45.000", line);
    return out;
}

// ---- the pins, by source scan ---------------------------------------------------------------------------------------------
std::string g_root;
std::string readFile(const char* rel) {
    std::ifstream in(g_root + "\\" + rel, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}
// The text with every run of whitespace removed (the layout is not what is pinned).
std::string squeeze(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') out += c;
    return out;
}
// The squeezed text of the function whose definition starts at `head` (first occurrence), up to its closing brace at column 0.
std::string functionBody(const std::string& src, const char* head) {
    const size_t at = src.find(head);
    if (at == std::string::npos) return {};
    size_t end = src.find("\n}\n", at);
    if (end == std::string::npos) end = src.size();
    return squeeze(src.substr(at, end - at + 2));
}
bool has(const std::string& s, const char* piece) { return s.find(squeeze(piece)) != std::string::npos; }
size_t countOf(const std::string& s, const std::string& needle) {
    size_t n = 0;
    for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + 1)) ++n;
    return n;
}
bool inOrder(const std::string& s, std::initializer_list<const char*> pieces) {
    size_t prev = 0;
    for (const char* p : pieces) {
        const size_t at = s.find(squeeze(p), prev);
        if (at == std::string::npos) return false;
        prev = at;
    }
    return true;
}

void pins() {
    if (g_root.empty()) {
        std::puts("  pins: no repo root given (--self-test <root>); the source pins were not run");
        return;
    }
    const std::string layer = readFile("src\\d3d11\\ui_layer.cpp");
    const std::string vs = readFile("src\\d3d11\\vscreen.cpp");
    const std::string sm = readFile("src\\d3d11\\screen_motion.cpp");
    const std::string nt = readFile("src\\d3d11\\native_temporal.cpp");
    const std::string ns = readFile("src\\d3d11\\native_sharpen.cpp");
    check(!layer.empty() && !vs.empty() && !sm.empty() && !nt.empty() && !ns.empty(), "P0: the five sources are readable from the repo root");
    const std::string all = squeeze(layer);

    // P1: the gate. The key off is the first test of mapsGate and everything after it is behind it; the combine is the pure one.
    const std::string gate = functionBody(layer, "bool mapsGate(uint64_t now, uint64_t gateFrame, bool today,");
    check(!gate.empty() &&
              inOrder(gate, {"const bool keyOn = m.keyCfg == UiMapsKey::On;", "if (!keyOn && !m.active) {", "detail::g_uiLayerMapsOn = false;",
                             "*decided = false;", "return today;", "const bool smLive = screenMotionLive();", "uiMapsStep(m.gate, named)"}),
          "P1a: with the key off (and the gate not switching out) mapsGate returns today's verdict at its first test, before it reads screen motion, steps, counts or logs");
    check(gate.find("Log::get()") != std::string::npos && gate.find("Log::get()") > gate.find(squeeze("const bool smLive = screenMotionLive();")),
          "P1b: no line is logged before that early return");
    const std::string tick = functionBody(layer, "void onFootGateTick() {");
    check(inOrder(tick, {"const uint64_t gateFrame = detail::g_uiLayerGateFrame++;", "if (!detail::g_uiLayerLive) {", "mapsLayerNotLive(gateFrame);", "return;",
                         "const bool byJournal = uiLayerOnFootStep(g_onFoot, known, onFoot, now);", "const bool byDepth = uiLayerWorldScreenStep(g_world, counted, draws);",
                         "mapsGate(now, gateFrame, byJournal || byDepth, known, onFoot, &decidedByNaming)", "if (g_screenHeld == before || decidedByNaming) return;"}),
          "P1c: the gate's tick still steps the journal and the depth every frame, hands today's verdict to mapsGate and keeps its own flip lines for the frames mapsGate does not decide");
    check(has(tick, "const bool named = detail::g_uiLayerNamedAt == gateFrame;") || has(gate, "const bool named = detail::g_uiLayerNamedAt == gateFrame;"),
          "P1d: a frame is named when a draw attributed itself to the frame that is ending");

    // P2: the take. Only a TAKE of the 2D screen composite, not the route's re-issue, marks the eye, and only with the gate on.
    check(has(all, "if (g_draw.family == UiLayerFamily::kScreen && !g_draw.hdr && !g_draw.reissue && detail::g_uiLayerMapsOn) { e.screenTakenSeq = g_draw.seq;") &&
              countOf(all, "screenTakenSeq=g_draw.seq;") == 1,
          "P2a: the layer marks an eye's 2D screen as taken in exactly one place: a counted draw of the screen family that is not the route's re-issue, with the gate on");
    const std::string reissueBegin = functionBody(layer, "bool uiLayerWorldReissueBegin(");
    check(has(all, "g_draw.reissue = true;") && countOf(all, "g_draw.reissue=true;") == 1 && inOrder(reissueBegin, {"g_draw.reissue = true;", "beginGuarded(ctx, 0)"}),
          "P2b: the route's re-issue sets the flag before it binds the layer, in the one place it begins");
    const std::string boundary = functionBody(layer, "void uiLayerFrameBoundary(");
    check(has(all, "++g_frameTakenDraws;") && countOf(all, "++g_frameTakenDraws;") == 1 && has(boundary, "g_frameTakenDraws = 0;"),
          "P2c: the per-frame taken-draw count is incremented once per counted draw and reset at the frame boundary");

    // P3: the door. The route's re-issued world first, then the gate's: the key off answers the route's and nothing else.
    const std::string door = functionBody(layer, "bool uiLayerDoorLayerOnly(uint32_t eye, uint64_t sequence) {");
    check(inOrder(door, {"if (vrWorldRouteDoorLayerOnly(eye, sequence)) return true;", "if (!detail::g_uiLayerMapsOn || eye > 1) return false;",
                         "vScreenEyeDrawsThisFrame()", "uiMapsDoor(g_eye[eye].screenTakenSeq, sequence, eyeDraws, g_frameTakenDraws)"}),
          "P3a: the door's predicate asks the route first, answers false for the key off before it reads a count, and decides by the pure uiMapsDoor");
    check(countOf(squeeze(nt), "uiLayerDoorLayerOnly(") == 1 && countOf(squeeze(ns), "uiLayerDoorLayerOnly(") == 1 &&
              nt.find("vrWorldRouteDoorLayerOnly(") == std::string::npos && ns.find("vrWorldRouteDoorLayerOnly(") == std::string::npos,
          "P3b: the temporal door and the sharpen door each ask the layer's one predicate once, and neither asks the route's directly any more");

    // P4: the recognition. The existing call site is untouched; the new one is behind the gate and only for a taken 2D screen; and the curved
    // screen's (curvedScreenSwallowed: the curve substitution returns before the flat tail, so for a frame the route owns it runs the
    // recognition itself, behind the route's gate) is the third and last.
    const auto recognitionPlaces = [](const std::string& squeezed) {
        return countOf(squeezed, "screenMotionRecognize()") == 3 &&
               has(squeezed, "if(uiLayer&&uiFamily==UiLayerFamily::kScreen&&uiLayerMapsOn()&&screenMotionLive()&&screenMotionRecognize())uiLayerMapsNoteRecognised();") &&
               has(squeezed, "if(screenMotionLive()&&uiLayerWorldReissuePending())screenMotionRecognize();") &&
               has(squeezed, "if(!routeOwns)return;if(screenMotionLive())screenMotionRecognize();worldScreenReissueCurved(self);");
    };
    check(recognitionPlaces(squeeze(vs)),
          "P4a: the recognition is called in exactly three places in vscreen.cpp: the route's (unchanged), a taken 2D screen's behind uiLayerMapsOn(), and the curved screen's after the substitution (behind routeOwns)");
    {   // controls: the same predicate over copies with one edit each must fail, so P4a can fail
        const std::string sq = squeeze(vs);
        const std::string third = "if(!routeOwns)return;if(screenMotionLive())screenMotionRecognize();worldScreenReissueCurved(self);";
        const size_t at = sq.find(third);
        std::string without = sq, extra = sq, ungated = sq;
        if (at != std::string::npos) {
            without.replace(at, third.size(), "if(!routeOwns)return;worldScreenReissueCurved(self);");
            extra.replace(at, third.size(), third + "screenMotionRecognize();");
            ungated.replace(at, third.size(), "if(screenMotionLive())screenMotionRecognize();worldScreenReissueCurved(self);");
        }
        check(at != std::string::npos && !recognitionPlaces(without) && !recognitionPlaces(extra) && !recognitionPlaces(ungated),
              "P4a control: without the curved screen's recognition, with a fourth, or with the curved screen's ungated by routeOwns, the pin fails");
    }
    const std::string fwd = functionBody(vs, "void forwardWithVerdict(ID3D11DeviceContext* self, DrawVerdict v,");
    check(inOrder(fwd, {"uiLayer = uiLayerDecide(self, static_cast<int>(uiFamily)", "worldReissue.on = uiLayerWorldReissuePending();",
                        "screenMotionRecognize()", "uiLayer = uiLayerNoteOther(", "if (g_state->curveThisDraw) {", "const bool layered = uiLayer && uiLayerBegin(self);"}),
          "P4b: the recognition sits right after the decision and before the curved screen's substitution and the draw's own issue, so the curved screen is covered");

    // P5: the naming. Told once, at the one place screen motion names the source.
    const std::string source = functionBody(sm, "void screenMotionSource(");
    check(countOf(squeeze(sm), "uiLayerNoteScreenNamed();") == 1 &&
              inOrder(source, {"g.sourcePrevious=g.sourceFrame;g.sourceFrame=g.frame;g.sourceWrite=next;", "uiLayerNoteScreenNamed();", "if(terrain)g.terrainFrame=g.frame;"}),
          "P5: screen motion tells the layer a frame named the screen's source in exactly one place, right where it records the naming");

    // P6: the boundary order the pull would need is not needed: the layer attributes the naming to its own frame count.
    check(has(squeeze(vs), "tkUiLayer.run([&] { uiLayerFrameBoundary(g_state->ownerCtx); });") && countOf(all, "g_uiLayerGateFrame++") == 1,
          "P6: the layer's boundary runs the gate once a frame and counts the frames itself, so a naming is attributed to the right frame whichever boundary runs first");

    // P7: the reader's fixture is the formatters' own output (a formatter that changes fails here; --emit-fixture regenerates it).
    std::string fixture = readFile("tools\\maps_sharp_fixture.log");
    fixture.erase(std::remove(fixture.begin(), fixture.end(), '\r'), fixture.end());
    check(!fixture.empty() && fixture == fixtureText(),
          "P7: tools\\maps_sharp_fixture.log is exactly what the formatters write for its numbers (regenerate: on_foot_maps_test --emit-fixture > tools\\maps_sharp_fixture.log)");

    // P8: the composites the decision SEES. The window line's screen-draws= (which the reader sets against screen-takes to tell a cockpit that
    // drew no 2D screen composite from composites drawn and not taken) is counted in exactly one place: uiLayerDecide, in the statement right
    // after the family is set, for the screen family alone and only while the maps gate is on -- whatever the decision comes to. Counted in the
    // take instead it would equal screen-takes and say nothing; counted ungated it would run with the key off.
    const auto drawsPlaces = [](const std::string& sq) {
        return countOf(sq, "++g_maps.win.screenDraws;") == 1 &&
               has(sq, "if (family == UiLayerFamily::kScreen && detail::g_uiLayerMapsOn) ++g_maps.win.screenDraws; UiLayerDrawFacts f;") &&
               inOrder(sq, {"const UiLayerFamily family = static_cast<UiLayerFamily>(familyInt);",
                            "if (family == UiLayerFamily::kScreen && detail::g_uiLayerMapsOn) ++g_maps.win.screenDraws;"});
    };
    check(drawsPlaces(all),
          "P8: screen-draws is counted once, in uiLayerDecide right after the family is set, for the screen family and only with the maps gate on");
    {   // controls: the same predicate over copies with one edit each must fail, so P8 can fail
        const std::string counter = squeeze("if (family == UiLayerFamily::kScreen && detail::g_uiLayerMapsOn) ++g_maps.win.screenDraws;");
        const std::string taken = squeeze("++g_maps.win.screenTakes;");
        const size_t at = all.find(counter), tk = all.find(taken);
        std::string removed = all, ungated = all, anyFamily = all, inTake = all, twice = all;
        if (at != std::string::npos && tk != std::string::npos) {
            removed.replace(at, counter.size(), "");
            ungated.replace(at, counter.size(), squeeze("if (family == UiLayerFamily::kScreen) ++g_maps.win.screenDraws;"));
            anyFamily.replace(at, counter.size(), squeeze("if (detail::g_uiLayerMapsOn) ++g_maps.win.screenDraws;"));
            inTake = removed;
            inTake.insert(inTake.find(taken) + taken.size(), "++g_maps.win.screenDraws;");
            twice.insert(tk + taken.size(), "++g_maps.win.screenDraws;");
        }
        check(at != std::string::npos && tk != std::string::npos && !drawsPlaces(removed) && !drawsPlaces(ungated) && !drawsPlaces(anyFamily) &&
                  !drawsPlaces(inTake) && !drawsPlaces(twice),
              "P8 control: without the counter, ungated by the maps gate, for every family, counted in the take instead, or counted twice, the pin fails");
    }
}

// ---- the cost of the naming check ------------------------------------------------------------------------------------------
// --bench: the CPU the pure half spends per frame (the step and the combine, once a frame) and per door question (four a frame), as
// the compiler built them here (/O2); the runtime around them is a handful of loads and stores (ui_layer.cpp mapsGate), measured by
// counting in the design note. Not part of the gate.
#ifdef _WIN32
#include <windows.h>
void bench() {
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    volatile uint32_t sink = 0;
    const uint32_t n = 50000000;
    UiMapsGate g;
    QueryPerformanceCounter(&a);
    for (uint32_t i = 0; i < n; ++i) {
        const bool named = (i % 7) != 0;
        const UiMapsEdge e = uiMapsStep(g, named);
        sink = sink + static_cast<uint32_t>(e) + (uiMapsGateHeld(true, g, (i & 1) != 0, (i & 2) != 0) ? 1u : 0u);
    }
    QueryPerformanceCounter(&b);
    const double step = 1e9 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart) / n;
    QueryPerformanceCounter(&a);
    for (uint32_t i = 0; i < n; ++i) sink = sink + static_cast<uint32_t>(uiMapsDoor(i & 3, i & 3 ? i & 3 : 1, 2 + (i & 1), 2));
    QueryPerformanceCounter(&b);
    const double door = 1e9 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart) / n;
    std::printf("on foot maps bench (%u iterations each): the step and the combine %.2f ns a frame; the door predicate %.2f ns a question "
                "(4 a frame) -- %.1f ns a frame in all\n", n, step, door, step + 4 * door);
}
#else
void bench() {}
#endif

// ---- the runner ------------------------------------------------------------------------------------------------------------
struct Case {
    const char* id;
    void (*run)();
};
const Case kCases[] = {{"R1", caseR1}, {"R2", caseR2}, {"R3", caseR3}, {"R4", caseR4}, {"R5", caseR5},
                       {"R6", caseR6}, {"R7", caseR7}, {"R8", caseR8}, {"R9", caseR9}};

bool selected(const std::string& only, const char* id) {
    if (only.empty()) return true;
    return ("," + only + ",").find(std::string(",") + id + ",") != std::string::npos;
}

int run(const std::string& only, bool withPins) {
    for (const char* id = only.c_str(); *id;) {
        const char* comma = std::strchr(id, ',');
        const std::string one = comma ? std::string(id, comma) : std::string(id);
        bool known = false;
        for (const Case& c : kCases) known = known || one == c.id;
        if (!known) {
            std::fprintf(stderr, "FAIL: --only names a case that does not exist: %s\n", one.c_str());
            return 1;
        }
        id = comma ? comma + 1 : id + one.size();
    }
    unsigned ran = 0;
    try {
        for (const Case& c : kCases) {
            if (!selected(only, c.id)) continue;
            c.run();
            ++ran;
        }
        if (withPins) pins();
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
    std::printf("PASS: %u on-foot maps checks (%u cases%s)\n", g_checks, ran, withPins && !g_root.empty() ? ", pins" : "");
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    std::string only;
    bool self = false, dry = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--bench")) {
            bench();
            return 0;
        }
        if (!std::strcmp(argv[i], "--emit-fixture")) {
            const std::string text = fixtureText();
#ifdef _WIN32
            _setmode(_fileno(stdout), _O_BINARY);   // LF, not the console's CRLF: the file is checked in as written
#endif
            std::fwrite(text.data(), 1, text.size(), stdout);
            return 0;
        }
        if (!std::strcmp(argv[i], "--self-test")) self = true;
        else if (!std::strcmp(argv[i], "--dry-run")) dry = true;
        else if (!std::strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (argv[i][0] != '-') g_root = argv[i];
        else {
            std::fputs("usage: on_foot_maps_test --self-test [<repo root>] [--only R1,R5,...] | --dry-run\n", stderr);
            return 2;
        }
    }
    if (dry) {
        std::puts("on foot maps test: dry run (no checks run)");
        return 0;
    }
    if (!self) {
        std::fputs("usage: on_foot_maps_test --self-test [<repo root>] [--only R1,R5,...] | --dry-run\n", stderr);
        return 2;
    }
    // The pins read the sources and are skipped under --only (the mutation tool runs one rule at a time, without a root).
    return run(only, only.empty());
}

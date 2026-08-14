// GENERATED from tools/gate_test/gate_test.cpp in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 c52a8cc8fe29a5e2]
// gate_test -- replays frame sequences through the head-offset gate.
//
// WHY
//
// The gate decides whether to move the viewpoint of a headset somebody is
// wearing, and its failure class is "the offset applied in the cockpit". It had
// no automated coverage at all: pose_test covers six lines of arithmetic and
// openvr_smoke covers the transport, while the ~500-line decision machine
// between them was only ever exercised by flying the game and reading a log
// afterwards.
//
// Six defects in it were found that way, at roughly one test flight each, and
// two more were found by a code review that the flights had already passed.
// Every one of them is a replayable sequence of (panel draws, eye draws, key
// press, view index) -- which is the whole input surface. The gate is pure:
// counters in, one published bit out.
//
// WHAT EACH SCENARIO IS
//
// The named ones below are not invented cases. They are the situations that
// have actually gone wrong, kept as tests so they cannot go wrong quietly again.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/common/config.h"
#include "../../src/common/frame_flag.h"
#include "../../src/common/log.h"
#include "../../src/d3d11/head_offset_gate.h"

using namespace edvr;

namespace {

int g_bad = 0;
uint32_t g_frame = 0;

// A frame with the flat on-foot panel composited: first person.
void panelFrame(uint32_t n = 1) {
    for (uint32_t i = 0; i < n; ++i) headOffsetGateFrame(g_frame++, 4, 120);
}

// A frame with a full stereo scene and no panel: the external camera, the
// cockpit, or anything else that draws the world into both eyes.
void sceneFrame(uint32_t n = 1) {
    for (uint32_t i = 0; i < n; ++i) headOffsetGateFrame(g_frame++, 0, 2500);
}

// Neither: a menu, a loading screen, a mode change we cannot see.
void idleFrame(uint32_t n = 1) {
    for (uint32_t i = 0; i < n; ++i) headOffsetGateFrame(g_frame++, 0, 3);
}

bool offsetOn() { return externalCameraOnFoot(); }

void check(bool want, const char* what) {
    if (offsetOn() == want) return;
    printf("  FAIL  %s -- offset is %s, expected %s\n", what,
           offsetOn() ? "ON" : "off", want ? "ON" : "off");
    ++g_bad;
}

// Every scenario starts from a clean gate with the shipped configuration.
void begin(bool keyBound) {
    headOffsetGateReset();
    headOffsetGateConfigure();
    headOffsetGateSetKeyBound(keyBound);
    headOffsetGateSetView(-1);   // nothing supplying an index unless said
    g_frame = 0;
}

// Enter the camera the way a set-up player does: on foot, press the key, the
// panel stops and the scene appears a couple of frames later.
void enterCamera() {
    panelFrame(200);
    headOffsetGateKeyPressed();
    panelFrame(2);          // the game takes a few frames to change mode
    sceneFrame(4);
}

}  // namespace

int main(int argc, char** argv) {
    // The real ini, so the scenarios run against the shipped defaults rather
    // than against numbers this file made up. A test that invents its own
    // configuration cannot tell you the configuration you ship is safe.
    const std::string dir = argc > 1 ? argv[1] : ".";
    Config::get().init(std::wstring(dir.begin(), dir.end()));
    printf("edvr gate frame-feed test\n");

    // ---------------------------------------------------------------- arming
    //
    // BOARDING A SHIP MUST NOT ARM IT. This is the one that matters most. The
    // panel stops and a full scene is drawn -- which is exactly what entering
    // the external camera looks like, and exactly what walking into your own
    // ship looks like (EVIDENCE 6ac.6b). With no camera key bound the gate has
    // no way to tell them apart, so it must do nothing.
    begin(/*keyBound=*/false);
    panelFrame(200);
    sceneFrame(600);
    check(false, "boarding a ship with no key bound");

    // ...and it must still be off much later, because a ship's cockpit draws a
    // full scene forever and the panel never comes back.
    sceneFrame(5000);
    check(false, "still in the ship 5600 frames later");

    // THE SAME, WITH THE VIEW ALREADY ON THE WANTED ONE.
    //
    // This scenario is why the two above are not enough, and writing it is what
    // showed the first version of this test did not discriminate: with the view
    // index at its default 0, the VIEW gate rejected the boarding case and the
    // arming rule was never consulted. Reverting the arming rule to its old
    // shape still passed.
    //
    // The game REMEMBERS the camera view across uses, so a player who cycled to
    // view 1 earlier has the live index reading 1 while they walk around. The
    // view gate then passes, and the arming rule is the ONLY thing left between
    // walking into your own ship and the offset applying in your cockpit.
    begin(/*keyBound=*/false);
    headOffsetGateSetView(1);
    panelFrame(200);
    sceneFrame(600);
    check(false, "boarding a ship, view already 1, no key bound");
    sceneFrame(3000);
    check(false, "...and still off deep into the flight");

    // A KEY BOUND BUT NEVER PRESSED is the same situation. gateHaveKey used to
    // be set by the first press, so a correctly configured player ran the weak
    // path until they happened to press it -- which is when they needed it.
    begin(/*keyBound=*/true);
    headOffsetGateSetView(1);
    panelFrame(200);
    sceneFrame(600);
    check(false, "key bound but never pressed, view 1, boarding a ship");

    // THE HAPPY PATH. Key bound, key pressed, panel stops, scene appears.
    begin(true);
    headOffsetGateSetView(1);        // the game says view 1
    enterCamera();
    check(true, "entering the camera on the wanted view");

    // ------------------------------------------------------------------ views
    //
    // The wrong view must not arm, even with everything else right. The default
    // camera view faces back at the commander, and placing the viewpoint at
    // their head there means facing the wrong way.
    begin(true);
    headOffsetGateSetView(0);
    enterCamera();
    check(false, "in the camera on view 0 when the offset is for view 1");

    // ...and it engages the moment the view becomes the wanted one, with no
    // further keypress.
    headOffsetGateSetView(1);
    sceneFrame(2);
    check(true, "the view changed to 1 while in the camera");

    // ...and disengages again on the way past.
    headOffsetGateSetView(2);
    sceneFrame(2);
    check(false, "the view changed to 2 while in the camera");

    // ------------------------------------------------------------------ exits
    //
    // THE KEYED EXIT, which is the only exit render state cannot supply:
    // leaving the camera for a ship produces no panel frame ever.
    begin(true);
    headOffsetGateSetView(1);
    enterCamera();
    check(true, "in the camera");
    headOffsetGateKeyPressed();
    sceneFrame(1);
    check(false, "the camera key was pressed again");
    sceneFrame(3000);
    check(false, "and it stays off in the ship afterwards");

    // THE SECOND ENTRY OF A SESSION. This failed in every session until the
    // intent age was reset on a keypress: the age accrued for the whole first
    // camera session, so the next press was born already past its grace period
    // and was discarded on the frame it was made.
    begin(true);
    headOffsetGateSetView(1);
    enterCamera();
    check(true, "first entry");
    headOffsetGateKeyPressed();      // leave
    sceneFrame(2000);                // spend a while in the ship
    check(false, "left the camera");
    panelFrame(200);                 // disembark: first person again
    headOffsetGateKeyPressed();      // enter a second time
    panelFrame(2);
    sceneFrame(4);
    check(true, "SECOND entry in the same session");

    // THE PANEL COMING BACK is first person again, so the offset comes off.
    begin(true);
    headOffsetGateSetView(1);
    enterCamera();
    check(true, "in the camera");
    panelFrame(3);
    check(false, "the flat panel came back");
    // ...and the intent went with it, so the NEXT press is a fresh entry rather
    // than a toggle back off.
    headOffsetGateKeyPressed();
    panelFrame(2);
    sceneFrame(4);
    check(true, "the press after a panel-return exit is a fresh entry");

    // NEITHER PANEL NOR SCENE for a long stretch -- a menu, a load screen --
    // drops the latch rather than carrying it into whatever comes back.
    begin(true);
    headOffsetGateSetView(1);
    enterCamera();
    check(true, "in the camera");
    idleFrame(400);
    check(false, "400 frames of neither panel nor scene");

    // -------------------------------------------------------------- intent
    //
    // A PRESS THAT DID SOMETHING ELSE expires. A camera key pressed in a menu,
    // or that the game ignored, must not latch: it would invert the next real
    // press and the offset would never arm again that session.
    begin(true);
    panelFrame(200);
    headOffsetGateKeyPressed();
    headOffsetGateSetView(1);
    panelFrame(400);                 // the panel never stops: it did not enter
    sceneFrame(4);
    check(false, "a press that never entered the camera expired");

    // -------------------------------------------------------------- the gate
    //
    // SWITCHING THE GATE OFF must clear the latch, not freeze it. It used to
    // take a bare early return, leaving gateInCamera true -- so re-enabling it
    // republished a stale latch wherever the player had gone by then.
    begin(true);
    headOffsetGateSetView(1);
    enterCamera();
    check(true, "in the camera");
    {
        // Same effect as fix.head_offset_gate = 0 arriving on a config reload.
        Config::get().set("fix.head_offset_gate", "0");
        headOffsetGateConfigure();
        sceneFrame(1);
        check(false, "the gate was switched off");
        // Now the player boards a ship while it is off...
        sceneFrame(2000);
        // ...and switches it back on. The old code resumed the stale latch here.
        Config::get().set("fix.head_offset_gate", "1");
        headOffsetGateConfigure();
        sceneFrame(4);
        check(false, "the gate was switched back on somewhere else");
    }

    // ------------------------------------------------------------------ done
    Config::get().set("fix.head_offset_gate", "1");
    headOffsetGateReset();

    if (g_bad) {
        printf("\nGATE TEST FAILED (%d)\n", g_bad);
        return 1;
    }
    printf("  ok    %u scenarios: the offset arms only where it should\n", 14u);
    printf("\nGATE TEST PASSED\n");
    return 0;
}

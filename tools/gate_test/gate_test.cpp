// GENERATED from tools/gate_test/gate_test.cpp in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 c4756e5e74e13399]
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

// The view the SHIPPED ini asks for, rather than a number written here.
//
// The scenarios are about the gate's logic, not about which view somebody has
// tuned their offsets for -- that changes, and it just did, from 1 to 2. A test
// that hardcodes it fails for the one reason that is not a bug, which trains
// people to edit the test until it goes quiet.
//
// Reading it from the file also means these scenarios run against the
// configuration that ships, so a default nobody can reach is a test failure
// rather than a surprise in a headset.
int g_wantView = 2;
int g_otherView = 3;   // any view that is not the wanted one

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

// Counted here rather than written into the summary by hand. That number has
// been wrong twice already -- it is exactly the kind of thing that drifts
// silently, and a test that misreports how much it checked is halfway to a test
// that checks nothing.
int g_checks = 0;

void check(bool want, const char* what) {
    ++g_checks;
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
//
// The scene run is 12 frames, not 4, and the difference is the test being
// realistic rather than the code being lenient. The gate now requires the panel
// to have been gone for several consecutive frames before arming -- one dropped
// frame used to be enough, which is a one-frame pose jump on any hitch. Nobody
// enters a camera for four frames; the mode change alone takes 25 to 86
// (6ac.6c), so a helper that fed four was encoding an entry that cannot happen.
void enterCamera() {
    panelFrame(200);
    headOffsetGateKeyPressed();
    panelFrame(2);          // the game takes a few frames to change mode
    sceneFrame(12);
}

}  // namespace

int main(int argc, char** argv) {
    // The real ini, so the scenarios run against the shipped defaults rather
    // than against numbers this file made up. A test that invents its own
    // configuration cannot tell you the configuration you ship is safe.
    const std::string dir = argc > 1 ? argv[1] : ".";
    Config::get().init(std::wstring(dir.begin(), dir.end()));

    g_wantView = Config::get().getIntInRange("fix.head_offset_view", 2, -1, 63);
    // A shipped -1 means "any view", which would make every view scenario below
    // vacuous rather than failing -- so it is refused here. -1 is a legitimate
    // thing for a USER to set; it is not a legitimate thing to ship, because it
    // arms in the view that faces back at the commander.
    if (g_wantView < 0) {
        printf("  FAIL  edvr.ini ships fix.head_offset_view = %d (any view), so "
               "the offset would arm in the front-facing view and every view "
               "scenario here would pass without testing anything.\n", g_wantView);
        return 1;
    }
    g_otherView = g_wantView > 0 ? g_wantView - 1 : g_wantView + 1;
    printf("edvr gate frame-feed test -- edvr.ini wants view %d\n", g_wantView);

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
    headOffsetGateSetView(g_wantView);
    panelFrame(200);
    sceneFrame(600);
    check(false, "boarding a ship, view already the wanted one, no key bound");
    sceneFrame(3000);
    check(false, "...and still off deep into the flight");

    // A KEY BOUND BUT NEVER PRESSED is the same situation. gateHaveKey used to
    // be set by the first press, so a correctly configured player ran the weak
    // path until they happened to press it -- which is when they needed it.
    begin(/*keyBound=*/true);
    headOffsetGateSetView(g_wantView);
    panelFrame(200);
    sceneFrame(600);
    check(false, "key bound but never pressed, wanted view, boarding a ship");

    // THE HAPPY PATH. Key bound, key pressed, panel stops, scene appears.
    begin(true);
    headOffsetGateSetView(g_wantView);        // the game says view 1
    enterCamera();
    check(true, "entering the camera on the wanted view");

    // ------------------------------------------------------------------ views
    //
    // The wrong view must not arm, even with everything else right. The default
    // camera view faces back at the commander, and placing the viewpoint at
    // their head there means facing the wrong way.
    begin(true);
    headOffsetGateSetView(g_otherView);
    enterCamera();
    check(false, "in the camera on view 0 when the offset is for view 1");

    // ...and it engages the moment the view becomes the wanted one, with no
    // further keypress.
    headOffsetGateSetView(g_wantView);
    sceneFrame(2);
    check(true, "the view changed to the wanted one while in the camera");

    // ...and disengages again on the way past.
    headOffsetGateSetView(g_otherView);
    sceneFrame(2);
    check(false, "the view changed away again while in the camera");

    // ------------------------------------------------------------------ exits
    //
    // THE KEYED EXIT, which is the only exit render state cannot supply:
    // leaving the camera for a ship produces no panel frame ever.
    begin(true);
    headOffsetGateSetView(g_wantView);
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
    headOffsetGateSetView(g_wantView);
    enterCamera();
    check(true, "first entry");
    headOffsetGateKeyPressed();      // leave
    sceneFrame(2000);                // spend a while in the ship
    check(false, "left the camera");
    panelFrame(200);                 // disembark: first person again
    headOffsetGateKeyPressed();      // enter a second time
    panelFrame(2);
    sceneFrame(12);
    check(true, "SECOND entry in the same session");

    // THE PANEL COMING BACK is first person again, so the offset comes off.
    begin(true);
    headOffsetGateSetView(g_wantView);
    enterCamera();
    check(true, "in the camera");
    panelFrame(3);
    check(false, "the flat panel came back");
    // ...and the intent went with it, so the NEXT press is a fresh entry rather
    // than a toggle back off.
    headOffsetGateKeyPressed();
    panelFrame(2);
    sceneFrame(12);
    check(true, "the press after a panel-return exit is a fresh entry");

    // NEITHER PANEL NOR SCENE for a long stretch -- a menu, a load screen --
    // drops the latch rather than carrying it into whatever comes back.
    begin(true);
    headOffsetGateSetView(g_wantView);
    enterCamera();
    check(true, "in the camera");
    idleFrame(400);
    check(false, "400 frames of neither panel nor scene");

    // A SECOND PRESS WHILE STILL IN FIRST PERSON is still an entry.
    //
    // Measured in a real session: two presses 1.2 s apart while entering the
    // camera, the panel not yet stopped, and the blind toggle read the second
    // as leaving. Every later entry then needed an even number of presses.
    // The panel being up is proof the player is not in the camera, so there is
    // nothing to toggle out of.
    begin(true);
    headOffsetGateSetView(g_wantView);
    panelFrame(200);
    headOffsetGateKeyPressed();      // the player presses...
    panelFrame(1);
    headOffsetGateKeyPressed();      // ...and presses again, still on the panel
    panelFrame(2);
    sceneFrame(12);
    check(true, "two presses while the panel was still up, then the camera");

    // ...and three presses is no different from one, for the same reason.
    begin(true);
    headOffsetGateSetView(g_wantView);
    panelFrame(200);
    headOffsetGateKeyPressed();
    headOffsetGateKeyPressed();
    headOffsetGateKeyPressed();
    panelFrame(2);
    sceneFrame(12);
    check(true, "three presses while the panel was up");

    // But IN the camera, a second press still means leave -- that is the case
    // the panel cannot answer, and the toggle has to survive there.
    begin(true);
    headOffsetGateSetView(g_wantView);
    enterCamera();
    check(true, "in the camera");
    headOffsetGateKeyPressed();
    sceneFrame(2);
    check(false, "pressing again IN the camera still leaves");

    // ONE DROPPED PANEL FRAME MUST NOT ARM ANYTHING.
    //
    // A hitch, a stutter, one composite missed -- sincePanel >= 1 satisfied the
    // window, so the gate armed for exactly as long as it took the panel to
    // come back. That is a one-frame pose jump of the whole offset, and the
    // panel's return then ate the pending intent as well, so the entry the
    // player actually asked for was discarded too.
    begin(true);
    headOffsetGateSetView(g_wantView);
    panelFrame(200);
    headOffsetGateKeyPressed();      // a real entry press, pending
    sceneFrame(1);                   // one frame without the panel
    check(false, "a single dropped panel frame did not arm");
    panelFrame(30);                  // ...and the panel comes back
    check(false, "still off after the panel returned");
    // The press must SURVIVE that, and the real entry still work.
    sceneFrame(12);
    check(true, "the real entry still works after the hitch");

    // THE SHIP VANITY CAMERA MUST NOT GET THE ON-FOOT OFFSET.
    //
    // The same Elite binding opens the ship's camera. On-foot panel credit used
    // to outlive the panel by 300 frames, so boarding and pressing the key
    // within about three seconds armed the on-foot offset on the ship camera --
    // the offset applied in a cockpit, which is the outcome this gate exists to
    // prevent.
    begin(true);
    headOffsetGateSetView(g_wantView);
    panelFrame(200);                 // on foot
    sceneFrame(120);                 // board the ship: a full scene, no panel
    headOffsetGateKeyPressed();      // check the ship camera
    sceneFrame(20);
    check(false, "the ship vanity camera did not get the on-foot offset");
    sceneFrame(600);
    check(false, "...and still not, later in the flight");

    // -------------------------------------------------------------- intent
    //
    // A PRESS THAT DID SOMETHING ELSE expires. A camera key pressed in a menu,
    // or that the game ignored, must not latch: it would invert the next real
    // press and the offset would never arm again that session.
    begin(true);
    panelFrame(200);
    headOffsetGateKeyPressed();
    headOffsetGateSetView(g_wantView);
    panelFrame(400);                 // the panel never stops: it did not enter
    sceneFrame(12);
    check(false, "a press that never entered the camera expired");

    // -------------------------------------------------------------- the gate
    //
    // SWITCHING THE GATE OFF must clear the latch, not freeze it. It used to
    // take a bare early return, leaving gateInCamera true -- so re-enabling it
    // republished a stale latch wherever the player had gone by then.
    begin(true);
    headOffsetGateSetView(g_wantView);
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
        sceneFrame(12);
        check(false, "the gate was switched back on somewhere else");
    }

    // ------------------------------------------------------------------ done
    Config::get().set("fix.head_offset_gate", "1");
    headOffsetGateReset();

    if (g_bad) {
        printf("\nGATE TEST FAILED (%d)\n", g_bad);
        return 1;
    }
    printf("  ok    %d assertion(s): the offset arms only where it should\n", g_checks);
    printf("\nGATE TEST PASSED\n");
    return 0;
}

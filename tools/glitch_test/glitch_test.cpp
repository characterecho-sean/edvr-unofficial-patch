// GENERATED from tools/glitch_test/glitch_test.cpp in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 1c5ff287fa48099f]
// glitch_test -- drives the transition-flash detector without the game.
//
// The port of glitch_frame.cpp into this repo compiled, linked, and passed
// every existing test on the first try. None of that is evidence that it WORKS:
// the detector is fed entirely through hooks in context_hook.cpp, and a wiring
// mistake there -- a call in the wrong place, an eye-draw count that is one
// frame stale, a Map gate that never hands it the buffer -- produces exactly
// the same clean build and a fix that silently never fires.
//
// So the module is driven directly here, with synthetic camera writes, and
// asked to do the thing it exists for. What this proves:
//
//   1. It validates on smooth motion rather than disabling itself.
//   2. It withholds the frame where the camera jumps.
//   3. It does NOT withhold ordinary frames, including fast smooth ones --
//      the failure that matters more, because a detector that marks everything
//      is judder, and judder is worse than the flash it replaces.
//   4. It refuses to act on frames that do not look like a rendered scene,
//      which is what stops it firing in menus and loading screens.
//
// What it does NOT prove: that the hooks in context_hook.cpp call any of this.
// That needs the game. See docs/TESTING.md.
//
// Usage: glitch_test.exe <scratch dir>
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/common/config.h"
#include "../../src/common/frame_flag.h"
#include "../../src/d3d11/glitch_frame.h"

using namespace edvr;

namespace {

int g_fails = 0;

void ok(const char* what) { printf("  ok    %s\n", what); }

void fail(const char* what, const std::string& detail) {
    printf("  FAIL  %s -- %s\n", what, detail.c_str());
    ++g_fails;
}

void check(const char* what, bool cond, const std::string& detail = "") {
    if (cond) ok(what);
    else fail(what, detail);
}

std::wstring widen(const char* p) {
    std::wstring out;
    for (const char* c = p; *c; ++c) out.push_back(static_cast<wchar_t>(*c));
    return out;
}

constexpr uint32_t kBytes = 5376;
constexpr uint32_t kOffset = 1100;      // in floats
constexpr uint32_t kFloats = kBytes / 4;
constexpr uint32_t kEyeDraws = 500;     // comfortably over minEyeDraws (100)

// One 5376-byte constant buffer with a camera position in it.
struct Buffer {
    float f[kFloats] = {};
    const void* res = reinterpret_cast<const void*>(0xC0FFEE);

    void setPos(float x, float y, float z) {
        f[kOffset + 0] = x;
        f[kOffset + 1] = y;
        f[kOffset + 2] = z;
    }
};

// One frame, in the order the real one happens.
//
// The verdict is sampled after the write and BEFORE the boundary, because that
// is where the compositor reads it: the real call order is Submit, Submit,
// Present, WaitGetPoses, so by the time the boundary runs the frame has already
// gone to the headset. Sampling after the boundary would test a flag nobody
// acts on -- and would have passed while the fix withheld nothing at all, which
// is a mistake this module's own comments record being made.
bool frame(Buffer& b, float x, float y, float z, uint32_t eyeDraws = kEyeDraws) {
    b.setPos(x, y, z);
    glitchFrameObserve(b.f, kBytes, b.res);
    const bool marked = glitchFrameMarked();
    glitchFrameBoundary(eyeDraws);
    clearGlitchFrame();
    return marked;
}

// A stretch of ordinary flight in which one frame in three reports the far
// shadow cascade instead of the near camera, then returns to the path.
//
// THE FIELD SHAPE, not an invention. Frames 20736, 20739, 20742, 20744, 20747,
// 20751, 20754, 20758, 20761 of one session: nine of these inside a second, each
// returning immediately -- which is why the rebase cooldown never engaged and
// the detector withheld every one of them.
//
// `sep` wobbles by a fraction of a percent per flip, because the real one does:
// the measured pair ranged 562,949 to 571,365. A fixture with a constant
// magnitude would pass with an equality test, which is the implementation this
// data refutes.
uint32_t cascadeFlips(Buffer& b, float& x, float sep, uint32_t frames) {
    uint32_t withheld = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        x += 30.0f;
        if ((i % 3) == 2) {
            // static_cast<int>, and it is not decoration: `i` is unsigned, so
            // `(i % 5) - 2` wraps to about 4.29 billion whenever i % 5 is 0 or 1.
            // The first version of this fixture did exactly that and invented a
            // second separation out of the overflow -- which the detector then
            // handled correctly, costing one frame each, and the test read that
            // as the suppression failing. Same wrap as EDVR-99.
            const float wobble =
                sep * (1.0f + 0.007f * static_cast<float>(static_cast<int>(i % 5) - 2));
            if (frame(b, x, wobble, 0.0f)) ++withheld;
        } else {
            if (frame(b, x, 0.0f, 0.0f)) ++withheld;
        }
    }
    return withheld;
}

// Ordinary flight, long enough to rebuild the prediction and settle.
void settle(Buffer& b, float& x, uint32_t frames = 20) {
    for (uint32_t i = 0; i < frames; ++i) {
        x += 30.0f;
        frame(b, x, 0.0f, 0.0f);
    }
}

// One frame off the path by `units`, returning the next frame. A real flash.
bool oneFrameExcursion(Buffer& b, float& x, float units) {
    x += 30.0f;
    const bool marked = frame(b, x, units, 0.0f);
    x += 30.0f;
    frame(b, x, 0.0f, 0.0f);
    return marked;
}

bool writeIni(const std::wstring& dir, const char* body) {
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\edvr.ini";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(f, body, static_cast<DWORD>(strlen(body)), &written, nullptr);
    CloseHandle(f);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        printf("usage: glitch_test.exe <scratch dir>\n");
        return 2;
    }
    const std::wstring scratch = widen(argv[1]);

    // Its own ini, not this repo's: the test states every value it depends on,
    // so a change to edvr.ini cannot quietly turn the detector off and leave
    // this passing vacuously.
    static const char kIni[] =
        "[fix]\r\n"
        "transition_flash = 1\r\n"
        "[advanced]\r\n"
        "transition_flash_units = 2000\r\n"
        "transition_flash_speed_factor = 8.0\r\n"
        "transition_flash_max_consecutive = 2\r\n"
        "camera_buffer_bytes = 5376\r\n"
        "camera_buffer_offset = 1100\r\n"
        "[log]\r\n"
        "enabled = 0\r\n";
    if (!writeIni(scratch, kIni)) {
        printf("  FAIL  could not write the scratch ini\n");
        return 1;
    }
    Config::get().init(scratch);
    installGlitchFrameFix();

    check("the detector arms", glitchFrameNeedsEyeDraws(),
          "it disabled itself at install");
    check("it wants the configured buffer", glitchFrameWantsBuffer(kBytes));
    check("...and no other size", !glitchFrameWantsBuffer(256));

    Buffer b;
    float x = 1000.0f;

    // --- 1. Validation on smooth motion -----------------------------------
    //
    // 300 rendered frames with the camera moving. Anything less and the
    // detector is still deciding whether it has found a camera at all, and
    // refuses to act -- which is the state a naive test would mistake for
    // "no false positives".
    for (uint32_t i = 0; i < 400; ++i) {
        x += 30.0f;                      // ordinary flight, ~30 units a frame
        frame(b, x, 0.0f, 0.0f);
    }
    check("validation passes on smooth motion", glitchFrameNeedsEyeDraws(),
          "the detector stood down instead of validating");

    // --- 2. Ordinary frames are left alone --------------------------------
    uint32_t falsePositives = 0;
    for (uint32_t i = 0; i < 200; ++i) {
        x += 30.0f;
        if (frame(b, x, 0.0f, 0.0f)) ++falsePositives;
    }
    check("ordinary frames are not withheld", falsePositives == 0,
          std::to_string(falsePositives) + " of 200 marked");

    // Fast but smooth: accelerating to supercruise speeds, far past the
    // 2000-unit floor, which must NOT fire -- the floor is compared against a
    // PREDICTION rather than against the last position, and the speed term
    // covers the acceleration on top.
    //
    // Ramped, not stepped. The first version of this test went from 30 to 4000
    // units a frame in one frame and then asserted nothing fired. That step IS
    // a discontinuity by any definition, the detector was right to mark it, and
    // the test was wrong: real acceleration is gradual, and asserting otherwise
    // would have meant weakening a detector that was working.
    float step = 30.0f;
    uint32_t fastFalsePositives = 0;
    for (uint32_t i = 0; i < 300; ++i) {
        if (step < 4000.0f) step *= 1.05f;    // 5% a frame
        x += step;
        if (frame(b, x, 0.0f, 0.0f)) ++fastFalsePositives;
    }
    check("smooth acceleration to supercruise is not withheld",
          fastFalsePositives == 0,
          std::to_string(fastFalsePositives) + " of 300 marked while accelerating");

    // Back down to ordinary flight, symmetrically, and settle there.
    while (step > 30.0f) {
        step /= 1.05f;
        x += step;
        frame(b, x, 0.0f, 0.0f);
    }
    for (uint32_t i = 0; i < 20; ++i) {
        x += 30.0f;
        frame(b, x, 0.0f, 0.0f);
    }

    // --- 3. The frame that jumps IS withheld ------------------------------
    //
    // The measured shape, from the ring dump quoted in edvr.ini: the camera
    // leaves for somewhere about 18000 units away and returns the next frame,
    // against ordinary frame-to-frame movement of around 30.
    const bool markedOnJump = frame(b, x + 18000.0f, 9000.0f, -3000.0f);
    check("the jump frame is withheld", markedOnJump,
          "a frame 18000 units off its prediction was not marked");

    // And the return is not marked a second time.
    x += 30.0f;
    const bool markedOnReturn = frame(b, x, 0.0f, 0.0f);
    check("...and only that frame", !markedOnReturn,
          "the frame that came back was withheld too, which is judder");

    // --- 4. Menus and loading screens are refused -------------------------
    //
    // The same jump on frames that did not draw a scene. This is the gate that
    // stops the galaxy map and every loading screen reading as a glitch: the
    // camera legitimately teleports there and withholding achieves nothing.
    for (uint32_t i = 0; i < 5; ++i) {
        x += 30.0f;
        frame(b, x, 0.0f, 0.0f, 20);     // 20 eye draws: a menu, not a scene
    }
    const bool markedInMenu = frame(b, x + 18000.0f, 9000.0f, -3000.0f, 20);
    check("a jump with no scene drawn is IGNORED", !markedInMenu,
          "it would fire in the galaxy map and in every loading screen");

    // --- 5. Render-pass flips are not transitions -------------------------
    //
    // EVIDENCE 6v: the signal this detector watches -- the frame's furthest
    // camera -- is selected by which passes ran, not by where the view is. On a
    // planet surface the frame alternates between two shadow cascades and the
    // "jump" between them is their fixed separation, repeating forever while the
    // player flies straight.
    //
    // Measured in the field before this was fixed: forty logged jumps, thirty
    // eight of them between 562,949 and 571,365 units, withheld in bursts of
    // nine inside a second, until the runaway guard took the whole fix off the
    // field for the session.
    settle(b, x, 40);

    // FIRST, WITHOUT THE FIX, so the fixture is known to reproduce the failure.
    // A harness that cannot produce the field failure proves nothing about a
    // change that makes it stop -- and this file's own history has an assertion
    // that passed against a detector that withheld nothing at all.
    Config::get().set("advanced.transition_flash_repeat_percent", "0");
    installGlitchFrameFix();
    settle(b, x, 10);
    const uint32_t unsuppressed = cascadeFlips(b, x, 568000.0f, 30);
    check("the fixture reproduces the judder without suppression",
          unsuppressed >= 5,
          std::to_string(unsuppressed) +
              " of 30 frames withheld; the field cadence is one in three, so a "
              "low number here means the fixture is not exercising the bug");

    Config::get().set("advanced.transition_flash_repeat_percent", "2.0");
    installGlitchFrameFix();
    settle(b, x, 10);

    // The first flip of a magnitude cannot be known to repeat, so it is
    // withheld. Every one after it is recognised.
    const uint32_t firstRun = cascadeFlips(b, x, 568000.0f, 30);
    check("a recurring separation is withheld once, not repeatedly",
          firstRun <= 1,
          std::to_string(firstRun) + " frames withheld in 30");
    const uint32_t secondRun = cascadeFlips(b, x, 568000.0f, 60);
    check("...and not at all once it is recognised", secondRun == 0,
          std::to_string(secondRun) + " frames withheld in a further 60");

    // --- 6. The real thing is still caught --------------------------------
    //
    // The regression guard for this whole change. A one-frame excursion on a
    // magnitude nothing has seen before is exactly what the fix exists to
    // withhold, and suppression must not have touched it.
    settle(b, x, 20);
    check("a novel one-frame excursion is still withheld",
          oneFrameExcursion(b, x, 18000.0f),
          "the suppression swallowed a genuine transition");

    // Two genuine transitions of SIMILAR but not equal size both fire. This is
    // the tolerance's real boundary: 2,468 and 2,609 are the closest pair among
    // fourteen measured genuine residuals, 5.7% apart, and 2% must not merge
    // them. Set against separations that repeat inside 1.5%, that is the whole
    // margin this approach runs on -- so it is asserted rather than assumed.
    settle(b, x, 20);
    const bool near1 = oneFrameExcursion(b, x, 2468.0f);
    settle(b, x, 20);
    const bool near2 = oneFrameExcursion(b, x, 2609.0f);
    check("two genuine transitions 5.7% apart are both withheld", near1 && near2,
          std::string("first ") + (near1 ? "fired" : "MISSED") + ", second " +
              (near2 ? "fired" : "MISSED"));

    // --- 6b. A pass flip must not blind the detector to a real one --------
    //
    // The consequential one, and the reason this is not only a judder fix.
    //
    // 6v.4: arrivals and wakes churn the pass mix FOR SECONDS -- which is
    // exactly when genuine transitions happen. Every mark costs the detector its
    // next frame or two while the prediction is rebuilt. So a cascade flip
    // firing shortly before the real bad frame spends the shot early, and the
    // flash the fix exists to remove is shown, with a spurious hitch on top.
    // That is the fix defeated at precisely its target event, and it plausibly
    // accounts for some share of "detected and let through" reports.
    //
    // Asserted at several spacings because the blindness is one to two frames
    // and a single spacing could miss it by luck.
    // The genuine magnitudes DIFFER, and they have to.
    //
    // The first version of this fixture used 15,000 five times, and four of them
    // were suppressed -- correctly. Five transitions of identical size are a
    // repeating residual by any definition, which is the one thing this fix
    // treats as not-a-transition. That is the tradeoff stated plainly: genuine
    // flashes are separated because real motion varies (fourteen measured, the
    // closest pair 5.7% apart), and if a build ever produced identical ones this
    // would suppress them. transition_flash_repeat_percent = 0 is the escape.
    settle(b, x, 20);
    const float kGenuine[5] = {15000.0f, 17000.0f, 19500.0f, 22500.0f, 26000.0f};
    uint32_t missedAfterFlip = 0;
    for (uint32_t gap = 1; gap <= 5; ++gap) {
        settle(b, x, 20);
        // A flip of a magnitude already known to recur...
        x += 30.0f;
        frame(b, x, 568000.0f, 0.0f);
        // ...then the genuine excursion, `gap` frames later.
        for (uint32_t i = 0; i < gap; ++i) {
            x += 30.0f;
            frame(b, x, 0.0f, 0.0f);
        }
        x += 30.0f;
        if (!frame(b, x, kGenuine[gap - 1], 0.0f)) ++missedAfterFlip;
        x += 30.0f;
        frame(b, x, 0.0f, 0.0f);
    }
    check("a recognised pass flip does not blind the detector to a real one",
          missedAfterFlip == 0,
          std::to_string(missedAfterFlip) +
              " of 5 genuine excursions were missed because a shadow-cascade flip "
              "had just spent the detector's shot");

    // --- 7. Several separations coexist -----------------------------------
    //
    // A station arrival churns the pass mix and produces more than one pair.
    // Each novel one costs a single frame; none of them costs a stream.
    settle(b, x, 20);
    uint32_t churn = 0;
    for (uint32_t round = 0; round < 3; ++round) {
        churn += cascadeFlips(b, x, 120000.0f + 40000.0f * round, 30);
    }
    check("three distinct separations cost three frames, not thirty", churn <= 3,
          std::to_string(churn) + " frames withheld across three separations");

    // --- 7b. SEVERAL CAMERAS IN ONE FRAME ---------------------------------
    //
    // The failure the first version of this fix shipped with, and the reason the
    // replay below is not enough on its own: the ring records one camera per
    // frame -- the furthest -- while the real frame hands the detector several,
    // in increasing distance, and it re-decides on each.
    //
    // The verdict used to be taken on the FIRST magnitude to cross the
    // threshold, while the residual the boundary logs and remembers is the LAST
    // one evaluated. So a frame opening on a novel magnitude and closing on a
    // known one was withheld AND recorded as recurring, and the field log has
    // exactly that: "a jump of about 564596 world units ... 2 times" followed by
    // "frame 13438 was drawn from 564654 world units ... Withheld", same
    // millisecond, same frame, both from the withheld branch.
    settle(b, x, 40);
    {
        // Teach it the separation, on a frame with a single camera.
        x += 30.0f;
        frame(b, x, 568000.0f, 0.0f);
        settle(b, x, 20);
        x += 30.0f;
        frame(b, x, 568000.0f, 0.0f);
        settle(b, x, 20);

        // Now a frame carrying TWO cameras beyond the near one: a novel
        // magnitude first, then the known separation, which is further out and
        // so is what the boundary will see.
        x += 30.0f;
        b.setPos(x, 0.0f, 0.0f);
        glitchFrameObserve(b.f, kBytes, b.res);          // the near camera
        b.setPos(x, 300000.0f, 0.0f);
        glitchFrameObserve(b.f, kBytes, b.res);          // novel, crosses first
        b.setPos(x, 568000.0f, 0.0f);
        glitchFrameObserve(b.f, kBytes, b.res);          // the known separation
        const bool marked = glitchFrameMarked();
        glitchFrameBoundary(kEyeDraws);
        clearGlitchFrame();
        check("the verdict follows the camera the frame is judged on", !marked,
              "a frame whose furthest camera is a recognised separation was "
              "withheld because an earlier, novel candidate had already decided "
              "it -- the two halves are reading different residuals");
        settle(b, x, 20);
    }

    // --- 7c. The separation DRIFTS ----------------------------------------
    //
    // It is not a fixed number. Measured over four minutes of one flight:
    // 563,308 then 548,012 then 532,120, each 2.7-2.9% from the last, so each
    // fell outside its predecessor's 2% window and cost a frame -- while all the
    // sightings in between matched fine and kept refreshing an entry pinned to a
    // number nothing had reported for a minute.
    //
    // The rate here is 0.5% per sighting, which is roughly two hundred times
    // what the field showed -- the flips arrive about thirty a second, so those
    // three readings are 0.002% apart each. Deliberately harsher than reality,
    // and still inside what the running mean can hold: the lag is d/(w-d), so
    // 0.5% at weight 0.5 settles 1.0% behind against a 2% window.
    //
    // Sixty sightings, so the total travel is 26%. No fixed tolerance covers
    // that, and one wide enough to try would be wider than the 5.7% gap between
    // the closest pair of genuine transitions.
    settle(b, x, 40);
    {
        uint32_t drifted = 0;
        float sep = 563308.0f;
        for (uint32_t i = 0; i < 60; ++i) {
            settle(b, x, 6);
            x += 30.0f;
            if (frame(b, x, sep, 0.0f)) ++drifted;
            x += 30.0f;
            frame(b, x, 0.0f, 0.0f);
            sep *= 0.995f;
        }
        check("a separation that drifts is followed, not re-learned", drifted <= 1,
              std::to_string(drifted) +
                  " frames withheld tracking one separation across 26% of drift "
                  "-- the entry is being left behind rather than following");
    }

    // --- 7d. A frame where the buffer is not a camera ---------------------
    //
    // The offset EDVR watches stops holding a camera during heavy frames.
    // Measured in a station arrival, from a ring the player dumped himself:
    // |pos| of 1.03e26 and 2.71e17 across six consecutive frames at 2,200+ eye
    // draws. finite3 passes those -- 1e26 is a perfectly finite float -- and then
    // squaring it gives 1e52 against a float ceiling of 3.4e38, so the residual
    // is infinity.
    //
    // Infinity does not merely produce a wrong answer, it POISONS the memory:
    // fabsf(entry - inf) <= inf is true, so an infinite residual matches
    // whichever entry it meets first and the running mean then drags that entry
    // to infinity. A separation learned over a minute of flight is destroyed by
    // one garbage frame, and the cascade it was suppressing starts costing
    // frames again with nothing connecting the two.
    settle(b, x, 40);
    {
        // Teach it a separation and confirm it is being suppressed.
        x += 30.0f;
        frame(b, x, 568000.0f, 0.0f);
        settle(b, x, 10);
        x += 30.0f;
        frame(b, x, 568000.0f, 0.0f);
        settle(b, x, 10);
        x += 30.0f;
        const bool suppressedBefore = !frame(b, x, 568000.0f, 0.0f);
        check("the separation is being suppressed before the garbage frame",
              suppressedBefore, "the fixture is not set up");

        // Now the buffer stops being a camera.
        settle(b, x, 10);
        x += 30.0f;
        frame(b, x, 1.03e26f, 0.0f);
        settle(b, x, 10);

        // The separation must still be recognised.
        //
        // This pins the guard inside the memory rather than the one at the
        // caller, and cannot separate them: with infinity, EVERY entry matches,
        // so the poisoning lands on whichever slot is at index 0 -- arbitrary,
        // and the first version of this case passed because it happened to miss
        // the entry under test. The assertion is kept as the property that
        // matters (a garbage frame must not cost a learned separation); the
        // caller-side check is defence in depth that no fixture can isolate.
        x += 30.0f;
        check("a garbage frame does not destroy a learned separation",
              !frame(b, x, 568000.0f, 0.0f),
              "an infinite residual matched a real entry and dragged it away");
        settle(b, x, 20);
    }

    // --- 7e. A frame with no world camera in it ---------------------------
    //
    // Elite renders camera-relative, so the main view's transform lives in head
    // space at magnitude ~0.09 and the only world-space cameras in a frame come
    // from auxiliary passes. A frame where none of those ran therefore reports
    // its furthest camera at the origin -- and the "jump" that produces is just
    // the distance from the last world camera to zero.
    //
    // Three field instances, each a good frame withheld: 7,618 units "from"
    // (-0 -0 +0), 10,277 from (-0 +0 +0), 6,856 from (+0 +0 -0). The first is
    // the frame a player reported a flash on.
    //
    // The repeat suppression cannot catch these: the magnitude is wherever the
    // previous world camera happened to be, so it never repeats.
    settle(b, x, 40);
    {
        // Flying along with a world-space pass running, as usual.
        for (uint32_t i = 0; i < 20; ++i) {
            x += 30.0f;
            frame(b, x, 4000.0f, 0.0f);
        }
        // Now a frame in which no auxiliary pass ran. The camera it reports is
        // the head-space one, a fraction of a unit from the origin.
        x += 30.0f;
        const bool markedOnOrigin = frame(b, 0.04f, -0.02f, 0.07f);
        check("a frame with no world camera is not a jump", !markedOnOrigin,
              "the furthest camera collapsing to the origin was read as motion");

        // ...and the world pass coming back is not one either, which is the
        // other half: if the origin frame had entered the track, the return to
        // the real camera would look like the same jump reversed.
        x += 30.0f;
        const bool markedOnWorldReturn = frame(b, x, 4000.0f, 0.0f);
        check("...and neither is the world camera coming back", !markedOnWorldReturn,
              "the origin frame got into the prediction and the recovery fired");
        settle(b, x, 20);
    }

    // --- 7f. The swing: a camera orbiting at a fixed radius ---------------
    //
    // Fixture E. The six positions below are measured, not invented -- the
    // withheld frames of the 19:18 session, six of ten sharing one radius
    // (7,609-7,635, inside 0.3%) while pointing in wildly different directions.
    // An auxiliary camera at a fixed distance, swinging as the player turns.
    //
    // The separation memory keys on jump size, which varies with where the swing
    // came from; it caught two of these six in the field. The radius does not
    // vary, and a sphere can be certified by watching rather than by paying a
    // withheld frame per magnitude.
    settle(b, x, 40);
    {
        // THE DIRECTION SWEEPS CONTINUOUSLY, and that is what makes this a test
        // of the radius rather than of the memory beside it.
        //
        // The first draft cycled the six measured positions in a fixed order --
        // which makes the jumps between them a repeating set of six magnitudes,
        // exactly what the separation memory already catches. It passed with the
        // sphere switched off, so it was testing the wrong invariant. The field
        // swing was continuous: a camera at one distance pointing somewhere new
        // each time, so no jump size ever repeats.
        //
        // Radius 7,619 with a 0.15% wobble, both from the measurement (six
        // withholds spanning 7,609-7,635). The direction advances 27-39 degrees
        // a step, so consecutive positions are always new.
        // THE FRAME ALTERNATES between a near camera and the sphere, which is
        // what the field did -- the sphere is an auxiliary pass that runs on
        // some frames, and the jump is from the near track out to it.
        //
        // A sweep with nothing to alternate against produces no jumps at all:
        // the step-to-step motion along a sphere is smooth, so the speed term in
        // the threshold covers it and nothing is ever marked. That draft passed
        // with the sphere switched off because there was nothing to suppress.
        //
        // The near track is deliberately kept SMALL. With it out at a hundred
        // thousand units the jump length is dominated by the track and barely
        // moves as the sphere swings, which puts consecutive jumps inside 2% of
        // each other -- and then the separation memory catches them and the
        // fixture is, again, testing the wrong invariant. In the field the two
        // were comparable and the jumps ranged 6,639 to 10,111, which is what
        // this reproduces.
        const float kRadius = 7619.0f;
        float ang = 0.0f, nx = 1500.0f;
        uint32_t withheldEarly = 0, withheldLate = 0;
        for (uint32_t i = 0; i < 240; ++i) {
            nx += 7.0f;
            bool marked = false;
            if ((i % 3) == 2) {
                // The turn RATE drifts, so no two jumps are the same size. A
                // fixed step gives a handful of repeating magnitudes, which is
                // exactly what the memory beside this one already handles.
                ang += 0.31f + 0.0037f * static_cast<float>(i);
                const float r = kRadius * (1.0f + 0.0015f * sinf(ang * 2.7f));
                // Components chosen so x^2 + y^2 + z^2 is exactly r^2: the
                // radius is the constant under test and must not wander with
                // the direction.
                marked = frame(b, r * cosf(ang), r * sinf(ang) * 0.8f,
                               r * sinf(ang) * 0.6f);
            } else {
                marked = frame(b, nx, 0.0f, 0.0f);
            }
            if (i < 90) { if (marked) ++withheldEarly; }
            else        { if (marked) ++withheldLate; }
        }
        check("an orbiting camera certifies and stops costing frames",
              withheldLate == 0,
              std::to_string(withheldLate) +
                  " frames still withheld after the sphere should have certified "
                  "(" + std::to_string(withheldEarly) + " before it did)");
    }

    // --- 7g. The real thing during a swing --------------------------------
    //
    // Fixture F, and the regression guard for the whole idea. With a certified
    // sphere active, a genuine one-frame excursion whose camera lands on NO
    // certified shell must still be withheld.
    {
        // THE SEPARATION MEMORY IS SWITCHED OFF FOR THIS CASE, so the assertion
        // can only be about the shell.
        //
        // With both active the test cannot say which one acted, and that is not
        // hypothetical: two drafts of this case failed because an earlier fixture
        // had taught the separation table a magnitude within 2% of the excursion.
        // The detector was right both times; the test was measuring the wrong
        // thing. Isolating the invariant under test is what makes a pass mean
        // what the name says.
        Config::get().set("advanced.transition_flash_repeat_percent", "0");
        installGlitchFrameFix();

        // LONGER THAN THE REBASE COOLDOWN, which is 120 frames.
        //
        // The swing ends on a jump, the return to the smooth track is a change
        // of reference frame, and that stands the detector down for 120 frames.
        // A 20-frame settle left the excursion below inside that window, so it
        // was not marked -- and the assertion read that as the sphere swallowing
        // it. The next case has the same requirement for the same reason.
        settle(b, x, 150);
        x += 30.0f;
        // 11,662 off the path -- inside the measured genuine band of 2,300 to
        // 24,000, and at a radius on no certified shell.
        const bool caught = frame(b, x, 10000.0f, -6000.0f);
        check("a genuine excursion off every shell is still withheld", caught,
              "the sphere swallowed a transition that was nowhere near it");
        x += 30.0f;
        frame(b, x, 0.0f, 0.0f);
    }

    // THE ACCEPTED LIMIT, asserted rather than left to be discovered.
    //
    // A real wrong-viewpoint frame that lands ON a certified shell, within half a
    // percent, is suppressed and therefore shown. The shell is thin and a glitch
    // offset is a vector sum with the excursion rather than a swing, so landing
    // on it is a coincidence -- and a rarer one than the blindness windows the
    // false positives themselves open at every transition (6a.1). This test
    // exists so the limit is documented by something that fails if it changes,
    // not by a comment.
    {
        settle(b, x, 150);   // past the rebase cooldown, as above
        // Straight onto the shell: same distance, a direction not seen before.
        const float onShell[3] = {0.0f, -7619.0f, 0.0f};
        x += 30.0f;
        const bool suppressedOnShell = !frame(b, onShell[0], onShell[1], onShell[2]);
        check("an excursion landing exactly on a certified shell is NOT withheld",
              suppressedOnShell,
              "this is the accepted limit of the radius invariant; if this now "
              "fires, the shell has been widened or the rule has changed");
        x += 30.0f;
        frame(b, x, 0.0f, 0.0f);

        // Back on, for the complementarity case below -- which is about the
        // separation memory doing the work the shell cannot.
        Config::get().set("advanced.transition_flash_repeat_percent", "2.0");
        installGlitchFrameFix();
    }

    // --- 7h. Complementarity ----------------------------------------------
    //
    // Fixture G. 6v's measured block alternation holds two FIXED positions, so
    // each has one direction and can never certify a sphere -- if it did, a
    // parked camera would be enough to switch the detector off. The separation
    // memory is what takes that case. Two invariants, neither redundant.
    settle(b, x, 40);
    {
        // 6v: A = (+9082 -135976 -5865), |A| = 136,405, in blocks of 4-6 frames.
        uint32_t withheld = 0;
        for (uint32_t i = 0; i < 90; ++i) {
            x += 30.0f;
            if ((i % 5) == 4) {
                if (frame(b, 9082.0f, -135976.0f, -5865.0f)) ++withheld;
            } else {
                frame(b, x, 0.0f, 0.0f);
            }
        }
        check("a parked auxiliary camera is caught, and not by the sphere",
              withheld <= 1,
              std::to_string(withheld) +
                  " frames withheld on the 6v block pair; it has one direction, so "
                  "the separation memory has to be the thing that catches it");
    }

    // --- 8. The field replay ----------------------------------------------
    //
    // Not a shape derived from the field: the field itself. These are the
    // furthest-camera positions and eye-draw counts EDVR recorded in its own ring
    // buffer, dumped with the Pause key during a burst of judder over a planet
    // surface on 2026-08-14, frames 13425-13470.
    //
    // The synthetic fixtures above all pass while this one reproduces judder,
    // which is the whole reason it is here. A fixture written from a description
    // of a bug tests the description.
    settle(b, x, 200);
    {
        struct Rec { float x, y, z; uint32_t eye; };
        static const Rec kRing[] = {
#include "field_ring_13425.inc"
        };
        uint32_t withheld = 0, farFrames = 0;
        for (const Rec& r : kRing) {
            const bool isFar = r.y < -100000.0f;
            if (isFar) ++farFrames;
            b.setPos(r.x, r.y, r.z);
            glitchFrameObserve(b.f, kBytes, b.res);
            const bool marked = glitchFrameMarked();
            glitchFrameBoundary(r.eye);
            clearGlitchFrame();
            if (marked) ++withheld;
        }
        printf("      [dbg] replay: %u far frames, %u withheld\n", farFrames, withheld);
        check("the field burst costs one frame, not one every three",
              withheld <= 2,
              std::to_string(withheld) + " of " + std::to_string(farFrames) +
                  " cascade frames withheld replaying the recorded ring");
    }

    clearGlitchFrame();
    shutdownGlitchFrameFix();

    if (g_fails) {
        printf("GLITCH TEST FAILED (%d)\n", g_fails);
        return 1;
    }
    printf("GLITCH TEST PASSED\n");
    return 0;
}


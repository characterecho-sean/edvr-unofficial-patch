// GENERATED from src/d3d11/camera_view.h in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 f33d05c05a7bc453]
// Which external-camera view the game is showing, read from the game.
//
// WHY THIS EXISTS
//
// The head-offset gate applies only in one camera view -- the one behind the
// commander -- so it has to know which view is showing. Counting presses of the
// player's next-camera key works and is anchored (the game's view resets to 0
// at every launch, and this counts from 0 when the proxy loads), but it cannot
// survive a MISSED press: one dropped keypress desynchronises the count for the
// rest of the session, silently, and the offset then arms in the wrong view or
// never arms at all.
//
// The game's own value has no such failure. This reads it.
//
// READ ONLY, AND NARROW
//
// Nothing here writes to the game. What it retains is one small integer: the
// index of the camera view on screen. It records nothing else, keeps no
// contents, and skips mapped images so the game's code is not read at all.
//
// It is camera state, which is what this project is for. It is not gameplay
// state: not position, velocity, inventory, credits, weapons, missions,
// factions or market data, and nothing here can become a write.
//
// HOW IT FINDS IT (EVIDENCE 6ad.7, 6ad.8)
//
//     exe base + camera_index_type_offset    the type pointer
//     scan committed private pages for it    a contiguous array of records
//     record[ordinal] + value_offset         the view index
//
// The scan runs ONCE, and only when asked -- the caller picks the moment,
// because timing is the whole difficulty. At DLL-attach the process holds
// 127 MB of the 11 GB it reaches in play, so a scan there searches an empty
// heap and finds nothing. The head-offset gate asks on the first frame it sees
// the flat panel, which is the earliest moment the game is known to be loaded
// AND the player known to be on foot.
//
// WHEN IT BREAKS
//
// camera_index_type_offset is an offset into a specific build of the game's
// executable and a game update will move it (6ad.8g). Every failure resolves to
// "do not know" rather than to a number: no records found, too few for the
// ordinal, or a value outside the plausible range all report -1, and the caller
// falls back to counting keypresses. A wrong view number would silently arm the
// offset in the wrong place, which is worse than not knowing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace edvr {

// One stretch of the array, found by walking record addresses at its stride.
//
// `slots` is how many stride-sized POSITIONS the run spans. `present` is how
// many of them actually held a record. They are not the same number and the
// difference is the whole point: the ordinal indexes slots, not records, so a
// run with a hole in it still has its ordinal exactly where it always was.
struct CameraViewRun {
    const uint8_t* base;
    size_t         slots;
    size_t         present;
};

// Group SORTED record addresses into runs, tolerating up to `maxGap` empty
// slots inside one run.
//
// TOLERATING GAPS IS NOT A LOOSENING, IT IS THE BUG FIX
//
// The first version ended a run at the first address that was not exactly one
// stride on, which assumes every slot of the array is occupied at every moment.
// It is not. From a user's logs (issue #2), the same array, three scans, twelve
// minutes apart, identical each time:
//
//     run 0 at 000002CF115436C0, 10 record(s) -- too short for the ordinal
//     run 1 at 000002CF115437C8,  2 record(s) -- too short for the ordinal
//
// 0x...36C0 + 10 * 0x18 = 0x...37B0, and run 1 begins at 0x...37C8 -- one slot
// further on. One record in the middle had stopped carrying the type pointer.
// And 0x...36C0 + 11 * 0x18 = 0x...37C8, so run 1's base IS the ordinal-11
// record: the answer was present, readable, and correct, and the grouping threw
// it away because neither fragment was twelve long on its own. Explorer Cam was
// dead for the remaining twenty-four minutes of that session and every retry
// found the same split.
//
// The safeguards that make gaps safe to bridge are unchanged: a candidate still
// has to read a plausible view AT the ordinal, and two qualifying runs are still
// a refusal rather than a coin toss. A bridged slot that happens to be the
// ordinal reads as empty and is rejected on its own merits.
inline std::vector<CameraViewRun> cameraViewGroupRuns(
        const std::vector<const uint8_t*>& sorted, size_t stride, size_t maxGap) {
    std::vector<CameraViewRun> runs;
    if (stride == 0) return runs;
    for (const uint8_t* rec : sorted) {
        if (!runs.empty()) {
            CameraViewRun& back = runs.back();
            const uint8_t* end = back.base + back.slots * stride;
            if (rec >= end) {
                const size_t d = static_cast<size_t>(rec - end);
                if (d % stride == 0 && d / stride <= maxGap) {
                    back.slots += d / stride + 1;
                    ++back.present;
                    continue;
                }
            }
        }
        runs.push_back({rec, 1, 1});
    }
    return runs;
}

// The per-sample certification decision, exposed as a pure function so the
// frame-feed test can drive it without the scan machinery. Both of this
// module's field-caught certification bugs (6au: rebuild noise counted as
// behaviour; 6aw: a counter steps like a player) lived exactly here.
//
// A change counts only when it steps UP BY EXACTLY ONE and the player is IN
// the external camera; any other change resets everything (an oscillating
// slot must never accumulate). With a next-view key bound ("witnessed" mode)
// certification needs TWO steps each coincident with a real press -- a
// record that moves exactly when the finger does is the preset, and a
// counter ticking between presses can hardly ever qualify, with the
// two-qualifiers refusal covering the storm case where it briefly might.
// Without the key, the legacy bar stands: three sequential in-camera steps.
struct CameraViewVote {
    uint32_t last = 0;
    uint32_t changes = 0;      // sequential in-camera steps
    uint32_t coincident = 0;   // ...of which landed beside a witnessed press
    bool     primed = false;
    // Primed at the value the gate's count PREDICTED (0 after a disembark,
    // the last confirmed view within a session). An anchored candidate
    // certifies on TWO sequential in-camera steps -- which is exactly the
    // player's mandatory cycle from the opening view to their preset -- so a
    // keyless install certifies at the moment of arrival. An impostor must
    // sit at the predicted value at priming AND step twice in-camera, a far
    // narrower coincidence than the unanchored three-step bar tolerates.
    bool     anchored = false;
};
bool cameraViewCertStep(CameraViewVote* vote, uint32_t value, bool inCamera,
                        bool pressRecent, bool witnessed);

// A camera entry happened (the gate's latch, relayed by the frame path):
// rescan promptly if nothing is certified, so fresh candidates exist while
// the player is still cycling to their view. Runs on the rescan budget.
void cameraViewNudgeRescan();

// The player pressed the next-view key (called from the hotkey watcher,
// which gates it on gameplay). Timestamps the press for coincidence testing.
void cameraViewNotePress();

// Whether a next-view key is configured at all: chooses between the
// witnessed and legacy certification bars above.
void cameraViewSetPressWitness(bool nextKeyBound);

// Reads d3d11.camera_index_*. Safe to call repeatedly.
void cameraViewConfigure();

// Run the scan, once. The CALLER decides when, and must not call this at
// startup -- see the note above about the heap being empty then.
void cameraViewRequestScan();

// Does a slice of the scan if one is running, and nothing at all otherwise.
// Called once per frame from the Present path.
//
// `eyeDraws` is the frame's draw count into the eye textures, which is how the
// scanner knows the game is actually being played rather than sitting in a
// menu. The menu manages about twenty; a drawn scene is thousands. Without it,
// a player who leaves the game on the main menu can burn every scan attempt
// before they start playing -- the menu satisfies the panel signature about
// four seconds after launch, and each attempt cools down for forty seconds.
void cameraViewTick(uint32_t eyeDraws);

// The view the game reports, or -1 when it is not known.
int cameraViewCurrent();

}  // namespace edvr

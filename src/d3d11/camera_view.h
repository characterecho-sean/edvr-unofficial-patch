// GENERATED from src/d3d11/camera_view.h in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 07006448cf4be7e1]
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

#include <cstdint>

namespace edvr {

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

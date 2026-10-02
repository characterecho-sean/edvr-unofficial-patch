// Elite's terrain checkerboard rendering in VR: the worker that reads it, and the word it publishes (design doc
// docs/design-flat-temporal-aa-2026-09-23.md, section 84; the words are src/common/terrain_checkerboard_notice.h, the reading
// is terrain_checkerboard_reader.h, the toast and the Status page's hint are menu.cpp's).
//
// THE RULE: no file is ever read on the render thread. The reader polls Elite's graphics settings on a thread of its own (every
// tcn::kPollMs) and publishes one atomic word; everything here that the render thread calls is one atomic load, except the
// tick's very first call, which starts the worker. VR profile only: the flat profile never starts it, reads it or logs it.
#pragma once

#include "../common/terrain_checkerboard_notice.h"

#include <cstdint>

namespace edvr {

// Called every frame from the VR branch of the menu's tick (the VR frame boundary; the flat profile returns before it). The first
// call starts the worker, once: lazily and not from DllMain, from the Present thread, after the graphics DLL has been pinned at the
// first device creation (module_pin.h) so no thread of it can return into unmapped code. Afterwards it is one relaxed atomic load.
// Starts nothing in the flat profile, and nothing once terrainCheckerboardShutdown has run.
void terrainCheckerboardTick();

// The published pair, consistent (one atomic word): the state (Unknown until the first read finishes) and its version (0 until
// then, 1 for the first answer, then up by one at every change of the state). Any thread, no lock, no file.
void terrainCheckerboardPublished(tcn::State* state, uint32_t* version);

// Whether the option is ON in the game's active preset right now (the VR profile only: false for anything else). The Status page's
// live predicate; the hint ends when the file says off.
bool terrainCheckerboardOn();

// Stops the worker: the stop flag and its wake event, as the journal worker's. The worker only reads, so nothing needs to run at
// process exit; this is for a host that lets go of the DLL.
void terrainCheckerboardShutdown();

// ---- for tools\terrain_checkerboard_test only ---------------------------------------------------------------------------------
// Points the next start at a fixture folder pair and a short poll. Never called in the game.
void terrainCheckerboardTestSetPaths(const wchar_t* optionsFolder, const wchar_t* gameDir, uint32_t pollMs);
// Whether a worker was started and has not exited.
bool terrainCheckerboardTestWorkerRunning();
// Stops any worker, waits for it to exit, and forgets everything (the published word, the once-only start, the paths).
void terrainCheckerboardTestReset();

}  // namespace edvr

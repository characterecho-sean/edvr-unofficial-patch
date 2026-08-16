// GENERATED from src/d3d11/journal_watch.h in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 86707f2bd1b2f481]
// Elite's own journal, read for the boundaries it states outright.
//
// The game writes a documented event stream for third-party tools:
// Journal.*.log in the Saved Games folder, one JSON object per line. EDVR has
// been reconstructing two facts from render-state heuristics that this file
// simply states: whether GAMEPLAY has started (LoadGame -- before it, every
// keypress is menu navigation, and the next-view key is an arrow that menus
// eat, 6ba), and where an ON-FOOT SESSION begins (Disembark -- the boundary
// at which the game resets its external-camera view to 0, 6ay).
//
// READ ONLY, names only. The tail is polled about once a second, and the only
// thing parsed out of it is which event happened; no payload is kept. If the
// folder cannot be found or the reads keep failing, this says so once and
// stays off -- every consumer keeps its heuristic fallback, so a missing
// journal returns EDVR to exactly yesterday's behaviour.
#pragma once

#include <cstdint>

namespace edvr {

// Reads d3d11.journal_watch and d3d11.journal_dir, resolves the folder, and
// says in the log what will be watched. Call once at install.
void journalWatchConfigure();

// Poll the tail. Call once per frame; it does file work about once a second
// and nothing at all when disabled or failed.
void journalWatchTick();

// Is the journal being read at all? False when disabled by config, the folder
// was not found, or the fault budget was spent -- the callers' cue to fall
// back to heuristics.
bool journalWatchActive();

// Has gameplay started in the CURRENT game process (LoadGame seen in the
// live journal)? Menu time answers false, which is what gates the hotkeys:
// a next-view press in a menu is somebody navigating a menu.
bool journalGameplay();

// How many times the commander has disembarked this process. A change is the
// authoritative new-on-foot-session boundary.
uint32_t journalDisembarks();

void journalWatchShutdown();

}  // namespace edvr

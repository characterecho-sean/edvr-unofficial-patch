// GENERATED from src/d3d11/journal_watch.h in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 9eef3171c54c09e9]
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

// ...and boarded. A change while the gate believes the player is in the
// external camera means they left it for a vehicle.
uint32_t journalEmbarks();

// The live on-foot state from Status.json, updated by the game about once a
// second. 6bb measured the flags HOLDING through the entire external-camera
// window (Flags2 0x8011, frozen, across eight view changes) -- which is what
// makes keyless camera detection sound: on foot per the game itself, with the
// on-foot screen gone and a stereo scene rendering, is the external camera,
// because boarding drops these flags and announces itself as Embark.
// `known` is false at menus (no Flags2 in the file), when the file is
// missing, or when the watcher is off -- callers then fall back to keys.
bool journalOnFootKnown();
bool journalOnFoot();

// How many Status.json samples have been read. The gate uses it to require
// an on-foot sample taken AFTER the panel stopped before arming keylessly:
// boarding from the camera stops the panel while the previous sample still
// says on-foot, and one stale second of that must not put the offset in a
// boarding animation.
uint32_t journalStatusSamples();

void journalWatchShutdown();

}  // namespace edvr

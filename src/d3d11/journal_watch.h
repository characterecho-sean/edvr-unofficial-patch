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
// READ ONLY, names only. The tail is polled twice a second, and the only
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

// Poll the tail. Call once per frame; it does file work twice a second
// and nothing at all when disabled or failed.
void journalWatchTick();

// Is the journal being read at all? False when disabled by config, the folder
// was not found, or the fault budget was spent -- the callers' cue to fall
// back to heuristics.
bool journalWatchActive();

// Reread Status.json every 100 ms instead of the journal's ~500 ms while
// true. The FSS theater's mode gate asks for this: its authority signal
// should not lag the player by half a second more than it must.
void journalWatchSetEagerStatus(bool eager);

// Status.json's GuiFocus is 9: the player is in the Full System Scanner
// RIGHT NOW. The game states the mode outright, so entry and exit by any
// path -- keybind, ESC, an interdiction -- all land here within a poll.
// Known is false whenever the watcher is off or the field is absent
// (menus, shutdown).
bool journalFssFocusKnown();
bool journalFssFocus();

// Flags bit 4: supercruise, where the FSS keys actually do something.
bool journalSupercruiseKnown();
bool journalSupercruise();

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

// Is a jump tunnel plausibly on screen? Armed by StartJump, narrowed to the
// tunnel itself by Status' FSD-jump flag (StartJump fires at the COUNTDOWN,
// five seconds of normal space where the forming-wormhole sprite is still
// world-anchored -- measured being wrongly pinned before the flag was
// consulted), cleared by the event that resolves the jump -- FSDJump for
// hyperspace, SupercruiseEntry for the branch that never grows a tunnel --
// and expiring on its own after a cancelled charge, which resolves with
// neither. Scopes the witchspace star fix: the same sprite family draws a
// sun's flare in ordinary space, where the game places it correctly and a
// pin would be pure error (measured 2026-08-19: the flare counter-moving
// at a star).
bool journalInJumpTunnel();

void journalWatchShutdown();

// WHICH JOURNAL IS THIS SESSION'S, as a pure decision over times alone.
//
// Separated from the directory walk so it can be put in a table: the bug it
// encodes was invisible by reading and cost a user three crashed sessions to
// find (issue #19).
//
// WRITE TIME CANNOT TELL OUR JOURNAL FROM THE ONE THE LAST CRASH LEFT BEHIND.
//
// The watcher filtered on last-write time against notBefore, which is our start
// minus thirty seconds of slack. That slack is not optional -- the journal for
// this process is created around the moment the watcher arms, and the two clocks
// are not promised to be ordered -- but it is also exactly wide enough to admit
// the previous session's journal when that session ENDED inside it. A crash and
// a quick relaunch is that case, and it is not hypothetical: in issue #19 the
// game died and was restarted 15 and 19 seconds later, twice, and both times the
// dead session's journal passed the test, won on write time, and was replayed
// from the top. EDVR read the previous run's LoadGame and announced that
// gameplay had started 0.1 seconds into a process still at the launcher. The
// cold start in the same report -- no recent crash, nothing stale to find -- got
// it right five minutes later, which is what a real LoadGame looks like.
//
// Creation time separates them and write time cannot. A dead session's journal
// was created when THAT session started, which is however long it ran ago; ours
// is created when the game launches, which is around now. So provenance outranks
// recency: a file created since we started beats one merely written since,
// whatever the write times say.
//
// The same thirty seconds of slack NARROWS the hole rather than closing it. What
// a stale journal must now beat is the length of the session that wrote it, not
// the gap between its crash and the relaunch -- so a run that died less than
// thirty seconds after its own journal was created still slips through, and a
// crash during startup in a three-mod D3D chain is exactly that shape. A much
// smaller target than the old test, and not zero.
//
// The write-time test stays as a second tier rather than being replaced. If
// creation times are unavailable or untrustworthy -- a filesystem that does not
// keep them, a folder redirected somewhere exotic -- filtering on them alone
// would find nothing, and the journal watcher would silently never arm. That is
// the failure this project keeps meeting: a protection absent in exactly the
// configuration nobody can diagnose from. A file that is merely live is still
// adopted; the caller just refuses to treat its existing contents as ours.
//
// Times are FILETIME values as uint64. `count` entries, parallel arrays.
// Returns the winner's index, or -1 when nothing is live enough to consider.
struct JournalPick {
    int  index = -1;   // -1: no candidate passed the write-time test
    bool ours = false; // the winner was CREATED since notBefore
};
JournalPick journalPickNewest(const uint64_t* creation, const uint64_t* write,
                              size_t count, uint64_t notBefore);

}  // namespace edvr

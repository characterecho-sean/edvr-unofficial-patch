#pragma once

// F2: a READ-ONLY observer of the game's skinning job assembly (FUN_144c540e0, EliteDangerous64.exe+0x4C540E0, build 332841).
//
// The game assigns every skinned entity ("entry") its rows in the palette by walking a list, once a frame. The D3D side can see
// the jobs that result (the chain dispatch's t0 table) but not whose they are. This hook lets the original run, then reads the
// list the original left: per entry its address, vtable, mesh data and assigned base (skin_entity_walk.h). It writes nothing in
// game memory. It is gated: with the gate shut the relay jumps straight to the original and no C++ runs.
//
// It stands down, with one line saying why and the prefix join of the job table taking over, when: the game build is not 332841,
// the 28 prologue bytes are not the ones read from that build's executable, CodeHook refuses, the first 120 lists with something to read are all
// unusable (a list of no entries is judged only when a chain dispatch with jobs finds it the newest: 120 of those), or a second processor node appears. A fault while reading is caught per call and flags that one snapshot; it never
// reaches the game.
//
// The calling thread is recorded with every snapshot (and the first call is reported), because nothing says statically which
// thread runs this function. The consumer (the chain dispatch, on the render thread) pairs the NEWEST snapshot with its
// dispatch; skin_join.h's feeder declines a pairing that is not consecutive, and the GPU checks the pairing against the dispatch's
// own job table every frame.

#include <cstddef>
#include <cstdint>

#include "skin_join.h"

namespace edvr {

enum class SkinHookState { NotTried = 0, Armed = 1, StoodDown = 2 };

struct SkinHookStats {
    SkinHookState state = SkinHookState::NotTried;
    uint64_t calls = 0;             // calls observed
    uint64_t usable = 0;            // lists that passed skinjoin::checkSnapshot
    uint64_t faulted = 0, overflowed = 0, implausible = 0, otherUnusable = 0;
    uint64_t nodeChanges = 0;       // calls whose node differs from the first one seen
    uint64_t judged = 0;            // lists the stand-down counts: every one that was not a clean empty list
    uint64_t emptyLists = 0;        // clean empty lists (no entries, end row 1): the assembler had nobody to skin; judged only by a dispatch with jobs (below)
    uint64_t emptyWithJobs = 0;     // chain dispatches with jobs whose newest list was a clean empty one (the evidence an empty list can give)
    uint32_t firstTid = 0, lastTid = 0, threads = 0;   // threads seen (up to 4 distinct)
    uint32_t lastEntries = 0, lastEnd = 0;
    char why[320] = {};             // why it stood down, when it did
};

// Install the hook (once; later calls report the same result). `why` receives the one-sentence log line for the outcome.
SkinHookState skinEntityHookArm(char* why, size_t cap);
SkinHookState skinEntityHookState();
// Shut the gate (the relay then forwards straight to the original) or open it again. No-ops unless armed.
void skinEntityHookSetGate(bool open);
// One chain dispatch with `jobs` rows in its job table, told before the newest snapshot is asked for. An empty list is judged only here, and only
// when the table has jobs; no-ops unless armed.
void skinEntityHookNoteChain(uint32_t jobs);
// A copy of the newest snapshot; false when there is none yet or the hook stood down.
bool skinEntityHookLatest(skinjoin::Snapshot& out);
SkinHookStats skinEntityHookStats();
// The next event line (first call, a new thread, a stand-down), or false. Events are queued by the game's thread and read by
// the consumer, so the hook itself never logs.
bool skinEntityHookNextEvent(char* line, size_t cap);

#ifdef EDVR_SKIN_HOOK_TEST
unsigned skinEntityHookSelfTest(char* detail, size_t cap);
#endif

}  // namespace edvr

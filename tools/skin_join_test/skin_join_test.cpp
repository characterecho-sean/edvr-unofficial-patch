// skin_join_test: the F2 identity rules (src/d3d11/skin_join.h), pure and fast.
//
//   --dry-run    the same run (the rig never writes a file), for the gate's --dry-run convention
//   --self-test  every check; the optional argument is the repository root (unused here)
//
// Cases (each check is labelled "J<case>.<what>"; mutants.py names the case that must catch each mutation):
//   J1  the snapshot a hook may offer: what checkSnapshot accepts and refuses
//   J2  the hook protocol: who is offered what, and why not (no history, no snapshot, stale, gap, unusable, new node), plans' arrays
//   J3  the history certificates: first, gap, same buffer, shrunk, pose
//   J4  the join on a steady world: hook and prefix, the first hook frame unverified
//   J5  an entity removed or inserted: the hook follows it, the prefix loses everything after the change
//   J6  an entity's children change: the range and the layout checks
//   J7  the hook disagrees with the dispatch's table: counted, named, and the frame falls back
//   J8  the tables' limits: pose, cap, duplicate base, no history
//   J9  the documented residuals, pinned so a change to them is a decision
//   J10 the periodic line
//   J11 the hook's reading of the game's list (skin_entity_walk.h) over a fake heap with faults in it
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "skin_entity_walk.h"
#include "skin_join.h"
#include "skin_join_world.h"

using namespace edvr::skinjoin;

namespace {
unsigned g_checks = 0, g_failures = 0;
void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", label);
        std::fflush(stdout);
    }
}

using namespace skin_join_world;

// ---- J1 ------------------------------------------------------------------------------------------------------------------
void caseSnapshots() {
    const Built good = build(steady(), 1);
    const char* why = nullptr;
    check(checkSnapshot(good.snap, &why), "J1.a list the game's assembly would make is accepted");
    {
        Built b = good; b.snap.flags = kSnapFault;
        check(!checkSnapshot(b.snap), "J1.b a walk that faulted is refused");
    }
    {
        Built b = good; b.snap.flags = kSnapOverflow;
        check(!checkSnapshot(b.snap), "J1.c a list longer than a snapshot is refused");
    }
    {
        Built b = good; b.snap.n = 0;
        check(!checkSnapshot(b.snap), "J1.d an empty list is refused");
    }
    {
        Built b = good; b.snap.e[1].dst = b.snap.e[0].dst;
        check(!checkSnapshot(b.snap), "J1.e bases that do not increase are refused");
    }
    {
        Built b = good; b.snap.e[2].count = 0;
        check(!checkSnapshot(b.snap), "J1.f an entry with no bones is refused");
    }
    {
        Built b = good; b.snap.e[0].count = b.snap.e[1].dst;   // the primary job runs into the next entity
        check(!checkSnapshot(b.snap), "J1.g an entity whose primary job overruns its range is refused");
    }
    {
        Built b = good; b.snap.end = b.snap.e[2].dst;          // the last range ends before its primary job does
        check(!checkSnapshot(b.snap), "J1.h a list whose end is inside the last primary job is refused");
    }
    {
        Built b = good; b.snap.e[0].key = 0;
        check(!checkSnapshot(b.snap), "J1.i an entry without an address is refused");
    }
    {
        Built b = good; b.snap.flags = kSnapNodeChanged;
        check(!checkSnapshot(b.snap), "J1.j a list from another node is refused");
    }
    {
        Built b = good; b.snap.end = kMaxRows + 1;
        check(!checkSnapshot(b.snap), "J1.k an end beyond the tables is refused");
    }
}

// ---- J2 ------------------------------------------------------------------------------------------------------------------
void caseProtocol() {
    const World w = steady();
    {
        Run r;
        r.frame(build(w, 1), true, true, false);
        check(r.decline == kDeclineNoSnapshot && !(r.plan.flags & kPlanHook) && (r.plan.flags & kPlanHistory), "J2.a no snapshot: no hook offer, history flag kept");
    }
    {
        Run r;
        r.frame(build(w, 1), false);
        check(r.decline == kDeclineNoHistory && !(r.plan.flags & kPlanHistory) && !(r.plan.flags & kPlanHook), "J2.b no history: no join at all this frame");
    }
    {
        Run r;
        r.frame(build(w, 1));
        check(r.decline == kDeclineNoPrevious, "J2.c the first list has nothing to compare with");
        r.frame(build(w, 2));
        check(r.decline == kDeclineNone && (r.plan.flags & kPlanHook) && r.plan.m == 3 && r.plan.prevM == 3, "J2.d the second list is offered, with both lengths");
        bool identity = true;
        for (uint32_t i = 0; i < 3; ++i) identity = identity && r.plan.prevIdx[i] == i && r.plan.rs[i] == r.plan.prevRs[i];
        check(identity && r.plan.rs[3] == r.plan.end && r.plan.prevRs[3] == r.plan.prevEnd, "J2.e a steady world maps every entity to itself, bases and ends carried");
        check(r.plan.count[0] == 40 && r.plan.count[1] == 50 && r.plan.count[2] == 30, "J2.f the plan carries each entity's primary bone count");
    }
    {
        Run r;
        r.frame(build(w, 1));
        r.frame(build(w, 2));
        r.frame(build(w, 2));
        check(r.decline == kDeclineStale, "J2.g the same call twice is stale");
        r.frame(build(w, 3));
        check(r.decline == kDeclineNone, "J2.h a stale step changes nothing: the next call is still consecutive");
    }
    {
        Run r;
        r.frame(build(w, 1));
        r.frame(build(w, 2));
        r.frame(build(w, 4));
        check(r.decline == kDeclineGap, "J2.i a skipped call is a gap");
        r.frame(build(w, 5));
        check(r.decline == kDeclineNone, "J2.j after a gap the new call is the base again");
    }
    {
        Run r;
        r.frame(build(w, 1));
        Built bad = build(w, 2);
        bad.snap.e[1].count = 0;
        r.frame(bad);
        check(r.decline == kDeclineUnusable, "J2.k an unusable list is declined");
        r.frame(build(w, 3));
        check(r.decline == kDeclineNoPrevious, "J2.l and the list after it has no previous");
        r.frame(build(w, 4));
        check(r.decline == kDeclineNone, "J2.m and the one after that is offered again");
    }
    {
        Run r;
        r.frame(build(w, 1));
        Built moved = build(w, 2);
        moved.snap.node = 0x2000;
        r.frame(moved);
        check(r.decline == kDeclineNodeChanged, "J2.n a list from another node has no continuity");
        r.frame(build(w, 3));
        check(r.decline == kDeclineNodeChanged, "J2.o the node it came from is the one remembered from the call before");
    }
    {
        // keys: removed, inserted, reused with another mesh, duplicated
        World a = steady();
        World b = steady();
        b[1].key = 202;                         // a new key in the second slot
        b[2].mesh = 0x999;                      // the same key with another mesh
        Run r;
        r.frame(build(a, 1));
        r.frame(build(b, 2));
        check(r.plan.prevIdx[0] == 0 && r.plan.prevIdx[1] == kNone && r.plan.prevIdx[2] == kNone, "J2.p a new key, or a key with another mesh, has no previous entity");
        World c = steady();
        c[2].key = c[0].key;                    // two entries with one key
        Run r2;
        r2.frame(build(a, 1));
        r2.frame(build(c, 2));
        check(r2.plan.prevIdx[0] == kNone && r2.plan.prevIdx[2] == kNone, "J2.q a key listed twice joins neither entry");
        World twiceBefore = steady();
        twiceBefore[2].key = twiceBefore[0].key;   // the PREVIOUS list names one key twice
        Run r5;
        r5.frame(build(twiceBefore, 1));
        r5.frame(build(a, 2));
        check(r5.plan.prevIdx[0] == kNone && r5.plan.prevIdx[1] == 1, "J2.w a key the previous list names twice joins neither entry, and the others are unaffected");
        World d = steady();
        d[0].jobs[0].second = 41; d[0].jobs[1].second = 11;   // same entity, primary bone count changed
        Run r3;
        r3.frame(build(a, 1));
        r3.frame(build(d, 2));
        check(r3.plan.prevIdx[0] == kNone && r3.plan.prevIdx[1] == 1, "J2.r an entity whose primary bone count changed is not the same entity");
        Run r4;
        r4.frame(build(a, 1));
        World e = steady();
        e[1].vtable = 0xC0DE;
        r4.frame(build(e, 2));
        check(r4.plan.prevIdx[1] == kNone && r4.plan.prevIdx[0] == 0, "J2.s a key with another vtable is not the same entity");
    }
    {
        Run r;
        r.frame(build(w, 1, false, 7), true, true, true, 7);
        r.frame(build(w, 2, false, 7), true, true, true, 7);
        r.frame(build(w, 3, false, 8), true, true, true, 7);
        check(r.feeder.counters().sameThread == 2 && r.feeder.counters().otherThread == 1, "J2.t the producing thread is compared with the consuming one");
        check(r.feeder.counters().offered == 2 && r.feeder.counters().decline[kDeclineNoPrevious] == 1, "J2.u the counters name what was offered and what was declined");
    }
    {
        Plan p;
        std::vector<uint32_t> words;
        JoinFeeder f;
        Built b = build(w, 1);
        f.step(&b.snap, 1, true, uint32_t(b.jobs.size()), 1, p);
        Built b2 = build(w, 2);
        f.step(&b2.snap, 1, true, uint32_t(b2.jobs.size()), 0, p);
        p.words(words);
        check(words.size() == kPlanWords && words[0] == (kPlanHistory | kPlanHook) && words[1] == 3 && words[3] == p.end && words[5] == 6 && words[6] == 6 &&
              words[7] == 0 && words[kPlanRsAt + 1] == p.rs[1] && words[kPlanCountAt + 2] == 30 && words[kPlanPrevIdxAt + 1] == 1 && words[kPlanPrevRsAt + 3] == p.prevEnd,
              "J2.v the plan's words are laid out where JoinCS reads them");
    }
}

// ---- J3 ------------------------------------------------------------------------------------------------------------------
void caseHistory() {
    PaletteHistory h;
    const uint64_t big = 48ull * 174763;
    check(h.note(10, 0xA, big, 1000, true) == kHistoryFirst, "J3.a the first chain dispatch has no earlier one");
    check(h.note(11, 0xB, big, 1000, true) == kHistoryOk, "J3.b consecutive frames, buffers swapped, room enough, pose built: valid");
    check(h.note(13, 0xA, big, 1000, true) == kHistoryGap, "J3.c a missed frame is a gap");
    check(h.note(14, 0xA, big, 1000, true) == kHistorySame, "J3.d the same buffer twice means no swap");
    check(h.note(15, 0xB, 48ull * 500, 1000, true) == kHistoryOk, "J3.e a smaller NEW buffer is fine while the previous one is large enough");
    check(h.note(16, 0xA, big, 1000, true) == kHistoryShrunk, "J3.f the previous buffer cannot hold the rows in use");
    check(h.note(17, 0xB, big, 1000, false) == kHistoryPose, "J3.g no pose table from the previous frame");
    check(h.counters().frames == 7 && h.counters().verdict[kHistoryOk] == 2 && h.counters().verdict[kHistoryGap] == 1, "J3.h the verdicts are counted");
}

// ---- J4 ------------------------------------------------------------------------------------------------------------------
void caseSteady() {
    const World w = steady();
    for (int reversed = 0; reversed < 2; ++reversed) {
        Run r;
        r.frame(build(w, 1, reversed != 0));
        check(r.result.stats[kStatJoined] == 0 && r.result.stats[kStatNoHistory] == 0 && r.result.stats[kStatFailPrefix] == r.result.stats[kStatJobs],
              "J4.a the first frame has no previous: every job fails and none is joined");
        r.frame(build(w, 2, reversed != 0));
        const uint32_t* s = r.result.stats;
        check(s[kStatHookUsed] == 0 && s[kStatPrefixUsed] == 1 && s[kStatPrevNotVerified] == 1,
              "J4.b the first hook frame is not trusted (the previous frame was not verified): the prefix is used");
        check(s[kStatJoined] == 6 && s[kStatJobs] == 6, "J4.c a steady world joins every job by the prefix");
        r.frame(build(w, 3, reversed != 0));
        s = r.result.stats;
        check(s[kStatHookUsed] == 1 && s[kStatPrefixUsed] == 0 && s[kStatJoined] == 6 && s[kStatHookDisagree] == 0, "J4.d then the hook's list is used and joins every job");
        bool same = true;
        for (const JobRow& j : build(w, 3).jobs) same = same && r.joined(j.dst) == j.dst;
        check(same, "J4.e a steady world joins each base to itself");
    }
    {
        Run r;
        r.frame(build(w, 1));
        r.frame(build(w, 2));
        r.frame(build(w, 3), false);
        check(r.result.stats[kStatNoHistory] == 1 && r.result.stats[kStatJoined] == 0 && r.result.stats[kStatHookUsed] == 0 && r.result.stats[kStatPrefixUsed] == 0,
              "J4.f without history nothing is joined and neither source is credited");
        bool none = true;
        for (uint32_t i = 0; i < kMaxRows; ++i) none = none && r.joined(i) == 0;
        check(none, "J4.g and the join table is empty");
        r.frame(build(w, 4));
        check(r.result.stats[kStatPrefixUsed] == 1 && r.result.stats[kStatHookUsed] == 0 && r.result.stats[kStatJoined] == 6,
              "J4.h the frame after a no-history frame prefers the prefix again: the hook's own previous frame was a decline");
    }
}

// ---- J5 ------------------------------------------------------------------------------------------------------------------
void caseInsertRemove() {
    World a = steady();
    // remove the first entity: the other two move down
    World removed;
    removed.push_back(a[1]);
    removed.push_back(a[2]);
    {
        Run r;
        r.frame(build(a, 1));
        r.frame(build(a, 2));
        r.frame(build(a, 3));
        const Built before = build(a, 3);
        r.frame(build(removed, 4));
        const Built after = build(removed, 4);
        const uint32_t* s = r.result.stats;
        check(s[kStatHookUsed] == 1 && s[kStatHookDisagree] == 0, "J5.a a removed entity: the list and the table still agree");
        check(s[kStatJobs] == 3 && s[kStatJoined] == 3, "J5.b the surviving entities' jobs are all joined");
        check(r.joined(baseOf(after, 2000)) == baseOf(before, 2000) && r.joined(baseOf(after, 2001)) == baseOf(before, 2001) &&
              r.joined(baseOf(after, 3000)) == baseOf(before, 3000),
              "J5.c each survivor is joined to its own previous base though its base moved");
        check(baseOf(after, 2000) != baseOf(before, 2000), "J5.d (the bases did move)");
    }
    {
        // the same change joined by the table prefix alone: nothing after the change has history
        Run r;
        r.frame(build(a, 1));
        r.frame(build(a, 2));
        r.frame(build(removed, 3), true, true, false);   // the hook delivered nothing this frame
        const uint32_t* s = r.result.stats;
        check(s[kStatPrefixUsed] == 1 && s[kStatJoined] == 0 && s[kStatFailPrefix] == 3, "J5.e the prefix join loses every job after the change");
    }
    {
        // a primary job whose bind changed and whose bone count did not: the prefix ends there (job 3 of 6)
        World bound = a;
        bound[1].jobs[0].first = 2999;
        Run r;
        r.frame(build(a, 1));
        r.frame(build(a, 2));
        r.frame(build(bound, 3), true, true, false);
        const uint32_t* s = r.result.stats;
        check(s[kStatPrefixUsed] == 1 && s[kStatJoined] == 3 && s[kStatFailPrefix] == 3, "J5.j the prefix compares the bind as well as the bone count");
    }
    {
        // insert at the front
        World inserted;
        inserted.push_back(Ent{150, 0xA11CE, 0, {{5000, 25}}});
        for (const Ent& e : a) inserted.push_back(e);
        Run r;
        r.frame(build(a, 1));
        r.frame(build(a, 2));
        r.frame(build(a, 3));
        const Built before = build(a, 3);
        r.frame(build(inserted, 4));
        const Built after = build(inserted, 4);
        const uint32_t* s = r.result.stats;
        check(s[kStatHookUsed] == 1 && s[kStatJoined] == 6 && s[kStatFailNoPrevEntity] == 1, "J5.f an inserted entity has no history and the rest keep theirs");
        check(r.joined(baseOf(after, 5000)) == 0, "J5.g the new entity's base is not joined");
        check(r.joined(baseOf(after, 1002)) == baseOf(before, 1002), "J5.h a child job is joined at its offset inside its entity");
    }
    {
        // the table order is arbitrary: reversed tables, same answer
        Run r;
        r.frame(build(a, 1, true));
        r.frame(build(a, 2, true));
        r.frame(build(a, 3, true));
        r.frame(build(removed, 4, true));
        check(r.result.stats[kStatJoined] == 3 && r.result.stats[kStatHookUsed] == 1, "J5.i the hook's join does not depend on the table's order");
    }
}

// ---- J6 ------------------------------------------------------------------------------------------------------------------
void caseChildren() {
    World a = steady();
    auto warm = [&](Run& r, const World& w) { r.frame(build(w, 1)); r.frame(build(w, 2)); r.frame(build(w, 3)); };
    {
        World b = a;
        b[1].jobs.push_back({2002, 5});   // the second entity gains a child
        Run r;
        warm(r, a);
        const Built before = build(a, 3);
        r.frame(build(b, 4));
        const Built after = build(b, 4);
        const uint32_t* s = r.result.stats;
        check(s[kStatHookUsed] == 1 && s[kStatFailRange] == 3, "J6.a an entity whose range grew fails all its jobs on the range");
        check(r.joined(baseOf(after, 1000)) == baseOf(before, 1000) && r.joined(baseOf(after, 3000)) == baseOf(before, 3000) && r.joined(baseOf(after, 2000)) == 0,
              "J6.b the other entities keep their history");
    }
    {
        World b = a;
        std::swap(b[0].jobs[1], b[0].jobs[2]);   // the same range, two children exchanged
        Run r;
        warm(r, a);
        r.frame(build(b, 4));
        check(r.result.stats[kStatFailLayout] == 2 && r.result.stats[kStatJoined] == 4, "J6.c two children that traded places fail on the layout; the primary job and the others join");
    }
    {
        World same = a;
        same[0].jobs = {{1000, 40}, {1001, 12}, {1001, 6}};   // two children with one bind and two sizes
        World b = same;
        std::swap(b[0].jobs[1], b[0].jobs[2]);                // ... that trade places: the bind matches, the bone count does not
        Run r;
        warm(r, same);
        r.frame(build(b, 4));
        check(r.result.stats[kStatFailLayout] == 2 && r.result.stats[kStatJoined] == 4, "J6.e two children of one bind that traded sizes fail on the layout");
    }
    {
        World b = a;
        b[0].jobs[1].first = 1111;               // a child replaced by another mesh of the same size
        Run r;
        warm(r, a);
        r.frame(build(b, 4));
        check(r.result.stats[kStatFailLayout] == 1 && r.result.stats[kStatJoined] == 5, "J6.d a child replaced by another bind fails on the layout");
    }
}

// ---- J7 ------------------------------------------------------------------------------------------------------------------
void caseDisagree() {
    World a = steady();
    auto warm = [&](Run& r) { r.frame(build(a, 1)); r.frame(build(a, 2)); r.frame(build(a, 3)); };
    {
        // the list claims a layout the table does not have: drop a job from the table
        Run r;
        warm(r);
        Built b = build(a, 4);
        b.jobs.erase(b.jobs.begin() + 1);
        r.frame(b);
        const uint32_t* s = r.result.stats;
        check(s[kStatHookDisagree] == 1 && s[kStatHookUsed] == 0 && s[kStatPrefixUsed] == 1, "J7.a a table that does not tile the list's ranges makes the frame fall back to the prefix");
        check((s[kStatMismatchBits] & (kMmSum | kMmEntitySum)) == (kMmSum | kMmEntitySum), "J7.b and the causes are named (sum and entity sum)");
        check(r.result.prevHookOk == 0, "J7.c and the next frame will not trust the hook");
    }
    {
        Run r;
        warm(r);
        Built b = build(a, 4);
        b.jobs[0].count += 1;                        // the primary job's count disagrees with the entry's
        r.frame(b);
        check((r.result.stats[kStatMismatchBits] & kMmHeadCount) != 0 && r.result.stats[kStatHookDisagree] == 1, "J7.d a primary job whose count differs from its entry's is a disagreement");
    }
    {
        Run r;
        warm(r);
        Built b = build(a, 4);
        b.jobs[3].dst = 900;                         // a job outside every range
        r.frame(b);
        check((r.result.stats[kStatMismatchBits] & kMmNotInRange) != 0, "J7.e a job outside every entity's range is a disagreement");
    }
    {
        Run r;
        warm(r);
        Built b = build(a, 4);
        b.jobs.erase(b.jobs.begin());               // an entity's primary job missing
        r.frame(b);
        check((r.result.stats[kStatMismatchBits] & kMmHeads) != 0, "J7.f an entry with no job at its base is a disagreement");
    }
    {
        Run r;
        warm(r);
        Built b = build(a, 4);
        b.snap.n = 0;
        b.snap.flags = 0;
        r.frame(b);
        check(r.decline == kDeclineUnusable && r.result.stats[kStatHookDisagree] == 0, "J7.g an empty list is the CPU's decline, not a GPU disagreement");
        Run r2;
        warm(r2);
        Built c = build(a, 4);
        Plan p;
        r2.feeder.step(&c.snap, 1, true, uint32_t(c.jobs.size()), 0, p);
        p.m = 0;
        const JoinResult j = cpuJoin(p, c.jobs, r2.prevJobs, r2.prevInfo, r2.prevPose, 1);
        check((j.stats[kStatMismatchBits] & kMmNoPlan) != 0 && j.stats[kStatHookDisagree] == 1, "J7.h a plan with no entities is refused on the GPU too");
    }
    {
        // after a disagreement the hook has to be verified again before it is believed
        Run r;
        warm(r);
        Built b = build(a, 4);
        b.jobs.erase(b.jobs.begin() + 1);
        r.frame(b);
        r.frame(build(a, 5));
        check(r.result.stats[kStatHookUsed] == 0 && r.result.stats[kStatPrevNotVerified] == 1 && r.result.stats[kStatPrefixUsed] == 1, "J7.i the frame after a disagreement uses the prefix and says why");
        r.frame(build(a, 6));
        check(r.result.stats[kStatHookUsed] == 1, "J7.j and the one after that trusts the hook again");
    }
}

// ---- J8 ------------------------------------------------------------------------------------------------------------------
void caseLimits() {
    World a = steady();
    auto warm = [&](Run& r, bool pose = true) { r.frame(build(a, 1)); r.frame(build(a, 2)); r.frame(build(a, 3), true, pose); };
    {
        Run r;
        r.frame(build(a, 1));
        r.frame(build(a, 2));
        r.frame(build(a, 3), true, false);       // no pose records in the table the next frame will read
        r.frame(build(a, 4));
        check(r.result.stats[kStatFailPose] == 6 && r.result.stats[kStatJoined] == 0, "J8.a a base with no previous pose record has no history");
        (void)warm;
    }
    {
        Run r;
        r.frame(build(a, 1));
        r.frame(build(a, 2));
        r.frame(build(a, 3));
        Built b = build(a, 4);
        Run rr;
        rr.frame(build(a, 1)); rr.frame(build(a, 2)); rr.frame(build(a, 3));
        rr.prevPose[b.jobs[2].dst] = 0;          // a conflicted base: the pose pass killed it
        rr.frame(b);
        check(rr.result.stats[kStatFailPose] == 1 && rr.result.stats[kStatJoined] == 5 && rr.joined(b.jobs[2].dst) == 0, "J8.b a conflicted pose record fails that one base only");
    }
    {
        Run r;
        warm(r);
        Built b = build(a, 4);
        b.jobs.push_back(JobRow{0, kMaxRows - 2, 9, 10});   // runs past the tables
        b.snap.flags = 0;
        r.frame(b, true, true, false);
        check(r.result.stats[kStatFailCap] == 1, "J8.c a job running past the tables is counted and skipped");
        Built c = build(a, 5);
        c.jobs.push_back(JobRow{0, 5, 9, 0});               // no bones
        r.frame(c, true, true, false);
        check(r.result.stats[kStatFailCap] == 1, "J8.d a job with no bones is counted and skipped");
    }
    {
        Run r;
        warm(r);
        Built b = build(a, 4);
        JobRow dup = b.jobs[1];
        dup.bind = 777;
        b.jobs.push_back(dup);                              // a second job on an occupied base
        r.frame(b, true, true, false);
        check(r.result.stats[kStatDupBase] == 1 && r.result.dstInfo[dup.dst].bind == b.jobs[1].bind, "J8.e a second job on a base is counted and the first keeps it");
    }
    {
        Run r;
        warm(r);
        Built b = build(a, 4);
        r.frame(b, false);
        r.frame(build(a, 5), false);
        check(r.result.stats[kStatNoHistory] == 1 && r.result.stats[kStatJoined] == 0, "J8.f history off twice in a row joins nothing");
        check(r.result.dstInfo[b.jobs[0].dst].count == b.jobs[0].count, "J8.g but the by-base table is still built for the next frame");
    }
}

// ---- J9 ------------------------------------------------------------------------------------------------------------------
void caseResiduals() {
    World a = steady();
    // an entity destroyed and another with the same tuples created in its place in the same frame
    World replaced = a;
    replaced[1].key = 999;
    {
        Run r;
        r.frame(build(a, 1)); r.frame(build(a, 2)); r.frame(build(a, 3));
        r.frame(build(replaced, 4));
        check(r.result.stats[kStatHookUsed] == 1 && r.result.stats[kStatFailNoPrevEntity] == 2 && r.result.stats[kStatJoined] == 4,
              "J9.a the hook does not hand a replaced entity the old one's history");
    }
    {
        Run r;
        r.frame(build(a, 1)); r.frame(build(a, 2));
        r.frame(build(replaced, 3), true, true, false);
        check(r.result.stats[kStatPrefixUsed] == 1 && r.result.stats[kStatJoined] == 6, "J9.b the prefix join does (the documented residual of the fallback)");
    }
    {
        // the same address reused with the same vtable, mesh and bone count: believed to be the same entity
        Run r;
        r.frame(build(a, 1)); r.frame(build(a, 2)); r.frame(build(a, 3));
        World again = a;     // identical key, vtable, mesh, count: nothing a list can show
        r.frame(build(again, 4));
        check(r.result.stats[kStatJoined] == 6, "J9.c an entity re-created at the same address with the same mesh and size is taken for the old one (the hook's residual)");
    }
}

// ---- J10 -----------------------------------------------------------------------------------------------------------------
void caseLine() {
    uint32_t d[kStatWords]{};
    WindowCpu c;
    d[kStatFrames] = 120; d[kStatHookUsed] = 110; d[kStatPrefixUsed] = 8; d[kStatNoHistory] = 2; d[kStatJobs] = 20520; d[kStatJoined] = 20400;
    d[kStatFailNoPrevEntity] = 20; d[kStatFailPose] = 100; d[kStatHookDisagree] = 3; d[kStatMismatchBits] = kMmSum; d[kStatPoseRecords] = 4000;
    d[kStatLastJobs] = 171; d[kStatLastEntities] = 12;
    c.offered = 118; c.decline[kDeclineGap] = 1; c.sameThread = 119; c.otherThread = 0; c.history[kHistoryGap] = 2;
    const std::string line = joinLine(d, c, true, "armed");
    check(line.find("skin join:") == 0 && line.find("source=hook+prefix") != std::string::npos && line.find("hook=armed") != std::string::npos &&
          line.find("frames=120") != std::string::npos && line.find("joined=20400") != std::string::npos && line.find("disagreements 3 (causes 0x8)") != std::string::npos,
          "J10.a the line names the source, the hook's state, the frames, the joins and the disagreements");
    check(line.size() < 1150, "J10.b the line fits the log's limit");
    check(joinLine(d, c, false, "armed").find("hook=off") != std::string::npos, "J10.c a hook that never armed says off");
    uint32_t z[kStatWords]{};
    check(joinLine(z, WindowCpu{}, true, "armed").find("source=none") != std::string::npos, "J10.d a window with no frames has no source");
}
// ---- J11 -----------------------------------------------------------------------------------------------------------------
// A fake heap: blocks of bytes at addresses; a read that is not wholly inside one fails, as an unmapped page would.
struct FakeHeap {
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> blocks;
    uint64_t next = 0x100000;
    mutable unsigned reads = 0;
    uint64_t alloc(size_t n) {
        const uint64_t at = next;
        blocks.push_back({at, std::vector<uint8_t>(n, 0)});
        next += (n + 15) & ~size_t(15);
        next += 64;
        return at;
    }
    bool read(uint64_t address, void* out, size_t n) const {
        ++reads;
        for (const auto& b : blocks)
            if (address >= b.first && address + n <= b.first + b.second.size()) { std::memcpy(out, &b.second[address - b.first], n); return true; }
        return false;
    }
    void put(uint64_t address, const void* data, size_t n) {
        for (auto& b : blocks)
            if (address >= b.first && address + n <= b.first + b.second.size()) { std::memcpy(&b.second[address - b.first], data, n); return; }
    }
    template <class T> void set(uint64_t address, T v) { put(address, &v, sizeof(T)); }
};
// A node with n entries (counts, bases from `first`), as the decompile says they are laid out.
struct FakeNode {
    uint64_t node = 0;
    std::vector<uint64_t> entries, meshes;
};
FakeNode fakeList(FakeHeap& h, const std::vector<uint16_t>& counts, uint32_t first) {
    FakeNode f;
    f.node = h.alloc(0x200);
    uint32_t dst = first;
    uint64_t previous = 0;
    for (size_t i = 0; i < counts.size(); ++i) {
        const uint64_t e = h.alloc(0x100), mesh = h.alloc(0x50);
        h.set<uint64_t>(e, 0x140001000ull + (i & 1));
        h.set<uint64_t>(e + 0x38, mesh);
        h.set<uint16_t>(mesh, counts[i]);
        h.set<uint32_t>(e + 0xA8, dst);
        dst += counts[i];
        if (previous) h.set<uint64_t>(previous + 8, e); else h.set<uint64_t>(f.node + 0xA8, e);
        previous = e;
        f.entries.push_back(e);
        f.meshes.push_back(mesh);
    }
    h.set<uint32_t>(f.node + 0xC4, dst);
    return f;
}

void caseWalk() {
    auto walk = [](FakeHeap& h, uint64_t node, Snapshot& s) { walkNode([&h](uint64_t a, void* o, size_t n) { return h.read(a, o, n); }, node, s); };
    static Snapshot s;   // 33 KB: not on the stack
    {
        FakeHeap h;
        const FakeNode f = fakeList(h, {40, 50, 30}, 1);
        walk(h, f.node, s);
        check(s.n == 3 && s.flags == 0 && s.end == 121 && s.node == f.node, "J11.a a list laid out as the decompile says is read whole, with the node and its end row");
        check(s.e[0].key == f.entries[0] && s.e[0].mesh == f.meshes[0] && s.e[0].dst == 1 && s.e[0].count == 40 && s.e[1].dst == 41 && s.e[2].dst == 91 && s.e[2].count == 30,
              "J11.b each entry carries its address, mesh, base and bone count");
        check(s.e[0].vtable == 0x140001000ull && s.e[1].vtable == 0x140001001ull, "J11.c and its vtable");
        check(checkSnapshot(s), "J11.d and a list so read passes the snapshot checks");
    }
    {
        FakeHeap h;
        const FakeNode f = fakeList(h, {40, 50}, 1);
        walk(h, 0x7000000, s);
        check((s.flags & kSnapFault) && s.n == 0, "J11.e a node that is not mapped faults and reads nothing");
        h.reads = 0;
        walk(h, 0x500, s);
        check((s.flags & kSnapFault) && s.n == 0 && h.reads == 0, "J11.f a node address that cannot be a user object is refused before any read");
        h.set<uint64_t>(f.entries[0] + 8, 0x7100000);
        walk(h, f.node, s);
        check((s.flags & kSnapFault) && s.n == 1, "J11.g a next pointer into unmapped memory faults after the entries before it");
    }
    {
        FakeHeap h;
        const FakeNode f = fakeList(h, {40, 50, 30}, 1);
        h.set<uint64_t>(f.entries[1] + 8, 0x1235);
        walk(h, f.node, s);
        check((s.flags & kSnapImplausible) && s.n == 2, "J11.h a next pointer that cannot be an object is implausible, not a fault");
        h.set<uint64_t>(f.entries[1] + 8, f.entries[2]);
        h.set<uint64_t>(f.entries[2] + 0x38, 0);
        walk(h, f.node, s);
        check((s.flags & kSnapImplausible) && s.n == 2, "J11.i an entry without mesh data is implausible");
        h.set<uint64_t>(f.entries[2] + 0x38, f.meshes[2]);
        h.set<uint64_t>(f.entries[2] + 0x38, 0x7200000);
        walk(h, f.node, s);
        check((s.flags & kSnapFault) && s.n == 2, "J11.j mesh data that is not mapped faults on the bone count");
        h.set<uint64_t>(f.entries[2] + 0x38, f.meshes[2]);
        h.set<uint64_t>(f.entries[0] + 8, f.entries[0] + 4);
        walk(h, f.node, s);
        check((s.flags & kSnapImplausible) && s.n == 1, "J11.k an unaligned entry pointer is implausible");
    }
    {
        FakeHeap h;
        const FakeNode f = fakeList(h, {10, 20}, 5);
        h.set<uint64_t>(f.entries[1] + 8, f.entries[0]);
        walk(h, f.node, s);
        check((s.flags & kSnapOverflow) && s.n == kMaxEntries, "J11.l a list that points back into itself ends at the snapshot's capacity");
    }
    {
        FakeHeap h;
        std::vector<uint16_t> counts(kMaxEntries + 1, 1);
        const FakeNode f = fakeList(h, counts, 1);
        walk(h, f.node, s);
        check((s.flags & kSnapOverflow) && s.n == kMaxEntries, "J11.m more entries than a snapshot holds are an overflow, the first 1024 kept");
    }
    {
        FakeHeap h;
        const FakeNode f = fakeList(h, {10, 20}, 5);
        h.set<uint64_t>(f.node + 0xA8, 0);
        walk(h, f.node, s);
        check(s.n == 0 && s.flags == 0 && s.end == 35 && !checkSnapshot(s), "J11.n an empty list is read as empty (and refused by the snapshot checks)");
    }
    {
        // the offsets are the decompile's and nothing else
        check(kOffNodeList == 0xA8 && kOffNodeEnd == 0xC4 && kOffEntryNext == 0x08 && kOffEntryMesh == 0x38 && kOffEntryDst == 0xA8, "J11.o the offsets are the decompile's: node +A8 and +C4, entry +08, +38 and +A8");
        check(userPointer(0x10000) && !userPointer(0xFFFF) && !userPointer(0x7FFFFFFE0000ull) && !userPointer(0x10001) && userPointer(0x7FFFFFFD0000ull), "J11.p the pointer test: aligned, above 64 KB, below the user ceiling");
    }
}
}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--self-test" || a == "--dry-run") selfTest = true;
        else if (i == 2 && argv[1] == std::string("--self-test")) continue;
        else {
            std::fprintf(stderr, "usage: skin_join_test --self-test [repository root] | --dry-run\n");
            return 2;
        }
    }
    if (!selfTest) {
        std::fprintf(stderr, "usage: skin_join_test --self-test [repository root] | --dry-run\n");
        return 2;
    }
    caseSnapshots();
    caseProtocol();
    caseHistory();
    caseSteady();
    caseInsertRemove();
    caseChildren();
    caseDisagree();
    caseLimits();
    caseResiduals();
    caseLine();
    caseWalk();
    if (g_failures) {
        std::printf("FAIL: skin join: %u of %u checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: %u skin join checks\n", g_checks);
    return 0;
}

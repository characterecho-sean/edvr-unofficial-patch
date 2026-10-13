// mode_watch_test -- the mode watch's decisions (src\d3d11\mode_watch_logic.h): a change waits for its confirming read 250 ms later, a
// half-written file is ignored, an unknown read at a pending confirm retries on a bounded clock and then drops, a flip-flop drops, a live
// rise posts Vr from a flat commit, the toggles post in order, and a file the
// launch or a live rise made stale (a /vr or /novr override, a live rise over a flat file) is not evidence until a write. Built from the
// repo root by build.bat's :rig_mode_watch_test. Windows-free: it includes only the pure headers. --dry-run writes nothing; --self-test
// runs the checks.
#include "../../src/d3d11/mode_watch_logic.h"
#include "../../src/common/install_scope.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace edvr;
using namespace edvr::modewatch;

int g_checks = 0;
int g_fails = 0;

void check(bool ok, const char* name) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}

// A read of a mode, as readGameMode makes it from Settings.xml's text.
ModeRead readOf(GameMode mode) {
    return readGameMode(mode == GameMode::Flat ? "<StereoscopicMode>0</StereoscopicMode>" : "<StereoscopicMode>4</StereoscopicMode>");
}

// A read of an exact Settings.xml value (3 is a VR mode too).
ModeRead readValue(int value) {
    const std::string xml = "<StereoscopicMode>" + std::to_string(value) + "</StereoscopicMode>";
    return readGameMode(xml.c_str());
}

ModeRead unknownRead() { return readGameMode("<Settings></Settings>"); }

bool outcomeIs(const Step& s, Outcome o) { return s.outcome == o; }

// A read that follows a write of Settings.xml (a folder notification or the stat), and a read from a hint or a live rise.
Step afterWrite(Watch& w, uint64_t t, const ModeRead& r) { return w.onRead(t, r, true); }
Step hint(Watch& w, uint64_t t, const ModeRead& r) { return w.onRead(t, r, false); }

void writeThenConfirm() {
    Watch w(GameMode::Flat, GameMode::Flat);
    const Step first = afterWrite(w, 1000, readOf(GameMode::Vr));
    check(outcomeIs(first, Outcome::Pending) && first.mode == GameMode::Vr && w.confirmDueMs() == 1250,
        "flat launch, write 0 -> 4: pending Vr, confirm at +250");
    const Step hintRead = hint(w, 1100, readOf(GameMode::Vr));
    check(outcomeIs(hintRead, Outcome::Same) && w.pending() && w.committed() == GameMode::Flat,
        "flat launch, read at +100 still 4: same, still pending");
    const Step confirm = w.onRead(1250, readOf(GameMode::Vr), false);
    check(outcomeIs(confirm, Outcome::Posted) && confirm.mode == GameMode::Vr && w.committed() == GameMode::Vr && !w.pending(),
        "flat launch, read at +250 still 4: posted Vr");
}

void halfWrittenDuringPending() {
    Watch w(GameMode::Flat, GameMode::Flat);
    afterWrite(w, 0, readOf(GameMode::Vr));
    const Step half = afterWrite(w, 100, unknownRead());
    check(outcomeIs(half, Outcome::Ignored) && !half.staleFile && w.pending() && w.confirmDueMs() == 250,
        "half-written file during pending: ignored, pending and due kept");
    const Step confirm = w.onRead(250, readOf(GameMode::Vr), false);
    check(outcomeIs(confirm, Outcome::Posted) && confirm.mode == GameMode::Vr,
        "half-written file during pending: posted Vr at the confirm");
}

void flipFlop() {
    Watch w(GameMode::Flat, GameMode::Flat);
    afterWrite(w, 0, readOf(GameMode::Vr));
    const Step back = afterWrite(w, 100, readOf(GameMode::Flat));
    check(outcomeIs(back, Outcome::Dropped) && !w.pending() && w.committed() == GameMode::Flat,
        "flip-flop 0 -> 4 -> 0 within 250 ms: dropped, flat stays committed");
    const Step later = afterWrite(w, 300, readOf(GameMode::Flat));
    check(outcomeIs(later, Outcome::Same) && w.committed() == GameMode::Flat,
        "flip-flop: nothing posted after the drop");
}

void confirmDisagrees() {
    Watch w(GameMode::Flat, GameMode::Flat);
    afterWrite(w, 0, readOf(GameMode::Vr));
    const Step confirm = w.onRead(250, readOf(GameMode::Flat), false);
    check(outcomeIs(confirm, Outcome::Dropped) && w.committed() == GameMode::Flat && !w.pending(),
        "confirm read disagrees (flat at +250): dropped");
}

void hintNoChange() {
    Watch vr(GameMode::Vr, GameMode::Vr);
    check(outcomeIs(hint(vr, 0, readOf(GameMode::Vr)), Outcome::Same) && !vr.pending(),
        "hint read with no change from a Vr launch: same");
    Watch flat(GameMode::Flat, GameMode::Flat);
    check(outcomeIs(hint(flat, 0, readOf(GameMode::Flat)), Outcome::Same) && !flat.pending() && flat.confirmDueMs() == 0,
        "hint read with no change from a flat launch: same, nothing due");
    check(!vr.stale() && !flat.stale(), "launches whose file matches the class they run are not stale");
}

void liveRise() {
    Watch flat(GameMode::Flat, GameMode::Flat);
    const Step rise = flat.onLiveRise(0);
    check(outcomeIs(rise, Outcome::Posted) && rise.mode == GameMode::Vr && flat.committed() == GameMode::Vr,
        "live rise from a flat launch: posted Vr");

    Watch vr(GameMode::Vr, GameMode::Vr);
    check(outcomeIs(vr.onLiveRise(0), Outcome::Nothing) && vr.committed() == GameMode::Vr,
        "live rise from a Vr launch: nothing");

    Watch after(GameMode::Vr, GameMode::Vr);
    afterWrite(after, 0, readOf(GameMode::Flat));
    afterWrite(after, 250, readOf(GameMode::Flat));
    check(after.committed() == GameMode::Flat, "live rise after a posted Flat: flat committed first");
    const Step again = after.onLiveRise(300);
    check(outcomeIs(again, Outcome::Posted) && again.mode == GameMode::Vr && after.committed() == GameMode::Vr,
        "live rise after a posted Flat: posted Vr");
}

// A live rise does not cancel a pending file change; the confirm of that change finds the class already posted.
void liveRiseDuringPending() {
    Watch w(GameMode::Flat, GameMode::Flat);
    afterWrite(w, 0, readOf(GameMode::Vr));
    const Step rise = w.onLiveRise(100);
    check(outcomeIs(rise, Outcome::Posted) && rise.mode == GameMode::Vr && w.committed() == GameMode::Vr,
        "live rise while a Vr file change is pending: posted Vr");
    check(w.pending() && w.confirmDueMs() == 250, "live rise while pending: the pending change is kept, due unchanged");
    const Step confirm = w.onRead(250, readOf(GameMode::Vr), false);
    check(outcomeIs(confirm, Outcome::Same) && w.committed() == GameMode::Vr && !w.pending(),
        "live rise during pending: the confirm returns same (already Vr)");
}

// Live rise over a flat file (Epic's "Launch in VR", no switch in the file): the live rise posts Vr and the file still says 0. A hint read
// of that unchanged 0 is ignored (stale); a write that holds Vr clears the staleness with nothing posted; a later write of 0 posts Flat.
void liveRiseOverFlatFile() {
    Watch w(GameMode::Flat, GameMode::Flat);
    const Step rise = w.onLiveRise(0);
    check(outcomeIs(rise, Outcome::Posted) && rise.mode == GameMode::Vr && w.committed() == GameMode::Vr && w.stale(),
        "live rise over a flat file: posted Vr, the file is stale");
    const Step hinted = hint(w, 100, readOf(GameMode::Flat));
    check(outcomeIs(hinted, Outcome::Ignored) && hinted.staleFile && !w.pending() && w.committed() == GameMode::Vr,
        "live rise over a flat file: a hint read of the unchanged 0 is ignored (stale file), nothing posted");
    const Step writtenVr = afterWrite(w, 200, readOf(GameMode::Vr));
    check(outcomeIs(writtenVr, Outcome::Same) && !writtenVr.staleFile && !w.stale() && w.committed() == GameMode::Vr,
        "live rise over a flat file: a write of 4 clears the staleness, same");
    const Step toFlat = afterWrite(w, 300, readOf(GameMode::Flat));
    check(outcomeIs(toFlat, Outcome::Pending) && toFlat.mode == GameMode::Flat, "live rise over a flat file: write 0 pending Flat");
    const Step toFlatConfirm = w.onRead(550, readOf(GameMode::Flat), false);
    check(outcomeIs(toFlatConfirm, Outcome::Posted) && toFlatConfirm.mode == GameMode::Flat && w.committed() == GameMode::Flat,
        "live rise over a flat file: the confirmed write of 0 posts Flat");
}

void threeToggles() {
    Watch w(GameMode::Vr, GameMode::Vr);
    std::vector<GameMode> posted;
    uint64_t t = 0;
    const GameMode reads[] = {GameMode::Flat, GameMode::Vr, GameMode::Flat};
    for (GameMode m : reads) {
        const Step pending = afterWrite(w, t, readOf(m));
        if (!outcomeIs(pending, Outcome::Pending)) break;
        const Step confirm = w.onRead(t + 250, readOf(m), false);
        if (outcomeIs(confirm, Outcome::Posted)) posted.push_back(confirm.mode);
        t += 1000;
    }
    const bool order = posted.size() == 3 && posted[0] == GameMode::Flat && posted[1] == GameMode::Vr && posted[2] == GameMode::Flat;
    check(order, "three toggles off-on-off from a Vr launch: posted Flat, Vr, Flat in order");
}

void unknownLaunch() {
    const ModeRead launch = unknownRead();
    Watch w(launch.mode, launch.mode);
    check(w.committed() == GameMode::Vr, "unknown launch file counts as Vr");
    check(outcomeIs(afterWrite(w, 0, readValue(3)), Outcome::Same) && !w.pending(),
        "unknown launch, then a read of 3: same");
}

// A /vr launch over a file at 0 (EDLaunch /Steam /VR with Settings.xml at 0): the launch runs Vr from the start, so the file's unchanged 0
// is not evidence and posts nothing. The file's own writes are posted in each direction.
void vrLaunchOverFlatFile() {
    Watch w(GameMode::Flat, GameMode::Vr);
    check(w.committed() == GameMode::Vr && !w.pending() && w.stale(), "vr launch over a flat file: runs Vr, the file is stale");
    const Step unchanged = hint(w, 0, readOf(GameMode::Flat));
    check(outcomeIs(unchanged, Outcome::Ignored) && unchanged.staleFile && !w.pending() && w.committed() == GameMode::Vr,
        "vr launch over a flat file: a hint read of the unchanged 0 is ignored (stale file), not posted Flat");
    const Step rise = w.onLiveRise(10);
    check(outcomeIs(rise, Outcome::Nothing) && w.committed() == GameMode::Vr, "vr launch over a flat file: a live rise is nothing");
    const Step toVr = afterWrite(w, 100, readOf(GameMode::Vr));
    check(outcomeIs(toVr, Outcome::Same) && !toVr.staleFile && !w.stale() && w.committed() == GameMode::Vr,
        "vr launch over a flat file: a write of 4 is same (already Vr) and clears the staleness");
    const Step toFlat = afterWrite(w, 400, readOf(GameMode::Flat));
    check(outcomeIs(toFlat, Outcome::Pending) && toFlat.mode == GameMode::Flat, "vr launch over a flat file: write 0 pending Flat");
    const Step toFlatConfirm = w.onRead(650, readOf(GameMode::Flat), false);
    check(outcomeIs(toFlatConfirm, Outcome::Posted) && toFlatConfirm.mode == GameMode::Flat && w.committed() == GameMode::Flat,
        "vr launch over a flat file: write 0 confirmed posts Flat");
}

// The flight D case: /vr over a file at 0, a hint read ignored, a write of 3 (Vr) same, then the 3D-off write of 0 posts Flat.
void vrOverFlatFlightD() {
    Watch w(GameMode::Flat, GameMode::Vr);
    check(outcomeIs(hint(w, 0, readOf(GameMode::Flat)), Outcome::Ignored), "/vr over 0: a hint read of 0 is ignored");
    check(outcomeIs(afterWrite(w, 100, readValue(3)), Outcome::Same) && !w.pending(), "/vr over 0: a write of 3 is same");
    check(outcomeIs(afterWrite(w, 200, readOf(GameMode::Flat)), Outcome::Pending) && w.pending(), "/vr over 0: a write of 0 is pending");
    const Step confirm = w.onRead(450, readOf(GameMode::Flat), false);
    check(outcomeIs(confirm, Outcome::Posted) && confirm.mode == GameMode::Flat && w.committed() == GameMode::Flat,
        "/vr over 0: the confirm posts Flat");
}

// A /vr launch over a file at 0 with no earlier write (flight B's missed 3D-off): the first write of 0 posts Flat.
void vrOverFlatNoWrite() {
    Watch w(GameMode::Flat, GameMode::Vr);
    check(outcomeIs(afterWrite(w, 0, readOf(GameMode::Flat)), Outcome::Pending), "/vr over 0, no earlier write: a write of 0 is pending");
    const Step confirm = w.onRead(250, readOf(GameMode::Flat), false);
    check(outcomeIs(confirm, Outcome::Posted) && confirm.mode == GameMode::Flat && w.committed() == GameMode::Flat,
        "/vr over 0, no earlier write: the confirm posts Flat");
}

// A /novr launch over a file at 4: the launch runs flat, the file is stale, and the first write of 4 posts Vr.
void novrOverVrFile() {
    Watch w(GameMode::Vr, GameMode::Flat);
    check(w.committed() == GameMode::Flat && w.stale(), "/novr over 4: runs flat, the file is stale");
    check(outcomeIs(hint(w, 0, readOf(GameMode::Vr)), Outcome::Ignored), "/novr over 4: a hint read of 4 is ignored");
    check(outcomeIs(afterWrite(w, 100, readOf(GameMode::Vr)), Outcome::Pending), "/novr over 4: a write of 4 is pending");
    const Step confirm = w.onRead(350, readOf(GameMode::Vr), false);
    check(outcomeIs(confirm, Outcome::Posted) && confirm.mode == GameMode::Vr && w.committed() == GameMode::Vr,
        "/novr over 4: the confirm posts Vr");
}

// An unknown read before a pending change's confirm moves nothing. At or after the confirm it is retried on a 100 ms clock, so the loop never
// computes a zero wait. The review's repro (a change at 1000, an unknown read at the 1250 deadline) leaves a wait above zero.
void unknownConfirmRetries() {
    Watch w(GameMode::Flat, GameMode::Flat);
    afterWrite(w, 1000, readOf(GameMode::Vr));
    const Step early = w.onRead(1100, unknownRead(), false);
    check(outcomeIs(early, Outcome::Ignored) && w.pending() && w.confirmDueMs() == 1250 && w.unknownRetries() == 0,
        "unknown before the deadline (1100): ignored, deadline 1250 unchanged, no retry counted");
    const Step due = w.onRead(1251, unknownRead(), false);
    check(outcomeIs(due, Outcome::Ignored) && w.pending() && w.confirmDueMs() == 1351 && w.unknownRetries() == 1,
        "unknown at 1251 (after the 1250 deadline): ignored, pending kept, deadline moved to 1351");

    Watch repro(GameMode::Flat, GameMode::Flat);
    afterWrite(repro, 1000, readOf(GameMode::Vr));
    repro.onRead(1250, unknownRead(), false);
    const uint64_t wait = repro.confirmDueMs() > 1251 ? repro.confirmDueMs() - 1251 : 0;
    check(repro.pending() && wait > 0, "review repro: unknown at the 1250 deadline leaves a next wait above zero (99 ms)");
}

// Unknown, unknown, then a valid read at the moved deadline: the change posts once, and a later read of the same mode posts nothing.
void unknownConfirmRecovery() {
    Watch w(GameMode::Flat, GameMode::Flat);
    afterWrite(w, 1000, readOf(GameMode::Vr));
    w.onRead(1251, unknownRead(), false);
    w.onRead(1351, unknownRead(), false);
    const Step valid = w.onRead(1451, readOf(GameMode::Vr), false);
    check(outcomeIs(valid, Outcome::Posted) && valid.mode == GameMode::Vr && w.committed() == GameMode::Vr && !w.pending() &&
              w.unknownRetries() == 0, "unknown, unknown, then valid at the moved deadline (1451): posted Vr");
    const Step again = w.onRead(1500, readOf(GameMode::Vr), false);
    check(outcomeIs(again, Outcome::Same) && w.committed() == GameMode::Vr, "after the post, a later read of Vr is same: no second post");
}

// The worker reads a pending change when its confirm is due, so this runs the reads the worker makes while the file stays invalid. It
// is the schedule the watch sets, not mode_watch.cpp itself: that loop needs the Windows folder watch and the config, so the rig does not
// drive it. Twenty retries, then the twenty-first read drops the change; after that no read is due and the watch waits for a write.
void unknownConfirmDrops() {
    Watch w(GameMode::Flat, GameMode::Flat);
    afterWrite(w, 1000, readOf(GameMode::Vr));
    unsigned reads = 0, inFirstSecond = 0;
    bool zeroWait = false;
    Step last{Outcome::Nothing, GameMode::Vr};
    while (w.pending() && reads < 100) {
        const uint64_t t = w.confirmDueMs();
        last = w.onRead(t, unknownRead(), false);
        ++reads;
        if (t < 2250) ++inFirstSecond;
        if (outcomeIs(last, Outcome::Ignored) && w.pending() && w.confirmDueMs() <= t) zeroWait = true;
    }
    check(reads == 21 && outcomeIs(last, Outcome::Dropped) && !w.pending() && w.committed() == GameMode::Flat,
        "unknown confirms: 20 retries, the 21st read drops the change, flat stays committed");
    check(!zeroWait, "unknown confirms: no read leaves a wait of zero");
    check(inFirstSecond <= 11, "unknown confirms: at most 11 timed reads in the first second of an invalid file");
    check(w.confirmDueMs() == 0, "unknown confirms: after the drop nothing is due");
    check(outcomeIs(afterWrite(w, 5000, readOf(GameMode::Vr)), Outcome::Pending), "after the drop a later write of Vr is pending again");
}

// An unknown read before the deadline, with a retry already spent, is still ignored and moves nothing.
void unknownBeforeDeadlineAfterRetry() {
    Watch w(GameMode::Flat, GameMode::Flat);
    afterWrite(w, 1000, readOf(GameMode::Vr));
    w.onRead(1251, unknownRead(), false);   // deadline 1351
    const Step early = w.onRead(1300, unknownRead(), false);
    check(outcomeIs(early, Outcome::Ignored) && w.confirmDueMs() == 1351 && w.unknownRetries() == 1,
        "unknown before the moved deadline (1300): ignored, deadline 1351 unchanged");
}

}  // namespace

int main(int argc, char** argv) {
    bool dryRun = false, selfTest = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0) dryRun = true;
        if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
    }
    if (dryRun && !selfTest) {
        std::printf("mode_watch_test: dry run, nothing written\n");
        return 0;
    }
    writeThenConfirm();
    halfWrittenDuringPending();
    flipFlop();
    confirmDisagrees();
    hintNoChange();
    liveRise();
    liveRiseDuringPending();
    liveRiseOverFlatFile();
    threeToggles();
    unknownLaunch();
    vrLaunchOverFlatFile();
    vrOverFlatFlightD();
    vrOverFlatNoWrite();
    novrOverVrFile();
    unknownConfirmRetries();
    unknownConfirmRecovery();
    unknownConfirmDrops();
    unknownBeforeDeadlineAfterRetry();
    if (g_fails) {
        std::printf("FAIL: mode_watch_test %d of %d checks failed\n", g_fails, g_checks);
        return 1;
    }
    std::printf("PASS: mode_watch_test %d checks (confirm at 250 ms, half-written reads ignored, flip-flop dropped, live rise, toggles in "
                "order, unknown launch, stale file after a /vr or /novr launch or a live rise, until a write)\n", g_checks);
    return 0;
}

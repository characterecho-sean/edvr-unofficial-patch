// menu_edit_hold_test: held numeric edits in the F8 menu are coalesced before the settings file is written and re-read (src/d3d11/menu_edit_hold.h), and the config refresh
// window line (src/d3d11/config_refresh_line.h) says what it counted. Pure.
//
//   --dry-run    the same run (the rig never writes a file), for the gate's --dry-run convention; with no repository root the cases that read a source (H11, H13.p) are skipped
//   --self-test  every check; the optional argument is the repository root, where the glue is read as text: src/d3d11/menu.cpp and src/d3d11/perf_monitor.cpp (the
//                mutation tool hands it a temp root holding an edited copy)
//
// F16 priced a five-second hold on a numeric row at 100 refreshes of the config on the frame thread (93.4 ms in all, 1.17 ms at the worst, against an 11.1 ms frame).
// Sean chose (2026-10-09) to write and reload every 250 ms while a number is held, and once on release: about four a second, a preview that moves in quarter-second steps.
// The rig drives the coalescer the way the menu tick does -- a frame clock, the tracker's edge-and-repeat (400 ms, then every 83 ms), the key state of the last poll --
// and holds it to: a tap writes once, a long hold writes about every 250 ms and none of its steps waits longer than that, a row or page switch writes first, a close,
// a shutdown and any other change flush, nothing is written twice or lost, and a failed write puts the row back. Cases ("H<case>.<what>"; mutants.py names the case
// that must catch each mutation):
//   H1  a tap writes once, on release
//   H2  a long hold writes about every 250 ms (about 20 writes in five seconds, not 100, not 3); every step is written within the bound; the last write is the value shown
//   H3  a key held with no step coming (a row at its limit) is written by the bound, once, and not again while it stays down
//   H4  a step of another row, a highlight on another row: the first row is written first, in order
//   H5  a page switch writes
//   H6  a close and a shutdown write what is held, and nothing when nothing is
//   H7  any other change goes behind the held one
//   H8  nothing is written twice
//   H9  a failed write rolls the row back to what the file holds, and the next burst starts from there
//   H10 the burst is one change from its first step's old value to its last step's
//   H11 the glue (menu.cpp) is wired: held steps, flushes, the reload that must not put a held value back, the worker that writes what was queued before a quit (read as text)
//   H12 the refresh budget: a five-second hold is 12 to 22 writes at any frame rate, a third of the old cost or less
//   H13 the config refresh window line: counts as counted, the closing sentence only for a window with nothing in it; the glue prints it through the formatter (read as text)
//   H14 a write that fails while a NEWER edit of the row exists (the 0.19.0 release review, "existing limitation"): the row keeps showing the newer value, the next repeat steps from
//       it (the review's case ends at 6, not 1), the newer edit's own failure still rolls the row back, an older job never does, and the edit counters are per row
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "config_refresh_line.h"
#include "menu_edit_hold.h"
#include "menu_keys.h"

using namespace edvr;

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
bool has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }
size_t occurrences(const std::string& text, const char* needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}
bool readText(const std::string& path, std::string* out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char chunk[4096];
    for (size_t got; (got = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) out->append(chunk, got);
    std::fclose(f);
    return true;
}
const char* labelf(const char* fmt, unsigned a, unsigned b = 0) {
    static char buf[200];
    std::snprintf(buf, sizeof(buf), fmt, a, b);
    return buf;
}

// menu_keys.cpp's tracker (keyRepeatStep, act = true), copied: that file links the hotkey registry.
int trackerStep(KeyRepeat& k, bool down, uint64_t now) {
    if (!down) {
        k.down = false;
        k.nextMs = 0;
        return 0;
    }
    if (!k.down) {
        k.down = true;
        k.nextMs = now + kKeyRepeatFirstMs;
        return 1;
    }
    if (now >= k.nextMs) {
        k.nextMs = now + kKeyRepeatMs;
        return 1;
    }
    return 0;
}

// The job the menu writes, reduced to what the coalescer and the log care about.
struct Job {
    int def = -1;
    int value = 0;     // the last step's value
    int before = 0;    // what the file held before the burst's first step
    unsigned step = 0; // the sequence number of the step that made it
    uint32_t gen = 0;  // the row's edit number when the step was made (EditGeneration)
};
struct Write {
    Job job;
    EditFlush why;
    uint64_t at;
};
void mergeJob(Job& pending, const Job& latest) {
    const int before = pending.before;
    pending = latest;
    pending.before = before;
}

// One row, one key, and the menu tick's order: the coalescer's tick first (from the last poll), then the poll, then a step -> hold.
struct Sim {
    std::vector<Write> writes;
    std::vector<uint64_t> stepAt;   // when each step was made
    uint64_t now = 1000;
    KeyRepeat key;
    int def = 7, page = 0;
    int highlight = 7;       // the row highlighted
    int shown = 100;         // what the row shows: every step at once
    int file = 100;          // what the file holds: what the writes landed
    unsigned steps = 0;
    bool limited = false;    // the row is at its limit: the key is down and no step is made
    EditGeneration rowEdits;   // menu.cpp's RowState::edits for this row
    EditCoalescer<Job> held{&Sim::sink, this, &mergeJob};
    static void sink(void* ctx, const Job& job, EditFlush why) {
        Sim* s = static_cast<Sim*>(ctx);
        s->writes.push_back({job, why, s->now});
    }
    void step() {
        ++steps;
        stepAt.push_back(now);
        Job j;
        j.def = def;
        j.value = ++shown;
        j.before = shown - 1;
        j.step = steps;
        j.gen = rowEdits.next();   // menu.cpp's enqueueChange
        held.hold(j, def, page, now);
    }
    // menu.cpp's drainWrites on a write that failed: the row goes back to what the file holds unless the row has been edited since.
    void writeFailed(const Write& w) {
        if (rowEdits.rollbackDue(w.job.gen)) shown = file;
    }
    void frame(uint32_t dt, bool keyDown) {
        now += dt;
        held.tick(now, key.down, highlight, page);
        const int n = trackerStep(key, keyDown, now);
        if (n && !limited) step();
    }
    void run(uint32_t ms, uint32_t dt, bool keyDown) {
        for (uint32_t t = 0; t < ms; t += dt) frame(dt, keyDown);
    }
};

// ---- H1 ---------------------------------------------------------------------------------------------------------------------------
void caseTap() {
    for (uint32_t dt : {8u, 11u, 16u, 33u}) {
        Sim s;
        s.run(dt, dt, true);          // the press frame: the first step
        s.run(60, dt, true);          // held a moment
        check(s.writes.empty(), labelf("H1.a a tap writes nothing while the key is down (frame %u ms)", dt));
        const uint64_t released = s.now + dt;
        s.run(4 * dt, dt, false);
        check(s.writes.size() == 1 && s.writes[0].why == EditFlush::Release && s.writes[0].job.value == s.shown && s.writes[0].job.before == 100 &&
                  s.writes[0].at <= released + dt,
              labelf("H1.b ...and writes once, the frame after the key comes up, with the value shown (frame %u ms)", dt));
        s.run(2000, dt, false);
        check(s.writes.size() == 1, labelf("H1.c ...and nothing more (frame %u ms)", dt));
    }
    {
        Sim s;
        s.frame(16, true);    // press and release inside one frame: the key is up by the next poll
        s.frame(16, false);
        s.frame(16, false);
        check(s.writes.size() == 1 && s.writes[0].why == EditFlush::Release, "H1.d a tap shorter than a frame is written once on the frame after");
    }
}

// ---- H2 ---------------------------------------------------------------------------------------------------------------------------
void caseLongHold() {
    check(kEditMaxWaitMs == 250, "H2.z the constant: a burst waits 250 ms for its write at the most (Sean, 2026-10-09; the first choice was 2000)");
    for (uint32_t dt : {8u, 11u, 16u, 33u}) {
        Sim s;
        s.run(5000, dt, true);
        const uint64_t releaseAt = s.now;
        s.run(4 * dt, dt, false);
        check(s.steps >= 40, labelf("H2.pre a five-second hold makes the steps (%u at %u ms frames)", s.steps, dt));
        check(s.writes.size() >= 12 && s.writes.size() <= 22,
              labelf("H2.a a five-second hold writes about every 250 ms, once more on release: %u writes (a write per step would be 50 or more, one burst for the hold 1 to 3; frame %u ms)", unsigned(s.writes.size()), dt));
        bool bounded = true;
        for (size_t i = 0; i < s.stepAt.size(); ++i) {
            // the first write that carries step i + 1 (a write carries every step up to its job's)
            uint64_t written = UINT64_MAX;
            for (const Write& w : s.writes)
                if (w.job.step >= i + 1) { written = w.at; break; }
            bounded = bounded && written != UINT64_MAX && written - s.stepAt[i] <= 250 + 2 * dt;
        }
        check(bounded, labelf("H2.b ...no step waits more than the bound for its write (%u ms), so the live effect of a held number moves in quarter-second steps (frame %u ms)", 250u, dt));
        check(!s.writes.empty() && s.writes.front().why == EditFlush::MaxWait && s.writes.front().at - s.stepAt.front() >= 250 - dt,
              labelf("H2.c ...the first of them is the bound, not an early write (frame %u ms)", dt));
        check(!s.writes.empty() && s.writes.back().job.value == s.shown && s.writes.back().at <= releaseAt + 2 * dt,
              labelf("H2.d ...the last carries the value shown, no later than a frame after the key comes up (frame %u ms)", dt));
        unsigned last = 0;
        bool ordered = true;
        for (const Write& w : s.writes) {
            ordered = ordered && w.job.step > last && w.job.value == 100 + int(w.job.step);
            last = w.job.step;
        }
        check(ordered, labelf("H2.e ...each write carries a later step than the one before it (frame %u ms)", dt));
    }
}

// ---- H3 ---------------------------------------------------------------------------------------------------------------------------
// There is no still time: a held edit is written within the bound of its first step whether or not steps keep coming, so a key held with no step coming (a row at its
// limit) is written promptly by the bound, and a pause long enough to be told from the 83 ms repeat would be longer than the bound anyway.
void caseNoStepsComing() {
    for (uint32_t dt : {8u, 16u, 33u}) {
        Sim s;
        s.frame(dt, true);       // the press: the one step
        s.limited = true;        // the row is at its limit: the key stays down, no step comes
        s.run(1000, dt, true);
        check(s.writes.size() == 1 && s.writes[0].why == EditFlush::MaxWait && s.writes[0].at - s.stepAt[0] >= 250 && s.writes[0].at - s.stepAt[0] <= 250 + 2 * dt,
              labelf("H3.a a key held with no step coming is written by the bound, 250 ms after the step, and once while the key stays down (frame %u ms)", dt));
    }
}
// ---- H4 ---------------------------------------------------------------------------------------------------------------------------
void caseRowSwitch() {
    {
        Sim s;
        s.run(160, 16, true);                 // the step on row 7 (the first at the press), before the bound
        const int shownA = s.shown;
        s.def = 9;
        s.highlight = 9;
        s.shown = 500;
        s.step();                              // a step of ANOTHER row while the first is held: the first is written first
        check(s.writes.size() == 1 && s.writes[0].job.def == 7 && s.writes[0].job.value == shownA && s.writes[0].why == EditFlush::RowSwitch,
              "H4.a a step of another row writes the held row first, with its own value");
        s.run(100, 16, false);
        check(s.writes.size() == 2 && s.writes[1].job.def == 9 && s.writes[1].job.value == 501 && s.writes[1].job.before == 500,
              "H4.b ...and the second row's edit is its own (its old value, its value), written in turn on release");
    }
    {
        Sim s;
        s.run(160, 16, true);
        s.highlight = 3;                       // the highlight moved off the row with the key still down
        s.frame(16, true);
        check(s.writes.size() == 1 && s.writes[0].why == EditFlush::RowSwitch && s.writes[0].job.def == 7, "H4.c a highlight on another row writes the held edit on that tick, key down or not");
    }
}

// ---- H5 ---------------------------------------------------------------------------------------------------------------------------
void casePageSwitch() {
    Sim s;
    s.run(160, 16, true);
    s.page = 2;
    s.frame(16, true);
    check(s.writes.size() == 1 && s.writes[0].why == EditFlush::PageSwitch, "H5.a another page shown writes the held edit on that tick");
    s.frame(16, true);
    check(s.writes.size() == 1, "H5.b ...once");
}

// ---- H6 ---------------------------------------------------------------------------------------------------------------------------
void caseCloseShutdown() {
    {
        Sim s;
        s.run(160, 16, true);
        const bool wrote = s.held.flush(EditFlush::Close);
        check(wrote && s.writes.size() == 1 && s.writes[0].why == EditFlush::Close && s.writes[0].job.value == s.shown && !s.held.pending(), "H6.a a close writes what is held, with the value shown");
        check(!s.held.flush(EditFlush::Close) && s.writes.size() == 1, "H6.b ...and a close with nothing held writes nothing");
    }
    {
        Sim s;
        s.step();
        s.held.flush(EditFlush::Shutdown);
        check(s.writes.size() == 1 && s.writes[0].why == EditFlush::Shutdown && s.writes[0].job.value == s.shown, "H6.c a shutdown writes what is held");
    }
}

// ---- H7 ---------------------------------------------------------------------------------------------------------------------------
void caseOther() {
    Sim s;
    s.step();
    s.step();
    // menu.cpp's enqueueChange for a change that is not a held step: flush(Other), then queue its own.
    s.held.flush(EditFlush::Other);
    s.writes.push_back({Job{3, 1, 0, 0}, EditFlush::None, s.now});
    check(s.writes.size() == 2 && s.writes[0].job.def == 7 && s.writes[0].why == EditFlush::Other && s.writes[1].job.def == 3, "H7.a another change goes behind the held edit: the order the player made them in");
}

// ---- H8 ---------------------------------------------------------------------------------------------------------------------------
void caseNoDouble() {
    Sim s;
    s.run(160, 16, true);
    s.held.flush(EditFlush::Close);
    s.run(500, 16, false);
    s.held.flush(EditFlush::Shutdown);
    check(s.writes.size() == 1, "H8.a a flush, then the ticks after it, then another flush: written once");
    check(!s.held.pending() && s.held.flushes() == 1, "H8.b ...and the coalescer is empty");
}

// ---- H9 ---------------------------------------------------------------------------------------------------------------------------
// menu.cpp's drainWrites on a failed write: the row is put back to what the file holds (rowValue), and menuNoteConfigReloaded leaves a row with a held edit alone.
void caseRollback() {
    Sim s;
    s.run(700, 16, true);    // several steps and several bounds
    s.run(100, 16, false);
    check(s.writes.size() >= 2, "H9.pre a hold of 700 ms was written more than once");
    const Write failed = s.writes.back();
    // Every write but the last landed; the last fails: the file keeps what the one before it wrote, and the row goes back to that.
    s.file = s.writes[s.writes.size() - 2].job.value;
    s.shown = s.file;
    check(s.shown < failed.job.value && !s.held.pending(), "H9.a the failed write puts the row back to what the file holds, and nothing is held");
    s.key = KeyRepeat();
    const size_t before = s.writes.size();
    s.frame(16, true);
    check(s.shown == s.file + 1 && s.writes.size() == before, "H9.b the next step starts from the file's value, not from the failed burst's");
    s.frame(16, false);
    s.frame(16, false);
    check(s.writes.size() == before + 1 && s.writes.back().job.before == s.file && s.writes.back().job.value == s.file + 1, "H9.c ...and is written as a burst of its own");
}

// ---- H14 --------------------------------------------------------------------------------------------------------------------------
// menu.cpp's drainWrites on a write that failed while a NEWER edit of the row exists. The review's scratch case is the first block: the file holds 0, a held key steps every 83 ms
// from 1000, the 250 ms bound writes A = 4 at 1250, B = 5 is made at 1332 and held, and A's write fails. The row went to 0 and the next repeat stepped to 1: the burst's last write
// was 1, not 6.
void caseFailedWhileNewer() {
    {
        Sim s;
        s.shown = s.file = 0;
        for (uint64_t at : {1000u, 1083u, 1166u, 1249u}) { s.now = at; s.step(); }
        s.now = 1250;
        s.held.tick(s.now, true, s.highlight, s.page);
        check(s.writes.size() == 1 && s.writes[0].job.value == 4 && s.writes[0].why == EditFlush::MaxWait, "H14.pre the bound writes A = 4 at 1250 ms");
        s.now = 1332;
        s.step();
        check(s.shown == 5 && s.held.pending(), "H14.pre2 B = 5 is shown and held");
        s.now = 1350;
        s.held.tick(s.now, true, s.highlight, s.page);
        s.writeFailed(s.writes[0]);   // the worker reports A failed
        check(s.shown == 5, "H14.a A's write fails while the newer B is held: the row keeps showing 5, not the file's 0");
        s.now = 1415;
        s.step();
        check(s.shown == 6, "H14.b ...so the next repeat steps from there: 6, not 1");
        s.now = 1498;
        s.held.tick(s.now, false, s.highlight, s.page);   // the key comes up
        check(s.writes.size() == 2 && s.writes[1].job.value == 6 && s.writes[1].job.before == 4 && s.writes[1].why == EditFlush::Release,
              "H14.c ...and the write on release carries 6 (one burst, 4 to 6)");
    }
    {
        // the newer edit is already queued behind the failed one (written on release, not yet reported): the failed one still leaves the row alone, and the newer one's failure rolls it back
        Sim s;
        s.shown = s.file = 0;
        for (uint64_t at : {1000u, 1083u, 1166u, 1249u}) { s.now = at; s.step(); }
        s.now = 1250;
        s.held.tick(s.now, true, s.highlight, s.page);
        s.now = 1332;
        s.step();
        s.now = 1400;
        s.held.tick(s.now, false, s.highlight, s.page);
        check(s.writes.size() == 2 && !s.held.pending() && s.shown == 5, "H14.pre3 A = 4 and B = 5 are both queued");
        s.writeFailed(s.writes[0]);
        check(s.shown == 5, "H14.d the older of two queued writes fails: the row keeps showing 5");
        s.writeFailed(s.writes[1]);
        check(s.shown == 0, "H14.e ...and the newer one's failure, the row's latest edit, still puts the row back to what the file holds");
    }
    {
        Sim s;
        s.shown = s.file = 0;
        s.now = 1000; s.step();
        s.now = 1083; s.step();
        s.now = 1100;
        s.held.tick(s.now, false, s.highlight, s.page);
        check(s.writes.size() == 1 && s.writes[0].job.value == 2 && !s.held.pending(), "H14.pre4 a burst of two is written on release");
        s.writeFailed(s.writes[0]);
        check(s.shown == 0, "H14.f a failed write that was the row's latest edit puts the row back, as before (H9)");
    }
    {
        // the counters: strictly increasing, an older job is never the latest, and one row's edits do not count against another's
        EditGeneration a, b;
        const uint32_t a1 = a.next();
        const uint32_t a2 = a.next();
        const uint32_t b1 = b.next();
        check(a1 != a2 && !a.rollbackDue(a1) && a.rollbackDue(a2) && b.rollbackDue(b1), "H14.g an edit counter is per row: an older job never rolls back, the latest does, another row's edit counts for nothing");
        b.next();
        b.next();
        check(a.rollbackDue(a2), "H14.h ...and edits of other rows after it change nothing");
    }
}

// ---- H10 --------------------------------------------------------------------------------------------------------------------------
void caseMerge() {
    Sim s;
    s.run(1000, 16, true);
    s.run(100, 16, false);
    bool chained = !s.writes.empty() && s.writes[0].job.before == 100 && s.writes.back().job.value == s.shown;
    bool multi = false;
    for (size_t i = 0; i < s.writes.size(); ++i) {
        if (i) chained = chained && s.writes[i].job.before == s.writes[i - 1].job.value;
        multi = multi || s.writes[i].job.value - s.writes[i].job.before >= 2;
    }
    check(chained && multi, "H10.a a burst is one change: each write goes from the value before its first step to the value of its last, the next from there (the log and the Status page say A -> B once per write)");
}
// ---- H12 --------------------------------------------------------------------------------------------------------------------------
void caseBudget() {
    for (uint32_t dt : {6u, 11u, 16u, 25u, 33u}) {
        Sim s;
        s.run(5000, dt, true);
        s.run(3 * dt, dt, false);
        check(s.steps >= 40 && s.writes.size() >= 12 && s.writes.size() <= 22,
              labelf("H12.a five seconds of hold: %u writes, one refresh each -- the F16 hold was 100 (frame %u ms)", unsigned(s.writes.size()), dt));
    }
}
// ---- H13 --------------------------------------------------------------------------------------------------------------------------
void caseLine() {
    char buf[400];
    formatConfigRefreshWindow(buf, sizeof(buf), 32400, 100, 93.4, 1.17, 100);
    check(has(buf, "ending 32400;") && has(buf, "100 refreshes on the frame thread") && has(buf, "93.4 ms in all") && has(buf, "0.93 ms mean") && has(buf, "1.17 ms max") &&
              has(buf, "100 menu edits queued"),
          "H13.a the line reports the counts as counted (the F16 window: 100 refreshes, 93.4 ms in all, 0.93 mean, 1.17 max, 100 edits)");
    check(!has(buf, "No refresh") && !has(buf, "All zero") && !has(buf, "no refresh"), "H13.b ...and does not say that nothing happened (F16's line said so after those counts)");
    formatConfigRefreshWindow(buf, sizeof(buf), 3600, 0, 0.0, 0.0, 0);
    check(has(buf, "0 refreshes") && has(buf, "0 menu edits queued") && has(buf, "No refresh and no menu edit in this window."), "H13.c a window with nothing in it says so, so it is not mistaken for a build without the line");
    formatConfigRefreshWindow(buf, sizeof(buf), 3600, 0, 0.0, 0.0, 12);
    check(!has(buf, "No refresh") && has(buf, "12 menu edits queued"), "H13.d ...but not a window with menu edits and no refresh yet (a hold still in progress)");
    formatConfigRefreshWindow(buf, sizeof(buf), 3600, 3, 4.5, 2.0, 0);
    check(!has(buf, "No refresh") && has(buf, "3 refreshes") && has(buf, "1.50 ms mean"), "H13.e ...nor a window with a refresh by hand and no menu edit");
}

// ---- H11 and H13.p: the glue, read as text ------------------------------------------------------------------------------------------
void caseGlue(const std::string& root) {
    if (root.empty()) return;
    std::string menu, perf;
    const bool readMenu = readText(root + "/src/d3d11/menu.cpp", &menu);
    const bool readPerf = readText(root + "/src/d3d11/perf_monitor.cpp", &perf);
    check(readMenu && readPerf, "H11.p0 src/d3d11/menu.cpp and perf_monitor.cpp can be read under the repository root");
    if (!readMenu || !readPerf) return;
    check(occurrences(menu, "applyChange(defIndex, formatNumber(v, d.precision), true);") == 1, "H11.p1 a stepped Number row holds its step");
    check(occurrences(menu, "applyResolutionChange(defIndex, v, next, true);") == 1, "H11.p2 ...and so does the per-headset render width row");
    check(occurrences(menu, "if (!held) g_held.flush(EditFlush::Other);") == 1, "H11.p3 any change that is not a held step writes what is held first");
    check(occurrences(menu, "if (held) g_held.hold(job, job.def, s.page, s.tickMs);\n    else enqueueWrite(job);") == 1, "H11.p4 a held step goes to the coalescer, every other change to the writer at once");
    check(occurrences(menu, "tickHeldEdit(now);") == 2, "H11.p5 the held edit's tick runs in both menu ticks (flat and VR)");
    check(occurrences(menu, "g_held.tick(now, stepKeyHeld(), highlightedSettingDef(), g_s.page);") == 1, "H11.p6 ...from the key state of the last poll, the highlighted row and the page");
    check(occurrences(menu, "g_held.flush(EditFlush::Close);") == 1, "H11.p7 closing the menu writes what is held");
    check(occurrences(menu, "g_held.flush(EditFlush::Shutdown);   // before the writer stops, which writes what was queued before it quit\n    stopWriter();") == 1,
          "H11.p8 a shutdown writes what is held, before the writer stops");
    check(occurrences(menu, "if (g_writer.queue.empty()) return;") == 1 && !has(menu, "if (g_writer.quit) return;"), "H11.p9 the writer, told to quit, still writes what was queued before");
    check(occurrences(menu, "if (g_held.pending() && g_held.def() == i) continue;") == 1, "H11.p10 a reload of an earlier write does not put a held row back to the file's value");
    check(occurrences(menu, "g_rows[w.job.def].value = rowValue(d);") == 1, "H11.p11 a failed write still puts the row back to what the file holds");
    check(occurrences(menu, "job.gen = g_rows[job.def].edits.next();") == 1 && occurrences(menu, "EditGeneration edits;") == 1 && occurrences(menu, "uint32_t    gen = 0;") == 1,
          "H11.p13 every edit takes the row's next edit number, and the job carries it");
    check(occurrences(menu, "if (g_rows[w.job.def].edits.rollbackDue(w.job.gen)) {\n                g_rows[w.job.def].value = rowValue(d);") == 1,
          "H11.p14 ...and a failed write puts the row back only if no newer edit of the row has been made since");
    check(occurrences(menu, "    pending.before = before;\n    pending.fromValue = fromValue;\n    pending.dropped = dropped;") == 1, "H11.p12 a burst keeps the first step's old value for the log");
    check(occurrences(perf, "formatConfigRefreshWindow(refreshLine, sizeof(refreshLine), s.frameNo, reloads, reloadMs, reloadMaxMs, edits);") == 1 && !has(perf, "All zero"),
          "H13.p1 the monitor prints the config refresh line through the formatter, with no sentence of its own");
}
}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false;
    std::string root;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--self-test" || a == "--dry-run") selfTest = true;
        else if (i == 2 && argv[1] == std::string("--self-test")) root = a;
        else {
            std::fprintf(stderr, "usage: menu_edit_hold_test --self-test [repository root] | --dry-run\n");
            return 2;
        }
    }
    if (!selfTest) {
        std::fprintf(stderr, "usage: menu_edit_hold_test --self-test [repository root] | --dry-run\n");
        return 2;
    }
    caseTap();
    caseLongHold();
    caseNoStepsComing();
    caseRowSwitch();
    casePageSwitch();
    caseCloseShutdown();
    caseOther();
    caseNoDouble();
    caseRollback();
    caseFailedWhileNewer();
    caseMerge();
    caseBudget();
    caseLine();
    caseGlue(root);
    if (g_failures) {
        std::printf("FAIL: menu edit hold: %u of %u checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: %u menu edit hold checks\n", g_checks);
    return 0;
}

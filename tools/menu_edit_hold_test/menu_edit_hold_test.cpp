// menu_edit_hold_test: held numeric edits in the F8 menu are coalesced before the settings file is written and re-read (src/d3d11/menu_edit_hold.h), and the config refresh
// window line (src/d3d11/config_refresh_line.h) says what it counted. Pure.
//
//   --dry-run    the same run (the rig never writes a file), for the gate's --dry-run convention; with no repository root the cases that read a source (H11, H13.p) are skipped
//   --self-test  every check; the optional argument is the repository root, where the glue is read as text: src/d3d11/menu.cpp and src/d3d11/perf_monitor.cpp (the
//                mutation tool hands it a temp root holding an edited copy)
//
// F16 priced a five-second hold on a numeric row at 100 refreshes of the config on the frame thread (93.4 ms in all, 1.17 ms at the worst, against an 11.1 ms frame).
// The rig drives the coalescer the way the menu tick does -- a frame clock, the tracker's edge-and-repeat (400 ms, then every 83 ms), the key state of the last poll --
// and holds it to: a tap writes once, a long hold writes a bounded few and none of its steps waits longer than the bound, a row or page switch writes first, a close,
// a shutdown and any other change flush, nothing is written twice or lost, and a failed write puts the row back. Cases ("H<case>.<what>"; mutants.py names the case
// that must catch each mutation):
//   H1  a tap writes once, on release
//   H2  a long hold writes a bounded few; every step is written within the bound; the last write is the value shown
//   H3  a key held with no step coming writes after the still time, not before the first repeat
//   H4  a step of another row, a highlight on another row: the first row is written first, in order
//   H5  a page switch writes
//   H6  a close and a shutdown write what is held, and nothing when nothing is
//   H7  any other change goes behind the held one
//   H8  nothing is written twice
//   H9  a failed write rolls the row back to what the file holds, and the next burst starts from there
//   H10 the burst is one change from its first step's old value to its last step's
//   H11 the glue (menu.cpp) is wired: held steps, flushes, the reload that must not put a held value back, the worker that writes what was queued before a quit (read as text)
//   H12 the refresh budget: a five-second hold is at most three writes at any frame rate
//   H13 the config refresh window line: counts as counted, the closing sentence only for a window with nothing in it; the glue prints it through the formatter (read as text)
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
        held.hold(j, def, page, now);
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
    check(kEditStillMs == 500 && kEditMaxWaitMs == 2000, "H2.z the constants: a pause is 500 ms (longer than the 400 ms before the first repeat), a burst waits 2000 ms for its write at the most");
    for (uint32_t dt : {8u, 11u, 16u, 33u}) {
        Sim s;
        s.run(5000, dt, true);
        const uint64_t releaseAt = s.now;
        s.run(4 * dt, dt, false);
        check(s.steps >= 40, labelf("H2.pre a five-second hold makes the steps (%u at %u ms frames)", s.steps, dt));
        check(s.writes.size() >= 2 && s.writes.size() <= 3, labelf("H2.a a five-second hold writes two or three times, not once a step (%u writes, frame %u ms)", unsigned(s.writes.size()), dt));
        bool bounded = true;
        for (size_t i = 0; i < s.stepAt.size(); ++i) {
            // the first write that carries step i + 1 (a write carries every step up to its job's)
            uint64_t written = UINT64_MAX;
            for (const Write& w : s.writes)
                if (w.job.step >= i + 1) { written = w.at; break; }
            bounded = bounded && written != UINT64_MAX && written - s.stepAt[i] <= 2000 + 2 * dt;
        }
        check(bounded, labelf("H2.b ...no step waits more than the bound for its write (%u ms), so the live effect of a long hold shows up about every two seconds (frame %u ms)", 2000u, dt));
        check(!s.writes.empty() && s.writes.front().why == EditFlush::MaxWait && s.writes.front().at - s.stepAt.front() >= 2000 - dt,
              labelf("H2.c ...the first of them is the bound, not an early write (frame %u ms)", dt));
        check(!s.writes.empty() && s.writes.back().why == EditFlush::Release && s.writes.back().job.value == s.shown && s.writes.back().at <= releaseAt + 2 * dt,
              labelf("H2.d ...the last is the release, with the value shown (frame %u ms)", dt));
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
void caseStill() {
    for (uint32_t dt : {8u, 16u, 33u}) {
        Sim s;
        s.run(1000, dt, true);   // the first repeat comes at 400 ms: that gap is no pause
        check(s.writes.empty(), labelf("H3.a the 400 ms before the first repeat is not a pause: nothing written in the first second (frame %u ms)", dt));
        s.limited = true;        // the row reached its limit: the key stays down, no step comes
        const uint64_t lastStep = s.stepAt.back();
        s.run(1200, dt, true);
        check(s.writes.size() == 1 && s.writes[0].why == EditFlush::Still && s.writes[0].at - lastStep >= 500 && s.writes[0].at - lastStep <= 500 + 2 * dt,
              labelf("H3.b a key held with no step for the still time writes then (frame %u ms)", dt));
    }
}

// ---- H4 ---------------------------------------------------------------------------------------------------------------------------
void caseRowSwitch() {
    {
        Sim s;
        s.run(300, 16, true);                 // steps on row 7 (the first at the press)
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
        s.run(300, 16, true);
        s.highlight = 3;                       // the highlight moved off the row with the key still down
        s.frame(16, true);
        check(s.writes.size() == 1 && s.writes[0].why == EditFlush::RowSwitch && s.writes[0].job.def == 7, "H4.c a highlight on another row writes the held edit on that tick, key down or not");
    }
}

// ---- H5 ---------------------------------------------------------------------------------------------------------------------------
void casePageSwitch() {
    Sim s;
    s.run(300, 16, true);
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
        s.run(200, 16, true);
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
    s.run(300, 16, true);
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
    s.run(1000, 16, true);
    s.run(100, 16, false);
    check(s.writes.size() == 1, "H9.pre a burst of steps was written once");
    const Write failed = s.writes[0];
    // The write fails: the file keeps `file`, the row goes back to it.
    s.shown = s.file;
    check(s.shown == 100 && !s.held.pending(), "H9.a the failed write puts the row back to what the file holds, and nothing is held");
    s.run(0, 16, false);
    s.key = KeyRepeat();
    s.frame(16, true);
    check(s.shown == 101 && s.writes.size() == 1, "H9.b the next step starts from the file's value (101, not past the failed burst's)");
    s.frame(16, false);
    s.frame(16, false);
    check(s.writes.size() == 2 && s.writes[1].job.before == 100 && s.writes[1].job.value == 101 && failed.job.value > 101, "H9.c ...and is written as a burst of its own (100 -> 101)");
}

// ---- H10 --------------------------------------------------------------------------------------------------------------------------
void caseMerge() {
    Sim s;
    s.run(1000, 16, true);
    s.run(100, 16, false);
    check(s.writes.size() == 1 && s.writes[0].job.before == 100 && s.writes[0].job.value == s.shown && s.writes[0].job.value > 105,
          "H10.a a burst is one change: from the value before its first step to the value of its last (the log and the Status page say 100 -> N once)");
}

// ---- H12 --------------------------------------------------------------------------------------------------------------------------
void caseBudget() {
    for (uint32_t dt : {6u, 11u, 16u, 25u, 33u}) {
        Sim s;
        s.run(5000, dt, true);
        s.run(3 * dt, dt, false);
        check(s.steps >= 40 && s.writes.size() <= 3, labelf("H12.a five seconds of hold: %u steps, at most three writes (one refresh each) -- the F16 hold was 100 (frame %u ms)", s.steps, dt));
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
    caseStill();
    caseRowSwitch();
    casePageSwitch();
    caseCloseShutdown();
    caseOther();
    caseNoDouble();
    caseRollback();
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

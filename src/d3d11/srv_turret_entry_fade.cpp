// The SRV turret's entry fade (issue 140): the module. srv_turret_entry_fade.h says what it is and holds the state machine and the log lines (tools/srv_turret_entry_fade_test runs them);
// this file feeds the machine, keeps the luma window the instrument reads, and writes the lines.
//
// THE INSTRUMENT. While the commander is in the Scorpion in VR the luma probe's frame-by-frame mode (luma_probe.h lumaProbeBurst) samples the game's eye image every pass; the module
// pairs the two eyes' readings into a rolling window. The window is kept while in the SRV and not only around an edge because the edge cannot be known ahead: Status.json says the
// turret view is on up to a few hundred ms after the game switched, and the point of the trace is to show the game's own fade, which is before the edge. The price is sixteen small
// row copies an eye a frame, in the Scorpion only; outside it the mode is off and the probe is the two-second rounds it always was. A run's report is written kReportAfterMs after the
// run ends (the readbacks land one to three frames late), so a log without the lines after an "on" edge is a build without this module, and a log with the skipped line is a hold that
// had nothing to wait for.
#include "srv_turret_entry_fade.h"

#include "journal_watch.h"
#include "luma_probe.h"
#include "srv_turret.h"

#include "../common/log.h"
#include "../common/native_frame.h"
#include "../common/runtime_profile.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace edvr {

namespace ef = srv_turret::entryfade;

namespace {

constexpr size_t kPairs = 384;   // paired frames kept: 4.3 s at 90 Hz, 2.7 s at 144 Hz; a report is written 250 ms after a run whose window began 1000 ms before the edge, so it needs about 2.35 s

struct Pair {
    uint32_t seq = 0;
    double tMs = 0.0;
    float mean[2] = {-1.0f, -1.0f};
};

ef::Machine g_machine;
uint32_t g_moveCount = 0, g_moveSerial = 0;   // srvTurretEntryFadeNoteMove: how many times the camera move was published, and the serial of the latest
Pair g_pairs[kPairs];
size_t g_pairCount = 0;                       // oldest first, in seq order
struct Pending {
    bool active = false;
    ef::Run run;
    double atMs = 0.0;
};
Pending g_pending;
uint32_t g_reports = 0;
SrvTurretPressWords g_press;   // srvTurretEntryFadeNotePress: this frame's press gate words
bool g_capSaid = false;

// A reading into the window: the pair of its frame (the other eye's reading may be there already), or a new pair in seq order.
void addReading(const LumaBurstReading& r) {
    if (r.eye < 0 || r.eye > 1) return;
    size_t i = g_pairCount;
    while (i > 0 && g_pairs[i - 1].seq > r.seq) --i;   // the position after the last pair at or before r.seq
    if (i > 0 && g_pairs[i - 1].seq == r.seq) {
        g_pairs[i - 1].mean[r.eye] = r.mean;
        return;
    }
    if (g_pairCount == kPairs) {   // full: the oldest goes
        std::memmove(&g_pairs[0], &g_pairs[1], (kPairs - 1) * sizeof(Pair));
        --g_pairCount;
        if (i > 0) --i;
    }
    std::memmove(&g_pairs[i + 1], &g_pairs[i], (g_pairCount - i) * sizeof(Pair));
    Pair p;
    p.seq = r.seq;
    p.tMs = r.tMs;
    p.mean[r.eye] = r.mean;
    g_pairs[i] = p;
    ++g_pairCount;
}

void drainReadings() {
    LumaBurstReading buf[32];
    for (;;) {
        const size_t n = lumaProbeTakeBurst(buf, 32);
        for (size_t k = 0; k < n; ++k) addReading(buf[k]);
        if (n < 32) break;
    }
}

void say(const char* line) { Log::get().note("%s", line); }

// A run's report: its line, then the luma window from kLeadMs before the edge to kTailMs past the run's end, thinned to at most kMaxSamples.
void report(const ef::Run& run) {
    if (g_reports >= ef::kMaxReports) {
        if (!g_capSaid) {
            g_capSaid = true;
            char line[200];
            ef::formatCapLine(line, sizeof(line));
            say(line);
        }
        return;
    }
    ++g_reports;
    char line[1200];
    if (run.skip != ef::Skip::kNone) {
        ef::formatSkipped(line, sizeof(line), run);
        say(line);
        return;
    }
    ef::formatRun(line, sizeof(line), run);
    say(line);
    ef::formatPressEntry(line, sizeof(line), run);   // the press-to-sheet line: one flight measures it
    say(line);
    const double lo = run.startMs - ef::kLeadMs;
    const double runEnd = run.fadeEnded ? run.endMs : (run.settle != ef::Settle::kNone ? run.settleMs + ef::kFadeInMs : run.startMs + ef::kCapMs);
    const double hi = runEnd + ef::kTailMs;
    uint32_t total = 0;
    for (size_t i = 0; i < g_pairCount; ++i) {
        if (g_pairs[i].tMs >= lo && g_pairs[i].tMs <= hi) ++total;
    }
    if (total == 0) {
        ef::formatNoLuma(line, sizeof(line), run);
        say(line);
        return;
    }
    const uint32_t stride = (total + ef::kMaxSamples - 1) / ef::kMaxSamples;
    int len = ef::formatTraceHeader(line, sizeof(line), run, total, stride);
    uint32_t onLine = 0, seen = 0;
    for (size_t i = 0; i < g_pairCount; ++i) {
        const Pair& p = g_pairs[i];
        if (p.tMs < lo || p.tMs > hi) continue;
        if (seen++ % stride != 0) continue;
        if (len < 0 || static_cast<size_t>(len) + 40 >= sizeof(line)) break;
        ef::Sample s;
        s.tMs = p.tMs - run.startMs;
        s.mean[0] = p.mean[0];
        s.mean[1] = p.mean[1];
        s.phase = ef::phaseAt(run, s.tMs);
        len += ef::appendSample(line, sizeof(line), static_cast<size_t>(len), s);
        if (++onLine == ef::kSamplesPerLine) {
            say(line);
            len = ef::formatTraceHeader(line, sizeof(line), run, total, stride);
            onLine = 0;
        }
    }
    if (onLine > 0) say(line);
}

}  // namespace

void srvTurretEntryFadeNotePress(const SrvTurretPressWords& w) { g_press = w; }

void srvTurretEntryFadeNoteMove(uint32_t serial) {
    ++g_moveCount;
    g_moveSerial = serial;
}

double srvTurretEntryFadeTick(uint32_t frameNo) {
    const double now = lumaProbeClockMs();
    const bool vr = runtimeVrProfile();
    // In the Scorpion in VR: Status.json's seat bits say an SRV (Flags bit 26 -> seat kind bit 2) and the journal says which one. The burst reading and the 100 ms Status.json cadence
    // are on exactly then; the cadence is what brings the edge in sooner.
    const bool inSrv = vr && (journalSeatKind() & 4u) != 0 && journalSrvScorpion();
    lumaProbeBurst(inSrv);
    journalWatchSetEagerStatusSrv(inSrv);
    drainReadings();
    if (!vr) {
        g_machine.reset();
        return 0.0;
    }

    ef::Input in;
    in.nowMs = now;
    in.frame = frameNo;
    in.on = srvTurretViewOn();
    in.moving = srvTurretBackSeatedZ() != 0.0;
    in.moveCount = g_moveCount;
    in.moveSerial = g_moveSerial;
    in.pressAgeMs = g_press.ageMs;
    in.sheetEdge = g_press.sheetEdge;
    in.noPress = g_press.noPress;
    in.fallback = g_press.fallback;
    in.sheetLagMs = g_press.sheetLagMs;
    in.statusLagMs = g_press.statusLagMs;
    if (in.on || g_machine.phase() != ef::Phase::kIdle) {
        EdvrNativeNowPose np{};
        // The word is meaningful from the version 3 input on (the same guard the F89 readers use); 0 is "not reported", which the machine reads as no signal.
        if (nativeFrameNowPose(np) && np.inputVersion >= 3) in.appliedWord = np.eyeAlignApplied;
    }

    ef::Events ev;
    const double level = g_machine.step(in, ev);
    if (ev.started || ev.skipped) {
        // A new edge: the run before it reports now if its report was still waiting (it ended less than kReportAfterMs ago) or was cut short by this edge.
        if (g_pending.active) {
            report(g_pending.run);
            g_pending.active = false;
        } else if (g_machine.havePrev() && g_machine.prev().cutShort) {
            report(g_machine.prev());
        }
        if (ev.skipped) report(g_machine.run());   // no hold ran: its one line now
    }
    if (ev.ended) {
        g_pending.active = true;
        g_pending.run = g_machine.run();
        g_pending.atMs = now + ef::kReportAfterMs;
    }
    if (g_pending.active && now >= g_pending.atMs) {
        report(g_pending.run);
        g_pending.active = false;
    }
    return level;
}

}  // namespace edvr

// The rig for the skin ledger (src\d3d11\skin_ledger.h, docs/kinematic-motion-injection-2026-09-19.md, "F2 study"): the
// F2 settling instrument that rides an armed eye run. It links nothing of the DLLs and needs no device; the glue that
// reads the context (object_probe.cpp, exposure_fix.cpp) is held by source text.
//
// Every check's label starts with its case id (S1..S12), which is how tools\skin_ledger_test\mutants.py tells a
// mutation that tripped the right check from one that tripped something else:
//
//   S1   the constants: the window, the two shader hashes, the row and job sizes, a kept prefix that is whole rows
//   S2   UNARMED: every event is a no-op, nothing is planned or staged, no report line, no state to write
//   S3   the window: its first and last frame are in, the frames either side are not, and events outside are ignored
//   S4   the copy plan: jobs and joints per dispatch, joints once per frame per buffer, bind poses once per run, the caps
//   S5   the numbers: jobs, bones, dst end, the running-sum gaps and overlaps, the t33 bases that are not a job's dst
//   S6   a whole run end to end: 20 frame lines and a RAN result, and finished only after the last line was said
//   S7   FEWER FRAMES THAN THE WINDOW: three frames of events still close and report, saying PARTIAL, one line a frame
//   S8   NEVER RAN: no dispatch at all, and dispatches that were none of the palette chain, are told apart
//   S9   BROKEN: a chain that ran with copies lost, a job table that never arrived, a missing palette
//   S10  a run the process ended before the window closed: the report says so and still comes out
//   S11  the file: the layout the checker reads, written by the production serialiser, and a fixture directory
//        (skin file, bones files, pool files) that tools\skin_palette_check.py --verify-fixture reads end to end
//   S12  the glue, by source text: the hook asks before it acts, the arm and the report sit where they must,
//        the palette copy is whole
//
//   skin_ledger_test.exe --dry-run                   touches nothing
//   skin_ledger_test.exe --self-test [root]          root is the repository, for the S12 source pins (skipped without it)
//   skin_ledger_test.exe --fixture <dir>             writes the S11 fixture into <dir> (and runs the self-test without pins)
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "skin_ledger.h"

using namespace edvr;
using namespace edvr::skin;

namespace {
unsigned g_checks = 0, g_failures = 0;

void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", label);
        std::fflush(stdout);   // a crash after this check must not take its label with it
    }
}

bool contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

std::vector<uint8_t> jobsOf(const std::vector<std::vector<uint32_t>>& jobs) {   // each {src, dst, bind, count}
    std::vector<uint8_t> out;
    for (const auto& j : jobs) {
        for (uint32_t v : j) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
            out.insert(out.end(), p, p + 4);
        }
    }
    return out;
}

DispatchInfo infoOf(uint32_t groups, uint64_t t0 = 0x1000, uint64_t t1 = 0x2000, uint64_t t2 = 0x3000, uint64_t u0 = 0x4000) {
    DispatchInfo i;
    i.x = groups;
    i.y = i.z = 1;
    i.t0 = t0;
    i.t1 = t1;
    i.t2 = t2;
    i.u0 = u0;
    i.v0.valid = i.v1.valid = i.v2.valid = i.vu.valid = 1;
    i.v0.stride = 16;
    i.v1.stride = 16;
    i.v2.stride = 48;
    i.vu.stride = 48;
    return i;
}

// One healthy frame: a chain dispatch of `groups` jobs, its copies delivered, the pool drawn, both palettes read back.
void healthyFrame(SkinLedger& s, uint32_t f, const std::vector<uint8_t>& jobs, bool secondPalette = true) {
    s.noteDispatch(f, kClearHash);
    s.noteDispatch(f, kChainHash);
    const uint32_t groups = static_cast<uint32_t>(jobs.size() / kJobBytes);
    const ChainPlan plan = s.planChain(f, infoOf(groups), static_cast<uint32_t>(jobs.size()), 480, 4096);
    if (plan.disp >= 0) {
        if (plan.stageJobs) s.deliver(Kind::Jobs, f, plan.disp, jobs.data(), static_cast<uint32_t>(jobs.size()));
        const std::vector<uint8_t> joints(480, 0x11);
        if (plan.stageJoints) s.deliver(Kind::Joints, f, plan.disp, joints.data(), static_cast<uint32_t>(joints.size()));
        if (plan.stageBind) {
            const std::vector<uint8_t> bind(4096, 0x22);
            s.deliver(Kind::Bind, f, plan.bind, bind.data(), static_cast<uint32_t>(bind.size()));
        }
    }
    s.notePoolDraw(f, 0, 0xA000);
    s.notePaletteIssued(f, true);
    s.notePaletteGot(f);
    if (secondPalette) {
        s.notePaletteIssued(f, true);
        s.notePaletteGot(f);
    }
}

struct Lines {
    std::vector<std::string> v;
    std::string at(size_t i) const { return i < v.size() ? v[i] : std::string(); }   // a report with fewer lines is a failed check, not a crash
    bool finishedDuring = false;
    const SkinLedger* watch = nullptr;
    SkinLedger::Sink sink() {
        return [this](const std::string& l) {
            v.push_back(l);
            if (watch && watch->finished()) finishedDuring = true;
        };
    }
    std::string result() const {
        for (const std::string& l : v)
            if (l.find("skin ledger RESULT") != std::string::npos) return l;
        return "";
    }
};

// ---- S1 ------------------------------------------------------------------------------------------------------------
void constantCases() {
    check(kFrames == 20 && kMaxDispatches == 4 && kMaxBinds == 4 && kMaxPalettes == 4, "S1.constants: a 20-frame window, 4 dispatches a frame, 4 bind poses, 4 palettes");
    check(kChainHash == 0x6FE04AF836BB1DBAull && kClearHash == 0x7B2A531B72941109ull,
          "S1.hashes: APPLY_BIND_POSE_TRANSFORMS_CS is 6FE04AF836BB1DBA and CLEAR_TRANSFORM_DATA_CS is 7B2A531B72941109");
    check(kRowBytes == 48 && kJobBytes == 16 && kRecordBytes == 336, "S1.sizes: a palette row is 48 bytes, a job 16, a pool record 336");
    check(kKeepPaletteBytes % kRowBytes == 0 && kKeepPaletteBytes / kRowBytes == 65536,
          "S1.keep: the kept prefix of a palette copy is whole rows, rows 0..65535 (a box that is not whole rows is what read zeros)");
}

// ---- S2 ------------------------------------------------------------------------------------------------------------
void unarmedCases() {
    SkinLedger s;
    check(!s.armed() && !s.finished() && !s.inWindow(0) && !s.inWindow(100), "S2.state: a fresh ledger is not armed, not finished and has no window");
    s.noteDispatch(100, kChainHash);
    const ChainPlan plan = s.planChain(100, infoOf(3), 48, 480, 4096);
    s.notePoolDraw(100, 0, 1);
    s.notePaletteIssued(100, true);
    s.notePaletteGot(100);
    s.noteLost(100);
    s.setPalette(0, 7, 1000);
    const std::vector<uint8_t> bytes(48, 1);
    s.deliver(Kind::Jobs, 100, 0, bytes.data(), 48);
    s.deliver(Kind::Bind, 100, 0, bytes.data(), 48);
    const Frame& f = s.frame(0);
    check(plan.disp == -1 && !plan.stageJobs && !plan.stageJoints && !plan.stageBind,
          "S2.plan: unarmed, a chain dispatch is planned for no copy at all");
    check(f.dispatches == 0 && f.chainSeen == 0 && f.d.empty() && !f.pool && f.palIssued == 0 && f.palGot == 0 && f.lost == 0 && s.binds().empty(),
          "S2.events: unarmed, no event leaves any state behind (dispatches, plan, pool draw, palette copies, lost copies, bind poses)");
    Lines lines;
    s.report("000000", nullptr, "window closed", "written", lines.sink());
    check(lines.v.empty() && !s.finished(), "S2.report: unarmed, the report says nothing and does not mark itself finished");
    // Armed, then reset: the same silence, and nothing of the run survives
    s.arm(10);
    healthyFrame(s, 10, jobsOf({{0, 1, 0, 5}}));
    s.reset();
    check(!s.armed() && s.frame(0).dispatches == 0 && s.frame(0).d.empty() && s.binds().empty(), "S2.reset: a reset drops the run's frames and bind poses and disarms");
    Lines after;
    s.report("000000", nullptr, "window closed", "written", after.sink());
    check(after.v.empty(), "S2.reset: after a reset the report is silent again");
}

// ---- S3 ------------------------------------------------------------------------------------------------------------
void windowCases() {
    SkinLedger s;
    s.arm(100);
    check(s.armed() && s.frame0() == 100, "S3.arm: the window starts at the frame it was armed for");
    check(!s.inWindow(99) && s.inWindow(100) && s.inWindow(119) && !s.inWindow(120), "S3.bounds: frames 100..119 are in, 99 and 120 are out");
    s.noteDispatch(99, kChainHash);
    s.noteDispatch(120, kChainHash);
    s.noteDispatch(100, kChainHash);
    s.noteDispatch(119, kChainHash);
    uint32_t total = 0;
    for (uint32_t i = 0; i < kFrames; ++i) total += s.frame(i).chainSeen;
    check(total == 2 && s.frame(0).chainSeen == 1 && s.frame(19).chainSeen == 1, "S3.events: only the events inside the window are counted, the first frame's and the last frame's");
    s.noteDispatch(105, 0x1234);
    s.noteDispatch(105, kClearHash);
    check(s.frame(5).dispatches == 2 && s.frame(5).clears == 1 && s.frame(5).chainSeen == 0, "S3.counts: every dispatch counts, the identity fill is counted apart, neither is a chain dispatch");
    check(s.planChain(99, infoOf(2), 32, 480, 4096).disp == -1 && s.planChain(120, infoOf(2), 32, 480, 4096).disp == -1,
          "S3.plan: a chain dispatch outside the window is planned for no copy");
}

// ---- S4 ------------------------------------------------------------------------------------------------------------
void planCases() {
    SkinLedger s;
    s.arm(0);
    ChainPlan a = s.planChain(0, infoOf(3), 48, 480, 4096);
    check(a.disp == 0 && a.stageJobs && a.stageJoints && a.stageBind && a.bind == 0, "S4.first: the first dispatch stages its job table, its joints and its bind poses");
    ChainPlan b = s.planChain(0, infoOf(2, 0x1100), 32, 480, 4096);
    check(b.disp == 1 && b.stageJobs && !b.stageJoints && !b.stageBind && b.bind == 0,
          "S4.shared: a second dispatch of the frame over the same joints and bind poses stages only its own job table");
    check(s.frame(0).d[1].jointsFrom == 0 && s.frame(0).d[0].jointsFrom == 0, "S4.shared: it points at the dispatch that holds the joints");
    ChainPlan c = s.planChain(0, infoOf(2, 0x1200, 0x2200, 0x3200), 32, 960, 8192);
    check(c.disp == 2 && c.stageJobs && c.stageJoints && c.stageBind && c.bind == 1, "S4.distinct: another joint buffer and another bind-pose buffer are staged");
    ChainPlan d = s.planChain(0, infoOf(2, 0x1300, 0x2300, 0x3300), 32, 960, 8192);
    ChainPlan e = s.planChain(0, infoOf(2, 0x1400, 0x2400, 0x3400), 32, 960, 8192);
    check(d.disp == 3 && e.disp == -1 && s.frame(0).chainOver == 1, "S4.cap: the fifth dispatch of a frame is counted, not kept");
    ChainPlan nextFrame = s.planChain(1, infoOf(3), 48, 480, 4096);
    check(nextFrame.disp == 0 && nextFrame.stageJoints && !nextFrame.stageBind && nextFrame.bind == 0,
          "S4.perFrame: the joints are staged again in the next frame, the bind poses (the same buffer) are not");
    SkinLedger t;
    t.arm(0);
    for (uint64_t k = 0; k < kMaxBinds + 1; ++k) (void)t.planChain(static_cast<uint32_t>(k), infoOf(1, 0x10 + k, 0x20 + k, 0x30 + k), 16, 48, 48);
    check(t.binds().size() == kMaxBinds && t.frame(kMaxBinds).declined >= 1, "S4.binds: the fifth distinct bind-pose buffer of a run is declined and counted");
    SkinLedger u;
    u.arm(0);
    const ChainPlan big = u.planChain(0, infoOf(1), 16, kMaxCopyBytes + 1, 48);
    const ChainPlan zero = u.planChain(0, infoOf(1, 0x50, 0x60, 0x70), 0, 48, 48);
    check(!big.stageJoints && u.frame(0).declined >= 1, "S4.size: a copy over the whole-buffer cap is declined, not staged");
    check(!zero.stageJobs && u.frame(0).declined >= 2, "S4.size: a buffer of no bytes is declined too");
    SkinLedger w;
    w.arm(0);
    DispatchInfo unresolved = infoOf(1);
    unresolved.t0 = 0;
    const ChainPlan nr = w.planChain(0, unresolved, 16, 48, 48);
    check(!nr.stageJobs && nr.disp == 0, "S4.unresolved: a binding that did not resolve is declined, the dispatch itself is still kept");
}

// ---- S5 ------------------------------------------------------------------------------------------------------------
std::vector<uint8_t> poolOf(const std::vector<uint32_t>& bases) {
    std::vector<uint8_t> p(bases.size() * kRecordBytes, 0);
    for (size_t i = 0; i < bases.size(); ++i) std::memcpy(p.data() + i * kRecordBytes, &bases[i], 4);
    return p;
}

void numberCases() {
    SkinLedger s;
    s.arm(0);
    // three jobs in running-sum order: dst 1 (5 bones), 6 (4), 10 (6)
    const std::vector<uint8_t> jobs = jobsOf({{0, 1, 0, 5}, {5, 6, 5, 4}, {9, 10, 9, 6}});
    healthyFrame(s, 0, jobs);
    std::vector<uint8_t> pool = poolOf({0, 1, 6, 6, 10, 777});   // two rigid, a duplicate, 777 is no job's dst
    PoolRef pr{pool.data(), pool.size()};
    FrameStats st = s.analyse(0, &pr);
    check(st.groups == 3 && st.jobs == 3 && st.jobsMissing == 0 && st.bones == 15 && st.dstEnd == 16, "S5.jobs: three jobs, fifteen bones, dst end row 16");
    check(st.gaps == 0 && st.overlaps == 0, "S5.runningSum: jobs that each start where the last ended have no gap and no overlap");
    check(st.poolKnown && st.poolSkinned == 4 && st.poolNotDst == 1, "S5.pool: four distinct nonzero bases (a duplicate counted once), one of them (777) is not a job's dst");
    // a gap and an overlap
    SkinLedger g;
    g.arm(0);
    healthyFrame(g, 0, jobsOf({{0, 1, 0, 5}, {5, 8, 5, 4}, {9, 10, 9, 6}}));   // 1..5, then 8..11 (gap), then 10..15 (overlap)
    st = g.analyse(0, nullptr);
    check(st.gaps == 1 && st.overlaps == 1 && !st.poolKnown, "S5.runningSum: a job that starts late is a gap, one that starts early an overlap; no pool copy means the bases are unknown, not zero");
    // a job table that is short: groups with no job
    SkinLedger m;
    m.arm(0);
    m.noteDispatch(0, kChainHash);
    const ChainPlan p = m.planChain(0, infoOf(5), 80, 480, 4096);
    const std::vector<uint8_t> two = jobsOf({{0, 1, 0, 5}, {5, 6, 5, 4}});
    m.deliver(Kind::Jobs, 0, p.disp, two.data(), static_cast<uint32_t>(two.size()));
    st = m.analyse(0, nullptr);
    check(st.groups == 5 && st.jobs == 2 && st.jobsMissing == 3, "S5.missing: five groups and a table of two jobs is three groups with no job");
    // a job of count 0 advances nothing and is not a gap
    SkinLedger z;
    z.arm(0);
    healthyFrame(z, 0, jobsOf({{0, 1, 0, 5}, {5, 9, 5, 0}, {5, 6, 5, 4}}));   // the empty job sits at 9, inside 6..9's tail
    st = z.analyse(0, nullptr);
    check(st.gaps == 0 && st.overlaps == 0 && st.bones == 9 && st.dstEnd == 10, "S5.zero: a job with no bones is counted nowhere in the running sum, nor in the dst end");
}

// ---- S6 ------------------------------------------------------------------------------------------------------------
void runCases() {
    SkinLedger s;
    s.arm(500);
    const std::vector<uint8_t> jobs = jobsOf({{0, 1, 0, 5}, {5, 6, 5, 4}});
    std::vector<uint8_t> pool = poolOf({0, 1, 6});
    for (uint32_t k = 0; k < kFrames; ++k) healthyFrame(s, 500 + k, jobs, k != 0);
    s.setPalette(0, 0xA000, 8388624);
    s.setPalette(1, 0xB000, 8388624);
    PoolRef pools[kFrames];
    for (auto& p : pools) p = PoolRef{pool.data(), pool.size()};
    Lines lines;
    lines.watch = &s;
    s.report("123456", pools, "window closed", "written", lines.sink());
    check(lines.v.size() == kFrames + 1, "S6.lines: twenty frame lines and one result");
    check(contains(lines.at(0), "skin ledger frame 500:") && contains(lines.at(19), "skin ledger frame 519:"), "S6.lines: the frame lines carry their ledger frame numbers in order");
    check(contains(lines.at(3), "chain 1 kept 1") && contains(lines.at(3), "2 jobs") && contains(lines.at(3), "9 bones") && contains(lines.at(3), "pool draw yes") &&
              contains(lines.at(3), "palettes copied 2 read back 2") && contains(lines.at(3), "(not a job dst 0)"),
          "S6.lines: a frame line says its dispatches, jobs, bones, pool draw, palette copies and t33 mismatches");
    const std::string r = lines.result();
    check(contains(r, "RESULT RAN") && contains(r, "eye run 123456") && contains(r, "frames with chain 20/20") && contains(r, "skin_123456.bin written"),
          "S6.result: a whole run says RAN, names the run and the file");
    check(!lines.finishedDuring && s.finished(), "S6.finished: finished() is false while any line is being said and true after the last");
    Lines again;
    s.report("123456", pools, "window closed", "written", again.sink());
    check(again.v.empty(), "S6.once: a second report says nothing");
}

// ---- S7 ------------------------------------------------------------------------------------------------------------
void fewFrameCases() {
    SkinLedger s;
    s.arm(40);
    const std::vector<uint8_t> jobs = jobsOf({{0, 1, 0, 5}});
    for (uint32_t k = 0; k < 3; ++k) healthyFrame(s, 40 + k, jobs);
    Lines lines;
    lines.watch = &s;
    s.report("000042", nullptr, "window closed", "written", lines.sink());
    check(lines.v.size() == kFrames + 1, "S7.lines: three frames of events still give twenty frame lines and a result");
    const std::string r = lines.result();
    check(contains(r, "RESULT PARTIAL") && contains(r, "frames with chain 3/20") && contains(r, "pool draw 3/20"), "S7.result: the result says PARTIAL and counts the frames that arrived");
    check(contains(lines.at(10), "0 dispatches") && contains(lines.at(10), "pool draw NO"), "S7.lines: a frame nothing arrived in says so");
    check(s.finished() && !lines.finishedDuring, "S7.finished: the window closed and reported, and finished only after the lines");
}

// ---- S8 ------------------------------------------------------------------------------------------------------------
void neverRanCases() {
    SkinLedger s;
    s.arm(0);
    Lines none;
    s.report("000001", nullptr, "window closed", "written", none.sink());
    check(none.v.size() == kFrames + 1 && contains(none.result(), "RESULT NEVER RAN") && contains(none.result(), "no compute dispatch reached the hook"),
          "S8.noDispatch: an armed run that saw no dispatch at all says NEVER RAN and that none reached the hook");
    SkinLedger t;
    t.arm(0);
    for (uint32_t k = 0; k < 5; ++k) {
        t.noteDispatch(k, 0xDEADBEEF);
        t.noteDispatch(k, kClearHash);
    }
    Lines other;
    t.report("000002", nullptr, "window closed", "written", other.sink());
    check(contains(other.result(), "RESULT NEVER RAN") && contains(other.result(), "10 compute dispatches seen, none was cs_6FE04AF836BB1DBA"),
          "S8.otherDispatch: dispatches that were none of the palette chain say so, with the count and the hash looked for");
    check(!contains(none.result(), "none was cs_") && !contains(other.result(), "no compute dispatch reached"),
          "S8.distinct: the two never-ran reasons are told apart");
    check(t.finished() && s.finished(), "S8.finished: a never-ran run is finished all the same");
    SkinLedger f;
    f.arm(0);
    f.noteForeign(5);
    Lines fl;
    f.report("000009", nullptr, "window closed", "written", fl.sink());
    check(contains(fl.result(), "RESULT NEVER RAN") && contains(fl.result(), "5 dispatches on deferred contexts counted and not read"),
          "S8.foreign: dispatches recorded on deferred contexts are counted in the result, which is still NEVER RAN (none was read)");
}

// ---- S9 ------------------------------------------------------------------------------------------------------------
void brokenCases() {
    const std::vector<uint8_t> jobs = jobsOf({{0, 1, 0, 5}});
    {
        SkinLedger s;
        s.arm(0);
        for (uint32_t k = 0; k < kFrames; ++k) healthyFrame(s, k, jobs);
        s.noteLost(7);
        Lines l;
        s.report("000003", nullptr, "window closed", "written", l.sink());
        check(contains(l.result(), "RESULT BROKEN") && contains(l.result(), "1 copies were lost"), "S9.lost: one lost copy makes a run BROKEN, and the line counts it");
    }
    {
        SkinLedger s;
        s.arm(0);
        s.noteDispatch(0, kChainHash);
        const ChainPlan p = s.planChain(0, infoOf(2), 32, 480, 4096);   // staged; the job table never arrives, the rest does
        const std::vector<uint8_t> joints(480, 1), bind(4096, 2);
        s.deliver(Kind::Joints, 0, p.disp, joints.data(), 480);
        s.deliver(Kind::Bind, 0, p.bind, bind.data(), 4096);
        s.notePoolDraw(0, 0, 1);
        Lines l;
        s.report("000004", nullptr, "window closed", "written", l.sink());
        check(contains(l.result(), "RESULT BROKEN") && contains(l.result(), "0 of 1 chain frames hold their data"),
              "S9.undelivered: a chain frame whose staged copies never arrived is BROKEN, not PARTIAL");
    }
    {
        SkinLedger s;
        s.arm(0);
        const std::vector<uint8_t> two = jobsOf({{0, 1, 0, 5}, {5, 6, 5, 4}});
        s.noteDispatch(0, kChainHash);
        const ChainPlan p = s.planChain(0, infoOf(5), 80, 480, 4096);
        const std::vector<uint8_t> joints(480, 1), bind(4096, 2);
        s.deliver(Kind::Jobs, 0, p.disp, two.data(), static_cast<uint32_t>(two.size()));
        s.deliver(Kind::Joints, 0, p.disp, joints.data(), 480);
        s.deliver(Kind::Bind, 0, p.bind, bind.data(), 4096);
        Lines l;
        s.report("000005", nullptr, "window closed", "written", l.sink());
        check(contains(l.result(), "RESULT BROKEN") && contains(l.result(), "3 groups have no job table"), "S9.short: groups without a job are BROKEN and counted");
    }
    {
        SkinLedger s;
        s.arm(0);
        for (uint32_t k = 0; k < kFrames; ++k) {
            s.noteDispatch(k, kChainHash);
            const ChainPlan p = s.planChain(k, infoOf(1), 16, 48, 48);
            const std::vector<uint8_t> j = jobsOf({{0, 1, 0, 5}}), x(48, 3);
            s.deliver(Kind::Jobs, k, p.disp, j.data(), 16);
            s.deliver(Kind::Joints, k, p.disp, x.data(), 48);
            if (p.stageBind) s.deliver(Kind::Bind, k, p.bind, x.data(), 48);
            s.notePoolDraw(k, 0, 1);
            s.notePaletteIssued(k, true);   // issued, never read back
        }
        Lines l;
        s.report("000006", nullptr, "window closed", "written", l.sink());
        check(contains(l.result(), "RESULT BROKEN") && contains(l.result(), "0 of 20 pool-draw frames hold a palette"), "S9.palette: pool draws with no palette read back are BROKEN");
    }
    {
        SkinLedger s;   // the chain ran every frame with its data, and no pool draw ever bound a palette
        s.arm(0);
        for (uint32_t k = 0; k < kFrames; ++k) {
            s.noteDispatch(k, kChainHash);
            const ChainPlan p = s.planChain(k, infoOf(1), 16, 48, 48);
            const std::vector<uint8_t> j = jobsOf({{0, 1, 0, 5}}), x(48, 3);
            s.deliver(Kind::Jobs, k, p.disp, j.data(), 16);
            s.deliver(Kind::Joints, k, p.disp, x.data(), 48);
            if (p.stageBind) s.deliver(Kind::Bind, k, p.bind, x.data(), 48);
        }
        Lines l;
        s.report("000010", nullptr, "window closed", "written", l.sink());
        check(contains(l.result(), "RESULT BROKEN") && contains(l.result(), "pool draws seen in 0 of 20 frames"),
              "S9.noPool: a chain that ran with no pool draw to join it to is BROKEN and says no pool draw was seen, not NEVER RAN and not PARTIAL");
    }
    {
        SkinLedger s;
        s.arm(0);
        for (uint32_t k = 0; k < kFrames; ++k) healthyFrame(s, k, jobs);
        s.notePaletteIssued(3, false);   // a palette copy that could not be staged
        Lines l;
        s.report("000007", nullptr, "window closed", "written", l.sink());
        check(contains(l.result(), "RESULT BROKEN") && contains(l.result(), "1 skipped"), "S9.skipped: a copy that could not be staged is counted and makes the run BROKEN");
    }
}

// ---- S10 -----------------------------------------------------------------------------------------------------------
void shutdownCases() {
    SkinLedger s;
    s.arm(0);
    const std::vector<uint8_t> jobs = jobsOf({{0, 1, 0, 5}});
    for (uint32_t k = 0; k < 4; ++k) healthyFrame(s, k, jobs);
    Lines l;
    l.watch = &s;
    s.report("000008", nullptr, "SHUTDOWN before the window closed", "not written", l.sink());
    check(l.v.size() == kFrames + 1 && contains(l.result(), "SHUTDOWN before the window closed") && contains(l.result(), "skin_000008.bin not written"),
          "S10.shutdown: a run the process ended says what closed it and that its file was not written");
    check(contains(l.at(0), "t33 skinned bases ? 0"), "S10.pool: with no pool copies the bases are said to be unknown, not zero");
    check(s.finished() && !l.finishedDuring, "S10.finished: finished after the lines, as on a normal close");
}

// ---- S11 -----------------------------------------------------------------------------------------------------------
uint32_t u32at(const std::vector<uint8_t>& b, size_t at) {
    uint32_t v = 0;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}

void floatsOf(std::vector<uint8_t>& out, const float* f, size_t n) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(f);
    out.insert(out.end(), p, p + n * 4);
}

// out = S o M for one bone: both 3x4 row-major (the shader's: rows of float4, translation in .w)
void composeBone(const float* S, const float* M, float* out) {
    for (int j = 0; j < 3; ++j) {
        for (int k = 0; k < 3; ++k) {
            float a = 0;
            for (int i = 0; i < 3; ++i) a += S[j * 4 + i] * M[i * 4 + k];
            out[j * 4 + k] = a;
        }
        float t = S[j * 4 + 3];
        for (int i = 0; i < 3; ++i) t += S[j * 4 + i] * M[i * 4 + 3];
        out[j * 4 + 3] = t;
    }
}

void boneMatrix(float* m, float angle, float tx, float ty, float tz) {   // a rotation about z with a translation
    const float c = std::cos(angle), s = std::sin(angle);
    const float v[12] = {c, -s, 0, tx, s, c, 0, ty, 0, 0, 1, tz};
    std::memcpy(m, v, sizeof(v));
}

bool writeBytes(const std::string& path, const std::vector<uint8_t>& b) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(f);
}

// A run of 20 frames written through the production serialiser, with the files around it the checker reads:
// three skinned characters (5, 4 and 6 bones); the second leaves at frame 12 (the list shifts), a fourth joins at 15
// (appended). Two palette buffers alternate by frame; the one not bound holds the last frame's rows. Frame 0 is the
// arming frame: one palette learned, no copy of the other.
std::string g_fixtureDir;

void fixtureCases(const std::string& dir) {
    constexpr uint32_t kF0 = 7000;
    constexpr uint32_t kPaletteRows = 64;
    constexpr uint32_t kPaletteBytes = kPaletteRows * kRowBytes;
    const std::string stamp = "090909";
    SkinLedger s;
    s.arm(kF0);
    s.setPalette(0, 0xA0A0, kPaletteBytes);
    s.setPalette(1, 0xB0B0, kPaletteBytes);
    // bind poses (t1): 15 + 6 bones, a fixed inverse bind each
    std::vector<float> bind;
    for (int b = 0; b < 21; ++b) {
        float m[12];
        boneMatrix(m, 0.1f * b, 0.01f * b, -0.02f * b, 0.5f);
        bind.insert(bind.end(), m, m + 12);
    }
    std::vector<uint8_t> bindBytes;
    floatsOf(bindBytes, bind.data(), bind.size());
    std::vector<std::vector<uint8_t>> pal(2);
    pal[0].assign(kPaletteBytes, 0);
    pal[1].assign(kPaletteBytes, 0);
    // bindBase is in float4 elements of t1 (three a bone): the compute indexes t1 at bindBase + 3 i
    struct Ent { uint32_t counts; uint32_t bindBase; bool alive; };
    for (uint32_t k = 0; k < kFrames; ++k) {
        const uint32_t f = kF0 + k;
        std::vector<Ent> ents = {{5, 0, true}, {4, 15, k < 12}, {6, 27, true}};
        if (k >= 15) ents.push_back({6, 45, true});
        // jobs in list order: src running over entities, dst running from 1
        std::vector<std::vector<uint32_t>> jobs;
        std::vector<uint32_t> entityOf;   // which character a job belongs to (its position in the pool follows the character)
        uint32_t src = 0, dst = 1;
        for (size_t ei = 0; ei < ents.size(); ++ei) {
            const Ent& e = ents[ei];
            if (!e.alive) continue;
            jobs.push_back({src, dst, e.bindBase, e.counts});
            entityOf.push_back(static_cast<uint32_t>(ei));
            src += e.counts;
            dst += e.counts;
        }
        // joints (t2): src rows
        std::vector<float> joints;
        for (uint32_t r = 0; r < src; ++r) {
            float m[12];
            boneMatrix(m, 0.05f * static_cast<float>(k) + 0.02f * static_cast<float>(r), 0.1f * static_cast<float>(r), 0.03f * static_cast<float>(k), 0.2f);
            joints.insert(joints.end(), m, m + 12);
        }
        std::vector<uint8_t> jointBytes;
        floatsOf(jointBytes, joints.data(), joints.size());
        const std::vector<uint8_t> jobBytes = jobsOf(jobs);
        // the palette the chain writes
        const int boundIdx = static_cast<int>(k % 2);
        std::vector<uint8_t> cur = pal[boundIdx];   // the buffer the chain writes still holds what it held two frames ago
        for (const auto& j : jobs) {                // ... in every row no job rewrites (stale rows); the jobs' rows are new
            for (uint32_t i = 0; i < j[3]; ++i) {
                float out[12];
                composeBone(&joints[(j[0] + i) * 12], &bind[(j[2] + 3 * i) * 4], out);
                std::memcpy(cur.data() + static_cast<size_t>(j[1] + i) * kRowBytes, out, kRowBytes);
            }
        }
        pal[boundIdx] = cur;
        // frame 0 has only one palette learned; from frame 1 the other holds the last frame's rows
        s.noteDispatch(f, kClearHash);
        s.noteDispatch(f, kChainHash);
        DispatchInfo info = infoOf(static_cast<uint32_t>(jobs.size()), 0x1000 + (k & 1), 0x2000, 0x3000, boundIdx ? 0xB0B0 : 0xA0A0);
        const ChainPlan plan = s.planChain(f, info, static_cast<uint32_t>(jobBytes.size()), static_cast<uint32_t>(jointBytes.size()), static_cast<uint32_t>(bindBytes.size()));
        if (plan.stageJobs) s.deliver(Kind::Jobs, f, plan.disp, jobBytes.data(), static_cast<uint32_t>(jobBytes.size()));
        if (plan.stageJoints) s.deliver(Kind::Joints, f, plan.disp, jointBytes.data(), static_cast<uint32_t>(jointBytes.size()));
        if (plan.stageBind) s.deliver(Kind::Bind, f, plan.bind, bindBytes.data(), static_cast<uint32_t>(bindBytes.size()));
        s.notePoolDraw(f, boundIdx, boundIdx ? 0xB0B0 : 0xA0A0);
        for (int p = 0; p < 2; ++p) {
            if (k == 0 && p != boundIdx) continue;
            s.notePaletteIssued(f, true);
            s.notePaletteGot(f);
            writeBytes(dir + "\\bones" + std::to_string(p) + "_" + stamp + "_" + std::to_string(f) + ".bin", pal[p]);
        }
        // the pool: rigid records, and two records per living character at its base (one a duplicate)
        std::vector<uint32_t> bases = {0, 0, 0};
        std::vector<float> xs = {0, 0, 0};   // a character's position follows the character, not the base it is given
        for (size_t ji = 0; ji < jobs.size(); ++ji) {
            const auto& j = jobs[ji];
            bases.push_back(j[1]);
            xs.push_back(10.0f + 3.0f * static_cast<float>(entityOf[ji]) + 0.01f * static_cast<float>(k));
            if (j[3] == 5) {   // a duplicate record at the same base, as the pool has
                bases.push_back(j[1]);
                xs.push_back(xs.back());
            }
        }
        std::vector<uint8_t> poolRecs = poolOf(bases);
        for (size_t r = 0; r < bases.size(); ++r) {
            const float pos[3] = {xs[r], 2.0f, -5.0f};
            std::memcpy(poolRecs.data() + r * kRecordBytes + 16, pos, 12);
            const float one = 1.0f;
            std::memcpy(poolRecs.data() + r * kRecordBytes + 4, &one, 4);
        }
        std::vector<uint8_t> file(32, 0);
        std::memcpy(file.data(), "EDVRPOOL", 8);
        const uint32_t hdr[6] = {1u, f, static_cast<uint32_t>(poolRecs.size()), 0u, kRecordBytes, static_cast<uint32_t>(bases.size())};
        std::memcpy(file.data() + 8, hdr, sizeof(hdr));
        file.insert(file.end(), poolRecs.begin(), poolRecs.end());
        writeBytes(dir + "\\pool_" + stamp + "_" + std::to_string(f) + ".bin", file);
    }
    std::vector<uint8_t> bytes = s.serialize();
    check(bytes.size() > 104 && std::memcmp(bytes.data(), "EDVRSKN1", 8) == 0 && u32at(bytes, 8) == 1 && u32at(bytes, 12) == kFrames && u32at(bytes, 16) == kF0 &&
              u32at(bytes, 20) == 2 && u32at(bytes, 24) == kKeepPaletteBytes && u32at(bytes, 28) == kRowBytes && u32at(bytes, 32) == kJobBytes &&
              u32at(bytes, 36) == 1,
          "S11.header: the serialised file starts EDVRSKN1, version 1, 20 frames, the first frame, 2 palettes, the kept prefix, the row and job sizes, one bind-pose buffer");
    uint64_t chain = 0;
    std::memcpy(&chain, bytes.data() + 40, 8);
    check(chain == kChainHash, "S11.header: the chain hash sits at byte 40");
    check(writeBytes(dir + "\\skin_" + stamp + ".bin", bytes), "S11.write: the fixture's skin file is written");
    Lines lines;
    std::vector<PoolRef> none(kFrames);
    s.report(stamp, none.data(), "window closed", "written", lines.sink());
    check(contains(lines.result(), "RESULT"), "S11.report: the fixture run reports");
    std::printf("[skin_ledger_test] fixture: %zu bytes in skin_%s.bin, bones and pool files for frames %u..%u in %s\n", bytes.size(), stamp.c_str(), kF0, kF0 + kFrames - 1, dir.c_str());
}

// ---- S12 -----------------------------------------------------------------------------------------------------------
std::string slurp(const std::string& root, const char* rel) {
    std::ifstream in(root + "\\" + rel, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string t = ss.str();
    std::string out;
    for (char c : t)
        if (c != '\r') out += c;
    return out;
}

size_t at(const std::string& t, const char* needle, size_t from = 0) { return t.find(needle, from); }
unsigned count(const std::string& t, const char* needle) {
    unsigned n = 0;
    for (size_t p = t.find(needle); p != std::string::npos; p = t.find(needle, p + 1)) ++n;
    return n;
}
// the text of the function whose definition line starts with `head`, up to the next line that starts a column-0 `}`
std::string body(const std::string& t, const char* head) {
    const size_t s = t.find(head);
    if (s == std::string::npos) return "";
    const size_t e = t.find("\n}\n", s);
    return e == std::string::npos ? t.substr(s) : t.substr(s, e - s);
}

void sourcePins(const std::string& root) {
    const std::string ex = slurp(root, "src\\d3d11\\exposure_fix.cpp");
    const std::string op = slurp(root, "src\\d3d11\\object_probe.cpp");
    const std::string oh = slurp(root, "src\\d3d11\\object_probe.h");
    const std::string bat = slurp(root, "build.bat");
    check(!ex.empty() && !op.empty() && !oh.empty() && !bat.empty(), "S12.sources: the glue sources are readable from the repository root");
    const std::string disp = body(ex, "void STDMETHODCALLTYPE hookedDispatch(");
    check(count(disp, "if (objectProbeLedgerActive()) objectProbeNoteDispatch(self, x, y, z, ") == 2,
          "S12.hook: both of hookedDispatch's paths (deferred and owner context) ask objectProbeLedgerActive() before calling the skin ledger");
    check(count(ex, "objectProbeNoteDispatch(") == 2, "S12.hook: nothing else in the exposure hook calls the skin ledger");
    check(at(disp, "drawCensusDispatch(self, x, y, z, false") != std::string::npos &&
              at(disp, "objectProbeNoteDispatch(self, x, y, z, false)") > at(disp, "drawCensusDispatch(self, x, y, z, false") &&
              at(disp, "objectProbeNoteDispatch(self, x, y, z, false)") < at(disp, "s->realDispatch(self, x, y, z);", at(disp, "drawCensusDispatch(self, x, y, z, false")),
          "S12.hook: on the owner path the ledger reads after the census record and before the game's dispatch is forwarded");
    check(at(oh, "void objectProbeNoteDispatch(") != std::string::npos, "S12.header: object_probe.h declares objectProbeNoteDispatch");
    const std::string note = body(op, "void objectProbeNoteDispatch(");
    check(!note.empty() && at(note, "if (!detail::g_objectProbeLedgerOn || !ctx) return;") != std::string::npos &&
              at(note, "if (!detail::g_objectProbeLedgerOn || !ctx) return;") < at(note, "CSGetShader") &&
              at(note, "if (!g_skin.inWindow(frame)) return;") < at(note, "CSGetShader"),
          "S12.unarmed: objectProbeNoteDispatch returns when the ledger is off, and outside the window, before it touches the context");
    check(at(note, "if (foreign) {") != std::string::npos && at(note, "if (foreign) {") < at(note, "CSGetShader") &&
              at(note, "g_skinForeign.fetch_add(1, std::memory_order_relaxed);") != std::string::npos &&
              at(note, "g_skinForeign.fetch_add(1, std::memory_order_relaxed);") < at(note, "CSGetShader"),
          "S12.foreign: a dispatch on a deferred context (any thread) is counted atomically and returns before the ledger's state is touched");
    check(at(note, "CopyResource") == std::string::npos && at(note, "skinStage(") != std::string::npos && at(note, "kChainHash") != std::string::npos,
          "S12.chain: only the palette chain's dispatches stage a copy (hash compared), through skinStage");
    const std::string arm = body(op, "void objectProbeArmLedger(");
    check(at(arm, "g_skin.arm(g_ledgerFrame0);") != std::string::npos && at(arm, "g_skin.arm(g_ledgerFrame0);") > at(arm, "detail::g_objectProbeLedgerOn = true;") &&
              at(arm, "g_skin.arm(g_ledgerFrame0);") > at(arm, "ledgerRelease();") && at(arm, "skin ledger: armed with eye run") != std::string::npos,
          "S12.arm: the eye run's arm arms the skin ledger once (after the release of the last run's state) and says so");
    check(count(op, "g_skin.arm(") == 1, "S12.arm: nothing else arms it (no config key, no second trigger)");
    const std::string wl = body(op, "void writeLedger(");
    check(at(wl, "writeSkinLedger(dir, \"window closed\");") != std::string::npos &&
              at(wl, "writeSkinLedger(dir, \"window closed\");") < at(wl, "std::vector<uint8_t>().swap(g_ledgerPool[i]);") &&
              at(wl, "g_skin.reset();") > at(wl, "std::vector<uint8_t>().swap(g_ledgerPool[i]);"),
          "S12.write: the ledger write reports the skin ledger before the pool copies it joins are let go, and resets it after");
    const std::string ws = body(op, "void writeSkinLedger(");
    check(at(ws, "if (!g_skin.armed() || g_skin.finished()) return;") != std::string::npos && at(ws, "g_skin.writeFile(path)") < at(ws, "g_skin.report("),
          "S12.report: the skin file is written before the report, which names what became of it; an unarmed or finished ledger writes nothing");
    const std::string sd = body(op, "void objectProbeShutdown(");
    check(at(sd, "g_skin.armed() && !g_skin.finished()") != std::string::npos && at(sd, "SHUTDOWN before the window closed") != std::string::npos,
          "S12.shutdown: a run armed and never reported is reported at shutdown");
    const std::string pc = body(op, "void paletteCapture(");
    check(at(pc, "g_palette[i], g_paletteBytes[i], g_paletteBytes[i])") != std::string::npos && at(pc, "kLedgerBonesMax") == std::string::npos,
          "S12.whole: every learned palette is copied whole (the cap is its own size, so auxStage takes CopyResource), not as a box of the first megabyte");
    check(at(pc, "g_skin.notePoolDraw(") != std::string::npos && at(pc, "g_paletteSeen[i] = true;") != std::string::npos,
          "S12.both: the first pool draw of a frame notes the bound palette and copies every learned one");
    check(at(op, "constexpr uint32_t kLedgerBonesMax = skin::kKeepPaletteBytes;") != std::string::npos &&
              at(op, "ledgerRead(ctx, c, into, what == 0 ? 0u : kLedgerBonesMax)") != std::string::npos,
          "S12.keep: the readback keeps the first kLedgerBonesMax bytes of a palette, none of the instance stream's is trimmed");
    const std::string rel = body(op, "void ledgerRelease(");
    check(at(rel, "skinReleaseCopies();") != std::string::npos, "S12.release: the skin ledger's staging buffers go with the ledger's");
    const std::string block = [&] {
        const size_t s = bat.find(":rig_skin_ledger_test");
        if (s == std::string::npos) return std::string();
        const size_t e = bat.find("\n:rig_", s + 10);
        return bat.substr(s, e == std::string::npos ? std::string::npos : e - s);
    }();
    check(!block.empty() && contains(block, "skin_ledger_test.cpp") && contains(block, "skin_palette_check.py\" --verify-fixture") &&
              contains(block, "mutants.py\" --self-test") && contains(block, "skin_palette_check.py\" --self-test"),
          "S12.build: build.bat compiles the rig, writes the fixture, has the checker read it and runs both self-tests");
}

}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false, dryRun = false;
    std::string root, fixture;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--self-test") selfTest = true;
        else if (a == "--dry-run") dryRun = true;
        else if (a == "--fixture" && i + 1 < argc) fixture = argv[++i];
        else if (selfTest && root.empty() && a.rfind("--", 0) != 0) root = a;
    }
    if (dryRun) {
        std::printf("[skin_ledger_test] dry-run: touches nothing (no log, no file, no device)\n");
        return 0;
    }
    if (!selfTest && fixture.empty()) {
        std::printf("usage: skin_ledger_test.exe --dry-run | --self-test [root] | --fixture <dir>\n");
        return 2;
    }
    constantCases();
    unarmedCases();
    windowCases();
    planCases();
    numberCases();
    runCases();
    fewFrameCases();
    neverRanCases();
    brokenCases();
    shutdownCases();
    if (!fixture.empty()) {
        CreateDirectoryA(fixture.c_str(), nullptr);
        fixtureCases(fixture);
    } else {
        // The self-test keeps its fixture in a scratch directory of its own so S11's layout checks run every time.
        char tmp[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, tmp);
        const std::string dir = std::string(tmp) + "skin_ledger_test_" + std::to_string(GetCurrentProcessId());
        CreateDirectoryA(dir.c_str(), nullptr);
        fixtureCases(dir);
        // best-effort cleanup of the files this run just wrote
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileA((dir + "\\" + fd.cFileName).c_str());
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryA(dir.c_str());
    }
    if (selfTest) {
        if (root.empty()) std::printf("[skin_ledger_test] SKIPPED the S12 source pins: no repository root given\n");
        else sourcePins(root);
    }
    if (g_failures) {
        std::printf("FAIL: skin ledger: %u of %u checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: %u skin ledger checks\n", g_checks);
    return 0;
}

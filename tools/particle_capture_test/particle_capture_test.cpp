// The particle billboards' constants capture and the Unmap readers probe, pure (no D3D, no files written).
//   P1 the shadow holds exactly the rows the fix reads, byte for byte as the whole-block copy did, and the bytes
//      read from the mapped pointer drop from 5376 to 160 (a counting memcpy shim; the legacy copy is the fail-before)
//   P2 every mapped size: bounds never exceeded, refusals, the readers' gates unchanged
//   P3 the target retires after kRetireFrames without a match, re-arms on the next one, a match postpones it
//   P4 the probe: counts, the sampled mean, the clock's own cost taken off, "not exercised", the window, truncation
//   P5 a smoke-free stretch after a plume: the bytes read from mapped memory, before and after
//   P6 source pins (with a root argument): the probe and the retirement are wired where they run
#include "particle_capture_core.h"
#include "unmap_read_probe.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace edvr;

static int failures = 0;
static void check(bool ok, const char* label) {
    if (!ok) { std::printf("FAIL %s\n", label); ++failures; }
}

// A copy that counts and checks that it never reads outside the mapping it was told about.
struct CountingCopy {
    const uint8_t* base = nullptr;
    uint32_t       mapped = 0;
    uint64_t       bytes = 0;
    uint64_t       calls = 0;
    bool           outOfBounds = false;
    void operator()(void* dst, const void* src, size_t n) {
        const uint8_t* s = static_cast<const uint8_t*>(src);
        if (s < base || s + n > base + mapped) outOfBounds = true;
        std::memcpy(dst, src, n);
        bytes += n;
        ++calls;
    }
};

// What the shadow was before this change: every byte of the block, every write.
static uint32_t legacyCapture(particlecap::Shadow& s, bool steady, const void* data, uint32_t bytes, CountingCopy& c) {
    if (!steady || !data || bytes < 64 || bytes > particlecap::kMaxShadow) { s.valid = false; return 0; }
    c(s.bytes, data, bytes);
    s.size = bytes;
    s.valid = true;
    return bytes;
}

static std::vector<uint8_t> randomBlock(uint32_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = static_cast<uint8_t>(rng());
    return v;
}

// What particle_fix.cpp reads of the shadow, and the gates it reads them behind (solveCamera, shapeOk).
struct ReaderView {
    bool solveGate = false, shapeGate = false;
    uint8_t rows[particlecap::kRangeBytes] = {};
};
static ReaderView view(const particlecap::Shadow& s) {
    ReaderView v;
    v.solveGate = s.valid && s.size >= 274 * 16;
    v.shapeGate = s.valid && (s.size / 4) >= (276 + 4) * 4;
    std::memcpy(v.rows, s.bytes + particlecap::kRangeBegin, particlecap::kRangeBytes);
    return v;
}

static void p1Equivalence() {
    for (uint32_t seed = 1; seed <= 200; ++seed) {
        auto blk = randomBlock(5376, seed);
        particlecap::Shadow a, b;
        CountingCopy ca{blk.data(), 5376}, cb{blk.data(), 5376};
        const uint32_t na = particlecap::capture(a, true, blk.data(), 5376, ca);
        const uint32_t nb = legacyCapture(b, true, blk.data(), 5376, cb);
        check(std::memcmp(a.bytes + particlecap::kRangeBegin, b.bytes + particlecap::kRangeBegin, particlecap::kRangeBytes) == 0,
              "P1.rows-270-279-identical-to-the-whole-block-copy");
        check(a.valid && b.valid && a.size == b.size, "P1.size-and-valid-identical");
        check(std::memcmp(view(a).rows, view(b).rows, particlecap::kRangeBytes) == 0 &&
              view(a).solveGate == view(b).solveGate && view(a).shapeGate == view(b).shapeGate,
              "P1.what-the-readers-see-identical");
        check(na == 160 && ca.bytes == 160 && ca.calls == 1, "P1.new-reads-160-bytes-in-one-copy");
        check(nb == 5376 && cb.bytes == 5376, "P1.legacy-reads-5376-bytes-(the-fail-before)");
        check(!ca.outOfBounds, "P1.in-bounds");
    }
    // The rows the readers index, by name: 270..273 (solveCamera), 278 and 279 (shapeOk).
    auto blk = randomBlock(5376, 7);
    particlecap::Shadow s;
    CountingCopy c{blk.data(), 5376};
    particlecap::capture(s, true, blk.data(), 5376, c);
    for (uint32_t row : {270u, 271u, 272u, 273u, 278u, 279u})
        check(std::memcmp(s.bytes + row * 16, blk.data() + row * 16, 16) == 0, "P1.reader-row-present");
    check(particlecap::kRangeBegin == 4320 && particlecap::kRangeEnd == 4480 && particlecap::kRangeBytes == 160,
          "P1.range-constants");
}

static void p2Sizes() {
    const uint32_t sizes[] = {0, 1, 63, 64, 65, 208, 4319, 4320, 4321, 4383, 4384, 4399, 4400, 4479, 4480, 4481, 5376, 8191, 8192, 8193, 65536};
    for (uint32_t mapped : sizes) {
        auto mem = randomBlock(mapped ? mapped : 1, mapped + 11);
        particlecap::Shadow s;
        // Seed the shadow with a valid older capture so a refusal visibly invalidates it.
        s.valid = true; s.size = 5376;
        CountingCopy c{mem.data(), mapped};
        const uint32_t got = particlecap::capture(s, true, mem.data(), mapped, c);
        const bool accepted = mapped >= particlecap::kMinBlock && mapped <= particlecap::kMaxShadow;
        const uint32_t want = !accepted ? 0u
                              : mapped <= particlecap::kRangeBegin ? 0u
                              : (mapped < particlecap::kRangeEnd ? mapped : particlecap::kRangeEnd) - particlecap::kRangeBegin;
        check(got == want && c.bytes == want, "P2.copied-bytes-for-size");
        check(!c.outOfBounds, "P2.never-reads-past-the-mapping");
        check(s.valid == accepted, "P2.valid-follows-acceptance");
        if (accepted) {
            check(s.size == mapped, "P2.size-is-the-mapped-size");
            const uint32_t have = mapped > particlecap::kRangeBegin ? (mapped < 4480 ? mapped : 4480) - 4320 : 0;
            check(have == 0 || std::memcmp(s.bytes + 4320, mem.data() + 4320, have) == 0, "P2.rows-inside-the-mapping-match");
            // The readers' gates are the legacy gates: solveCamera needs 274 rows mapped, shapeOk 280.
            const ReaderView v = view(s);
            check(v.solveGate == (mapped >= 4384) && v.shapeGate == (mapped >= 4480), "P2.reader-gates-unchanged");
            // A gate that passes must find every row it reads inside the copied range.
            if (v.solveGate) check(std::memcmp(s.bytes + 4320, mem.data() + 4320, 64) == 0, "P2.solve-rows-present");
            if (v.shapeGate) check(std::memcmp(s.bytes + 4448, mem.data() + 4448, 32) == 0, "P2.shape-rows-present");
        } else {
            check(c.calls == 0, "P2.refused-reads-nothing");
        }
    }
    particlecap::Shadow s;
    auto mem = randomBlock(5376, 3);
    CountingCopy c{mem.data(), 5376};
    particlecap::capture(s, true, mem.data(), 5376, c);
    check(s.valid, "P2.armed");
    particlecap::capture(s, false, mem.data(), 5376, c);
    check(!s.valid && c.calls == 1, "P2.stock-mode-invalidates-and-reads-nothing");
    particlecap::capture(s, true, mem.data(), 5376, c);
    particlecap::capture(s, true, nullptr, 5376, c);
    check(!s.valid && c.calls == 2, "P2.null-pointer-invalidates-and-reads-nothing");
}

static void p3Retire() {
    using particlecap::kRetireFrames;
    {
        particlecap::Tracker t;
        t.noteMatch();
        bool fired = false;
        uint32_t at = 0;
        for (uint32_t i = 1; i <= kRetireFrames + 5; ++i) {
            if (t.frameBoundary(true)) { fired = true; at = i; break; }
        }
        check(fired && at == kRetireFrames, "P3.retires-on-exactly-the-Nth-frame-without-a-match");
        check(t.retired == 1 && t.retiredPending, "P3.counted");
        bool again = false;
        for (uint32_t i = 0; i < 2 * kRetireFrames; ++i) again |= t.frameBoundary(false);
        check(!again, "P3.no-target-no-repeat");
        check(t.noteLearned() && t.rearmed == 1, "P3.re-arms-once-on-the-next-learn");
        check(!t.noteLearned() && t.rearmed == 1, "P3.not-twice");
        // Learned at a match: the clock restarts from it.
        t.noteMatch();
        bool early = false;
        for (uint32_t i = 1; i < kRetireFrames; ++i) early |= t.frameBoundary(true);
        check(!early, "P3.fresh-clock-after-re-arm");
        check(t.frameBoundary(true) && t.retired == 2, "P3.retires-again");
    }
    {
        particlecap::Tracker t;
        bool fired = false;
        for (uint32_t i = 0; i < 5000; ++i) {
            if (i % (kRetireFrames - 1) == 0) t.noteMatch();
            fired |= t.frameBoundary(true);
        }
        check(!fired, "P3.a-match-inside-the-window-keeps-the-target");
    }
    {
        particlecap::Tracker t;
        t.noteMatch();
        for (uint32_t i = 1; i < kRetireFrames; ++i) t.frameBoundary(true);
        t.noteMatch();    // one frame short of retirement
        bool early = false;
        for (uint32_t i = 1; i < kRetireFrames; ++i) early |= t.frameBoundary(true);
        check(!early && t.frameBoundary(true), "P3.a-late-match-postpones-by-a-full-window");
    }
    {
        particlecap::Tracker t;
        check(!t.noteLearned() && t.rearmed == 0, "P3.first-learn-is-not-a-re-arm");
    }
}

// The Map/Unmap tee as particle_fix.cpp drives it: 60 writes a frame while a target is held.
struct Session {
    particlecap::Shadow sh;
    particlecap::Tracker tr;
    bool hasTarget = false;
    bool retireEnabled = true;
    CountingCopy copy;
    std::vector<uint8_t> blk = randomBlock(5376, 99);
    uint64_t readBytes = 0;
    Session() { copy.base = blk.data(); copy.mapped = 5376; }
    void frame(bool smoke, bool legacy) {
        if (smoke) {                       // particleOnDraw: a matched draw learns, or keeps, the target
            tr.noteMatch();
            if (!hasTarget) { hasTarget = true; sh.valid = false; tr.noteLearned(); }
        }
        for (int w = 0; w < 60; ++w) {     // the game's writes
            if (!hasTarget) continue;      // particleTarget() null: the Map tee remembers nothing
            if (legacy) { CountingCopy c{blk.data(), 5376}; legacyCapture(sh, true, blk.data(), 5376, c); readBytes += c.bytes; }
            else readBytes += particlecap::capture(sh, true, blk.data(), 5376, copy);
        }
        if (!legacy && retireEnabled && tr.frameBoundary(hasTarget)) { hasTarget = false; sh.valid = false; }
    }
};

static void p5Session() {
    Session now, was;
    for (int f = 0; f < 100; ++f) { now.frame(true, false); was.frame(true, true); }
    const uint64_t smokeNow = now.readBytes, smokeWas = was.readBytes;
    check(smokeNow == 100ull * 60 * 160 && smokeWas == 100ull * 60 * 5376, "P5.while-smoke-is-on-screen-160-against-5376-per-write");
    for (int f = 0; f < 2900; ++f) { now.frame(false, false); was.frame(false, true); }
    const uint64_t quietNow = now.readBytes - smokeNow, quietWas = was.readBytes - smokeWas;
    check(quietWas == 2900ull * 60 * 5376, "P5.legacy-copies-every-write-for-the-whole-quiet-stretch");
    check(quietNow == (particlecap::kRetireFrames - 1) * 60ull * 160, "P5.new-pays-only-until-retirement");
    check(!now.hasTarget && !now.sh.valid, "P5.retired-and-shadow-dropped");
    check(quietNow * 100 < quietWas, "P5.over-a-hundredfold-fewer-bytes-read-from-mapped-memory");
    const uint64_t before = now.readBytes;
    now.frame(true, false);                      // smoke again
    check(now.hasTarget && now.tr.rearmed == 1, "P5.re-armed-by-the-next-matched-draw");
    check(now.readBytes - before == 60ull * 160, "P5.and-capturing-again-in-that-frame");
    check(now.sh.valid && std::memcmp(now.sh.bytes + 4320, now.blk.data() + 4320, 160) == 0, "P5.shadow-rebuilt-from-the-next-write");
}

// ---- the probe ----
static int64_t g_fake = 0;
static int64_t fakeClock() { return g_fake++; }

static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

static void p4Probe() {
    unmapprobe::g_clockForTest = fakeClock;
    unmapprobe::g_frequencyForTest = 1000000000;
    {
        unmapprobe::Probe p;
        p.win.frames = 10;
        for (int i = 0; i < 64; ++i)
            p.timed(unmapprobe::kParticle, [&]() -> uint64_t { g_fake += 2000; return 160; });
        check(p.win.calls[unmapprobe::kParticle] == 64 && p.win.sampled[unmapprobe::kParticle] == 8, "P4.counts-every-call-and-times-one-in-eight");
        check(p.win.bytes[unmapprobe::kParticle] == 64 * 160, "P4.bytes-summed-over-every-call");
        check(p.overheadTicks == 1, "P4.clock-pair-cost-measured");
        char line[1024];
        unmapprobe::formatLine(line, sizeof line, p.win, p.overheadTicks, 1000000000, "particle target armed");
        const std::string s = line;
        check(has(s, "particle 6.4/frame 160 B/call 2.00 us/call 12.8 us/frame"), "P4.particle-figures");
        check(has(s, "total 12.8 us/frame"), "P4.total");
        for (const char* r : {"panel not exercised", "camera-glitch not exercised", "camera-sunglare not exercised",
                              "camera-temporal not exercised", "scene-cb not exercised", "billboard not exercised"})
            check(has(s, r), "P4.unexercised-readers-say-so");
        check(!has(s, "particle not exercised"), "P4.exercised-reader-does-not-say-so");
        check(has(s, "; particle target armed"), "P4.extra-appended");
        check(!has(s, "block is the camera block"), "P4.no-shared-note-without-captures");
    }
    {
        unmapprobe::Probe p;
        char line[1024];
        unmapprobe::formatLine(line, sizeof line, p.win, 0, 1000000000, "");
        const std::string s = line;
        int n = 0;
        for (size_t at = 0; (at = s.find("not exercised", at)) != std::string::npos; ++at) ++n;
        check(n == unmapprobe::kReaderCount, "P4.every-reader-not-exercised-in-an-empty-window");
        check(!has(s, "total") && !has(s, "us/call") && !has(s, "nan") && !has(s, "inf"), "P4.empty-window-has-no-figures");
        check(has(s, "(0 frame(s)"), "P4.zero-frames-says-so");
    }
    {
        unmapprobe::Probe p;
        p.win.frames = 5;
        for (int i = 0; i < 5; ++i) p.timed(unmapprobe::kPanel, [&]() -> uint64_t { return 208; });
        char line[1024];
        unmapprobe::formatLine(line, sizeof line, p.win, 0, 1000000000, "");
        check(has(line, "panel 1.0/frame 208 B/call no timed sample"), "P4.fewer-than-eight-calls-has-no-timed-sample");
    }
    {
        unmapprobe::Probe p;
        int ran = 0;
        p.timedIf(false, unmapprobe::kCamSunglare, [&]() -> uint64_t { ++ran; return 48; });
        p.timedIf(true, unmapprobe::kCamSunglare, [&]() -> uint64_t { ++ran; return 48; });
        check(ran == 2 && p.win.calls[unmapprobe::kCamSunglare] == 1, "P4.a-reader-that-will-not-read-runs-uncounted");
    }
    {
        unmapprobe::Probe p;
        p.noteParticleCapture(true); p.noteParticleCapture(true); p.noteParticleCapture(true); p.noteParticleCapture(false);
        char line[1024];
        unmapprobe::formatLine(line, sizeof line, p.win, 0, 1000000000, "");
        check(has(line, "particle block is the camera block on 3 of 4 capture(s)"), "P4.shared-block-count");
    }
    {
        unmapprobe::Probe p;
        char line[1024];
        int extras = 0;
        auto extra = [&](char* o, size_t c) { ++extras; std::snprintf(o, c, "x"); };
        check(!unmapprobe::frameTick(p, 1000, line, sizeof line, extra), "P4.first-tick-starts-the-window");
        check(!unmapprobe::frameTick(p, 30999, line, sizeof line, extra) && extras == 0, "P4.not-due-before-thirty-seconds");
        p.win.calls[unmapprobe::kBillboard] = 3;
        check(unmapprobe::frameTick(p, 31000, line, sizeof line, extra) && extras == 1, "P4.due-at-thirty-seconds");
        check(has(line, "billboard 1.0/frame") || has(line, "billboard"), "P4.line-carries-the-window");
        check(p.win.calls[unmapprobe::kBillboard] == 0 && p.win.frames == 0, "P4.window-reset");
        check(!unmapprobe::frameTick(p, 40000, line, sizeof line, extra), "P4.next-window-starts-from-the-line");
    }
    {
        unmapprobe::Probe p;
        p.win.calls[unmapprobe::kParticle] = 5;
        char tiny[24];
        std::memset(tiny, 'z', sizeof tiny);
        const size_t n = unmapprobe::formatLine(tiny, sizeof tiny, p.win, 0, 1000000000, "extra");
        check(n < sizeof tiny && tiny[n] == '\0', "P4.truncation-stays-inside-the-buffer");
    }
    unmapprobe::g_clockForTest = nullptr;
    unmapprobe::g_frequencyForTest = 0;
}

static std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static int countOf(const std::string& s, const std::string& part) {
    int n = 0;
    for (size_t at = 0; (at = s.find(part, at)) != std::string::npos; at += part.size()) ++n;
    return n;
}

static void p6Pins(const std::string& root) {
    const std::string pf = slurp(root + "\\src\\d3d11\\particle_fix.cpp");
    const std::string vs = slurp(root + "\\src\\d3d11\\vscreen.cpp");
    check(!pf.empty() && !vs.empty(), "P6.sources-readable");
    check(countOf(pf, "g_blocks.capture(res") == 1, "P6.particle-capture-goes-through-the-core");
    check(pf.find("memcpy(g_shadow") == std::string::npos, "P6.no-whole-block-memcpy-left");
    check(countOf(pf, "g_track.noteMatch();") == 1, "P6.a-matched-draw-tells-the-tracker");
    check(countOf(pf, "g_track.noteLearned()") == 1, "P6.a-learn-tells-the-tracker");
    check(countOf(pf, "g_track.frameBoundary(anyBlockWatched())") == 1, "P6.the-boundary-asks-the-tracker");
    check(countOf(vs, "particleFrameBoundary();") == 1, "P6.vscreen-calls-the-boundary-once");
    check(countOf(vs, "unmapprobe::frameTick(") == 1, "P6.vscreen-prints-the-line-once");
    for (const char* r : {"kPanel", "kCamGlitch", "kCamSunglare", "kCamTemporal", "kSceneCb", "kParticle", "kBillboard"})
        check(countOf(vs, std::string("unmapprobe::") + r) == 1, (std::string("P6.reader-wired-once-") + r).c_str());
    check(countOf(vs, "unmapprobe::g_probe.noteParticleCapture(camBlockReadThisUnmap)") == 1, "P6.shared-block-noted");
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string mode = argv[1];
    if (mode == "--dry-run") {
        std::puts("particle_capture_test: P1-P6 pure cases; no files, no GPU device");
        return 0;
    }
    if (mode != "--self-test") return 2;
    p1Equivalence();
    p2Sizes();
    p3Retire();
    p4Probe();
    p5Session();
    if (argc > 2) p6Pins(argv[2]);
    if (failures) { std::printf("particle_capture_test: %d FAILED\n", failures); return 1; }
    std::puts("particle_capture_test: PASS P1-P6");
    return 0;
}

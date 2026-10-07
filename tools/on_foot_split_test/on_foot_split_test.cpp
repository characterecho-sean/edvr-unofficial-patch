// on_foot_split_test: the ship split on foot (src/common/temporal_mode.h; docs/per-object-motion.md, 2026-10-07).
//
// The bug: in Explorer Cam the ground within ten metres of the commander smeared when he moved. The temporal pass sends a pixel
// nearer than advanced.temporal_aa_ship_metres (10 m) down the HEAD path -- the head's own delta, right for a cockpit that rides with
// the head -- and farther ones down the WORLD path, the game's camera rows. On foot there is no ship, the ground at 1.5 to 10 m is the
// world, and the head path missed the camera's whole walk (eye dump 090359: 24.9 px a frame at the median against 0.84 px for the
// camera's own rows, by block matching the raw crops). The fix is one value: while Status.json says the commander is on foot and not
// seated, the split the shader reads is a millimetre. The shader is not touched. What this rig holds:
//
//   R1  the pure verdict (temporalFootVerdict): on foot, in a ship, in an SRV, unknown, stale, not watched, before LoadGame, the
//       contradictions, the order of the reasons, and the whole table of 192 input combinations against the spec written out again here;
//       and the verdict over time (TemporalFootTracker): the staleness clock at 5000/5001 ms, a stall of the pass's own restarting it, the
//       first question's tick, a count that moves, the changes it returns, a clock 2^32 ms old;
//   R2  the seat from Status.json's flags (journalSeatedFromFlags): bits 24, 25, 26 of Flags and 1, 2 of Flags2, and the bits beside
//       them that must not seat anyone;
//   R3  the split the shader is given (temporalShipSplitMetres): the configured value bit for bit unless on foot, a millimetre on foot,
//       never zero (zero is the world path OFF), and a player's zero stays zero;
//   R4  the production shader on WARP over patches of two real eye dumps (tools/on_foot_split_fixture.py): the Steam Explorer Cam dump
//       and a Frontier cockpit dump, each patch run as a virtual eye with the constants of its motion.csv row and the dump's scene
//       depth:
//         R4a  the dump's own split (10 m) reproduces what the game's shader wrote there -- path, flags, motion, predicted depth -- on
//              every pixel the engine, the holo and the screen paths do not own: the cockpit case and the on-foot dump with the mode
//              OFF are the shader as it was, and the constants this rig builds are the pass's;
//         R4b  with the on-foot split no pixel of the Steam ground takes the head path, every pixel's motion is the camera rows' with
//              their translation (an independent float64 port, written here), and the near ground's motion differs from the head
//              path's by the camera's walk;
//         R4c  the same on the walking rows of the dump's other frames (the camera moving 0.4 to 0.5 m a frame), depth of frame 16401:
//              the near ground now moves as the camera says;
//         R4d  the cockpit: the configured split is the dump's bit for bit and the outputs byte for byte; and the same cockpit under
//              walking rows stays on the head path with the head's motion (the ship rides with the head), which is what a seated
//              commander must never lose to a stale on-foot word (shown flipped, for the record);
//         R4e  the diagnostic counter the registration line prints (Stats[15], the world path's pixels) reads every pixel on foot;
//   R5  source pins: the pass reads the split only through temporalShipSplitMetres, reads each of the journal's answers once, tells
//       each change of mode once and the interval's totals with their zeros; journal_watch.cpp sets the seat from its pure function;
//       the shader's reading of split.x (the gate and the comparison) is the one this relies on. Each pin carries a control;
//   R6  six mutated shaders (the gate, the comparison, the camera's translation, the head's, the decision path, the far plane): the
//       same judge that passed the production shader must refuse every one.
// tools/on_foot_split_test/mutants.py holds the rig to the headers: a rule flipped in them must fail the rig, by the label named.
//
// Run from the repository root: on_foot_split_test.exe --self-test <root>. Exit 0 all passed, 1 a check failed (the first line says which:
// "FAIL R<n><letter>. ..."). --dry-run says what it would do and does nothing. The rig writes no file.

#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "journal_watch.h"             // the seat's masks and pure function only; nothing of journal_watch.cpp is linked
#include "temporal_mode.h"             // the pure verdict and the split
#include "temporal_shader_source.h"    // the production shader text

namespace {
using Microsoft::WRL::ComPtr;
using edvr::TemporalFoot;
using edvr::TemporalFootWhy;

unsigned g_checks = 0;
[[noreturn]] void die(const char* label, const std::string& why) {
    std::fflush(stdout);
    std::printf("FAIL %s. %s\n", label, why.c_str());
    std::fflush(stdout);
    std::exit(1);
}
void check(const char* label, bool ok, const std::string& why) {
    ++g_checks;
    if (!ok) die(label, why);
}
std::string fmt(const char* f, ...) {
    char b[1024];
    va_list a;
    va_start(a, f);
    vsnprintf(b, sizeof b, f, a);
    va_end(a);
    return b;
}
std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) die("R0", fmt("cannot open %s (run from the repository root, or pass it)", path.c_str()));
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
size_t count(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
    return n;
}
bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }

// ---------------------------------------------------------------------------------------------------------------------------------
// R1 -- the verdict
// ---------------------------------------------------------------------------------------------------------------------------------
TemporalFoot foot(bool watching, bool gameplay, bool known, bool vehicleKnown, bool onFoot, bool seated, unsigned age) {
    TemporalFoot f;
    f.watching = watching;
    f.gameplay = gameplay;
    f.known = known;
    f.vehicleKnown = vehicleKnown;
    f.onFoot = onFoot;
    f.seated = seated;
    f.sampleAgeMs = age;
    return f;
}
TemporalFootWhy why(const TemporalFoot& f) { return edvr::temporalFootVerdict(f); }

void testVerdict() {
    // the named scenarios: the commander as Status.json sees him
    const TemporalFoot onFoot = foot(true, true, true, true, true, false, 0);
    check("R1a", why(onFoot) == TemporalFootWhy::OnFoot && edvr::temporalOnFoot(onFoot), "Explorer Cam (Flags2 bit 0, nothing seated, fresh) is on foot");
    const TemporalFoot ship = foot(true, true, true, true, false, true, 0);   // Flags bit 24, Flags2 clear
    check("R1b", why(ship) == TemporalFootWhy::Seated && !edvr::temporalOnFoot(ship), "a ship's cockpit is seated, not on foot");
    const TemporalFoot srv = foot(true, true, true, true, false, true, 100);   // Flags bit 26
    check("R1c", why(srv) == TemporalFootWhy::Seated && !edvr::temporalOnFoot(srv), "an SRV is seated, not on foot");
    check("R1d", why(foot(true, true, false, true, true, false, 0)) == TemporalFootWhy::Unknown &&
                     why(foot(true, true, true, false, true, false, 0)) == TemporalFootWhy::Unknown &&
                     why(foot(true, true, false, false, false, false, 0)) == TemporalFootWhy::Unknown,
          "a Status.json without Flags2 (a menu) or without Flags is unknown, whatever the other fields say");
    check("R1e", why(foot(true, true, true, true, true, false, edvr::kTemporalFootStaleMs)) == TemporalFootWhy::OnFoot &&
                     why(foot(true, true, true, true, true, false, edvr::kTemporalFootStaleMs + 1)) == TemporalFootWhy::Stale &&
                     why(foot(true, true, true, true, true, false, 0x7FFFFFFFu)) == TemporalFootWhy::Stale,
          "the read count's age: the limit itself is fresh, one millisecond past it is stale");
    check("R1f", why(foot(false, true, true, true, true, false, 0)) == TemporalFootWhy::NotWatching &&
                     why(foot(true, false, true, true, true, false, 0)) == TemporalFootWhy::NoGameplay,
          "a journal that is not read cannot say, and a Status.json before this process's LoadGame may be the last session's");
    check("R1g", why(foot(true, true, true, true, true, true, 0)) == TemporalFootWhy::Seated,
          "a sample that says both on foot and seated is read as seated (the cockpit's rule on any doubt)");
    check("R1h", why(foot(true, true, true, true, false, false, 0)) == TemporalFootWhy::NotOnFoot,
          "Flags2 without OnFoot and nothing seated is not on foot");
    // the order of the reasons: each earlier reason hides every later one
    check("R1i",
          why(foot(false, false, false, false, false, true, 0xFFFFFF)) == TemporalFootWhy::NotWatching &&
              why(foot(true, false, false, false, false, true, 0xFFFFFF)) == TemporalFootWhy::NoGameplay &&
              why(foot(true, true, false, false, false, true, 0xFFFFFF)) == TemporalFootWhy::Unknown &&
              why(foot(true, true, true, true, false, true, 0xFFFFFF)) == TemporalFootWhy::Stale &&
              why(foot(true, true, true, true, false, true, 0)) == TemporalFootWhy::Seated &&
              why(foot(true, true, true, true, false, false, 0)) == TemporalFootWhy::NotOnFoot,
          "the order of the reasons: journal off, no LoadGame, unknown, stale, seated, not on foot");
    // the whole table, against the spec written out again: on foot iff every condition holds
    unsigned yes = 0, rows = 0;
    for (unsigned bits = 0; bits < 64; ++bits) {
        for (unsigned age : {0u, edvr::kTemporalFootStaleMs, edvr::kTemporalFootStaleMs + 1}) {
            const bool w = bits & 1, g = bits & 2, k = bits & 4, v = bits & 8, o = bits & 16, s = bits & 32;
            const bool want = w && g && k && v && o && !s && age <= 5000u;
            const TemporalFoot f = foot(w, g, k, v, o, s, age);
            const bool got = edvr::temporalOnFoot(f);
            check("R1j", got == want, fmt("watching %d gameplay %d known %d vehicleKnown %d onFoot %d seated %d age %u: spec %d, got %d", w, g, k, v, o, s, age, want, got));
            check("R1j", (why(f) == TemporalFootWhy::OnFoot) == got, "the verdict and the yes agree");
            ++rows;
            yes += got;
        }
    }
    check("R1j", rows == 192 && yes == 2, fmt("of 192 rows exactly the two fresh on-foot ones (age 0 and the limit) say yes (%u of %u)", yes, rows));
    std::set<std::string> names;
    for (unsigned w = 0; w <= static_cast<unsigned>(TemporalFootWhy::NotOnFoot); ++w) {
        const char* n = edvr::temporalFootWhyName(static_cast<TemporalFootWhy>(w));
        check("R1k", n && n[0] && std::strcmp(n, "?") != 0, "every reason has a name the log prints");
        names.insert(n);
    }
    check("R1k", names.size() == 7, "...and no two reasons share one");
    check("R1l", edvr::kTemporalFootStaleMs == 5000, "the staleness limit is five seconds (ten missed reads at the worker's 500 ms)");
    std::printf("(R1) the verdict: 7 scenarios, the order of the reasons, 192 combinations of its inputs against the spec (%u yes)\n", yes);
}

// The verdict over time: the staleness clock, the gap rule and the changes (TemporalFootTracker, which the pass steps once per eye
// evaluation). Scripted timelines; the clock is the test's.
void testTracker() {
    using edvr::TemporalFootTracker;
    const TemporalFoot yes = foot(true, true, true, true, true, false, 0x7FFFFFFFu);   // the input's own age is the tracker's to replace
    const TemporalFoot ship = foot(true, true, true, true, false, true, 0);
    const unsigned long long T = 1000000;

    {   // R1m: continuous looking at 16 ms, the count never moving: fresh at the limit, stale one millisecond past it
        TemporalFootTracker t;
        check("R1m", t.step(T, yes, 0) && t.on && t.changes == 1 && t.why == TemporalFootWhy::OnFoot,
              "the first question comes on, told as a change, whatever the count says (a count of 0 included) and whatever age the input carries");
        check("R1m", !t.step(T + 16, yes, 0) && t.on && t.changes == 1, "the same answer is no change");
        bool steady = true;
        for (unsigned long long now = T + 32; now <= T + 4992; now += 16) steady = steady && !t.step(now, yes, 0) && t.on;
        check("R1m", steady, "five seconds of looking with a quiet count: still on foot");
        check("R1m", !t.step(T + 5000, yes, 0) && t.on, "the count's age is 5000 ms exactly: fresh");
        check("R1m", t.step(T + 5001, yes, 0) && !t.on && t.why == TemporalFootWhy::Stale && t.changes == 2, "one millisecond more: stale, told as a change");
        check("R1m", !t.step(T + 5017, yes, 0) && !t.on && t.why == TemporalFootWhy::Stale, "and stale it stays across continuous looking");
        check("R1m", t.step(T + 5033, yes, 1) && t.on && t.why == TemporalFootWhy::OnFoot && t.changes == 3, "a count that moves restarts the clock: on foot again");
    }
    {   // R1n: the asker's own stall is not the watcher's silence
        TemporalFootTracker t;
        t.step(T, yes, 7);
        check("R1n", !t.step(T + 8000, yes, 7) && t.on && t.why == TemporalFootWhy::OnFoot, "a gap of eight seconds between two questions restarts the clock: still on foot");
        bool stale = false;
        for (unsigned long long now = T + 8100; now <= T + 8000 + 5200; now += 100) stale = stale || t.step(now, yes, 7);
        check("R1n", stale && !t.on && t.why == TemporalFootWhy::Stale, "...and five more seconds of looking with the same count is stale");
    }
    {   // R1o: the gap rule's edge: 1000 ms is not a gap, 1001 is
        TemporalFootTracker a, b;
        for (TemporalFootTracker* t : {&a, &b}) {
            t->step(T, yes, 3);
            for (unsigned long long now = T + 500; now <= T + 4500; now += 500) t->step(now, yes, 3);
        }
        check("R1o", a.step(T + 5500, yes, 3) && !a.on && a.why == TemporalFootWhy::Stale, "a gap of exactly 1000 ms is not a stall: the count's age is 5500 ms, stale");
        check("R1o", !b.step(T + 5501, yes, 3) && b.on, "a gap of 1001 ms is: the clock restarts and the commander is still on foot");
        check("R1o", edvr::kTemporalFootGapMs == 1000, "the gap is a second");
    }
    {   // R1p: the clock starts at the first question, wherever the tick count is: under a second, or zero
        TemporalFootTracker t;
        check("R1p", t.step(900, yes, 0) && t.on, "a first question at tick 900 comes on");
        bool steady = true;
        for (unsigned long long now = 916; now <= 5892; now += 16) steady = steady && !t.step(now, yes, 0) && t.on;
        check("R1p", steady && !t.step(5900, yes, 0) && t.on, "its clock started at the first question (900), not at tick 0: fresh at 5900, 5000 ms on");
        check("R1p", t.step(5901, yes, 0) && !t.on && t.why == TemporalFootWhy::Stale, "...and stale at 5901");
        TemporalFootTracker z;
        check("R1p", z.step(0, yes, 0) && z.on && z.changes == 1, "a first question at tick 0 comes on");
        check("R1p", !z.step(16, yes, 0) && z.on, "...and is fresh a moment later (an age is never computed backwards)");
    }
    {   // R1q: the count moving every four seconds keeps it fresh for a minute; a count that never moves does not
        TemporalFootTracker t;
        bool fresh = t.step(T, yes, 0);
        unsigned count = 0;
        for (unsigned long long now = T + 100; now <= T + 60000; now += 100) {
            if ((now - T) % 4000 == 0) ++count;
            fresh = fresh && !t.step(now, yes, count) && t.on;
        }
        check("R1q", fresh && t.changes == 1, "a count that moves every four seconds is fresh for a minute");
    }
    {   // R1r: changes are counted and returned, and the verdict's reasons follow the input
        TemporalFootTracker t;
        check("R1r", !t.step(T, ship, 1) && !t.on && t.why == TemporalFootWhy::Seated && t.changes == 0, "a ship is no change from the start, and says why");
        check("R1r", t.step(T + 16, yes, 1) && t.on && t.changes == 1, "on foot: a change");
        check("R1r", t.step(T + 32, ship, 1) && !t.on && t.why == TemporalFootWhy::Seated && t.changes == 2, "boarding: a change, seated");
        check("R1r", !t.step(T + 48, ship, 1) && t.changes == 2, "and no change after it");
        TemporalFoot nobody = yes;
        nobody.watching = false;
        check("R1r", t.step(T + 64, yes, 1) && t.step(T + 80, nobody, 1) && !t.on && t.why == TemporalFootWhy::NotWatching && t.changes == 4, "the journal stopping is a change back, with its reason");
    }
    {   // R1s: a clock that has run for 49 days of continuous looking is stale, not fresh again (the age does not wrap)
        TemporalFootTracker t;
        t.step(T, yes, 0);
        bool wrapped = false;
        const unsigned long long end = T + 0x100000000ull + 3000;   // 2^32 ms and three seconds on
        for (unsigned long long now = T + 1000; now <= end; now += 1000) {
            t.step(now, yes, 0);
            if (now > T + 5001 && t.on) wrapped = true;
        }
        check("R1s", !wrapped && !t.on && t.why == TemporalFootWhy::Stale, "past 2^32 ms of continuous looking the age is clamped, not wrapped to fresh");
    }
    std::printf("(R1) the tracker: the staleness clock at 5000/5001 ms, a stall of ours (a gap over 1000 ms) restarting it, a first question at tick 0, a count that moves, the changes it returns, a 49-day age\n");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// R2 -- the seat, from Status.json's words
// ---------------------------------------------------------------------------------------------------------------------------------
void testSeat() {
    using edvr::journalSeatedFromFlags;
    check("R2a", edvr::kStatusFlagsSeated == 0x07000000u && edvr::kStatusFlags2Seated == 0x6u, "the masks: Flags bits 24-26, Flags2 bits 1-2");
    check("R2b", journalSeatedFromFlags(1u << 24, 0) && journalSeatedFromFlags(1u << 25, 0) && journalSeatedFromFlags(1u << 26, 0),
          "Flags bit 24 (the main ship), 25 (a fighter), 26 (an SRV) each seat the commander");
    check("R2c", journalSeatedFromFlags(0, 1u << 1) && journalSeatedFromFlags(0, 1u << 2),
          "Flags2 bit 1 (a taxi) and bit 2 (someone else's ship) each seat the commander");
    check("R2d", journalSeatedFromFlags(1u << 24, 1) && journalSeatedFromFlags(0, 3) && journalSeatedFromFlags(0, 5),
          "OnFoot beside a seat is seated");
    // the bits beside them, and the rest of both words, seat nobody
    bool beside = true;
    for (unsigned b = 0; b < 32; ++b) {
        const bool seatBit = b == 24 || b == 25 || b == 26;
        beside = beside && (journalSeatedFromFlags(1u << b, 0) == seatBit);
    }
    check("R2e", beside, "of Flags' 32 bits exactly 24, 25 and 26 seat the commander (23 and 27 do not; neither do supercruise 4, the FSD jump 30, landed 1)");
    bool beside2 = true;
    for (unsigned b = 0; b < 32; ++b) {
        const bool seatBit = b == 1 || b == 2;
        beside2 = beside2 && (journalSeatedFromFlags(0, 1u << b) == seatBit);
    }
    check("R2f", beside2, "of Flags2's 32 bits exactly 1 and 2 seat the commander (OnFoot 0, on foot in a station 3, on a planet 4, exterior 15 do not)");
    check("R2g", !journalSeatedFromFlags(0, 0x8011u) && !journalSeatedFromFlags(0x40000010u, 1) && !journalSeatedFromFlags(0, 0),
          "Explorer Cam's own Flags2 (0x8011, measured holding through the whole camera window) and a jump in supercruise seat nobody");
    std::printf("(R2) the seat: Flags bits 24-26 and Flags2 bits 1-2, and no other of 64 bits\n");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// R3 -- the split the shader is given
// ---------------------------------------------------------------------------------------------------------------------------------
void testSplit() {
    using edvr::temporalShipSplitMetres;
    check("R3a", edvr::kTemporalShipMetres == 10.0f, "the configured split's default is ten metres");
    bool bitwise = true;
    for (float configured : {10.0f, 0.0f, 0.5f, 40.0f, 100000.0f, 1e-9f, -1.0f, std::nanf("")})
        bitwise = bitwise && sameBits(temporalShipSplitMetres(configured, false), configured);
    check("R3b", bitwise, "off foot the shader's split is the configured one, bit for bit, whatever it is");
    bool tiny = true;
    for (float configured : {10.0f, 0.5f, 40.0f, 100000.0f})
        tiny = tiny && sameBits(temporalShipSplitMetres(configured, true), edvr::kTemporalOnFootSplitMetres);
    check("R3c", tiny, "on foot the split is the one millimetre, whatever was configured");
    check("R3d", edvr::kTemporalOnFootSplitMetres > 0.0f && edvr::kTemporalOnFootSplitMetres < 0.025f,
          "the on-foot split is positive (zero is the world path OFF: everything would take the head path) and under the 0.025 m near plane");
    check("R3e", temporalShipSplitMetres(0.0f, true) == 0.0f && temporalShipSplitMetres(-5.0f, true) == -5.0f,
          "a player's zero (the world path off) stays zero on foot: the mode never turns on what was turned off");
    check("R3f", temporalShipSplitMetres(10.0f, true) < 10.0f && temporalShipSplitMetres(10.0f, true) > 0.0f, "...and the on-foot value is below any configured one");
    std::printf("(R3) the split: configured bit for bit off foot, %g m on foot, zero stays zero\n", static_cast<double>(edvr::kTemporalOnFootSplitMetres));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The fixture (tools/on_foot_split_fixture.py writes it; the layout is documented there)
// ---------------------------------------------------------------------------------------------------------------------------------
#pragma pack(push, 1)
struct FixCaseHead { char name[24]; uint32_t stamp, frame, eyeW, eyeH, crop[4]; float split; uint32_t rows, patches; };
struct FixRow {
    uint32_t frame;
    float tanNow[4], tanPrev[4], headR[9], headTv[3], camR[9], camTv[3], projA, projB;
    uint32_t worldOn, depthMotion;
};
struct FixPatchHead { uint32_t x0, y0, w, h; };
#pragma pack(pop)
static_assert(sizeof(FixCaseHead) == 68 && sizeof(FixRow) == 148 && sizeof(FixPatchHead) == 16, "the fixture's records");

constexpr int kP = 48;   // a patch's side
struct Patch {
    uint32_t x0 = 0, y0 = 0;                // in the decision crop
    std::vector<float> depth;               // reversed-Z scene depth, kP x kP
    std::vector<uint16_t> flags;            // the dump's decision word
    std::vector<int16_t> motion;            // the dump's motion, 1/256 px, x then y
    std::vector<float> zpred;               // the dump's predicted previous depth
};
struct Case {
    std::string name;
    uint32_t stamp = 0, frame = 0, eyeW = 0, eyeH = 0, crop[4] = {};
    float split = 0.0f;                     // what the dump's shader ran with
    std::vector<FixRow> rows;
    std::vector<Patch> patches;
    const FixRow& row(uint32_t f) const {
        for (const FixRow& r : rows) if (r.frame == f) return r;
        die("R0", fmt("%s has no row for frame %u", name.c_str(), f));
    }
};

std::vector<Case> loadFixture(const std::string& path) {
    const std::string data = slurp(path);
    size_t off = 0;
    auto take = [&](void* dst, size_t n) {
        if (off + n > data.size()) die("R0", "the fixture ends early");
        std::memcpy(dst, data.data() + off, n);
        off += n;
    };
    char magic[8];
    uint32_t version, n;
    take(magic, 8);
    take(&version, 4);
    take(&n, 4);
    if (std::memcmp(magic, "EDVRFOOT", 8) != 0 || version != 1) die("R0", "not an on-foot split fixture of this version");
    std::vector<Case> cases;
    for (uint32_t i = 0; i < n; ++i) {
        FixCaseHead h{};
        take(&h, sizeof h);
        Case c;
        c.name = std::string(h.name, strnlen(h.name, sizeof h.name));
        c.stamp = h.stamp; c.frame = h.frame; c.eyeW = h.eyeW; c.eyeH = h.eyeH; c.split = h.split;
        std::memcpy(c.crop, h.crop, sizeof c.crop);
        for (uint32_t k = 0; k < h.rows; ++k) {
            FixRow r{};
            take(&r, sizeof r);
            c.rows.push_back(r);
        }
        for (uint32_t k = 0; k < h.patches; ++k) {
            FixPatchHead ph{};
            take(&ph, sizeof ph);
            if (ph.w != kP || ph.h != kP) die("R0", "a patch is not 48 x 48");
            Patch p;
            p.x0 = ph.x0; p.y0 = ph.y0;
            p.depth.resize(kP * kP); p.flags.resize(kP * kP); p.motion.resize(kP * kP * 2); p.zpred.resize(kP * kP);
            take(p.depth.data(), p.depth.size() * 4);
            take(p.flags.data(), p.flags.size() * 2);
            take(p.motion.data(), p.motion.size() * 2);
            take(p.zpred.data(), p.zpred.size() * 4);
            c.patches.push_back(std::move(p));
        }
        cases.push_back(std::move(c));
    }
    if (off != data.size()) die("R0", "trailing bytes in the fixture");
    return cases;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The independent arithmetic: one pixel's decision in float64, from the eye's full geometry (not the patch's virtual eye)
// ---------------------------------------------------------------------------------------------------------------------------------
struct Pixel {
    bool isFar = false, world = false, valid = false;   // (far is a macro of windows.h)
    double z = 0.0, mx = 0.0, my = 0.0, zp = 0.0;
    uint32_t path = 0, flags = 0;
};
constexpr uint32_t kFlagBits = 15u | 128u | 256u | 1024u;   // the decision word's bits the paths decide

Pixel expectPixel(const Case& c, const FixRow& r, const std::vector<float>& depth, const Patch& p, int x, int y, double split, bool tvCamOn = true) {
    Pixel o;
    float zr = 0.0f;   // the nearest of the 3x3, clamped to the patch the way the shader's depth tile clamps to the eye
    for (int oy = -1; oy <= 1; ++oy)
        for (int ox = -1; ox <= 1; ++ox)
            zr = std::max(zr, depth[static_cast<size_t>(std::min(std::max(y + oy, 0), kP - 1)) * kP + std::min(std::max(x + ox, 0), kP - 1)]);
    const float own = depth[static_cast<size_t>(y) * kP + x];
    const double den = static_cast<double>(zr) - r.projA;
    o.isFar = zr <= 0.0f || den <= 0.0;
    o.z = o.isFar ? 0.0 : r.projB / den;
    const bool worldOn = r.worldOn != 0 && split > 0.0;
    o.world = worldOn && (o.isFar || o.z > split);
    const double X = c.crop[0] + p.x0 + x, Y = c.crop[1] + p.y0 + y;
    const double W = c.eyeW, H = c.eyeH;
    const double dx = r.tanNow[0] + (X + 0.5) / W * (static_cast<double>(r.tanNow[1]) - r.tanNow[0]);
    const double dy = r.tanNow[3] - (Y + 0.5) / H * (static_cast<double>(r.tanNow[3]) - r.tanNow[2]);
    const double d[3] = {dx, dy, -1.0};
    const float* R = o.world ? r.camR : r.headR;
    double tv[3] = {o.world ? r.camTv[0] : r.headTv[0], o.world ? r.camTv[1] : r.headTv[1], o.world ? r.camTv[2] : r.headTv[2]};
    if (o.world && !tvCamOn) tv[0] = tv[1] = tv[2] = 0.0;
    double dp[3];
    for (int i = 0; i < 3; ++i) {
        dp[i] = static_cast<double>(R[i * 3]) * d[0] + static_cast<double>(R[i * 3 + 1]) * d[1] + static_cast<double>(R[i * 3 + 2]) * d[2];
        if (!o.isFar) dp[i] = dp[i] * o.z + tv[i];
    }
    o.valid = dp[2] < -1e-6;
    const double xt = dp[0] / -dp[2], yt = dp[1] / -dp[2];
    const double px = (xt - r.tanPrev[0]) / (static_cast<double>(r.tanPrev[1]) - r.tanPrev[0]) * W - 0.5;
    const double py = (r.tanPrev[3] - yt) / (static_cast<double>(r.tanPrev[3]) - r.tanPrev[2]) * H - 0.5;
    o.mx = px - X;
    o.my = py - Y;
    o.zp = o.isFar ? 0.0 : -dp[2];
    o.path = o.world ? 2u : 1u;
    o.flags = o.path | (worldOn ? 128u : 0u) | (own > r.projA ? 256u : 0u) | (o.valid ? 1024u : 0u);
    return o;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The production shader on WARP: the mv entry, the capture variant (EDVR_TEMPORAL_TRACE 1: DT at u7 is what the eye dump's D files are)
// ---------------------------------------------------------------------------------------------------------------------------------
struct Params {   // the cbuffer P, as temporal_pass.cpp's PassParams lays it out (528 bytes, 33 rows); checked against the compiled shader
    int32_t region[4];
    int32_t size[2];
    int32_t texSize[2];
    float tanNow[4], tanPrev[4], jit[4], dR0[4], dR1[4], dR2[4];
    float cand[4][3][4];
    float blend, gamma;
    int32_t haveHistory, candMask;
    float knobs[4], tvUsed[4], tvCand[4], tvCam[4], split[4], fovea0[4], fovea1[4], movers[4], probe[4], holoJitter[4], skip[4], lead[4];
};
static_assert(sizeof(Params) == 528, "the cbuffer is 33 16-byte rows");

constexpr UINT kStatsN = 64;
constexpr int kSlotStats = 2, kSlotMV = 3, kSlotDT = 7;
const DXGI_FORMAT kFmt[8] = {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32G32_FLOAT,
                             DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT};
const UINT kBpp[8] = {16, 16, 4, 8, 4, 4, 16, 16};

struct Outputs {
    std::vector<uint8_t> dt, mv, stats;   // DT (motion xy, predicted depth, flags), MV (written, hidden-rejected), Stats
    bool operator==(const Outputs& o) const { return dt == o.dt && mv == o.mv && stats == o.stats; }
    float dtAt(size_t i, int k) const { float f; std::memcpy(&f, &dt[(i * 4 + k) * 4], 4); return f; }
    uint32_t stat(int i) const { uint32_t u; std::memcpy(&u, &stats[static_cast<size_t>(i) * 4], 4); return u; }
};

void hr(HRESULT h, const char* what) { check("R0", SUCCEEDED(h), fmt("%s failed (0x%08X)", what, static_cast<unsigned>(h))); }

struct Rig {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11SamplerState> smp;
    ComPtr<ID3D11Resource> res[8], stage[8];
    ComPtr<ID3D11UnorderedAccessView> uav[8];
    std::vector<uint8_t> fill[8];
    ComPtr<ID3D11ShaderResourceView> zero;   // S and H: the colour is no concern of the decisions
    ComPtr<ID3D11ShaderResourceView> z;      // this patch's depth, set by bindDepth
};

void initRig(Rig& R) {
    D3D_FEATURE_LEVEL level{};
    hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &R.dev, &level, &R.ctx), "D3D11CreateDevice(WARP)");
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(Params);
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr(R.dev->CreateBuffer(&cbd, nullptr, &R.cb), "P constant buffer");
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    hr(R.dev->CreateSamplerState(&sd, &R.smp), "sampler");
    const float sentinel = -123.456f;
    for (int s = 0; s < 8; ++s) {
        if (s == kSlotStats) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kStatsN * 4;
            bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bd.StructureByteStride = 4;
            ComPtr<ID3D11Buffer> b;
            hr(R.dev->CreateBuffer(&bd, nullptr, &b), "Stats buffer");
            hr(R.dev->CreateUnorderedAccessView(b.Get(), nullptr, &R.uav[s]), "Stats UAV");
            R.res[s] = b;
            D3D11_BUFFER_DESC sb = bd;
            sb.Usage = D3D11_USAGE_STAGING;
            sb.BindFlags = 0;
            sb.MiscFlags = 0;
            sb.StructureByteStride = 0;
            sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Buffer> st;
            hr(R.dev->CreateBuffer(&sb, nullptr, &st), "Stats staging");
            R.stage[s] = st;
            R.fill[s].assign(kStatsN * 4, 0);
            continue;
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kP;
        td.Height = kP;
        td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = kFmt[s];
        td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> t;
        hr(R.dev->CreateTexture2D(&td, nullptr, &t), "output texture");
        hr(R.dev->CreateUnorderedAccessView(t.Get(), nullptr, &R.uav[s]), "output UAV");
        R.res[s] = t;
        D3D11_TEXTURE2D_DESC sd2 = td;
        sd2.Usage = D3D11_USAGE_STAGING;
        sd2.BindFlags = 0;
        sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> st;
        hr(R.dev->CreateTexture2D(&sd2, nullptr, &st), "output staging");
        R.stage[s] = st;
        R.fill[s].resize(static_cast<size_t>(kP) * kP * kBpp[s]);
        for (size_t o = 0; o < R.fill[s].size(); o += 4) std::memcpy(&R.fill[s][o], &sentinel, 4);
    }
    {   // colour: zeros
        std::vector<float> zeros(static_cast<size_t>(kP) * kP * 4, 0.0f);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kP;
        td.Height = kP;
        td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{zeros.data(), kP * 16, 0};
        ComPtr<ID3D11Texture2D> t;
        hr(R.dev->CreateTexture2D(&td, &init, &t), "colour texture");
        hr(R.dev->CreateShaderResourceView(t.Get(), nullptr, &R.zero), "colour SRV");
    }
}

void bindDepth(Rig& R, const std::vector<float>& depth) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kP;
    td.Height = kP;
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{depth.data(), kP * 4, 0};
    ComPtr<ID3D11Texture2D> t;
    hr(R.dev->CreateTexture2D(&td, &init, &t), "depth texture");
    R.z.Reset();
    hr(R.dev->CreateShaderResourceView(t.Get(), nullptr, &R.z), "depth SRV");
}

Outputs dispatch(Rig& R, ID3D11ComputeShader* cs, const Params& p) {
    R.ctx->UpdateSubresource(R.cb.Get(), 0, nullptr, &p, 0, 0);
    for (int s = 0; s < 8; ++s) R.ctx->UpdateSubresource(R.res[s].Get(), 0, nullptr, R.fill[s].data(), s == kSlotStats ? 0u : kP * kBpp[s], 0);
    R.ctx->CSSetShader(cs, nullptr, 0);
    R.ctx->CSSetConstantBuffers(0, 1, R.cb.GetAddressOf());
    R.ctx->CSSetSamplers(0, 1, R.smp.GetAddressOf());
    ID3D11ShaderResourceView* srv[16] = {};
    srv[0] = srv[1] = R.zero.Get();
    srv[2] = R.z.Get();
    R.ctx->CSSetShaderResources(0, 16, srv);
    ID3D11UnorderedAccessView* uavs[8];
    for (int s = 0; s < 8; ++s) uavs[s] = R.uav[s].Get();
    R.ctx->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
    R.ctx->Dispatch((kP + 7) / 8, (kP + 7) / 8, 1);
    ID3D11UnorderedAccessView* nulls[8] = {};
    R.ctx->CSSetUnorderedAccessViews(0, 8, nulls, nullptr);
    ID3D11ShaderResourceView* nullSrv[16] = {};
    R.ctx->CSSetShaderResources(0, 16, nullSrv);
    Outputs out;
    for (int s : {kSlotStats, kSlotMV, kSlotDT}) {
        R.ctx->CopyResource(R.stage[s].Get(), R.res[s].Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        hr(R.ctx->Map(R.stage[s].Get(), 0, D3D11_MAP_READ, 0, &m), "map output");
        std::vector<uint8_t>& dst = s == kSlotStats ? out.stats : s == kSlotMV ? out.mv : out.dt;
        if (s == kSlotStats) {
            dst.assign(static_cast<const uint8_t*>(m.pData), static_cast<const uint8_t*>(m.pData) + kStatsN * 4);
        } else {
            const size_t row = static_cast<size_t>(kP) * kBpp[s];
            dst.resize(row * kP);
            for (int y = 0; y < kP; ++y) std::memcpy(dst.data() + row * y, static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(m.RowPitch) * y, row);
        }
        R.ctx->Unmap(R.stage[s].Get(), 0);
    }
    return out;
}

// The patch as a virtual eye: the eye's tangents narrowed to the patch's pixels, so a pixel's direction and its projection last frame
// are the real eye's to float precision (the shader sees a 48 x 48 eye).
void windowTan(const float t[4], const Case& c, const Patch& p, float out[4]) {
    const double X0 = static_cast<double>(c.crop[0]) + p.x0, Y0 = static_cast<double>(c.crop[1]) + p.y0;
    const double W = c.eyeW, H = c.eyeH;
    const double l = t[0], r = t[1], bottom = t[2], top = t[3];
    out[0] = static_cast<float>(l + X0 / W * (r - l));
    out[1] = static_cast<float>(l + (X0 + kP) / W * (r - l));
    out[3] = static_cast<float>(top - Y0 / H * (top - bottom));
    out[2] = static_cast<float>(top - (Y0 + kP) / H * (top - bottom));
}

// The pass's constants for one eye-frame: motion.csv's row, as temporal_pass.cpp builds them (the head's delta in dR and tvUsed, the
// camera rows in candidate 2 and tvCam), the split in split.x, a depth bound, no engine, holo, screen or celestial path.
Params paramsFor(const Case& c, const FixRow& r, const Patch& p, float split, bool tvCamOn = true) {
    Params q{};
    q.region[2] = q.size[0] = q.texSize[0] = kP;
    q.region[3] = q.size[1] = q.texSize[1] = kP;
    windowTan(r.tanNow, c, p, q.tanNow);
    windowTan(r.tanPrev, c, p, q.tanPrev);
    q.jit[2] = 1.0f;
    q.jit[3] = 0.5f;
    for (int i = 0; i < 3; ++i) {
        float* head[3] = {q.dR0, q.dR1, q.dR2};
        for (int k = 0; k < 3; ++k) {
            head[i][k] = r.headR[i * 3 + k];
            q.cand[2][i][k] = r.camR[i * 3 + k];
        }
        q.tvUsed[i] = q.tvCand[i] = r.headTv[i];
        q.tvCam[i] = tvCamOn ? r.camTv[i] : 0.0f;
    }
    q.tvUsed[3] = r.depthMotion ? 1.0f : 0.0f;
    q.tvCam[3] = r.worldOn ? 1.0f : 0.0f;
    q.blend = 0.7f;
    q.gamma = 1.25f;
    q.knobs[0] = r.projA;
    q.knobs[1] = 1.0f;
    q.knobs[2] = r.projB;
    q.knobs[3] = 50000.0f;
    q.split[0] = split;
    q.probe[0] = 1.0f;
    return q;
}

struct Variant {
    std::string name;
    std::string text;
    ComPtr<ID3DBlob> code;
    std::string log;
    HRESULT hr = E_FAIL;
    ComPtr<ID3D11ComputeShader> cs;
};
void compileOne(Variant* v) {
    const D3D_SHADER_MACRO macros[] = {{"EDVR_TEMPORAL_DIAGNOSTICS", "1"}, {"EDVR_TEMPORAL_TRACE", "1"}, {nullptr, nullptr}};   // the build's trace variant
    ComPtr<ID3DBlob> errors;
    v->hr = D3DCompile(v->text.data(), v->text.size(), "temporal", macros, nullptr, "mv", "cs_5_0", 0, 0, v->code.ReleaseAndGetAddressOf(), errors.GetAddressOf());
    if (errors && errors->GetBufferSize()) v->log.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The judge: what the shader must do on the patches. No dying inside: the controls run mutated shaders through the same judge.
// ---------------------------------------------------------------------------------------------------------------------------------
struct Verdict {
    std::string first;   // label and reason of the first thing that went wrong
    bool ok() const { return first.empty(); }
    void bad(const char* label, const std::string& why) { if (first.empty()) first = std::string(label) + ". " + why; }
};
struct Tally {
    size_t dumpChecked = 0, dumpNear = 0;       // R4a: the pixels compared with the dump, and those the dump had on the head path
    size_t portChecked = 0;                     // R4b-d: the pixels compared with the float64 port
    size_t nearHead = 0, shifted = 0;           // R4b: the near ground on foot, and how much of it moved differently from the head path's
    size_t hybrids = 0;                         // R4c: near pixel-frames under the walking rows
    double worstDump = 0.0, worstPort = 0.0, worstZ = 0.0;
    double walkSum = 0.0, biggestWalk = 0.0;    // the head path's miss on the dump's own frame: what the on-foot motion differs from the head path's by
    double biggestHybrid = 0.0;                 // ...and under the walking rows of the dump's other frames
    double tvWorthSum = 0.0;                    // what the camera's translation term is worth on those pixels
    size_t tvWorthOver = 0;                     // ...and on how many it is over half a pixel
};

constexpr double kMotionTol = 0.01;        // px: a WARP float32 against a float64 port, over a quarter of a pixel's span in the worst place
constexpr double kDumpTol = 0.004;         // px: the fixture's 1/256 px quantisation (0.002) and the game GPU's float32 noise (0.0004)

// R4a: the dump's own constants and split must give the dump's own decisions
void judgeDump(Rig& R, ID3D11ComputeShader* cs, const Case& c, Verdict& v, Tally& t) {
    const FixRow& row = c.row(c.frame);
    const char* label = "R4a";
    for (const Patch& p : c.patches) {
        if (!v.ok()) return;
        bindDepth(R, p.depth);
        const Outputs o = dispatch(R, cs, paramsFor(c, row, p, c.split));
        for (int y = 2; y < kP - 2; ++y) {
            for (int x = 2; x < kP - 2; ++x) {
                const size_t i = static_cast<size_t>(y) * kP + x;
                const uint32_t stored = p.flags[i];
                if ((stored & 15u) != 1u && (stored & 15u) != 2u) continue;   // the engine, holo, screen and celestial paths are not this shader run
                ++t.dumpChecked;
                if ((stored & 15u) == 1u) ++t.dumpNear;
                const uint32_t got = static_cast<uint32_t>(o.dtAt(i, 3));
                if ((got & kFlagBits) != (stored & kFlagBits))
                    v.bad(label, fmt("%s patch (%u,%u) pixel (%d,%d): the shader's path and flags %u are not the dump's %u", c.name.c_str(), p.x0, p.y0, x, y, got & kFlagBits, stored & kFlagBits));
                const double mx = p.motion[i * 2] / 256.0, my = p.motion[i * 2 + 1] / 256.0;
                const double e = std::hypot(o.dtAt(i, 0) - mx, o.dtAt(i, 1) - my);
                t.worstDump = std::max(t.worstDump, e);
                if (!(e < kDumpTol))
                    v.bad(label, fmt("%s patch (%u,%u) pixel (%d,%d): motion (%.4f, %.4f) is %.4f px from the dump's (%.4f, %.4f)", c.name.c_str(), p.x0, p.y0, x, y, o.dtAt(i, 0), o.dtAt(i, 1), e, mx, my));
                if (p.zpred[i] > 0.0f) {
                    const double ez = std::fabs(o.dtAt(i, 2) - static_cast<double>(p.zpred[i])) / p.zpred[i];
                    t.worstZ = std::max(t.worstZ, ez);
                    if (!(ez < 2e-4)) v.bad(label, fmt("%s patch (%u,%u) pixel (%d,%d): predicted depth %.5f is not the dump's %.5f", c.name.c_str(), p.x0, p.y0, x, y, o.dtAt(i, 2), p.zpred[i]));
                }
            }
        }
    }
}

// One patch under one row and one split against the float64 port: path and flags exactly, motion and predicted depth within tolerance.
// `expectWorldEverywhere` (on foot): no pixel on the head path.
void judgeAgainstPort(Rig& R, ID3D11ComputeShader* cs, const Case& c, const FixRow& row, const Patch& p, float split, const char* label, bool expectWorldEverywhere,
                      Verdict& v, Tally& t, const char* what) {
    if (!v.ok()) return;
    bindDepth(R, p.depth);
    const Outputs o = dispatch(R, cs, paramsFor(c, row, p, split));
    uint32_t worldCount = 0;
    for (int y = 0; y < kP; ++y) {
        for (int x = 0; x < kP; ++x) {
            const size_t i = static_cast<size_t>(y) * kP + x;
            const Pixel e = expectPixel(c, row, p.depth, p, x, y, split);
            worldCount += e.world ? 1 : 0;
            if (x < 2 || y < 2 || x >= kP - 2 || y >= kP - 2) continue;   // the borders' dilation is the patch's, not the eye's: the port does it too, but only the interior is claimed
            ++t.portChecked;
            const uint32_t got = static_cast<uint32_t>(o.dtAt(i, 3));
            if ((got & kFlagBits) != e.flags)
                v.bad(label, fmt("%s %s patch (%u,%u) pixel (%d,%d): path and flags %u, the arithmetic says %u", what, c.name.c_str(), p.x0, p.y0, x, y, got & kFlagBits, e.flags));
            if (expectWorldEverywhere && (got & 15u) != 2u)
                v.bad(label, fmt("%s %s patch (%u,%u) pixel (%d,%d): on foot the pixel took path %u, not the world path (depth %.2f m)", what, c.name.c_str(), p.x0, p.y0, x, y, got & 15u, e.z));
            const double em = std::hypot(o.dtAt(i, 0) - e.mx, o.dtAt(i, 1) - e.my);
            t.worstPort = std::max(t.worstPort, em);
            if (!(em < kMotionTol))
                v.bad(label, fmt("%s %s patch (%u,%u) pixel (%d,%d): motion (%.4f, %.4f) is %.4f px from the arithmetic's (%.4f, %.4f)", what, c.name.c_str(), p.x0, p.y0, x, y, o.dtAt(i, 0), o.dtAt(i, 1), em, e.mx, e.my));
            if (e.zp > 0.0) {
                const double ez = std::fabs(o.dtAt(i, 2) - e.zp) / e.zp;
                t.worstZ = std::max(t.worstZ, ez);
                if (!(ez < 2e-4)) v.bad(label, fmt("%s %s patch (%u,%u) pixel (%d,%d): predicted depth %.5f, the arithmetic says %.5f", what, c.name.c_str(), p.x0, p.y0, x, y, o.dtAt(i, 2), e.zp));
            }
        }
    }
    // the diagnostic counter the registration line prints: Stats[15] is every pixel of the dispatch that took the world path
    if (o.stat(15) != worldCount)
        v.bad("R4e", fmt("%s %s patch (%u,%u): Stats[15] counts %u world-path pixels, the arithmetic %u", what, c.name.c_str(), p.x0, p.y0, o.stat(15), worldCount));
}

// R4b: on foot, the Steam dump's own frame
void judgeOnFoot(Rig& R, ID3D11ComputeShader* cs, const Case& c, Verdict& v, Tally& t) {
    const FixRow& row = c.row(c.frame);
    const float split = edvr::temporalShipSplitMetres(edvr::kTemporalShipMetres, true);
    for (const Patch& p : c.patches) {
        judgeAgainstPort(R, cs, c, row, p, split, "R4b", true, v, t, "on foot:");
        if (!v.ok()) return;
        // what the head path would have written on the pixels the dump had there (path 1), and what the camera's translation is worth
        bindDepth(R, p.depth);
        const Outputs foot = dispatch(R, cs, paramsFor(c, row, p, split));
        const Outputs noTv = dispatch(R, cs, paramsFor(c, row, p, split, false));
        for (int y = 2; y < kP - 2; ++y) {
            for (int x = 2; x < kP - 2; ++x) {
                const size_t i = static_cast<size_t>(y) * kP + x;
                if ((p.flags[i] & 15u) != 1u) continue;
                const double mxDump = p.motion[i * 2] / 256.0, myDump = p.motion[i * 2 + 1] / 256.0;
                const double shift = std::hypot(foot.dtAt(i, 0) - mxDump, foot.dtAt(i, 1) - myDump);
                const double tvWorth = std::hypot(foot.dtAt(i, 0) - noTv.dtAt(i, 0), foot.dtAt(i, 1) - noTv.dtAt(i, 1));
                ++t.nearHead;
                t.walkSum += shift;
                t.biggestWalk = std::max(t.biggestWalk, shift);
                if (shift > 2.0) ++t.shifted;
                // the camera rows' translation is in the motion: without tvCam the same pixel moves differently
                t.tvWorthSum += tvWorth;
                if (tvWorth > 0.5) ++t.tvWorthOver;
            }
        }
    }
}

// R4c: the Steam depth under the walking rows of the dump's other frames, mode off and on, against the port
void judgeWalking(Rig& R, ID3D11ComputeShader* cs, const Case& c, Verdict& v, Tally& t) {
    const float on = edvr::temporalShipSplitMetres(edvr::kTemporalShipMetres, true);
    for (const FixRow& row : c.rows) {
        if (row.frame == c.frame) continue;
        for (const Patch& p : c.patches) {
            if (!v.ok()) return;
            judgeAgainstPort(R, cs, c, row, p, c.split, "R4c", false, v, t, "hybrid, mode off:");
            judgeAgainstPort(R, cs, c, row, p, on, "R4c", true, v, t, "hybrid, on foot:");
            // what the head path misses: the same near pixels under the two splits
            bindDepth(R, p.depth);
            const Outputs off = dispatch(R, cs, paramsFor(c, row, p, c.split));
            const Outputs foot = dispatch(R, cs, paramsFor(c, row, p, on));
            for (int y = 2; y < kP - 2; ++y)
                for (int x = 2; x < kP - 2; ++x) {
                    const size_t i = static_cast<size_t>(y) * kP + x;
                    if ((static_cast<uint32_t>(off.dtAt(i, 3)) & 15u) != 1u) continue;
                    ++t.hybrids;
                    t.biggestHybrid = std::max(t.biggestHybrid, static_cast<double>(std::hypot(foot.dtAt(i, 0) - off.dtAt(i, 0), foot.dtAt(i, 1) - off.dtAt(i, 1))));
                }
        }
    }
}

// R4d: the cockpit
void judgeCockpit(Rig& R, ID3D11ComputeShader* cs, const Case& cockpit, const Case& steam, Verdict& v, Tally& t) {
    const FixRow& row = cockpit.row(cockpit.frame);
    // the configured split under every state that is not on foot is the dump's split, bit for bit, and the outputs byte for byte
    const float seatedSplit = edvr::temporalShipSplitMetres(edvr::kTemporalShipMetres, false);
    if (!sameBits(seatedSplit, cockpit.split)) v.bad("R4d", "the split a cockpit gets is not the one the dump ran with");
    for (const Patch& p : cockpit.patches) {
        if (!v.ok()) return;
        bindDepth(R, p.depth);
        const Outputs dump = dispatch(R, cs, paramsFor(cockpit, row, p, cockpit.split));
        const Outputs seated = dispatch(R, cs, paramsFor(cockpit, row, p, seatedSplit));
        if (!(dump == seated)) v.bad("R4d", fmt("cockpit patch (%u,%u): the outputs under the seated split are not byte for byte the dump's split's", p.x0, p.y0));
        // a stale on-foot word in a cockpit, for the record: its near pixels would leave the head path
        const Outputs wrong = dispatch(R, cs, paramsFor(cockpit, row, p, edvr::temporalShipSplitMetres(edvr::kTemporalShipMetres, true)));
        for (size_t i = 0; i < static_cast<size_t>(kP) * kP; ++i)
            if ((p.flags[i] & 15u) == 1u && (static_cast<uint32_t>(wrong.dtAt(i, 3)) & 15u) == 1u && i % kP > 2 && i % kP < kP - 3 && i / kP > 2 && i / kP < kP - 3)
                v.bad("R4d", fmt("cockpit patch (%u,%u): a pixel the dump had on the head path stays there under the on-foot split (it should not: the split is what keeps it)", p.x0, p.y0));
    }
    // the ship rides with the head: the cockpit's depth under the walking rows of the Steam dump (a camera moving half a metre a frame
    // against a head that does not) keeps its near pixels on the head path, with the head's motion, while the split is the configured one
    for (const FixRow& wr : steam.rows) {
        Case walk = cockpit;
        FixRow r2 = wr;
        for (const Patch& p : cockpit.patches) {
            if (!v.ok()) return;
            judgeAgainstPort(R, cs, walk, r2, p, cockpit.split, "R4d", false, v, t, "cockpit under walking rows:");
            bindDepth(R, p.depth);
            const Outputs o = dispatch(R, cs, paramsFor(walk, r2, p, cockpit.split));
            for (int y = 2; y < kP - 2; ++y)
                for (int x = 2; x < kP - 2; ++x) {
                    const size_t i = static_cast<size_t>(y) * kP + x;
                    const Pixel e = expectPixel(walk, r2, p.depth, p, x, y, cockpit.split);
                    if (!e.isFar && e.z <= cockpit.split && (static_cast<uint32_t>(o.dtAt(i, 3)) & 15u) != 1u)
                        v.bad("R4d", fmt("cockpit under walking rows: a pixel %.2f m away left the head path with the split at %.0f m", e.z, static_cast<double>(cockpit.split)));
                }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// R4 and R6: the production shader, then six broken ones
// ---------------------------------------------------------------------------------------------------------------------------------
Verdict judgeAll(Rig& R, ID3D11ComputeShader* cs, const Case& steam, const Case& cockpit, Tally& tSteam, Tally& tCockpit) {
    Verdict v;
    judgeDump(R, cs, steam, v, tSteam);
    judgeDump(R, cs, cockpit, v, tCockpit);
    judgeOnFoot(R, cs, steam, v, tSteam);
    judgeWalking(R, cs, steam, v, tSteam);
    judgeCockpit(R, cs, cockpit, steam, v, tCockpit);
    return v;
}

void testShader(const std::string& root) {
    const std::vector<Case> cases = loadFixture(root + "/tools/on_foot_split_test/fixture_onfoot.bin");
    const Case* steam = nullptr;
    const Case* cockpit = nullptr;
    for (const Case& c : cases) {
        if (c.name == "steam") steam = &c;
        if (c.name == "frontier-cockpit") cockpit = &c;
    }
    check("R0", steam && cockpit, "the fixture holds the Steam and the Frontier cockpit cases");
    check("R0", steam->rows.size() == 16 && steam->patches.size() == 8 && cockpit->patches.size() == 6, "the fixture's rows and patches");

    // the production text and six broken ones, compiled concurrently
    struct Mutant { const char* name; const char* anchor; const char* mutated; };
    const Mutant ms[] = {
        {"the world path's gate demands more than a millimetre", "bool worldOn = tvCam.w != 0.0 && split.x > 0.0;", "bool worldOn = tvCam.w != 0.0 && split.x > 1.0;"},
        {"the split's comparison ignores the split's value", "if (worldOn && (far || z > split.x) && !scannerUi) {\n                count15 = 1;", "if (worldOn && (far || z > 10.0) && !scannerUi) {\n                count15 = 1;"},
        {"no pixel with a depth takes the world path", "if (worldOn && (far || z > split.x) && !scannerUi) {\n                count15 = 1;", "if (worldOn && (far) && !scannerUi) {\n                count15 = 1;"},
        {"the camera's translation dropped", "dp = dp * z + tvCam.xyz;\n                    zPred = -dp.z;\n#if EDVR_CELESTIAL\n                    float3 celestialDp;\n                    if (celestialPixel(p, d, z, celestialDp)) {\n                        dp = celestialDp;\n                        zPred = -dp.z;\n                        count39 = 1;",
         "dp = dp * z;\n                    zPred = -dp.z;\n#if EDVR_CELESTIAL\n                    float3 celestialDp;\n                    if (celestialPixel(p, d, z, celestialDp)) {\n                        dp = celestialDp;\n                        zPred = -dp.z;\n                        count39 = 1;"},
        {"the head's translation dropped", "dp = dp * z + tvUsed.xyz;\n                zPred = -dp.z;", "dp = dp * z;\n                zPred = -dp.z;"},
        {"the world path's decision code left at the head's", "decisionPath = count39 != 0 ? 12u : (count15 != 0 ? 2u : 1u);", "decisionPath = count39 != 0 ? 12u : (count15 != 0 ? 1u : 1u);"},
    };
    constexpr size_t kM = sizeof(ms) / sizeof(ms[0]);
    std::vector<Variant> v(1 + kM);
    v[0].name = "production";
    v[0].text = edvr::kTemporalCsHlsl;
    for (size_t k = 0; k < kM; ++k) {
        v[1 + k].name = ms[k].name;
        v[1 + k].text = edvr::kTemporalCsHlsl;
        const size_t n = count(v[1 + k].text, ms[k].anchor);
        check("R6", n == 1, fmt("control '%s': its anchor is in the shader exactly once (%zu)", ms[k].name, n));
        v[1 + k].text.replace(v[1 + k].text.find(ms[k].anchor), std::strlen(ms[k].anchor), ms[k].mutated);
    }
    {
        std::vector<std::thread> threads;
        for (Variant& x : v) threads.emplace_back(compileOne, &x);
        for (std::thread& t : threads) t.join();
    }
    for (const Variant& x : v) check("R0", SUCCEEDED(x.hr), fmt("the shader text compiles (%s): %s", x.name.c_str(), x.log.c_str()));

    // the cbuffer is the one PassParams writes
    {
        ComPtr<ID3D11ShaderReflection> refl;
        hr(D3DReflect(v[0].code->GetBufferPointer(), v[0].code->GetBufferSize(), __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(refl.GetAddressOf())), "D3DReflect");
        ID3D11ShaderReflectionConstantBuffer* cbr = refl->GetConstantBufferByName("P");
        D3D11_SHADER_BUFFER_DESC bd{};
        check("R0", cbr && SUCCEEDED(cbr->GetDesc(&bd)) && bd.Size == sizeof(Params), "cbuffer P is 528 bytes, as PassParams");
        std::map<std::string, UINT> off;
        for (UINT i = 0; i < bd.Variables; ++i) {
            D3D11_SHADER_VARIABLE_DESC vd{};
            cbr->GetVariableByIndex(i)->GetDesc(&vd);
            off[vd.Name] = vd.StartOffset;
        }
        check("R0", off["tanNow"] == offsetof(Params, tanNow) && off["tanPrev"] == offsetof(Params, tanPrev) && off["dR0"] == offsetof(Params, dR0) &&
                        off["c2R0"] == offsetof(Params, cand) + 2 * 48 && off["knobs"] == offsetof(Params, knobs) && off["tvUsed"] == offsetof(Params, tvUsed) &&
                        off["tvCam"] == offsetof(Params, tvCam) && off["split"] == offsetof(Params, split) && off["probe"] == offsetof(Params, probe),
              "the cbuffer's fields sit where PassParams puts them (the ones this rig sets)");
    }
    Rig R;
    initRig(R);
    for (Variant& x : v) hr(R.dev->CreateComputeShader(x.code->GetBufferPointer(), x.code->GetBufferSize(), nullptr, &x.cs), "CreateComputeShader");

    Tally ts, tc;
    const Verdict prod = judgeAll(R, v[0].cs.Get(), *steam, *cockpit, ts, tc);
    check("R4", prod.ok(), prod.first);
    check("R4a", ts.dumpChecked > 14000 && tc.dumpChecked > 11000, fmt("the dumps' pixels reproduced: Steam %zu, cockpit %zu", ts.dumpChecked, tc.dumpChecked));
    check("R4a", ts.dumpNear > 6000 && tc.dumpNear > 4500, fmt("...of them on the head path: Steam %zu, cockpit %zu", ts.dumpNear, tc.dumpNear));
    check("R4b", ts.nearHead > 6000, fmt("the Steam near ground the dump had on the head path, now run on foot: %zu pixels", ts.nearHead));
    const double meanWalk = ts.nearHead ? ts.walkSum / static_cast<double>(ts.nearHead) : 0.0;
    check("R4b", ts.shifted > ts.nearHead * 6 / 10 && meanWalk > 3.0, fmt("on foot most of the near ground moves visibly differently from the head path's (%zu of %zu over 2 px, mean %.1f px)", ts.shifted, ts.nearHead, meanWalk));
    check("R4b", ts.tvWorthOver > ts.nearHead * 9 / 10 && ts.tvWorthSum / static_cast<double>(ts.nearHead) > 3.0,
          fmt("the camera rows' translation is in the motion: dropping it moves %zu of %zu pixels, %.1f px on average", ts.tvWorthOver, ts.nearHead, ts.tvWorthSum / static_cast<double>(ts.nearHead)));
    check("R4c", ts.hybrids > 80000 && ts.biggestHybrid > 30.0, fmt("the walking rows: %zu near pixel-frames, the head path misses up to %.1f px", ts.hybrids, ts.biggestHybrid));
    std::printf("(R4a) the dump's own split reproduces the game's decisions: Steam %zu pixels (%zu on the head path), cockpit %zu (%zu on it); motion worst %.4f px against the dump, "
                "predicted depth worst %.1e\n", ts.dumpChecked, ts.dumpNear, tc.dumpChecked, tc.dumpNear, std::max(ts.worstDump, tc.worstDump), std::max(ts.worstZ, tc.worstZ));
    std::printf("(R4b) on foot, the Steam ground the dump had on the head path (%zu pixels): all on the world path, motion = the camera rows with their translation (worst %.4f px against the "
                "float64 port); the walk the head path dropped on this frame: mean %.1f px, up to %.1f px, %zu pixels over 2 px\n", ts.nearHead, ts.worstPort, meanWalk, ts.biggestWalk, ts.shifted);
    std::printf("(R4c,d) %zu hybrid near pixel-frames (the dump's depth, the rows of its other 15 frames; the head path misses up to %.1f px) and the cockpit under the same rows: path, flags, motion and "
                "depth as the arithmetic (%zu Steam and %zu cockpit pixel-frames)\n", ts.hybrids, ts.biggestHybrid, ts.portChecked, tc.portChecked);

    // R6: the same judge over each broken shader
    for (size_t k = 0; k < kM; ++k) {
        Tally a, b;
        const Verdict bad = judgeAll(R, v[1 + k].cs.Get(), *steam, *cockpit, a, b);
        check("R6", !bad.ok(), fmt("control '%s' is caught (the judge found nothing wrong with a broken shader)", ms[k].name));
        std::printf("    control '%s': caught -- %s\n", ms[k].name, bad.first.substr(0, 150).c_str());
    }
    std::printf("(R6) six broken shaders -- the gate, the comparison, the far plane only, the camera's translation, the head's, the decision code -- each refused by the judge the production shader passed\n");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// R5 -- the source pins, each with its control
// ---------------------------------------------------------------------------------------------------------------------------------
struct Pin { const std::string* text; std::string needle; size_t want; const char* label; const char* what; };

void runPins(const std::vector<Pin>& pins) {
    for (const Pin& p : pins) {
        const size_t n = count(*p.text, p.needle);
        check(p.label, n == p.want, fmt("%s: %zu occurrence(s) of the line, %zu expected", p.what, n, p.want));
        // the control: the same text with the line removed (or, for a line that must be absent, put back) no longer satisfies the pin
        std::string cut = *p.text;
        if (p.want == 0) {
            cut += "\n" + p.needle + "\n";
        } else {
            for (size_t at = cut.find(p.needle); at != std::string::npos; at = cut.find(p.needle)) cut.erase(at, p.needle.size());
        }
        check(p.label, count(cut, p.needle) != p.want, fmt("%s: its control still passes", p.what));
    }
}

void testPins(const std::string& root) {
    const std::string pass = slurp(root + "/src/d3d11/temporal_pass.cpp");
    const std::string vscreen = slurp(root + "/src/d3d11/vscreen.cpp");
    const std::string journal = slurp(root + "/src/d3d11/journal_watch.cpp");
    const std::string shader = slurp(root + "/src/d3d11/temporal_shader_source.h");
    const std::vector<Pin> pins = {
        {&pass, "p.split[0] = temporalShipSplitMetres(g_shipMetres, footSplit);", 1, "R5a", "the shader's split is the pure function's, once"},
        {&pass, "p.split[0] = g_shipMetres;", 0, "R5b", "nothing hands the shader the configured split raw"},
        {&pass, "const bool footSplit = footSplitNow();", 1, "R5c", "the mode is asked once, where the constants are built"},
        {&pass, "bool footSplitNow() {", 1, "R5d", "the mode has one definition"},
        {&pass, "f.watching = journalWatchActive();", 1, "R5e", "the verdict's input: the journal is read"},
        {&pass, "f.gameplay = journalGameplay();", 1, "R5e", "the verdict's input: LoadGame"},
        {&pass, "f.known = journalOnFootKnown();", 1, "R5e", "the verdict's input: Flags2 is in the file"},
        {&pass, "f.onFoot = journalOnFoot();", 1, "R5e", "the verdict's input: Flags2 bit 0"},
        {&pass, "f.vehicleKnown = journalSeatedKnown();", 1, "R5e", "the verdict's input: Flags is in the file"},
        {&pass, "f.seated = journalSeated();", 1, "R5e", "the verdict's input: the seat"},
        {&pass, "if (g_foot.step(GetTickCount64(), f, journalStatusSamples())) {", 1, "R5f", "the verdict over time is the tracker's, stepped once, with the watcher's read count"},
        {&pass, "TemporalFootTracker g_foot;", 1, "R5f", "the pass holds one tracker"},
        {&pass, "return g_foot.on;", 1, "R5f", "the mode is the tracker's"},
        {&pass, "\"temporal aa: on foot -- the ship split is off, near pixels take the world path (the game's camera rows, \"", 1, "R5g", "the change to on foot is told, in the words the flight looks for"},
        {&pass, "\"temporal aa: no longer on foot (%s) -- the ship split is back at %.0f m: pixels nearer take the head's \"", 1, "R5g", "the change back is told"},
        {&pass, "\"temporal aa on foot: the ship split was off for %u of %u eye-frames%s (the world path on for %u of them); the journal \"", 1, "R5h", "the interval's totals line, with its zeros"},
        {&vscreen, "temporalPassNoteFootTotals();", 1, "R5h", "the totals block prints the on-foot line beside the others"},
        {&pass, "fprintf(f, \",shipSplit\");", 1, "R5i", "the eye dump's motion trace names the split the shader read"},
        {&journal, "s.pub.seated = sawFlags && journalSeatedFromFlags(flags, sawFlags2 ? flags2 : 0u);", 1, "R5j", "the seat is the pure function of the same read"},
        {&journal, "s.pub.seatedKnown = sawFlags;", 1, "R5j", "the seat is known whenever Flags is"},
        {&journal, "s.pub.seatedKnown = false;", 1, "R5j", "three missed reads drop the seat with the rest"},
        // the shader's reading of split.x, which a millimetre relies on: > 0 turns the world path on, z > split picks a pixel
        {&shader, "split.x > 0.0", 3, "R5k", "the shader reads split.x > 0 as the world path being available (the history fetch, the motion entry, the flag)"},
        {&shader, "(far || z > split.x)", 2, "R5k", "the shader sends a pixel to the world path when it is far or farther than split.x"},
    };
    runPins(pins);
    check("R5l", count(pass, "float    g_shipMetres = kTemporalShipMetres;") == 1 && count(pass, "g_shipMetres = ship;") == 1,
          "the configured split is still read once from advanced.temporal_aa_ship_metres");
    std::printf("(R5) %zu source pins, each with its control (the line removed must fail it)\n", pins.size());
}

}  // namespace

int main(int argc, char** argv) {
    std::string root = ".";
    bool dry = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0) dry = true;
        else if (std::strcmp(argv[i], "--self-test") == 0) {}
        else root = argv[i];
    }
    if (dry) {
        std::printf("on_foot_split_test: dry run -- would check the verdict, the seat, the split, the production shader on WARP over %s/tools/on_foot_split_test/fixture_onfoot.bin, "
                    "and the source pins; it writes no file\n", root.c_str());
        return 0;
    }
    testVerdict();
    testTracker();
    testSeat();
    testSplit();
    testPins(root);
    testShader(root);
    std::printf("PASS: on_foot_split_test, %u checks\n", g_checks);
    return 0;
}

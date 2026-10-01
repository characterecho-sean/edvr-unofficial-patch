// The VR camera census's rig (src/d3d11/vr_camera_census_core.h, vr_camera_census.cpp, and the observe-only mode of
// flat_camera_inject.cpp; design doc section 82, "Pre-build findings and the stop").
//
// What runs here is the pure half the DLL compiles (the decisions, the tables, the budget, every line's text), checked
// against the game-camera model the rest of the camera work is checked against (tools\c2_derive_test), plus SOURCE PINS
// for what no rig can run because it needs the game: that with the key off every entry point returns before touching
// anything, that the detour's observe branch ends the call ahead of every write, and that the only store observe mode makes
// is the return slot. Each pin is counted against the same text with the needle removed, so the scan is known to be able to
// fail. A last group holds the log lines the DLL writes to the lines tools\edvr_log.py's --camera-census fixture carries.
//
// --self-test runs everything and prints "vr camera census: PASS" only when every check holds.
// --print-lines prints the golden lines (what the reader's fixture holds). --dry-run runs nothing.
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../c2_derive_test/c2_derive_model.h"
#include "../../src/d3d11/flat_camera_phase.h"
#include "../../src/d3d11/vr_camera_census_core.h"
#include "../../src/d3d11/vr_world_route_math.h"   // the route's REAL 5 s line formatters: the fixture's route lines are theirs, so a drift fails here

namespace {

using namespace edvr;

int g_failures = 0;
void check(bool ok, const char* what) {
    if (ok) { std::printf("  ok    %s\n", what); return; }
    ++g_failures;
    std::printf("  FAIL  %s\n", what);
}
bool within(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

std::string slurp(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
unsigned count(const std::string& text, const std::string& needle) {
    unsigned n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}
// The text of the function whose definition line starts with `head`, up to the line "}" that closes it at column 0.
std::string functionBody(const std::string& text, const std::string& head) {
    const size_t at = text.find(head);
    if (at == std::string::npos) return std::string();
    const size_t end = text.find("\n}\n", at);
    return end == std::string::npos ? std::string() : text.substr(at, end + 3 - at);
}
// True when `body` (a functionBody starting with `head`) runs `statements` first, straight after the opening brace.
bool startsWith(const std::string& body, const std::string& head, const std::string& statements) {
    return !body.empty() && body.compare(0, head.size(), head) == 0 && body.compare(head.size(), statements.size(), statements) == 0;
}

// A scripted camera in the game's own layout: the derive model's struct, derived, so its projection and rows are the ones
// the game's functions would leave.
struct Model {
    c2derive::Cam cam;
    Model(uint32_t kind, float fov, float aspect, float bx, float by, float nearZ = 0.025f, float farZ = 50000.0f) {
        c2derive::makeCamera(cam);
        c2derive::camU(cam, c2derive::kCamKind) = kind;
        c2derive::camF(cam, c2derive::kCamAngular) = fov;
        c2derive::camF(cam, 0x260) = aspect;
        c2derive::camF(cam, c2derive::kCamBoundX) = bx;
        c2derive::camF(cam, c2derive::kCamBoundY) = by;
        c2derive::camF(cam, c2derive::kCamNear) = nearZ;
        c2derive::camF(cam, c2derive::kCamFar) = farZ;
        c2derive::derive(cam);
    }
    VrCensusSnap snap() const {
        VrCensusSnap s;
        std::memcpy(s.bytes, cam.b + kVrCensusSnapFrom, kVrCensusSnapBytes);
        return s;
    }
};

// ---------------------------------------------------------------------------
// The key and the decision to run.
// ---------------------------------------------------------------------------
void testKey() {
    std::printf("key\n");
    bool on = true, off = true;
    for (const char* t : {"on", "ON", "On", "oN"}) on &= vrCameraCensusKeyFromText(t) == VrCameraCensusKey::On;
    for (const char* t : {"off", "", "onn", "o", "yes", "1", "true", "auto", " on", "on "}) off &= vrCameraCensusKeyFromText(t) == VrCameraCensusKey::Off;
    check(on, "'on' in any case reads as on");
    check(off && vrCameraCensusKeyFromText(nullptr) == VrCameraCensusKey::Off,
          "everything else reads as off: absent, empty, a typo, 'yes', '1', 'auto', stray spaces (a typo never installs a hook)");
    check(vrCameraCensusWantedFor(true, VrCameraCensusKey::On) && !vrCameraCensusWantedFor(false, VrCameraCensusKey::On) &&
          !vrCameraCensusWantedFor(true, VrCameraCensusKey::Off) && !vrCameraCensusWantedFor(false, VrCameraCensusKey::Off),
          "the census runs only with the key on AND the VR profile: the flat profile never does, key or no key");
    auto ignoresProfile = [](bool, VrCameraCensusKey key) { return key == VrCameraCensusKey::On; };
    check(ignoresProfile(false, VrCameraCensusKey::On) && !vrCameraCensusWantedFor(false, VrCameraCensusKey::On),
          "control: a decision that ignored the profile would run in flat, and the row above sees it");
    check(std::strcmp(vrCameraCensusKeyName(VrCameraCensusKey::On), "on") == 0 &&
          std::strcmp(vrCameraCensusKeyName(VrCameraCensusKey::Off), "off") == 0, "the key's names are the ini's words");
}

// ---------------------------------------------------------------------------
// The camera struct's layout against the model, and the rows the composer writes.
// ---------------------------------------------------------------------------
void testLayout(const std::string& injectCpp) {
    std::printf("layout\n");
    check(kVrCensusAxes == c2derive::kCamAxes && kVrCensusProj == 0x1D0 && kVrCensusFlags == c2derive::kCamFlags &&
          kVrCensusNear == c2derive::kCamNear && kVrCensusFar == c2derive::kCamFar && kVrCensusKind == c2derive::kCamKind &&
          kVrCensusFov == c2derive::kCamAngular && kVrCensusBoundX == c2derive::kCamBoundX &&
          kVrCensusBoundY == c2derive::kCamBoundY && kVrCensusViewportW == c2derive::kCamViewportW &&
          kVrCensusViewportH == c2derive::kCamViewportH && kVrCensusFlagProj == c2derive::kFlagProj,
          "the census's camera offsets are the model's typed table (+0x250 flags, +0x254 near, +0x258 far, +0x264 kind, +0x280 fov, +0x28C bound, +0x2A0 viewport)");
    // The projection block: the model's projSlot(c, i) is helper +0x1B0 + 4 i, camera +0x1D0 + 4 i. The aspect has no named constant; the
    // model's own camera maker writes it at +0x260 and the projection builder reads it there.
    Model m(3, 1.1f, 1.6f, 0.0f, 0.0f);
    const VrCensusSnap s = m.snap();
    check(s.f(kVrCensusAspect) == 1.6f && s.f(kVrCensusFov) == 1.1f && s.u(kVrCensusKind) == 3 &&
          s.f(kVrCensusProj) == *c2derive::projSlot(m.cam, 0) && s.f(kVrCensusProj + 4 * 5) == *c2derive::projSlot(m.cam, 5),
          "a model camera read through the census's snapshot gives its own aspect, fov, kind and projection terms");
    check(kVrCensusSnapFrom + kVrCensusSnapBytes > kVrCensusViewportH + 4 && kVrCensusSnapFrom <= kVrCensusAxes &&
          kVrCensusSnapFrom + kVrCensusSnapBytes > kVrCensusProj + 64,
          "one snapshot of the camera covers every field the census reads");
    // The injector's own constants, read from its source: the kind and the bound pair are the same addresses.
    check(injectCpp.find("constexpr uint32_t kCamKind = 0x264;") != std::string::npos &&
          injectCpp.find("constexpr uint32_t kCamBoundX = 0x28C;") != std::string::npos &&
          injectCpp.find("constexpr uint32_t kCamBoundY = 0x290;") != std::string::npos &&
          injectCpp.find("constexpr uint32_t kCamFlags = 0x250;") != std::string::npos,
          "the injector's kind, bound pair and flag word are the addresses the census reads");
}

void testComposeRows() {
    std::printf("composed rows\n");
    struct Case { uint32_t kind; float fov, aspect, bx, by, nearZ, farZ; };
    const Case cases[] = {
        {3, 1.1f, 1.6f, 0.0f, 0.0f, 0.025f, 50000.0f},          // the flat world camera's shape
        {3, 1.0122f, 5040.0f / 2835.0f, 0.0f, 0.0f, 0.025f, 50000.0f},
        {3, 1.3f, 0.9f, -0.03f, 0.011f, 0.0675f, 50000.0f},     // an asymmetric HMD-eye shape
        {3, 1.2f, 0.93f, 0.0021f, -0.0004f, 0.05f, 20000.0f},
        {0, 0.9f, 1.5f, 0.0f, 0.0f, 0.5f, 100000.0f},
        {0, 1.4f, 1.0f, 0.02f, -0.02f, 0.2f, 5000.0f},
    };
    unsigned exact = 0, total = 0;
    for (const Case& k : cases) {
        Model m(k.kind, k.fov, k.aspect, k.bx, k.by, k.nearZ, k.farZ);
        // Turn the camera: the rows carry the rotation, so a sweep of yaws and pitches moves every term.
        for (int step = 0; step < 4; ++step) {
            float* a = &c2derive::camF(m.cam, c2derive::kCamAxes);
            const float yaw = 0.35f + 0.6f * step, pitch = -0.12f + 0.2f * step;
            const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
            a[0] = cy; a[1] = 0.0f; a[2] = -sy; a[3] = 0.0f;
            a[4] = sy * sp; a[5] = cp; a[6] = cy * sp; a[7] = 0.0f;
            a[8] = sy * cp; a[9] = -sp; a[10] = cy * cp;
            c2derive::camU(m.cam, c2derive::kCamFlags) |= c2derive::kFlagProj | c2derive::kFlagVP;
            c2derive::derive(m.cam);
            float want[16], got[16];
            c2derive::composeSceneCb(m.cam, want);
            const bool ok = vrCensusComposeRows(m.snap(), got);
            ++total;
            if (ok && std::memcmp(want, got, sizeof(want)) == 0) ++exact;
        }
    }
    check(exact == total && total == 24,
          "the census's composed rows are the model's composeSceneCb to the bit, over six cameras (symmetric, asymmetric, kinds 0 and 3) and four turns each");
    Model m(3, 1.1f, 1.6f, 0.0f, 0.0f);
    float rows[16];
    VrCensusSnap dirty = m.snap();
    uint32_t flags = dirty.u(kVrCensusFlags) | kVrCensusFlagProj;
    std::memcpy(dirty.bytes + (kVrCensusFlags - kVrCensusSnapFrom), &flags, 4);
    check(!vrCensusComposeRows(dirty, rows), "a camera whose projection is still dirty after the body has no rows to offer");
    VrCensusSnap poisoned = m.snap();
    const float nan = std::nanf("");
    std::memcpy(poisoned.bytes + (kVrCensusProj - kVrCensusSnapFrom), &nan, 4);
    check(!vrCensusComposeRows(poisoned, rows), "a non-finite projection yields no rows (so nothing is joined on garbage)");
    // What the rows join on: the same camera again matches within 1e-5, another camera does not, the tolerance is a boundary.
    float a[16], b[16], c[16];
    Model eye(3, 1.3f, 0.9f, -0.03f, 0.011f, 0.0675f), world(3, 1.0122f, 5040.0f / 2835.0f, 0.0f, 0.0f);
    vrCensusComposeRows(eye.snap(), a);
    vrCensusComposeRows(eye.snap(), b);
    vrCensusComposeRows(world.snap(), c);
    check(vrCensusRowsMatch(a, b, 1.0e-5f) && !vrCensusRowsMatch(a, c, 1.0e-5f),
          "the join: a camera's rows match themselves and do not match another camera's");
    std::memcpy(b, a, sizeof(a));
    b[7] += 9.0e-6f;
    const bool inside = vrCensusRowsMatch(a, b, 1.0e-5f);
    b[7] = a[7] + 1.1e-5f;
    check(inside && !vrCensusRowsMatch(a, b, 1.0e-5f), "the join's tolerance is 1e-5: 9e-6 apart matches, 1.1e-5 apart does not");
    b[7] = nan;
    check(!vrCensusRowsMatch(a, b, 1.0e-5f), "a non-finite row never matches");
}

void testTangentsAndLeak() {
    std::printf("tangents and the leak measure\n");
    struct Case { float fov, aspect, bx, by; };
    const Case cases[] = {{1.1f, 1.6f, 0.0f, 0.0f}, {1.0122f, 5040.0f / 2835.0f, 0.0f, 0.0f}, {1.3f, 0.9f, -0.03f, 0.011f}, {1.2f, 0.93f, 0.0021f, -0.0004f}};
    bool tangentsOk = true, measureOk = true, expectOk = true;
    for (const Case& k : cases) {
        Model m(3, k.fov, k.aspect, k.bx, k.by);
        VrCensusSig sig;
        vrCensusSigFromSnap(m.snap(), &sig);
        float t[4];
        if (!vrCensusTangents(sig, t)) { tangentsOk = false; continue; }
        const double th = std::tan(static_cast<double>(k.fov * 0.5f)), w = th * k.aspect;
        const double want[4] = {-w * (1 + 2 * k.bx), w * (1 - 2 * k.bx), -th * (1 + 2 * k.by), th * (1 - 2 * k.by)};
        for (int i = 0; i < 4; ++i) if (!within(t[i], want[i], 2.0e-6 * (1 + std::fabs(want[i])))) tangentsOk = false;
        // The eye rows measure (p8, p9) = (2 boundX, 2 boundY), and the expectation built from the tangents agrees.
        float rows[16];
        vrCensusComposeRows(m.snap(), rows);
        float six[6][4] = {};
        std::memcpy(six, rows, sizeof(rows));
        double mx = 0, my = 0, ex = 0, ey = 0;
        if (!flatCameraMeasureRowShift(six, mx, my)) measureOk = false;
        if (!within(mx, 2.0 * k.bx, 1.0e-6) || !within(my, 2.0 * k.by, 1.0e-6)) measureOk = false;
        if (!vrCensusExpectedMeasure(t, 0.0f, 0.0f, &ex, &ey) || !within(ex, mx, 1.0e-6) || !within(ey, my, 1.0e-6)) expectOk = false;
    }
    check(tangentsOk, "a camera's tangents, read from its derived projection, are tan(fov/2) x aspect and tan(fov/2) with the bound shift, four cameras");
    check(measureOk, "flatCameraMeasureRowShift on a camera's composed rows reads (2 boundX, 2 boundY): the off-centre terms");
    check(expectOk, "the expectation built from a camera's own tangents equals what its rows measure");
    VrCensusSig ortho;
    ortho.kind = 1; ortho.p0 = 1; ortho.p5 = 1;
    float t[4];
    check(!vrCensusTangents(ortho, t), "an orthographic camera has no tangents to offer");
    VrCensusSig degenerate;
    degenerate.kind = 3;
    check(!vrCensusTangents(degenerate, t), "a degenerate projection (p0 = 0) has none either");

    // The leak measure, end to end: the eye's frustum as EDVR advertises it, plus the tangent shift, built into a camera,
    // composed, measured; the expectation from the same frustum and shift must agree, and a world phase added to the bound
    // pair (jx/R_w, -jy/R_h, the injector's own rule) must show up as exactly 2 x that in the leak.
    const float frustum[4] = {-1.2f, 0.7f, -0.9f, 1.1f};   // {left, right, down, up}, native_temporal_test's eye 0
    const float shift[2] = {0.00037f, -0.00021f};
    const double l = frustum[0] + shift[0], r = frustum[1] + shift[0], d = frustum[2] + shift[1], u = frustum[3] + shift[1];
    const double wHalf = (r - l) / 2, tanHalf = (u - d) / 2;
    const float bx = static_cast<float>(-(r + l) / (4 * wHalf)), by = static_cast<float>(-(u + d) / (4 * tanHalf));
    const float aspect = static_cast<float>(wHalf / tanHalf), fov = static_cast<float>(2 * std::atan(tanHalf));
    auto leakOf = [&](float extraBx, float extraBy, double* lx, double* ly) {
        Model eye(3, fov, aspect, bx + extraBx, by + extraBy);
        float rows[16];
        vrCensusComposeRows(eye.snap(), rows);
        float six[6][4] = {};
        std::memcpy(six, rows, sizeof(rows));
        double mx = 0, my = 0, sx = 0, sy = 0;
        flatCameraMeasureRowShift(six, mx, my);
        vrCensusExpectedMeasure(frustum, shift[0], shift[1], &sx, &sy);
        *lx = mx - sx;
        *ly = my - sy;
    };
    double lx = 0, ly = 0;
    leakOf(0.0f, 0.0f, &lx, &ly);
    check(within(lx, 0.0, 2.0e-6) && within(ly, 0.0, 2.0e-6),
          "an eye camera built from the advertised frustum and shift leaks nothing: the measured shift is the expected one");
    const float jx = 0.5f, jy = -0.25f;
    leakOf(jx / 5040.0f, -jy / 2835.0f, &lx, &ly);
    check(within(lx, 2.0 * jx / 5040.0, 3.0e-6) && within(ly, 2.0 * (-jy / 2835.0), 3.0e-6) && std::fabs(lx) > 1.0e-4,
          "a world phase of (0.5, -0.25) px at 5040x2835 leaking into the eye camera reads as (1.98e-4, 1.76e-4) of leak: far above the baseline");
    double trueX = 0, trueY = 0;
    const float flat4[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    check(vrCensusExpectedMeasure(frustum, 0.0f, 0.0f, &trueX, &trueY) && within(trueX, 0.5 / 1.9, 1.0e-6) && within(trueY, -0.1, 1.0e-6) &&
          !vrCensusExpectedMeasure(flat4, 0.0f, 0.0f, &trueX, &trueY),
          "the expectation for {-1.2, 0.7, -0.9, 1.1} is (0.2632, -0.1), and a degenerate frustum (zero width) has none");
}

// ---------------------------------------------------------------------------
// The bounded tables.
// ---------------------------------------------------------------------------
VrCensusSig sigOf(uint32_t kind, float aspect, float nearZ, float fov, float bx = 0, float by = 0) {
    VrCensusSig s;
    s.kind = kind; s.aspect = aspect; s.nearZ = nearZ; s.farZ = 50000.0f; s.fov = fov; s.boundX = bx; s.boundY = by;
    s.viewportW = 5040; s.viewportH = 2835; s.p0 = 0.6f; s.p5 = 1.0f; s.p8 = 2 * bx; s.p9 = 2 * by;
    return s;
}

// The call that reports a camera to the table: the frame, where the call fell, and its view and context.
VrCensusCameraTable::Call callAt(uint64_t frame, uint32_t caller = 0, uint32_t ordinal = 1, uint32_t draw = 0, bool drawKnown = false,
                                 VrCensusTone tone = VrCensusTone::None, uintptr_t view = 0, uintptr_t ctx = 0) {
    VrCensusCameraTable::Call c;
    c.frame = frame; c.callerRva = caller; c.ordinal = ordinal; c.draw = draw; c.drawKnown = drawKnown; c.tone = tone;
    c.view = view; c.ctx = ctx;
    return c;
}

void testCameraTable() {
    std::printf("camera table\n");
    VrCensusCameraTable table;
    const float noTan[4] = {};
    const VrCensusSig world = sigOf(3, 1.7778f, 0.025f, 1.0122f);
    check(table.note(0x1000, world, noTan, false, callAt(1, 0x594E13, 1, 0, false, VrCensusTone::None, 0xA000, 0xA100)) == VrCensusCameraTable::Event::New &&
          table.used() == 1 && table.at(0).linePending && table.at(0).calls == 1 && table.at(0).firstFrame == 1,
          "a camera pointer seen for the first time is a new row with its line pending");
    check(table.note(0x1000, world, noTan, false, callAt(1, 0x594E13, 2, 5, true, VrCensusTone::Before, 0xB000, 0xB100)) == VrCensusCameraTable::Event::Known &&
          table.used() == 1 && table.at(0).calls == 2 && table.at(0).firstOrdinal == 1 && table.at(0).firstView == 0xA000 && table.at(0).firstCtx == 0xA100,
          "the same camera again is known: one row, its first call's ordinal, view and context kept");
    check(table.takeCameraLine(0) && !table.takeCameraLine(0), "a first-sight line prints once");
    // Changes.
    VrCensusSig moved = world;
    moved.boundX = 5.0e-8f;   // a rounding: not a change
    check(table.note(0x1000, moved, noTan, false, callAt(2)) == VrCensusCameraTable::Event::Known,
          "a bound that moves by less than 1e-7 is not a change");
    VrCensusSig noisy = world;
    noisy.aspect = std::nextafter(world.aspect, 2.0f);
    noisy.fov = world.fov * (1.0f + 1.0e-7f);
    noisy.viewportW = world.viewportW + 0.001f;
    VrCensusSig realMove = world;
    realMove.aspect = world.aspect * 1.0001f;
    check(!vrCensusSigDiffers(world, noisy) && vrCensusSigDiffers(world, realMove),
          "an aspect or field of view that differs only in its last bits is not a change (an eye camera re-derives them every frame), a 1e-4 move is");
    VrCensusSig eyeMoved = world;
    eyeMoved.boundX = 0.0002f;
    check(table.note(0x1000, eyeMoved, noTan, false, callAt(3)) == VrCensusCameraTable::Event::Changed &&
          table.at(0).changes == 1 && table.at(0).changePending && table.at(0).changeFrame == 3 && table.at(0).changeFrom.boundX == 0.0f,
          "a bound that moves is a change: the row keeps what it was and the frame it moved in");
    VrCensusSig nearMoved = eyeMoved;
    nearMoved.nearZ = 0.03f;
    table.note(0x1000, nearMoved, noTan, false, callAt(4));
    check(table.at(0).changes == 2 && table.at(0).changeFrom.boundX == 0.0f && table.at(0).changeFrame == 3,
          "a second change before the boundary prints keeps the FIRST pending change (what the line will say)");
    // The per-camera cap: four lines, then counted.
    unsigned printed = 0;
    VrCensusSig s = world;
    for (unsigned i = 0; i < 12; ++i) {
        s.nearZ = 0.1f + 0.01f * i;
        table.note(0x1000, s, noTan, false, callAt(10 + i));
        if (table.takeChangeLine(0)) ++printed;
    }
    check(printed + 1 >= kVrCensusMaxChangesPerCamera && table.at(0).changeLines == kVrCensusMaxChangesPerCamera && table.at(0).changes >= 12,
          "a camera prints at most four 'changed:' lines; every change past them is counted in the row");
    // Capacity.
    VrCensusCameraTable full;
    for (uintptr_t i = 0; i < VrCensusCameraTable::kCapacity; ++i)
        full.note(0x10000 + i * 0x100, world, noTan, false, callAt(1));
    check(full.used() == 64 && full.overflow() == 0, "the table holds the first 64 cameras of a session");
    check(full.note(0x99999, world, noTan, false, callAt(1)) == VrCensusCameraTable::Event::Full &&
          full.used() == 64 && full.overflow() == 1 &&
          full.note(0x10000, world, noTan, false, callAt(1)) == VrCensusCameraTable::Event::Known && full.overflow() == 1,
          "a 65th camera is counted in overflow, not held, and a camera already held is still known");
    // The tangents and the first call's place are kept for the line.
    VrCensusCameraTable t2;
    const float tan4[4] = {-0.5f, 0.5f, -0.3f, 0.3f};
    t2.note(0x2000, world, tan4, true, callAt(7, 0x58DE73, 4, 1234, true, VrCensusTone::After, 0xE000, 0xE100));
    check(t2.at(0).tanValid && t2.at(0).tan[1] == 0.5f && t2.at(0).callerRva == 0x58DE73 && t2.at(0).firstDraw == 1234 &&
          t2.at(0).firstTone == VrCensusTone::After && t2.at(0).firstView == 0xE000 && t2.at(0).firstCtx == 0xE100,
          "a new row keeps the tangents, the caller, the draw, the tone and the view and context of its first call");
}

void testFrameBuffer() {
    std::printf("frame buffer\n");
    VrCensusFrame f;
    bool added = true;
    for (uint32_t i = 0; i < VrCensusFrame::kCapacity; ++i) {
        VrCensusCall* c = f.add();
        if (!c) { added = false; break; }
        c->camera = 0x1000 + i;
    }
    check(added && f.calls == 160 && f.recorded == 160 && f.truncated() == 0, "a frame records its first 160 calls");
    check(f.add() == nullptr && f.add() == nullptr && f.calls == 162 && f.recorded == 160 && f.truncated() == 2,
          "a call past 160 is counted, not recorded, and the frame says how many it cut");
    f.toneSeen = true;
    f.reset();
    check(f.calls == 0 && f.recorded == 0 && !f.toneSeen, "a boundary empties the frame and its tone flag");
    f.call[0].rowsValid = true;
    VrCensusCall* c = f.add();
    check(c && !c->rowsValid && c->camera == 0 && c->tone == VrCensusTone::None, "a record handed out is initialised (it never carries the last frame's rows)");
}

void testWindow() {
    std::printf("5 s window\n");
    VrCensusWindow w;
    w.noteCall(true, 3, 0x594E13, 0xA0, VrCensusTone::Before, false, true);    // an injected world call
    w.noteCall(true, 3, 0x594EAB, 0xA0, VrCensusTone::After, false, false);    // not injected (after the trigger)
    w.noteCall(true, 0, 0x594E13, 0xB0, VrCensusTone::After, true, false);
    w.noteCall(false, 0, 0x58DE73, 0xC0, VrCensusTone::None, false, false);
    w.noteCall(true, 9, 0x111111, 0xB0, VrCensusTone::None, false, true);      // (a kind the detour never injects, but the window only counts)
    check(w.calls == 5 && w.stale == 1 && w.kinds[3] == 2 && w.kinds[0] == 1 && w.kinds[7] == 1 && w.kinds[6] == 1 &&
          w.cameraCount == 3 && w.callerCount == 4 && w.toneBefore == 1 && w.toneAfter == 2 && w.toneNone == 2,
          "a window counts calls, stale calls, kinds (other and unreadable apart), distinct cameras and callers, and tone positions");
    check(w.injCalls == 2, "a window counts the calls the observer heard with willInject set (inj-calls)");
    VrCensusWindowText text;
    text.hook = "installed";
    text.camerasTotal = 3;
    char line[kVrCensusLineBytes + 1];
    vrCensusFormatWindow(line, sizeof(line), w, text);
    check(std::strstr(line, "kinds=0:1,3:2,other:1,unreadable:1") != nullptr && std::strstr(line, "callers=+0x594E13:2,+0x594EAB:1,+0x58DE73:1,+0x111111:1") != nullptr &&
          std::strstr(line, "cameras-seen=3 cameras-total=3 tone=1/2/2") != nullptr && std::strstr(line, " stale=1 inj-calls=2 kinds=") != nullptr,
          "the 5 s line names the kinds and the callers busiest first, and the tone split, and the injected calls beside the stale ones");
    VrCensusWindow empty;
    vrCensusFormatWindow(line, sizeof(line), empty, VrCensusWindowText{});
    check(std::strstr(line, "frames=0 calls=0 posts=0 off-thread=0 stale=0 inj-calls=0 kinds=- callers=- cameras-seen=0") != nullptr &&
          std::strstr(line, "progress=no hook=pending windows=0") != nullptr,
          "an empty window still prints every field (zeros included): an absent line is what 'the census never ran' looks like");
    // The worst case still fits a log line: sixteen callers, every kind, six-digit counts.
    VrCensusWindow big;
    for (uint32_t i = 0; i < 40; ++i) big.noteCall(i % 9 != 8, i % 9, 0x594E13 + i, 0x1000 + i, static_cast<VrCensusTone>(i % 3), (i & 1) != 0, (i % 3) == 0);
    big.frames = big.calls = big.posts = big.injCalls = 99999999999ull;
    big.onFootFrames = big.eyeDraws = big.eyeOnFoot = 99999999999ull;
    big.windows = 12;
    VrCensusWindowText bigText;
    bigText.hook = "installed"; bigText.offThread = 99999999999ull; bigText.offThreadOverflow = 99999999999ull;
    bigText.camerasTotal = 64; bigText.cameraTableOverflow = 99999999999ull;
    const int n = vrCensusFormatWindow(line, sizeof(line), big, bigText);
    check(n > 0 && static_cast<size_t>(n) <= kVrCensusLineBytes && std::strlen(line) <= kVrCensusLineBytes && std::strstr(line, ",+more:") != nullptr,
          "the worst-case 5 s line (forty callers, every kind, eleven-digit counts) is at most 400 characters and says how many callers it left out");
    // Thinning: every window for the first three minutes, then one line per twelve windows covering all of them.
    unsigned early = 0;
    for (uint32_t t = 1; t <= kVrCensusEveryWindow; ++t) if (vrCensusWindowPrints(t)) ++early;
    unsigned hour = 0;
    for (uint32_t t = 1; t <= 720; ++t) if (vrCensusWindowPrints(t)) ++hour;
    check(early == kVrCensusEveryWindow && !vrCensusWindowPrints(kVrCensusEveryWindow + 1) && vrCensusWindowPrints(48) &&
          !vrCensusWindowPrints(47) && vrCensusWindowPrints(720) && hour == 36 + 57,
          "the first 36 windows (three minutes) each print; after that one in twelve (a minute), so an hour is 93 lines");
    bool fits = true;
    VrCensusBudget b;
    for (uint32_t t = 1; t <= 720 * 2; ++t) if (vrCensusWindowPrints(t)) fits &= b.take(VrCensusLines::Window) || t > 720;
    check(fits && b.used[static_cast<size_t>(VrCensusLines::Window)] <= VrCensusBudget::kCap[static_cast<size_t>(VrCensusLines::Window)],
          "the first hour of 5 s lines fits the class's cap of 100");
}

void testBudget() {
    std::printf("line budget\n");
    VrCensusBudget b;
    unsigned taken = 0;
    while (taken < 100000 && b.take(VrCensusLines::Camera)) ++taken;   // the bounds keep a cap that never closes from hanging the rig
    check(taken == 64 && b.suppressed[static_cast<size_t>(VrCensusLines::Camera)] == 1 && b.take(VrCensusLines::Changed),
          "a class is capped on its own (cameras 64) and a full class does not starve the others");
    unsigned calls = 0;
    while (calls < 100000 && b.take(VrCensusLines::Call)) ++calls;
    check(calls == 480 && VrCensusBudget::capTotal() == 100 + 64 + 24 + 480 + 16 + 16 + 24 + 220 + 1200 + 100 + 100 + 100,
          "call lines are capped at 480 (three sequences of the 160-call frame) and the caps add up to 2,444: the 724 of the first three sequences, plus the episodes' own "
          "classes (220 header, pass, join-draws and join lines; 1,200 call lines) and the three companion lines a 5 s window adds (100 each)");
    unsigned episodeLines = 0, episodeCalls = 0;
    while (episodeLines < 100000 && b.take(VrCensusLines::Episode)) ++episodeLines;
    while (episodeCalls < 100000 && b.take(VrCensusLines::EpisodeCall)) ++episodeCalls;
    check(episodeLines == 220 && episodeCalls == kVrCensusMaxEpisodes * kVrCensusEpisodeLines &&
              episodeLines >= kVrCensusMaxEpisodes * (1 + 1 + 1 + kVrCensusJoinRows + 4 + 1),
          "ten episodes fit their classes: a header, the pass's rows, the join's draw counts, twelve join signatures, four rows lines and the overflow note each (200 of the 220), "
          "and 120 call lines each (1,200)");
    VrCensusBudget separate;
    for (unsigned guard = 0; guard < 100000 && separate.take(VrCensusLines::EpisodeCall); ++guard) {}
    for (unsigned guard = 0; guard < 100000 && separate.take(VrCensusLines::Episode); ++guard) {}
    check(separate.take(VrCensusLines::Call) && separate.take(VrCensusLines::Window) && separate.take(VrCensusLines::Runs) && separate.take(VrCensusLines::Cpu) &&
              separate.take(VrCensusLines::Counters),
          "the episodes' classes are their own: an episode that used every one of its lines starves neither the first three sequences, the 5 s lines nor the window's companions");
    check(std::strcmp(vrCensusLinesName(VrCensusLines::Episode), "episode") == 0 && std::strcmp(vrCensusLinesName(VrCensusLines::EpisodeCall), "episode-call") == 0 &&
              std::strcmp(vrCensusLinesName(VrCensusLines::Runs), "runs") == 0 && std::strcmp(vrCensusLinesName(VrCensusLines::Cpu), "detour") == 0 &&
              std::strcmp(vrCensusLinesName(VrCensusLines::Counters), "episodes") == 0,
          "the new classes have names for the 'line budget reached' note");
    std::printf("  note  a hard worst case of %u lines, about 400 in a real session (see the budget's comment)\n", VrCensusBudget::capTotal());
    // A realistic session: ten minutes, twenty cameras, three sequences of 110 calls, eight eye draws.
    VrCensusBudget real;
    unsigned session = 0;
    for (uint32_t t = 1; t <= 120; ++t) if (vrCensusWindowPrints(t) && real.take(VrCensusLines::Window)) ++session;
    for (int i = 0; i < 20; ++i) if (real.take(VrCensusLines::Camera)) ++session;
    for (int i = 0; i < 3 * (1 + 110); ++i) if (real.take(VrCensusLines::Call)) ++session;
    for (int i = 0; i < 16; ++i) if (real.take(VrCensusLines::Eye)) ++session;
    for (int i = 0; i < 6; ++i) if (real.take(VrCensusLines::Info)) ++session;
    check(session >= 380 && session <= 430, "a ten-minute session that reaches on-foot (twenty cameras, 110-call frames) logs about 400 lines");
    std::printf("  note  that session: %u lines\n", session);
    check(vrCensusPrintsSequence(true, 0) && vrCensusPrintsSequence(true, 2) && !vrCensusPrintsSequence(true, 3) && !vrCensusPrintsSequence(false, 0),
          "a call sequence prints for an on-foot frame (the tone was seen), the first three only");
    VrCensusEyeBudget eye;
    bool ok = true;
    for (uint64_t frame = 10; frame < 14; ++frame) { ok &= eye.take(frame); ok &= eye.take(frame); }
    check(ok && eye.draws() == 8 && eye.frames() == 4 && !eye.take(10) && !eye.take(99),
          "eye draws are read back for the first four on-foot frames, eight draws in all, and a ninth or a fifth frame is refused");
    VrCensusEyeBudget early;
    bool e = early.take(5) && early.take(5) && early.take(5) && early.take(6) && early.take(7) && early.take(8) && !early.take(9);
    check(e && early.draws() == 6, "three draws in one frame still count against the frame allowance of four, not its own");
}

void testOffThread() {
    std::printf("calls on other threads\n");
    VrCensusOffThread t;
    t.note(11, 0x1000, 3, true, 0x594E13);
    t.note(11, 0x1000, 3, true, 0x594E13);
    t.note(12, 0x1000, 3, true, 0x594E13);
    t.note(11, 0x2000, 0, false, 0x58DE73);
    check(t.total() == 4 && t.used() == 3 && t.overflow() == 0, "a repeated tuple is one entry; another thread, another camera or caller is another");
    VrCensusOffThread::Entry e;
    uint64_t seen = 0;
    unsigned entries = 0;
    bool hasUnreadable = false;
    while (t.takeNew(&e)) { ++entries; seen += e.calls; hasUnreadable |= !e.kindReadable && e.camera == 0x2000; }
    check(entries == 3 && seen == 4 && hasUnreadable && !t.takeNew(&e), "the boundary takes each entry once, with its count, and an unreadable kind says so");
    for (uint32_t i = 0; i < 40; ++i) t.note(100 + i, 0x5000 + i, 3, true, 0x594E13);
    check(t.used() == VrCensusOffThread::kCapacity && t.overflow() == 40 - (VrCensusOffThread::kCapacity - 3) && t.total() == 44,
          "the table holds sixteen tuples; the calls it cannot name are counted in overflow");
    // Concurrency: four threads, one camera each, ten thousand calls each; the totals are exact and every call is somewhere.
    VrCensusOffThread c;
    std::vector<std::thread> threads;
    for (uint32_t k = 0; k < 4; ++k)
        threads.emplace_back([&c, k] { for (int i = 0; i < 10000; ++i) c.note(500 + k, 0x9000 + k, 3, true, 0x594E13 + (i & 1)); });
    for (auto& th : threads) th.join();
    uint64_t sum = 0;
    VrCensusOffThread::Entry x;
    while (c.takeNew(&x)) sum += x.calls;
    check(c.total() == 40000 && sum + c.overflow() == 40000 && c.used() == 8,
          "four threads noting ten thousand calls each lose none: the entries' counts and the overflow add to the total");
}

// ---------------------------------------------------------------------------
// The episodes (design-world-camera-motion-2026-09-30.md section 6, Phase 0): the trigger machine, the sampled frame's calls, the join, the matches, the
// call-line selection, the naming runs and the observer's CPU. Pure: the glue (vr_camera_census.cpp) drives the same classes in the same order.
// ---------------------------------------------------------------------------
VrCensusEpisodes::Inputs readings(VrCensusFoot foot, bool named, bool guiKnown = false, uint32_t gui = 0) {
    VrCensusEpisodes::Inputs in;
    in.foot = foot; in.named = named; in.guiKnown = guiKnown; in.gui = gui;
    return in;
}

void testEpisodeMachine() {
    std::printf("episodes: the triggers\n");
    // The key-on trigger: armed at the boundary that starts frame 1, sampled kVrCensusEpisodeDelay (30) frames later.
    {
        VrCensusEpisodes e;
        e.restart();
        e.keyOn(1);
        check(e.armed() && e.started() == 1 && e.triggers() == 1 && e.skipped() == 0 && e.armedFrame() == 1 && e.sampleFrame() == 31 &&
                  e.trigger().kind == VrCensusTriggerKind::KeyOn && std::strcmp(e.stateName(), "armed") == 0,
              "the census turning on arms one episode for 30 frames later");
        bool early = false;
        for (uint64_t f = 1; f <= 30; ++f) early = early || e.boundary(f, f > 1, readings(VrCensusFoot::No, false));
        check(!early && e.armed(), "no boundary before the sampled frame takes the episode live");
        check(e.boundary(31, true, readings(VrCensusFoot::No, false)) && e.live() && std::strcmp(e.stateName(), "live") == 0 &&
                  !e.boundary(32, true, readings(VrCensusFoot::No, false)) && e.live(),
              "the boundary that starts frame 31 takes it live, once; it stays live until it is finished");
        e.finish();
        check(!e.live() && !e.armed() && e.started() == 1 && std::strcmp(e.stateName(), "idle") == 0, "finish() ends it; the count of episodes taken stays");
    }
    // The journal's on-foot reading flips, either way. The first known reading is a baseline; unknown and off say nothing.
    {
        VrCensusEpisodes e;
        e.restart();
        e.boundary(2, true, readings(VrCensusFoot::No, false));
        check(e.triggers() == 0, "the first known reading of the journal is a baseline, not a flip");
        e.boundary(3, true, readings(VrCensusFoot::Unknown, false));
        e.boundary(4, true, readings(VrCensusFoot::Off, false));
        e.boundary(5, true, readings(VrCensusFoot::No, false));
        check(e.triggers() == 0, "unknown (a menu) and off (no journal) between two readings of 'not on foot' are no flip");
        e.boundary(6, true, readings(VrCensusFoot::Yes, false));
        check(e.armed() && e.started() == 1 && e.trigger().kind == VrCensusTriggerKind::Foot && e.trigger().from == 0 && e.trigger().to == 1 &&
                  e.armedFrame() == 6 && e.sampleFrame() == 36,
              "no -> yes is a trigger: foot:no>yes, armed at the boundary that read it");
        for (uint64_t f = 7; f <= 36; ++f) e.boundary(f, true, readings(VrCensusFoot::Yes, false));
        check(e.live(), "...and the episode of it goes live at frame 36");
        e.finish();
        e.boundary(37, true, readings(VrCensusFoot::Unknown, false));   // a menu, then back to the same word: no flip
        e.boundary(38, true, readings(VrCensusFoot::Yes, false));
        check(e.triggers() == 1, "yes -> unknown -> yes is not a flip");
        e.boundary(39, true, readings(VrCensusFoot::No, false));
        check(e.armed() && e.trigger().kind == VrCensusTriggerKind::Foot && e.trigger().from == 1 && e.trigger().to == 0 && e.triggers() == 2,
              "yes -> no (boarding) is a trigger too: foot:yes>no");
    }
    // The naming flips and holds kVrCensusNamingHold frames.
    {
        VrCensusEpisodes e;
        e.restart();
        e.boundary(2, true, readings(VrCensusFoot::Yes, false));   // baseline: unnamed
        e.boundary(3, true, readings(VrCensusFoot::Yes, true));
        e.boundary(4, true, readings(VrCensusFoot::Yes, true));
        check(e.triggers() == 0, "a flip to named that has held two frames is not yet a trigger");
        e.boundary(5, true, readings(VrCensusFoot::Yes, false));   // a blip: back to the held value
        e.boundary(6, true, readings(VrCensusFoot::Yes, true));
        e.boundary(7, true, readings(VrCensusFoot::Yes, true));
        check(e.triggers() == 0, "a blip back to the held value starts the count again");
        e.boundary(8, true, readings(VrCensusFoot::Yes, true));
        check(e.armed() && e.trigger().kind == VrCensusTriggerKind::Naming && e.trigger().from == 0 && e.trigger().to == 1 && e.armedFrame() == 8 && e.triggers() == 1,
              "three frames in a row of the new value are the flip: naming:unnamed>named, at the third");
        for (uint64_t f = 9; f <= 38; ++f) e.boundary(f, true, readings(VrCensusFoot::Yes, true));
        e.finish();
        check(e.triggers() == 1, "a value that stays is no further trigger");
        e.boundary(39, true, readings(VrCensusFoot::Yes, false));
        e.boundary(40, true, readings(VrCensusFoot::Yes, false));
        e.boundary(41, true, readings(VrCensusFoot::Yes, false));
        check(e.armed() && e.trigger().kind == VrCensusTriggerKind::Naming && e.trigger().from == 1 && e.trigger().to == 0 && e.triggers() == 2,
              "and back: naming:named>unnamed after three unnamed frames");
        // Two of one value then the other candidate: the run restarts on a change of candidate, not a sum.
        VrCensusEpisodes f;
        f.restart();
        f.boundary(2, true, readings(VrCensusFoot::Yes, false));
        f.boundary(3, true, readings(VrCensusFoot::Yes, true));
        f.boundary(4, true, readings(VrCensusFoot::Yes, true));
        f.boundary(5, true, readings(VrCensusFoot::Yes, false));
        f.boundary(6, true, readings(VrCensusFoot::Yes, true));
        check(f.triggers() == 0, "named, named, unnamed, named is no flip: the hold is of consecutive frames");
    }
    // GuiFocus: a change between two known values.
    {
        VrCensusEpisodes e;
        e.restart();
        e.boundary(2, true, readings(VrCensusFoot::No, false, true, 0));
        e.boundary(3, true, readings(VrCensusFoot::No, false, false, 0));   // Status.json unreadable for a moment
        e.boundary(4, true, readings(VrCensusFoot::No, false, true, 0));
        check(e.triggers() == 0, "GuiFocus unknown between two readings of the same value is no change");
        {   // the discriminating case: an unreadable Status.json comes with a 0 in the reading, and that 0 is not a GuiFocus
            VrCensusEpisodes keep;
            keep.restart();
            keep.boundary(2, true, readings(VrCensusFoot::No, false, true, 6));    // the galaxy map is open
            keep.boundary(3, true, readings(VrCensusFoot::No, false, false, 0));   // Status.json unreadable for a moment: handed as 0, known = false
            keep.boundary(4, true, readings(VrCensusFoot::No, false, true, 6));
            check(keep.triggers() == 0, "GuiFocus 6, then unreadable (the reading's 0 is no value), then 6 again: no trigger either way (the open map is not a change)");
        }
        e.boundary(5, true, readings(VrCensusFoot::No, false, true, 6));
        check(e.armed() && e.trigger().kind == VrCensusTriggerKind::Gui && e.trigger().from == 0 && e.trigger().to == 6 && e.armedFrame() == 5,
              "0 -> 6 (the galaxy map opening) is a trigger: gui:0>6");
        e.abandon();
        check(!e.armed() && e.started() == 1, "abandon() drops an armed episode (the census went off); the count stays");
        // On foot GuiFocus is unknown on every frame (flight 1): never a trigger.
        VrCensusEpisodes foot;
        foot.restart();
        for (uint64_t f = 2; f < 40; ++f) foot.boundary(f, true, readings(VrCensusFoot::Yes, true, false, 0));
        check(foot.triggers() == 0, "a GuiFocus that is never known is never a trigger");
    }
    // Triggers that arrive while an episode is armed are counted and said once; so are those after the cap.
    {
        VrCensusEpisodes e;
        e.restart();
        e.keyOn(1);
        e.boundary(2, true, readings(VrCensusFoot::No, false));
        e.boundary(3, true, readings(VrCensusFoot::Yes, false));   // a flip while armed
        e.boundary(4, true, readings(VrCensusFoot::No, false));    // and another
        const char* why = nullptr;
        check(e.triggers() == 3 && e.skipped() == 2 && e.started() == 1 && e.takeSkipNote(&why) && std::strstr(why, "armed") != nullptr && !e.takeSkipNote(&why),
              "two triggers while an episode is armed are counted (3 triggers, 2 skipped) and said once, naming why");
        // The session's cap: ten episodes, then every trigger is skipped; the first skip here was the cap's.
        VrCensusEpisodes cap;
        cap.restart();
        uint64_t frame = 1;
        for (uint32_t i = 0; i < kVrCensusMaxEpisodes; ++i) {
            cap.keyOn(frame);
            // Bounded: an episode that never goes live must fail the checks below, not hang the rig.
            for (uint32_t guard = 0; guard < 200 && !cap.boundary(frame, true, readings(VrCensusFoot::No, false)); ++guard) ++frame;
            cap.finish();
            ++frame;
        }
        check(cap.started() == kVrCensusMaxEpisodes && cap.skipped() == 0 && cap.triggers() == kVrCensusMaxEpisodes, "ten episodes can be taken in a session");
        cap.keyOn(frame);
        cap.boundary(frame, true, readings(VrCensusFoot::No, false));
        const char* capWhy = nullptr;
        check(!cap.armed() && cap.started() == kVrCensusMaxEpisodes && cap.skipped() == 1 && cap.takeSkipNote(&capWhy) && std::strstr(capWhy, "all taken") != nullptr,
              "an eleventh trigger is counted and never sampled, and the note says the session's episodes were all taken");
        // A restart (the census off and on) keeps the count and the cap, and forgets the readings.
        VrCensusEpisodes r;
        r.restart();
        r.boundary(2, true, readings(VrCensusFoot::Yes, false, true, 6));
        r.restart();
        r.boundary(2, true, readings(VrCensusFoot::No, true, true, 0));
        check(r.triggers() == 0, "after a restart no flip is judged against a reading from before it: yes -> no across the gap is a baseline, not a trigger");
        VrCensusEpisodes kept;
        kept.restart();
        kept.keyOn(1);
        kept.restart();
        check(!kept.armed() && kept.started() == 1 && kept.triggers() == 1, "a restart drops what was armed and keeps the counters");
    }
    // Several triggers at one boundary: the first arms, the rest are skipped (order: foot, naming, GuiFocus).
    {
        VrCensusEpisodes both;
        both.restart();
        both.boundary(2, true, readings(VrCensusFoot::No, false, true, 0));
        both.boundary(3, true, readings(VrCensusFoot::Yes, false, true, 7));
        check(both.armed() && both.trigger().kind == VrCensusTriggerKind::Foot && both.triggers() == 2 && both.skipped() == 1,
              "a journal flip and a GuiFocus change at one boundary: the flip arms the episode, the change is counted as skipped");
    }
}

void testNamingRuns() {
    std::printf("episodes: the on-foot naming runs\n");
    check(VrCensusRuns::bin(1) == 0 && VrCensusRuns::bin(2) == 1 && VrCensusRuns::bin(3) == 2 && VrCensusRuns::bin(4) == 3 && VrCensusRuns::bin(8) == 3 &&
              VrCensusRuns::bin(9) == 4 && VrCensusRuns::bin(30) == 4 && VrCensusRuns::bin(31) == 5 && VrCensusRuns::bin(89) == 5 && VrCensusRuns::bin(90) == 6 &&
              VrCensusRuns::bin(100000) == 6,
          "the bins are 1, 2, 3, 4-8, 9-30, 31-89 and 90+ frames, at their edges");
    check(std::strcmp(VrCensusRuns::binName(0), "1") == 0 && std::strcmp(VrCensusRuns::binName(3), "4-8") == 0 && std::strcmp(VrCensusRuns::binName(6), "90+") == 0,
          "...and they are named as the line writes them");
    VrCensusRuns r;
    auto frames = [&](int n, bool onFoot, bool named) { for (int i = 0; i < n; ++i) r.noteFrame(onFoot, named); };
    frames(5, true, true);     // a named run of 5
    frames(1, true, false);    // an unnamed run of 1
    frames(3, true, true);     // named 3
    frames(2, true, false);    // unnamed 2
    frames(100, true, true);   // named 100 (still open)
    check(r.frames == 111 && r.named[3] == 1 && r.named[2] == 1 && r.named[6] == 0 && r.unnamed[0] == 1 && r.unnamed[1] == 1 && r.open == 1 && r.openLength == 100 &&
              r.longestNamed == 100 && r.longestUnnamed == 2,
          "a run is counted when it ends: 5 named, 1 unnamed, 3 named, 2 unnamed are in their bins, the 100 still open is not, and it is the longest named already");
    r.endRun();
    frames(0, true, true);
    check(r.named[6] == 1 && r.open == -1, "the open run is counted when it ends");
    // The carry: a window prints with a run open; the next window does not count it again, and carries its length.
    VrCensusRuns w;
    for (int i = 0; i < 7; ++i) w.noteFrame(true, true);
    w.resetWindow();
    check(w.frames == 0 && w.named[3] == 0 && w.open == 1 && w.openLength == 7 && w.longestNamed == 7 && w.longestUnnamed == 0,
          "a window's reset empties its counts and carries the open run, which is the longest of its kind already");
    {   // ...and with runs counted in both kinds first: every bin of both is emptied
        VrCensusRuns full;
        for (int i = 0; i < 5; ++i) full.noteFrame(true, true);
        for (int i = 0; i < 2; ++i) full.noteFrame(true, false);
        for (int i = 0; i < 3; ++i) full.noteFrame(true, true);   // a named 5 and an unnamed 2 are counted; a named 3 is open
        check(full.named[3] == 1 && full.unnamed[1] == 1 && full.open == 1 && full.openLength == 3, "a window with a named run of 5 and an unnamed run of 2 counted, and a named run of 3 open");
        full.resetWindow();
        uint64_t left = 0;
        for (int b = 0; b < VrCensusRuns::kBins; ++b) left += full.named[b] + full.unnamed[b];
        check(left == 0 && full.frames == 0 && full.open == 1 && full.openLength == 3 && full.longestNamed == 3 && full.longestUnnamed == 0,
              "a window's reset empties every bin of both kinds, the frame count and the longest of the unnamed, and carries the open named run of 3");
    }
    for (int i = 0; i < 3; ++i) w.noteFrame(true, true);
    w.noteFrame(true, false);   // ends the named run at 10
    check(w.named[4] == 1 && w.frames == 4 && w.longestNamed == 10 && w.open == 0 && w.openLength == 1,
          "...and when it ends in the next window it is counted once, at its whole length (10: the 9-30 bin)");
    // Not on foot ends the run and counts no frame.
    VrCensusRuns off;
    for (int i = 0; i < 4; ++i) off.noteFrame(true, false);
    off.noteFrame(false, false);
    off.noteFrame(false, true);
    check(off.frames == 4 && off.unnamed[3] == 1 && off.open == -1 && off.longestUnnamed == 4,
          "a frame the journal does not say on foot ends the run and is not counted (a ship's frames are no on-foot frame)");
    // H2's discriminator: a world where the naming blips for one or two frames, and a map that holds it for ten.
    VrCensusRuns h2;
    for (int i = 0; i < 300; ++i) h2.noteFrame(true, !(i == 50 || i == 51 || i == 120 || (i >= 200 && i < 210)));
    check(h2.unnamed[0] == 1 && h2.unnamed[1] == 1 && h2.unnamed[2] == 0 && h2.unnamed[4] == 1 && h2.longestUnnamed == 10,
          "H2's reading: a blip of one frame, one of two, and a map's ten unnamed frames land in the 1, 2 and 9-30 bins, nothing in the 3 bin, longest 10");
}

void testObserverCpu() {
    std::printf("episodes: the observer halves' CPU\n");
    VrCensusCpu c;
    unsigned picked = 0;
    for (int i = 1; i <= 64; ++i) if (c.pick()) ++picked;
    check(picked == 4, "one call in sixteen is picked: 4 of 64");
    VrCensusCpu d;
    bool first15 = false;
    for (int i = 1; i <= 15; ++i) first15 = first15 || d.pick();
    check(!first15 && d.pick(), "the sixteenth call is the first picked: the first fifteen cost a counter and a compare");
    c.notePre(false, 100); c.notePre(false, 300); c.notePost(false, 50); c.notePost(false, 150);
    c.notePre(true, 400); c.notePost(true, 200);
    check(c.mode[0].sampled == 2 && c.mode[0].preTicks == 400 && c.mode[0].preMax == 300 && c.mode[0].postSampled == 2 && c.mode[0].postMax == 150 &&
              c.mode[1].sampled == 1 && c.mode[1].preTicks == 400 && c.mode[1].postTicks == 200,
          "the halves of an observed call and of an injected call are accumulated apart: sampled, sum, longest");
    const uint32_t tickBefore = c.tick;
    c.resetWindow();
    check(c.mode[0].sampled == 0 && c.mode[1].preTicks == 0 && c.tick == tickBefore, "a window's reset empties the sums and keeps the call counter");
    // The line, from a clock of 10 MHz (ticks of 0.1 us): 2 timed observed calls (pre mean 20 us, max 30; post mean 10, max 15), 1 injected (40 and 20).
    VrCensusCpu e;
    e.notePre(false, 100 * 2); e.notePre(false, 100 * 3 * 1); e.notePost(false, 100); e.notePost(false, 150);
    e.notePre(true, 400); e.notePost(true, 200);
    VrCensusWindow w;
    w.frames = 100; w.calls = 1000; w.injCalls = 400;
    char line[kVrCensusLineBytes + 1];
    vrCensusFormatCpu(line, sizeof(line), w, e, 10000000, 3);
    // Observed: 600 calls x (25 us + 12.5 us) = 22500 us; injected: 400 x (40 + 20) = 24000 us; 46500 us over 100 frames = 0.465 ms a frame.
    check(std::strcmp(line,
                      "vr camera census: detour windows=3 every=16 timed=observer-halves frames=100 calls=1000 sampled=3 est-ms-frame=0.465 "
                      "obs-calls=600 obs-sampled=2 obs-pre-us=25/30 obs-post-us=12.5/15 inj-calls=400 inj-sampled=1 inj-pre-us=40/40 inj-post-us=20/20") == 0,
          "the detour line: windows, how it is sampled, frames, calls, sampled, the estimated ms a frame, and per mode the calls, the sampled and the mean/longest microseconds of each half");
    if (std::strstr(line, "est-ms-frame=0.465") == nullptr) std::printf("  note  %s\n", line);
    VrCensusCpu none;
    vrCensusFormatCpu(line, sizeof(line), w, none, 10000000, 1);
    check(std::strstr(line, " sampled=0 est-ms-frame=- obs-calls=600 obs-sampled=0 obs-pre-us=- obs-post-us=- inj-calls=400 inj-sampled=0 inj-pre-us=- inj-post-us=-") != nullptr,
          "with no call timed every figure is a dash, never a zero: a window that timed nothing cannot be read as a free detour");
    VrCensusWindow idle;
    vrCensusFormatCpu(line, sizeof(line), idle, none, 10000000, 1);
    check(std::strstr(line, "frames=0 calls=0 sampled=0 est-ms-frame=- obs-calls=0 ") != nullptr, "an empty window still prints the line (zeros included): an absent line is what 'the census never ran' looks like");
    vrCensusFormatCpu(line, sizeof(line), w, e, 0, 1);
    check(std::strstr(line, " est-ms-frame=- ") != nullptr && std::strstr(line, "obs-pre-us=-") != nullptr, "a clock whose frequency is unknown prints dashes");
    VrCensusCpu onlyObserved;
    onlyObserved.notePre(false, 100); onlyObserved.notePost(false, 100);
    vrCensusFormatCpu(line, sizeof(line), w, onlyObserved, 10000000, 1);
    check(std::strstr(line, "est-ms-frame=-") != nullptr, "calls injected in the window with no injected call timed make the estimate a dash (a mode with calls and no sample cannot be estimated)");
}

// An episode frame's calls, the join, the matches and the selection.
void testEpisodeFrame() {
    std::printf("episodes: the sampled frame's calls, the join and the matches\n");
    std::unique_ptr<VrCensusEpisodeFrame> fp(new VrCensusEpisodeFrame);
    VrCensusEpisodeFrame& f = *fp;
    f.reset();
    // The cockpit has hundreds of refresh calls a frame: the buffer holds 640, tallies every one and says how many it could not hold.
    uint32_t added = 0;
    for (uint32_t i = 0; i < 700; ++i) {
        VrCensusCall* c = f.add(i % 50 != 49, i % 50 == 49 ? 0 : (i % 7 == 0 ? 5 : i % 7 == 1 ? 9 : 3), 0x594E00 + (i % 20));
        if (c) { ++added; c->camera = 0x1000 + i; }
    }
    check(added == 640 && f.calls == 700 && f.recorded == 640 && f.truncated() == 60, "an episode frame records its first 640 calls and counts the rest (700 calls, 60 truncated)");
    uint64_t kinds = 0;
    for (int k = 0; k < 8; ++k) kinds += f.kinds[k];
    check(kinds == 700 && f.kinds[5] > 0 && f.kinds[6] > 0 && f.kinds[7] == 14,
          "the kind tally covers every call, recorded or not (other kinds and unreadable ones in their own slots)");
    uint64_t inTable = 0;
    for (uint32_t i = 0; i < f.callerCount; ++i) inTable += f.callers[i].n;
    check(f.callerCount == 16 && inTable == 16 * 35 && f.callerOverflow == 4 * 35,
          "the caller tally holds sixteen distinct callers and counts the calls of the rest in an overflow (twenty were called from, 35 calls each)");
    f.reset();
    check(f.calls == 0 && f.recorded == 0 && f.kinds[3] == 0 && f.callerCount == 0 && f.callerOverflow == 0, "a go-live empties the buffer and every tally");
    VrCensusCall* fresh = f.add(true, 3, 0x1);
    check(fresh && fresh->camera == 0 && !fresh->rowsValid && !fresh->axesValid && fresh->tone == VrCensusTone::None,
          "a record handed out is initialised (it never carries the last episode's rows or axes)");

    // The join: signatures, the first of a depth, the cap.
    VrCensusJoin join;
    join.begin();
    VrCensusJoinRow* a = join.add(VrCensusJoinDepth::Screen, 5040, 2835, 0xAA, 0xBB, 100);
    check(a && join.used == 1 && a->draws == 1 && a->firstDraw == 100 && join.find(VrCensusJoinDepth::Screen, 0xAA, 0xBB) == a &&
              join.find(VrCensusJoinDepth::Screen, 0xAA, 0xCC) == nullptr && join.find(VrCensusJoinDepth::Eye0, 0xAA, 0xBB) == nullptr,
          "a row is a (depth, vertex shader, pixel shader): the same pair into another depth, or another pixel shader, is another row");
    check(join.hasDepth(VrCensusJoinDepth::Screen) && !join.hasDepth(VrCensusJoinDepth::Eye0), "a depth's first row is known by hasDepth (the readback is spent on it only)");
    for (uint32_t i = 1; i < kVrCensusJoinRows; ++i) join.add(VrCensusJoinDepth::Eye1, 2620, 2533, 0x10 + i, 0x20, 200 + i);
    check(join.used == kVrCensusJoinRows && join.add(VrCensusJoinDepth::Eye1, 2620, 2533, 0x999, 0x20, 300) == nullptr && join.overflowRows == 1 && join.overflowDraws == 1,
          "the table keeps twelve signatures; a thirteenth is counted in the overflow, never lost silently");
    join.seen = 77;
    join.views = 3;
    check(join.relevantDraws() == kVrCensusJoinRows + 1, "the draws that reached a screen's or an eye's depth are the twelve rows' first draws and the one past the table");
    join.begin();
    check(join.used == 0 && join.overflowRows == 0 && join.overflowDraws == 0 && join.seen == 0 && join.views == 0 && join.relevantDraws() == 0,
          "an episode's go-live empties the join: its rows, its overflow, the draws the hook was handed and the views resolved");
    VrCensusDepthCache cache;
    for (uintptr_t i = 1; i <= VrCensusDepthCache::kSize; ++i) cache.put(reinterpret_cast<const void*>(i * 0x100), i % 2 == 0, VrCensusJoinDepth::Eye0, 2620, 2533);
    check(cache.find(reinterpret_cast<const void*>(0x300)) && cache.find(reinterpret_cast<const void*>(0x300))->relevant == false && cache.find(reinterpret_cast<const void*>(0x400))->relevant &&
              cache.find(reinterpret_cast<const void*>(0x900)) == nullptr,
          "the depth cache remembers each view's answer, relevant or not");
    cache.put(reinterpret_cast<const void*>(0x900), true, VrCensusJoinDepth::Screen, 5040, 2835);
    check(cache.find(reinterpret_cast<const void*>(0x900)) && cache.find(reinterpret_cast<const void*>(0x800)) == nullptr,
          "a ninth view replaces the last (the cache is for the handful of depth targets a frame binds)");
    cache.clear();
    check(cache.find(reinterpret_cast<const void*>(0x100)) == nullptr, "...and clear() forgets them");

    // Matching the join's rows to the calls' composed rows (the reader's join rule, 1e-5).
    f.reset();
    Model eyeModel(3, 1.5708f, 0.95f, 0.13f, -0.05f), worldModel(3, 1.0122f, 5040.0f / 2835.0f, 0.0f, 0.0f);
    float eyeRows[16], worldRows[16];
    check(vrCensusComposeRows(eyeModel.snap(), eyeRows) && vrCensusComposeRows(worldModel.snap(), worldRows), "the two models compose rows");
    for (int i = 0; i < 5; ++i) {   // five calls: world, eye, eye again (the same pose twice), a call with no rows, world
        VrCensusCall* c = f.add(true, i == 1 || i == 2 ? 5 : 3, 0x594E13);
        c->kind = i == 1 || i == 2 ? 5 : 3;
        c->kindReadable = true;
        c->rowsValid = i != 3;
        std::memcpy(c->rows, i == 1 || i == 2 ? eyeRows : worldRows, sizeof(eyeRows));
    }
    uint32_t ordinals[6] = {};
    check(vrCensusMatchRows(f, eyeRows, kVrCensusRowsTol, ordinals, 6) == 2 && ordinals[0] == 2 && ordinals[1] == 3,
          "rows equal to a call's composed rows match it: the two calls that composed the eye's rows, by their 1-based ordinals");
    float nudged[16];
    std::memcpy(nudged, eyeRows, sizeof(nudged));
    nudged[0] += 9.0e-6f;
    const uint32_t near1 = vrCensusMatchRows(f, nudged, kVrCensusRowsTol, ordinals, 6);
    nudged[0] += 2.0e-5f;
    check(near1 == 2 && vrCensusMatchRows(f, nudged, kVrCensusRowsTol, ordinals, 6) == 0, "9e-6 apart still matches, 3e-5 apart does not (the reader's tolerance, 1e-5)");
    check(vrCensusMatchRows(f, worldRows, kVrCensusRowsTol, ordinals, 1) == 2 && ordinals[0] == 1, "a list shorter than the match count still counts them all (2 matched, 1 listed)");
    VrCensusEpisodeFrame& g = f;
    for (uint32_t i = 0; i < 40; ++i) { VrCensusCall* c = g.add(true, 3, 0x1); c->rowsValid = true; std::memcpy(c->rows, worldRows, sizeof(worldRows)); }
    check(vrCensusMatchRows(g, worldRows, kVrCensusRowsTol, ordinals, 6) == 42, "forty more calls with the same rows: 42 matched, six listed");
    float nan16[16];
    for (float& v : nan16) v = std::nanf("");
    check(vrCensusMatchRows(g, nan16, kVrCensusRowsTol, ordinals, 6) == 0, "NaN rows match nothing");

    // The pass's chosen rows against the calls' view axes: identity, transposed, nothing near.
    f.reset();
    const float axesA[12] = {0.9f, 0.1f, -0.2f, 5.0f, 0.3f, 0.8f, 0.1f, 6.0f, -0.1f, 0.2f, 0.95f, 7.0f};   // rotation lanes 0..2 of three rows; lane 3 differs on purpose
    float axesT[12];
    for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) axesT[r * 4 + c] = axesA[c * 4 + r]; axesT[r * 4 + 3] = 0.0f; }
    float chosenA[12];
    std::memcpy(chosenA, axesA, sizeof(chosenA));
    chosenA[3] = chosenA[7] = chosenA[11] = 0.0f;   // the translation lane is not compared
    for (int i = 0; i < 4; ++i) {
        VrCensusCall* c = f.add(true, i == 2 ? 5 : 3, 0x594E13);
        c->axesValid = i != 3;
        std::memcpy(c->axes, i == 1 ? axesT : axesA, sizeof(axesA));   // call 2 holds the transposed matrix, 1 and 3 the plain one, 4 has no axes
        if (i == 0) c->axes[0] += 0.5f;                                // call 1 is the plain one moved well away
    }
    VrCensusAxesMatch m = vrCensusMatchAxes(f, chosenA, kVrCensusAxesTol);
    check(m.count == 2 && m.ordinals[0] == 2 && m.ordinals[1] == 3 && m.nearest == 3 && m.nearestDiff == 0.0f && !m.nearestTransposed,
          "the rows equal call 3's axes exactly and call 2's transposed: both match, the exact one (3) is the nearest and the relation is named identity");
    VrCensusAxesMatch t = vrCensusMatchAxes(f, axesT, kVrCensusAxesTol);
    check(t.count >= 1 && t.nearest != 0 && t.nearestDiff == 0.0f, "rows that are the transpose of a call's axes match it under the transposed reading (distance 0)");
    float far[12];
    std::memcpy(far, chosenA, sizeof(far));
    far[0] += 0.05f;
    far[5] += 0.05f;
    const VrCensusAxesMatch farMatch = vrCensusMatchAxes(f, far, kVrCensusAxesTol);
    check(farMatch.count == 0 && farMatch.nearest != 0 && farMatch.nearestDiff > 0.04f && farMatch.nearestDiff < 0.06f,
          "rows 0.05 away from every call match none, and the nearest call and how near it is are still said");
    std::unique_ptr<VrCensusEpisodeFrame> emptyFrame(new VrCensusEpisodeFrame);
    emptyFrame->reset();
    const VrCensusAxesMatch none = vrCensusMatchAxes(*emptyFrame, chosenA, kVrCensusAxesTol);
    check(none.count == 0 && none.nearest == 0, "a frame with no call that has axes has no nearest call");
    float nanRows[12];
    for (float& v : nanRows) v = std::nanf("");
    const VrCensusAxesMatch nan = vrCensusMatchAxes(f, nanRows, kVrCensusAxesTol);
    check(nan.count == 0, "NaN rows equal no call's axes");
    std::memcpy(far, chosenA, sizeof(far));
    far[3] = 99.0f;
    check(vrCensusMatchAxes(f, far, kVrCensusAxesTol).count == 2, "the translation lane (3, 7, 11) is not compared: it changes nothing");
    {   // The tolerance is inclusive (0.75 and 1.0 are exact in a float, so the difference is exactly 0.25).
        std::unique_ptr<VrCensusEpisodeFrame> edgeFrame(new VrCensusEpisodeFrame);
        edgeFrame->reset();
        VrCensusCall* edgeCall = edgeFrame->add(true, 3, 0x1);
        edgeCall->axesValid = true;
        std::memset(edgeCall->axes, 0, sizeof(edgeCall->axes));
        edgeCall->axes[0] = 1.0f;
        float edge[12] = {};
        edge[0] = 0.75f;
        check(vrCensusMatchAxes(*edgeFrame, edge, 0.25f).count == 1 && vrCensusMatchAxes(*edgeFrame, edge, 0.2499f).count == 0,
              "the axes tolerance is inclusive: a difference of exactly the tolerance matches, a hair under it does not");
    }

    // The selection: which calls print when the lines are fewer than the calls.
    std::unique_ptr<VrCensusEpisodeFrame> sp(new VrCensusEpisodeFrame);
    VrCensusEpisodeFrame& big = *sp;
    big.reset();
    for (uint32_t i = 0; i < 500; ++i) {   // 400 kind-1 calls from one caller, 90 kind-3 calls whose caller alternates (each its own run), then ten kind-5 calls of ONE run
        const uint32_t caller = i < 400 ? 0x58DE73 : i < 490 ? 0x594E13 + (i & 1) : 0x594FE1;
        VrCensusCall* c = big.add(true, i < 400 ? 1 : i < 490 ? 3 : 5, caller);
        c->kind = i < 400 ? 1 : i < 490 ? 3 : 5;
        c->kindReadable = true;
        c->callerRva = caller;
        c->tone = i < 450 ? VrCensusTone::Before : VrCensusTone::After;
    }
    bool matched[640] = {}, selected[640] = {};
    matched[17] = matched[333] = true;
    const uint32_t picked = vrCensusSelectCalls(big, matched, 120, selected);
    uint32_t count = 0, kind5 = 0;
    for (uint32_t i = 0; i < 500; ++i) { if (selected[i]) ++count; if (selected[i] && big.call[i].kind == 5) ++kind5; }
    check(picked == 120 && count == 120 && selected[17] && selected[333] && kind5 == 10,
          "of 500 calls and 120 lines: the two that matched something print, and every kind-5 call (ten of them), whatever place they are in");
    bool runStarts = selected[0] && selected[400] && selected[450];
    check(runStarts, "...and the first call of each run of one (kind, caller, tone) too, so the runs the reader reduces are all there");
    check(picked == count && selected[0] && selected[1] && selected[2], "...and the rest of the lines go to the first calls in order");
    bool few[640] = {}, fewSel[640] = {};
    const uint32_t all = vrCensusSelectCalls(f, few, 120, fewSel);
    check(all == f.recorded && fewSel[0] && fewSel[f.recorded - 1], "a frame with fewer calls than lines prints every one");
    matched[17] = matched[333] = false;
    bool mixed[640] = {};
    for (uint32_t i = 0; i < 200; ++i) mixed[i] = true;
    const uint32_t capped = vrCensusSelectCalls(big, mixed, 120, selected);
    check(capped == 120 && selected[119] && !selected[120] && !selected[150] && !selected[495],
          "when the matched calls alone exceed the lines the first of them take them, and not even a kind-5 call is added past the cap: never more lines than the cap");
    {   // A run is (kind, caller, tone): calls that differ only in the tone are two runs.
        std::unique_ptr<VrCensusEpisodeFrame> tp(new VrCensusEpisodeFrame);
        tp->reset();
        for (uint32_t i = 0; i < 20; ++i) {
            VrCensusCall* c = tp->add(true, 3, 0x594E13);
            c->kind = 3;
            c->kindReadable = true;
            c->callerRva = 0x594E13;
            c->tone = i < 10 ? VrCensusTone::Before : VrCensusTone::After;
        }
        bool noMatch[640] = {}, toneSel[640] = {};
        const uint32_t two = vrCensusSelectCalls(*tp, noMatch, 2, toneSel);
        check(two == 2 && toneSel[0] && toneSel[10] && !toneSel[1],
              "a change of the tone alone starts a run: with two lines the first call and the first call after the tone changed print, not the second call");
    }
}

// One whole episode, printed by the function the DLL prints it with: the order of its lines, the cap on call lines, and the matches made over ALL the recorded calls.
void testEpisodePrint() {
    std::printf("episodes: one episode printed\n");
    std::unique_ptr<VrCensusEpisodeFrame> fp(new VrCensusEpisodeFrame);
    VrCensusEpisodeFrame& f = *fp;
    f.reset();
    Model eye(3, 1.5708f, 0.95f, 0.13f, -0.05f), scene(3, 1.0122f, 5040.0f / 2835.0f, 0.0f, 0.0f);
    float eyeRows[16], sceneRows[16];
    check(vrCensusComposeRows(eye.snap(), eyeRows) && vrCensusComposeRows(scene.snap(), sceneRows), "two cameras compose rows");
    float sceneAxes[12], eyeAxes[12];
    {   // the cameras' axes as the census snapshots them (camera+0x20)
        const VrCensusSnap sn = scene.snap();
        std::memcpy(sceneAxes, sn.bytes, sizeof(sceneAxes));
        std::memcpy(eyeAxes, sn.bytes, sizeof(eyeAxes));
        eyeAxes[1] += 0.2f;   // the eye's pose is not the scene camera's
    }
    // 300 recorded calls: 284 kind-1 calls (each its own rows and axes), the scene camera's call at n=250 (the one the join's rows and the pass's rows match), and fifteen kind-5 calls
    // from n=286 (the eye camera's, with rows and axes of their own).
    for (uint32_t i = 0; i < 300; ++i) {
        const bool isScene = i == 249, isEye = i >= 285;
        VrCensusCall* c = f.add(true, isEye ? 5 : isScene ? 3 : 1, isEye ? 0x594FE1 : 0x58DE73);
        c->kind = isEye ? 5 : isScene ? 3 : 1;
        c->kindReadable = true;
        c->callerRva = isEye ? 0x594FE1 : 0x58DE73;
        c->tone = isEye ? VrCensusTone::After : VrCensusTone::Before;
        c->rowsValid = true;
        std::memcpy(c->rows, isScene ? sceneRows : eyeRows, sizeof(c->rows));
        if (!isScene && !isEye) c->rows[0] += 0.5f + 0.001f * static_cast<float>(i);   // a UI call's rows are its own
        c->axesValid = true;
        std::memcpy(c->axes, isScene ? sceneAxes : eyeAxes, sizeof(c->axes));
        if (!isScene && !isEye) c->axes[0] += 0.3f + 0.001f * static_cast<float>(i);
    }
    VrCensusJoin join;
    join.begin();
    VrCensusJoinRow* a = join.add(VrCensusJoinDepth::Screen, 5040, 2835, 0xD1, 0xE1, 41);
    a->draws = 22; a->depthWrite = 1; a->b1Bound = true; a->b1 = 0x1eb2e751e20ull; a->b1Bytes = 5376; a->rowsAsked = a->rowsRead = true;
    std::memcpy(a->rows, sceneRows, sizeof(a->rows));
    VrCensusJoinRow* b = join.add(VrCensusJoinDepth::Screen, 5040, 2835, 0xD2, 0xE1, 77);
    b->draws = 300; b->depthWrite = 0; b->b1Bound = true; b->b1 = 0x1eb2e751e20ull; b->b1Bytes = 5376; b->why = "skip";
    join.seen = 900; join.views = 3;
    VrCensusEpisodePrint in;
    in.n = 7; in.frame = 400; in.armedFrame = 370; in.trigger = VrCensusTrigger{VrCensusTriggerKind::Naming, 1, 0};
    in.foot = VrCensusFoot::Yes; in.named = false; in.haveChosen = true; in.chosenBound = true;
    std::memcpy(in.chosen, sceneAxes, sizeof(in.chosen));   // the pass chose the scene camera's rows
    std::vector<std::pair<VrCensusLines, std::string>> out;
    vrCensusPrintEpisode([&](VrCensusLines cls, const char* text) { out.emplace_back(cls, text); }, in, f, join);
    unsigned callLines = 0, tooLong = 0;
    bool has250 = false, has286 = false;
    size_t firstCall = out.size(), lastCall = 0, passAt = out.size(), drawsAt = out.size(), joinAt = out.size();
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i].second.size() > kVrCensusLineBytes) ++tooLong;
        if (out[i].first == VrCensusLines::EpisodeCall) {
            ++callLines;
            if (firstCall == out.size()) firstCall = i;
            lastCall = i;
            has250 = has250 || out[i].second.find(" n=250 ") != std::string::npos;
            has286 = has286 || out[i].second.find(" n=286 ") != std::string::npos;
        }
        if (out[i].second.find("pass-rows ep=7 ") != std::string::npos) passAt = i;
        if (out[i].second.find("join-draws ep=7 ") != std::string::npos) drawsAt = i;
        if (out[i].second.find("join ep=7 sig=1 ") != std::string::npos) joinAt = i;
    }
    check(out.size() > 0 && out[0].first == VrCensusLines::Episode &&
              out[0].second.find("vr camera census: episode frame=400 n=7/10 trigger=naming:named>unnamed armed=370 foot=yes gui=- named=0 phase=- calls=300 recorded=300 printed=120 ") == 0,
          "the first line is the episode's header, and says 300 calls recorded and 120 printed");
    check(callLines == 120 && tooLong == 0, "the call lines are capped at 120 (kVrCensusEpisodeLines) and no line is over 400 characters");
    check(has250 && has286, "the scene camera's call (n=250) that the join's rows and the pass's rows matched is printed though it is past the 120th call, and so is a kind-5 call (n=286)");
    check(firstCall < passAt && lastCall < passAt && passAt < drawsAt && drawsAt < joinAt,
          "the order is the header, the call lines, the pass's rows, the join's draw counts, then the join's signatures");
    unsigned sceneJoin = 0;
    for (const auto& l : out) if (l.second.find("join ep=7 sig=1 ") != std::string::npos && l.second.find("rows=read match=250") != std::string::npos) ++sceneJoin;
    unsigned passLine = 0;
    for (const auto& l : out) if (l.second.find("pass-rows ep=7 frame=400 valid=1 bound=1 ") != std::string::npos && l.second.find(" axes-match=250 how=identity nearest=250 diff=0.000e+00") != std::string::npos) ++passLine;
    check(sceneJoin == 1 && passLine == 1,
          "the join's rows matched call 250 (over all 300 recorded calls, not the printed ones) and the pass's rows equal call 250's view axes, read as they are");
    unsigned rowsLines = 0, drawsLine = 0, skipLine = 0;
    for (const auto& l : out) {
        if (l.second.find("join-rows ep=7 sig=1 ") != std::string::npos) ++rowsLines;
        if (l.second.find("join-draws ep=7 seen=900 relevant=322 views=3 signatures=2") != std::string::npos) ++drawsLine;
        if (l.second.find("join ep=7 sig=2 ") != std::string::npos && l.second.find(" rows=- why=skip") != std::string::npos) ++skipLine;
    }
    check(rowsLines == 1 && drawsLine == 1 && skipLine == 1,
          "a rows line only for the signature that read its rows; the draw counts say 900 seen, 322 relevant (22 + 300), 3 views, 2 signatures; the second signature says why=skip");
    // The cap on the Episode class is a real ceiling: the lines of one episode never exceed 1 + 1 + 1 + 12 + 4 + 1 (the join-more note) of that class.
    VrCensusJoin full;
    full.begin();
    for (uint32_t i = 0; i < kVrCensusJoinRows + 3; ++i) {
        VrCensusJoinRow* r = full.add(VrCensusJoinDepth::Eye0, 2620, 2533, 0x100 + i, 0x200, 10 + i);
        if (r) { r->draws = 2; r->rowsAsked = r->rowsRead = i < 4; r->why = i < 4 ? nullptr : "skip"; std::memcpy(r->rows, eyeRows, sizeof(r->rows)); }
    }
    full.seen = 50;
    f.reset();
    std::vector<std::pair<VrCensusLines, std::string>> out2;
    in.haveChosen = false;
    vrCensusPrintEpisode([&](VrCensusLines cls, const char* text) { out2.emplace_back(cls, text); }, in, f, full);
    unsigned episodeClass = 0;
    bool more = false, noCalls = true;
    for (const auto& l : out2) {
        if (l.first == VrCensusLines::Episode) ++episodeClass;
        if (l.first == VrCensusLines::EpisodeCall) noCalls = false;
        if (l.second.find("join-more ep=7 signatures=3 draws=3") != std::string::npos) more = true;
    }
    check(more && noCalls && episodeClass == 1 + 1 + 1 + kVrCensusJoinRows + 4 + 1 && out2[1].second.find("pass-rows ep=7 frame=400 valid=0 ") != std::string::npos,
          "an episode of a frame with no call, fifteen signatures (twelve kept, four of them with rows) and no pass rows prints its header, pass-rows valid=0, join-draws, the twelve "
          "signatures, four rows lines and the overflow note: exactly the Episode class's per-episode ceiling, and no call line");
}

// The episodes' part of a boundary, in the order the glue runs it (vr_camera_census.cpp): the frame that ended is rolled (an episode's frame prints and finishes),
// then the triggers its readings make are judged and the armed frame goes live. The recording rule of a call is the glue's: an episode frame records every call
// into the episode buffer, any other frame by the old rule into the ordinary one.
struct EpisodeSim {
    VrCensusEpisodes eps;
    std::unique_ptr<VrCensusEpisodeFrame> epFrame{new VrCensusEpisodeFrame};
    VrCensusFrame frame;
    VrCensusFoot foot = VrCensusFoot::No;
    bool named = false, guiKnown = false;
    uint32_t gui = 0;
    uint64_t frameNo = 1;
    uint32_t sequencesLogged = 0;
    std::vector<uint64_t> printedEpisodeFrames;       // the frame numbers of the episodes that printed
    std::vector<uint32_t> printedEpisodeRecorded;     // ...and how many calls each recorded
    std::vector<uint64_t> printedSequenceFrames;
    EpisodeSim() { eps.restart(); eps.keyOn(frameNo); epFrame->reset(); }   // the census turned on: frame 1 starts, the key-on trigger arms for frame 31
    void call(uint32_t kind, uint32_t caller, bool tone) {
        const bool episodeFrame = eps.live();
        const bool recording = !episodeFrame && vrCensusMayRecord(foot, VrCensusPhase{}) && vrCensusPrintsSequence(true, sequencesLogged);
        if (episodeFrame) { ++frame.calls; epFrame->add(true, kind, caller); }
        else if (recording) frame.add(); else ++frame.calls;
        frame.progress = true;
        if (tone) frame.toneSeen = true;
    }
    void boundary() {
        const bool sampled = vrCensusSamplesFrame(frame.toneSeen, frame.progress, foot, VrCensusPhase{});
        if (vrCensusPrintsSequence(sampled, sequencesLogged) && frame.recorded > 0) { ++sequencesLogged; printedSequenceFrames.push_back(frameNo); }
        if (eps.live()) { printedEpisodeFrames.push_back(frameNo); printedEpisodeRecorded.push_back(epFrame->recorded); eps.finish(); }
        frame.reset();
        ++frameNo;
        VrCensusEpisodes::Inputs in;
        in.foot = foot; in.named = named; in.guiKnown = guiKnown; in.gui = gui;
        if (eps.boundary(frameNo, frameNo > 1, in)) epFrame->reset();
    }
};

void testEpisodeSession() {
    std::printf("episodes: a scripted session through the core\n");
    // The key-on episode: a cockpit frame (the journal says not on foot), every call of it recorded -- the ordinary rule would record none.
    {
        EpisodeSim sim;
        bool ordinaryRecordedNothing = true, sampledAllCalls = false;
        for (int f = 1; f <= 40; ++f) {
            for (int i = 0; i < 300; ++i) sim.call(i % 3 == 0 ? 5 : 3, 0x594E13, i > 250);
            ordinaryRecordedNothing = ordinaryRecordedNothing && sim.frame.recorded == 0;
            if (f == 31) sampledAllCalls = sim.epFrame->calls == 300 && sim.epFrame->recorded == 300 && sim.epFrame->kinds[5] == 100 && sim.epFrame->kinds[3] == 200;
            sim.boundary();
        }
        check(ordinaryRecordedNothing, "an aboard frame records nothing by the ordinary rule (the journal says not on foot), the sampled one included");
        check(sampledAllCalls, "...and the sampled frame, aboard, recorded and tallied all 300 of its calls into the episode buffer, kind 5 and kind 3 apart");
        check(sim.printedEpisodeFrames.size() == 1 && sim.printedEpisodeFrames[0] == 31 && sim.printedEpisodeRecorded[0] == 300 && sim.printedSequenceFrames.empty() && sim.eps.started() == 1,
              "one episode, of frame 31 (30 frames after the key went on), and no first-three sequence: the commander was never on foot");
    }
    // Triggers: a map opens (GuiFocus 0 -> 6) while the key-on episode is still armed (skipped), a map closes after it was taken (armed and sampled).
    {
        EpisodeSim sim;
        sim.guiKnown = true;
        for (int f = 1; f <= 100; ++f) {
            if (f == 10) sim.gui = 6;     // read at the boundary that ends frame 10: a trigger, but the key-on episode is armed until frame 31
            if (f == 50) sim.gui = 0;     // the map closes: read at the boundary that ends frame 50, armed for frame 51, sampled at 81
            for (int i = 0; i < 10; ++i) sim.call(3, 0x594E13, false);
            sim.boundary();
        }
        check(sim.eps.started() == 2 && sim.eps.triggers() == 3 && sim.eps.skipped() == 1 && sim.printedEpisodeFrames.size() == 2 &&
                  sim.printedEpisodeFrames[0] == 31 && sim.printedEpisodeFrames[1] == 81,
              "key-on (sampled at frame 31), a map opening (GuiFocus 0 -> 6) while it was armed (skipped), a map closing (6 -> 0, sampled at frame 81): started 2, triggers 3, skipped 1");
    }
    // An on-foot frame the first-three rule would have recorded, when it is the episode's frame: the episode takes it, the ordinary buffer records nothing and the frame
    // spends none of the three sequences (the next on-foot frame prints as the first).
    {
        EpisodeSim sim;
        sim.foot = VrCensusFoot::Yes;
        for (int f = 1; f <= 34; ++f) {
            for (int i = 0; i < 20; ++i) sim.call(3, 0x594E13, i >= 16);
            sim.boundary();
        }
        check(sim.printedEpisodeFrames.size() == 1 && sim.printedEpisodeFrames[0] == 31 && sim.sequencesLogged == 3 &&
                  std::find(sim.printedSequenceFrames.begin(), sim.printedSequenceFrames.end(), 31ull) == sim.printedSequenceFrames.end() &&
                  sim.printedSequenceFrames[0] == 1 && sim.printedSequenceFrames[1] == 2 && sim.printedSequenceFrames[2] == 3,
              "on foot the first three frames print as sequences (1, 2, 3) as before; the key-on episode's frame (31) is the episode's only: no duplicate lines, no sequence spent");
        EpisodeSim late;
        late.foot = VrCensusFoot::Yes;
        late.sequencesLogged = 2;   // two of the three already printed
        for (int f = 1; f <= 33; ++f) {
            for (int i = 0; i < 20; ++i) late.call(3, 0x594E13, i >= 16);
            late.boundary();
        }
        check(late.printedEpisodeFrames.size() == 1 && late.sequencesLogged == 3 && late.printedSequenceFrames.size() == 1 && late.printedSequenceFrames[0] == 1,
              "with one sequence left, the episode's frame (31) does not take it: frame 1 prints it as before");
    }
}

// ---------------------------------------------------------------------------
// The text of every line.
// ---------------------------------------------------------------------------
struct Golden { const char* name; std::string text; };

// The lines tools\edvr_log.py's --camera-census fixture carries are built here from fixed inputs, so a drift of either
// side breaks a build: this rig pins the exact text, and the next group finds each line in the reader's own source.
std::vector<Golden> goldenLines() {
    std::vector<Golden> out;
    char line[kVrCensusLineBytes + 1];
    auto add = [&](const char* name) { out.push_back({name, line}); };

    VrCensusWindow w;
    w.frames = 450; w.calls = 10012; w.posts = 10012; w.stale = 0; w.injCalls = 6750;
    w.kinds[0] = 1800; w.kinds[1] = 600; w.kinds[3] = 7612;
    w.callers[0] = {0x594E13, 3337}; w.callers[1] = {0x594EAB, 3337}; w.callers[2] = {0x594FE1, 3337}; w.callers[3] = {0x58DE73, 1}; w.callerCount = 4;
    w.cameraCount = 14; w.toneBefore = 9000; w.toneAfter = 1012; w.toneNone = 0;
    w.toneFrames = 450; w.onFootFrames = 450; w.eyeDraws = 900; w.eyeOnFoot = 900; w.windows = 1; w.progressSeen = true;
    VrCensusWindowText text;
    text.hook = "installed"; text.camerasTotal = 14; text.foot = VrCensusFoot::Yes;
    vrCensusFormatWindow(line, sizeof(line), w, text);
    add("window");

    VrCensusCamera world;
    world.camera = 0x241dc2e2960;
    world.firstSig = sigOf(3, 5040.0f / 2835.0f, 0.025f, 1.0122f);
    world.firstSig.p0 = 0.9f; world.firstSig.flags = 0;
    world.tanValid = true; world.tan[0] = -0.5625f; world.tan[1] = 0.5625f; world.tan[2] = -0.3164f; world.tan[3] = 0.3164f;
    world.callerRva = 0x594E13; world.firstOrdinal = 1; world.firstDraw = 0; world.firstDrawKnown = true;
    world.firstTone = VrCensusTone::Before; world.firstFrame = 1;
    world.firstView = 0x241dd00a000; world.firstCtx = 0x241dd00a100;
    vrCensusFormatCamera(line, sizeof(line), world);
    add("camera-world");

    VrCensusCamera eye;
    eye.camera = 0x241df6d0bb0;
    eye.firstSig = sigOf(3, 0.9f, 0.05f, 1.3f, -0.03f, 0.011f);
    eye.firstSig.viewportW = 2620; eye.firstSig.viewportH = 2533;
    eye.tanValid = true; eye.tan[0] = -1.2f; eye.tan[1] = 0.7f; eye.tan[2] = -0.9f; eye.tan[3] = 1.1f;
    eye.callerRva = 0x594FE1; eye.firstOrdinal = 98; eye.firstDraw = 8210; eye.firstDrawKnown = true;
    eye.firstTone = VrCensusTone::After; eye.firstFrame = 1;
    eye.firstView = 0x241dd00e000; eye.firstCtx = 0x241dd00e100;
    vrCensusFormatCamera(line, sizeof(line), eye);
    add("camera-eye");

    VrCensusCamera moved = eye;
    moved.changeFrom = eye.firstSig;
    moved.sig = eye.firstSig;
    moved.sig.boundX = -0.0297f; moved.sig.nearZ = 0.06f;
    moved.changeFrame = 2; moved.changes = 7;
    vrCensusFormatChanged(line, sizeof(line), moved);
    add("changed");

    const VrCensusPhase jitter{true, 0.252f, -0.126f};
    vrCensusFormatSequence(line, sizeof(line), 4, 1, VrCensusFoot::Yes, jitter, 107, 107);
    add("sequence");
    vrCensusFormatSequence(line, sizeof(line), 5, 2, VrCensusFoot::Off, VrCensusPhase{}, 107, 100);
    add("sequence-no-route");

    VrCensusCall call;
    call.camera = 0x241dc2e2960; call.view = 0x241dd00a000; call.kind = 3; call.kindReadable = true; call.callerRva = 0x594E13; call.draw = 6500; call.drawKnown = true;
    call.tone = VrCensusTone::Before; call.preFlags = 0x1C; call.postFlags = 0; call.postSeen = true; call.rowsValid = true;
    call.willInject = true; call.role = 0;
    const float rowsWorld[16] = {0.5625f, 0, 0, 0.8f, 0, 1.0f, 0, -0.1f, 0.2f, 0, 1.0f, 0.6f, 0, 0, 0.025f, 0};
    std::memcpy(call.rows, rowsWorld, sizeof(rowsWorld));
    vrCensusFormatCall(line, sizeof(line), 4, 1, call);
    add("call-world");
    VrCensusCall firstPerson = call;
    firstPerson.role = 1;
    vrCensusFormatCall(line, sizeof(line), 4, 13, firstPerson);
    add("call-first-person");
    VrCensusCall aux = call;
    aux.willInject = false; aux.role = 2; aux.callerRva = 0x58DE73;
    vrCensusFormatCall(line, sizeof(line), 4, 14, aux);
    add("call-aux");
    VrCensusCall eyeCall = call;
    eyeCall.camera = 0x241df6d0bb0; eyeCall.view = 0x241dd00e000; eyeCall.callerRva = 0x594FE1; eyeCall.draw = 8210; eyeCall.tone = VrCensusTone::After;
    eyeCall.kind = 5; eyeCall.willInject = false; eyeCall.role = kVrCensusRoleNone;
    const float rowsEye[16] = {1.1f, 0, 0, 0.8f, 0, 1.0f, 0, -0.1f, 0.2f, 0, 1.0f, 0.6f, 0, 0, 0.05f, 0};
    std::memcpy(eyeCall.rows, rowsEye, sizeof(rowsEye));
    vrCensusFormatCall(line, sizeof(line), 4, 98, eyeCall);
    add("call-eye");
    VrCensusCall ortho = call;
    ortho.camera = 0x241de000100; ortho.view = 0x241dd00c000; ortho.kind = 1; ortho.callerRva = 0x58DE73; ortho.draw = 0; ortho.drawKnown = false;
    ortho.tone = VrCensusTone::None; ortho.postSeen = false; ortho.rowsValid = false; ortho.willInject = false; ortho.role = kVrCensusRoleNone;
    vrCensusFormatCall(line, sizeof(line), 4, 2, ortho);
    add("call-ortho");

    vrCensusFormatEye(line, sizeof(line), 0, 4, VrCensusFoot::Yes, jitter, true, 8213, 0x1eb2e751e20ull, 0, 5376, rowsEye, true, 0.0002, -0.0001, nullptr);
    add("eye");
    vrCensusFormatEye(line, sizeof(line), 1, 4, VrCensusFoot::Yes, VrCensusPhase{}, true, 8220, 0x1eb2e751e20ull, 0, 5376, nullptr, false, 0, 0, "map");
    add("eye-failed");
    const float frustum[4] = {-1.2f, 0.7f, -0.9f, 1.1f}, shift[2] = {0.0f, 0.0f};
    double trueX = 0, trueY = 0;
    vrCensusExpectedMeasure(frustum, 0.0f, 0.0f, &trueX, &trueY);   // the rows of an unshifted, unleaked eye measure exactly this
    vrCensusFormatEyeGeometry(line, sizeof(line), 0, 4, true, 4711, frustum, shift, true, trueX, trueY);
    add("eye-geometry");
    vrCensusFormatEyeGeometry(line, sizeof(line), 1, 4, false, 0, nullptr, nullptr, false, 0, 0);
    add("eye-geometry-unavailable");

    VrCensusOffThread::Entry other;
    other.thread = 4321; other.camera = 0x241dc2e2960; other.kind = 3; other.kindReadable = true; other.callerRva = 0x594E13; other.calls = 57;
    vrCensusFormatOtherThread(line, sizeof(line), other);
    add("other-thread");

    // The episodes' lines (Phase 0). A cockpit episode 30 frames after the galaxy map opened (GuiFocus 0 -> 6), and an on-foot one after the naming flipped to unnamed.
    VrCensusEpisodeHeader eh;
    eh.n = 3; eh.frame = 5231; eh.armedFrame = 5201;
    eh.trigger = VrCensusTrigger{VrCensusTriggerKind::Gui, 0, 6};
    eh.foot = VrCensusFoot::No; eh.guiKnown = true; eh.gui = 6; eh.named = false;
    eh.calls = 612; eh.recorded = 612; eh.printed = 120;
    eh.kinds[0] = 12; eh.kinds[1] = 300; eh.kinds[3] = 290; eh.kinds[5] = 10;
    const VrCensusEpisodeFrame::Caller ehCallers[5] = {{0x594E13, 130}, {0x594EAB, 130}, {0x594FE1, 40}, {0x58DE73, 300}, {0x59A010, 12}};
    eh.callers = ehCallers; eh.callerCount = 5;
    vrCensusFormatEpisode(line, sizeof(line), eh);
    add("episode-cockpit");
    VrCensusEpisodeHeader ef;
    ef.n = 4; ef.frame = 6120; ef.armedFrame = 6090;
    ef.trigger = VrCensusTrigger{VrCensusTriggerKind::Naming, 1, 0};
    ef.foot = VrCensusFoot::Yes; ef.guiKnown = false; ef.named = false; ef.phase = jitter;
    ef.calls = 107; ef.recorded = 107; ef.printed = 107;
    ef.kinds[1] = 8; ef.kinds[3] = 93; ef.kinds[5] = 6;
    const VrCensusEpisodeFrame::Caller efCallers[3] = {{0x594E13, 40}, {0x594EAB, 40}, {0x594FE1, 27}};
    ef.callers = efCallers; ef.callerCount = 3;
    vrCensusFormatEpisode(line, sizeof(line), ef);
    add("episode-foot");
    VrCensusEpisodeHeader ek;
    ek.n = 1; ek.frame = 31; ek.armedFrame = 1; ek.trigger = VrCensusTrigger{VrCensusTriggerKind::KeyOn, 0, 0};
    ek.foot = VrCensusFoot::Off; ek.calls = 0; ek.recorded = 0; ek.printed = 0;
    vrCensusFormatEpisode(line, sizeof(line), ek);
    add("episode-key-on-empty");
    VrCensusEpisodeHeader eg = ek;
    eg.trigger = VrCensusTrigger{VrCensusTriggerKind::Foot, 0, 1};
    vrCensusFormatEpisode(line, sizeof(line), eg);
    add("episode-foot-flip");

    const float passRows[12] = {0.9f, 0, -0.1f, 0.5f, 0, 1.0f, 0, -1.5f, 0.1f, 0, 0.9f, 2.5f};
    VrCensusAxesMatch pm;
    pm.count = 2; pm.ordinals[0] = 98; pm.ordinals[1] = 101; pm.nearest = 98; pm.nearestDiff = 0.0f;
    vrCensusFormatPassRows(line, sizeof(line), 3, 5231, true, true, passRows, pm);
    add("pass-rows");
    VrCensusAxesMatch pn;
    pn.nearest = 17; pn.nearestDiff = 1.5e-3f; pn.nearestTransposed = true;
    vrCensusFormatPassRows(line, sizeof(line), 3, 5231, true, false, passRows, pn);
    add("pass-rows-no-match");
    vrCensusFormatPassRows(line, sizeof(line), 3, 5231, false, false, nullptr, VrCensusAxesMatch{});
    add("pass-rows-none");

    VrCensusJoinRow jr;
    jr.depth = VrCensusJoinDepth::Eye0; jr.w = 2620; jr.h = 2533; jr.vs = 0x5C36AF051B98B9F1ull; jr.ps = 0xCFE84157BC76E921ull; jr.firstDraw = 8210; jr.draws = 1432;
    jr.depthWrite = 1; jr.b1Bound = true; jr.b1 = 0x1eb2e751e20ull; jr.b1First = 0; jr.b1Bytes = 5376; jr.rowsAsked = true; jr.rowsRead = true;
    jr.matchCount = 2; jr.matches[0] = 98; jr.matches[1] = 101;
    std::memcpy(jr.rows, rowsEye, sizeof(rowsEye));
    vrCensusFormatJoin(line, sizeof(line), 3, 1, jr);
    add("join");
    vrCensusFormatJoinRows(line, sizeof(line), 3, 1, jr.rows);
    add("join-rows");
    VrCensusJoinRow js;
    js.depth = VrCensusJoinDepth::Screen; js.w = 5040; js.h = 2835; js.vs = 0xDFED8E1C9E191BECull; js.ps = 0x143AAE0597E2F7BFull; js.firstDraw = 6500; js.draws = 22;
    js.depthWrite = 0; js.b1Bound = true; js.b1 = 0x1eb2e751e20ull; js.b1Bytes = 5376; js.why = "skip";
    vrCensusFormatJoin(line, sizeof(line), 3, 2, js);
    add("join-skip");
    VrCensusJoinRow jn;
    jn.depth = VrCensusJoinDepth::EyeUnknown; jn.w = 2620; jn.h = 2533; jn.firstDraw = 91; jn.draws = 3; jn.rowsAsked = true; jn.why = "no-b1";
    vrCensusFormatJoin(line, sizeof(line), 3, 3, jn);
    add("join-no-b1");
    VrCensusJoinRow jt = jr;
    jt.depth = VrCensusJoinDepth::Eye1; jt.matchCount = 0;
    vrCensusFormatJoin(line, sizeof(line), 3, 4, jt);
    add("join-no-match");
    vrCensusFormatJoinMore(line, sizeof(line), 3, 4, 212);
    add("join-more");
    vrCensusFormatJoinDraws(line, sizeof(line), 3, 95, 75, 4, 4);
    add("join-draws");
    vrCensusFormatJoinDraws(line, sizeof(line), 4, 0, 0, 0, 0);
    add("join-draws-none");

    VrCensusRuns runs;
    runs.frames = 4500;
    runs.named[0] = 1; runs.named[6] = 1;
    runs.unnamed[0] = 3; runs.unnamed[1] = 1; runs.unnamed[2] = 2; runs.unnamed[4] = 1;
    runs.longestNamed = 3012; runs.longestUnnamed = 14; runs.open = 1; runs.openLength = 3012;
    vrCensusFormatRuns(line, sizeof(line), runs, 1);
    add("runs");
    vrCensusFormatRuns(line, sizeof(line), VrCensusRuns{}, 12);
    add("runs-empty");
    VrCensusCpu cpuSums;
    cpuSums.notePre(false, 200); cpuSums.notePre(false, 300); cpuSums.notePost(false, 100); cpuSums.notePost(false, 150);
    cpuSums.notePre(true, 400); cpuSums.notePost(true, 200);
    VrCensusWindow cpuWindow;
    cpuWindow.frames = 100; cpuWindow.calls = 1000; cpuWindow.injCalls = 400;
    vrCensusFormatCpu(line, sizeof(line), cpuWindow, cpuSums, 10000000, 3);
    add("detour");
    vrCensusFormatCpu(line, sizeof(line), VrCensusWindow{}, VrCensusCpu{}, 10000000, 1);
    add("detour-empty");
    VrCensusEpisodeCounters counters;
    counters.taken = 3; counters.triggers = 5; counters.skipped = 2;
    vrCensusFormatEpisodeCounters(line, sizeof(line), counters, 1, false);
    add("episodes-idle");
    counters.state = "armed"; counters.trigger = VrCensusTrigger{VrCensusTriggerKind::Gui, 0, 6}; counters.armedFrame = 5201; counters.sampleFrame = 5231;
    vrCensusFormatEpisodeCounters(line, sizeof(line), counters, 12, true);
    add("episodes-armed");
    vrCensusFormatEpisodeCounters(line, sizeof(line), VrCensusEpisodeCounters{}, 1, false);
    add("episodes-zero");
    return out;
}

void testFormats() {
    std::printf("line text\n");
    const std::vector<Golden> g = goldenLines();
    auto at = [&](const char* name) -> std::string {
        for (const Golden& x : g) if (std::strcmp(x.name, name) == 0) return x.text;
        return std::string("<missing ") + name + ">";
    };
    check(at("window") ==
              "vr camera census 5s: frames=450 calls=10012 posts=10012 off-thread=0 stale=0 inj-calls=6750 kinds=0:1800,1:600,3:7612 "
              "callers=+0x594E13:3337,+0x594EAB:3337,+0x594FE1:3337,+0x58DE73:1 cameras-seen=14 cameras-total=14 tone=9000/1012/0 "
              "tone-frames=450 on-foot-frames=450 foot=yes eye-draws=900/900 progress=yes hook=installed windows=1 cam-overflow=0 thread-overflow=0",
          "the 5 s line: frames, calls, posts, off-thread, stale, injected calls, kinds, callers, cameras, tone split, tone frames, sampled frames, the journal, eye draws, progress, hook");
    check(at("camera-world") ==
              "vr camera census: camera=0x241dc2e2960 kind=3 caller=+0x594E13 thread=owner aspect=1.777778 near=0.025 far=50000 fov=1.0122 "
              "bound=(0,0) offcentre=(0,0) viewport=(5040,2835) tan=(-0.5625,0.5625,-0.3164,0.3164) view=0x241dd00a000 vctx=0x241dd00a100 "
              "first-call=1 draw=0 tone=before frame=1",
          "the per-camera line: kind, caller, thread, aspect, near, far, fov, bound, off-centre, viewport, tangents, view, context, first call, draw, tone, frame");
    check(at("camera-eye") ==
              "vr camera census: camera=0x241df6d0bb0 kind=3 caller=+0x594FE1 thread=owner aspect=0.9 near=0.05 far=50000 fov=1.3 "
              "bound=(-0.03,0.011) offcentre=(-0.06,0.022) viewport=(2620,2533) tan=(-1.2,0.7,-0.9,1.1) view=0x241dd00e000 vctx=0x241dd00e100 "
              "first-call=98 draw=8210 tone=after frame=1",
          "an asymmetric eye camera's line shows its bound pair and off-centre terms");
    check(at("changed") ==
              "vr camera census: changed: camera=0x241df6d0bb0 frame=2 n=7 near=0.05->0.06 bound=(-0.03,0.011)->(-0.0297,0.011)",
          "the changed line names only the fields that moved, old->new");
    check(at("sequence") == "vr camera census: sequence frame=4 index=1/3 foot=yes phase=0.2520,-0.1260 calls=107 recorded=107 truncated=0",
          "the sequence header names what the journal said and the phase the route chose for the frame (render pixels, four decimals)");
    check(at("sequence-no-route") == "vr camera census: sequence frame=5 index=2/3 foot=off phase=- calls=107 recorded=100 truncated=7",
          "a frame the route was not jittering has phase=-: a zero phase of a warm-up frame (0.0000,0.0000) and no route at all read differently");
    check(at("call-world") ==
              "vr camera census: call frame=4 n=1 camera=0x241dc2e2960 kind=3 caller=+0x594E13 draw=6500 tone=before inj=1 role=scene fl=0x1C>0x0 "
              "view=0x241dd00a000 rows=[0.5625,0,0,0.8,0,1,0,-0.1,0.2,0,1,0.6,0,0,0.025,0]",
          "a call line: ordinal, camera, kind, caller, draw, tone, what the detour decided (inj, role), dirty flags before>after, the view, the sixteen composed floats");
    check(at("call-first-person") ==
              "vr camera census: call frame=4 n=13 camera=0x241dc2e2960 kind=3 caller=+0x594E13 draw=6500 tone=before inj=1 role=fp fl=0x1C>0x0 "
              "view=0x241dd00a000 rows=[0.5625,0,0,0.8,0,1,0,-0.1,0.2,0,1,0.6,0,0,0.025,0]",
          "the first-person role prints as fp");
    check(at("call-aux") ==
              "vr camera census: call frame=4 n=14 camera=0x241dc2e2960 kind=3 caller=+0x58DE73 draw=6500 tone=before inj=0 role=aux fl=0x1C>0x0 "
              "view=0x241dd00a000 rows=[0.5625,0,0,0.8,0,1,0,-0.1,0.2,0,1,0.6,0,0,0.025,0]",
          "an excluded kind-3 call prints inj=0 role=aux");
    check(at("call-eye") ==
              "vr camera census: call frame=4 n=98 camera=0x241df6d0bb0 kind=5 caller=+0x594FE1 draw=8210 tone=after inj=0 role=- fl=0x1C>0x0 "
              "view=0x241dd00e000 rows=[1.1,0,0,0.8,0,1,0,-0.1,0.2,0,1,0.6,0,0,0.05,0]",
          "an eye camera's call (kind 5), after the tone, is never injected and has no role");
    check(at("call-ortho") ==
              "vr camera census: call frame=4 n=2 camera=0x241de000100 kind=1 caller=+0x58DE73 draw=- tone=none inj=0 role=- fl=0x1C>- view=0x241dd00c000 rows=-",
          "a call with no draw progress, no post half and no rows prints dashes, never a guess");
    check(at("eye") ==
              "vr camera census: eye=0 frame=4 foot=yes phase=0.2520,-0.1260 draw=8213 b1=0x1eb2e751e20 first=0 bytes=5376 "
              "rows=[1.1,0,0,0.8,0,1,0,-0.1,0.2,0,1,0.6,0,0,0.05,0] meas=(0.0002,-0.0001)",
          "an eye draw's line: the frame's phase, the b1 buffer, its rows 270..273, the measured shift");
    check(at("eye-failed") == "vr camera census: eye=1 frame=4 foot=yes phase=- draw=8220 b1=0x1eb2e751e20 first=0 bytes=5376 rows=- meas=- why=map",
          "a failed readback says why and never prints rows");
    check(at("eye-geometry") ==
              "vr camera census: eye-geometry eye=0 frame=4 seq=4711 frustum=[-1.2,0.7,-0.9,1.1] shift=(0,0) expect=(0.2631579,-0.1) "
              "expect-shifted=(0.2631579,-0.1) leak=(0.000e+00,0.000e+00)",
          "the eye geometry line: the advertised frustum and shift, the expectation and the leak");
    check(at("eye-geometry-unavailable") == "vr camera census: eye-geometry eye=1 frame=4 geometry=unavailable",
          "no advertised geometry is said, not invented");
    check(at("other-thread") == "vr camera census: other-thread tid=4321 camera=0x241dc2e2960 kind=3 caller=+0x594E13 calls=57",
          "a call on another thread: thread, camera, kind, caller, count");
    // THE EPISODES' LINES.
    check(at("episode-cockpit") ==
              "vr camera census: episode frame=5231 n=3/10 trigger=gui:0>6 armed=5201 foot=no gui=6 named=0 phase=- calls=612 recorded=612 printed=120 "
              "kinds=0:12,1:300,3:290,5:10 callers=+0x58DE73:300,+0x594E13:130,+0x594EAB:130,+0x594FE1:40,+more:1",
          "an episode's header: the sampled frame, its ordinal of ten, the trigger (gui:0>6, the galaxy map opening), the frame it was armed at, the journal, GuiFocus, the naming, "
          "the route's phase, the calls (all, recorded, printed), the kinds of all of them and the four busiest callers, the rest counted");
    check(at("episode-foot") ==
              "vr camera census: episode frame=6120 n=4/10 trigger=naming:named>unnamed armed=6090 foot=yes gui=- named=0 phase=0.2520,-0.1260 calls=107 recorded=107 printed=107 "
              "kinds=1:8,3:93,5:6 callers=+0x594E13:40,+0x594EAB:40,+0x594FE1:27",
          "an on-foot episode: GuiFocus unknown is a dash, the naming flip is named>unnamed, the route's phase rides on the line");
    check(at("episode-key-on-empty") ==
              "vr camera census: episode frame=31 n=1/10 trigger=key-on armed=1 foot=off gui=- named=0 phase=- calls=0 recorded=0 printed=0 kinds=- callers=-" &&
              at("episode-foot-flip").find(" trigger=foot:no>yes armed=1 ") != std::string::npos,
          "an episode of a frame with no call prints dashes, never invented counts; the journal's flip is foot:no>yes");
    check(at("pass-rows") ==
              "vr camera census: pass-rows ep=3 frame=5231 valid=1 bound=1 rows=[0.9,0,-0.1,0.5,0,1,0,-1.5,0.1,0,0.9,2.5] axes-match=98,101 how=identity nearest=98 diff=0.000e+00",
          "the pass's chosen rows: whether the block bound at the scene's first draw was the one chosen, the twelve floats, the calls whose view axes equal them, the nearest and how it fits");
    check(at("pass-rows-no-match") ==
              "vr camera census: pass-rows ep=3 frame=5231 valid=1 bound=0 rows=[0.9,0,-0.1,0.5,0,1,0,-1.5,0.1,0,0.9,2.5] axes-match=- how=transpose nearest=17 diff=1.500e-03" &&
              at("pass-rows-none") == "vr camera census: pass-rows ep=3 frame=5231 valid=0 bound=- rows=- axes-match=- how=- nearest=- diff=-",
          "no call whose axes equal the rows still names the nearest and how near; a frame the pass chose nothing in says valid=0 and prints dashes");
    check(at("join") ==
              "vr camera census: join ep=3 sig=1 depth=eye eye=0 size=2620x2533 draw=8210 draws=1432 vs=0x5C36AF051B98B9F1 ps=0xCFE84157BC76E921 dw=yes b1=0x1eb2e751e20 first=0 "
              "bytes=5376 rows=read match=98,101" &&
              at("join-rows") == "vr camera census: join-rows ep=3 sig=1 rows=[1.1,0,0,0.8,0,1,0,-0.1,0.2,0,1,0.6,0,0,0.05,0]",
          "a join signature: the depth (screen or eye, and which), its size, the first draw, the draws it took, the shaders, whether it writes depth, b1 and its size, the calls its rows "
          "matched; its rows follow on a line of their own, in a call line's format");
    check(at("join-skip") ==
              "vr camera census: join ep=3 sig=2 depth=screen eye=- size=5040x2835 draw=6500 draws=22 vs=0xDFED8E1C9E191BEC ps=0x143AAE0597E2F7BF dw=no b1=0x1eb2e751e20 first=0 "
              "bytes=5376 rows=- why=skip" &&
              at("join-no-b1") == "vr camera census: join ep=3 sig=3 depth=eye eye=- size=2620x2533 draw=91 draws=3 vs=0x0 ps=0x0 dw=- b1=- first=- bytes=- rows=- why=no-b1" &&
              at("join-no-match").find(" rows=read match=-") != std::string::npos,
          "a signature after the first of its depth costs no readback and says why=skip; a draw with no b1 bound says why=no-b1 and prints dashes; rows that matched no call say match=-");
    check(at("join-more") == "vr camera census: join-more ep=3 signatures=4 draws=212", "the signatures the table could not keep are counted");
    check(at("join-draws") == "vr camera census: join-draws ep=3 seen=95 relevant=75 views=4 signatures=4" &&
              at("join-draws-none") == "vr camera census: join-draws ep=4 seen=0 relevant=0 views=0 signatures=0",
          "the join's draw counts: every draw the per-draw hook was handed, those into the screen's or an eye's depth, the views it resolved, the signatures it kept; seen=0 is a hook that "
          "never ran, which a frame with no draw into those depths (seen=95, relevant=0) is not");
    check(at("runs") ==
              "vr camera census: runs windows=1 frames=4500 named=1:1,2:0,3:0,4-8:0,9-30:0,31-89:0,90+:1 unnamed=1:3,2:1,3:2,4-8:0,9-30:1,31-89:0,90+:0 longest=named:3012,unnamed:14 "
              "open=named:3012" &&
              at("runs-empty") == "vr camera census: runs windows=12 frames=0 named=1:0,2:0,3:0,4-8:0,9-30:0,31-89:0,90+:0 unnamed=1:0,2:0,3:0,4-8:0,9-30:0,31-89:0,90+:0 "
                                  "longest=named:0,unnamed:0 open=-",
          "the naming runs line: the bins 1, 2, 3, 4-8, 9-30, 31-89 and 90+ for each way, the longest of each, the run still open; an empty window prints every bin (zeros included)");
    check(at("episodes-idle") == "vr camera census: episodes windows=1 taken=3/10 triggers=5 skipped=2 state=idle" &&
              at("episodes-armed") == "vr camera census: episodes windows=12 taken=3/10 triggers=5 skipped=2 state=armed trigger=gui:0>6 armed=5201 sample=5231" &&
              at("episodes-zero") == "vr camera census: episodes windows=1 taken=0/10 triggers=0 skipped=0 state=idle",
          "the episodes' counters line, with every window and zeros included (an absent line is what 'the episode code never ran' looks like): episodes taken of ten, triggers, those skipped, "
          "and, while one is armed or live, its trigger, the frame it was armed at and the frame it samples");
    check(at("detour") ==
              "vr camera census: detour windows=3 every=16 timed=observer-halves frames=100 calls=1000 sampled=3 est-ms-frame=0.465 obs-calls=600 obs-sampled=2 obs-pre-us=25/30 "
              "obs-post-us=12.5/15 inj-calls=400 inj-sampled=1 inj-pre-us=40/40 inj-post-us=20/20" &&
              at("detour-empty") ==
                  "vr camera census: detour windows=1 every=16 timed=observer-halves frames=0 calls=0 sampled=0 est-ms-frame=- obs-calls=0 obs-sampled=0 obs-pre-us=- obs-post-us=- "
                  "inj-calls=0 inj-sampled=0 inj-pre-us=- inj-post-us=-",
          "the detour's CPU line says what is timed and how often, and per mode the calls, the timed ones and mean/longest microseconds; nothing timed is a dash, never a zero");
    bool lengthOk = true;
    for (const Golden& x : g) if (x.text.size() > kVrCensusLineBytes) lengthOk = false;
    check(lengthOk, "every line above is at most 400 characters");
    // The worst case of the long lines at realistic maxima: sixteen floats at their widest (-1.234568e-05), a user-mode pointer
    // for the camera and the view (twelve hex digits), a caller inside a 256 MB image, a six-digit draw, nine-digit frame.
    VrCensusCall worst;
    worst.camera = 0x7FFFFFFFFFFFull; worst.view = 0x7FFFFFFFFFFFull; worst.kind = 5; worst.kindReadable = true; worst.callerRva = 0xFFFFFFF;
    worst.draw = 999999; worst.drawKnown = true; worst.tone = VrCensusTone::Before; worst.preFlags = worst.postFlags = 0xFF; worst.postSeen = worst.rowsValid = true;
    worst.willInject = true; worst.role = 0;   // the longest words the new tokens take: inj=1 role=scene
    for (int i = 0; i < 16; ++i) worst.rows[i] = -1.2345678e-05f * (i + 1);
    char line[kVrCensusLineBytes + 1];
    const int n = vrCensusFormatCall(line, sizeof(line), 999999999ull, 160, worst);
    check(n > 0 && std::strlen(line) <= kVrCensusLineBytes && std::strstr(line, "rows=[") != nullptr && line[std::strlen(line) - 1] == ']' &&
              std::strstr(line, " inj=1 role=scene ") != nullptr,
          "the widest realistic call line (pointers, a long frame number, inj and role, and sixteen long floats) still fits 400 characters whole");
    std::printf("  note  widest realistic call line: %zu characters\n", std::strlen(line));
    // The eye line at its widest realistic: sixteen long floats, a b1 pointer, a long frame, the journal's longest word, a phase of a
    // pixel either way, and the measured shift at its longest.
    {
        float eyeRows[16];
        for (int i = 0; i < 16; ++i) eyeRows[i] = -1.2345678e-05f * (i + 1);
        const VrCensusPhase wide{true, -0.9999f, -0.9999f};
        char eyeLine[kVrCensusLineBytes + 1];
        const int en = vrCensusFormatEye(eyeLine, sizeof(eyeLine), 1, 999999999ull, VrCensusFoot::Unknown, wide, true, 999999, 0x7FFFFFFFFFFFull, 0,
                                         16777216, eyeRows, true, -1.2345678e-05, -1.2345678e-05, nullptr);
        check(en > 0 && std::strlen(eyeLine) <= kVrCensusLineBytes && eyeLine[std::strlen(eyeLine) - 1] == ')' && std::strstr(eyeLine, "phase=-0.9999,-0.9999 ") != nullptr,
              "the widest realistic eye line (phase included) still fits 400 characters whole, its measured shift last");
        std::printf("  note  widest realistic eye line: %zu characters\n", std::strlen(eyeLine));
        char seqLine[kVrCensusLineBytes + 1];
        const int sn = vrCensusFormatSequence(seqLine, sizeof(seqLine), 999999999ull, 3, VrCensusFoot::Unknown, wide, 160, 160);
        check(sn > 0 && std::strlen(seqLine) <= kVrCensusLineBytes && std::strstr(seqLine, " phase=-0.9999,-0.9999 calls=160 recorded=160 truncated=0") != nullptr,
              "a sequence header at its widest fits, and ends with its counts");
        const VrCensusPhase odd[] = {{true, -0.00002f, 0.00004f}, {true, std::nanf(""), 0.5f}, {true, -0.0f, 0.0f}};
        char oddLine[kVrCensusLineBytes + 1];
        vrCensusFormatSequence(oddLine, sizeof(oddLine), 1, 1, VrCensusFoot::Yes, odd[0], 1, 1);
        const bool tiny = std::strstr(oddLine, " phase=0.0000,0.0000 ") != nullptr;
        vrCensusFormatSequence(oddLine, sizeof(oddLine), 1, 1, VrCensusFoot::Yes, odd[1], 1, 1);
        const bool notANumber = std::strstr(oddLine, " phase=nan,0.5000 ") != nullptr;
        vrCensusFormatSequence(oddLine, sizeof(oddLine), 1, 1, VrCensusFoot::Yes, odd[2], 1, 1);
        const bool negZero = std::strstr(oddLine, " phase=0.0000,0.0000 ") != nullptr && std::strstr(oddLine, "-0.0000") == nullptr;
        check(tiny && notANumber && negZero,
              "a phase that rounds to zero prints 0.0000 (never -0.0000), a jittering warm-up frame's zero is 0.0000,0.0000, and a NaN axis prints nan");
    }
    // The episodes' lines at their widest realistic: a 9-digit frame, every kind and caller with five-digit counts, the longest words, long floats, eleven-digit window counts.
    {
        char wide[kVrCensusLineBytes + 1];
        VrCensusEpisodeHeader h;
        h.n = 10; h.frame = 999999999ull; h.armedFrame = 999999969ull;
        h.trigger = VrCensusTrigger{VrCensusTriggerKind::Naming, 0, 1};
        h.foot = VrCensusFoot::Unknown; h.guiKnown = true; h.gui = 999; h.named = true; h.phase = VrCensusPhase{true, -0.9999f, -0.9999f};
        h.calls = 99999; h.recorded = 640; h.printed = 120;
        for (int k = 0; k < 8; ++k) h.kinds[k] = 99999;
        VrCensusEpisodeFrame::Caller callers[16];
        for (int i = 0; i < 16; ++i) callers[i] = {0xFFFFFFFu - static_cast<uint32_t>(i), 99999ull};
        h.callers = callers; h.callerCount = 16; h.callerOverflow = 99999999ull;
        const int hn = vrCensusFormatEpisode(wide, sizeof(wide), h);
        check(hn > 0 && std::strlen(wide) <= kVrCensusLineBytes && std::strstr(wide, ",+more:") != nullptr && std::strstr(wide, "kinds=0:99999,1:99999,2:99999,3:99999,4:99999,5:99999,other:99999,unreadable:99999") != nullptr,
              "the widest realistic episode header (nine-digit frame, every kind, sixteen callers, the longest trigger and phase) fits 400 characters whole, kinds and callers both");
        std::printf("  note  widest realistic episode header: %zu characters\n", std::strlen(wide));
        VrCensusJoinRow jw;
        jw.depth = VrCensusJoinDepth::Screen; jw.w = 16384; jw.h = 16384; jw.vs = ~0ull; jw.ps = ~0ull; jw.firstDraw = 999999; jw.draws = 99999999999ull; jw.depthWrite = 1;
        jw.b1Bound = true; jw.b1 = 0x7FFFFFFFFFFFull; jw.b1First = 4294967295u; jw.b1Bytes = 4294967295u; jw.rowsRead = true; jw.matchCount = 99999;
        for (int i = 0; i < 6; ++i) jw.matches[i] = 640 - static_cast<uint32_t>(i);
        const int jn = vrCensusFormatJoin(wide, sizeof(wide), 10, 12, jw);
        check(jn > 0 && std::strlen(wide) <= kVrCensusLineBytes && std::strstr(wide, " match=640,639,638,637,636,635+99993") != nullptr,
              "the widest realistic join line (a full-width hash pair, 16384-square, eleven-digit draws, six matches and a count of the rest) fits whole, its matches last");
        std::printf("  note  widest realistic join line: %zu characters\n", std::strlen(wide));
        float wideRows[16];
        for (int i = 0; i < 16; ++i) wideRows[i] = -1.2345678e-05f * (i + 1);
        const int rn = vrCensusFormatJoinRows(wide, sizeof(wide), 10, 12, wideRows);
        check(rn > 0 && std::strlen(wide) <= kVrCensusLineBytes && wide[std::strlen(wide) - 1] == ']', "the widest join rows line (sixteen long floats) fits whole");
        std::printf("  note  widest realistic join-rows line: %zu characters\n", std::strlen(wide));
        VrCensusAxesMatch pw;
        pw.count = 99999; pw.nearest = 640; pw.nearestDiff = 3.0e38f; pw.nearestTransposed = true;
        for (uint32_t i = 0; i < VrCensusAxesMatch::kMax; ++i) pw.ordinals[i] = 640 - i;
        const int pn = vrCensusFormatPassRows(wide, sizeof(wide), 10, 999999999ull, true, true, wideRows, pw);
        check(pn > 0 && std::strlen(wide) <= kVrCensusLineBytes && std::strstr(wide, " diff=3.000e+38") != nullptr && std::strstr(wide, " axes-match=640,639,638,637,636,635+99993 ") != nullptr,
              "the widest realistic pass-rows line (twelve long floats, six matches, the nearest call and its distance) fits whole, the distance last");
        std::printf("  note  widest realistic pass-rows line: %zu characters\n", std::strlen(wide));
        VrCensusRuns rw;
        rw.frames = 99999999999ull; rw.longestNamed = rw.longestUnnamed = 4294967295u; rw.open = 0; rw.openLength = 4294967295u;
        for (int b = 0; b < VrCensusRuns::kBins; ++b) { rw.named[b] = rw.unnamed[b] = 99999999999ull; }
        const int un = vrCensusFormatRuns(wide, sizeof(wide), rw, 12);
        check(un > 0 && std::strlen(wide) <= kVrCensusLineBytes && std::strstr(wide, " open=unnamed:4294967295") != nullptr,
              "the widest realistic runs line (eleven-digit counts in every bin, the longest runs and the open one) fits whole, the open run last");
        std::printf("  note  widest realistic runs line: %zu characters\n", std::strlen(wide));
        VrCensusCpu cw;
        cw.mode[0].sampled = cw.mode[1].sampled = cw.mode[0].postSampled = cw.mode[1].postSampled = 99999999999ull;
        cw.mode[0].preTicks = cw.mode[1].preTicks = cw.mode[0].postTicks = cw.mode[1].postTicks = 99999999999ull * 100;
        cw.mode[0].preMax = cw.mode[1].preMax = cw.mode[0].postMax = cw.mode[1].postMax = 999999999999ull;
        VrCensusWindow ww;
        ww.frames = ww.calls = 99999999999ull; ww.injCalls = 49999999999ull;
        const int cn = vrCensusFormatCpu(wide, sizeof(wide), ww, cw, 10000000, 12);
        check(cn > 0 && std::strlen(wide) <= kVrCensusLineBytes && std::strstr(wide, " inj-post-us=") != nullptr && wide[std::strlen(wide) - 1] != ' ',
              "the widest realistic detour line (eleven-digit counts, long means and maxima) fits whole, down to the injected post half");
        std::printf("  note  widest realistic detour line: %zu characters\n", std::strlen(wide));
        // The 5 s line is untouched by the episodes (it is full at 400 characters): the counters are their own line, and at their widest they fit whole.
        VrCensusEpisodeCounters wc;
        wc.taken = 10; wc.triggers = wc.skipped = 4294967295u; wc.state = "armed"; wc.trigger = VrCensusTrigger{VrCensusTriggerKind::Naming, 0, 1};
        wc.armedFrame = wc.sampleFrame = 999999999ull;
        const int cn2 = vrCensusFormatEpisodeCounters(wide, sizeof(wide), wc, 12, true);
        check(cn2 > 0 && std::strlen(wide) <= kVrCensusLineBytes && std::strstr(wide, " sample=999999999") != nullptr, "the widest episode counters line fits whole");
        std::printf("  note  widest episode counters line: %zu characters\n", std::strlen(wide));
    }
    worst.rows[3] = std::nanf("");
    vrCensusFormatCall(line, sizeof(line), 1, 1, worst);
    check(std::strstr(line, ",nan,") != nullptr && std::strstr(line, "-nan") == nullptr && std::strstr(line, "(ind)") == nullptr,
          "a non-finite float prints as 'nan' (the compiler's spelling of a NaN never reaches the log)");
    // A line never holds a space inside a (..) or [..] value, so the reader can split on spaces.
    bool noSpaces = true;
    for (const Golden& x : g) {
        int depth = 0;
        for (char ch : x.text) { if (ch == '(' || ch == '[') ++depth; else if (ch == ')' || ch == ']') --depth; else if (ch == ' ' && depth > 0) noSpaces = false; }
    }
    check(noSpaces, "no value holds a space inside its brackets: a line splits into key=value tokens");
}

// ---------------------------------------------------------------------------
// The glue, without D3D: the same calls in the same order the DLL makes, through the core's own tables, so the order of the
// log and every cap is exercised end to end.
// ---------------------------------------------------------------------------
struct Sim {
    VrCensusFrame frame;
    VrCensusCameraTable cameras;
    VrCensusBudget budget;
    VrCensusFoot foot = VrCensusFoot::Yes;   // what the journal says; the glue reads it once a boundary
    VrCensusPhase phase;                     // what the route chose for the frame in progress, latched at the boundary that opened it
    bool progressAvailable = true;           // false: vrWorldRouteDrawProgress answers false (the skeleton, a route that does not watch)
    uint32_t sequencesLogged = 0;
    uint64_t frameNo = 1, toneFrames = 0, sampledFrames = 0;
    std::vector<std::string> log;
    void say(VrCensusLines c, const char* line) { if (budget.take(c)) log.emplace_back(line); }
    void call(uintptr_t camera, uint32_t kind, uint32_t caller, bool tone, const VrCensusSig& sig, bool inject = false,
              uint8_t role = kVrCensusRoleNone) {
        const bool recording = vrCensusMayRecord(foot, phase) && vrCensusPrintsSequence(true, sequencesLogged);
        VrCensusCall* rec = recording ? frame.add() : (++frame.calls, nullptr);
        if (progressAvailable) { frame.progress = true; if (tone) frame.toneSeen = true; }
        const VrCensusTone where = !progressAvailable ? VrCensusTone::None : tone ? VrCensusTone::After : VrCensusTone::Before;
        if (rec) { rec->camera = camera; rec->view = camera + 0x5000; rec->kind = kind; rec->kindReadable = true; rec->callerRva = caller;
                   rec->drawKnown = progressAvailable; rec->draw = frame.calls; rec->tone = where; rec->postSeen = true; rec->rowsValid = true;
                   rec->willInject = inject; rec->role = role; }
        const float noTan[4] = {};
        cameras.note(camera, sig, noTan, false, callAt(frameNo, caller, frame.calls, frame.calls, progressAvailable, where,
                                                       camera + 0x5000, camera + 0x5100));
    }
    // The Present boundary: the frame that ended is judged under the phase latched when it began, then the next frame's is latched.
    void boundary(const VrCensusPhase& next = VrCensusPhase{}) {
        char line[kVrCensusLineBytes + 1];
        const bool sampled = vrCensusSamplesFrame(frame.toneSeen, frame.progress, foot, phase);
        if (frame.toneSeen) ++toneFrames;
        if (sampled) ++sampledFrames;
        for (size_t i = 0; i < cameras.used(); ++i) {
            if (cameras.takeCameraLine(i)) { vrCensusFormatCamera(line, sizeof(line), cameras.at(i)); say(VrCensusLines::Camera, line); }
            if (cameras.takeChangeLine(i)) { vrCensusFormatChanged(line, sizeof(line), cameras.at(i)); say(VrCensusLines::Changed, line); }
        }
        if (vrCensusPrintsSequence(sampled, sequencesLogged) && frame.recorded > 0) {
            ++sequencesLogged;
            vrCensusFormatSequence(line, sizeof(line), frameNo, sequencesLogged, foot, phase, frame.calls, frame.recorded);
            say(VrCensusLines::Call, line);
            for (uint32_t i = 0; i < frame.recorded; ++i) { vrCensusFormatCall(line, sizeof(line), frameNo, i + 1, frame.call[i]); say(VrCensusLines::Call, line); }
        }
        frame.reset();
        ++frameNo;
        phase = next;
    }
};

void testSession() {
    std::printf("a scripted session through the core\n");
    Sim sim;
    const VrCensusSig world = sigOf(3, 1.7778f, 0.025f, 1.0122f), eye = sigOf(3, 0.9f, 0.05f, 1.3f, -0.03f, 0.011f), ui = sigOf(1, 1.0f, 0.1f, 0.0f);
    // Ten frames in the cockpit (no tone), then six on foot: world calls before the tone, eye calls after it.
    sim.foot = VrCensusFoot::No;
    for (int f = 0; f < 10; ++f) { for (int i = 0; i < 40; ++i) sim.call(0x1000, 3, 0x594E13, false, world); sim.boundary(); }
    const size_t cockpitLines = sim.log.size();
    check(cockpitLines == 1 && sim.sequencesLogged == 0, "frames without the tone print only the camera's first-sight line, never a call sequence");
    // Ten more in the cockpit WITH the tone (the detector fires for every frame the game draws the same chain): the journal
    // says not on foot, so nothing is recorded and no sequence is spent on them.
    for (int f = 0; f < 10; ++f) {
        for (int i = 0; i < 30; ++i) sim.call(0x1000, 3, 0x594E13, false, world);
        for (int i = 0; i < 4; ++i) sim.call(0x3000, 3, 0x594FE1, true, eye);
        check(sim.frame.recorded == 0 && sim.frame.calls == 34, "a cockpit frame with the tone counts its calls and records none");
        sim.boundary();
    }
    check(sim.sequencesLogged == 0 && sim.toneFrames == 10 && sim.sampledFrames == 0,
          "ten cockpit frames with the tone draw are tone frames and none is sampled: the journal's word keeps the budget for the commander on foot");
    sim.foot = VrCensusFoot::Unknown;
    for (int i = 0; i < 4; ++i) sim.call(0x3000, 3, 0x594FE1, true, eye);
    sim.boundary();
    check(sim.sequencesLogged == 0 && sim.sampledFrames == 0, "a menu frame (the journal read, no Flags2) is not sampled either");
    sim.foot = VrCensusFoot::Yes;
    for (int f = 0; f < 6; ++f) {
        for (int i = 0; i < 60; ++i) sim.call(0x1000, 3, i % 3 ? 0x594EAB : 0x594E13, false, world);
        for (int i = 0; i < 5; ++i) sim.call(0x2000, 3, 0x594FE1, false, ui);   // a kind-1 camera drawn before the tone
        for (int i = 0; i < 4; ++i) sim.call(0x3000, 3, 0x594FE1, true, f == 2 ? sigOf(3, 0.9f, 0.05f, 1.3f, -0.0298f, 0.011f) : eye);
        sim.boundary();
    }
    unsigned sequences = 0, callLines = 0, cameraLines = 0, changed = 0;
    size_t firstCall = 0, lastCamera = 0;
    for (size_t i = 0; i < sim.log.size(); ++i) {
        const std::string& l = sim.log[i];
        if (l.find("sequence frame=") != std::string::npos) ++sequences;
        if (l.find(": call frame=") != std::string::npos) { ++callLines; if (!firstCall) firstCall = i; }
        if (l.find("camera census: camera=0x") != std::string::npos) { ++cameraLines; lastCamera = i; }
        if (l.find("changed:") != std::string::npos) ++changed;
    }
    check(sequences == 3 && callLines == 3 * (60 + 5 + 4) && sim.sequencesLogged == 3,
          "six on-foot frames print the call sequence of the first three only, every call of each (69 lines a frame)");
    check(cameraLines == 3 && lastCamera < firstCall, "each camera's line was printed once, before the first sequence that names it");
    check(sim.toneFrames == 17 && sim.sampledFrames == 6, "seventeen tone frames in all, six of them sampled (the six on foot)");
    check(changed >= 1 && changed <= 4, "the eye camera's bound that moved in frame 3 printed a changed line (bounded)");
    check(sim.budget.total() == sim.log.size() && sim.log.size() < VrCensusBudget::capTotal(), "every printed line was taken from the budget");
    // After three sequences the recording stops: a further frame's calls are counted and no record is written.
    for (int i = 0; i < 30; ++i) sim.call(0x1000, 3, 0x594E13, true, world);
    check(sim.frame.recorded == 0 && sim.frame.calls == 30, "once three sequences are out, a call is counted and nothing is recorded");
    // The journal flips to on foot AT a boundary (the glue reads it before it rolls the frame that ended): that frame was
    // recorded under the old word, so it has no calls; it is sampled by the rule but prints no empty sequence and spends
    // none of the three. The next frame, recorded under the new word, prints.
    Sim flip;
    flip.foot = VrCensusFoot::No;
    for (int i = 0; i < 4; ++i) flip.call(0x3000, 3, 0x594FE1, true, eye);
    flip.foot = VrCensusFoot::Yes;
    flip.boundary();
    check(flip.sequencesLogged == 0 && flip.sampledFrames == 1 && flip.log.size() == 1,
          "the frame the journal flips at is sampled by the rule but recorded nothing: no empty sequence header, none of the three spent");
    for (int i = 0; i < 4; ++i) flip.call(0x3000, 3, 0x594FE1, true, eye);
    flip.boundary();
    check(flip.sequencesLogged == 1 && flip.log.back().find("call frame=2 n=4 ") != std::string::npos,
          "the next frame, recorded under the new word, prints its whole sequence");
    // With the journal not read at all the tone alone decides, as the brief has it: the first tone frame prints a sequence.
    Sim bare;
    bare.foot = VrCensusFoot::Off;
    for (int i = 0; i < 20; ++i) bare.call(0x1000, 3, 0x594E13, false, world);
    for (int i = 0; i < 4; ++i) bare.call(0x3000, 3, 0x594FE1, true, eye);
    bare.boundary();
    check(bare.sequencesLogged == 1 && bare.sampledFrames == 1 && bare.log.back().find("call frame=1 n=24 ") != std::string::npos,
          "with no journal the tone alone decides: the first frame with the tone prints its whole sequence");
    bool any = false;
    for (const std::string& l : bare.log) any = any || l.find("foot=off") != std::string::npos;
    check(any, "...and its header says foot=off, so the reader knows the journal was not consulted");
    // The world route reports no draw progress at all (the skeleton, or a route that does not watch): there is no tone to see,
    // so the journal alone decides, and only a journal that positively says on foot samples.
    Sim mute;
    mute.progressAvailable = false;
    mute.foot = VrCensusFoot::Yes;
    for (int i = 0; i < 20; ++i) mute.call(0x1000, 3, 0x594E13, false, world);
    for (int i = 0; i < 4; ++i) mute.call(0x3000, 3, 0x594FE1, true, eye);
    mute.boundary();
    bool dashes = false;
    for (const std::string& l : mute.log) dashes = dashes || (l.find(": call frame=1 n=24 ") != std::string::npos && l.find("draw=- tone=none") != std::string::npos);
    check(mute.sequencesLogged == 1 && mute.sampledFrames == 1 && mute.toneFrames == 0 && dashes,
          "with no draw progress and a journal that says on foot the frame is sampled, and every call prints draw=- tone=none");
    for (VrCensusFoot f : {VrCensusFoot::Off, VrCensusFoot::Unknown, VrCensusFoot::No}) {
        Sim none;
        none.progressAvailable = false;
        none.foot = f;
        for (int i = 0; i < 24; ++i) none.call(0x1000, 3, 0x594E13, false, world);
        none.boundary();
        check(none.sequencesLogged == 0 && none.sampledFrames == 0,
              "with no draw progress and no positive word from the journal (off, a menu, a ship) nothing is sampled");
    }
}

// STAGE 2: the world route jitters. Its first frames are a warm-up with a ZERO phase (vrWorldRouteWorldPhase answers true and 0,0);
// an eye camera with no phase in the world cameras has nothing to leak, so those frames must not spend the samples (flight 1 spent
// all of them there). The frames after carry a phase and are sampled. One frame's phase is latched at the boundary that opens it.
void testPhaseSession() {
    std::printf("a scripted session with the route jittering: a zero-phase warm-up, then phases\n");
    const VrCensusSig world = sigOf(3, 1.7778f, 0.025f, 1.0122f), eye = sigOf(3, 0.95f, 0.025f, 1.5997f, 0.0891f, 0.0f);
    const VrCensusPhase zero{true, 0.0f, 0.0f};
    const VrCensusPhase moving[] = {{true, 0.252f, -0.126f}, {true, -0.189f, 0.063f}, {true, 0.126f, 0.252f},
                                    {true, -0.252f, -0.063f}, {true, 0.063f, 0.189f}};
    // One on-foot frame: the injected scene calls and the first-person ones before the tone, an auxiliary one (excluded), the eyes' after.
    auto frame = [&](Sim& s) {
        for (int i = 0; i < 12; ++i) s.call(0x1000, 3, i % 3 ? 0x594EAB : 0x594E13, false, world, s.phase.jittering && vrCensusPhaseNonZero(s.phase), 0);
        for (int i = 0; i < 3; ++i) s.call(0x1000, 3, 0x594E13, false, world, s.phase.jittering && vrCensusPhaseNonZero(s.phase), 1);
        s.call(0x2000, 3, 0x58DE73, false, sigOf(3, 1.0f, 0.1f, 1.5f), false, 2);
        for (int i = 0; i < 6; ++i) s.call(0x3000 + 0x100 * (i / 3), 5, 0x594FE1, true, eye);
    };
    Sim jit;
    jit.foot = VrCensusFoot::Yes;
    jit.phase = zero;   // frame 1's choice, latched at the boundary before it
    for (int f = 0; f < 3; ++f) {   // the warm-up: jittering, zero phase
        frame(jit);
        check(jit.frame.recorded == 0 && jit.frame.calls == 22 && jit.frame.toneSeen,
              "a warm-up frame (the route jitters, the phase is zero) counts its 22 calls and records none: the tone was seen and the commander is on foot, but nothing can leak");
        jit.boundary(f < 2 ? zero : moving[0]);
    }
    check(jit.sequencesLogged == 0 && jit.sampledFrames == 0 && jit.toneFrames == 3,
          "three warm-up frames are tone frames and none is sampled: no sequence is spent on them");
    bool noSequence = true;
    for (const std::string& l : jit.log) noSequence = noSequence && l.find("sequence frame=") == std::string::npos;
    check(noSequence, "...and the log holds no sequence header for them");
    for (int f = 0; f < 5; ++f) {   // frames 4..8 carry a phase
        frame(jit);
        const bool records = f < 3;
        check((jit.frame.recorded == 22) == records, "a frame with a non-zero phase records its calls while sequences are still wanted (the first three)");
        jit.boundary(f < 4 ? moving[f + 1] : zero);
    }
    check(jit.sequencesLogged == 3 && jit.sampledFrames == 5,
          "the five frames that carry a phase are sampled and the first three of them print their sequence: the budget is spent on frames that can leak");
    std::vector<std::string> headers, calls;
    for (const std::string& l : jit.log) {
        if (l.find("sequence frame=") != std::string::npos) headers.push_back(l);
        if (l.find(": call frame=") != std::string::npos) calls.push_back(l);
    }
    check(headers.size() == 3 &&
              headers[0] == "vr camera census: sequence frame=4 index=1/3 foot=yes phase=0.2520,-0.1260 calls=22 recorded=22 truncated=0" &&
              headers[1] == "vr camera census: sequence frame=5 index=2/3 foot=yes phase=-0.1890,0.0630 calls=22 recorded=22 truncated=0" &&
              headers[2] == "vr camera census: sequence frame=6 index=3/3 foot=yes phase=0.1260,0.2520 calls=22 recorded=22 truncated=0",
          "each sequence header carries the phase of ITS frame, latched at the boundary that opened it (not the next frame's)");
    unsigned scene = 0, fp = 0, aux = 0, eyes = 0, injected = 0;
    for (const std::string& l : calls) {
        if (l.find(" kind=3 ") != std::string::npos && l.find(" inj=1 role=scene ") != std::string::npos) { ++scene; ++injected; }
        if (l.find(" kind=3 ") != std::string::npos && l.find(" inj=1 role=fp ") != std::string::npos) { ++fp; ++injected; }
        if (l.find(" kind=3 ") != std::string::npos && l.find(" inj=0 role=aux ") != std::string::npos) ++aux;
        if (l.find(" kind=5 ") != std::string::npos && l.find(" inj=0 role=- ") != std::string::npos) ++eyes;
    }
    check(calls.size() == 66 && scene == 36 && fp == 9 && aux == 3 && eyes == 18 && injected == 45,
          "the logged calls say what the detour decided: 36 injected scene, 9 injected first-person, 3 excluded auxiliary, and the eyes' 18 kind-5 calls never injected");
    // With the route NOT jittering the same frames are sampled the old way: the first three print, phase=-.
    Sim plain;
    plain.foot = VrCensusFoot::Yes;
    for (int f = 0; f < 5; ++f) { frame(plain); plain.boundary(); }
    bool dashes = plain.sequencesLogged == 3 && plain.sampledFrames == 5;
    for (const std::string& l : plain.log) if (l.find("sequence frame=") != std::string::npos) dashes = dashes && l.find(" foot=yes phase=- calls=22 ") != std::string::npos;
    check(dashes, "with the route not jittering the rule is exactly what it was: the first three on-foot frames print (phase=-), five are sampled");
    // The rule the census would run if it ignored the phase: it samples the warm-up (the control the row above sees).
    const VrCensusPhase notJittering{};
    check(vrCensusSamplesFrame(true, true, VrCensusFoot::Yes, notJittering) && !vrCensusSamplesFrame(true, true, VrCensusFoot::Yes, zero),
          "control: a sampler that ignored the phase would sample a warm-up frame, and this one does not");
    // The route stops jittering mid-session (idle, released): the frames after are judged by today's rule again.
    Sim idle;
    idle.foot = VrCensusFoot::Yes;
    idle.phase = moving[0];
    frame(idle); idle.boundary(notJittering);   // frame 1 carried a phase and is sampled
    frame(idle); idle.boundary(notJittering);   // frame 2: the route is not jittering, today's rule samples it
    check(idle.sampledFrames == 2 && idle.sequencesLogged == 2, "a route that stops jittering leaves the frames judged by the old rule: both are sampled");
}

void testSampling() {
    std::printf("which frames are sampled\n");
    check(vrCensusFootFrom(false, false, false) == VrCensusFoot::Off && vrCensusFootFrom(false, true, true) == VrCensusFoot::Off &&
          vrCensusFootFrom(true, false, false) == VrCensusFoot::Unknown && vrCensusFootFrom(true, false, true) == VrCensusFoot::Unknown &&
          vrCensusFootFrom(true, true, false) == VrCensusFoot::No && vrCensusFootFrom(true, true, true) == VrCensusFoot::Yes,
          "the journal's state: off when it is not read, unknown (a menu) without Flags2, no in a ship, yes on foot");
    const VrCensusPhase none{};   // the route is not jittering: the phase has no say
    bool samples = true, never = true, record = true;
    for (int foot = 0; foot < 4; ++foot) {
        const VrCensusFoot f = static_cast<VrCensusFoot>(foot);
        const bool allows = f == VrCensusFoot::Yes || f == VrCensusFoot::Off;
        if (vrCensusSamplesFrame(true, true, f, none) != allows) samples = false;     // progress available, tone seen
        if (vrCensusSamplesFrame(false, true, f, none)) never = false;                // progress available, no tone: never
        if (vrCensusSamplesFrame(false, false, f, none) != (f == VrCensusFoot::Yes)) samples = false;   // no progress: the journal alone
        if (vrCensusSamplesFrame(true, false, f, none) != (f == VrCensusFoot::Yes)) samples = false;
        if (vrCensusMayRecord(f, none) != allows) record = false;
    }
    check(samples && never && record,
          "with the route not jittering, with draw progress a frame is sampled when the tone was seen and the journal says on foot or is not read, and never "
          "without the tone; with none only a journal that says on foot samples (exactly the rule before stage 2)");
    auto toneOnly = [](bool tone, VrCensusFoot) { return tone; };   // the brief's rule, as the census would run without the journal
    check(toneOnly(true, VrCensusFoot::No) && !vrCensusSamplesFrame(true, true, VrCensusFoot::No, none),
          "control: the tone alone would sample a cockpit frame, and the journal's word keeps it out");

    // THE PHASE: the truth table. While the route jitters a frame is sampled only with a non-zero phase; the journal, the tone and the
    // progress keep their say on top of it (the phase can only take a sample away). Every cell is checked against the rule written out.
    struct PhaseCase { const char* what; VrCensusPhase p; bool allows; };
    const PhaseCase phases[] = {
        {"not jittering, zero", {false, 0.0f, 0.0f}, true},
        {"not jittering, a phase the route left behind", {false, 0.25f, -0.1f}, true},
        {"jittering, zero (a warm-up frame)", {true, 0.0f, 0.0f}, false},
        {"jittering, negative zero", {true, -0.0f, -0.0f}, false},
        {"jittering, x only", {true, 0.25f, 0.0f}, true},
        {"jittering, y only", {true, 0.0f, -0.1f}, true},
        {"jittering, both", {true, -0.25f, 0.1f}, true},
        {"jittering, a tiny phase", {true, 1.0e-6f, 0.0f}, true},
        {"jittering, a NaN axis and a zero", {true, std::nanf(""), 0.0f}, false},
    };
    bool table = true, phaseOnly = true, never2 = true;
    unsigned cells = 0;
    for (const PhaseCase& pc : phases) {
        if (vrCensusPhaseAllowsSample(pc.p) != pc.allows) { table = false; std::printf("  note  phase case '%s' disagrees\n", pc.what); }
        for (int foot = 0; foot < 4; ++foot) {
            const VrCensusFoot f = static_cast<VrCensusFoot>(foot);
            for (int tone = 0; tone < 2; ++tone) {
                for (int progress = 0; progress < 2; ++progress) {
                    const bool base = progress ? (tone && (f == VrCensusFoot::Yes || f == VrCensusFoot::Off)) : f == VrCensusFoot::Yes;
                    const bool want = base && pc.allows;
                    if (vrCensusSamplesFrame(tone != 0, progress != 0, f, pc.p) != want) table = false;
                    if (!base && vrCensusSamplesFrame(tone != 0, progress != 0, f, pc.p)) never2 = false;   // a phase never ADDS a sample
                    ++cells;
                }
            }
            const bool mayRecord = (f == VrCensusFoot::Yes || f == VrCensusFoot::Off) && pc.allows;
            if (vrCensusMayRecord(f, pc.p) != mayRecord) record = false;
        }
        // The same frame with and without the phase differs exactly when the route jitters with nothing to show.
        if (vrCensusSamplesFrame(true, true, VrCensusFoot::Yes, pc.p) != pc.allows) phaseOnly = false;
    }
    check(table && record && cells == 9 * 4 * 2 * 2,
          "the phase truth table: nine phase states x four journal states x tone x progress (144 cells) are sampled exactly when the old rule says so AND the route is "
          "not jittering or its phase is non-zero; calls are recorded under the same condition");
    check(phaseOnly && never2,
          "a frame on foot with the tone seen is sampled exactly when the phase allows it, and a phase never adds a sample the old rule refused");
    check(vrCensusPhaseNonZero({true, 0.0f, 1.0e-30f}) && !vrCensusPhaseNonZero({true, 0.0f, 0.0f}) && !vrCensusPhaseNonZero({true, -0.0f, 0.0f}) &&
              vrCensusAbs(-2.5f) == 2.5f && vrCensusAbs(2.5f) == 2.5f && vrCensusAbs(-0.0f) == 0.0f,
          "a phase is non-zero when either axis is: |x| + |y| > 0, however small, of either sign");
    // The control the rows above see: two wrong rules, each of which the table catches. One ignores the phase (the warm-up would be
    // sampled again); the other applies the zero test even when the route is not jittering (the frames of a route that is off would
    // starve, which is not "exactly today's rule").
    auto mutantIgnoresPhase = [](const VrCensusPhase&) { return true; };
    auto mutantZeroTestAlways = [](const VrCensusPhase& p) { return vrCensusPhaseNonZero(p); };
    auto mutantBackwards = [](const VrCensusPhase& p) { return p.jittering || vrCensusPhaseNonZero(p); };
    bool ignoreCaught = false, zeroCaught = false, backwardsCaught = false;
    for (const PhaseCase& pc : phases) {
        if (mutantIgnoresPhase(pc.p) != pc.allows) ignoreCaught = true;
        if (mutantZeroTestAlways(pc.p) != pc.allows) zeroCaught = true;
        if (mutantBackwards(pc.p) != pc.allows) backwardsCaught = true;
    }
    check(ignoreCaught && zeroCaught && backwardsCaught,
          "control: a rule that ignored the phase, one that applied the zero test to a route that is not jittering, and one with the jittering test inverted "
          "each disagree with the table at a cell, so the table can fail");
    check(std::strcmp(vrCensusFootName(VrCensusFoot::Yes), "yes") == 0 && std::strcmp(vrCensusFootName(VrCensusFoot::No), "no") == 0 &&
          std::strcmp(vrCensusFootName(VrCensusFoot::Unknown), "unknown") == 0 && std::strcmp(vrCensusFootName(VrCensusFoot::Off), "off") == 0,
          "the journal's states have the words the log uses");
}

// ---------------------------------------------------------------------------
// Source pins: what no rig can run because it needs the game.
// ---------------------------------------------------------------------------
struct Pin { const std::string* text; const char* needle; unsigned times; const char* what; };

void runPins(const std::vector<Pin>& pins, const char* control) {
    for (const Pin& pin : pins) {
        check(count(*pin.text, pin.needle) == pin.times, pin.what);
        std::string without = *pin.text;
        for (size_t at = without.find(pin.needle); at != std::string::npos; at = without.find(pin.needle))
            without.erase(at, std::strlen(pin.needle));
        if (count(without, pin.needle) != 0) check(false, control);
    }
}

void testSourcePins(const std::string& injectCpp) {
    std::printf("source pins\n");
    const std::string censusCpp = slurp("src/d3d11/vr_camera_census.cpp");
    const std::string censusH = slurp("src/d3d11/vr_camera_census.h");
    const std::string phaseH = slurp("src/d3d11/flat_camera_phase.h");
    check(!censusCpp.empty() && !injectCpp.empty() && !censusH.empty() && !phaseH.empty(),
          "the census, injector and admission sources are readable from the repo root");

    // KEY OFF = NOTHING.
    const std::string eyeHead = "void vrCameraCensusEyeDraw(ID3D11DeviceContext* ctx, uint32_t eye) {";
    const std::string boundaryHead = "void vrCameraCensusFrameBoundary() {";
    const std::string readHead = "bool readWanted() {";
    const std::string eyeDraw = functionBody(censusCpp, eyeHead);
    const std::string boundary = functionBody(censusCpp, boundaryHead);
    const std::string readWanted = functionBody(censusCpp, readHead);
    check(!eyeDraw.empty() && !boundary.empty() && !readWanted.empty() &&
          censusCpp.find("bool vrCameraCensusWanted() { return g_wanted; }") != std::string::npos,
          "the census's entry points can be delimited in the source");
    check(startsWith(eyeDraw, eyeHead, "\n    if (!g_wanted) return;\n"),
          "KEY OFF: the eye-draw hook's first statement is 'if (!g_wanted) return;', ahead of any other work");
    const size_t offReturn = boundary.find("        return;\n    }\n");
    check(startsWith(boundary, boundaryHead, "\n    const bool wanted = readWanted();\n    if (!wanted) {\n        if (g_wanted) deactivate();") &&
          offReturn != std::string::npos && offReturn < boundary.find("new (std::nothrow) State") &&
          boundary.find("new (std::nothrow) State") > boundary.find("if (!wanted) {") &&
          boundary.find("flatCameraInjectDisarm()") > boundary.find("if (!wanted) {") &&
          boundary.find("flatCameraInjectObserveFrame()") > boundary.find("if (!wanted) {"),
          "KEY OFF: the boundary reads the key, and with it off returns before the state is allocated, the owner is set or the hook is asked for");
    check(startsWith(readWanted, readHead, "\n    if (!runtimeVrProfile()) return false;") &&
          readWanted.find("getString(\"advanced.vr_camera_census\", \"off\")") > readWanted.find("runtimeVrProfile()"),
          "KEY OFF outside VR: the flat profile returns before it asks for the key (and Config reads the key off there anyway)");
    std::string censusCode;   // the glue without its // comments, so prose that says "new" is not an allocation
    {
        size_t from = 0;
        while (from < censusCpp.size()) {
            size_t to = censusCpp.find('\n', from);
            if (to == std::string::npos) to = censusCpp.size();
            std::string ln = censusCpp.substr(from, to - from);
            const size_t slashes = ln.find("//");
            if (slashes != std::string::npos) ln.erase(slashes);
            censusCode += ln;
            censusCode += '\n';
            from = to + 1;
        }
    }
    check(count(censusCode, "new (std::nothrow) State") == 1 && count(censusCode, "new ") == 1 && count(censusCode, "malloc") == 0 &&
          count(censusCode, "std::vector") == 0 && count(censusCode, "std::string ") == 1,
          "the only allocation in the census's code is the one State, made at the first boundary with the key on (the key's one std::string is a three-character small string)");
    const std::string sampleAsk = "if (!vrCensusSamplesFrame(toneSeen, have, s->foot, phase)) return;";
    check(count(censusCpp, "journalOnFootKnown()") == 1 && count(censusCpp, "journalOnFoot()") == 1 &&
          boundary.find("s->foot = currentFoot();") != std::string::npos && boundary.find("s->foot = currentFoot();") < boundary.find("rollFrame(s);") &&
          eyeDraw.find(sampleAsk) != std::string::npos &&
          eyeDraw.find(sampleAsk) < eyeDraw.find("s->eye.take(s->frame)") &&
          eyeDraw.find(sampleAsk) < eyeDraw.find("readEyeRows("),
          "the journal is asked in one place (currentFoot), once a boundary ahead of the frame's roll, and an eye draw the journal or the route's phase rules "
          "out is returned before it spends the eye budget or stalls on a readback");
    // THE PHASE (stage 2): the census asks the route for it in one place, latches it at the boundary AFTER the frame that ended has been
    // judged under the previous one, and reads it at the eye draw before the sampling decision uses it.
    const std::string rollBody = functionBody(censusCpp, "void rollFrame(State* s) {");
    const std::string preBody = functionBody(censusCpp, "bool observePre(const FlatCameraObserveCall& call) noexcept {");
    check(!rollBody.empty() && !preBody.empty() && count(censusCpp, "vrWorldRouteWorldPhase(") == 1 && count(censusCpp, "readPhase()") == 3,
          "the route's phase is asked in one place, readPhase(), which the boundary and the eye draw call (definition plus two calls)");
    check(boundary.find("rollFrame(s);") != std::string::npos && boundary.find("s->phase = readPhase();") != std::string::npos &&
          boundary.find("rollFrame(s);") < boundary.find("s->phase = readPhase();") &&
          boundary.find("s->phase = readPhase();") < boundary.find("flatCameraInjectObserveFrame()") &&
          boundary.find("s->phase = readPhase();") > boundary.find("if (!wanted) {"),
          "the boundary judges the frame that ended under the phase latched when it began (rollFrame), THEN latches the next frame's, before the window opens; "
          "with the key off it returns before asking the route for anything");
    check(rollBody.find("vrCensusSamplesFrame(s->current.toneSeen, s->current.progress, s->foot, s->phase)") != std::string::npos &&
          rollBody.find("readPhase()") == std::string::npos &&
          censusCpp.find("vrCensusFormatSequence(line, kVrCensusLineBytes + 1, s->frame, s->sequencesLogged, s->foot, s->phase, f.calls, f.recorded);") != std::string::npos,
          "the roll and the sequence header use the phase latched for that frame, never a fresh ask (the route's boundary has moved on by then)");
    check(eyeDraw.find("const VrCensusPhase phase = readPhase();") != std::string::npos &&
          eyeDraw.find("const VrCensusPhase phase = readPhase();") > eyeDraw.find("if (!g_wanted) return;") &&
          eyeDraw.find("const VrCensusPhase phase = readPhase();") < eyeDraw.find(sampleAsk) &&
          eyeDraw.find("vrCensusFormatEye(line, kVrCensusLineBytes + 1, eye, s->frame, s->foot, phase,") != std::string::npos,
          "the eye draw reads the running frame's phase after the key-off return and before it decides, and prints that same phase on its line");
    check(preBody.find("vrCensusMayRecord(s->foot, s->phase)") != std::string::npos &&
          preBody.find("call.willInject") != std::string::npos && preBody.find("record->willInject = call.willInject;") != std::string::npos &&
          preBody.find("record->role = call.role;") != std::string::npos && preBody.find("vrWorldRouteWorldPhase") == std::string::npos &&
          preBody.find("readPhase") == std::string::npos,
          "the refresh's pre half decides recording from the latched phase and copies what the detour decided (willInject, role) into the record and the window; "
          "it never asks the route (no call into the route on the hot path)");
    check(count(censusCpp, "flatCameraInjectSetObserver(&g_observer)") == 1 && count(censusCpp, "flatCameraInjectObserveFrame()") == 1 &&
          count(censusCpp, "flatCameraInjectPause(") == 2,
          "the hook is asked for from one place (the boundary, after the key is on), and paused and unpaused from the two edges of it");
    // The key-off edge hands the relay's gate back to the route when the detour is not quiet (the world route injects, or a camera it injected
    // still waits for its flush): the pause is asked as flatCameraVrQuiet(), after the observer is detached, never a bare true. The unpause stays false.
    const std::string deactivateBody = functionBody(censusCpp, "void deactivate() {");
    const std::string activateBody = functionBody(censusCpp, "void activate(State* s) {");
    check(!deactivateBody.empty() && !activateBody.empty() &&
              deactivateBody.find("flatCameraInjectSetObserver(nullptr);") != std::string::npos &&
              deactivateBody.find("flatCameraInjectPause(flatCameraVrQuiet());") != std::string::npos &&
              deactivateBody.find("flatCameraInjectSetObserver(nullptr);") < deactivateBody.find("flatCameraInjectPause(flatCameraVrQuiet());") &&
              deactivateBody.find("flatCameraInjectPause(true)") == std::string::npos &&
              activateBody.find("flatCameraInjectPause(false);") != std::string::npos,
          "the key-off edge pauses the relay only when the detour is quiet (flatCameraInjectPause(flatCameraVrQuiet()), after the observer is detached): a census "
          "key-off never closes the gate on the route's injection; the key-on edge reopens it");
    // The announcement says what is true while the route injects: the CENSUS never writes a camera; the detour does, for the route.
    check(censusCpp.find("the census itself never writes a camera (the route's injection, when it runs, is its own)") != std::string::npos &&
              censusCpp.find("nothing is written to any camera") == std::string::npos && censusH.find("the CENSUS never writes a") != std::string::npos &&
              censusH.find("only ever OBSERVES") == std::string::npos,
          "the announcement and the header say the census itself never writes a camera (the route's injection is its own), not that nothing is written to any camera");
    check(censusCpp.find("flatRuntime") == std::string::npos && injectCpp.find("flatRuntimePhaseState(&jx, &jy, &rw, &rh, &applied);") != std::string::npos,
          "the census never calls the flat runtime (no phase source is needed: observe mode asks for no phase)");
    // A second place that reads the key would need its own off path.
    check(count(slurp("src/d3d11/vscreen.cpp") + slurp("src/d3d11/device_hook.cpp"), "advanced.vr_camera_census") == 0,
          "no other hook reads the key: the census's own boundary is the only place the decision is made");

    // The detour.
    const std::string pre = functionBody(injectCpp, "void refreshPre(uintptr_t r0, uintptr_t ctx, uintptr_t p2, uintptr_t camera) noexcept {");
    const std::string post = functionBody(injectCpp, "void refreshPost() noexcept {");
    const std::string observeCall = functionBody(injectCpp, "void observeCall(");
    const std::string observeOff = functionBody(injectCpp, "void observeOffThread(uintptr_t r0, uintptr_t camera) noexcept {");
    check(!pre.empty() && !post.empty() && !observeCall.empty() && !observeOff.empty(), "the detour's halves can be delimited in the source");
    const size_t branch = pre.find("if (observe) {\n        observeCall(");
    const size_t firstWrite = std::min({pre.find("flushCamera(camera)"), pre.find("sehWriteF32("), pre.find("sehWriteU32("), pre.find("sehWriteU64("),
                                        pre.find("flatRuntimeNoteCameraApplied"), pre.find("injected.noteInjected")});
    check(branch != std::string::npos && firstWrite != std::string::npos && branch < firstWrite,
          "OBSERVE-ONLY: the detour's observe branch ends the call ahead of the first flush, bound write, flag write or return-slot write of the inject path");
    check(pre.find("        observeCall(r0, ctx, p2, camera, callNo, readable, kind, callerRva, gate);\n        return;\n    }\n    // A camera this session injected, now not") != std::string::npos,
          "...and the branch is followed directly by the flush code it skips");
    const size_t phaseAsk = pre.find("flatRuntimePhaseState(");
    check(phaseAsk != std::string::npos && pre.rfind("if (!observe) {", phaseAsk) != std::string::npos && pre.rfind("if (!observe) {", phaseAsk) > pre.find("if (readable && kind == 3) {"),
          "OBSERVE-ONLY: the flat runtime's phase is asked for only under 'if (!observe)'");
    check(count(observeCall, "sehWriteU64(r0, static_cast<uint64_t>(g_stubB))") == 1 && count(observeCall, "sehWrite") == 1 &&
          observeCall.find("flushCamera") == std::string::npos && observeCall.find("flatRuntime") == std::string::npos,
          "OBSERVE-ONLY: observeCall's one write is the body's return slot (the post half's hook); no camera byte, flag or flush");
    check(observeOff.find("sehWrite") == std::string::npos && observeOff.find("flush") == std::string::npos,
          "OBSERVE-ONLY: the off-thread report reads and counts, it never writes");
    const size_t observeBranch = post.find("if (g_refreshTls.observe) {");
    const size_t observeEnd = post.find("        return;\n    }\n", observeBranch);
    check(observeBranch != std::string::npos && observeEnd != std::string::npos &&
          post.substr(observeBranch, observeEnd - observeBranch).find("sehWrite") == std::string::npos &&
          post.substr(observeBranch, observeEnd - observeBranch).find("flatRuntimeNoteCameraApplied") == std::string::npos &&
          post.find("sehWriteF32(camera + kCamBoundX, g_refreshTls.entryX);") > observeBranch,
          "OBSERVE-ONLY: the post half's observe branch restores nothing and notes nothing applied, and returns ahead of the restore");
    // The install and the switch.
    const std::string observeFrame = functionBody(injectCpp, "bool flatCameraInjectObserveFrame() {");
    check(count(injectCpp, "observeOnly.store(true") == 1 && count(injectCpp, "installRefreshHook(true)") == 1 &&
          observeFrame.find("observeOnly.store(true") < observeFrame.find("installRefreshHook(true)") &&
          count(injectCpp, "installRefreshHook(false)") == 1,
          "the switch is set in one place, ahead of the install it guards, and the flat path installs through the same function with it off");
    check(count(injectCpp, "admission.observeOnly = observe;") == 1 && count(phaseH, "if (in.observeOnly) return FlatCameraAdmit::Observed;") == 1,
          "the detour hands the switch to the admission table, and the table answers Observed for it");
    check(count(injectCpp, "flatcpu::Scope timed(flatcpu::kInject);") == 2,
          "the detour's CPU timer is still in its two halves, and the observe path added none");

    // The native temporal getter.
    const std::string nativeCpp = slurp("src/d3d11/native_temporal.cpp");
    const std::string geometry = functionBody(nativeCpp, "bool nativeTemporalEyeGeometry(");
    check(!geometry.empty() && geometry.find("if (t_insideTreat) return false;") != std::string::npos &&
          geometry.find("std::lock_guard<std::mutex> lock(mutex);") != std::string::npos &&
          geometry.find("t_insideTreat") < geometry.find("lock_guard") &&
          geometry.find("|| !current->begun || eye > 1") != std::string::npos,
          "nativeTemporalEyeGeometry takes the channel's mutex the way nativeTemporalDrawJitter does: never inside treat(), only with a begun frame, eye 0 or 1");
    check(count(censusH, "bool nativeTemporalEyeGeometry(uint32_t eye, uint64_t* sequence, float frustum[4], float shift[2]);") == 1,
          "the getter's declaration is in the census header, where native_temporal.cpp includes it");

    // THE EPISODES (design-world-camera-motion-2026-09-30.md section 6, Phase 0): with the key off nothing of them is installed, allocated, timed or logged.
    const std::string joinBody = functionBody(censusCpp, "void joinDraw(ID3D11DeviceContext* ctx, uint32_t ordinal) {");
    const std::string goLiveBody = functionBody(censusCpp, "void goLive(State* s) {");
    const std::string stepBody = functionBody(censusCpp, "void episodeStep(State* s) {");
    const std::string printEpisodeBody = functionBody(censusCpp, "void printEpisode(State* s) {");
    const std::string joinFacts = functionBody(censusCpp, "void readJoinFacts(State* s, ID3D11DeviceContext* ctx, VrCensusJoinRow* row, bool wantRows) {");
    check(!joinBody.empty() && !goLiveBody.empty() && !stepBody.empty() && !printEpisodeBody.empty() && !joinFacts.empty(), "the episodes' functions can be delimited in the source");
    check(startsWith(joinBody, "void joinDraw(ID3D11DeviceContext* ctx, uint32_t ordinal) {", "\n    State* s = g_state;\n    if (!s || !s->active || !ctx || !s->episodes.live()) return;\n"),
          "KEY OFF: the join's per-draw function returns first unless a census is active and an episode is live (and it is reachable only through the pointer goLive sets)");
    check(count(censusCpp, "detail::g_vrCensusJoinDraw = &joinDraw;") == 1 && goLiveBody.find("detail::g_vrCensusJoinDraw = &joinDraw;") != std::string::npos &&
              count(censusCpp, "goLive(s)") == 1 && stepBody.find("if (s->episodes.boundary(s->frame, ended, in)) goLive(s);") != std::string::npos,
          "KEY OFF: the join's pointer is set in one place (goLive), which only the episode step calls, when the episode machine says the armed frame has come");
    check(count(censusCpp, "detail::g_vrCensusJoinDraw = nullptr;") == 3 && functionBody(censusCpp, "void deactivate() {").find("detail::g_vrCensusJoinDraw = nullptr;") != std::string::npos &&
              activateBody.find("detail::g_vrCensusJoinDraw = nullptr;") != std::string::npos && rollBody.find("detail::g_vrCensusJoinDraw = nullptr;") < rollBody.find("printEpisode(s);") &&
              rollBody.find("printEpisode(s);") < rollBody.find("s->episodes.finish();"),
          "the pointer is cleared in three places: the census turning on, the key going off, and the boundary that ends the sampled frame (before the frame is printed)");
    check(count(censusH, "inline VrCensusJoinDrawFn g_vrCensusJoinDraw = nullptr;") == 1 && count(censusCpp + censusH, "g_vrCensusJoinDraw = &") == 1,
          "KEY OFF: the pointer starts null in its one definition, and is never given a value anywhere but goLive");
    check(boundary.find("if (!wanted) {") < boundary.find("episodeStep(s);") && boundary.find("rollFrame(s);") < boundary.find("episodeStep(s);") &&
              boundary.find("episodeStep(s);") < boundary.find("s->phase = readPhase();") && boundary.find("s->named = uiLayerLastFrameNamed();") < boundary.find("rollFrame(s);") &&
              boundary.find("journalGuiFocus(&s->gui)") < boundary.find("rollFrame(s);") && count(censusCpp, "uiLayerLastFrameNamed()") == 1 && count(censusCpp, "journalGuiFocus(") == 1,
          "the boundary reads the naming and GuiFocus of the frame that ended once, before it rolls it, steps the episodes after the roll (s->frame is then the frame that starts) and "
          "before the phase is latched; with the key off it returned before any of it");
    check(preBody.find("const bool timed = s->cpu.pick();") != std::string::npos && preBody.find("const bool timed = s->cpu.pick();") < preBody.find("ticksNow()") &&
              preBody.find("const int64_t t0 = timed ? ticksNow() : 0;") != std::string::npos && count(preBody, "ticksNow()") == 2 &&
              preBody.find("if (timed) s->cpu.notePre(call.willInject, static_cast<uint64_t>(ticksNow() - t0));") != std::string::npos &&
              count(censusCpp, "QueryPerformanceCounter(") == 1 && count(censusCpp, "PostTimer timer(s, p.timed, p.injected);") == 1 &&
              censusCpp.find("t0(isTimed ? ticksNow() : 0)") != std::string::npos,
          "THE DETOUR'S CPU: the clock is read only for the one call in sixteen the counter picks (both halves: a counter and a compare for every other call), by one performance-counter call, "
          "and the post half's timer reads it only when its call was picked");
    check(preBody.find("const bool episodeFrame = s->episodes.live();") != std::string::npos && preBody.find("record = s->epFrame.add(call.kindReadable, call.kind, callerRva);") != std::string::npos &&
              preBody.find("const bool recording = !episodeFrame && vrCensusMayRecord(s->foot, s->phase) && vrCensusPrintsSequence(true, s->sequencesLogged);") != std::string::npos,
          "an episode's frame records every call into its own buffer; the first-three-frames rule is untouched and never records the episode's frame (no duplicate lines)");
    check(count(censusCpp, "temporalPassChosenRows(") == 1 && printEpisodeBody.find("temporalPassChosenRows(in.chosen, &in.chosenBound)") != std::string::npos &&
              printEpisodeBody.find("temporalPassChosenRows(") < printEpisodeBody.find("vrCensusPrintEpisode(") && count(censusCpp, "vrCensusPrintEpisode(") == 1,
          "the pass's chosen rows are read once, in printEpisode, at the end of the sampled frame, where the pass's own boundary has not yet reset them, and the lines are the core's own "
          "(the same code the reader's fixture is built with)");
    check(startsWith(joinFacts, "void readJoinFacts(State* s, ID3D11DeviceContext* ctx, VrCensusJoinRow* row, bool wantRows) {", "\n    FlatComputeInternalScope internal;") &&
              joinFacts.find("depthWriteOf(ctx)") != std::string::npos &&
              joinFacts.find("readEyeRows(s, ctx, rows, &b1, &first, &bytes, &why, wantRows)") != std::string::npos,
          "the join's state reads and its one copy go through the same readback function as the eye draw's, inside the flat compute scope, and rows are asked for only when the signature is its depth's first");
    check(joinBody.find("bindingGet(BindSlot::Dsv0)") != std::string::npos && joinBody.find("depthCache.find(dsv)") != std::string::npos &&
              joinBody.find("if (dsv == s->joinLastDsv && vs == s->joinLastVs && ps == s->joinLastPs) {") < joinBody.find("depthCache.find(dsv)") &&
              joinBody.find("readJoinFacts(s, ctx, row, firstOfDepth);") != std::string::npos && joinBody.substr(joinBody.find('\n')).find("D3D11") == std::string::npos &&
              joinBody.find("->Get") == std::string::npos && joinBody.find("ctx->") == std::string::npos,
          "the common draw (the same depth and shaders as the one before) is three compares and no D3D call; a view is resolved once a frame, and only a new signature reads state");
    {   // the route's per-draw hook, and the temporal pass's accessor (read-only)
        const std::string routeCpp = slurp("src/d3d11/vr_world_route.cpp");
        const std::string routeDraw = functionBody(routeCpp, "void vrWorldRouteDraw(");
        check(!routeDraw.empty() && count(routeCpp, "g_vrCensusJoinDraw") == 2 &&
                  routeDraw.find("if (detail::g_vrCensusJoinDraw) detail::g_vrCensusJoinDraw(ctx, f.draws);") != std::string::npos &&
                  routeDraw.find("++f.draws;") < routeDraw.find("detail::g_vrCensusJoinDraw") && routeDraw.find("detail::g_vrCensusJoinDraw") < routeDraw.find("bindingGet(BindSlot::Rtv0)"),
              "the route's per-draw hook reaches the census's join through one null-tested pointer, after it counted the draw and before it looks at the colour target (depth-only draws count), "
              "and nothing else in the route touches it: a draw with the census off, or outside an episode's frame, costs one load");
        const std::string tpCpp = slurp("src/d3d11/temporal_pass.cpp");
        const std::string chosen = functionBody(tpCpp, "bool temporalPassChosenRows(float rows[12], bool* bound) {");
        check(!chosen.empty() && chosen.find("chooseCameraRows") == std::string::npos && chosen.find("g_chosenThisFrame =") == std::string::npos &&
                  chosen.find("g_curValid =") == std::string::npos && chosen.find("++") == std::string::npos && chosen.find("memcpy(rows, g_curRows, sizeof(g_curRows));") != std::string::npos &&
                  chosen.find("!g_chosenThisFrame || !g_curValid") != std::string::npos,
              "the accessor of the pass's chosen rows only reads: it chooses nothing, latches nothing and counts nothing, and answers only for a frame the pass chose rows in");
    }

    const std::vector<Pin> pins = {
        {&censusCpp, "FlatComputeInternalScope internal;", 2, "the eye readback's copy, Map and Unmap, and the join's state reads and copy, run inside FlatComputeInternalScope (the hooks step aside)"},
        {&censusCpp, "D3D11_USAGE_STAGING", 1, "the readback goes through one staging buffer (the eye's and the join's)"},
        {&censusCpp, "box{offset, 0, 0, offset + 64u, 1, 1}", 1, "the copy is the 64 bytes of rows 270..273 only"},
        {&censusCpp, "s->eye.take(s->frame)", 1, "an eye draw is read back only when the eye budget takes it"},
        {&censusCpp, "const bool firstOfDepth = !s->join.hasDepth(d->depth);", 1, "a join readback is spent on a depth's first signature only (at most four a frame)"},
    };
    runPins(pins, "source pin control: a source with the line removed no longer contains it");
}

// ---------------------------------------------------------------------------
// The reader's fixture: a synthetic flight log written by the very formatters the DLL compiles, from cameras built by the
// derive model, so the rows the eye draws read back are the rows a camera's call composed. tools\edvr_log.py's
// --camera-census self-test reads tools\camera_census_fixture.log; this rig holds that file to exactly what the formatters
// write (--print-fixture regenerates it), so a drift of either the writer or the reader breaks a build.
//
// The story is flight 1's, one stage on: an on-foot session with the world route jittering (stage 2).
//   - the WORLD camera is one object that was the left EYE camera in the cockpit (its first-sight line says kind 5) and is the
//     kind-3 world camera on foot; it is refreshed with two projections, the scene's (near 0.025) and the first-person weapon
//     camera's (a tighter field of view, near 0.0675), both carrying the frame's phase in their bound pair (the injector's own
//     rule: bound += (jx / W, -jy / H), so the rows measure flatProjectionJitter's shift);
//   - an ortho UI camera (kind 1), an AUXILIARY kind-3 camera (excluded: inj=0 role=aux) that was a kind-5 call in the first logged
//     frame (one camera, two kinds in two frames), and an eye camera per eye (kind 5, three call sites each, after the tone, never
//     injected, the eye shift off because the route owns the world);
//   - four on-foot frames 4..7, each with a NON-ZERO phase (a zero-phase frame is not sampled while the route jitters): the first three
//     carry a call sequence, all four an eye draw per eye;
//   - the route's own two 5 s lines, back to back, before each census 5 s line (the route's side is written here as text).
// ---------------------------------------------------------------------------
struct FixtureCam {
    uintptr_t ptr;
    uint32_t kind;
    float fov, aspect, nearZ;
    uint32_t caller;
    float viewportW, viewportH;
    uintptr_t view;   // the pass object the refresh is handed with the camera
};
std::string fixtureLog() {
    std::string out;
    int ms = 0;
    auto put = [&](const std::string& text) {
        char stamp[32];
        const int total = 43200000 + ms;   // 12:00:00 and up
        std::snprintf(stamp, sizeof(stamp), "[%02d:%02d:%02d.%03d] ", total / 3600000, total / 60000 % 60, total / 1000 % 60, total % 1000);
        out += stamp;
        out += text;
        out += "\n";
        ms += 37;
    };
    char line[kVrCensusLineBytes + 1];
    put("version 0.18.0-rc.4-31-g0a1b2c3d (build 68C0A1F2) -- synthetic fixture for edvr_log.py --camera-census");
    put("vr camera census: on (advanced.vr_camera_census); the census itself never writes a camera (the route's injection, when it runs, is its own); owner thread 4321; "
        "5 s lines: 36 windows, then one per 12; cameras first 64, call sequences first 3 and eye draws first 4 on-foot frames "
        "(tone drawn, journal read=yes says on foot; while the route jitters, only a non-zero phase); line budget 724");

    const float renderW = 5040.0f, renderH = 2835.0f;
    const FixtureCam world{0x241dc2e2960, 3, 1.0122f, renderW / renderH, 0.025f, 0x594E13, renderW, renderH, 0x241dd00a000};
    const float firstPersonFov = 0.8453f, firstPersonNear = 0.0675f;   // x1.23 tighter, a larger near plane: flight 1's weapon camera
    const uintptr_t worldViews[3] = {0x241dd00a000, 0x241dd00a800, 0x241dd00b000};
    const FixtureCam ui{0x241de000100, 1, 0.0f, 1.0f, 0.1f, 0x58DE73, renderW, renderH, 0x241dd00c000};
    const uintptr_t auxPtr = 0x241de100200, auxView = 0x241dd00c800;
    const float frustumOf[2][4] = {{-1.2f, 0.7f, -0.9f, 1.1f}, {-0.7f, 1.2f, -0.9f, 1.1f}};
    const uintptr_t eyePtr[2] = {0x241df6d0bb0, 0x241df6d0ff0};
    const uintptr_t eyeView[2] = {0x241dd00e000, 0x241dd00f000};
    const uint32_t callers3[3] = {0x594E13, 0x594EAB, 0x594FE1};
    // What the route chose for each on-foot frame 4..7: the raster phase in render pixels, positive right/down (about 1e-4 NDC).
    const VrCensusPhase phaseOf[4] = {{true, 0.2520f, -0.1260f}, {true, -0.1890f, 0.0630f}, {true, 0.1260f, 0.2520f}, {true, -0.2520f, -0.0630f}};
    // The eye cameras: built from EDVR's advertised frustum with the eye shift OFF (the route owns the world), so the bound pair is the
    // frustum's own and does not move from frame to frame.
    struct EyeCam { float bx, by, aspect, fov; };
    EyeCam eyeCam[2];
    for (int e = 0; e < 2; ++e) {
        const double l = frustumOf[e][0], r = frustumOf[e][1], d = frustumOf[e][2], u = frustumOf[e][3];
        const double wHalf = (r - l) / 2, tanHalf = (u - d) / 2;
        eyeCam[e].bx = static_cast<float>(-(r + l) / (4 * wHalf));
        eyeCam[e].by = static_cast<float>(-(u + d) / (4 * tanHalf));
        eyeCam[e].aspect = static_cast<float>(wHalf / tanHalf);
        eyeCam[e].fov = static_cast<float>(2 * std::atan(tanHalf));
    }
    const EyeCam auxEye{0.0f, 0.0f, 1.0f, 1.5708f};   // the auxiliary camera's kind-5 face in the first logged frame
    // The injector's own rule: the frame's phase goes into the bound pair as (jx / W, -jy / H); the rows then measure 2x that.
    auto phaseBound = [&](const VrCensusPhase& p, float* bx, float* by) { *bx = p.x / renderW; *by = -p.y / renderH; };

    // Three lines of the world route's 5 s window, written by the route's OWN formatters (vr_world_route_math.h: vrWorldFormatWindow,
    // vrWorldFormatInjectWindow and, while the census is on, vrWorldFormatRefusalWindow), so the fixture the reader's self-test parses is
    // what the route prints and a change to any line fails this rig's fixture pin until the fixture is regenerated and the reader parses it.
    auto routeWindow = [&](VrWorldState state, bool gate, const VrWorldWindow& win, const VrWorldRefusalWindow& refusal) {
        char text[1400];
        vrWorldFormatWindow(text, sizeof(text), VrWorldKey::Auto, state, true, gate, win);
        put(text);
        vrWorldFormatInjectWindow(text, sizeof(text), win.inject);
        put(text);
        vrWorldFormatRefusalWindow(text, sizeof(text), refusal);
        put(text);
    };

    // The first window: a commander in a ship. The route is idle, the tone is drawn every frame, the journal says not on foot, so
    // nothing is sampled.
    {
        VrWorldWindow rw;
        rw.hdr.frames = 448; rw.gateFrames = 448; rw.hdr.lastVerdict = "none"; rw.jitter = "idle";
        VrWorldRefusalWindow rf;   // the census is on and the route treats nothing: "on, and nothing asked" (treated=0 asked=0 sampled=0 pixels=0)
        rf.census = true; rf.every = kFlatMonoRefusalEvery;
        routeWindow(VrWorldState::Observing, false, rw, rf);
    }
    {
        VrCensusWindow w;
        w.frames = 448; w.calls = 41788; w.posts = 41788;
        w.kinds[1] = 896; w.kinds[3] = 38204; w.kinds[5] = 2688;
        w.callers[0] = {0x594E13, 13895}; w.callers[1] = {0x594EAB, 13895}; w.callers[2] = {0x594FE1, 13895}; w.callers[3] = {0x58DE73, 103}; w.callerCount = 4;
        w.cameraCount = 3; w.toneBefore = 32000; w.toneAfter = 9788; w.toneFrames = 448; w.windows = 1; w.progressSeen = true;
        w.eyeDraws = 896;
        VrCensusWindowText t;
        t.hook = "installed"; t.camerasTotal = 3; t.foot = VrCensusFoot::No;
        vrCensusFormatWindow(line, sizeof(line), w, t);
        put(line);
        // The window's companions: the episodes' counters (the key-on trigger armed the first episode, sampled at frame 31), no on-foot frame (the journal says not on foot), and the
        // observer halves timed on 1 call in 16 (a 10 MHz clock: a tick is 0.1 us).
        VrCensusEpisodeCounters counters;
        counters.taken = 1; counters.triggers = 1; counters.skipped = 0;
        vrCensusFormatEpisodeCounters(line, sizeof(line), counters, 1, false);
        put(line);
        vrCensusFormatRuns(line, sizeof(line), VrCensusRuns{}, 1);
        put(line);
        VrCensusCpu cpu;
        for (int i = 0; i < 2612; ++i) { cpu.notePre(false, 30 + (i % 7)); cpu.notePost(false, 22 + (i % 5)); }
        cpu.notePre(false, 520);
        vrCensusFormatCpu(line, sizeof(line), w, cpu, 10000000, 1);
        put(line);
    }

    // Camera lines: the first sight of each, in the order the boundary prints them. A kind-5 camera (an eye's) is the game's custom matrix:
    // the bound pair is zero, the off-centre terms are the matrix's own, there are no tangents and no viewport (as flight 1's lines were).
    auto cam5Line = [&](uintptr_t ptr, uint32_t caller, uintptr_t view, const EyeCam& ec, VrCensusTone tone, uint32_t ordinal, bool drawKnown,
                        uint32_t draw, uint64_t frame) {
        VrCensusCamera row;
        row.camera = ptr;
        VrCensusSig& s = row.firstSig;
        s.kind = 5; s.aspect = ec.aspect; s.nearZ = 0.025f; s.farZ = 50000.0f; s.fov = ec.fov;
        s.boundX = 0.0f; s.boundY = 0.0f; s.viewportW = 0.0f; s.viewportH = 0.0f; s.p8 = 2.0f * ec.bx; s.p9 = 2.0f * ec.by;
        row.tanValid = false;
        row.callerRva = caller; row.firstOrdinal = ordinal; row.firstDraw = draw; row.firstDrawKnown = drawKnown; row.firstTone = tone; row.firstFrame = frame;
        row.firstView = view; row.firstCtx = 0x241dce749f40ull;
        vrCensusFormatCamera(line, sizeof(line), row);
        put(line);
    };
    auto camLine = [&](const FixtureCam& c, float bx, float by, VrCensusTone tone, uint32_t ordinal, uint32_t draw, uint64_t frame) {
        Model m(c.kind, c.fov, c.aspect, bx, by, c.nearZ, 50000.0f);
        c2derive::camF(m.cam, c2derive::kCamViewportW) = c.viewportW;   // not part of the derivation: no re-derive needed
        c2derive::camF(m.cam, c2derive::kCamViewportH) = c.viewportH;
        VrCensusCamera row;
        row.camera = c.ptr;
        vrCensusSigFromSnap(m.snap(), &row.firstSig);
        row.tanValid = vrCensusTangents(row.firstSig, row.tan);
        row.callerRva = c.caller; row.firstOrdinal = ordinal; row.firstDraw = draw; row.firstDrawKnown = true; row.firstTone = tone; row.firstFrame = frame;
        row.firstView = c.view; row.firstCtx = c.view + 0x100;
        vrCensusFormatCamera(line, sizeof(line), row);
        put(line);
    };
    // The world camera's object was the LEFT EYE camera in the cockpit: its first-sight line, at the first frame, says kind 5 and is the
    // eye's (tone none: the route reported no progress yet).
    cam5Line(world.ptr, 0x594E13, eyeView[0], eyeCam[0], VrCensusTone::None, 19, false, 0, 1);
    camLine(ui, 0.0f, 0.0f, VrCensusTone::Before, 2, 0, 1);
    // The on-foot frames 4..7: the cameras first seen on foot (an auxiliary camera that is a kind-5 call in frame 4, and the two eyes).
    const uint64_t firstOnFoot = 4;
    cam5Line(auxPtr, 0x594FE1, auxView, auxEye, VrCensusTone::After, 97, true, 8209, firstOnFoot);
    for (int e = 0; e < 2; ++e) cam5Line(eyePtr[e], 0x594E13, eyeView[e], eyeCam[e], VrCensusTone::After, 103 + 3 * e, true, 8210 + static_cast<uint32_t>(e), firstOnFoot);

    // One 'changed:' line as the boundary prints it: the signature the camera had, the one it has now.
    auto changedLine = [&](uintptr_t ptr, uint64_t frame, uint32_t n, const VrCensusSig& from, const VrCensusSig& to) {
        VrCensusCamera row;
        row.camera = ptr;
        row.changeFrom = from; row.sig = to; row.changeFrame = frame; row.changes = n;
        vrCensusFormatChanged(line, sizeof(line), row);
        put(line);
    };
    auto sigFor = [&](uint32_t kind, float aspect, float nearZ, float fov, float bx, float by, float vw, float vh) {
        VrCensusSig s = sigOf(kind, aspect, nearZ, fov, bx, by);
        s.viewportW = vw; s.viewportH = vh;
        return s;
    };
    auto axesOf = [](Model& m, float yaw, float pitch) {
        float* a = &c2derive::camF(m.cam, c2derive::kCamAxes);
        const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
        a[0] = cy; a[1] = 0.0f; a[2] = -sy; a[3] = 0.0f;
        a[4] = sy * sp; a[5] = cp; a[6] = cy * sp; a[7] = 0.0f;
        a[8] = sy * cp; a[9] = -sp; a[10] = cy * cp;
        c2derive::camU(m.cam, c2derive::kCamFlags) |= c2derive::kFlagProj | c2derive::kFlagVP;
        c2derive::derive(m.cam);
    };
    // The camera a call composed its rows from: a perspective model with the call's own terms, the head turned as the frame turns it. The
    // world's first-person camera shares the world camera's object, so it turns with it; the eyes follow the head, not the world.
    auto composeCall = [&](float fov, float aspect, float bx, float by, float nearZ, float yaw, float rows[16]) {
        Model m(3, fov, aspect, bx, by, nearZ);
        axesOf(m, yaw, -0.12f);
        return vrCensusComposeRows(m.snap(), rows);
    };

    // One sequence per on-foot frame (4, 5, 6), then the eye draws of frames 4..7 with their geometry and leak.
    uint32_t worldChanges = 0;
    for (int f = 0; f < 4; ++f) {
        const uint64_t frame = firstOnFoot + f;
        float bx = 0.0f, by = 0.0f, prevBx = 0.0f, prevBy = 0.0f;
        phaseBound(phaseOf[f], &bx, &by);
        if (f > 0) phaseBound(phaseOf[f - 1], &prevBx, &prevBy);
        // The 'changed:' lines of a camera whose terms moved since the last frame: the world camera's kind and field at the first on-foot frame,
        // its bound pair (the phase) in every frame; the auxiliary camera's kind when it stopped being a kind-5 call.
        if (f < 3) {
            worldChanges += 9;
            const VrCensusSig from = f == 0 ? sigFor(5, eyeCam[0].aspect, 0.025f, eyeCam[0].fov, 0.0f, 0.0f, 0.0f, 0.0f)
                                            : sigFor(3, world.aspect, 0.025f, world.fov, prevBx, prevBy, renderW, renderH);
            changedLine(world.ptr, frame, worldChanges, from, sigFor(3, world.aspect, 0.025f, world.fov, bx, by, renderW, renderH));
            if (f == 1) changedLine(auxPtr, frame, 1, sigFor(5, auxEye.aspect, 0.025f, auxEye.fov, 0.0f, 0.0f, 0.0f, 0.0f),
                                    sigFor(3, 1.0f, 0.1f, 1.5708f, 0.0f, 0.0f, 2048.0f, 2048.0f));
        }
        if (f < 3) {
            struct Call { uintptr_t ptr, view; uint32_t kind; float fov, aspect, bx, by, nearZ, yaw; uint32_t caller, draw; bool rows; VrCensusTone tone; bool inject; uint8_t role; };
            std::vector<Call> calls;
            const float worldYaw = 0.35f + 0.004f * static_cast<float>(f), eyeYaw = 0.02f * static_cast<float>(f);
            uint32_t draw = 40;
            for (int i = 0; i < 12; ++i) {
                draw += 310 + 17 * i;
                calls.push_back({world.ptr, worldViews[i % 3], 3, world.fov, world.aspect, bx, by, world.nearZ, worldYaw, callers3[i % 3], draw, true,
                                 VrCensusTone::Before, true, 0});
            }
            for (int i = 0; i < 3; ++i) {
                draw += 55;
                calls.push_back({world.ptr, worldViews[2], 3, firstPersonFov, world.aspect, bx, by, firstPersonNear, worldYaw, 0x594E13, draw, true,
                                 VrCensusTone::Before, true, 1});
            }
            for (int i = 0; i < 2; ++i) calls.push_back({ui.ptr, ui.view, 1, ui.fov, ui.aspect, 0.0f, 0.0f, ui.nearZ, worldYaw, 0x58DE73, draw + 9, false,
                                                         VrCensusTone::Before, false, kVrCensusRoleNone});
            if (f == 0) calls.push_back({auxPtr, auxView, 5, auxEye.fov, auxEye.aspect, 0.0f, 0.0f, 0.025f, eyeYaw, 0x594FE1, 8209, true,
                                         VrCensusTone::After, false, kVrCensusRoleNone});
            else calls.push_back({auxPtr, auxView, 3, 1.5708f, 1.0f, 0.0f, 0.0f, 0.1f, worldYaw, 0x58DE73, draw + 12, true, VrCensusTone::Before, false, 2});
            for (int i = 0; i < 6; ++i) {
                const int e = i / 3;
                calls.push_back({eyePtr[e], eyeView[e], 5, eyeCam[e].fov, eyeCam[e].aspect, eyeCam[e].bx, eyeCam[e].by, 0.025f, eyeYaw, callers3[i % 3],
                                 8210u + static_cast<uint32_t>(i), true, VrCensusTone::After, false, kVrCensusRoleNone});
            }
            vrCensusFormatSequence(line, sizeof(line), frame, static_cast<uint32_t>(f + 1), VrCensusFoot::Yes, phaseOf[f],
                                   static_cast<uint32_t>(calls.size()), static_cast<uint32_t>(calls.size()));
            put(line);
            uint32_t ordinal = 0;
            for (const Call& c : calls) {
                ++ordinal;
                VrCensusCall rec;
                rec.camera = c.ptr; rec.view = c.view; rec.kind = c.kind; rec.kindReadable = true; rec.callerRva = c.caller;
                rec.draw = c.draw; rec.drawKnown = true;
                rec.tone = c.tone; rec.preFlags = 0x1C; rec.postSeen = true;
                rec.willInject = c.inject; rec.role = c.role;
                rec.rowsValid = c.rows && composeCall(c.fov, c.aspect, c.bx, c.by, c.nearZ, c.yaw, rec.rows);
                rec.postFlags = c.rows ? 0x0 : 0x4;
                vrCensusFormatCall(line, sizeof(line), frame, ordinal, rec);
                put(line);
            }
        }
        // The eye draws of this frame: the rows the same camera composed in the sequence, read back from b1.
        for (int e = 0; e < 2; ++e) {
            float rows[16];
            composeCall(eyeCam[e].fov, eyeCam[e].aspect, eyeCam[e].bx, eyeCam[e].by, 0.025f, 0.02f * static_cast<float>(f), rows);
            float six[6][4] = {};
            std::memcpy(six, rows, sizeof(rows));
            double mx = 0, my = 0;
            const bool measured = flatCameraMeasureRowShift(six, mx, my);
            vrCensusFormatEye(line, sizeof(line), static_cast<uint32_t>(e), frame, VrCensusFoot::Yes, phaseOf[f], true, 8213u + 7u * static_cast<uint32_t>(e),
                              0x1eb2e751e20ull, 0, 5376, rows, measured, mx, my, nullptr);
            put(line);
            const float noShift[2] = {0.0f, 0.0f};
            vrCensusFormatEyeGeometry(line, sizeof(line), static_cast<uint32_t>(e), frame, true, 4700 + frame, frustumOf[e], noShift, measured, mx, my);
            put(line);
        }
    }
    // THE EPISODES (Phase 0), written by the very function the DLL prints them with (vrCensusPrintEpisode), from calls built by the derive model so the rows a join read
    // back are the rows a call composed and the axes the pass's rows are matched against are the axes of that call's camera.
    //   1. the key-on episode, frame 31, in the cockpit (the journal says not on foot, GuiFocus 0): the eyes' kind-5 calls, a kind-3 scene camera and the UI's kind 1; the join finds
    //      each eye's first draw into its depth, matches its rows to that eye's kind-5 calls, and the pass chose rows equal to eye 0's view axes (H3: the eye cameras' rows drive it);
    //   2. a GuiFocus 0 -> 6 episode (the galaxy map), frame 1560: the pass's rows are NOT an eye camera's but a kind-3 camera's, and as the transpose (where the chooser strays);
    //   3. a naming flip on foot (named -> unnamed: a map opened), frame 6120, the 2D screen's depth: its first draw's rows match nothing the refresh composed.
    {
        struct EpCall { uintptr_t ptr, view; uint32_t kind; float fov, aspect, bx, by, nearZ, yaw; uint32_t caller, draw; VrCensusTone tone; };
        auto fillCall = [&](VrCensusEpisodeFrame& ep, const EpCall& c, float rowsOut[16], float axesOut[12]) {
            Model m(3, c.fov, c.aspect, c.bx, c.by, c.nearZ);
            axesOf(m, c.yaw, -0.12f);
            VrCensusCall* rec = ep.add(true, c.kind, c.caller);
            rec->camera = c.ptr; rec->view = c.view; rec->kind = c.kind; rec->kindReadable = true; rec->callerRva = c.caller;
            rec->draw = c.draw; rec->drawKnown = true; rec->tone = c.tone; rec->preFlags = 0x1C; rec->postSeen = true; rec->postFlags = 0x0;
            rec->rowsValid = vrCensusComposeRows(m.snap(), rec->rows);
            const VrCensusSnap snap = m.snap();
            std::memcpy(rec->axes, snap.bytes + (kVrCensusAxes - kVrCensusSnapFrom), sizeof(rec->axes));
            rec->axesValid = true;
            if (rowsOut) std::memcpy(rowsOut, rec->rows, sizeof(rec->rows));
            if (axesOut) std::memcpy(axesOut, rec->axes, sizeof(rec->axes));
        };
        auto printEp = [&](const VrCensusEpisodePrint& in, VrCensusEpisodeFrame& ep, VrCensusJoin& join) {
            vrCensusPrintEpisode([&](VrCensusLines, const char* text) { put(text); }, in, ep, join);
        };
        auto joinRow = [&](VrCensusJoin& join, VrCensusJoinDepth depth, uint32_t w, uint32_t h, uint64_t vs, uint64_t ps, uint32_t draw, uint64_t draws, int8_t dw, uint64_t b1,
                           const float* rows, const char* why) {
            VrCensusJoinRow* r = join.add(depth, w, h, vs, ps, draw);
            r->draws = draws; r->depthWrite = dw; r->b1Bound = b1 != 0; r->b1 = b1; r->b1Bytes = b1 ? 5376 : 0; r->rowsAsked = rows != nullptr || why != nullptr;
            if (rows) { r->rowsRead = true; std::memcpy(r->rows, rows, sizeof(r->rows)); } else r->why = why;
            return r;
        };
        std::unique_ptr<VrCensusEpisodeFrame> ep(new VrCensusEpisodeFrame);
        VrCensusJoin join;
        const uint64_t sceneVs = 0x5C36AF051B98B9F1ull, scenePs = 0xCFE84157BC76E921ull, glassVs = 0x11A2B3C4D5E6F708ull, glassPs = 0x0F0E0D0C0B0A0908ull;
        float eyeRows0[16], eyeRows1[16], sceneRows[16], eyeAxes0[12], sceneAxes[12];
        {   // 1. key-on, in the cockpit
            ep->reset();
            join.begin();
            for (int i = 0; i < 2; ++i) fillCall(*ep, {ui.ptr, ui.view, 1, ui.fov, ui.aspect, 0.0f, 0.0f, ui.nearZ, 0.7f, 0x58DE73, 0, VrCensusTone::Before}, nullptr, nullptr);
            for (int i = 0; i < 6; ++i)
                fillCall(*ep, {world.ptr, worldViews[i % 3], 3, world.fov, world.aspect, 0.0f, 0.0f, world.nearZ, 0.35f + 0.01f * static_cast<float>(i / 2), callers3[i % 3],
                               static_cast<uint32_t>(120 + 40 * i), VrCensusTone::Before}, i == 0 ? sceneRows : nullptr, i == 0 ? sceneAxes : nullptr);
            for (int i = 0; i < 6; ++i) {
                const int e = i / 3;
                fillCall(*ep, {eyePtr[e], eyeView[e], 5, eyeCam[e].fov, eyeCam[e].aspect, eyeCam[e].bx, eyeCam[e].by, 0.025f, 0.02f + 0.001f * static_cast<float>(e), callers3[i % 3],
                               8210u + static_cast<uint32_t>(i), VrCensusTone::After}, i % 3 == 0 ? (e == 0 ? eyeRows0 : eyeRows1) : nullptr, i == 0 ? eyeAxes0 : nullptr);
            }
            joinRow(join, VrCensusJoinDepth::Eye0, 2620, 2533, sceneVs, scenePs, 8210, 1432, 1, 0x1eb2e751e20ull, eyeRows0, nullptr);
            joinRow(join, VrCensusJoinDepth::Eye0, 2620, 2533, glassVs, glassPs, 8650, 61, 0, 0x1eb2e751e20ull, nullptr, "skip");
            joinRow(join, VrCensusJoinDepth::Eye1, 2620, 2533, sceneVs, scenePs, 8215, 1432, 1, 0x1eb2e751e20ull, eyeRows1, nullptr);
            join.seen = 3100; join.views = 4;   // the shadow atlas and the three depths joined (2,925 draws into them)
            VrCensusEpisodePrint in;
            in.n = 1; in.frame = 31; in.armedFrame = 1; in.trigger = VrCensusTrigger{VrCensusTriggerKind::KeyOn, 0, 0};
            in.foot = VrCensusFoot::No; in.guiKnown = true; in.gui = 0; in.named = false;
            in.haveChosen = true; in.chosenBound = true;
            std::memcpy(in.chosen, eyeAxes0, sizeof(in.chosen));
            in.chosen[3] = in.chosen[7] = in.chosen[11] = 0.0f;
            printEp(in, *ep, join);
        }
        {   // 2. the galaxy map opened (GuiFocus 0 -> 6): the pass's rows are the scene camera's axes, transposed
            ep->reset();
            join.begin();
            for (int i = 0; i < 4; ++i) fillCall(*ep, {ui.ptr, ui.view, 1, ui.fov, ui.aspect, 0.0f, 0.0f, ui.nearZ, 0.5f, 0x58DE73, 0, VrCensusTone::Before}, nullptr, nullptr);
            for (int i = 0; i < 5; ++i)
                fillCall(*ep, {world.ptr, worldViews[i % 3], 3, world.fov, world.aspect, 0.0f, 0.0f, world.nearZ, 0.9f, callers3[i % 3], static_cast<uint32_t>(90 + 30 * i),
                               VrCensusTone::Before}, i == 0 ? sceneRows : nullptr, i == 0 ? sceneAxes : nullptr);
            float eye1Rows[16];
            for (int i = 0; i < 6; ++i) {
                const int e = i / 3;
                fillCall(*ep, {eyePtr[e], eyeView[e], 5, eyeCam[e].fov, eyeCam[e].aspect, eyeCam[e].bx, eyeCam[e].by, 0.025f, 0.5f, callers3[i % 3], 8300u + static_cast<uint32_t>(i),
                               VrCensusTone::After}, i == 3 ? eye1Rows : nullptr, nullptr);
            }
            joinRow(join, VrCensusJoinDepth::Eye0, 2620, 2533, sceneVs, scenePs, 8300, 900, 1, 0x1eb2e751e20ull, nullptr, "map");
            joinRow(join, VrCensusJoinDepth::Eye1, 2620, 2533, sceneVs, scenePs, 8305, 900, 1, 0x1eb2e751e20ull, eye1Rows, nullptr);
            join.seen = 2100; join.views = 3;
            VrCensusEpisodePrint in;
            in.n = 2; in.frame = 1560; in.armedFrame = 1530; in.trigger = VrCensusTrigger{VrCensusTriggerKind::Gui, 0, 6};
            in.foot = VrCensusFoot::No; in.guiKnown = true; in.gui = 6; in.named = false;
            in.haveChosen = true; in.chosenBound = false;
            for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) in.chosen[r * 4 + c] = c < 3 ? sceneAxes[c * 4 + r] : 0.0f;   // the transpose of the scene camera's axes
            printEp(in, *ep, join);
        }
        {   // 3. on foot, the naming flipped to unnamed (a map): the 2D screen's own depth, whose first draw's rows no refresh call composed
            ep->reset();
            join.begin();
            for (int i = 0; i < 6; ++i)
                fillCall(*ep, {world.ptr, worldViews[i % 3], 3, world.fov, world.aspect, 0.0f, 0.0f, world.nearZ, 0.6f, callers3[i % 3], static_cast<uint32_t>(60 + 25 * i),
                               VrCensusTone::Before}, nullptr, nullptr);
            float mapRows[16];
            for (int i = 0; i < 16; ++i) mapRows[i] = 0.01f * static_cast<float>(i + 1);
            joinRow(join, VrCensusJoinDepth::Screen, 5040, 2835, 0xDFED8E1C9E191BECull, 0x143AAE0597E2F7BFull, 41, 22, 1, 0x1eb2e751e20ull, mapRows, nullptr);
            joinRow(join, VrCensusJoinDepth::Screen, 5040, 2835, glassVs, glassPs, 77, 311, 0, 0x1eb2e751e20ull, nullptr, "skip");
            joinRow(join, VrCensusJoinDepth::EyeUnknown, 2620, 2533, glassVs, scenePs, 90, 4, 1, 0, nullptr, "no-b1");
            join.seen = 905; join.views = 5;
            VrCensusEpisodePrint in;
            in.n = 3; in.frame = 6120; in.armedFrame = 6090; in.trigger = VrCensusTrigger{VrCensusTriggerKind::Naming, 1, 0};
            in.foot = VrCensusFoot::Yes; in.guiKnown = false; in.named = false; in.phase = phaseOf[0];
            in.haveChosen = false;
            printEp(in, *ep, join);
        }
    }
    // The on-foot window: the route's two lines, then the census's 5 s line, which says how many calls the detour injected.
    {
        VrWorldWindow rw;
        rw.hdr.frames = 450; rw.gateFrames = 450; rw.gateFlips = 1; rw.hdr.hdrFrames = 450; rw.hdr.triggerFrames = 450;
        rw.hdr.treated = 450; rw.ownedFrames = 450; rw.takes = 900; rw.layerOnly = 900; rw.enters = 1;
        rw.hdr.lastVerdict = "treated"; rw.jitter = "on"; rw.phaseX = -0.2520f; rw.phaseY = -0.0630f; rw.rowsX = 0.1260f; rw.rowsY = 0.2520f;
        rw.foldMode[1] = 450;
        rw.hdr.lastTriggerVs = 0xDFED8E1C9E191BECull; rw.hdr.lastTriggerPs = 0x143AAE0597E2F7BFull;
        rw.hdr.lastTargetWidth = 2520; rw.hdr.lastTargetHeight = 1417; rw.hdr.lastHdrWidth = 5040; rw.hdr.lastHdrHeight = 2835;
        rw.hdr.noteSelection("selected");
        for (int i = 1; i < 450; ++i) rw.hdr.noteSelection("selected");
        rw.inject.scene = 5400; rw.inject.firstPerson = 1350; rw.inject.auxiliary = 450; rw.inject.unsupported = 2700;
        rw.inject.otherKind = 900; rw.inject.injectedKind[3] = 6750; rw.inject.pairChecked = 448;
        rw.inject.fovNarrowest = 0.8203f; rw.inject.fovWidest = 0.9831f;   // the struct's two fields of view: the weapon's and the scene's
        // The refusal census over the window: one sample in four of 450 asking resolves (113 dispatched, 112 read back, none dropped), each
        // sample the whole 5040x2835 render. 3.65% of the pixels refused: the sky (sentinel), stale slots, a few masked records and
        // out-of-range reprojections; the first-person pixels are credited (mode 1), so almost none are refused for the weapon.
        VrWorldRefusalWindow rf;
        rf.census = true; rf.every = kFlatMonoRefusalEvery; rf.treated = 450; rf.asked = 450; rf.sampled = 113; rf.read = 112; rf.dropped = 0;
        rf.width = 5040; rf.height = 2835; rf.pixels = 112ull * 5040ull * 2835ull;
        rf.counts[kFlatMonoClassStale] = 40007520; rf.counts[kFlatMonoClassMasked] = 800150; rf.counts[kFlatMonoClassSentinel] = 16003008;
        rf.counts[kFlatMonoClassRange] = 1600300; rf.counts[kFlatMonoClassWeaponRefused] = 0;
        routeWindow(VrWorldState::Owned, true, rw, rf);
    }
    {
        VrCensusWindow w;
        w.frames = 450; w.calls = 10800; w.posts = 10800; w.stale = 0; w.injCalls = 6750;
        w.kinds[1] = 900; w.kinds[3] = 7200; w.kinds[5] = 2700;
        w.callers[0] = {0x594E13, 3150}; w.callers[1] = {0x594EAB, 3150}; w.callers[2] = {0x594FE1, 3150}; w.callers[3] = {0x58DE73, 1350}; w.callerCount = 4;
        w.cameraCount = 5; w.toneBefore = 8100; w.toneAfter = 2700; w.toneFrames = 450; w.onFootFrames = 450; w.eyeDraws = 900; w.eyeOnFoot = 900;
        w.windows = 1; w.progressSeen = true;
        VrCensusWindowText t;
        t.hook = "installed"; t.camerasTotal = 5; t.foot = VrCensusFoot::Yes;
        vrCensusFormatWindow(line, sizeof(line), w, t);
        put(line);
        // The episodes taken: key-on, the galaxy map, a map on foot; two triggers found one armed.
        VrCensusEpisodeCounters counters;
        counters.taken = 3; counters.triggers = 5; counters.skipped = 2;
        vrCensusFormatEpisodeCounters(line, sizeof(line), counters, 1, false);
        put(line);
        // On foot: 413 frames counted, the world named almost all of them; two one-frame blips and a two-frame one of unnamed, and a seven-frame stretch (a map).
        VrCensusRuns runs;
        runs.frames = 413;
        runs.named[0] = 1; runs.named[5] = 1; runs.named[6] = 1;
        runs.unnamed[0] = 2; runs.unnamed[1] = 1; runs.unnamed[3] = 1;
        runs.longestNamed = 211; runs.longestUnnamed = 7; runs.open = 1; runs.openLength = 211;
        vrCensusFormatRuns(line, sizeof(line), runs, 1);
        put(line);
        VrCensusCpu cpu;
        for (int i = 0; i < 253; ++i) { cpu.notePre(false, 31 + (i % 6)); cpu.notePost(false, 24 + (i % 4)); }
        for (int i = 0; i < 422; ++i) { cpu.notePre(true, 36 + (i % 8)); cpu.notePost(true, 26 + (i % 3)); }
        cpu.notePre(true, 610);
        vrCensusFormatCpu(line, sizeof(line), w, cpu, 10000000, 1);
        put(line);
    }
    return out;
}


// Two log texts are the same when they differ at most in the last digits of the numbers in them: every line's text is
// identical and every number is within 1e-5 (relative above one). The scripted session builds its cameras with the
// library's tan, cos, sin and atan, which may round their last bit differently on another CPU, so the rows it prints can
// differ in the seventh digit from one machine to the next; the text the formatters write cannot.
bool sameWithinRounding(const std::string& a, const std::string& b, std::string* why) {
    size_t i = 0, j = 0;
    auto number = [](const std::string& s, size_t at, size_t* end, double* value) {
        size_t k = at;
        if (k < s.size() && s[k] == '-') ++k;
        if (k >= s.size() || !std::isdigit(static_cast<unsigned char>(s[k]))) return false;
        // A hex literal (0x241dc2e2960, +0x594E13, fl=0x1C) is text, not a number.
        if (s[k] == '0' && k + 1 < s.size() && s[k + 1] == 'x') return false;
        while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) ++k;
        if (k + 1 < s.size() && s[k] == '.' && std::isdigit(static_cast<unsigned char>(s[k + 1]))) {
            ++k;
            while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) ++k;
        }
        if (k + 1 < s.size() && (s[k] == 'e' || s[k] == 'E') &&
            (std::isdigit(static_cast<unsigned char>(s[k + 1])) ||
             ((s[k + 1] == '-' || s[k + 1] == '+') && k + 2 < s.size() && std::isdigit(static_cast<unsigned char>(s[k + 2]))))) {
            k += 2;
            while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) ++k;
        }
        *end = k;
        *value = std::strtod(s.substr(at, k - at).c_str(), nullptr);
        return true;
    };
    auto hexEnd = [](const std::string& s, size_t at) {   // the end of a 0x literal starting at `at`, or `at` when there is none
        if (at + 1 >= s.size() || s[at] != '0' || s[at + 1] != 'x') return at;
        size_t k = at + 2;
        while (k < s.size() && std::isxdigit(static_cast<unsigned char>(s[k]))) ++k;
        return k;
    };
    while (i < a.size() && j < b.size()) {
        const size_t ha = hexEnd(a, i), hb = hexEnd(b, j);
        if (ha != i || hb != j) {   // a hex literal is text, every digit of it
            if (a.compare(i, ha - i, b, j, hb - j) != 0 || ha == i || hb == j) { if (why) *why = "hex differs near: " + a.substr(i, 40); return false; }
            i = ha; j = hb;
            continue;
        }
        size_t ea = 0, eb = 0;
        double va = 0, vb = 0;
        const bool na = number(a, i, &ea, &va), nb = number(b, j, &eb, &vb);
        if (na != nb) { if (why) *why = "a number against text near: " + a.substr(i, 40); return false; }
        if (na) {
            const double scale = std::max(1.0, std::max(std::fabs(va), std::fabs(vb)));
            if (!(std::fabs(va - vb) <= 1.0e-5 * scale)) { if (why) *why = "numbers differ near: " + a.substr(i, 40); return false; }
            i = ea; j = eb;
            continue;
        }
        if (a[i] != b[j]) { if (why) *why = "text differs near: " + a.substr(i, 40); return false; }
        ++i; ++j;
    }
    if (i != a.size() || j != b.size()) { if (why) *why = "one text is longer"; return false; }
    return true;
}

void testReaderFixture() {
    std::printf("the reader's fixture\n");
    const std::string fixture = slurp("tools/camera_census_fixture.log");
    const std::string built = fixtureLog();
    std::string normalised;
    for (char ch : fixture) if (ch != '\r') normalised += ch;
    check(!fixture.empty(), "tools/camera_census_fixture.log is readable from the repo root");
    std::string why;
    const bool same = sameWithinRounding(normalised, built, &why);
    if (!same) std::printf("  note  %s\n", why.c_str());
    check(same,
          "the reader's fixture file is what the DLL's formatters write for the scripted session, to the last digits of its numbers (python tools\\edvr_log.py --camera-census reads it; regenerate with --print-fixture)");
    check(sameWithinRounding("a 1.0000001 b 0x1F", "a 1.0000002 b 0x1F", nullptr) && !sameWithinRounding("a 1.0001 b", "a 1.0002 b", nullptr) &&
          !sameWithinRounding("a 1 b 0x1F", "a 1 b 0x1E", nullptr) && !sameWithinRounding("a 1 b", "a 1 c", nullptr) && !sameWithinRounding("a 1", "a 1 ", nullptr) &&
          sameWithinRounding("x=-0 y=1e-09", "x=0 y=3e-09", nullptr),
          "the comparison tolerates the seventh digit and a hex literal is text: a digit off at 1e-4, a changed word or hex, a longer line are all different");
    // The golden lines are pinned above; the fixture carries lines of the same classes from the scripted session (the off-thread line is
    // not in it: the fixture is a clean flight, so the reader's verdict on it can PASS, and the reader's own self-test adds that line).
    const char* classes[] = {"vr camera census 5s: frames=", "vr camera census: camera=0x", "vr camera census: changed: camera=0x",
                             "vr camera census: sequence frame=", "vr camera census: call frame=", "vr camera census: eye=",
                             "vr camera census: eye-geometry eye=",
                             "vr world route 5s: key=auto state=owned", "vr world route inject 5s: inj-scene=5400",
                             "vr camera census: episode frame=", "vr camera census: pass-rows ep=", "vr camera census: join ep=", "vr camera census: join-rows ep=",
                             "vr camera census: runs windows=", "vr camera census: detour windows=", "vr camera census: episodes windows=",
                             "vr camera census: join-draws ep=",
                             "vr world route refusal 5s: census=on every=4 treated=450 asked=450 sampled=113 read=112 dropped=0 size=5040x2835 pixels=1600300800 refused=58410978 refused-pct=3.650"};
    unsigned found = 0;
    for (const char* cls : classes) if (built.find(cls) != std::string::npos) ++found;
    check(found == 18, "the fixture holds a line of every class the rig pins: 5 s, camera, changed, sequence, call, eye, eye-geometry, the route's three lines, and the episodes' header, "
                       "pass rows, join draw counts, join, join rows and the window's three companions (episode counters, runs, detour)");
    check(built.find(" kind=5 caller=+0x594FE1 draw=8212 tone=after inj=0 role=- ") != std::string::npos && built.find(" trigger=key-on ") != std::string::npos &&
              built.find(" trigger=gui:0>6 ") != std::string::npos && built.find(" trigger=naming:named>unnamed ") != std::string::npos &&
              built.find(" how=identity ") != std::string::npos && built.find(" how=transpose ") != std::string::npos && built.find(" rows=- why=map") != std::string::npos &&
              built.find(" rows=- why=skip") != std::string::npos && built.find(" rows=- why=no-b1") != std::string::npos && built.find(" rows=read match=-") != std::string::npos &&
              built.find("vr camera census: episodes windows=1 taken=3/10 triggers=5 skipped=2 state=idle") != std::string::npos,
          "the fixture carries each shape the reader's episodes section reads: three triggers, the pass's rows as the axes and as their transpose, a join that matched calls and one that "
          "matched none, a readback that failed, a skipped signature, a draw with no b1, and the counters of the 5 s line");
    check(built.find(" fov=0.8203..0.9831") != std::string::npos && built.find("vr world route refusal 5s: census=on every=4 treated=0 asked=0 sampled=0 read=0 dropped=0 size=0x0 pixels=0 refused=0 refused-pct=0.000") != std::string::npos,
          "the fixture carries the experiment build's tokens: the injector's field-of-view range, and a refusal line for a window in which the census was on and nothing was treated (never the same text as a census that ran)");
    // The stage 2 tokens are in the fixture's census lines, from the formatters: a sampled frame's phase, what the detour decided for a call.
    check(built.find(" phase=0.2520,-0.1260 calls=24 recorded=24 truncated=0") != std::string::npos &&
              built.find(" inj=1 role=scene ") != std::string::npos && built.find(" inj=1 role=fp ") != std::string::npos &&
              built.find(" inj=0 role=aux ") != std::string::npos && built.find(" kind=5 caller=+0x594FE1 draw=8212 tone=after inj=0 role=- ") != std::string::npos &&
              built.find(" stale=0 inj-calls=6750 kinds=1:900,3:7200,5:2700 ") != std::string::npos,
          "the fixture carries the new tokens: a sequence's phase, injected scene and first-person calls, an excluded auxiliary call, kind-5 eye calls with no role, inj-calls");
    std::string mutated = normalised;
    const size_t at = mutated.find("vr camera census 5s:");
    if (at != std::string::npos) mutated[at + 3] = '#';
    check(!sameWithinRounding(mutated, built, nullptr), "control: a fixture file with one character altered is no longer the formatters' output, so the row above can fail");
    std::string nudged = normalised;
    const size_t num = nudged.find("phase=0.2520,-0.1260");
    if (num != std::string::npos) nudged.replace(num, 20, "phase=0.2521,-0.1260");
    check(num != std::string::npos && !sameWithinRounding(nudged, built, nullptr),
          "control: a number moved in its fourth decimal is no longer the formatters' output either");
    std::string toggled = normalised;
    const size_t inj = toggled.find(" inj=1 role=scene ");
    if (inj != std::string::npos) toggled[inj + 5] = '0';
    check(inj != std::string::npos && !sameWithinRounding(toggled, built, nullptr),
          "control: a call line whose inj= was flipped is no longer the formatters' output");
}

int runSelfTest() {
    const std::string injectCpp = slurp("src/d3d11/flat_camera_inject.cpp");
    testKey();
    testLayout(injectCpp);
    testComposeRows();
    testTangentsAndLeak();
    testCameraTable();
    testFrameBuffer();
    testWindow();
    testBudget();
    testOffThread();
    testEpisodeMachine();
    testNamingRuns();
    testObserverCpu();
    testEpisodeFrame();
    testEpisodePrint();
    testEpisodeSession();
    testFormats();
    testSession();
    testPhaseSession();
    testSampling();
    testSourcePins(injectCpp);
    testReaderFixture();
    if (g_failures == 0) {
        std::printf("vr camera census: PASS\n");
        return 0;
    }
    std::printf("vr camera census: %d FAILED check(s)\n", g_failures);
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) return runSelfTest();
    if (argc == 2 && std::strcmp(argv[1], "--print-lines") == 0) {
        for (const Golden& g : goldenLines()) std::printf("%-26s %s\n", g.name, g.text.c_str());
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--print-fixture") == 0) {
        const std::string text = fixtureLog();
        _setmode(_fileno(stdout), _O_BINARY);               // LF only: the fixture file is this, byte for byte
        std::fwrite(text.data(), 1, text.size(), stdout);   // tools\camera_census_fixture.log
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::printf("vr camera census test: dry run (no checks run)\n");
        return 0;
    }
    std::printf("usage: vr_camera_census_test --self-test|--print-lines|--print-fixture|--dry-run\n");
    return 2;
}

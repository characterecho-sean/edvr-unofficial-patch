// F154 checks (native on-foot VR): the pick mesh's lever read from the game, the cursor's live A/B and the signed turn instrument (native_on_foot_f154.h). Header
// only, run by native_frame_test (run154). The lever is exercised as the hook reads it, from a block of memory laid out as the owner, its first entry, the output
// block and the fov chain are in the game; the turn instrument against synthetic runs of a head turning past a HUD frame, a pick point held on the panel (k = 1),
// locked to the camera (k = 0) or turning against it (k = -1, -2) or twice as far (k = 2).
#pragma once

#include "../../src/d3d11/native_on_foot_f154.h"
#include "f117_checks.h"
#include "f120_checks.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace f154test {

namespace y = edvr::nofoot::f154;
namespace w = edvr::nofoot::f120;
namespace z = edvr::nofoot::f117;
namespace t = f120test;

// A block of the game's memory standing at a base address; a read outside it fails (what guardedRead does for an unreadable address).
struct Mem {
    std::vector<uint8_t> bytes;
    uintptr_t base = 0x100000;
    Mem() : bytes(0x8000, 0) {}
    template <class T>
    void put(uintptr_t offset, const T& v) {
        std::memcpy(&bytes[offset], &v, sizeof(T));
    }
    bool read(uintptr_t address, void* out, size_t n) const {
        if (address < base || address + n > base + bytes.size()) return false;
        std::memcpy(out, &bytes[address - base], n);
        return true;
    }
    uintptr_t at(uintptr_t offset) const { return base + offset; }
};
// The layout: owner at +0x000 (service pointer at +8, count +0x1B0, array +0x1B8), the array at +0x400, the first entry S at +0x800 (scale +0xE0, flag +0x1C0), the
// output block at +0xC00 (position +0x40, scale +0x50), the service A at +0x1000 (render state +0x830, camera object +0x838), the camera object Y at +0x2000
// (fov of C1 at Y+0x10+0x70+0x260), the render state W at +0x3000 (W18 pointer +0x18, mode +0x3694) and W18 at +0x7000 (fov +0x17C).
constexpr uintptr_t kOwner = 0x000, kArray = 0x400, kEntry = 0x800, kOut = 0xC00, kService = 0x1000, kCamObj = 0x2000, kRender = 0x3000, kUiObj = 0x7000;
inline void layout(Mem& m, double rho, double scale, uint8_t flag, const float q[3], double fovC1Deg, double fovUiDeg, int32_t mode) {
    m.put<uint64_t>(kOwner + 8, m.at(kService));
    m.put<uint64_t>(kOwner + w::kOffOwnerCount, 1);
    m.put<uint64_t>(kOwner + w::kOffOwnerArray, m.at(kArray));
    m.put<uint64_t>(kArray, m.at(kEntry));
    m.put<float>(kEntry + y::kOffEntryScale, static_cast<float>(scale));
    m.put<uint8_t>(kEntry + w::kOffScreenFlag, flag);
    m.put<float>(kOut + y::kOffOutScale, static_cast<float>(scale * rho));
    for (int k = 0; k < 3; ++k) m.put<float>(kOut + y::kOffOutPos + 4 * k, q[k]);
    m.put<uint64_t>(kService + y::kOffServiceCamera, m.at(kCamObj));
    m.put<uint64_t>(kService + y::kOffServiceRender, m.at(kRender));
    const double d2r = 3.14159265358979323846 / 180.0;
    m.put<float>(kCamObj + y::kCameraObjectToCm + y::kCmToC1 + y::kOffC1Fov, static_cast<float>(fovC1Deg * d2r));
    m.put<uint64_t>(kRender + y::kOffRenderUi, m.at(kUiObj));
    m.put<int32_t>(kRender + y::kOffRenderMode, mode);
    m.put<float>(kUiObj + y::kOffUiFov, static_cast<float>(fovUiDeg * d2r));
}

inline double dist3(const double a[3], const double b[3]) { return f120test::dist3(a, b); }

// Streams of one run: a head turning by deg about y (yaw) or x (pitch) past a fixed HUD frame, the game's point at k times the held turn, F117's at kUv times it and the
// written one at kWritten times it. The mouse is still (the same uv), the eye moves by eyeStepMm along x each frame.
inline y::Sec runFit(double kGame, double kUv, double kWritten, bool yaw, int steps, double stepDeg, const double p0[3], double eyeStepMm = 0.0, bool locked = false) {
    y::TurnTracker tracker;
    y::Sec sec;
    const w::Mat qh = w::mul(t::rotY(15.0), t::rotX(-5.0));
    for (int i = 0; i <= steps; ++i) {
        const double deg = stepDeg * i;
        const w::Mat qe = yaw ? t::rotY(deg) : t::rotX(deg);
        y::TurnInput in;
        in.frame = 100 + static_cast<uint32_t>(i);
        in.key = 0x1000;
        in.u = 0.4;
        in.v = 0.6;
        in.haveEye = in.haveHrc = true;
        t::rows(qe, 1.0, in.E);
        t::rows(qh, 0.052, in.H);
        in.eye[0] = 1.0 + eyeStepMm * 0.001 * i;
        in.eye[1] = 1.6;
        in.eye[2] = -2.0;
        const double ks[y::kStreams] = {kGame, kUv, kWritten};
        for (int s = 0; s < y::kStreams; ++s) {
            // the held point is p0 turned by -deg (M = R R0t = rotation by -deg for this head); a stream with k turns k times as far
            const w::Mat m = locked ? w::identity() : (yaw ? t::rotY(-ks[s] * deg) : t::rotX(-ks[s] * deg));
            w::apply(m, p0, in.p[s]);
        }
        tracker.place(in, sec);
    }
    return sec;
}

template <class Check>
inline void run154(Check&& check) {
    const double kPi = t::kPi;
    const double p0[3] = {-0.02, 0.05, 0.65};
    const double ahead[3] = {0.0, 0.0, 0.65};
    {   // the setting: the five words, anything else is shipped and unknown
        y::Setting s = y::Setting::kGame;
        const char* words[] = {"shipped", "uv", "game", "drawn", "mirrored"};
        bool ok = true;
        for (int i = 0; i < 5; ++i) {
            y::Setting got = y::Setting::kShipped;
            ok = ok && y::parseSetting(words[i], &got) && static_cast<int>(got) == i && std::strcmp(y::settingName(got), words[i]) == 0;
        }
        const bool u1 = !y::parseSetting("", &s) && s == y::Setting::kShipped;
        s = y::Setting::kDrawn;
        const bool u2 = !y::parseSetting("panel", &s) && s == y::Setting::kShipped;
        s = y::Setting::kDrawn;
        const bool u3 = !y::parseSetting("Drawn", &s) && s == y::Setting::kShipped;
        s = y::Setting::kDrawn;
        const bool u4 = !y::parseSetting(nullptr, &s) && s == y::Setting::kShipped;
        check(ok && u1 && u2 && u3 && u4, "F154: the setting is shipped, uv, game, drawn or mirrored (exact words); an empty, other, wrong-case or missing value is shipped and unknown");
        check(!y::needsLever(y::Setting::kShipped) && !y::needsLever(y::Setting::kUv) && !y::needsLever(y::Setting::kGame) && y::needsLever(y::Setting::kDrawn) &&
                  y::needsLever(y::Setting::kMirrored) && y::appliesF117(y::Setting::kShipped) && y::appliesF117(y::Setting::kUv) && !y::appliesF117(y::Setting::kGame) &&
                  !y::appliesF117(y::Setting::kDrawn) && !y::appliesF117(y::Setting::kMirrored) && y::appliesF120(y::Setting::kShipped) &&
                  !y::appliesF120(y::Setting::kUv) && !y::appliesF120(y::Setting::kGame) && !y::appliesF120(y::Setting::kDrawn) && !y::appliesF120(y::Setting::kMirrored),
              "F154: only drawn and mirrored need the lever; F117 acts under shipped and uv, F120 under shipped alone");
    }
    {   // the build's addresses
        check(y::kOwnerRva == 0x47299F0 && y::kOwnerRva == w::kOwnerRva && y::kOwnerSlotRva == 0x5607E68 && y::kOwnerStolen == 7 && y::kOffEntryScale == 0xE0 &&
                  y::kOffOutPos == 0x40 && y::kOffOutScale == 0x50 && y::kOffOwnerService == 0x8 && y::kOffServiceRender == 0x830 && y::kOffServiceCamera == 0x838 &&
                  y::kCameraObjectToCm == 0x10 && y::kCmToC1 == 0x70 && y::kOffC1Fov == 0x260 && y::kOffRenderUi == 0x18 && y::kOffUiFov == 0x17C &&
                  y::kOffRenderMode == 0x3694 && w::kOwnerPrologue[0] == 0x4C && w::kOwnerPrologue[1] == 0x8B && w::kOwnerPrologue[2] == 0xDC && w::kOwnerPrologue[3] == 0x49 &&
                  w::kOwnerPrologue[4] == 0x89 && w::kOwnerPrologue[5] == 0x5B && w::kOwnerPrologue[6] == 0x18,
              "F154: the hook is at exe+0x47299F0 behind the owner vtable's slot at exe+0x5607E68; the first seven bytes it moves are mov r11,rsp (4C 8B DC) and mov [r11+0x18],rbx "
              "(49 89 5B 18); the lever and cross-check offsets are the RE's");
    }
    {   // the lever undo inverts the forward lever, and mirrored gives |rho| times the drawn point about (0, 0, q.z)
        const double rhos[] = {-2.0, -1.87, 2.0, 1.0};
        const double pts[][3] = {{-0.02, 0.05, 0.65}, {0.4, -0.3, 0.7}, {0.0, 0.0, 0.5}, {-0.6, 0.35, 0.9}};
        double worstUndo = 0.0, worstMirror = 0.0, worstId = 0.0;
        bool refusedNone = true;
        for (double rho : rhos)
            for (double qz : {0.7, 0.3}) {
                y::Lever lever;
                lever.valid = true;
                lever.rho = rho;
                lever.q[2] = qz;
                for (const auto& p : pts) {
                    double P[3], back[3], mir[3];
                    y::applyLever(rho, qz, p, P);
                    refusedNone = refusedNone && y::undoLever(&lever, P, back) == y::Refusal::kNone && y::mirrorLever(&lever, P, mir) == y::Refusal::kNone;
                    worstUndo = std::fmax(worstUndo, dist3(back, p));
                    const double a = std::fabs(rho);
                    const double want[3] = {a * p[0], a * p[1], qz + a * (p[2] - qz)};
                    worstMirror = std::fmax(worstMirror, dist3(mir, want));
                    if (rho == 1.0) worstId = std::fmax(worstId, std::fmax(dist3(P, p), std::fmax(dist3(back, p), dist3(mir, p))));
                }
            }
        check(refusedNone && worstUndo < 1e-9 && worstMirror < 1e-9 && worstId < 1e-12,
              "F154: the lever undo inverts the forward lever for rho = -2, -1.87, +2 and 1 (within 1e-9); mirrored gives |rho| p0 about (0, 0, q.z) for each of them; rho = 1 is the identity");
        // the forward lever's shape, and why it reaches twice as far as the head's turn: a point 0.74 m ahead turned by 10 deg about the camera moves 2 x as far on the pick mesh
        double P[3];
        y::applyLever(-2.0, 0.7, ahead, P);
        check(std::fabs(P[0]) < 1e-12 && std::fabs(P[1]) < 1e-12 && std::fabs(P[2] - (0.7 - 2.0 * (0.65 - 0.7))) < 1e-12,
              "F154: the forward lever leaves the axis point on the axis and magnifies z about q.z (rho = -2 turns 0.65 m with q.z = 0.7 into 0.8 m)");
    }
    {   // the refusals: no lever, a tiny or non-finite rho, a non-finite point
        y::Lever good;
        good.valid = true;
        good.rho = -1.87;
        good.q[2] = 0.7;
        y::Lever tiny = good, nan = good, inf = good, nanq = good, invalid = good;
        tiny.rho = 0.04;
        nan.rho = std::numeric_limits<double>::quiet_NaN();
        inf.rho = std::numeric_limits<double>::infinity();
        nanq.q[2] = std::numeric_limits<double>::quiet_NaN();
        invalid.valid = false;
        const double P[3] = {0.1, 0.2, 0.8}, bad[3] = {0.1, std::numeric_limits<double>::quiet_NaN(), 0.8};
        double out[3];
        const y::Lever edge = [&] {
            y::Lever l = good;
            l.rho = -0.05;
            return l;
        }();
        check(y::undoLever(nullptr, P, out) == y::Refusal::kNoLever && y::undoLever(&invalid, P, out) == y::Refusal::kNoLever &&
                  y::mirrorLever(nullptr, P, out) == y::Refusal::kNoLever && y::undoLever(&tiny, P, out) == y::Refusal::kTinyRho &&
                  y::mirrorLever(&tiny, P, out) == y::Refusal::kTinyRho && y::undoLever(&nan, P, out) == y::Refusal::kNonFinite &&
                  y::mirrorLever(&inf, P, out) == y::Refusal::kNonFinite && y::undoLever(&nanq, P, out) == y::Refusal::kNonFinite &&
                  y::undoLever(&good, bad, out) == y::Refusal::kNonFinite && y::mirrorLever(&good, bad, out) == y::Refusal::kNonFinite &&
                  y::undoLever(&edge, P, out) == y::Refusal::kNone && y::mirrorLever(&edge, P, out) == y::Refusal::kNone,
              "F154: drawn and mirrored refuse with no lever (none, or not valid), a rho under 0.05 in size, a rho or q.z that is not a number and a point that is not "
              "a number; a rho of exactly -0.05 is taken");
    }
    {   // each setting selects its point, and only drawn and mirrored need the lever
        const std::vector<z::Tri> turned = f117test::mesh(true), upright = f117test::mesh(false);
        float s[3];
        f117test::spriteAt(turned, 0.7, 0.2, s);
        const z::Decision d = z::decide(turned.data(), turned.size(), 0.7, 0.2, s, z::Mode::kPanel);
        const double game[3] = {s[0], s[1], s[2]};
        const w::Mat qe0 = w::identity(), qh0 = w::mul(t::rotY(15.0), t::rotX(-5.0));
        w::Tracker tr;
        tr.place(t::mk(100, qe0, qh0, d.target));
        const w::Out f120 = tr.place(t::mk(101, w::mul(t::rotY(20.0), qe0), qh0, d.target));
        double want120[3];
        const bool has120 = w::finalTarget(true, d.target, f120, want120);
        y::Lever lever;
        lever.valid = true;
        lever.rho = -1.87;
        lever.q[2] = 0.7;
        double wantDrawn[3], wantMirror[3];
        y::undoLever(&lever, game, wantDrawn);
        y::mirrorLever(&lever, game, wantMirror);
        const bool moved = d.why == z::Why::kMoved;
        const y::Pick shipped = y::choose(y::Setting::kShipped, true, moved, d.target, f120, game, nullptr);
        const y::Pick uv = y::choose(y::Setting::kUv, true, moved, d.target, f120, game, nullptr);
        const y::Pick gamePick = y::choose(y::Setting::kGame, true, moved, d.target, f120, game, &lever);
        const y::Pick drawn = y::choose(y::Setting::kDrawn, true, moved, d.target, f120, game, &lever);
        const y::Pick mirrored = y::choose(y::Setting::kMirrored, true, moved, d.target, f120, game, &lever);
        check(moved && has120 && shipped.write && dist3(shipped.target, want120) < 1e-12 && shipped.applies && uv.write && dist3(uv.target, d.target) < 1e-12 &&
                  dist3(uv.target, want120) > 0.01 && !gamePick.write && drawn.write && dist3(drawn.target, wantDrawn) < 1e-12 && dist3(drawn.target, game) > 0.01 &&
                  mirrored.write && dist3(mirrored.target, wantMirror) < 1e-12 && dist3(mirrored.target, drawn.target) > 0.01 && drawn.refused == y::Refusal::kNone,
              "F154: shipped writes F120's point (F117's, then the head's turn), uv writes F117's alone, game writes nothing, drawn writes the game's sprite with the lever undone and "
              "mirrored writes it taken as |rho|");
        // an upright panel (F117 leaves it) writes nothing under uv; drawn and mirrored on a panel that is not screen-space do not apply
        float su[3];
        f117test::spriteAt(upright, 0.7, 0.2, su);
        const z::Decision du = z::decide(upright.data(), upright.size(), 0.7, 0.2, su, z::Mode::kPanel);
        const double gameU[3] = {su[0], su[1], su[2]};
        const y::Pick uvUp = y::choose(y::Setting::kUv, false, du.why == z::Why::kMoved, du.target, w::Out{}, gameU, nullptr);
        const y::Pick drawnNot = y::choose(y::Setting::kDrawn, false, false, du.target, w::Out{}, gameU, &lever);
        const y::Pick mirrorNot = y::choose(y::Setting::kMirrored, false, false, du.target, w::Out{}, gameU, &lever);
        check(du.why == z::Why::kUpright && !uvUp.write && !drawnNot.applies && !drawnNot.write && drawnNot.refused == y::Refusal::kNone && !mirrorNot.applies &&
                  !mirrorNot.write,
              "F154: an upright pick mesh is left alone under uv; drawn and mirrored do not apply to a panel that is not screen-space (no write, no refusal)");
        // drawn and mirrored without a lever, with a tiny one: the game's place stands, the refusal is named
        const y::Pick noLeverDrawn = y::choose(y::Setting::kDrawn, true, moved, d.target, f120, game, nullptr);
        const y::Pick noLeverMirror = y::choose(y::Setting::kMirrored, true, moved, d.target, f120, game, nullptr);
        y::Lever tiny = lever;
        tiny.rho = 0.01;
        const y::Pick tinyDrawn = y::choose(y::Setting::kDrawn, true, moved, d.target, f120, game, &tiny);
        y::Lever nan = lever;
        nan.rho = std::numeric_limits<double>::quiet_NaN();
        const y::Pick nanMirror = y::choose(y::Setting::kMirrored, true, moved, d.target, f120, game, &nan);
        check(!noLeverDrawn.write && noLeverDrawn.refused == y::Refusal::kNoLever && noLeverDrawn.applies && !noLeverMirror.write &&
                  noLeverMirror.refused == y::Refusal::kNoLever && !tinyDrawn.write && tinyDrawn.refused == y::Refusal::kTinyRho && !nanMirror.write &&
                  nanMirror.refused == y::Refusal::kNonFinite,
              "F154: drawn and mirrored on a screen-space panel with no fresh lever, a rho under 0.05 or a rho that is not a number write nothing and name the refusal (the game's place stands)");
        // the counts a placement makes
        y::Sec sec;
        sec.addPlacement(true, drawn, true);
        sec.addPlacement(true, noLeverDrawn, true);
        sec.addPlacement(true, tinyDrawn, true);
        sec.addPlacement(true, nanMirror, true);
        sec.addPlacement(false, drawnNot, true);
        sec.addPlacement(true, drawn, false);
        check(sec.placements == 6 && sec.screen == 5 && sec.written == 1 && sec.writeFailed == 1 && sec.refusedLever[0] == 1 && sec.refusedLever[1] == 1 &&
                  sec.refusedLever[2] == 1,
              "F154: a placement counts a screen-space panel, a write, a failed write and its refusal by cause (no lever, tiny rho, not finite) apart");
    }
    {   // the lever table: this frame and the one before, per owner, the oldest dropped; the cross-check once a frame
        y::LeverTable table;
        y::Lever a;
        a.valid = true;
        a.owner = 0xA000;
        a.frame = 500;
        a.rho = -1.9;
        y::Lever b = a;
        b.owner = 0xB000;
        b.rho = 1.5;
        table.put(a);
        table.put(b);
        y::Lever got;
        const bool same = table.fresh(0xA000, 500, &got) && got.rho == -1.9, before = table.fresh(0xA000, 501, &got) && got.rho == -1.9, stale = table.fresh(0xA000, 502, &got),
                   other = table.fresh(0xB000, 501, &got) && got.rho == 1.5, unknown = table.fresh(0xC000, 500, &got), backwards = table.fresh(0xA000, 499, &got);
        y::Lever a2 = a;
        a2.frame = 510;
        a2.rho = -2.1;
        table.put(a2);
        const bool replaced = table.fresh(0xA000, 511, &got) && got.rho == -2.1 && !table.fresh(0xA000, 500, &got) && table.fresh(0xB000, 501, &got);
        y::Lever c = a, d = a, e = a;
        c.owner = 0xC000;
        c.frame = 520;
        d.owner = 0xD000;
        d.frame = 521;
        e.owner = 0xE000;
        e.frame = 522;
        table.put(c);
        table.put(d);
        table.put(e);   // five owners, four slots: the oldest (b, frame 500) goes
        const bool evicted = !table.fresh(0xB000, 501, &got) && table.fresh(0xE000, 522, &got) && table.fresh(0xC000, 521, &got) && table.fresh(0xA000, 511, &got);
        const bool crossFirst = table.crossDue(600);
        table.crossTaken(600);
        const bool crossOnce = !table.crossDue(600) && table.crossDue(601);
        table.clear();
        check(same && before && !stale && other && !unknown && !backwards && replaced && evicted && crossFirst && crossOnce && !table.fresh(0xE000, 522, &got),
              "F154: a lever is used by its owner's placement in the frame it was read and the next, not two frames on, not by another owner, not when the frame counter went "
              "backwards; a newer reading replaces it; the oldest of five owners is dropped; the cross-check is read once a frame");
    }
    {   // the hook's reading of the owner, its first entry and the output block
        const float q[3] = {0.01f, -0.02f, 0.74f};
        Mem m;
        layout(m, -1.87, 1.0, 1, q, 93.6, 56.25, 4);
        auto rd = [&](uintptr_t address, void* out, size_t n) { return m.read(address, out, n); };
        y::Lever lever;
        const y::ReadResult ok = y::readLever(m.at(kOwner), m.at(kOut), rd, &lever);
        check(ok == y::ReadResult::kLever && lever.valid && lever.owner == m.at(kOwner) && std::fabs(lever.rho + 1.87) < 1e-6 && std::fabs(lever.scale - 1.0) < 1e-12 &&
                  std::fabs(lever.q[0] - 0.01) < 1e-7 && std::fabs(lever.q[1] + 0.02) < 1e-7 && std::fabs(lever.q[2] - 0.74) < 1e-6,
              "F154: the hook reads rho = out scale / the entry's scale with its sign (-1.87), the owner, and q' (the output block's position) from the game's layout");
        Mem m2;
        layout(m2, 2.0, 0.5, 1, q, 93.6, 56.25, 4);
        auto rd2 = [&](uintptr_t address, void* out, size_t n) { return m2.read(address, out, n); };
        y::Lever l2;
        const y::ReadResult ok2 = y::readLever(m2.at(kOwner), m2.at(kOut), rd2, &l2);
        check(ok2 == y::ReadResult::kLever && std::fabs(l2.rho - 2.0) < 1e-6 && std::fabs(l2.scale - 0.5) < 1e-12,
              "F154: rho is the output's scale over the entry's, not the output's alone (an entry of scale 0.5 and an output of 1.0 give +2)");
        // the refusals of the read, each its own result
        Mem notScreen;
        layout(notScreen, -1.87, 1.0, 0, q, 93.6, 56.25, 4);
        Mem empty;
        layout(empty, -1.87, 1.0, 1, q, 93.6, 56.25, 4);
        empty.put<uint64_t>(kOwner + w::kOffOwnerCount, 0);
        Mem zero;
        layout(zero, -1.87, 0.0, 1, q, 93.6, 56.25, 4);
        zero.put<float>(kOut + y::kOffOutScale, 1.0f);
        const float nanq[3] = {0.0f, std::numeric_limits<float>::quiet_NaN(), 0.7f};
        Mem badq;
        layout(badq, -1.87, 1.0, 1, nanq, 93.6, 56.25, 4);
        Mem noArray;
        layout(noArray, -1.87, 1.0, 1, q, 93.6, 56.25, 4);
        noArray.put<uint64_t>(kOwner + w::kOffOwnerArray, 0);
        Mem wild;
        layout(wild, -1.87, 1.0, 1, q, 93.6, 56.25, 4);
        wild.put<uint64_t>(kArray, 0xDEAD0000ull);
        y::Lever junk;
        auto run = [&](const Mem& mem, uintptr_t out) {
            auto r = [&](uintptr_t address, void* o, size_t n) { return mem.read(address, o, n); };
            return y::readLever(mem.at(kOwner), out, r, &junk);
        };
        check(run(notScreen, notScreen.at(kOut)) == y::ReadResult::kNotScreen && run(empty, empty.at(kOut)) == y::ReadResult::kEmpty &&
                  run(zero, zero.at(kOut)) == y::ReadResult::kBadScale && run(badq, badq.at(kOut)) == y::ReadResult::kBadScale &&
                  run(noArray, noArray.at(kOut)) == y::ReadResult::kUnreadable && run(wild, wild.at(kOut)) == y::ReadResult::kUnreadable &&
                  run(m, 0x10) == y::ReadResult::kUnreadable && !junk.valid,
              "F154: the read names each case apart: the flag clear is not screen-space, a count of 0 is empty, an entry scale of 0 or a q' that is not a number is a bad scale, a "
              "null array, a wild pointer and an output block outside memory are unreadable; none fills the lever");
        // the cross-check
        const y::Cross c = y::readCross(m.at(kOwner), rd);
        check(c.haveC1 && c.haveUi && c.haveMode && c.mode == 4 && std::fabs(c.fovC1 * 180.0 / kPi - 93.6) < 1e-3 && std::fabs(c.fovUi * 180.0 / kPi - 56.25) < 1e-3 &&
                  std::fabs(c.rhoFov() - std::tan(0.5 * 93.6 * kPi / 180.0) / std::tan(0.5 * 56.25 * kPi / 180.0)) < 1e-4 && c.rhoFov() > 1.99 && c.rhoFov() < 2.0,
              "F154: the cross-check reads the camera's fov (93.6 deg), the UI's (56.25 deg) and the render mode (4) along the game's chain and gives rho_fov = tan(0.5 fov_C1) / "
              "tan(0.5 fov_UI) = 1.992");
        Mem noCam;
        layout(noCam, -1.87, 1.0, 1, q, 93.6, 56.25, 4);
        noCam.put<uint64_t>(kService + y::kOffServiceCamera, 0);
        Mem noService;
        layout(noService, -1.87, 1.0, 1, q, 93.6, 56.25, 4);
        noService.put<uint64_t>(kOwner + y::kOffOwnerService, 0xBAD0000ull);
        auto rc = [&](uintptr_t address, void* o, size_t n) { return noCam.read(address, o, n); };
        auto rs = [&](uintptr_t address, void* o, size_t n) { return noService.read(address, o, n); };
        const y::Cross cc = y::readCross(noCam.at(kOwner), rc), cs = y::readCross(noService.at(kOwner), rs);
        check(!cc.haveC1 && cc.haveUi && cc.haveMode && std::isnan(cc.rhoFov()) && !cs.haveC1 && !cs.haveUi && !cs.haveMode && std::isnan(cs.rhoFov()),
              "F154: a broken chain reports what it reached (no camera: the UI's fov and the mode still read, rho_fov not a number) and an unreadable service reads nothing");
    }
    {   // the signed turn instrument recovers k on yaw and on pitch
        const double ks[] = {0.0, 1.0, -1.0, -2.0, 2.0};
        double worstYaw = 0.0, worstPitch = 0.0;
        bool validYaw = true, validPitch = true, otherInvalid = true;
        for (double k : ks) {
            const y::Sec yawRun = runFit(k, k, k, true, 12, 0.5, p0), pitchRun = runFit(k, k, k, false, 12, 0.5, p0);
            for (int s = 0; s < y::kStreams; ++s) {
                validYaw = validYaw && yawRun.fit[s].valid(1);
                validPitch = validPitch && pitchRun.fit[s].valid(0);
                worstYaw = std::fmax(worstYaw, std::fabs(yawRun.fit[s].k(1) - k));
                worstPitch = std::fmax(worstPitch, std::fabs(pitchRun.fit[s].k(0) - k));
            }
        }
        check(validYaw && validPitch && worstYaw < 0.05 && worstPitch < 0.05,
              "F154: the signed turn fit recovers k = 0, 1, -1, -2 and +2 on yaw and on pitch within 0.05 on a 6 deg turn with the mouse still, for all three streams");
        // the turn about the other axis is not read: a yaw-only run (p straight ahead) has no pitch to fit
        const y::Sec yawOnly = runFit(1.0, 1.0, 1.0, true, 12, 0.5, ahead), pitchOnly = runFit(1.0, 1.0, 1.0, false, 12, 0.5, ahead);
        otherInvalid = yawOnly.fit[y::kGame].valid(1) && !yawOnly.fit[y::kGame].valid(0) && pitchOnly.fit[y::kGame].valid(0) && !pitchOnly.fit[y::kGame].valid(1);
        check(otherInvalid, "F154: a yaw-only turn gives a yaw k and no pitch k, and the reverse (an axis with under about a degree of model turn reads n/a)");
        // a large turn is exact for a point straight ahead: 20 deg of head, the point turning against it twice as far
        const y::Sec big = runFit(-2.0, 1.0, 0.0, true, 10, 2.0, ahead);
        check(std::fabs(big.fit[y::kGame].k(1) + 2.0) < 1e-6 && std::fabs(big.fit[y::kUv].k(1) - 1.0) < 1e-6 && std::fabs(big.fit[y::kWritten].k(1)) < 1e-6 &&
                  std::fabs(big.turnMaxDeg - 20.0) < 0.01,
              "F154: the three streams are fitted apart (k = -2, 1 and 0 here) and exact for a point ahead through a 20 deg head turn; the run's largest turn is 20 deg");
    }
    {   // judged only past 2 deg, the eye's move, the pure translation
        const y::Sec shortTurn = runFit(1.0, 1.0, 1.0, true, 3, 0.51, p0);   // 1.53 deg
        const y::Sec edge = runFit(1.0, 1.0, 1.0, true, 4, 0.51, p0);    // 2.04 deg: the last placement counts
        check(shortTurn.judged == 0 && shortTurn.judgedRuns == 0 && !shortTurn.judgedSecond() && !shortTurn.fit[y::kGame].valid(1) && edge.judged == 1 && edge.judgedRuns == 1 &&
                  edge.runs == 1 && edge.judgedSecond(),
              "F154: a placement counts toward k once the head has turned 2 deg against the HUD frame since its run began (1.53 deg none, 2.04 deg one placement in one judged run)");
        // a pure eye translation: nothing turns, so nothing is judged and k has no value; the move is reported in the eye's axes
        const y::Sec shift = runFit(1.0, 1.0, 1.0, true, 10, 0.0, p0, 4.0);
        check(shift.judged == 0 && !shift.fit[y::kGame].valid(0) && !shift.fit[y::kGame].valid(1) && std::fabs(shift.eyeMoveMaxMm - 40.0) < 1e-6 && shift.turnMaxDeg < 1e-9,
              "F154: a pure eye translation judges nothing (the head did not turn) and reports the eye's largest move, 40 mm after ten 4 mm steps");
        // a turn with the eye moving and the pick locked to the camera: k is 0 and the move is reported beside it
        const y::Sec locked = runFit(0.0, 0.0, 0.0, true, 12, 0.5, p0, 0.5, true);
        check(locked.judged > 0 && std::fabs(locked.fit[y::kGame].k(1)) < 0.05 && std::fabs(locked.eyeMoveMaxMm - 6.0) < 1e-6,
              "F154: a head turn with the eye moving 0.5 mm a frame and the points locked to the camera gives k near 0 and reports the 6 mm the eye moved");
    }
    {   // runs: a moving mouse starts a new run each placement; a gap of more than 3 frames starts one; the frame counter going back starts one
        y::TurnTracker tr;
        y::Sec sec;
        const w::Mat qh = w::mul(t::rotY(15.0), t::rotX(-5.0));
        auto feed = [&](uint32_t frame, double deg, double u, uintptr_t key = 0x1000) {
            y::TurnInput in;
            in.frame = frame;
            in.key = key;
            in.u = u;
            in.v = 0.6;
            in.haveEye = in.haveHrc = true;
            t::rows(t::rotY(deg), 1.0, in.E);
            t::rows(qh, 0.052, in.H);
            for (int s = 0; s < y::kStreams; ++s) w::apply(t::rotY(-deg), p0, in.p[s]);
            tr.place(in, sec);
        };
        for (int i = 0; i < 8; ++i) feed(100 + static_cast<uint32_t>(i), 3.0 * i, 0.4 + 0.01 * i);   // the mouse moving
        const uint32_t movingRuns = sec.runs, movingJudged = sec.judged;
        feed(200, 0.0, 0.4);
        feed(203, 4.0, 0.4);   // 3 frames on: the same run, 4 deg judged
        feed(207, 8.0, 0.4);   // 4 frames on: a new run
        feed(208, 9.0, 0.4);   // 1 deg since it began: not judged
        feed(300, 0.0, 0.4, 0x2000);
        feed(301, 6.0, 0.4, 0x2000);   // another panel: its own run, judged
        feed(50, 0.0, 0.4);            // the frame counter went back: a new run on the first panel
        check(movingRuns == 8 && movingJudged == 0 && sec.runs == 8 + 4 && sec.judged == 2 && sec.judgedRuns == 2,
              "F154: a mouse that moves starts a run at every placement and judges none; the same uv within 3 frames goes on, a gap of 4 frames, another panel and the frame "
              "counter going back each start a run");
    }
    {   // every refusal of the turn instrument is its own count and none feeds a fit
        y::TurnTracker tr;
        y::Sec sec;
        y::TurnInput base;
        base.frame = 100;
        base.key = 0x1000;
        base.haveEye = base.haveHrc = true;
        t::rows(w::identity(), 1.0, base.E);
        t::rows(w::identity(), 0.052, base.H);
        for (int s = 0; s < y::kStreams; ++s)
            for (int k = 0; k < 3; ++k) base.p[s][k] = p0[k];
        y::TurnInput noEye = base, noHrc = base, zeroRow = base, nanPoint = base, lean = base;
        noEye.haveEye = false;
        noHrc.haveHrc = false;
        for (int k = 0; k < 3; ++k) zeroRow.H[3 + k] = 0.0;
        nanPoint.p[y::kUv][1] = std::numeric_limits<double>::quiet_NaN();
        lean.H[3] = 0.3 * 0.052;
        const y::TurnWhy a = tr.place(noEye, sec), b = tr.place(noHrc, sec), c = tr.place(zeroRow, sec), d = tr.place(nanPoint, sec), e = tr.place(lean, sec),
                         f = tr.place(base, sec);
        check(a == y::TurnWhy::kNoEye && b == y::TurnWhy::kNoHrc && c == y::TurnWhy::kDegenerate && d == y::TurnWhy::kDegenerate && e == y::TurnWhy::kDegenerate &&
                  f == y::TurnWhy::kFed && sec.refusedTurn[0] == 1 && sec.refusedTurn[1] == 1 && sec.refusedTurn[2] == 3 && sec.runs == 1 && sec.judged == 0,
              "F154: no eye view, no HRC matrix and a degenerate input (a zero row, a leaning row, a point that is not a number) are each counted apart and start no run");
    }
    {   // the shortest-arc rotation vector
        double r[3];
        const double a[3] = {0.0, 0.0, 1.0}, b[3] = {std::sin(30.0 * kPi / 180.0), 0.0, std::cos(30.0 * kPi / 180.0)}, c[3] = {0.0, 0.0, 2.0}, o[3] = {0.0, 0.0, -1.0}, z0[3] = {};
        const bool ok = y::rotVec(a, b, r) && std::fabs(r[1] - 30.0 * kPi / 180.0) < 1e-12 && std::fabs(r[0]) < 1e-12 && std::fabs(r[2]) < 1e-12;
        const bool same = y::rotVec(a, c, r) && r[0] == 0.0 && r[1] == 0.0 && r[2] == 0.0;
        double r2[3];
        check(ok && same && !y::rotVec(a, o, r2) && !y::rotVec(z0, a, r2),
              "F154: the rotation vector takes the direction a to b about y by 30 deg (ahead to the right), is zero for the same direction at another length and refuses opposite and zero vectors");
    }
    {   // the seconds add: counts add, the extremes are kept, the fit sums add
        y::Sec a, b;
        a.placements = 3;
        a.written = 1;
        a.refusedLever[0] = 2;
        a.turnMaxDeg = 4.0;
        a.eyeMoveMaxMm = 1.0;
        a.addCall(y::ReadResult::kLever);
        a.addLever(-1.9, 0.7);
        b.placements = 5;
        b.refusedTurn[2] = 1;
        b.turnMaxDeg = 9.0;
        b.eyeMoveMaxMm = 0.5;
        b.addCall(y::ReadResult::kNotScreen);
        b.addCall(y::ReadResult::kUnreadable);
        b.addLever(-1.8, 0.8);
        const double aa[3] = {0.1, 0.2, 0.0}, bb[3] = {0.2, 0.2, 0.0};
        a.fit[y::kGame].add(aa, bb);
        b.fit[y::kGame].add(aa, bb);
        b.haveCross = true;
        b.cross.haveC1 = true;
        b.cross.fovC1 = 1.6;
        a.merge(b);
        check(a.placements == 8 && a.written == 1 && a.refusedLever[0] == 2 && a.refusedTurn[2] == 1 && a.turnMaxDeg == 9.0 && a.eyeMoveMaxMm == 1.0 && a.calls == 3 &&
                  a.leverReads == 2 && a.notScreen == 1 && a.unread == 1 && std::fabs(a.rhoMin + 1.9) < 1e-12 && std::fabs(a.rhoMax + 1.8) < 1e-12 &&
                  std::fabs(a.rhoSum + 3.7) < 1e-12 && a.haveCross && a.cross.haveC1 && std::fabs(a.fit[y::kGame].sab[1] - 0.08) < 1e-12 &&
                  std::fabs(a.fit[y::kGame].saa[1] - 0.08) < 1e-12,
              "F154: merged seconds add their counts, keep the largest turn and eye move and the extremes of rho, take the last cross-check and add the fit's sums");
    }
    {   // the lines
        char line[2000];
        const y::Sec empty;
        y::formatTurn(line, sizeof(line), "1 s", "shipped", empty);
        const std::string none = line;
        y::Sec s = runFit(-2.0, 1.0, 0.5, true, 12, 0.5, p0);
        s.placements = 120;
        s.screen = 118;
        s.written = 90;
        s.refusedLever[0] = 7;
        s.addCall(y::ReadResult::kLever);
        s.addLever(-1.87, 0.74);
        s.addLever(-1.86, 0.75);
        s.haveCross = true;
        s.cross.haveC1 = s.cross.haveUi = s.cross.haveMode = true;
        s.cross.fovC1 = 93.6 * kPi / 180.0;
        s.cross.fovUi = 56.25 * kPi / 180.0;
        s.cross.mode = 4;
        const int n = y::formatTurn(line, sizeof(line), "so far, 7 s with a judged run", "drawn", s);
        const std::string full = line;
        check(none.rfind("panel cursor turn (F154) 1 s: setting shipped; placements 0: screen-space 0, written 0, refused: no lever 0, tiny rho 0, not finite 0, write failed 0, "
                         "no eye view 0, no HRC matrix 0, degenerate row 0; judged 0 placements in 0 of 0 runs",
                         0) == 0 &&
                  none.find("k yaw: game n/a uv n/a written n/a; k pitch: game n/a uv n/a written n/a") != std::string::npos &&
                  none.find("rho mean n/a min n/a max n/a from 0 owner calls") != std::string::npos && none.find("rho_fov n/a fov_C1 n/a deg fov_UI n/a deg mode n/a; q.z n/a m") != std::string::npos &&
                  full.rfind("panel cursor turn (F154) so far, 7 s with a judged run: setting drawn; placements 120: screen-space 118, written 90, refused: no lever 7, ", 0) == 0 &&
                  full.find("k yaw: game -2.00 uv +1.00 written +0.50;") != std::string::npos && full.find("rho mean -1.865 min -1.870 max -1.860 from 1 owner calls (screen-space 2, not 0, unread 0)") != std::string::npos &&
                  full.find("rho_fov 1.992 fov_C1 93.60 deg fov_UI 56.25 deg mode 4; q.z +0.7450 m") != std::string::npos && n > 0 && n < 1000 && none.size() < 1000,
              "F154: the turn line carries the window, the setting, the placements and refusals by cause, the judged placements and runs, the largest turn and eye move, k for yaw "
              "and pitch per stream (n/a without a turn about the axis), rho mean/min/max, the cross-check and q.z, and stays well inside the log's 1,200 characters");
        y::formatWrites(line, sizeof(line), 10, "mirrored", s);
        const std::string writes = line;
        check(writes.rfind("panel cursor writes (F154) 10 s: setting mirrored; placements 120: screen-space 118, written 90, refused: no lever 7, tiny rho 0, not finite 0, write failed 0; "
                           "owner calls 1 (screen-space 2, not 0, unread 0); rho mean -1.865 min -1.870 max -1.860, rho_fov 1.992 fov_C1 93.60 deg fov_UI 56.25 deg mode 4, "
                           "q.z +0.7450 m",
                           0) == 0 && writes.size() < 1000,
              "F154: the writes line carries the window's seconds, the setting, the placements, writes, refusals by cause, the owner calls and the lever they read "
              "(rho, the cross-check, q.z), so rho is on record without a judged second");
        y::FrameRec r;
        r.ledger = 1234;
        r.render = 5678;
        r.panel = 0x12345678ABCDull;
        r.setting = static_cast<uint8_t>(y::Setting::kDrawn);
        r.u = 0.25;
        r.v = 0.75;
        r.p[y::kGame][0] = 0.1;
        r.wrote = true;
        r.haveLever = true;
        r.rho = -1.87;
        r.q[2] = 0.74;
        r.haveEye = r.haveHrc = true;
        r.viewId = 2;
        for (int i = 0; i < 9; ++i) r.E[i] = r.H[i] = (i % 4 == 0) ? 1.0 : 0.0;
        y::formatFrame(line, sizeof(line), r);
        const std::string fr = line;
        y::FrameRec bare;
        y::formatFrame(line, sizeof(line), bare);
        const std::string frNone = line;
        check(fr.rfind("panel cursor frame (F154): ledger frame 1234 render frame 5678 panel 5678ABCD setting drawn uv 0.25000 0.75000; game P +0.10000 ", 0) == 0 &&
                  fr.find("(written); rho -1.8700 q' +0.00000 +0.00000 +0.74000; eye view 2 rows +1.000000 +0.000000 +0.000000 / +0.000000 +1.000000 +0.000000") != std::string::npos &&
                  fr.find("; HUD rows +1.000000") != std::string::npos && frNone.find("(not written); rho none q' none; eye view 0 rows none origin none; HUD rows none") != std::string::npos &&
                  fr.size() < 1000,
              "F154: the dump-sync line carries the eye run's frame and the native frame, the panel, the setting, the uv, the three points, whether it was written, the lever, the eye "
              "view's rows and origin and the HUD rows (none where unread)");
    }
}

}  // namespace f154test

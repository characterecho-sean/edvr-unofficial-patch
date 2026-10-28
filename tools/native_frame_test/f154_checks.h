// F154 and F155 checks (native on-foot VR): the pick mesh's lever read from the game, the drawn cursor with its hide fallback, the signed turn instrument
// (native_on_foot_f154.h). Header only, run by native_frame_test (run154). The lever is exercised as the hook reads it, from a block of memory laid out as the owner, its
// first entry, the output block and the fov chain are in the game; the sprite's writes against a controller as a block of memory; the turn instrument against synthetic
// runs of a head turning past a HUD frame, a pick point held on the panel (k = 1), locked to the camera (k = 0) or turning against it (k = -1, -2) or twice as far (k = 2).
#pragma once

#include "../../src/d3d11/native_on_foot_f154.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace f154test {

namespace y = edvr::nofoot::f154;
namespace p = edvr::nofoot::f111;

constexpr double kPi = 3.14159265358979323846;

inline y::Mat rotX(double deg) {
    const double c = std::cos(deg * kPi / 180.0), s = std::sin(deg * kPi / 180.0);
    y::Mat r = y::identity();
    r.m[4] = c;
    r.m[5] = -s;
    r.m[7] = s;
    r.m[8] = c;
    return r;
}
inline y::Mat rotY(double deg) {
    const double c = std::cos(deg * kPi / 180.0), s = std::sin(deg * kPi / 180.0);
    y::Mat r = y::identity();
    r.m[0] = c;
    r.m[2] = s;
    r.m[6] = -s;
    r.m[8] = c;
    return r;
}
// A frame whose axes in the world are the columns of q: its rows (axes) are the rows of qt, scaled (the HRC matrix carries a scale of 0.052).
inline void rows(const y::Mat& q, double scale, double out[9]) {
    const y::Mat t = y::transpose(q);
    for (int i = 0; i < 9; ++i) out[i] = t.m[i] * scale;
}
inline double dist3(const double a[3], const double b[3]) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

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
    m.put<uint64_t>(kOwner + y::kOffOwnerCount, 1);
    m.put<uint64_t>(kOwner + y::kOffOwnerArray, m.at(kArray));
    m.put<uint64_t>(kArray, m.at(kEntry));
    m.put<float>(kEntry + y::kOffEntryScale, static_cast<float>(scale));
    m.put<uint8_t>(kEntry + y::kOffScreenFlag, flag);
    m.put<float>(kOut + y::kOffOutScale, static_cast<float>(scale * rho));
    for (int k = 0; k < 3; ++k) m.put<float>(kOut + y::kOffOutPos + 4 * k, q[k]);
    m.put<uint64_t>(kService + y::kOffServiceCamera, m.at(kCamObj));
    m.put<uint64_t>(kService + y::kOffServiceRender, m.at(kRender));
    const double d2r = kPi / 180.0;
    m.put<float>(kCamObj + y::kCameraObjectToCm + y::kCmToC1 + y::kOffC1Fov, static_cast<float>(fovC1Deg * d2r));
    m.put<uint64_t>(kRender + y::kOffRenderUi, m.at(kUiObj));
    m.put<int32_t>(kRender + y::kOffRenderMode, mode);
    m.put<float>(kUiObj + y::kOffUiFov, static_cast<float>(fovUiDeg * d2r));
}

// Streams of one run: a head turning by deg about y (yaw) or x (pitch) past a fixed HUD frame, the game's point at k times the held turn and the written one at
// kWritten times it. The mouse is still (the same uv), the eye moves by eyeStepMm along x each frame.
inline y::Sec runFit(double kGame, double kWritten, bool yaw, int steps, double stepDeg, const double p0[3], double eyeStepMm = 0.0, bool locked = false) {
    y::TurnTracker tracker;
    y::Sec sec;
    const y::Mat qh = y::mul(rotY(15.0), rotX(-5.0));
    for (int i = 0; i <= steps; ++i) {
        const double deg = stepDeg * i;
        const y::Mat qe = yaw ? rotY(deg) : rotX(deg);
        y::TurnInput in;
        in.frame = 100 + static_cast<uint32_t>(i);
        in.key = 0x1000;
        in.u = 0.4;
        in.v = 0.6;
        in.haveEye = in.haveHrc = true;
        rows(qe, 1.0, in.E);
        rows(qh, 0.052, in.H);
        in.eye[0] = 1.0 + eyeStepMm * 0.001 * i;
        in.eye[1] = 1.6;
        in.eye[2] = -2.0;
        const double ks[y::kStreams] = {kGame, kWritten};
        for (int s = 0; s < y::kStreams; ++s) {
            // the held point is p0 turned by -deg (M = R R0t = rotation by -deg for this head); a stream with k turns k times as far
            const y::Mat m = locked ? y::identity() : (yaw ? rotY(-ks[s] * deg) : rotX(-ks[s] * deg));
            y::apply(m, p0, in.p[s]);
        }
        tracker.place(in, sec);
    }
    return sec;
}

// A controller as a block of memory with the fields the placement leaves (the mouse NDC at +0x84, the hit byte at +0x100, the sprite's position at +0x140).
struct Controller {
    std::vector<uint8_t> mem;
    Controller() : mem(0x200) {
        for (size_t i = 0; i < mem.size(); ++i) mem[i] = static_cast<uint8_t>(i * 7u + 3u);
        const float ndc[2] = {0.25f, -0.4f}, pos[3] = {-0.02f, 0.05f, 0.65f};
        std::memcpy(&mem[p::kOffMouse], ndc, sizeof(ndc));
        std::memcpy(&mem[p::kOffPos], pos, sizeof(pos));
        mem[p::kOffHit] = 1;
    }
    uintptr_t address() { return reinterpret_cast<uintptr_t>(mem.data()); }
    void sprite(float out[3]) const { std::memcpy(out, &mem[p::kOffPos], 12); }
    // The count of bytes that differ from a copy outside the sprite's 12.
    size_t outsideSprite(const std::vector<uint8_t>& before) const {
        size_t n = 0;
        for (size_t i = 0; i < mem.size(); ++i)
            if (mem[i] != before[i] && (i < p::kOffPos || i >= p::kOffPos + 12)) ++n;
        return n;
    }
};

template <class Check>
inline void run154(Check&& check) {
    const double p0[3] = {-0.02, 0.05, 0.65};
    const double ahead[3] = {0.0, 0.0, 0.65};
    {   // the build's addresses and the screen-space test
        check(y::kOwnerRva == 0x47299F0 && y::kOwnerSlotRva == 0x5607E68 && y::kOwnerStolen == 7 && y::kOffEntryScale == 0xE0 && y::kOffOutPos == 0x40 &&
                  y::kOffOutScale == 0x50 && y::kOffOwnerCount == 0x1B0 && y::kOffOwnerArray == 0x1B8 && y::kOffScreenFlag == 0x1C0 && y::kOffOwnerService == 0x8 &&
                  y::kOffServiceRender == 0x830 && y::kOffServiceCamera == 0x838 && y::kCameraObjectToCm == 0x10 && y::kCmToC1 == 0x70 && y::kOffC1Fov == 0x260 &&
                  y::kOffRenderUi == 0x18 && y::kOffUiFov == 0x17C && y::kOffRenderMode == 0x3694 && y::kOwnerPrologue[0] == 0x4C && y::kOwnerPrologue[1] == 0x8B &&
                  y::kOwnerPrologue[2] == 0xDC && y::kOwnerPrologue[3] == 0x49 && y::kOwnerPrologue[4] == 0x89 && y::kOwnerPrologue[5] == 0x5B && y::kOwnerPrologue[6] == 0x18 &&
                  y::kOwnerPrologue[15] == 0x41 && sizeof(y::kOwnerPrologue) == 16,
              "F154: the hook is at exe+0x47299F0 behind the owner vtable's slot at exe+0x5607E68; its 16-byte pin starts with mov r11,rsp (4C 8B DC) and mov [r11+0x18],rbx "
              "(49 89 5B 18), 7 bytes are moved; the lever and cross-check offsets are the RE's");
        check(y::screenSpace(y::kOwnerRva, 2, 1) && !y::screenSpace(0x4526E40, 2, 1) && !y::screenSpace(y::kOwnerRva, 0, 1) && !y::screenSpace(y::kOwnerRva, 2, 0),
              "F155: the screen-space test wants the pinned owner function, a count above 0 and the flag set; the upright owner (exe+0x4526E40) is not screen-space");
    }
    {   // the lever undo inverts the forward lever
        const double rhos[] = {-2.0, -1.87, 2.0, 1.0};
        const double pts[][3] = {{-0.02, 0.05, 0.65}, {0.4, -0.3, 0.7}, {0.0, 0.0, 0.5}, {-0.6, 0.35, 0.9}};
        double worstUndo = 0.0, worstId = 0.0;
        bool refusedNone = true;
        for (double rho : rhos)
            for (double qz : {0.7, 0.3}) {
                y::Lever lever;
                lever.valid = true;
                lever.rho = rho;
                lever.q[2] = qz;
                for (const auto& pt : pts) {
                    double P[3], back[3];
                    y::applyLever(rho, qz, pt, P);
                    refusedNone = refusedNone && y::undoLever(&lever, P, back) == y::Refusal::kNone;
                    worstUndo = std::fmax(worstUndo, dist3(back, pt));
                    if (rho == 1.0) worstId = std::fmax(worstId, std::fmax(dist3(P, pt), dist3(back, pt)));
                }
            }
        check(refusedNone && worstUndo < 1e-9 && worstId < 1e-12,
              "F154: the lever undo inverts the forward lever for rho = -2, -1.87, +2 and 1 (within 1e-9); rho = 1 is the identity");
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
        y::Lever tiny = good, nan = good, inf = good, nanq = good, invalid = good, edge = good;
        tiny.rho = 0.04;
        nan.rho = std::numeric_limits<double>::quiet_NaN();
        inf.rho = std::numeric_limits<double>::infinity();
        nanq.q[2] = std::numeric_limits<double>::quiet_NaN();
        invalid.valid = false;
        edge.rho = -0.05;
        const double P[3] = {0.1, 0.2, 0.8}, bad[3] = {0.1, std::numeric_limits<double>::quiet_NaN(), 0.8};
        double out[3];
        check(y::undoLever(nullptr, P, out) == y::Refusal::kNoLever && y::undoLever(&invalid, P, out) == y::Refusal::kNoLever &&
                  y::undoLever(&tiny, P, out) == y::Refusal::kTinyRho && y::undoLever(&nan, P, out) == y::Refusal::kNonFinite &&
                  y::undoLever(&inf, P, out) == y::Refusal::kNonFinite && y::undoLever(&nanq, P, out) == y::Refusal::kNonFinite &&
                  y::undoLever(&good, bad, out) == y::Refusal::kNonFinite && y::undoLever(&edge, P, out) == y::Refusal::kNone,
              "F154: the undo refuses with no lever (none, or not valid), a rho under 0.05 in size, a rho or q.z that is not a number and a point that is not a number; a rho of "
              "exactly -0.05 is taken");
    }
    {   // F155: what a placement decides: drawn on a screen-space panel with a lever, the hide fallback for each refusal, nothing elsewhere
        y::Lever lever;
        lever.valid = true;
        lever.rho = -1.949;
        lever.q[2] = 0.6949;
        const double game[3] = {-0.18, 0.09, 0.9};
        double want[3];
        y::undoLever(&lever, game, want);
        const y::Decision drawn = y::decide(false, true, game, &lever);
        y::Lever tiny = lever, nan = lever, stale;   // stale: a lever table with no fresh lever hands the placement none (a null pointer)
        tiny.rho = 0.01;
        nan.rho = std::numeric_limits<double>::quiet_NaN();
        const y::Decision noLever = y::decide(false, true, game, nullptr), noValid = y::decide(false, true, game, &stale), tinyRho = y::decide(false, true, game, &tiny),
                          notFinite = y::decide(false, true, game, &nan);
        check(drawn.act == y::Act::kDrawn && drawn.refused == y::Refusal::kNone && dist3(drawn.target, want) < 1e-12 && dist3(drawn.target, game) > 0.1,
              "F155: a placement on a screen-space panel with a fresh lever writes the drawn point, the game's sprite with the lever undone, always (there is no setting)");
        check(noLever.act == y::Act::kHide && noLever.refused == y::Refusal::kNoLever && noValid.act == y::Act::kHide && noValid.refused == y::Refusal::kNoLever &&
                  tinyRho.act == y::Act::kHide && tinyRho.refused == y::Refusal::kTinyRho && notFinite.act == y::Act::kHide && notFinite.refused == y::Refusal::kNonFinite,
              "F155: on a screen-space panel the hide fallback takes each refusal: no lever (the hook never armed, or none this frame or the one before), a rho under 0.05 "
              "in size, a value that is not a number");
        const y::Decision upright = y::decide(false, false, game, &lever), flatScreen = y::decide(true, true, game, &lever), flatUpright = y::decide(true, false, game, nullptr);
        check(upright.act == y::Act::kNone && upright.refused == y::Refusal::kNone && flatScreen.act == y::Act::kNone && flatUpright.act == y::Act::kNone,
              "F155: a panel that is not screen-space keeps the game's own place (no write, no refusal), and the flat profile writes nothing whatever the panel");
    }
    {   // the writes, against a controller's memory
        y::Lever lever;
        lever.valid = true;
        lever.rho = -1.949;
        lever.q[2] = 0.6949;
        float s0[3];
        Controller c;
        c.sprite(s0);
        const double game[3] = {s0[0], s0[1], s0[2]};
        const std::vector<uint8_t> before = c.mem;
        std::vector<uintptr_t> at;
        auto writer = [&](uintptr_t address, float v) {
            at.push_back(address - c.address());
            std::memcpy(&c.mem[address - c.address()], &v, sizeof(v));
            return true;
        };
        const y::Decision d = y::decide(false, true, game, &lever);
        const y::Applied a = y::perform(d, c.address(), writer);
        float now[3];
        c.sprite(now);
        const double wantD[3] = {static_cast<float>(d.target[0]), static_cast<float>(d.target[1]), static_cast<float>(d.target[2])};
        const double got[3] = {now[0], now[1], now[2]};
        check(a.drawn && !a.hidden && !a.writeFailed && !a.hideFailed && at.size() == 3 && at[0] == p::kOffPos && at[1] == p::kOffPos + 4 && at[2] == p::kOffPos + 8 &&
                  dist3(got, wantD) < 1e-12 && c.outsideSprite(before) == 0 && c.mem[p::kOffHit] == 1,
              "F155: the drawn write is the sprite's three floats at +0x140, x then y then z, and no other byte of the controller changes (the mouse NDC and the hit byte stay)");
        // the hide: z first, to the pinned position, which cannot be seen
        Controller h;
        const std::vector<uint8_t> hb = h.mem;
        std::vector<uintptr_t> hat;
        auto hwriter = [&](uintptr_t address, float v) {
            hat.push_back(address - h.address());
            std::memcpy(&h.mem[address - h.address()], &v, sizeof(v));
            return true;
        };
        const y::Decision dh = y::decide(false, true, game, nullptr);
        const y::Applied ah = y::perform(dh, h.address(), hwriter);
        float hs[3];
        h.sprite(hs);
        const float typical[3] = {-0.02f, 0.05f, 0.65f}, behindAxis[3] = {0.0f, 0.0f, -1.0f}, farAhead[3] = {1.0e5f, 1.0e5f, 1.0f};
        check(ah.hidden && !ah.drawn && !ah.writeFailed && !ah.hideFailed && hat.size() == 3 && hat[0] == p::kOffPos + 8 && hat[1] == p::kOffPos &&
                  hat[2] == p::kOffPos + 4 && hs[0] == y::kHidden[0] && hs[1] == y::kHidden[1] && hs[2] == y::kHidden[2] && y::unseen(hs) && h.outsideSprite(hb) == 0 &&
                  !y::unseen(typical) && !y::unseen(behindAxis) && !y::unseen(farAhead),
              "F155: the hide fallback is F122's: z first, then x and y, to (1e5, 1e5, -1) behind the pick camera and far off its axis; nothing else is written; a cursor on the "
              "panel, a point behind on the axis and a point far to the side but ahead are not taken for unseen");
        // a drawn write that fails part way falls back to the hide; both failing is counted apart
        Controller f;
        int calls = 0;
        auto failSecond = [&](uintptr_t address, float v) {
            ++calls;
            if (calls == 2) return false;   // the drawn point's y
            std::memcpy(&f.mem[address - f.address()], &v, sizeof(v));
            return true;
        };
        const y::Applied af = y::perform(d, f.address(), failSecond);
        float fs[3];
        f.sprite(fs);
        check(!af.drawn && af.writeFailed && af.hidden && !af.hideFailed && y::unseen(fs),
              "F155: a drawn write that stops part way is a failed write and the sprite is hidden instead (z first, so what the first write put there is out of view)");
        Controller g;
        auto failAll = [&](uintptr_t, float) { return false; };
        const y::Applied ag = y::perform(d, g.address(), failAll), ah2 = y::perform(dh, g.address(), failAll);
        auto never = [&](uintptr_t, float) {
            calls += 1000;
            return true;
        };
        calls = 0;
        const y::Applied an = y::perform(y::decide(false, false, game, &lever), g.address(), never);
        check(!ag.drawn && ag.writeFailed && !ag.hidden && ag.hideFailed && !ah2.hidden && ah2.hideFailed && !ah2.writeFailed && !an.drawn && !an.hidden && !an.writeFailed &&
                  !an.hideFailed && calls == 0,
              "F155: a drawn write and its hide both failing counts a failed write and a failed hide; a hide that fails counts a failed hide; no decision means no write call");
        // the counts a placement makes
        y::Sec sec;
        sec.addPlacement(true, d, a);                                                              // drawn
        sec.addPlacement(true, dh, ah);                                                            // no lever, hidden
        y::Lever tiny = lever;
        tiny.rho = 0.01;
        y::Lever nan = lever;
        nan.rho = std::numeric_limits<double>::quiet_NaN();
        const y::Decision dt = y::decide(false, true, game, &tiny), dn = y::decide(false, true, game, &nan);
        y::Applied hid;
        hid.hidden = true;
        sec.addPlacement(true, dt, hid);                                                           // tiny rho, hidden
        sec.addPlacement(true, dn, hid);                                                           // not finite, hidden
        sec.addPlacement(true, d, af);                                                             // drawn write failed, hidden
        sec.addPlacement(true, dh, ag);                                                            // (counted as the failed write + hide below)
        sec.addPlacement(false, y::decide(false, false, game, nullptr), y::Applied{});             // not screen-space
        check(sec.placements == 7 && sec.screen == 6 && sec.written == 1 && sec.hidden == 4 && sec.writeFailed == 2 && sec.hideFailed == 1 && sec.refusedLever[0] == 2 &&
                  sec.refusedLever[1] == 1 && sec.refusedLever[2] == 1,
              "F155: a placement counts a screen-space panel, the drawn write, the sprites hidden, a failed write, a failed hide and each refusal by cause apart");
    }
    {   // a call that left the sprite as it found it did not place it
        const float found[3] = {-0.02f, 0.05f, 0.65f}, same[3] = {-0.02f, 0.05f, 0.65f}, moved[3] = {-0.02f, 0.05f, 0.6500001f}, hiddenNow[3] = {y::kHidden[0], y::kHidden[1], y::kHidden[2]};
        check(!y::spritePlaced(found, same) && y::spritePlaced(found, moved) && y::spritePlaced(hiddenNow, found) && y::spritePlaced(found, hiddenNow),
              "F155: a call that left the sprite as it found it (our last write, the hit byte an earlier call's) did not place it, so the lever is not undone twice; a change of one "
              "float, or from the hidden position, is a placement");
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
        empty.put<uint64_t>(kOwner + y::kOffOwnerCount, 0);
        Mem zero;
        layout(zero, -1.87, 0.0, 1, q, 93.6, 56.25, 4);
        zero.put<float>(kOut + y::kOffOutScale, 1.0f);
        const float nanq[3] = {0.0f, std::numeric_limits<float>::quiet_NaN(), 0.7f};
        Mem badq;
        layout(badq, -1.87, 1.0, 1, nanq, 93.6, 56.25, 4);
        Mem noArray;
        layout(noArray, -1.87, 1.0, 1, q, 93.6, 56.25, 4);
        noArray.put<uint64_t>(kOwner + y::kOffOwnerArray, 0);
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
        bool validYaw = true, validPitch = true;
        for (double k : ks) {
            const y::Sec yawRun = runFit(k, k, true, 12, 0.5, p0), pitchRun = runFit(k, k, false, 12, 0.5, p0);
            for (int s = 0; s < y::kStreams; ++s) {
                validYaw = validYaw && yawRun.fit[s].valid(1);
                validPitch = validPitch && pitchRun.fit[s].valid(0);
                worstYaw = std::fmax(worstYaw, std::fabs(yawRun.fit[s].k(1) - k));
                worstPitch = std::fmax(worstPitch, std::fabs(pitchRun.fit[s].k(0) - k));
            }
        }
        check(validYaw && validPitch && worstYaw < 0.05 && worstPitch < 0.05,
              "F154: the signed turn fit recovers k = 0, 1, -1, -2 and +2 on yaw and on pitch within 0.05 on a 6 deg turn with the mouse still, for both streams");
        // the turn about the other axis is not read: a yaw-only run (p straight ahead) has no pitch to fit
        const y::Sec yawOnly = runFit(1.0, 1.0, true, 12, 0.5, ahead), pitchOnly = runFit(1.0, 1.0, false, 12, 0.5, ahead);
        check(yawOnly.fit[y::kGame].valid(1) && !yawOnly.fit[y::kGame].valid(0) && pitchOnly.fit[y::kGame].valid(0) && !pitchOnly.fit[y::kGame].valid(1),
              "F154: a yaw-only turn gives a yaw k and no pitch k, and the reverse (an axis with under about a degree of model turn reads n/a)");
        // the flown case: the game's point turning twice against the head, the written point held on the panel; exact for a point ahead through a 20 deg head turn
        const y::Sec big = runFit(-2.0, 1.0, true, 10, 2.0, ahead);
        check(std::fabs(big.fit[y::kGame].k(1) + 2.0) < 1e-6 && std::fabs(big.fit[y::kWritten].k(1) - 1.0) < 1e-6 && std::fabs(big.turnMaxDeg - 20.0) < 0.01,
              "F154: the two streams are fitted apart (the game's point at k = -2, the written one at 1 here) and exact for a point ahead through a 20 deg head turn; the run's "
              "largest turn is 20 deg");
    }
    {   // judged only past 2 deg, the eye's move, the pure translation
        const y::Sec shortTurn = runFit(1.0, 1.0, true, 3, 0.51, p0);   // 1.53 deg
        const y::Sec edge = runFit(1.0, 1.0, true, 4, 0.51, p0);        // 2.04 deg: the last placement counts
        check(shortTurn.judged == 0 && shortTurn.judgedRuns == 0 && !shortTurn.judgedSecond() && !shortTurn.fit[y::kGame].valid(1) && edge.judged == 1 &&
                  edge.judgedRuns == 1 && edge.runs == 1 && edge.judgedSecond(),
              "F154: a placement counts toward k once the head has turned 2 deg against the HUD frame since its run began (1.53 deg none, 2.04 deg one placement in one judged run)");
        // a pure eye translation: nothing turns, so nothing is judged and k has no value; the move is reported in the eye's axes
        const y::Sec shift = runFit(1.0, 1.0, true, 10, 0.0, p0, 4.0);
        check(shift.judged == 0 && !shift.fit[y::kGame].valid(0) && !shift.fit[y::kGame].valid(1) && std::fabs(shift.eyeMoveMaxMm - 40.0) < 1e-6 && shift.turnMaxDeg < 1e-9,
              "F154: a pure eye translation judges nothing (the head did not turn) and reports the eye's largest move, 40 mm after ten 4 mm steps");
        // a turn with the eye moving and the pick locked to the camera: k is 0 and the move is reported beside it
        const y::Sec locked = runFit(0.0, 0.0, true, 12, 0.5, p0, 0.5, true);
        check(locked.judged > 0 && std::fabs(locked.fit[y::kGame].k(1)) < 0.05 && std::fabs(locked.eyeMoveMaxMm - 6.0) < 1e-6,
              "F154: a head turn with the eye moving 0.5 mm a frame and the points locked to the camera gives k near 0 and reports the 6 mm the eye moved");
    }
    {   // runs: a moving mouse starts a new run each placement; a gap of more than 3 frames starts one; the frame counter going back starts one
        y::TurnTracker tr;
        y::Sec sec;
        const y::Mat qh = y::mul(rotY(15.0), rotX(-5.0));
        auto feed = [&](uint32_t frame, double deg, double u, uintptr_t key = 0x1000) {
            y::TurnInput in;
            in.frame = frame;
            in.key = key;
            in.u = u;
            in.v = 0.6;
            in.haveEye = in.haveHrc = true;
            rows(rotY(deg), 1.0, in.E);
            rows(qh, 0.052, in.H);
            for (int s = 0; s < y::kStreams; ++s) y::apply(rotY(-deg), p0, in.p[s]);
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
        rows(y::identity(), 1.0, base.E);
        rows(y::identity(), 0.052, base.H);
        for (int s = 0; s < y::kStreams; ++s)
            for (int k = 0; k < 3; ++k) base.p[s][k] = p0[k];
        y::TurnInput noEye = base, noHrc = base, zeroRow = base, nanPoint = base, lean = base;
        noEye.haveEye = false;
        noHrc.haveHrc = false;
        for (int k = 0; k < 3; ++k) zeroRow.H[3 + k] = 0.0;
        nanPoint.p[y::kWritten][1] = std::numeric_limits<double>::quiet_NaN();
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
    {   // the seconds add: counts add, the extremes are kept, the fit's sums add
        y::Sec a, b;
        a.placements = 3;
        a.written = 1;
        a.hidden = 2;
        a.refusedLever[0] = 2;
        a.turnMaxDeg = 4.0;
        a.eyeMoveMaxMm = 1.0;
        a.addCall(y::ReadResult::kLever);
        a.addLever(-1.9, 0.7);
        b.placements = 5;
        b.hideFailed = 1;
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
        check(a.placements == 8 && a.written == 1 && a.hidden == 2 && a.hideFailed == 1 && a.refusedLever[0] == 2 && a.refusedTurn[2] == 1 && a.turnMaxDeg == 9.0 &&
                  a.eyeMoveMaxMm == 1.0 && a.calls == 3 && a.leverReads == 2 && a.notScreen == 1 && a.unread == 1 && std::fabs(a.rhoMin + 1.9) < 1e-12 &&
                  std::fabs(a.rhoMax + 1.8) < 1e-12 && std::fabs(a.rhoSum + 3.7) < 1e-12 && a.haveCross && a.cross.haveC1 && std::fabs(a.fit[y::kGame].sab[1] - 0.08) < 1e-12 &&
                  std::fabs(a.fit[y::kGame].saa[1] - 0.08) < 1e-12,
              "F154: merged seconds add their counts, keep the largest turn and eye move and the extremes of rho, take the last cross-check and add the fit's sums");
    }
    {   // the lines
        char line[2000];
        const y::Sec empty;
        y::formatTurn(line, sizeof(line), "1 s", empty);
        const std::string none = line;
        y::Sec s = runFit(-2.0, 0.5, true, 12, 0.5, p0);
        s.placements = 120;
        s.screen = 118;
        s.written = 90;
        s.hidden = 28;
        s.refusedLever[0] = 7;
        s.addCall(y::ReadResult::kLever);
        s.addLever(-1.87, 0.74);
        s.addLever(-1.86, 0.75);
        s.haveCross = true;
        s.cross.haveC1 = s.cross.haveUi = s.cross.haveMode = true;
        s.cross.fovC1 = 93.6 * kPi / 180.0;
        s.cross.fovUi = 56.25 * kPi / 180.0;
        s.cross.mode = 4;
        const int n = y::formatTurn(line, sizeof(line), "so far, 7 s with a judged run", s);
        const std::string full = line;
        check(none.rfind("panel cursor turn (F154) 1 s: placements 0: screen-space 0, written 0, hidden 0, refused: no lever 0, tiny rho 0, not finite 0, write failed 0, "
                         "hide failed 0, no eye view 0, no HRC matrix 0, degenerate row 0; judged 0 placements in 0 of 0 runs",
                         0) == 0 &&
                  none.find("k yaw: game n/a written n/a; k pitch: game n/a written n/a") != std::string::npos &&
                  none.find("rho mean n/a min n/a max n/a from 0 owner calls") != std::string::npos && none.find("rho_fov n/a fov_C1 n/a deg fov_UI n/a deg mode n/a; q.z n/a m") != std::string::npos &&
                  full.rfind("panel cursor turn (F154) so far, 7 s with a judged run: placements 120: screen-space 118, written 90, hidden 28, refused: no lever 7, ", 0) == 0 &&
                  full.find("k yaw: game -2.00 written +0.50;") != std::string::npos && full.find("rho mean -1.865 min -1.870 max -1.860 from 1 owner calls (screen-space 2, not 0, unread 0)") != std::string::npos &&
                  full.find("rho_fov 1.992 fov_C1 93.60 deg fov_UI 56.25 deg mode 4; q.z +0.7450 m") != std::string::npos && n > 0 && n < 1000 && none.size() < 1000,
              "F154: the turn line carries the window, the placements, the drawn writes, the sprites hidden and the refusals by cause, the judged placements and runs, the largest "
              "turn and eye move, k for yaw and pitch for the game's and the written point (n/a without a turn about the axis), rho mean/min/max, the cross-check and q.z, and stays "
              "well inside the log's 1,200 characters");
        y::formatWrites(line, sizeof(line), 10, s);
        const std::string writes = line;
        check(writes.rfind("panel cursor writes (F154) 10 s: placements 120: screen-space 118, written 90, hidden 28, refused: no lever 7, tiny rho 0, not finite 0, write failed 0, "
                           "hide failed 0; owner calls 1 (screen-space 2, not 0, unread 0); rho mean -1.865 min -1.870 max -1.860, rho_fov 1.992 fov_C1 93.60 deg fov_UI 56.25 deg "
                           "mode 4, q.z +0.7450 m",
                           0) == 0 && writes.size() < 1000,
              "F154: the writes line carries the window's seconds, the placements, drawn writes, sprites hidden, refusals by cause, the owner calls and the lever they read (rho, the "
              "cross-check, q.z), so rho is on record without a judged second");
        y::FrameRec r;
        r.ledger = 1234;
        r.render = 5678;
        r.panel = 0x12345678ABCDull;
        r.outcome = y::Outcome::kDrawn;
        r.u = 0.25;
        r.v = 0.75;
        r.p[y::kGame][0] = 0.1;
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
        check(fr.rfind("panel cursor frame (F154): ledger frame 1234 render frame 5678 panel 5678ABCD uv 0.25000 0.75000; game P +0.10000 ", 0) == 0 &&
                  fr.find("(drawn); rho -1.8700 q' +0.00000 +0.00000 +0.74000; eye view 2 rows +1.000000 +0.000000 +0.000000 / +0.000000 +1.000000 +0.000000") != std::string::npos &&
                  fr.find("; HUD rows +1.000000") != std::string::npos && frNone.find("(left); rho none q' none; eye view 0 rows none origin none; HUD rows none") != std::string::npos &&
                  fr.size() < 1000,
              "F154: the dump-sync line carries the eye run's frame and the native frame, the panel, the uv, the game's and the written point, whether it was drawn, hidden or left, "
              "the lever, the eye view's rows and origin and the HUD rows (none where unread)");
    }
}

}  // namespace f154test

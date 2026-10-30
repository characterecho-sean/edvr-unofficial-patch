// B3 checks (the scoped sniper's lens at full detail), runB3 below. Header only, run by native_frame_test.
//
// B3 (src\d3d11\scope_zoom_b3.h, src\common\scope_zoom_window.h): the game draws the LEFT eye as the zoomed view -- its told frustum the eye's own tangents divided by M, an off-axis window about
// its direction to the far point c -- and the lens takes its picture from that eye. What is checked here is the arithmetic every module shares: the window (centred on c, divided by M, inside
// the eye's span), the reprojection of a lens pixel's world point from the right eye's depth through the left eye's zoomed rows (a synthetic world and two eyes 6.3 cm apart: the point lands
// where the left eye draws it at every depth, a plain sample of the same direction does not), the decisions (the gate and the hold, the strip's ramp, the lens's source and the reason it fell
// back to A, the reset edge, the light grid's bypass), the tick itself run tick after tick (b3Step: ADS entry and exit, the share dropping, the key and the weapon changing mid-press, a
// lost frame), the readout pin taking the right eye alone, the window's wire form between the halves, and the press line and the runtime's report line within the log's limit.
#pragma once

#include "../../src/d3d11/native_on_foot_f85.h"
#include "../../src/d3d11/scope_zoom_b3.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace b3test {

namespace sz = edvr::scopezoom;
namespace sw = edvr::scopewin;

// A pinhole eye looking down +z (x right, y up) at pos, with the tangents {L, R, D, U}, as the game's scene rows lay it out (native_on_foot_core.h worldClip: clip_j = (p - o) . rows[0..11] +
// rows[12 + j], the origin o = the eye's position): reverse-Z with an infinite far plane, so the depth is n / z. jx, jy translate the NDC square (the TAA jitter).
struct Cam {
    float rows[16] = {};
    double o[3] = {};
};
inline Cam makeCam(const double pos[3], const double tan4[4], double n, double jx = 0.0, double jy = 0.0) {
    Cam c;
    const double sx = tan4[1] - tan4[0], sy = tan4[3] - tan4[2];
    double m[16] = {};
    m[0] = 2.0 / sx;
    m[8] = -(tan4[1] + tan4[0]) / sx;
    m[5] = 2.0 / sy;
    m[9] = -(tan4[3] + tan4[2]) / sy;
    m[11] = 1.0;
    m[14] = n;
    for (int i = 0; i < 4; ++i) {
        m[4 * i + 0] += jx * m[4 * i + 3];
        m[4 * i + 1] += jy * m[4 * i + 3];
    }
    for (int i = 0; i < 16; ++i) c.rows[i] = static_cast<float>(m[i]);
    for (int i = 0; i < 3; ++i) c.o[i] = pos[i];
    return c;
}
// The point P in the eye's NDC square and its reverse-Z depth; false at or behind the eye.
inline bool projectCam(const Cam& c, const double P[3], double ndc[2], double* depth) {
    const double v[3] = {P[0] - c.o[0], P[1] - c.o[1], P[2] - c.o[2]};
    double clip[4];
    for (int j = 0; j < 4; ++j) clip[j] = v[0] * c.rows[j] + v[1] * c.rows[4 + j] + v[2] * c.rows[8 + j] + c.rows[12 + j];
    if (!(clip[3] > 1.0e-9)) return false;
    ndc[0] = clip[0] / clip[3];
    ndc[1] = clip[1] / clip[3];
    if (depth) *depth = clip[2] / clip[3];
    return true;
}

// ---- the frame thread's tick: scope_zoom_b3.h's b3Step, b3IdleStep and b3OffStep driven as native_on_foot.cpp's s1::b3Tick and s1::tick drive them, tick after tick at a frame's pace ---------------
// The window is latched at the first tick it can start, with the sights up, and held; the strip goes black over a tenth of a second and the zoom is asked once it is; the request ends with the
// window, the strip stays three ticks and comes back. The cases are the presses the flight will make: ADS entry and exit, the share dropping under 0.9, the key toggled mid-press, a weapon
// switch mid-press, and the lens not qualifying or the left rows missing.
struct TickSim {
    sz::B3Loop loop;
    sz::B3StepIn in;
    sz::B3StepOut out;
    sw::Request last;   // what the tick published
    uint32_t ticks = 0, askedTicks = 0, heldTicks = 0, requestTicks = 0, noLeftRows = 0;
    TickSim() {
        in.keyOn = in.rendered = in.shareMode = in.qualifies = in.sightsFull = in.leftOk = true;
        in.share = 1.0;
        in.dt = 0.011;
        in.m = 4.067;
        in.tcNow[0] = 0.0100;
        in.tcNow[1] = -0.0200;
    }
    void tick() {
        out = sz::b3Step(loop, in);
        last = out.request;
        ++ticks;
        askedTicks += out.askedTick ? 1u : 0u;
        heldTicks += out.heldTick ? 1u : 0u;
        requestTicks += out.requestTick ? 1u : 0u;
        noLeftRows += out.noLeftRowsTick ? 1u : 0u;
    }
    void run(int n) {
        for (int i = 0; i < n; ++i) tick();
    }
    void idle() {   // the sights down: tick() calls b3IdleStep
        last = sz::b3IdleStep(loop, in.dt);
        out = sz::B3StepOut{};
        ++ticks;
    }
};

template <class Check>
inline void runB3Tick(Check&& check) {
    // ADS entry: latched at the first tick, the strip black over ten ticks, the zoom asked on the tenth and never before the strip is black
    {
        TickSim t;
        t.tick();
        check(t.out.latched && t.out.heldTick && t.out.askedTick && t.out.asked && t.out.frameAsked && !t.out.requestTick && !t.last.zoom,
              "B3 tick: the window is latched at the first tick the sights are up with the share full, and the zoom is not yet asked");
        check(t.loop.tc[0] == 0.0100 && t.loop.tc[1] == -0.0200 && t.loop.m == 4.067, "B3 tick: the window's centre and narrowing are the first tick's");
        bool stripBlackWhenAsked = true, neverZoomEarly = true;
        int askedAt = -1;
        for (int i = 2; i <= 14; ++i) {
            t.in.tcNow[0] = 0.0100 + 0.001 * i;   // the head drifts: the latched window does not follow
            t.tick();
            if (t.last.zoom && askedAt < 0) askedAt = i;
            if (t.last.zoom) stripBlackWhenAsked = stripBlackWhenAsked && t.last.strip >= 0.999f;
            if (!t.last.zoom && i >= 11) neverZoomEarly = false;
        }
        check(askedAt >= 10 && askedAt <= 11 && stripBlackWhenAsked && neverZoomEarly, "B3 tick: the zoom is asked for on the tenth tick, with the strip black in the same request, and stays asked");
        check(t.last.tc[0] == 0.0100f && t.last.tc[1] == -0.0200f && t.last.m == 4.067f && t.loop.tc[0] == 0.0100, "B3 tick: the request carries the latched centre and narrowing while the head drifts");
        check(t.askedTicks == t.ticks && t.heldTicks == t.ticks && t.requestTicks >= 3 && t.noLeftRows == 0u, "B3 tick: every tick counts asked and held, the zoom's ticks only from the tenth");
        uint32_t w[sw::kWords];   // the request as the runtime reads it: five words and back
        sw::toWords(t.last, w);
        const sw::Request back = sw::fromWords(w);
        check(back.zoom && back.m == 4.067f && back.tc[0] == 0.0100f && back.strip == 1.0f, "B3 tick: and it crosses to the runtime whole");
    }
    // ADS exit by the sights coming down: b3IdleStep. The request ends at once, the strip stays black three ticks and comes back
    {
        TickSim t;
        t.run(14);
        check(t.last.zoom && t.loop.latched, "B3 tick (exit): the zoom stands before the exit");
        t.idle();
        check(!t.last.zoom && !t.loop.latched && t.last.strip >= 0.999f, "B3 tick (exit): the sights down end the request at once, the strip still black");
        t.idle();
        t.idle();
        check(t.last.strip >= 0.999f, "B3 tick (exit): the strip stays black for three ticks, the runtime's answer lagging the ask by a frame or two");
        t.idle();
        check(t.last.strip < 0.999f && t.last.strip > 0.8f, "B3 tick (exit): then it comes back");
        for (int i = 0; i < 12; ++i) t.idle();
        check(t.last.strip == 0.0f && !t.last.zoom && t.loop.strip.fade == 0.0, "B3 tick (exit): and is gone after another tenth of a second");
        t.tick();   // the next press begins again from the start
        check(t.out.latched && !t.last.zoom && t.last.strip > 0.0f && t.last.strip < 0.2f, "B3 tick (exit): the next press latches again and ramps from nothing");
        const sw::Request off = sz::b3OffStep(t.loop);
        check(!off.zoom && off.strip == 0.0f && !t.loop.latched && t.loop.strip.fade == 0.0, "B3 tick (off): an unarmed tick or the flat profile drops the window and the strip at once");
    }
    // the sights up but w under 1 (the game's own transition): the hold ends
    {
        TickSim t;
        t.run(14);
        t.in.sightsFull = false;
        t.tick();
        check(!t.out.latched && !t.last.zoom && !t.out.frameAsked && t.last.strip >= 0.999f, "B3 tick: w under 1 in the sights ends the window the same tick");
    }
    // the share dropping: held to 0.9, ended below it; never started under full weight
    {
        TickSim t;
        t.run(14);
        t.in.share = 0.95;
        t.tick();
        check(t.out.latched && t.last.zoom, "B3 tick (share): at 0.95 the window is held and the zoom stays asked (the runtime refuses to narrow until the share is full)");
        t.in.share = 0.85;
        t.tick();
        check(!t.out.latched && !t.last.zoom && t.last.strip >= 0.999f && !t.out.frameAsked, "B3 tick (share): under 0.9 the window ends at once and the strip holds");
        t.in.share = 1.0;
        t.tick();
        check(t.out.latched && t.last.zoom, "B3 tick (share): a share back at full weight with the sights still up starts a new window, and the zoom is asked again at once (the strip never left black)");
        TickSim u;
        u.in.share = 0.95;
        u.run(5);
        check(!u.out.latched && u.heldTicks == 0u && u.askedTicks == 5u && u.noLeftRows == 0u, "B3 tick (share): a share short of full weight never starts the window, and that is not the left rows' fault");
    }
    // the key toggled mid-press; a weapon switch mid-press
    {
        TickSim t;
        t.run(14);
        t.in.keyOn = false;
        t.tick();
        check(!t.out.latched && !t.out.asked && !t.out.askedTick && !t.last.zoom && t.last.strip >= 0.999f && !t.out.frameAsked, "B3 tick (key): the key off mid-press ends the window and the request at once");
        t.run(2);
        t.in.keyOn = true;
        t.in.tcNow[0] = -0.0300;
        t.in.tcNow[1] = 0.0150;
        t.tick();
        check(t.out.latched && t.loop.tc[0] == -0.0300 && t.loop.tc[1] == 0.0150 && t.last.zoom, "B3 tick (key): the key back on latches a new window at the new centre, the strip never having left black");
        TickSim w;
        w.run(14);
        w.in.rendered = false;   // a weapon switch to a sight that is not a rendered entry
        w.tick();
        check(!w.out.latched && !w.out.asked && !w.last.zoom, "B3 tick (weapon): a switch to an entry that is not rendered ends the window at once");
        w.run(6);
        check(!w.loop.latched && w.last.strip < 0.999f && w.last.strip > 0.0f, "B3 tick (weapon): and the strip comes back");
        w.in.rendered = true;
        w.in.tcNow[0] = 0.0500;
        w.tick();
        check(w.out.latched && w.loop.tc[0] == 0.0500, "B3 tick (weapon): the switch back latches the window at its new centre");
    }
    // the lens not qualifying, the left rows missing, the gate never met
    {
        TickSim t;
        t.in.leftOk = false;
        t.run(6);
        check(!t.out.latched && t.noLeftRows == 6u && t.askedTicks == 6u && t.heldTicks == 0u && !t.out.frameAsked, "B3 tick (rows): without the left eye's rows the window never starts, and every such tick is counted");
        TickSim q;
        q.in.qualifies = false;
        q.run(4);
        check(!q.out.latched && q.askedTicks == 0u && q.noLeftRows == 0u && q.heldTicks == 0u, "B3 tick (lens): a lens that does not qualify never starts the window, and is not counted as asked");
        TickSim h;
        h.run(14);
        h.in.qualifies = false;   // S went stale mid-press: a held window needs neither the lens's S and c nor the left rows
        h.in.leftOk = false;
        h.tick();
        check(h.out.latched && h.last.zoom && !h.out.frameAsked, "B3 tick (lens): the window holds without S or the left rows, but the frame is not one the lens asks for");
        TickSim m;
        m.in.shareMode = false;
        m.run(4);
        check(!m.out.latched && m.askedTicks == 0u, "B3 tick (mode): a sight eye that does not share the right eye's picture never starts it");
    }
    // the frame record's asked (the doors count a frame as not zoomed only once a window began)
    {
        TickSim t;
        t.in.share = 0.95;
        t.tick();
        check(t.out.asked && t.out.askedTick && !t.out.latched && !t.out.frameAsked, "B3 tick (record): asked but no window yet: the frame is not asked of the doors (they count it as not asked, not as a fall-back)");
        t.in.share = 1.0;
        t.tick();
        check(t.out.frameAsked, "B3 tick (record): the frame is asked from the tick the window latches");
        t.in.qualifies = false;
        t.tick();
        check(t.out.latched && !t.out.frameAsked, "B3 tick (record): and not in a frame the lens does not qualify for");
    }
    // a lost frame: ticks stop with the zoom asked. The runtime reads the request for 200 ms, then none; the strip decays after
    {
        TickSim t;
        t.run(14);
        sw::clear();
        sw::publish(t.last, 5000);
        check(sw::read(5100).zoom && sw::read(5200).zoom, "B3 tick (lost frames): the zoom stands for 200 ms of silence");
        check(!sw::read(5250).zoom && sw::read(5250).strip > 0.7f && !sw::read(5300).zoom && sw::read(5300).strip > 0.4f && sw::read(5300).strip < 0.6f && sw::read(5500).strip == 0.0f,
              "B3 tick (lost frames): then none at once, and the strip decays to nothing over another 200 ms, so a stalled frame thread cannot leave the left eye zoomed");
        t.tick();
        sw::publish(t.last, 5600);
        check(sw::read(5610).zoom, "B3 tick (lost frames): a tick after the stall is read again");
        sw::clear();   // one tick lost between two: the request stands (the runtime reads the last one)
        sw::publish(t.last, 6000);
        t.tick();
        t.tick();   // never published
        check(sw::read(6022).zoom && t.loop.latched, "B3 tick (lost frames): a tick or two never published leaves the last request standing");
        sw::clear();
    }
}

template <class Check>
inline void runB3(Check&& check) {
    // ---- the table's flag ---------------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(sz::lensRenderedEntry("$wpn_m_sniper_plasma_charged_name;", sz::kLensScopeYes), "B3 table: the scoped Executioner is a rendered entry");
        check(!sz::lensRenderedEntry("$wpn_m_sniper_plasma_charged_name;", sz::kLensScopeNo), "B3 table: the bare Executioner is not (A's digital zoom serves it)");
        check(!sz::lensRenderedEntry("wpn_m_sniper_plasma_charged", sz::kLensScopeUnknown), "B3 table: an unknown scope state is not a rendered entry");
        check(!sz::lensRenderedEntry("wpn_m_assaultrifle_plasma_fauto", sz::kLensScopeYes) && !sz::lensRenderedEntry("wpn_m_assaultrifle_laser_fauto", sz::kLensScopeYes) &&
                  !sz::lensRenderedEntry("wpn_m_submachinegun_laser_fauto", sz::kLensScopeYes) && !sz::lensRenderedEntry("wpn_m_sniper_kinetic_heavy", sz::kLensScopeYes) &&
                  !sz::lensRenderedEntry(nullptr, sz::kLensScopeYes) && !sz::lensRenderedEntry("", sz::kLensScopeYes),
              "B3 table: no other row is rendered, and no name is not");
        uint32_t rendered = 0;
        for (const sz::LensEntry& e : sz::kLensTable) rendered += e.renderedScoped ? 1u : 0u;
        check(rendered == 1u && sz::lensTableEntries() == 7u, "B3 table: exactly one row is marked rendered, and the table still holds its seven entries");
    }
    // ---- the window ---------------------------------------------------------------------------------------------------------------------------------------------------------
    const double trueL[4] = {-0.80, 0.90, -0.80, 0.90};   // the left eye's own tangents, an asymmetric pair
    const double tcFar[2] = {6.3e-4, 0.0};                // its direction to the far point c
    double win[4];
    {
        check(sz::b3Window(trueL, tcFar, 4.067, win), "B3 window: a window for the scoped Executioner's M");
        check(std::fabs((win[0] + win[1]) * 0.5 - tcFar[0]) < 1.0e-12 && std::fabs((win[2] + win[3]) * 0.5 - tcFar[1]) < 1.0e-12, "B3 window: centred on the direction to c");
        check(std::fabs(sz::b3Narrowing(trueL, win, 0) - 4.067) < 1.0e-9 && std::fabs(sz::b3Narrowing(trueL, win, 1) - 4.067) < 1.0e-9, "B3 window: its span is the eye's own divided by M, on both axes");
        check(win[0] >= trueL[0] && win[1] <= trueL[1] && win[2] >= trueL[2] && win[3] <= trueL[3], "B3 window: inside the eye's own span");
        double edge[4];
        const double tcEdge[2] = {0.88, -0.78};   // near the right and bottom edges: the window is moved in, not cut
        check(sz::b3Window(trueL, tcEdge, 4.0, edge) && edge[1] <= trueL[1] + 1e-12 && edge[2] >= trueL[2] - 1e-12 && std::fabs((edge[1] - edge[0]) - (trueL[1] - trueL[0]) / 4.0) < 1e-12,
              "B3 window: near the span's edge the window moves in and keeps its size");
        double bad[4];
        const double outside[2] = {1.0, 0.0}, nan2[2] = {std::nan(""), 0.0};
        check(!sz::b3Window(trueL, outside, 4.0, bad) && !sz::b3Window(trueL, nan2, 4.0, bad) && !sz::b3Window(trueL, tcFar, 1.04, bad) && !sz::b3Window(trueL, tcFar, 8.5, bad) &&
                  !sz::b3Window(trueL, tcFar, std::nan(""), bad),
              "B3 window: a centre outside the span, a centre that is no number, an M under 1.05 or over 8 and an M that is no number make none");
        const double flat[4] = {0.1, 0.1, -0.5, 0.5};
        check(!sz::b3Window(flat, tcFar, 4.0, bad), "B3 window: an empty span makes none");
        check(sz::b3WindowMatches(win, win, 1e-6) && !sz::b3WindowMatches(trueL, win, 3e-3), "B3 window: the rows' window matches the told one, and the unzoomed eye's does not");
        double closeW[4] = {win[0] * 1.001, win[1] * 1.001, win[2] * 1.001, win[3] * 1.001};
        check(sz::b3WindowMatches(win, closeW, 3e-3), "B3 window: a part in a thousand of jitter-removal error still matches");
        double a2[4], b2[4];
        const double tcm[2] = {0.01, -0.02};
        check(sz::b3Window(trueL, tcm, 3.0, a2) && sw::window(trueL, tcm, 3.0, b2) && std::memcmp(a2, b2, sizeof(a2)) == 0, "B3 window: the frame thread's and the runtime's window are the one function");
    }
    // ---- the copy window of the left picture ---------------------------------------------------------------------------------------------------------------------------------
    {
        int rect[4];
        check(sz::b3CopyRect(win, tcFar, 4032, 4032, rect), "B3 copy: a window of the left picture for the lens to read");
        const double cxPx = (tcFar[0] - win[0]) / (win[1] - win[0]) * 4032.0, cyPx = (win[3] - tcFar[1]) / (win[3] - win[2]) * 4032.0;
        check(rect[0] < cxPx && rect[2] > cxPx && rect[1] < cyPx && rect[3] > cyPx && rect[0] >= 0 && rect[1] >= 0 && rect[2] <= 4032 && rect[3] <= 4032, "B3 copy: about c's place in the picture and inside it");
        const double wantHalf = std::tan(sz::kB3CopyHalfDeg / sz::kDeg) / (win[1] - win[0]) * 4032.0;
        check(std::fabs((rect[2] - rect[0]) - 2.0 * wantHalf) <= 2.0, "B3 copy: kB3CopyHalfDeg about the centre in the zoomed tangent plane");
        check(wantHalf * 2.0 * wantHalf * 2.0 * 4.0 < 40.0e6, "B3 copy: the copy is a few megabytes, not the picture");
        const double corner[2] = {win[0] + 0.001, win[3] - 0.001};
        check(sz::b3CopyRect(win, corner, 4032, 4032, rect) && rect[0] == 0 && rect[1] == 0, "B3 copy: cut to the picture at its corner");
        check(!sz::b3CopyRect(win, tcFar, 0, 4032, rect) && !sz::b3CopyRect(win, corner, 4032, 0, rect), "B3 copy: an empty picture makes none");
        const double flat[4] = {0.1, 0.1, -0.5, 0.5};
        check(!sz::b3CopyRect(flat, tcFar, 4032, 4032, rect), "B3 copy: an empty span makes none");
    }
    // ---- the reprojection -----------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const double eyeR[3] = {0.0315, 0.0, 0.0}, eyeL[3] = {-0.0315, 0.0, 0.0};
        const double trueR[4] = {-0.90, 0.80, -0.80, 0.90};
        const double n = 0.1;
        const Cam camR = makeCam(eyeR, trueR, n), camL = makeCam(eyeL, win, n);
        float T[16], u[4];
        const double zero2[2] = {0.0, 0.0};
        check(sz::b3Matrices(camR.rows, camR.o, zero2, camL.rows, camL.o, zero2, T, u), "B3 reproject: the matrices of two eyes");
        // rays of the right eye through several pixels, each met at several depths (the sky as depth 0)
        const double ndcQ[4][2] = {{0.0, 0.0}, {0.05, -0.03}, {-0.08, 0.06}, {0.12, 0.10}};
        const double depthsM[6] = {0.7, 1.0, 3.0, 10.0, 100.0, 0.0};
        double worstPlain = 0.0, worstNaive = 1.0e9, naiveAt3 = 0.0, worstSky = 0.0;
        bool allOk = true, anyFront = false;
        for (const auto& q : ndcQ) {
            for (double z : depthsM) {
                const double tx = (q[0] * (trueR[1] - trueR[0]) + (trueR[1] + trueR[0])) * 0.5, ty = (q[1] * (trueR[3] - trueR[2]) + (trueR[3] + trueR[2])) * 0.5;
                const double dist = z > 0.0 ? z : 1.0e5;   // a sky pixel is read as a point far away along its ray
                const double P[3] = {eyeR[0] + dist * tx, eyeR[1] + dist * ty, eyeR[2] + dist};
                const double depth = z > 0.0 ? n / z : 0.0;
                const double nearDepth = n / 0.6;
                double ndcL[2], want[2];
                const bool got = sz::b3Reproject(T, u, q[0], q[1], depth > sz::b3SkyDepth(nearDepth) ? depth : sz::b3SkyDepth(nearDepth), ndcL);
                const bool direct = projectCam(camL, P, want, nullptr);
                if (!got || !direct) { allOk = false; continue; }
                anyFront = true;
                const double err = std::hypot(ndcL[0] - want[0], ndcL[1] - want[1]);
                if (z > 0.0) worstPlain = std::fmax(worstPlain, err); else worstSky = std::fmax(worstSky, err);
                // the same direction taken straight through the left eye's window, with no parallax removed
                const double naive[2] = {(tx - (win[0] + win[1]) * 0.5) / ((win[1] - win[0]) * 0.5), (ty - (win[2] + win[3]) * 0.5) / ((win[3] - win[2]) * 0.5)};
                const double nerr = std::hypot(naive[0] - want[0], naive[1] - want[1]);
                if (z == 3.0 && q[0] == 0.0) naiveAt3 = nerr;
                if (z > 0.0 && z <= 10.0) worstNaive = std::fmin(worstNaive, nerr);
            }
        }
        check(allOk && anyFront && worstPlain < 2.0e-5, "B3 reproject: a world point from the right eye's depth lands where the left eye draws it, at every depth from 0.7 m to 100 m and in several directions");
        check(worstSky < 2.0e-5, "B3 reproject: a sky pixel (no depth) is read as a point far away, and its direction alone decides");
        check(naiveAt3 > 0.05 && worstNaive > 0.01, "B3 reproject: the same direction sampled straight is off by the parallax (more than 0.05 of the window's half span at 3 m), which the reprojection removes");
        // the parallax instrument: the target at 3 m against the far point, in the left picture's pixels
        const double cNdc[2] = {0.0, 0.0};
        const double par = sz::b3ParallaxPx(T, u, cNdc, n / 3.0, n / 0.6, 4032, 4032);
        const double expectPx = 0.0315 * 2.0 / 3.0 / ((win[1] - win[0])) * 4032.0;   // 6.3 cm over 3 m, in tangent units of the window, in pixels
        check(std::fabs(par - expectPx) < 0.02 * expectPx && par > 100.0, "B3 reproject: the parallax at c is 6.3 cm over the distance, in the left picture's pixels (3 m: a few hundred)");
        const double parFar = sz::b3ParallaxPx(T, u, cNdc, 0.0, n / 0.6, 4032, 4032);
        check(std::isfinite(parFar) && parFar < 0.01, "B3 reproject: the parallax of a sky pixel is nothing");
        // jitter: the rows were drawn with the TAA jitter, and the matrices made without it are the same
        const Cam camRj = makeCam(eyeR, trueR, n, 0.0007, -0.0004), camLj = makeCam(eyeL, win, n, -0.0009, 0.0006);
        const double jR[2] = {0.0007, -0.0004}, jL[2] = {-0.0009, 0.0006};
        float T2[16], u2[4];
        bool same = sz::b3Matrices(camRj.rows, camRj.o, jR, camLj.rows, camLj.o, jL, T2, u2);
        for (int i = 0; i < 16; ++i) same = same && std::fabs(T[i] - T2[i]) <= 2.0e-4 * (1.0 + std::fabs(T[i]));
        for (int i = 0; i < 4; ++i) same = same && std::fabs(u[i] - u2[i]) <= 2.0e-4 * (1.0 + std::fabs(u[i]));
        check(same, "B3 reproject: matrices made from jittered rows and the jitter are the matrices of the unjittered rows");
        {   // the frame thread's step: the left rows' tangents with the jitter taken out are the window the runtime told the game (and with it left in, they are off by the jitter)
            sz::ClipZoom zl = sz::clipZoom(camLj.rows), zj = zl;
            float tanU[4], tanJ[4];
            zl.offX -= jL[0];
            zl.offY -= jL[1];
            const bool got = zl.ok && sz::tangentsOf(zl, tanU) && sz::tangentsOf(zj, tanJ);
            const double lu[4] = {tanU[0], tanU[1], tanU[2], tanU[3]}, lj[4] = {tanJ[0], tanJ[1], tanJ[2], tanJ[3]};
            check(got && sz::b3WindowMatches(win, lu, 1.0e-4) && !sz::b3WindowMatches(win, lj, 1.0e-4) && sz::b3WindowMatches(win, lj, 3.0e-3),
                  "B3 reproject: the left rows' tangents with the jitter taken out are the told window (to a part in ten thousand), with it left in they are off by the jitter, within the match's tolerance");
        }
        float T3[16], u3[4];
        check(sz::b3Matrices(camRj.rows, camRj.o, zero2, camLj.rows, camLj.o, zero2, T3, u3) && std::fabs(T3[12] - T[12]) + std::fabs(T3[0] - T[0]) > 1.0e-5,
              "B3 reproject: control: the jitter left in is a different matrix");
        // behind the left eye
        const double eyeFar[3] = {-0.0315, 0.0, 2.0};
        const Cam camBehind = makeCam(eyeFar, win, n);
        float Tb[16], ub[4];
        double ndcB[2];
        check(sz::b3Matrices(camR.rows, camR.o, zero2, camBehind.rows, camBehind.o, zero2, Tb, ub) && !sz::b3Reproject(Tb, ub, 0.0, 0.0, n / 1.0, ndcB), "B3 reproject: a point behind the left eye is refused");
        check(std::isnan(sz::b3ParallaxPx(Tb, ub, cNdc, n / 1.0, n / 0.6, 4032, 4032)), "B3 reproject: and its parallax is no number");
        // matrices that cannot be had
        float Tz[16], uz[4];
        Cam camZero = camR;
        for (float& f : camZero.rows) f = 0.0f;
        check(!sz::b3Matrices(camZero.rows, camZero.o, zero2, camL.rows, camL.o, zero2, Tz, uz), "B3 reproject: a singular right eye makes none");
        double inv[16], id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, bad[16] = {};
        check(sz::b3Invert4(id, inv) && inv[0] == 1.0 && inv[5] == 1.0 && inv[15] == 1.0 && !sz::b3Invert4(bad, inv), "B3 reproject: the inverse of the identity is the identity, of zeros none");
        // the left picture's pixel
        double px[2];
        const double centre[2] = {0.0, 0.0};
        sz::b3LeftPx(centre, 4032, 2016, px);
        check(px[0] == 2016.0 && px[1] == 1008.0, "B3 reproject: the NDC square's centre is the left picture's centre, y up");
    }
    // ---- the gate and the hold -----------------------------------------------------------------------------------------------------------------------------------------------
    {
        sz::B3GateIn g;
        g.keyOn = g.qualifies = g.rendered = g.shareMode = g.leftRowsOk = g.sightsFull = true;
        g.share = 1.0;
        check(sz::b3Gate(g).latchNow && !sz::b3Gate(g).hold, "B3 gate: everything up and not latched yet: the window starts");
        g.latched = true;
        check(!sz::b3Gate(g).latchNow && sz::b3Gate(g).hold, "B3 gate: latched, it is held and does not start again");
        sz::B3GateIn h = g;
        h.qualifies = false;
        h.leftRowsOk = false;
        h.share = 0.95;
        check(sz::b3Gate(h).hold, "B3 gate: a held window needs neither the lens's S and c nor the left rows, and holds at a share of 0.95");
        h.share = 0.85;
        check(!sz::b3Gate(h).hold, "B3 gate: a share under 0.9 ends the hold (the release has begun while the cover still stands)");
        sz::B3GateIn s = g;
        s.latched = false;
        s.share = 0.95;
        check(!sz::b3Gate(s).latchNow, "B3 gate: it starts only at full weight of the share");
        s = g;
        s.latched = false;
        s.leftRowsOk = false;
        check(!sz::b3Gate(s).latchNow, "B3 gate: it does not start without the left eye's rows");
        s.leftRowsOk = true;
        s.qualifies = false;
        check(!sz::b3Gate(s).latchNow, "B3 gate: nor when the lens does not qualify");
        for (int bit = 0; bit < 3; ++bit) {
            sz::B3GateIn k = g;
            if (bit == 0) k.keyOn = false;
            if (bit == 1) k.rendered = false;
            if (bit == 2) k.shareMode = false;
            check(!sz::b3Gate(k).hold && !sz::b3Gate([&] { sz::B3GateIn j = k; j.latched = false; return j; }()).latchNow,
                  bit == 0 ? "B3 gate: the key off holds nothing and starts nothing" : bit == 1 ? "B3 gate: an entry that is not rendered holds nothing and starts nothing"
                                                                                                : "B3 gate: a sight eye that does not share the right eye's picture holds nothing and starts nothing");
        }
        sz::B3GateIn k = g;
        k.sightsFull = false;
        check(!sz::b3Gate(k).hold, "B3 gate: the sights down end the hold");
    }
    // ---- the strip's ramp and the zoom's request ---------------------------------------------------------------------------------------------------------------------------------
    {
        sz::B3Strip st;
        int requestAt = -1;
        for (int i = 0; i < 20; ++i) {
            sz::b3StripStep(st, true, 0.01);
            if (st.request && requestAt < 0) requestAt = i;
        }
        check(requestAt >= 9 && requestAt <= 10 && st.fade == 1.0 && st.request, "B3 strip: it goes black over 0.1 s, and the zoom is asked for only once it is");
        sz::B3Strip st2;
        sz::b3StripStep(st2, true, 0.02);
        check(st2.fade > 0.15 && st2.fade < 0.25 && !st2.request, "B3 strip: a fifth of the way after 0.02 s, and no zoom yet");
        sz::b3StripStep(st, false, 0.01);
        check(!st.request && st.fade == 1.0, "B3 strip: the zoom's request ends at once with the window");
        for (int i = 0; i < 2; ++i) sz::b3StripStep(st, false, 0.01);
        check(st.fade == 1.0 && st.offTicks == 3, "B3 strip: it stays black for three ticks (the runtime's answer lags the ask by a frame or two)");
        sz::b3StripStep(st, false, 0.01);
        check(st.fade < 1.0 && st.fade > 0.85, "B3 strip: then it comes back");
        for (int i = 0; i < 20; ++i) sz::b3StripStep(st, false, 0.01);
        check(st.fade == 0.0, "B3 strip: and is gone after another tenth of a second");
        sz::B3Strip st3;
        sz::b3StripStep(st3, true, std::nan(""));
        sz::b3StripStep(st3, true, -1.0);
        check(st3.fade == 0.0 && !st3.request, "B3 strip: a tick time that is no number or negative moves nothing");
        check(!sz::b3ResetEdge(false, false) && !sz::b3ResetEdge(true, true) && sz::b3ResetEdge(false, true) && sz::b3ResetEdge(true, false),
              "B3 reset: the left eye's history ends at both edges of the window (the right eye's is never asked: it is never zoomed)");
    }
    // ---- the lens's source and why it fell back --------------------------------------------------------------------------------------------------------------------------------------
    {
        using S = sz::B3Source;
        sz::B3SourceIn in;
        check(sz::b3SourceOf(in) == S::kNotAsked, "B3 source: not asked is the plain lens, with nothing to explain");
        in.asked = true;
        check(sz::b3SourceOf(in) == S::kNotZoomed, "B3 source: asked and the left eye not zoomed in this frame falls back to A");
        in.zoomedFrame = true;
        check(sz::b3SourceOf(in) == S::kNoCopy, "B3 source: zoomed and no copy of the left picture falls back to A");
        in.copyMade = true;
        check(sz::b3SourceOf(in) == S::kStale, "B3 source: a copy from another frame falls back to A (stale)");
        in.copySameFrame = true;
        check(sz::b3SourceOf(in) == S::kNoMatrices, "B3 source: no usable matrices falls back to A");
        in.matricesOk = true;
        check(sz::b3SourceOf(in) == S::kNoShader, "B3 source: the pass or its resources not made falls back to A");
        in.shaderOk = true;
        check(sz::b3SourceOf(in) == S::kZoomed, "B3 source: everything in hand, the lens takes the left eye's zoomed picture");
        sz::B3SourceIn stale = in;
        stale.zoomedFrame = false;
        stale.copyMade = stale.copySameFrame = stale.matricesOk = stale.shaderOk = false;
        stale.asked = false;
        check(sz::b3SourceOf(stale) == S::kNotAsked, "B3 source: a frame not asked for is not counted as a fall-back whatever else is missing");
        check(sz::kB3Sources == 7u, "B3 source: seven buckets");
    }
    // ---- the light grid cache stands aside for the zoomed eye ---------------------------------------------------------------------------------------------------------------------------------------
    {
        check(sz::b3LightGridBypass(true, 0, 2016, 2016) && !sz::b3LightGridBypass(true, 1, 2016, 2016) && !sz::b3LightGridBypass(false, 0, 2016, 2016) && !sz::b3LightGridBypass(true, 0, 256, 256) &&
                  !sz::b3LightGridBypass(true, 0, 2016, 512),
              "B3 light grid: the first eye-sized call of a frame in which eye 0 is zoomed is bypassed, no other call is");
        float a[116], b[116];
        for (int i = 0; i < 116; ++i) a[i] = b[i] = static_cast<float>(i) * 0.5f;
        check(sz::b3RowsChanged(a, b) == 0u, "B3 shadow rows: equal blocks differ in no row");
        b[0] += 1.0f;
        b[7] += 1.0f;
        b[115] = -1.0f;
        check(sz::b3RowsChanged(a, b) == 3u, "B3 shadow rows: a change in a row counts the row once, wherever in it, and the last row counts");
    }
    // ---- the readout pin takes the right eye alone while the left is zoomed ---------------------------------------------------------------------------------------------------------------
    {
        namespace f85 = edvr::nofoot::f85;
        f85::CamK k;
        k.ok = true;
        for (int i = 0; i < 3; ++i) k.K3[i][i] = 1.0;   // a camera that is the identity: the two eyes' estimates agree when they are the same
        const double oh[3] = {1.0, 2.0, 3.0}, ow[3] = {4.0, 5.0, 6.0};
        f85::KLatch latch;
        check(!latch.usable(20), "B3 pin: no latch before any eye is in");
        check(latch.putSolo(1, 20, k, oh, ow) && latch.agreed && latch.usable(20) && latch.usable(20 + f85::kLatchHoldFrames) && !latch.usable(20 + f85::kLatchHoldFrames + 1u) && latch.solo == 1u,
              "B3 pin: the right eye alone vouches for its frame, the hold runs from it, and it is counted");
        check(latch.pairs == 0u && latch.disagreed == 0u, "B3 pin: and it is no pairing and no disagreement");
        f85::CamK bad;
        check(!latch.putSolo(1, 30, bad, oh, ow) && !latch.usable(30), "B3 pin: an estimate that is not one vouches for nothing");
        check(!latch.putSolo(2, 30, k, oh, ow), "B3 pin: an eye that is neither is refused");
        // the window ends: the eyes pair again as before
        f85::KLatch pair;
        pair.put(0, 40, k, oh, ow);
        check(pair.put(1, 40, k, oh, ow) && pair.agreed && pair.usable(40), "B3 pin: with the window gone the two eyes pair and compare as they did");
        // the decision f85LatchPut makes per estimate (scope_zoom_b3.h b3PinAction), driven over frames: the eyes pair until eye 0 is the zoomed view, then eye 1 vouches alone
        using PA = sz::B3PinAction;
        check(sz::b3PinAction(false, 0) == PA::kPair && sz::b3PinAction(false, 1) == PA::kPair && sz::b3PinAction(true, 1) == PA::kSolo && sz::b3PinAction(true, 0) == PA::kSkip &&
                  sz::b3PinAction(true, 2) == PA::kSkip,
              "B3 pin: both eyes pair unless eye 0 is zoomed; then eye 1 vouches alone and eye 0 is not put");
        f85::KLatch drive;
        uint32_t soloCount = 0;
        const auto put = [&](bool zoomed, int e, uint32_t frame) {
            const PA a = sz::b3PinAction(zoomed, e);
            if (a == PA::kSolo) {
                if (drive.putSolo(1, frame, k, oh, ow)) ++soloCount;
            } else if (a == PA::kPair) {
                drive.put(e, frame, k, oh, ow);
            }
        };
        put(false, 0, 10);
        put(false, 1, 10);
        check(drive.pairs == 1u && drive.usable(10) && soloCount == 0u, "B3 pin: frames before the window pair");
        put(true, 0, 11);
        put(true, 1, 11);
        check(drive.pairs == 1u && drive.solo == 1u && soloCount == 1u && drive.usable(11) && drive.pairedFrame == 11u && drive.disagreed == 0u,
              "B3 pin: a frame with eye 0 zoomed is vouched for by eye 1 alone, counted, and disagrees with nothing");
        put(true, 1, 12);
        check(drive.usable(12) && drive.solo == 2u, "B3 pin: and the window's later frames hold the latch the same way");
        put(false, 0, 13);
        put(false, 1, 13);
        check(drive.pairs == 2u && drive.pairedFrame == 13u && drive.usable(13), "B3 pin: the frame after the window pairs again");
    }
    // ---- the window between the halves ---------------------------------------------------------------------------------------------------------------------------------------------------
    {
        sw::Request r;
        r.zoom = true;
        r.m = 4.067f;
        r.tc[0] = 6.3e-4f;
        r.tc[1] = -0.02f;
        r.strip = 1.0f;
        uint32_t w[sw::kWords];
        sw::toWords(r, w);
        const sw::Request back = sw::fromWords(w);
        check(back.zoom && back.m == r.m && back.tc[0] == r.tc[0] && back.tc[1] == r.tc[1] && back.strip == 1.0f, "B3 wire: the window crosses as five words and comes back whole");
        sw::Request none;
        uint32_t z[sw::kWords] = {1, 2, 3, 4, 5};
        sw::toWords(none, z);
        bool zeros = true;
        for (uint32_t v : z) zeros = zeros && v == 0;
        check(zeros && !sw::fromWords(z).zoom && sw::fromWords(z).strip == 0.0f, "B3 wire: no zoom and no strip is five zeros, which read as none");
        sw::Request stripOnly;
        stripOnly.strip = 0.5f;
        sw::toWords(stripOnly, z);
        check(!sw::fromWords(z).zoom && sw::fromWords(z).strip == 0.5f, "B3 wire: a strip with no zoom crosses alone");
        uint32_t noMagic[sw::kWords];
        std::memcpy(noMagic, w, sizeof(w));
        noMagic[0] &= 0x0000FFFFu;
        check(!sw::fromWords(noMagic).zoom && sw::fromWords(noMagic).strip == 0.0f, "B3 wire: words without the magic are none");
        sw::Request outOfRange = r;
        outOfRange.m = 9.0f;
        sw::toWords(outOfRange, z);
        check(!sw::fromWords(z).zoom, "B3 wire: a narrowing out of range is none");
        outOfRange = r;
        outOfRange.tc[0] = std::nanf("");
        sw::toWords(outOfRange, z);
        check(!sw::fromWords(z).zoom, "B3 wire: a centre that is no number is none");
        uint32_t forged[sw::kWords];
        std::memcpy(forged, w, sizeof(w));
        forged[1] = sw::bitsOf(0.5f);
        check(!sw::fromWords(forged).zoom, "B3 wire: a narrowing under 1 on the wire is none");
        forged[1] = w[1];
        forged[4] = sw::bitsOf(7.0f);
        check(sw::fromWords(forged).strip == 1.0f, "B3 wire: a strip over 1 reads 1");
        // stale: the d3d11 half stopped publishing
        sw::clear();
        check(!sw::read(1000).zoom, "B3 wire: nothing published reads none");
        sw::publish(r, 1000);
        check(sw::read(1100).zoom && sw::read(1200).zoom && !sw::read(1201).zoom && sw::read(1201).strip > 0.9f, "B3 wire: the zoom stands for 200 ms of silence and then ends at once, the strip decaying");
        check(sw::read(1400).strip == 0.0f && sw::read(1300).strip > 0.4f && sw::read(1300).strip < 0.6f, "B3 wire: the strip decays to nothing over another 200 ms");
        sw::clear();
        check(!sw::read(1100).zoom, "B3 wire: forgotten reads none");
        EdvrNativeScopeZoomReport rep{};
        rep.size = sizeof(rep);
        rep.version = EDVR_NATIVE_SCOPE_ZOOM_REPORT_VERSION_1;
        check(sw::reportReadable(&rep), "B3 report: a report the d3d11 half can read");
        EdvrNativeScopeZoomReport cut = rep, later = rep, nan = rep;
        cut.size = 8;
        later.version = 2;
        nan.rawSpan = std::nanf("");
        check(!sw::reportReadable(&cut) && !sw::reportReadable(&later) && !sw::reportReadable(&nan) && !sw::reportReadable(nullptr), "B3 report: a report cut short, from a later version, with a number that is none, or absent is not");
    }
    // ---- the lens's reach outside the copy ---------------------------------------------------------------------------------------------------------------------------------------------------
    {
        // the lens's reach outside the window copy. The copy: 6 degrees about the window's centre in the zoomed picture, here the tangent range tc +- tan(6 deg) less the filter's margin.
        const double zoomTan[4] = {-0.20, 0.22, -0.20, 0.22};
        int rect[4];
        const double tc[2] = {0.01, 0.01};
        check(sz::b3CopyRect(zoomTan, tc, 4032, 4032, rect), "B3 outside: the copy for the estimate");
        double lo[2], hi[2];
        check(sz::b3CopyTangents(zoomTan, rect, 4032, 4032, lo, hi), "B3 outside: the copy's tangents");
        const double half = std::tan(sz::kB3CopyHalfDeg / sz::kDeg);
        check(std::fabs((lo[0] + hi[0]) * 0.5 - tc[0]) < 1.0e-3 && std::fabs((hi[0] - lo[0]) * 0.5 - half) < 2.0e-3 && std::fabs((lo[1] + hi[1]) * 0.5 - tc[1]) < 1.0e-3,
              "B3 outside: they are the window's centre and kB3CopyHalfDeg about it, less the margin");
        const int bad[4] = {10, 10, 10, 20};
        double blo[2], bhi[2];
        check(!sz::b3CopyTangents(zoomTan, bad, 4032, 4032, blo, bhi) && !sz::b3CopyTangents(zoomTan, rect, 0, 4032, blo, bhi), "B3 outside: an empty rect or picture makes none");
        float ray[sz::kLensRays];
        for (float& v : ray) v = 3.0f;   // a lens 3 degrees in radius
        double frac = -1.0;
        const double centred[2] = {0.01, 0.01};
        check(sz::b3OutsideFraction(ray, 4.0, centred, lo, hi, &frac) && frac == 0.0, "B3 outside: a lens 3 degrees across at M 4, its source centred in the copy, reads wholly inside it");
        const double drift4[2] = {0.01 + std::tan(4.0 / sz::kDeg), 0.01};   // c has drifted 4 degrees off the window: the source of the lens (0.75 degree) reaches 4.75 degrees
        check(sz::b3OutsideFraction(ray, 4.0, drift4, lo, hi, &frac) && frac == 0.0, "B3 outside: 4 degrees of drift still leaves the 0.75 degree source inside the 6 degree copy");
        const double drift6[2] = {0.01 + std::tan(6.0 / sz::kDeg), 0.01};
        check(sz::b3OutsideFraction(ray, 4.0, drift6, lo, hi, &frac) && frac > 0.3 && frac < 0.7, "B3 outside: at the copy's edge about half the lens reads outside it");
        const double drift20[2] = {0.01 + std::tan(20.0 / sz::kDeg), 0.01};
        check(sz::b3OutsideFraction(ray, 4.0, drift20, lo, hi, &frac) && frac == 1.0, "B3 outside: far past the copy all of it does");
        const double driftUp[2] = {0.01, 0.01 + std::tan(20.0 / sz::kDeg)};
        check(sz::b3OutsideFraction(ray, 4.0, driftUp, lo, hi, &frac) && frac == 1.0, "B3 outside: and on the other axis");
        float big[sz::kLensRays];
        for (float& v : big) v = 11.0f;   // a lens 11 degrees in radius at M 4: its source is 2.75 degrees
        check(sz::b3OutsideFraction(big, 4.0, centred, lo, hi, &frac) && frac == 0.0, "B3 outside: at M 4 even the largest lens reads inside the copy when c is on the window");
        check(sz::b3OutsideFraction(big, 1.2, centred, lo, hi, &frac) && frac > 0.2 && frac < 0.7, "B3 outside: the easing lens at M 1.2 reads wider than the copy");
        float gap[sz::kLensRays];
        for (float& v : gap) v = 3.0f;
        gap[10] = static_cast<float>(sz::kLensMaxDeg);   // one ray escaped: it takes its neighbours' radius, as the lens does
        double fgap = -1.0, fplain = -1.0;
        check(sz::b3OutsideFraction(gap, 4.0, drift6, lo, hi, &fgap) && sz::b3OutsideFraction(ray, 4.0, drift6, lo, hi, &fplain) && std::fabs(fgap - fplain) < 1.0e-9,
              "B3 outside: an escaped ray takes its neighbours' radius");
        float open[sz::kLensRays];
        for (float& v : open) v = static_cast<float>(sz::kLensMaxDeg);
        const double nanTc[2] = {std::nan(""), 0.0};
        check(!sz::b3OutsideFraction(open, 4.0, centred, lo, hi, &frac) && !sz::b3OutsideFraction(ray, 4.0, nanTc, lo, hi, &frac) && !sz::b3OutsideFraction(ray, 0.5, centred, lo, hi, &frac) &&
                  !sz::b3OutsideFraction(ray, std::nan(""), centred, lo, hi, &frac),
              "B3 outside: an open sight, a centre or an M that is no number, or an M under 1 gives none");
        float skew[sz::kLensRays];
        for (uint32_t k = 0; k < sz::kLensRays; ++k) {
            double dx, dy;
            sz::lensRayDir(k, &dx, &dy);
            skew[k] = dx > 0.0 ? 8.0f : 1.0f;   // a lens that reaches 8 degrees to the right and 1 to the left
        }
        const double edgeRight[2] = {hi[0] - std::tan(1.0 / sz::kDeg) / 4.0 + 0.0, 0.01};
        double fskew = 0.0;
        check(sz::b3OutsideFraction(skew, 4.0, edgeRight, lo, hi, &fskew) && fskew > 0.0 && fskew < 1.0, "B3 outside: the area counted is the lens's own shape, not a disc");
    }
    // ---- the press line ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        sz::B3Press p;
        p.press = 7;
        p.keyOn = true;
        p.askedTicks = 900;
        p.heldTicks = 880;
        p.requestTicks = 860;
        p.noLeftRows = 2;
        p.zoomFrames = 850;
        p.tc[0] = 6.3e-4;
        p.tc[1] = -0.0001;
        p.m = 4.067;
        p.driftN = 800;
        p.driftSumDeg = 8.0;
        p.driftMaxDeg = 0.08;
        p.resets[0] = 2;
        p.resets[1] = 1;
        p.zoomEdgeResets = 2;
        p.source[static_cast<uint32_t>(sz::B3Source::kZoomed)] = 800;
        p.source[static_cast<uint32_t>(sz::B3Source::kNotZoomed)] = 30;
        p.source[static_cast<uint32_t>(sz::B3Source::kNoCopy)] = 1;
        p.copies = 810;
        p.parN = 100;
        p.parSumPx = 12000.0;
        p.parMaxPx = 190.0;
        p.outN = 240;
        p.outFrames = 12;
        p.outSum = 240 * 0.021;
        p.outMax = 0.35;
        p.drawFrames[0] = 850;
        p.drawFrames[1] = 50;
        p.draws[0][0] = 850u * 2100u;
        p.draws[1][0] = 850u * 3900u;
        p.draws[0][1] = 50u * 3800u;
        p.draws[1][1] = 50u * 3900u;
        p.lgCalls = 850;
        p.lgBypass = 840;
        p.shN[0][0] = 800;
        p.shChanged[0][0] = 1600;
        p.shN[0][1] = 40;
        p.shChanged[0][1] = 40;
        p.shN[1][0] = 800;
        p.shChanged[1][0] = 800;
        p.shN[1][1] = 40;
        p.shChanged[1][1] = 20;
        p.shPairN[0] = 800;
        p.shPairDiff[0] = 12000;
        p.ftN[0] = 850;
        p.ftSum[0] = 850 * 11.1;
        p.ftMax[0] = 14.2;
        p.ftN[1] = 50;
        p.ftSum[1] = 50 * 11.2;
        p.ftMax[1] = 12.0;
        p.pinSolo = 20;
        p.pinNoPair = 1;
        char line[sz::kB3PressLineBytes];
        const int len = sz::formatB3Press(line, sizeof(line), p);
        const std::string s(line);
        check(len > 0 && static_cast<size_t>(len) == s.size() && s.rfind("scope zoom (B3): press 7 key on; asked 900, held 880, zoom asked 860 ticks, no left rows 2; zoomed frames 850; tc (0.0006, -0.0001) M 4.067, window drift mean 0.010 max 0.080 deg; "
                                                                      "DLSS resets L 2 R 1 (zoom edge 2); lens source: zoomed 800, A: not asked 0, not zoomed 30, no copy 1, stale 0, no matrices 0, no shader 0; copies 810; "
                                                                      "parallax at c mean 120.0 max 190.0 px; lens outside the copy on 12 of 240 frames, area mean 0.021 max 0.350; ", 0) == 0,
              "B3 line: the first parts, in the order --health reads them");
        check(s.find("draws a frame in/out L 2100/3800 R 3900/3900 (frames 850/50); light grid bypass 840 of 850 calls; shadow rows changed a frame in/out L 2.00/1.00 R 1.00/0.50, L vs R 15.00/none; "
                     "frame ms in/out 11.10/11.20 (max 14.20/12.00); readout pin solo 20, no pair 1") != std::string::npos,
              "B3 line: the per-eye draws, the light grid, the shadow rows, the frame times and the pin");
        sz::B3Press empty;
        char eline[sz::kB3PressLineBytes];
        sz::formatB3Press(eline, sizeof(eline), empty);
        const std::string es(eline);
        check(es.find("tc (none, none) M none, window drift mean none max none deg") != std::string::npos && es.find("parallax at c mean none max none px; lens outside the copy on 0 of 0 frames, area mean none max none; ") != std::string::npos && es.find("key off;") != std::string::npos &&
                  es.find("draws a frame in/out L none/none R none/none (frames 0/0)") != std::string::npos && es.find("frame ms in/out none/none (max none/none)") != std::string::npos,
              "B3 line: a press that measured nothing says none, so a rule can read it");
        // the longest the line can be: every counter six digits (the convention of the S1 line), then ten digits
        sz::B3Press worst;
        worst.press = 999999;
        worst.keyOn = true;
        worst.askedTicks = worst.heldTicks = worst.requestTicks = worst.noLeftRows = worst.zoomFrames = worst.driftN = worst.zoomEdgeResets = 999999;
        worst.resets[0] = worst.resets[1] = worst.copies = worst.parN = worst.lgCalls = worst.lgBypass = worst.pinSolo = worst.pinNoPair = 999999;
        for (uint32_t& v : worst.source) v = 999999;
        worst.tc[0] = worst.tc[1] = -1.6;
        worst.m = 8.0;
        worst.driftSumDeg = 1.0e5;
        worst.driftMaxDeg = 180.0;
        worst.parSumPx = 9.0e9;
        worst.parMaxPx = 99999.9;
        worst.outN = worst.outFrames = 999999;
        worst.outSum = 999999.0;
        worst.outMax = 1.0;
        for (int k = 0; k < 2; ++k) {
            worst.drawFrames[k] = 999999;
            worst.shPairN[k] = 999999;
            worst.shPairDiff[k] = 999999ull * 29ull;
            worst.ftN[k] = 999999;
            worst.ftSum[k] = 9999999.0;
            worst.ftMax[k] = 250000.0;
            for (int e = 0; e < 2; ++e) {
                worst.draws[e][k] = 999999ull * 99999ull;
                worst.shN[e][k] = 999999;
                worst.shChanged[e][k] = 999999ull * 29ull;
            }
        }
        char wline[sz::kB3PressLineBytes];
        const int wlen = sz::formatB3Press(wline, sizeof(wline), worst);
        check(wlen > 0 && static_cast<size_t>(wlen) < sz::kB3PressLineBytes - 1 && wlen <= 1167 && std::strstr(wline, "readout pin solo 999999, no pair 999999") != nullptr,
              "B3 line: the worst case (every counter six digits) keeps its end and fits the log's 1200 bytes with its stamp and marker");
        // counts that could not be taken are n/a, not a wrapped difference
        sz::B3Press unknown = p;
        unknown.temporalKnown = false;
        unknown.lightGridKnown = false;
        char uline[sz::kB3PressLineBytes];
        sz::formatB3Press(uline, sizeof(uline), unknown);
        const std::string us(uline);
        check(us.find("zoomed frames n/a; tc (0.0006, -0.0001) M 4.067") != std::string::npos && us.find("DLSS resets L n/a R n/a (zoom edge n/a); lens source: zoomed 800") != std::string::npos &&
                  us.find("light grid bypass n/a of n/a calls; shadow rows") != std::string::npos && us.find("zoomed frames 850") == std::string::npos && us.find("DLSS resets L 2 R 1") == std::string::npos,
              "B3 line: the temporal pass's counts and the light grid's, when the press could not take them, say n/a and print no figure");
        check(us.find("copies 810; parallax at c mean 120.0") != std::string::npos && us.find("readout pin solo 20, no pair 1") != std::string::npos, "B3 line: ... and every other figure is as it was");
        {
            uint32_t d = 77;
            check(sz::b3Since(true, true, 10, 25, &d) && d == 15u, "B3 since: a difference of two readings of a running total");
            d = 77;
            check(!sz::b3Since(false, true, 10, 25, &d) && d == 77u && !sz::b3Since(true, false, 10, 25, &d) && d == 77u,
                  "B3 since: no baseline at the press's start, or no reading now, is no figure (and the figure is left alone)");
            check(!sz::b3Since(true, true, 25, 10, &d) && d == 77u, "B3 since: a total that went backwards (its owner restarted) is no figure, not a wrapped difference of four billion");
            check(sz::b3Since(true, true, 0, 0x1FFFFFFFFull, &d) && d == 0xFFFFFFFFu, "B3 since: a difference past 32 bits is held at the top, not wrapped");
        }
        sz::B3Press ten = worst;
        ten.press = 4294967295u;
        ten.askedTicks = ten.heldTicks = ten.requestTicks = ten.noLeftRows = ten.zoomFrames = ten.driftN = ten.zoomEdgeResets = 4294967295u;
        ten.resets[0] = ten.resets[1] = ten.copies = ten.parN = ten.lgCalls = ten.lgBypass = ten.pinSolo = ten.pinNoPair = 4294967295u;
        for (uint32_t& v : ten.source) v = 4294967295u;
        char tline[sz::kB3PressLineBytes];
        const int tlen = sz::formatB3Press(tline, sizeof(tline), ten);
        check(tlen > 0 && static_cast<size_t>(tlen) < sz::kB3PressLineBytes - 1 && std::strstr(tline, "readout pin solo 4294967295, no pair 4294967295") != nullptr,
              "B3 line: even with ten-digit counts it keeps its end");
    }

    // ---- the runtime's report line ---------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        EdvrNativeScopeZoomReport r{};
        r.size = sizeof(r);
        r.version = EDVR_NATIVE_SCOPE_ZOOM_REPORT_VERSION_1;
        r.frames = 260;
        r.refused[1] = 3;
        r.stripFrames = 12;
        r.meshWithheld = 260;
        r.seconds = 4.33f;
        r.peakM = 4.067f;
        const float tt[4] = {-0.8f, 0.9f, -0.8f, 0.9f}, zt[4] = {-0.2f, 0.22f, -0.2f, 0.22f};
        for (int i = 0; i < 4; ++i) {
            r.trueTan[i] = tt[i];
            r.zoomTan[i] = zt[i];
        }
        r.rawReads = 260;
        r.matrixReads = 130;
        r.rawSpan = 1.7f;
        r.matrixSpan = 0.42f;
        r.rawCallerRva[0] = 0x4E3C93u;
        r.rawCallerReads[0] = 200u;
        r.rawCallerRva[1] = 0x2842AE3u;
        r.rawCallerReads[1] = 55u;
        r.rawCallerRva[2] = 0xFFFFFFFFu;
        r.rawCallerReads[2] = 3u;
        r.rawCallerOther = 2;
        r.mirrorFrames = 272;
        r.mirrorWidthMax = 0.0834f;
        r.shareNotDrawnPassed = 1;
        check(sw::reportReadable(&r), "B3 runtime line: the report is readable");
        char line[sz::kB3RuntimeLineBytes];
        const int len = sz::formatB3Runtime(line, sizeof(line), r);
        const std::string s(line);
        check(len > 0 && static_cast<size_t>(len) == s.size() &&
                  s.rfind("scope zoom (B3) runtime: zoomed frames 260 over 4.33 s, peak narrowing 4.067; refused: answer not version 11 0, share not at full weight 3, share not drawn 0, geometry 0; "
                          "strip darkened 12 frames, hidden-area mesh withheld 260 frames; ", 0) == 0,
              "B3 runtime line: the frames, the refusals, the strip and the mesh, in the order --health reads them");
        check(s.find("the game's reads of eye 0's projection while it stood: raw getter 260 (its UI scale reads it; vertical tangent span answered 1.7000), matrix getter 130 (the picture is drawn with it; "
                     "span 0.4200), span ratio raw over matrix 4.048; raw getter callers (RVA in the game's executable: reads) 0x4E3C93:200 0x2842AE3:55 outside:3, other callers 2") != std::string::npos,
              "B3 runtime line: what each getter answered (the span), their ratio, and who read the raw getter with the rest counted as other");
        check(s.find("; right-eye mirror strip on 272 frames, widest 0.083 of its width; passed past a share not drawn 1") != std::string::npos &&
                  s.rfind("passed past a share not drawn 1") == s.size() - std::strlen("passed past a share not drawn 1"),
              "B3 runtime line: the right eye's mirror strip (frames drawn with it, the widest share of the eye's width it covered) and the frames passed, at the end");
        EdvrNativeScopeZoomReport ranNot = r;
        ranNot.mirrorFrames = ranNot.shareNotDrawnPassed = 0;
        ranNot.mirrorWidthMax = 0.f;
        char rline[sz::kB3RuntimeLineBytes];
        sz::formatB3Runtime(rline, sizeof(rline), ranNot);
        check(std::strstr(rline, "; right-eye mirror strip on 0 frames, widest none of its width; passed past a share not drawn 0") != nullptr,
              "B3 runtime line: a press where the mirror strip never ran says zero frames and none, so it reads as one that did not run and not as one with no counts");
        EdvrNativeScopeZoomReport nanMirror = r;
        nanMirror.mirrorWidthMax = std::nanf("");
        check(!sw::reportReadable(&nanMirror), "B3 runtime line: a mirror width that is no number is not a readable report");
        check(s.find("deg vertical") == std::string::npos, "B3 runtime line: no angle sum (it is not 1 / M for a window off the axis)");
        EdvrNativeScopeZoomReport none = r;
        none.rawReads = 0;
        none.rawSpan = 0.f;
        none.matrixReads = 0;
        none.matrixSpan = 0.f;
        std::memset(none.rawCallerRva, 0, sizeof(none.rawCallerRva));
        std::memset(none.rawCallerReads, 0, sizeof(none.rawCallerReads));
        none.rawCallerOther = 0;
        char nline[sz::kB3RuntimeLineBytes];
        sz::formatB3Runtime(nline, sizeof(nline), none);
        check(std::strstr(nline, "raw getter 0 (its UI scale reads it; vertical tangent span answered none), matrix getter 0 (the picture is drawn with it; span none), span ratio raw over matrix none; "
                                 "raw getter callers (RVA in the game's executable: reads) none, other callers 0") != nullptr,
              "B3 runtime line: getters nobody read say none for the span, the ratio and the callers, so a rule can tell a getter never read from one read");
        EdvrNativeScopeZoomReport half = r;
        half.matrixSpan = 0.f;
        char hline[sz::kB3RuntimeLineBytes];
        sz::formatB3Runtime(hline, sizeof(hline), half);
        check(std::strstr(hline, "(the picture is drawn with it; span none), span ratio raw over matrix none;") != nullptr, "B3 runtime line: one span missing leaves no ratio");
        // the longest it can be: every count six digits (a press does not last a million frames), four callers of the longest address, a long press, tangents and spans inside 100
        EdvrNativeScopeZoomReport worst = r;
        worst.frames = worst.stripFrames = worst.meshWithheld = worst.rawReads = worst.matrixReads = worst.rawCallerOther = 999999u;
        worst.mirrorFrames = worst.shareNotDrawnPassed = 999999u;
        for (int i = 0; i < 4; ++i) {
            worst.refused[i] = 999999u;
            worst.trueTan[i] = -99.9999f;
            worst.zoomTan[i] = 99.9999f;
            worst.rawCallerRva[i] = 0xFFFFFFFEu;
            worst.rawCallerReads[i] = 999999u;
        }
        worst.seconds = 99999.99f;
        worst.peakM = 8.0f;
        worst.rawSpan = worst.matrixSpan = 99.9999f;
        worst.mirrorWidthMax = 1.0f;
        char wline[sz::kB3RuntimeLineBytes];
        const int wlen = sz::formatB3Runtime(wline, sizeof(wline), worst);
        check(wlen > 0 && static_cast<size_t>(wlen) < sz::kB3RuntimeLineBytes - 1 && wlen <= 1167 && std::strstr(wline, "passed past a share not drawn 999999") != nullptr,
              "B3 runtime line: the worst case (six-digit counts, four callers, the mirror strip's segment) keeps its end and fits the log's 1200 bytes with its stamp and marker");
        EdvrNativeScopeZoomReport tenDigits = worst;
        tenDigits.shareNotDrawnPassed = 4294967295u;
        char tdline[sz::kB3RuntimeLineBytes];
        const int tdlen = sz::formatB3Runtime(tdline, sizeof(tdline), tenDigits);
        check(tdlen > 0 && static_cast<size_t>(tdlen) < sz::kB3RuntimeLineBytes - 1, "B3 runtime line: and a count of ten digits in the last field cannot overrun its buffer");
        EdvrNativeScopeZoomReport nanSpan = r, later = r;
        nanSpan.matrixSpan = std::nanf("");
        later.version = 2;
        check(!sw::reportReadable(&nanSpan) && !sw::reportReadable(&later), "B3 runtime line: a span that is no number, or a later version, is not a report the d3d11 half reads");
    }
    runB3Tick(check);
}

}  // namespace b3test

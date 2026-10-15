// sky_flicker_test: the pure parts of the sky-flicker diagnostic build (docs/sky-flicker-2026-10-13.md) and the sky ring's shader on WARP.
//
//   --dry-run    the same run (the rig never writes a file), for the gate's --dry-run convention
//   --self-test  every check
//
// Cases:
//   E  the camera-row event detector (I-1, src/common/camera_row_events.h) on synthetic row sequences built from the review's numbers: 1.006 s cycle,
//      partner 11 frames later, excursions of 1.35 to 3.04 deg and 0.118 m that return the next frame; a steady turn, a constant-velocity flight,
//      a cut that stays, a gap in the frames, one eye only, the f36358-f36360 three-frame shape
//   C  the pick rules (camera_nearest.h, camera_jump_guard.h) and the eye-target filter (camera_target_tag.h) on the probe-face sequence: bursts of
//      six frames of camera writes, a forward face 2.3 deg from the head picked by the legacy chooser, a face at 7 deg and at 3.4 deg that is not,
//      and a real cut that must still be taken; a model of the chooser's continuity gate around the production filter
//   B  the burst offset (baseline 86 writes, bursts of 6 frames at 103 to 123, 11 frames apart on a 91-frame cycle)
//   M  P3, the mini-render record and its join with the events (src/common/mini_render.h)
//   S  the sky ring: half floats, the entry layout, the ring's slot arithmetic and wrap, the readback decode, the windows around events, the
//      per-second buckets and the "no samples" line; and G, the shipped HLSL on WARP against the CPU reference, including the ring wrap and the
//      asynchronous readback
//   F  the flat scene picker's candidate log (src/d3d11/flat_scene_log.h): the F8 flip's 256x256 versus 1440x810, the picker unchanged, and what the unflown rule WOULD drop
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../../src/common/camera_row_events.h"
#include "../../src/common/camera_nearest.h"
#include "../../src/common/camera_jump_guard.h"
#include "../../src/common/camera_target_tag.h"
#include "../../src/common/mini_render.h"
#include "../../src/common/sky_ring.h"
#include "sky_ring_gpu.h"
#include "sky_ring_shader.h"
#include "flat_scene_log.h"

using Microsoft::WRL::ComPtr;
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
bool approx(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// ---- synthetic rows ---------------------------------------------------------------------------------------------------------------------------------------

// A rigid 3x4: a yaw about +Y by `yawDeg`, translation (tx, ty, tz). Row-major, translation in column 3.
void rowsOf(double yawDeg, double tx, double ty, double tz, float out[12]) {
    const double r = yawDeg * 3.14159265358979323846 / 180.0, c = std::cos(r), s = std::sin(r);
    const float m[12] = {float(c), 0, float(s), float(tx), 0, 1, 0, float(ty), float(-s), 0, float(c), float(tz)};
    std::memcpy(out, m, sizeof(m));
}

// The review's offset of the forward face from the head: (-0.054, -0.094, 0.045) m, 0.1176 m.
constexpr double kFaceT[3] = {-0.054, -0.094, 0.045};
// The review's measured spike rotations, degrees (reviews/skybox-flicker-2026-10-13.md): 2.41, 2.34, 2.29, 2.26, 2.22, 2.17, 2.01, 1.97, 1.59, 1.60, 1.35, 1.36.
constexpr double kSpikeDeg[] = {2.41, 2.34, 2.29, 2.26, 2.22, 2.17, 2.01, 1.97, 1.59, 1.60, 1.35, 1.36};

// A history with both eyes for frames [first, first+n): the head turns turnDegPerFrame; `excursion(frame)` returns the yaw offset in degrees to add and sets
// the translation offset, or 0 for none. Interleaved eye 0, eye 1 per frame, as the dump has them.
template <class Fn>
std::vector<camevents::Row> history(uint32_t first, uint32_t n, double turnDegPerFrame, Fn excursion, double velocityMPerFrame = 0.0, bool bothEyes = true) {
    std::vector<camevents::Row> rows;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t f = first + i;
        double dyaw = 0.0, off[3] = {0, 0, 0};
        const bool hit = excursion(f, &dyaw, off);
        for (uint32_t eye = 0; eye < (bothEyes ? 2u : 1u); ++eye) {
            camevents::Row r;
            r.frame = f;
            r.eye = eye;
            r.qpc = static_cast<int64_t>(f) * 111111;   // 10 MHz counter, 90 Hz frames
            r.valid = true;
            rowsOf(double(i) * turnDegPerFrame + (hit ? dyaw : 0.0), (hit ? off[0] : 0.0), (hit ? off[1] : 0.0),
                   double(i) * velocityMPerFrame + (hit ? off[2] : 0.0), r.rows);
            rows.push_back(r);
        }
    }
    return rows;
}

// ---- E: the event detector ----------------------------------------------------------------------------------------------------------------------------------
void caseEvents() {
    // The 1.006 s cycle: leaders 91 frames apart, a partner 11 frames after each, one-frame excursions of the review's angles.
    const uint32_t leaders[] = {120, 211, 302, 393};
    auto cycle = [&](uint32_t f, double* dyaw, double* off) {
        for (size_t k = 0; k < 4; ++k) {
            for (uint32_t partner = 0; partner < 2; ++partner) {
                if (f == leaders[k] + partner * 11) {
                    *dyaw = kSpikeDeg[(k * 2 + partner) % 12];
                    off[0] = kFaceT[0]; off[1] = kFaceT[1]; off[2] = kFaceT[2];
                    return true;
                }
            }
        }
        return false;
    };
    auto rows = history(100, 400, 0.05, cycle);
    const auto steps = [&] { std::vector<camevents::Step> s(rows.size()); camevents::computeSteps(rows.data(), rows.size(), s.data()); return s; }();
    // The rot/mv columns: an ordinary row steps 0.05 deg and 0 m; an excursion row steps ~2.41 deg and 0.1176 m; the row after steps back.
    size_t at = 0;
    for (size_t i = 0; i < rows.size(); ++i) if (rows[i].frame == 120 && rows[i].eye == 0) at = i;
    check(steps[at].have && steps[at].gap == 1 && approx(steps[at].rotDeg, 2.41 + 0.05, 0.02) && approx(steps[at].mvM, 0.1174, 0.001),
          "E1 the leader's row steps 2.4 deg and 0.117 m from the row before it (the review's spike)");
    check(steps[at + 2].have && approx(steps[at + 2].rotDeg, 2.41 - 0.05, 0.02) && approx(steps[at + 2].mvM, 0.1174, 0.001),
          "E1 ...and the next frame steps back by the same amount");
    check(!steps[0].have && !steps[1].have, "E1 the first row of each eye has no step");
    const auto events = camevents::detectEvents(rows.data(), rows.size());
    check(events.size() == 8, "E2 eight events: four leaders and their partners");
    bool frames = events.size() == 8;
    for (size_t k = 0; frames && k < 4; ++k) frames = events[k * 2].frame == leaders[k] && events[k * 2 + 1].frame == leaders[k] + 11;
    check(frames, "E2 at the leader frames and 11 frames after");
    bool shape = true;
    for (const auto& e : events) shape = shape && e.run == 1 && e.eyeMask == 3u && e.mvM > 0.11f && e.mvM < 0.125f && e.rotDeg > 1.3f && e.rotDeg < 2.5f;
    check(shape, "E2 each a one-frame excursion in both eyes of 1.3 to 2.5 deg and 0.118 m");
    const camevents::Summary sum = camevents::summarize(events, 400, 10000000.0);
    check(sum.events == 8 && sum.leaders == 4 && sum.partners == 4 && sum.byRun[1] == 8 && sum.both == 8, "E3 the summary counts 4 leaders, 4 partners, 8 one-frame events in both eyes");
    check(sum.havePartnerGap && approx(sum.partnerGapFrames, 11.0, 0.01), "E3 the leader-to-partner gap is 11 frames");
    check(sum.havePeriod && approx(sum.periodFrames, 91.0, 0.01) && approx(sum.periodSec, 91.0 / 90.0, 0.002), "E3 the cycle is 91 frames, 1.011 s at the rig's clock");
    char line[900];
    const int len = camevents::formatSummary(line, sizeof(line), sum, events);
    check(len > 0 && std::strstr(line, "camera-row events: 8 of 400 frames") == line && std::strstr(line, "frames: 120 131 211 222 302 313 393 404") &&
          std::strstr(line, "gaps: 11 80 11 80 11 80 11") && std::strstr(line, "leaders=4 partners=4") && std::strstr(line, "partner_gap=11.0 frames") &&
          std::strstr(line, "period=91.0 frames (1.011 s)"),
          "E3 the summary line names the events, the 11/80 gaps, the partner gap and the period");
    check(camevents::eventCovering(events, 120) == 1 && camevents::eventCovering(events, 121) == 0 && camevents::eventCovering(events, 131) == 1, "E3 eventCovering marks the excursion's frame only");

    // A dump with no events says so, distinct from a dump that never ran this code.
    auto quiet = history(100, 200, 0.05, [](uint32_t, double*, double*) { return false; });
    const auto none = camevents::detectEvents(quiet.data(), quiet.size());
    const camevents::Summary zero = camevents::summarize(none, 200, 10000000.0);
    camevents::formatSummary(line, sizeof(line), zero, none);
    check(none.empty() && std::strcmp(line, "camera-row events: 0 of 200 frames") == 0, "E4 no events: \"camera-row events: 0 of N frames\"");

    // Genuine turning, 0.66 deg a frame (the review's f35478-85), no translation: no event.
    auto turn = history(100, 60, 0.66, [](uint32_t, double*, double*) { return false; });
    check(camevents::detectEvents(turn.data(), turn.size()).empty(), "E5 a steady 0.66 deg a frame turn is no event");
    // A flight at 3 m a frame (270 m/s at 90 Hz) is no event; the same flight with the probe face in one frame is.
    auto flight = history(100, 60, 0.05, [](uint32_t, double*, double*) { return false; }, 3.0);
    check(camevents::detectEvents(flight.data(), flight.size()).empty(), "E6 a constant-velocity flight at 3 m a frame is no event");
    auto flightHit = history(100, 60, 0.05, [](uint32_t f, double* dyaw, double* off) { if (f != 130) return false; *dyaw = 2.2; off[0] = kFaceT[0]; off[1] = kFaceT[1]; off[2] = kFaceT[2]; return true; }, 3.0);
    const auto fe = camevents::detectEvents(flightHit.data(), flightHit.size());
    check(fe.size() == 1 && fe[0].frame == 130 && fe[0].run == 1, "E6 ...and the face in one frame of that flight is found at its frame");
    // A cut that stays: the pose jumps 5 m and 30 deg and holds. Not an event.
    auto cut = history(100, 60, 0.05, [](uint32_t f, double* dyaw, double* off) { if (f < 130) return false; *dyaw = 30.0; off[0] = 5.0; off[1] = 0; off[2] = 0; return true; });
    check(camevents::detectEvents(cut.data(), cut.size()).empty(), "E7 a cut that stays is no event");
    // A missing frame inside the bracket: no event across the gap (the history says which frames it has).
    auto gap = history(100, 40, 0.05, [](uint32_t f, double* dyaw, double* off) { if (f != 120) return false; *dyaw = 2.4; off[0] = kFaceT[0]; off[1] = kFaceT[1]; off[2] = kFaceT[2]; return true; });
    std::vector<camevents::Row> gapped;
    for (const auto& r : gap) if (r.frame != 121) gapped.push_back(r);
    check(camevents::detectEvents(gapped.data(), gapped.size()).empty(), "E8 a frame missing after the excursion leaves it unjudged, not an event");
    camevents::Step st[2000];
    camevents::computeSteps(gapped.data(), gapped.size(), st);
    size_t after = 0;
    for (size_t i = 0; i < gapped.size(); ++i) if (gapped[i].frame == 122 && gapped[i].eye == 0) after = i;
    char stepText[64];
    camevents::formatStep(stepText, sizeof(stepText), st[after]);
    check(st[after].gap == 2 && std::strstr(stepText, "gap=2"), "E8 a step across a missing frame prints gap=2");
    camevents::formatStep(stepText, sizeof(stepText), st[0]);
    check(std::strcmp(stepText, " rot=- mv=-") == 0, "E8 a row with no previous prints rot=- mv=-");
    // One eye only.
    auto left = history(100, 40, 0.05, [](uint32_t f, double* dyaw, double* off) { if (f != 120) return false; *dyaw = 2.4; off[0] = kFaceT[0]; off[1] = kFaceT[1]; off[2] = kFaceT[2]; return true; }, 0.0, false);
    const auto le = camevents::detectEvents(left.data(), left.size());
    const camevents::Summary ls = camevents::summarize(le, 40, 10000000.0);
    check(le.size() == 1 && le[0].eyeMask == 1u && ls.leftOnly == 1 && ls.both == 0, "E9 an excursion in the left eye only is one left-only event");
    // The f36358-f36360 shape: 2.90 deg accepted, the same rows again, then 3.04 deg, then normal. A run of three.
    auto three = history(36340, 40, 0.05, [](uint32_t f, double* dyaw, double* off) {
        if (f < 36358 || f > 36360) return false;
        *dyaw = f == 36360 ? 3.04 : 2.90;
        off[0] = kFaceT[0]; off[1] = kFaceT[1]; off[2] = kFaceT[2];
        return true;
    });
    const auto te = camevents::detectEvents(three.data(), three.size());
    check(te.size() == 1 && te[0].frame == 36358 && te[0].run == 3, "E10 the f36358-f36360 shape (2.90, the same, 3.04 deg) is one event of three frames");
    // Invalid rows (no rows picked that frame) are not rows: the excursion across them is unjudged.
    auto holes = history(100, 40, 0.05, [](uint32_t f, double* dyaw, double* off) { if (f != 120) return false; *dyaw = 2.4; off[0] = kFaceT[0]; off[1] = kFaceT[1]; off[2] = kFaceT[2]; return true; });
    for (auto& r : holes) if (r.frame == 121) r.valid = false;
    check(camevents::detectEvents(holes.data(), holes.size()).empty(), "E11 a frame with no rows is not a row: nothing is judged across it");
}

// ---- C: the pick rules and the eye-target filter on the probe-face sequence ----------------------------------------------------------------------------------
struct Write {
    float rows[12];
    camtag::Target tag;
    bool bound;      // the object bound at the scene's first draw
    uint32_t seq;
};

camtag::Target eyeTag() { camtag::Target t; t.w = 2016; t.h = 2016; t.fmt = 10; t.src = 1; t.colours = 4; return t; }
camtag::Target faceTag() { camtag::Target t; t.w = 256; t.h = 256; t.fmt = 10; t.src = 1; t.colours = 4; return t; }

// The chooser's selection as chooseCameraRows makes it for a continuous frame: a write is a candidate when it is within 3 deg of last frame's pick; the bound
// object's candidates first, then the latest. (A model; production's loop is in temporal_pass.cpp, and the filter around it is production's own, camtag::Filter.)
struct Sim {
    float prev[12];
    bool havePrev = false;
    camnearest::State nearState;
    jumpguard::Guard guard;
    bool nearOn = false, guardOn = false, eyeOn = false;
    uint32_t eyeW = 2016, eyeH = 2016;
    camtag::Submit submit;   // none unless a case sets it: the size rule alone
    uint32_t guardReplaced = 0, nearReplaced = 0, cutWrites = 0, failOpen = 0;
    // Primed with the pick of the frame before the first (a chooser in steady flight; the first frame of a session has no previous pick and takes the latest).
    Sim() { rowsOf(-0.05, 0, 0, 0, prev); havePrev = true; }

    // Returns the index of the applied write.
    int frame(const std::vector<Write>& w) {
        camtag::Filter verdict;
        for (const auto& x : w) verdict.note(x.tag, eyeW, eyeH, submit);
        cutWrites += verdict.cut;
        if (verdict.failedOpen(eyeOn)) ++failOpen;
        const bool filter = verdict.active(eyeOn);
        std::vector<int> cont;
        for (int i = 0; i < int(w.size()); ++i) {
            if (filter && camtag::rejectsFor(w[i].tag, eyeW, eyeH, submit)) continue;
            if (!havePrev || campose::rotationDeg(prev, w[i].rows) < 3.0f) {
                if (havePrev && std::memcmp(w[i].rows, prev, sizeof(prev)) == 0) continue;   // the twin
                cont.push_back(i);
            }
        }
        std::sort(cont.begin(), cont.end(), [&](int a, int b) {
            if (w[a].bound != w[b].bound) return w[a].bound;
            return w[a].seq > w[b].seq;
        });
        int pick = cont.empty() ? int(w.size()) - 1 : cont[0];
        int apply = pick;
        if (!cont.empty()) {
            std::vector<const float*> alts;
            for (int i : cont) alts.push_back(w[i].rows);
            const bool agreed = camnearest::legacyAgrees(nearState, w[pick].rows);
            camnearest::noteLegacy(nearState, w[pick].rows);
            const auto nv = camnearest::decide(nearState, w[pick].rows, alts.data(), int(alts.size()), agreed);
            const auto jd = jumpguard::decide(guard, w[pick].rows, alts.data(), int(alts.size()));
            const int nu = camnearest::applied(nearOn, nv);
            const int ju = jumpguard::applied(guardOn && !nearOn, jd);
            bool replaced = false;
            if (nu >= 0 && cont[nu] != pick) { apply = cont[nu]; ++nearReplaced; }
            else if (ju >= 0 && cont[ju] != pick) { apply = cont[ju]; replaced = true; ++guardReplaced; }
            camnearest::commit(nearState, w[apply].rows);
            jumpguard::commit(guard, w[apply].rows, replaced);
        }
        std::memcpy(prev, w[apply].rows, sizeof(prev));
        havePrev = true;
        return apply;
    }
};

// 364 frames (four 91-frame cycles) of a level cockpit looking forward: the eye write every frame, and in the 6 frames of a burst (the review's two probes,
// bursts 11 frames apart on a 91-frame cycle) ONE cube face a frame: back (180 deg), four sides (90 deg) and the forward face last, at offset 5. The forward
// face is `fwdDeg` from the head; the others are 90 and 180 deg away and never within the 3 deg gate. The faces are written after the eye, in the same
// constant block (bound), and drawn into a 256x256 target.
std::vector<std::vector<Write>> probeFaceFrames(double fwdDeg, uint32_t frames, std::vector<uint32_t>* burstFrames) {
    std::vector<std::vector<Write>> all;
    uint32_t seq = 1;
    for (uint32_t f = 0; f < frames; ++f) {
        std::vector<Write> w;
        Write eye{};
        rowsOf(f * 0.05, 0, 0, 0, eye.rows);
        eye.tag = eyeTag(); eye.bound = true; eye.seq = seq++;
        w.push_back(eye);
        const uint32_t phase = f % 91;
        const bool burst = (phase < 6) || (phase >= 11 && phase < 17);
        if (burst) {
            if (burstFrames) burstFrames->push_back(f);
            const uint32_t offset = phase < 6 ? phase : phase - 11;
            const double yawOffsets[6] = {180.0, 90.0, 90.0, 90.0, 90.0, fwdDeg};
            Write fw{};
            rowsOf(f * 0.05 + yawOffsets[offset], kFaceT[0], kFaceT[1], kFaceT[2], fw.rows);
            fw.tag = faceTag(); fw.bound = true; fw.seq = seq++;
            w.push_back(fw);
        }
        all.push_back(w);
    }
    return all;
}

std::vector<camevents::Event> eventsOfSim(Sim& sim, const std::vector<std::vector<Write>>& frames) {
    std::vector<camevents::Row> rows;
    for (uint32_t f = 0; f < frames.size(); ++f) {
        const int a = sim.frame(frames[f]);
        for (uint32_t eye = 0; eye < 2; ++eye) {
            camevents::Row r;
            r.frame = f; r.eye = eye; r.qpc = int64_t(f) * 111111; r.valid = true;
            std::memcpy(r.rows, frames[f][a].rows, sizeof(r.rows));
            rows.push_back(r);
        }
    }
    return camevents::detectEvents(rows.data(), rows.size());
}

void caseChoosers() {
    // 1. The legacy chooser, forward face 2.3 deg from the head: the face is the latest continuous write of its frame and is picked, one frame in each burst
    //    (the review: events at burst offset 5, twice a cycle), and the next frame returns to the eye. The detector finds those picks as events.
    std::vector<uint32_t> bursts;
    const auto near23 = probeFaceFrames(2.3, 364, &bursts);
    Sim legacy;
    int facePicks = 0;
    for (uint32_t f = 0; f < near23.size(); ++f) {
        const int a = legacy.frame(near23[f]);
        if (near23[f][a].tag.w == 256) ++facePicks;
    }
    check(facePicks == 8, "C1 the legacy chooser picks the probe's forward face when it lies 2.3 deg from the head: one frame in each burst, 8 in four cycles (the review's mechanism)");
    {
        Sim again;
        const auto ev = eventsOfSim(again, near23);
        bool at = ev.size() == 8;
        for (size_t k = 0; at && k < 4; ++k) at = ev[k * 2].frame == k * 91 + 5 && ev[k * 2 + 1].frame == k * 91 + 16;
        check(at, "C1 the detector finds them: events at burst offset 5 of both probes, 11 frames apart, every 91 frames");
    }
    // 2. With the eye-target rule on (always, in VR), no face is ever picked and the picked rows never leave the head's track.
    Sim eye;
    eye.eyeOn = true;
    int facePicksEye = 0;
    double worst = 0;
    for (uint32_t f = 0; f < near23.size(); ++f) {
        const int a = eye.frame(near23[f]);
        if (near23[f][a].tag.w == 256) ++facePicksEye;
        float head[12];
        rowsOf(f * 0.05, 0, 0, 0, head);
        if (std::memcmp(head, near23[f][a].rows, sizeof(head)) != 0) worst += 1.0;   // not bit-equal to the head's track
    }
    check(facePicksEye == 0 && worst == 0.0, "C2 the eye-target rule: the faces are never a candidate; the picked rows are the head's track to the last bit");
    check(eye.cutWrites == bursts.size() && bursts.size() == 48, "C2 ...and the filter counted the face of each burst frame (48 in four cycles) as rejected");
    // 3. Key off counts what it would reject, and changes nothing: the picks equal the legacy picks.
    Sim off;
    bool same = true;
    Sim legacy2;
    for (uint32_t f = 0; f < near23.size(); ++f) same = same && off.frame(near23[f]) == legacy2.frame(near23[f]);
    check(same && off.cutWrites == eye.cutWrites, "C3 key off: the same picks as the legacy chooser, and the would-be rejections counted");
    // 4. The nearest-write rule and the jump guard each remove the excursion by the symptom: the picked rows stay within 1 cm of the head's track.
    for (int arm = 0; arm < 2; ++arm) {
        Sim s;
        (arm == 0 ? s.nearOn : s.guardOn) = true;
        double w2 = 0;
        for (uint32_t f = 0; f < near23.size(); ++f) {
            const int a = s.frame(near23[f]);
            float head[12];
            rowsOf(f * 0.05, 0, 0, 0, head);
            w2 = std::max<double>(w2, campose::translationM(head, near23[f][a].rows));
        }
        check(w2 < 0.01 && (arm == 0 ? s.nearReplaced : s.guardReplaced) > 0, arm == 0 ? "C4 fix.camera_nearest replaces the face picks with the head's track" : "C4 fix.camera_jump_guard replaces the face picks with the head's track");
    }
    // 5. A forward face at 7 deg and at 3.4 deg (the review's quiet bursts): outside the 3 deg gate, so the legacy chooser never takes it, with or without the filter.
    for (double deg : {7.0, 3.4}) {
        const auto fr = probeFaceFrames(deg, 364, nullptr);
        Sim s;
        int picks = 0;
        for (uint32_t f = 0; f < fr.size(); ++f) { const int a = s.frame(fr[f]); if (fr[f][a].tag.w == 256) ++picks; }
        check(picks == 0, deg > 5 ? "C5 a forward face 7 deg from the head is outside the gate: never picked" : "C5 a forward face 3.4 deg from the head is outside the gate: never picked");
    }
    // 6. A forward face at exactly 2.9 deg (inside) is taken by the legacy chooser, 3.04 deg (the f36360 frame) is refused.
    for (double deg : {2.9, 3.04}) {
        const auto fr = probeFaceFrames(deg, 364, nullptr);
        Sim s;
        int picks = 0;
        for (uint32_t f = 0; f < fr.size(); ++f) { const int a = s.frame(fr[f]); if (fr[f][a].tag.w == 256) ++picks; }
        check(deg < 3.0 ? picks > 0 : picks == 0, deg < 3.0 ? "C6 a forward face 2.9 deg from the head is inside the 3 deg gate and is picked" : "C6 a forward face 3.04 deg from the head is refused by the gate");
    }
    // 7. The filter stands down when nothing would be eligible (a wrong eye size): the picks are the legacy picks, and the stand-down is counted.
    Sim blind;
    blind.eyeOn = true;
    blind.eyeW = 640; blind.eyeH = 640;   // every write, face and eye, is far from 640: nothing eligible
    Sim legacy3;
    bool sameBlind = true;
    for (uint32_t f = 0; f < near23.size(); ++f) sameBlind = sameBlind && blind.frame(near23[f]) == legacy3.frame(near23[f]);
    check(sameBlind && blind.failOpen == near23.size(), "C7 a wrong eye size rejects every write, so the filter stands down: the legacy picks, every frame counted fail_open");
    // 8. An unknown write (no draw followed) is never rejected.
    camtag::Target unknown;
    check(!camtag::rejects(unknown, 2016, 2016) && camtag::rejects(faceTag(), 2016, 2016) && !camtag::rejects(eyeTag(), 2016, 2016) && !camtag::rejects(faceTag(), 0, 0),
          "C8 unknown never rejects; 256x256 against a 2016 eye does; the eye does not; no eye size known never rejects");
    camtag::Target shadow = eyeTag(); shadow.w = 2048; shadow.h = 1024;
    camtag::Target depthOnly = eyeTag(); depthOnly.src = 2; depthOnly.colours = 0;
    camtag::Target wide = eyeTag(); wide.w = 2016 + 600;
    check(camtag::rejects(shadow, 2016, 2016) && !camtag::rejects(depthOnly, 2016, 2016) && camtag::rejects(wide, 2016, 2016),
          "C8 a 2048x1024 sun cascade is not eye-sized; a depth-only pass of the eye is; a target a quarter wider than the eye is not");
    // 9. A real cut still gets taken: the camera jumps 5 m (a rotation-preserving jump, so it passes the 3 deg gate) and holds, while a stale write of the
    //    old view keeps being written. The legacy picks the new pose at once; the nearest rule vetoes it once and takes it the frame it holds
    //    (the second); the guard replaces it three frames and takes it on the fourth.
    for (int arm = 0; arm < 3; ++arm) {
        Sim s;
        if (arm == 1) s.nearOn = true;
        if (arm == 2) s.guardOn = true;
        int first = -1;
        float target[12], lastRows[12] = {};
        for (uint32_t f = 0; f < 30; ++f) {
            std::vector<Write> w;
            Write old{};
            rowsOf(f * 0.05, 0, 0, 0, old.rows);
            old.tag = eyeTag(); old.bound = false; old.seq = f * 2 + 1;            // a stale write of the old view keeps being written
            w.push_back(old);
            Write now{};
            rowsOf(f * 0.05, 5.0, 0, 0, now.rows);
            if (f >= 10) {
                now.tag = eyeTag(); now.bound = true; now.seq = f * 2 + 2;
                w.push_back(now);
            }
            const int a = s.frame(w);
            std::memcpy(lastRows, w[a].rows, sizeof(lastRows));
            std::memcpy(target, now.rows, sizeof(target));
            if (f >= 10 && first < 0 && campose::translationM(lastRows, now.rows) < 0.01f) first = int(f - 10);
        }
        check(first >= 0 && campose::translationM(lastRows, target) < 0.01f,
              arm == 0 ? "C9 legacy: the cut is taken" : arm == 1 ? "C9 nearest: the cut is taken" : "C9 jump guard: the cut is taken");
        if (arm == 0) check(first == 0, "C9 the legacy chooser takes the cut at once");
        if (arm == 1) check(first == 1, "C9 nearest vetoes the cut for one frame and takes it the frame it holds");
        if (arm == 2) check(first == 3, "C9 the jump guard replaces the cut for three frames and takes it on the fourth");
    }
    // 11. The hangar flight of 2026-10-13 (reviews/flight-2026-10-14-analysis.md): the size rule cannot see the bad picks, because the probe's forward face
    //     is tagged to a target of the EYE's size -- 2016x2016 format 27, R8G8B8A8_TYPELESS, the texture the game submits -- while the good picks of the same
    //     burst frames are tagged 2016x2016 format 26 (R11G11B10_FLOAT, the scene's lit HDR). Outside bursts the normal pick is a late head-camera write that
    //     is ALSO tagged 27 (418 of 456 frames in the log), on the same track as the scene writes to a millimetre.
    {
        camtag::Target hdr = eyeTag(); hdr.fmt = 26;            // the scene's lit HDR target
        camtag::Target sub = eyeTag(); sub.fmt = 27;            // the submitted eye texture
        camtag::Submit submit; submit.fmt = 27; submit.w = 2016; submit.h = 2016;   // the game submits R8G8B8A8_TYPELESS (27), 2016x2016 per eye
        check(camtag::judge(hdr, 2016, 2016, submit) == camtag::Why::Keep, "C11 format 26 at the eye's size is the scene's: kept");
        check(camtag::judge(sub, 2016, 2016, submit) == camtag::Why::SubmitTarget, "C11 format 27 at the eye's size is the submitted texture: rejected");
        camtag::Target typed = sub; typed.fmt = 28;
        camtag::Target other = sub; other.fmt = 39;
        camtag::Target depthOnlySub = sub; depthOnlySub.src = 2;
        check(camtag::judge(typed, 2016, 2016, submit) == camtag::Why::SubmitTarget && camtag::judge(depthOnlySub, 2016, 2016, submit) == camtag::Why::Keep &&
              camtag::judge(faceTag(), 2016, 2016, submit) == camtag::Why::NotEyeSized && camtag::judge(sub, 2016, 2016, camtag::Submit{}) == camtag::Why::Keep &&
              camtag::judge(camtag::Target{}, 2016, 2016, submit) == camtag::Why::Keep && camtag::judge(other, 2016, 2016, submit) == camtag::Why::Keep,
              "C11 the R8G8B8A8 family is one format; a depth-only pass is not the submit texture; the faces are not eye-sized; without a submit identity or a tag nothing is rejected");

        // The sequence: head track yaw 0.05 deg a frame at (-3.006, -1.774, -13.028); the scene write (tag 26) every frame, the late head write (tag 27, 1 mm
        // off) on frames with no probe write at the end, the probe's faces in bursts (back, four sides tagged to the probe's small targets), and on burst
        // offset 2 of both probes the forward face -- 2.5 deg from the head, 0.117 m away -- written last and tagged 27 (the 4 of 4 bad picks of the flight).
        std::vector<std::vector<Write>> flight;
        std::vector<uint32_t> badFrames;
        uint32_t seq = 1;
        for (uint32_t f = 0; f < 364; ++f) {
            std::vector<Write> w;
            Write scene{};
            rowsOf(f * 0.05, -3.006, -1.774, -13.028, scene.rows);
            scene.tag = hdr; scene.bound = true; scene.seq = seq++;
            w.push_back(scene);
            const uint32_t phase = f % 91;
            const bool burst = (phase < 6) || (phase >= 11 && phase < 17);
            const uint32_t offset = phase < 6 ? phase : phase - 11;
            if (burst && offset == 2) {
                Write face{};
                rowsOf(f * 0.05 + 2.5, -2.940, -1.698, -12.968, face.rows);
                face.tag = sub; face.bound = true; face.seq = seq++;
                w.push_back(face);
                badFrames.push_back(f);
            } else if (burst) {
                Write face{};
                rowsOf(f * 0.05 + (offset == 0 ? 180.0 : 90.0), -2.940, -1.698, -12.968, face.rows);
                face.tag = faceTag(); face.bound = true; face.seq = seq++;
                w.push_back(face);
            } else {
                Write late{};
                rowsOf(f * 0.05, -3.0059, -1.774, -13.028, late.rows);
                late.tag = sub; late.bound = true; late.seq = seq++;
                w.push_back(late);
            }
            flight.push_back(w);
        }
        check(badFrames.size() == 8, "C11 the sequence has the bad frames: offset 2 of both probes, four cycles");
        Sim legacyFlight;
        std::vector<int> legacyPick;
        int badPicked = 0;
        for (uint32_t f = 0; f < flight.size(); ++f) {
            const int a = legacyFlight.frame(flight[f]);
            legacyPick.push_back(a);
            if (std::find(badFrames.begin(), badFrames.end(), f) != badFrames.end() && flight[f][a].tag.fmt == 27 && flight[f][a].rows[3] > -2.95f) ++badPicked;
        }
        check(badPicked == 8, "C11 the legacy chooser takes the probe's forward face on every bad frame (tagged format 27)");
        Sim sizeOnly;
        sizeOnly.eyeOn = true;
        int sizeBad = 0;
        for (uint32_t f = 0; f < flight.size(); ++f) {
            const int a = sizeOnly.frame(flight[f]);
            if (flight[f][a].rows[3] > -2.95f) ++sizeBad;
        }
        check(sizeBad == 8, "C11 the size rule alone does not catch them (the flight's finding): the same 8 bad picks");
        Sim identity;
        identity.eyeOn = true;
        identity.submit = submit;
        int identityBad = 0, picked26 = 0, picked27 = 0;
        double maxDiffM = 0;
        std::vector<camevents::Row> rows;
        for (uint32_t f = 0; f < flight.size(); ++f) {
            const int a = identity.frame(flight[f]);
            if (flight[f][a].rows[3] > -2.95f) ++identityBad;
            if (flight[f][a].tag.fmt == 26) ++picked26;
            if (flight[f][a].tag.fmt == 27) ++picked27;
            maxDiffM = std::max<double>(maxDiffM, campose::translationM(flight[f][a].rows, flight[f][legacyPick[f]].rows));
            for (uint32_t e2 = 0; e2 < 2; ++e2) {
                camevents::Row r; r.frame = f; r.eye = e2; r.qpc = int64_t(f) * 111111; r.valid = true;
                std::memcpy(r.rows, flight[f][a].rows, sizeof(r.rows));
                rows.push_back(r);
            }
        }
        check(identityBad == 0 && picked27 == 0 && picked26 == int(flight.size()), "C11 the identity rule: no bad pick, every pick is the scene's format 26, none is format 27");
        check(camevents::detectEvents(rows.data(), rows.size()).empty(), "C11 ...and the picked rows have no camera-row event");
        check(maxDiffM < 0.15 && identity.cutWrites > 0, "C11 ...and where the filter changes a pick it moves to the scene write: a millimetre on normal frames, the probe's 0.117 m on the bad ones");
        // Fail open: a game that draws its scene straight into the submitted texture has every write tagged 27: nothing is eligible, the filter stands down.
        Sim straight;
        straight.eyeOn = true;
        straight.submit = submit;
        Sim straightLegacy;
        bool same2 = true;
        for (uint32_t f = 0; f < 40; ++f) {
            std::vector<Write> w;
            Write a{};
            rowsOf(f * 0.05, 0, 0, 0, a.rows);
            a.tag = sub; a.bound = true; a.seq = f + 1;
            w.push_back(a);
            same2 = same2 && straight.frame(w) == straightLegacy.frame(w);
        }
        check(same2 && straight.failOpen == 40, "C11 every write drawn into the submitted texture: nothing is eligible, the filter stands down (fail open) every frame");
    }

    // 10. The 30 s lines.
    camtag::Stats st;
    st.frames = 2700; st.writes = 250000; st.tagged = 249000; st.unknown = 1000; st.rejected = 2160; st.rejectFrames = 360; st.failOpen = 0; st.eyeW = 2016; st.eyeH = 2016;
    camtag::noteRejected(st, 256, 256); camtag::noteRejected(st, 256, 256); camtag::noteRejected(st, 2048, 1024);
    char line[400];
    camtag::formatReport(line, sizeof(line), st);
    check(std::strstr(line, "temporal camera eye target: frames=2700 writes=250000 tagged=249000 unknown=1000") && (std::strstr(line, "eye=2016x2016 submit=") && !std::strstr(line, "key=")) &&
          std::strstr(line, "rejected_sizes=256x256:2,2048x1024:1"), "C10 the eye-target line names the counts, the eye size and the rejected sizes");
    camnearest::Stats ns; ns.frames = 100; ns.differs = 3; ns.reanchors = 1; ns.maxDistM = 0.2f;
    camnearest::formatReport(line, sizeof(line), ns, false);
    check(std::strstr(line, "temporal camera nearest: frames=100 differs_from_legacy=3 reanchors=1 max_dist_m=0.200 key=off"), "C10 the nearest-write line");
    jumpguard::Stats js; js.frames = 100; js.rejected = 4; js.replaced = 3; js.maxJumpM = 0.118f;
    jumpguard::formatReport(line, sizeof(line), js, "superseded");
    check(std::strstr(line, "temporal camera jump guard: frames=100 rejected=4 replaced=3 passed_through=97") && std::strstr(line, "key=superseded max_jump_m=0.118"), "C10 the jump guard line, with the superseded key state");
}

// ---- B: the burst offset ------------------------------------------------------------------------------------------------------------------------------------
void caseBurst() {
    camtag::BurstTracker t;
    // The review's pattern: baseline 86, two bursts of six frames 11 apart on a 91-frame cycle, counts 110,111,106,114,106,114 and 107,114,103,114,103,117.
    const uint32_t a[6] = {110, 111, 106, 114, 106, 114}, b[6] = {107, 114, 103, 114, 103, 117};
    for (uint32_t f = 0; f < 400; ++f) {
        const uint32_t phase = f % 91;
        uint32_t w = 86 + (f % 3 == 0 ? 1 : 0);
        if (phase < 6) w = a[phase];
        else if (phase >= 11 && phase < 17) w = b[phase - 11];
        t.push(f, w);
    }
    check(t.offsetOf(2) == camtag::BurstTracker::kNone, "B1 the first frames are not judged (no baseline yet)");
    check(t.offsetOf(91) == 0 && t.offsetOf(92) == 1 && t.offsetOf(96) == 5 && t.offsetOf(97) == camtag::BurstTracker::kNone, "B2 a burst's offsets run 0 to 5 over six frames, then none");
    check(t.offsetOf(102) == 0 && t.offsetOf(107) == 5 && t.offsetOf(108) == camtag::BurstTracker::kNone, "B2 the partner burst 11 frames later does the same");
    check(t.offsetOf(150) == camtag::BurstTracker::kNone, "B2 a quiet frame has no offset");
    char text[64];
    camtag::formatBurst(text, sizeof(text), t, 94);
    check(std::strcmp(text, " burst=3 wr=114/86") == 0 || std::strstr(text, " burst=3 wr=114/"), "B3 the TCAM tag reads burst=3 wr=114/86");
    camtag::formatBurst(text, sizeof(text), t, 150);
    check(std::strstr(text, " burst=- wr=") == text, "B3 outside a burst: burst=-");
    camtag::formatBurst(text, sizeof(text), t, 5000);
    check(std::strcmp(text, " burst=? wr=?") == 0, "B3 a frame the tracker does not keep: burst=?");
    // The 2048-frame window: an old frame is gone, a recent one stays.
    camtag::BurstTracker t2;
    for (uint32_t f = 0; f < 3000; ++f) t2.push(f, 86);
    check(!t2.known(100) && t2.known(2999) && t2.known(1000), "B4 the tracker keeps the last 2048 frames");
    // A step in the baseline (a scene change from 86 to 140 writes) is a burst only until the median catches up, not forever.
    camtag::BurstTracker t3;
    for (uint32_t f = 0; f < 100; ++f) t3.push(f, f < 50 ? 86 : 140);
    check(t3.offsetOf(99) == camtag::BurstTracker::kNone, "B5 a lasting rise in the write count becomes the baseline");
}

// ---- M: the mini-render record -------------------------------------------------------------------------------------------------------------------------------
void caseMini() {
    check(minirender::isMiniBind(4, 256, 256) && !minirender::isMiniBind(3, 256, 256) && !minirender::isMiniBind(4, 2016, 2016) &&
          !minirender::isMiniBind(4, 256, 128) && !minirender::isMiniBind(4, 0, 0) && minirender::isMiniBind(5, 1024, 1024) && !minirender::isMiniBind(4, 1025, 1025),
          "M1 a mini view is four or more colour targets with a square depth view of 1024 or less");
    auto ring = std::make_unique<minirender::Ring>();
    ring->notePass(36358, 256, 256, 4, 0x1A2B3C);
    for (int i = 0; i < 285; ++i) ring->noteDraw(36358);
    ring->notePass(36358, 256, 256, 4, 0x1A2B3C);   // the same caller again: one RVA
    ring->notePass(36358, 256, 256, 4, 0x4D5E6F);   // a second caller
    ring->notePass(36358, 256, 256, 4, 0x777777);   // a third: not kept
    const float rows[12] = {1, 0, 0, -0.054f, 0, 1, 0, -0.094f, 0, 0, 1, 0.045f};
    ring->noteWrite(36358, rows);
    ring->noteWrite(36358, rows);
    const minirender::Frame* f = ring->find(36358);
    check(f && f->passes == 4 && f->draws == 285 && f->rvaN == 2 && f->rva[0] == 0x1A2B3C && f->rva[1] == 0x4D5E6F && f->writes == 2 && f->haveRows && f->w == 256,
          "M2 a frame keeps its passes, draws, the first two distinct caller RVAs and the camera writes drawn into it");
    char line[300];
    minirender::formatFrame(line, sizeof(line), *f);
    check(std::strcmp(line, "P3 f36358 mini=256x256 colours=4 passes=4 draws=285 rva=0x1A2B3C,0x4D5E6F writes=2 first_write_t=(-0.054,-0.094,+0.045)") == 0, "M2 the P3 frame line format");
    check(ring->find(36357) == nullptr, "M3 a frame without a mini-render has no record");
    ring->noteDraw(36357);   // a draw count alone is not a pass
    check(ring->find(36357) == nullptr, "M3 draws without a pass do not make a mini-render frame");
    // Cadence and the join: mini-renders on 36340, 36358, 36376 (every 18 frames); events at 36358 (same frame), 36377 (the frame after), 36400 (neither), 100 (no record).
    for (uint32_t m : {36340u, 36376u}) { ring->notePass(m, 256, 256, 4, 0x1A2B3C); ring->noteDraw(m); }
    const minirender::Join j = minirender::joinEvents(*ring, {36358, 36377, 36400, 100}, 36000, 36500);
    check(j.events == 4 && j.sameFrame == 1 && j.previousFrame == 1 && j.either == 2 && j.neither == 2 && j.outOfRange == 1,
          "M4 the join: an event with a mini-render the same frame, one the frame before, one with neither, one outside the kept frames");
    minirender::formatJoin(line, sizeof(line), j);
    check(std::strcmp(line, "P3 join: events=4 mini_same_frame=1 mini_previous_frame=1 either=2 neither=2 no_record=1") == 0, "M4 the P3 join line format");
    minirender::formatSummary(line, sizeof(line), *ring, 500);
    check(std::strstr(line, "P3 mini-render frames: 3 in the kept 500 frames (0.6%)") && std::strstr(line, "first size 256x256") && std::strstr(line, "cadence median 18 frames"),
          "M5 the summary: frames, share of the kept frames, size and the cadence");
    auto empty = std::make_unique<minirender::Ring>();
    minirender::formatSummary(line, sizeof(line), *empty, 100);
    check(std::strstr(line, "P3 mini-render frames: 0 in the kept 100 frames (none seen"), "M5 an empty ring says none seen, distinct from the line being absent");
    // Ring wrap: frame 36358 + 2048 reuses the slot.
    empty->notePass(5, 256, 256, 4, 1);
    empty->notePass(5 + minirender::kKeepFrames, 256, 256, 4, 2);
    check(empty->find(5) == nullptr && empty->find(5 + minirender::kKeepFrames) != nullptr, "M6 the ring keeps 2048 frames: the slot belongs to the newer frame");
}

// ---- S: the sky ring, pure --------------------------------------------------------------------------------------------------------------------------------------
void caseSky() {
    using namespace skyring;
    // Half floats against known values and round trips.
    check(f32ToF16(1.0f) == 0x3C00 && f32ToF16(0.0f) == 0 && f32ToF16(-2.0f) == 0xC000 && f32ToF16(65504.0f) == 0x7BFF && f32ToF16(1e9f) == 0x7C00 &&
          f32ToF16(0.5f) == 0x3800 && f32ToF16(5.9604645e-8f) == 0x0001, "S1 half float encodings: 1, 0, -2, max, overflow, 0.5, the smallest denormal");
    bool roundTrip = true;
    for (float v : {0.0f, 0.001f, 0.0123f, 0.3333f, 0.912f, 1.0f, 2.1f, 29.5f, 1000.25f}) roundTrip = roundTrip && std::fabs(f16ToF32(f32ToF16(v)) - v) <= v * 0.0006f + 1e-7f;
    check(roundTrip, "S1 half float round trips within 0.06 percent");
    // The entry layout: pack and unpack, 32 bytes, empty slot reads as nothing.
    Entry e;
    e.seq = 4321; e.frame = 36358; e.eye = 1; e.depthValid = true; e.skyN = 5120; e.bright = 12;
    e.rawMean = 0.0123f; e.rawMax = 0.912f; e.outMean = 0.0119f; e.outMax = 0.874f; e.mvMean = 0.34f; e.mvMax = 21.5f;
    uint32_t dw[kEntryDwords];
    pack(e, dw);
    Entry back;
    check(kEntryBytes == 32 && unpack(dw, &back) && back.seq == 4321 && back.frame == 36358 && back.eye == 1 && back.depthValid && !back.expAvail && back.skyN == 5120 &&
          back.bright == 12 && approx(back.rawMean, 0.0123, 1e-5) && approx(back.outMax, 0.874, 5e-4) && approx(back.mvMax, 21.5, 0.02), "S2 an entry is 32 bytes and round trips");
    uint32_t zero[kEntryDwords] = {};
    check(!unpack(zero, &back), "S2 a zero slot (seq 0) is empty");
    // Slot arithmetic and the wrap.
    check(slotOf(1) == 0 && slotOf(2048) == 2047 && slotOf(2049) == 0 && slotOf(4096) == 2047 && slotOf(4097) == 0, "S3 dispatch n lands in slot (n-1) mod 2048");
    std::vector<uint32_t> ringWords(kRingEntries * kEntryDwords, 0);
    for (uint32_t seq = 1; seq <= 2100; ++seq) {
        Entry x;
        x.seq = seq; x.frame = 1000 + seq / 2; x.eye = seq & 1u; x.skyN = 100 + (seq % 50);
        pack(x, &ringWords[size_t(slotOf(seq)) * kEntryDwords]);
    }
    const auto decoded = decodeRing(ringWords.data());
    check(decoded.size() == kRingEntries && decoded.front().seq == 53 && decoded.back().seq == 2100, "S3 2100 dispatches leave the last 2048: 53 to 2100");
    bool ordered = true;
    for (size_t i = 1; i < decoded.size(); ++i) ordered = ordered && decoded[i].seq == decoded[i - 1].seq + 1;
    check(ordered, "S3 the decode is in dispatch order with no gap across the wrap");
    check(decodeRing(std::vector<uint32_t>(kRingEntries * kEntryDwords, 0).data()).empty(), "S3 a ring that never ran decodes to nothing");
    // The windows around events: +-45 frames, both eyes, capped.
    std::vector<Entry> entries;
    for (uint32_t f = 1000; f < 1400; ++f)
        for (uint32_t eye = 0; eye < 2; ++eye) { Entry x; x.seq = uint32_t(entries.size()) + 1; x.frame = f; x.eye = eye; x.skyN = 10; entries.push_back(x); }
    bool trunc = false;
    auto picks = windowsAround(entries, {{1100, 1}}, kMaxLines, &trunc);
    check(picks.size() == 2 * (45 + 1 + 45) && !trunc && picks.front().delta == -45 && picks.back().delta == 45, "S4 one event: 91 frames of both eyes, -45 to +45");
    size_t inEvent = 0;
    for (const auto& p : picks) if (p.inEvent) ++inEvent;
    check(inEvent == 2, "S4 only the event's own frame (both eyes) is marked");
    picks = windowsAround(entries, {{1100, 3}}, kMaxLines, &trunc);
    check(picks.size() == 2 * (45 + 3 + 45), "S4 a three-frame event widens the window by its run");
    picks = windowsAround(entries, {{1100, 1}, {1130, 1}, {1250, 1}}, kMaxLines, &trunc);
    check(picks.size() == 2 * (121 + 91), "S4 overlapping windows (1100 and 1130) are merged, not repeated: frames 1055 to 1175, then 1205 to 1295");
    picks = windowsAround(entries, {{1100, 1}, {1250, 1}, {1350, 1}}, 150, &trunc);
    check(picks.size() == 150 && trunc, "S4 the line cap truncates, and says so");
    check(windowsAround(entries, {}, kMaxLines, &trunc).empty(), "S4 no events, no windows");
    // The per-second buckets.
    const auto buckets = bucketsOf(entries);
    check(!buckets.empty() && buckets.front().eyeFrames == 2 * 80 && buckets.front().firstFrame == 1000 && buckets.front().lastFrame == 1079, "S5 buckets of 90 frames by frame/90");
    // The "no samples" line.
    char line[300];
    formatNoSamples(line, sizeof(line), false, 0);
    check(std::strncmp(line, "sky ring: no samples (off or never ran)", 39) == 0 && std::strstr(line, "key=off dispatches=0"), "S6 the ring that never ran says \"sky ring: no samples (off or never ran)\"");
    // The reducer: the CPU reference, by hand. Four samples, one not sky.
    Reducer r;
    Sample s0, s1, s2, s3;
    s0.depth = 0.0f; s0.rawRgb[0] = s0.rawRgb[1] = s0.rawRgb[2] = 0.5f; s0.outN = 4; for (auto& o : s0.outRgb) o[0] = o[1] = o[2] = 0.25f; s0.mv[0] = 3; s0.mv[1] = 4;
    s1.depth = 0.05f; s1.rawRgb[0] = s1.rawRgb[1] = s1.rawRgb[2] = 1.0f; s1.outN = 4; for (auto& o : s1.outRgb) o[0] = o[1] = o[2] = 0.0f; s1.outRgb[2][0] = s1.outRgb[2][1] = s1.outRgb[2][2] = 1.0f;
    s2.depth = 0.5f; s2.rawRgb[0] = s2.rawRgb[1] = s2.rawRgb[2] = 1.0f;   // not sky (0.5 > A)
    s3.depth = -1.0f; s3.outN = 1;                                         // sky, black
    r.add(s0, 0.1f, true); r.add(s1, 0.1f, true); r.add(s2, 0.1f, true); r.add(s3, 0.1f, true);
    const Entry re = r.finish(7, 99, 0, true);
    check(re.skyN == 3 && re.bright == 2 && approx(re.rawMean, (0.5 + 1.0 + 0.0) / 3.0, 1e-3) && approx(re.rawMax, 1.0, 1e-6) && approx(re.outMax, 1.0, 1e-3) &&
          approx(re.outMean, (0.25 + 0.25 + 0.0) / 3.0, 1e-3) && approx(re.mvMean, 5.0 / 3.0, 0.01) && approx(re.mvMax, 5.0, 1e-6),
          "S7 the reference reduction: 3 sky samples of 4, mean and max luma, 2 bright (output block max over 0.05), motion length 5 px");
    Reducer none;
    none.add(s0, 0.1f, false);
    check(none.finish(1, 1, 0, false).skyN == 0, "S7 with no depth copy nothing is sky");
    // Input coordinates: the grid's centres.
    uint32_t x, y;
    inputCoord(0, 2016, 2016, &x, &y);
    check(x == 7 && y == 7, "S8 sample 0 of a 2016 eye is the centre of the first 15.75 px cell");
    inputCoord(kSamples - 1, 2016, 2016, &x, &y);
    check(x == 2008 && y == 2008, "S8 the last sample is the last cell's centre");
}

// ---- G: the shipped HLSL on WARP ----------------------------------------------------------------------------------------------------------------------------
struct Tex {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
};
ComPtr<ID3D11Device> g_dev;
ComPtr<ID3D11DeviceContext> g_ctx;

Tex makeTex(uint32_t w, uint32_t h, DXGI_FORMAT fmt, const void* data, uint32_t pitch) {
    Tex t;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{data, pitch, 0};
    if (FAILED(g_dev->CreateTexture2D(&d, &init, &t.tex))) return t;
    g_dev->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv);
    return t;
}

// A synthetic eye: raw luma and output luma are position hashes, the sky is the upper 40% of rows plus a few scattered sky pixels, the motion vectors are
// a smooth field with one hot cell. The twin of every texel is kept for the CPU reference.
struct Scene {
    uint32_t inW, inH, outW, outH;
    std::vector<uint32_t> raw, out;      // RGBA8
    std::vector<uint16_t> mv;            // RG16F
    std::vector<float> depth;
    Tex rawT, outT, mvT, depthT;
    float projA = 0.1f;
    static uint32_t hash(uint32_t a) { a ^= a >> 16; a *= 0x7feb352dU; a ^= a >> 15; a *= 0x846ca68bU; a ^= a >> 16; return a; }
    void build(uint32_t iw, uint32_t ih, uint32_t ow, uint32_t oh, uint32_t seed) {
        inW = iw; inH = ih; outW = ow; outH = oh;
        raw.resize(size_t(iw) * ih); out.resize(size_t(ow) * oh); mv.resize(size_t(iw) * ih * 2); depth.resize(size_t(iw) * ih);
        for (uint32_t y = 0; y < ih; ++y)
            for (uint32_t x = 0; x < iw; ++x) {
                const uint32_t h = hash(x * 7919u + y * 104729u + seed);
                const uint32_t v = (h & 0xFF) < 12 ? 200 + (h >> 8) % 56 : (h >> 8) % 24;   // mostly dark, a few bright points
                raw[size_t(y) * iw + x] = v | (v << 8) | (v << 16) | 0xFF000000u;
                const bool sky = y < ih * 2 / 5 || (h % 97) == 0;
                depth[size_t(y) * iw + x] = sky ? ((h & 1) ? 0.0f : 0.05f) : 0.5f;   // 0 and 0.05 are both far with A = 0.1
                const float mx = float(x % 7) * 0.1f + (y == 1 && x == 2 ? 21.0f : 0.0f), my = float(y % 5) * 0.2f;
                mv[(size_t(y) * iw + x) * 2] = skyring::f32ToF16(mx);
                mv[(size_t(y) * iw + x) * 2 + 1] = skyring::f32ToF16(my);
            }
        for (uint32_t y = 0; y < oh; ++y)
            for (uint32_t x = 0; x < ow; ++x) {
                const uint32_t h = hash(x * 31337u + y * 7727u + seed * 3u);
                const uint32_t v = (h & 0x3F) < 3 ? 180 + (h >> 8) % 76 : (h >> 8) % 20;
                out[size_t(y) * ow + x] = (v) | (((v + 5) & 0xFF) << 8) | (((v + 9) & 0xFF) << 16) | 0xFF000000u;
            }
        rawT = makeTex(iw, ih, DXGI_FORMAT_R8G8B8A8_UNORM, raw.data(), iw * 4);
        outT = makeTex(ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, out.data(), ow * 4);
        mvT = makeTex(iw, ih, DXGI_FORMAT_R16G16_FLOAT, mv.data(), iw * 4);
        depthT = makeTex(iw, ih, DXGI_FORMAT_R32_FLOAT, depth.data(), iw * 4);
    }
    static void rgb(uint32_t px, float c[3]) { c[0] = (px & 0xFF) / 255.0f; c[1] = ((px >> 8) & 0xFF) / 255.0f; c[2] = ((px >> 16) & 0xFF) / 255.0f; }
    // The CPU twin of one dispatch.
    skyring::Entry reference(uint32_t seq, uint32_t frame, uint32_t eye, bool depthValid) const {
        skyring::Reducer r;
        for (uint32_t s = 0; s < skyring::kSamples; ++s) {
            uint32_t x, y, ox, oy, side;
            skyring::inputCoord(s, inW, inH, &x, &y);
            skyring::outputBlock(x, y, inW, inH, outW, outH, &ox, &oy, &side);
            skyring::Sample sm;
            rgb(raw[size_t(y) * inW + x], sm.rawRgb);
            sm.outN = side * side;
            for (uint32_t j = 0; j < sm.outN; ++j) {
                const uint32_t px = std::min(ox + (j & 1u), outW - 1), py = std::min(oy + (j >> 1), outH - 1);
                rgb(out[size_t(py) * outW + px], sm.outRgb[j]);
            }
            sm.depth = depth[size_t(y) * inW + x];
            sm.mv[0] = skyring::f16ToF32(mv[(size_t(y) * inW + x) * 2]);
            sm.mv[1] = skyring::f16ToF32(mv[(size_t(y) * inW + x) * 2 + 1]);
            r.add(sm, projA, depthValid);
        }
        return r.finish(seq, frame, eye, depthValid);
    }
};

bool sameEntry(const skyring::Entry& g, const skyring::Entry& c, std::string* why) {
    auto rel = [](double a, double b) { return std::fabs(a - b) <= std::fabs(b) * 2.5e-3 + 2e-4; };
    char buf[300];
    if (g.seq != c.seq || g.frame != c.frame || g.eye != c.eye || g.depthValid != c.depthValid || g.skyN != c.skyN || g.bright != c.bright ||
        !rel(g.rawMean, c.rawMean) || !rel(g.rawMax, c.rawMax) || !rel(g.outMean, c.outMean) || !rel(g.outMax, c.outMax) || !rel(g.mvMean, c.mvMean) || !rel(g.mvMax, c.mvMax)) {
        std::snprintf(buf, sizeof(buf), "gpu seq=%u f=%u eye=%u n=%u b=%u raw=%.5f/%.4f out=%.5f/%.4f mv=%.4f/%.3f | cpu seq=%u f=%u eye=%u n=%u b=%u raw=%.5f/%.4f out=%.5f/%.4f mv=%.4f/%.3f",
                      g.seq, g.frame, g.eye, g.skyN, g.bright, g.rawMean, g.rawMax, g.outMean, g.outMax, g.mvMean, g.mvMax,
                      c.seq, c.frame, c.eye, c.skyN, c.bright, c.rawMean, c.rawMax, c.outMean, c.outMax, c.mvMean, c.mvMax);
        *why = buf;
        return false;
    }
    return true;
}

bool readBack(SkyRingGpu& gpu, std::vector<skyring::Entry>* out) {
    const int h = gpu.requestCopy(g_ctx.Get());
    if (h < 0) return false;
    bool failed = false;
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        if (gpu.poll(g_ctx.Get(), h, out, &failed)) return true;
        if (failed) return false;
        g_ctx->Flush();
        Sleep(1);
    }
    return false;
}

void caseGpu() {
    D3D_FEATURE_LEVEL level{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_dev, &level, &g_ctx))) {
        check(false, "G0 WARP could not start");
        return;
    }
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(kSkyRingCs, std::strlen(kSkyRingCs), "sky ring", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &errors);
    if (FAILED(hr)) {
        std::printf("FAIL: G0 the shipped HLSL did not compile: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        ++g_failures; ++g_checks;
        return;
    }
    ComPtr<ID3D11ComputeShader> cs;
    check(SUCCEEDED(g_dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs)), "G0 the shipped HLSL compiles and makes a compute shader");
    SkyRingGpu gpu;
    check(gpu.create(g_dev.Get(), cs.Get()) && gpu.ready(), "G0 the ring, its views and its staging buffers are made");
    // A ring that never ran decodes to nothing and its dispatch counter is zero.
    std::vector<skyring::Entry> got;
    check(readBack(gpu, &got) && got.empty() && gpu.dispatches() == 0, "G1 before any dispatch the ring reads back empty (no sample is distinguishable from a sample)");

    // Eyes of two shapes: DLSS 2x (input 512, output 1024) and 1:1.
    static Scene s2x, s1x;
    s2x.build(512, 480, 1024, 960, 11);
    s1x.build(256, 256, 256, 256, 29);
    check(s2x.rawT.srv && s2x.outT.srv && s2x.mvT.srv && s2x.depthT.srv && s1x.depthT.srv, "G1 the synthetic textures are made");
    uint32_t seq = 0;
    std::string why;
    bool allSame = true;
    auto run = [&](Scene& sc, uint32_t frame, uint32_t eye, bool depthValid) {
        ++seq;
        const bool ran = gpu.dispatch(g_ctx.Get(), eye, frame, sc.rawT.srv.Get(), sc.outT.srv.Get(), sc.mvT.srv.Get(), sc.depthT.srv.Get(), sc.inW, sc.inH, sc.outW, sc.outH, sc.projA, depthValid);
        return ran;
    };
    check(run(s2x, 36358, 0, true), "G2 a dispatch");
    check(run(s2x, 36358, 1, true), "G2 a dispatch for the other eye");
    check(run(s1x, 36359, 0, true), "G2 a dispatch at 1:1 (output the size of the input)");
    check(run(s2x, 36359, 1, false), "G2 a dispatch with no depth copy");
    got.clear();
    check(readBack(gpu, &got) && got.size() == 4, "G2 four entries read back");
    if (got.size() == 4) {
        const skyring::Entry c0 = s2x.reference(1, 36358, 0, true), c1 = s2x.reference(2, 36358, 1, true), c2 = s1x.reference(3, 36359, 0, true), c3 = s2x.reference(4, 36359, 1, false);
        allSame = sameEntry(got[0], c0, &why) && sameEntry(got[1], c1, &why) && sameEntry(got[2], c2, &why) && sameEntry(got[3], c3, &why);
        if (!allSame) std::printf("  %s\n", why.c_str());
        check(allSame, "G3 the GPU's entries equal the CPU reference: sky count, bright count, luma mean and max of raw and output, motion mean and max");
        check(got[0].skyN > 5000 && got[0].skyN < skyring::kSamples && got[0].bright > 0 && got[0].mvMax > 20.0f, "G3 the synthetic sky is non-trivial: thousands of samples, bright points, the hot motion cell");
        check(got[3].skyN == 0 && !got[3].depthValid && got[3].rawMean == 0.0f, "G3 a frame with no depth copy records n=0 and says so");
        check(got[1].eye == 1 && got[0].eye == 0 && got[0].frame == 36358 && got[2].frame == 36359, "G3 the frame and eye are stamped");
    } else {
        check(false, "G3 (skipped: the readback did not return four entries)");
    }
    // The ring wraps: 2100 dispatches in all leave the last 2048 and the slot arithmetic holds. Small textures keep WARP quick.
    static Scene tiny;
    tiny.build(64, 64, 64, 64, 5);
    while (seq < 2100) {
        ++seq;
        gpu.dispatch(g_ctx.Get(), seq & 1u, 40000 + seq / 2, tiny.rawT.srv.Get(), tiny.outT.srv.Get(), tiny.mvT.srv.Get(), tiny.depthT.srv.Get(), 64, 64, 64, 64, tiny.projA, true);
    }
    got.clear();
    check(readBack(gpu, &got) && got.size() == skyring::kRingEntries && got.front().seq == 53 && got.back().seq == 2100 && gpu.dispatches() == 2100,
          "G4 2100 dispatches: the ring reads back 2048 entries, seq 53 to 2100");
    bool contiguous = got.size() == skyring::kRingEntries;
    for (size_t i = 1; contiguous && i < got.size(); ++i) contiguous = got[i].seq == got[i - 1].seq + 1;
    check(contiguous && got.back().frame == 40000 + 2100 / 2 && got.back().eye == 0 && got.front().frame == 40000 + 53 / 2, "G4 contiguous in dispatch order across the wrap, each stamped with its frame and eye");
    const skyring::Entry tinyRef = tiny.reference(2100, 40000 + 2100 / 2, 0, true);
    std::string why2;
    check(sameEntry(got.back(), tinyRef, &why2), "G4 the last entry still equals the CPU reference after the wrap");
    // Two readbacks in flight, a third refused, and the buffers free again once polled.
    const int h0 = gpu.requestCopy(g_ctx.Get()), h1 = gpu.requestCopy(g_ctx.Get()), h2 = gpu.requestCopy(g_ctx.Get());
    check(h0 == 0 && h1 == 1 && h2 == -1, "G5 two staging buffers in flight; a third request is refused, not queued");
    std::vector<skyring::Entry> a, b;
    bool failed = false;
    for (int i = 0; i < 5000 && !(gpu.poll(g_ctx.Get(), h0, &a, &failed) || failed); ++i) { g_ctx->Flush(); Sleep(1); }
    for (int i = 0; i < 5000 && !(gpu.poll(g_ctx.Get(), h1, &b, &failed) || failed); ++i) { g_ctx->Flush(); Sleep(1); }
    check(a.size() == skyring::kRingEntries && b.size() == skyring::kRingEntries && gpu.requestCopy(g_ctx.Get()) >= 0, "G5 both polled without waiting, and the buffer is free again");
    // A dispatch with a null view is refused and counts nothing.
    const uint64_t before = gpu.dispatches();
    check(!gpu.dispatch(g_ctx.Get(), 0, 1, nullptr, s2x.outT.srv.Get(), s2x.mvT.srv.Get(), s2x.depthT.srv.Get(), 512, 480, 1024, 960, 0.1f, true) && gpu.dispatches() == before,
          "G6 a dispatch with a missing view is refused and counted as nothing");
    gpu.release();
    check(!gpu.ready() && gpu.dispatches() == 0, "G6 release leaves it unready");
}

// ---- F: the flat scene candidate log -------------------------------------------------------------------------------------------------------------------------
void addTarget(FlatRuntimePrefix& p, uint32_t w, uint32_t h, uint32_t draws, uint32_t format = 26) {
    static int dummy[256];
    const uint32_t i = p.targetsUsed++;
    FlatRuntimeTarget& t = p.targets[i];
    t.resource = &dummy[i];
    auto& k = t.writes.key;
    k.format = format; k.width = w; k.height = h; k.depthWidth = w; k.depthHeight = h;
    k.color = &dummy[i + 100]; k.rtv = &dummy[i + 100]; k.depth = &dummy[i + 150]; k.dsv = &dummy[i + 150];
    t.writes.draws = draws;
}

void caseFlat() {
    auto p = std::make_unique<FlatRuntimePrefix>();
    p->width = 3840; p->height = 2160;
    // The 20261001_103559 flip: Elite at 1440x810 on a 3840x2160 output (off-shape: under half), and the probe's 256x256 with more draws (285 against 120).
    addTarget(*p, 1440, 810, 120);
    addTarget(*p, 256, 256, 285);
    addTarget(*p, 2048, 1024, 40, 26);       // a sun cascade is not an R11G11B10F scene target with its own-size depth here: still listed (it passes the tests)
    addTarget(*p, 640, 360, 5);              // under eight draws: not a candidate
    addTarget(*p, 800, 600, 90, 27);         // not R11G11B10F: not a candidate
    const FlatSceneFacts off = flatSceneFacts(*p, 3840, 2160);
    check(off.offShape && !off.onShape && off.width == 256 && off.height == 256 && off.draws == 285, "F1 the picker is unchanged: the 256x256 probe out-draws the scene and wins as the off-shape candidate (the F8 flip)");
    {   // What the flat counterpart of the rule WOULD do (it is not applied: unflown): drop the tiny square, leave the 1440x810 scene.
        FlatSceneCandidate c0[kFlatSceneMaxCandidates];
        const uint32_t n0 = flatSceneCandidates(*p, 3840, 2160, c0);
        const FlatSceneWould would = flatSceneWould(c0, n0);
        check(would.rejected == 1 && would.width == 1440 && would.height == 810 && would.draws == 120 && !would.onShape,
              "F2 the would-be rule drops the 256x256 and would leave the 1440x810 scene (reported, not applied)");
    }
    addTarget(*p, 2880, 1620, 100);
    const FlatSceneFacts ons = flatSceneFacts(*p, 3840, 2160);
    check(ons.onShape && ons.width == 2880, "F2 an on-shape scene wins, as it did");
    // The rule's edge: a square target under a sixteenth of the output's area (200x200 on 1024x1024) is excluded; at exactly a sixteenth (256x256) it is not.
    auto q = std::make_unique<FlatRuntimePrefix>();
    q->width = 1024; q->height = 1024;
    addTarget(*q, 200, 200, 200);
    check(flatSceneFacts(*q, 1024, 1024).offShape, "F3 a 200x200 target is still the (off-shape) scene: the picker is unchanged");
    {
        FlatSceneCandidate cq[kFlatSceneMaxCandidates];
        const uint32_t nq = flatSceneCandidates(*q, 1024, 1024, cq);
        const FlatSceneWould wq = flatSceneWould(cq, nq);
        check(nq == 1 && cq[0].tinySquare && wq.rejected == 1 && wq.width == 0, "F3 ...and the would-be rule reports it as a tiny square (a sixteenth of the output's area) that would leave no scene");
    }
    check(!flatTinySquare(256, 256, 1024, 1024) && flatTinySquare(255, 255, 1024, 1024) && !flatTinySquare(1440, 810, 3840, 2160), "F3 the edge is exactly a sixteenth, and a non-square target is never tiny");

    // The candidate list: every target that passes, biggest draw count first; tiny squares flagged.
    FlatSceneCandidate c[kFlatSceneMaxCandidates];
    const uint32_t n = flatSceneCandidates(*p, 3840, 2160, c);
    check(n == 4 && c[0].width == 256 && c[0].draws == 285 && c[0].tinySquare && !c[0].onShape && c[1].width == 1440 && c[1].draws == 120 && !c[1].tinySquare &&
          c[2].width == 2880 && c[2].onShape && c[3].draws == 40, "F4 the candidate list: 256x256 (285, tiny square), 1440x810 (120), 2880x1620 (on-shape, 100)... in draw order");
    // The tracker: the first look logs; a winner change logs at once; 5 s logs; a steady winner in between does not.
    FlatSceneLog log;
    bool a = log.note(1000, 1440, 810);
    bool b = log.note(1010, 1440, 810);
    bool d = log.note(1020, 256, 256);
    bool e = log.note(1030, 1440, 810);
    bool f = log.note(1040, 1440, 810);
    bool g = log.note(6100, 1440, 810);
    check(a && !b && d && e && !f && g, "F5 the log fires on the first look, on each change of winner, and every 5 s, and not between");
    check(log.frames == 6 && log.won[0].w == 1440 && log.won[0].frames == 5 && log.won[1].w == 256 && log.won[1].frames == 1, "F5 frames won per size are counted");
    FlatSceneLog flapping;
    unsigned lines = 0;
    for (uint32_t i = 0; i < 1000; ++i) lines += flapping.note(2000 + i, (i & 1) ? 256 : 1440, (i & 1) ? 256 : 810) ? 1 : 0;
    check(lines <= 13 + 1, "F5 a winner that flips every frame cannot fill the log: the change lines are capped in a 30 s window");
    FlatSceneLog none;
    none.note(0, 0, 0);
    none.note(10, 0, 0);
    check(none.noneFrames == 2, "F5 a frame with no scene is counted as none");
    char line[1100];
    const FlatSceneFacts w = flatSceneFacts(*p, 3840, 2160);
    flatSceneFormatLine(line, sizeof(line), 10288, 3840, 2160, w, c, n, log, "the winner changed or the first look");
    check(std::strstr(line, "flat scene candidates (frame 10288, output 3840x2160, the winner changed or the first look): winner 2880x1620 on-shape draws=100") &&
          std::strstr(line, "candidates: 256x256 fmt=26 draws=285 off-shape tiny-square, 1440x810 fmt=26 draws=120 off-shape") && std::strstr(line, "frames won in this window (6 frames): 1440x810:5 256x256:1"),
          "F6 the line names the output, the winner, every candidate with size, format and draws, and the frames won per size");
    check(std::strstr(line, "would_reject=1 (not applied; the winner would not change)") && std::strstr(line, "frames won by a would-be-rejected size: 0") && !std::strstr(line, "fix."),
          "F7 the line says what the unflown rule would drop (1 tiny square), that it is not applied, and that here the winner would not change; no key is named");
    {   // The F8 flip frame: no on-shape candidate, the 256x256 wins; the rule would change the winner, and the window counts the frames a would-be-rejected size won.
        auto flip = std::make_unique<FlatRuntimePrefix>();
        flip->width = 3840; flip->height = 2160;
        addTarget(*flip, 1440, 810, 120);
        addTarget(*flip, 256, 256, 285);
        FlatSceneCandidate cf[kFlatSceneMaxCandidates];
        const uint32_t nf = flatSceneCandidates(*flip, 3840, 2160, cf);
        const FlatSceneFacts wf = flatSceneFacts(*flip, 3840, 2160);
        FlatSceneLog lf;
        lf.note(1000, 1440, 810, false);
        lf.note(1010, wf.width, wf.height, flatTinySquare(wf.width, wf.height, 3840, 2160));
        char fl[1100];
        flatSceneFormatLine(fl, sizeof(fl), 10300, 3840, 2160, wf, cf, nf, lf, "the winner changed");
        check(wf.width == 256 && std::strstr(fl, "winner 256x256 off-shape draws=285") && std::strstr(fl, "would_reject=1 (not applied; the rule would leave 1440x810, the winner changes)") &&
              std::strstr(fl, "frames won by a would-be-rejected size: 1"),
              "F7 on the flip frame the picker still names 256x256 and the line says the rule would leave 1440x810 and counts the frame a would-be-rejected size won");
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--self-test" || a == "--dry-run") selfTest = true;
        else {
            std::fprintf(stderr, "usage: sky_flicker_test --self-test | --dry-run\n");
            return 2;
        }
    }
    if (!selfTest) {
        std::fprintf(stderr, "usage: sky_flicker_test --self-test | --dry-run\n");
        return 2;
    }
    caseEvents();
    caseChoosers();
    caseBurst();
    caseMini();
    caseSky();
    caseFlat();
    caseGpu();
    std::printf("sky_flicker_test: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

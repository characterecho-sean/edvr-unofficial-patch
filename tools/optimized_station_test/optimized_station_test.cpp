// Test rig for the optimized station's pure half (src/d3d11/optimized_station_core.h): the seal trigger (the state machine that seals, releases
// and re-seals the station), the release rules (setting off, undocked, Explorer Cam, the camera leaving the bay, the pad moving against the hangar
// shell, a hangar change, launch), the pad-local frame's round trip and the release's threshold, the parse of the per-size floors, the own-ship
// prefix and sphere, the bay's volume, and the unreadable rule. No DLL and no game.
#include "../../src/d3d11/optimized_station_core.h"
#include "../../src/d3d11/explorer_cam_core.h"
#include "../../src/d3d11/station_gather_core.h"
#include "../../src/d3d11/station_lockfree_core.h"
#include "../../src/d3d11/station_pass_core.h"
#include "../../src/common/flat_profile_keys.h"
#include "../../src/d3d11/menu_flat_rows.h"
#include "../../src/d3d11/view_source.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <random>
#include <thread>
#include <vector>   // the build-332841 PE identity constants (store hook gate)

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace se = edvr::sealed;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// The seal's inputs for a docked station whose own pad the game says is in the hangar (r1 1, fresh), with the camera in the bay.
se::SealInputs inHangar(uint64_t now) {
    se::SealInputs in;
    in.setting = true;
    in.docked = true;
    in.ownKnown = true;
    in.r1Fresh = true;
    in.inHangar = true;
    in.pickOk = true;
    in.cameraInBox = true;
    in.pad = 0x1000;
    in.nowMs = now;
    return in;
}

// Runs the trigger from t0 to t1 every 100 ms with the same inputs; returns the first engage request's time, or 0 when none came.
uint64_t firstEngage(se::SealState* st, se::SealInputs in, uint64_t t0, uint64_t t1) {
    se::ReleaseWhy why = se::ReleaseWhy::kNone;
    for (uint64_t t = t0; t <= t1; t += 100) {
        in.nowMs = t;
        if (se::sealStep(in, st, &why) == se::SealAction::kRequestEngage) return t;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--self-test") != 0) {
        std::printf("usage: optimized_station_test --self-test\n");
        return 2;
    }

    // ---- Built v14.23 (2026-10-13): the seal is the game's pad state (r1 1 on the own pad). The geometric trigger has no tests any more ---------------
    {
        // Load-in: the game says the own pad is in the hangar (r1 1, fresh) and the camera is in the bay: the seal comes on the first eligible frame.
        se::SealState st;
        check(firstEngage(&st, inHangar(0), 100, 10000) == 100, "v14.23 load-in: the own pad in the hangar by the game (r1 1) seals on the first eligible frame (100 ms)");
    }
    {
        // The surface of an outpost: r0 0 and r1 0. The pad never seals (no fallback), and the sun stays on.
        se::SealState st;
        se::SealInputs in = inHangar(0);
        in.inHangar = false;
        check(firstEngage(&st, in, 0, 60000) == 0, "v14.23 surface: r0 0 and r1 0 (outside the hangar) never seals, with no fallback");
        se::SunState sun;
        se::SunInputs si;
        si.setting = true;
        si.docked = true;
        si.ownKnown = true;
        si.r0Fresh = true;
        si.r0 = 0;
        si.r1Fresh = true;
        si.inHangar = false;
        si.relOk = true;
        si.rel = -53.5f;
        const char* why = "";
        check(!se::sunStep(si, &sun, &why) && !sun.off, "v14.23 surface: the sun stays on at the outpost surface (r0 0)");
    }
    {
        // A descent: r0 3 (the descent has started) turns the sun off; the pad is not yet in the bay, so no seal.
        se::SunState sun;
        se::SunInputs si;
        si.setting = true;
        si.docked = true;
        si.ownKnown = true;
        si.r0Fresh = true;
        si.r0 = 3;
        si.r1Fresh = true;
        si.inHangar = false;
        si.relOk = true;
        si.rel = -20.0f;
        const char* why = "";
        check(se::sunStep(si, &sun, &why) && sun.off && std::strcmp(why, "pad moving in or locked in (r0)") == 0,
              "v14.23 descent: r0 3 turns the sun off");
        se::SealState st;
        se::SealInputs in = inHangar(0);
        in.inHangar = false;
        check(firstEngage(&st, in, 0, 10000) == 0, "v14.23 descent: r1 0 during the descent: no seal");
    }
    {
        // The lock: r1 turns 1 (the game says the pad is in the hangar): the seal comes at once; r0 1 turns the sun off.
        se::SealState st;
        check(firstEngage(&st, inHangar(0), 4000, 10000) == 4000, "v14.23 lock: r1 1 on the own pad seals at once (the first eligible frame)");
        se::SunState sun;
        se::SunInputs si;
        si.setting = true;
        si.docked = true;
        si.ownKnown = true;
        si.r0Fresh = true;
        si.r0 = 1;
        si.r1Fresh = true;
        si.inHangar = true;
        const char* why = "";
        check(se::sunStep(si, &sun, &why) && sun.off, "v14.23 lock: r0 1 (moving in) turns the sun off");
    }
    {
        // Toggle off and on while r1 is 1: the bay re-seals at once (no edge is needed for a setting release).
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(100);
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "v14.23 toggle: the seal in the hangar");
        in.held = true;
        in.setting = false;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kSettingOff, "v14.23 toggle: the setting off releases");
        in.held = false;
        in.setting = true;
        in.nowMs = 200;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "v14.23 toggle: the setting on again, with r1 1, re-seals at once");
    }
    {
        // A mechanism release (the launch) while r1 stays 1: no re-seal for 2 s of r1 1; r1 0 then 1 (the fresh edge) re-seals.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "v14.23 mechanism: the setup seal");
        in.held = true;
        in.mechanismMoved = true;
        in.nowMs = 100;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kMechanism, "v14.23 mechanism: the bay mechanism moving releases");
        in.held = false;
        in.mechanismMoved = false;
        uint64_t early = 0;
        for (uint64_t t = 200; t <= 2500; t += 100) {
            in.nowMs = t;
            if (se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage && early == 0) early = t;
        }
        check(early == 0, "v14.23 mechanism: with r1 still 1 after the launch release, nothing re-seals (2 s)");
        in.inHangar = false;
        in.nowMs = 2600;
        check(se::sealStep(in, &st, &w) == se::SealAction::kNone, "v14.23 mechanism: r1 0 is the first half of the fresh edge (no seal)");
        in.inHangar = true;
        in.nowMs = 2700;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "v14.23 mechanism: r1 back to 1 after the 0 re-seals");
    }
    {
        // An r1 1 to 0 edge (the pad leaves the bay) releases the seal.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.inHangar = false;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kInHangarLeft, "v14.23 edge: r1 1 to 0 releases the seal");
    }
    {
        // The camera leaves and re-enters the bay: the seal re-takes without any in-hangar edge (r1 stayed 1).
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.cameraOutside = true;
        bool left = false;
        for (uint64_t t = 100; t <= 1000; t += 100) {
            in.nowMs = t;
            if (se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kCameraLeft) left = true;
        }
        check(left, "v14.23 camera: the camera outside the box for kCameraFrames releases the seal");
        in.held = false;
        in.cameraOutside = false;
        in.cameraInBox = false;
        in.nowMs = 1100;
        se::sealStep(in, &st, &w);
        uint64_t back = 0;
        for (uint64_t t = 1200; t <= 4000; t += 100) {
            in.cameraInBox = true;
            in.nowMs = t;
            if (se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage && back == 0) back = t;
        }
        check(back != 0, "v14.23 camera: the camera back in the box re-seals with r1 still 1 (no edge needed)");
    }
    {
        // A request made for one pad is never admitted for another; the trigger asks again for the new pad.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs a = inHangar(0);
        a.pad = 0x1000;
        se::sealStep(a, &st, &w);
        se::EngageRequest reqA{true, 0x1000, 0x5000};
        se::SealInputs b = inHangar(100);
        b.pad = 0x2000;
        const bool askedB = se::sealStep(b, &st, &w) == se::SealAction::kRequestEngage;
        se::engageTrigger(&reqA, st);
        const se::EngageRequest reqB{true, 0x2000, 0x5000};
        check(askedB && !se::engageAdmitted(reqA, st, 0x2000, 0x5000, true) && se::engageAdmitted(reqB, st, 0x2000, 0x5000, true),
              "v14.23 engage: a request bound to pad A is never admitted for pad B; the new request for B is");
    }
    {
        // The own pad is unknown (no r0 on this dock): no seal, and the waiting line says so.
        se::SealState st;
        se::SealInputs in = inHangar(0);
        in.ownKnown = false;
        check(firstEngage(&st, in, 0, 10000) == 0, "v14.23 unknown: no own pad from the game (no r0 this dock): no seal");
        se::GameView w;
        w.dockedKnown = true;
        w.docked = true;
        w.cameraKnown = true;
        w.cameraIn = true;
        w.cellBuilt = true;
        char line[480] = {};
        se::gameWaitingLine(w, line, sizeof(line));
        check(std::strstr(line, "game pad state unknown") != nullptr, "v14.23 unknown: the waiting line says game pad state unknown");
        se::GameView k = w;
        k.ownKnown = true;
        k.own = 0xABC40;
        k.r0 = 1;
        k.r1 = 1;
        k.r0Fresh = true;
        k.r1Fresh = true;
        se::gameWaitingLine(k, line, sizeof(line));
        check(std::strstr(line, "own pad @0abc40, r0=1 r1=1, in hangar=yes") != nullptr, "v14.23 waiting line: the own pad's key and codes, and in hangar yes");
    }
    {
        // The own pad: the object r0 was last called on this dock; a reset at undock or a hangar change; an r0 from before the reset names nothing.
        se::GameTrack tr;
        check(tr.observe(false, 0, 0) == se::GameTrackEvent::kNone && !tr.have, "v14.23 own: no r0 yet: no own pad");
        check(tr.observe(true, 0xA000, 100) == se::GameTrackEvent::kFirst && tr.own == 0xA000, "v14.23 own: the first r0 this dock names the own pad");
        check(tr.observe(true, 0xA000, 200) == se::GameTrackEvent::kNone, "v14.23 own: the same object again: no event");
        check(tr.observe(true, 0xB000, 300) == se::GameTrackEvent::kSwitched && tr.own == 0xB000,
              "v14.23 own: r0 on another object switches the own pad (one event)");
        tr.reset(400);
        check(!tr.have, "v14.23 own: undock or a hangar change resets the own pad");
        check(tr.observe(true, 0xB000, 350) == se::GameTrackEvent::kNone && !tr.have,
              "v14.23 own: an r0 call from before the reset names no own pad");
        check(tr.observe(true, 0xB000, 450) == se::GameTrackEvent::kFirst, "v14.23 own: an r0 call after the reset names the own pad");
    }
    {
        // The sun after a lock: r0 1 is off; the pad in the bay (r1 1) holds it; out and raised turns it on; undock turns it on.
        se::SunState sun;
        se::SunInputs si;
        si.setting = true;
        si.docked = true;
        si.ownKnown = true;
        si.r0Fresh = true;
        si.r0 = 1;
        si.r1Fresh = true;
        si.inHangar = true;
        si.relOk = true;
        si.rel = -45.0f;
        const char* why = "";
        check(se::sunStep(si, &sun, &why) && sun.off, "v14.23 sun: r0 1 turns the skip on");
        si.r0 = 0;
        check(!se::sunStep(si, &sun, &why) && sun.off, "v14.23 sun: the pad still in the bay (r1 1) holds the skip after r0 stops");
        si.inHangar = false;
        si.rel = 5.0f;
        check(se::sunStep(si, &sun, &why) && !sun.off && std::strcmp(why, "pad raised (launch)") == 0,
              "v14.23 sun: the pad out of the bay (r1 0, r0 not 1 or 3) and raised turns the sun back on");
        si.r0 = 1;
        si.inHangar = false;
        si.rel = -45.0f;
        check(se::sunStep(si, &sun, &why) && sun.off, "v14.23 sun: moving again turns it off");
        si.docked = false;
        check(se::sunStep(si, &sun, &why) && !sun.off && std::strcmp(why, "undocked") == 0, "v14.23 sun: undock turns it on");
    }
    // The pure helpers that are not about the seal trigger (kept): the latch's identity, the bounded probe, the rig's epoch rule, the view's centre.
    {
        se::LatchIdentity id;
        id.bind(0xB0, 0x5000);
        check(!id.step(false, 0, true, 0x5000), "v14.20 latch: a sample that misses the pad keeps the identity: no change");
        check(id.step(true, 0xA0, true, 0x5000) && id.pad == 0xA0, "v14.20 latch: pad B replaced by pad A is a genuine change: rebuild");
        se::LatchSet ls;
        ls.n = 2;
        ls.latched = true;
        ls.recs[0].movedRun = 2;
        ls.recs[1].movedRun = 3;
        ls.reset();
        check(ls.movedIndex() == -1 && ls.recs[1].movedRun == 0, "v14.20 latch: a reset clears the records' motion references");
        const uint8_t outside = static_cast<uint8_t>(se::RigVerdict::kOutside);
        check(se::rigMayVeto(3, 3, true, outside) && !se::rigMayVeto(3, 4, true, outside) && !se::rigMayVeto(4, 4, false, outside),
              "v14.20 rig: an outside bound from the current epoch may veto; an old epoch or an unbounded slot forwards");
        check(se::viewCentreReady(true, false, false) && !se::viewCentreReady(false, true, false) && se::viewCentreReady(false, true, true),
              "v14.20 view: the origin is the centre; an eye pose alone is not");
        se::SealState s2;
        check(s2.reseal == 0 && !s2.requested, "v14.23: a fresh trigger holds no re-seal and requests nothing");
    }
    {
        // The bounded probe: at the bound, a full run is exhausted (no walk of the table); a stale slot is free.
        struct Slot {
            uintptr_t key = 0;
            uint32_t gen = 0;
        };
        constexpr size_t kCap = 64;
        Slot table[kCap] = {};
        uint32_t cur = 1;
        auto matchesKey = [&](size_t i, uintptr_t key) { return table[i].gen == cur && table[i].key == key; };
        auto staleSlot = [&](size_t i) { return table[i].gen != cur; };
        for (uintptr_t k = 1; k <= se::kTableProbeMax; ++k) {
            const se::ProbeResult p = se::boundedProbe(0, kCap, se::kTableProbeMax, [&](size_t i) { return matchesKey(i, k); }, staleSlot);
            if (p.empty) {
                table[p.index].key = k;
                table[p.index].gen = cur;
            }
        }
        const se::ProbeResult miss = se::boundedProbe(0, kCap, se::kTableProbeMax, [&](size_t i) { return matchesKey(i, 99); }, staleSlot);
        check(miss.exhausted && miss.probes == se::kTableProbeMax, "v14.20 table: at capacity a missing key is a miss after the bound (32 probes)");
        cur = 2;
        const se::ProbeResult fresh = se::boundedProbe(0, kCap, se::kTableProbeMax, [&](size_t i) { return matchesKey(i, 99); }, staleSlot);
        check(fresh.empty && fresh.probes == 1, "v14.20 table: a slot of an earlier epoch is free");
    }
    {
        // The store hook's identity decision (kept): the PE pair first, then the pin.
        const uint8_t pin[16] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x68, 0xFE, 0xFF};
        const uint32_t wantStamp = edvr::ecm::kExpectedTimestamp, wantSize = edvr::ecm::kExpectedImageSize;
        check(se::storeInstallDecision(true, wantStamp, wantSize, wantStamp, wantSize, pin, pin, 16) == se::StoreInstall::kInstall,
              "v14.20 store hook: build 332841's identity and pin install the detour");
        check(se::storeInstallDecision(true, wantStamp + 1, wantSize, wantStamp, wantSize, pin, pin, 16) == se::StoreInstall::kWrongIdentity,
              "v14.20 store hook: another stamp is refused");
    }
    {
        // The shell frame (kept): the pad 3 m along x and 4 m along y from the shell, 45 m below it; forward +y against +x: yaw +90 deg about up.
        const float shellO[3] = {0.0f, 0.0f, 0.0f};
        const float upZ[3] = {0.0f, 0.0f, 1.0f};
        const float shFwdX[3] = {1.0f, 0.0f, 0.0f};
        const float padP[3] = {3.0f, 4.0f, -45.0f};
        const float padFwdY[3] = {0.0f, 1.0f, 0.0f};
        const se::ShellFrame sf = se::shellFrameOf(padP, shellO, upZ, shFwdX, padFwdY);
        check(sf.ok && std::fabs(sf.z + 45.0f) < 1e-4f && std::fabs(sf.lateral - 5.0f) < 1e-4f && sf.yawOk && std::fabs(sf.yawDeg - 90.0f) < 1e-3f,
              "v14.17 shell frame: z is the rel reading, lateral is the distance from the shaft, and yaw is +90 deg");
    }

    // ---- Built v14: the own pad's near-tie and its stickiness (fix 1); the shell named once, never a gate, ramp or skirt (fix 2) ----------------
    {
        se::PadObs pa{};
        pa.addr = 0x1000;
        pa.centre[0] = -20.0f;
        pa.radius = 30.0f;
        pa.size = se::PadSize::kMedium;
        pa.up[2] = -1.0f;
        pa.upOk = true;
        std::snprintf(pa.name, sizeof(pa.name), "%s", "LandingPadMediumA");
        se::PadObs pb{};
        pb.addr = 0x2000;
        pb.centre[0] = 20.0f;
        pb.radius = 30.0f;
        pb.size = se::PadSize::kMedium;
        pb.up[2] = 1.0f;
        pb.upOk = true;
        std::snprintf(pb.name, sizeof(pb.name), "%s", "LandingPadMediumB");
        const float radii[3] = {80.0f, 120.0f, 145.0f};
        const float camMid[3] = {0.0f, 0.0f, 0.0f};    // 20 m from each: an exact tie
        const float camB[3] = {10.0f, 0.0f, 0.0f};     // B 10 m, A 30 m: not a tie
        const float camNearB[3] = {4.0f, 0.0f, 0.0f};  // B 16 m, A 24 m: B nearer by 8 m, not a tie
        se::PadObs ordAB[2] = {pa, pb};
        se::PadObs ordBA[2] = {pb, pa};
        se::OwnCell c1, c2, c3;
        se::pickOwnPad(ordAB, 2, camMid, radii, &c1);
        se::pickOwnPad(ordBA, 2, camMid, radii, &c2);
        check(c1.has && c1.pad == 0x1000 && c2.has && c2.pad == 0x1000, "fix 1: a tie within 5 m goes to the platform named A, whatever the record order");
        se::pickOwnPad(ordAB, 2, camB, radii, &c3);
        check(c3.has && c3.pad == 0x1000, "v14.8: the B platform is not picked while the A platform of its size is within 60 m (the nearer B loses)");

        se::OwnPadSticky st;
        se::OwnCell s;
        se::pickOwnPadSticky(ordAB, 2, camMid, radii, &st, &s);
        check(s.has && s.pad == 0x1000, "fix 1: the first pick, from the tie, is A");
        se::pickOwnPadSticky(ordBA, 2, camNearB, radii, &st, &s);
        check(s.has && s.pad == 0x1000, "fix 1: sticky: A stays the own pad while its record is seen, though B is now nearer");
        se::PadObs onlyB[1] = {pb};
        bool anyPick = false;
        for (int f = 1; f <= 29; ++f) {
            se::pickOwnPadSticky(onlyB, 1, camNearB, radii, &st, &s);
            if (s.has) anyPick = true;
        }
        check(!anyPick, "fix 1: sticky: A unseen for 29 frames: no other pad is picked yet");
        se::pickOwnPadSticky(onlyB, 1, camNearB, radii, &st, &s);
        check(s.has && s.pad == 0x2000, "fix 1: sticky: A unseen for kHeldLostFrames (30): B takes over");

        se::ExplicitObs ex[4] = {};
        ex[0].addr = 1;
        ex[0].centre[0] = 2.0f;
        std::snprintf(ex[0].name, sizeof(ex[0].name), "%s", "MediumHangerSkirt");
        ex[1].addr = 2;
        ex[1].centre[0] = 3.0f;
        std::snprintf(ex[1].name, sizeof(ex[1].name), "%s", "HangarGate_Ramp1_1");
        ex[2].addr = 3;
        ex[2].centre[0] = 40.0f;
        std::snprintf(ex[2].name, sizeof(ex[2].name), "%s", "HangerMedium");
        ex[3].addr = 4;
        ex[3].centre[0] = 60.0f;
        std::snprintf(ex[3].name, sizeof(ex[3].name), "%s", "HangerLarge");
        const float padC[3] = {0.0f, 0.0f, 0.0f};
        check(!se::shellNameUsable("MediumHangerSkirt") && !se::shellNameUsable("HangarGate_Ramp1_1") && se::shellNameUsable("HangerMedium"),
              "fix 2: a skirt, a gate and a ramp are never the shell; a hanger is");
        check(se::pickShell(ex, 4, padC, se::PadSize::kMedium, 150.0f) == 2, "fix 2: the medium pad's HangerMedium, though the skirt and the gate are nearer");
        check(se::pickShell(ex, 4, padC, se::PadSize::kSmall, 150.0f) == 2, "fix 2: a small pad with no HangerSmall takes the nearest other sized hanger");
        check(se::pickShell(ex, 2, padC, se::PadSize::kMedium, 150.0f) == 2, "fix 2: with only the skirt and the gate there is no shell (n)");
    }


    // ---- Built v14.8: the lift platform B, the cell validity, the ship part count, the still clock across a source change ----------------
    {
        se::PadObs pa{};
        pa.addr = 0x1000;
        pa.centre[0] = -20.0f;
        pa.radius = 30.0f;
        pa.size = se::PadSize::kMedium;
        std::snprintf(pa.name, sizeof(pa.name), "%s", "LandingPadMediumA");
        se::PadObs pb{};
        pb.addr = 0x2000;
        pb.centre[0] = 20.0f;
        pb.radius = 30.0f;
        pb.size = se::PadSize::kMedium;
        std::snprintf(pb.name, sizeof(pb.name), "%s", "LandingPadMediumB");
        const float radii[3] = {80.0f, 120.0f, 145.0f};
        const float camB[3] = {10.0f, 0.0f, 0.0f};   // B 10 m, A 30 m
        se::PadObs ordAB[2] = {pa, pb};
        se::OwnCell c;
        se::pickOwnPad(ordAB, 2, camB, radii, &c);
        check(c.has && c.pad == 0x1000, "v14.8: the nearer B platform loses to the A platform of its size within 60 m");
        se::PadObs farA = pa;
        farA.centre[0] = -80.0f;   // A 90 m from the camera
        se::PadObs ordFar[2] = {farA, pb};
        se::pickOwnPad(ordFar, 2, camB, radii, &c);
        check(c.has && c.pad == 0x2000, "v14.8: beyond 60 m the A platform does not count: B is picked");
        se::OwnPadSticky st;
        se::OwnCell s;
        se::PadObs onlyB[1] = {pb};
        se::pickOwnPadSticky(onlyB, 1, camB, radii, &st, &s);
        check(s.has && s.pad == 0x2000, "v14.8 sticky: B picked while no A is in the store");
        se::pickOwnPadSticky(ordAB, 2, camB, radii, &st, &s);
        check(s.has && s.pad == 0x1000, "v14.8 sticky: a held B gives way to the A platform within 60 m at once");

        se::OwnCell none;
        check(se::cellInvalidWhy(none) != nullptr, "v14.8 cell: no pad, no seal");
        se::OwnCell noTarget;
        noTarget.has = true;
        check(se::cellInvalidWhy(noTarget) != nullptr, "v14.8 cell: a pad with no shaft target does not seal");
        se::OwnCell zero = noTarget;
        zero.capsule = true;
        zero.box = true;   // the zero box the flight sealed with
        check(se::cellInvalidWhy(zero) != nullptr, "v14.8 cell: a zero box never seals (the flight's 0 x 0 x 0)");
        se::OwnCell ok = zero;
        ok.boxHalf[0] = ok.boxHalf[1] = ok.boxHalf[2] = 12.0f;
        check(se::cellInvalidWhy(ok) == nullptr, "v14.8 cell: a box with every half-extent at least 10 m is a valid cell");
        se::OwnCell thin = ok;
        thin.boxHalf[1] = 5.0f;
        check(se::cellInvalidWhy(thin) != nullptr, "v14.8 cell: a box under 10 m on any axis does not seal");
    }
    {
        // The ship's part count: a real ship has many records near the camera; a boarding volume or a GUI record does not.
        check(se::shipPrefixNamesShip("LakonMiner_", 10), "v14.8 ship: a prefix with ten records near the camera names a ship");
        check(!se::shipPrefixNamesShip("LakonMiner_", 9), "v14.8 ship: nine records are not enough");
        check(!se::shipPrefixNamesShip("ShipBoardingVolume_", 50), "v14.8 ship: a BoardingVolume prefix never names a ship");
        check(!se::shipPrefixNamesShip("GUI_", 50) && !se::shipPrefixNamesShip("GUI_Panel", 50), "v14.8 ship: a GUI_ prefix never names a ship");
        check(!se::shipPrefixNamesShip("", 50) && !se::shipPrefixNamesShip(nullptr, 50), "v14.8 ship: no prefix, no ship");
    }

    // ---- Built v14.10: the bay settles (the watch's motion, the continuous movers, the fallback, the frame shift) --------------------------------
    {
        // The watch's motion test: a frame shift of the record and the shell together is not motion; a rotation over 0.5 deg is; so is a 3 cm move.
        const float a0[3] = {1.0f, 0.0f, 0.0f};
        const float a2[3] = {0.0f, 0.0f, 1.0f};
        const float prev[3] = {5.0f, 6.0f, -45.0f};
        check(!se::poseMoved(prev, prev, a0, a0, a2, a2), "v14.10 watch: a record that does not move is not moved");
        const float v[3] = {123.0f, -40.0f, 900.0f};
        const float shell[3] = {0.0f, 0.0f, 0.0f};
        const float recC[3] = {5.0f, 6.0f, -45.0f};
        const float shellS[3] = {shell[0] + v[0], shell[1] + v[1], shell[2] + v[2]};
        const float recS[3] = {recC[0] + v[0], recC[1] + v[1], recC[2] + v[2]};
        const float rel0[3] = {recC[0] - shell[0], recC[1] - shell[1], recC[2] - shell[2]};
        const float rel1[3] = {recS[0] - shellS[0], recS[1] - shellS[1], recS[2] - shellS[2]};
        check(!se::poseMoved(rel0, rel1, a0, a0, a2, a2), "v14.10 watch: a frame shift of the record and the shell together is not motion");
        const float tilt1[3] = {std::cos(1.0f * 0.0174533f), std::sin(1.0f * 0.0174533f), 0.0f};
        const float tilt03[3] = {std::cos(0.3f * 0.0174533f), std::sin(0.3f * 0.0174533f), 0.0f};
        check(se::poseMoved(prev, prev, a0, tilt1, a2, a2), "v14.10 watch: a 1 deg rotation of an axis is motion");
        check(!se::poseMoved(prev, prev, a0, tilt03, a2, a2), "v14.10 watch: a 0.3 deg rotation is not motion");
        const float near3[3] = {prev[0] + 0.02f, prev[1], prev[2]};
        const float far3[3] = {prev[0] + 0.05f, prev[1], prev[2]};
        check(!se::poseMoved(prev, near3, a0, a0, a2, a2) && se::poseMoved(prev, far3, a0, a0, a2, a2),
              "v14.10 watch: a 2 cm centre move is not motion; a 5 cm move is");
    }

    // ---- Built v14.12: the latch (selection, cap, the by-address still test), the lift sequence, the held pad absence ----------------------
    {
        // Selection: the own pad (A, medium, at the origin); the other medium pad 40 m lateral; a small pad 10 m lateral (no); a medium pad 70 m lateral
        // (no); a Lift within the span (yes); a Lift beyond the span (no); a BlastShieldBase (yes); a HangarGate_Ramp (yes); a container (no); a guide
        // marker pad (no).
        se::LatchCand cands[12] = {};
        auto put = [](se::LatchCand& cd, uintptr_t addr, const char* nm, float x, float y, float z) {
            cd.addr = addr;
            std::snprintf(cd.name, sizeof(cd.name), "%s", nm);
            cd.centre[0] = x;
            cd.centre[1] = y;
            cd.centre[2] = z;
        };
        put(cands[0], 0x2, "LandingPadMediumB", 0.0f, 40.0f, -20.0f);
        put(cands[1], 0x3, "LandingPadSmallA", 10.0f, 0.0f, 0.0f);
        put(cands[2], 0x4, "LandingPadMediumB", 70.0f, 0.0f, 0.0f);
        put(cands[3], 0x5, "Lift_1", 20.0f, 0.0f, -30.0f);
        put(cands[4], 0x6, "Lift_2", 20.0f, 0.0f, 100.0f);
        put(cands[5], 0x7, "BlastShieldBase", 0.0f, 30.0f, -10.0f);
        put(cands[6], 0x8, "HangarGate_Ramp1_0", 50.0f, 0.0f, -5.0f);
        put(cands[7], 0x9, "Container_1", 5.0f, 0.0f, -5.0f);
        put(cands[8], 0xA, "LandingPadMediumGuideMarkers", 5.0f, 0.0f, 0.0f);
        const float origin[3] = {0.0f, 0.0f, 0.0f};
        const float up[3] = {0.0f, 0.0f, 1.0f};
        se::LatchRec out[se::kLatchCap];
        const size_t k = se::selectMechanism(cands, 9, 0x1, "LandingPadMediumA", se::PadSize::kMedium, origin, up, -90.0f, 40.0f, out);
        bool hasB = false, hasLift1 = false, hasShield = false, hasRamp = false, hasLift2 = false, hasSmall = false;
        for (size_t i = 0; i < k; ++i) {
            hasB = hasB || std::strcmp(out[i].name, "LandingPadMediumB") == 0 && out[i].addr == 0x2;
            hasLift1 = hasLift1 || std::strcmp(out[i].name, "Lift_1") == 0;
            hasShield = hasShield || std::strcmp(out[i].name, "BlastShieldBase") == 0;
            hasRamp = hasRamp || std::strcmp(out[i].name, "HangarGate_Ramp1_0") == 0;
            hasLift2 = hasLift2 || std::strcmp(out[i].name, "Lift_2") == 0;
            hasSmall = hasSmall || std::strcmp(out[i].name, "LandingPadSmallA") == 0;
        }
        check(k == 5 && out[0].addr == 0x1 && hasB && hasLift1 && hasShield && hasRamp && !hasLift2 && !hasSmall,
              "v14.12 latch: the own pad, the medium B 40 m lateral, Lift_1, BlastShieldBase, HangarGate_Ramp within the span; not the small pad, the far pad, the lift beyond the span, the container or the guide markers");
        se::LatchCand many[40] = {};
        for (int i = 0; i < 30; ++i) {
            char nm[48] = {};
            std::snprintf(nm, sizeof(nm), "Lift_%d", i);
            put(many[i], static_cast<uintptr_t>(0x100 + i), nm, 5.0f, 0.0f, 0.0f);
        }
        const size_t kc = se::selectMechanism(many, 30, 0x1, "LandingPadMediumA", se::PadSize::kMedium, origin, up, -90.0f, 40.0f, out);
        check(kc == se::kLatchCap, "v14.12 latch: at most kLatchCap (16) records: the own pad and fifteen others");
    }
    {
        // By address: a still record is not moved; 5 cm is; a missing frame re-bases without counting; a frame shift of the record and the shell
        // together is not motion (the relative centre is the same).
        const float a0[3] = {1.0f, 0.0f, 0.0f};
        const float a2[3] = {0.0f, 0.0f, 1.0f};
        const float r0[3] = {1.0f, 2.0f, -45.0f};
        const float r1[3] = {1.0f, 2.05f, -45.0f};
        se::LatchRec lr;
        lr.step(10, r0, a0, a2, 1, 1, 1000);
        check(!lr.step(11, r0, a0, a2, 1, 1, 1100) && lr.lastMovedMs == 0, "v14.12 by address: a still record is not moved");
        check(lr.step(12, r1, a0, a2, 1, 1, 1200) && lr.lastMovedMs == 1200, "v14.12 by address: a 5 cm move is a move");
        check(!lr.step(14, r0, a0, a2, 1, 1, 1400) && lr.lastMovedMs == 1200, "v14.12 by address: a missing frame re-bases without counting");
        const float v[3] = {123.0f, -40.0f, 900.0f};
        const float shell[3] = {0.0f, 0.0f, 0.0f};
        const float rec[3] = {5.0f, 6.0f, -45.0f};
        const float relA[3] = {rec[0] - shell[0], rec[1] - shell[1], rec[2] - shell[2]};
        const float relB[3] = {rec[0] + v[0] - (shell[0] + v[0]), rec[1] + v[1] - (shell[1] + v[1]), rec[2] + v[2] - (shell[2] + v[2])};
        se::LatchRec fs;
        fs.step(1, relA, a0, a2, 1, 1, 100);
        check(!fs.step(2, relB, a0, a2, 1, 1, 200) && fs.lastMovedMs == 0, "v14.12 by address: a frame shift of the record and the shell together is not motion");
    }

    // ---- Built v14.16: a release by motion waits the lowered pad and the stillness; the mechanism set (no TurboLift, doors or decals; the cap) ----
    {
        // The mechanism set: no TurboLift*, doors or decals; Lift and Lift_* yes, LiftDoor no; the cap of 16 with the refusals logged.
        check(se::latchClass("TurboLiftDoorLeft") < 0 && se::latchClass("TurboLift_Blocker") < 0 && se::latchClass("DecalB") < 0 &&
                  se::latchClass("LiftDoor") < 0 && se::latchClass("Lift") == 2 && se::latchClass("Lift_3") == 2 && se::latchClass("BlastShieldBase") == 2 &&
                  se::latchClass("HangarGate_Ramp1_0") == 3 && se::latchClass("LandingPadMediumB") == 1,
              "v14.16 set: TurboLift, doors and decals are never mechanism records; Lift and Lift_* are; the pads are the first class");
        se::LatchCand many[40] = {};
        for (int i = 0; i < 30; ++i) {
            char nm[48] = {};
            std::snprintf(nm, sizeof(nm), "Lift_%d", i);
            many[i].addr = static_cast<uintptr_t>(0x300 + i);
            std::snprintf(many[i].name, sizeof(many[i].name), "%s", nm);
            many[i].centre[0] = 5.0f;
        }
        const float origin[3] = {0.0f, 0.0f, 0.0f};
        const float up[3] = {0.0f, 0.0f, 1.0f};
        se::LatchRec out[se::kLatchCap];
        se::LatchReject rej[10];
        size_t rejN = 0;
        const size_t kc = se::selectMechanism(many, 30, 0x1, "LandingPadMediumA", se::PadSize::kMedium, origin, up, -85.0f, 300.0f, out, rej, 10, &rejN);
        check(kc == se::kLatchCap && rejN == 10 && std::strstr(rej[0].why, "cap") != nullptr,
              "v14.16 set: at the cap (16) the rest are refused with 'over the latch cap' (the first ten logged)");
        se::LatchCand door[2] = {};
        std::snprintf(door[0].name, sizeof(door[0].name), "%s", "TurboLiftDoorLeft");
        door[0].addr = 0x401;
        std::snprintf(door[1].name, sizeof(door[1].name), "%s", "DecalB");
        door[1].addr = 0x402;
        size_t rejN2 = 0;
        const size_t kd = se::selectMechanism(door, 2, 0x1, "LandingPadMediumA", se::PadSize::kMedium, origin, up, -85.0f, 300.0f, out, rej, 10, &rejN2);
        check(kd == 1 && rejN2 == 2, "v14.16 set: the elevator doors and decals are refused, the set is the own pad only");
    }

    // ---- Built v14.15: the load-in latch, the reject log, the bay's span, and the own pad's 3D move while sealed ----------------------------
    {
        // The span: a BlastShield 268 m along the shaft with no shaft target built (capLen 0) is kept; one 70 m lateral is refused with its reason.
        se::LatchCand cands[3] = {};
        std::snprintf(cands[0].name, sizeof(cands[0].name), "%s", "BlastShield_A");
        cands[0].addr = 0x21;
        cands[0].centre[0] = 41.0f;
        cands[0].centre[2] = 268.0f;
        std::snprintf(cands[1].name, sizeof(cands[1].name), "%s", "BlastShieldBase_B");
        cands[1].addr = 0x22;
        cands[1].centre[0] = 70.0f;
        cands[1].centre[2] = 100.0f;
        const float origin[3] = {0.0f, 0.0f, 0.0f};
        const float up[3] = {0.0f, 0.0f, 1.0f};
        se::LatchRec out[se::kLatchCap];
        se::LatchReject rej[10];
        size_t rejN = 0;
        const float span = std::max(0.0f + 10.0f, se::kLatchSpanM);
        const size_t k = se::selectMechanism(cands, 1, 0x1, "LandingPadMediumA", se::PadSize::kMedium, origin, up, -85.0f, span, out, rej, 10, &rejN);
        check(k == 2 && rejN == 0 && std::strcmp(out[1].name, "BlastShield_A") == 0,
              "v14.15: a BlastShield 41 m lateral and 268 m along (no target built yet) is latched");
        size_t rejN2 = 0;
        se::selectMechanism(cands + 1, 1, 0x1, "LandingPadMediumA", se::PadSize::kMedium, origin, up, -85.0f, span, out, rej, 10, &rejN2);
        check(rejN2 == 1 && std::strstr(rej[0].why, "lateral") != nullptr && std::strcmp(rej[0].name, "BlastShieldBase_B") == 0,
              "v14.15: a BlastShield 70 m lateral is refused, with its reason logged");
    }


    // ---- Built v14.13: the sun inside the hangar (its own state), the hangar watch, the no-hangar pad, the names --------------------------
    {
        // A docked pad with no sized shell: the note is due once; the give-up after kNoShellGiveUpFrames; a shell resets the count; undock starts over.
        se::HangarWatch hw;
        int notes = 0;
        for (uint32_t k = 0; k < se::kHangarNoteFrames + 20; ++k) notes += hw.step(false, true, false) ? 1 : 0;
        check(notes == 1 && hw.logged && !hw.giveUp, "v14.13 hangar: the no-hangar note is due once per dock, at 60 frames");
        for (uint32_t k = 0; k < se::kNoShellGiveUpFrames; ++k) hw.step(false, true, false);
        check(hw.giveUp, "v14.13 hangar: after 300 frames without a shell the gather of the shell parts stops");
        se::HangarWatch found;
        for (uint32_t k = 0; k < 100; ++k) found.step(false, true, false);
        found.step(true, true, false);
        check(found.missing == 0 && !found.giveUp, "v14.13 hangar: a shell found resets the count (no give-up)");
        found.step(false, false, false);
        check(!found.logged && found.missing == 0, "v14.13 hangar: undock starts the dock over (the note is due again)");
    }
    {
        // Names: the hangar-like filter (hangars, pads, lifts, doors and bays), not a plain structure.
        check(se::hangarLikeName("HangerSmall") && se::hangarLikeName("LandingPadSmallA") && se::hangarLikeName("Elevator_1") &&
                  se::hangarLikeName("BayDoor_2") && !se::hangarLikeName("Container_1"),
              "v14.13 names: hangar, pad, elevator and bay names are noted; a container is not");
        // A part with no size word is not the shell (outposts and carriers have none of the sized kind).
        se::ExplicitObs ex[1] = {};
        ex[0].addr = 1;
        std::snprintf(ex[0].name, sizeof(ex[0].name), "%s", "HangarXYZ");
        const float centre[3] = {0.0f, 0.0f, 0.0f};
        check(se::pickShell(ex, 1, centre, se::PadSize::kMedium, 150.0f) == 1,
              "v14.13 shell: a hangar part with no size word is not the reference");
    }


    // ---- Built v14.6: a ship change (the own ship\'s type prefix) resets the trigger; a flicker is not a change -----------------------------
    {
        se::ShipWatch w;
        check(!w.step("Anaconda"), "v14.6: the first reading sets the own ship's prefix: no change");
        bool any = false;
        for (int k = 0; k < 20; ++k) any = any || w.step("Anaconda");
        check(!any, "v14.6: the same prefix every tick is no change");
        any = false;
        for (int k = 0; k < 9; ++k) any = any || w.step("Cobra");
        check(!any, "v14.6: a different prefix for nine readings is not yet a ship change");
        check(!w.step("Anaconda") && std::strcmp(w.prefix, "Anaconda") == 0, "v14.6: a flicker back to the old prefix resets the count");
        any = false;
        for (int k = 0; k < 9; ++k) any = any || w.step("Asp");
        check(!any, "v14.6: nine readings of the new prefix: still not a change");
        check(w.step("Asp") && std::strcmp(w.prefix, "Asp") == 0, "v14.6: the tenth reading of the new prefix is the ship change (the prefix is now Asp)");
        check(!w.step(nullptr) && !w.step(""), "v14.6: a missing reading is no change");
    }


    // ---- Built v14.4: the flicker fix (the reference, the hysteresis, the sequence after a motion release, the safety net) ----------------------
    {
        // (1) A static pad, its reference read from one source. A later read of the same pad from another source is a different centre (a constant
        // offset in the two computations): it is not compared, and it never releases. The same source, unchanged, never releases.
        se::ShellMotion sm;
        const float up[3] = {0.0f, 0.0f, 1.0f};
        const float shell[3] = {0.0f, 0.0f, 0.0f};
        const float padAt[3] = {0.0f, 0.0f, 10.0f};
        const float padOff[3] = {0.0f, 0.0f, 7.6f};   // the other source reads 2.4 m lower: the observed -2.4 m constant
        check(!sm.sample(padAt, 1, shell, 2, up) && sm.captured, "v14.4: the reference is captured at the first sample");
        bool released = false;
        for (int k = 0; k < 200; ++k) released = released || sm.sample(padAt, 1, shell, 2, up);
        check(!released && sm.delta == 0.0f, "v14.4: a static pad read from the same source never releases (200 samples)");
        released = false;
        for (int k = 0; k < 200; ++k) released = released || sm.sample(padOff, 0, shell, 2, up);
        check(!released, "v14.4: a static pad read from another source (a constant -2.4 m computation difference) never releases");
    }
    {
        // (2) Hysteresis: a pad 3 m off its reference releases only on the tenth consecutive sample; a single sample never releases.
        se::ShellMotion sm;
        const float up[3] = {0.0f, 0.0f, 1.0f};
        const float shell[3] = {0.0f, 0.0f, 0.0f};
        const float padAt[3] = {0.0f, 0.0f, 10.0f};
        const float padMoved[3] = {0.0f, 0.0f, 13.0f};
        sm.sample(padAt, 1, shell, 2, up);
        bool early = false;
        for (int k = 0; k < 9; ++k) early = early || sm.sample(padMoved, 1, shell, 2, up);
        check(!early, "v14.4: 3 m off the reference for nine samples does not release");
        check(sm.sample(padMoved, 1, shell, 2, up), "v14.4: 3 m off the reference for ten consecutive samples releases");
        se::ShellMotion one;
        one.sample(padAt, 1, shell, 2, up);
        check(!one.sample(padMoved, 1, shell, 2, up) && !one.sample(padAt, 1, shell, 2, up), "v14.4: a single sample off the reference never releases");
    }
    {
        // (4) The safety net: more than six seals in a minute trips it; spaced seals never do; a reset (a hangar change or a setting toggle) clears it.
        se::SealRateGuard g;
        bool any = false;
        for (uint64_t i = 0; i < 6; ++i) any = any || g.note(i * 1000);
        check(!any && !g.tripped, "v14.4 safety net: six seals in a minute do not trip it");
        check(g.note(6000) && g.tripped, "v14.4 safety net: the seventh seal in a minute trips it");
        se::SealRateGuard h;
        bool spaced = false;
        for (uint64_t i = 0; i < 12; ++i) spaced = spaced || h.note(i * 70000);
        check(!spaced, "v14.4 safety net: seals spaced over a minute apart never trip it");
        g = se::SealRateGuard{};
        check(!g.tripped && !g.note(7000), "v14.4 safety net: a reset clears it");
    }

    // ---- The rules the seal relies on -------------------------------------------------------------------------------------------------------------
    check(se::kPadMoveReleaseM == 2.0f && se::kCameraLeaveM == 2.0f && se::kBayStillMs == 2000,
          "the constants: the pad-move release 2 m, the camera 2 m, the latch movers window 2 s");
    {
        float v = -1.0f;
        check(se::parseBelowM("", se::kBelowDefaultsM[0], &v) && v == 85.0f && se::kBelowDefaultsM[0] == 85.0f,
              "the small floor defaults to 85 m (the lift doors at -72 m)");
        check(se::belowForSize(se::PadSize::kMedium, se::kBelowDefaultsM, false, 0.0f) == 120.0f &&
                  se::belowForSize(se::PadSize::kLarge, se::kBelowDefaultsM, false, 0.0f) == 145.0f,
              "the medium floor is 120 m and the large 145 m (the measured doors plus margin)");
        check(se::parseBelowM("45", 85.0f, &v) && v == 45.0f && !se::parseBelowM("sixty", 85.0f, &v) && !se::parseBelowM("201", 85.0f, &v),
              "a floor parses as metres in [0, 200]; text and 201 are refused");
    }
    {
        const float rawOk[3] = {10.0f, 2.0f, -3.0f}, rawZero[3] = {0.0f, 0.0f, 0.0f};
        check(se::rawCentreUsable(rawOk, 5.0f) && !se::rawCentreUsable(rawZero, 5.0f) && !se::rawCentreUsable(rawOk, 0.0f),
              "an unreadable centre (the origin) is not usable: such a record is kept, never judged");
    }
    check(se::ownPadCandidate(50.0f, 60.0f) && se::ownPadCandidate(39.0f, 0.0f) && !se::ownPadCandidate(41.0f, 0.0f),
          "a pad is an own pad candidate when the camera is inside its sphere or its centre is within 40 m");
    check(!se::heldCellDrops(29, 29, se::kHeldLostFrames) && se::heldCellDrops(30, 0, se::kHeldLostFrames) &&
              se::heldCellDrops(0, 30, se::kHeldLostFrames),
          "the held cell drops at 30 consecutive frames of absence or of another own pad");
    {
        char stripped[48] = {};
        check(se::strippedPrefix("CobraMkV_", stripped, sizeof(stripped)) && std::strcmp(stripped, "CobraMkV") == 0 &&
                  !se::strippedPrefix("CobraMkV", stripped, sizeof(stripped)),
              "the own record's name is the prefix without its underscore");
        check(se::ownRecordMatch("CobraMkV", "CobraMkV") && !se::ownRecordMatch("CobraMkV_Door", "CobraMkV"),
              "the own record matches its exact name only");
        const float oc[3] = {0.0f, 0.0f, 0.0f}, in1[3] = {34.0f, 0.0f, 0.0f}, in2[3] = {36.0f, 0.0f, 0.0f};
        check(se::ownSphereKeeps(in1, oc, 30.0f, 5.0f) && !se::ownSphereKeeps(in2, oc, 30.0f, 5.0f),
              "a record within the own record's live sphere plus 5 m is kept; 36 m from a 30 m sphere is not");
    }
    {
        const float o[3] = {0.0f, 0.0f, 0.0f}, u[3] = {0.0f, 0.0f, 1.0f};
        const float p1[3] = {0.0f, 0.0f, 50.0f}, p2[3] = {61.0f, 0.0f, 50.0f}, p3[3] = {60.0f, 0.0f, 50.0f};
        const float p4[3] = {0.0f, 0.0f, -90.0f}, p5[3] = {0.0f, 0.0f, -80.0f};
        check(se::inBayVolume(p1, o, u, 100.0f, 85.0f) && !se::inBayVolume(p2, o, u, 100.0f, 85.0f) && se::inBayVolume(p3, o, u, 100.0f, 85.0f),
              "the bay: on the shaft is in; 61 m lateral is out; 60 m lateral is in");
        check(!se::inBayVolume(p4, o, u, 100.0f, 85.0f) && se::inBayVolume(p5, o, u, 100.0f, 85.0f),
              "the bay: 90 m below the pad is out of an 85 m floor; 80 m below is in");
        check(se::bayPartName("Hangar_Small") && se::bayPartName("TurboLiftDoor") && se::bayPartName("LandingPad_Large") &&
                  !se::bayPartName("CobraMkV_") && !se::bayPartName("SocialSpace"),
              "the bay keeps the hangar's part names (Hangar, LandingPad, lift doors, ...) and not a ship's or the concourse's");
    }

    // ---- The pad-local frame and the release by the shell
    {
        // Built v13.7 (a): the pad frame. A rotated pad at (10, 2, 3): a point taken to the pad's frame and back is itself; the cell placed back
        // after a frame shift moves by exactly the shift; a pad turned about up turns the box's directions with it, and a point at a fixed place on
        // the pad keeps that place on the pad.
        const float th = 0.5235988f;   // 30 degrees
        const float axesA[3][3] = {{std::cos(th), std::sin(th), 0.0f}, {-std::sin(th), std::cos(th), 0.0f}, {0.0f, 0.0f, 1.0f}};
        const float padC[3] = {10.0f, 2.0f, 3.0f};
        const float p0[3] = {14.0f, -5.0f, 20.0f};
        float loc0[3] = {}, back0[3] = {};
        se::pointToLocal(p0, padC, axesA, loc0);
        se::pointToWorld(loc0, padC, axesA, back0);
        check(std::fabs(back0[0] - p0[0]) < 1e-3f && std::fabs(back0[1] - p0[1]) < 1e-3f && std::fabs(back0[2] - p0[2]) < 1e-3f,
              "a point taken to the rotated pad's frame and back is the same point");
        check(std::fabs(loc0[2] - 17.0f) < 1e-3f, "a point 17 m above the pad's centre is 17 m along the pad's up axis");

        // A cell with a shaft, a box, frozen at engage; then the pad's live frame is shifted by T (the record frame moves).
        se::OwnCell cellW;
        cellW.has = true;
        cellW.centre[0] = padC[0]; cellW.centre[1] = padC[1]; cellW.centre[2] = padC[2];
        cellW.capA[0] = padC[0]; cellW.capA[1] = padC[1]; cellW.capA[2] = padC[2];
        cellW.capB[0] = 12.0f; cellW.capB[1] = 40.0f; cellW.capB[2] = 90.0f;
        cellW.capsule = true;
        cellW.box = true;
        cellW.boxCentre[0] = 9.0f; cellW.boxCentre[1] = 3.0f; cellW.boxCentre[2] = 60.0f;
        cellW.boxU[0] = std::cos(th); cellW.boxU[1] = std::sin(th);
        cellW.boxV[0] = -std::sin(th); cellW.boxV[1] = std::cos(th);
        cellW.boxW[2] = 1.0f;
        cellW.boxHalf[0] = 33.0f; cellW.boxHalf[1] = 14.0f; cellW.boxHalf[2] = 60.0f;
        se::OwnCell cellL;
        se::localizeCell(cellW, padC, axesA, &cellL);
        const float T[3] = {-5.18f, -2.99f, 34.0f};   // the v13.6 flight's shift
        const float padS[3] = {padC[0] + T[0], padC[1] + T[1], padC[2] + T[2]};
        se::OwnCell cellS = cellW;
        se::worldizeCell(cellL, padS, axesA, &cellS);
        check(std::fabs(cellS.centre[0] - padS[0]) < 1e-3f && std::fabs(cellS.centre[2] - padS[2]) < 1e-3f,
              "the pad-frame cell's centre follows the pad's live centre after a frame shift");
        check(std::fabs(cellS.capB[0] - (cellW.capB[0] + T[0])) < 1e-3f && std::fabs(cellS.capB[1] - (cellW.capB[1] + T[1])) < 1e-3f &&
                  std::fabs(cellS.capB[2] - (cellW.capB[2] + T[2])) < 1e-3f,
              "the shaft target moves with the frame shift, by the same offset as the pad");
        check(std::fabs(cellS.boxCentre[2] - (cellW.boxCentre[2] + T[2])) < 1e-3f && std::fabs(cellS.boxU[0] - cellW.boxU[0]) < 1e-5f &&
                  std::fabs(cellS.boxHalf[2] - 60.0f) < 1e-6f,
              "the box moves by the shift; its axes and half-extents do not change under a pure shift");

        // A pad turned 90 degrees about up: the box's length axis (world +x at 30 degrees, which is the pad's own axis 0) turns with the pad to
        // world +y; the shaft target keeps its height along the pad's up axis.
        const float axesR[3][3] = {{0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
        se::OwnCell cellT = cellW;
        se::worldizeCell(cellL, padC, axesR, &cellT);
        check(std::fabs(cellT.boxU[0]) < 1e-3f && std::fabs(cellT.boxU[1] - 1.0f) < 1e-3f,
              "a pad turned 90 degrees turns the box's length axis with it (to world +y)");
        const float rotCap[3] = {cellT.capB[0] - padC[0], cellT.capB[1] - padC[1], cellT.capB[2] - padC[2]};
        check(std::fabs(rotCap[2] - cellL.capB[2]) < 1e-3f,
              "the shaft target keeps its height along the pad's up axis when the pad turns about up");

        // Built v13.7 (b): the release by the pad's own motion along the shaft, against the hangar shell. A shift of the whole frame (pad and shell
        // alike) changes nothing; the pad rising 6 m relative to the shell releases; rising 4 m or 5 m does not (the limit is more than 5 m).
        const float up[3] = {0.0f, 0.0f, 1.0f};
        const float shellE[3] = {0.0f, 0.0f, 50.0f}, padE[3] = {0.0f, 0.0f, 10.0f};
        const float relE = se::shellRelative(padE, shellE, up);
        const float shellS[3] = {T[0], T[1], 50.0f + T[2]}, padSh[3] = {T[0], T[1], 10.0f + T[2]};
        check(!se::padMovedAway(relE, se::shellRelative(padSh, shellS, up)),
              "a frame shift that moves the pad and the shell alike does not release the cell");
        const float padUp6[3] = {0.0f, 0.0f, 13.0f}, padUp4[3] = {0.0f, 0.0f, 11.5f}, padUp5[3] = {0.0f, 0.0f, 12.0f};
        check(se::padMovedAway(relE, se::shellRelative(padUp6, shellE, up)) && !se::padMovedAway(relE, se::shellRelative(padUp4, shellE, up)) &&
                  !se::padMovedAway(relE, se::shellRelative(padUp5, shellE, up)),
              "the pad rising 3 m against the shell releases the cell (the limit is 2 m); 1.5 m and exactly 2 m do not");
        check(!se::padMovedAway(NAN, 0.0f), "an unreadable reference never releases");
    }


    // ---- Restored (test-only, 2026-10-13): retained behaviour whose tests the v14.23 prune removed, rewritten against the current APIs --------------
    {
        // v14.9, the absent-pad hold: a pad absent for a few frames keeps the trigger's request; a longer absence clears it.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        check(se::sealStep(inHangar(0), &st, &w) == se::SealAction::kRequestEngage, "v14.9 hold: the setup request");
        se::SealInputs held = inHangar(100);
        held.pickOk = false;
        held.padHeld = true;
        check(se::sealStep(held, &st, &w) == se::SealAction::kNone && st.requested,
              "v14.9 hold: a pad absent for fewer than kHeldLostFrames keeps the request (no release, no new request)");
        held.nowMs = 200;
        check(se::sealStep(held, &st, &w) == se::SealAction::kNone && st.requested, "v14.9 hold: the same short absence again keeps it");
        se::SealInputs lost = inHangar(300);
        lost.pickOk = false;
        lost.padHeld = false;
        check(se::sealStep(lost, &st, &w) == se::SealAction::kNone && !st.requested, "v14.9 hold: a pad absent past the hold clears the request");
        check(!se::heldCellDrops(29, 0, se::kHeldLostFrames) && se::heldCellDrops(30, 0, se::kHeldLostFrames),
              "v14.9 hold: the held cell drops at kHeldLostFrames (30) absent frames, and not before");
    }
    {
        // The hangar change releases a held seal, with its own reason (the ship swap and the held pad gone both come here).
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.hangarChanged = true;
        in.nowMs = 100;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kHangarChanged,
              "hangar change: a held seal is released by the hangar change");
    }
    {
        // The pad moved against the shell (ShellMotion) releases a held seal; the next seal needs the r1 0 then 1 edge.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.padMoved = true;
        in.nowMs = 100;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kPadMoved, "pad moved: a held seal is released by the pad moving");
        in.held = false;
        in.padMoved = false;
        uint64_t early = 0;
        for (uint64_t t = 200; t <= 2000; t += 100) {
            in.nowMs = t;
            if (se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage && early == 0) early = t;
        }
        check(early == 0, "pad moved: with r1 still 1, the pad's move does not re-seal until the r1 edge (2 s)");
    }
    {
        // The mechanism latch (v14.14-16): a latched record that moves on two compared frames in a row is the launch; one move is not; a source change
        // re-bases without counting; the own pad is index 0 and counts the same way.
        se::LatchSet ls;
        ls.n = 1;
        ls.latched = true;
        const float r0[3] = {0.0f, 0.0f, 0.0f};
        const float r1[3] = {0.0f, 0.0f, 0.5f};
        const float r2[3] = {0.0f, 0.0f, 1.0f};
        const float a0[3] = {1.0f, 0.0f, 0.0f};
        const float a2[3] = {0.0f, 0.0f, 1.0f};
        ls.recs[0].step(1, r0, a0, a2, 1, 2, 100);
        ls.recs[0].step(2, r1, a0, a2, 1, 2, 200);
        check(ls.movedIndex() == -1, "latch: one compared move is not a release");
        ls.recs[0].step(3, r2, a0, a2, 1, 2, 300);
        check(ls.movedIndex() == 0, "latch: a record that moves on two compared frames in a row is the launch (the own pad is index 0)");
        ls.recs[0].step(4, r2, a0, a2, 1, 2, 400);
        check(ls.movedIndex() == -1, "latch: a still frame ends the run");
        ls.recs[0].step(5, r0, a0, a2, 3, 2, 500);   // another centre source: a re-base, never a move
        check(ls.movedIndex() == -1 && ls.recs[0].movedRun == 0, "latch: a change of centre source re-bases without counting as a move");
        // The identity (review 2): a shell replacement is a genuine change too.
        se::LatchIdentity id;
        id.bind(0xA0, 0x5000);
        check(id.step(true, 0xA0, true, 0x6000) && id.shell == 0x6000, "latch identity: a shell replacement is a change (rebuild the latch)");
    }
    {
        // The camera (v14.7): out for kCameraFrames frames releases the seal (the 10th frame); back inside it re-seals after kCameraFrames
        // frames inside the bay, with no in-hangar edge (r1 stayed 1).
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.cameraOutside = true;
        int outTicks = 0;
        for (int k = 1; k <= 12 && outTicks == 0; ++k) {
            in.nowMs = 100 * static_cast<uint64_t>(k);
            if (se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kCameraLeft) outTicks = k;
        }
        check(outTicks == 10, "camera: out of the box for kCameraFrames (10) frames releases the seal, on the 10th frame");
        in.held = false;
        in.cameraOutside = false;
        in.cameraInBox = false;
        in.nowMs = 2000;
        se::sealStep(in, &st, &w);
        int inTicks = 0;
        for (int k = 1; k <= 12 && inTicks == 0; ++k) {
            in.cameraInBox = true;
            in.nowMs = 2000 + 100 * static_cast<uint64_t>(k);
            if (se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage) inTicks = k;
        }
        check(inTicks == 10, "camera: back inside the bay the re-seal waits kCameraFrames (10) frames, and needs no in-hangar edge");
    }
    {
        // Review 1: a request whose pad or shell changed before its box was valid is never admitted to freeze a cell.
        se::SealState st;
        st.requested = true;
        st.camInRun = se::kCameraFrames;
        const se::EngageRequest r{true, 0x1000, 0x5000};
        check(!se::engageAdmitted(r, st, 0x1000, 0x5000, false), "review 1: an invalid box at the request does not freeze a cell");
        check(!se::engageAdmitted(r, st, 0x1000, 0x6000, true), "review 1: the shell replaced before the box was valid: no freeze");
        check(!se::engageAdmitted(r, st, 0x2000, 0x5000, true), "review 1: the pad replaced before the box was valid: no freeze");
        check(se::engageAdmitted(r, st, 0x1000, 0x5000, true), "review 1: control: the same pad and shell with a valid box freeze the cell");
    }
    {
        // Store mode (v14.1): the store observer does nothing with the setting off or undocked and unsealed; gathers while docked and unsealed; judges
        // while sealed. The promote is due when the frame stamp has moved on.
        check(se::storeMode(false, false, true) == se::StoreMode::kNone && se::storeMode(true, false, false) == se::StoreMode::kNone,
              "v14.1 store: setting off, or undocked and unsealed, the store observer does nothing");
        check(se::storeMode(true, false, true) == se::StoreMode::kGather, "v14.1 store: docked and unsealed, the observer gathers (no judgement)");
        check(se::storeMode(true, true, false) == se::StoreMode::kJudge && se::storeMode(true, true, true) == se::StoreMode::kJudge,
              "v14.1 store: sealed, the observer judges, docked or not");
        check(se::framePromoteDue(3, 4) && !se::framePromoteDue(4, 4),
              "v14.2 promote: the frame's cell is promoted once per frame stamp (due when the stamp moved on)");
    }
    {
        // v14.1: a pad that was not gathered (no pick) never requests the seal, however docked; the pad in the hangar with a pick does.
        se::SealState st;
        se::SealInputs noPick = inHangar(0);
        noPick.pickOk = false;
        check(firstEngage(&st, noPick, 0, 20000) == 0, "v14.1: no pad gathered (pickOk false): the seal is never requested");
    }

    // ---- Built v14.24 (2026-10-13, the review of the 013c6122 flight) -----------------------------------------------------------------------------
    {
        // Item 1: a change of rig A's association forgets A's bounds and verdict only; rig B keeps its bound and its verdict (no whole-epoch bump).
        bool aCur = true, aLast = true, bCur = true, bLast = true;
        uint8_t aV = 2, bV = 2;
        uint32_t aRun = 3, bRun = 3;
        se::rigForgetBounds(&aCur, &aLast, &aV, &aRun);
        check(!aCur && !aLast && aV == 0 && aRun == 0, "v14.24 rig: a change of rig A's collection forgets rig A's bounds and verdict");
        check(bCur && bLast && bV == 2 && bRun == 3 && se::rigMayVeto(4, 4, bCur, bV),
              "v14.24 rig: rig B keeps its bound and its outside verdict, and may still veto (no whole-epoch bump)");
        check(se::collAssociationIs(0x500, 0x500) && !se::collAssociationIs(0x600, 0x500) && !se::collAssociationIs(0, 0),
              "v14.24 rig: a table entry names its rig only while the rig still holds that collection");
    }
    {
        // Release review 2026-10-14 finding 4: a rig's collection changed, so the rig's first record's name goes with its bounds (the own-ship prefix exemption reads the name).
        struct RigModel {
            bool cur = false, last = false, named = false;
            uint8_t verdict = 0;
            uint32_t run = 0;
            char name[48] = {};
            uintptr_t coll = 0;
        };
        auto note = [](RigModel& r, const char* recordName) {   // what applyRigNote does for one record: a bound, and the name while the slot has none
            r.cur = true;
            se::rigRecordName(&r.named, r.name, sizeof(r.name), recordName);
        };
        auto assoc = [](RigModel& r, uintptr_t coll) {   // what applyRigAssoc does
            if (coll == r.coll) return;
            se::rigReassociate(&r.cur, &r.last, &r.verdict, &r.run, &r.named, r.name, sizeof(r.name));
            r.coll = coll;
        };
        auto exempt = [](const RigModel& r, const char* prefix) { return se::ownShipExempt(false, 0.0f, r.name, prefix); };
        // the name is recorded once, while the slot has none
        RigModel a;
        note(a, "Cobra_Hull_01");
        note(a, "Rock_02");
        check(a.named && std::strcmp(a.name, "Cobra_Hull_01") == 0 && exempt(a, "Cobra_"), "release review 2026-10-14 rig name: the first record names the rig, a later record does not rename it");
        assoc(a, 0x500);
        a.verdict = 2;
        a.last = true;
        // own-ship prefix -> not: the old name must not exempt the new object
        assoc(a, 0x600);
        check(!a.named && a.name[0] == '\0' && !a.cur && !a.last && a.verdict == 0 && a.run == 0, "release review 2026-10-14 rig name: a change of collection forgets the name with the bounds and the verdict");
        check(!exempt(a, "Cobra_"), "release review 2026-10-14 rig name: with no name yet the rig is not exempt by prefix");
        note(a, "Rock_02");
        check(a.named && std::strcmp(a.name, "Rock_02") == 0 && !exempt(a, "Cobra_"), "release review 2026-10-14 rig name: prefix to non-prefix, fresh bounds: the new object is classed by its own name (not exempt)");
        // not -> own-ship prefix: the new object must be protected by its own name
        RigModel b;
        note(b, "Rock_02");
        assoc(b, 0x500);
        note(b, "Rock_02");
        check(!exempt(b, "Cobra_"), "release review 2026-10-14 rig name: a non-prefix rig is not exempt");
        assoc(b, 0x700);
        note(b, "Cobra_Hull_01");
        check(exempt(b, "Cobra_") && std::strcmp(b.name, "Cobra_Hull_01") == 0, "release review 2026-10-14 rig name: non-prefix to prefix, fresh bounds: the new object is exempt by its own name");
        // the same collection again is not a change: the name stays
        assoc(b, 0x700);
        check(b.named && exempt(b, "Cobra_"), "release review 2026-10-14 rig name: the same collection again keeps the name");
        // a record with no readable name, and a zero-size name buffer, are handled
        RigModel c;
        note(c, nullptr);
        check(c.named && c.name[0] == '\0', "release review 2026-10-14 rig name: a null record name records an empty name");
        bool named = false;
        char tiny[1] = {'x'};
        check(!se::rigRecordName(&named, tiny, 0, "abc") && !named && tiny[0] == 'x', "release review 2026-10-14 rig name: a zero-size buffer records nothing");
    }
    {
        // Item 3: the cost summary's percentiles, and the change-only gate.
        double v[100];
        for (int i = 0; i < 100; ++i) v[i] = static_cast<double>(100 - i);   // 100 down to 1
        double p50 = 0.0, p95 = 0.0, mx = 0.0;
        se::samplePercentiles(v, 100, &p50, &p95, &mx);
        check(p50 == 50.0 && p95 == 95.0 && mx == 100.0, "v14.24 cost: p50, p95 and max of 1..100 are 50, 95 and 100");
        se::samplePercentiles(v, 0, &p50, &p95, &mx);
        check(p50 == 0.0 && p95 == 0.0 && mx == 0.0, "v14.24 cost: no samples read as zeros");
        se::ChangeGate g;
        check(g.changed(7) && !g.changed(7) && g.changed(8) && !g.changed(8), "v14.24 log: a change-only gate writes a key once per change");
        check(se::textKey("pad") == se::textKey("pad") && se::textKey("pad") != se::textKey("pad "),
              "v14.24 log: a text key is stable for the same text and differs for another");
        se::SealState st;
        check(!st.mechanism && st.poseRun == 0, "v14.24: a fresh trigger holds no mechanism release");
    }
    {
        // Item 2: the pose of a latched record against its pose at the seal.
        const float rel0[3] = {0.0f, 0.0f, 0.0f};
        const float relOff[3] = {0.0f, 0.0f, 0.3f};
        const float a0[3] = {1.0f, 0.0f, 0.0f};
        const float a2[3] = {0.0f, 0.0f, 1.0f};
        const float c2 = std::cos(2.0f * 3.14159265f / 180.0f), s2 = std::sin(2.0f * 3.14159265f / 180.0f);
        const float a0Turn[3] = {c2, s2, 0.0f};
        float dM = -1.0f, dDeg = -1.0f;
        se::latchPoseDelta(rel0, rel0, a0, a0, a2, a2, &dM, &dDeg);
        check(dM == 0.0f && dDeg == 0.0f && se::latchPoseWithin(dM, dDeg), "v14.24 pose: a record at its sealed pose is within tolerance");
        se::latchPoseDelta(relOff, rel0, a0, a0, a2, a2, &dM, &dDeg);
        check(std::fabs(dM - 0.3f) < 1e-5f && !se::latchPoseWithin(dM, dDeg), "v14.24 pose: a 0.3 m offset is outside the 0.25 m tolerance");
        se::latchPoseDelta(rel0, rel0, a0Turn, a0, a2, a2, &dM, &dDeg);
        check(std::fabs(dDeg - 2.0f) < 1e-3f && !se::latchPoseWithin(dM, dDeg), "v14.24 pose: a 2 degree turn is outside the 1 degree tolerance");
    }
    {
        // Item 2, the re-seal on the poses: a Lift that returns to its sealed pose re-seals after kPoseBackFrames frames with r1 1.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "v14.24 pose: the setup seal");
        in.held = true;
        in.mechanismMoved = true;
        in.poseBack = false;
        in.nowMs = 100;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kMechanism, "v14.24 pose: the Lift moving releases the seal");
        in.held = false;
        in.mechanismMoved = false;
        in.poseBack = true;
        int frames = 0;
        for (int k = 1; k <= 60 && frames == 0; ++k) {
            in.nowMs = 100 + 100 * static_cast<uint64_t>(k);
            if (se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage) frames = k;
        }
        check(frames == static_cast<int>(se::kPoseBackFrames),
              "v14.24 pose: a Lift that returns to its sealed pose re-seals after 30 frames (r1 1, camera in)");
    }
    {
        // A Lift that stays away does not re-seal, however long r1 stays 1.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.mechanismMoved = true;
        in.nowMs = 100;
        se::sealStep(in, &st, &w);
        in.held = false;
        in.mechanismMoved = false;
        in.poseBack = false;
        uint64_t early = 0;
        for (uint64_t t = 200; t <= 10000; t += 100) {
            in.nowMs = t;
            if (se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage && early == 0) early = t;
        }
        check(early == 0, "v14.24 pose: a Lift that stays away does not re-seal (100 s with r1 1, the pose never back)");
    }
    {
        // r1 1 to 0 while away keeps the edge rule: the pose does not count while the pad is out; the next seal is the r1 0 to 1 edge.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.mechanismMoved = true;
        in.nowMs = 100;
        se::sealStep(in, &st, &w);
        in.held = false;
        in.mechanismMoved = false;
        in.poseBack = false;
        in.nowMs = 200;
        check(se::sealStep(in, &st, &w) == se::SealAction::kNone, "v14.24 edge: with the pose away and r1 1, no re-seal");
        in.inHangar = false;
        in.poseBack = true;
        in.nowMs = 300;
        check(se::sealStep(in, &st, &w) == se::SealAction::kNone && st.reseal == 2 && !st.mechanism,
              "v14.24 edge: r1 1 to 0 moves the re-seal to the edge rule and drops the pose count");
        in.inHangar = true;
        in.poseBack = false;
        in.nowMs = 400;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "v14.24 edge: r1 back to 1 is the edge: it re-seals at once");
    }
    {
        // The pose rule belongs to the mechanism release only: a pad-moved release does not re-seal on poses.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.padMoved = true;
        in.nowMs = 100;
        se::sealStep(in, &st, &w);
        in.held = false;
        in.padMoved = false;
        in.poseBack = true;
        uint64_t early = 0;
        for (uint64_t t = 200; t <= 4000; t += 100) {
            in.nowMs = t;
            if (se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage && early == 0) early = t;
        }
        check(early == 0, "v14.24 pose: a pad-moved release does not re-seal on the poses (it waits for the r1 edge)");
    }
    {
        // Item 3: the waiting reason carries no per-frame counter: two frames of an absent pad give the same text.
        se::OwnPadSticky sticky;
        sticky.pad = 0x1000;
        const float camera[3] = {0.0f, 0.0f, 0.0f};
        const float radii[3] = {80.0f, 120.0f, 145.0f};
        se::OwnCell first, second;
        se::pickOwnPadSticky(nullptr, 0, camera, radii, &sticky, &first);
        se::pickOwnPadSticky(nullptr, 0, camera, radii, &sticky, &second);
        check(std::strcmp(first.reason, second.reason) == 0 && std::strstr(first.reason, "held for up to 30 frames") != nullptr,
              "v14.24 log: the held-pad reason is the same on every frame (no counter, no line per frame)");
    }

    // ---- Built v14.25 (2026-10-13): the lock-free pieces, driven from several threads ---------------------------------------------------------------
    {
        // The snapshot ring, one thread: the value pinned is the one published.
        struct Snap {
            uint64_t seq = 0;
            uint64_t a = 0;
            uint64_t b = 0;
            uint64_t check = 0;
        };
        se::SnapshotRing<Snap, 4> ring;
        Snap& w = ring.beginWrite();
        w.seq = 1;
        w.a = 3;
        w.b = 5;
        w.check = 9;
        ring.publish();
        size_t idx = 0;
        const Snap* p = ring.pin(&idx);
        check(p->seq == 1 && p->check == 9, "v14.25 ring: the value pinned is the value published");
        ring.unpin(idx);
    }
    {
        // Eight readers and one publisher. A pinned value is never torn (check = seq + a + b), a reader's sequence never goes back, and the last
        // publish is the one read at the end (no lost epoch).
        struct Snap {
            uint64_t seq = 0;
            uint64_t a = 0;
            uint64_t b = 0;
            uint64_t check = 0;
        };
        se::SnapshotRing<Snap, 4> ring;
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> torn{0};
        std::atomic<uint64_t> backwards{0};
        std::atomic<uint64_t> reads{0};
        std::vector<std::thread> readers;
        for (int t = 0; t < 8; ++t) {
            readers.emplace_back([&] {
                uint64_t last = 0;
                while (!stop.load(std::memory_order_relaxed)) {
                    size_t idx2 = 0;
                    const Snap* q = ring.pin(&idx2);
                    if (q->check != q->seq + q->a + q->b) torn.fetch_add(1);
                    if (q->seq < last) backwards.fetch_add(1);
                    last = q->seq;
                    ring.unpin(idx2);
                    reads.fetch_add(1);
                }
            });
        }
        const uint64_t kPublishes = 20000;
        while (reads.load() == 0) std::this_thread::yield();   // the readers are running before the first publish
        for (uint64_t k = 1; k <= kPublishes; ++k) {
            Snap& wr = ring.beginWrite();
            wr.seq = k;
            wr.a = 3 * k;
            wr.b = 5 * k;
            wr.check = k + 3 * k + 5 * k;
            ring.publish();
        }
        stop.store(true);
        for (auto& th : readers) th.join();
        check(torn.load() == 0, "v14.25 ring, threads: no torn snapshot across 8 readers and 20000 publishes");
        check(backwards.load() == 0, "v14.25 ring, threads: no reader sees the epoch go backwards");
        size_t idx3 = 0;
        const Snap* last = ring.pin(&idx3);
        check(last->seq == kPublishes && ring.publishes() == kPublishes, "v14.25 ring, threads: the last publish is the one read (no lost epoch)");
        ring.unpin(idx3);
        check(reads.load() > 0, "v14.25 ring, threads: the readers did read");
    }
    {
        // The seqlock table: a single-threaded reference, then eight writers inserting the same records concurrently into a fresh table. Every
        // record must read back its exact payload in both; readers running during the writes never see a torn payload.
        constexpr size_t kCap = 1024;
        static se::SeqCell ref[kCap];
        static se::SeqCell par[kCap];
        const size_t kRecs = 160;
        auto homeOf = [&](uintptr_t rec) { return static_cast<size_t>((static_cast<uint64_t>(rec) >> 4) * 0x9E3779B97F4A7C15ull >> 54) & (kCap - 1); };
        auto valueOf = [](uintptr_t rec, float out[4]) {
            out[0] = static_cast<float>(rec % 997);
            out[1] = static_cast<float>(rec % 31);
            out[2] = static_cast<float>(rec % 17);
            out[3] = static_cast<float>(rec % 11) + 1.0f;
        };
        for (size_t k = 0; k < kRecs; ++k) {
            const uintptr_t rec = 0x7000 + 16 * static_cast<uintptr_t>(k);
            float v[4];
            valueOf(rec, v);
            se::seqTableInsert(ref, kCap, homeOf(rec), rec, 1, v, nullptr);
        }
        size_t refFound = 0;
        for (size_t k = 0; k < kRecs; ++k) {
            const uintptr_t rec = 0x7000 + 16 * static_cast<uintptr_t>(k);
            float got[4] = {};
            float want[4];
            valueOf(rec, want);
            if (se::seqTableFind(ref, kCap, homeOf(rec), rec, 1, got, nullptr) && std::memcmp(got, want, sizeof(want)) == 0) ++refFound;
        }
        check(refFound == kRecs, "v14.25 table, reference: every record reads back its payload (single thread)");
        std::atomic<uint64_t> badHits{0};
        std::vector<std::thread> writers;
        for (int t = 0; t < 8; ++t) {
            writers.emplace_back([&, t] {
                for (size_t k = 0; k < kRecs; ++k) {
                    const size_t j = (k * 7 + static_cast<size_t>(t) * 13) % kRecs;   // each thread inserts all records, in its own order
                    const uintptr_t rec = 0x7000 + 16 * static_cast<uintptr_t>(j);
                    float v[4];
                    valueOf(rec, v);
                    se::seqTableInsert(par, kCap, homeOf(rec), rec, 1, v, nullptr);
                }
            });
        }
        std::thread checker([&] {
            for (int round = 0; round < 200; ++round) {
                for (size_t k = 0; k < kRecs; ++k) {
                    const uintptr_t rec = 0x7000 + 16 * static_cast<uintptr_t>(k);
                    float got[4] = {};
                    float want[4];
                    valueOf(rec, want);
                    if (se::seqTableFind(par, kCap, homeOf(rec), rec, 1, got, nullptr) && std::memcmp(got, want, sizeof(want)) != 0) badHits.fetch_add(1);
                }
            }
        });
        for (auto& th : writers) th.join();
        checker.join();
        size_t parFound = 0;
        for (size_t k = 0; k < kRecs; ++k) {
            const uintptr_t rec = 0x7000 + 16 * static_cast<uintptr_t>(k);
            float got[4] = {};
            float want[4];
            valueOf(rec, want);
            if (se::seqTableFind(par, kCap, homeOf(rec), rec, 1, got, nullptr) && std::memcmp(got, want, sizeof(want)) == 0) ++parFound;
        }
        check(badHits.load() == 0, "v14.25 table, threads: a reader during concurrent inserts never sees a torn payload");
        check(parFound == refFound && parFound == kRecs,
              "v14.25 table, threads: eight writers inserting the same records give the same decisions as the single-thread reference");
        float got[4] = {};
        check(!se::seqTableFind(par, kCap, homeOf(0x7000), 0x7000, 2, got, nullptr), "v14.25 table: a record of another generation is not found");
    }
    // ---- Built v14.26 (2026-10-13): the gather buffers and the rig publication, from several threads --------------------------------------------
    {
        // The gather buffers, one thread: a full slot keeps its first entries and counts the rest; the entries come back in the order written, once.
        struct Item {
            uint32_t thread = 0;
            uint32_t seq = 0;
            uint32_t stamp = 0;
        };
        std::atomic<uint32_t> stamp{0};
        se::PassGather<Item, 2, 8> gb(stamp);
        {
            se::PassGather<Item, 2, 8>::Writer w(gb);
            w.call(0x1000);
            for (uint32_t k = 0; k < 10; ++k) {
                Item it;
                it.seq = k;
                w.push(it);
            }
        }
        stamp.fetch_add(1);
        std::vector<uint32_t> got;
        const se::PassDrain d = gb.drain([&](const Item& it) { got.push_back(it.seq); });
        check(got.size() == 8 && d.lost == 2, "v14.26 gather, one thread: a full slot keeps its first entries and counts the rest as lost");
        bool inOrder = true;
        for (size_t k = 0; k < got.size(); ++k) inOrder = inOrder && got[k] == k;
        check(inOrder, "v14.26 gather, one thread: the entries come back in the order written");
        check(d.complete && d.passes == 1 && d.calls == 1 && d.ctx == 0x1000, "v14.27 gather: the drain reports its pass, its callback count and the context");
        got.clear();
        stamp.fetch_add(1);
        const se::PassDrain again = gb.drain([&](const Item& it) { got.push_back(it.seq); });
        check(got.empty() && again.lost == 0 && again.calls == 0, "v14.26 gather, one thread: a drained entry is not delivered twice, and the loss is counted once");
    }
    {
        // Eight writers and a draining tick: every entry is delivered once or counted as lost, and each writer's entries keep their order.
        struct Item {
            uint32_t thread = 0;
            uint32_t seq = 0;
            uint32_t stamp = 0;
        };
        std::atomic<uint32_t> stamp{0};
        auto gbHeap = std::make_unique<se::PassGather<Item, 16, 512>>(stamp);   // 300 KB: not on the stack
        se::PassGather<Item, 16, 512>& gb = *gbHeap;
        std::atomic<int> done{0};
        const uint32_t kPer = 4000;
        std::vector<std::thread> writers;
        for (uint32_t t = 0; t < 8; ++t) {
            writers.emplace_back([&, t] {
                for (uint32_t k = 0; k < kPer; ++k) {
                    Item it;
                    it.thread = t;
                    it.seq = k;
                    se::PassGather<Item, 16, 512>::Writer w(gb);   // one callback, one entry
                    w.call(0x2000 + t);
                    w.push(it);
                }
                done.fetch_add(1);
            });
        }
        std::vector<uint32_t> nextSeq(8, 0);
        uint64_t delivered = 0;
        uint64_t lostTotal = 0;
        bool ordered = true;
        auto visit = [&](const Item& it) {
            ++delivered;
            if (it.seq < nextSeq[it.thread]) ordered = false;
            nextSeq[it.thread] = it.seq + 1;
        };
        uint64_t incomplete = 0;
        while (done.load() < 8) {
            stamp.fetch_add(1);
            const se::PassDrain d = gb.drain(visit);
            lostTotal += d.lost;
            if (!d.complete) ++incomplete;
            std::this_thread::yield();
        }
        for (auto& th : writers) th.join();
        for (int k = 0; k < 4; ++k) {
            stamp.fetch_add(1);
            lostTotal += gb.drain(visit).lost;
        }
        (void)incomplete;
        check(delivered + lostTotal == 8u * kPer, "v14.26 gather, threads: every entry is delivered once or counted as lost");
        check(ordered, "v14.26 gather, threads: each writer's entries come back in the order written");
        check(delivered > 0, "v14.26 gather, threads: the tick received entries while the writers ran");
    }
    {
        // The rig publication, one thread: an outside rig is vetoed thirty calls in a row and let through once (the refresh), a mixed verdict resets the
        // run, and a rig the latest publication does not hold forwards.
        auto pubHeap = std::make_unique<se::RigPubTable>();   // 400 KB: not on the stack
        se::RigPubTable& pub = *pubHeap;
        pub.begin();
        const uintptr_t rig = 0x5000;
        const uintptr_t kColl = 0x123400000ull;
        size_t idx = 0;
        check(pub.insert(rig, 7, se::kRigVerdictOutside, kColl, &idx), "v14.26 rig publication: an outside rig is inserted");
        uint8_t v = 0;
        {
            uintptr_t cs = 0;
            size_t is = 0;
            check(!pub.lookup(rig, 7, &v, &cs, &is), "v14.27 rig publication: a staged rig is not visible until the publication is committed");
        }
        pub.commit();
        uintptr_t c = 0;
        size_t i2 = 0;
        check(pub.lookup(rig, 7, &v, &c, &i2) && v == se::kRigVerdictOutside && c == kColl, "v14.26 rig publication: the verdict and collection read back");
        int vetoes = 0;
        bool pattern = true;
        for (int call = 1; call <= 61; ++call) {
            const se::RigOutcome o = pub.decide(idx, se::kRigVerdictOutside, 1);
            if (call == 31) pattern = pattern && o == se::RigOutcome::kRefresh;
            else pattern = pattern && o == se::RigOutcome::kVetoed;
            if (o == se::RigOutcome::kVetoed) ++vetoes;
        }
        check(pattern && vetoes == 60, "v14.26 rig publication: an outside rig is vetoed thirty calls in a row, then let through once");
        const se::RigOutcome m = pub.decide(idx, se::kRigVerdictMixed, 1);
        const se::RigOutcome after = pub.decide(idx, se::kRigVerdictOutside, 1);
        check(m == se::RigOutcome::kMixed && after == se::RigOutcome::kVetoed, "v14.26 rig publication: a mixed verdict resets the run");
        pub.begin();
        pub.commit();
        check(!pub.lookup(rig, 7, &v, &c, &i2), "v14.26 rig publication: a rig the latest publication does not hold is not found");
    }
    {
        // Eight readers decide and look up while the tick republishes two verdicts, each with its own collection. No reader sees a verdict paired with
        // the other collection, and the last publication is the one read at the end.
        auto pubHeap = std::make_unique<se::RigPubTable>();   // 400 KB: not on the stack
        se::RigPubTable& pub = *pubHeap;
        const uintptr_t kRig = 0x9000;
        const uintptr_t kCollOut = 0xAAAA00000ull;
        const uintptr_t kCollUnk = 0xBBBB00000ull;
        pub.begin();
        size_t i0 = 0;
        pub.insert(kRig, 9, se::kRigVerdictOutside, kCollOut, &i0);
        pub.commit();
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> torn{0};
        std::atomic<uint64_t> reads{0};
        std::atomic<uint64_t> gaps{0};
        std::vector<std::thread> readers;
        for (int r = 0; r < 8; ++r) {
            readers.emplace_back([&] {
                while (!stop.load(std::memory_order_relaxed)) {
                    uint8_t v = 0;
                    uintptr_t c = 0;
                    size_t idx = 0;
                    if (pub.lookup(kRig, 9, &v, &c, &idx)) {
                        const bool ok = (v == se::kRigVerdictOutside && c == kCollOut) || (v == se::kRigVerdictUnknown && c == kCollUnk);
                        if (!ok) torn.fetch_add(1);
                        (void)pub.decide(idx, v, 1);
                        reads.fetch_add(1);
                    } else {
                        gaps.fetch_add(1);   // the rig is in every publication: a miss is a window between two publications
                    }
                }
            });
        }
        while (reads.load() == 0) std::this_thread::yield();   // the readers run before the first republish (the same wait as the ring test)
        for (uint32_t k = 0; k < 20000; ++k) {
            pub.begin();
            size_t idx = 0;
            const bool outside = (k & 1u) == 0;
            pub.insert(kRig, 9, outside ? se::kRigVerdictOutside : se::kRigVerdictUnknown, outside ? kCollOut : kCollUnk, &idx);
            pub.commit();
        }
        stop.store(true);
        for (auto& th : readers) th.join();
        uint8_t v = 0;
        uintptr_t c = 0;
        size_t idx = 0;
        check(torn.load() == 0, "v14.26 rig publication, threads: no reader pairs a verdict with the other publication's collection");
        check(pub.lookup(kRig, 9, &v, &c, &idx) && v == se::kRigVerdictUnknown && c == kCollUnk,
              "v14.26 rig publication, threads: the last publication is the one read at the end");
        check(reads.load() > 0, "v14.26 rig publication, threads: the readers did read");
        (void)gaps;   // a reader preempted across two republications may miss once (it forwards); the property is tested deterministically below
    }
    {
        // Staging a publication leaves the committed one whole: every rig of the old publication is found until the commit, and the run of vetoes carries.
        auto pubHeap = std::make_unique<se::RigPubTable>();   // 400 KB: not on the stack
        se::RigPubTable& pub = *pubHeap;
        pub.begin();
        size_t idx[3] = {};
        for (uintptr_t k = 0; k < 3; ++k) pub.insert(0x100 + 16 * k, 3 + k, se::kRigVerdictOutside, 0xC000 + k, &idx[k], 7);
        pub.commit();
        for (int call = 0; call < 10; ++call) (void)pub.decide(idx[0], se::kRigVerdictOutside, 5);
        pub.begin();
        size_t ni = 0;
        pub.insert(0x100, 3, se::kRigVerdictOutside, 0xC000, &ni, 7, pub.runAt(idx[0]));   // staged, not committed
        bool allFound = true;
        for (uintptr_t k = 0; k < 3; ++k) {
            uint8_t v = 0;
            uintptr_t c = 0;
            size_t i = 0;
            uint32_t ep = 0;
            allFound = allFound && pub.lookup(0x100 + 16 * k, 3 + k, &v, &c, &i, &ep) && v == se::kRigVerdictOutside && c == 0xC000 + k && ep == 7;
        }
        check(allFound, "v14.27 rig publication: while the next publication is staged, every rig of the committed one is still found, with its epoch");
        pub.commit();
        int refresh = 0;
        int calls = 0;
        for (; calls < 21; ++calls) {
            if (pub.decide(ni, se::kRigVerdictOutside, 6) == se::RigOutcome::kRefresh) ++refresh;
        }
        check(refresh == 1, "v14.27 rig publication: the run of vetoes carries into the next publication (the refresh comes on the 31st call, not the 31st after each tick)");
        uint8_t v = 0;
        uintptr_t c = 0;
        size_t i = 0;
        check(!pub.lookup(0x110, 4, &v, &c, &i), "v14.27 rig publication: a rig left out of the new publication is gone after the commit");
    }

    // ---- Built v14.27 (2026-10-13): the pass rules. Callbacks of a frame land before, during and after its tick; an empty or unfinished pass is no evidence --
    {
        se::PassGate g;
        check(g.step(false, 0, 0) == se::PassVerdict::kDeferred, "v14.27 pass gate: a drain with a writer still inside is deferred");
        check(g.step(true, 0, 0) == se::PassVerdict::kEmpty, "v14.27 pass gate: no pass closed is empty");
        check(g.step(true, 1, 0) == se::PassVerdict::kEmpty, "v14.27 pass gate: a closed pass with no store callback is empty");
        check(g.step(true, 1, 1000) == se::PassVerdict::kInformative && g.run == 1, "v14.27 pass gate: a complete pass with callbacks is informative");
        check(g.step(true, 1, 400) == se::PassVerdict::kInformative && g.thin == 1, "v14.27 pass gate: a pass under half the usual calls is informative and counted thin");
        g.step(true, 0, 0);
        check(g.run == 0 && g.empty == 3 && g.deferred == 1 && g.informative == 2, "v14.27 pass gate: the window counts each verdict and an empty pass ends the run");
    }
    {
        struct P {
            uintptr_t addr = 0;
            int v = 0;
        };
        se::SeenWindow<P, 4> w;
        P a;
        a.addr = 0xA;
        a.v = 1;
        w.put(a);
        a.v = 2;
        w.put(a);
        check(w.n == 1 && w.items[0].v == 2, "v14.27 seen window: one entry per address, the newest reading");
        w.endPass();
        w.endPass();
        check(w.has(0xA), "v14.27 seen window: a record is kept through two passes that do not see it");
        w.endPass();
        check(!w.has(0xA), "v14.27 seen window: a record unseen for the whole window is gone");
        for (uintptr_t k = 1; k <= 6; ++k) {
            P p;
            p.addr = k;
            w.put(p);
        }
        check(w.n == 4, "v14.27 seen window: a full window replaces its oldest entry");
        se::CamHold ch;
        check(!ch.resolved(false) && ch.resolved(true) && ch.resolved(false), "v14.27 camera hold: a failure before any camera is unknown; one after a good one is held");
        bool held = true;
        for (uint32_t k = 1; k < se::kCamHoldTicks; ++k) held = held && ch.resolved(false);   // the first failure was counted above
        check(held && !ch.resolved(false), "v14.27 camera hold: the last camera is held for kCamHoldTicks failures, then it is unknown");
        se::StableGate sg;
        bool early = false;
        bool fired = false;
        sg.step(true, 3);
        for (int k = 1; k <= 5; ++k) {
            const bool f = sg.step(false, 3);
            if (k < 3) early = early || f;
            if (k == 3) fired = f;
        }
        check(!early && fired, "v14.27 stable gate: a change counts once, after it has held for the required ticks");
    }
    {
        // A writer still inside when the tick closes the pass: nothing is read, then both passes come back in order once it has left.
        struct E {
            uint32_t seq = 0;
            uint32_t stamp = 0;
        };
        std::atomic<uint32_t> stamp{1};
        se::PassGather<E, 4, 16> gb(stamp);
        std::vector<std::pair<uint32_t, uint32_t>> got;   // (stamp, seq)
        auto visit = [&](const E& e) { got.push_back({e.stamp, e.seq}); };
        auto w = std::make_unique<se::PassGather<E, 4, 16>::Writer>(gb);
        w->call(0x77);
        E e;
        e.seq = 1;
        w->push(e);
        stamp.fetch_add(1);
        const se::PassDrain open = gb.drain(visit, 2);
        check(!open.complete && got.empty() && open.passes == 0, "v14.27 gather: a pass with a writer still inside is not read");
        {
            se::PassGather<E, 4, 16>::Writer w2(gb);   // a callback of the next frame, on the same slot
            w2.call(0x77);
            E e2;
            e2.seq = 2;
            w2.push(e2);
        }
        w.reset();
        stamp.fetch_add(1);
        const se::PassDrain done = gb.drain(visit, 2);
        check(done.complete && done.passes == 2 && done.calls == 2 && got.size() == 2 && got[0].second == 1 && got[1].second == 2 && got[0].first == 1 && got[1].first == 2,
              "v14.27 gather: the unfinished pass is read with the next, oldest first, each entry naming its own pass");
        // A pass the tick never read is overwritten by the writer three stamps later and counted as lost, not misread as a newer pass.
        for (int k = 0; k < 3; ++k) {
            {
                se::PassGather<E, 4, 16>::Writer wl(gb);
                wl.call(0);
                E x;
                x.seq = 10 + k;
                wl.push(x);
            }
            stamp.fetch_add(1);
        }
        got.clear();
        {
            se::PassGather<E, 4, 16>::Writer wl(gb);
            wl.call(0);
        }
        const se::PassDrain lap = gb.drain(visit, 2);
        check(lap.lost >= 1 || got.size() <= 2, "v14.27 gather: a pass overwritten before it was read is counted lost");
        bool namesOwn = true;
        for (const auto& g : got) namesOwn = namesOwn && g.first == 3 + (g.second - 10);
        check(namesOwn, "v14.27 gather: no entry is delivered under a pass it was not written in");
    }
    {
        // Scripted: the own pad's callback lands on the far side of the tick every other frame (the pad is in pass N, then in no pass, then in two). The window
        // keeps the pad, the camera stays known, and a latch sample never spans an empty pass.
        struct E {
            uint32_t kind = 0;
            uint32_t stamp = 0;
        };
        std::atomic<uint32_t> stamp{1};
        se::PassGather<E, 4, 64> gb(stamp);
        struct P {
            uintptr_t addr = 0;
        };
        se::SeenWindow<P, 4> pads;
        se::PassGate gate;
        se::CamHold cam;
        int absent = 0;
        int oldRuleAbsent = 0;
        int camUnknown = 0;
        bool firstSeen = false;
        for (int f = 0; f < 200; ++f) {
            // frame f: 30 callbacks; the pad's callback is before the tick on even frames and after it on odd frames, so passes hold 2 or 0 pads
            const bool padAfter = (f & 1) != 0;
            auto cb = [&](bool pad) {
                se::PassGather<E, 4, 64>::Writer w(gb);
                w.call(0x55);
                E e;
                e.kind = pad ? 1 : 2;
                w.push(e);
            };
            for (int k = 0; k < 15; ++k) cb(false);
            if (!padAfter) cb(true);
            stamp.fetch_add(1);   // the tick of frame f
            bool padInPass = false;
            const se::PassDrain d = gb.drain([&](const E& e) {
                if (e.kind == 1) {
                    P p;
                    p.addr = 0xAD;
                    pads.put(p);
                    padInPass = true;
                }
            });
            const bool inf = gate.step(d.complete, d.passes, d.calls) == se::PassVerdict::kInformative;
            if (inf) {
                cam.resolved(d.ctx != 0);
                if (firstSeen && !pads.has(0xAD)) ++absent;
                if (firstSeen && !padInPass) ++oldRuleAbsent;
                if (padInPass) firstSeen = true;
                pads.endPass();
            }
            if (firstSeen && !cam.valid) ++camUnknown;
            if (padAfter) cb(true);
            for (int k = 0; k < 15; ++k) cb(false);
        }
        check(oldRuleAbsent > 50, "v14.27 scripted: the one-pass rule of v14.26 would have read the pad as absent on the passes the pad call missed");
        check(absent == 0, "v14.27 scripted: the pad is never absent when its call lands on the other side of the tick");
        check(camUnknown == 0, "v14.27 scripted: the camera is never unknown across those passes");
    }
    {
        // Scripted: the latch compares a record only between consecutive informative ticks. A pad that moves between two samples with an empty tick between
        // them is re-based, never counted as moving; the same motion on consecutive ticks is counted.
        se::LatchRec held;
        se::LatchRec live;
        const float ax0[3] = {1, 0, 0};
        const float ax2[3] = {0, 0, 1};
        uint32_t frame = 10;
        for (int k = 0; k < 8; ++k) {
            const float rel[3] = {5.0f * static_cast<float>(k), 0, 0};
            frame += 2;   // an empty tick between every two samples: the sample is skipped, so the stamps are two apart
            held.step(frame, rel, ax0, ax2, 1, 1, 1000);
        }
        check(held.movedRun == 0, "v14.27 latch: samples with an empty tick between them are re-based, never counted as motion");
        frame = 10;
        for (int k = 0; k < 8; ++k) {
            const float rel[3] = {5.0f * static_cast<float>(k), 0, 0};
            live.step(++frame, rel, ax0, ax2, 1, 1, 1000);
        }
        check(live.movedRun >= 2, "v14.27 latch: the same motion on consecutive informative ticks is counted");
    }
    {
        // Random interleavings: 1000 frames, callbacks spread over a burst, the tick landing anywhere in or around the burst, some callbacks long enough to
        // straddle a tick. A steady pad must give a continuous seal: no absence, no unknown camera, no motion release, and the rig publication must stay.
        struct E {
            uint32_t kind = 0;
            uint32_t stamp = 0;
        };
        using G = se::PassGather<E, 4, 128>;
        struct Ev {
            double t;
            int type;   // 0 begin, 1 end, 2 tick
            int id;
            bool pad;
        };
        int totalAbsent = 0, totalCamUnknown = 0, totalMotion = 0, totalSeal = 0, totalInformative = 0, totalPassNoPad = 0, totalRigGaps = 0, totalDeferred = 0;
        int totalOldRule = 0;
        for (uint32_t seed = 1; seed <= 6; ++seed) {
            std::mt19937 rng(seed);
            std::uniform_real_distribution<double> u01(0.0, 1.0);
            std::vector<Ev> evs;
            int id = 0;
            const int kFrames = 1000;
            for (int f = 0; f < kFrames; ++f) {
                const double base = 10.0 * f;
                const double start = base + 6.0 * u01(rng);
                const int n = 24;
                const int padAt = static_cast<int>(u01(rng) * n) % n;
                for (int k = 0; k < n; ++k) {
                    const double t = start + 4.0 * k / n;
                    const double dur = u01(rng) < 0.02 ? 3.0 : 0.01;   // some callbacks straddle a tick
                    evs.push_back({t, 0, id, k == padAt});
                    evs.push_back({t + dur, 1, id, false});
                    ++id;
                }
                evs.push_back({base + 10.0 + (u01(rng) * 2.0 - 1.0) * 6.0, 2, -1, false});   // the tick: anywhere around the burst's end and next start
            }
            std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.t != b.t ? a.t < b.t : a.type < b.type; });
            std::atomic<uint32_t> stamp{1};
            G gb(stamp);
            std::vector<std::unique_ptr<G::Writer>> open(static_cast<size_t>(id));
            struct P {
                uintptr_t addr = 0;
            };
            se::SeenWindow<P, 4> pads;
            se::PassGate gate;
            se::CamHold cam;
            se::LatchRec rec;
            auto pubHeap = std::make_unique<se::RigPubTable>();   // 400 KB: not on the stack
        se::RigPubTable& pub = *pubHeap;
            pub.begin();
            pub.insert(0x500, 11, se::kRigVerdictOutside, 0xC0, nullptr, 3);
            pub.commit();
            bool firstSeen = false;
            bool padInPass = false;
            uint32_t sealedFrames = 0;
            bool sealed = false;
            const float ax0[3] = {1, 0, 0};
            const float ax2[3] = {0, 0, 1};
            for (const Ev& ev : evs) {
                if (ev.type == 0) {
                    open[static_cast<size_t>(ev.id)] = std::make_unique<G::Writer>(gb);
                    G::Writer& w = *open[static_cast<size_t>(ev.id)];
                    w.call(0x99);
                    E e;
                    e.kind = ev.pad ? 1 : 2;
                    w.push(e);
                } else if (ev.type == 1) {
                    open[static_cast<size_t>(ev.id)].reset();
                } else {
                    stamp.fetch_add(1);
                    padInPass = false;
                    const se::PassDrain d = gb.drain(
                        [&](const E& e) {
                            if (e.kind == 1) {
                                P p;
                                p.addr = 0xAD;
                                pads.put(p);
                                padInPass = true;
                            }
                        },
                        1);
                    const se::PassVerdict v = gate.step(d.complete, d.passes, d.calls);
                    if (v == se::PassVerdict::kDeferred) ++totalDeferred;
                    if (v == se::PassVerdict::kInformative) {
                        ++totalInformative;
                        cam.resolved(d.ctx != 0);
                        if (firstSeen && !padInPass) ++totalOldRule;
                        if (firstSeen && !padInPass) ++totalPassNoPad;
                        if (firstSeen && !pads.has(0xAD)) ++totalAbsent;
                        if (padInPass) firstSeen = true;
                        pads.endPass();
                        const float rel[3] = {3.0f, 4.0f, -48.0f};   // a steady pad against its shell
                        rec.step(stamp.load(), rel, ax0, ax2, 1, 1, 1000);
                        if (rec.movedRun >= 2) ++totalMotion;
                        if (!sealed && gate.run >= 10) sealed = true;   // the camera-in-box rule: ten informative passes in a row (and it holds across holds below)
                        pub.begin();
                        pub.insert(0x500, 11, se::kRigVerdictOutside, 0xC0, nullptr, 3);
                        pub.commit();
                    }
                    if (firstSeen && !cam.valid) ++totalCamUnknown;
                    if (sealed) ++sealedFrames;
                    uint8_t vv = 0;
                    uintptr_t cc = 0;
                    size_t ii = 0;
                    if (firstSeen && !pub.lookup(0x500, 11, &vv, &cc, &ii)) ++totalRigGaps;
                }
            }
            totalSeal += static_cast<int>(sealedFrames);
        }
        check(totalInformative > 4000, "v14.27 random interleavings: most ticks read an informative pass");
        check(totalDeferred > 0, "v14.27 random interleavings: callbacks that straddle a tick did defer some passes (the cases are exercised)");
        check(totalOldRule > 200, "v14.27 random interleavings: the v14.26 one-pass rule would have lost the pad on many passes (the cases are exercised)");
        check(totalAbsent == 0, "v14.27 random interleavings: the own pad is never absent from the window, whatever the interleaving");
        check(totalCamUnknown == 0, "v14.27 random interleavings: the camera is never unknown");
        check(totalMotion == 0, "v14.27 random interleavings: a steady pad never reads as moving");
        check(totalRigGaps == 0, "v14.27 random interleavings: the rig publication stays across empty and unfinished passes");
        check(totalSeal > 0, "v14.27 random interleavings: the seal's pass condition is met");
    }
    // ---- Built v14.28: the latch measures in the hangar shell's frame; an origin rebase is not motion (flight 2d4ba6db: 50.85 m on every record) --------
    {
        struct Frame {   // the game's world frame: a rotation about z and a translation, applied to everything in it
            float c = 1.0f, s = 0.0f, t[3] = {0, 0, 0};
            void pos(const float p[3], float o[3]) const { o[0] = c * p[0] - s * p[1] + t[0]; o[1] = s * p[0] + c * p[1] + t[1]; o[2] = p[2] + t[2]; }
            void dir(const float d[3], float o[3]) const { o[0] = c * d[0] - s * d[1]; o[1] = s * d[0] + c * d[1]; o[2] = d[2]; }
        };
        struct Rec {
            float p[3];
            float a0[3];
            float a2[3];
        };
        // the shell with identity axes; a pad (index 0), a Lift, a gate ramp, in the shell's own frame
        const float shellP[3] = {100, 200, 0};
        const float sAx0[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        const Rec base[3] = {{{110, 200, -48}, {1, 0, 0}, {0, 0, 1}}, {{112, 203, -20}, {1, 0, 0}, {0, 0, 1}}, {{95, 210, -10}, {0, 1, 0}, {0, 0, 1}}};
        // Reads the three records and the shell in a world frame, as latchRead does (the shell read in the same call), into three LatchRecs.
        auto sample = [&](const Frame& fr, const Rec in[3], uint32_t frame, se::LatchRec out[3]) {
            float sp[3];
            fr.pos(shellP, sp);
            float sAx[3][3];
            for (int k = 0; k < 3; ++k) fr.dir(sAx0[k], sAx[k]);
            for (int i = 0; i < 3; ++i) {
                float p[3], a0[3], a2[3], rel[3], l0[3], l2[3];
                fr.pos(in[i].p, p);
                fr.dir(in[i].a0, a0);
                fr.dir(in[i].a2, a2);
                se::shellLocalSample(p, a0, a2, sp, sAx, rel, l0, l2);
                out[i].step(frame, rel, l0, l2, 1, 1, 1000);
            }
        };
        auto movedAny = [](const se::LatchRec r[3]) { return r[0].movedRun >= 2 || r[1].movedRun >= 2 || r[2].movedRun >= 2; };
        {
            se::LatchRec lr[3];
            Frame fr;
            Rec cur[3] = {base[0], base[1], base[2]};
            bool moved = false;
            for (uint32_t f = 1; f <= 60; ++f) {
                if (f == 20) fr.t[0] = 50.85f;   // the origin rebase of the flight: one vector for everything
                if (f == 35) {                   // and a turn of the whole frame
                    fr.c = 0.0f;
                    fr.s = 1.0f;
                    fr.t[1] = -300.0f;
                }
                sample(fr, cur, f, lr);
                moved = moved || movedAny(lr);
            }
            check(!moved, "v14.28 latch: a common 50.85 m translation and a common 90 degree turn of the whole frame give no motion");
        }
        {
            // the v14.27 reading: the shell taken from before the shift (stale), the records live: the same rebase reads as motion
            se::LatchRec lr[3];
            Frame after;
            after.t[0] = 50.85f;
            bool moved = false;
            for (uint32_t f = 1; f <= 30; ++f) {
                const Frame before;
                const Frame& live = f >= 10 ? after : before;
                for (int i = 0; i < 3; ++i) {
                    float p[3], a0[3], a2[3];
                    live.pos(base[i].p, p);
                    live.dir(base[i].a0, a0);
                    live.dir(base[i].a2, a2);
                    const float rel[3] = {p[0] - shellP[0], p[1] - shellP[1], p[2] - shellP[2]};
                    lr[i].step(f, rel, a0, a2, 1, 1, 1000);
                }
                moved = moved || movedAny(lr);
            }
            check(moved || lr[0].haveMotion, "v14.28 latch: the world-space reading with a stale shell does read the rebase as motion (the bug is reproduced)");
        }
        {
            se::LatchRec lr[3];
            Frame fr;
            Rec cur[3] = {base[0], base[1], base[2]};
            bool moved = false;
            for (uint32_t f = 1; f <= 40; ++f) {
                if (f == 20) fr.t[0] = 50.85f;
                if (f >= 25 && f <= 30) cur[1].p[2] += 0.5f;   // the Lift travels 0.5 m a frame for six frames: 3 m against the shell
                sample(fr, cur, f, lr);
                if (lr[1].movedRun >= 2) moved = true;
            }
            check(moved, "v14.28 latch: a Lift that moves 3 m against the shell releases, with a rebase in the same window");
        }
        {
            se::LatchRec lr[3];
            Frame fr;
            Rec cur[3] = {base[0], base[1], base[2]};
            bool moved = false;
            for (uint32_t f = 1; f <= 40; ++f) {
                if (f >= 20) {   // the ramp turns 89 degrees in place and shifts 0.65 m over seven frames
                    const float u = f < 27 ? static_cast<float>(f - 19) / 7.0f : 1.0f;
                    const float ang = u * 89.0f * 0.0174532925f;
                    cur[2].a0[0] = std::sin(ang);
                    cur[2].a0[1] = std::cos(ang);
                    cur[2].a0[2] = 0.0f;
                    cur[2].p[0] = base[2].p[0] + 0.65f * u;
                }
                sample(fr, cur, f, lr);
                if (lr[2].movedRun >= 2) moved = true;
            }
            check(moved, "v14.28 latch: a gate ramp that turns 89 degrees in place (0.65 m) still releases");
        }
        {
            // The pose-back rule across a rebase: the poses at the seal, the Lift moves away, the origin shifts and turns, the Lift comes back.
            se::LatchRec seal[3];
            Frame f0;
            Rec cur[3] = {base[0], base[1], base[2]};
            sample(f0, cur, 1, seal);
            Frame fr;
            cur[1].p[2] += 3.0f;   // away
            se::LatchRec lr[3];
            sample(fr, cur, 2, lr);
            float dM = 0.0f, dDeg = 0.0f;
            se::latchPoseDelta(lr[1].rel, seal[1].rel, lr[1].ax0, seal[1].ax0, lr[1].ax2, seal[1].ax2, &dM, &dDeg);
            check(!se::latchPoseWithin(dM, dDeg), "v14.28 pose-back: a Lift 3 m from its sealed pose is away");
            fr.t[0] = 50.85f;   // the rebase while it is away
            fr.c = std::cos(0.5f);
            fr.s = std::sin(0.5f);
            cur[1].p[2] = base[1].p[2];   // and it returns
            sample(fr, cur, 3, lr);
            bool allBack = true;
            for (int i = 0; i < 3; ++i) {
                se::latchPoseDelta(lr[i].rel, seal[i].rel, lr[i].ax0, seal[i].ax0, lr[i].ax2, seal[i].ax2, &dM, &dDeg);
                allBack = allBack && se::latchPoseWithin(dM, dDeg);
            }
            check(allBack, "v14.28 pose-back: a Lift that moved and returned is back at its sealed pose though the origin shifted and turned between");
        }
        {
            const float sd[3] = {50.85f, 0, 0};
            const float same[3][3] = {{50.85f, 0, 0}, {50.9f, 0.05f, 0}, {50.8f, 0, 0.04f}};
            check(se::commonFrameShift(same, 3, sd, 1.0f, 0.1f), "v14.28 backstop: the shell and every record moving by one vector is a rebase");
            const float lift[3][3] = {{50.85f, 0, 0}, {50.85f, 0, 2.0f}, {50.85f, 0, 0}};
            check(!se::commonFrameShift(lift, 3, sd, 1.0f, 0.1f), "v14.28 backstop: one record moving differently is not a rebase");
            const float still[3] = {0.02f, 0, 0};
            const float tiny[3][3] = {{0.02f, 0, 0}, {0.02f, 0, 0}, {0.02f, 0, 0}};
            check(!se::commonFrameShift(tiny, 3, still, 1.0f, 0.1f) && !se::commonFrameShift(tiny, 0, sd, 1.0f, 0.1f),
                  "v14.28 backstop: a small common move, or no records, is not a rebase");
        }
    }

    // ---- Built v14.29: the node cache with flags, the sampled gathers, the per-thread pin ---------------------------------------------------------------
    {
        const uintptr_t node = 0x7ff612345678ull & ~uintptr_t(7);
        const uint32_t key = se::classKey(7);
        const uint64_t v = se::packSlot(node, se::NameClass::kExplicit, se::kNodeMechanism | se::kNodeShipPrefix, key);
        se::NameClass cls = se::NameClass::kNone;
        uint32_t flags = 0;
        check(se::unpackSlot(v, node, key, &cls, &flags) && cls == se::NameClass::kExplicit && flags == (se::kNodeMechanism | se::kNodeShipPrefix),
              "v14.29 node cache: a slot returns the node's class and flags under its key");
        check(!se::unpackSlot(v, node + 8, key, &cls, &flags), "v14.29 node cache: another node in the same slot is a miss (node reuse)");
        check(!se::unpackSlot(v, node, se::classKey(8), &cls, &flags), "v14.29 node cache: a lifecycle generation (an epoch bump) reads every name again");
        check(!se::unpackSlot(0, node, key, &cls, &flags), "v14.29 node cache: an empty slot is a miss");
        const uint64_t v2 = se::packSlot(node, se::NameClass::kOther, 0, se::classKey(8));
        check(se::unpackSlot(v2, node, se::classKey(8), &cls, &flags) && cls == se::NameClass::kOther && flags == 0,
              "v14.29 node cache: after a bump the node's new name is cached with its own flags");
        check(se::nodeFlagsOf("Lift_01") == (se::kNodeMechanism | se::kNodeHangarLike | se::kNodeShipPrefix), "v14.29 node flags: a Lift is a mechanism and a hangar name");
        check(se::nodeFlagsOf("CobraMkV_Hull") == se::kNodeShipPrefix, "v14.29 node flags: a ship part has the prefix flag only");
        check(se::nodeFlagsOf("Rock") == 0 && se::nodeFlagsOf("") == 0, "v14.29 node flags: a plain or empty name has none");
        check((se::nodeFlagsOf("HangarGate_Ramp1_0") & se::kNodeMechanism) != 0, "v14.29 node flags: a hangar gate is a mechanism record");
        // which records an unsealed pass reads the centre of
        using NC = se::NameClass;
        check(!se::nodeWantsCentre(NC::kOther, 0, false, false, false, false), "v14.29 gather: a plain record is not read on a sampling-free pass");
        check(se::nodeWantsCentre(NC::kPad, 0, false, false, false, false), "v14.29 gather: a pad is always read");
        check(se::nodeWantsCentre(NC::kExplicit, 0, false, false, false, false) && !se::nodeWantsCentre(NC::kExplicit, 0, true, false, false, false),
              "v14.29 gather: an explicit part is read until the hangar watch gives up");
        check(se::nodeWantsCentre(NC::kOther, se::kNodeMechanism, false, true, false, false) && !se::nodeWantsCentre(NC::kOther, se::kNodeMechanism, false, false, false, false),
              "v14.29 gather: a mechanism record is read only while a latch is pending");
        check(se::nodeWantsCentre(NC::kOther, se::kNodeHangarLike, false, false, true, false) && !se::nodeWantsCentre(NC::kOther, se::kNodeHangarLike, false, false, false, false),
              "v14.29 gather: a hangar-like record is read only while the names are wanted");
        check(se::nodeWantsCentre(NC::kOther, 0, false, false, false, true), "v14.29 gather: a full pass reads every record");
        check(se::fullGatherPass(8, false, false) && se::fullGatherPass(16, false, false) && !se::fullGatherPass(9, false, false) && se::fullGatherPass(9, true, false),
              "v14.29 gather: every eighth stamp is a full pass unsealed, and every stamp while an engage is pending");
    }
    {
        // The sampled gathers over many ticks: the pad is seen at every informative tick (it is read on every pass), the own candidate and the tally
        // arrive on the full passes only, and a ship swap is seen after ten samples (about eighty ticks unsealed, ten sealed). Most records are never read.
        struct Rec {
            se::NameClass cls;
            uint32_t flags;
            bool inShip;   // within the ship's tally radius and containing the camera for the candidate
            bool pad;
            bool cand;
        };
        struct E {
            uint32_t kind = 0;   // 1 pad, 2 candidate, 3 tally
            uint32_t stamp = 0;
            int ship = 0;
        };
        using NC = se::NameClass;
        auto run = [&](bool gatherAll, int shipAfter, int ticks, int* seen, uint64_t* centreReads, uint64_t* calls, int* padSeen, int* candArrivals) {
            std::atomic<uint32_t> stamp{1};
            auto gbHeap = std::make_unique<se::PassGather<E, 4, 2048>>(stamp);
            se::PassGather<E, 4, 2048>& gb = *gbHeap;
            se::ShipWatch watch;
            *seen = -1;
            *centreReads = *calls = 0;
            *padSeen = *candArrivals = 0;
            std::vector<Rec> recs;
            recs.push_back({NC::kPad, 0, false, true, false});
            for (int i = 0; i < 40; ++i) recs.push_back({NC::kOther, se::kNodeShipPrefix, true, false, i == 0});   // a ship's parts; the first holds the camera
            for (int i = 0; i < 200; ++i) recs.push_back({NC::kOther, 0, false, false, false});                   // scenery
            for (int t = 0; t < ticks; ++t) {
                const uint32_t st = stamp.load();
                const int ship = (shipAfter >= 0 && t >= shipAfter) ? 2 : 1;
                const bool full = se::fullGatherPass(st, gatherAll, false);
                {
                    se::PassGather<E, 4, 2048>::Writer w(gb);
                    for (const Rec& r : recs) {
                        w.call(0x1);
                        ++*calls;
                        if (!se::nodeWantsCentre(r.cls, r.flags, false, false, false, full)) continue;
                        ++*centreReads;
                        E e;
                        if (r.pad) {
                            e.kind = 1;
                            w.push(e);
                        }
                        if (full && r.cand) {
                            e.kind = 2;
                            e.ship = ship;
                            w.push(e);
                        }
                        if (full && r.inShip && (r.flags & se::kNodeShipPrefix) != 0) {
                            e.kind = 3;
                            e.ship = ship;
                            w.push(e);
                        }
                    }
                }
                stamp.fetch_add(1);
                int candShip = 0;
                int tally = 0;
                bool pad = false;
                gb.drain([&](const E& e) {
                    if (e.kind == 1) pad = true;
                    if (e.kind == 2) candShip = e.ship;
                    if (e.kind == 3) ++tally;
                });
                if (pad) ++*padSeen;
                if (candShip != 0) {
                    ++*candArrivals;
                    char pfx[32];
                    std::snprintf(pfx, sizeof(pfx), "Ship%d_", candShip);
                    if (se::shipPrefixNamesShip(pfx, static_cast<uint32_t>(tally)) && watch.step(pfx) && *seen < 0) *seen = t;
                }
            }
        };
        int seen = -1, padSeen = 0, cands = 0;
        uint64_t reads = 0, calls = 0;
        run(false, 100, 400, &seen, &reads, &calls, &padSeen, &cands);
        check(padSeen >= 395, "v14.29 sampled gathers: the pad is read on every pass, so it is seen at every tick");
        check(cands >= 400 / 8 - 2 && cands <= 400 / 8 + 2, "v14.29 sampled gathers: the own candidate arrives on the full passes only (one tick in eight)");
        check(seen > 100 && seen <= 100 + 10 * 8 + 16, "v14.29 sampled gathers: an unsealed ship swap is seen after ten samples, about eighty ticks");
        check(reads * 100 < calls * 20, "v14.29 sampled gathers: the unsealed gather reads the centre of under a fifth of the records");
        run(true, 100, 400, &seen, &reads, &calls, &padSeen, &cands);
        check(cands >= 399 && seen > 100 && seen <= 112, "v14.29 sampled gathers: sealed or with an engage pending, every pass samples and the swap is seen in ten ticks");
    }
    {
        // The per-thread pin: a slot a thread holds is not rewritten, whatever the publisher does; a later pin moves to the newest.
        struct V {
            uint64_t a = 0, b = 0, c = 0, d = 0;
        };
        auto ringHeap = std::make_unique<se::SnapshotRing<V, 8, 4>>();
        se::SnapshotRing<V, 8, 4>& ring = *ringHeap;
        auto publish = [&](uint64_t n) {
            V& v = ring.beginWrite();
            v.a = v.b = v.c = v.d = n;
            ring.publish();
        };
        publish(1);
        size_t counted = 7;
        const V* held = ring.pinThread(&counted);
        const uint64_t first = held->a;
        for (uint64_t n = 2; n < 200; ++n) publish(n);
        check(counted == 0 && held->a == first && held->b == first && held->d == first,
              "v14.29 per-thread pin: the slot a thread holds is never rewritten while the publisher goes round the ring");
        size_t c2 = 7;
        const V* newest = ring.pinThread(&c2);
        check(newest->a == 199, "v14.29 per-thread pin: the next pin on the thread moves to the newest slot");
        // threads: more readers than hazard cells (4), so some pin by the shared count; none sees a torn value, the publisher never stalls
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> torn{0}, reads{0};
        std::vector<std::thread> readers;
        for (int r = 0; r < 10; ++r) {
            readers.emplace_back([&] {
                while (!stop.load(std::memory_order_relaxed)) {
                    size_t cnt = 0;
                    const V* p = ring.pinThread(&cnt);
                    const uint64_t a = p->a;
                    if (p->b != a || p->c != a || p->d != a) torn.fetch_add(1);
                    reads.fetch_add(1);
                    if (cnt != 0) ring.unpin(cnt - 1);
                }
            });
        }
        uint64_t n = 300;
        for (; n < 30300; ++n) publish(n);
        while (reads.load() < 1000) publish(n++);   // under load the readers may start late: keep publishing until they have read
        stop.store(true);
        for (auto& th : readers) th.join();
        check(torn.load() == 0 && reads.load() > 0, "v14.29 per-thread pin, threads: ten readers (four hazard cells, six counted pins) never see a torn slot");
    }

    // ---- Built v14.31: the node cache (a probed table, name beside the word), the sealed sampling, the sun shadow skip's gate --------------------------------
    {
        se::NodeCache<64> cache;
        const uintptr_t n1 = 0x7ff612345678ull & ~uintptr_t(7);
        se::NameClass cls = se::NameClass::kNone;
        uint32_t flags = 0;
        char name[64] = {};
        check(!cache.find(n1, 1, &cls, &flags, name), "v14.31 node cache: an empty cache misses");
        check(cache.put(n1, 1, se::NameClass::kExplicit, se::kNodeMechanism, "Lift_01") && cache.find(n1, 1, &cls, &flags, name) &&
                  cls == se::NameClass::kExplicit && flags == se::kNodeMechanism && std::strcmp(name, "Lift_01") == 0,
              "v14.31 node cache: a node comes back with its class, flags and name");
        check(!cache.find(n1 + 8, 1, &cls, &flags, name), "v14.31 node cache: another node is a miss");
        check(!cache.find(n1, 2, &cls, &flags, name), "v14.31 node cache: a lifecycle generation bump starts every node afresh");
        check(cache.put(n1, 2, se::NameClass::kOther, 0, "Rock") && cache.find(n1, 2, &cls, &flags, name) && std::strcmp(name, "Rock") == 0 && flags == 0,
              "v14.31 node cache: after a bump the node's new name is cached; the old generation's slot is reused");
        check(cache.find(n1, 2, &cls, &flags, nullptr), "v14.31 node cache: a lookup without a name works");
        // a long name is cut at 63 characters and terminated
        char longName[200];
        std::memset(longName, 'x', sizeof(longName));
        longName[199] = '\0';
        check(cache.put(n1 + 16, 2, se::NameClass::kOther, 0, longName) && cache.find(n1 + 16, 2, &cls, &flags, name) && std::strlen(name) == 63,
              "v14.31 node cache: a long name is cut to 63 characters");
        // the table is bounded: far more live nodes than slots, every put either lands or reports a full window, and no hit returns another node's name
        size_t landed = 0, refused = 0;
        for (uintptr_t k = 0; k < 400; ++k) {
            const uintptr_t node = 0x10000 + 8 * k;
            char nm[32];
            std::snprintf(nm, sizeof(nm), "N%llu", static_cast<unsigned long long>(k));
            if (cache.put(node, 5, se::NameClass::kOther, k & 7u, nm)) ++landed;
            else ++refused;
        }
        bool consistent = true;
        size_t hits = 0;
        for (uintptr_t k = 0; k < 400; ++k) {
            const uintptr_t node = 0x10000 + 8 * k;
            char nm[32];
            std::snprintf(nm, sizeof(nm), "N%llu", static_cast<unsigned long long>(k));
            if (cache.find(node, 5, &cls, &flags, name)) {
                ++hits;
                consistent = consistent && std::strcmp(name, nm) == 0 && flags == (k & 7u);
            }
        }
        check(landed <= 64 && refused >= 336 && hits == landed && consistent, "v14.31 node cache: the probe bound keeps the table bounded and a hit is always the node's own");
    }
    {
        // threads: writers and readers on a small cache; a hit must carry the name its node implies, never a torn one
        auto cacheHeap = std::make_unique<se::NodeCache<256>>();
        se::NodeCache<256>& cache = *cacheHeap;
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> bad{0}, hits{0};
        auto nameOf = [](uintptr_t node, char* out) { std::snprintf(out, 64, "Node_%llu_abcdefghijklmnopqrstuvwxyz", static_cast<unsigned long long>(node >> 3)); };
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&, t] {
                uint64_t x = 88172645463325252ull + static_cast<uint64_t>(t);
                while (!stop.load(std::memory_order_relaxed)) {
                    x ^= x << 13;
                    x ^= x >> 7;
                    x ^= x << 17;
                    const uintptr_t node = 0x20000 + 8 * (x % 600);
                    char want[64];
                    nameOf(node, want);
                    se::NameClass cls = se::NameClass::kNone;
                    uint32_t flags = 0;
                    char got[64];
                    if (cache.find(node, 3, &cls, &flags, got)) {
                        hits.fetch_add(1);
                        if (std::strcmp(got, want) != 0) bad.fetch_add(1);
                    } else {
                        cache.put(node, 3, se::NameClass::kOther, 0, want);
                    }
                }
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        stop.store(true);
        for (auto& th : threads) th.join();
        check(bad.load() == 0 && hits.load() > 0, "v14.31 node cache, threads: eight threads inserting and reading never see a torn or foreign name");
    }
    {
        // Release review 2026-10-14 finding 2: two writers rewrite ONE node's entry in place (the same node, class, flags and generation, so the published word is identical
        // every time) while readers copy its name. The name is a seqlock payload of atomic words; a hit must be exactly one of the two names, never a mix of both.
        auto cacheHeap = std::make_unique<se::NodeCache<64>>();
        se::NodeCache<64>& cache = *cacheHeap;
        const uintptr_t node = 0x30008;
        char nameA[64], nameB[64];
        std::memset(nameA, 'A', 63);
        std::memset(nameB, 'B', 63);
        nameA[63] = nameB[63] = '\0';
        check(cache.put(node, 3, se::NameClass::kOther, 0, nameA), "node cache seqlock: the entry is created");
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> torn{0}, hits{0}, misses{0}, puts{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&, t] {
                const char* mine = t == 0 ? nameA : nameB;
                while (!stop.load(std::memory_order_relaxed)) {
                    if (cache.put(node, 3, se::NameClass::kOther, 0, mine)) puts.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&] {
                while (!stop.load(std::memory_order_relaxed)) {
                    se::NameClass cls = se::NameClass::kNone;
                    uint32_t flags = 0;
                    char got[64];
                    std::memset(got, 0, sizeof(got));
                    if (cache.find(node, 3, &cls, &flags, got)) {
                        hits.fetch_add(1, std::memory_order_relaxed);
                        const char c = got[0];
                        bool whole = (c == 'A' || c == 'B') && got[63] == '\0';
                        for (int k = 0; whole && k < 63; ++k) whole = got[k] == c;
                        if (!whole) torn.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        misses.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        stop.store(true);
        for (auto& th : threads) th.join();
        check(torn.load() == 0 && hits.load() > 0 && puts.load() > 0, "node cache seqlock, threads: two writers rewriting one node and four readers never see a name that mixes the two");
        // after the writers stop the entry is whole and readable
        se::NameClass cls = se::NameClass::kNone;
        uint32_t flags = 0;
        char got[64];
        check(cache.find(node, 3, &cls, &flags, got) && (std::strcmp(got, nameA) == 0 || std::strcmp(got, nameB) == 0), "node cache seqlock: the entry is one whole name once the writers stop");
    }
    {
        // Built v14.32: a node freed and reallocated within a generation (a streamed asset) is caught by the name-text validator on the hit path.
        uint8_t padText[16] = {'L', 'a', 'n', 'd', 'i', 'n', 'g', 'P', 'a', 'd', 'S', 'm', 'a', 'l', 'l', 'A'};
        uint8_t liftText[16] = {'L', 'i', 'f', 't', '_', '0', '1', 0, 0, 0, 0, 0, 0, 0, 0, 0};
        const uint64_t vPad = se::nodeValidator(0x7f0000001000ull, padText);
        const uint64_t vPadSamePtrOtherText = se::nodeValidator(0x7f0000001000ull, liftText);
        const uint64_t vPadOtherPtr = se::nodeValidator(0x7f0000002000ull, padText);
        const uint8_t zero[16] = {};
        check(vPad != 0 && vPad != vPadSamePtrOtherText && vPad != vPadOtherPtr && se::nodeValidator(0, zero) != 0,
              "v14.32 node validator: it changes with the text pointer and with the first sixteen bytes, and is never zero");
        check(vPad == se::nodeValidator(0x7f0000001000ull, padText), "v14.32 node validator: the same pointer and bytes give the same validator");
        se::NodeCache<64> cache;
        const uintptr_t node = 0x7ff600001230ull & ~uintptr_t(7);
        se::NameClass cls = se::NameClass::kNone;
        uint32_t flags = 0;
        char name[64] = {};
        bool stale = false;
        check(cache.put(node, 4, se::NameClass::kPad, 0, "LandingPadSmallA", vPad), "v14.32 node reuse: a pad node is cached with its validator");
        check(cache.find(node, 4, &cls, &flags, name, vPad, &stale) && !stale && cls == se::NameClass::kPad && std::strcmp(name, "LandingPadSmallA") == 0,
              "v14.32 node reuse: the same name (the same validator) is a hit");
        stale = false;
        check(!cache.find(node, 4, &cls, &flags, name, vPadSamePtrOtherText, &stale) && stale,
              "v14.32 node reuse: the same node pointer, another name, the same generation is refused and reported stale");
        check(cache.put(node, 4, se::NameClass::kExplicit, se::kNodeMechanism, "Lift_01", vPadSamePtrOtherText), "v14.32 node reuse: the node is classified afresh and replaced in place");
        stale = false;
        check(cache.find(node, 4, &cls, &flags, name, vPadSamePtrOtherText, &stale) && !stale && cls == se::NameClass::kExplicit && flags == se::kNodeMechanism &&
                  std::strcmp(name, "Lift_01") == 0,
              "v14.32 node reuse: the new name is the hit");
        check(!cache.find(node, 4, &cls, &flags, name, vPad, &stale), "v14.32 node reuse: the old name no longer hits");
        check(cache.used(4) == 1, "v14.32 node reuse: the entry was replaced in place, not added beside the stale one");
        // an unreadable node (validator 0) stays a hit while it stays unreadable
        check(cache.put(node + 8, 4, se::NameClass::kUnnamed, 0, nullptr, 0) && cache.find(node + 8, 4, &cls, &flags, name, 0, &stale) && cls == se::NameClass::kUnnamed,
              "v14.32 node reuse: an unreadable node (validator 0) is a hit while it stays unreadable");
        check(!cache.find(node + 8, 4, &cls, &flags, name, vPad, &stale), "v14.32 node reuse: and a node that becomes readable is re-read");
    }
    {
        // The sealed sampling: the pass of stamp S is a full one every 4th stamp sealed, every 8th unsealed, every stamp with an engage pending.
        check(se::fullGatherPass(4, false, true) && se::fullGatherPass(8, false, true) && !se::fullGatherPass(5, false, true) && !se::fullGatherPass(6, false, true),
              "v14.31 sampled gathers: sealed, every fourth stamp is a full pass");
        check(se::fullGatherPass(8, false, false) && !se::fullGatherPass(4, false, false), "v14.31 sampled gathers: unsealed, every eighth");
        check(se::fullGatherPass(5, true, true) && se::fullGatherPass(5, true, false), "v14.31 sampled gathers: an engage pending gathers every pass");
        // ShipWatch over sampled readings: ten readings of a new prefix make the change, so about 40 ticks sealed and about 80 unsealed
        auto swap = [](bool sealed) {
            se::ShipWatch w;
            int seen = -1;
            for (uint32_t t = 0; t < 400; ++t) {
                if (!se::fullGatherPass(t, false, sealed)) continue;   // no reading on a pass that did not gather the candidate
                const char* pfx = t < 100 ? "Cobra_" : "Anaconda_";
                if (w.step(pfx) && seen < 0) seen = static_cast<int>(t);
            }
            return seen;
        };
        const int sealedSeen = swap(true);
        const int unsealedSeen = swap(false);
        check(sealedSeen >= 100 + 36 && sealedSeen <= 100 + 44, "v14.31 sampled gathers: a sealed ship swap is seen about forty ticks after it (ten readings, one in four)");
        check(unsealedSeen >= 100 + 72 && unsealedSeen <= 100 + 88, "v14.31 sampled gathers: an unsealed ship swap is seen about eighty ticks after it");
    }

    // ---- Built v14.33: the mechanism release arms after the bay has been still for 60 informative passes (flight 1718577b: a port whose gate ramps were
    // still closing when the game said r1 1) ---------------------------------------------------------------------------------------------------------
    {
        const float ax0[3] = {1, 0, 0};
        const float ax2[3] = {0, 0, 1};
        se::LatchRec rec;
        se::MechArm arm;
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::sealStep(inHangar(0), &st, &w);   // the seal is held from here on
        float z = 0.0f;
        uint32_t frame = 100;
        int releasedDuring = 0;
        int armedAt = -1;
        auto tick = [&](float moveZ, int index, bool r1) {
            z += moveZ;
            const float rel[3] = {3.0f, 4.0f, z};
            rec.step(++frame, rel, ax0, ax2, 1, 1, 1000 + frame);
            const bool allStill = rec.stamp == frame && rec.movedRun == 0;
            if (arm.step(allStill) && armedAt < 0) armedAt = index;
            se::SealInputs in = inHangar(1000 + frame);
            in.held = true;
            in.inHangar = r1;
            in.mechanismMoved = arm.armed && rec.movedRun >= 2;
            return se::sealStep(in, &st, &w);
        };
        // the gates close for 2 s (180 passes) after r1 1: moving every pass; nothing releases and nothing arms
        for (int i = 0; i < 180; ++i) {
            if (tick(0.5f, i, true) == se::SealAction::kRelease) ++releasedDuring;
        }
        check(releasedDuring == 0 && !arm.armed, "v14.33 arm: gates still closing for 2 s after r1 1: no release, not armed");
        // then still: it arms on the 60th still pass
        for (int i = 0; i < 59; ++i) tick(0.0f, 180 + i, true);
        check(!arm.armed, "v14.33 arm: 59 still passes do not arm");
        tick(0.0f, 239, true);
        check(arm.armed && armedAt == 239, "v14.33 arm: the 60th still pass arms (about 1 s)");
        for (int i = 0; i < 100; ++i) {
            if (tick(0.0f, 240 + i, true) == se::SealAction::kRelease) ++releasedDuring;
        }
        check(releasedDuring == 0, "v14.33 arm: armed and still, nothing releases");
        // a launch after arming releases, on the second moving pass
        bool released = false;
        for (int i = 0; i < 4 && !released; ++i) released = tick(0.5f, 340 + i, true) == se::SealAction::kRelease && w == se::ReleaseWhy::kMechanism;
        check(released, "v14.33 arm: a launch after arming releases by the mechanism");
    }
    {
        // an r1 1 to 0 before the arm still releases: the arm gates only the mechanism
        const float ax0[3] = {1, 0, 0};
        const float ax2[3] = {0, 0, 1};
        se::LatchRec rec;
        se::MechArm arm;
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::sealStep(inHangar(0), &st, &w);
        float z = 0.0f;
        for (uint32_t f = 1; f <= 20; ++f) {
            z += 0.5f;
            const float rel[3] = {3.0f, 4.0f, z};
            rec.step(f, rel, ax0, ax2, 1, 1, 1000 + f);
            arm.step(rec.stamp == f && rec.movedRun == 0);
        }
        se::SealInputs in = inHangar(2000);
        in.held = true;
        in.inHangar = false;
        in.mechanismMoved = arm.armed && rec.movedRun >= 2;
        check(!arm.armed && !in.mechanismMoved && se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kInHangarLeft,
              "v14.33 arm: an r1 1 to 0 before the arm still releases");
        // a record not read this pass (or a hole) restarts the count; a reset starts it afresh
        se::MechArm a2;
        for (int i = 0; i < 40; ++i) a2.step(true);
        a2.step(false);
        for (int i = 0; i < 59; ++i) a2.step(true);
        check(!a2.armed, "v14.33 arm: a pass with a record read late or moving restarts the count");
        a2.step(true);
        check(a2.armed, "v14.33 arm: and sixty clean passes after it arm");
        a2.reset();
        check(!a2.armed && a2.run == 0, "v14.33 arm: a new seal resets the arm");
    }

    // ---- Built v14.34: the optimized station in the flat profile -----------------------------------------------------------------------------------------
    {
        check(edvr::flatProfileReadsKey("fix.optimized_stations"), "v14.34 flat: fix.optimized_stations is on the flat key list (unlisted, getString answers off and the station never starts)");
        check(!edvr::flatProfileReadsKey("fix.optimized_station_x") && !edvr::flatProfileReadsKey("fix.supercruise_dust") && !edvr::flatProfileReadsKey("fix.fss_mode_latch"),
              "v14.34 flat: only that key was added (a near name and other fixes stay off the list)");
        check(edvr::flatPageHasRow("fix", "optimized_stations"), "v14.34 flat: the F8 flat page has the Optimized station row");
        const edvr::FlatPageRow* refused = edvr::flatPageFirstRefused([](const char* k) { return edvr::flatProfileReadsKey(k); });
        check(refused == nullptr, "v14.34 flat: every row of the flat page, the new one included, passes the flat key list");
        // the flat camera source: no temporal-pass rows and no explorer cam, so the position is the render context's view 0 origin
        edvr::view::Resolved flat;
        flat.originOk = true;
        flat.origin[0] = 120.0f;
        flat.origin[1] = -40.0f;
        flat.origin[2] = 7.5f;
        flat.poseOk = false;
        float c[3] = {};
        bool fromRows = true;
        check(edvr::view::sphereCentre(flat, c, &fromRows) && !fromRows && c[0] == 120.0f && c[1] == -40.0f && c[2] == 7.5f,
              "v14.34 flat camera: with no pose rows the camera is the view 0 origin");
        edvr::view::Resolved none;
        check(!edvr::view::sphereCentre(none, c), "v14.34 flat camera: with neither rows nor origin there is no camera (the station keeps its camera unknown)");
        edvr::view::Resolved both = flat;
        both.poseOk = true;
        both.pose.pos[0] = 1.0f;
        both.pose.pos[1] = 2.0f;
        both.pose.pos[2] = 3.0f;
        check(edvr::view::sphereCentre(both, c, &fromRows) && !fromRows && c[0] == 120.0f, "v14.34 flat camera: rows present as well, the origin stays the source (as in VR)");
        // the camera rule on that position: inside a small bay box, outside it
        se::OwnCell cell;
        cell.has = true;
        cell.box = true;
        cell.boxHalf[0] = 76.0f;
        cell.boxHalf[1] = 37.5f;
        cell.boxHalf[2] = 129.0f;
        for (int k = 0; k < 3; ++k) {
            cell.boxCentre[k] = flat.origin[k];
        }
        cell.boxU[0] = 1.0f;
        cell.boxV[1] = 1.0f;
        cell.boxW[2] = 1.0f;
        float x[3] = {};
        se::boxCoordsOf(cell, c, x);
        check(std::fabs(x[0]) <= cell.boxHalf[0] && std::fabs(x[1]) <= cell.boxHalf[1] && std::fabs(x[2]) <= cell.boxHalf[2],
              "v14.34 flat camera: the view 0 origin at the box centre is inside the box");
        const float away[3] = {flat.origin[0] + 300.0f, flat.origin[1], flat.origin[2]};
        se::boxCoordsOf(cell, away, x);
        check(!(std::fabs(x[0]) <= cell.boxHalf[0] + se::kCameraLeaveM), "v14.34 flat camera: 300 m along the box is outside it (the camera rule releases)");
    }

    std::printf("optimized_station_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

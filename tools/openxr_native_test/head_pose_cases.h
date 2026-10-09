#pragma once
// Elite's "now" head pose, answered at the drawn frame's display time (src/openxr/head_pose_time.h, native_runtime_host.h locateHeadFor,
// src/openxr/pose_gap.h; docs\terrain-culling.md): through the real hop to the owner thread, from a foreign thread, the instant a call is
// located at, the figures the diagnostic takes from each locate, what the WaitGetPoses publish point keeps, and who is said to be answered.
// The window arithmetic, the filter and the line formats are held in tools\openxr_pose_test.
#include "launch_centre_cases.h"
#include "frame_end_overlap_cases.h"
#include <cmath>
#include <cstring>

namespace edvr::openxr::test {
template<class Check> void runHeadPoseCases(Check&& check) {
  using namespace launch_fixture;
  const auto close = [](double a, double b, double eps) { return std::fabs(a - b) <= eps; };
  OwnerBuiltFixture f;
  check(f.route.bind() && f.owner.start() && f.owner.invoke([&] { f.build(); }) && f.initialized, "(the head-pose cases' host is built on its own owner thread)");
  if (!f.initialized) return;
  auto& h = *f.hostPtr;
  const DWORD me = GetCurrentThreadId();
  const XrTime display = 5000000000, period = 11111111;
  char lines[8][352]{};
  unsigned lineCount = 0;
  const auto collect = [&](const char* line) { if (lineCount < 8) std::snprintf(lines[lineCount++], sizeof(lines[0]), "%s", line); };
  const double half = 3.14159265358979323846 / 180.0 * 0.5;
  const auto yawed = [&](double degrees) { f.fake.head.orientation = {0.0f, float(std::sin(degrees * half)), 0.0f, float(std::cos(degrees * half))}; };
  // The publish point: the frame the game now holds, kept for the pose calls to be located at and measured against.
  XrVector3f turn{0.0f, 0.5235988f, 0.0f};   // 30 degrees a second about y
  bool published = false;
  check(f.owner.invoke([&] {
    h.publishPoseGap(display, period, XrPosef{{0, 0, 0, 1}, {0, 0, 0}}, true, XR_SPACE_VELOCITY_ANGULAR_VALID_BIT, turn, 1000, collect);
    published = h.latestPoseFrame.valid;
  }) && published, "(a frame is published to the host)");
  bool kept = false;
  f.owner.invoke([&] {
    const auto& p = h.latestPoseFrame;
    kept = p.displayTime == display && p.period == period && p.orientationKnown && p.speedKnown && close(p.speedDegPerSec, 30.0, 1e-3) &&
           p.orientation[0] == 0 && p.orientation[3] == 1 && h.poseGap.waits() == 1;
  });
  check(kept, "the publish point keeps the frame's display time and period, its orientation, the head speed in degrees a second, and counts the wait");
  vr::TrackedDevicePose_t pose{};
  const auto locateAs = [&](bool atDisplay, float prediction, uint32_t rva = 0x4E3881) {
    HeadCall call;call.thread = me;call.rva = rva;call.display = atDisplay;
    pose = {};
    return h.locateHeadFor(h.geometryGeneration, vr::TrackingUniverseSeated, prediction, call, pose);
  };
  const auto flushLine = [&](std::string& out) {
    lineCount = 0;
    f.owner.invoke([&] { h.poseGap.flushNow(0, collect); });
    out = lineCount ? lines[lineCount - 1] : "";
    return lineCount;
  };
  yawed(0.0);
  // ---- inside the game's image and asking for "now": the display time ----------------------------------------------------------------
  check(locateAs(true, 0.0f) && f.fake.lastTime == display && h.lastHeadTime == display && pose.bPoseIsValid,
        "a call flagged as Elite's \"now\" is located at exactly the latest frame's display time");
  check(locateAs(true, 0.003f) && f.fake.lastTime == display && locateAs(true, -0.004f) && f.fake.lastTime == display,
        "...whatever small prediction Elite passed (the filter, not the host, decides what is \"now\")");
  std::string line;
  flushLine(line);
  char want[512];
  std::snprintf(want, sizeof(want), "pose gap: tid %lu calls 3 from exe+0x4E3881 prediction -0.3 ms target-minus-display mean 0.00 ms (min 0.00 max 0.00) angle-to-drawn mean 0.000 max 0.000 deg head 30.0 deg/s waitgetposes 1 failed 0", (unsigned long)me);
  check(line == want, "the diagnostic records the CALLER's thread (the hop to the owner does not lose it), the return RVA, the mean prediction, a gap of 0, an angle of 0 and the wait the publish point counted (the exact line)");
  // ---- everything else: today's answer ------------------------------------------------------------------------------------------------
  check(locateAs(false, 0.0f) && f.fake.lastTime != display, "a call that is not flagged (outside the image, or with a real prediction) is located on the wall clock as always, not at the display time");
  flushLine(line);
  check(line.find("target-minus-display mean ") != std::string::npos && line.find("target-minus-display mean 0.00") == std::string::npos && line.find("fallback") == std::string::npos,
        "...and its gap is the true one, not 0, with no fallback (it was never asked for the display time)");
  const XrTime lateTarget = f.fake.lastTime;
  check(locateAs(false, 0.25f) && f.fake.lastTime - lateTarget >= 249000000 && f.fake.lastTime - lateTarget < 300000000,
        "...and the prediction it passed is still added: a quarter second later than a call a moment ago");
  const double lateGap = double(f.fake.lastTime - display) / 1e6;
  flushLine(line);
  char wantGap[160];
  std::snprintf(wantGap, sizeof(wantGap), "target-minus-display mean %.2f ms (min %.2f max %.2f)", lateGap, lateGap, lateGap);
  check(line.find(wantGap) != std::string::npos, "...and its gap is the located instant less the display time, in milliseconds");
  // The angle: the pose handed back against the pose that frame was drawn with.
  yawed(10.0);
  check(locateAs(true, 0.0f), "(a head turned 10 degrees from the drawn frame)");
  flushLine(line);
  check(line.find("angle-to-drawn mean 10.000 max 10.000 deg") != std::string::npos, "the angle between the pose handed back and the drawn pose is 10.000 degrees");
  yawed(0.0);
  // ---- no frame to be answered at: today's answer, counted ----------------------------------------------------------------------------
  f.owner.invoke([&] { h.latestPoseFrame.valid = false; });
  check(locateAs(true, 0.0f, 0x777777) && f.fake.lastTime != display, "with no frame yet an Elite \"now\" call is located at now + prediction, as before");
  flushLine(line);
  check(line.find("target-minus-display n/a angle-to-drawn n/a head n/a") != std::string::npos && line.find(" fallback 1") != std::string::npos,
        "...and the line says there was nothing to measure against, and one fallback");
  f.owner.invoke([&] { h.latestPoseFrame.valid = true;h.latestPoseFrame.displayTime = 0; });
  check(locateAs(true, 0.0f, 0x777777) && f.fake.lastTime != 0, "a display time that is not positive falls back too");
  flushLine(line);
  check(line.find(" fallback 1") != std::string::npos, "...counted as a fallback");
  f.owner.invoke([&] { h.latestPoseFrame.displayTime = -5; });
  check(locateAs(true, 0.0f, 0x777777) && f.fake.lastTime != -5, "...a negative one too");
  flushLine(line);
  f.owner.invoke([&] { h.latestPoseFrame.displayTime = display; });
  // ---- who is said to be answered ------------------------------------------------------------------------------------------------------
  check(h.poseSightings.distinct() == 1, "(so far one Elite caller has been answered at the display time: the fallbacks (from another address) and the unflagged calls said nothing)");
  check(locateAs(true, 0.0f, 0x4E3881), "(the same caller again)");
  check(h.poseSightings.distinct() == 1, "the same return address is not said twice");
  check(locateAs(true, 0.0f, 0x1234567), "(another caller in the image)");
  check(h.poseSightings.distinct() == 2, "a further distinct return address is said once (a caller that appears after a game update)");
  flushLine(line);
  // ---- failures: the owner's busy gate, a retired generation, an origin the runtime does not locate, a locate the runtime refuses ---
  HeadCall call;call.thread = me;call.rva = 0x4E3881;call.display = true;
  vr::TrackedDevicePose_t none{};
  check(!h.locateHeadFor(h.geometryGeneration + 1, vr::TrackingUniverseSeated, 0.0f, call, none) &&
            !h.locateHeadFor(h.geometryGeneration, vr::TrackingUniverseStanding, 0.0f, call, none),
        "a stale generation and an origin the runtime does not locate return false");
  f.fake.locateResult = XR_ERROR_TIME_INVALID;
  check(!h.locateHeadFor(h.geometryGeneration, vr::TrackingUniverseSeated, 0.0f, call, none), "...so does a locate the runtime refuses");
  f.fake.locateResult = XR_SUCCESS;
  flushLine(line);
  check(line.find(" calls 3 ") != std::string::npos && line.find(" failed 3") != std::string::npos && line.find("target-minus-display n/a") != std::string::npos,
        "all three failures are counted in `failed` on the caller's line, the silent busy-gate path included, and left out of every mean");
  check(h.poseSightings.distinct() == 2, "(a failed locate says nothing of who is answered)");
  h.noteHeadCallFailed(call, 0.5f);
  flushLine(line);
  check(line.find(" calls 1 ") != std::string::npos && line.find(" failed 1") != std::string::npos && line.find("prediction 500.0 ms") != std::string::npos,
        "a pose call that never reached the owner (no live session, a bad origin) is counted too, with its prediction");
  // The old entry point is the plain call.
  check(h.locateHead(h.geometryGeneration, vr::TrackingUniverseSeated, 0.0f, none) && f.fake.lastTime != display, "locateHead (no caller known) is located at now + prediction, as it always was");
  flushLine(line);
  check(line.find(" tid 0 calls 1 from exe+0x0 ") != std::string::npos, "...and is recorded under thread 0, RVA 0");
  // The publish point writes the window when it is 60 s old, and only then.
  f.owner.invoke([&] { h.poseGap.flushNow(0, collect); });
  lineCount = 0;
  f.owner.invoke([&] {
    PoseGapStats::Sample s{};s.thread = 5;s.rva = 0x4E3881;s.located = true;
    h.poseGap.note(s);
    h.publishPoseGap(display, period, XrPosef{{0, 0, 0, 1}, {0, 0, 0}}, true, 0, XrVector3f{0, 0, 0}, 59999, collect);
  });
  check(lineCount == 0, "the publish point writes nothing for a window under 60 s old");
  f.owner.invoke([&] { h.publishPoseGap(display, period, XrPosef{{0, 0, 0, 1}, {0, 0, 0}}, true, 0, XrVector3f{0, 0, 0}, 60000 + 1, collect); });
  check(lineCount == 1 && std::string(lines[0]).rfind("pose gap: tid 5 calls 1 ", 0) == 0, "...and the window's line once it is 60 s old");
  f.owner.stop();
}
} // namespace edvr::openxr::test

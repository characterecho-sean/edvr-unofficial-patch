#pragma once
// advanced.cull_pose, the runtime's half (src/common/cull_pose.h, native_runtime_host.h publishCullPose / locateHeadFor, src/openxr/pose_gap.h;
// docs\terrain-culling.md round 6): what the host publishes to the game's reads, the instant a Display or Next call is located at (through the
// real hop to the owner thread, from a foreign thread), the figures the instrument takes from each locate, and what the WaitGetPoses publish
// point keeps. The window arithmetic and the line format are held in tools\openxr_pose_test.
#include "launch_centre_cases.h"
#include "frame_end_overlap_cases.h"
#include "cull_probe_cases.h"
#include <cmath>
#include <cstring>

namespace edvr::openxr::test {
template<class Check> void runCullPoseCases(Check&& check) {
  using namespace launch_fixture;
  using probe_fixture::build332841;
  namespace cp = edvr::cullpose;
  const auto close = [](double a, double b, double eps) { return std::fabs(a - b) <= eps; };
  // ---- what the host publishes to the game's reads ------------------------------------------------------------------------------------
  {
    Fixture f;auto& h = f.host;
    h.featureFrame.cullPose = 3;
    h.publishCullPose();
    check(h.poseMode == 0u && h.geometry.read().cullPose == 0u && h.poseStandDownNoted,
          "on another build (this rig's own executable) the runtime stands down: nothing is published, and it says so once");
    h.publishCullPose();
    check(h.poseStandDownNoted && h.geometry.read().cullPose == 0u, "...and is not noted twice");
    h.systemInterface.callers().useModule(build332841());
    h.publishCullPose();
    check(h.poseMode == 3u && h.geometry.read().cullPose == 3u && !h.poseStandDownNoted, "on build 332841 display_direct is published as 3 (the runtime locates it as display; the graphics half owns the patch)");
    bool codes = true;
    for (uint32_t code = 0; code <= 4; ++code) {
      h.featureFrame.cullPose = code;h.publishCullPose();
      codes = codes && h.geometry.read().cullPose == code && h.poseMode == code;
    }
    check(codes, "every code 0..4 is published as itself");
    h.featureFrame.cullPose = 5;h.publishCullPose();
    check(h.geometry.read().cullPose == 0u && !h.poseStandDownNoted, "a code past 4 reads off, and an off key is not a stand-down");
    h.featureFrame.cullPose = 0xFFFFFFFFu;h.publishCullPose();
    check(h.geometry.read().cullPose == 0u, "...a wild one too");
    SystemRead fresh{};fresh.cullPose = 4;
    SystemPublication publication;
    publication.begin(fresh);
    check(publication.read().cullPose == 0u, "a new session generation starts with the switch off, whatever the metadata said");
  }
  // ---- locating at the instant, through the hop from a foreign thread -----------------------------------------------------------------------
  {
    OwnerBuiltFixture f;
    check(f.route.bind() && f.owner.start() && f.owner.invoke([&] { f.build(); }) && f.initialized, "(the pose cases' host is built on its own owner thread)");
    if (f.initialized) {
      auto& h = *f.hostPtr;
      const DWORD me = GetCurrentThreadId();
      const XrTime display = 5000000000, period = 11111111;
      char lines[8][352]{};
      unsigned lineCount = 0;
      const auto collect = [&](const char* line) { if (lineCount < 8) std::snprintf(lines[lineCount++], sizeof(lines[0]), "%s", line); };
      const double half = 3.14159265358979323846 / 180.0 * 0.5;
      const auto yawed = [&](double degrees) { f.fake.head.orientation = {0.0f, float(std::sin(degrees * half)), 0.0f, float(std::cos(degrees * half))}; };
      // The publish point: the frame the game now holds, kept for the pose calls to be located and measured against.
      XrVector3f turn{0.0f, 0.5235988f, 0.0f};   // 30 degrees a second about y
      bool published = false;
      check(f.owner.invoke([&] {
        h.poseMode = 0;
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
      const auto locateAs = [&](cp::Time time, float prediction, uint32_t rva = cp::kDirectPoseReturnRva) {
        HeadCall call;call.thread = me;call.rva = rva;call.time = time;
        pose = {};
        return h.locateHeadFor(h.geometryGeneration, vr::TrackingUniverseSeated, prediction, call, pose);
      };
      const auto flushLine = [&](uint32_t mode, std::string& out) {
        lineCount = 0;
        f.owner.invoke([&] { h.poseGap.flushNow(0, mode, collect); });
        out = lineCount ? lines[lineCount - 1] : "";
        return lineCount;
      };
      yawed(0.0);
      check(locateAs(cp::Time::Display, -0.25f) && f.fake.lastTime == display && h.lastHeadTime == display && pose.bPoseIsValid,
            "a Display call is located at exactly the latest frame's display time, whatever prediction Elite passed");
      std::string line;
      flushLine(1, line);
      char want[512];
      std::snprintf(want, sizeof(want), "pose gap: mode display tid %lu calls 1 from exe+0x4E3881 prediction -250.0 ms target-minus-display mean 0.00 ms (min 0.00 max 0.00) angle-to-drawn mean 0.000 max 0.000 deg head 30.0 deg/s waitgetposes 1 failed 0", (unsigned long)me);
      check(line == want, "...the instrument records the CALLER's thread (the hop to the owner does not lose it), the return RVA, the prediction, a gap of 0, an angle of 0 and the wait the publish point counted (the exact line)");
      check(locateAs(cp::Time::Next, 0.0f) && f.fake.lastTime == display + period && h.lastHeadTime == display + period,
            "a Next call is located one display period later: the arithmetic the game-pose array uses");
      flushLine(2, line);
      check(line.find("target-minus-display mean 11.11 ms (min 11.11 max 11.11)") != std::string::npos, "...a gap of one period, 11.11 ms");
      check(locateAs(cp::Time::Now, 0.0f) && f.fake.lastTime != display && f.fake.lastTime != display + period,
            "a Now call is located on the wall clock as always, not at either frame instant");
      flushLine(0, line);
      check(line.find("target-minus-display mean ") != std::string::npos && line.find("target-minus-display mean 0.00") == std::string::npos && line.find("fallback") == std::string::npos,
            "...with a gap that is not 0, and no fallback (it was never asked for a frame instant)");
      // The angle: the pose handed back against the pose that frame was drawn with.
      yawed(10.0);
      check(locateAs(cp::Time::Display, 0.0f), "(a head turned 10 degrees from the drawn frame)");
      flushLine(1, line);
      check(line.find("angle-to-drawn mean 10.000 max 10.000 deg") != std::string::npos, "the angle between the pose handed back and the drawn pose is 10.000 degrees");
      yawed(0.0);
      // A call that cannot be formed falls back to now + prediction, and is counted as such.
      f.owner.invoke([&] { h.latestPoseFrame.valid = false; });
      check(locateAs(cp::Time::Display, 0.0f) && f.fake.lastTime != display && f.fake.lastTime != display + period, "with no frame yet a Display call is located at now + prediction, as before");
      flushLine(1, line);
      check(line.find("target-minus-display n/a angle-to-drawn n/a head n/a") != std::string::npos && line.find(" fallback 1") != std::string::npos,
            "...and the line says there was nothing to measure against, and one fallback");
      f.owner.invoke([&] { h.latestPoseFrame.valid = true;h.latestPoseFrame.period = 0; });
      check(locateAs(cp::Time::Next, 0.0f) && f.fake.lastTime != display && f.fake.lastTime != display + period, "a Next call with no positive period falls back too");
      flushLine(2, line);
      check(line.find(" fallback 1") != std::string::npos, "...counted as a fallback");
      f.owner.invoke([&] { h.latestPoseFrame.period = period; });
      // A reference-space change pending between the display time and one period later: Next would cross it and falls back; Display does not.
      f.owner.invoke([&] {
        XrEventDataReferenceSpaceChangePending event{XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING};
        event.session = session();event.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;event.poseValid = XR_FALSE;event.changeTime = display + period / 2;
        h.changes.note(event);
      });
      check(locateAs(cp::Time::Next, 0.0f) && f.fake.lastTime != display + period && f.fake.lastTime != display, "a Next call that would cross a pending reference-space change is located at now + prediction instead");
      flushLine(2, line);
      check(line.find(" fallback 1") != std::string::npos, "...and counted as a fallback");
      check(locateAs(cp::Time::Display, 0.0f) && f.fake.lastTime == display, "(a Display call, which crosses nothing, is not affected)");
      f.owner.invoke([&] { uint64_t applied = 0;h.changes.advance(display + period, applied); });
      check(locateAs(cp::Time::Next, 0.0f) && f.fake.lastTime == display + period, "...and once the change is applied Next is located one period later again");
      flushLine(1, line);   // (a clean window for the failures below)
      // Failures: the owner's busy gate, a retired generation, an origin the runtime does not locate, a locate the runtime refuses.
      HeadCall call;call.thread = me;call.rva = cp::kDirectPoseReturnRva;call.time = cp::Time::Display;
      vr::TrackedDevicePose_t none{};
      check(!h.locateHeadFor(h.geometryGeneration + 1, vr::TrackingUniverseSeated, 0.0f, call, none) &&
                !h.locateHeadFor(h.geometryGeneration, vr::TrackingUniverseStanding, 0.0f, call, none),
            "a stale generation and an origin the runtime does not locate return false");
      f.fake.locateResult = XR_ERROR_TIME_INVALID;
      check(!h.locateHeadFor(h.geometryGeneration, vr::TrackingUniverseSeated, 0.0f, call, none), "...so does a locate the runtime refuses");
      f.fake.locateResult = XR_SUCCESS;
      flushLine(1, line);
      check(line.find(" calls 3 ") != std::string::npos && line.find(" failed 3") != std::string::npos && line.find("target-minus-display n/a") != std::string::npos,
            "all three failures are counted in `failed` on the caller's line, the silent busy-gate path included, and left out of every mean");
      h.noteHeadCallFailed(call, 0.5f);
      flushLine(1, line);
      check(line.find(" calls 1 ") != std::string::npos && line.find(" failed 1") != std::string::npos && line.find("prediction 500.0 ms") != std::string::npos,
            "a pose call that never reached the owner (no live session, a bad origin) is counted too, with its prediction");
      // The old entry point is the plain call.
      check(h.locateHead(h.geometryGeneration, vr::TrackingUniverseSeated, 0.0f, none) && f.fake.lastTime != display && f.fake.lastTime != display + period, "locateHead (no caller known) is located at now + prediction, as it always was");
      flushLine(0, line);
      check(line.find(" tid 0 calls 1 from exe+0x0 ") != std::string::npos, "...and is recorded under thread 0, RVA 0");
      // The publish point writes a window under the mode it was opened under, and closes it as soon as the published mode changes.
      f.owner.invoke([&] { h.poseGap.flushNow(0, 0, collect);h.poseMode = 4; });
      lineCount = 0;
      f.owner.invoke([&] {
        PoseGapStats::Sample s{};s.thread = 5;s.rva = cp::kDirectPoseReturnRva;s.located = true;
        h.poseGap.note(s);
        h.publishPoseGap(display, period, XrPosef{{0, 0, 0, 1}, {0, 0, 0}}, true, 0, XrVector3f{0, 0, 0}, 10, collect);
      });
      check(lineCount == 1 && std::string(lines[0]).rfind("pose gap: mode off tid 5 calls 1 ", 0) == 0, "the publish point flushes a window the moment the mode it publishes differs from the one the window was opened under, labelled with the old mode");
      f.owner.invoke([&] { h.poseGap.flushNow(0, 4, collect); });
      check(lineCount == 1, "(an empty window writes nothing)");
      f.owner.stop();
    }
  }
}
} // namespace edvr::openxr::test

#pragma once
// Elite's "now" head pose, answered at the drawn frame's display time (src/openxr/head_pose_time.h; docs\terrain-culling.md): which calls it
// answers (a return address inside the game's image and a prediction under 5 ms either way, at its boundary and on both signs), which it
// leaves alone, the instant it forms under each advanced.cull_pose time (now, display, next), and the log lines that say whom it is answering,
// and when the cap is hit.
#include "../../src/openxr/head_pose_time.h"
#include "../../src/openxr/space_pose.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace head_pose_time_cases {
using namespace edvr::openxr;
namespace cullpose = ::edvr::cullpose;

template<class Check>
void runHeadPoseTimeCases(Check&& check) {
  // ---- what "now" is ---------------------------------------------------------------------------------------------------------------------
  {
    check(kNowPredictionLimitSeconds==0.005f,"the bound is 5 ms");
    check(isNowRequest(0.0f)&&isNowRequest(-0.0f)&&isNowRequest(0.001f)&&isNowRequest(0.0049f)&&isNowRequest(0.00499f)&&isNowRequest(0.004999f),
          "a prediction under 5 ms is a request for \"now\"");
    check(!isNowRequest(0.005f)&&!isNowRequest(0.0051f)&&!isNowRequest(0.011f)&&!isNowRequest(0.0111111f)&&!isNowRequest(0.1f)&&!isNowRequest(1.0f),
          "...exactly 5 ms is already a prediction, and so is anything over it (a frame period is 11.1 ms)");
    check(isNowRequest(-0.001f)&&isNowRequest(-0.0049f)&&isNowRequest(-0.004999f)&&!isNowRequest(-0.005f)&&!isNowRequest(-0.0051f)&&!isNowRequest(-0.011f)&&!isNowRequest(-0.25f),
          "...on both signs: -4.9 ms is \"now\", -5 ms and the quarter second Elite's older callers pass are not");
    check(!isNowRequest(std::numeric_limits<float>::quiet_NaN())&&!isNowRequest(std::numeric_limits<float>::infinity())&&!isNowRequest(-std::numeric_limits<float>::infinity()),
          "a NaN and an infinity are not \"now\"");
  }
  // ---- whose call it is --------------------------------------------------------------------------------------------------------------------
  {
    check(answeredAtDisplayTime(0x4E3881,0.0f)&&answeredAtDisplayTime(0,0.0f)&&answeredAtDisplayTime(0x6000000,0.003f)&&answeredAtDisplayTime(kFrameUnknown-1,-0.004f),
          "a return address inside the image with a prediction of about 0 is answered at the display time: the build's own RVA is not asked");
    check(!answeredAtDisplayTime(0x4E3881,0.005f)&&!answeredAtDisplayTime(0x4E3881,-0.005f)&&!answeredAtDisplayTime(0x4E3881,0.25f)&&!answeredAtDisplayTime(0x4E3881,-0.25f)&&
              !answeredAtDisplayTime(0x4E3881,0.011f),
          "...a real prediction from inside the image is not (a caller that wants the future)");
    check(!answeredAtDisplayTime(kFrameOutside,0.0f)&&!answeredAtDisplayTime(kFrameUnknown,0.0f)&&!answeredAtDisplayTime(kFrameOutside,0.25f),
          "...a caller outside the image, or one whose address was not captured, never is");
    // The image: where an address is inside it.
    const ExeModule module{0x140000000ull,0x1000,0x1234,0x1000};
    check(frameRva(module,0x140000000ull)==0&&frameRva(module,0x140000FFFull)==0xFFF&&frameRva(module,0x140001000ull)==kFrameOutside&&frameRva(module,0x13FFFFFFFull)==kFrameOutside&&
              frameRva(module,0)==kFrameUnknown&&frameRva(ExeModule{},0x140000000ull)==kFrameOutside,
          "an address is inside the image from its base to the last byte of its size; one past either end, and any address when the image is unknown, is outside; null is not captured");
    check(answeredAtDisplayTime(frameRva(module,0x140000800ull),0.0f)&&!answeredAtDisplayTime(frameRva(module,0x140001000ull),0.0f)&&!answeredAtDisplayTime(frameRva(module,0x13FFFFFFFull),0.0f),
          "...so the filter holds at the image's edges");
  }
  // ---- the instant ---------------------------------------------------------------------------------------------------------------------------
  {
    int64_t t=-7;
    check(displayTimeTarget(5000000000,&t)&&t==5000000000&&displayTimeTarget(1,&t)&&t==1&&displayTimeTarget(INT64_MAX,&t)&&t==INT64_MAX,"the instant is the latest frame's display time");
    t=-7;
    check(!displayTimeTarget(0,&t)&&!displayTimeTarget(-1,&t)&&!displayTimeTarget(INT64_MIN,&t)&&t==-7,"with no frame yet (a display time that is not positive) there is none, and the output is left alone");
  }
  // ---- how long a display time stays good ----------------------------------------------------------------------------------------------------
  {
    constexpr int64_t period = 11111111, display = 5000000000;
    check(kPoseFrameMaxToleranceNs == 50000000, "the cap on the tolerance is 50 ms");
    check(poseFrameTolerance(period) == period && poseFrameTolerance(1) == 1 && poseFrameTolerance(kPoseFrameMaxToleranceNs) == kPoseFrameMaxToleranceNs,
          "the tolerance is one display period, as the frame reports it");
    check(poseFrameTolerance(kPoseFrameMaxToleranceNs + 1) == kPoseFrameMaxToleranceNs && poseFrameTolerance(INT64_MAX) == kPoseFrameMaxToleranceNs,
          "...capped at 50 ms, so a period that is not a display period cannot stretch it");
    check(poseFrameTolerance(0) == 0 && poseFrameTolerance(-1) == 0 && poseFrameTolerance(INT64_MIN) == 0, "...and no period (zero, negative) is no tolerance");
    check(displayTimeFresh(display, period, display - 42000000) && displayTimeFresh(display, period, display - 1) && displayTimeFresh(display, period, display),
          "a display time ahead of now, or exactly now, is good (the real case: now is about 42 ms before it)");
    check(displayTimeFresh(display, period, display + 1) && displayTimeFresh(display, period, display + period),
          "...and stays good until it is one display period behind now, inclusive");
    check(!displayTimeFresh(display, period, display + period + 1) && !displayTimeFresh(display, period, display + 2 * period) &&
              !displayTimeFresh(display, period, display + 5000000000),
          "...one nanosecond past that, or a second later, it is not");
    check(displayTimeFresh(display, 0, display) && !displayTimeFresh(display, 0, display + 1) && !displayTimeFresh(display, -5, display + 1),
          "with no period the display time must not be behind now at all");
    check(displayTimeFresh(display, INT64_MAX, display + kPoseFrameMaxToleranceNs) && !displayTimeFresh(display, INT64_MAX, display + kPoseFrameMaxToleranceNs + 1),
          "a huge period is held to the 50 ms cap");
    check(!displayTimeFresh(0, period, 1) && !displayTimeFresh(-7, period, 1) && displayTimeFresh(1, period, 1 + period) && !displayTimeFresh(1, period, 2 + period),
          "a display time that is not positive is never judged good once now is ahead of it; the smallest positive one follows the same rule as any other");
    check(displayTimeFresh(INT64_MAX, period, INT64_MAX) && displayTimeFresh(INT64_MAX, period, INT64_MAX - 1) && !displayTimeFresh(INT64_MAX - 3 * period, period, INT64_MAX),
          "the comparison does not overflow at the ends of the range");
  }
  // ---- the instant, per advanced.cull_pose time -----------------------------------------------------------------------------------------------
  {
    using cullpose::Time;
    constexpr int64_t period = 11111111, display = 5000000000, lead = 42000000;
    const auto add = [](int64_t d, int64_t p, int64_t& out) { return nextPredictionTime(d, p, out); };
    const auto never = [](int64_t, int64_t) { return false; };
    int64_t at = -7;
    check(poseTargetTime(Time::Display, display, period, display - lead, add, never, &at) && at == display,
          "cull_pose display: the instant is the latest frame's display time (the shipped behaviour)");
    at = -7;
    check(poseTargetTime(Time::Next, display, period, display - lead, add, never, &at) && at == display + period,
          "cull_pose next: the instant is the display time plus one period of that frame");
    at = -7;
    check(!poseTargetTime(Time::Now, display, period, display - lead, add, never, &at) && at == -7,
          "cull_pose now: no explicit instant (the call is located at now + prediction, the behaviour before the fix) and the output is left alone");
    // The freshness test applies to the CACHED frame, in every mode: one period behind now is still good, a nanosecond more is not.
    at = -7;
    check(poseTargetTime(Time::Display, display, period, display + period, add, never, &at) && at == display &&
              poseTargetTime(Time::Next, display, period, display + period, add, never, &at) && at == display + period,
          "a cached frame exactly one period behind now still answers, in both modes");
    at = -7;
    check(!poseTargetTime(Time::Display, display, period, display + period + 1, add, never, &at) && !poseTargetTime(Time::Next, display, period, display + period + 1, add, never, &at) && at == -7,
          "...and one nanosecond more, neither does: the freshness test is on the cached frame, not on the target one period ahead of it");
    check(poseTargetTime(Time::Next, display, period, display + period - 1000000, add, never, &at) && at == display + period &&
              !poseTargetTime(Time::Next, display, period, display + 2 * period, add, never, &at),
          "...a stale frame falls back in next as it does in display, whatever the target would have been");
    // No frame / no period / overflow.
    check(!poseTargetTime(Time::Display, 0, period, 1, add, never, &at) && !poseTargetTime(Time::Next, -5, period, 1, add, never, &at),
          "no display time (not positive) is no instant, in both modes");
    at = -7;
    check(!poseTargetTime(Time::Next, display, 0, display, add, never, &at) && !poseTargetTime(Time::Next, display, -1, display, add, never, &at) && at == -7 &&
              poseTargetTime(Time::Display, display, 0, display, add, never, &at) && at == display,
          "cull_pose next with a frame that reports no period has no target (display, which never adds one, still answers)");
    check(!poseTargetTime(Time::Next, INT64_MAX - 5, period, INT64_MAX - 5, add, never, &at) && poseTargetTime(Time::Display, INT64_MAX - 5, period, INT64_MAX - 5, add, never, &at) && at == INT64_MAX - 5,
          "a display time so late that one more period overflows has no next target and does not wrap");
    // A reference-space change between the display time and the target: a pose there is in another origin.
    const auto crossing = [&](int64_t from, int64_t to) { return from < display + 5000000 && display + 5000000 <= to; };
    at = -7;
    check(!poseTargetTime(Time::Next, display, period, display - lead, add, crossing, &at) && at == -7 && poseTargetTime(Time::Display, display, period, display - lead, add, crossing, &at) && at == display,
          "a reference change between the display time and the target: next has no instant (display never crosses it)");
    int64_t seenFrom = 0, seenTo = 0;
    poseTargetTime(Time::Next, display, period, display - lead, add, [&](int64_t f, int64_t t) { seenFrom = f; seenTo = t; return false; }, &at);
    check(seenFrom == display && seenTo == display + period, "...asked about exactly the span from the display time to the target");
    int64_t addOrCrossCalls = 0;
    poseTargetTime(Time::Display, display, period, display - lead, [&](int64_t, int64_t, int64_t&) { ++addOrCrossCalls; return true; }, [&](int64_t, int64_t) { ++addOrCrossCalls; return false; }, &at);
    check(addOrCrossCalls == 0, "(display neither adds a period nor asks about a reference change)");
  }
  // ---- the lines -----------------------------------------------------------------------------------------------------------------------------
  {
    HeadPoseSightings s;
    std::vector<std::string> lines;
    const auto sink=[&](const char* l){lines.emplace_back(l);};
    s.note(0x4E3881,sink);
    check(lines.size()==1&&lines[0]=="head pose: Elite's \"now\" requests are answered at the drawn frame's display time (first from exe+0x4E3881)"&&s.distinct()==1,
          "the first caller answered writes the line (the exact text), with the return RVA");
    s.note(0x4E3881,sink);s.note(0x4E3881,sink);
    check(lines.size()==1,"...the same caller again writes nothing");
    s.note(0x2A03F51,sink);
    check(lines.size()==2&&lines[1]=="head pose: another Elite caller of \"now\" is answered at the drawn frame's display time (exe+0x2A03F51)"&&s.distinct()==2,
          "a further distinct return address writes one more line, named (a caller that appears after a game update)");
    for(uint32_t i=0;i<6;++i)s.note(0x100+i,sink);
    check(lines.size()==8&&s.distinct()==8,"(six more distinct callers: eight lines in all so far)");
    s.note(0x200,sink);
    check(lines.size()==9&&lines[8]=="head pose: another Elite caller of \"now\" is answered at the drawn frame's display time (exe+0x200)"&&s.distinct()==9,
          "the eighth FURTHER caller (the ninth in all) is still named");
    s.note(0x300,sink);
    check(lines.size()==10&&lines[9]=="head pose: more than 8 further callers of \"now\"; the rest are not logged"&&s.distinct()==9,
          "the next distinct caller finds the cap: one line says the rest are not logged, and it is not remembered");
    s.note(0x301,sink);s.note(0x300,sink);s.note(0x4E3881,sink);s.note(0x200,sink);
    check(lines.size()==10,"...which is said once; later callers, new or old, write nothing");
  }
  {
    // Under cull_pose next the phrase says so: the one-period lead is the thing under test.
    HeadPoseSightings s;
    std::vector<std::string> lines;
    const auto sink=[&](const char* l){lines.emplace_back(l);};
    s.note(0x4E3881,cullpose::Time::Next,sink);
    s.note(0x2A03F51,cullpose::Time::Next,sink);
    check(lines.size()==2&&lines[0]=="head pose: Elite's \"now\" requests are answered one display period after the drawn frame's display time (first from exe+0x4E3881)"&&
              lines[1]=="head pose: another Elite caller of \"now\" is answered one display period after the drawn frame's display time (exe+0x2A03F51)",
          "under cull_pose next the lines say one display period after the drawn frame's display time (the exact text)");
    HeadPoseSightings d;
    std::vector<std::string> displayLines;
    d.note(0x4E3881,cullpose::Time::Display,[&](const char* l){displayLines.emplace_back(l);});
    check(displayLines.size()==1&&displayLines[0]=="head pose: Elite's \"now\" requests are answered at the drawn frame's display time (first from exe+0x4E3881)",
          "...and under display they are the shipped text");
  }
}
}  // namespace head_pose_time_cases

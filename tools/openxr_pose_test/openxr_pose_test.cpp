#include "../../src/openxr/space_pose.h"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>

using namespace edvr::openxr;
namespace {
int checks = 0, failures = 0, calls = 0;
XrTime seenTime = 0;
XrSpace seenView{}, seenOrigin{};
enum FakeMode { Good, Pending, Lost, BadLocType, BadLocNext, BadVelType, BadVelNext };
FakeMode mode = Good;
constexpr auto locationValid = XR_SPACE_LOCATION_POSITION_VALID_BIT |
                               XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
constexpr auto linearValid = XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
constexpr auto angularValid = XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;

void check(bool value, const char* name) {
  ++checks;
  if (!value) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}
bool same(float a, float b) { return std::fabs(a - b) < 1e-5f; }
bool unchanged(const TimedHeadPose& a, const TimedHeadPose& b) {
  return std::memcmp(&a, &b, sizeof a) == 0;
}
void vectorEquals(const vr::HmdVector3_t& actual, const XrVector3f& expected) {
  check(same(actual.v[0], expected.x), "vector x");
  check(same(actual.v[1], expected.y), "vector y");
  check(same(actual.v[2], expected.z), "vector z");
}
void invalidEquals(const TimedHeadPose& actual, bool connected) {
  check(!actual.pose.bPoseIsValid && actual.pose.bDeviceIsConnected == connected,
        "invalid pose connection");
  check(actual.pose.eTrackingResult == (connected ? vr::TrackingResult_Running_OutOfRange
                                                : vr::TrackingResult_Uninitialized),
        "invalid tracking result");
  for (int row = 0; row < 3; ++row)
    for (int column = 0; column < 4; ++column)
      check(actual.pose.mDeviceToAbsoluteTracking.m[row][column] == (row == column ? 1.f : 0.f),
            "invalid identity matrix");
  vectorEquals(actual.pose.vVelocity, {});
  vectorEquals(actual.pose.vAngularVelocity, {});
  check(!actual.linearVelocityValid && !actual.angularVelocityValid, "invalid velocity flags");
}
XrPosef pose(float x = 0, float y = 0, float z = 0, float w = 1) {
  XrPosef p{}; p.orientation = {x, y, z, w}; p.position = {1, 2, 3}; return p;
}
XrResult XRAPI_PTR fake(XrSpace view, XrSpace origin, XrTime time, XrSpaceLocation* location) {
  ++calls; seenView = view; seenOrigin = origin; seenTime = time;
  check(location && location->type == XR_TYPE_SPACE_LOCATION && location->next,
        "typed location input chain");
  if (!location || !location->next) return XR_ERROR_VALIDATION_FAILURE;
  auto* velocity = static_cast<XrSpaceVelocity*>(location->next);
  check(velocity->type == XR_TYPE_SPACE_VELOCITY && velocity->next == nullptr,
        "typed velocity input chain");
  location->locationFlags = locationValid;
  location->pose = pose();
  velocity->velocityFlags = linearValid | angularValid;
  velocity->linearVelocity = {4, 5, 6}; velocity->angularVelocity = {7, 8, 9};
  if (mode == Pending) return XR_SESSION_LOSS_PENDING;
  if (mode == Lost) return XR_ERROR_SESSION_LOST;
  if (mode == BadLocType) location->type = XR_TYPE_SPACE_VELOCITY;
  if (mode == BadLocNext) location->next = nullptr;
  if (mode == BadVelType) velocity->type = XR_TYPE_SPACE_LOCATION;
  if (mode == BadVelNext) velocity->next = location;
  return XR_SUCCESS;
}
void conversionTests() {
  TimedHeadPose output{}, before{};
  const float s = static_cast<float>(std::sqrt(.5));
  check(makeHeadPose(pose(0, s, 0, s), locationValid, linearValid | angularValid,
                     {4, 5, 6}, {7, 8, 9}, true, output), "yaw conversion");
  const float expected[3][4] = {{0, 0, 1, 1}, {0, 1, 0, 2}, {-1, 0, 0, 3}};
  for (int row = 0; row < 3; ++row)
    for (int column = 0; column < 4; ++column)
      check(same(output.pose.mDeviceToAbsoluteTracking.m[row][column], expected[row][column]),
            "yaw matrix element");
  check(output.pose.bPoseIsValid && output.pose.bDeviceIsConnected &&
        output.pose.eTrackingResult == vr::TrackingResult_Running_OK, "valid tracking without tracked flags");
  check(output.linearVelocityValid && output.angularVelocityValid, "both velocity flags");
  vectorEquals(output.pose.vVelocity, {4, 5, 6});
  vectorEquals(output.pose.vAngularVelocity, {7, 8, 9});

  check(makeHeadPose(pose(), locationValid, linearValid, {4, 5, 6}, {NAN, NAN, NAN}, true, output),
        "unavailable angular data ignored");
  check(output.linearVelocityValid && !output.angularVelocityValid, "linear only");
  vectorEquals(output.pose.vVelocity, {4, 5, 6}); vectorEquals(output.pose.vAngularVelocity, {});
  check(makeHeadPose(pose(), locationValid, angularValid, {NAN, NAN, NAN}, {7, 8, 9}, true, output),
        "unavailable linear data ignored");
  check(!output.linearVelocityValid && output.angularVelocityValid, "angular only");
  vectorEquals(output.pose.vVelocity, {}); vectorEquals(output.pose.vAngularVelocity, {7, 8, 9});
  check(makeHeadPose(pose(), locationValid, 0, {NAN, NAN, NAN}, {NAN, NAN, NAN}, true, output),
        "unavailable velocity data ignored");
  vectorEquals(output.pose.vVelocity, {}); vectorEquals(output.pose.vAngularVelocity, {});

  XrPosef bad = pose(); bad.position.x = NAN; bad.orientation.w = NAN;
  for (auto flags : {XrSpaceLocationFlags(0), XrSpaceLocationFlags(XR_SPACE_LOCATION_POSITION_VALID_BIT),
                     XrSpaceLocationFlags(XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)}) {
    check(makeHeadPose(bad, flags, linearValid | angularValid, {NAN, 0, 0}, {NAN, 0, 0}, true, output),
          "unavailable location ignored");
    invalidEquals(output, true);
  }
  check(makeHeadPose(bad, locationValid, linearValid | angularValid, {NAN, 0, 0}, {NAN, 0, 0},
                     false, output), "disconnected data ignored");
  invalidEquals(output, false);

  check(makeHeadPose(pose(), locationValid, 0, {}, {}, true, output), "restore valid output");
  before = output;
  check(!makeHeadPose(pose(), locationValid, linearValid, {NAN, 0, 0}, {}, true, output) &&
        unchanged(output, before), "valid linear NaN rejected atomically");
  check(!makeHeadPose(pose(), locationValid, angularValid, {}, {NAN, 0, 0}, true, output) &&
        unchanged(output, before), "valid angular NaN rejected atomically");
  check(!makeHeadPose(pose(0, 0, 0, 2), locationValid, 0, {}, {}, true, output) &&
        unchanged(output, before), "nonunit quaternion rejected atomically");
  bad = pose(); bad.position.y = INFINITY;
  check(!makeHeadPose(bad, locationValid, 0, {}, {}, true, output) && unchanged(output, before),
        "infinite position rejected atomically");
}
void locateTests() {
  const XrSpace view = reinterpret_cast<XrSpace>(1), origin = reinterpret_cast<XrSpace>(2);
  TimedHeadPose output{};
  calls = 0; mode = Good;
  check(locateHeadAt(fake, view, origin, 77, true, output) == XR_SUCCESS &&
        seenTime == 77 && seenView == view && seenOrigin == origin && output.time == 77,
        "exact locate inputs and timestamp");
  vectorEquals(output.pose.vVelocity, {4, 5, 6}); vectorEquals(output.pose.vAngularVelocity, {7, 8, 9});
  const TimedHeadPose before = output;
  mode = Pending;
  check(locateHeadAt(fake, view, origin, 77, true, output) == XR_SESSION_LOSS_PENDING &&
        unchanged(output, before), "pending output unchanged");
  mode = Lost;
  check(locateHeadAt(fake, view, origin, 77, true, output) == XR_ERROR_SESSION_LOST &&
        unchanged(output, before), "lost output unchanged");
  for (int value = BadLocType; value <= BadVelNext; ++value) {
    mode = static_cast<FakeMode>(value);
    check(locateHeadAt(fake, view, origin, 77, true, output) == XR_ERROR_VALIDATION_FAILURE &&
          unchanged(output, before), "malformed output chain rejected atomically");
  }
  mode = Good;
  check(locateHeadAt(nullptr, view, origin, 1, true, output) == XR_ERROR_FUNCTION_UNSUPPORTED &&
        unchanged(output, before), "missing locate function");
  check(locateHeadAt(fake, nullptr, origin, 1, true, output) == XR_ERROR_HANDLE_INVALID &&
        calls == 7 && unchanged(output, before), "null view no dispatch");
  check(locateHeadAt(fake, view, nullptr, 1, true, output) == XR_ERROR_HANDLE_INVALID &&
        calls == 7 && unchanged(output, before), "null origin no dispatch");
}
void timeTests() {
  XrTime next = 0;
  check(nextPredictionTime(200, 11, next) && next == 211, "runtime period used exactly");
  check(nextPredictionTime((std::numeric_limits<XrTime>::max)() - 1, 1, next) &&
        next == (std::numeric_limits<XrTime>::max)(), "maximum boundary");
  next = 9;
  check(!nextPredictionTime((std::numeric_limits<XrTime>::max)(), 1, next) && next == 9,
        "overflow unchanged");
  check(nextPredictionTime((std::numeric_limits<XrTime>::min)(), 1, next) &&
        next == (std::numeric_limits<XrTime>::min)() + 1, "minimum boundary");
  next = 9;
  check(!nextPredictionTime(10, 0, next) && next == 9, "zero period unchanged");
  check(!nextPredictionTime(10, -1, next) && next == 9, "negative period unchanged");
}
} // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--dry-run") {
    std::cout << "openxr_pose_test: dry-run (no runtime or file writes)\n"; return 0;
  }
  if (argc != 2 || std::string(argv[1]) != "--self-test") return 2;
  conversionTests(); locateTests(); timeTests();
  std::cout << "openxr_pose_test: " << checks << " checks, " << failures << " failures\n";
  return failures ? 1 : 0;
}

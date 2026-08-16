// pose_test -- the head-pose offset arithmetic, without a headset.
//
// Six lines of matrix work that fail quietly. A transposed index garbles the
// view; a flipped sine turns the wrong way. The second is INVISIBLE at exactly
// 180 degrees, because sin(pi) is zero -- and 180 is the only angle the feature
// was written for, so the one case anybody would fly is the one case that
// cannot catch the mistake. Hence the 90-degree assertions below.
//
// Convention under test: OpenVR tracking space is +x right, +y up, -z forward,
// and HmdMatrix34_t is row-major 3x4 with the position in column 3.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../../src/openvr/pose_offset.h"

namespace {

int g_fails = 0;

void ok(const char* what) { printf("  ok    %s\n", what); }

void fail(const char* what, const char* detail) {
    printf("  FAIL  %s -- %s\n", what, detail);
    ++g_fails;
}

bool near(float a, float b) { return fabsf(a - b) < 1e-4f; }

void identity(float m[3][4]) {
    memset(m, 0, sizeof(float) * 12);
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
}

// Where the headset is pointing, in tracking space: the pose's rotation applied
// to the headset's own forward, which is -z.
void forward(const float m[3][4], float out[3]) {
    for (unsigned i = 0; i < 3; ++i) out[i] = -m[i][2];
}

void checkVec3(const char* what, float gx, float gy, float gz,
               float x, float y, float z) {
    const float got[3] = {gx, gy, gz};
    if (near(got[0], x) && near(got[1], y) && near(got[2], z)) {
        ok(what);
        return;
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "got (%+.3f %+.3f %+.3f), wanted (%+.3f %+.3f %+.3f)",
             got[0], got[1], got[2], x, y, z);
    fail(what, buf);
}

void yaw(float deg, float* s, float* c) {
    const float r = deg * 3.14159265358979f / 180.0f;
    *s = sinf(r);
    *c = cosf(r);
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const float zero[3] = {0.0f, 0.0f, 0.0f};
    float m[3][4], f[3], s, c;

    // --- translation ------------------------------------------------------
    identity(m);
    const float off[3] = {0.1f, 1.0f, -2.0f};
    edvr::applyPoseOffset(m, off, 0.0f, 1.0f);
    checkVec3("translation lands in column 3", m[0][3], m[1][3], m[2][3],
              0.1f, 1.0f, -2.0f);
    forward(m, f);
    checkVec3("...and does not touch the rotation", f[0], f[1], f[2], 0.0f, 0.0f, -1.0f);

    // --- the half turn the feature exists for ------------------------------
    identity(m);
    yaw(180.0f, &s, &c);
    edvr::applyPoseOffset(m, zero, s, c);
    forward(m, f);
    checkVec3("180 turns forward to backward", f[0], f[1], f[2], 0.0f, 0.0f, 1.0f);

    // --- the angles that can catch a flipped sine -------------------------
    //
    // A positive yaw about +y turns -z toward... this is the assertion that
    // pins the handedness, and it is the whole reason the test exists: at 180
    // both sign conventions agree and neither can be wrong.
    identity(m);
    yaw(90.0f, &s, &c);
    edvr::applyPoseOffset(m, zero, s, c);
    forward(m, f);
    checkVec3("+90 sends forward to -x", f[0], f[1], f[2], -1.0f, 0.0f, 0.0f);

    identity(m);
    yaw(-90.0f, &s, &c);
    edvr::applyPoseOffset(m, zero, s, c);
    forward(m, f);
    checkVec3("-90 sends forward to +x, the other way", f[0], f[1], f[2], 1.0f, 0.0f, 0.0f);

    // --- the yaw must not move the player ---------------------------------
    //
    // Rotating column 3 as well would swing them around the tracking origin
    // instead of turning them on the spot, silently discarding a tuned
    // placement. Position is set FIRST here so the turn has something to move.
    identity(m);
    m[0][3] = 3.0f; m[1][3] = 1.0f; m[2][3] = -2.0f;
    yaw(180.0f, &s, &c);
    edvr::applyPoseOffset(m, zero, s, c);
    checkVec3("a turn leaves the position alone", m[0][3], m[1][3], m[2][3],
              3.0f, 1.0f, -2.0f);

    // --- up must survive any yaw ------------------------------------------
    //
    // The turn is about the TRACKING frame's up axis, so up is its fixed axis.
    // If this moves, the rotation was applied in the head's frame instead and
    // the horizon will tilt when the player looks up or down.
    identity(m);
    yaw(37.0f, &s, &c);
    edvr::applyPoseOffset(m, zero, s, c);
    checkVec3("up is the axis, so it is unchanged", m[0][1], m[1][1], m[2][1],
              0.0f, 1.0f, 0.0f);

    // --- a tilted head still turns about world up -------------------------
    //
    // Pitch the pose down 45 degrees, then yaw 180. The result must still be
    // level-with-the-world backwards, not rolled. Post-multiplying instead of
    // pre-multiplying passes every test above and fails this one.
    identity(m);
    {
        const float p = 45.0f * 3.14159265358979f / 180.0f;
        const float cp = cosf(p), sp = sinf(p);
        // Rx(-45): pitch the headset down.
        m[1][1] = cp;  m[1][2] = sp;
        m[2][1] = -sp; m[2][2] = cp;
    }
    yaw(180.0f, &s, &c);
    edvr::applyPoseOffset(m, zero, s, c);
    // The head's up was (0, +0.707, -0.707) after that pitch. A half turn about
    // WORLD up negates x and z and leaves y, giving (0, +0.707, +0.707).
    //
    // Post-multiplying instead would turn about the HEAD's own up, which leaves
    // that column untouched at (0, +0.707, -0.707) -- so this one assertion is
    // the entire discriminator between the two conventions, and every other
    // test in this file passes under both. It is also the assertion that caught
    // the author asserting the wrong one of them.
    checkVec3("a pitched head yaws about world up, not its own",
              m[0][1], m[1][1], m[2][1], 0.0f, cosf(0.785398f), sinf(0.785398f));

    if (g_fails) {
        printf("POSE TEST FAILED (%d)\n", g_fails);
        return 1;
    }
    printf("POSE TEST PASSED\n");
    return 0;
}

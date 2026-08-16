// Move and turn an HMD pose, in the tracking frame.
//
// Extracted from compositor_hook.cpp so it can be tested without a headset. The
// arithmetic is six lines and it is the kind that fails quietly: a transposed
// index garbles the view, and a flipped sine turns the wrong way -- which is
// INVISIBLE at exactly 180 degrees, because sin(pi) is zero. The one angle this
// was written for is the one angle that cannot detect the mistake, so it gets
// asserted at other angles instead.
#pragma once

namespace edvr {

// `m` is HmdMatrix34_t's row-major 3x4: the left 3x3 is the rotation and column
// 3 is the position. Translation in the tracking frame is therefore an add to
// column 3, which is why this touches the rotation not at all.
//
// The yaw is PRE-multiplied (Ry * R), about the tracking frame's up axis rather
// than the headset's own. Post-multiplying would put the turn in the head's
// frame, so its axis would tip as the player looked up or down -- the same
// distinction EVIDENCE 6x.1 refuted for translation.
//
// Columns 0..2 only. Rotating column 3 as well would swing the player around
// the tracking origin instead of turning them on the spot.
inline void applyPoseOffset(float m[3][4], const float offset[3],
                            float yawSin, float yawCos) {
    if (yawSin != 0.0f || yawCos != 1.0f) {
        for (unsigned j = 0; j < 3; ++j) {
            const float x = m[0][j], z = m[2][j];
            m[0][j] = yawCos * x + yawSin * z;
            m[2][j] = -yawSin * x + yawCos * z;
        }
    }
    for (unsigned a = 0; a < 3; ++a) m[a][3] += offset[a];
}

}  // namespace edvr

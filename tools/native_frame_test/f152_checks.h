// F152 checks (native on-foot VR), run152 below. Header only, run by native_frame_test.
//
// F152 (native_on_foot_f152.h): the pure parts of two rules. The coat's Gear_Back is promoted only while the head's yaw against the body is under a threshold, with a hysteresis (stop
// at |phi| >= 45 deg, resume below 38) and a stale sample (none within 100 ms) keeping the last decision; phi is the body pin's eye-minus-neck vector's atan2(right, forward). The
// readout's origin rows are the engine shadow's when it has them, else native on foot's own, and where both have rows they are compared.
#pragma once

#include "../../src/d3d11/native_on_foot_f152.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace f152test {

namespace f152 = edvr::nofoot::f152;

inline bool near152(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

template <class Check>
inline void run152(Check&& check) {
    // ---- 1. phi from the eye-minus-neck vector ------------------------------------------------------------------------------------------------------------------------------
    {
        double phi = 99.0;
        check(f152::headYawDeg(0.0, 0.09, &phi) && near152(phi, 0.0), "F152: the head square to the body (eye 9 cm forward of the neck, 0 to the side): phi = 0");
        check(f152::headYawDeg(0.09, 0.0, &phi) && near152(phi, 90.0), "F152: ... 9 cm to the right and none forward: +90 deg");
        check(f152::headYawDeg(-0.09, 0.0, &phi) && near152(phi, -90.0), "F152: ... to the left: -90 deg");
        check(f152::headYawDeg(0.0, -0.09, &phi) && near152(std::fabs(phi), 180.0), "F152: ... behind the neck: 180 deg");
        const double s = std::sin(55.0 * 3.14159265358979323846 / 180.0), c = std::cos(55.0 * 3.14159265358979323846 / 180.0);
        check(f152::headYawDeg(0.10 * s, 0.10 * c, &phi) && near152(phi, 55.0, 1e-9), "F152: a vector 55 deg to the right of forward reads 55 deg (the flight's 55 to 62)");
        phi = 77.0;
        check(!f152::headYawDeg(0.01, 0.01, &phi) && phi == 77.0, "F152: a horizontal part under 3 cm points nowhere: no sample, phi untouched");
        check(!f152::headYawDeg(std::numeric_limits<double>::quiet_NaN(), 0.09, &phi) && !f152::headYawDeg(0.09, std::numeric_limits<double>::infinity(), &phi) && phi == 77.0,
              "F152: a part that is not finite gives no sample");
    }
    // ---- 2. the decision with its hysteresis --------------------------------------------------------------------------------------------------------------------------------
    {
        bool d = true;
        d = f152::turnPromote(d, 0.0);
        check(d, "F152: 0 deg: promote");
        d = f152::turnPromote(d, 50.0);
        check(!d, "F152: 50 deg: not");
        d = f152::turnPromote(d, 40.0);
        check(!d, "F152: 40 deg: still not (the hysteresis: resume below 38)");
        d = f152::turnPromote(d, 37.0);
        check(d, "F152: 37 deg: promote");
        d = f152::turnPromote(d, 44.9);
        check(d, "F152: 44.9 deg after resuming: still promoted (stop at 45)");
        d = f152::turnPromote(d, 45.0);
        check(!d, "F152: 45 deg: stops");
        d = f152::turnPromote(d, 38.0);
        check(!d, "F152: 38 deg: does not resume (below 38)");
        d = f152::turnPromote(d, 37.999);
        check(d, "F152: 37.999 deg: resumes");
        d = f152::turnPromote(d, -60.0);
        check(!d, "F152: -60 deg: stops (the turn to the left counts the same)");
        d = f152::turnPromote(d, -39.0);
        check(!d, "F152: -39 deg: still not");
        d = f152::turnPromote(d, -20.0);
        check(d, "F152: -20 deg: resumes");
        d = f152::turnPromote(d, std::numeric_limits<double>::quiet_NaN());
        check(d, "F152: a yaw that is not a number keeps the decision (promoting)");
        d = f152::turnPromote(false, std::numeric_limits<double>::quiet_NaN());
        check(!d, "F152: ... and not promoting");
        // every step of a sweep from 0 to 90 and back: stops exactly at 45 on the way out, resumes exactly under 38 on the way back, and between the two thresholds both states hold
        bool state = true;
        int stopAt = -1, resumeAt = -1;
        for (int deg = 0; deg <= 90; ++deg) {
            const bool next = f152::turnPromote(state, static_cast<double>(deg));
            if (state && !next) stopAt = deg;
            state = next;
        }
        for (int deg = 90; deg >= 0; --deg) {
            const bool next = f152::turnPromote(state, static_cast<double>(deg));
            if (!state && next) resumeAt = deg;
            state = next;
        }
        check(stopAt == 45 && resumeAt == 37 && state, "F152: a sweep 0 -> 90 -> 0 deg stops at 45 on the way out and resumes at 37 on the way back (7 deg of hysteresis)");
    }
    // ---- 3. the sample word and the stale rule -------------------------------------------------------------------------------------------------------------------------------
    {
        float phi = 0.0f;
        uint32_t tick = 0;
        f152::unpackSample(f152::packSample(-52.25f, 123456789u), &phi, &tick);
        check(phi == -52.25f && tick == 123456789u, "F152: the sample word holds the yaw and the tick, exactly");
        check(f152::packSample(0.0f, 0u) == 0u && f152::packSample(0.0f, 1u) != 0u, "F152: 0 is none yet; a zero yaw at any other tick is a sample");
        check(f152::sampleFresh(1000u, 1000u) && f152::sampleFresh(1000u, 1100u) && !f152::sampleFresh(1000u, 1101u) && !f152::sampleFresh(1000u, 5000u),
              "F152: a sample is fresh for 100 ms");
        check(f152::sampleFresh(0xFFFFFFF0u, 0x00000010u) && !f152::sampleFresh(0xFFFFFFF0u, 0x00000100u), "F152: ... across the 32-bit tick's wrap");
        // the decision from the word: 0 -> promote, 50 -> not, 40 -> still not, 37 -> promote; a stale sample keeps the decision
        bool d = true;
        f152::TurnDecision t = f152::turnDecide(d, f152::packSample(0.0f, 1000u), 1010u);
        check(t.promote && t.fresh && t.phiDeg == 0.0f, "F152: the decision for a fresh 0 deg sample: promote");
        d = t.promote;
        t = f152::turnDecide(d, f152::packSample(50.0f, 1100u), 1110u);
        check(!t.promote && t.fresh && t.phiDeg == 50.0f, "F152: a fresh 50 deg sample: not promoting");
        d = t.promote;
        t = f152::turnDecide(d, f152::packSample(40.0f, 1200u), 1210u);
        check(!t.promote && t.fresh, "F152: a fresh 40 deg sample after it: still not promoting");
        d = t.promote;
        t = f152::turnDecide(d, f152::packSample(37.0f, 1300u), 1310u);
        check(t.promote && t.fresh, "F152: a fresh 37 deg sample after it: promoting");
        d = t.promote;
        t = f152::turnDecide(d, f152::packSample(60.0f, 1400u), 1410u);
        d = t.promote;
        check(!d, "F152: a fresh 60 deg sample: not promoting");
        t = f152::turnDecide(d, f152::packSample(10.0f, 1500u), 1700u);
        check(!t.promote && !t.fresh, "F152: a 10 deg sample 200 ms old is stale: the last decision (not promoting) stands");
        t = f152::turnDecide(true, f152::packSample(80.0f, 1500u), 1700u);
        check(t.promote && !t.fresh, "F152: ... and a stale 80 deg sample does not stop a promotion either");
        t = f152::turnDecide(false, 0u, 2000u);
        check(!t.promote && !t.fresh, "F152: no sample yet keeps the decision (promoting at the start)");
        t = f152::turnDecide(true, f152::packSample(std::numeric_limits<float>::quiet_NaN(), 1500u), 1510u);
        check(t.promote && !t.fresh, "F152: a sample word whose yaw is not a number keeps the decision");
    }
    // ---- 4. the origin rows ------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        float eng[6][4] = {}, own[6][4] = {}, out[6][4] = {};
        for (int r = 0; r < 6; ++r)
            for (int c = 0; c < 4; ++c) {
                eng[r][c] = 1.0f + static_cast<float>(r) * 4.0f + static_cast<float>(c);
                own[r][c] = eng[r][c];
            }
        bool compared = true;
        double diff = 9.0;
        int src = f152::chooseOrigin(false, eng, false, own, out, &compared, &diff);
        check(src == f152::kOriginNone && !compared && diff == 0.0, "F152: no rows from either shadow: the origin is none");
        src = f152::chooseOrigin(true, eng, false, own, out, &compared, &diff);
        check(src == f152::kOriginEngine && !compared && std::memcmp(out, eng, sizeof(out)) == 0, "F152: the engine's rows only: they are taken, nothing compared");
        std::memset(out, 0, sizeof(out));
        src = f152::chooseOrigin(false, eng, true, own, out, &compared, &diff);
        check(src == f152::kOriginOwn && !compared && std::memcmp(out, own, sizeof(out)) == 0, "F152: engine motion down, the own shadow's rows: they are taken (the AA off case)");
        std::memset(out, 0, sizeof(out));
        src = f152::chooseOrigin(true, eng, true, own, out, &compared, &diff);
        check(src == f152::kOriginEngine && compared && diff == 0.0 && std::memcmp(out, eng, sizeof(out)) == 0, "F152: both have rows: the engine's first, compared, equal");
        own[5][2] += 0.25f;
        src = f152::chooseOrigin(true, eng, true, own, out, &compared, &diff);
        check(src == f152::kOriginEngine && compared && near152(diff, 0.25, 1e-6) && std::memcmp(out, eng, sizeof(out)) == 0,
              "F152: ... a difference of 0.25 in one lane shows as the max difference, and the engine's rows are still the ones taken");
        own[5][2] = std::numeric_limits<float>::quiet_NaN();
        src = f152::chooseOrigin(true, eng, true, own, out, &compared, &diff);
        check(compared && std::isinf(diff), "F152: ... a lane that is not a number is an infinite difference");
        check(std::strcmp(f152::originText(f152::kOriginEngine), "the engine shadow") == 0 && std::strcmp(f152::originText(f152::kOriginOwn), "the own shadow") == 0 &&
                  std::strcmp(f152::originText(f152::kOriginNone), "none") == 0 && f152::kOriginCount == 3,
              "F152: the sources are named: the engine shadow, the own shadow, none");
        // the rows are 96 bytes at byte 4320 of the scene constants
        check(f152::kRowsOffset == 4320u && f152::kRowsBytes == 96u && f152::kRowsFirst == 270u && f152::kRowCount == 6u, "F152: the rows are 270..275, 96 bytes at byte 4320");
    }
    // ---- 5. a ranged UpdateSubresource into the rows ------------------------------------------------------------------------------------------------------------------------------
    {
        float rows[6][4] = {};
        uint8_t buf[512];
        for (size_t i = 0; i < sizeof(buf); ++i) buf[i] = static_cast<uint8_t>(i & 0xFF);
        check(!f152::overlayRange(rows, buf, 0, 100) && !f152::overlayRange(rows, buf, 5000, 5100) && !f152::overlayRange(rows, buf, 4400 + 16, 4400) && !f152::overlayRange(rows, nullptr, 4320, 4336),
              "F152: a range outside rows 270..275, an empty one and no data change nothing");
        std::memset(rows, 0, sizeof(rows));
        check(f152::overlayRange(rows, buf, 4320, 4336) && reinterpret_cast<const uint8_t*>(rows)[0] == 0 && reinterpret_cast<const uint8_t*>(rows)[15] == 15 &&
                  reinterpret_cast<const uint8_t*>(rows)[16] == 0,
              "F152: a range that is exactly row 270 lands in row 270 and no other");
        std::memset(rows, 0, sizeof(rows));
        check(f152::overlayRange(rows, buf, 4300, 4340) && reinterpret_cast<const uint8_t*>(rows)[0] == 20 && reinterpret_cast<const uint8_t*>(rows)[19] == 39 &&
                  reinterpret_cast<const uint8_t*>(rows)[20] == 0,
              "F152: a range that starts before the rows lands from its 21st byte: rows 270's first 20 bytes only");
        std::memset(rows, 0, sizeof(rows));
        check(f152::overlayRange(rows, buf, 4400, 4500) && reinterpret_cast<const uint8_t*>(rows)[80] == 0 && reinterpret_cast<const uint8_t*>(rows)[95] == 15 &&
                  reinterpret_cast<const uint8_t*>(rows)[79] == 0,
              "F152: a range that runs past the rows lands up to their end only (rows 275's last 16 bytes: bytes 4400..4415)");
    }
}

}  // namespace f152test

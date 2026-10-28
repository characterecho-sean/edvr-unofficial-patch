// F152 checks (native on-foot VR), run152 below. Header only, run by native_frame_test.
//
// F152 (native_on_foot_f152.h): the readout's origin rows are the engine shadow's when it has them, else native on foot's own, and where both have rows they are compared; a ranged
// UpdateSubresource lands in rows 270..275 in place. (F152's head-turn gate on the coat's Gear_Back was removed in F157; its checks went with it.)
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
    // ---- 1. the origin rows ------------------------------------------------------------------------------------------------------------------------------------------------------
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
    // ---- 2. a ranged UpdateSubresource into the rows ------------------------------------------------------------------------------------------------------------------------------
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

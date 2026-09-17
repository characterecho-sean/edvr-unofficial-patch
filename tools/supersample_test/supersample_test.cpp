// supersample_test -- table tests for the eye-region rule
// (src/common/supersample_math.h's supersampleRegionFromBounds), without a
// headset, a game, a device, or either DLL: everything under test is
// header-only, so this links nothing from src/ at all.
//
// What the field cannot check cheaply and a build can: the eye REGION the
// Submit bounds name inside a texture -- the double-wide texture Elite
// submits (each eye named by bounds) and the flipped bounds OpenVR
// permits -- which is where the other eye's half is kept out of every
// door pass's reach.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../../src/common/supersample_math.h"

namespace {

int g_fails = 0;

void ok(const char* what) { printf("  ok    %s\n", what); }

void check(bool got, bool want, const char* what) {
    if (got == want) {
        ok(what);
        return;
    }
    printf("  FAIL  %s -- got %s, wanted %s\n", what, got ? "true" : "false",
           want ? "true" : "false");
    ++g_fails;
}

void checkEq(uint32_t got, uint32_t want, const char* what) {
    if (got == want) return;
    printf("  FAIL  %s -- got %u, wanted %u\n", what, got, want);
    ++g_fails;
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("edvr supersample test\n\n");

    // ---- supersampleRegionFromBounds: the double-wide texture Elite
    // submits (2912x1560, measured 2026-08-17), and flipped bounds. -------
    {
        using edvr::supersampleRegionFromBounds;
        uint32_t r[4] = {};
        bool fu = true, fv = true;
        check(supersampleRegionFromBounds(2912, 1560, nullptr, r, &fu, &fv), true,
              "region: null bounds name the whole texture");
        checkEq(r[0], 0, "region null x0"); checkEq(r[1], 0, "region null y0");
        checkEq(r[2], 2912, "region null x1"); checkEq(r[3], 1560, "region null y1");
        check(!fu && !fv, true, "region: null bounds run forwards");

        const float left[4] = {0.0f, 0.0f, 0.5f, 1.0f};
        check(supersampleRegionFromBounds(2912, 1560, left, r, &fu, &fv), true,
              "region: the left eye of a double-wide texture");
        checkEq(r[0], 0, "left x0"); checkEq(r[2], 1456, "left x1");
        checkEq(r[1], 0, "left y0"); checkEq(r[3], 1560, "left y1");

        const float right[4] = {0.5f, 0.0f, 1.0f, 1.0f};
        check(supersampleRegionFromBounds(2912, 1560, right, r, &fu, &fv), true,
              "region: the right eye of a double-wide texture");
        checkEq(r[0], 1456, "right x0"); checkEq(r[2], 2912, "right x1");
        check(!fu && !fv, true, "region: the right eye runs forwards");

        // Flipped v: the ordinary way to say the origin is at the bottom.
        // Same pixels as the left eye, direction remembered.
        const float flippedV[4] = {0.0f, 1.0f, 0.5f, 0.0f};
        check(supersampleRegionFromBounds(2912, 1560, flippedV, r, &fu, &fv), true,
              "region: flipped v names the same pixels");
        checkEq(r[0], 0, "flipped v x0"); checkEq(r[2], 1456, "flipped v x1");
        checkEq(r[1], 0, "flipped v y0"); checkEq(r[3], 1560, "flipped v y1");
        check(fv && !fu, true, "region: flipped v is remembered, u is not");

        // Backwards u on the right eye: u 1.0 -> 0.5.
        const float backU[4] = {1.0f, 0.0f, 0.5f, 1.0f};
        check(supersampleRegionFromBounds(2912, 1560, backU, r, &fu, &fv), true,
              "region: backwards u names the right eye's pixels");
        checkEq(r[0], 1456, "backwards u x0"); checkEq(r[2], 2912, "backwards u x1");
        check(fu && !fv, true, "region: backwards u is remembered, v is not");

        // One texture per eye (the Pimax), null bounds, plainly.
        check(supersampleRegionFromBounds(5424, 5356, nullptr, r, &fu, &fv), true,
              "region: a per-eye texture with null bounds");
        checkEq(r[2], 5424, "per-eye x1"); checkEq(r[3], 5356, "per-eye y1");

        const float tiny[4] = {0.0f, 0.0f, 0.001f, 1.0f};
        check(supersampleRegionFromBounds(2912, 1560, tiny, r, &fu, &fv), false,
              "region: a sliver is not an eye image");
        const float nan[4] = {0.0f, 0.0f, NAN, 1.0f};
        check(supersampleRegionFromBounds(2912, 1560, nan, r, &fu, &fv), false,
              "region: NaN bounds are refused");
    }

    if (g_fails) {
        printf("\nSUPERSAMPLE TEST FAILED (%d)\n", g_fails);
        return 1;
    }
    printf("\nSUPERSAMPLE TEST PASSED\n");
    return 0;
}

// Build gate for fix.hud_quality's pure arithmetic (src/d3d11/hud_quality_math.*):
// the key's parsing (off | 1.0 | 1.25, anything else refused as off); the
// factor target / HMD-quality-multiplier, including the 0.7 -> ~1.4286
// case the log line itself quotes, the 1.0-at-1.0 no-op, 1.25-at-1.0 ->
// 1.25, the 4x cap, and an unknown (<= 0) multiplier refusing rather than
// dividing by it; the rounding a fractional factor applies to a texture's
// width/height (and, cast back at the call site, a scissor rect's corner --
// the same rule, proven once here rather than trusted at the call site);
// and the match against a set of sizes fss_res's own classifier (borrowed
// from ui_depth.cpp) will have learned, including no match, an exact match
// among several, and a near-miss that must NOT match.
//
// The implementation under test is compiled INTO this TU (the
// static_prop_gate_test / lod_governor_test pattern) so a mismatch between
// this rig and the real module is impossible by construction. It has no
// Config, no Log, no D3D11 and no device: hud_quality_math.cpp was written
// apart from fss_res.cpp's Config/Log/device_hook/ui_depth wiring exactly so
// this rig would not need to fake any of that. No hooks, no game.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/d3d11/hud_quality_math.cpp"

namespace {

int g_fails = 0;

void ok(const char* what) { std::printf("  ok    %s\n", what); }

void fail(const char* what, const std::string& detail) {
    std::printf("  FAIL  %s -- %s\n", what, detail.c_str());
    ++g_fails;
}

bool near(float a, float b, float eps = 0.001f) {
    const float d = a > b ? a - b : b - a;
    return d <= eps;
}

void expectParse(const char* text, float wantTarget, bool wantRecognized,
                 const char* what) {
    bool recognized = false;
    const float got = edvr::hudQualityParseTarget(text, &recognized);
    if (near(got, wantTarget) && recognized == wantRecognized) {
        ok(what);
        return;
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "parsed %.4f/%s, wanted %.4f/%s", static_cast<double>(got),
                  recognized ? "recognized" : "unrecognized",
                  static_cast<double>(wantTarget),
                  wantRecognized ? "recognized" : "unrecognized");
    fail(what, buf);
}

void expectFactorOff(float target, float mult, const char* what) {
    float factor = -1.0f;
    if (!edvr::hudQualityFactor(target, mult, &factor)) {
        ok(what);
        return;
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "returned true with factor %.4f",
                  static_cast<double>(factor));
    fail(what, buf);
}

void expectFactor(float target, float mult, float want, const char* what) {
    float factor = -1.0f;
    if (!edvr::hudQualityFactor(target, mult, &factor)) {
        fail(what, "returned false (treated as off)");
        return;
    }
    if (near(factor, want, 0.001f)) {
        ok(what);
        return;
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "factor %.6f, wanted %.6f",
                  static_cast<double>(factor), static_cast<double>(want));
    fail(what, buf);
}

void expectRound(uint32_t v, float factor, uint32_t want, const char* what) {
    const uint32_t got = edvr::hudQualityRoundDim(v, factor);
    if (got == want) {
        ok(what);
        return;
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "rounded to %u, wanted %u", got, want);
    fail(what, buf);
}

void expectMatch(uint32_t w, uint32_t h, const uint32_t* lw,
                 const uint32_t* lh, uint32_t n, int want, const char* what) {
    const int got = edvr::hudQualityMatchLearned(w, h, lw, lh, n);
    if (got == want) {
        ok(what);
        return;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "matched index %d, wanted %d", got, want);
    fail(what, buf);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--self-test") != 0) {
        std::printf("usage: hud_quality_test.exe --self-test\n");
        return 2;
    }

    // --- the key's parsing --------------------------------------------------
    expectParse("off", 0.0f, true, "off parses to no target");
    expectParse("1.0", 1.0f, true, "1.0 parses to a 1.0 target");
    expectParse("1.25", 1.25f, true, "1.25 parses to a 1.25 target");
    expectParse("bogus", 0.0f, false, "an unrecognized value is refused as off");
    expectParse("1.5", 0.0f, false, "a value that is not one of the three choices is refused");
    expectParse("", 0.0f, false, "an empty value is refused as off, not silently accepted");
    expectParse("1", 0.0f, false, "\"1\" is not \"1.0\" -- exact text, no float parsing of the key");
    expectParse(nullptr, 0.0f, false, "a null value is refused defensively");

    // --- the factor arithmetic ----------------------------------------------
    // The log line's own example: HMD Quality 0.70, target 1.0.
    expectFactor(1.0f, 0.70f, 1.4286f, "HMD Quality 0.7 at target 1.0 gives factor ~1.4286");
    // 1.0 at 1.0 is exactly the game's own size: no-op, not a 1.0x inflate.
    expectFactorOff(1.0f, 1.0f, "1.0 at HMD Quality 1.0 is a no-op (factor would be 1.0)");
    expectFactor(1.25f, 1.0f, 1.25f, "1.25 target at HMD Quality 1.0 gives factor 1.25");
    // A low HMD Quality would ask for more than 4x; capped, not refused.
    expectFactor(1.25f, 0.2f, 4.0f, "a factor over 4 is capped at 4, not refused");
    // No fxcfg read, or a read that found nothing usable: deviceHookHmdQuality
    // signals this with 0 (or, defensively, any non-positive value).
    expectFactorOff(1.0f, 0.0f, "an unknown (zero) HMD multiplier turns the feature off");
    expectFactorOff(1.0f, -1.0f, "a negative HMD multiplier turns the feature off too");
    expectFactorOff(0.0f, 0.7f, "a zero target (the key parsed to off) does nothing");
    // Just over the 1% floor should still fire; just under should not --
    // the boundary the log's "on but no interface surface matched" line
    // must never be reached by mistake for a target barely above today's.
    expectFactor(1.02f, 1.0f, 1.02f, "1.02 at HMD Quality 1.0 clears the 1% floor");
    expectFactorOff(1.005f, 1.0f, "1.005 at HMD Quality 1.0 does not clear the 1% floor");

    // --- fractional viewport/texture/scissor rounding ------------------------
    // The worked example this feature's own log line quotes: a 908x1361
    // vector surface at factor 1.4286 (HMD Quality 0.7 -> target 1.0).
    expectRound(908, 1.4286f, 1297, "908 wide rounds to 1297 at factor 1.4286");
    expectRound(1361, 1.4286f, 1944, "1361 tall rounds to 1944 at factor 1.4286");
    // A whole factor must reproduce the integer behaviour advanced.surface_inflate
    // already ships (2x, 3x, 4x): the shared rounding must not perturb it.
    expectRound(500, 2.0f, 1000, "a whole factor (2x) rounds exactly, unchanged from today");
    expectRound(333, 3.0f, 999, "...and 3x");
    // Round-half-up, not banker's rounding or truncation: 1 * 1.5 = 1.5
    // exactly (1.5 has an exact float32 representation, unlike 1.05, so
    // this is a test of the rounding rule and not of binary fraction noise).
    expectRound(1, 1.5f, 2, "exact .5 rounds up, matching the viewport path's +0.5f");
    // A factor of 1.0 must be a true no-op at the pixel level (hudQualityFactor
    // already refuses this case, but the rounding helper is tested alone too).
    expectRound(908, 1.0f, 908, "factor 1.0 changes nothing, pixel for pixel");

    // --- matching a candidate size against the learned set --------------------
    {
        const uint32_t lw[] = {908, 512, 256};
        const uint32_t lh[] = {1361, 724, 256};
        expectMatch(908, 1361, lw, lh, 3, 0, "an exact match against the first learned size");
        expectMatch(256, 256, lw, lh, 3, 2, "...and the last");
        expectMatch(909, 1361, lw, lh, 3, -1, "one pixel off in width does not match");
        expectMatch(908, 1360, lw, lh, 3, -1, "one pixel off in height does not match either");
        expectMatch(100, 100, lw, lh, 3, -1, "a size the classifier never learned does not match");
        expectMatch(908, 1361, lw, lh, 0, -1, "an empty learned set never matches");
        expectMatch(908, 1361, nullptr, nullptr, 0, -1, "null learned arrays are handled, not dereferenced");
    }

    if (g_fails) {
        std::printf("HUD QUALITY TEST FAILED (%d)\n", g_fails);
        return 1;
    }
    std::printf("HUD QUALITY TEST PASSED\n");
    return 0;
}

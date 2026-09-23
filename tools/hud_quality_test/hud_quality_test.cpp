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

void expectRatioNear(uint32_t a, uint32_t b, uint32_t tolerance, bool want,
                     const char* what) {
    const bool got = edvr::hudQualityRatioNear(a, b, tolerance);
    if (got == want) {
        ok(what);
        return;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "near() returned %s, wanted %s",
                 got ? "true" : "false", want ? "true" : "false");
    fail(what, buf);
}

const char* verdictName(edvr::HudQualityRatioVerdict v) {
    switch (v) {
        case edvr::HudQualityRatioVerdict::kNoSlot: return "kNoSlot";
        case edvr::HudQualityRatioVerdict::kNewCandidate: return "kNewCandidate";
        case edvr::HudQualityRatioVerdict::kSameSession: return "kSameSession";
        case edvr::HudQualityRatioVerdict::kConfirmed: return "kConfirmed";
    }
    return "?";
}

void expectVerdict(edvr::HudQualityRatioSlot* slots, uint32_t* count, uint32_t capacity,
                   uint32_t rw, uint32_t rh, uint32_t internalW, uint32_t tolerance,
                   edvr::HudQualityRatioVerdict want, const char* what) {
    const edvr::HudQualityRatioVerdict got =
        edvr::hudQualityRatioObserve(slots, count, capacity, rw, rh, internalW, tolerance);
    if (got == want) {
        ok(what);
        return;
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "verdict %s, wanted %s", verdictName(got), verdictName(want));
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

    // --- ratio-to-internal-resolution arithmetic -------------------------
    {
        // fss_res.h's own two-session census, the founding evidence for the
        // whole match: 908x1361 at scene 4340x4284, and 1363x2042 at scene
        // 6510x6426 -- "the same fraction to four significant figures".
        const uint32_t rw1 = edvr::hudQualityRatioX10000(908, 4340);
        const uint32_t rh1 = edvr::hudQualityRatioX10000(1361, 4284);
        const uint32_t rw2 = edvr::hudQualityRatioX10000(1363, 6510);
        const uint32_t rh2 = edvr::hudQualityRatioX10000(2042, 6426);
        char what[128];
        std::snprintf(what, sizeof(what),
                     "the width ratio (908/4340=%u, 1363/6510=%u ten-thousandths) agrees within tolerance",
                     rw1, rw2);
        if (edvr::hudQualityRatioNear(rw1, rw2, 10)) ok(what); else fail(what, "did not agree");
        std::snprintf(what, sizeof(what),
                     "the height ratio (1361/4284=%u, 2042/6426=%u ten-thousandths) agrees within tolerance",
                     rh1, rh2);
        if (edvr::hudQualityRatioNear(rh1, rh2, 10)) ok(what); else fail(what, "did not agree");
    }
    {
        const uint32_t r = edvr::hudQualityRatioX10000(0, 4340);
        if (r == 0) ok("a zero width ratios to 0, not a crash");
        else fail("a zero width ratios to 0, not a crash", "nonzero");
    }
    {
        const uint32_t r = edvr::hudQualityRatioX10000(1000, 0);
        if (r == 0) ok("a zero internal dimension ratios to 0 (nothing to divide by)");
        else fail("a zero internal dimension ratios to 0 (nothing to divide by)", "nonzero");
    }
    expectRatioNear(2092, 2094, 10, true, "two ten-thousandths apart, tolerance 10: near");
    expectRatioNear(2092, 2103, 10, false, "eleven ten-thousandths apart, tolerance 10: not near");
    expectRatioNear(2092, 2092, 0, true, "exact equality passes a zero tolerance");

    // --- the ratio-observation state machine (replaces the classifier) ----
    {
        edvr::HudQualityRatioSlot slots[4];
        uint32_t count = 0;
        // "First panel created before any draw": hudQualityRatioObserve's
        // own signature takes only a ratio and an internal width -- no
        // learned-surface table, no classifier, no draw of any kind is an
        // input to it, so its very first call (an empty table, exactly a
        // cold session's first candidate) already proves the match does
        // not and cannot depend on anything happening after this call.
        expectVerdict(slots, &count, 4, 2092, 3177, 4340, 10,
                     edvr::HudQualityRatioVerdict::kNewCandidate,
                     "the first-ever candidate (before any draw anywhere) is recorded, not used");
        if (count == 1) ok("...and the table now holds one entry");
        else fail("...and the table now holds one entry", "did not grow");

        // A second sighting at the SAME internal width (a second panel this
        // same session, or a second session that happened to run the same
        // HMD Quality): no new evidence, still not usable.
        expectVerdict(slots, &count, 4, 2092, 3177, 4340, 10,
                     edvr::HudQualityRatioVerdict::kSameSession,
                     "a repeat at the same internal width is not new evidence");
        if (count == 1) ok("...and no second entry was created for it");
        else fail("...and no second entry was created for it", "table grew");

        // A DIFFERENT internal width (a later session at a different HMD
        // Quality, fss_res.h's own two-resolution proof) confirms it --
        // and the very same call is where a caller would inflate.
        expectVerdict(slots, &count, 4, 2094, 3178, 6510, 10,
                     edvr::HudQualityRatioVerdict::kConfirmed,
                     "a different internal width now confirms the ratio (usable from this call on)");

        // Once confirmed, it stays confirmed on the next sighting, at
        // either width.
        expectVerdict(slots, &count, 4, 2092, 3177, 4340, 10,
                     edvr::HudQualityRatioVerdict::kConfirmed,
                     "an already-confirmed ratio stays confirmed");

        // A genuinely different ratio (not a rounding of the same one)
        // gets its OWN entry rather than corrupting the first.
        expectVerdict(slots, &count, 4, 1500, 2800, 4340, 10,
                     edvr::HudQualityRatioVerdict::kNewCandidate,
                     "a different ratio is tracked separately, not conflated");
        if (count == 2) ok("...and the table now holds two entries");
        else fail("...and the table now holds two entries", "did not grow to 2");

        // Filling the table, then a third distinct ratio with no room left.
        uint32_t fillCount = count;
        edvr::HudQualityRatioSlot fillSlots[2] = {slots[0], slots[1]};
        expectVerdict(fillSlots, &fillCount, 2, 9000, 100, 5000, 10,
                     edvr::HudQualityRatioVerdict::kNoSlot,
                     "a third distinct ratio is refused once the table is full");
        if (fillCount == 2) ok("...and the full table is left unchanged");
        else fail("...and the full table is left unchanged", "count moved");
    }

    if (g_fails) {
        std::printf("HUD QUALITY TEST FAILED (%d)\n", g_fails);
        return 1;
    }
    std::printf("HUD QUALITY TEST PASSED\n");
    return 0;
}

// vr_ssaa_hold_test -- the Supersampling hold's pure decision (src\d3d11\vr_ssaa_hold_math.h): the Settings.xml mode parse, the
// held value for each (profile, mode known, mode, requested), and the toast text. Drives the very functions the DLL calls.
// Built from the repo root by build.bat's :rig_vr_ssaa_hold_test. --dry-run writes nothing; --self-test runs the checks.
#include "../../src/d3d11/vr_ssaa_hold_math.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_checks = 0;
int g_fails = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        std::printf("FAIL: %s\n", what);
    }
}

bool same(float a, float b) { return std::fabs(a - b) < 1e-6f; }

void parseCases() {
    using edvr::ssaahold::parseStereoscopicMode;
    int m = -1;
    check(parseStereoscopicMode("<StereoscopicMode>3</StereoscopicMode>", &m) && m == 3, "mode 3 parses");
    check(parseStereoscopicMode("<A/>\r\n  <StereoscopicMode>\t5 \r\n</StereoscopicMode>", &m) && m == 5, "mode 5 with whitespace parses");
    check(parseStereoscopicMode("<StereoscopicMode>0<", &m) && m == 0, "mode 0 parses (off)");
    check(parseStereoscopicMode("<StereoscopicMode>6</StereoscopicMode>", &m) && m == 6, "mode 6 (the top of the game's clamp) parses");
    m = -7;
    check(!parseStereoscopicMode("<StereoscopicMode>7</StereoscopicMode>", &m) && m == -7, "mode 7 is refused and *mode is untouched");
    check(!parseStereoscopicMode("<StereoscopicMode>12</StereoscopicMode>", &m) && m == -7, "two digits past 6 are refused");
    check(!parseStereoscopicMode("<StereoscopicMode>x</StereoscopicMode>", &m), "a non-number is refused");
    check(!parseStereoscopicMode("<StereoscopicMode></StereoscopicMode>", &m), "an empty tag is refused");
    check(!parseStereoscopicMode("<StereoscopicMode>3", &m), "a tag with no closing '<' after the number is refused");
    check(!parseStereoscopicMode("<PresetName>Custom</PresetName>", &m), "a file with no StereoscopicMode tag is refused");
    check(!parseStereoscopicMode(nullptr, &m), "a null text is refused");
}

void decideCases() {
    using edvr::ssaahold::decide;
    using edvr::ssaahold::kHeldValue;
    // VR profile, mode known, 3D on: held at 1.0 whatever was asked.
    auto d = decide(true, true, 3, 0.5f);
    check(d.hold && same(d.value, kHeldValue), "VR, mode 3: held at 1.0 for a requested 0.5");
    d = decide(true, true, 5, 1.25f);
    check(d.hold && same(d.value, kHeldValue), "VR, mode 5: held at 1.0 for a requested 1.25");
    d = decide(true, true, 3, 1.0f);
    check(d.hold && same(d.value, 1.0f), "VR, mode 3: a requested 1.0 is held at 1.0 too");
    // 3D off: exactly today's behaviour, the game's own value passes.
    d = decide(true, true, 0, 0.5f);
    check(!d.hold && same(d.value, 0.5f), "VR, mode 0: not held, the requested 0.5 passes");
    // Flat profile: never held, whatever the mode.
    d = decide(false, true, 3, 0.85f);
    check(!d.hold && same(d.value, 0.85f), "flat profile, mode 3: not held, the requested 0.85 passes");
    // Mode unknown (no Settings.xml, or no tag): not held.
    d = decide(true, false, -1, 0.5f);
    check(!d.hold && same(d.value, 0.5f), "VR, mode unknown: not held, the requested 0.5 passes");
    d = decide(true, false, 3, 0.5f);
    check(!d.hold && same(d.value, 0.5f), "VR, mode unknown even with a stale 3: not held");
}

void toastCases() {
    char buf[160] = {};
    const int n = edvr::ssaahold::formatHoldToast(buf, sizeof(buf), 0.85f);
    check(n > 0 && std::strstr(buf, "Supersampling 0.85 is held at 1.0 in VR: use HMD Image Quality to set resolution") != nullptr,
          "the toast names the requested value and says where to set resolution");
    check(std::strlen(buf) < 120, "the toast fits its buffer");
}

}  // namespace

int main(int argc, char** argv) {
    bool dryRun = false, selfTest = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0) dryRun = true;
        if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
    }
    if (dryRun && !selfTest) {
        std::printf("vr_ssaa_hold_test: dry run, nothing written\n");
        return 0;
    }
    parseCases();
    decideCases();
    toastCases();
    if (g_fails) {
        std::printf("vr_ssaa_hold_test: %d of %d checks FAILED\n", g_fails, g_checks);
        return 1;
    }
    std::printf("PASS: %d vr ssaa hold checks (Settings.xml mode parse, held value by profile and mode, the toast)\n", g_checks);
    return 0;
}

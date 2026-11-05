// TEMPORARY, with the probe: src\d3d11\cockpit_glass_probe_core.h held to its contract. The canopy hashes are the ones
// tools\effects_arc.py extracts for CockpitGlass_Forward; this holds the table's shape and the formatting a log reader depends on.
#include "../../src/d3d11/cockpit_glass_probe_core.h"

#include <cstdio>
#include <cstring>

using namespace edvr::glass_probe;

static int g_failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

static uint32_t bitsOf(float f) {
    uint32_t b;
    std::memcpy(&b, &f, sizeof b);
    return b;
}

int main() {
    // The table: the flown pair is where the port starts, every hash is distinct, and nothing else matches.
    check(canopyVsIndex(0x8C091FFD08644E02ull) == 0, "the flown canopy VS is the intact one");
    check(canopyPsIndex(0x4E4FF61E8A08FC7Eull) == 2, "the flown canopy PS is intact q3");
    check(canopyPsIndex(0x8C091FFD08644E02ull) == -1, "a VS hash is not a canopy PS");
    check(canopyVsIndex(0) == -1 && canopyPsIndex(0) == -1, "an unbound shader is not the canopy");
    for (int i = 0; i < kCanopyPsCount; ++i)
        for (int j = 0; j < kCanopyVsCount; ++j) check(kCanopyPs[i].hash != kCanopyVs[j].hash, "PS and VS hashes distinct");
    for (int i = 0; i < kCanopyPsCount; ++i)
        for (int j = i + 1; j < kCanopyPsCount; ++j) check(kCanopyPs[i].hash != kCanopyPs[j].hash, "PS hashes distinct");

    // Every logged register is inside the copied range.
    for (const RegSpan& s : kCb1Spans) check(s.count > 0 && s.first + s.count <= kCb1Regs, "a cb1 span inside the copy");

    // Components: floats as %.6g, a denormal-patterned value as the integer it is.
    char t[32];
    formatComponent(bitsOf(1.0f), t, sizeof t);
    check(std::strcmp(t, "1") == 0, "1.0f prints 1");
    formatComponent(bitsOf(-0.5f), t, sizeof t);
    check(std::strcmp(t, "-0.5") == 0, "-0.5f prints -0.5");
    formatComponent(3u, t, sizeof t);
    check(std::strcmp(t, "#3") == 0, "integer 3 prints #3");
    formatComponent(0u, t, sizeof t);
    check(std::strcmp(t, "0") == 0, "zero prints 0");

    // Registers: a full one, and one past the end that says so instead of reading as zeros.
    uint8_t data[32] = {};
    const float r0[4] = {1.0f, 2.0f, 0.25f, 0.0f};
    std::memcpy(data, r0, sizeof r0);
    const uint32_t two = 2;
    std::memcpy(data + 16 + 4, &two, 4);
    char line[256];
    formatRegs(data, sizeof data, 0, 3, line, sizeof line);
    check(std::strcmp(line, "[0] 1 2 0.25 0 [1] 0 #2 0 0 [2] -") == 0, "registers print, a short buffer reads short");
    formatRegs(data, sizeof data, 0, 2, line, 12);
    check(std::strlen(line) == 11, "a small output buffer truncates, terminated");
    formatRegs(nullptr, 0, 290, 1, line, sizeof line);
    check(std::strcmp(line, "[290] -") == 0, "no data reads as missing");

    // The key and the sampling rule.
    check(probeKeyOn("on") && !probeKeyOn("off") && !probeKeyOn("") && !probeKeyOn(nullptr) && !probeKeyOn("1"), "only on arms it");
    check(sampleDue(false, 0, 12, 5, 0, 30000), "the first canopy draw is sampled at once");
    check(!sampleDue(false, 1, 12, 20000, 0, 30000), "not again inside the interval");
    check(sampleDue(false, 1, 12, 30000, 0, 30000), "again once the interval passes");
    check(!sampleDue(true, 1, 12, 90000, 0, 30000), "never while a readback is out");
    check(!sampleDue(false, 12, 12, 900000, 0, 30000), "never past the cap");

    std::printf("cockpit glass probe test: %s\n", g_failures ? "FAILED" : "passed");
    return g_failures ? 1 : 0;
}

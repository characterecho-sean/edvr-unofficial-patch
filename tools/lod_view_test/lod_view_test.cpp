// lod_view_test -- the LoD view probe's pure half (src/d3d11/lod_view_core.h), without a game, a device or the DLL: the identity and prologue
// decision, the slot the builder wrote, the output-size factor, the per-view table's first and 5 s lines, its overflow, and the line text.
// Everything under test is header-only, so this links nothing from src/.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/d3d11/lod_view_core.h"

namespace {

namespace lv = edvr::lodview;

int g_fails = 0;

void check(bool cond, const char* what) {
    if (cond) {
        std::printf("  ok    %s\n", what);
        return;
    }
    std::printf("  FAIL  %s\n", what);
    ++g_fails;
}

void testInstallDecision() {
    std::printf("install decision\n");
    uint8_t good[lv::kPrologueBytes];
    std::memcpy(good, lv::kPrologue, sizeof(good));
    check(lv::installDecision(true, lv::kExpectedTimestamp, lv::kExpectedImageSize, good, sizeof(good)) == lv::Install::kInstall,
          "build 334473 with its prologue installs");
    check(lv::installDecision(false, lv::kExpectedTimestamp, lv::kExpectedImageSize, good, sizeof(good)) == lv::Install::kUnreadable,
          "an unreadable PE header refuses");
    check(lv::installDecision(true, lv::kExpectedTimestamp, lv::kExpectedImageSize, nullptr, sizeof(good)) == lv::Install::kUnreadable,
          "an unreadable prologue refuses");
    check(lv::installDecision(true, lv::kExpectedTimestamp, lv::kExpectedImageSize, good, sizeof(good) - 1) == lv::Install::kUnreadable,
          "a short prologue read refuses");
    check(lv::installDecision(true, 0x6A989634u, lv::kExpectedImageSize, good, sizeof(good)) == lv::Install::kWrongIdentity,
          "build 332841's stamp refuses");
    check(lv::installDecision(true, lv::kExpectedTimestamp, 0x6409000u, good, sizeof(good)) == lv::Install::kWrongIdentity,
          "another image size refuses");
    uint8_t bad[lv::kPrologueBytes];
    std::memcpy(bad, good, sizeof(bad));
    bad[15] ^= 0x01;
    check(lv::installDecision(true, lv::kExpectedTimestamp, lv::kExpectedImageSize, bad, sizeof(bad)) == lv::Install::kWrongPrologue,
          "one changed prologue byte refuses");
    check(lv::kPrologue[0] == 0x48 && lv::kPrologue[4] == 0x18, "the first five bytes are mov [rsp+0x18],rbx (nothing relative for CodeHook)");
}

void testLayout() {
    std::printf("layout\n");
    check(lv::kSlotCount == 64, "64 slots of 0x570 before the count at +0x15C00");
    check(lv::writtenSlot(1) == 0, "count 1 -> slot 0");
    check(lv::writtenSlot(64) == 63, "count 64 -> slot 63");
    check(lv::writtenSlot(0) == lv::kSlotCount, "count 0 reads nothing");
    check(lv::writtenSlot(65) == lv::kSlotCount, "a count past the array reads nothing");
}

void testFactor() {
    std::printf("output factor\n");
    check(lv::outputFactor(960, 600, 960, 600, 1920, 1200) == 0.5f, "SS 0.5: a render-size view gets 600/1200");
    check(std::fabs(lv::outputFactor(1286, 804, 1286, 804, 1920, 1200) - 804.0f / 1200.0f) < 1e-7f, "SS 0.67: render height over output height");
    check(lv::outputFactor(2048, 2048, 960, 600, 1920, 1200) == 1.0f, "a shadow-map-sized view is left alone");
    check(lv::outputFactor(960, 601, 960, 600, 1920, 1200) == 1.0f, "a view one row off the render size is left alone");
    check(lv::outputFactor(1920, 1200, 1920, 1200, 1920, 1200) == 1.0f, "SS 1.0: nothing to do");
    check(lv::outputFactor(2880, 1800, 2880, 1800, 1920, 1200) == 1.0f, "SS 1.5: the finer LoD the player chose is kept");
    check(lv::outputFactor(960, 1200, 960, 1200, 1920, 1200) == 1.0f, "render below output on one axis only: left alone");
    check(lv::outputFactor(960, 600, 0, 0, 1920, 1200) == 1.0f, "no measured render size: left alone");
    check(lv::outputFactor(0, 0, 960, 600, 1920, 1200) == 1.0f, "a zero-sized view: left alone");
}

lv::Observation obs(uint32_t caller, uint32_t w, uint32_t h, float a) {
    lv::Observation o;
    o.callerRva = caller;
    o.width = w;
    o.height = h;
    o.a = a;
    return o;
}

void testTable() {
    std::printf("view table\n");
    lv::ViewTable t;
    lv::Due due[lv::kEntries];
    t.observe(obs(0x2010D0, 960, 600, 1.8e-3f));
    t.observe(obs(0x2010D0, 960, 600, 1.8e-3f));
    t.observe(obs(0x288C4AE, 2048, 2048, 4.0e-4f));
    check(t.used() == 2, "two (caller, size) pairs make two entries");
    size_t n = t.takeDue(1000, due, lv::kEntries);
    check(n == 2 && due[0].first && due[1].first, "both are due a first line");
    check(due[0].callsSince == 2 && due[1].callsSince == 1, "the first line counts the calls so far");
    check(t.takeDue(1001, due, lv::kEntries) == 0, "nothing is due right after");
    t.observe(obs(0x2010D0, 960, 600, 1.9e-3f));
    check(t.takeDue(1000 + lv::kReportEveryMs - 1, due, lv::kEntries) == 0, "not before 5 s");
    n = t.takeDue(1000 + lv::kReportEveryMs, due, lv::kEntries);
    check(n == 1 && !due[0].first && due[0].callsSince == 1 && due[0].entry.last.a == 1.9e-3f,
          "at 5 s the called entry is due again, with its latest values and the calls since");
    check(t.takeDue(1000 + 3 * lv::kReportEveryMs, due, lv::kEntries) == 0, "an entry not called since its line is not repeated");
    t.observe(obs(0x2010D0, 1920, 1200, 9.0e-4f));
    n = t.takeDue(1000 + 3 * lv::kReportEveryMs, due, lv::kEntries);
    check(n == 1 && due[0].first && due[0].entry.last.width == 1920, "the same caller at a new size is a new entry");

    lv::ViewTable full;
    for (uint32_t i = 0; i < lv::kEntries + 3; ++i) full.observe(obs(0x1000 + i, 64, 64, 1.0f));
    check(full.used() == lv::kEntries && full.overflow() == 3, "past 32 entries the calls are counted as overflow");
    lv::Due two[2];
    check(full.takeDue(0, two, 2) == 2 && full.takeDue(0, two, 2) == 2, "takeDue honours its cap and leaves the rest due");
}

void testLine() {
    std::printf("line text\n");
    lv::Due d;
    d.first = true;
    d.callsSince = 3;
    d.entry.last = obs(0x2010D0, 960, 600, 1.803e-3f);
    d.entry.last.slot = 2;
    d.entry.last.factor = 0.5f;
    char text[400];
    const int len = lv::formatView(text, sizeof(text), d, 960, 600, 1920, 1200, true);
    const std::string s(text);
    check(len > 0 && static_cast<size_t>(len) < sizeof(text), "the line fits its buffer");
    check(s.rfind("lod view: first caller=0x2010D0 size=960x600 slot=2 primary=0 A=1.803000e-03 B=0.000000e+00 calls=3", 0) == 0,
          "the first line's view half");
    check(s.find("| scene 960x600 output 1920x1200 key on factor 0.5000") != std::string::npos, "the runtime's sizes, the key and the factor");
    d.first = false;
    lv::formatView(text, sizeof(text), d, 0, 0, 0, 0, false);
    check(std::string(text).rfind("lod view: every 5s caller=", 0) == 0, "the 5 s line says so");
    check(std::string(text).find("scene 0x0 output 0x0 key off") != std::string::npos, "sizes not yet measured print as 0x0");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--self-test") != 0) {
        std::printf("usage: lod_view_test --self-test\n");
        return 2;
    }
    testInstallDecision();
    testLayout();
    testFactor();
    testTable();
    testLine();
    if (g_fails) {
        std::printf("lod_view_test: %d FAILED\n", g_fails);
        return 1;
    }
    std::printf("lod_view_test: all passed\n");
    return 0;
}

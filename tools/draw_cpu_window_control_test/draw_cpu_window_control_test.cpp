// Focused control-side tests for the existing timed-draw CPU denominator.
// Standalone checks for the draw-window denominator and production wiring.
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "draw_cpu_window.h"

namespace {
unsigned g_checks = 0;
unsigned g_failures = 0;

bool check(bool condition, const char* label) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", label);
    }
    return condition;
}

std::string readFile(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::string functionBody(const std::string& source, const std::string& signature) {
    const std::size_t start = source.find(signature);
    if (start == std::string::npos) return {};
    const std::size_t open = source.find('{', start + signature.size());
    if (open == std::string::npos) return {};
    unsigned depth = 0;
    for (std::size_t i = open; i < source.size(); ++i) {
        if (source[i] == '{') ++depth;
        else if (source[i] == '}' && --depth == 0)
            return source.substr(open, i - open + 1);
    }
    return {};
}

bool windowMathChecks() {
    bool okay = true;
    edvr::draw_cpu::Window window;

    // Keep the frame-level clamp: clamping each timed draw independently
    // would incorrectly turn the first frame's 100/130 pair into 20 ticks.
    window.noteDraw(100);
    window.noteDraw(20);
    window.closeFrame(true, 120, 130);
    window.noteDraw(50);
    window.noteDraw(25);
    window.closeFrame(true, 75, 30);
    okay &= check(window.windowTimedDraws == 4 && window.windowOwnTicks == 45 &&
                  std::abs(window.meanMs(1000) - 11.25) < 1e-12,
                  "frame-clamped numerator is divided by actual timed-draw count");

    window.noteDraw(100);
    window.closeFrame(false, 100, 0);
    okay &= check(window.windowTimedDraws == 4 && window.windowOwnTicks == 45 &&
                  window.frameTimedDraws == 0,
                  "unsampled frames are excluded and per-frame count is cleared");

    edvr::draw_cpu::Window zero;
    zero.closeFrame(true, 0, 0);
    okay &= check(!zero.hasTimedDraws(),
                  "no timed callbacks remains unavailable rather than measured zero");
    zero.noteDraw(10);
    zero.closeFrame(true, 10, 10);
    okay &= check(zero.hasTimedDraws() && zero.windowTimedDraws == 1 &&
                  zero.meanMs(1000) == 0.0,
                  "a fully forwarded timed callback is measured zero");

    window.resetWindow();
    okay &= check(!window.hasTimedDraws() && window.windowOwnTicks == 0 &&
                  window.frameTimedDraws == 0,
                  "report close clears aggregate totals after pending frame state has already closed");
    return okay;
}

bool productionWiringChecks(const std::string& monitor,
                            const std::string& monitorHeader,
                            const std::string& screen) {
    if (monitor.empty() || monitorHeader.empty() || screen.empty())
        return check(false, "control production source inputs are readable");
    bool okay = true;
    const std::string frame = functionBody(monitor, "void perfMonitorFrame(");
    const std::string ticks = functionBody(monitor, "void perfMonitorDrawTicks(");
    const std::string active = functionBody(monitor, "void perfMonitorSetActive(");
    const std::size_t estimate = frame.find("(s.drawWholeTicks - s.drawRealTicks) *");
    const bool headerAndState = monitor.find("draw_cpu_window.h") != std::string::npos &&
        monitor.find("draw_cpu::Window drawCpuWindow;") != std::string::npos;
    const std::size_t scale = frame.find("static_cast<int64_t>(kPerfMonitorDrawTimeStride);");
    const std::size_t closeValid = frame.find(
        "s.drawCpuWindow.closeFrame(true, s.drawWholeTicks, s.drawRealTicks);");
    const std::size_t report = frame.find("draw hook CPU: 1800-frame window ending");
    const std::size_t reset = frame.find("s.drawCpuWindow.resetWindow();");
    const std::size_t totalsReset = frame.find("s.drawWholeTicks = s.drawRealTicks = 0;");
    okay &= check(headerAndState,
                  "control includes the denominator header and stores its window in monitor state");
    okay &= check(!frame.empty() && estimate != std::string::npos &&
                  scale != std::string::npos && estimate < scale &&
                  closeValid != std::string::npos && estimate < closeValid &&
                  closeValid < report && report < reset && reset < totalsReset,
                  "legacy scaled frame estimate and 1800-frame report boundary are preserved");
    okay &= check(frame.find("detail::g_perfMonitorSampleDraws && qpcFrequency() > 0") != std::string::npos &&
                  frame.find("s.drawCpuWindow.closeFrame(false, 0, 0);") != std::string::npos &&
                  frame.find("s.drawCpuWindow.closeFrame(false, 0, 0);") < totalsReset,
                  "invalid or unsampled frames do not enter the timed-draw denominator");
    okay &= check(!ticks.empty() && ticks.find("g_s.drawCpuWindow.noteDraw(wholeTicks);") != std::string::npos &&
                  ticks.find("g_s.drawCpuWindow.noteDraw(wholeTicks);") >
                      ticks.find("if (realTicks > 0) g_s.drawRealTicks += realTicks;"),
                  "denominator counts the same callbacks as the existing DrawClock totals");
    okay &= check(!active.empty() && active.find("drawCpuWindow") == std::string::npos,
                  "opening or closing the monitor does not reset the denominator");
    okay &= check(monitor.find("constexpr uint32_t kDrawSampleEvery = 16;") != std::string::npos &&
                  monitor.find("detail::g_perfMonitorSampleDraws = (s.frameNo % kDrawSampleEvery) == 0;") != std::string::npos &&
                  monitorHeader.find("kPerfMonitorDrawTimeStride = 64") != std::string::npos,
                  "main offset-zero 1-in-16 frame and per-thread 1-in-64 draw samplers remain unchanged");
    const std::size_t drawClockSample = screen.find("on(perfMonitorSampleDraws() &&");
    const std::size_t drawClockStride = screen.find(
        "(++t_drawClockOrdinal % kPerfMonitorDrawTimeStride) == 0", drawClockSample);
    okay &= check(drawClockSample != std::string::npos &&
                  screen.find("thread_local uint32_t t_drawClockOrdinal = 0;") != std::string::npos &&
                  drawClockStride != std::string::npos,
                  "the original per-thread offset-zero 1-in-64 DrawClock selection is unchanged");
    okay &= check(screen.find("if (on) perfMonitorDrawTicks(qpcNow() - t0, real);") != std::string::npos,
                  "the existing DrawClock callback remains the sole denominator source");
    okay &= check(frame.find("per timed draw sample across %llu samples") != std::string::npos &&
                  frame.find("per-timed-draw mean unavailable (%llu valid timed draw samples)") != std::string::npos,
                  "report distinguishes a measured zero mean from no timed samples");
    const std::string cpuScopeSuffix =
        "subtracts the first forwarding interval, including indexed-instanced weapon-motion reissue; includes later EDVR reissues; not total EDVR CPU";
    const std::size_t cpuScopeFirst = frame.find(cpuScopeSuffix);
    const std::size_t cpuScopeSecond = frame.find(cpuScopeSuffix, cpuScopeFirst + cpuScopeSuffix.size());
    okay &= check(cpuScopeFirst != std::string::npos && cpuScopeSecond != std::string::npos &&
                  frame.find(cpuScopeSuffix, cpuScopeSecond + cpuScopeSuffix.size()) == std::string::npos,
                  "both report branches label subtract-first-forwarding scope and exclude total EDVR CPU");

    return okay;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--dry-run") {
        std::puts("draw_cpu_window_control_test: dry-run (no source access)");
        return 0;
    }
    if (argc != 5 || std::string(argv[1]) != "--self-test") {
        std::puts("usage: draw_cpu_window_control_test --dry-run | --self-test <perf_monitor.cpp> <perf_monitor.h> <vscreen.cpp>");
        return 2;
    }
    bool okay = windowMathChecks();
    okay &= productionWiringChecks(readFile(argv[2]), readFile(argv[3]), readFile(argv[4]));
    std::printf("draw_cpu_window_control_test: %u checks, %u failures\n",
                g_checks, g_failures);
    return okay && g_failures == 0 ? 0 : 1;
}

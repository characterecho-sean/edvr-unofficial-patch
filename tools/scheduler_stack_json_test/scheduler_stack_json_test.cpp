// Build gate for the production SchedulerStackProbe JSON writer. The
// kinematic writer shipped three serialization failures before its gate
// existed; compilation cannot catch a dropped quote, so this rig
// serializes a deterministic fixture through the real writeJson and hands
// the text to tools\scheduler_stack_json_selftest.py, which strict-parses
// it and asserts every fixture value round-trips. The C++ side runs a
// quote/brace structural scan first so a broken stream is diagnosed here
// even before the python parse.
#include "../../src/d3d11/scheduler_stack_probe.h"
#include "../../src/common/log.h"
#include <cstdio>
#include <cwchar>
#include <sstream>
#include <string>

namespace edvr {
// Linker stubs for the hook-install paths (arm/finish/reset), which this
// rig never calls; the pattern is set by kinematic_json_test. The Log stub
// stands in for src\common\log.cpp (the fixture must not write log
// files).
const char* attachSchedulerStackHooks(SchedulerStackProbe*) noexcept { return "identity_mismatch"; }
void detachSchedulerStackHooks(SchedulerStackProbe*) noexcept {}
Log& Log::get() { static Log instance; return instance; }
Log::~Log() = default;
void Log::note(const char*, ...) {}
}
using namespace edvr;

namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
}

// Outside string/escape state, braces must balance and every string must
// terminate. A dropped quote leaves the scanner inside a string (the rest
// of the document reads as string content) or unbalances the braces, so
// this catches the missing-quote class without a JSON library.
bool structuralScan(const std::string& s) {
    bool inStr = false, esc = false;
    int depth = 0;
    for (char c : s) {
        if (inStr) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') inStr = true;
        else if (c == '{') ++depth;
        else if (c == '}') { if (--depth < 0) return false; }
    }
    return !inStr && depth == 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    if (!std::wcscmp(argv[1], L"--dry-run")) {
        std::puts("scheduler_stack_json_test: dry-run (no runtime, device or files)");
        return 0;
    }
    if (std::wcscmp(argv[1], L"--self-test")) return 2;
    schedulerStackProbe.selfTestPopulateForJson();
    std::ostringstream j;
    j << '{';
    schedulerStackProbe.writeJson(j);
    j << '}';
    const std::string out = j.str();
    check(structuralScan(out), "quote/brace balance outside escapes");
    check(out.rfind("{\"schedulerStack\":{", 0) == 0,
          "document opens with the schedulerStack object");
    check(out.size() > 2 && out[out.size() - 1] == '}', "document closes");
    // JSON on stdout for the python gate; diagnostics on stderr.
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fputc('\n', stdout);
    std::fprintf(stderr, "scheduler_stack_json_test: %u checks, %u failures\n",
                 checks, failures);
    return failures ? 1 : 0;
}

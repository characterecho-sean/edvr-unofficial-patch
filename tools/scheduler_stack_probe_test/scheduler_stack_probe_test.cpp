// Build gate for the SchedulerStackProbe's capture/table logic -- the parts
// a flight would exercise at 36-540 calls/frame: the stride-tolerant stack
// scan (range filter, gap budget, cap, fault handling), the FNV-1a-64
// signature, and the per-target signature table (aggregation, overflow).
// arm()/finish() are NOT used (they would hook the test process); state is
// seeded directly, the pattern kinematic_probe_test set. The fault fixture
// runs against a page this process owns.
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <sstream>
#include <string>
#define private public
#include "../../src/d3d11/scheduler_stack_probe.h"
#undef private
#include "../../src/common/log.h"
// The implementation under test is compiled INTO this TU: MSVC encodes
// access specifiers in decorated names, so a separately linked
// scheduler_stack_probe.obj would not resolve the private members this rig
// seeds (the kinematic_probe_test pattern).
#include "../../src/d3d11/scheduler_stack_probe.cpp"

namespace edvr {
// Linker stubs: this rig never hooks anything (arm/finish/reset unused),
// and the Log stub stands in for src\common\log.cpp (note() must not
// write files here).
const char* attachSchedulerStackHooks(SchedulerStackProbe*) noexcept { return "installed"; }
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

constexpr uintptr_t kBase = 0x100000000ull; // synthetic image base for the tests
constexpr uintptr_t kLo = kBase + SchedulerStackProbe::kRangeLo;
constexpr uintptr_t kHi = kBase + SchedulerStackProbe::kRangeHi;

// Seed as arm() leaves the probe, minus the hook install: cleared, active,
// imageBase_ carrying the synthetic base so crafted stacks fall in range.
void seed(SchedulerStackProbe& p) {
    p.clearLocked();
    p.imageBase_ = kBase;
    p.active_.store(true, std::memory_order_release);
}

// The capture: an in-range image with junk between the return addresses.
// image[i] slots chosen so hits and misses interleave within the gap
// budget; the scan must collect exactly the in-range values in order.
void caseCaptureCollectsInRangeWithGaps() {
    uint64_t image[64]{};
    image[0] = kLo + 0x100;          // immediate caller (the stub)
    image[1] = 0xDEADBEEFull;        // local, not a return address
    image[2] = kBase + 0x43245A0ull; // scheduler candidate (.text, in range)
    image[3] = 0x1234ull;
    image[4] = 0x1235ull;
    image[5] = kHi - 0x10;           // deepest frame
    uint64_t out[SchedulerStackProbe::kStackCap]{};
    bool faulted = true;
    const uint32_t n = SchedulerStackProbe::captureStack(
        reinterpret_cast<uintptr_t>(&image[0]), kLo, kHi, out,
        SchedulerStackProbe::kStackCap, &faulted);
    check(n == 3, "capture: three in-range qwords collected");
    check(!faulted, "capture: no fault on an owned buffer");
    check(out[0] == image[0] && out[1] == image[2] && out[2] == image[5],
          "capture: hits in scan order, junk skipped");
    check(out[1] > kLo && out[1] < kHi, "capture: collected VA is inside the range");
}

// More than kMaxGaps consecutive misses end the scan: deeper real frames
// must not be reached across a region that cannot be a stack.
void caseCaptureGapBudget() {
    uint64_t image[64]{};
    image[0] = kLo + 0x100;
    for (uint32_t i = 1; i <= SchedulerStackProbe::kMaxGaps + 1; ++i)
        image[i] = 0x1000 + i; // all misses, one past the budget
    image[SchedulerStackProbe::kMaxGaps + 2] = kLo + 0x200; // beyond the wall
    uint64_t out[SchedulerStackProbe::kStackCap]{};
    bool faulted = false;
    const uint32_t n = SchedulerStackProbe::captureStack(
        reinterpret_cast<uintptr_t>(&image[0]), kLo, kHi, out,
        SchedulerStackProbe::kStackCap, &faulted);
    check(n == 1, "capture: the gap budget truncates the scan");
    check(out[0] == image[0], "capture: the prefix before the wall is kept");
}

// kStackCap in-range values: the signature is bounded even on an
// artificially dense stack.
void caseCaptureCap() {
    uint64_t image[64]{};
    for (uint32_t i = 0; i < 40; ++i) image[i] = kLo + i * 0x20;
    uint64_t out[SchedulerStackProbe::kStackCap]{};
    bool faulted = false;
    const uint32_t n = SchedulerStackProbe::captureStack(
        reinterpret_cast<uintptr_t>(&image[0]), kLo, kHi, out,
        SchedulerStackProbe::kStackCap, &faulted);
    check(n == SchedulerStackProbe::kStackCap, "capture: capped at kStackCap");
    check(out[SchedulerStackProbe::kStackCap - 1] == image[SchedulerStackProbe::kStackCap - 1],
          "capture: the cap keeps the deepest collected frame");
}

// A fault mid-scan keeps the collected prefix and says so: a garbage RSP
// must read as a faulted capture, never as a shallow stack.
void caseCaptureFaultKeepsPrefix() {
    // Two committed pages; the scan starts one qword before a NOACCESS page
    // so the second read faults.
    uint8_t* region = static_cast<uint8_t*>(VirtualAlloc(nullptr, 8192,
                                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(region != nullptr, "capture fault: fixture pages allocated");
    if (!region) return;
    DWORD oldProtect = 0;
    check(VirtualProtect(region + 4096, 4096, PAGE_NOACCESS, &oldProtect) != 0,
          "capture fault: guard page armed");
    uint64_t* last = reinterpret_cast<uint64_t*>(region + 4096 - 8);
    *last = kLo + 0x100;
    uint64_t out[SchedulerStackProbe::kStackCap]{};
    bool faulted = false;
    const uint32_t n = SchedulerStackProbe::captureStack(
        reinterpret_cast<uintptr_t>(last), kLo, kHi, out,
        SchedulerStackProbe::kStackCap, &faulted);
    check(n == 1 && out[0] == *last, "capture fault: the prefix survives");
    check(faulted, "capture fault: the fault is reported");
    VirtualFree(region, 0, MEM_RELEASE);
}

// Golden FNV-1a-64 vectors (reference values computed offline; the empty
// list is the offset basis, the FNV spec's own check).
void caseFnvGolden() {
    const uint64_t three[] = {0x1462B4A0ull, 0x144321940ull, 0x1436A1000ull};
    check(SchedulerStackProbe::fnv1a64(three, 3) == 0xDA6A4E19B4964CA1ull,
          "fnv: three-word golden vector");
    const uint64_t one[] = {0x1462B4A0ull};
    check(SchedulerStackProbe::fnv1a64(one, 1) == 0x8744B2F324534AF7ull,
          "fnv: one-word golden vector");
    check(SchedulerStackProbe::fnv1a64(nullptr, 0) == 0xCBF29CE484222325ull,
          "fnv: the empty list is the offset basis");
}

// The table path: identical stacks aggregate under one signature with the
// call count, and an inactive probe counts nothing.
void caseNoteEntryAggregates() {
    SchedulerStackProbe p;
    seed(p);
    // Zeroed across the whole scan window, not just the slots used. noteEntry
    // reads until kMaxGaps misses in a row, and an 8-slot image left fewer
    // than kMaxGaps zeros after its last hit, so the scan ran past the array
    // into whatever the stack held there. An in-range leftover (the capture
    // cases above fill their images with in-range values) made the depth 3:
    // a pooled build failed "the exemplar kept the real depth" once in a run
    // where ten standalone runs of the same binary passed (2026-09-29).
    uint64_t image[SchedulerStackProbe::kScanSlots]{};
    image[0] = kLo + 0x100;
    image[1] = 0x99;
    image[2] = kLo + 0x200;
    const uintptr_t rsp = reinterpret_cast<uintptr_t>(&image[0]);
    p.noteEntry(0, rsp);
    p.noteEntry(0, rsp);
    SchedulerStackProbe::Summary s = p.summary();
    check(s.calls[0] == 2, "noteEntry: both calls counted");
    check(s.unique[0] == 1, "noteEntry: one signature for two identical stacks");
    check(s.overflow[0] == 0 && s.collisions[0] == 0, "noteEntry: no overflow, no collisions");
    {
        std::lock_guard<std::mutex> lock(p.mutex_);
        check(p.targets_[0].sigs[0].count == 2, "noteEntry: the signature's count aggregated");
        check(p.targets_[0].sigs[0].depth == 2, "noteEntry: the exemplar kept the real depth");
        check(p.targets_[0].sigs[0].stack[0] == image[0] &&
              p.targets_[0].sigs[0].stack[1] == image[2],
              "noteEntry: the exemplar is a raw sample of the stack");
    }
    check(s.calls[1] == 0 && s.calls[2] == 0 && s.calls[3] == 0,
          "noteEntry: other targets untouched");
    p.active_.store(false, std::memory_order_release);
    p.noteEntry(0, rsp);
    s = p.summary();
    check(s.calls[0] == 2, "noteEntry: an inactive probe counts nothing");
    check(s.faults[0] == 0, "noteEntry: a clean capture faults nothing");
}

// kSigCap distinct signatures: the 65th distinct one is counted as
// overflow, the table stays bounded, earlier signatures still aggregate.
void caseNoteEntryOverflow() {
    SchedulerStackProbe p;
    seed(p);
    // The whole scan window, zeroed, for the reason caseNoteEntryAggregates
    // gives: one hit then seven zeros is fewer than kMaxGaps.
    uint64_t image[SchedulerStackProbe::kScanSlots]{};
    const uintptr_t rsp = reinterpret_cast<uintptr_t>(&image[0]);
    for (uint32_t i = 0; i < SchedulerStackProbe::kSigCap + 6; ++i) {
        image[0] = kLo + 0x100 + i * 0x40; // distinct caller per signature
        p.noteEntry(2, rsp);
    }
    image[0] = kLo + 0x100; // back to the first signature
    p.noteEntry(2, rsp);
    const SchedulerStackProbe::Summary s = p.summary();
    check(s.calls[2] == SchedulerStackProbe::kSigCap + 7, "overflow: every call counted");
    check(s.unique[2] == SchedulerStackProbe::kSigCap, "overflow: the table stays at kSigCap");
    check(s.overflow[2] == 6, "overflow: only the distinct-past-cap calls overflow");
    {
        std::lock_guard<std::mutex> lock(p.mutex_);
        check(p.targets_[2].sigs[0].count == 2,
              "overflow: the surviving first signature still aggregates");
    }
}

// The scan's upper bound is build 334925's by default and moves with setScanHi:
// an unknown build's hook sets it from that build's own executable section.
void caseScanBound() {
    SchedulerStackProbe p;
    check(p.rangeHi_ == SchedulerStackProbe::kRangeHi, "scan bound: defaults to build 334925's");
    seed(p);
    uint64_t image[SchedulerStackProbe::kScanSlots]{};
    image[0] = kBase + 0x2000; // past a rebuilt image's end below
    image[1] = kBase + 0x1200;
    const uintptr_t rsp = reinterpret_cast<uintptr_t>(&image[0]);
    p.noteEntry(0, rsp);
    p.setScanHi(0x1800);
    check(p.rangeHi_ == 0x1800, "scan bound: setScanHi takes effect");
    p.noteEntry(1, rsp);
    std::lock_guard<std::mutex> lock(p.mutex_);
    check(p.targets_[0].sigs[0].depth == 2, "scan bound: the default keeps both frames");
    check(p.targets_[1].sigs[0].depth == 1 && p.targets_[1].sigs[0].stack[0] == image[1],
          "scan bound: a lower bound drops the frame above it");
}

// A minimal AMD64 PE header block with only the fields executableEndRva reads.
struct FakeSection { uint32_t va, vsize, chars; };
void fakeHeaders(uint8_t* h, size_t len, uint32_t baseOfCode, uint32_t align, uint32_t imageSize,
                 const FakeSection* secs, uint32_t count) {
    std::memset(h, 0, len);
    auto put = [&](size_t at, uint32_t v, size_t n) {
        for (size_t i = 0; i < n; ++i) h[at + i] = uint8_t(v >> (8 * i));
    };
    const size_t nt = 0x80, opt = nt + 24, optSize = 240;
    put(0, 0x5A4D, 2); put(0x3C, uint32_t(nt), 4);
    put(nt, 0x4550, 4); put(nt + 4, 0x8664, 2); put(nt + 6, count, 2); put(nt + 20, uint32_t(optSize), 2);
    put(opt, 0x20B, 2); put(opt + 20, baseOfCode, 4); put(opt + 32, align, 4); put(opt + 56, imageSize, 4);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = opt + optSize + size_t(i) * 40;
        put(at + 8, secs[i].vsize, 4); put(at + 12, secs[i].va, 4); put(at + 36, secs[i].chars, 4);
    }
}

void caseExecutableEnd() {
    constexpr uint32_t kExec = 0x60000020u, kData = 0x40000040u;
    uint8_t h[4096];
    // Build 334925's headers (read from the archived exe): .text at 0x1000 with a virtual size of 0x4DB498E, then
    // .rdata at 0x4DB6000; the end rounds up to the section alignment, and is kRangeHi.
    const FakeSection known[] = {{0x1000, 0x4DB498E, kExec}, {0x4DB6000, 0x10AF5EC, kData}};
    fakeHeaders(h, sizeof(h), 0x1000, 0x1000, 0x640C000, known, 2);
    check(SchedulerStackProbe::executableEndRva(h, sizeof(h)) == SchedulerStackProbe::kRangeHi,
          "pe end: build 334925's headers give kRangeHi");
    // Already a multiple of the alignment: no extra page.
    const FakeSection exact[] = {{0x1000, 0x4DB5000, kExec}, {0x4DB6000, 0x1000, kData}};
    fakeHeaders(h, sizeof(h), 0x1000, 0x1000, 0x640C000, exact, 2);
    check(SchedulerStackProbe::executableEndRva(h, sizeof(h)) == 0x4DB6000,
          "pe end: an exact multiple stays");
    // A rebuilt, larger .text.
    const FakeSection bigger[] = {{0x1000, 0x4F00001, kExec}, {0x4F02000, 0x1000, kData}};
    fakeHeaders(h, sizeof(h), 0x1000, 0x1000, 0x6500000, bigger, 2);
    check(SchedulerStackProbe::executableEndRva(h, sizeof(h)) == 0x4F02000,
          "pe end: a larger .text rounds up");
    // An earlier executable section that does not hold the code start is skipped.
    const FakeSection two[] = {{0x1000, 0x800, kExec}, {0x2000, 0x3000, kExec}};
    fakeHeaders(h, sizeof(h), 0x2000, 0x1000, 0x8000, two, 2);
    check(SchedulerStackProbe::executableEndRva(h, sizeof(h)) == 0x5000,
          "pe end: the section holding BaseOfCode");
    // Refusals: not executable, no section holds the code start, the end past the image, a bad alignment.
    const FakeSection data[] = {{0x1000, 0x2000, kData}};
    fakeHeaders(h, sizeof(h), 0x1000, 0x1000, 0x8000, data, 1);
    check(SchedulerStackProbe::executableEndRva(h, sizeof(h)) == 0, "pe end: not executable -> 0");
    const FakeSection text[] = {{0x1000, 0x2000, kExec}};
    fakeHeaders(h, sizeof(h), 0x9000, 0x1000, 0x8000, text, 1);
    check(SchedulerStackProbe::executableEndRva(h, sizeof(h)) == 0, "pe end: no section holds the code start -> 0");
    fakeHeaders(h, sizeof(h), 0x1000, 0x1000, 0x2000, text, 1);
    check(SchedulerStackProbe::executableEndRva(h, sizeof(h)) == 0, "pe end: end past SizeOfImage -> 0");
    fakeHeaders(h, sizeof(h), 0x1000, 0, 0x8000, text, 1);
    check(SchedulerStackProbe::executableEndRva(h, sizeof(h)) == 0, "pe end: zero alignment -> 0");
    fakeHeaders(h, sizeof(h), 0x1000, 0x1800, 0x8000, text, 1);
    check(SchedulerStackProbe::executableEndRva(h, sizeof(h)) == 0, "pe end: non-power-of-two alignment -> 0");
    fakeHeaders(h, sizeof(h), 0x1000, 0x1000, 0x8000, text, 1);
    check(SchedulerStackProbe::executableEndRva(h, sizeof(h)) == 0x3000, "pe end: the plain case holds");
    // Damaged headers.
    check(SchedulerStackProbe::executableEndRva(nullptr, 0) == 0, "pe end: no buffer -> 0");
    check(SchedulerStackProbe::executableEndRva(h, 0x20) == 0, "pe end: truncated before e_lfanew -> 0");
    check(SchedulerStackProbe::executableEndRva(h, 0x120) == 0, "pe end: truncated section table -> 0");
    h[0] = 'X';
    check(SchedulerStackProbe::executableEndRva(h, sizeof(h)) == 0, "pe end: bad MZ -> 0");
    fakeHeaders(h, sizeof(h), 0x1000, 0x1000, 0x8000, text, 1);
    h[0x80] = 'Q';
    check(SchedulerStackProbe::executableEndRva(h, sizeof(h)) == 0, "pe end: bad PE signature -> 0");
    fakeHeaders(h, sizeof(h), 0x1000, 0x1000, 0x8000, text, 1);
    h[0x80 + 4] = 0x4C; h[0x80 + 5] = 0x01; // i386
    check(SchedulerStackProbe::executableEndRva(h, sizeof(h)) == 0, "pe end: not AMD64 -> 0");
    fakeHeaders(h, sizeof(h), 0x1000, 0x1000, 0x8000, text, 1);
    h[0x80 + 6] = 97; // NumberOfSections
    check(SchedulerStackProbe::executableEndRva(h, sizeof(h)) == 0, "pe end: 97 sections -> 0");
}

// The same parse on a real image: this test executable's own headers place this function inside its code section.
void caseExecutableEndOfThisImage() {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const uintptr_t end = SchedulerStackProbe::executableEndRva(base, 4096);
    const uintptr_t here = reinterpret_cast<uintptr_t>(&caseExecutableEndOfThisImage) - reinterpret_cast<uintptr_t>(base);
    check(end != 0 && here < end, "pe end: this image's own code lies below its executable end");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    if (!std::wcscmp(argv[1], L"--dry-run")) {
        std::puts("scheduler_stack_probe_test: dry-run (no runtime, device or files)");
        return 0;
    }
    if (std::wcscmp(argv[1], L"--self-test")) return 2;
    caseCaptureCollectsInRangeWithGaps();
    caseCaptureGapBudget();
    caseCaptureCap();
    caseCaptureFaultKeepsPrefix();
    caseFnvGolden();
    caseNoteEntryAggregates();
    caseNoteEntryOverflow();
    caseScanBound();
    caseExecutableEnd();
    caseExecutableEndOfThisImage();
    std::fprintf(stderr, "scheduler_stack_probe_test: %u checks, %u failures\n",
                 checks, failures);
    return failures ? 1 : 0;
}

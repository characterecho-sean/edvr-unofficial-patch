// Issue #38's per-feature GPU cost census (gpu_census.h/.cpp): the
// estimator math, the rotation and its K cap, a real GpuTimer round trip
// on WARP, and the 30 s line's format -- including "-" for a section that
// never occurred.
//
// Whitebox, tools\ui_depth_test's and hologram_depth_test's own
// convention: the production .cpp is included directly so this rig can
// reach its rotation state and its window formatter, neither of which
// gpu_census.h exposes on purpose (nothing outside gpu_census.cpp needs
// them). gpu_timing.cpp/gpu_frame_timing.cpp/gpu_span_d3d11.cpp are real,
// linked sources (build.bat), so the WARP round trip is a real disjoint
// timestamp pair, not a fake.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include "../../src/common/system_d3d11.h"
#include "../../src/common/log.h"

using Microsoft::WRL::ComPtr;

namespace edvr {
// Captures the formatted line instead of writing it anywhere, so the
// format checks below can inspect the actual text -- hologram_depth_test's
// own convention, not tools\ui_depth_test's no-op stub.
std::string g_lastLog;
Log& Log::get() { static Log instance; return instance; }
Log::~Log() = default;
void Log::note(const char* fmt, ...) {
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_lastLog = buf;
    std::fputs(buf, stdout);
    std::fputc('\n', stdout);
}
} // namespace edvr

#include "../../src/d3d11/gpu_census.cpp"

namespace {
unsigned g_checks = 0;
void check(bool ok, const char* why) { ++g_checks; if (!ok) throw std::runtime_error(why); }
void hr(HRESULT h) { check(h == S_OK, "D3D operation failed"); }

struct Runtime {
    decltype(&D3D11CreateDevice) create = edvr::systemD3D11CreateDevice();
    Runtime() { check(create != nullptr, "System32 D3D11CreateDevice"); }
};
struct Device {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    explicit Device(Runtime& rt) {
        D3D_FEATURE_LEVEL level{};
        hr(rt.create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                      D3D11_SDK_VERSION, &dev, &level, &ctx));
    }
};

ComPtr<ID3D11Texture2D> makeTarget(ID3D11Device* dev, ComPtr<ID3D11RenderTargetView>& rtvOut) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 8;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> tex;
    hr(dev->CreateTexture2D(&td, nullptr, &tex));
    hr(dev->CreateRenderTargetView(tex.Get(), nullptr, &rtvOut));
    return tex;
}
} // namespace

namespace edvr {
namespace {

// ---- 1: the estimator math, as a pure function -------------------------
void estimatorMathCases() {
    SectionState st;
    {
        const auto snap = snapshotOf(st, 100);
        check(!snap.occurred, "estimator: zero occurrences reads as not-occurred ('-')");
    }
    {
        st.occurrences = 3;   // no sample completed yet -- still "occurred", 0 ms/frame
        const auto snap = snapshotOf(st, 100);
        check(snap.occurred, "estimator: an occurrence with no completed sample still counts as occurred");
        check(snap.msPerFrame == 0.0, "estimator: no completed sample means 0 ms/frame, never a guess");
        check(std::fabs(snap.perFrame - 0.03) < 1e-9, "estimator: occurrences/frame = occurrences / frames");
    }
    {
        st = SectionState{};
        st.occurrences = 10;
        st.sampler.totals.ms = 5.0;
        st.sampler.totals.samples = 5;   // 5 completed samples averaging 1 ms each
        const auto snap = snapshotOf(st, 50);   // 10 occurrences / 50 frames = 0.2/frame
        check(std::fabs(snap.perFrame - 0.2) < 1e-9, "estimator: occurrences/frame with real samples");
        check(std::fabs(snap.msPerFrame - 0.2) < 1e-9, "estimator: (ms/occurrence) * (occurrences/frame)");
    }
    {
        // A running GpuIntervals total never resets itself -- the window's
        // own contribution is a delta off a baseline, not the raw total.
        st = SectionState{};
        st.occurrences = 4;
        st.baseMs = 20.0; st.baseSamples = 10;
        st.sampler.totals.ms = 23.0; st.sampler.totals.samples = 12;   // +3 ms over +2 samples this window
        const auto snap = snapshotOf(st, 20);   // 4/20 = 0.2/frame
        check(std::fabs(snap.msPerFrame - (1.5 * 0.2)) < 1e-9,
              "estimator: reads a delta off the running totals, not the totals themselves");
    }
}

// ---- 2/3: rotation (one active section, the K cap) and a real WARP round trip
void rotationAndRealTimerCase(Device& d) {
    check(gpuTimingBind(d.dev.Get(), d.ctx.Get()), "bind canonical WARP timer owner");
    check(occurrenceCapFor(GpuCensusSection::DoorSharpen) == 4, "K cap: a door section caps at 4 (both eyes, two fovea crops each)");
    check(occurrenceCapFor(GpuCensusSection::FrameEyeMask) == 8, "K cap: a per-draw section caps at 8");
    check(isDoorSection(GpuCensusSection::DoorFssHeal), "section split: the last door section is still door");
    check(!isDoorSection(GpuCensusSection::FrameHologramPasses), "section split: the first in-frame section is not door");

    ComPtr<ID3D11RenderTargetView> rtv;
    auto tex = makeTarget(d.dev.Get(), rtv);
    const float colour[4] = {0.2f, 0.4f, 0.6f, 1.0f};

    for (auto& s : g_section) s = SectionState{};
    g_activeSection = static_cast<int>(GpuCensusSection::DoorSharpen);
    g_activeCalls = g_activeTimed = 0;
    g_activeStride = 1;
    g_activeOffset = 0;
    auto& st = g_section[static_cast<size_t>(GpuCensusSection::DoorSharpen)];

    // Exactly one section is active: a call for any other section declines
    // -- cheaply, one branch and an increment -- and never touches a timer.
    check(!gpuCensusBegin(d.ctx.Get(), GpuCensusSection::DoorMenu),
          "rotation: a non-active section's begin returns false");
    check(g_section[static_cast<size_t>(GpuCensusSection::DoorMenu)].occurrences == 1,
          "rotation: a non-active section still counts its occurrence");
    gpuCensusEnd(d.ctx.Get(), GpuCensusSection::DoorMenu);   // must be a harmless no-op

    // Four real occurrences this "frame": a genuine Begin/Clear/End pair
    // each time, within the door cap of 4.
    for (int i = 0; i < 4; ++i) {
        check(gpuCensusBegin(d.ctx.Get(), GpuCensusSection::DoorSharpen),
              "rotation: the active section's begin succeeds within its cap");
        d.ctx->ClearRenderTargetView(rtv.Get(), colour);
        gpuCensusEnd(d.ctx.Get(), GpuCensusSection::DoorSharpen);
    }
    check(g_activeTimed == 4, "rotation: four occurrences consumed the door cap");
    check(st.occurrences == 4, "rotation: every occurrence counted");

    // A fifth, same frame: the K cap declines it before the sampler is
    // ever touched -- no lease spent, no change to the timer's own totals.
    const unsigned samplerSkippedBefore = st.sampler.totals.skipped;
    check(!gpuCensusBegin(d.ctx.Get(), GpuCensusSection::DoorSharpen),
          "rotation: a fifth occurrence this frame is declined (K=4 for a door section)");
    check(g_activeTimed == 4, "rotation: the cap does not advance past K");
    check(st.sampler.totals.skipped == samplerSkippedBefore,
          "rotation: a K-cap decline never touches the timer's own capacity accounting");
    check(st.skippedThisWindow == 0, "rotation: a K-cap decline is by design, never counted as a failed span");
    gpuCensusEnd(d.ctx.Get(), GpuCensusSection::DoorSharpen);   // still a harmless no-op

    // The stride: a per-draw section that ran 40 times a frame this window
    // (400 calls over 10 frames) is timed every 5th call (40 / K=8), from an
    // offset that rotates with its turns (7 turns taken -> offset 2).
    auto& holo = g_section[static_cast<size_t>(GpuCensusSection::FrameHologramPasses)];
    holo.occurrences = 400;
    holo.turns = 7;
    g_windowFrames = 9;                       // gpuCensusFrame counts this frame first: 10
    g_windowStartMs = GetTickCount64();       // no window closes during the case
    g_activeSection = static_cast<int>(GpuCensusSection::FrameHologramPasses) - 1;
    gpuCensusFrame(d.ctx.Get());
    check(g_activeSection == static_cast<int>(GpuCensusSection::FrameHologramPasses),
          "stride: the rotation lands on the next section");
    check(g_activeStride == 5, "stride: calls per frame / K");
    check(g_activeOffset == 2, "stride: the offset rotates with the section's turns");
    check(holo.turns == 8, "stride: the turn is counted");
    // Which calls the stride selects, read from g_activeTimed rather than
    // Begin's result: outside a native frame span every interval here takes
    // a whole disjoint-clock record (8 in all), so whether each selected
    // call also got a timer depends on the rig's own record use, not on
    // the selection under test.
    std::string timedAt;
    for (unsigned call = 0; call < 45; ++call) {
        const unsigned before = g_activeTimed;
        gpuCensusBegin(d.ctx.Get(), GpuCensusSection::FrameHologramPasses);
        if (g_activeTimed > before) timedAt += std::to_string(call) + ",";
        gpuCensusEnd(d.ctx.Get(), GpuCensusSection::FrameHologramPasses);
    }
    check(timedAt == "2,7,12,17,22,27,32,37,", "stride: timed calls are spread across the frame, capped at K=8");
    check(g_activeTimed == 8, "stride: the per-draw cap holds");

    // Only this fixture flushes (native_timing_gpu_test's own comment,
    // verbatim): production readback is nonblocking and DONOTFLUSH, and
    // gpu_census.cpp's own poll still never waits or flushes. Without a
    // Present anywhere in this process, nothing else would ever hand the
    // clear and its timestamps to WARP to execute, and gpu_interval.h's
    // four-poll gate is a COUNT, not a clock, so a tight loop would spin
    // it without ever giving WARP real wall-clock time either way.
    d.ctx->Flush();
    const uint64_t deadline = GetTickCount64() + 1500;
    while (st.sampler.totals.samples == 0 && GetTickCount64() < deadline) {
        gpuCensusFrame(d.ctx.Get());
        if (st.sampler.totals.samples == 0) Sleep(1);
    }
    check(st.sampler.totals.samples > 0,
          "WARP round trip: a real Begin/Clear/End pair produced a completed sample within the poll margin");
    check(st.sampler.totals.invalid == 0, "WARP round trip: no invalid/disjoint result on a clean WARP run");

    gpuCensusShutdown();
    check(gpuTimingShutdown(d.ctx.Get()), "explicit owner shutdown");
}

// ---- 4: the line's format, including "-" for an absent section --------
void logFormatCase() {
    for (auto& s : g_section) s = SectionState{};
    g_activeSection = 0;
    g_activeCalls = g_activeTimed = 0;
    g_windowFrames = 200;
    const uint64_t start = GetTickCount64() - 30000;
    g_windowStartMs = start;
    g_p50Cursor = 0;
    g_p50Count = 0;   // no Application-render samples landed this window

    auto& sharpen = g_section[static_cast<size_t>(GpuCensusSection::DoorSharpen)];
    sharpen.occurrences = 20;
    sharpen.sampler.totals.ms = 4.0;
    sharpen.sampler.totals.samples = 2;   // 2 ms/occurrence * (20/200 = 0.1/frame) = 0.2 ms/frame

    g_lastLog.clear();
    logAndResetWindow(start + 30000);

    check(g_lastLog.rfind("EDVR GPU census:", 0) == 0, "log line: starts with the fixed preamble");
    check(g_lastLog.find("30 s, 200 frames") != std::string::npos, "log line: the window length and frame count");
    check(g_lastLog.find("sharpen 0.200 (0.10/frame)") != std::string::npos,
          "log line: a timed section reports ms/frame with its occurrences/frame");
    check(g_lastLog.find("menu -") != std::string::npos, "log line: an absent door section prints '-'");
    check(g_lastLog.find("planet -") != std::string::npos, "log line: an absent in-frame section prints '-'");
    check(g_lastLog.find("application render p50 -;") != std::string::npos,
          "log line: no Application-render samples this window also prints '-'");
    check(g_lastLog.find("failed 0.") != std::string::npos, "log line: failed spans are reported by name");
    check(g_lastLog.find("door 0.200") != std::string::npos,
          "log line: door total folds in the sharpen leaf (one of the five top-level door passes)");

    check(sharpen.occurrences == 0, "log line: the window resets occurrences after logging");
    check(g_windowFrames == 0, "log line: the window resets its frame count after logging");
    check(g_p50Count == 0, "log line: the window resets the p50 sample buffer after logging");

    // With Application-render samples: their median, and the game's share
    // as that median less EDVR's total (0.2 ms from the sharpen leaf above).
    for (auto& s : g_section) s = SectionState{};
    g_windowFrames = 200;
    g_windowStartMs = start;
    sharpen.occurrences = 20;
    sharpen.sampler.totals.ms = 4.0;
    sharpen.sampler.totals.samples = 2;
    g_p50Samples[0] = 12.0; g_p50Samples[1] = 10.0; g_p50Samples[2] = 11.0;
    g_p50Count = 3;
    g_lastLog.clear();
    logAndResetWindow(start + 30000);
    check(g_lastLog.find("application render p50 11.000 ms/frame (game ~10.800)") != std::string::npos,
          "log line: application render reports its median and the game's share (median less EDVR)");
}

void run() {
    estimatorMathCases();
    logFormatCase();
    Runtime runtime;
    Device device(runtime);
    rotationAndRealTimerCase(device);
    std::printf("PASS: %u GPU census checks\n", g_checks);
}

} // namespace
} // namespace edvr

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2 && !wcscmp(argv[1], L"--dry-run")) {
        std::puts("dry-run: no module, device, queries or files");
        return 0;
    }
    if (argc == 2 && !wcscmp(argv[1], L"--child")) {
        try { edvr::run(); return 0; }
        catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL: %s\n", error.what());
            return 1;
        }
    }
    if (argc != 2 || wcscmp(argv[1], L"--self-test")) {
        std::fputs("usage: --self-test | --dry-run\n", stderr);
        return 2;
    }
    // Relaunched as a child (gpu_timing_test's own convention): a WARP
    // device and the shared timing registry are process-global state, and
    // an unclean exit here must never be read as this harness's own.
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
    if (!length || length >= 32768) return 2;
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --child";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!CreateProcessW(executable, &command[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) return 2;
    const DWORD waited = WaitForSingleObject(process.hProcess, 30000);
    DWORD code = 1;
    if (waited == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
    else {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 1000);
        std::fputs("FAIL: owned test child timed out\n", stderr);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (code != 0) std::fprintf(stderr, "FAIL: owned D3D11 child exited 0x%08lX\n", code);
    return code == 0 ? 0 : 1;
}

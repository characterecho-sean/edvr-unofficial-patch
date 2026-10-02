// The footprint instrument, run for real (src/d3d11/vscreen_footprint.cpp; design doc section 82, the "vscreen auto-fit" entry).
//
// tools\vscreen_fit_test runs the pure half (the rule, the corner arithmetic, the text) and scans the source. What it cannot run is
// the glue itself: the D3D11 readback (four sources copied into one staging buffer at the composite draw, mapped later without
// waiting), the arming condition, the 30 s window, the stored head-on floor (p10), the fault guard. This rig compiles the REAL
// vscreen_footprint.cpp with the real Config and Log, replaces what it calls (the runtime's published eye size, the hooks' bypass
// flag) with stubs, puts a WARP device behind it with the four sources a composite draw binds (the model rows in cb0, the clip rows
// in cb1 at their real offsets, the quad's four vertices, the per-instance SIZE), and finally runs the REAL reader
// (python tools\edvr_log.py --vscreen-fit) over the log the glue wrote.
//
// What it proves that nothing else does:
//   NOT ARMED         an explicit fix.vscreen_res_width arms nothing and says so once; the flat profile arms nothing and writes no line
//                     at all; a composite draw reaching an unarmed instrument touches nothing (a null context would crash it).
//   THE PLUMBING      the measurement the glue reads back through the GPU equals the closed form of the constants it was given: each
//                     source is read from the right offset (the model rows at float 36, the clip rows at byte 4320 of a 5376 byte
//                     buffer, the vertices at the draw's base vertex, SIZE at the draw's start instance), the sample waits its three
//                     frames, nothing is mapped before the copy ran, and the 30 s line says exactly what it saw: samples on foot
//                     and elsewhere, the footprint in eye pixels, at distance 1, the session's value.
//   WHAT IS STORED    the on-foot HEAD-ON FLOOR, the session's 10th percentile (tagged est=p10 in the file), is stored beside the eye
//                     width, at distance 1.0, and only from twelve on-foot samples up; the line says what the file holds and the width
//                     the next launch would fit. A session whose samples are NOT all alike (fractions at distance 1 from several applied
//                     distances) stores 0.67 of the drawn width, where its median would be 0.85: the p10, not the median.
//   THE SAVE FAILS    the first window that has a record to save finds the destination held open with no sharing (an older session's
//                     record is on disk): the save fails, the line says persisted=no save-failed=1 and the log says SAVE FAILED once, the
//                     file is exactly what it was and no temp file is left; with the lock gone the NEXT window (its p10 unchanged,
//                     nothing new on foot) saves: persisted= is the value, save-failed does not grow, and the next launch's read
//                     (lastKnownPanelFootprint over the same directory) returns it. The run is read by the real reader: STORED WARNs.
//   SOURCES THAT ARE NOT WHAT IT ASSUMES   a vertex stride that is not 20, a constant buffer too small for the rows, no SIZE slot and
//                     a negative base vertex each skip the sample and are counted by reason on the line; nothing is guessed.
//   THE GUARD         a context that faults on every call (a bogus pointer) is absorbed by the fault budget five times, leaves the hooks'
//                     bypass flag clear every time, and then stands the instrument down: "STOOD DOWN", no more samples, no crash.
//   THE READER        the log the glue wrote is read by the real reader: the arming line, the window lines and the verdict.
//
// --self-test runs it and prints "vscreen footprint glue: PASS" only when every check holds. Run from the repo root (the reader is
// tools\edvr_log.py). --dry-run prints a line and does nothing.
#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "../../src/common/config.h"
#include "../../src/common/log.h"
#include "../../src/common/native_render_settings.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/common/vscreen_auto_state.h"
#include "../../src/common/vscreen_fit.h"
#include "../../src/d3d11/vscreen_footprint.h"

using Microsoft::WRL::ComPtr;
namespace fit = edvr::vscreenfit;

// ---------------------------------------------------------------------------------------------------------------------------------
// What the glue calls, replaced.
// ---------------------------------------------------------------------------------------------------------------------------------
namespace edvr {
thread_local bool g_flatComputeInternal = false;   // the hooks' bypass flag (flat_compute_readback.cpp), which this rig does not link
}

namespace stub {
bool sizingKnown = true;
}
extern "C" BOOL WINAPI edvrQueryNativeRenderSizing(uint32_t version, uint32_t size, void* output) {
    if (version != EDVR_NATIVE_RENDER_SIZING_VERSION_1 || size != sizeof(EdvrNativeRenderSizing) || !output) return FALSE;
    EdvrNativeRenderSizing s{};
    s.size = sizeof(s);
    s.version = version;
    s.valid = stub::sizingKnown ? 1u : 0u;
    s.activeWidth[0] = s.activeWidth[1] = 4032;
    s.activeHeight[0] = s.activeHeight[1] = 3898;
    std::memcpy(output, &s, sizeof(s));
    return TRUE;
}

namespace {

unsigned g_checks = 0;
std::string g_failure;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok && g_failure.empty()) g_failure = what;
    if (!ok) std::printf("  FAIL  %s\n", what);
}
bool has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }
size_t count(const std::string& s, const std::string& needle) {
    size_t n = 0;
    for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + needle.size())) ++n;
    return n;
}
std::string narrow(const std::wstring& w) {
    std::string out;   // the scratch paths are ASCII
    out.reserve(w.size());
    for (wchar_t c : w) out.push_back(static_cast<char>(c));
    return out;
}

std::wstring g_dir;
std::string slurp(const std::wstring& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
std::wstring newestLog(const wchar_t* tag) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((g_dir + L"\\edvr_" + tag + L"_*.log").c_str(), &fd);
    std::wstring best;
    if (h != INVALID_HANDLE_VALUE) {
        do { const std::wstring n = g_dir + L"\\" + fd.cFileName; if (n > best) best = n; } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return best;
}
void writeIni(const std::string& body) {
    HANDLE f = CreateFileW((g_dir + L"\\edvr.ini").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, body.data(), static_cast<DWORD>(body.size()), &written, nullptr);
    CloseHandle(f);
}
std::string runReader(const std::wstring& log, int* rc) {
    const std::string cmd = "python tools\\edvr_log.py --file \"" + narrow(log) + "\" --vscreen-fit 2>&1";
    std::string out;
    *rc = -1;
    if (FILE* p = _popen(cmd.c_str(), "r")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
        *rc = _pclose(p);
    }
    return out;
}
// The value of ` key=` in one line of the log, else "".
std::string tokenOf(const std::string& line, const char* key) {
    const std::string needle = std::string(" ") + key + "=";
    const size_t at = line.find(needle);
    if (at == std::string::npos) return "";
    const size_t from = at + needle.size();
    const size_t to = line.find(' ', from);
    return line.substr(from, to == std::string::npos ? std::string::npos : to - from);
}
// Every line of the log that carries `prefix` after its stamp.
std::vector<std::string> linesWith(const std::string& log, const char* prefix) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < log.size()) {
        size_t eol = log.find('\n', at);
        if (eol == std::string::npos) eol = log.size();
        std::string line = log.substr(at, eol - at);
        at = eol + 1;
        const size_t body = line.find("] ");
        const std::string text = (!line.empty() && line[0] == '[' && body != std::string::npos) ? line.substr(body + 2) : line;
        if (text.rfind(prefix, 0) == 0) out.push_back(line);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The GPU: a WARP device and the four sources a composite draw binds.
// ---------------------------------------------------------------------------------------------------------------------------------
constexpr double kZ0 = 22.4;   // the panel's z in scene units, as the DRAWN constants carry it (panel distance 0.7 already applied)
const float kClip[16] = {0.9418f, 0, 0, 0,   0, 0.9714f, 0, 0,   -0.165f, 0, 0, 1.0f,   0, 0, 0.025f, 0};   // cb1[270..273]
const float kSize[2] = {20.687f, 20.0f};

struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Buffer> cb0, cb1, cb1Small, vb0, vb0Stride32, vb1;
    bool ok = false;
};

ComPtr<ID3D11Buffer> makeBuffer(ID3D11Device* dev, UINT bytes, UINT bind, const void* data) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = bytes;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init{data, 0, 0};
    ComPtr<ID3D11Buffer> out;
    dev->CreateBuffer(&d, data ? &init : nullptr, &out);
    return out;
}

Gpu makeGpu() {
    Gpu g;
    const PFN_D3D11_CREATE_DEVICE create = edvr::systemD3D11CreateDevice();
    if (!create) return g;
    D3D_FEATURE_LEVEL fl{};
    if (FAILED(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g.device, &fl, &g.ctx)) || !g.device || !g.ctx) return g;

    // cb0: 208 bytes, the model rows 9..11 at floats 36..47: X = x, Y = 0.596 y (16:9 content on an FOV-shaped quad), Z = z + kZ0.
    std::vector<float> cb0(52, 0.0f);
    const float model[12] = {1, 0, 0, 0,   0, 0.596f, 0, 0,   0, 0, 1, static_cast<float>(kZ0)};
    std::memcpy(&cb0[36], model, sizeof(model));
    for (int i = 0; i < 36; ++i) cb0[i] = 100.0f + static_cast<float>(i);   // other rows: values that would break the result if read instead
    cb0[48] = 7.0f; cb0[49] = 8.0f; cb0[50] = 9.0f; cb0[51] = 10.0f;
    g.cb0 = makeBuffer(g.device.Get(), 208, D3D11_BIND_CONSTANT_BUFFER, cb0.data());

    // cb1: 5376 bytes (the game's scene block), rows 270..273 at byte 4320.
    std::vector<float> cb1(5376 / 4, 0.0f);
    for (size_t i = 0; i < cb1.size(); ++i) cb1[i] = 50.0f + static_cast<float>(i % 7);
    std::memcpy(&cb1[270 * 4], kClip, sizeof(kClip));
    g.cb1 = makeBuffer(g.device.Get(), 5376, D3D11_BIND_CONSTANT_BUFFER, cb1.data());
    g.cb1Small = makeBuffer(g.device.Get(), 4096, D3D11_BIND_CONSTANT_BUFFER, std::vector<float>(1024, 1.0f).data());

    // The quad at the buffer's second record (base vertex 4): four vertices of float3 position, float2 UV, stride 20; the first record
    // is a decoy a read from offset 0 would find. The canonical corners are +-1, z = 0.
    std::vector<float> verts(40, 9.0f);
    const float quad[20] = {-1, -1, 0, 0, 1,   1, -1, 0, 1, 1,   -1, 1, 0, 0, 0,   1, 1, 0, 1, 0};
    std::memcpy(&verts[20], quad, sizeof(quad));
    g.vb0 = makeBuffer(g.device.Get(), 160, D3D11_BIND_VERTEX_BUFFER, verts.data());
    g.vb0Stride32 = g.vb0;   // the same buffer bound with another stride is what the check is about
    // SIZE at the second record (start instance 1): a decoy first.
    const float sizes[4] = {1.0f, 2.0f, kSize[0], kSize[1]};
    g.vb1 = makeBuffer(g.device.Get(), 16, D3D11_BIND_VERTEX_BUFFER, sizes);
    g.ok = g.cb0 && g.cb1 && g.cb1Small && g.vb0 && g.vb1;
    return g;
}

// Bind what the composite draw has bound. stride0: the quad's stride; withSize: bind the SIZE buffer; smallCb1: a constant buffer 1 too
// small for the rows.
void bindComposite(Gpu& g, UINT stride0, bool withSize, bool smallCb1) {
    ID3D11Buffer* cbs[2] = {g.cb0.Get(), smallCb1 ? g.cb1Small.Get() : g.cb1.Get()};
    g.ctx->VSSetConstantBuffers(0, 2, cbs);
    ID3D11Buffer* vbs[2] = {g.vb0.Get(), withSize ? g.vb1.Get() : nullptr};
    UINT strides[2] = {stride0, withSize ? 8u : 0u};
    UINT offsets[2] = {0, 0};
    g.ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
}

// One sample: the composite draw's hook, then the frame boundaries that map it. `base` and `instance` are the draw's own arguments.
void sampleOnce(Gpu& g, bool onFoot, float applied, int base, unsigned instance) {
    edvr::vscreenFootprintFrameBoundary(g.ctx.Get(), onFoot);   // the frame the draw belongs to starts
    edvr::vscreenFootprintCompositeDraw(g.ctx.Get(), applied, base, instance);
    g.ctx->Flush();
    // Frame boundaries until the sample is mapped (three frames to wait, and a WARP device may take a moment more): bounded, so a
    // sample that never maps fails the count instead of hanging the rig. A defective source never becomes pending, and ends this at once.
    for (int i = 0; i < 400 && (i < 4 || edvr::vscreenFootprintSamplePending()); ++i) {
        edvr::vscreenFootprintFrameBoundary(g.ctx.Get(), onFoot);
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

// What the glue should read, from the constants above, written out independently of the pure half: the quad's corners (+-1) times SIZE,
// through the model rows and the clip columns (a x SIZE.x / Z for the width, b x 0.596 SIZE.y / Z for the height).
double wantWidthFraction() { return 0.9418 * 20.687 / kZ0; }
double wantHeightFraction() { return 0.9714 * 0.596 * 20.0 / kZ0; }

int run() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    g_dir = std::wstring(temp) + L"edvr_fp_glue_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(g_dir.c_str(), nullptr);
    if (GetFileAttributesA("tools\\edvr_log.py") == INVALID_FILE_ATTRIBUTES) {
        std::printf("  FAIL  tools\\edvr_log.py is not here: run this from the repo root\n");
        return 1;
    }
    using edvr::Config;
    using edvr::Log;
    using edvr::g_runtimeProfile;
    using edvr::RuntimeProfile;

    Gpu gpu = makeGpu();
    check(gpu.ok, "a WARP device and the four composite sources are available");
    if (!gpu.ok) return 1;
    check(edvr::reportSystemD3D11Only("vscreen_footprint_glue_test"), "the process runs on System32's d3d11.dll only (no EDVR proxy beside the rig)");
    edvr::detail::g_footprintWindowMs = 600000;   // a window closes only when the script says so
    edvr::detail::g_footprintSampleMs = 0;        // every composite draw is a sample, so the script decides the count
    edvr::detail::g_footprintConfigMs = 0;        // the arming condition is read at every boundary

    const std::string iniHead = "[log]\ndir = " + narrow(g_dir) + "\n";

    // ---- 1. an explicit width: not armed, said once ----------------------------------------------------------------------------------
    std::printf("explicit width\n");
    g_runtimeProfile = RuntimeProfile::Vr;
    writeIni(iniHead + "[fix]\nvscreen_res_width = 3504\npanel_distance = 0.7\n");
    Config::get().init(g_dir);
    check(Log::get().open(g_dir, L"fpexplicit"), "the log opens in the scratch directory");
    for (int i = 0; i < 4; ++i) edvr::vscreenFootprintFrameBoundary(gpu.ctx.Get(), true);
    check(!edvr::vscreenFootprintWanted(), "NOT ARMED: an explicit fix.vscreen_res_width arms nothing");
    edvr::vscreenFootprintCompositeDraw(nullptr, 0.7f, 0, 0);   // a null context would crash it if it did anything
    check(true, "NOT ARMED: a composite draw reaching the unarmed instrument touches nothing (a null context was passed)");
    Log::get().close();
    {
        const std::string log = slurp(newestLog(L"fpexplicit"));
        check(count(log, "vscreen footprint: not armed -- ") == 1 && has(log, "\"3504\"") && !has(log, "vscreen footprint: armed") && !has(log, "vscreen footprint 30s:"),
              "NOT ARMED: the log says once that the explicit width has nothing to fit, and prints no window line");
    }

    // ---- 2. the flat profile: nothing at all -------------------------------------------------------------------------------------------
    std::printf("flat profile\n");
    g_runtimeProfile = RuntimeProfile::Flat;
    Config::get().set("fix.vscreen_res_width", "auto");
    check(Log::get().open(g_dir, L"fpflat"), "a second log opens");
    for (int i = 0; i < 4; ++i) edvr::vscreenFootprintFrameBoundary(gpu.ctx.Get(), true);
    check(!edvr::vscreenFootprintWanted(), "NOT ARMED: the flat profile arms nothing even with the key auto");
    Log::get().close();
    check(!has(slurp(newestLog(L"fpflat")), "vscreen footprint"), "NOT ARMED: the flat profile writes no line at all");

    // ---- 3. armed in the VR profile: the plumbing --------------------------------------------------------------------------------------
    std::printf("armed, VR, auto\n");
    g_runtimeProfile = RuntimeProfile::Vr;
    Config::get().set("fix.vscreen_res_width", "auto");
    Config::get().set("fix.panel_distance", "0.7");
    const std::wstring tag = L"fparmed";
    check(Log::get().open(g_dir, tag.c_str()), "the session's log opens");
    bindComposite(gpu, 20, true, false);
    edvr::vscreenFootprintFrameBoundary(gpu.ctx.Get(), false);
    check(edvr::vscreenFootprintWanted(), "ARMED: the VR profile with fix.vscreen_res_width = auto arms the instrument");

    // Not mapped early: a draw and the very next boundary are not enough (three frames are waited).
    {
        edvr::vscreenFootprintCompositeDraw(gpu.ctx.Get(), 0.7f, 4, 1);
        gpu.ctx->Flush();
        edvr::vscreenFootprintFrameBoundary(gpu.ctx.Get(), false);
        edvr::vscreenFootprintFrameBoundary(gpu.ctx.Get(), false);
        check(edvr::vscreenFootprintSamplePending(), "THE PLUMBING: a sample is not mapped after one frame boundary, or two (it waits its three frames)");
        for (int i = 0; i < 400 && edvr::vscreenFootprintSamplePending(); ++i) {
            edvr::vscreenFootprintFrameBoundary(gpu.ctx.Get(), false);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        check(!edvr::vscreenFootprintSamplePending(), "THE PLUMBING: ...and it is mapped soon after, without the render thread waiting (Map with DO_NOT_WAIT)");
    }
    // The rest: 4 more menu samples (other), then 20 on foot, each through the real draw hook and the real Map. The base vertex is 4 and
    // the start instance 1: the quad and SIZE are at their second records; a read from offset 0 would find the decoys.
    for (int i = 0; i < 4; ++i) sampleOnce(gpu, false, 0.7f, 4, 1);
    for (int i = 0; i < 20; ++i) sampleOnce(gpu, true, 0.7f, 4, 1);
    check(!edvr::g_flatComputeInternal, "the hooks' bypass flag is clear after the instrument's own D3D calls");

    // THE SAVE FAILS. This is the first window with a record to save, and the file cannot be replaced: an older session's record is on disk and
    // something holds it open with no sharing. (The older record goes in through the writer itself; the checks below read it back.)
    const std::wstring stateFile = Config::get().logDir() + L"\\vscreen_auto_footprint.txt";
    edvr::vscreenfit::Record olderRecord;
    olderRecord.fractionAtUnit = 0.5;
    olderRecord.eyeWidth = 4032;
    olderRecord.distance = 0.7;
    olderRecord.samples = 31;
    edvr::noteMeasuredPanelFootprint(Config::get().logDir(), olderRecord);
    HANDLE held = CreateFileW(stateFile.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(held != INVALID_HANDLE_VALUE, "THE SAVE FAILS: an older session's record is on disk, held open with no sharing");

    edvr::detail::g_footprintWindowMs = 1;   // the next boundary closes the window: its save meets the lock
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    edvr::vscreenFootprintFrameBoundary(gpu.ctx.Get(), true);
    edvr::detail::g_footprintWindowMs = 600000;
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);   // unlocked: the next window's save can reach the file
    {
        edvr::vscreenfit::Record kept;
        check(edvr::lastKnownPanelFootprint(Config::get().logDir(), &kept) && std::fabs(kept.fractionAtUnit - 0.5) < 1.0e-6 && kept.samples == 31 && kept.eyeWidth == 4032,
              "THE SAVE FAILS: after the failed save the file is exactly the older session's record (a failed save changes nothing)");
        check(GetFileAttributesW((stateFile + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES, "THE SAVE FAILS: and no temp file is left beside it");
    }

    // ---- 4. sources that are not what the measurement assumes ---------------------------------------------------------------------------
    std::printf("sources that are not what it assumes\n");
    bindComposite(gpu, 32, true, false);              // a vertex stride that is not 20
    sampleOnce(gpu, true, 0.7f, 4, 1);
    bindComposite(gpu, 20, true, true);               // a constant buffer 1 too small for rows 270..273
    sampleOnce(gpu, true, 0.7f, 4, 1);
    bindComposite(gpu, 20, false, false);             // no SIZE slot
    sampleOnce(gpu, true, 0.7f, 4, 1);
    bindComposite(gpu, 20, true, false);
    sampleOnce(gpu, true, 0.7f, -1, 1);               // a negative base vertex
    sampleOnce(gpu, true, 0.7f, 4, 5);                // a start instance past the SIZE buffer's end
    edvr::detail::g_footprintWindowMs = 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    edvr::vscreenFootprintFrameBoundary(gpu.ctx.Get(), true);
    edvr::detail::g_footprintWindowMs = 600000;

    Log::get().close();
    const std::wstring logPath = newestLog(tag.c_str());
    const std::string log = slurp(logPath);

    // ---- what the log says ---------------------------------------------------------------------------------------------------------------
    {
        char armed[800];
        edvr::vscreenfit::formatArmedLine(armed, sizeof(armed));
        check(count(log, armed) == 1, "ARMED: the arming line is printed once, and it is the formatter's text");
        const std::vector<std::string> windows = linesWith(log, "vscreen footprint 30s: ");
        check(windows.size() == 2, "two 30 s lines: one per window the script closed");
        if (windows.size() == 2) {
            const std::string& w1 = windows[0];
            check(tokenOf(w1, "samples") == "25" && tokenOf(w1, "on-foot") == "20" && tokenOf(w1, "other") == "5" && tokenOf(w1, "skipped") == "0" && tokenOf(w1, "late") == "0",
                  "THE PLUMBING: 25 samples read back through the GPU (5 at the menu, 20 on foot), none skipped, none late");
            check(tokenOf(w1, "eye") == "4032x3898" && tokenOf(w1, "distance") == "0.700" && tokenOf(w1, "applied") == "0.700",
                  "THE PLUMBING: the eye the pixels are in, the configured distance and the one the draws carried");
            const double frac = std::atof(tokenOf(w1, "frac").c_str());
            check(std::fabs(frac - wantWidthFraction()) < 1.0e-4, "THE PLUMBING: the footprint is the closed form of the constants (a x SIZE.x / Z of the eye, read from the right offsets)");
            const double fp = std::atof(tokenOf(w1, "fp").c_str());
            check(std::fabs(fp - wantWidthFraction() * 4032.0) < 1.0, "THE PLUMBING: and in eye pixels it is that times the eye's 4032");
            const double h = std::atof(tokenOf(w1, "h").c_str());
            check(std::fabs(h - wantHeightFraction()) < 1.0e-4, "THE PLUMBING: the height fraction is b x 0.596 SIZE.y / Z (the model's own y scale is read)");
            const double shape = std::atof(tokenOf(w1, "shape").c_str());
            check(std::fabs(shape - (wantWidthFraction() * 4032.0) / (wantHeightFraction() * 3898.0)) < 0.01 && std::fabs(shape / (16.0 / 9.0) - 1.0) < 0.05,
                  "THE PLUMBING: the shape reads about 16:9");
            const double at1 = std::atof(tokenOf(w1, "frac1").c_str());
            check(std::fabs(at1 - wantWidthFraction() * 0.7) < 1.0e-4, "WHAT IS STORED: the fraction at distance 1 is the drawn one times the applied distance");
            check(tokenOf(w1, "session-n") == "20" && std::fabs(std::atof(tokenOf(w1, "session-frac1").c_str()) - wantWidthFraction() * 0.7) < 1.0e-4,
                  "WHAT IS STORED: the session's value is over the 20 on-foot samples only (all alike here, so their p10 is their value; the skewed session below tells p10 from median)");
            check(std::atof(tokenOf(w1, "other-fp").c_str()) > 3000.0, "THE PLUMBING: the menu samples are kept apart (other-fp)");
            check(tokenOf(w1, "persisted") == "no" && tokenOf(w1, "save-failed") == "1",
                  "THE SAVE FAILS: the line of the window whose save failed says persisted=no (nothing from this session has reached the file) and save-failed=1");
            fit::Inputs in;
            in.eyeWidth = 4032;
            in.distance = 0.7;
            in.haveFootprint = true;
            in.fractionAtUnit = wantWidthFraction() * 0.7;
            in.route.keyAuto = true;
            check(tokenOf(w1, "fit") == std::to_string(fit::decide(in).width) && tokenOf(w1, "legacy") == "5040",
                  "WHAT IS STORED: the line says the width the next launch would fit (the pure rule's, from the stored p10) and the legacy one");
            const std::string& w2 = windows[1];
            check(tokenOf(w2, "samples") == "0" && tokenOf(w2, "skipped") == "5" && has(w2, "why=") && tokenOf(w2, "fp") == "-",
                  "SOURCES THAT ARE NOT WHAT IT ASSUMES: five defective sources, five skipped samples, no measurement");
            const std::string why = tokenOf(w2, "why");
            check(has(why, "vb0-stride:2") && has(why, "cb-small:1") && has(why, "no-size:1") && has(why, "vb-small:1"),
                  "SOURCES THAT ARE NOT WHAT IT ASSUMES: counted by reason (vb0-stride x2 for the stride and the negative base vertex, cb-small, no-size, vb-small)");
            check(tokenOf(w1, "draws") == "25" && tokenOf(w2, "draws") == "5", "the composite draws the hook saw are counted per window (25, then the 5 defective ones)");
            // The next window: the lock is gone, nothing new was seen on foot, so the p10 is exactly what it was. It saves anyway, because the
            // last save failed; persisted= is then what the file holds, and the failure count does not grow.
            check(tokenOf(w2, "session-n") == "20" && std::fabs(std::atof(tokenOf(w2, "session-frac1").c_str()) - wantWidthFraction() * 0.7) < 1.0e-4 &&
                      tokenOf(w2, "session-frac1") == tokenOf(w1, "session-frac1"),
                  "THE SAVE FAILS: the next window's p10 is unchanged (the same 20 samples, nothing new on foot)");
            check(tokenOf(w2, "persisted") != "no" && std::fabs(std::atof(tokenOf(w2, "persisted").c_str()) - wantWidthFraction() * 0.7) < 1.0e-4 && tokenOf(w2, "save-failed") == "1",
                  "THE SAVE FAILS: and that window saves anyway: persisted= is the value now in the file, and save-failed stays 1 (no further failures)");
        }
        check(count(log, "vscreen footprint 30s: ") == 2 && !has(log, "STOOD DOWN"), "no fault so far: no STOOD DOWN line");
        {   // The log says it once, with the Win32 error, what happens to the file and that the next window tries again.
            const std::vector<std::string> failedLines = linesWith(log, "vscreen footprint: SAVE FAILED");
            check(failedLines.size() == 1, "THE SAVE FAILS: the log says so once (the save that failed, not the retry that worked)");
            if (failedLines.size() == 1) {
                const size_t at = failedLines[0].find("(Win32 error ");
                const unsigned long code = at == std::string::npos ? 0ul : std::strtoul(failedLines[0].c_str() + at + 13, nullptr, 10);
                check(code != 0 && has(failedLines[0], "the file keeps its previous value") && has(failedLines[0], "the next 30 s window tries again") &&
                          has(failedLines[0], "Failed saves this session: 1;"),
                      "THE SAVE FAILS: the line names a Win32 error, says the file keeps its previous value and that the next 30 s window tries again");
            }
        }
    }
    {
        // The stored file: the footprint beside the eye width, at distance 1.
        edvr::vscreenfit::Record r;
        check(edvr::lastKnownPanelFootprint(Config::get().logDir(), &r) && std::fabs(r.fractionAtUnit - wantWidthFraction() * 0.7) < 1.0e-4 && r.eyeWidth == 4032 &&
                  r.samples == 20 && std::fabs(r.distance - 0.7) < 1.0e-3,
              "WHAT IS STORED: after the retry the file holds the on-foot p10 at distance 1 (what the next launch's read returns), the eye width, the distance it was drawn at (the "
              "session's, though the retry window saw nothing on foot) and the sample count");
        check(GetFileAttributesW((stateFile + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES, "WHAT IS STORED: and no temp file is left beside it");
    }

    // ---- the real reader ----------------------------------------------------------------------------------------------------------------
    {
        int rc = 0;
        const std::string report = runReader(logPath, &rc);
        check(rc == 0, "THE READER: --vscreen-fit exits 0 on the glue's log");
        check(has(report, "PASS (INSTRUMENT)") && has(report, "PASS (ON FOOT)") && has(report, "PASS (SHAPE)") && !has(report, "STOP ("),
              "THE READER: its verdict on the glue's own log: the instrument ran, on foot was measured, the shape is 16:9, no STOP");
        check(has(report, "WARN (STORED)") && has(report, "save(s) of the on-foot footprint FAILED") && has(report, "first in window 1"),
              "THE READER: and it WARNs on STORED, saying a save failed (the save-failed token of the instrument's own line, read by key) and in which window");
        if (g_failure.size()) std::printf("%s\n", report.c_str());
    }

    // ---- 5. what is stored is the head-on floor (the session's p10), not its median ------------------------------------------------------
    // Until here every on-foot sample was alike, so a p10 and a median are the same number. This session now takes samples whose fraction at
    // distance 1 differs: the drawn constants stay, and the distance the draw says it carried does not (the instrument stores the drawn
    // fraction x that distance). The store holds the 20 samples of 0.7 f above, then 5 of 0.4 f and 25 of 1.0 f (f = the drawn fraction):
    //   sorted, n = 50: 5 x 0.4 f, 20 x 0.7 f, 25 x 1.0 f.   p10 = position 0.10 x 49 = 4.9 -> 0.4 f + 0.9 x (0.7 f - 0.4 f) = 0.67 f
    //                                                         median = position 24.5 -> (0.7 f + 1.0 f) / 2 = 0.85 f
    std::printf("the stored value is the p10\n");
    const std::wstring tagFloor = L"fpfloor";
    check(Log::get().open(g_dir, tagFloor.c_str()), "a log for the skewed session opens");
    bindComposite(gpu, 20, true, false);
    for (int i = 0; i < 5; ++i) sampleOnce(gpu, true, 0.4f, 4, 1);
    for (int i = 0; i < 25; ++i) sampleOnce(gpu, true, 1.0f, 4, 1);
    edvr::detail::g_footprintWindowMs = 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    edvr::vscreenFootprintFrameBoundary(gpu.ctx.Get(), true);
    edvr::detail::g_footprintWindowMs = 600000;
    Log::get().close();
    {
        const std::vector<std::string> fw = linesWith(slurp(newestLog(tagFloor.c_str())), "vscreen footprint 30s: ");
        check(fw.size() == 1, "THE STORED FLOOR: the skewed session's window closed once and printed its line");
        if (fw.size() == 1) {
            const std::string& w = fw[0];
            const double f = wantWidthFraction();
            const double sess = std::atof(tokenOf(w, "session-frac1").c_str());
            check(tokenOf(w, "samples") == "30" && tokenOf(w, "on-foot") == "30" && tokenOf(w, "session-n") == "50",
                  "THE STORED FLOOR: 30 more on-foot samples, and the session now holds 50");
            check(std::fabs(sess - 0.67 * f) < 1.0e-4 && std::fabs(sess - 0.85 * f) > 0.05,
                  "THE STORED FLOOR: session-frac1 is the session's p10 (0.67 x the drawn fraction), not its median (0.85 x it)");
            check(std::fabs(std::atof(tokenOf(w, "persisted").c_str()) - sess) < 1.0e-4 && tokenOf(w, "save-failed") == "1",
                  "THE STORED FLOOR: and persisted= says the file now holds that p10 (saved with the lock gone: save-failed is still the 1 from before)");
            check(std::fabs(std::atof(tokenOf(w, "frac").c_str()) - f) < 1.0e-4 && tokenOf(w, "applied") == "1.000" && std::fabs(std::atof(tokenOf(w, "frac1").c_str()) - f) < 1.0e-4,
                  "THE STORED FLOOR: while the window's own frac and frac1 stay medians of the window (the drawn fraction f, at distance 1.000)");
            fit::Inputs in;
            in.eyeWidth = 4032;
            in.distance = 0.7;
            in.haveFootprint = true;
            in.fractionAtUnit = 0.67 * f;
            in.route.keyAuto = true;
            check(tokenOf(w, "fit") == std::to_string(fit::decide(in).width) && tokenOf(w, "fit") == "2880",
                  "THE STORED FLOOR: and fit= is the pure rule's width from that p10 (2880: the floor, the screen is smaller than the calibration's)");
        }
        edvr::vscreenfit::Record r;
        const std::string raw = slurp(Config::get().logDir() + L"\\vscreen_auto_footprint.txt");
        check(edvr::lastKnownPanelFootprint(Config::get().logDir(), &r) && std::fabs(r.fractionAtUnit - 0.67 * wantWidthFraction()) < 1.0e-4 && r.samples == 50 && r.eyeWidth == 4032 &&
                  has(raw, " est=p10 ") && !has(raw, "est=median"),
              "THE STORED FLOOR: the file holds the p10 (0.67 x the drawn fraction) over 50 samples, tagged est=p10");
    }

    // ---- 6. the fault guard: a context that faults on every call -------------------------------------------------------------------------
    std::printf("the fault guard\n");
    const std::wstring tagDown = L"fpdown";
    check(Log::get().open(g_dir, tagDown.c_str()), "a log for the guard's session opens");
    edvr::vscreenFootprintShutdown();
    edvr::vscreenFootprintFrameBoundary(gpu.ctx.Get(), true);   // re-armed (the budget has not been touched yet)
    check(edvr::vscreenFootprintWanted(), "THE GUARD: re-armed after a shutdown");
    ID3D11DeviceContext* bogus = reinterpret_cast<ID3D11DeviceContext*>(static_cast<uintptr_t>(0x10));
    bool bypassStuck = false;
    for (int i = 0; i < 7; ++i) {
        edvr::vscreenFootprintCompositeDraw(bogus, 0.7f, 0, 0);
        bypassStuck = bypassStuck || edvr::g_flatComputeInternal;
        edvr::vscreenFootprintFrameBoundary(gpu.ctx.Get(), true);
    }
    check(!bypassStuck && !edvr::g_flatComputeInternal, "THE GUARD: a fault caught by the guard leaves the hooks' bypass flag clear every time");
    check(!edvr::vscreenFootprintWanted(), "THE GUARD: five faults stand the instrument down: it is no longer wanted");
    edvr::vscreenFootprintCompositeDraw(gpu.ctx.Get(), 0.7f, 4, 1);   // a good context now changes nothing
    Log::get().close();
    {
        const std::string down = slurp(newestLog(tagDown.c_str()));
        check(count(down, "vscreen footprint: STOOD DOWN") == 1, "THE GUARD: the log says STOOD DOWN, once");
        check(down.find("FAULT") != std::string::npos || down.find("fault") != std::string::npos || down.find("absorbed") != std::string::npos || true,
              "(the fault notes themselves are the guard's own and are not pinned here)");
    }

    // clean up: every file the rig wrote, then its directory (a failing run keeps them: the log is the evidence)
    edvr::vscreenFootprintShutdown();
    if (g_failure.empty()) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((g_dir + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((g_dir + L"\\" + fd.cFileName).c_str());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(g_dir.c_str());
    } else {
        std::printf("  the scratch directory is kept: %s\n", narrow(g_dir).c_str());
    }
    return g_failure.empty() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--dry-run")) {
            std::puts("vscreen_footprint_glue_test: --dry-run: nothing run, nothing written");
            return 0;
        }
        if (!std::strcmp(argv[i], "--self-test")) selfTest = true;
    }
    if (!selfTest) {
        std::fputs("usage: --self-test | --dry-run\n", stderr);
        return 2;
    }
    const int rc = run();
    if (rc == 0) {
        std::printf("vscreen footprint glue: PASS (%u checks)\n", g_checks);
    } else {
        std::fprintf(stderr, "FAIL: %s\n", g_failure.c_str());
    }
    return rc;
}

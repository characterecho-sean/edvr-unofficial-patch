// The VR camera census's glue, run for real (src/d3d11/vr_camera_census.cpp; design doc section 82).
//
// tools\vr_camera_census_test runs the pure half and scans the source. What it cannot run is the glue itself: the two
// halves of the observer the refresh detour calls around the game's body, the boundary that rolls a frame and prints, the
// eye draw's GPU readback, and the ORDER of it all. This rig compiles the REAL vr_camera_census.cpp with the real Config
// and Log, replaces what it calls -- the injector, the world route's draw progress, the journal, EDVR's advertised eye
// geometry -- with stubs a script drives, puts a WARP device behind the eye readback, feeds the observer cameras built by
// the game-camera derive model (in memory, so the glue's guarded reads are real reads and "the body" between the observer's
// halves is the model's own derive), and finally runs the REAL reader (python tools\edvr_log.py --camera-census) over the
// log the glue wrote. What the reader says is the verdict: the two eye cameras found by the content join -- the composed
// rows the census logged for a camera equal, within 1e-5, the rows read back from the GPU at the eye's draw -- and told
// from the world's.
//
// What it proves that nothing else does:
//   KEY OFF = NOTHING   five boundaries and an eye draw in the VR profile with the key off, and again in the flat profile
//                       with the key on, allocate nothing on the calling thread (a replaced operator new counts), ask the
//                       injector for nothing, and leave no line in the log.
//   THE HOT PATH        across every refresh call of the session the observer's two halves allocate nothing, write not one
//                       byte of the camera or of the view it is handed (hashed before and after each half), and every pre
//                       half is followed by its post half; the off-thread half allocates nothing either. A control: the
//                       same hashing does see the model's derive change the camera.
//   THE PROTOCOL        the observer is registered before the hook can exist, the owner is named, then the hook is asked
//                       for: S P D F at the first boundary, D F at every later one; the key going off detaches the observer
//                       and closes the gate -- asked as flatCameraVrQuiet(), so while the world route injects (the detour is not
//                       quiet) the gate is left to the route -- and going on again reopens it.
//   SAMPLING            the ship frames (the journal says not on foot) print no sequence and spend no eye readback; the
//                       frame the journal flips at prints no empty sequence; and, STAGE 2, the route's zero-phase warm-up
//                       (vrWorldRouteWorldPhase true with a phase of 0, 0 for four frames, the commander on foot, the tone
//                       drawn, the eye draws finding no b1) is not sampled: no sequence, no eye line. The next three frames,
//                       which carry a phase, print a sequence each (the header naming ITS frame's phase) and eight eye draws
//                       in four frames read back, with an unbound b1 and a too-small b1 named, and a fifth frame reads nothing.
//   STAGE 2 TOKENS      the harness plays the detour: it puts the frame's phase into the world and first-person cameras' bound
//                       pair (the injector's rule) and tells the observer what it decided (willInject, role); the call lines,
//                       the 5 s line's inj-calls and the sequence and eye lines' phase= say it; a key-off census never asks
//                       the route for its phase.
//   THE LOG             every line class present, bounded, at most 400 characters a line, and the 5 s line's counters equal
//                       to what the script drove.
//   THE READER          its own join finds the eye cameras (kind 5, by the content join) and the world camera (kind 3, by
//                       its calls), and its stage 2 verdict, run over the glue's own text, measures the injected phase in the
//                       kind-3 rows and finds no leak in the eyes: STOP for the seven off-thread calls and WARN for the four
//                       failed readbacks the log holds on purpose, PASS on all six lines once those are taken out.
//
// --self-test runs it and prints "vr camera census glue: PASS" only when every check holds. Run from the repo root (the
// reader is tools\edvr_log.py). --dry-run prints a line and does nothing.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "../c2_derive_test/c2_derive_model.h"
#include "../../src/common/config.h"
#include "../../src/common/log.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/d3d11/binding_shadow.h"
#include "../../src/d3d11/depth_probe.h"
#include "../../src/d3d11/flat_camera_inject.h"
#include "../../src/d3d11/journal_watch.h"
#include "../../src/d3d11/temporal_pass.h"
#include "../../src/d3d11/ui_layer.h"
#include "../../src/d3d11/vr_camera_census.h"
#include "../../src/d3d11/vr_camera_census_core.h"
#include "../../src/d3d11/vr_world_route.h"
#include "../../src/d3d11/vr_world_route_math.h"   // the route's REAL 5 s line formatters: the log the reader reads carries the route's own text
#include "../../src/d3d11/vscreen.h"

// ---------------------------------------------------------------------------------------------------------------------
// An allocation counter: every global operator new is counted on the thread that made it, so a section of code can say
// "nothing was allocated here" without the log's flusher or any other thread confusing the count.
// ---------------------------------------------------------------------------------------------------------------------
namespace {
thread_local uint64_t t_allocs = 0;
}
void* operator new(size_t n) {
    ++t_allocs;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t n) {
    ++t_allocs;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new(size_t n, const std::nothrow_t&) noexcept { ++t_allocs; return std::malloc(n ? n : 1); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { ++t_allocs; return std::malloc(n ? n : 1); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

// ---------------------------------------------------------------------------------------------------------------------
// What the glue calls, replaced: each records what it was asked and answers what the script says.
// ---------------------------------------------------------------------------------------------------------------------
namespace stub {
const edvr::FlatCameraObserver* observer = nullptr;
int setObserverCalls = 0, pauseCalls = 0, disarmCalls = 0, observeFrameCalls = 0;
bool paused = false;
bool progressHave = true, progressTone = false;
uint32_t progressDraw = 0;
bool journalActive = true, journalKnown = true, journalOnFoot = true;
bool geometryKnown = true;
uint64_t geometrySequence = 4700;
// The world route's phase for the frame (vrWorldRouteWorldPhase): whether it is jittering and the phase it chose, in render pixels.
bool phaseJittering = false;
float phaseX = 0.0f, phaseY = 0.0f;
int phaseCalls = 0;
// flatCameraVrQuiet: the detour is quiet when this frame asked for neither injection nor observation and no injected camera still waits for its
// flush. True for a census-only process; false while the world route injects.
bool vrQuiet = true;
// THE EPISODES: what the glue's questions about the game's state are answered with. Status.json's GuiFocus; the rows the temporal pass chose for the frame
// (temporalPassChosenRows); the depth views the binding shadow's probe resolves and their sizes; the depth probe's scene pair (which view is which eye); the
// panel's and the eye's sizes (vscreen.h).
bool guiKnown = false;
uint32_t gui = 0;
bool chosenValid = false, chosenBound = false;
float chosen[12] = {};
struct Target { const void* ptr; uint32_t w, h; };
Target targets[8] = {};
size_t targetCount = 0;
const void* eyeDsv[2] = {nullptr, nullptr};
constexpr uint32_t kPanelW = 5040, kPanelH = 2835, kEyeW = 2620, kEyeH = 2533;
int resolveCalls = 0;
// The order the injector was asked things in, one letter each: S set-observer, P pause, D disarm, F observe-frame.
char order[64] = {};
size_t orderN = 0;
void mark(char c) {
    if (orderN + 1 < sizeof(order)) { order[orderN++] = c; order[orderN] = 0; }
}
void clearOrder() { orderN = 0; order[0] = 0; }
}  // namespace stub

namespace edvr {
thread_local bool g_flatComputeInternal = false;   // the hooks' bypass flag (flat_compute_readback.cpp), which this rig does not link

void flatCameraInjectSetObserver(const FlatCameraObserver* o) { stub::observer = o; ++stub::setObserverCalls; stub::mark('S'); }
void flatCameraInjectPause(bool p) { stub::paused = p; ++stub::pauseCalls; stub::mark('P'); }
void flatCameraInjectDisarm() { ++stub::disarmCalls; stub::mark('D'); }
bool flatCameraInjectObserveFrame() { ++stub::observeFrameCalls; stub::mark('F'); return true; }
const char* flatCameraInjectObserveStatus() { return "installed"; }
bool flatCameraVrQuiet() { return stub::vrQuiet; }

bool vrWorldRouteDrawProgress(uint32_t* drawOrdinal, bool* toneSeen, uint64_t* frame) {
    if (!stub::progressHave) return false;
    *drawOrdinal = stub::progressDraw++;
    *toneSeen = stub::progressTone;
    *frame = 1;
    return true;
}

// STAGE 2: the route's phase, as the skeleton states it (false with 0, 0 when the route is not jittering).
bool vrWorldRouteWorldPhase(float* x, float* y) {
    ++stub::phaseCalls;
    if (x) *x = stub::phaseJittering ? stub::phaseX : 0.0f;
    if (y) *y = stub::phaseJittering ? stub::phaseY : 0.0f;
    return stub::phaseJittering;
}

bool journalWatchActive() { return stub::journalActive; }
bool journalOnFootKnown() { return stub::journalActive && stub::journalKnown; }
bool journalOnFoot() { return stub::journalOnFoot; }
bool journalGuiFocus(uint32_t* focus) {
    if (!stub::guiKnown) return false;
    if (focus) *focus = stub::gui;
    return true;
}

// ui_layer.h's two counters, which uiLayerLastFrameNamed() reads: the layer's frame count at its boundary, and the frame a draw last named the 2D screen's source at.
// "The frame that just ended named its source" is NamedAt + 1 == GateFrame, so the script sets both at each boundary.
namespace detail {
uint64_t g_uiLayerGateFrame = 0;
uint64_t g_uiLayerNamedAt = 0;
BindingSlot g_bindingSlots[static_cast<size_t>(BindSlot::Count)];   // the binding shadow's storage (binding_shadow.h reads it inline): the script binds depths and shaders in it
}  // namespace detail
bool bindingResolveProbe(void* view, ResourceInfo* out) {
    ++stub::resolveCalls;
    for (size_t i = 0; i < stub::targetCount; ++i) {
        if (stub::targets[i].ptr != view) continue;
        *out = ResourceInfo{};
        out->isTexture2D = true;
        out->a = stub::targets[i].w;
        out->b = stub::targets[i].h;
        out->resource = view;
        return true;
    }
    return false;
}
bool vScreenPanelSize(uint32_t* w, uint32_t* h) {
    if (w) *w = stub::kPanelW;
    if (h) *h = stub::kPanelH;
    return true;
}
bool vScreenIsEyeSized(uint32_t w, uint32_t h) { return w == stub::kEyeW && h == stub::kEyeH; }
bool depthProbeCurrentSceneEyeOf(ID3D11DepthStencilView* dsv, int* outEye, int* outTargetIndex) {
    if (outEye) *outEye = -1;
    if (outTargetIndex) *outTargetIndex = -1;
    for (int eye = 0; eye < 2; ++eye) {
        if (stub::eyeDsv[eye] && stub::eyeDsv[eye] == static_cast<const void*>(dsv)) { if (outEye) *outEye = eye; return true; }
    }
    return false;
}
bool temporalPassChosenRows(float rows[12], bool* bound) {
    if (!stub::chosenValid) return false;
    std::memcpy(rows, stub::chosen, sizeof(stub::chosen));
    if (bound) *bound = stub::chosenBound;
    return true;
}

// EDVR's advertised eye geometry: the frusta the camera model below was built to match within about 4e-5 (the same
// numbers the reader's fixture uses), and a shift of nothing.
bool nativeTemporalEyeGeometry(uint32_t eye, uint64_t* sequence, float frustum[4], float shift[2]) {
    if (!stub::geometryKnown || eye > 1) return false;
    const float f[2][4] = {{-1.2f, 0.7f, -0.9f, 1.1f}, {-0.7f, 1.2f, -0.9f, 1.1f}};
    if (sequence) *sequence = stub::geometrySequence;
    if (frustum) std::memcpy(frustum, f[eye], sizeof(f[eye]));
    if (shift) { shift[0] = 0.0f; shift[1] = 0.0f; }
    return true;
}
}  // namespace edvr

namespace {

using Microsoft::WRL::ComPtr;
using namespace edvr;

int g_failures = 0;
void check(bool ok, const char* what) {
    if (ok) { std::printf("  ok    %s\n", what); return; }
    ++g_failures;
    std::printf("  FAIL  %s\n", what);
}
unsigned occurrences(const std::string& text, const std::string& needle) {
    unsigned n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}
// The whole line (no newline) that holds the first occurrence of `needle` at or after `from`; empty when there is none.
std::string lineWith(const std::string& text, const std::string& needle, size_t from = 0) {
    const size_t at = text.find(needle, from);
    if (at == std::string::npos) return std::string();
    const size_t prev = text.rfind('\n', at);
    const size_t begin = prev == std::string::npos ? 0 : prev + 1;
    const size_t end = text.find('\n', at);
    return text.substr(begin, (end == std::string::npos ? text.size() : end) - begin);
}
bool has(const std::string& line, const std::string& needle) { return !line.empty() && line.find(needle) != std::string::npos; }
// A log line from its census text on (the log puts a stamp before it).
std::string bodyOf(const std::string& line) {
    const size_t at = line.find("vr camera census");
    std::string body = at == std::string::npos ? std::string() : line.substr(at);
    while (!body.empty() && (body.back() == '\r' || body.back() == '\n')) body.pop_back();
    return body;
}
// The LAST line that holds `needle` (a 5 s window may close earlier than the script expects; the one that matters is the final one).
std::string lastLineWith(const std::string& text, const std::string& needle) {
    const size_t at = text.rfind(needle);
    if (at == std::string::npos) return std::string();
    const size_t prev = text.rfind('\n', at);
    const size_t begin = prev == std::string::npos ? 0 : prev + 1;
    const size_t end = text.find('\n', at);
    return text.substr(begin, (end == std::string::npos ? text.size() : end) - begin);
}
std::string slurp(const std::wstring& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
std::string hexOf(uintptr_t v, bool upper) {
    char b[32];
    std::snprintf(b, sizeof(b), upper ? "0x%llX" : "0x%llx", static_cast<unsigned long long>(v));
    return b;
}
// The unsigned number after `key` (which includes its '='); 0 when it is not there.
unsigned long long valueOf(const std::string& line, const char* key) {
    const size_t at = line.find(key);
    return at == std::string::npos ? 0 : std::strtoull(line.c_str() + at + std::strlen(key), nullptr, 10);
}
// Every census line except the call lines: what a failing run prints.
std::string censusLinesOnly(const std::string& log) {
    std::string out;
    size_t p = 0;
    while (p < log.size()) {
        size_t e = log.find('\n', p);
        if (e == std::string::npos) e = log.size();
        const std::string l = log.substr(p, e - p);
        if (l.find("vr camera census") != std::string::npos && l.find(": call frame=") == std::string::npos) out += l + "\n";
        p = e + 1;
    }
    return out;
}

// ---- a camera in the game's own layout, derived by the decompile-cited model ----
struct Model {
    c2derive::Cam cam;
    bool custom = false;   // a kind-5 camera (an eye's): the game gives it its matrix, the model derives it as a perspective camera
    Model(uint32_t kind, float fov, float aspect, float bx, float by, float nearZ, float vw, float vh) {
        using namespace c2derive;
        makeCamera(cam);   // leaves the view, projection and view-projection dirty, as a camera the game has just set up is
        camU(cam, kCamKind) = kind;
        camF(cam, kCamAngular) = fov;
        camF(cam, 0x260) = aspect;
        camF(cam, kCamBoundX) = bx;
        camF(cam, kCamBoundY) = by;
        camF(cam, kCamNear) = nearZ;
        camF(cam, kCamFar) = 50000.0f;
        camF(cam, kCamViewportW) = vw;
        camF(cam, kCamViewportH) = vh;
    }
    uintptr_t ptr() const { return reinterpret_cast<uintptr_t>(cam.b); }
    uint32_t kind() const { return c2derive::camU(cam, c2derive::kCamKind); }
    void dirty() { c2derive::camU(cam, c2derive::kCamFlags) |= c2derive::kFlagView | c2derive::kFlagProj | c2derive::kFlagVP; }
    void bound(float bx, float by) {
        c2derive::camF(cam, c2derive::kCamBoundX) = bx;
        c2derive::camF(cam, c2derive::kCamBoundY) = by;
        dirty();
    }
    void turn(float yaw) {   // the head turns: the axes move and the camera is dirty again
        float* a = &c2derive::camF(cam, c2derive::kCamAxes);
        const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(-0.12f), sp = std::sin(-0.12f);
        a[0] = cy;      a[1] = 0.0f;  a[2] = -sy;     a[3] = 0.0f;
        a[4] = sy * sp; a[5] = cp;    a[6] = cy * sp; a[7] = 0.0f;
        a[8] = sy * cp; a[9] = -sp;   a[10] = cy * cp;
        dirty();
    }
    // A kind-5 camera, the eye's kind in the real game (flight 1): its struct says 5 and the projection is the game's custom matrix, so
    // the model derives it as the perspective camera it was built from and leaves the kind word at 5.
    void makeKind5() { custom = true; c2derive::camU(cam, c2derive::kCamKind) = 5; }
    void body() {   // what the game's refresh does between the observer's two halves
        if (custom) c2derive::camU(cam, c2derive::kCamKind) = 3;
        c2derive::derive(cam);
        if (custom) c2derive::camU(cam, c2derive::kCamKind) = 5;
    }
    uint64_t hash() const { return fnv1a64(cam.b, sizeof(cam.b)); }
};

// The view (pass) object the refresh's second argument points at, and the view-constant context inside it: memory with a
// pattern, so a write into it by the observer would change its hash.
struct View {
    alignas(16) uint8_t mem[0x400];
    View() { for (size_t i = 0; i < sizeof(mem); ++i) mem[i] = static_cast<uint8_t>(i * 7 + 3); }
    uintptr_t ptr() const { return reinterpret_cast<uintptr_t>(mem); }
    uint64_t hash() const { return fnv1a64(mem, sizeof(mem)); }
};

// The observer's contract, measured over every call of the session.
struct HotPath {
    uint64_t calls = 0, posts = 0, preAsksPost = 0;
    uint64_t allocs = 0;          // operator new, on this thread, inside the two halves
    uint64_t writes = 0;          // calls in which a half changed the camera or the view
    uint64_t bodyChanges = 0;     // control: calls in which the model's derive changed the camera (the hash can see a change)
} g_hot;

uint64_t g_callNo = 0;
uint64_t g_injectedCalls = 0, g_injectedScene = 0, g_injectedFirstPerson = 0;   // calls the harness, playing the detour, told the observer it injected
// One refresh call as the detour makes it: the pre half, the game's body (the model's derive), the post half when the pre
// half asked for it. With no observer the hook's gate is shut and the body runs straight through. `inject` and `role` are what the
// detour decided for the call before the observer hears of it (the route's stage 2): the harness has already put the frame's phase
// into the camera's bound pair at the frame's start (driveFrame), as the detour writes it before the game's body.
void refresh(Model& m, View& v, uint32_t caller, bool toneNow, bool inject = false, uint8_t role = kVrCensusRoleNone) {
    stub::progressTone = toneNow;
    FlatCameraObserveCall call;
    call.camera = m.ptr();
    call.ctx = v.ptr() + 0x100;
    call.p2 = v.ptr();
    call.callerRva = caller;
    call.callNo = ++g_callNo;
    call.kind = m.kind();
    call.kindReadable = true;
    call.window = 0;
    call.willInject = inject;
    call.role = role;
    if (stub::observer && inject) {
        ++g_injectedCalls;
        if (role == 0) ++g_injectedScene;
        if (role == 1) ++g_injectedFirstPerson;
    }
    if (!stub::observer) { m.body(); return; }
    const uint64_t camBefore = m.hash(), viewBefore = v.hash();
    const uint64_t a0 = t_allocs;
    const bool wantPost = stub::observer->pre(call);
    const uint64_t a1 = t_allocs;
    const uint64_t camAfterPre = m.hash();
    bool wrote = camAfterPre != camBefore || v.hash() != viewBefore;
    m.body();
    const uint64_t camAfterBody = m.hash();
    if (camAfterBody != camAfterPre) ++g_hot.bodyChanges;
    const uint64_t a2 = t_allocs;
    if (wantPost) {
        stub::observer->post(call.camera, call.ctx);
        ++g_hot.posts;
        ++g_hot.preAsksPost;
    }
    const uint64_t a3 = t_allocs;
    wrote = wrote || m.hash() != camAfterBody || v.hash() != viewBefore;
    if (wrote) ++g_hot.writes;
    g_hot.allocs += (a1 - a0) + (a3 - a2);
    ++g_hot.calls;
}

// ---- a WARP device and a scene constant buffer of 336 float4s, rows 270..273 being the eye camera's rows ----
struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Buffer> scene, tiny;
    std::vector<float> cpu;   // the scene buffer's contents: every float4 different, so a wrong row offset reads wrong
    bool ok = false;
    Gpu() {
        const auto create = systemD3D11CreateDevice();
        if (!create) return;
        D3D_FEATURE_LEVEL level{};
        if (FAILED(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context))) return;
        cpu.resize(336 * 4);
        for (size_t i = 0; i < cpu.size(); ++i) cpu[i] = 100.0f + static_cast<float>(i) * 0.25f;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 336 * 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init{cpu.data(), 0, 0};
        if (FAILED(device->CreateBuffer(&bd, &init, &scene))) return;
        D3D11_BUFFER_DESC tinyDesc{};   // (not "small": windows.h defines that as a macro)
        tinyDesc.ByteWidth = 64;
        tinyDesc.Usage = D3D11_USAGE_DEFAULT;
        tinyDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const std::vector<float> ones(16, 1.0f);
        D3D11_SUBRESOURCE_DATA init2{ones.data(), 0, 0};
        if (FAILED(device->CreateBuffer(&tinyDesc, &init2, &tiny))) return;
        ok = true;
    }
    // The eye camera's rows into rows 270..273, composed by the DECOMPILE-CITED composer (FUN_140596830's model), not by the
    // census's own function: the census recomputes the same rows from the camera it saw, and the two must agree.
    void setEyeRows(Model& eye) {
        float rows[16];
        c2derive::composeSceneCb(eye.cam, rows);
        std::memcpy(&cpu[270 * 4], rows, sizeof(rows));
        context->UpdateSubresource(scene.Get(), 0, nullptr, cpu.data(), 0, 0);
    }
    void bindScene() { ID3D11Buffer* b = scene.Get(); context->VSSetConstantBuffers(1, 1, &b); }
    void bindTiny() { ID3D11Buffer* b = tiny.Get(); context->VSSetConstantBuffers(1, 1, &b); }
    void unbind() { ID3D11Buffer* b = nullptr; context->VSSetConstantBuffers(1, 1, &b); }
};

std::wstring g_dir;
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
void writeIni(const char* body) {
    HANDLE f = CreateFileW((g_dir + L"\\edvr.ini").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, body, static_cast<DWORD>(std::strlen(body)), &written, nullptr);
    CloseHandle(f);
}
void removeScratch() {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((g_dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((g_dir + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(g_dir.c_str());
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_ACP, 0, w.c_str(), static_cast<int>(w.size()), &out[0], n, nullptr, nullptr);
    return out;
}
// tools\edvr_log.py over one log; the exit status goes to *rc.
std::string runReader(const std::wstring& log, int* rc) {
    const std::string cmd = "python tools\\edvr_log.py --file \"" + narrow(log) + "\" --camera-census 2>&1";
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

// The five cameras of the scene, and the views they are refreshed with. File scope so the frames and the eye draws share them.
Model g_world(3, 1.0122f, 5040.0f / 2835.0f, 0.0f, 0.0f, 0.025f, 5040.0f, 2835.0f);
Model g_weapon(3, 0.8236f, 5040.0f / 2835.0f, 0.0f, 0.0f, 0.0675f, 5040.0f, 2835.0f);
Model g_ui(1, 0.0f, 1.0f, 0.0f, 0.0f, 0.1f, 5040.0f, 2835.0f);
// The eye cameras are kind 5, the game's custom matrix (flight 1: every eye call in 4.63 million was kind 5), built from EXACTLY the
// four tangents EDVR advertises for each eye ({-1.2, 0.7, -0.9, 1.1} and {-0.7, 1.2, -0.9, 1.1}; bound +-0.5/3.8 = +-0.131579,
// fov 2 atan(1)), so an eye that nothing touches leaks about 1e-8.
constexpr float kEyeBound = 0.5f / 3.8f, kEyeFov = 1.5707964f;
Model g_eyeL(3, kEyeFov, 0.95f, kEyeBound, -0.05f, 0.025f, 0.0f, 0.0f);
Model g_eyeR(3, kEyeFov, 0.95f, -kEyeBound, -0.05f, 0.025f, 0.0f, 0.0f);
View g_viewWorld, g_viewWeapon, g_viewUi, g_viewEyeL, g_viewEyeR;
constexpr float kRenderW = 5040.0f, kRenderH = 2835.0f;

// One frame of the game's refresh traffic: the world's passes before the tone, the eye views after it, then the eye
// composite draws. Returns the number of refresh calls made. The route's phase for the frame is the stub's (what the census reads
// at this frame's eye draws too): when the route jitters with a phase that is not zero the harness, playing the detour, puts it into
// the world and first-person cameras' bound pair before the game's body (the injector's own rule, bound += (jx / W, -jy / H)) and
// tells the observer it is injecting those calls (role 0 scene, 1 first-person); a kind-5 eye call is never injected.
int driveFrame(int n, Gpu& gpu) {
    stub::progressDraw = 0;
    const bool inject = stub::phaseJittering && (stub::phaseX != 0.0f || stub::phaseY != 0.0f);
    const float phaseBx = inject ? stub::phaseX / kRenderW : 0.0f, phaseBy = inject ? -stub::phaseY / kRenderH : 0.0f;
    g_world.turn(0.35f + 0.004f * static_cast<float>(n));
    g_weapon.turn(0.35f + 0.004f * static_cast<float>(n));
    g_world.bound(phaseBx, phaseBy);
    g_weapon.bound(phaseBx, phaseBy);
    g_ui.dirty();
    g_eyeL.turn(0.02f * static_cast<float>(n));
    g_eyeR.turn(0.02f * static_cast<float>(n));
    // A remnant of the eye shift moves the eye cameras' bound pair by 3e-7: once, at frame 3, so a 'changed:' line is due for each
    // eye camera (the bound threshold is 1e-7), and small enough that the rows it makes leak 6e-7, under the verdict's 1e-6.
    if (n == 3) {
        g_eyeL.bound(kEyeBound + 3.0e-7f, -0.05f + 3.0e-7f);
        g_eyeR.bound(-kEyeBound + 3.0e-7f, -0.05f + 3.0e-7f);
    }
    int calls = 0;
    const uint32_t callers[3] = {0x594E13, 0x594EAB, 0x594FE1};
    for (int i = 0; i < 36; ++i) { refresh(g_world, g_viewWorld, callers[i % 3], false, inject, 0); ++calls; }
    for (int i = 0; i < 6; ++i) { refresh(g_weapon, g_viewWeapon, 0x594E13, false, inject, 1); ++calls; }
    for (int i = 0; i < 2; ++i) { refresh(g_ui, g_viewUi, 0x58DE73, false); ++calls; }
    for (int i = 0; i < 2; ++i) {
        refresh(g_eyeL, g_viewEyeL, 0x594FE1, true);
        refresh(g_eyeR, g_viewEyeR, 0x594FE1, true);
        calls += 2;
    }
    stub::progressTone = true;   // the tone was drawn before the eye composite
    gpu.setEyeRows(g_eyeL);
    vrCameraCensusEyeDraw(gpu.context.Get(), 0);
    gpu.setEyeRows(g_eyeR);
    vrCameraCensusEyeDraw(gpu.context.Get(), 1);
    return calls;
}

// The numbers of every "vr camera census 5s:" line, summed: a window closes when five seconds have passed, and a slow
// machine may close one early, so the totals are what hold, not any one line.
struct WindowSums {
    unsigned lines = 0;
    unsigned long long calls = 0, posts = 0, offThread = 0, eyeDraws = 0, eyeOnFoot = 0, toneFrames = 0, onFootFrames = 0, injCalls = 0;
    bool kind1 = false, kind3 = false, kind5 = false, hook = false, progress = false, footYes = false;
};
WindowSums sumWindows(const std::string& log) {
    WindowSums s;
    const std::string key = "vr camera census 5s: ";
    for (size_t at = log.find(key); at != std::string::npos; at = log.find(key, at + 1)) {
        const std::string line = lineWith(log, key, at);
        ++s.lines;
        s.calls += valueOf(line, " calls=");
        s.posts += valueOf(line, " posts=");
        s.offThread += valueOf(line, " off-thread=");
        s.toneFrames += valueOf(line, " tone-frames=");
        s.onFootFrames += valueOf(line, " on-foot-frames=");
        s.injCalls += valueOf(line, " inj-calls=");
        const size_t e = line.find(" eye-draws=");
        if (e != std::string::npos) {
            s.eyeDraws += std::strtoull(line.c_str() + e + 11, nullptr, 10);
            const size_t slash = line.find('/', e);
            if (slash != std::string::npos) s.eyeOnFoot += std::strtoull(line.c_str() + slash + 1, nullptr, 10);
        }
        s.kind1 = s.kind1 || has(line, " kinds=1:") || has(line, ",1:");
        s.kind3 = s.kind3 || has(line, " kinds=3:") || has(line, ",3:");
        s.kind5 = s.kind5 || has(line, " kinds=5:") || has(line, ",5:");
        s.hook = s.hook || has(line, " hook=installed");
        s.progress = s.progress || has(line, " progress=yes");
        s.footYes = s.footYes || has(line, " foot=yes");
    }
    return s;
}

// ---- 7. THE EPISODES (design-world-camera-motion-2026-09-30.md section 6, Phase 0) ------------------------------------------------------------
// Two episodes through the real glue, with the key on and the WARP device behind the join's readbacks:
//   - the KEY-ON episode, aboard (the journal says not on foot, GuiFocus 0): frame 31 records ALL its calls (the first-three rule would record none of an aboard frame), and at its
//     draws the join finds the screen's depth and each eye's: signatures per (depth, vertex shader, pixel shader), the depth write read from the bound state, the b1 size, and the
//     rows 270..273 read back for the first signature of each depth and matched to the calls that composed them; the temporal pass's rows (stubbed) are matched to the calls' view axes;
//   - an episode armed by the journal flipping to on foot (frame 41, sampled at 71) while a GuiFocus change and a naming flip arrive after it (both counted and skipped), sampled
//     with the naming held and the screen's depth drawn into.
// And the contract around them: the per-draw hook is null except in a sampled frame, a depth view is resolved once a frame, nothing allocates (the observer's halves, the join's
// draws), the reader reports each episode and the window's counters, runs and detour lines.
int runEpisodes(Gpu& gpu) {
    std::printf("episodes\n");
    const std::wstring tagEp = L"glueep";
    check(Log::get().open(g_dir, tagEp.c_str()), "a third log opens for the episodes");
    const uint64_t allocsAtStart = g_hot.allocs, writesAtStart = g_hot.writes;
    static char shadowDsv, eyeLDsv, eyeRDsv, screenDsv;   // the depth views' identities
    stub::targets[0] = {&shadowDsv, 4096, 4096};
    stub::targets[1] = {&eyeLDsv, stub::kEyeW, stub::kEyeH};
    stub::targets[2] = {&eyeRDsv, stub::kEyeW, stub::kEyeH};
    stub::targets[3] = {&screenDsv, stub::kPanelW, stub::kPanelH};
    stub::targetCount = 4;
    stub::eyeDsv[0] = &eyeLDsv;
    stub::eyeDsv[1] = &eyeRDsv;
    ComPtr<ID3D11DepthStencilState> dssWrite, dssRead;
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    check(SUCCEEDED(gpu.device->CreateDepthStencilState(&dd, &dssWrite)), "a depth-stencil state that writes depth");
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    check(SUCCEEDED(gpu.device->CreateDepthStencilState(&dd, &dssRead)), "a depth-stencil state that does not");

    stub::journalOnFoot = false;      // aboard
    stub::guiKnown = true;
    stub::gui = 0;
    stub::phaseJittering = false;
    uint32_t drawNo = 0;
    uint64_t joinAllocs = 0, joinDraws = 0;
    int hookNonNullFrames = 0;
    std::vector<int> liveFrames;
    auto bindDepth = [&](const void* dsv, uint64_t vs, uint64_t ps, ID3D11DepthStencilState* dss) {
        detail::g_bindingSlots[static_cast<size_t>(BindSlot::Dsv0)].ptr = const_cast<void*>(dsv);
        detail::g_bindingSlots[static_cast<size_t>(BindSlot::Vs)].hash = vs;
        detail::g_bindingSlots[static_cast<size_t>(BindSlot::Ps)].hash = ps;
        gpu.context->OMSetDepthStencilState(dss, 0);
    };
    auto draws = [&](int n) {   // what the route's per-draw hook does: one call through the pointer, when it is set
        for (int i = 0; i < n; ++i) {
            ++drawNo;
            if (!detail::g_vrCensusJoinDraw) continue;
            const uint64_t a0 = t_allocs;
            detail::g_vrCensusJoinDraw(gpu.context.Get(), drawNo);
            joinAllocs += t_allocs - a0;
            ++joinDraws;
        }
    };
    const uint32_t callers[3] = {0x594E13, 0x594EAB, 0x594FE1};
    auto frame = [&](int k, bool named) {
        stub::progressDraw = 0;
        drawNo = 0;
        g_world.turn(0.35f + 0.004f * static_cast<float>(k));
        g_eyeL.turn(0.02f * static_cast<float>(k));
        g_eyeR.turn(0.02f * static_cast<float>(k));
        for (int i = 0; i < 2; ++i) refresh(g_ui, g_viewUi, 0x58DE73, false);
        for (int i = 0; i < 4; ++i) refresh(g_world, g_viewWorld, callers[i % 3], false);
        for (int i = 0; i < 2; ++i) { refresh(g_eyeL, g_viewEyeL, 0x594FE1, true); refresh(g_eyeR, g_viewEyeR, 0x594FE1, true); }
        // The pass chose the left eye's view axes this frame (the stub), from the block bound at the first scene draw.
        stub::chosenValid = true;
        stub::chosenBound = true;
        std::memcpy(stub::chosen, &c2derive::camF(g_eyeL.cam, c2derive::kCamAxes), sizeof(stub::chosen));
        if (detail::g_vrCensusJoinDraw) { ++hookNonNullFrames; liveFrames.push_back(k); }
        gpu.bindScene();
        bindDepth(&shadowDsv, 0x01, 0x02, dssWrite.Get());   draws(20);   // a shadow map: neither the screen's depth nor an eye's
        gpu.setEyeRows(g_eyeL);
        bindDepth(&eyeLDsv, 0xA1, 0xB1, dssWrite.Get());     draws(30);
        bindDepth(&eyeLDsv, 0xC1, 0xB1, dssRead.Get());      draws(5);    // a second signature into the left eye's depth: no readback
        gpu.setEyeRows(g_eyeR);
        bindDepth(&eyeRDsv, 0xA1, 0xB1, dssWrite.Get());     draws(30);
        gpu.setEyeRows(g_world);
        bindDepth(&screenDsv, 0xD1, 0xE1, dssWrite.Get());   draws(10);   // the 2D screen's depth, drawn through the world camera's block
        detail::g_uiLayerGateFrame = static_cast<uint64_t>(k) + 1;        // the layer's boundary counted this frame; did a draw name the screen's source in it
        detail::g_uiLayerNamedAt = named ? static_cast<uint64_t>(k) : 0;
        vrCameraCensusFrameBoundary();
    };
    check(detail::g_vrCensusJoinDraw == nullptr && vrCameraCensusWanted(), "before any episode's frame the per-draw hook is null (and the census is on)");
    // The census was turned on last by section 4 (frame 1 started there, the key-on trigger armed an episode for frame 31): frames 1..31 are aboard.
    for (int k = 1; k <= 39; ++k) frame(k, false);
    check(liveFrames.size() == 1 && liveFrames[0] == 31 && detail::g_vrCensusJoinDraw == nullptr,
          "the per-draw hook was set for exactly one frame (the 31st, thirty frames after the key went on) and is null again after its boundary");
    stub::journalOnFoot = true;       // the journal flips to on foot: read at the boundary that ends frame 40, an episode is armed for frame 71 ...
    stub::gui = 6;                    // ... and a GuiFocus change at that boundary arrives while it is armed: counted, skipped
    for (int k = 40; k <= 49; ++k) frame(k, false);
    for (int k = 50; k <= 76; ++k) frame(k, true);   // the naming flips and holds from frame 50: a third trigger, skipped as well
    check(liveFrames.size() == 2 && liveFrames[1] == 71 && detail::g_vrCensusJoinDraw == nullptr && hookNonNullFrames == 2,
          "the second episode's frame is the 71st (thirty frames after the journal's flip was read), and the hook is null at every other frame");
    check(stub::resolveCalls == 8, "each depth view was resolved once a sampled frame (four a frame, two frames): the cache answers every later draw");
    check(joinDraws == 2 * (20 + 30 + 5 + 30 + 10) && joinAllocs == 0, "the join's hook was called for every draw of the two frames (190) and allocated nothing");
    check(g_hot.allocs == allocsAtStart && g_hot.writes == writesAtStart,
          "HOT PATH: across the episodes' frames (every call of them recorded into the episode buffer) the observer's halves allocated nothing and wrote not a byte of any camera or view");
    Sleep(5200);
    vrCameraCensusFrameBoundary();    // the 5 s window is due: its line and its three companions

    // KEY OFF with an episode armed is dropped: arm one (a GuiFocus change), then turn the census off before its frame.
    stub::gui = 0;
    frame(77, true);
    Config::get().set("advanced.vr_camera_census", "off");
    vrCameraCensusFrameBoundary();
    check(!vrCameraCensusWanted() && detail::g_vrCensusJoinDraw == nullptr,
          "the key going off while an episode is armed leaves the per-draw hook null (nothing runs for the dropped episode)");
    Log::get().close();

    // ---- the log ----
    const std::wstring epPath = newestLog(tagEp.c_str());
    const std::string log = slurp(epPath);
    check(!log.empty(), "the glue wrote the episodes' log");
    const std::string head3 = bodyOf(lineWith(log, "vr camera census: episode frame=31 "));
    check(head3 == "vr camera census: episode frame=31 n=3/10 trigger=key-on armed=1 foot=no gui=0 named=0 phase=- calls=10 recorded=10 printed=10 kinds=1:2,3:4,5:4 "
                   "callers=+0x594FE1:5,+0x58DE73:2,+0x594E13:2,+0x594EAB:1",
          "EPISODE 3 (the key-on one, aboard): frame 31, armed at frame 1, the journal says not on foot, GuiFocus 0, all 10 calls recorded and tallied by kind and caller");
    if (head3.empty()) std::printf("  note  %s\n", censusLinesOnly(log).c_str());
    const std::string head4 = bodyOf(lineWith(log, "vr camera census: episode frame=71 "));
    check(head4 == "vr camera census: episode frame=71 n=4/10 trigger=foot:no>yes armed=41 foot=yes gui=6 named=1 phase=- calls=10 recorded=10 printed=10 kinds=1:2,3:4,5:4 "
                   "callers=+0x594FE1:5,+0x58DE73:2,+0x594E13:2,+0x594EAB:1",
          "EPISODE 4: armed by the journal's flip (foot:no>yes) at frame 41, sampled at 71 with the journal on foot, GuiFocus 6 and the naming held");
    check(occurrences(log, "vr camera census: episode frame=") == 2 && occurrences(log, "vr camera census: sequence frame=") == 0,
          "exactly two episodes printed, and no first-three sequence (all three were printed before): the episodes' frames are not spent twice");
    // The calls of episode 3: all ten, in order, kind 1, 3 and 5, the eyes' two kind-5 calls each aboard.
    {
        unsigned calls3 = 0, calls4 = 0;
        for (size_t at = log.find(": call frame=31 n="); at != std::string::npos; at = log.find(": call frame=31 n=", at + 1)) ++calls3;
        for (size_t at = log.find(": call frame=71 n="); at != std::string::npos; at = log.find(": call frame=71 n=", at + 1)) ++calls4;
        const std::string eyeCall = lineWith(log, "call frame=31 n=7 camera=" + hexOf(g_eyeL.ptr(), false) + " kind=5 ");
        check(calls3 == 10 && calls4 == 10 && has(eyeCall, "tone=after") && has(eyeCall, "view=" + hexOf(g_viewEyeL.ptr(), false)) && has(eyeCall, "rows=["),
              "each episode printed all ten calls, and an eye's kind-5 call (n=7: the first of the left eye's two) names its view and carries its composed rows");
    }
    const std::string b1 = hexOf(reinterpret_cast<uintptr_t>(gpu.scene.Get()), false);
    {
        // The join of episode 3: four signatures in the order their first draws came (the shadow map's draws joined nothing).
        const std::string s1 = lineWith(log, "vr camera census: join ep=3 sig=1 "), s2 = lineWith(log, "vr camera census: join ep=3 sig=2 "),
                          s3 = lineWith(log, "vr camera census: join ep=3 sig=3 "), s4 = lineWith(log, "vr camera census: join ep=3 sig=4 ");
        check(has(s1, "depth=eye eye=0 size=2620x2533 draw=21 draws=30 vs=0xA1 ps=0xB1 dw=yes b1=" + b1 + " first=0 bytes=5376 rows=read match=7,9") &&
                  has(s2, "depth=eye eye=0 size=2620x2533 draw=51 draws=5 vs=0xC1 ps=0xB1 dw=no b1=" + b1 + " first=0 bytes=5376 rows=- why=skip") &&
                  has(s3, "depth=eye eye=1 size=2620x2533 draw=56 draws=30 vs=0xA1 ps=0xB1 dw=yes b1=" + b1 + " first=0 bytes=5376 rows=read match=8,10") &&
                  has(s4, "depth=screen eye=- size=5040x2835 draw=86 draws=10 vs=0xD1 ps=0xE1 dw=yes b1=" + b1 + " first=0 bytes=5376 rows=read match=3,4,5,6"),
              "EPISODE 3's join: the left eye's depth (draws 21-50, writes depth, rows matched to the left eye's kind-5 calls 7 and 9), a second shader pair into it (5 draws, no depth "
              "write, no readback), the right eye's (matched to 8 and 10) and the screen's depth (matched to the world camera's four calls): the rows were read from the GPU and equal what "
              "each camera's calls composed");
        if (s1.empty() || s4.empty()) std::printf("  note  %s\n", censusLinesOnly(log).c_str());
        check(occurrences(log, "vr camera census: join-rows ep=3 sig=") == 3 && occurrences(log, "vr camera census: join-rows ep=4 sig=") == 3 &&
                  occurrences(log, "vr camera census: join ep=3 ") == 4 && occurrences(log, "vr camera census: join ep=4 ") == 4,
              "a rows line for each signature that read them (three of four), a join line for each of the four, in both episodes: at most four readbacks an episode");
        check(occurrences(log, "vr camera census: join-draws ep=3 seen=95 relevant=75 views=4 signatures=4") == 1 && occurrences(log, "vr camera census: join-draws ep=4 seen=95 relevant=75 views=4 signatures=4") == 1,
              "each episode says what its per-draw hook was handed: 95 draws (the 20 into the shadow map included), 75 of them into the screen's or an eye's depth, four views resolved, four signatures kept");
        const std::string p3 = lineWith(log, "vr camera census: pass-rows ep=3 ");
        check(has(p3, "frame=31 valid=1 bound=1 rows=[") && has(p3, " axes-match=7,8,9,10 how=identity nearest=7 diff=0.000e+00"),
              "EPISODE 3's pass rows: the left eye's view axes, which equal those of both eyes' four kind-5 calls (the eyes turn together): 7, 8, 9 and 10, read as they are");
    }
    check(occurrences(log, "vr camera census: episodes windows=1 taken=4/10 triggers=7 skipped=3 state=idle") == 1,
          "the window's counters line: 4 episodes taken, 7 triggers (the key going on three times in the session, the journal's flip, GuiFocus, the naming), 3 skipped, nothing armed now");
    {
        const std::string runs = lastLineWith(log, "vr camera census: runs windows=1 ");
        check(has(runs, " unnamed=1:0,2:0,3:0,4-8:0,9-30:1,31-89:0,90+:0 ") && has(runs, " open=named:"),
              "the naming runs: on foot from frame 40, ten unnamed frames (a run in the 9-30 bin) and then named, the named run still open when the window printed");
        const std::string detour = lastLineWith(log, "vr camera census: detour windows=1 ");
        check(has(detour, " every=16 timed=observer-halves ") && !has(detour, " sampled=0 ") && !has(detour, " est-ms-frame=- ") && has(detour, " inj-calls=0 ") &&
                  !has(detour, " obs-pre-us=- ") && !has(detour, " obs-post-us=- "),
              "the detour's CPU line: observer halves timed on one call in sixteen, both halves measured (neither says a dash), an estimate made, no injected call in this scenario");
    }
    check(occurrences(log, "vr camera census: off (advanced.vr_camera_census)") == 1, "the key-off said so once");

    // ---- the real reader over the episodes' log ----
    int rc = -1;
    const std::string report = runReader(epPath, &rc);
    check(rc == 0 && report.find("== episodes (2 printed; the last `episodes` line: 4 taken of 10, 7 trigger(s), 3 skipped) ==") != std::string::npos,
          "tools\\edvr_log.py --camera-census over the episodes' log: two episodes printed, four taken, seven triggers, three skipped");
    {
        const size_t at = report.find("== episodes (");
        const std::string eps = at == std::string::npos ? std::string() : report.substr(at);
        const std::string eyeL = hexOf(g_eyeL.ptr(), true), eyeR = hexOf(g_eyeR.ptr(), true), world = hexOf(g_world.ptr(), true);
        check(has(eps, "episode 3/10: frame 31, trigger key-on (armed at frame 1); journal foot=no, GuiFocus 0, naming unnamed") &&
                  has(eps, "episode 4/10: frame 71, trigger foot no>yes (armed at frame 41); journal foot=yes, GuiFocus 6, naming NAMED"),
              "the reader names each episode: its trigger, the frame it was armed at, the journal, GuiFocus and the naming");
        check(has(eps, "10 call(s), 10 recorded, 10 printed; by kind: k1 x2, k3 x4, k5 x4;") &&
                  has(eps, "join: 4 signature(s); the per-draw hook was handed 95 draw(s), 75 of them into a screen- or eye-sized depth, 4 depth view(s) resolved"),
              "...its calls by kind and caller, and its four join signatures with the draws the hook was handed");
        check(has(eps, "eye 0 2620x2533: first draw 21 (30 draw(s)), vs 0xA1 ps 0xB1, depth write yes, b1 " + hexOf(reinterpret_cast<uintptr_t>(gpu.scene.Get()), true)) &&
                  has(eps, "rows 270..273 equal the composed rows of: camera " + eyeL + " (kind 5) caller +0x594FE1, 2 call(s) n=7/9, view " + hexOf(g_viewEyeL.ptr(), true) + ", tone after") &&
                  has(eps, "rows 270..273 equal the composed rows of: camera " + eyeR + " (kind 5) caller +0x594FE1, 2 call(s) n=8/10") &&
                  has(eps, "screen 5040x2835: first draw 86 (10 draw(s)), vs 0xD1 ps 0xE1, depth write yes") &&
                  has(eps, "rows 270..273 equal the composed rows of: camera " + world + " (kind 3)") && has(eps, "depth write no") && has(eps, "rows not read (skip)"),
              "the reader joins each depth's first draw to the camera whose calls composed its rows: the left eye's depth to the left eye's kind-5 calls, the right eye's to the right's, the screen's to "
              "the world camera's kind-3 calls; the second signature has no rows and says why");
        check(has(eps, "the pass's chosen rows (bound block yes) equal the view axes (as they are) of: camera " + eyeL + " (kind 5)") && has(eps, "-> the rows are an eye camera's (kind 5)"),
              "the pass's rows are reported as the eye cameras' view axes (kind 5), read as they are");
        check(has(eps, "H3 (the cockpit's maps are driven by kind-5 eye cameras' rows): 1 aboard episode(s)") &&
                  has(eps, "episode 3 (key-on, GuiFocus 0): eye 0 depth rows equal kind 5 call(s); eye 1 depth rows equal kind 5 call(s); the pass's rows equal kind 5 call(s)' axes"),
              "H3's facts: the aboard episode's eye depths' rows and the pass's rows are kind-5 calls'");
        check(has(eps, "== on-foot naming runs") && has(eps, "H2 (the longest unnamed run in an on-foot world stays under 3 frames): 1 unnamed run(s) of 3 frames or more, and 0 of 1 or 2") &&
                  has(eps, "== the detour's CPU (the observer's two halves, 1 call in 16 timed;"),
              "the naming runs (H2: the ten unnamed frames are one run of three or more) and the detour's CPU sections follow");
    }
    return g_failures;
}

int run() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    g_dir = std::wstring(temp) + L"edvr_census_glue_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(g_dir.c_str(), nullptr);
    if (GetFileAttributesA("tools\\edvr_log.py") == INVALID_FILE_ATTRIBUTES) {
        std::printf("  FAIL  tools\\edvr_log.py is not here: run this from the repo root\n");
        return 1;
    }
    g_runtimeProfile = RuntimeProfile::Vr;
    writeIni("[advanced]\nvr_camera_census = off\n");
    Config::get().init(g_dir);
    g_eyeL.makeKind5();   // the eye cameras are the game's kind 5
    g_eyeR.makeKind5();

    Gpu gpu;
    check(gpu.ok, "a WARP device and two constant buffers are available for the eye readback");
    if (!gpu.ok) return 1;

    // ---- 1. KEY OFF: nothing is asked of anything, nothing is allocated, nothing is written -------------------------------
    std::printf("key off\n");
    const std::wstring tagOff = L"glueoff";
    check(Log::get().open(g_dir, tagOff.c_str()), "the log opens in the scratch directory");
    check(!vrCameraCensusWanted(), "before any boundary the census is not wanted");
    {
        const uint64_t before = t_allocs;
        for (int i = 0; i < 5; ++i) vrCameraCensusFrameBoundary();
        vrCameraCensusEyeDraw(nullptr, 0);   // a null context would crash it if it did anything
        check(t_allocs == before, "KEY OFF (VR profile): five boundaries and an eye draw allocate nothing");
    }
    check(!vrCameraCensusWanted() && stub::setObserverCalls == 0 && stub::pauseCalls == 0 && stub::disarmCalls == 0 && stub::observeFrameCalls == 0,
          "KEY OFF: ...and ask the injector for nothing: no observer, no owner, no hook, no pause");
    check(detail::g_vrCensusJoinDraw == nullptr && stub::resolveCalls == 0,
          "KEY OFF: ...and the episodes' per-draw hook is null (the route's draw costs one load of a null pointer) and no depth view was ever resolved");
    g_runtimeProfile = RuntimeProfile::Flat;
    Config::get().set("advanced.vr_camera_census", "on");
    {
        const uint64_t before = t_allocs;
        for (int i = 0; i < 5; ++i) vrCameraCensusFrameBoundary();
        vrCameraCensusEyeDraw(nullptr, 1);
        check(t_allocs == before, "KEY ON IN THE FLAT PROFILE: the same five boundaries allocate nothing");
    }
    check(!vrCameraCensusWanted() && stub::setObserverCalls == 0 && stub::disarmCalls == 0 && stub::observeFrameCalls == 0,
          "KEY ON IN THE FLAT PROFILE: ...and ask the injector for nothing (the flat profile never runs the census)");
    check(stub::phaseCalls == 0, "KEY OFF (both cases): the census never asked the world route for its phase");
    Log::get().close();
    {
        const std::string quiet = slurp(newestLog(tagOff.c_str()));
        check(!quiet.empty() && quiet.find("vr camera census") == std::string::npos,
              "KEY OFF and the flat profile: the log was written (it has its version line) and holds no census line");
    }
    g_runtimeProfile = RuntimeProfile::Vr;   // the key is on in memory; the first VR boundary below activates

    // ---- 2. KEY ON (VR): activation, the ship, the journal's flip, the eye draws ---------------------------------------------
    std::printf("key on, a ship\n");
    const std::wstring tagOn = L"glueon";
    check(Log::get().open(g_dir, tagOn.c_str()), "a second log opens for the session");
    stub::journalOnFoot = false;   // in a ship: Flags2 is read and OnFoot is clear
    const uint64_t viewHashes[5] = {g_viewWorld.hash(), g_viewWeapon.hash(), g_viewUi.hash(), g_viewEyeL.hash(), g_viewEyeR.hash()};
    // The world route's choice for each frame. In a ship (frames 1-3) it is not jittering. On foot it jitters from frame 4: a WARM-UP with
    // a ZERO phase (frames 4-7: what flight 1's whole sample was spent on), then a phase of about 1e-4 NDC that moves every frame (frames
    // 8-12). The census samples frames 8-12 only. The script sets the stub for frame f before the frame runs, and for frame f + 1 just
    // before the census's boundary that closes frame f (the route's boundary runs first).
    struct Choice { bool jittering; float x, y; };
    static const float kPhaseX[5] = {0.2520f, -0.1890f, 0.1260f, -0.2520f, 0.0630f}, kPhaseY[5] = {-0.1260f, 0.0630f, 0.2520f, -0.0630f, 0.1890f};
    auto choice = [](int f) -> Choice {
        if (f <= 3) return {false, 0.0f, 0.0f};
        if (f <= 7 || f >= 13) return {true, 0.0f, 0.0f};   // the warm-up, and the route warming again after the last frame (the boundaries of section 3)
        return {true, kPhaseX[f - 8], kPhaseY[f - 8]};
    };
    auto route = [&](int f) { const Choice c = choice(f); stub::phaseJittering = c.jittering; stub::phaseX = c.x; stub::phaseY = c.y; };
    route(1);
    stub::clearOrder();
    const uint64_t allocsBeforeOn = t_allocs;
    vrCameraCensusFrameBoundary();
    check(t_allocs > allocsBeforeOn, "control: the allocation counter sees the census's one table being made at activation");
    check(vrCameraCensusWanted() && stub::setObserverCalls == 1 && stub::disarmCalls == 1 && stub::observeFrameCalls == 1 &&
          stub::pauseCalls == 1 && !stub::paused && stub::observer != nullptr,
          "KEY ON: the first boundary registers the observer, names this thread the owner, asks for the hook, and opens the gate");
    check(std::strcmp(stub::order, "SPDF") == 0,
          "...in that order: observer registered and gate opened BEFORE the owner is named and the hook asked for (S P D F)");

    uint64_t driven = 0;
    int perFrame = 0;
    unsigned toneFrames = 0;
    gpu.bindScene();
    // One frame: the route has set its choice for the frame (route(f)); the game draws it; then the route chooses the next frame's and
    // the census's boundary closes this one.
    auto frame = [&](int f) {
        stub::clearOrder();
        route(f);
        perFrame = driveFrame(f, gpu);
        driven += static_cast<uint64_t>(perFrame);
        ++toneFrames;
        route(f + 1);
        vrCameraCensusFrameBoundary();
    };
    for (int f = 1; f <= 3; ++f) {   // three frames in a ship, each with the tone and its eye draws on a real b1
        frame(f);
        if (f == 1) check(std::strcmp(stub::order, "DF") == 0, "every later boundary is D F: the owner named, the window opened, nothing registered again");
    }
    // The journal flips to on foot now: the boundary that follows frame 4 is the first to read it, so frame 4 was recorded
    // under the old word (nothing) and is the flip frame.
    stub::journalOnFoot = true;
    frame(4);
    // Frames 5-7 are the route's warm-up (it jitters, the phase is zero): the journal says on foot and the tone is drawn, but nothing can
    // leak, so they are not sampled. Their eye draws find NO b1 bound: a census that sampled them would log a 'why=no-b1' line for each.
    gpu.unbind();
    for (int f = 5; f <= 7; ++f) frame(f);
    frame(8);         // the first frame with a phase: sampled, and its eye draws find no b1 bound
    gpu.bindTiny();   // frame 9: a b1 too small to hold rows 270..273
    frame(9);
    gpu.bindScene();  // frames 10 and 11: the real scene buffer; frame 12 is past the eight-draw budget
    for (int f = 10; f <= 12; ++f) frame(f);

    std::printf("the observer's contract over %llu refresh calls\n", static_cast<unsigned long long>(g_hot.calls));
    check(g_hot.calls == driven && driven == static_cast<uint64_t>(perFrame) * 12u && perFrame == 48, "the script drove 12 frames of 48 refresh calls through the observer");
    check(g_hot.allocs == 0, "HOT PATH: across every call the observer's two halves allocated nothing");
    check(g_hot.writes == 0, "ZERO MUTATION: across every call neither half changed a byte of the camera or of the view it was handed");
    check(g_hot.bodyChanges >= 12 * 5, "...and that hashing does see a change: the model's derive changed the camera on each camera's first call of every frame");
    check(g_injectedCalls == 210 && g_injectedScene == 180 && g_injectedFirstPerson == 30,
          "the script injected 210 calls the observer heard, 36 scene and 6 first-person a frame in the five frames with a phase, none in the warm-up");
    check(stub::phaseCalls == 1 + 12 + 24,
          "the census asked the route for its phase once at the activation boundary, once at each of the 12 frame boundaries, and at each of the 24 eye draws "
          "(before the sampling decision uses it, sampled or not)");
    // The world route's two 5 s lines, written by the route's OWN formatters (vrWorldFormatWindow and vrWorldFormatInjectWindow, through
    // the real Log, from what the script drove): the reader's verdict needs the render size (hdr=) and the route's own counts, and a
    // change to either line breaks this rig until the reader parses the new text.
    {
        VrWorldWindow rw;
        rw.hdr.frames = 12; rw.gateFrames = 12; rw.gateFlips = 1; rw.hdr.hdrFrames = 12; rw.hdr.triggerFrames = 12;
        rw.hdr.treated = 12; rw.ownedFrames = 5; rw.takes = 10; rw.layerOnly = 10; rw.enters = 1;
        rw.hdr.lastVerdict = "treated"; rw.jitter = "on"; rw.phaseX = 0.0630f; rw.phaseY = 0.1890f; rw.rowsX = -0.2520f; rw.rowsY = -0.0630f;
        rw.foldMode[1] = 12;
        rw.hdr.lastTriggerVs = 0xDFED8E1C9E191BECull; rw.hdr.lastTriggerPs = 0x143AAE0597E2F7BFull;
        rw.hdr.lastTargetWidth = 2520; rw.hdr.lastTargetHeight = 1417; rw.hdr.lastHdrWidth = 5040; rw.hdr.lastHdrHeight = 2835;
        for (int i = 0; i < 12; ++i) rw.hdr.noteSelection("selected");
        rw.inject.scene = g_injectedScene; rw.inject.firstPerson = g_injectedFirstPerson; rw.inject.warming = 4 * 42;
        rw.inject.unsupported = 12 * 4; rw.inject.otherKind = 12 * 2; rw.inject.injectedKind[3] = g_injectedCalls; rw.inject.pairChecked = 5;
        rw.inject.fovNarrowest = 0.8203f; rw.inject.fovWidest = 0.9831f;
        char text[1400];
        vrWorldFormatWindow(text, sizeof(text), VrWorldKey::Auto, VrWorldState::Owned, true, true, rw);
        Log::get().note("%s", text);
        vrWorldFormatInjectWindow(text, sizeof(text), rw.inject);
        Log::get().note("%s", text);
        // The refusal census's line (the experiment build): the census key is on here, three samples were read back, 3.0% of their pixels refused (2.0% stale, 1.0% sky).
        // This window models the log of an older build whose steady-detail key was set off (a current build always says on): every stale pixel refused, none
        // kept, no depth check. The reader's off-state logic has to keep reading such a log, so the token is set to off here on purpose.
        VrWorldRefusalWindow rf;
        rf.census = true; rf.every = kFlatMonoRefusalEvery; rf.treated = 12; rf.asked = 12; rf.sampled = 3; rf.read = 3; rf.dropped = 0;
        rf.steady = "off";
        rf.width = 5040; rf.height = 2835; rf.pixels = 3ull * 5040ull * 2835ull;
        rf.counts[kFlatMonoClassStale] = 857304; rf.counts[kFlatMonoClassSentinel] = 428652;
        vrWorldFormatRefusalWindow(text, sizeof(text), rf);
        Log::get().note("%s", text);
        // The same window with the steady detail on, as a current build always writes it (the depth-validated form): the 857304 stale pixels split in two, 90% kept (last frame's depth
        // confirmed the camera term) and 10% refused (it did not), and the resolver's depth check counted twelve frames, eleven that ran and one that could not.
        VrWorldRefusalWindow ro = rf;
        ro.steady = "on";
        ro.counts[kFlatMonoClassStale] = 85730; ro.counts[kFlatMonoRefusalStaleKept] = 771574;
        ro.checked = 11; ro.skipped = 1;
        vrWorldFormatRefusalWindow(text, sizeof(text), ro);
        Log::get().note("%s", text);
    }
    check(g_hot.posts == g_hot.calls && g_hot.preAsksPost == g_hot.calls, "every pre half asked for its post half, and every post half ran");
    {
        const uint64_t after[5] = {g_viewWorld.hash(), g_viewWeapon.hash(), g_viewUi.hash(), g_viewEyeL.hash(), g_viewEyeR.hash()};
        bool same = true;
        for (int i = 0; i < 5; ++i) same = same && after[i] == viewHashes[i];
        check(same, "the five view objects are byte-for-byte what they were before the session");
    }

    // ---- 3. a call on another thread, and the 5 s line -------------------------------------------------------------------------
    std::printf("another thread and the 5 s line\n");
    std::atomic<uint64_t> otherAllocs{~0ull};
    std::thread other([&] {
        const uint64_t a0 = t_allocs;
        for (int i = 0; i < 7; ++i) stub::observer->offThread(g_world.ptr(), 0x594E13, 3, true, GetCurrentThreadId());
        otherAllocs.store(t_allocs - a0);
    });
    other.join();
    check(otherAllocs.load() == 0, "the off-thread half allocated nothing (seven calls)");
    vrCameraCensusFrameBoundary();   // prints the other-thread line
    Sleep(5200);
    vrCameraCensusFrameBoundary();   // the 5 s window is due

    // ---- 4. KEY OFF, then ON again ------------------------------------------------------------------------------------------------
    std::printf("key off, key on\n");
    Config::get().set("advanced.vr_camera_census", "off");
    stub::clearOrder();
    vrCameraCensusFrameBoundary();
    check(!vrCameraCensusWanted() && stub::observer == nullptr && stub::paused && stub::pauseCalls == 2 && std::strcmp(stub::order, "SP") == 0,
          "the key going off detaches the observer and closes the gate (S P), and the census is no longer wanted");
    const int disarmsAfterOff = stub::disarmCalls, framesAfterOff = stub::observeFrameCalls;
    {
        const uint64_t before = t_allocs;
        for (int i = 0; i < 3; ++i) vrCameraCensusFrameBoundary();
        vrCameraCensusEyeDraw(nullptr, 1);
        check(t_allocs == before && stub::disarmCalls == disarmsAfterOff && stub::observeFrameCalls == framesAfterOff,
              "...and with the key off again boundaries and eye draws allocate nothing and ask the injector for nothing");
    }
    {   // a call that arrives while the gate is shut goes straight through the game's body
        const uint64_t callsBefore = g_hot.calls;
        refresh(g_world, g_viewWorld, 0x594E13, false);
        check(g_hot.calls == callsBefore, "a refresh call with the observer detached is not observed");
    }
    Config::get().set("advanced.vr_camera_census", "on");
    stub::clearOrder();
    vrCameraCensusFrameBoundary();
    check(vrCameraCensusWanted() && stub::observer != nullptr && !stub::paused && stub::setObserverCalls == 3 && std::strcmp(stub::order, "SPDF") == 0,
          "...and turning it on again re-registers the observer and reopens the gate (S P D F)");
    {   // The detour is NOT quiet -- the world route injects, or a camera it injected still waits for its flush: a census key-off detaches the observer
        // and leaves the relay's gate to the route (the pause is asked as flatCameraVrQuiet(), here false, so the gate stays open).
        stub::vrQuiet = false;
        Config::get().set("advanced.vr_camera_census", "off");
        stub::clearOrder();
        vrCameraCensusFrameBoundary();
        check(!vrCameraCensusWanted() && stub::observer == nullptr && !stub::paused && std::strcmp(stub::order, "SP") == 0,
              "a census key-off while the detour is not quiet (the route injects) detaches the observer and leaves the gate open: the pause is asked as flatCameraVrQuiet()");
        stub::vrQuiet = true;
        Config::get().set("advanced.vr_camera_census", "on");
        stub::clearOrder();
        vrCameraCensusFrameBoundary();
        check(vrCameraCensusWanted() && stub::observer != nullptr && !stub::paused && std::strcmp(stub::order, "SPDF") == 0,
              "...and the census comes back as before (S P D F)");
    }
    Log::get().close();

    // ---- 5. the log the glue wrote ---------------------------------------------------------------------------------------------------
    std::printf("the log\n");
    const std::wstring logPath = newestLog(tagOn.c_str());
    const std::string log = slurp(logPath);
    check(!log.empty(), "the glue wrote a log");
    check(occurrences(log, "vr camera census: on (advanced.vr_camera_census)") == 1 && occurrences(log, "vr camera census: off (advanced.vr_camera_census)") == 2,
          "it says once that the census is on and once for each of the two key-offs that it went off (a second activation does not announce itself again)");
    check(has(lineWith(log, "vr camera census: on (advanced.vr_camera_census)"), "the census itself never writes a camera (the route's injection, when it runs, is its own)") &&
              !has(lineWith(log, "vr camera census: on (advanced.vr_camera_census)"), "nothing is written to any camera") &&
              has(lineWith(log, "vr camera census: off (advanced.vr_camera_census)"), "the census observer is detached and the refresh hook stays in place"),
          "the announcement says the census itself never writes a camera (the route's injection is its own), and the key-off line says the observer is detached");
    check(has(lineWith(log, "vr camera census: on (advanced.vr_camera_census)"), "while the route jitters, only a non-zero phase"),
          "the announcement says that while the route jitters only a frame with a non-zero phase is sampled");
    check(occurrences(log, "vr camera census: episodes: up to 10, one frame each, 30 frames after a trigger") == 1 &&
              occurrences(log, "vr camera census: episodes: a trigger (1 of 2 so far) arrived while an episode was armed") == 1,
          "the episodes are announced once, and the first trigger that found one armed (the journal's flip at frame 4, while the key-on episode waited for frame 31) is said once, with the count");
    check(occurrences(log, "vr camera census: camera=0x") == 5, "one line for each of the five cameras, printed once");
    {
        // The eye cameras' bound moved by a remnant of the eye shift at frame 3: one line each. The world's and the first-person camera's bound
        // pair carries the route's phase, which moves every frame once it is non-zero (frames 8-12): each prints its four lines and counts the rest.
        const std::string eyeL = "changed: camera=" + hexOf(g_eyeL.ptr(), false) + " frame=3 n=1 bound=(", eyeR = "changed: camera=" + hexOf(g_eyeR.ptr(), false) + " frame=3 n=1 bound=(";
        check(occurrences(log, "vr camera census: changed: camera=") == 10 && occurrences(log, eyeL) == 1 && occurrences(log, eyeR) == 1 &&
                  occurrences(log, "changed: camera=" + hexOf(g_world.ptr(), false) + " ") == 4 && occurrences(log, "changed: camera=" + hexOf(g_weapon.ptr(), false) + " ") == 4,
              "the bound pair EDVR moved at frame 3 is a 'changed:' line for each eye camera; the injected phase moves the world's and the first-person camera's bound every "
              "frame, four lines each (the per-camera cap), and nothing else changed");
    }
    {
        // The sequences are of frames 8, 9 and 10: the first three frames that carry a phase. The warm-up frames 4-7 (the route jittering with a zero
        // phase, the commander on foot, the tone drawn) are not sampled and print none.
        const char* phases[3] = {"0.2520,-0.1260", "-0.1890,0.0630", "0.1260,0.2520"};
        char want[160];
        bool all = true;
        for (int i = 1; i <= 3; ++i) {
            std::snprintf(want, sizeof(want), "vr camera census: sequence frame=%d index=%d/3 foot=yes phase=%s calls=%d recorded=%d truncated=0", 7 + i, i, phases[i - 1], perFrame, perFrame);
            all = all && occurrences(log, want) == 1;
        }
        check(all, "three call sequences, of frames 8, 9 and 10 -- the first frames with a phase -- each of all 48 calls, each header naming ITS frame's phase");
        bool none = true;
        for (int f : {1, 2, 3, 4, 5, 6, 7, 11, 12}) {
            std::snprintf(want, sizeof(want), "vr camera census: sequence frame=%d ", f);
            none = none && occurrences(log, want) == 0;
        }
        check(none && occurrences(log, "vr camera census: sequence frame=") == 3 && occurrences(log, "foot=no calls=") == 0 && occurrences(log, "phase=- calls=") == 0,
              "no sequence for the ship frames 1-3, none for the flip frame 4 (recorded under the old word), none for the warm-up frames 5-7 (a zero phase while the route "
              "jitters), none past the third");
    }
    check(occurrences(log, ": call frame=") == 3u * static_cast<unsigned>(perFrame), "every call of each sequence is a line");
    check(occurrences(log, " fl=0xE>0x0 ") == 15 && occurrences(log, " fl=0x0>0x0 ") == 3u * 48u - 15u,
          "each camera's first call of a frame shows the dirty bits the body cleared (0xE>0x0), its later calls none (0x0>0x0)");
    check(occurrences(log, " inj=1 role=scene ") == 108 && occurrences(log, " inj=1 role=fp ") == 18 && occurrences(log, " inj=0 role=- ") == 18 &&
              occurrences(log, " inj=1 ") == 126 && occurrences(log, " inj=0 role=aux ") == 0,
          "the call lines say what the detour decided: 108 scene and 18 first-person calls injected (36 and 6 in each of three frames), and the UI camera's and the eyes' "
          "18 calls neither injected nor given a role");
    {
        // The eye camera's call lines name the view it was refreshed with and carry its composed rows. (" draw=" is what a
        // call line has after the caller and the camera line does not: the camera line has " thread=owner".)
        const std::string eyeLine = lineWith(log, "camera=" + hexOf(g_eyeL.ptr(), false) + " kind=5 caller=+0x594FE1 draw=");
        check(has(eyeLine, ": call frame=") && has(eyeLine, "tone=after inj=0 role=- ") && has(eyeLine, "view=" + hexOf(g_viewEyeL.ptr(), false)) && has(eyeLine, "rows=[") &&
              !has(eyeLine, "rows=-"),
              "a call line for an eye camera says kind=5 tone=after, is never injected, names that camera's view, and carries the 16 composed rows");
        const std::string worldLine = lineWith(log, "camera=" + hexOf(g_world.ptr(), false) + " kind=3 caller=+0x594E13 draw=");
        check(has(worldLine, ": call frame=") && has(worldLine, "tone=before inj=1 role=scene ") && has(worldLine, "view=" + hexOf(g_viewWorld.ptr(), false)),
              "...and one for the world camera says tone=before, injected, scene, and names the world view");
    }
    check(occurrences(log, "vr camera census: eye=") == 8 && occurrences(log, "vr camera census: eye-geometry eye=") == 8,
          "eight eye draws in all (four frames, both eyes), each with its geometry line, and no ninth");
    {
        bool lines = true;
        const char* phases[4] = {"0.2520,-0.1260", "-0.1890,0.0630", "0.1260,0.2520", "-0.2520,-0.0630"};
        for (int f = 8; f <= 11; ++f) {
            for (int e = 0; e < 2; ++e) {
                char key[120];
                std::snprintf(key, sizeof(key), "vr camera census: eye=%d frame=%d foot=yes phase=%s ", e, f, phases[f - 8]);
                const std::string l = lineWith(log, key);
                const char* why = f == 8 ? "why=no-b1" : f == 9 ? "why=b1-too-small" : nullptr;
                if (why) lines = lines && has(l, why) && has(l, "rows=- meas=-");
                else lines = lines && has(l, "bytes=5376") && has(l, " first=0 ") && has(l, " meas=(") && !has(l, "why=");
            }
        }
        check(lines, "frames 8-11, both eyes, and only those, each line carrying its frame's phase: an unbound b1, a too-small b1 (named), then two frames of real rows and a measure");
    }
    {
        bool none = true;
        char key[80];
        for (int f : {1, 2, 3, 4, 5, 6, 7, 12}) {
            std::snprintf(key, sizeof(key), "vr camera census: eye=0 frame=%d ", f);
            none = none && occurrences(log, key) == 0;
            std::snprintf(key, sizeof(key), "vr camera census: eye=1 frame=%d ", f);
            none = none && occurrences(log, key) == 0;
        }
        check(none && occurrences(log, "why=no-b1") == 2,
              "no eye line for the ship frames, the flip frame, the warm-up frames 5-7 (their eye draws found no b1: a sampled frame would have logged 'why=no-b1') or the frame past the budget");
    }
    check(occurrences(log, "vr camera census: other-thread tid=") == 1 && has(lineWith(log, "vr camera census: other-thread tid="), "caller=+0x594E13 calls=7"),
          "the seven calls on another thread are reported once, with their count");
    check(occurrences(log, "vr world route 5s: key=auto") == 1 && occurrences(log, "vr world route inject 5s: inj-scene=180 inj-fp=30 ") == 1,
          "the world route's two 5 s lines the script wrote through the log are there, back to back");
    {
        const WindowSums w = sumWindows(log);
        check(w.lines >= 1, "the 5 s line printed after five seconds");
        check(w.calls == driven && w.posts == driven && w.offThread == 7,
              "the 5 s lines count every refresh call the script drove, each with its post half, and the seven off-thread");
        check(w.toneFrames == toneFrames && w.eyeDraws == 24 && w.eyeOnFoot == 10 && w.onFootFrames == 5,
              "...the frames in which the tone was drawn (12), the 24 eye draws made, the 10 that fell in the five frames the census sampled (8-12: the route's zero-phase "
              "warm-up is not one) and the five frames");
        check(w.injCalls == 210 && w.injCalls == g_injectedCalls, "...and inj-calls counts the 210 calls the observer heard with willInject set, none more");
        check(w.kind1 && w.kind3 && w.kind5 && w.hook && w.progress && w.footYes, "...by kind (1, 3 and 5), the hook's status, the route's draw progress and the journal's word");
    }
    {
        // Bounded: about 400 lines a session, at most 400 characters of census text a line.
        unsigned census = 0, longest = 0;
        for (size_t at = log.find("vr camera census"); at != std::string::npos; at = log.find("vr camera census", at + 1)) {
            const size_t end = log.find('\n', at);
            const unsigned len = static_cast<unsigned>((end == std::string::npos ? log.size() : end) - at);
            ++census;
            if (len > longest) longest = len;
        }
        char what[160];
        std::snprintf(what, sizeof(what), "bounded: %u census lines in the session (at most 400), the longest %u characters (at most 400)", census, longest);
        check(census <= 400 && longest <= 400, what);
    }

    // ---- 6. the real reader over the real log ------------------------------------------------------------------------------------------
    std::printf("the reader\n");
    int rc = -1;
    const std::string report = runReader(logPath, &rc);
    check(rc == 0 && report.find("camera census:") != std::string::npos, "tools\\edvr_log.py --camera-census ran over the glue's log and exited 0");
    {
        const std::string eyes = lineWith(report, "eye camera(s): ");
        check(has(eyes, hexOf(g_eyeL.ptr(), true)) && has(eyes, hexOf(g_eyeR.ptr(), true)) && has(eyes, "world camera: " + hexOf(g_world.ptr(), true)) &&
                  has(eyes, "other world-side kind-3 camera(s): " + hexOf(g_weapon.ptr(), true)),
              "the reader names both eye cameras, the world camera and the first-person camera: the content join found the cameras whose composed rows equal what the eye draws read from the GPU");
        const std::string world = lineWith(report, "world camera " + hexOf(g_world.ptr(), true) + ": ");
        check(has(world, "108 kind-3 call(s) over 3 logged frame(s) = 36.0 a frame, 108 of them before the tone") && has(world, "(1 projection(s); its first-seen kind was 3)"),
              "the world camera is named for its kind-3 calls before the tone (36 a frame), with its one projection");
        check(has(report, "camera " + hexOf(g_eyeL.ptr(), true) + " EYE: k5 x6 over 3 logged frame(s) (2.0 a frame)") &&
                  has(report, "camera " + hexOf(g_weapon.ptr(), true) + " world-side: k3 x18 over 3 logged frame(s) (6.0 a frame)"),
              "the roles section labels each camera by the kinds of its calls: the eyes kind 5, the first-person camera kind 3");
        const std::string a = lineWith(report, "(A) call signature");
        check(has(a, "kind, aspect, fov, shift separate every eye camera from EVERY world-side kind-3 camera"),
              "(A) a call's signature (kind, aspect, fov, off-centre) separates the eye cameras' calls from every world-side call, the first-person camera's included; the near plane "
              "(0.025, the scene's too) does not");
        const std::string b = lineWith(report, "(B) content join:");
        check(has(b, "2 of 8 eye draw(s) joined") && has(b, hexOf(g_eyeL.ptr(), true)) && has(b, hexOf(g_eyeR.ptr(), true)),
              "(B) two of the eight eye draws joined: frame 10's; the four whose readback failed and the two past the third sequence could not");
        const size_t joinAt = report.find("== the offline join (B)");
        const std::string j0 = lineWith(report, "eye 0 frame 10 draw", joinAt == std::string::npos ? 0 : joinAt);
        const std::string j1 = lineWith(report, "eye 1 frame 10 draw", joinAt == std::string::npos ? 0 : joinAt);
        check(joinAt != std::string::npos && has(j0, "camera " + hexOf(g_eyeL.ptr(), true) + " (kind 5)") && has(j1, "camera " + hexOf(g_eyeR.ptr(), true) + " (kind 5)") && has(j0, "tone after"),
              "...eye 0's rows joined to the LEFT eye camera and eye 1's to the RIGHT one, kind 5, at tone after");
        check(occurrences(report, "no rows were read (no-b1)") == 2 && occurrences(report, "no rows were read (b1-too-small)") == 2 &&
              occurrences(report, "frame 11's call sequence was not logged") == 2,
              "...and the joins that could not be made say why: no b1, a too-small b1, and no sequence logged for frame 11");
        check(has(lineWith(report, "(C) place in the frame:"), "every refresh of a world-side camera precedes every refresh of an eye camera in 3 of 3 logged sequence(s)"),
              "(C) the world's refreshes precede the eyes' in all three logged sequences");
        check(has(lineWith(report, "(D) caller:"), "+0x594FE1") && has(lineWith(report, "(D) caller:"), "the caller alone does not separate them") &&
                  !has(report, "not enough calls logged to say"),
              "(D) the caller is shared with the world camera, so it does not separate them, and nothing says 'not enough calls'");
        check(has(lineWith(report, "(F) view"), "no view is shared: the view separates them"),
              "(F) the view (the refresh's second argument) separates the eye cameras from the world's");
        check(report.find("(E) tangents:") != std::string::npos, "(E) the tangents comparison is reported (compared, or said why not)");
        check(occurrences(report, "the rows cannot tell which way the shift is carried") == 4 && report.find("\n    the rows measure as ") == std::string::npos,
              "the advertised shift is nothing here, so for each of the four eye draws that read rows the reader says the shift's sign cannot be told, and names none");
        check(report.find("leak measure over 4 eye draw(s): largest |leak| = ") != std::string::npos,
              "the leak is reported over the four eye draws that read rows");
        check(report.find("3 call sequence(s)") != std::string::npos && report.find("journal foot=yes, phase (0.2520, -0.1260) px") != std::string::npos &&
              report.find("1 other-thread entry") != std::string::npos,
              "the summary line counts three call sequences and the other-thread entry, and the sequences carry the journal's word and the route's phase");
    }
    {
        // THE VERDICT over the glue's own log. The log holds deliberate faults: four eye draws whose readback failed (no b1, a too-small b1) and seven calls on
        // another thread. So: (i) cannot measure four eye draws (WARN), (iv) STOP for the off-thread calls; (ii), (iii), (v) PASS.
        auto verdict = [](const std::string& text, const char* tag) {
            const std::string key = std::string("(") + tag + ") ";
            for (size_t p = 0; p < text.size();) {
                size_t e = text.find('\n', p);
                if (e == std::string::npos) e = text.size();
                const std::string l = text.substr(p, e - p);
                const size_t at = l.find(key);
                if (at != std::string::npos && at <= 6 && (l.compare(0, 4, "PASS") == 0 || l.compare(0, 4, "WARN") == 0 || l.compare(0, 4, "STOP") == 0 || l.compare(0, 3, "n/a") == 0)) return l;
                p = e + 1;
            }
            return std::string();
        };
        const std::string i = verdict(report, "i"), ii = verdict(report, "ii"), iii = verdict(report, "iii"), iv = verdict(report, "iv"), v = verdict(report, "v");
        check(i.compare(0, 4, "WARN") == 0 && has(i, "8 of 8 eye draw(s) had a non-zero world phase") && has(i, "4 eye draw(s) could not be measured") && has(i, "over 4 measured"),
              "THE VERDICT (i): every eye draw of the glue's log had a non-zero phase; four could not be measured (their readback failed): a WARN, never a PASS");
        check(ii.compare(0, 4, "PASS") == 0 && has(ii, "126 injected kind-3 call(s) in 3 sequence(s) with a non-zero phase measure the phase they were given (scene 108 call(s)") &&
                  has(ii, "first-person 18 call(s)") && has(ii, "at 5040x2835 (from a `vr world route 5s:` line's hdr=)"),
              "THE VERDICT (ii): the 126 injected calls of the three sequences carry the phase the headers name, measured from the rows the real glue logged, at the render size the route line gave");
        check(iii.compare(0, 4, "PASS") == 0 && has(iii, "route inj-kinds=3:210 over 1 window(s); 126 logged call(s) with inj=1, all kind 3"),
              "THE VERDICT (iii): only kind 3 was injected, by the route's inj-kinds= and by the census's own call lines");
        check(iv.compare(0, 4, "STOP") == 0 && has(iv, "the census counted 7 call(s) off the render thread"),
              "THE VERDICT (iv): the seven calls on another thread are a STOP");
        check(v.compare(0, 4, "PASS") == 0 && has(v, "inj-scene 180, inj-fp 30") && has(v, "scene 108 (108 injected), first-person 18 (18 injected)"),
              "THE VERDICT (v): the roles, from the route's counts and the logged calls");
        check(report.find("stage 2 verdict: STOP (") != std::string::npos, "...so the verdict over this log is STOP");
        check(has(report, "== refusal census") &&
                  has(report, "MEASURED 3 sample(s) of 5040x2835 (12 asked, 3 dispatched, 0 dropped), treated 12; pixels 42865200; refused 3.000% (1285956): stale-refused 2.000%, sentinel 1.000%; stale-kept 0.000%") &&
                  has(report, "steady-detail=on view=off census=on: MEASURED 3 sample(s) of 5040x2835 (12 asked, 3 dispatched, 0 dropped), treated 12; pixels 42865200; refused 1.200% (514382): "
                              "stale-refused 0.200%, sentinel 1.000%; stale-kept 1.800% (90.0% of the stale pixels); depth-check 11 ran, 1 skipped") &&
                  has(report, "refusal census: consistent (2 of 2 window(s) measured)") && has(v, "struct field of view 0.8203..0.9831 rad over 1 window(s)"),
              "THE REFUSAL CENSUS: the route's refusal lines, written by its own formatter through the real Log, are read by the real reader as two measured windows: the key off (3.000% refused: 2.000% stale-refused, "
              "1.000% sky, nothing kept) and the key on (the stale pixels split: 1.800% kept, 90% of them, 0.200% refused, and the depth check's eleven ran and one skipped frames), consistent, "
              "and (v) carries the inject line's field-of-view range");
        // The same log with its faults taken out (the four failed readbacks' lines, the off-thread calls): every line PASSES. The text is the glue's own.
        std::string clean;
        for (size_t p = 0; p < log.size();) {
            size_t e = log.find('\n', p);
            if (e == std::string::npos) e = log.size();
            std::string l = log.substr(p, e - p);
            p = e + 1;
            const bool eyeLine = l.find("vr camera census: eye=") != std::string::npos || l.find("vr camera census: eye-geometry eye=") != std::string::npos;
            if (eyeLine && (l.find(" frame=8 ") != std::string::npos || l.find(" frame=9 ") != std::string::npos)) continue;
            if (l.find("vr camera census: other-thread tid=") != std::string::npos) continue;
            const size_t off = l.find(" off-thread=7 ");
            if (off != std::string::npos) l.replace(off, 14, " off-thread=0 ");
            clean += l;
            clean += '\n';
        }
        const std::wstring cleanPath = g_dir + L"\\clean_census.log";
        if (FILE* f = _wfopen(cleanPath.c_str(), L"wb")) { std::fwrite(clean.data(), 1, clean.size(), f); std::fclose(f); }
        int rc2 = -1;
        const std::string report2 = runReader(cleanPath, &rc2);
        check(rc2 == 0 && report2.find("stage 2 verdict: PASS (6 PASS, 0 WARN, 0 STOP, 0 n/a)") != std::string::npos &&
                  has(verdict(report2, "i"), "4 of 4 eye draw(s) had a non-zero world phase") && has(verdict(report2, "i"), "below 1e-06"),
              "THE VERDICT over the same log with the failed readbacks and the off-thread calls taken out is PASS on all six lines: the eyes' rows did not move (leak below 1e-6 "
              "with a world phase of 1e-4) while the kind-3 calls carried it");
        if (g_failures) std::printf("---- the clean log's report verdict ----\n%s\n", report2.substr(report2.find("== stage 2 verdict") == std::string::npos ? 0 : report2.find("== stage 2 verdict")).c_str());
    }
    runEpisodes(gpu);
    if (g_failures) std::printf("---- the reader's report ----\n%s\n---- the census log, census lines only ----\n%s\n", report.c_str(), censusLinesOnly(log).c_str());
    return g_failures;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::printf("vr camera census glue test: dry run (no device, no log, no files)\n");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) {
        const int failed = run();
        if (failed == 0 && g_failures == 0) {
            removeScratch();   // a failure leaves the scratch directory (and both logs) to read
            std::printf("vr camera census glue: PASS\n");
            return 0;
        }
        std::printf("vr camera census glue: %d check(s) FAILED (scratch kept in %s)\n", g_failures ? g_failures : failed, narrow(g_dir).c_str());
        return 1;
    }
    std::printf("usage: vr_camera_census_glue_test --self-test | --dry-run\n");
    return 2;
}

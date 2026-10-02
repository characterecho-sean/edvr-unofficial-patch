// The intro movie's world-lock path and the splash, as src/d3d11/intro_panel.cpp does them TODAY (characterisation, written before the
// panels follow fix.panel_curvature; docs/intro-video.md, docs/frontier-intro-video-report.md).
//
// The change to come must leave this module byte-identical at curvature 0, so this rig pins what the real module does now, on a WARP
// device, with the real Config, Log and fault guard, and stubs only for what it calls from elsewhere: the vr half's published pose and
// tangents (frame_flag.h), the resampler (intro_upscale.h), the binding shadow's resolver and the on-foot screen's motion pass (the module
// links panel_curve.cpp for real now). The draw is the rig's own recording draw function, called the way vscreen.cpp forwards the game's
// draw: after introPanelOnComposite, before introPanelEndDraw.
//
// STEP 2 (the movie follows fix.panel_curvature; docs/intro-video.md, 2026-10-01). Every scenario of step 1 above passes UNCHANGED, with one
// strengthening: every step of every scenario also says introPanelStripArmed() is false at the draw and after it (curvature 0 arms nothing).
// The curved-* scenarios pin the new part: at curvature above 0 -- the surface strip wanted and not stood down -- the world constants carry the
// z column in cb2[3] (the seated +z axis through the projection), every other float the flat panel's to the bit; the strip is armed from a
// successful bind until introPanelEndDraw; stock, head, the refusals, the settle frames, a stood-down surface and curvature 0 never arm
// anything. MIRROR FIX (job 4): every step also says which way the strip's u runs -- introPanelStripReverseU() is true at exactly the armed draws
// (the movie's placement has its +x running to the viewer's LEFT, so u runs against x: it agrees with introPlacementXDir and with a minor of the rig's
// own over the bound bytes) and false everywhere else and after introPanelEndDraw. There is NO test of the bent edges against the eye (round 3 removed the one round 2 had: D3D clips what is behind the eye in
// homogeneous space, a test popped the whole bend off at about 20 degrees of head yaw): the centre's is the only one, and curved-no-edge-test
// pins the absence where a test would bite hardest.
//
// ONE PROCESS PER SCENARIO. The module keeps latches nothing resets: g_refused (set when the constants do not read as screen-space: the
// transform is off for the session), g_retired, g_lockRefusedNoted and g_anchored (each log line is once per process), g_recentreRequested,
// g_applied, g_frame and a fault budget of four. Configure and Shutdown clear none of them. So --self-test starts this exe once per
// scenario (--scenario <id>) and sums what each reports; a scenario is a session.
//
// Goldens (kGoldens below) are the 80 bytes the draw sees at VS b2, copied from the module's output on the commit this rig was written
// against, after the independent recomputation in this file (the arithmetic of buildWorldCb's own comments, in double, as matrices) agreed
// with them; every run checks both, so a golden is never just whatever came out. --emit-golden prints the table from the code as it is.
//
// SCENARIOS (labels are "<scenario>.<what>"; --list prints them):
//   golden, golden-zero   screen mode, the movie's stock cb2 in a left-eye and a right-eye buffer, Pimax tangents: the settle frames (the
//                         first composite of a buffer starts the readback; four frame edges later it binds), what is bound at VS b2 after
//                         each call, our 80 bytes for BOTH eyes over three published poses, the game's buffer back after introPanelEndDraw
//                         with no stray reference, the draw's arguments and the rest of the pipeline untouched, the resampler's pairing,
//                         the once-only log lines. golden-zero sets fix.panel_curvature to 0 and must see the same bytes.
//   golden-asymmetric     the same lock on a Quest-like asymmetric vertical frustum (m12 is not zero there)
//   head, head-symmetric, head-clamp-low, head-clamp-high
//                         fix.intro_video = head: the splash-sized panel with no world lock needs no pose and no eye; its bytes, and the
//                         factor held between 1 and 8
//   stock, stock-off, config   stock never applies the transform; the resampler alone keeps the path alive; the Wants table; the unrecognised value
//   splash, splash-no-fill   the game's WORLD-space cb2 from a real capture is refused by its own numbers, for the session; without a fill
//                         it is not even read
//   refuse-<clause>, accept-<edge>   every clause of the screen-space test, one buffer each, and the session-wide refusal
//   no-pose, no-tangents, degenerate-tangents, no-vertical, behind, non-finite, viewport
//                         the lock's refusals: the movie stays as the game drew it, one log line, retried at every draw
//   eyes-alike, eyes-symmetric, eyes-near-symmetric, eyes-faint-known, eyes-swapped
//                         the eye is read from the game's own cb2[4].x; two buffers that do not tell the eyes apart are refused
//   retire-used, retire-settling, retire-unseen   the first rendered scene stands everything down for the session
//   shutdown-relearn, third-buffer, small-cb, distance, gates, scene-arrived   the rest of what is observable
//   curved                curvature 0.3, 48 columns, five poses (A, B, C, D, I): every one bends the movie (bytes to the bit against the curved
//                         goldens, the flat panel's floats everywhere but cb2[3], the strip armed at the draw and not after); the gain, the
//                         first-armed line, the retirement line's count of armed draws, no line about an edge
//   curved-asymmetric     the Quest-like vertical frustum: the shear of the z column (m12)
//   curved-no-edge-test   the worst places for a test of the bent edges that is not there: distance at its 1 m floor with curvature 1.0 and a
//                         head yawed 40 degrees, one at 0.3 and 25 degrees, both ways round, the centre in front: armed, cb2[3] = col(cz), every
//                         time (the rig's own arithmetic shows a nearer edge would be behind the eye in each)
//   curved-unknown-xdir   (the mirror fix) a placement whose +x cannot be told to run left or right (a head rolled a quarter turn; the panel seen
//                         edge-on): bound and flat, cb2[3] exactly zero, not armed, one line; the draws around it, bent as ever
//   curved-right-running  (the mirror fix) a placement whose +x runs to the viewer's right (a reflected pose: the movie's own never does): armed,
//                         and u runs WITH x -- the flag is read from the constants, it is not a constant of the movie
//   curved-live           the curvature switched live: flat, bent (every float but cb2[3] the flat panel's), flat again to the bit; the z column
//                         does not depend on the curvature; the retirement line's armed count against its resized count
//   curved-stood-down     a faulting strip draw stands the surface down: the movie is flat from then on whatever the configuration
//   curved-armed-scope    armed is for the bind the last call made: a call that binds nothing clears it, and so does the shutdown
//   curved-refused        no pose, no tangents, behind, a degenerate viewport at curvature 0.3: nothing armed; the retry bends
//   curved-stock, curved-head, curved-splash, curved-eyes-alike   the step-1 scenarios of those names at curvature 0.3: unchanged, never armed
//
// tools\intro_curve_test\mutants.py compiles this rig against a copy of the module with ONE rule flipped and requires the rig to fail on
// a scenario that belongs to the rule.
//
// Usage: --self-test [<repo root, ignored>] [--keep]  |  --scenario <id> [--emit] [--trace] [--keep]  |  --list  |  --emit-golden  |  --dry-run
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../src/common/config.h"
#include "../../src/common/frame_flag.h"
#include "../../src/common/log.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/common/system_d3d11.h"
#include "binding_shadow.h"
#include "intro_curve_math.h"   // introPlacementXDir: the rule the module reads the movie's own placement with; the rig checks it against a minor of its own
#include "intro_panel.h"
#include "intro_upscale.h"
#include "panel_curve.h"
#include "screen_motion.h"

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------------------------------------------------------------------------
// The harness: every failing check is printed once under its label, and the scenario goes on.
// ---------------------------------------------------------------------------------------------------------------------------------
unsigned g_checks = 0;
unsigned g_failed = 0;
std::map<std::string, unsigned> g_failCount;
std::string g_note;       // what the scenario is on (frame, eye), appended to a failure's line
const char* g_scn = "";   // the scenario's id: the prefix of every label
bool g_emit = false;      // --emit: print the goldens instead of comparing
bool g_trace = false;     // --trace: one line per composite

std::string fmt(const char* f, ...) {
    char buf[700];
    va_list ap;
    va_start(ap, f);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, f, ap);
    va_end(ap);
    return std::string(buf);
}
std::string lab(const char* what) { return std::string(g_scn) + "." + what; }

void check(bool ok, const std::string& label, const std::string& detail = std::string()) {
    ++g_checks;
    if (ok) return;
    ++g_failed;
    if (++g_failCount[label] == 1) {
        std::string line = "FAIL: " + label;
        if (!detail.empty()) line += " -- " + detail;
        if (!g_note.empty()) line += " [" + g_note + "]";
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    }
}

bool closeTo(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
bool has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }
size_t count(const std::string& s, const std::string& needle) {
    size_t n = 0;
    for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + needle.size())) ++n;
    return n;
}
std::string narrow(const std::wstring& w) {
    std::string out;   // the scratch paths are ASCII
    for (wchar_t c : w) out.push_back(static_cast<char>(c));
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// What the pipeline looks like to a draw, as the context reports it.
// ---------------------------------------------------------------------------------------------------------------------------------
// Raw pointers, identities only: a snapshot holds no reference, so it cannot make the game's buffers look leaked (or hide a leak). What the
// context holds is alive for as long as it is bound, which is the only time a snapshot's pointers are compared with anything.
struct Snapshot {
    ID3D11Buffer* vb0 = nullptr;
    UINT stride0 = 0, offset0 = 0;
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN;
    UINT ibOffset = 0;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer* cb[4] = {nullptr, nullptr, nullptr, nullptr};   // VS constant buffers 0..3 (b2 is the placement the module replaces)
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11RasterizerState* rs = nullptr;
    D3D11_VIEWPORT vp{};
    ID3D11BlendState* blend = nullptr;
    float blendFactor[4] = {0, 0, 0, 0};
    UINT sampleMask = 0;
    ID3D11DepthStencilState* dss = nullptr;
    UINT stencilRef = 0;
    ID3D11RenderTargetView* rtv = nullptr;
};

template <class T>
T* rawOf(T* p) {   // a Get* returned a reference: keep the identity, give the reference back
    if (p) p->Release();
    return p;
}

Snapshot takeSnapshot(ID3D11DeviceContext* ctx) {
    Snapshot s;
    ID3D11Buffer* vb = nullptr;
    ctx->IAGetVertexBuffers(0, 1, &vb, &s.stride0, &s.offset0);
    s.vb0 = rawOf(vb);
    ID3D11Buffer* ib = nullptr;
    ctx->IAGetIndexBuffer(&ib, &s.ibFmt, &s.ibOffset);
    s.ib = rawOf(ib);
    ctx->IAGetPrimitiveTopology(&s.topo);
    ID3D11Buffer* cb[4] = {nullptr, nullptr, nullptr, nullptr};
    ctx->VSGetConstantBuffers(0, 4, cb);
    for (int i = 0; i < 4; ++i) s.cb[i] = rawOf(cb[i]);
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    s.srv = rawOf(srv);
    ID3D11SamplerState* smp = nullptr;
    ctx->PSGetSamplers(0, 1, &smp);
    s.sampler = rawOf(smp);
    ID3D11RasterizerState* rs = nullptr;
    ctx->RSGetState(&rs);
    s.rs = rawOf(rs);
    UINT nvp = 1;
    ctx->RSGetViewports(&nvp, &s.vp);
    ID3D11BlendState* bl = nullptr;
    ctx->OMGetBlendState(&bl, s.blendFactor, &s.sampleMask);
    s.blend = rawOf(bl);
    ID3D11DepthStencilState* ds = nullptr;
    ctx->OMGetDepthStencilState(&ds, &s.stencilRef);
    s.dss = rawOf(ds);
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, &dsv);
    s.rtv = rawOf(rtv);
    rawOf(dsv);
    return s;
}

enum Diff : unsigned { kDiffIa = 1, kDiffCb0 = 2, kDiffCb1 = 4, kDiffCb2 = 8, kDiffCb3 = 16, kDiffPs = 32, kDiffRs = 64, kDiffOm = 128 };
unsigned diffBits(const Snapshot& a, const Snapshot& b) {
    unsigned d = 0;
    if (a.vb0 != b.vb0 || a.stride0 != b.stride0 || a.offset0 != b.offset0 || a.ib != b.ib || a.ibFmt != b.ibFmt || a.ibOffset != b.ibOffset || a.topo != b.topo) d |= kDiffIa;
    if (a.cb[0] != b.cb[0]) d |= kDiffCb0;
    if (a.cb[1] != b.cb[1]) d |= kDiffCb1;
    if (a.cb[2] != b.cb[2]) d |= kDiffCb2;
    if (a.cb[3] != b.cb[3]) d |= kDiffCb3;
    if (a.srv != b.srv || a.sampler != b.sampler) d |= kDiffPs;
    if (a.rs != b.rs || std::memcmp(&a.vp, &b.vp, sizeof(a.vp)) != 0) d |= kDiffRs;
    if (a.blend != b.blend || std::memcmp(a.blendFactor, b.blendFactor, sizeof(a.blendFactor)) != 0 || a.sampleMask != b.sampleMask || a.dss != b.dss ||
        a.stencilRef != b.stencilRef || a.rtv != b.rtv)
        d |= kDiffOm;
    return d;
}

std::vector<uint8_t> readBuffer(ID3D11DeviceContext* ctx, ID3D11Buffer* buf) {
    std::vector<uint8_t> out;
    if (!buf) return out;
    D3D11_BUFFER_DESC d{};
    buf->GetDesc(&d);
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(dev.GetAddressOf());
    if (!dev) return out;
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth = d.ByteWidth;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;
    if (FAILED(dev->CreateBuffer(&sd, nullptr, staging.GetAddressOf()))) return out;
    ctx->CopyResource(staging.Get(), buf);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)) || !m.pData) return out;
    out.assign(static_cast<const uint8_t*>(m.pData), static_cast<const uint8_t*>(m.pData) + d.ByteWidth);
    ctx->Unmap(staging.Get(), 0);
    return out;
}

// What the game's forwarded draw saw: its arguments, the pipeline, and the bytes of whatever is bound at VS b2 at that moment.
struct DrawRecord {
    UINT count = 0, instances = 0, start = 0;
    INT base = 0;
    UINT startInstance = 0;
    Snapshot snap;
    std::vector<uint8_t> cb2Bytes;
    D3D11_BUFFER_DESC cb2Desc{};
};
std::vector<DrawRecord> g_draws;

// The game's draw, forwarded by the caller: the composite is a six-index instanced quad.
void recordDraw(ID3D11DeviceContext* ctx, UINT count, UINT instances, UINT start, INT base, UINT startInstance) {
    DrawRecord r;
    r.count = count;
    r.instances = instances;
    r.start = start;
    r.base = base;
    r.startInstance = startInstance;
    r.snap = takeSnapshot(ctx);
    r.cb2Bytes = readBuffer(ctx, r.snap.cb[2]);
    if (r.snap.cb[2]) r.snap.cb[2]->GetDesc(&r.cb2Desc);
    g_draws.push_back(std::move(r));
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
// What the module calls that this rig does not link: the vr half's published channel, the resampler, the binding shadow.
// ---------------------------------------------------------------------------------------------------------------------------------
namespace stub {
bool havePose = true;
float pose[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
bool haveTangents = true;
float outer = 1.5293f, inner = 1.0324f;   // the Pimax Crystal Super's horizontal pair: span 2.5617, frustum centre (outer-inner)/span = 0.19397
bool haveVertical = true;
float top = 1.2648f, bot = 1.2648f;       // t = -1.2648, b = +1.2648 (5424x5356)
bool sceneArrived = false;
unsigned recentreRequests = 0;
bool upscaleWants = false;                // the resampler's own want: false, so the transform alone decides
bool upscaleBegin = false;                // what introUpscaleBegin answers
unsigned upBegin = 0, upEnd = 0, upFrameEnd = 0, upShutdown = 0;
ID3D11ShaderResourceView* lastSrv = nullptr;
}  // namespace stub

namespace edvr {
bool headPose(float* out12) {
    if (!stub::havePose) return false;
    std::memcpy(out12, stub::pose, sizeof(stub::pose));
    return true;
}
bool eyeTangents(float* outerMag, float* innerMag) {
    if (!stub::haveTangents) return false;
    *outerMag = stub::outer;
    *innerMag = stub::inner;
    return true;
}
bool eyeTangentsVertical(float* topMag, float* botMag) {
    if (!stub::haveVertical) return false;
    *topMag = stub::top;
    *botMag = stub::bot;
    return true;
}
void requestIntroRecentre() { ++stub::recentreRequests; }
bool sceneArrived() { return stub::sceneArrived; }

bool introUpscaleWants() { return stub::upscaleWants; }
bool introUpscaleBegin(ID3D11DeviceContext*, ID3D11ShaderResourceView* srcSrv) {
    ++stub::upBegin;
    stub::lastSrv = srcSrv;
    return stub::upscaleBegin;
}
void introUpscaleEnd(ID3D11DeviceContext*) { ++stub::upEnd; }
void introUpscaleFrameEnd() { ++stub::upFrameEnd; }
void introUpscaleShutdown() { ++stub::upShutdown; }

// panel_curve.cpp is linked for real (fix.panel_curvature, and whether the surface strip is wanted); its on-foot half calls the screen's motion
// pass, which nothing here reaches.
void screenMotionDraw(ID3D11DeviceContext*, PanelCurveDrawFn, unsigned, unsigned, unsigned, int, unsigned, const float*) {}

// The real resolver is binding_shadow.cpp, which carries the whole hook layer. The module reads only a buffer's byte width (info.a).
bool bindingResolveResource(void* resource, ResourceInfo* out) {
    if (!resource || !out) return false;
    ID3D11Resource* r = static_cast<ID3D11Resource*>(resource);
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    D3D11_BUFFER_DESC d{};
    static_cast<ID3D11Buffer*>(resource)->GetDesc(&d);
    *out = ResourceInfo{};
    out->isBuffer = true;
    out->a = d.ByteWidth;
    out->b = d.StructureByteStride;
    out->resource = resource;
    return true;
}
}  // namespace edvr

namespace {

using namespace edvr;

// ---------------------------------------------------------------------------------------------------------------------------------
// The scratch directory, the module's log, and the files read back.
// ---------------------------------------------------------------------------------------------------------------------------------
std::wstring g_dir;
std::wstring g_tag;
std::string slurp(const std::wstring& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
std::wstring newestLog(const wchar_t* tag) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((g_dir + L"\\edvr_" + tag + L"_*.log").c_str(), &fd);
    std::wstring best;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = g_dir + L"\\" + fd.cFileName;
            if (n > best) best = n;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return best;
}
bool beginLog() { return Log::get().open(g_dir, g_tag.c_str()); }
std::string endLog() {
    Log::get().close();   // the flusher is joined: the file is complete
    return slurp(newestLog(g_tag.c_str()));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The game's constants and the vr half's, as measured.
// ---------------------------------------------------------------------------------------------------------------------------------
// The movie's own cb2 (docs/frontier-intro-video-report.md, "For the movie, cb2 reads"): 512 x 288 PIXELS, pixels to NDC, w a constant 1,
// the frustum centre in cb2[4].x -- positive in the left eye, mirrored in the right.
const float kStockL[20] = {512.0f, 288.0f, 0, 0,   -0.000368732f, 0, 0, 0,   0, 0.000373413f, 0, 0,   0, 0, 0, 0,   0.193907f, 0, 0, 1.0f};
const float kStockR[20] = {512.0f, 288.0f, 0, 0,   -0.000368732f, 0, 0, 0,   0, 0.000373413f, 0, 0,   0, 0, 0, 0,   -0.1940f, 0, 0, 1.0f};
// The splash's own cb2: world-space, from edvr_gfx_20260828_182818.log (the DCW read, capture 2).
const float kSplash[20] = {4.44444f, 2.5f, 0, 0,
                           -0.780684f, -0.0197438f, 9.48621e-08f, -0.000948464f,
                           -0.0047429f, 0.788105f, -7.60756e-06f, 0.076063f,
                           -0.194148f, 0.0601386f, 9.97268e-05f, -0.997103f,
                           -0.0725725f, -0.247872f, 0.0996338f, 3.76108f};

constexpr uint32_t kFillW = 1920, kFillH = 1080;   // the surface the movie is converted into (docs/intro-video.md)

// Head poses, row-major 3x4 (rotation in the left 3x3, translation in the last column). The rotations are built from (cos, sin) pairs that
// are exact in decimal -- (0.6, 0.8) and (0.96, 0.28) -- so every entry can be checked by hand: R = Ry(yaw) * Rx(pitch).
//   A: yaw  atan2(0.8, 0.6) = 53.13 deg, pitch 16.26 deg, at (0.25, 1.50, -0.50)
//   B: yaw -36.87 deg, pitch 36.87 deg, at (-0.10, 1.20, 0.30)
//   I: the identity at the origin (the closed form: the panel straight ahead at the distance)
const float kPoseA[12] = {0.6f, 0.224f, 0.768f, 0.25f,   0.0f, 0.96f, -0.28f, 1.5f,   -0.8f, 0.168f, 0.576f, -0.5f};
const float kPoseB[12] = {0.8f, -0.36f, -0.48f, -0.1f,   0.0f, 0.8f, -0.6f, 1.2f,   0.6f, 0.48f, 0.64f, 0.3f};
const float kPoseI[12] = {1, 0, 0, 0,   0, 1, 0, 0,   0, 0, 1, 0};
// A half turn: the runtime's own quirk on the measured rig (yaw 180 from the first frame). The panel at the game's forward is then behind.
const float kPoseBehind[12] = {-1, 0, 0, 0,   0, 1, 0, 0,   0, 0, -1, 0};

// The curved movie's poses. Every one of them bends the movie (a pose cannot keep it flat: the bent edges are not tested, only the centre).
//   C: yaw atan2(0.28, 0.96) = 16.26 deg to the left, a few centimetres off the origin
//   D: C mirrored
//   Yaw40Pos/Neg, Yaw25Pos/Neg: a head turned 40 (or 25) degrees about the vertical, either way, at the origin: where a bent panel's nearer edge is
//      beside or behind the eye (the worst places for a test of the edges that is not there: curved-no-edge-test)
const float kPoseC[12] = {0.96f, 0.0f, 0.28f, 0.10f,   0.0f, 1.0f, 0.0f, 0.05f,   -0.28f, 0.0f, 0.96f, -0.20f};
const float kPoseD[12] = {0.96f, 0.0f, -0.28f, -0.10f,   0.0f, 1.0f, 0.0f, 0.05f,   0.28f, 0.0f, 0.96f, -0.20f};
const float kPoseYaw40Pos[12] = {0.76604444f, 0.0f, 0.64278761f, 0.0f,   0.0f, 1.0f, 0.0f, 0.0f,   -0.64278761f, 0.0f, 0.76604444f, 0.0f};
const float kPoseYaw40Neg[12] = {0.76604444f, 0.0f, -0.64278761f, 0.0f,   0.0f, 1.0f, 0.0f, 0.0f,   0.64278761f, 0.0f, 0.76604444f, 0.0f};
const float kPoseYaw25Pos[12] = {0.90630779f, 0.0f, 0.42261826f, 0.0f,   0.0f, 1.0f, 0.0f, 0.0f,   -0.42261826f, 0.0f, 0.90630779f, 0.0f};
const float kPoseYaw25Neg[12] = {0.90630779f, 0.0f, -0.42261826f, 0.0f,   0.0f, 1.0f, 0.0f, 0.0f,   0.42261826f, 0.0f, 0.90630779f, 0.0f};

// Poses at which the movie's placement cannot be told to run its +x left or right (IntroXDir::kUnknown; docs\intro-video.md, the mirror fix):
//   Roll90: the head rolled a quarter turn about its view axis, so the panel's x axis is vertical on the screen: cb2[1].x and cb2[1].w are exactly
//      zero, and so are both terms of the rule (the centre is still straight ahead at 3.35 m)
//   EdgeOn: the head standing IN the panel's own plane, 3 m to one side and looking at its centre (yaw 90 degrees, at (3, 0, -3.35)): the panel is seen
//      edge-on, the two terms nearly cancel -- 2 % of their size in either eye, under the 5 % margin -- and the centre is still 3 m ahead
const float kPoseRoll90[12] = {0.0f, -1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f, 0.0f,   0.0f, 0.0f, 1.0f, 0.0f};

// REFLECTED poses (a mirror in x, determinant -1: not a head -- the module takes any 12 floats): the panel's +x axis, which the movie builds as
// minus the head's x axis, is then the viewer's RIGHT. The movie's own placement never runs that way, so this is how the rig gets the other half of
// the rule: Mirror is the identity reflected, MirrorYaw is pose C reflected (the same yaw and offsets, x reversed).
const float kPoseMirror[12] = {-1.0f, 0.0f, 0.0f, 0.0f,   0.0f, 1.0f, 0.0f, 0.0f,   0.0f, 0.0f, 1.0f, 0.0f};
const float kPoseMirrorYaw[12] = {-0.96f, 0.0f, -0.28f, 0.10f,   0.0f, 1.0f, 0.0f, 0.05f,   -0.28f, 0.0f, 0.96f, -0.20f};
const float kPoseEdgeOn[12] = {0.0f, 0.0f, 1.0f, 3.0f,   0.0f, 1.0f, 0.0f, 0.0f,   -1.0f, 0.0f, 0.0f, -3.35f};

// ---------------------------------------------------------------------------------------------------------------------------------
// The goldens: the 80 bytes of cb2 the draw sees at VS b2, from the module's output (see the header). Emitted by --emit-golden.
// ---------------------------------------------------------------------------------------------------------------------------------
struct Golden {
    const char* name;
    float v[20];
};
const Golden kGoldens[] = {
    // GOLDENS-BEGIN
    // The screen mode with the world lock, Pimax tangents (outer 1.5293, inner 1.0324, vertical 1.2648 both ways), distance 3.35, for the three
    // published poses A, B, I (see kPoseA..kPoseI). Name: pose-eye. Reviewed by hand for A: the panel's centre in view space is (2.13, -1.9748,
    // -1.4136), +0.0315 in the left eye; m00 = 2/2.5617 = 0.780732, m02 = -/+0.193973, m11 = 2/2.5296 = 0.790639, m12 = 0; so cb2[4] =
    // (m00*2.1615 + m02*-1.4136, m11*-1.9748, 0.7068, 1.4136) = (1.96175, -1.56135, 0.7068, 1.4136) -- the left eye above, as below.
    {"A-left", {
        1.0f, 1.0f, 0.0f, 0.0f,   // cb2[0]
        -1.41985583f, -0.787124157f, 1.70666492f, 3.41332984f,   // cb2[1]
        0.135780931f, 1.89753318f, 0.349999994f, 0.699999988f,   // cb2[2]
        0.0f, 0.0f, 0.0f, 0.0f,   // cb2[3]
        1.96175122f, -1.56135356f, 0.706799924f, 1.41359985f   // cb2[4]
    }},
    {"A-right", {
        1.0f, 1.0f, 0.0f, 0.0f,   // cb2[0]
        -2.74404168f, -0.787124157f, 1.70666492f, 3.41332984f,   // cb2[1]
        -0.135780931f, 1.89753318f, 0.349999994f, 0.699999988f,   // cb2[2]
        0.0f, 0.0f, 0.0f, 0.0f,   // cb2[3]
        1.36416531f, -1.56135356f, 0.706799924f, 1.41359985f   // cb2[4]
    }},
    {"B-left", {
        1.0f, 1.0f, 0.0f, 0.0f,   // cb2[0]
        -3.1897397f, 1.26502097f, -1.06666553f, -2.13333106f,   // cb2[1]
        0.29095912f, 1.58127773f, 0.75f, 1.5f,   // cb2[2]
        0.0f, 0.0f, 0.0f, 0.0f,   // cb2[3]
        -1.29997981f, -2.17267561f, 0.831999898f, 1.6639998f   // cb2[4]
    }},
    {"B-right", {
        1.0f, 1.0f, 0.0f, 0.0f,   // cb2[0]
        -2.36212349f, 1.26502097f, -1.06666553f, -2.13333106f,   // cb2[1]
        -0.29095912f, 1.58127773f, 0.75f, 1.5f,   // cb2[2]
        0.0f, 0.0f, 0.0f, 0.0f,   // cb2[3]
        -1.99470723f, -2.17267561f, 0.831999898f, 1.6639998f   // cb2[4]
    }},
    // The identity at the origin, the closed form: cb2[1].x = -W*m00 = -4.44444*0.780732 = -3.46991, cb2[2].y = H*m11 = 2.5*0.790639 = 1.97660,
    // cb2[4] = (m00*0.0315 -/+ m02*3.35, 0, 3.35/2, 3.35) = (+/-0.67440, 0, 1.675, 3.35). The signed zeros (-0.0) are the arithmetic's own.
    {"I-left", {
        1.0f, 1.0f, 0.0f, 0.0f,   // cb2[0]
        -3.46991444f, -0.0f, 0.0f, 0.0f,   // cb2[1]
        0.0f, 1.97659719f, -0.0f, -0.0f,   // cb2[2]
        0.0f, 0.0f, 0.0f, 0.0f,   // cb2[3]
        0.67440176f, -0.0f, 1.67499995f, 3.3499999f   // cb2[4]
    }},
    {"I-right", {
        1.0f, 1.0f, 0.0f, 0.0f,   // cb2[0]
        -3.46991444f, -0.0f, 0.0f, 0.0f,   // cb2[1]
        0.0f, 1.97659719f, -0.0f, -0.0f,   // cb2[2]
        0.0f, 0.0f, 0.0f, 0.0f,   // cb2[3]
        -0.67440176f, -0.0f, 1.67499995f, 3.3499999f   // cb2[4]
    }},
    // fix.intro_video = head: the movie's own constants with cb2[0] scaled by the splash's half-width in NDC (1.0440) over the panel's
    // (512 * 0.000368732 = 0.18879): x5.52993, so 512 -> 2831.32 and 288 -> 1592.62; every other float is the game's, to the bit.
    {"head-left", {
        2831.32471f, 1592.62012f, 0.0f, 0.0f,   // cb2[0]
        -0.000368731999f, 0.0f, 0.0f, 0.0f,   // cb2[1]
        0.0f, 0.000373413f, 0.0f, 0.0f,   // cb2[2]
        0.0f, 0.0f, 0.0f, 0.0f,   // cb2[3]
        0.193906993f, 0.0f, 0.0f, 1.0f   // cb2[4]
    }},
    {"head-right", {
        2831.32471f, 1592.62012f, 0.0f, 0.0f,   // cb2[0]
        -0.000368731999f, 0.0f, 0.0f, 0.0f,   // cb2[1]
        0.0f, 0.000373413f, 0.0f, 0.0f,   // cb2[2]
        0.0f, 0.0f, 0.0f, 0.0f,   // cb2[3]
        -0.194000006f, 0.0f, 0.0f, 1.0f   // cb2[4]
    }},
    // The same lock on an asymmetric vertical frustum (a Quest 3: t = -1.4281, b = +0.9657; horizontal outer 1.2000, inner 1.0155), pose A, so
    // m11 = 2/2.3938 = 0.835492 and m12 = (0.9657 - 1.4281)/2.3938 = -0.19318 -- the field's shear. cb2[4].y = m11*-1.9748 + m12*-1.4136 =
    // -1.64986 + 0.27308 = -1.37678, as below.
    {"Q-left", {
        1.0f, 1.0f, 0.0f, 0.0f,   // cb2[0]
        -2.12302828f, -0.172439277f, 1.70666492f, 3.41332984f,   // cb2[1]
        0.0582938753f, 2.14039588f, 0.349999994f, 0.699999988f,   // cb2[2]
        0.0f, 0.0f, 0.0f, 0.0f,   // cb2[3]
        2.06897306f, -1.3768698f, 0.706799924f, 1.41359985f   // cb2[4]
    }},
    {"Q-right", {
        1.0f, 1.0f, 0.0f, 0.0f,   // cb2[0]
        -2.69153166f, -0.172439277f, 1.70666492f, 3.41332984f,   // cb2[1]
        -0.0582938753f, 2.14039588f, 0.349999994f, 0.699999988f,   // cb2[2]
        0.0f, 0.0f, 0.0f, 0.0f,   // cb2[3]
        1.77666044f, -1.3768698f, 0.706799924f, 1.41359985f   // cb2[4]
    }},
    // THE CURVED MOVIE (fix.panel_curvature 0.3; step 2): the flat panel's cb2 with cb2[3] = the seated +z axis through the same projection -- the
    // view-space image of +z, cz = (A(0,2), A(1,2), A(2,2)) = the pose's third row, unit length -- as (m00 cz.x + m02 cz.z, m11 cz.y + m12 cz.z,
    // -cz.z / 2, -cz.z). Every other float is the flat panel's, to the bit (the A, B and I goldens are the flat A, B and I goldens with column 3
    // filled; no pose keeps the movie flat, so A and B -- a head turned 53 degrees, 37 degrees the other way, where a nearer edge is beside or
    // behind the eye -- are curved goldens too, as C, D and I).
    // Reviewed by hand. I (cz = (0, 0, 1)): column 3 = (m02, m12, -0.5, -1) = (-/+0.193973, 0, -0.5, -1), the left eye with the negative. C (a head
    // yawed 16.26 degrees, cz = (-0.28, 0, 0.96)): x = 0.780732 * -0.28 -/+ 0.193973 * 0.96 = -0.404819 left, -0.032391 right; w = -0.96, z =
    // -0.48. D is C mirrored: cz = (0.28, 0, 0.96), so +0.032391 and +0.404819 with the eyes swapped. A (cz = (-0.8, 0.168, 0.576)): x =
    // 0.780732 * -0.8 -/+ 0.193973 * 0.576 = -0.736315 left, -0.512857 right; y = 0.790639 * 0.168 = 0.132828; z = -0.288, w = -0.576. B (cz = (0.6,
    // 0.48, 0.64)): x = 0.780732 * 0.6 -/+ 0.193973 * 0.64 = 0.344296 left, 0.592582 right; y = 0.790639 * 0.48 = 0.379507; z = -0.32, w = -0.64.
    // QC (the Quest-like vertical frustum, pose C): y = m12 * 0.96 = -0.19318 * 0.96 = -0.185439, and x = 0.902731 * -0.28 + (-0.083277 left,
    // +0.083277 right) * 0.96 = -0.332711 and -0.172819.
    {"A-left-curved", {
        1.0f, 1.0f, 0.0f, 0.0f,   // cb2[0]
        -1.41985583f, -0.787124157f, 1.70666492f, 3.41332984f,   // cb2[1]
        0.135780931f, 1.89753318f, 0.349999994f, 0.699999988f,   // cb2[2]
        -0.736313581f, 0.132827327f, -0.287999988f, -0.575999975f,   // cb2[3]
        1.96175122f, -1.56135356f, 0.706799924f, 1.41359985f   // cb2[4]
    }},
    {"A-right-curved", {
        1.0f, 1.0f, 0.0f, 0.0f,   // cb2[0]
        -2.74404168f, -0.787124157f, 1.70666492f, 3.41332984f,   // cb2[1]
        -0.135780931f, 1.89753318f, 0.349999994f, 0.699999988f,   // cb2[2]
        -0.51285696f, 0.132827327f, -0.287999988f, -0.575999975f,   // cb2[3]
        1.36416531f, -1.56135356f, 0.706799924f, 1.41359985f   // cb2[4]
    }},
    {"B-left-curved", {
        1.0f, 1.0f, 0.0f, 0.0f,   // cb2[0]
        -3.1897397f, 1.26502097f, -1.06666553f, -2.13333106f,   // cb2[1]
        0.29095912f, 1.58127773f, 0.75f, 1.5f,   // cb2[2]
        0.344296396f, 0.379506648f, -0.319999993f, -0.639999986f,   // cb2[3]
        -1.29997981f, -2.17267561f, 0.831999898f, 1.6639998f   // cb2[4]
    }},
    {"B-right-curved", {
        1.0f, 1.0f, 0.0f, 0.0f,   // cb2[0]
        -2.36212349f, 1.26502097f, -1.06666553f, -2.13333106f,   // cb2[1]
        -0.29095912f, 1.58127773f, 0.75f, 1.5f,   // cb2[2]
        0.592581511f, 0.379506648f, -0.319999993f, -0.639999986f,   // cb2[3]
        -1.99470723f, -2.17267561f, 0.831999898f, 1.6639998f   // cb2[4]
    }},
    {"C-left-curved", {
        1.0f, 1.0f, 0.0f, 0.0f,   // cb2[0]
        -3.08972979f, -0.0f, 0.622221589f, 1.24444318f,   // cb2[1]
        0.0f, 1.97659719f, -0.0f, -0.0f,   // cb2[2]
        -0.404818654f, 0.0f, -0.479999989f, -0.959999979f,   // cb2[3]
        1.23025274f, -0.0395319425f, 1.5259999f, 3.05199981f   // cb2[4]
    }},
    {"C-right-curved", {
        1.0f, 1.0f, 0.0f, 0.0f,   // cb2[0]
        -3.57250595f, -0.0f, 0.622221589f, 1.24444318f,   // cb2[1]
        0.0f, 1.97659719f, -0.0f, -0.0f,   // cb2[2]
        -0.0323909968f, 0.0f, -0.479999989f, -0.959999979f,   // cb2[3]
        -0.00294286013f, -0.0395319425f, 1.5259999f, 3.05199981f   // cb2[4]
    }},
    {"D-left-curved", {
        1.0f, 1.0f, 0.0f, 0.0f,   // cb2[0]
        -3.57250595f, 0.0f, -0.622221589f, -1.24444318f,   // cb2[1]
        0.0f, 1.97659719f, -0.0f, -0.0f,   // cb2[2]
        0.0323909968f, 0.0f, -0.479999989f, -0.959999979f,   // cb2[3]
        0.00294286013f, -0.0395319425f, 1.5259999f, 3.05199981f   // cb2[4]
    }},
    {"D-right-curved", {
        1.0f, 1.0f, 0.0f, 0.0f,   // cb2[0]
        -3.08972979f, 0.0f, -0.622221589f, -1.24444318f,   // cb2[1]
        0.0f, 1.97659719f, -0.0f, -0.0f,   // cb2[2]
        0.404818654f, 0.0f, -0.479999989f, -0.959999979f,   // cb2[3]
        -1.23025274f, -0.0395319425f, 1.5259999f, 3.05199981f   // cb2[4]
    }},
    {"I-left-curved", {
        1.0f, 1.0f, 0.0f, 0.0f,   // cb2[0]
        -3.46991444f, -0.0f, 0.0f, 0.0f,   // cb2[1]
        0.0f, 1.97659719f, -0.0f, -0.0f,   // cb2[2]
        -0.193972751f, 0.0f, -0.5f, -1.0f,   // cb2[3]
        0.67440176f, -0.0f, 1.67499995f, 3.3499999f   // cb2[4]
    }},
    {"I-right-curved", {
        1.0f, 1.0f, 0.0f, 0.0f,   // cb2[0]
        -3.46991444f, -0.0f, 0.0f, 0.0f,   // cb2[1]
        0.0f, 1.97659719f, -0.0f, -0.0f,   // cb2[2]
        0.193972751f, 0.0f, -0.5f, -1.0f,   // cb2[3]
        -0.67440176f, -0.0f, 1.67499995f, 3.3499999f   // cb2[4]
    }},
    {"QC-left-curved", {
        1.0f, 1.0f, 0.0f, 0.0f,   // cb2[0]
        -3.74801397f, 0.240383729f, 0.622221589f, 1.24444318f,   // cb2[1]
        0.0f, 2.08872914f, -0.0f, -0.0f,   // cb2[2]
        -0.332710534f, -0.185439065f, -0.479999989f, -0.959999979f,   // cb2[3]
        0.992143631f, 0.547767103f, 1.5259999f, 3.05199981f   // cb2[4]
    }},
    {"QC-right-curved", {
        1.0f, 1.0f, 0.0f, 0.0f,   // cb2[0]
        -3.95528078f, 0.240383729f, 0.622221589f, 1.24444318f,   // cb2[1]
        0.0f, 2.08872914f, -0.0f, -0.0f,   // cb2[2]
        -0.17281875f, -0.185439065f, -0.479999989f, -0.959999979f,   // cb2[3]
        0.426949114f, 0.547767103f, 1.5259999f, 3.05199981f   // cb2[4]
    }},
    // GOLDENS-END
};
const Golden* findGolden(const char* name) {
    for (const Golden& g : kGoldens)
        if (!std::strcmp(g.name, name)) return &g;
    return nullptr;
}

std::string hexOf(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return fmt("0x%08X", u);
}

constexpr double kPiD = 3.14159265358979323846;
constexpr double kHalfW = 4.44444, kHalfH = 2.5;
// The panel's basis in view space, from the comments and nothing of the code, in double: cx and cy the panel's x and y columns (half-extents
// included), cz the seated +z axis -- from the panel toward the viewer, unit length -- and c0 the panel's centre, for one eye.
struct Basis {
    double cx[3], cy[3], cz[3], c0[3];
};
Basis expectedBasis(const float* pose, bool left, double dist) {
    double R[3][3], t[3];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) R[r][c] = pose[r * 4 + c];
        t[r] = pose[r * 4 + 3];
    }
    const double halfIpd = 0.0315;
    auto A = [&](int i, int j) { return R[j][i]; };   // the head's inverse is the rotation's transpose
    Basis b{};
    for (int i = 0; i < 3; ++i) {
        b.cx[i] = A(i, 0) * -kHalfW;   // the panel's +x is the viewer's LEFT (the game's own convention on both of its panels)
        b.cy[i] = A(i, 1) * kHalfH;
        b.cz[i] = A(i, 2);
        b.c0[i] = A(i, 0) * (0.0 - t[0]) + A(i, 1) * (0.0 - t[1]) + A(i, 2) * (-dist - t[2]);   // the panel at (0,0,-dist) seen from the head
    }
    b.c0[0] -= left ? -halfIpd : halfIpd;
    return b;
}

// FIXTURE ONLY, for curved-no-edge-test: where a bent panel's NEARER edge midpoint (x = -1 or +1, y = 0) lands in view space, z, in double: the
// centre plus x' cx plus z' cz with the strip's own arc x' = sin(pi c) / (pi c) and z' = W (1 - cos(pi c)) / (pi c) (panel_curve.cpp, the gain
// W the panel's half-width in metres). Negative is in front of the eye. The module has no test of it (the bent edges are not tested); this is
// here to show that a pose and distance a scenario picks really is one where a test of it WOULD have bitten -- the nearer edge at or behind the
// eye -- so the scenario means what it says if the geometry is ever changed.
double nearerEdgeViewZ(const float* pose, bool left, double dist, double curvature) {
    const Basis b = expectedBasis(pose, left, dist);
    const double k = kPiD * curvature, xe = std::sin(k) / k, ze = kHalfW * (1.0 - std::cos(k)) / k;
    return std::max(b.c0[2] - xe * b.cx[2] + ze * b.cz[2], b.c0[2] + xe * b.cx[2] + ze * b.cz[2]);
}

// What cb2 should be, from buildWorldCb's own comments and nothing of its code: the panel's three basis vectors and centre brought into view space
// by the head's inverse, the eye's lateral offset, and the eye's asymmetric frustum, as columns of Proj * View * Model with cb2[0] = (1, 1),
// cb2[3] zero for a flat panel -- or, for a bent one (`curved`), the z column: cz through the same projection. z = w/2. In double.
std::array<double, 20> expectedWorld(const float* pose, bool left, double dist, double outer, double inner, double top, double bot, bool curved = false) {
    const Basis b = expectedBasis(pose, left, dist);
    const double lt = left ? -outer : -inner, rt = left ? inner : outer, tp = -top, bt = bot;
    const double m00 = 2.0 / (rt - lt), m02 = (rt + lt) / (rt - lt), m11 = 2.0 / (bt - tp), m12 = (bt + tp) / (bt - tp);
    auto col = [&](const double* v, double* d) {
        d[0] = m00 * v[0] + m02 * v[2];
        d[1] = m11 * v[1] + m12 * v[2];
        d[3] = -v[2];
        d[2] = 0.5 * d[3];
    };
    std::array<double, 20> out{};
    out[0] = out[1] = 1.0;
    col(b.cx, &out[4]);
    col(b.cy, &out[8]);
    if (curved) col(b.cz, &out[12]);
    col(b.c0, &out[16]);
    return out;
}

// "" when the 80 bytes are the cb2 the arithmetic gives, else the first float that is not.
std::string expectedProblem(const std::vector<uint8_t>& bytes, const std::array<double, 20>& want) {
    if (bytes.size() != 80) return fmt("%zu bytes, want 80", bytes.size());
    float got[20];
    std::memcpy(got, bytes.data(), 80);
    for (int i = 0; i < 20; ++i)
        if (!closeTo(got[i], want[static_cast<size_t>(i)], 5e-6 * (1.0 + std::fabs(want[static_cast<size_t>(i)])))) return fmt("cb2 float %d = %.9g, the arithmetic gives %.9g", i, got[i], want[static_cast<size_t>(i)]);
    return std::string();
}

// "" when the 80 bytes are the golden's, to the bit.
std::string goldenProblem(const std::vector<uint8_t>& bytes, const Golden* g) {
    if (!g) return "no golden of that name";
    if (bytes.size() != 80) return fmt("%zu bytes, want 80", bytes.size());
    if (std::memcmp(bytes.data(), g->v, 80) == 0) return std::string();
    float got[20];
    std::memcpy(got, bytes.data(), 80);
    for (int i = 0; i < 20; ++i)
        if (std::memcmp(&got[i], &g->v[i], 4) != 0) return fmt("cb2 float %d = %.9g (%s), golden %.9g (%s)", i, got[i], hexOf(got[i]).c_str(), g->v[i], hexOf(g->v[i]).c_str());
    return "bytes differ";
}

// The table's text for one golden, each line prefixed "EMIT " (--emit-golden strips it) so it can be told from the scenario's other output.
void emitGolden(const std::string& name, const std::vector<uint8_t>& bytes) {
    if (bytes.size() != 80) {
        std::printf("EMIT     // %s: %zu bytes\n", name.c_str(), bytes.size());
        return;
    }
    float f[20];
    std::memcpy(f, bytes.data(), 80);
    std::printf("EMIT     {\"%s\", {\n", name.c_str());
    for (int row = 0; row < 5; ++row) {
        std::string line = "EMIT         ";
        for (int k = 0; k < 4; ++k) {
            const float v = f[row * 4 + k];
            uint32_t u;
            std::memcpy(&u, &v, 4);
            if (u == 0u) line += "0.0f";
            else if (u == 0x80000000u) line += "-0.0f";
            else {
                std::string num = fmt("%.9g", static_cast<double>(v));
                if (num.find_first_of(".en") == std::string::npos) num += ".0";   // 1 is not a float literal; 1.0f is
                line += num + "f";
            }
            line += (row == 4 && k == 3) ? "" : ",";
            if (k < 3) line += " ";
        }
        line += fmt("   // cb2[%d]", row);
        std::printf("%s\n", line.c_str());
    }
    std::printf("EMIT     }},\n");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The GPU: a WARP device and the game's state at the intro composite.
// ---------------------------------------------------------------------------------------------------------------------------------
struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Buffer> quadVb, quadIb, cb0, cb1, cb3, eyeCb[2], extraCb, smallCb;
    ComPtr<ID3D11Texture2D> srvTex, rtTex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> rs;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> dss;
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
    const PFN_D3D11_CREATE_DEVICE create = systemD3D11CreateDevice();
    if (!create) return g;
    D3D_FEATURE_LEVEL fl{};
    if (FAILED(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g.dev, &fl, &g.ctx)) || !g.dev || !g.ctx) return g;
    ID3D11Device* dev = g.dev.Get();

    // The unit quad the composite draws (the same mesh shape as the on-foot screen's: stride 20, corners at +-1) and its index pattern.
    const float quad[20] = {-1, -1, 0, 0, 1,   1, -1, 0, 1, 1,   -1, 1, 0, 0, 0,   1, 1, 0, 1, 0};
    const uint16_t pattern[6] = {0, 3, 1, 0, 2, 3};
    g.quadVb = makeBuffer(dev, sizeof(quad), D3D11_BIND_VERTEX_BUFFER, quad);
    g.quadIb = makeBuffer(dev, sizeof(pattern), D3D11_BIND_INDEX_BUFFER, pattern);
    float cb0[52];
    for (int i = 0; i < 52; ++i) cb0[i] = 100.0f + static_cast<float>(i);
    g.cb0 = makeBuffer(dev, sizeof(cb0), D3D11_BIND_CONSTANT_BUFFER, cb0);
    const float cb1[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    g.cb1 = makeBuffer(dev, sizeof(cb1), D3D11_BIND_CONSTANT_BUFFER, cb1);
    const float cb3[4] = {7, 8, 9, 10};
    g.cb3 = makeBuffer(dev, sizeof(cb3), D3D11_BIND_CONSTANT_BUFFER, cb3);
    // The game's cb2: one 80-byte buffer per eye (the census), a third that must be refused, and one too small to be cb2.
    g.eyeCb[0] = makeBuffer(dev, 80, D3D11_BIND_CONSTANT_BUFFER, kStockL);
    g.eyeCb[1] = makeBuffer(dev, 80, D3D11_BIND_CONSTANT_BUFFER, kStockR);
    g.extraCb = makeBuffer(dev, 80, D3D11_BIND_CONSTANT_BUFFER, kStockL);
    g.smallCb = makeBuffer(dev, 64, D3D11_BIND_CONSTANT_BUFFER, kStockL);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 16;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    dev->CreateTexture2D(&td, nullptr, &g.srvTex);
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    dev->CreateTexture2D(&td, nullptr, &g.rtTex);
    if (g.srvTex) dev->CreateShaderResourceView(g.srvTex.Get(), nullptr, &g.srv);
    if (g.rtTex) dev->CreateRenderTargetView(g.rtTex.Get(), nullptr, &g.rtv);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &g.sampler);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &g.rs);
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bd, &g.blend);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable = FALSE;
    dd.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
    dd.BackFace = dd.FrontFace;
    dev->CreateDepthStencilState(&dd, &g.dss);

    g.ok = g.quadVb && g.quadIb && g.cb0 && g.cb1 && g.cb3 && g.eyeCb[0] && g.eyeCb[1] && g.extraCb && g.smallCb && g.srv && g.rtv && g.sampler && g.rs && g.blend && g.dss;
    return g;
}

// The pipeline around the draw, bound once: if the module changes any of it, it stays changed and every later check sees it.
void bindPipeline(Gpu& g) {
    ID3D11DeviceContext* c = g.ctx.Get();
    ID3D11Buffer* vb = g.quadVb.Get();
    UINT stride = 20, offset = 0;
    c->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    c->IASetIndexBuffer(g.quadIb.Get(), DXGI_FORMAT_R16_UINT, 0);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* cbs[4] = {g.cb0.Get(), g.cb1.Get(), g.eyeCb[0].Get(), g.cb3.Get()};
    c->VSSetConstantBuffers(0, 4, cbs);
    ID3D11ShaderResourceView* srv = g.srv.Get();
    c->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* smp = g.sampler.Get();
    c->PSSetSamplers(0, 1, &smp);
    c->RSSetState(g.rs.Get());
    D3D11_VIEWPORT vp{0.0f, 0.0f, 5424.0f, 5356.0f, 0.0f, 1.0f};   // one eye of the measured rig
    c->RSSetViewports(1, &vp);
    const float factor[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    c->OMSetBlendState(g.blend.Get(), factor, 0xFFFFFFFEu);
    c->OMSetDepthStencilState(g.dss.Get(), 7);
    ID3D11RenderTargetView* rtv = g.rtv.Get();
    c->OMSetRenderTargets(1, &rtv, nullptr);
}

void setEyeConstants(Gpu& g, int eye, const float* f20) { g.ctx->UpdateSubresource(g.eyeCb[eye].Get(), 0, nullptr, f20, 0, 0); }

// How many references the game's cb2 buffers carry. The module takes one with VSGetConstantBuffers and gives it back; it keeps the
// pointer (as a key and as the restore target) and no reference. Measured around a call with the rig's own references alive at both ends.
struct Refs {
    ULONG e0 = 0, e1 = 0, x = 0;
    bool operator==(const Refs& o) const { return e0 == o.e0 && e1 == o.e1 && x == o.x; }
};
ULONG refCount(IUnknown* u) {
    if (!u) return 0;
    u->AddRef();
    return u->Release();
}
Refs takeRefs(const Gpu& g) { return Refs{refCount(g.eyeCb[0].Get()), refCount(g.eyeCb[1].Get()), refCount(g.extraCb.Get())}; }

// ---------------------------------------------------------------------------------------------------------------------------------
// A composite as vscreen.cpp makes it: ask the module, forward the game's draw, and call EndDraw if (and only if) it said yes.
// ---------------------------------------------------------------------------------------------------------------------------------
struct Step {
    bool bound = false;
    bool armed = false;        // introPanelStripArmed() between the module's answer and the game's draw: the strip is to be drawn, not the quad
    bool armedAfter = false;   // ... and after introPanelEndDraw, or after the answer when it was false (nothing is armed past a draw)
    bool reverseU = false;       // introPanelStripReverseU() at the draw: the strip's u runs against its x (the bound placement's +x runs left)
    bool reverseUAfter = false;  // ... and after introPanelEndDraw
    DrawRecord draw;
    Snapshot before, after;
    Refs refs0, refs1;
    ID3D11Buffer* game = nullptr;   // the game's buffer for this draw
    uint32_t reqCount = 6, reqInstances = 1;   // the draw's shape as the caller asked about it
};

Step compositeWith(Gpu& g, ID3D11Buffer* cb, uint32_t srvW = kFillW, uint32_t srvH = kFillH, char kind = 'X', uint32_t count = 6, uint32_t instances = 1) {
    ID3D11DeviceContext* ctx = g.ctx.Get();
    Step s;
    s.game = cb;
    s.reqCount = count;
    s.reqInstances = instances;
    ID3D11Buffer* bind = cb;
    ctx->VSSetConstantBuffers(2, 1, &bind);
    s.before = takeSnapshot(ctx);
    s.refs0 = takeRefs(g);
    s.bound = introPanelOnComposite(ctx, kind, count, instances, srvW, srvH);
    s.armed = introPanelStripArmed();
    s.reverseU = introPanelStripReverseU();
    recordDraw(ctx, count, instances, 0, 0, 0);   // the game's own draw, forwarded either way
    s.draw = g_draws.back();
    if (s.bound) introPanelEndDraw(ctx);
    s.armedAfter = introPanelStripArmed();
    s.reverseUAfter = introPanelStripReverseU();
    s.refs1 = takeRefs(g);
    s.after = takeSnapshot(ctx);
    if (g_trace)
        std::printf("TRACE %s: composite bound=%d refs %lu/%lu/%lu -> %lu/%lu/%lu\n", g_scn, s.bound, s.refs0.e0, s.refs0.e1, s.refs0.x, s.refs1.e0, s.refs1.e1, s.refs1.x);
    return s;
}
Step compositeStep(Gpu& g, int eye) { return compositeWith(g, g.eyeCb[eye].Get()); }

struct FrameResult {
    Step eye[2];
};
// One frame as the game plays it: the fill, both eyes' composites, the frame edge.
FrameResult runFrame(Gpu& g, bool fill = true, bool scene = false) {
    if (fill) introPanelNoteFill(kFillW, kFillH);
    FrameResult fr;
    fr.eye[0] = compositeStep(g, 0);
    fr.eye[1] = compositeStep(g, 1);
    introPanelTick(g.ctx.Get(), scene);
    return fr;
}

// The contract of one composite, bound or not: the draw's arguments are the game's, the pipeline at the draw is the game's except VS b2
// (ours when bound, the game's own buffer when not), the game's buffer is back afterwards, and no reference is left behind. cb2: -1 follows
// `wantBound`; 0 says VS b2 is the game's even though the call returned true (the resampler alone matched); 1 says ours. wantArmed: the strip is
// armed at the draw (the movie is bent) -- false everywhere at curvature 0, in every refused, stock, head and settling step at any curvature, and
// after the draw in every case.
void expectStep(const Step& s, bool wantBound, const char* what, Gpu& g, int cb2 = -1, bool wantArmed = false, int wantReverse = -1) {
    const std::string w = lab(what);
    const bool reverse = wantReverse < 0 ? wantArmed : wantReverse != 0;   // only curved-right-running says otherwise: its placement's +x runs right
    const bool ours = cb2 < 0 ? wantBound : cb2 != 0;
    check(s.bound == wantBound, w + "-returned", fmt("introPanelOnComposite returned %d, want %d", s.bound, wantBound));
    check(s.armed == wantArmed, w + "-armed", fmt("introPanelStripArmed() was %d at the draw, want %d", s.armed, wantArmed));
    check(!s.armedAfter, w + "-armed-after", "introPanelStripArmed() was still true after the draw (introPanelEndDraw) was made");
    // Which way the strip's u runs. An armed draw is the movie's own placement, whose +x runs to the viewer's LEFT (cb2[1].x is negative: the game's
    // convention on both of its panels), so u runs AGAINST x: introPanelStripReverseU() is true at the draw, and agrees with what the 20 bytes bound
    // say by the module's own rule (introPlacementXDir) and by a minor computed here, in double, from the same floats. Everywhere else it is
    // false -- at curvature 0, at stock and head, in the settle frames, when the lock is refused, for a draw that is not armed -- and false after
    // introPanelEndDraw in every case.
    check(s.reverseU == reverse, w + "-reverse", fmt("introPanelStripReverseU() was %d at the draw, want %d (true exactly for an armed draw: the movie's +x runs left)", s.reverseU, reverse));
    check(!s.reverseUAfter, w + "-reverse-after", "introPanelStripReverseU() was still true after the draw (introPanelEndDraw) was made");
    if (wantArmed && ours && s.draw.cb2Bytes.size() == 80) {
        float f[20];
        std::memcpy(f, s.draw.cb2Bytes.data(), 80);
        const double minor = static_cast<double>(f[4]) * f[19] - static_cast<double>(f[16]) * f[7];   // cb2[1].x * cb2[4].w - cb2[4].x * cb2[1].w
        const IntroXDir dir = introPlacementXDir(f);
        check((reverse ? minor < 0.0 : minor > 0.0) && dir == (reverse ? IntroXDir::kLeft : IntroXDir::kRight) && s.reverseU == (dir == IntroXDir::kLeft), w + "-xdir",
              fmt("the bound constants do not read as running +x to the %s (minor %.6g, introPlacementXDir %d), or the flag disagrees with the reading", reverse ? "left" : "right", minor, static_cast<int>(dir)));
    }
    check(s.draw.count == s.reqCount && s.draw.instances == s.reqInstances && s.draw.start == 0 && s.draw.base == 0 && s.draw.startInstance == 0, w + "-arguments",
          fmt("the game's draw was (%u, %u, %u, %d, %u)", s.draw.count, s.draw.instances, s.draw.start, s.draw.base, s.draw.startInstance));
    const unsigned d = diffBits(s.before, s.draw.snap);
    check(d == (ours ? kDiffCb2 : 0u), w + "-pipeline", fmt("at the draw the pipeline differs from the game's in bits 0x%X, want 0x%X (VS b2 only when ours)", d, ours ? kDiffCb2 : 0u));
    if (ours) {
        ID3D11Buffer* at = s.draw.snap.cb[2];
        check(at && at != g.eyeCb[0].Get() && at != g.eyeCb[1].Get() && at != g.extraCb.Get() && at != g.smallCb.Get(), w + "-ours", "VS b2 at the draw is not our own buffer");
        check(s.draw.cb2Desc.ByteWidth == 80 && s.draw.cb2Desc.Usage == D3D11_USAGE_DYNAMIC && s.draw.cb2Desc.BindFlags == D3D11_BIND_CONSTANT_BUFFER &&
                  s.draw.cb2Desc.CPUAccessFlags == D3D11_CPU_ACCESS_WRITE,
              w + "-ours-desc", "our buffer is not an 80-byte dynamic constant buffer");
    } else {
        check(s.draw.snap.cb[2] == s.game, w + "-game", "VS b2 at the draw is not the game's own buffer");
    }
    check(diffBits(s.before, s.after) == 0, w + "-restored", fmt("after the call the pipeline differs from before in bits 0x%X", diffBits(s.before, s.after)));
    check(s.refs0 == s.refs1, w + "-refs", fmt("references on the game's cb2 buffers went from %lu/%lu/%lu to %lu/%lu/%lu", s.refs0.e0, s.refs0.e1, s.refs0.x, s.refs1.e0, s.refs1.e1, s.refs1.x));
}

void setPose(const float* p) { std::memcpy(stub::pose, p, sizeof(stub::pose)); }

// g_curvature: the curvature the curved-* wrappers of the older scenarios run them at (fix.panel_curvature), when the call does not say; null
// leaves the key unset, which is the shipped 0. configure() hands the same Config to panel_curve.cpp (the surface strip is wanted from it) and
// to the intro module (which reads only its own keys), as the DLL's config reload does.
const char* g_curvature = nullptr;
double g_liveCurvature = 0.0;   // what fix.panel_curvature was last set to (0 until it is): what boundFrame expects the module to bend by
void configure(const char* video, const char* distance = nullptr, const char* curvature = nullptr, const char* segments = nullptr) {
    Config& c = Config::get();
    if (video) c.set("fix.intro_video", video);
    if (distance) c.set("advanced.intro_video_distance", distance);
    if (!curvature) curvature = g_curvature;
    if (curvature) {
        c.set("fix.panel_curvature", curvature);
        g_liveCurvature = std::atof(curvature);
    }
    if (segments) c.set("advanced.panel_curvature_segments", segments);
    introPanelConfigure(c);
    panelCurveConfigure(c);
}

// Frames until the buffers have settled: the first composite of a buffer starts a readback, retired at the fourth frame edge after it.
constexpr int kSettleFrames = 4;
void settle(Gpu& g, const char* what, bool expectFalse = true) {
    for (int f = 0; f < kSettleFrames; ++f) {
        FrameResult fr = runFrame(g);
        for (int e = 0; e < 2; ++e) {
            g_note = fmt("settle frame %d, %s eye", f, e ? "right" : "left");
            if (expectFalse) expectStep(fr.eye[e], false, what, g);
        }
    }
    g_note.clear();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// golden, golden-zero: the sequence and the bytes.
// ---------------------------------------------------------------------------------------------------------------------------------
void goldenBody(Gpu& g, bool zeroKey) {
    ID3D11DeviceContext* ctx = g.ctx.Get();
    setPose(kPoseA);
    check(beginLog(), lab("log"), "the scratch log opens");
    configure(nullptr, nullptr, zeroKey ? "0" : nullptr);   // fix.intro_video absent: the default is screen; the distance default 3.35
    check(introPanelWants(), lab("wants"), "the default (fix.intro_video absent) is screen: the panel is wanted");

    // The settle frames: the first composite of each buffer starts the readback and the module answers false, leaving VS b2 the game's.
    settle(g, "settle", true);
    check(stub::recentreRequests == 1, lab("recentre-once"), fmt("the vr half was asked to recentre %u times in the first frames, want once (the first screen composite)", stub::recentreRequests));

    struct Bound {
        const char* name;
        const float* pose;
    };
    const Bound frames[] = {{"A", kPoseA}, {"B", kPoseB}, {"I", kPoseI}, {"A", kPoseA}};
    ID3D11Buffer* oursL = nullptr;
    ID3D11Buffer* oursR = nullptr;
    std::map<std::string, std::vector<uint8_t>> bytes;
    int boundDraws = 0;
    for (size_t k = 0; k < sizeof(frames) / sizeof(frames[0]); ++k) {
        setPose(frames[k].pose);
        FrameResult fr = runFrame(g);
        for (int e = 0; e < 2; ++e) {
            const Step& s = fr.eye[e];
            g_note = fmt("bound frame %zu (pose %s), %s eye", k, frames[k].name, e ? "right" : "left");
            expectStep(s, true, "bound", g);
            if (!s.bound) continue;
            ++boundDraws;
            ID3D11Buffer* ours = s.draw.snap.cb[2];
            ID3D11Buffer*& keep = e ? oursR : oursL;
            if (!keep) keep = ours;
            check(keep == ours, lab("ours-stable"), "the dynamic buffer for this eye is not the same object at every draw");
            const std::string gname = fmt("%s-%s", frames[k].name, e ? "right" : "left");
            const std::array<double, 20> want = expectedWorld(frames[k].pose, e == 0, 3.35, stub::outer, stub::inner, stub::top, stub::bot);
            const std::string prob = expectedProblem(s.draw.cb2Bytes, want);
            check(prob.empty(), lab("arithmetic"), "the 80 bytes are not what the comments' arithmetic gives (pose " + gname + "): " + prob);
            if (g_emit) bytes[gname] = s.draw.cb2Bytes;
            else {
                const std::string gp = goldenProblem(s.draw.cb2Bytes, findGolden(gname.c_str()));
                check(gp.empty(), lab("golden"), "pose and eye " + gname + ": " + gp);
            }
        }
    }
    g_note.clear();
    for (const auto& b : bytes) emitGolden(b.first, b.second);
    check(oursL && oursR && oursL != oursR, lab("two-buffers"), "the two eyes are not served from two distinct buffers of ours");

    // introPanelEndDraw is idempotent: a second call with another buffer at VS b2 must not put a stale one back.
    {
        ID3D11Buffer* other = g.extraCb.Get();   // neither eye's: a stale restore target would be one of those
        ctx->VSSetConstantBuffers(2, 1, &other);
        introPanelEndDraw(ctx);
        const Snapshot s = takeSnapshot(ctx);
        check(s.cb[2] == other, lab("endDraw-idempotent"), "an unpaired introPanelEndDraw changed VS b2");
    }

    // The game's own buffers were never written.
    {
        const std::vector<uint8_t> l = readBuffer(ctx, g.eyeCb[0].Get()), r = readBuffer(ctx, g.eyeCb[1].Get());
        check(l.size() == 80 && std::memcmp(l.data(), kStockL, 80) == 0 && r.size() == 80 && std::memcmp(r.data(), kStockR, 80) == 0, lab("game-untouched"), "the game's cb2 buffers changed");
    }
    // The resampler is asked about every matched composite (8 frames, two eyes each) and told every frame edge; introPanelEndDraw always
    // asks it to restore, bound or not (8 bound draws, and the one unpaired call above).
    check(stub::upBegin == 16 && stub::upEnd == static_cast<unsigned>(boundDraws) + 1 && stub::upFrameEnd == 8, lab("resampler"),
          fmt("introUpscaleBegin %u times (want 16), End %u (want %d), FrameEnd %u (want 8)", stub::upBegin, stub::upEnd, boundDraws + 1, stub::upFrameEnd));
    check(stub::lastSrv == g.srv.Get(), lab("resampler-source"), "the resampler was not handed the view the draw would have sampled (PS slot 0)");

    const std::string log = endLog();
    check(count(log, "asking the vr half to") == 1, lab("log-recentre"), "the recentre line is not written exactly once");
    check(count(log, "intro video lock: holding") == 1 && has(log, "left eye first") && has(log, "head yaw 53.1 deg"), lab("log-holding"),
          "the 'holding' line is not written once, with the left eye first and the head's yaw at bind");
    check(count(log, "intro video size: engaged") == 1, lab("log-engaged"), "the 'engaged' line is not written exactly once");
    check(count(log, "read the movie panel's own constants") == 2 && has(log, "half-size 512 x 288 pixels") && has(log, "centred at 0.1939") && has(log, "centred at -0.1940"), lab("log-read"),
          "the readback line, once per eye, with the movie's own numbers (512 x 288, centred at +0.1939 and -0.1940)");
    check(!has(log, "do not read as a screen-space") && !has(log, "lock: cannot tell") && !has(log, "FAULT") && !has(log, "intro video curve"), lab("log-clean"),
          "a refusal, a fault or a line about the curve was logged on the golden path (curvature 0, or the key unset, says nothing about it)");
}
void scnGolden(Gpu& g) { goldenBody(g, false); }
void scnGoldenZero(Gpu& g) { goldenBody(g, true); }

// ---------------------------------------------------------------------------------------------------------------------------------
// Pieces the scenarios below share.
// ---------------------------------------------------------------------------------------------------------------------------------
void setConstants(Gpu& g, const float* left, const float* right) {
    setEyeConstants(g, 0, left);
    setEyeConstants(g, 1, right);
}
std::array<float, 20> stockWith(const float* base, int index, float value) {
    std::array<float, 20> f;
    std::memcpy(f.data(), base, 80);
    f[static_cast<size_t>(index)] = value;
    return f;
}

// The 80 bytes the draw saw for a bound step, against the arithmetic (the review half of a golden).
void expectWorldBytes(const Step& s, const float* pose, bool left, double dist, const char* what, bool curved = false) {
    const std::array<double, 20> want = expectedWorld(pose, left, dist, stub::outer, stub::inner, stub::top, stub::bot, curved);
    const std::string prob = expectedProblem(s.draw.cb2Bytes, want);
    check(prob.empty(), lab(what), prob);
}

// One frame in which both eyes must be bound, and whose bytes must be the arithmetic's for `pose` (eye 0 is the left, as the buffers say). The
// panel is expected flat -- cb2[3] zero and nothing armed -- unless the curvature the config has now is above 0 (`curvature` below 0 is that
// one; 0 for none is every older scenario): then the z column is in the bytes and the strip is armed, whatever the pose (there is no test of
// the bent edges: a pose that keeps the CENTRE in front is bound, and bent).
FrameResult boundFrame(Gpu& g, const float* pose, const char* what, double dist = 3.35, bool eye0Left = true, double curvature = -1.0) {
    if (curvature < 0.0) curvature = g_liveCurvature;
    setPose(pose);
    FrameResult fr = runFrame(g);
    for (int e = 0; e < 2; ++e) {
        g_note = fmt("%s eye", e ? "right" : "left");
        const bool left = (e == 0) == eye0Left;
        const bool bent = curvature > 0.0;
        expectStep(fr.eye[e], true, what, g, -1, bent);
        if (fr.eye[e].bound) expectWorldBytes(fr.eye[e], pose, left, dist, "world-bytes", bent);
    }
    g_note.clear();
    return fr;
}

// Frames in which neither eye may be bound: VS b2 stays the game's whatever else happens.
void unboundFrames(Gpu& g, int frames, const char* what) {
    for (int f = 0; f < frames; ++f) {
        FrameResult fr = runFrame(g);
        for (int e = 0; e < 2; ++e) {
            g_note = fmt("frame %d, %s eye", f, e ? "right" : "left");
            expectStep(fr.eye[e], false, what, g);
        }
    }
    g_note.clear();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// head: the splash-sized panel without the world lock. No pose, no tangents and no eye identity are needed; the constants are the movie's own
// with cb2[0] scaled (the factor derived from its own numbers); nothing else changes.
// ---------------------------------------------------------------------------------------------------------------------------------
std::array<double, 20> expectedHead(const float* stock) {
    const double halfNdc = static_cast<double>(stock[0]) * std::fabs(static_cast<double>(stock[4]));
    double scale = halfNdc > 1e-6 ? 1.0440 / halfNdc : 1.0;
    scale = std::min(std::max(scale, 1.0), 8.0);   // the splash's half-width in NDC over this panel's; at least 1, at most 8
    std::array<double, 20> out{};
    for (int i = 0; i < 20; ++i) out[static_cast<size_t>(i)] = stock[i];
    out[0] = stock[0] * scale;
    out[1] = stock[1] * scale;
    return out;
}

// The head variants: the movie's own numbers; a headset whose frustum is symmetric (both eyes' centre 0: head mode never needs the eye); a
// panel so big that the splash's width over it is under 1 (the scale is held at 1); one so small that it is over 8 (held at 8).
struct HeadCase {
    bool symmetric = false;
    float half = 512.0f;      // cb2[0].x
    float pxToNdc = -0.000368732f;   // cb2[1].x
    bool golden = true;
    const char* scaleLog = "x5.53";
};
void headBody(Gpu& g, const HeadCase& hc) {
    const bool symmetric = hc.symmetric;
    stub::havePose = false;
    stub::haveTangents = false;
    stub::haveVertical = false;
    std::array<float, 20> left = stockWith(kStockL, 0, hc.half), right = stockWith(kStockR, 0, hc.half);
    left[4] = right[4] = hc.pxToNdc;
    if (symmetric) left[16] = right[16] = 0.0f;
    setConstants(g, left.data(), right.data());
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("head");
    check(introPanelWants(), lab("wants"), "head mode is wanted");
    settle(g, "settle");
    for (int k = 0; k < 2; ++k) {
        FrameResult fr = runFrame(g);
        for (int e = 0; e < 2; ++e) {
            const Step& s = fr.eye[e];
            g_note = fmt("bound frame %d, %s eye", k, e ? "right" : "left");
            expectStep(s, true, "bound", g);
            if (!s.bound) continue;
            const std::string prob = expectedProblem(s.draw.cb2Bytes, expectedHead(e ? right.data() : left.data()));
            check(prob.empty(), lab("arithmetic"), "the bytes are not the movie's own constants with cb2[0] scaled: " + prob);
            if (!symmetric && hc.golden && k == 0) {
                const std::string gname = std::string("head-") + (e ? "right" : "left");
                if (g_emit) emitGolden(gname, s.draw.cb2Bytes);
                else {
                    const std::string gp = goldenProblem(s.draw.cb2Bytes, findGolden(gname.c_str()));
                    check(gp.empty(), lab("golden"), gname + ": " + gp);
                }
            }
        }
    }
    g_note.clear();
    const std::string log = endLog();
    check(count(log, "intro video size: SPLASH") == 1 && !has(log, "intro video lock:"), lab("log-config"), "head mode says SPLASH once and nothing about the lock");
    check(has(log, (std::string("so matching it is ") + hc.scaleLog + " here").c_str()) && count(log, "intro video size: engaged") == 1 &&
              has(log, (std::string(hc.scaleLog) + " its own size").c_str()),
          lab("log-scale"), std::string("the derived scale (") + hc.scaleLog + ") is logged, and 'engaged' once");
    check(!has(log, "holding") && !has(log, "cannot tell") && !has(log, "do not read as") && !has(log, "intro video curve"), lab("log-clean"), "a lock, refusal or curve line on the head path");
}
void scnHead(Gpu& g) { headBody(g, HeadCase{}); }
void scnHeadSymmetric(Gpu& g) { HeadCase hc; hc.symmetric = true; headBody(g, hc); }
// 3000 px: the panel already covers 1.106 of the view's half-width, the splash 1.044, so the factor would be 0.944 -- held at 1, x1.00.
void scnHeadClampLow(Gpu& g) { HeadCase hc; hc.half = 3000.0f; hc.golden = false; hc.scaleLog = "x1.00"; headBody(g, hc); }
// 0.00001 NDC per pixel: the panel covers 0.00512 of the half-width, the factor would be 204 -- held at 8, x8.00.
void scnHeadClampHigh(Gpu& g) { HeadCase hc; hc.pxToNdc = -0.00001f; hc.golden = false; hc.scaleLog = "x8.00"; headBody(g, hc); }

// ---------------------------------------------------------------------------------------------------------------------------------
// golden-asymmetric: a headset whose vertical frustum is NOT symmetric (a Quest 3: t = -1.4281, b = +0.9657, docs/intro-video.md), so m12 is
// not zero (the field's shear). Horizontal pair chosen to the same span as that comment's 2.2155. Pose A only.
// ---------------------------------------------------------------------------------------------------------------------------------
void scnGoldenAsymmetric(Gpu& g) {
    stub::outer = 1.2000f;
    stub::inner = 1.0155f;
    stub::top = 1.4281f;
    stub::bot = 0.9657f;
    setPose(kPoseA);
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen");
    settle(g, "settle");
    for (int k = 0; k < 2; ++k) {
        FrameResult fr = boundFrame(g, kPoseA, "bound");
        for (int e = 0; e < 2; ++e) {
            const Step& s = fr.eye[e];
            g_note = fmt("bound frame %d, %s eye", k, e ? "right" : "left");
            const std::string gname = std::string("Q-") + (e ? "right" : "left");
            if (k != 0 || !s.bound) continue;
            if (g_emit) emitGolden(gname, s.draw.cb2Bytes);
            else {
                const std::string gp = goldenProblem(s.draw.cb2Bytes, findGolden(gname.c_str()));
                check(gp.empty(), lab("golden"), gname + ": " + gp);
            }
        }
    }
    g_note.clear();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// stock, stock-off, config: what is wanted.
// ---------------------------------------------------------------------------------------------------------------------------------
// At stock the transform is never applied; the resampler alone can keep the path alive, and then a composite it takes is "bound" with VS b2
// still the game's.
void scnStock(Gpu& g) {
    check(beginLog(), lab("log"), "the scratch log opens");
    stub::upscaleWants = true;
    configure("stock");
    check(introPanelWants(), lab("wants-upscale"), "with the resampler wanting, the path is alive at stock");
    unboundFrames(g, 8, "no-transform");
    check(stub::recentreRequests == 1, lab("recentre"), fmt("recentre requests %u, want 1: the first screen composite asks even at stock when the resampler keeps the path alive", stub::recentreRequests));
    stub::upscaleBegin = true;
    FrameResult fr = runFrame(g);
    for (int e = 0; e < 2; ++e) {
        g_note = fmt("%s eye", e ? "right" : "left");
        expectStep(fr.eye[e], true, "upscaled", g, 0);
    }
    g_note.clear();
    check(stub::upBegin == 18 && stub::upEnd == 2, lab("resampler"), fmt("introUpscaleBegin %u times (want 18), End %u (want 2: once per call that said yes)", stub::upBegin, stub::upEnd));
    const std::string log = endLog();
    check(!has(log, "read the movie panel's own constants") && !has(log, "engaged") && !has(log, "holding"), lab("log-clean"), "the transform path ran at stock");
}
void scnStockOff(Gpu& g) {
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("stock");
    check(!introPanelWants(), lab("wants"), "stock with the resampler off: not wanted");
    unboundFrames(g, 6, "inert");
    check(stub::recentreRequests == 0 && stub::upBegin == 0 && stub::upEnd == 0, lab("inert"),
          fmt("the module was not inert: %u recentre requests, %u Begin, %u End", stub::recentreRequests, stub::upBegin, stub::upEnd));
    check(stub::upFrameEnd == 6, lab("frame-edges"), fmt("introUpscaleFrameEnd %u times, want one per frame (6)", stub::upFrameEnd));
}

void scnConfig(Gpu&) {
    check(beginLog(), lab("log"), "the scratch log opens");
    check(!introPanelWants(), lab("initial"), "before anything is configured the panel is not wanted");
    struct Row {
        const char* value;
        bool wants;
    };
    // screen: the default and its aliases; stock and its aliases (0, 1, 1.0, off); head and skip (skip leaves the other slices at the default);
    // sharp; a legacy numeric size above 1 means screen; an unrecognised value runs the default.
    const Row rows[] = {{"screen", true}, {"stock", false}, {"head", true}, {"skip", true}, {"sharp", true}, {"banana", true}, {"off", false}, {"0", false},
                        {"1.0", false}, {"2.5", true}, {"splash", true}, {"STOCK", false}};
    for (const Row& r : rows) {
        configure(r.value);
        g_note = std::string("fix.intro_video = ") + r.value;
        check(introPanelWants() == r.wants, lab("wants-table"), fmt("introPanelWants() is %d, want %d", introPanelWants(), r.wants));
    }
    g_note.clear();
    configure("stock");
    stub::upscaleWants = true;
    check(introPanelWants(), lab("wants-upscale"), "at stock the resampler's own want keeps the module alive");
    stub::upscaleWants = false;
    check(!introPanelWants(), lab("wants-upscale-off"), "...and without it nothing is wanted");
    const std::string log = endLog();
    check(has(log, "intro video lock: WORLD") && has(log, "intro video lock: head.") && has(log, "intro video size: SPLASH") && has(log, "intro video size: stock"), lab("log-lines"),
          "the lock and size lines are written when the mode changes");
    check(count(log, "is not screen, stock or skip") == 1 && has(log, "\"banana\""), lab("log-unrecognised"), "an unrecognised value is said once, with the value");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// splash-no-fill, splash: the game's world-space cb2.
// ---------------------------------------------------------------------------------------------------------------------------------
void scnSplashNoFill(Gpu& g) {
    setConstants(g, kSplash, kSplash);
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen");
    // The movie's last fill is in an earlier frame (so a fill of the same size is on record), then the splash: no fill in any frame after it.
    introPanelNoteFill(kFillW, kFillH);
    introPanelTick(g.ctx.Get(), false);
    for (int f = 0; f < 8; ++f) {
        FrameResult fr = runFrame(g, false);   // no fill this frame: the movie is not playing
        for (int e = 0; e < 2; ++e) {
            g_note = fmt("frame %d, %s eye", f, e ? "right" : "left");
            expectStep(fr.eye[e], false, "unmatched", g);
        }
    }
    g_note.clear();
    check(introPanelWants(), lab("wants"), "the splash is not even read without a fill: nothing is refused, the panel stays wanted");
    check(stub::recentreRequests == 1, lab("recentre"), fmt("recentre requests %u, want 1 (the first well-shaped composite asks, fill or not)", stub::recentreRequests));
    check(stub::upBegin == 0, lab("resampler"), "the resampler was asked about a composite that no fill matched");
    const std::string log = endLog();
    check(!has(log, "read the movie panel's own constants") && !has(log, "do not read as"), lab("log-clean"), "the module read or judged a buffer it had no fill for");
}

void scnSplash(Gpu& g) {
    setConstants(g, kSplash, kSplash);
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen");
    settle(g, "settle");   // a fill every frame, so the splash's buffers are read; the fourth frame edge retires them
    check(!introPanelWants(), lab("refused"), "the splash's constants refuse themselves: by the fourth frame edge the panel is not wanted");
    unboundFrames(g, 3, "after-refusal");
    check(stub::upBegin == 8, lab("resampler"), fmt("introUpscaleBegin %u times, want 8 (the four settle frames: nothing after the refusal)", stub::upBegin));
    // The refusal is for the session, not the buffer: the movie's own constants afterwards do not bring the panel back.
    setConstants(g, kStockL, kStockR);
    configure("screen");
    check(!introPanelWants(), lab("session"), "reconfiguring brought the panel back after a refusal");
    unboundFrames(g, 6, "session");
    const std::string log = endLog();
    check(has(log, "do not read as a screen-space placement") && has(log, "-0.1941 0.06014 9.973e-05 -0.9971") && has(log, "cb2[4].w 3.761") && has(log, "stock for this session"), lab("log-refusal"),
          "the refusal line carries the splash's cb2[3] and cb2[4].w from the capture");
    check(!has(log, "engaged") && !has(log, "holding") && !has(log, "read the movie panel's own constants"), lab("log-clean"), "the panel engaged on the splash's constants");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// refuse-*, accept-*: every clause of the screen-space test, one buffer each.
// ---------------------------------------------------------------------------------------------------------------------------------
struct Variant {
    const char* name;
    int index;      // the float of the movie's stock cb2 that is changed
    float value;
    bool accepted;
};
const Variant kVariants[] = {
    {"scale-x", 0, 8.0f, false},       // cb2[0].x under 16: a world-space quad is 4.4 units, the movie's 512 pixels
    {"scale-y", 1, 8.0f, false},       // cb2[0].y
    {"cb2-1-w", 7, 1e-4f, false},      // cb2[1].w: a perspective divide
    {"cb2-2-w", 11, 1e-4f, false},     // cb2[2].w
    {"cb2-3-x", 12, 1e-4f, false},     // cb2[3] must be zero entirely
    {"cb2-3-y", 13, 1e-4f, false},
    {"cb2-3-z", 14, 1e-4f, false},
    {"cb2-3-w", 15, 1e-4f, false},
    {"w-low", 19, 0.99f, false},       // cb2[4].w must be 1 (within a thousandth)
    {"w-high", 19, 1.01f, false},
    {"w-in-low", 19, 0.9995f, true},
    {"w-in-high", 19, 1.0005f, true},
};
void variantBody(Gpu& g, const Variant& v) {
    const std::array<float, 20> left = stockWith(kStockL, v.index, v.value), right = stockWith(kStockR, v.index, v.value);
    setPose(kPoseA);
    setConstants(g, left.data(), right.data());
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen");
    settle(g, "settle");
    if (v.accepted) {
        check(introPanelWants(), lab("wants"), "an accepted buffer left the panel not wanted");
        boundFrame(g, kPoseA, "bound");
        const std::string log = endLog();
        check(!has(log, "do not read as"), lab("log-clean"), "an accepted buffer was refused in the log");
        return;
    }
    check(!introPanelWants(), lab("refused"), "the constants did not read as screen-space, and the panel is still wanted");
    unboundFrames(g, 2, "after-refusal");
    setConstants(g, kStockL, kStockR);   // the session stays refused
    unboundFrames(g, 5, "session");
    check(!introPanelWants(), lab("session"), "a refusal is for the session: the movie's own constants later did not matter");
    const std::string log = endLog();
    check(has(log, "do not read as a screen-space placement") && has(log, "stock for this session"), lab("log-refusal"), "the refusal is not logged");
    check(!has(log, "engaged"), lab("log-clean"), "the panel engaged");
}
template <size_t K>
void scnVariant(Gpu& g) {
    variantBody(g, kVariants[K]);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The lock's refusals: no pose, no tangents, ...: the movie stays as the game drew it, one line however many draws, retried at every draw.
// ---------------------------------------------------------------------------------------------------------------------------------
struct Refusal {
    const char* needle;                // the reason, as the log says it
    void (*setup)(Gpu&);               // make it so
    void (*fix)(Gpu&);                 // make it not so
};
void setViewport(Gpu& g, float w, float h) {
    D3D11_VIEWPORT vp{0.0f, 0.0f, w, h, 0.0f, 1.0f};
    g.ctx->RSSetViewports(1, &vp);
}
const Refusal kRefusals[] = {
    {"no head pose has been published", [](Gpu&) { stub::havePose = false; }, [](Gpu&) { stub::havePose = true; }},
    {"no eye tangents have been published", [](Gpu&) { stub::haveTangents = false; }, [](Gpu&) { stub::haveTangents = true; }},
    {"the published tangents are degenerate", [](Gpu&) { stub::outer = 0.0004f; stub::inner = 0.0004f; }, [](Gpu&) { stub::outer = 1.5293f; stub::inner = 1.0324f; }},
    {"the vertical tangents are missing while the horizontal ones are present", [](Gpu&) { stub::haveVertical = false; }, [](Gpu&) { stub::haveVertical = true; }},
    {"the panel would be BEHIND you", [](Gpu&) { setPose(kPoseBehind); }, [](Gpu&) { setPose(kPoseA); }},
    {"the transform came out non-finite", [](Gpu&) { setPose(kPoseA); stub::pose[0] = std::nanf(""); }, [](Gpu&) { setPose(kPoseA); }},
    {"the viewport is degenerate", [](Gpu& g) { setViewport(g, 0.0f, 0.0f); }, [](Gpu& g) { setViewport(g, 5424.0f, 5356.0f); }},
};
void refusalBody(Gpu& g, const Refusal& r) {
    setPose(kPoseA);
    setConstants(g, kStockL, kStockR);
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen");
    r.setup(g);
    settle(g, "settle");
    unboundFrames(g, 3, "refused");
    check(introPanelWants(), lab("wants"), "a refusal of the lock is per draw: the panel is still wanted");
    check(stub::upEnd == 0, lab("no-end"), "introPanelEndDraw ran for a refused composite");
    r.fix(g);   // what was missing is published: the very next draw takes
    boundFrame(g, kPoseA, "retried");
    const std::string log = endLog();
    check(count(log, r.needle) == 1 && count(log, "The movie stays as the game drew it") == 1, lab("log-once"),
          fmt("the reason \"%s\" appears %zu times over six refused composites and a retry, want once", r.needle, count(log, r.needle)));
    check(count(log, "intro video lock: holding") == 1, lab("log-holding"), "the 'holding' line, once, when the retry took");
}
template <size_t K>
void scnRefusal(Gpu& g) {
    refusalBody(g, kRefusals[K]);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// eyes-*: the eye is read from the game's own cb2[4].x (positive in the left eye); two buffers that do not tell the eyes apart are refused.
// ---------------------------------------------------------------------------------------------------------------------------------
void eyesRefused(Gpu& g, float left16, float right16, const char* reads, bool bothSame) {
    setPose(kPoseA);
    const std::array<float, 20> l = stockWith(kStockL, 16, left16), r = stockWith(kStockR, 16, right16);
    setConstants(g, l.data(), r.data());
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen");
    settle(g, "settle");
    unboundFrames(g, 3, "refused");
    check(introPanelWants(), lab("wants"), "this refusal is of the lock only: the panel is still wanted");
    const std::string log = endLog();
    check(count(log, "intro video lock: cannot tell the eyes apart") == 1 && has(log, reads), lab("log-once"),
          fmt("the refusal line is not written once with the centre it read (%s)", reads));
    check(has(log, "and both eyes read the same") == bothSame, lab("log-suffix"), bothSame ? "the line does not say both eyes read the same" : "the line says both eyes read the same");
    check(!has(log, "holding"), lab("log-clean"), "the lock took");
}
void scnEyesAlike(Gpu& g) { eyesRefused(g, 0.1939f, 0.1939f, "reads 0.1939", true); }
void scnEyesSymmetric(Gpu& g) { eyesRefused(g, 0.0f, 0.0f, "reads 0.0000", false); }
void scnEyesNearSymmetric(Gpu& g) { eyesRefused(g, 0.01f, -0.01f, "reads 0.0100", false); }

void eyesBound(Gpu& g, float left16, float right16, bool eye0Left) {
    setPose(kPoseA);
    const std::array<float, 20> l = stockWith(kStockL, 16, left16), r = stockWith(kStockR, 16, right16);
    setConstants(g, l.data(), r.data());
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen");
    settle(g, "settle");
    boundFrame(g, kPoseA, "bound", 3.35, eye0Left);
}
void scnEyesFaintKnown(Gpu& g) { eyesBound(g, 0.03f, -0.03f, true); }    // 0.03 is past the 0.02 below which the sign is a coin flip
void scnEyesSwapped(Gpu& g) { eyesBound(g, -0.1940f, 0.193907f, false); }   // the eye is the sign of the game's own centre, not the draw order

// ---------------------------------------------------------------------------------------------------------------------------------
// retire-*: the first rendered scene stands everything down for the session.
// ---------------------------------------------------------------------------------------------------------------------------------
void afterRetirement(Gpu& g) {
    check(!introPanelWants(), lab("wants"), "after the scene arrived the panel is still wanted");
    check(stub::upShutdown == 1, lab("resampler-shutdown"), fmt("introUpscaleShutdown ran %u times, want once", stub::upShutdown));
    const unsigned beginsBefore = stub::upBegin;
    unboundFrames(g, 3, "after");   // a fill and composites that would have matched
    check(stub::upBegin == beginsBefore, lab("resampler-after"), "the resampler was asked about a composite after the intro was over");
    configure("screen");
    check(!introPanelWants(), lab("reconfigure"), "reconfiguring brought the panel back after the intro was over");
    introPanelTick(g.ctx.Get(), true);   // a second scene frame says nothing more
}
void scnRetireUsed(Gpu& g) {
    setPose(kPoseA);
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen");
    settle(g, "settle");
    boundFrame(g, kPoseA, "bound");
    boundFrame(g, kPoseB, "bound");
    introPanelTick(g.ctx.Get(), true);
    afterRetirement(g);
    const std::string log = endLog();
    check(count(log, "a rendered scene arrived -- the intro is over") == 1 && has(log, "It resized 4 draw(s)."), lab("log-retired"), "the retirement line, once, with the four draws it resized");
    check(!has(log, "of them were armed"), lab("log-retired-unarmed"), "the retirement line counts armed draws at curvature 0, where there are none");
}
void scnRetireSettling(Gpu& g) {
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen");
    unboundFrames(g, 2, "settling");
    introPanelTick(g.ctx.Get(), true);
    afterRetirement(g);
    const std::string log = endLog();
    check(count(log, "a rendered scene arrived -- the intro is over") == 1 && has(log, "It resized 0 draw(s)"), lab("log-retired"), "the retirement line says it resized 0 draws");
}
void scnRetireUnseen(Gpu& g) {
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen");
    introPanelTick(g.ctx.Get(), true);
    afterRetirement(g);
    const std::string log = endLog();
    check(count(log, "the movie's panel was never seen") == 1 && !has(log, "It resized"), lab("log-unseen"), "the unseen-panel line, once");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// shutdown-relearn, third-buffer, small-cb, distance, gates, scene-arrived.
// ---------------------------------------------------------------------------------------------------------------------------------
void scnShutdownRelearn(Gpu& g) {
    setPose(kPoseA);
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen");
    settle(g, "settle");
    boundFrame(g, kPoseA, "bound");
    introPanelShutdown();   // the DLL's teardown: slots and restore only
    check(introPanelWants(), lab("wants"), "a shutdown that is not a retirement leaves the panel wanted");
    settle(g, "relearn");   // the buffers are read again
    boundFrame(g, kPoseB, "bound-again");
    const std::string log = endLog();
    check(count(log, "asking the vr half to") == 1 && count(log, "intro video lock: holding") == 1 && count(log, "intro video size: engaged") == 1, lab("log-once"),
          "the recentre, 'holding' and 'engaged' lines are once per process, shutdown or not");
    check(count(log, "read the movie panel's own constants") == 4, lab("log-read"), "the readback line, two eyes twice");
}

void scnThirdBuffer(Gpu& g) {
    setPose(kPoseA);
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen");
    settle(g, "settle");
    for (int f = 0; f < 8; ++f) {   // long enough for a third buffer that was served to settle and bind
        introPanelNoteFill(kFillW, kFillH);
        const Step l = compositeStep(g, 0);
        const Step r = compositeStep(g, 1);
        const Step x = compositeWith(g, g.extraCb.Get());   // a third distinct cb2: one buffer per eye is the model, a third is refused
        introPanelTick(g.ctx.Get(), false);
        g_note = fmt("frame %d", f);
        expectStep(l, true, "left", g);
        expectStep(r, true, "right", g);
        expectStep(x, false, "third", g);
    }
    g_note.clear();
    check(introPanelWants(), lab("wants"), "a third buffer is refused alone: the panel stays wanted");
}

void scnSmallCb(Gpu& g) {
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen");
    for (int f = 0; f < 8; ++f) {
        introPanelNoteFill(kFillW, kFillH);
        const Step a = compositeWith(g, g.smallCb.Get());   // 64 bytes: not a cb2
        introPanelTick(g.ctx.Get(), false);
        g_note = fmt("frame %d", f);
        expectStep(a, false, "small", g);
    }
    g_note.clear();
    check(introPanelWants(), lab("wants"), "a buffer too small for cb2 is not a refusal of the session");
    const std::string log = endLog();
    check(!has(log, "read the movie panel's own constants") && !has(log, "do not read as"), lab("log-clean"), "a 64-byte buffer was read or judged");
}

void scnDistance(Gpu& g) {
    setPose(kPoseI);
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen");
    settle(g, "settle");
    struct Row {
        const char* value;   // advanced.intro_video_distance; nullptr is the default
        double want;
    };
    // the default 3.35; metres otherwise, held to 1..20; a value that is not a number is the default
    const Row rows[] = {{nullptr, 3.35}, {"5", 5.0}, {"0.5", 1.0}, {"50", 20.0}, {"banana", 3.35}, {"2.5", 2.5}};
    for (const Row& r : rows) {
        Config::get().set("advanced.intro_video_distance", r.value ? r.value : "");
        introPanelConfigure(Config::get());
        g_note = std::string("distance ") + (r.value ? r.value : "(default)");
        FrameResult fr = boundFrame(g, kPoseI, "bound", r.want);
        float w = 0.0f, z = 0.0f;
        if (fr.eye[0].bound && fr.eye[0].draw.cb2Bytes.size() == 80) {
            std::memcpy(&z, fr.eye[0].draw.cb2Bytes.data() + 72, 4);
            std::memcpy(&w, fr.eye[0].draw.cb2Bytes.data() + 76, 4);
        }
        check(w == static_cast<float>(r.want) && z == 0.5f * static_cast<float>(r.want), lab("distance"), fmt("cb2[4].w = %.9g, z = %.9g, want %.9g and half of it", w, z, r.want));
    }
    g_note.clear();
}

void scnGates(Gpu& g) {
    ID3D11DeviceContext* ctx = g.ctx.Get();
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen");
    introPanelNoteFill(kFillW, kFillH);
    check(!introPanelOnComposite(nullptr, 'X', 6, 1, kFillW, kFillH), lab("null-ctx"), "a null context was answered true");
    // the draw's shape: a six-index instanced quad of kind 'X'
    expectStep(compositeWith(g, g.eyeCb[0].Get(), kFillW, kFillH, 'N', 6, 1), false, "kind", g);
    expectStep(compositeWith(g, g.eyeCb[0].Get(), kFillW, kFillH, 'X', 4, 1), false, "count", g);
    expectStep(compositeWith(g, g.eyeCb[0].Get(), kFillW, kFillH, 'X', 6, 2), false, "instances", g);
    check(stub::recentreRequests == 0, lab("recentre-shape"), "a draw of the wrong shape asked for the recentre");
    // right shape, but not the surface the fill converted into: false, and the recentre is asked for (the first well-shaped composite)
    expectStep(compositeWith(g, g.eyeCb[0].Get(), 1280, 720), false, "surface", g);
    expectStep(compositeWith(g, g.eyeCb[0].Get(), 1280, kFillH), false, "surface-width", g);
    expectStep(compositeWith(g, g.eyeCb[0].Get(), kFillW, 720), false, "surface-height", g);
    check(stub::recentreRequests == 1, lab("recentre-once"), fmt("recentre requests %u after the first well-shaped composite, want 1", stub::recentreRequests));
    check(stub::upBegin == 0, lab("resampler-unmatched"), "the resampler was asked about a composite whose surface is not the fill's");
    // the fill's own surface: the readback starts, and the answer is still false
    expectStep(compositeWith(g, g.eyeCb[0].Get(), kFillW, kFillH), false, "matched-starts-readback", g);
    check(stub::upBegin == 1, lab("resampler-matched"), "the resampler was not asked about the one composite that matched the fill");
    // a fill from an earlier frame is not this frame's movie
    introPanelTick(ctx, false);
    expectStep(compositeWith(g, g.eyeCb[0].Get()), false, "stale-fill", g);
    check(stub::upBegin == 1, lab("resampler-stale"), "the resampler was asked about a composite whose fill is from an earlier frame");
    check(stub::recentreRequests == 1, lab("recentre-still-once"), "the recentre was asked for again");
    const std::string log = endLog();
    check(count(log, "asking the vr half to") == 1, lab("log-recentre"), "the recentre line is not written exactly once");
}

void scnSceneArrived(Gpu& g) {
    stub::sceneArrived = true;   // announced before the first composite: no recentre (the on-foot HUD's six-index composite must not trigger it)
    setPose(kPoseA);
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen");
    settle(g, "settle");
    boundFrame(g, kPoseA, "bound");
    check(stub::recentreRequests == 0, lab("recentre"), fmt("recentre requests %u, want 0 once the scene has arrived", stub::recentreRequests));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// THE CURVED MOVIE (fix.panel_curvature above 0): the world panel's cb2[3] carries the seated +z axis through the projection, and the strip is
// armed for the caller (vscreen.cpp draws panel_curve.cpp's strip instead of the quad); at curvature 0, at stock and head, in the settle
// frames, when the lock is refused and when the surface strip has stood down, the bytes are the flat panel's to the bit and nothing is
// armed. The bent edges are not tested against the eye (round 3): whatever the pose, a movie the lock holds with its centre in front is bent.
// ---------------------------------------------------------------------------------------------------------------------------------
const float* poseByName(const char* name) {
    if (!std::strcmp(name, "A")) return kPoseA;
    if (!std::strcmp(name, "B")) return kPoseB;
    if (!std::strcmp(name, "C")) return kPoseC;
    if (!std::strcmp(name, "D")) return kPoseD;
    if (!std::strcmp(name, "I")) return kPoseI;
    return nullptr;
}

// Whether two cb2 images are the same in every float of columns 0, 1, 2 and 4 (and what the one in column 3 is, is another question).
bool sameButColumn3(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != 80 || b.size() != 80) return false;
    return std::memcmp(a.data(), b.data(), 48) == 0 && std::memcmp(a.data() + 64, b.data() + 64, 16) == 0;
}
bool column3IsZero(const std::vector<uint8_t>& a) {
    if (a.size() != 80) return false;
    const uint8_t zero[16] = {0};
    return std::memcmp(a.data() + 48, zero, 16) == 0;   // exactly +0.0f in all four: the flat panel's, not merely a small number
}

// The bytes of one bent step against their golden "<pose>-<eye>-curved" and, when a curvature-0 golden of the pose exists (A, B and I do), in
// every float but column 3 against that one: the bend changes the z column and nothing else.
void expectCurvedGolden(const Step& s, const char* pose, bool left, std::map<std::string, std::vector<uint8_t>>* emit) {
    const std::string base = fmt("%s-%s", pose, left ? "left" : "right");
    const Golden* flat = findGolden(base.c_str());
    const std::string name = base + "-curved";
    if (emit) (*emit)[name] = s.draw.cb2Bytes;
    else {
        const std::string gp = goldenProblem(s.draw.cb2Bytes, findGolden(name.c_str()));
        check(gp.empty(), lab("golden"), name + ": " + gp);
    }
    if (flat) {
        std::vector<uint8_t> f(80);
        std::memcpy(f.data(), flat->v, 80);
        check(sameButColumn3(s.draw.cb2Bytes, f), lab("columns-unchanged"), base + ": columns 0, 1, 2 and 4 are not the curvature-0 golden's to the bit");
    }
    check(!column3IsZero(s.draw.cb2Bytes), lab("column3-written"), base + ": cb2[3] is still zero in a bent step");
}

// The lines of the edge test that is not there must not come back: no line about a bent edge, whatever the pose.
void expectNoEdgeLine(const std::string& log) {
    check(!has(log, "stays flat") && !has(log, "nearer edge") && !has(log, "edge of the bent panel"), lab("log-no-edge-line"), "the log has a line about a bent edge being behind the eye, and nothing tests the edges");
}

// curved: the screen mode at curvature 0.3 and 48 columns over five poses -- A (a head turned 53 degrees, where an edge of the bent panel is
// beside or behind the eye), B (the same, the other way), C, D and the identity: every one bends the movie. Bytes to the bit, the strip armed
// for every draw and for none after introPanelEndDraw, the gain, the first-armed line once, the retirement line's count, no edge line.
void scnCurved(Gpu& g) {
    ID3D11DeviceContext* ctx = g.ctx.Get();
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen", nullptr, "0.3", "48");
    check(introPanelWants(), lab("wants"), "the panel is wanted");
    check(introPanelStripGain() == 4.44444f, lab("gain"), fmt("introPanelStripGain() is %.9g, want the panel's half-width 4.44444", static_cast<double>(introPanelStripGain())));
    check(!introPanelStripArmed(), lab("armed-idle"), "the strip is armed before any composite");
    setPose(kPoseC);
    settle(g, "settle", true);   // nothing is bound, so nothing is armed (expectStep's default)
    const char* const frames[] = {"C", "A", "I", "B", "D", "C"};
    std::map<std::string, std::vector<uint8_t>> emit;
    unsigned bound = 0, armed = 0;
    for (size_t k = 0; k < sizeof(frames) / sizeof(frames[0]); ++k) {
        const float* pose = poseByName(frames[k]);
        setPose(pose);
        FrameResult fr = runFrame(g);
        for (int e = 0; e < 2; ++e) {
            const Step& s = fr.eye[e];
            g_note = fmt("frame %zu (pose %s), %s eye", k, frames[k], e ? "right" : "left");
            expectStep(s, true, "bound", g, -1, true);
            if (!s.bound) continue;
            ++bound;
            if (s.armed) ++armed;
            expectWorldBytes(s, pose, e == 0, 3.35, "arithmetic", true);
            expectCurvedGolden(s, frames[k], e == 0, g_emit ? &emit : nullptr);
            check(introPanelStripGain() == 4.44444f, lab("gain"), "introPanelStripGain() changed while the movie played");
        }
    }
    g_note.clear();
    for (const auto& b : emit) emitGolden(b.first, b.second);
    check(bound == 12 && armed == 12, lab("counts"), fmt("%u bound draws, %u armed, want 12 and 12 (six frames of two eyes, every one bent)", bound, armed));
    check(!introPanelStripArmed(), lab("armed-after"), "the strip is armed between draws");
    introPanelTick(ctx, true);   // the first rendered scene: the intro is over
    check(!introPanelStripArmed() && introPanelStripGain() == 4.44444f, lab("armed-retired"), "the strip is armed after the intro, or the gain moved");
    const std::string log = endLog();
    check(count(log, "intro video curve: the movie is drawn as a 48-column strip at curvature 0.300, gain 4.444 m") == 1, lab("log-armed"),
          "the first-armed line is not written once, with the live column count, the curvature and the gain");
    check(count(log, "placement's +x runs to the viewer's left, so the strip's u runs against x") == 1, lab("log-armed-direction"),
          "the first-armed line does not say, once, that the placement's +x runs to the viewer's left and the strip's u against x");
    check(!has(log, "does not read as running left or right"), lab("log-no-unknown"), "the line about a placement that cannot be read was written for a movie whose frame always can");
    expectNoEdgeLine(log);
    check(count(log, "intro video lock: holding") == 1 && count(log, "intro video size: engaged") == 1, lab("log-lock"), "the 'holding' or 'engaged' line is not written once");
    check(has(log, "It resized 12 draw(s). 12 of them were armed for the curved strip."), lab("log-retired"), "the retirement line does not count 12 draws, 12 of them armed");
    check(!has(log, "do not read as a screen-space") && !has(log, "lock: cannot tell") && !has(log, "FAULT"), lab("log-clean"), "a refusal or a fault was logged");
}

// curved-asymmetric: the Quest-like vertical frustum (m12 is not zero), curvature 0.3, pose C: the z column goes through the same projection, so
// its y is m11 cz.y + m12 cz.z -- the shear a symmetric headset cannot show.
void scnCurvedAsymmetric(Gpu& g) {
    stub::outer = 1.2000f;
    stub::inner = 1.0155f;
    stub::top = 1.4281f;
    stub::bot = 0.9657f;
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen", nullptr, "0.3");
    settle(g, "settle");
    std::map<std::string, std::vector<uint8_t>> emit;
    for (int k = 0; k < 2; ++k) {
        FrameResult fr = boundFrame(g, kPoseC, "bound");   // bent, by the arithmetic (g_liveCurvature = 0.3), and the bytes follow it
        for (int e = 0; e < 2; ++e) {
            const Step& s = fr.eye[e];
            g_note = fmt("bound frame %d, %s eye", k, e ? "right" : "left");
            if (k != 0 || !s.bound) continue;
            const std::string name = std::string("QC-") + (e ? "right" : "left") + "-curved";
            if (g_emit) emit[name] = s.draw.cb2Bytes;
            else {
                const std::string gp = goldenProblem(s.draw.cb2Bytes, findGolden(name.c_str()));
                check(gp.empty(), lab("golden"), name + ": " + gp);
            }
        }
    }
    g_note.clear();
    for (const auto& b : emit) emitGolden(b.first, b.second);
}

// curved-no-edge-test: the bent edges are NOT tested against the eye. D3D clips what is behind the eye in homogeneous space (clip z = w/2 here, so
// the plane is w = 0), so an edge beside or behind the eye just loses that part of the surface and the rest of the bend stays; the only test is
// the centre's (the 'behind' scenarios). Round 2 had a test of the edges (view z below -0.05); it popped the whole bend off at about 20 degrees of
// head yaw and could differ between the eyes near its threshold. These are the worst places for it: where the rig's own arithmetic puts a nearer
// edge at or behind the eye (and the centre still in front), every row is bound and ARMED in both eyes with cb2[3] = col(cz), bytes to the
// arithmetic -- the distance at its 1 m floor with curvature 1.0 (a closed cylinder, both edges 2.83 m toward the viewer) and a head yawed 40
// degrees either way and straight ahead; curvature 0.3 with a head yawed 25 degrees either way, at the default distance and at 1 m.
void scnCurvedNoEdgeTest(Gpu& g) {
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen", nullptr, "0.3");
    setPose(kPoseI);
    settle(g, "settle");
    struct Row {
        const char* name;
        const float* pose;
        const char* curvature;
        const char* distance;
    };
    const Row rows[] = {
        {"closed-1m-yaw40-pos", kPoseYaw40Pos, "1.0", "1"},        {"closed-1m-yaw40-neg", kPoseYaw40Neg, "1.0", "1"},
        {"closed-1m-identity", kPoseI, "1.0", "1"},                 {"gentle-default-yaw25-pos", kPoseYaw25Pos, "0.3", "3.35"},
        {"gentle-default-yaw25-neg", kPoseYaw25Neg, "0.3", "3.35"}, {"gentle-1m-yaw25-pos", kPoseYaw25Pos, "0.3", "1"},
        {"gentle-1m-yaw25-neg", kPoseYaw25Neg, "0.3", "1"},
    };
    for (const Row& r : rows) {
        configure(nullptr, r.distance, r.curvature);   // advanced.intro_video_distance and fix.panel_curvature, live
        const double c = std::atof(r.curvature), dist = std::atof(r.distance);
        g_note = fmt("%s: curvature %s, distance %s m", r.name, r.curvature, r.distance);
        for (int e = 0; e < 2; ++e) {
            check(expectedBasis(r.pose, e == 0, dist).c0[2] < 0.0, lab("fixture-centre"),
                  "the rig's own arithmetic puts the CENTRE behind the eye: the centre's test would refuse this draw, which is not what this row is about");
            check(nearerEdgeViewZ(r.pose, e == 0, dist, c) >= -0.05, lab("fixture-edge"),
                  "the rig's own arithmetic puts the nearer edge in front of the eye by a margin: a test of the edges would pass this row, so it shows nothing");
        }
        boundFrame(g, r.pose, "frame", dist, true, c);   // bound and armed in both eyes, cb2[3] = col(cz), bytes against the arithmetic
    }
    g_note.clear();
    const std::string log = endLog();
    expectNoEdgeLine(log);
    check(count(log, "intro video curve: the movie is drawn as a 64-column strip at curvature 1.000, gain 4.444 m") == 1, lab("log-armed"),
          "the first-armed line is not written once, at the first row's curvature");
}

// curved-unknown-xdir: a placement whose +x cannot be told to run left or right (the head rolled a quarter turn; the panel seen edge-on) is not bent for
// that draw -- the movie stays held on the game's forward, bound and flat, cb2[3] exactly zero, nothing armed, introPanelStripReverseU() false -- and the
// draws before and after it, at an ordinary pose, are bent as ever. One line says it, once, however many draws. A draw that cannot be told is flat, not
// mirrored.
void scnCurvedUnknownXDir(Gpu& g) {
    ID3D11DeviceContext* ctx = g.ctx.Get();
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen", nullptr, "0.3");
    setPose(kPoseI);
    settle(g, "settle");
    struct Row {
        const char* name;
        const float* pose;
        bool unknown;
    };
    const Row rows[] = {{"bent-first", kPoseI, false}, {"roll90", kPoseRoll90, true}, {"edge-on", kPoseEdgeOn, true}, {"bent-again", kPoseI, false}, {"roll90-again", kPoseRoll90, true}};
    unsigned binds = 0, armedBinds = 0;
    for (const Row& r : rows) {
        g_note = r.name;
        for (int e = 0; e < 2; ++e) {
            // the fixture: by the rig's own arithmetic (the bent panel's constants, in double, rounded to the floats the module writes) the placement reads
            // the way the row says, and the centre is in front of the eye (this is not the centre's own refusal)
            const std::array<double, 20> want = expectedWorld(r.pose, e == 0, 3.35, stub::outer, stub::inner, stub::top, stub::bot, true);
            float f[20];
            for (int i = 0; i < 20; ++i) f[i] = static_cast<float>(want[static_cast<size_t>(i)]);
            check(introPlacementXDir(f) == (r.unknown ? IntroXDir::kUnknown : IntroXDir::kLeft), lab("fixture-xdir"),
                  fmt("the rig's own arithmetic reads the %s eye's placement as %d", e ? "right" : "left", static_cast<int>(introPlacementXDir(f))));
            check(expectedBasis(r.pose, e == 0, 3.35).c0[2] < 0.0, lab("fixture-centre"), "the rig's own arithmetic puts the centre behind the eye");
        }
        FrameResult fr = boundFrame(g, r.pose, r.name, 3.35, true, r.unknown ? 0.0 : 0.3);   // an unknown row is expected flat: bound, cb2[3] zero, not armed
        for (int e = 0; e < 2; ++e) {
            if (fr.eye[e].bound) ++binds;
            if (fr.eye[e].armed) ++armedBinds;
            if (!r.unknown || fr.eye[e].draw.cb2Bytes.size() != 80) continue;
            float f[20];
            std::memcpy(f, fr.eye[e].draw.cb2Bytes.data(), 80);
            g_note = fmt("%s, %s eye", r.name, e ? "right" : "left");
            check(column3IsZero(fr.eye[e].draw.cb2Bytes), lab("column3-zero"), "cb2[3] is not exactly zero in a draw whose placement cannot be read");
            check(introPlacementXDir(f) == IntroXDir::kUnknown, lab("bound-xdir"), "the constants bound for an unreadable row read as running left or right");
        }
    }
    g_note.clear();
    check(binds == 10 && armedBinds == 4, lab("counts"), fmt("%u binds, %u armed, want 10 and 4 (five frames of two eyes, two of them ordinary)", binds, armedBinds));
    introPanelTick(ctx, true);   // the first rendered scene: the intro is over
    const std::string log = endLog();
    check(count(log, "does not read as running left or right on the screen") == 1, lab("log-unknown"), "the line about a placement that cannot be read is not written exactly once over six draws");
    check(count(log, "intro video curve: the movie is drawn as a 64-column strip at curvature 0.300") == 1, lab("log-armed"), "the first-armed line is not written once");
    check(has(log, "It resized 10 draw(s). 4 of them were armed for the curved strip."), lab("log-retired"), "the retirement line does not count 10 draws resized, 4 of them armed");
    expectNoEdgeLine(log);
}

// curved-right-running: a placement whose +x runs to the viewer's RIGHT, which the movie's own never does, so the rig feeds the module a REFLECTED pose
// (kPoseMirror, kPoseMirrorYaw: not a head, but the module takes any 12 floats). The bound constants read as running right, the strip is armed, and
// u runs WITH x -- introPanelStripReverseU() false -- in both eyes, so the flag is derived from the constants and is not a constant of the movie.
// Bytes against the rig's arithmetic, cb2[3] = col(cz) as for any bent draw; the first-armed line says right and with.
void scnCurvedRightRunning(Gpu& g) {
    ID3D11DeviceContext* ctx = g.ctx.Get();
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen", nullptr, "0.3");
    setPose(kPoseMirror);
    settle(g, "settle");
    const float* const poses[] = {kPoseMirror, kPoseMirrorYaw, kPoseMirror};
    unsigned binds = 0, armedBinds = 0;
    for (size_t k = 0; k < sizeof(poses) / sizeof(poses[0]); ++k) {
        setPose(poses[k]);
        FrameResult fr = runFrame(g);
        for (int e = 0; e < 2; ++e) {
            const Step& s = fr.eye[e];
            g_note = fmt("frame %zu, %s eye", k, e ? "right" : "left");
            check(expectedBasis(poses[k], e == 0, 3.35).c0[2] < 0.0, lab("fixture-centre"), "the rig's own arithmetic puts the centre behind the eye");
            expectStep(s, true, "bound", g, -1, true, 0);   // armed, and u runs WITH x
            if (s.bound) {
                ++binds;
                if (s.armed) ++armedBinds;
                expectWorldBytes(s, poses[k], e == 0, 3.35, "arithmetic", true);
            }
        }
    }
    g_note.clear();
    check(binds == 6 && armedBinds == 6, lab("counts"), fmt("%u binds, %u armed, want 6 and 6", binds, armedBinds));
    introPanelTick(ctx, true);
    const std::string log = endLog();
    check(count(log, "placement's +x runs to the viewer's right, so the strip's u runs with x") == 1, lab("log-armed-direction"),
          "the first-armed line does not say, once, that the placement's +x runs to the viewer's right and the strip's u with x");
    check(!has(log, "u runs against x") && !has(log, "does not read as running left or right"), lab("log-clean"), "a line about u running against x, or about an unreadable placement, was written");
}

// curved-live: the curvature switched while the movie plays -- the same pose at 0 (flat), 0.3 (bent: every float but cb2[3] the flat panel's, to the
// bit), 0 again (the flat bytes exactly: nothing lingers), 0.5, 0.7 and 1.0 -- a bent z column that does not depend on the curvature (0.7 and 1.0
// give the same bytes), and the retirement line's count of armed draws against the draws it resized (the two curvature-0 frames are resized, not armed).
void scnCurvedLive(Gpu& g) {
    ID3D11DeviceContext* ctx = g.ctx.Get();
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen", nullptr, "0");
    setPose(kPoseC);
    settle(g, "settle");
    unsigned binds = 0, armedBinds = 0;
    const auto tally = [&](const FrameResult& fr) {
        for (int e = 0; e < 2; ++e) {
            if (fr.eye[e].bound) ++binds;
            if (fr.eye[e].armed) ++armedBinds;
        }
    };
    std::array<std::vector<uint8_t>, 2> flatC, bentC, bentI07;
    {
        FrameResult fr = boundFrame(g, kPoseC, "flat");
        tally(fr);
        for (int e = 0; e < 2; ++e) flatC[static_cast<size_t>(e)] = fr.eye[e].draw.cb2Bytes;
    }
    configure(nullptr, nullptr, "0.3");
    {
        FrameResult fr = boundFrame(g, kPoseC, "bent");
        tally(fr);
        for (int e = 0; e < 2; ++e) {
            bentC[static_cast<size_t>(e)] = fr.eye[e].draw.cb2Bytes;
            g_note = e ? "right eye" : "left eye";
            check(sameButColumn3(bentC[static_cast<size_t>(e)], flatC[static_cast<size_t>(e)]) && !column3IsZero(bentC[static_cast<size_t>(e)]), lab("columns-unchanged"),
                  "switching the curvature on changed more than cb2[3], or left it zero");
        }
    }
    configure(nullptr, nullptr, "0");
    {
        FrameResult fr = boundFrame(g, kPoseC, "flat-again");
        tally(fr);
        for (int e = 0; e < 2; ++e) {
            g_note = e ? "right eye" : "left eye";
            check(fr.eye[e].draw.cb2Bytes == flatC[static_cast<size_t>(e)], lab("flat-again-bytes"), "back at curvature 0 the bytes are not the first flat frame's, to the bit");
        }
    }
    configure(nullptr, nullptr, "0.5");
    tally(boundFrame(g, kPoseC, "bent-0.5"));   // pose C bends at 0.5 as at 0.3: the curvature decides whether, not how far, and no pose keeps it flat
    configure(nullptr, nullptr, "0.7");
    FrameResult at07 = boundFrame(g, kPoseI, "bent-0.7");
    tally(at07);
    for (int e = 0; e < 2; ++e) bentI07[static_cast<size_t>(e)] = at07.eye[e].draw.cb2Bytes;
    configure(nullptr, nullptr, "1.0");
    FrameResult at10 = boundFrame(g, kPoseI, "bent-1.0");
    tally(at10);
    for (int e = 0; e < 2; ++e) {
        g_note = e ? "right eye" : "left eye";
        check(at10.eye[e].draw.cb2Bytes == bentI07[static_cast<size_t>(e)], lab("z-column-independent"), "cb2[3] changed with the curvature (it is the axis, not the bend)");
    }
    g_note.clear();
    check(binds == 12 && armedBinds == 8, lab("counts"), fmt("%u binds, %u armed, want 12 and 8 (six frames of two eyes, two of them at curvature 0)", binds, armedBinds));
    introPanelTick(ctx, true);   // the first rendered scene: the intro is over
    const std::string log = endLog();
    check(count(log, "intro video curve: the movie is drawn as a 64-column strip at curvature 0.300") == 1 && count(log, "intro video curve: the movie is drawn") == 1, lab("log-armed"),
          "the first-armed line is not written once, at the curvature the strip was first armed at");
    check(has(log, "It resized 12 draw(s). 8 of them were armed for the curved strip."), lab("log-retired"), "the retirement line does not count 12 draws resized, 8 of them armed");
    expectNoEdgeLine(log);
}

// curved-stood-down: the surface strip faults (the rig's draw function raises an access violation, as in panel_curve_test) and stands down for the
// session: the movie is flat from then on -- still held on the game's forward, bytes the flat panel's, nothing armed -- whatever the configuration.
void scnCurvedStoodDown(Gpu& g) {
    ID3D11DeviceContext* ctx = g.ctx.Get();
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen", nullptr, "0.3");
    setPose(kPoseI);
    settle(g, "settle");
    boundFrame(g, kPoseI, "bent");   // bent, by the arithmetic at curvature 0.3
    check(panelCurveSurfaceWanted(), lab("wanted"), "the surface strip is not wanted at curvature 0.3");
    const bool drew = panelCurveSurfaceDraw(ctx, introPanelStripGain(), 1, true, [](ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT) {
        *reinterpret_cast<volatile int*>(static_cast<uintptr_t>(0x10)) = 1;
    });
    check(!drew && panelCurveSurfaceInfo().standDown && !panelCurveSurfaceWanted(), lab("faulted"), "the faulting strip draw did not stand the surface strip down");
    g_liveCurvature = 0.0;   // the module is told curvature 0.3 and must still draw flat: what boundFrame expects is the flat panel
    for (int f = 0; f < 3; ++f) {
        g_note = fmt("frame %d after the stand-down", f);
        boundFrame(g, kPoseI, "flat", 3.35, true, 0.0);
    }
    configure("screen", nullptr, "0.7");   // a reload does not bring it back
    boundFrame(g, kPoseI, "flat-reconfigured", 3.35, true, 0.0);
    g_note.clear();
    check(introPanelStripGain() == 4.44444f, lab("gain"), "the gain moved");
    introPanelTick(ctx, true);   // the first rendered scene: the intro is over
    const std::string log = endLog();
    check(count(log, "intro video curve: the movie is drawn") == 1, lab("log-armed"), "the first-armed line is not written exactly once (the bent frame before the fault)");
    check(count(log, "the surface strip (the intro movie and the splash) faulted") == 1, lab("log-faulted"), "the stand-down line is not written once");
    check(has(log, "It resized 10 draw(s). 2 of them were armed for the curved strip."), lab("log-retired"),
          "the retirement line does not count 10 draws resized (five frames), 2 of them armed (the one before the fault)");
    expectNoEdgeLine(log);
}

// curved-armed-scope: armed belongs to the bind the last introPanelOnComposite made and to no other: a call that does not bind clears it (the
// caller may have skipped introPanelEndDraw), and a shutdown clears it.
void scnCurvedArmedScope(Gpu& g) {
    ID3D11DeviceContext* ctx = g.ctx.Get();
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen", nullptr, "0.3");
    setPose(kPoseI);
    settle(g, "settle");
    introPanelNoteFill(kFillW, kFillH);
    ID3D11Buffer* game = g.eyeCb[0].Get();
    ctx->VSSetConstantBuffers(2, 1, &game);
    check(introPanelOnComposite(ctx, 'X', 6, 1, kFillW, kFillH) && introPanelStripArmed() && introPanelStripReverseU(), lab("armed"), "a bent bind did not arm the strip, with u running against x");
    // the caller forgot introPanelEndDraw; the next composite is one of the wrong shape: it binds nothing and nothing is armed any longer
    check(!introPanelOnComposite(ctx, 'N', 6, 1, kFillW, kFillH) && !introPanelStripArmed() && !introPanelStripReverseU(), lab("cleared-by-the-next-call"),
          "a call that did not bind left the previous bind's strip armed, or its direction for u standing");
    introPanelEndDraw(ctx);
    ctx->VSSetConstantBuffers(2, 1, &game);
    check(introPanelOnComposite(ctx, 'X', 6, 1, kFillW, kFillH) && introPanelStripArmed() && introPanelStripReverseU(), lab("armed-again"), "the second bent bind did not arm the strip, with u running against x");
    introPanelShutdown();
    check(!introPanelStripArmed() && !introPanelStripReverseU(), lab("cleared-by-shutdown"), "the shutdown left the strip armed, or its direction for u standing");
    ctx->VSSetConstantBuffers(2, 1, &game);
    check(!introPanelStripArmed() && !introPanelStripReverseU(), lab("idle"), "the strip is armed, or u is said to run against x, with no bind in progress");
}

// curved-refused: at curvature 0.3 and a pose that bends, every refusal of the lock leaves the movie as the game drew it with nothing armed, and the
// retry (what was missing is published) bends it.
void scnCurvedRefused(Gpu& g) {
    check(beginLog(), lab("log"), "the scratch log opens");
    configure("screen", nullptr, "0.3");
    setPose(kPoseI);
    settle(g, "settle");
    const int which[] = {0, 1, 4, 6};   // no pose, no tangents, behind, a degenerate viewport (kRefusals above)
    for (int idx : which) {
        const Refusal& r = kRefusals[idx];
        g_note = r.needle;
        r.setup(g);
        unboundFrames(g, 2, "refused");
        r.fix(g);
        boundFrame(g, kPoseI, "retried");
    }
    g_note.clear();
    check(introPanelWants(), lab("wants"), "a refusal of the lock is per draw: the panel is still wanted");
}

// The older scenarios at curvature 0.3: stock never applies the transform, head locks to the head with no pose, the splash's constants are
// refused for the session, and two buffers that do not tell the eyes apart are refused -- none of them bends or arms anything, and each of those
// bodies already says so (expectStep's default is "not armed").
void scnCurvedStock(Gpu& g) { g_curvature = "0.3"; scnStock(g); }
void scnCurvedHead(Gpu& g) { g_curvature = "0.3"; scnHead(g); }
void scnCurvedSplash(Gpu& g) { g_curvature = "0.3"; scnSplash(g); }
void scnCurvedEyesAlike(Gpu& g) { g_curvature = "0.3"; scnEyesAlike(g); }

// ---------------------------------------------------------------------------------------------------------------------------------
// The runner: one process per scenario.
// ---------------------------------------------------------------------------------------------------------------------------------
struct Scenario {
    const char* id;
    void (*run)(Gpu&);
};
const Scenario kScenarios[] = {
    {"golden", scnGolden},
    {"golden-zero", scnGoldenZero},
    {"head", scnHead},
    {"head-symmetric", scnHeadSymmetric},
    {"head-clamp-low", scnHeadClampLow},
    {"head-clamp-high", scnHeadClampHigh},
    {"golden-asymmetric", scnGoldenAsymmetric},
    {"stock", scnStock},
    {"stock-off", scnStockOff},
    {"config", scnConfig},
    {"splash", scnSplash},
    {"splash-no-fill", scnSplashNoFill},
    {"refuse-scale-x", scnVariant<0>},
    {"refuse-scale-y", scnVariant<1>},
    {"refuse-cb2-1-w", scnVariant<2>},
    {"refuse-cb2-2-w", scnVariant<3>},
    {"refuse-cb2-3-x", scnVariant<4>},
    {"refuse-cb2-3-y", scnVariant<5>},
    {"refuse-cb2-3-z", scnVariant<6>},
    {"refuse-cb2-3-w", scnVariant<7>},
    {"refuse-w-low", scnVariant<8>},
    {"refuse-w-high", scnVariant<9>},
    {"accept-w-in-low", scnVariant<10>},
    {"accept-w-in-high", scnVariant<11>},
    {"no-pose", scnRefusal<0>},
    {"no-tangents", scnRefusal<1>},
    {"degenerate-tangents", scnRefusal<2>},
    {"no-vertical", scnRefusal<3>},
    {"behind", scnRefusal<4>},
    {"non-finite", scnRefusal<5>},
    {"viewport", scnRefusal<6>},
    {"eyes-alike", scnEyesAlike},
    {"eyes-symmetric", scnEyesSymmetric},
    {"eyes-near-symmetric", scnEyesNearSymmetric},
    {"eyes-faint-known", scnEyesFaintKnown},
    {"eyes-swapped", scnEyesSwapped},
    {"retire-used", scnRetireUsed},
    {"retire-settling", scnRetireSettling},
    {"retire-unseen", scnRetireUnseen},
    {"shutdown-relearn", scnShutdownRelearn},
    {"third-buffer", scnThirdBuffer},
    {"small-cb", scnSmallCb},
    {"distance", scnDistance},
    {"gates", scnGates},
    {"scene-arrived", scnSceneArrived},
    {"curved", scnCurved},
    {"curved-asymmetric", scnCurvedAsymmetric},
    {"curved-no-edge-test", scnCurvedNoEdgeTest},
    {"curved-unknown-xdir", scnCurvedUnknownXDir},
    {"curved-right-running", scnCurvedRightRunning},
    {"curved-live", scnCurvedLive},
    {"curved-stood-down", scnCurvedStoodDown},
    {"curved-armed-scope", scnCurvedArmedScope},
    {"curved-refused", scnCurvedRefused},
    {"curved-stock", scnCurvedStock},
    {"curved-head", scnCurvedHead},
    {"curved-splash", scnCurvedSplash},
    {"curved-eyes-alike", scnCurvedEyesAlike},
};

std::wstring widen(const char* s) {
    std::wstring w;
    for (; *s; ++s) w.push_back(static_cast<wchar_t>(*s));
    return w;
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

// One scenario, in this process.
int runScenario(const Scenario& sc, bool keep) {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    g_dir = std::wstring(temp) + L"edvr_ictest_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(g_dir.c_str(), nullptr);
    g_tag = L"ic" + widen(sc.id);
    g_runtimeProfile = RuntimeProfile::Vr;
    g_scn = sc.id;

    Gpu gpu = makeGpu();
    check(gpu.ok, lab("gpu"), "a WARP device and the game's state are available");
    if (gpu.ok) {
        check(reportSystemD3D11Only("intro_curve_test"), lab("system-d3d11"), "the process runs on System32's d3d11.dll only (no EDVR proxy beside the rig)");
        bindPipeline(gpu);
        sc.run(gpu);
    }
    g_draws.clear();
    gpu = Gpu{};
    if (Log::get().isOpen()) Log::get().close();
    if (keep) std::printf("the scratch directory is kept: %s\n", narrow(g_dir).c_str());
    else removeScratch();
    if (g_failed) std::printf("FAIL: %s: %u of %u checks failed, %zu distinct\n", sc.id, g_failed, g_checks, g_failCount.size());
    else std::printf("PASS: %u checks (%s)\n", g_checks, sc.id);
    return g_failed ? 1 : 0;
}

// This exe again, with its output captured. The pipe's write end is inheritable from CreatePipe until the parent closes it, so two threads
// starting children at once would hand each other's pipes to the wrong child (and a reader would wait for a process that is not its own):
// the window is held under one lock.
std::mutex g_spawnLock;
int runChild(const std::wstring& args, std::string* out) {
    wchar_t self[MAX_PATH * 2]{};
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH * 2)) return -1;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    std::unique_lock<std::mutex> spawning(g_spawnLock);
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = std::wstring(L"\"") + self + L"\" " + args;
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return -1;
    }
    CloseHandle(wr);
    spawning.unlock();
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) out->append(buf, n);
    CloseHandle(rd);
    DWORD code = static_cast<DWORD>(-1);
    if (WaitForSingleObject(pi.hProcess, 120000) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 0xDEAD);
        out->append("\nFAIL: the scenario did not finish within two minutes\n");
    }
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

std::vector<std::string> linesOf(const std::string& s) {
    std::vector<std::string> v;
    size_t at = 0;
    while (at < s.size()) {
        size_t eol = s.find('\n', at);
        if (eol == std::string::npos) eol = s.size();
        std::string line = s.substr(at, eol - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        v.push_back(line);
        at = eol + 1;
    }
    return v;
}

// The scenarios do not share anything, so a few run at a time; the results are printed in the table's order.
struct ChildResult {
    int rc = -1;
    std::string out;
};
int runAll(bool keep) {
    constexpr size_t kScenarioCount = sizeof(kScenarios) / sizeof(kScenarios[0]);
    std::vector<ChildResult> results(kScenarioCount);
    std::atomic<size_t> next{0};
    auto worker = [&]() {
        for (;;) {
            const size_t i = next++;
            if (i >= kScenarioCount) return;
            results[i].rc = runChild(std::wstring(L"--scenario ") + widen(kScenarios[i].id) + (keep ? L" --keep" : L""), &results[i].out);
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < 4; ++t) pool.emplace_back(worker);
    for (std::thread& t : pool) t.join();

    unsigned total = 0, badScenarios = 0;
    for (size_t index = 0; index < kScenarioCount; ++index) {
        const Scenario& sc = kScenarios[index];
        const std::string& out = results[index].out;
        const int rc = results[index].rc;
        unsigned n = 0;
        bool passed = false, anyFail = false;
        for (const std::string& line : linesOf(out)) {
            if (line.rfind("PASS: ", 0) == 0) {
                passed = std::sscanf(line.c_str(), "PASS: %u checks", &n) == 1;
            } else if (line.rfind("FAIL: ", 0) == 0) {
                anyFail = true;
                std::printf("%s\n", line.c_str());
            } else if (keep) {
                std::printf("%s\n", line.c_str());
            }
        }
        if (rc != 0 || !passed) {
            ++badScenarios;
            if (!anyFail) std::printf("FAIL: %s.crash: the scenario's process exited with code 0x%08X and reported no result\n", sc.id, static_cast<unsigned>(rc));
        } else {
            total += n;
            std::printf("%-24s %u checks\n", sc.id, n);
        }
        std::fflush(stdout);
    }
    if (badScenarios) {
        std::printf("FAIL: intro curve: %u of %zu scenarios failed\n", badScenarios, sizeof(kScenarios) / sizeof(kScenarios[0]));
        return 1;
    }
    std::printf("PASS: %u intro curve checks (%zu scenarios, each in its own process: the module's latches cannot be reset)\n", total, sizeof(kScenarios) / sizeof(kScenarios[0]));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool self = false, dry = false, keep = false, list = false, emitAll = false;
    const char* scenario = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--self-test")) self = true;
        else if (!std::strcmp(argv[i], "--dry-run")) dry = true;
        else if (!std::strcmp(argv[i], "--keep")) keep = true;
        else if (!std::strcmp(argv[i], "--list")) list = true;
        else if (!std::strcmp(argv[i], "--emit")) g_emit = true;
        else if (!std::strcmp(argv[i], "--trace")) g_trace = true;
        else if (!std::strcmp(argv[i], "--emit-golden")) emitAll = true;
        else if (!std::strcmp(argv[i], "--scenario") && i + 1 < argc) scenario = argv[++i];
        else if (argv[i][0] != '-') continue;   // the repo root build.bat passes, ignored
        else {
            std::fputs("usage: intro_curve_test --self-test [<repo root, ignored>] [--keep] | --scenario <id> [--emit] [--trace] [--keep] | --list | --emit-golden | --dry-run\n", stderr);
            return 2;
        }
    }
    if (dry) {
        std::puts("intro_curve_test: --dry-run: nothing run, nothing written");
        return 0;
    }
    if (list) {
        for (const Scenario& sc : kScenarios) std::puts(sc.id);
        return 0;
    }
    if (scenario) {
        for (const Scenario& sc : kScenarios)
            if (!std::strcmp(sc.id, scenario)) return runScenario(sc, keep);
        std::fprintf(stderr, "FAIL: no scenario named %s\n", scenario);
        return 2;
    }
    if (emitAll) {
        for (const char* id : {"golden", "head", "golden-asymmetric", "curved", "curved-asymmetric"}) {
            std::string out;
            runChild(std::wstring(L"--scenario ") + widen(id) + L" --emit", &out);
            for (const std::string& line : linesOf(out))
                if (line.rfind("EMIT ", 0) == 0) std::printf("%s\n", line.c_str() + 5);
        }
        return 0;
    }
    if (self) return runAll(keep);
    std::fputs("usage: intro_curve_test --self-test [<repo root, ignored>] [--keep] | --scenario <id> [--emit] [--trace] [--keep] | --list | --emit-golden | --dry-run\n", stderr);
    return 2;
}

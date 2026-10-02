// The curved screen's strip, run for real (src/d3d11/panel_curve.cpp; docs/screen-curvature.md; the VR world route's re-issue, commit c02dd865).
//
// The module replaces the game's 2D screen composite quad with a bent strip: it saves the game's input assembler (slot-0 vertex buffer, index
// buffer, topology), binds the strip, issues the draw through the ORIGINAL function pointer it is handed, and puts the game's state back. The
// VR world route then draws the SAME strip once more into the eye's layer (panelCurveReissue), so what has to hold is: the re-issue's input
// assembler state and draw arguments are identical to the substitution's, at any curvature, column count, sign and gain, and nothing of the
// game's own pipeline is left changed. This rig compiles the REAL panel_curve.cpp with the real Config, Log and fault guard, puts a WARP
// device behind it with the game's state (the canonical unit quad, its index pattern, the SIZE record, the placement constants, a sampler,
// a rasterizer state ...), passes it a recording draw function, and reads back what the draw saw.
//
//   C1  CURVATURE 0       at the default 64 columns nothing is wanted: panelCurveWants() is false, the re-issue is not ready and draws nothing
//                         (the composite path in vscreen.cpp is the gate for the substitution itself: it does not call in).
//   C2  THE SUBSTITUTION  curvature 0.3, 64 columns, sign +1, an explicit z gain: one draw of (384, 1, 0, 0, 0) with OUR vertex buffer (stride
//                         20, offset 0, 130 vertices) and OUR R16 index buffer (384 indices) and TRIANGLELIST bound; the vertex bytes equal an
//                         independent recomputation of the bend (UV from the UNBENT x, bottom row first, the game's winding); the screen's
//                         per-eye motion pass is issued once with the strip still bound and the curve {pi*c, 64, -gain, 0}; the game's
//                         buffers, offsets, formats and topology are back afterwards, whatever they were (canonical, odd, nothing bound),
//                         with no reference left on them.
//   C3  NO MOTION         withMotion = false: the same draw, the same state, and no motion pass.
//   C4  THE RE-ISSUE      the same draw as the substitution's (same buffer objects, same bytes, same arguments), the game's state back, no
//                         motion pass, the counter up by one; the first-call log line is written once over two re-issues.
//   C5  A LIVE CHANGE     curvature, columns, sign or gain changed in the config: the strip in hand is stale, the re-issue is not ready and
//                         draws nothing until the next substitution rebuilds; then both draws use the new strip; an unchanged config
//                         rebuilds nothing.
//   C6  THE TABLE         curvature 0.1/0.3/0.7/1.0 x columns 8/64/256 x sign +1/-1 x gain 1/35.556/200: substitution and re-issue identical in
//                         state, arguments and bytes, and the bytes match the independent bend.
//   C7  THE IDENTITY      one column at curvature 0 (the staged proof's first stage): the strip's bytes ARE the game's quad and its index
//                         buffer IS the game's.
//   C8  NOT READY         no z-gain override: nothing in vertex slots 1..3 stands the feature down; a SIZE that cannot be a panel's stands it
//                         down; the real SIZE is read through a staging copy only after a readback lag (not 10 ms after the copy, yes 60;
//                         the module's lag is 50 ms; the rig steps the clock timing.h gives tests, no Sleep), and the gain is SIZE.y x
//                         16/9 x |world x basis| / |world z basis| from the buffers the game bound. Learning is one-way (nothing in the
//                         module forgets a SIZE it has read, and one process sees one basis ratio), so this case runs second.
//   C9  FAULTS            a draw that faults: the substitution and the re-issue each return false, stand the whole feature down, put the game's
//                         state back, and later calls draw nothing; the re-issue's counter does not move.
//   C10 THE NUMBERS       panelCurveInfo() carries wanted / ready / curvature / segments / gain / reissues after each step; panelCurveShutdown()
//                         clears ready and gain and the strip in hand; the config's ranges (curvature 0..1, 1..256 columns, a z-gain
//                         override of 0..10000 and none: the gain C8 read from the game).
//   C11 THE PIPELINE      nothing but slot-0 vertex buffer, index buffer and topology is touched: the VS constant buffers, the PS resource,
//                         the sampler, the rasterizer state and viewport, the output merger and vertex slots 1..3 are the game's at draw time
//                         and afterwards.
//   C12 THE SURFACE STRIP panelCurveSurfaceWanted / Draw / Info, the strip of the intro movie and the splash (a composite that is not the
//                         screen): the arc of the screen over the CALLER's gain, direction and way for u to run (toward +1 is sign -1 in
//                         the screen's terms; reverseU true is u = (1 - x)/2 and nothing else of the strip -- the placement's +x runs to the
//                         viewer's left, as the intro composite's does), the vertex bytes equal an independent recomputation (curvature
//                         0.1..1 x columns 8/64/256 x toward +1/-1 x gain 1/4.44444/100 x reverseU false/true), one build per change of
//                         curvature, columns, gain, direction or reverseU and none otherwise (the info says which way the strip in hand
//                         runs, false once the shutdown has released it), nothing wanted
//                         or built at curvature 0 (the identity test's one column included) or above 1 or below 0, no SIZE and no override
//                         needed, no motion pass; drawn with the game's rasterizer state with the cull OFF and every other field the game's,
//                         that state created once per distinct description of the game's (and one that culls nothing, or none bound, left
//                         alone), the game's state back and no reference left on it; a null context or draw, a gain that is not a positive
//                         number and a direction that is not +-1 draw nothing and stand nothing down; the shutdown releases the strip.
//   C13 SURFACE VS SCREEN the two strips side by side: the surface leaves the screen's strip, gain, ready flag and counters alone; one generator
//                         builds both (the screen at sign -1 is the surface at toward +1, byte for byte, u along x); the screen, rebuilt after
//                         a surface strip that runs u against x, is still the strip it first drew (the screen never reverses); each build
//                         line of the surface says which way u runs; a fault of the screen's stands only
//                         the screen down, a fault of the surface's only the surface (the input assembler AND the rasterizer state back, the
//                         surface off for the session whatever the configuration or the shutdown does), each in its own log line.
//
// tools\panel_curve_test\mutants.py compiles this rig against a copy of the module with ONE rule flipped and requires the rig to fail on the
// case that belongs to the rule: every check below carries a label "C<case>.<what>", and that prefix is what the tool looks for.
//
// Usage: --self-test [<repo root, ignored>] [--keep]   |   --dry-run (nothing run, nothing written)
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "../../src/common/config.h"
#include "../../src/common/log.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/common/timing.h"
#include "binding_shadow.h"
#include "panel_curve.h"
#include "screen_motion.h"

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------------------------------------------------------------------------
// The harness: every failing check is printed once under its label, and the run goes on (one run shows every rule that broke).
// ---------------------------------------------------------------------------------------------------------------------------------
unsigned g_checks = 0;
unsigned g_failed = 0;
std::map<std::string, unsigned> g_failCount;
std::string g_note;   // what the table loop is on, appended to a failure's line

std::string fmt(const char* f, ...) {
    char buf[600];
    va_list ap;
    va_start(ap, f);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, f, ap);
    va_end(ap);
    return std::string(buf);
}

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

// The clock the module reads (timing.h: g_clockForTest is what tests swap): the 50 ms readback lag is stepped, not slept.
uint64_t g_fakeNow = 1000;
uint64_t fakeClock() { return g_fakeNow; }

// ---------------------------------------------------------------------------------------------------------------------------------
// The game's pipeline as the context reports it.
// ---------------------------------------------------------------------------------------------------------------------------------
struct Snapshot {
    // what the module changes for one draw and must put back
    ComPtr<ID3D11Buffer> vb0;
    UINT stride0 = 0, offset0 = 0;
    ComPtr<ID3D11Buffer> ib;
    DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN;
    UINT ibOffset = 0;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    // what it must never touch
    ComPtr<ID3D11Buffer> vb[3];   // vertex slots 1..3 (the SIZE record rides slot 1)
    UINT strides[3] = {0, 0, 0}, offsets[3] = {0, 0, 0};
    ComPtr<ID3D11Buffer> cb[2];   // VS constant buffers 0..1 (cb0 is the placement block the gain's world basis is read from)
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> rs;
    D3D11_VIEWPORT vp{};
    ComPtr<ID3D11BlendState> blend;
    float blendFactor[4] = {0, 0, 0, 0};
    UINT sampleMask = 0;
    ComPtr<ID3D11DepthStencilState> dss;
    UINT stencilRef = 0;
    ComPtr<ID3D11RenderTargetView> rtv;
};

Snapshot takeSnapshot(ID3D11DeviceContext* ctx) {
    Snapshot s;
    ctx->IAGetVertexBuffers(0, 1, s.vb0.GetAddressOf(), &s.stride0, &s.offset0);
    ctx->IAGetIndexBuffer(s.ib.GetAddressOf(), &s.ibFmt, &s.ibOffset);
    ctx->IAGetPrimitiveTopology(&s.topo);
    ID3D11Buffer* vb[3] = {nullptr, nullptr, nullptr};
    ctx->IAGetVertexBuffers(1, 3, vb, s.strides, s.offsets);
    for (int i = 0; i < 3; ++i) s.vb[i].Attach(vb[i]);
    ID3D11Buffer* cb[2] = {nullptr, nullptr};
    ctx->VSGetConstantBuffers(0, 2, cb);
    for (int i = 0; i < 2; ++i) s.cb[i].Attach(cb[i]);
    ctx->PSGetShaderResources(0, 1, s.srv.GetAddressOf());
    ctx->PSGetSamplers(0, 1, s.sampler.GetAddressOf());
    ctx->RSGetState(s.rs.GetAddressOf());
    UINT nvp = 1;
    ctx->RSGetViewports(&nvp, &s.vp);
    ctx->OMGetBlendState(s.blend.GetAddressOf(), s.blendFactor, &s.sampleMask);
    ctx->OMGetDepthStencilState(s.dss.GetAddressOf(), &s.stencilRef);
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, &dsv);
    s.rtv.Attach(rtv);
    if (dsv) dsv->Release();
    return s;
}

bool sameIa(const Snapshot& a, const Snapshot& b) {
    return a.vb0 == b.vb0 && a.stride0 == b.stride0 && a.offset0 == b.offset0 && a.ib == b.ib && a.ibFmt == b.ibFmt &&
           a.ibOffset == b.ibOffset && a.topo == b.topo;
}
std::string describeIa(const Snapshot& s) {
    return fmt("vb0=%p stride=%u offset=%u ib=%p fmt=%d offset=%u topology=%d", static_cast<void*>(s.vb0.Get()), s.stride0, s.offset0,
               static_cast<void*>(s.ib.Get()), static_cast<int>(s.ibFmt), s.ibOffset, static_cast<int>(s.topo));
}

enum PipelineDiff : unsigned { kDiffVs = 1, kDiffSrv = 2, kDiffSampler = 4, kDiffRs = 8, kDiffOm = 16, kDiffSlots = 32 };
unsigned pipelineDiff(const Snapshot& a, const Snapshot& b) {
    unsigned d = 0;
    if (a.cb[0] != b.cb[0] || a.cb[1] != b.cb[1]) d |= kDiffVs;
    if (a.srv != b.srv) d |= kDiffSrv;
    if (a.sampler != b.sampler) d |= kDiffSampler;
    if (a.rs != b.rs || std::memcmp(&a.vp, &b.vp, sizeof(a.vp)) != 0) d |= kDiffRs;
    if (a.blend != b.blend || std::memcmp(a.blendFactor, b.blendFactor, sizeof(a.blendFactor)) != 0 || a.sampleMask != b.sampleMask || a.dss != b.dss ||
        a.stencilRef != b.stencilRef || a.rtv != b.rtv)
        d |= kDiffOm;
    for (int i = 0; i < 3; ++i)
        if (a.vb[i] != b.vb[i] || a.strides[i] != b.strides[i] || a.offsets[i] != b.offsets[i]) d |= kDiffSlots;
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

// ---------------------------------------------------------------------------------------------------------------------------------
// What the module calls, recorded: the draw function it is handed and the screen's motion pass.
// ---------------------------------------------------------------------------------------------------------------------------------
struct DrawRecord {
    ID3D11DeviceContext* ctx = nullptr;
    UINT count = 0, instances = 0, start = 0;
    INT base = 0;
    UINT startInstance = 0;
    Snapshot snap;                    // the whole pipeline at the moment of the draw
    std::vector<uint8_t> vbBytes;     // the vertex buffer bound at slot 0 then (all of it)
    std::vector<uint8_t> ibBytes;     // the index buffer bound then
};
struct MotionCall {
    ID3D11DeviceContext* ctx = nullptr;
    edvr::PanelCurveDrawFn draw = nullptr;
    unsigned count = 0, instances = 0, start = 0;
    int base = 0;
    unsigned startInstance = 0;
    bool haveCurve = false;
    float curve[4] = {0, 0, 0, 0};
    Snapshot snap;
};
std::vector<DrawRecord> g_draws;
std::vector<MotionCall> g_motion;
unsigned g_faultDraws = 0;
bool g_motionFaults = false;   // the motion pass raises an access violation (C9)

void __stdcall recordingDraw(ID3D11DeviceContext* ctx, UINT count, UINT instances, UINT start, INT base, UINT startInstance) {
    DrawRecord r;
    r.ctx = ctx;
    r.count = count;
    r.instances = instances;
    r.start = start;
    r.base = base;
    r.startInstance = startInstance;
    r.snap = takeSnapshot(ctx);
    r.vbBytes = readBuffer(ctx, r.snap.vb0.Get());
    r.ibBytes = readBuffer(ctx, r.snap.ib.Get());
    g_draws.push_back(std::move(r));
}

// A draw that raises an access violation, the way a context gone bad does. Nothing with a destructor is alive in it.
void __stdcall faultingDraw(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT) {
    ++g_faultDraws;
    *reinterpret_cast<volatile int*>(static_cast<uintptr_t>(0x10)) = 1;
}

void resetRecorders() {
    g_draws.clear();
    g_motion.clear();
    g_faultDraws = 0;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
// What the module calls that this rig does not link: the binding shadow's resolver and the screen's motion pass.
// ---------------------------------------------------------------------------------------------------------------------------------
namespace edvr {

// The real resolver is binding_shadow.cpp, which carries the whole hook layer. The module reads only a buffer's byte width from it (info.a).
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

// The screen's per-eye motion pass (screen_motion.cpp), which the substitution issues with the strip still bound: recorded, not run.
void screenMotionDraw(ID3D11DeviceContext* ctx, PanelCurveDrawFn draw, unsigned count, unsigned instances, unsigned start, int base,
                      unsigned startInstance, const float* curve) {
    if (g_motionFaults) *reinterpret_cast<volatile int*>(static_cast<uintptr_t>(0x10)) = 1;   // before anything with a destructor exists
    MotionCall m;
    m.ctx = ctx;
    m.draw = draw;
    m.count = count;
    m.instances = instances;
    m.start = start;
    m.base = base;
    m.startInstance = startInstance;
    m.haveCurve = curve != nullptr;
    if (curve) std::memcpy(m.curve, curve, sizeof(m.curve));
    m.snap = takeSnapshot(ctx);
    g_motion.push_back(std::move(m));
}

}  // namespace edvr

namespace {

using namespace edvr;

// ---------------------------------------------------------------------------------------------------------------------------------
// The scratch directory, the log the module writes, and the files read back.
// ---------------------------------------------------------------------------------------------------------------------------------
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
        do {
            const std::wstring n = g_dir + L"\\" + fd.cFileName;
            if (n > best) best = n;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return best;
}
bool beginLog(const wchar_t* tag) { return Log::get().open(g_dir, tag); }
std::string endLog(const wchar_t* tag) {
    Log::get().close();   // the flusher is joined: the file is complete
    return slurp(newestLog(tag));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The GPU: a WARP device and the game's state.
// ---------------------------------------------------------------------------------------------------------------------------------
// The canonical unit quad (4 vertices of float3 position + float2 UV, stride 20, corners at +-1, z = 0, v = 1 at y = -1) and the game's index
// pattern. Each sits at the END of a bigger buffer, behind a decoy, so the offsets the game binds with are not zero and a restore that forgets
// them is seen.
const float kQuad[20] = {-1, -1, 0, 0, 1,   1, -1, 0, 1, 1,   -1, 1, 0, 0, 0,   1, 1, 0, 1, 0};
const uint16_t kPattern[6] = {0, 3, 1, 0, 2, 3};
constexpr UINT kQuadOffset = 80;       // bytes of decoy before the quad in the game's vertex buffer
constexpr UINT kPatternOffset = 12;    // bytes of decoy before the pattern in the game's index buffer

// The placement block cb0 (208 bytes): the world x basis at floats 36, 40, 44 and the z basis at 38, 42, 46. (3, 0, 4) is 5 long and
// (0, 0, 2.5) is 2.5 long: the basis ratio is exactly 2. Every other float is a decoy that would change the answer if it were read.
constexpr double kBasisLx = 5.0, kBasisLz = 2.5, kBasisRatio = kBasisLx / kBasisLz;
// The SIZE record. x is not y * 16/9 on purpose (native SteamVR: the FOV shape rides x), so a gain taken from x is seen.
constexpr float kSizeX = 20.254f, kSizeY = 20.0f;
constexpr double kAspect = 16.0 / 9.0;

struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Buffer> gameVb, gameIb16, gameIb32, sizeVb, cb0, cb1;
    ComPtr<ID3D11Texture2D> srvTex, rtTex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> rs;
    ComPtr<ID3D11RasterizerState> rsOdd;    // a state with every field off the default and the cull on the FRONT: what the cull-off copy must keep
    ComPtr<ID3D11RasterizerState> rsNone;   // a state that already culls nothing
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

    float vb[40];
    for (int i = 0; i < 20; ++i) vb[i] = 9.0f;                     // the decoy record
    std::memcpy(vb + 20, kQuad, sizeof(kQuad));                    // the quad, at byte 80
    g.gameVb = makeBuffer(dev, sizeof(vb), D3D11_BIND_VERTEX_BUFFER, vb);
    uint16_t ib16[12];
    for (int i = 0; i < 6; ++i) ib16[i] = 7;                       // decoy indices
    std::memcpy(ib16 + 6, kPattern, sizeof(kPattern));             // the pattern, at byte 12
    g.gameIb16 = makeBuffer(dev, sizeof(ib16), D3D11_BIND_INDEX_BUFFER, ib16);
    const uint32_t ib32[6] = {0, 3, 1, 0, 2, 3};
    g.gameIb32 = makeBuffer(dev, sizeof(ib32), D3D11_BIND_INDEX_BUFFER, ib32);

    const float size[4] = {kSizeX, kSizeY, 1.0f, 2.0f};            // the first record is the panel's SIZE
    g.sizeVb = makeBuffer(dev, sizeof(size), D3D11_BIND_VERTEX_BUFFER, size);

    float cb0[52];
    for (int i = 0; i < 52; ++i) cb0[i] = 100.0f + static_cast<float>(i);
    cb0[36] = 3.0f; cb0[40] = 0.0f; cb0[44] = 4.0f;                // the world x basis
    cb0[38] = 0.0f; cb0[42] = 0.0f; cb0[46] = 2.5f;                // the world z basis
    g.cb0 = makeBuffer(dev, sizeof(cb0), D3D11_BIND_CONSTANT_BUFFER, cb0);
    const float cb1[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    g.cb1 = makeBuffer(dev, sizeof(cb1), D3D11_BIND_CONSTANT_BUFFER, cb1);

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
    rd.CullMode = D3D11_CULL_BACK;   // the game's: back faces culled, so the winding is load-bearing
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &g.rs);
    D3D11_RASTERIZER_DESC odd{};
    odd.FillMode = D3D11_FILL_SOLID;
    odd.CullMode = D3D11_CULL_FRONT;
    odd.FrontCounterClockwise = TRUE;
    odd.DepthBias = 7;
    odd.DepthBiasClamp = 0.5f;
    odd.SlopeScaledDepthBias = 1.25f;
    odd.DepthClipEnable = FALSE;
    odd.ScissorEnable = TRUE;
    odd.MultisampleEnable = FALSE;
    odd.AntialiasedLineEnable = TRUE;
    dev->CreateRasterizerState(&odd, &g.rsOdd);
    D3D11_RASTERIZER_DESC none = rd;
    none.CullMode = D3D11_CULL_NONE;
    // Not the cull-off copy of the game's state under another name: the runtime hands back ONE object for identical descriptions, so a
    // state equal to what the module derives from rs would share its reference count with the module's cache and hide a leak (or look like one).
    none.DepthBias = 3;
    none.SlopeScaledDepthBias = 0.5f;
    dev->CreateRasterizerState(&none, &g.rsNone);
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

    g.ok = g.gameVb && g.gameIb16 && g.gameIb32 && g.sizeVb && g.cb0 && g.cb1 && g.srv && g.rtv && g.sampler && g.rs && g.rsOdd && g.rsNone && g.blend && g.dss;
    return g;
}

// The pipeline around the draw, bound once and never rebound: if the module changes any of it, it stays changed and every later check sees it.
void bindSizeSlot(Gpu& g, bool on) {
    ID3D11Buffer* vbs[3] = {on ? g.sizeVb.Get() : nullptr, nullptr, nullptr};
    UINT strides[3] = {on ? 8u : 0u, 0u, 0u};
    UINT offsets[3] = {0, 0, 0};
    g.ctx->IASetVertexBuffers(1, 3, vbs, strides, offsets);
}
void bindPipeline(Gpu& g) {
    ID3D11DeviceContext* c = g.ctx.Get();
    ID3D11Buffer* cbs[2] = {g.cb0.Get(), g.cb1.Get()};
    c->VSSetConstantBuffers(0, 2, cbs);
    ID3D11ShaderResourceView* srv = g.srv.Get();
    c->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* smp = g.sampler.Get();
    c->PSSetSamplers(0, 1, &smp);
    c->RSSetState(g.rs.Get());
    D3D11_VIEWPORT vp{0.0f, 0.0f, 16.0f, 16.0f, 0.0f, 1.0f};
    c->RSSetViewports(1, &vp);
    const float factor[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    c->OMSetBlendState(g.blend.Get(), factor, 0xFFFFFFFEu);
    c->OMSetDepthStencilState(g.dss.Get(), 7);
    ID3D11RenderTargetView* rtv = g.rtv.Get();
    c->OMSetRenderTargets(1, &rtv, nullptr);
    bindSizeSlot(g, true);
}

// The game's input assembler at the moment of the composite draw, in three states: the canonical one (the quad and the pattern, bound at
// the offsets they sit at, TRIANGLELIST), an odd one (other stride, offsets, index format and topology: the restore must put back whatever
// is there, not what the module expects), and nothing bound at all (a null buffer is a legitimate restore).
enum class GameState { Canonical, Odd, Empty };
void bindGame(Gpu& g, GameState v) {
    ID3D11DeviceContext* c = g.ctx.Get();
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, offset = 0;
    switch (v) {
    case GameState::Canonical:
        vb = g.gameVb.Get();
        stride = 20;
        offset = kQuadOffset;
        c->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        c->IASetIndexBuffer(g.gameIb16.Get(), DXGI_FORMAT_R16_UINT, kPatternOffset);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        break;
    case GameState::Odd:
        vb = g.gameVb.Get();
        stride = 40;
        offset = 20;
        c->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        c->IASetIndexBuffer(g.gameIb32.Get(), DXGI_FORMAT_R32_UINT, 4);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        break;
    case GameState::Empty:
        c->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        c->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        break;
    }
}

void setSize(Gpu& g, float x, float y) {
    const float size[4] = {x, y, 1.0f, 2.0f};
    g.ctx->UpdateSubresource(g.sizeVb.Get(), 0, nullptr, size, 0, 0);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The configuration, and the strip the configuration asks for -- recomputed here, not read from the module.
// ---------------------------------------------------------------------------------------------------------------------------------
struct Spec {
    float curvature;
    int columns;
    int sign;
    float gain;
    bool reverseU = false;   // u = (1 - x)/2 instead of (x + 1)/2: a surface whose placement has +x running to the viewer's left (never the screen's)
};
Spec specOf(const char* curvature, int columns, int sign, const char* gain) {
    return Spec{std::strtof(curvature, nullptr), columns, sign, std::strtof(gain, nullptr)};
}
void applyConfig(const char* curvature, int columns, int sign, const char* gain) {
    Config& c = Config::get();
    c.set("fix.panel_curvature", curvature);
    c.set("advanced.panel_curvature_segments", std::to_string(columns).c_str());
    c.set("advanced.panel_curvature_sign", std::to_string(sign).c_str());
    c.set("advanced.panel_curvature_z_gain", gain);   // "0" is no override: the gain is read from the game's buffers
    panelCurveConfigure(c);
}
void clearStandDown() { detail::g_panelCurveStoodDown = false; }

constexpr double kPiD = 3.14159265358979323846;
struct Vtx {
    float x, y, z, u, v;
};
static_assert(sizeof(Vtx) == 20, "the composite's stride is 20 bytes");

// theta = pi c x; x' = sin(theta)/(pi c); z' = -sign * gain * (1 - cos(theta))/(pi c); the UV from the UNBENT x (u = (x+1)/2, or (1-x)/2 for a
// reversed surface; v = 1 on the bottom row and 0 on the top); bottom row first, then the top row; per quad bl,tr,br then bl,tl,tr (the
// game's own pattern).
void expectedStrip(const Spec& s, std::vector<Vtx>* verts, std::vector<uint16_t>* idx) {
    const int n = s.columns;
    const double c = s.curvature;
    verts->assign(static_cast<size_t>(2 * (n + 1)), Vtx{});
    for (int i = 0; i <= n; ++i) {
        const double x = -1.0 + 2.0 * i / n;
        const double u = s.reverseU ? (1.0 - x) / 2.0 : (x + 1.0) / 2.0;
        double bx = x, bz = 0.0;
        if (c > 0.0) {
            const double k = kPiD * c, theta = k * x;
            bx = std::sin(theta) / k;
            bz = -static_cast<double>(s.sign) * static_cast<double>(s.gain) * (1.0 - std::cos(theta)) / k;
        }
        (*verts)[static_cast<size_t>(i)] = Vtx{static_cast<float>(bx), -1.0f, static_cast<float>(bz), static_cast<float>(u), 1.0f};
        (*verts)[static_cast<size_t>(n + 1 + i)] = Vtx{static_cast<float>(bx), 1.0f, static_cast<float>(bz), static_cast<float>(u), 0.0f};
    }
    idx->clear();
    for (int i = 0; i < n; ++i) {
        const uint16_t bl = static_cast<uint16_t>(i), br = static_cast<uint16_t>(i + 1);
        const uint16_t tl = static_cast<uint16_t>(n + 1 + i), tr = static_cast<uint16_t>(n + 2 + i);
        idx->insert(idx->end(), {bl, tr, br, bl, tl, tr});
    }
}

// "" when the two buffers are the strip the spec asks for, else the first thing that is not.
std::string stripProblem(const std::vector<uint8_t>& vb, const std::vector<uint8_t>& ib, const Spec& s) {
    std::vector<Vtx> want;
    std::vector<uint16_t> wantIdx;
    expectedStrip(s, &want, &wantIdx);
    if (vb.size() != want.size() * sizeof(Vtx)) return fmt("the vertex buffer is %zu bytes, want %zu (%zu vertices)", vb.size(), want.size() * sizeof(Vtx), want.size());
    if (ib.size() != wantIdx.size() * sizeof(uint16_t)) return fmt("the index buffer is %zu bytes, want %zu (%zu indices)", ib.size(), wantIdx.size() * sizeof(uint16_t), wantIdx.size());
    const double k = kPiD * s.curvature;
    const double tolX = 2e-6 * (1.0 + (k > 0.0 ? 1.0 / k : 0.0));
    const double tolZ = 2e-6 * (1.0 + (k > 0.0 ? std::fabs(static_cast<double>(s.gain)) / k : 0.0));
    for (size_t i = 0; i < want.size(); ++i) {
        Vtx got;
        std::memcpy(&got, vb.data() + i * sizeof(Vtx), sizeof(Vtx));
        const Vtx& w = want[i];
        if (!closeTo(got.x, w.x, tolX)) return fmt("vertex %zu x = %.7g, want %.7g", i, got.x, w.x);
        if (got.y != w.y) return fmt("vertex %zu y = %.7g, want %.7g", i, got.y, w.y);
        if (!closeTo(got.z, w.z, tolZ)) return fmt("vertex %zu z = %.7g, want %.7g", i, got.z, w.z);
        if (!closeTo(got.u, w.u, 1e-6)) return fmt("vertex %zu u = %.7g, want %.7g", i, got.u, w.u);
        if (got.v != w.v) return fmt("vertex %zu v = %.7g, want %.7g", i, got.v, w.v);
    }
    for (size_t i = 0; i < wantIdx.size(); ++i) {
        uint16_t got;
        std::memcpy(&got, ib.data() + i * sizeof(uint16_t), sizeof(got));
        if (got != wantIdx[i]) return fmt("index %zu = %u, want %u", i, got, wantIdx[i]);
    }
    return std::string();
}

// "" when two draws are the same draw: the context, the arguments, the buffers bound (the same objects, strides, offsets, format), the
// topology and the bytes in them.
std::string drawDifference(const DrawRecord& a, const DrawRecord& b) {
    if (a.ctx != b.ctx) return "another context";
    if (a.count != b.count || a.instances != b.instances || a.start != b.start || a.base != b.base || a.startInstance != b.startInstance)
        return fmt("arguments (%u,%u,%u,%d,%u) against (%u,%u,%u,%d,%u)", a.count, a.instances, a.start, a.base, a.startInstance, b.count, b.instances,
                   b.start, b.base, b.startInstance);
    if (!sameIa(a.snap, b.snap)) return "input assembler: " + describeIa(a.snap) + " against " + describeIa(b.snap);
    if (a.vbBytes != b.vbBytes) return "vertex bytes differ";
    if (a.ibBytes != b.ibBytes) return "index bytes differ";
    return std::string();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The checks that belong to every strip draw, whoever made it.
// ---------------------------------------------------------------------------------------------------------------------------------
std::string L(const std::string& tag, const char* who, const char* what) { return tag + "." + who + "-" + what; }

// One draw of the strip the spec asks for: our buffers bound (and not the game's), the arguments, the bytes, the pipeline around it the game's.
void verifyStripDraw(const std::string& tag, const char* who, const DrawRecord& d, const Spec& s, const Gpu& g, ID3D11DeviceContext* ctx, const Snapshot& baseline) {
    const UINT n = static_cast<UINT>(s.columns);
    check(d.ctx == ctx, L(tag, who, "context"), "the draw was handed another context");
    check(d.count == 6 * n && d.instances == 1 && d.start == 0 && d.base == 0 && d.startInstance == 0, L(tag, who, "arguments"),
          fmt("drawn with (%u, %u, %u, %d, %u), want (%u, 1, 0, 0, 0)", d.count, d.instances, d.start, d.base, d.startInstance, 6 * n));
    check(d.snap.vb0 && d.snap.vb0 != g.gameVb && d.snap.stride0 == 20 && d.snap.offset0 == 0, L(tag, who, "vertex-buffer"),
          "slot 0 held " + describeIa(d.snap) + " (the strip's own buffer at stride 20, offset 0 is wanted, not the game's)");
    check(d.snap.ib && d.snap.ib != g.gameIb16 && d.snap.ib != g.gameIb32 && d.snap.ibFmt == DXGI_FORMAT_R16_UINT && d.snap.ibOffset == 0, L(tag, who, "index-buffer"),
          "the index buffer was " + describeIa(d.snap));
    check(d.snap.topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, L(tag, who, "topology"), fmt("topology %d at the draw", static_cast<int>(d.snap.topo)));
    check(d.vbBytes.size() == 20u * 2u * (n + 1) && d.ibBytes.size() == 2u * 6u * n, L(tag, who, "sizes"),
          fmt("%zu vertex bytes and %zu index bytes, want %u and %u", d.vbBytes.size(), d.ibBytes.size(), 20u * 2u * (n + 1), 2u * 6u * n));
    const std::string problem = stripProblem(d.vbBytes, d.ibBytes, s);
    check(problem.empty(), L(tag, who, "bytes"), problem);
    check(pipelineDiff(d.snap, baseline) == 0, L(tag, who, "pipeline"), fmt("the pipeline around the draw differs from the game's (bits %u)", pipelineDiff(d.snap, baseline)));
}

// How many references the game's own input-assembler buffers carry. The module takes one on each with IAGet* and gives it back in the
// restore; a restore that forgets leaks the game's buffers, one reference a draw. Measured around a call, with the rig's own references
// (the snapshot taken before it) alive at both ends.
// The rasterizer states are in it too: the surface strip takes a reference on the game's with RSGetState and owes it back.
struct Refs {
    ULONG vb = 0, ib16 = 0, ib32 = 0, rs = 0, rsOdd = 0, rsNone = 0;
    bool operator==(const Refs& o) const { return vb == o.vb && ib16 == o.ib16 && ib32 == o.ib32 && rs == o.rs && rsOdd == o.rsOdd && rsNone == o.rsNone; }
};
ULONG refCount(IUnknown* u) {
    if (!u) return 0;
    u->AddRef();
    return u->Release();
}
Refs takeRefs(const Gpu& g) {
    Refs r{refCount(g.gameVb.Get()), refCount(g.gameIb16.Get()), refCount(g.gameIb32.Get()), refCount(g.rs.Get()), refCount(g.rsOdd.Get()), refCount(g.rsNone.Get())};
    // The recorders' snapshots hold whatever rasterizer state was bound at each draw -- the game's own, for a draw that did not change it. Those
    // are the rig's references, not the module's, and they come and go with the recorders: taken out, so a reference the MODULE forgot to give
    // back is still one too many. (The recorded buffers are the strips', never the game's, so the buffer counts need no such correction.)
    auto held = [&](const Snapshot& s) {
        const ID3D11RasterizerState* at = s.rs.Get();
        if (!at) return;
        if (at == g.rs.Get()) --r.rs;
        else if (at == g.rsOdd.Get()) --r.rsOdd;
        else if (at == g.rsNone.Get()) --r.rsNone;
    };
    for (const DrawRecord& d : g_draws) held(d.snap);
    for (const MotionCall& m : g_motion) held(m.snap);
    return r;
}

// The game's state back after a call: the input assembler exactly as it was, everything else too, and no reference left behind.
void verifyRestored(const std::string& tag, const char* who, const Snapshot& before, const Snapshot& after, const Refs* refs0 = nullptr, const Refs* refs1 = nullptr) {
    check(sameIa(before, after), L(tag, who, "restored"), "was " + describeIa(before) + ", now " + describeIa(after));
    check(pipelineDiff(before, after) == 0, L(tag, who, "restored-pipeline"), fmt("the pipeline differs afterwards (bits %u)", pipelineDiff(before, after)));
    if (refs0 && refs1)
        check(*refs0 == *refs1, L(tag, who, "restored-refs"),
              fmt("references on the game's vertex / index buffers and rasterizer states went from %lu/%lu/%lu and %lu/%lu/%lu to %lu/%lu/%lu and %lu/%lu/%lu", refs0->vb, refs0->ib16,
                  refs0->ib32, refs0->rs, refs0->rsOdd, refs0->rsNone, refs1->vb, refs1->ib16, refs1->ib32, refs1->rs, refs1->rsOdd, refs1->rsNone));
}

// The motion pass of a substitution: once, the strip's own draw arguments, the original function pointer, the curve {pi c, columns, -sign gain, 0},
// and the strip still bound (the game's tail issues it with the strip in hand).
void verifyMotion(const std::string& tag, const Spec& s, ID3D11DeviceContext* ctx, edvr::PanelCurveDrawFn drawFn, const DrawRecord& d) {
    check(g_motion.size() == 1, L(tag, "motion", "once"), fmt("the motion pass was issued %zu times, want 1", g_motion.size()));
    if (g_motion.size() != 1) return;
    const MotionCall& m = g_motion[0];
    check(m.ctx == ctx && m.draw == drawFn, L(tag, "motion", "original-pointer"), "the motion pass was handed another context or draw function");
    check(m.count == 6u * static_cast<unsigned>(s.columns) && m.instances == 1 && m.start == 0 && m.base == 0 && m.startInstance == 0, L(tag, "motion", "arguments"),
          fmt("(%u, %u, %u, %d, %u)", m.count, m.instances, m.start, m.base, m.startInstance));
    const float wantCurve[4] = {static_cast<float>(kPiD * s.curvature), static_cast<float>(s.columns), static_cast<float>(-s.sign) * s.gain, 0.0f};
    bool curveOk = m.haveCurve;
    for (int i = 0; i < 4 && curveOk; ++i) curveOk = closeTo(m.curve[i], wantCurve[i], 1e-5 * (1.0 + std::fabs(static_cast<double>(wantCurve[i]))));
    check(curveOk, L(tag, "motion", "curve"),
          fmt("curve {%.6g, %.6g, %.6g, %.6g}, want {%.6g, %.6g, %.6g, %.6g}", m.curve[0], m.curve[1], m.curve[2], m.curve[3], wantCurve[0], wantCurve[1], wantCurve[2], wantCurve[3]));
    check(m.snap.vb0 == d.snap.vb0 && m.snap.ib == d.snap.ib && m.snap.stride0 == 20 && m.snap.topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, L(tag, "motion", "strip-bound"),
          "at the motion pass the input assembler was " + describeIa(m.snap));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C1: curvature 0 at the default segment count.
// ---------------------------------------------------------------------------------------------------------------------------------
void case1(Gpu& g) {
    std::printf("C1 curvature 0 at the default 64 columns\n");
    ID3D11DeviceContext* ctx = g.ctx.Get();
    // No key set yet: the shipped state, which is off.
    panelCurveConfigure(Config::get());
    {
        const PanelCurveInfo d = panelCurveInfo();
        check(d.curvature == 0.0f && d.segments == 64 && !d.wanted, "C1.defaults: with no key set the curvature is 0 and the column count 64: off",
              fmt("curvature=%g segments=%d wanted=%d", d.curvature, d.segments, d.wanted));
    }
    applyConfig("0", 64, 1, "35.556");
    bindGame(g, GameState::Canonical);
    resetRecorders();
    const Snapshot before = takeSnapshot(ctx);
    const Refs refs0 = takeRefs(g);
    check(!panelCurveWants(), "C1.wants: curvature 0 at 64 columns is not wanted: the composite path does not call in");
    const PanelCurveInfo i = panelCurveInfo();
    check(!i.wanted && !i.standDown && !i.ready && i.curvature == 0.0f && i.segments == 64 && i.gain == 0.0f && i.reissues == 0, "C1.info: panelCurveInfo() says off, not stood down, nothing built, no re-issues",
          fmt("wanted=%d standDown=%d ready=%d curvature=%g segments=%d gain=%g reissues=%llu", i.wanted, i.standDown, i.ready, i.curvature, i.segments, i.gain, static_cast<unsigned long long>(i.reissues)));
    check(!panelCurveReissueReady(), "C1.reissue-ready: the re-issue is not ready when nothing is wanted");
    check(!panelCurveReissue(ctx, recordingDraw), "C1.reissue: the re-issue returns false");
    check(g_draws.empty() && g_motion.empty(), "C1.no-draw: nothing was drawn and no motion pass was issued", fmt("%zu draws", g_draws.size()));
    check(panelCurveInfo().reissues == 0, "C1.counter: the re-issue counter did not move");
    const Refs refs1 = takeRefs(g);
    verifyRestored("C1", "refused", before, takeSnapshot(ctx), &refs0, &refs1);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C8: not ready. The first case to use the substitution, because what it learns is never forgotten.
// ---------------------------------------------------------------------------------------------------------------------------------
void case8(Gpu& g) {
    std::printf("C8 the gain is not known yet\n");
    ID3D11DeviceContext* ctx = g.ctx.Get();
    const char* tag = "C8";
    applyConfig("0.3", 64, 1, "0");   // no override: the gain has to be read
    bindGame(g, GameState::Canonical);
    resetRecorders();
    check(beginLog(L"pclearn"), "C8.log: the scratch log opens");

    // Calls that cannot be served are refused plainly, and do not stand the feature down.
    {
        const bool a = panelCurveSubstitute(nullptr, recordingDraw);
        const bool b = panelCurveSubstitute(ctx, nullptr);
        const bool c = panelCurveReissue(nullptr, recordingDraw);
        const bool d = panelCurveReissue(ctx, nullptr);
        check(!a && !b && !c && !d && g_draws.empty(), "C8.null-arguments: a null context or a null draw function is refused with no draw");
        check(!panelCurveInfo().standDown && !panelCurveInfo().ready, "C8.null-quiet: ...and neither stands the feature down nor reads as ready");
    }

    // 1. Nothing in vertex slots 1..3: no SIZE to read, so no gain; the module stands down for the session and the game's own quad is drawn.
    bindSizeSlot(g, false);
    {
        const Snapshot before = takeSnapshot(ctx);
        const bool r = panelCurveSubstitute(ctx, recordingDraw);
        check(!r && g_draws.empty() && g_motion.empty(), "C8.nothing-bound: with no SIZE bound the substitution returns false and draws nothing");
        const PanelCurveInfo i = panelCurveInfo();
        check(i.standDown && !i.ready && !i.wanted && !panelCurveWants() && !panelCurveReissueReady(), "C8.nothing-bound-state: ...and stands the feature down (not wanted, not ready)",
              fmt("standDown=%d ready=%d wanted=%d", i.standDown, i.ready, i.wanted));
        verifyRestored(tag, "nothing-bound", before, takeSnapshot(ctx));
    }
    clearStandDown();
    bindSizeSlot(g, true);

    // 2. A SIZE that cannot be a panel's (0 x 0): read after the lag, refused, and the feature stands down.
    setSize(g, 0.0f, 0.0f);
    g_fakeNow = 10000;
    resetRecorders();
    {
        const bool first = panelCurveSubstitute(ctx, recordingDraw);
        check(!first && g_draws.empty() && !panelCurveInfo().standDown, "C8.bad-size-copy: the first call starts the copy, returns false and draws nothing");
        g_fakeNow += 60;
        const bool second = panelCurveSubstitute(ctx, recordingDraw);
        check(!second && g_draws.empty() && panelCurveInfo().standDown && !panelCurveInfo().ready, "C8.bad-size: a SIZE of 0 x 0 cannot be a panel's: false, no draw, stood down");
    }
    clearStandDown();

    // 3. The real SIZE. The first call starts the copy; the lag has not elapsed 10 ms later; after 60 ms the read happens and the strip is drawn.
    //    (The module's lag is 50 ms. Only "not at once" and "not never" are pinned, not the exact figure.)
    setSize(g, kSizeX, kSizeY);
    g_fakeNow += 1000;
    resetRecorders();
    {
        const bool first = panelCurveSubstitute(ctx, recordingDraw);
        check(!first && g_draws.empty() && g_motion.empty(), "C8.first-call: the first call starts the copy and returns false with no draw");
        const PanelCurveInfo i = panelCurveInfo();
        check(i.wanted && !i.ready && !i.standDown && i.gain == 0.0f && !panelCurveReissueReady(), "C8.first-call-state: wanted, not ready, not stood down, no gain yet, re-issue not ready",
              fmt("wanted=%d ready=%d standDown=%d gain=%g", i.wanted, i.ready, i.standDown, i.gain));
        g_fakeNow += 10;
        const bool early = panelCurveSubstitute(ctx, recordingDraw);
        check(!early && g_draws.empty() && !panelCurveInfo().ready, "C8.lag: 10 ms after the copy it is not read yet: false, no draw");
        g_fakeNow += 50;   // 60 ms after the copy
        const Snapshot before = takeSnapshot(ctx);
        const Refs refs0 = takeRefs(g);
        const bool second = panelCurveSubstitute(ctx, recordingDraw);
        const Refs refs1 = takeRefs(g);
        check(second && g_draws.size() == 1, "C8.learned: 60 ms after the copy the SIZE is read and the strip is drawn once", fmt("returned %d, %zu draws", second, g_draws.size()));
        const double wantGain = kSizeY * kAspect * kBasisRatio;   // SIZE.y x 16/9 x |world x basis| / |world z basis|
        const PanelCurveInfo li = panelCurveInfo();
        check(closeTo(li.gain, wantGain, 1e-3), "C8.gain: the gain is SIZE.y x 16/9 x the world basis ratio, from the buffers the game bound",
              fmt("gain %.5f, want %.5f (SIZE.x would give %.5f, no ratio %.5f)", li.gain, wantGain, kSizeX * kBasisRatio, kSizeY * kAspect));
        check(li.ready && li.wanted && !li.standDown && panelCurveReissueReady(), "C8.ready: ready, wanted, not stood down, and the re-issue is ready now");
        if (g_draws.size() == 1) {
            const Spec s{0.3f, 64, 1, static_cast<float>(wantGain)};
            verifyStripDraw(tag, "learned", g_draws[0], s, g, ctx, before);
            verifyMotion(tag, s, ctx, recordingDraw, g_draws[0]);
        }
        verifyRestored(tag, "learned", before, takeSnapshot(ctx), &refs0, &refs1);
    }
    const std::string log = endLog(L"pclearn");
    check(count(log, "nothing is bound to vertex slots 1..3") == 1, "C8.log-nothing-bound: the log says once that nothing is bound to the SIZE slots");
    check(count(log, "cannot be a panel size") == 1, "C8.log-bad-size: the log says once that the SIZE read back cannot be a panel's");
    check(has(log, "71.111") && has(log, "20.254") && has(log, "5.000") && has(log, "2.500"), "C8.log-numbers: the log carries the SIZE, the two basis lengths and the gain it uses");
    check(count(log, "substituting the panel's quad for a 64-column strip") == 1, "C8.log-substituting: the first substitution is announced once");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C2 and C3: the substitution, and the substitution without its motion pass.
// ---------------------------------------------------------------------------------------------------------------------------------
DrawRecord g_refSub;   // the substitution at (0.3, 64, +1, 35.556), kept for the cases that compare against it

void case2(Gpu& g) {
    std::printf("C2 the substitution: curvature 0.3, 64 columns, sign +1, gain 35.556\n");
    ID3D11DeviceContext* ctx = g.ctx.Get();
    applyConfig("0.3", 64, 1, "35.556");
    const Spec s = specOf("0.3", 64, 1, "35.556");
    const struct { GameState state; const char* name; } states[] = {{GameState::Canonical, "canonical"}, {GameState::Odd, "odd"}, {GameState::Empty, "empty"}};
    for (const auto& st : states) {
        bindGame(g, st.state);
        resetRecorders();
        const Snapshot before = takeSnapshot(ctx);
        const Refs refs0 = takeRefs(g);
        const std::string tag = std::string("C2");
        const std::string who = std::string("sub-") + st.name;
        const bool r = panelCurveSubstitute(ctx, recordingDraw, true);
        const Refs refs1 = takeRefs(g);
        check(r, L(tag, who.c_str(), "returned"), "the substitution returned false");
        check(g_draws.size() == 1, L(tag, who.c_str(), "draws"), fmt("%zu draws, want 1", g_draws.size()));
        if (g_draws.size() == 1) {
            const Snapshot baseline = before;
            verifyStripDraw(tag, who.c_str(), g_draws[0], s, g, ctx, baseline);
            verifyMotion(tag + "." + st.name, s, ctx, recordingDraw, g_draws[0]);
            if (st.state == GameState::Canonical) g_refSub = g_draws[0];
            else if (g_refSub.ctx) check(drawDifference(g_draws[0], g_refSub).empty(), L(tag, who.c_str(), "same-strip"), drawDifference(g_draws[0], g_refSub));
        }
        verifyRestored(tag, who.c_str(), before, takeSnapshot(ctx), &refs0, &refs1);
        const PanelCurveInfo i = panelCurveInfo();
        check(i.ready && i.wanted && !i.standDown && i.curvature == s.curvature && i.segments == 64 && closeTo(i.gain, s.gain, 1e-4), L(tag, who.c_str(), "info"),
              fmt("ready=%d wanted=%d standDown=%d curvature=%g segments=%d gain=%g", i.ready, i.wanted, i.standDown, i.curvature, i.segments, i.gain));
    }
    // the shape of the strip the game's own draw is replaced by: 130 vertices, 384 indices
    check(g_refSub.vbBytes.size() == 130 * 20 && g_refSub.ibBytes.size() == 384 * 2, "C2.shape: 64 columns are 130 vertices and 384 indices", fmt("%zu and %zu bytes", g_refSub.vbBytes.size(), g_refSub.ibBytes.size()));
}

void case3(Gpu& g) {
    std::printf("C3 the substitution without its motion pass\n");
    ID3D11DeviceContext* ctx = g.ctx.Get();
    applyConfig("0.3", 64, 1, "35.556");
    const Spec s = specOf("0.3", 64, 1, "35.556");
    bindGame(g, GameState::Canonical);
    resetRecorders();
    const Snapshot before = takeSnapshot(ctx);
    const Refs refs0 = takeRefs(g);
    const bool r = panelCurveSubstitute(ctx, recordingDraw, false);
    const Refs refs1 = takeRefs(g);
    check(r && g_draws.size() == 1, "C3.drawn: the substitution still draws the strip once", fmt("returned %d, %zu draws", r, g_draws.size()));
    check(g_motion.empty(), "C3.no-motion: withMotion = false issues no motion pass", fmt("%zu motion passes", g_motion.size()));
    if (g_draws.size() == 1) {
        verifyStripDraw("C3", "sub", g_draws[0], s, g, ctx, before);
        check(drawDifference(g_draws[0], g_refSub).empty(), "C3.same-as-with-motion: the same draw, state and bytes as the substitution with its motion pass", drawDifference(g_draws[0], g_refSub));
    }
    verifyRestored("C3", "sub", before, takeSnapshot(ctx), &refs0, &refs1);
    check(panelCurveInfo().ready, "C3.ready: ready after a substitution without motion");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C4: the re-issue.
// ---------------------------------------------------------------------------------------------------------------------------------
void case4(Gpu& g) {
    std::printf("C4 the re-issue is the substitution's draw again\n");
    ID3D11DeviceContext* ctx = g.ctx.Get();
    applyConfig("0.3", 64, 1, "35.556");
    const Spec s = specOf("0.3", 64, 1, "35.556");
    bindGame(g, GameState::Canonical);
    detail::g_panelCurveReissues = 0;   // the first-call line is keyed on the cumulative counter
    check(beginLog(L"pcreissue"), "C4.log: the scratch log opens");
    resetRecorders();
    check(panelCurveSubstitute(ctx, recordingDraw, true) && g_draws.size() == 1, "C4.setup: the substitution drew the strip");
    if (g_draws.size() != 1) { endLog(L"pcreissue"); return; }
    const DrawRecord sub = g_draws[0];

    for (int k = 1; k <= 2; ++k) {
        const std::string who = std::string("reissue") + std::to_string(k);
        resetRecorders();
        const Snapshot before = takeSnapshot(ctx);
        const Refs refs0 = takeRefs(g);
        check(panelCurveReissueReady(), L("C4", who.c_str(), "ready"), "the strip in hand is the one asked for, so the re-issue is ready");
        const uint64_t was = panelCurveInfo().reissues;
        const bool r = panelCurveReissue(ctx, recordingDraw);
        const Refs refs1 = takeRefs(g);
        check(r && g_draws.size() == 1, L("C4", who.c_str(), "drawn"), fmt("returned %d, %zu draws", r, g_draws.size()));
        if (g_draws.size() == 1) {
            verifyStripDraw("C4", who.c_str(), g_draws[0], s, g, ctx, before);
            const std::string diff = drawDifference(g_draws[0], sub);
            check(diff.empty(), L("C4", who.c_str(), "identical"), "the re-issue differs from the substitution's draw: " + diff);
        }
        check(g_motion.empty(), L("C4", who.c_str(), "no-motion"), "the re-issue issued the screen's motion pass");
        verifyRestored("C4", who.c_str(), before, takeSnapshot(ctx), &refs0, &refs1);
        check(panelCurveInfo().reissues == was + 1, L("C4", who.c_str(), "counter"), fmt("the counter went from %llu to %llu", static_cast<unsigned long long>(was), static_cast<unsigned long long>(panelCurveInfo().reissues)));
        check(panelCurveInfo().ready, L("C4", who.c_str(), "ready-flag"), "the re-issue left the substitution's ready flag alone");
    }
    const std::string log = endLog(L"pcreissue");
    check(count(log, "the VR world route's layer drew the same") == 1, "C4.log-once: the first-call line is written once over two re-issues", fmt("%zu times", count(log, "the VR world route's layer drew the same")));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C5: a live config change.
// ---------------------------------------------------------------------------------------------------------------------------------
struct Cfg {
    const char* curvature;
    int columns;
    int sign;
    const char* gain;
};
void apply(const Cfg& c) { applyConfig(c.curvature, c.columns, c.sign, c.gain); }
Spec specOf(const Cfg& c) { return specOf(c.curvature, c.columns, c.sign, c.gain); }

void case5(Gpu& g) {
    std::printf("C5 a live config change\n");
    ID3D11DeviceContext* ctx = g.ctx.Get();
    bindGame(g, GameState::Canonical);
    const Cfg base = {"0.3", 64, 1, "35.556"};
    const struct { const char* name; Cfg cfg; } dims[] = {{"curvature", {"0.5", 64, 1, "35.556"}},
                                                         {"columns", {"0.3", 32, 1, "35.556"}},
                                                         {"sign", {"0.3", 64, -1, "35.556"}},
                                                         {"gain", {"0.3", 64, 1, "50"}}};

    // An unchanged config rebuilds nothing: two substitutions and a re-issue use the one pair of buffers.
    apply(base);
    resetRecorders();
    check(panelCurveSubstitute(ctx, recordingDraw, true), "C5.base: the substitution at the base config drew");
    check(panelCurveSubstitute(ctx, recordingDraw, true), "C5.base-again: ...and again");
    check(panelCurveReissue(ctx, recordingDraw), "C5.base-reissue: ...and the re-issue");
    check(g_draws.size() == 3 && drawDifference(g_draws[0], g_draws[1]).empty() && drawDifference(g_draws[0], g_draws[2]).empty(), "C5.kept: an unchanged config rebuilds nothing: three draws, one strip",
          g_draws.size() == 3 ? drawDifference(g_draws[0], g_draws[1]) + drawDifference(g_draws[0], g_draws[2]) : fmt("%zu draws", g_draws.size()));
    if (g_draws.size() < 1) return;
    const DrawRecord baseDraw = g_draws[0];

    for (const auto& dim : dims) {
        const std::string name = dim.name;
        const Spec ns = specOf(dim.cfg);
        // changed: stale
        apply(dim.cfg);
        check(panelCurveInfo().wanted, L("C5", name.c_str(), "wanted"), "the changed config is still a wanted substitution");
        check(!panelCurveReissueReady(), L("C5", name.c_str(), "stale"), "the strip in hand was built for the old " + name + " and the re-issue says it is ready");
        resetRecorders();
        const uint64_t was = panelCurveInfo().reissues;
        check(!panelCurveReissue(ctx, recordingDraw) && g_draws.empty() && panelCurveInfo().reissues == was, L("C5", name.c_str(), "refused"), "the re-issue before the rebuild drew, or counted, or returned true");
        // rebuilt by the next substitution; both draws use the new strip
        const Snapshot before = takeSnapshot(ctx);
        resetRecorders();
        check(panelCurveSubstitute(ctx, recordingDraw, true) && g_draws.size() == 1, L("C5", name.c_str(), "rebuilt"), "the substitution after the change did not draw");
        check(panelCurveReissueReady(), L("C5", name.c_str(), "ready-again"), "the re-issue is not ready after the rebuild");
        if (g_draws.size() == 1) {
            verifyStripDraw("C5." + name, "sub", g_draws[0], ns, g, ctx, before);
            check(g_draws[0].vbBytes != baseDraw.vbBytes || g_draws[0].ibBytes != baseDraw.ibBytes, L("C5", name.c_str(), "new-strip"), "the strip did not change when " + name + " did");
            const DrawRecord sub = g_draws[0];
            resetRecorders();
            check(panelCurveReissue(ctx, recordingDraw) && g_draws.size() == 1, L("C5", name.c_str(), "reissue"), "the re-issue after the rebuild did not draw");
            if (g_draws.size() == 1) check(drawDifference(g_draws[0], sub).empty(), L("C5", name.c_str(), "reissue-identical"), drawDifference(g_draws[0], sub));
        }
        // and back: the base config is stale now, and the rebuild gives the base strip again
        apply(base);
        check(!panelCurveReissueReady(), L("C5", name.c_str(), "stale-back"), "the strip built for the changed " + name + " is called ready for the base config");
        resetRecorders();
        check(panelCurveSubstitute(ctx, recordingDraw, true) && g_draws.size() == 1, L("C5", name.c_str(), "rebuilt-back"), "the substitution after changing back did not draw");
        if (g_draws.size() == 1) check(g_draws[0].vbBytes == baseDraw.vbBytes && g_draws[0].ibBytes == baseDraw.ibBytes, L("C5", name.c_str(), "base-again"), "the base strip did not come back");
    }

    // Switched off live (curvature 0 at the default columns): not wanted, so not ready, whatever strip is in hand.
    applyConfig("0", 64, 1, "35.556");
    resetRecorders();
    check(!panelCurveWants() && !panelCurveReissueReady() && !panelCurveReissue(ctx, recordingDraw) && g_draws.empty(), "C5.off: switched off live, the re-issue is not ready and draws nothing");
    apply(base);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C6: the table.
// ---------------------------------------------------------------------------------------------------------------------------------
void case6(Gpu& g) {
    std::printf("C6 substitution and re-issue over curvature x columns x sign x gain\n");
    ID3D11DeviceContext* ctx = g.ctx.Get();
    bindGame(g, GameState::Canonical);
    const char* curvatures[] = {"0.1", "0.3", "0.7", "1.0"};
    const int columns[] = {8, 64, 256};
    const int signs[] = {1, -1};
    const char* gains[] = {"1", "35.556", "200"};
    unsigned combos = 0;
    for (const char* c : curvatures)
        for (int n : columns)
            for (int sg : signs)
                for (const char* gn : gains) {
                    g_note = fmt("curvature %s, %d columns, sign %+d, gain %s", c, n, sg, gn);
                    applyConfig(c, n, sg, gn);
                    const Spec s = specOf(c, n, sg, gn);
                    resetRecorders();
                    const Snapshot before = takeSnapshot(ctx);
                    const Refs refs0 = takeRefs(g);
                    const bool sub = panelCurveSubstitute(ctx, recordingDraw, true);
                    const size_t afterSub = g_draws.size();
                    const bool ready = panelCurveReissueReady();
                    const bool re = panelCurveReissue(ctx, recordingDraw);
                    const Refs refs1 = takeRefs(g);
                    check(sub && afterSub == 1 && ready && re && g_draws.size() == 2, "C6.drawn: the substitution and the re-issue each drew once",
                          fmt("sub=%d draws after it=%zu ready=%d reissue=%d draws=%zu", sub, afterSub, ready, re, g_draws.size()));
                    if (g_draws.size() == 2) {
                        verifyStripDraw("C6", "sub", g_draws[0], s, g, ctx, before);
                        verifyStripDraw("C6", "reissue", g_draws[1], s, g, ctx, before);
                        const std::string diff = drawDifference(g_draws[0], g_draws[1]);
                        check(diff.empty(), "C6.identical: the re-issue's state, arguments and bytes are the substitution's", diff);
                    }
                    check(g_motion.size() == 1, "C6.motion-once: only the substitution issues a motion pass", fmt("%zu motion passes", g_motion.size()));
                    if (g_motion.size() == 1 && !g_draws.empty()) verifyMotion("C6", s, ctx, recordingDraw, g_draws[0]);
                    verifyRestored("C6", "both", before, takeSnapshot(ctx), &refs0, &refs1);
                    ++combos;
                }
    g_note.clear();
    check(combos == 4 * 3 * 2 * 3, "C6.count: every combination ran", fmt("%u combinations", combos));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C7: the identity strip.
// ---------------------------------------------------------------------------------------------------------------------------------
void case7(Gpu& g) {
    std::printf("C7 one column at curvature 0 is the game's quad\n");
    ID3D11DeviceContext* ctx = g.ctx.Get();
    applyConfig("0", 1, 1, "35.556");
    bindGame(g, GameState::Canonical);
    resetRecorders();
    check(panelCurveWants() && panelCurveInfo().wanted, "C7.wants: a non-default column count at curvature 0 IS wanted (the identity test has to substitute)");
    const std::vector<uint8_t> gameVb = readBuffer(g.ctx.Get(), g.gameVb.Get());
    const std::vector<uint8_t> gameIb = readBuffer(g.ctx.Get(), g.gameIb16.Get());
    check(gameVb.size() == 160 && gameIb.size() == 24, "C7.fixture: the game's buffers read back");
    const Snapshot before = takeSnapshot(ctx);
    const Refs refs0 = takeRefs(g);
    const bool r = panelCurveSubstitute(ctx, recordingDraw, true);
    const Refs refs1 = takeRefs(g);
    check(r && g_draws.size() == 1, "C7.drawn: the identity strip is drawn once", fmt("returned %d, %zu draws", r, g_draws.size()));
    if (g_draws.size() == 1 && gameVb.size() == 160 && gameIb.size() == 24) {
        const DrawRecord& d = g_draws[0];
        check(d.count == 6 && d.instances == 1 && d.start == 0 && d.base == 0 && d.startInstance == 0, "C7.arguments: drawn with (6, 1, 0, 0, 0), as the game draws its quad");
        check(d.vbBytes.size() == 80 && std::memcmp(d.vbBytes.data(), gameVb.data() + kQuadOffset, 80) == 0, "C7.vertices: the strip's vertex bytes ARE the game's quad, to the byte",
              fmt("%zu bytes", d.vbBytes.size()));
        check(d.ibBytes.size() == 12 && std::memcmp(d.ibBytes.data(), gameIb.data() + kPatternOffset, 12) == 0, "C7.indices: the strip's index buffer IS the game's, to the byte (0,3,1,0,2,3)",
              fmt("%zu bytes", d.ibBytes.size()));
        check(d.snap.vb0 != g.gameVb && d.snap.ib != g.gameIb16 && d.snap.stride0 == 20 && d.snap.ibFmt == DXGI_FORMAT_R16_UINT && d.snap.topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
              "C7.own-buffers: the identity is the strip's own buffers, not the game's bound again");
        check(pipelineDiff(d.snap, before) == 0, "C7.pipeline: the pipeline around it is the game's");
    }
    check(g_motion.size() == 1 && g_motion[0].haveCurve && g_motion[0].curve[0] == 0.0f && g_motion[0].curve[1] == 1.0f, "C7.motion-curve: the motion pass is told zero curvature over one column");
    verifyRestored("C7", "sub", before, takeSnapshot(ctx), &refs0, &refs1);
    // The identity test at the default column count is the shipped state (C1): a column count other than 64 is what asks for it.
    applyConfig("0", 64, 1, "35.556");
    check(!panelCurveWants(), "C7.default-off: back at 64 columns it is not wanted again");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C9: faults.
// ---------------------------------------------------------------------------------------------------------------------------------
void case9(Gpu& g) {
    std::printf("C9 a draw that faults\n");
    ID3D11DeviceContext* ctx = g.ctx.Get();
    applyConfig("0.3", 64, 1, "35.556");
    bindGame(g, GameState::Odd);   // the odd state, so a restore that did not happen is visible
    clearStandDown();
    check(beginLog(L"pcfault"), "C9.log: the scratch log opens");
    resetRecorders();
    check(panelCurveSubstitute(ctx, recordingDraw, true) && panelCurveReissueReady(), "C9.setup: a strip is in hand and ready");
    resetRecorders();

    // A fault in the substitution's draw.
    {
        const Snapshot before = takeSnapshot(ctx);
        const Refs refs0 = takeRefs(g);
        const bool r = panelCurveSubstitute(ctx, faultingDraw, true);
        const Refs refs1 = takeRefs(g);
        check(!r && g_faultDraws == 1, "C9.sub-false: the faulting substitution returns false (after the draw was reached once)", fmt("returned %d, %u calls", r, g_faultDraws));
        const PanelCurveInfo i = panelCurveInfo();
        check(i.standDown && !i.ready && !i.wanted && !panelCurveWants() && !panelCurveReissueReady(), "C9.sub-standdown: the whole feature is stood down (not wanted, not ready)",
              fmt("standDown=%d ready=%d wanted=%d", i.standDown, i.ready, i.wanted));
        verifyRestored("C9", "sub", before, takeSnapshot(ctx), &refs0, &refs1);
        resetRecorders();
        const uint64_t was = panelCurveInfo().reissues;
        check(!panelCurveSubstitute(ctx, recordingDraw, true) && g_draws.empty() && g_motion.empty(), "C9.sub-later: a later substitution returns false and draws nothing");
        check(!panelCurveReissue(ctx, recordingDraw) && g_draws.empty() && panelCurveInfo().reissues == was, "C9.sub-later-reissue: a later re-issue returns false, draws nothing and counts nothing");
    }

    // A fault in the re-issue's draw (stand-down cleared by the rig; the strip in hand is still the current one).
    clearStandDown();
    check(panelCurveReissueReady(), "C9.setup-reissue: the strip in hand is ready again once the rig clears the stand-down");
    {
        bindGame(g, GameState::Odd);
        const Snapshot before = takeSnapshot(ctx);
        const Refs refs0 = takeRefs(g);
        const uint64_t was = panelCurveInfo().reissues;
        g_faultDraws = 0;
        const bool r = panelCurveReissue(ctx, faultingDraw);
        const Refs refs1 = takeRefs(g);
        check(!r && g_faultDraws == 1, "C9.reissue-false: the faulting re-issue returns false", fmt("returned %d, %u calls", r, g_faultDraws));
        const PanelCurveInfo i = panelCurveInfo();
        check(i.standDown && !i.ready && !i.wanted && !panelCurveReissueReady(), "C9.reissue-standdown: a fault in the re-issue stands the whole feature down", fmt("standDown=%d ready=%d wanted=%d", i.standDown, i.ready, i.wanted));
        check(i.reissues == was, "C9.reissue-counter: the counter did not move for a re-issue that faulted", fmt("%llu -> %llu", static_cast<unsigned long long>(was), static_cast<unsigned long long>(i.reissues)));
        verifyRestored("C9", "reissue", before, takeSnapshot(ctx), &refs0, &refs1);
        resetRecorders();
        check(!panelCurveSubstitute(ctx, recordingDraw, true) && !panelCurveReissue(ctx, recordingDraw) && g_draws.empty() && g_motion.empty(), "C9.reissue-later: later calls return false and draw nothing");
    }
    // A fault in the screen's motion pass, which runs with the strip still bound: the same stand-down, and the game's state back.
    // (The module's fault budget is five for the whole process; this case spends three of them.)
    clearStandDown();
    {
        bindGame(g, GameState::Odd);
        const Snapshot before = takeSnapshot(ctx);
        const Refs refs0 = takeRefs(g);
        resetRecorders();
        g_motionFaults = true;
        const bool r = panelCurveSubstitute(ctx, recordingDraw, true);
        g_motionFaults = false;
        const Refs refs1 = takeRefs(g);
        const PanelCurveInfo i = panelCurveInfo();
        check(!r && g_draws.size() == 1 && g_motion.empty() && i.standDown && !i.ready, "C9.motion-false: a fault in the motion pass returns false and stands the feature down (the strip was drawn once before it)",
              fmt("returned %d, %zu draws, standDown=%d ready=%d", r, g_draws.size(), i.standDown, i.ready));
        verifyRestored("C9", "motion", before, takeSnapshot(ctx), &refs0, &refs1);
    }
    clearStandDown();
    const std::string log = endLog(L"pcfault");
    check(count(log, "faulted, so it is off for the rest of this session") == 3 && has(log, "the substitution faulted") && has(log, "re-issue of the strip faulted"), "C9.log: each fault says what faulted and that the feature is off",
          fmt("%zu such lines", count(log, "faulted, so it is off for the rest of this session")));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C10: the numbers the route's lines read, and the shutdown.
// ---------------------------------------------------------------------------------------------------------------------------------
void case10(Gpu& g) {
    std::printf("C10 panelCurveInfo() after each step, and the shutdown\n");
    ID3D11DeviceContext* ctx = g.ctx.Get();
    bindGame(g, GameState::Canonical);
    clearStandDown();
    applyConfig("0.7", 128, -1, "12.5");
    panelCurveShutdown();   // nothing in hand
    resetRecorders();
    PanelCurveInfo i = panelCurveInfo();
    check(i.wanted && !i.standDown && !i.ready && closeTo(i.curvature, 0.7, 1e-6) && i.segments == 128 && i.gain == 0.0f, "C10.configured: configured and not built: wanted, curvature 0.7, 128 columns, no gain, not ready",
          fmt("wanted=%d ready=%d curvature=%g segments=%d gain=%g", i.wanted, i.ready, i.curvature, i.segments, i.gain));
    check(!panelCurveReissueReady(), "C10.not-built: nothing in hand, so the re-issue is not ready");
    const uint64_t r0 = i.reissues;

    check(panelCurveSubstitute(ctx, recordingDraw, true) && g_draws.size() == 1, "C10.sub: the substitution drew");
    i = panelCurveInfo();
    check(i.wanted && i.ready && !i.standDown && closeTo(i.curvature, 0.7, 1e-6) && i.segments == 128 && closeTo(i.gain, 12.5, 1e-4), "C10.built: after the substitution: ready, 128 columns, gain 12.5",
          fmt("wanted=%d ready=%d curvature=%g segments=%d gain=%g", i.wanted, i.ready, i.curvature, i.segments, i.gain));
    check(i.reissues == r0, "C10.no-reissues-yet: a substitution is not a re-issue");
    for (int k = 1; k <= 3; ++k) {
        check(panelCurveReissue(ctx, recordingDraw), "C10.reissue: the re-issue drew");
        check(panelCurveInfo().reissues == r0 + static_cast<uint64_t>(k), "C10.reissues: the re-issue counter is cumulative", fmt("%llu, want %llu", static_cast<unsigned long long>(panelCurveInfo().reissues), static_cast<unsigned long long>(r0 + static_cast<uint64_t>(k))));
    }
    const uint64_t r3 = panelCurveInfo().reissues;

    panelCurveShutdown();
    i = panelCurveInfo();
    check(!i.ready && i.gain == 0.0f, "C10.shutdown: the shutdown clears ready and the gain", fmt("ready=%d gain=%g", i.ready, i.gain));
    check(!panelCurveReissueReady(), "C10.shutdown-strip: ...and the strip in hand");
    resetRecorders();
    check(!panelCurveReissue(ctx, recordingDraw) && g_draws.empty() && panelCurveInfo().reissues == r3, "C10.shutdown-reissue: a re-issue after the shutdown returns false, draws nothing and counts nothing");
    check(panelCurveSubstitute(ctx, recordingDraw, true) && g_draws.size() == 1, "C10.rebuilt: the substitution after a shutdown builds the strip again");
    i = panelCurveInfo();
    check(i.ready && closeTo(i.gain, 12.5, 1e-4), "C10.rebuilt-info: ready with the gain again", fmt("ready=%d gain=%g", i.ready, i.gain));
    if (g_draws.size() == 1) check(stripProblem(g_draws[0].vbBytes, g_draws[0].ibBytes, specOf("0.7", 128, -1, "12.5")).empty(), "C10.rebuilt-strip: ...and it is the strip the config asks for");

    // The z-gain override: with none, or one outside 0..10000, the gain C8 read from the game's buffers is the one the strip is built with.
    const float learned = static_cast<float>(kSizeY * kAspect * kBasisRatio);
    for (const char* v : {"0", "20000", "-5"}) {
        applyConfig("0.3", 64, 1, v);
        resetRecorders();
        const bool r = panelCurveSubstitute(ctx, recordingDraw, true);
        const PanelCurveInfo oi = panelCurveInfo();
        check(r && g_draws.size() == 1 && closeTo(oi.gain, learned, 1e-3), "C10.override-ignored: no override, or one outside 0..10000, builds the strip with the gain read from the game",
              fmt("override %s: returned %d, gain %g, want %g", v, r, oi.gain, learned));
        if (g_draws.size() == 1)
            check(stripProblem(g_draws[0].vbBytes, g_draws[0].ibBytes, Spec{0.3f, 64, 1, learned}).empty(), "C10.override-ignored-strip: ...and the bytes carry that gain", fmt("override %s", v));
    }

    applyConfig("0", 64, 1, "12.5");
    i = panelCurveInfo();
    check(!i.wanted && i.curvature == 0.0f && i.segments == 64, "C10.off: curvature 0 at 64 columns: not wanted, and the numbers say so", fmt("wanted=%d curvature=%g segments=%d", i.wanted, i.curvature, i.segments));
    applyConfig("0", 8, 1, "12.5");
    check(panelCurveInfo().wanted && panelCurveInfo().segments == 8, "C10.identity-wanted: a column count other than 64 at curvature 0 is wanted");

    // The numbers are held to the ranges the config documents: a curvature outside 0..1 is treated as off, and the column count lies in 1..256
    // (the strip is built on the stack for at most 256 columns).
    applyConfig("1.5", 64, 1, "12.5");
    i = panelCurveInfo();
    check(i.curvature == 0.0f && !i.wanted, "C10.clamp-high: a curvature above 1 reads as off", fmt("curvature=%g wanted=%d", i.curvature, i.wanted));
    applyConfig("-0.2", 64, 1, "12.5");
    i = panelCurveInfo();
    check(i.curvature == 0.0f && !i.wanted, "C10.clamp-low: a negative curvature reads as off", fmt("curvature=%g wanted=%d", i.curvature, i.wanted));
    applyConfig("0.3", 1000, 1, "12.5");
    check(panelCurveInfo().segments == 256, "C10.segments-high: more than 256 columns is held to 256", fmt("segments=%d", panelCurveInfo().segments));
    applyConfig("0.3", 0, 1, "12.5");
    check(panelCurveInfo().segments == 1, "C10.segments-low: fewer than 1 column is held to 1", fmt("segments=%d", panelCurveInfo().segments));
    applyConfig("0", 64, 1, "12.5");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C11: nothing in the pipeline but slot 0, the index buffer and the topology.
// ---------------------------------------------------------------------------------------------------------------------------------
void case11(Gpu& g) {
    std::printf("C11 the rest of the game's pipeline\n");
    ID3D11DeviceContext* ctx = g.ctx.Get();
    applyConfig("0.3", 64, 1, "35.556");
    bindGame(g, GameState::Canonical);
    bindPipeline(g);
    resetRecorders();
    const Snapshot before = takeSnapshot(ctx);
    check(before.cb[0] == g.cb0 && before.cb[1] == g.cb1 && before.srv == g.srv && before.sampler == g.sampler && before.rs == g.rs && before.blend == g.blend && before.dss == g.dss && before.rtv == g.rtv &&
              before.vb[0] == g.sizeVb && before.strides[0] == 8,
          "C11.fixture: the sentinels are bound");
    const bool sub = panelCurveSubstitute(ctx, recordingDraw, true);
    const bool ready = panelCurveReissueReady();
    const bool re = panelCurveReissue(ctx, recordingDraw);
    check(sub && ready && re && g_draws.size() == 2, "C11.drawn: a substitution and a re-issue were made", fmt("sub=%d ready=%d reissue=%d draws=%zu", sub, ready, re, g_draws.size()));
    const Snapshot after = takeSnapshot(ctx);
    struct Group { const char* name; unsigned bit; };
    const Group groups[] = {{"vs-constant-buffers", kDiffVs}, {"ps-resource", kDiffSrv}, {"sampler", kDiffSampler}, {"rasterizer", kDiffRs}, {"output-merger", kDiffOm}, {"vertex-slots-1-3", kDiffSlots}};
    for (const Group& gr : groups) {
        for (size_t k = 0; k < g_draws.size(); ++k)
            check((pipelineDiff(g_draws[k].snap, before) & gr.bit) == 0, std::string("C11.at-draw-") + gr.name, fmt("changed at draw %zu (the %s)", k, k == 0 ? "substitution" : "re-issue"));
        check((pipelineDiff(after, before) & gr.bit) == 0, std::string("C11.after-") + gr.name, "changed after the calls");
    }
    check(sameIa(before, after), "C11.ia-restored: and the three things it does change are put back");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C12 and C13: the surface strip (panelCurveSurfaceWanted / Draw / Info), the intro movie's and the splash's.
// ---------------------------------------------------------------------------------------------------------------------------------
constexpr float kSurfaceGain = 4.44444f;   // the panel's half-width in metres: the caller's, the same for the movie and the splash

// The strip a surface asks for, in the generator's own terms: toward +1 (a step in +z' moves toward the viewer) is sign -1 there, and
// reverseU (the caller's placement has +x running to the viewer's left) is u = (1 - x)/2.
Spec surfaceSpec(float curvature, int columns, int toward, float gain, bool reverseU = false) { return Spec{curvature, columns, -toward, gain, reverseU}; }

// Everything but the cull: what the cull-off copy of a game's rasterizer state must keep.
bool sameButCull(const D3D11_RASTERIZER_DESC& a, const D3D11_RASTERIZER_DESC& b) {
    return a.FillMode == b.FillMode && a.FrontCounterClockwise == b.FrontCounterClockwise && a.DepthBias == b.DepthBias && a.DepthBiasClamp == b.DepthBiasClamp &&
           a.SlopeScaledDepthBias == b.SlopeScaledDepthBias && a.DepthClipEnable == b.DepthClipEnable && a.ScissorEnable == b.ScissorEnable &&
           a.MultisampleEnable == b.MultisampleEnable && a.AntialiasedLineEnable == b.AntialiasedLineEnable;
}

// One surface draw: our own buffers bound (not the game's, and not the screen's strip), the arguments, the bytes against the independent bend,
// every other binding the game's, and the rasterizer state the game's with the cull off (or the game's own, when it culls nothing or is unset).
void verifySurfaceDraw(const std::string& tag, const char* who, const DrawRecord& d, const Spec& s, const Gpu& g, ID3D11DeviceContext* ctx, const Snapshot& baseline,
                       ID3D11Buffer* notThisVb) {
    const UINT n = static_cast<UINT>(s.columns);
    check(d.ctx == ctx, L(tag, who, "context"), "the draw was handed another context");
    check(d.count == 6 * n && d.instances == 1 && d.start == 0 && d.base == 0 && d.startInstance == 0, L(tag, who, "arguments"),
          fmt("drawn with (%u, %u, %u, %d, %u), want (%u, 1, 0, 0, 0)", d.count, d.instances, d.start, d.base, d.startInstance, 6 * n));
    check(d.snap.vb0 && d.snap.vb0 != g.gameVb && d.snap.vb0.Get() != notThisVb && d.snap.stride0 == 20 && d.snap.offset0 == 0, L(tag, who, "vertex-buffer"),
          "slot 0 held " + describeIa(d.snap) + " (the surface strip's own buffer is wanted: not the game's, not the screen's)");
    check(d.snap.ib && d.snap.ib != g.gameIb16 && d.snap.ib != g.gameIb32 && d.snap.ibFmt == DXGI_FORMAT_R16_UINT && d.snap.ibOffset == 0, L(tag, who, "index-buffer"),
          "the index buffer was " + describeIa(d.snap));
    check(d.snap.topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, L(tag, who, "topology"), fmt("topology %d at the draw", static_cast<int>(d.snap.topo)));
    check(d.vbBytes.size() == 20u * 2u * (n + 1) && d.ibBytes.size() == 2u * 6u * n, L(tag, who, "sizes"),
          fmt("%zu vertex bytes and %zu index bytes, want %u and %u", d.vbBytes.size(), d.ibBytes.size(), 20u * 2u * (n + 1), 2u * 6u * n));
    const std::string problem = stripProblem(d.vbBytes, d.ibBytes, s);
    check(problem.empty(), L(tag, who, "bytes"), problem);
    check((pipelineDiff(d.snap, baseline) & ~static_cast<unsigned>(kDiffRs)) == 0 && std::memcmp(&d.snap.vp, &baseline.vp, sizeof(baseline.vp)) == 0, L(tag, who, "pipeline"),
          fmt("the pipeline around the draw differs from the game's (bits %u)", pipelineDiff(d.snap, baseline)));
    ID3D11RasterizerState* gameRs = baseline.rs.Get();
    D3D11_RASTERIZER_DESC gd{}, dd{};
    if (gameRs) gameRs->GetDesc(&gd);
    if (gameRs && gd.CullMode != D3D11_CULL_NONE) {
        ID3D11RasterizerState* at = d.snap.rs.Get();
        if (at) at->GetDesc(&dd);
        check(at && at != gameRs && dd.CullMode == D3D11_CULL_NONE && sameButCull(dd, gd), L(tag, who, "cull-off"),
              "the draw's rasterizer state is not the game's with the cull off and every other field the game's");
    } else {
        check(d.snap.rs.Get() == gameRs, L(tag, who, "rasterizer-left-alone"), "a state that culls nothing (or no state at all) was replaced");
    }
}

// The surface strip's own counters, as a change from where they stood (the counters are the session's: a case does not start at zero).
bool surfaceMoved(const PanelCurveSurfaceInfo& from, uint64_t built, uint64_t drawn, uint64_t states) {
    const PanelCurveSurfaceInfo i = panelCurveSurfaceInfo();
    return i.built == from.built + built && i.drawn == from.drawn + drawn && i.rasterStates == from.rasterStates + states;
}

// Two vertex buffers of 20-byte vertices that are the same strip but for u: the same positions, the same v, and u + u' = 1 at every vertex.
bool onlyUDiffers(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.empty() || a.size() != b.size() || a.size() % sizeof(Vtx) != 0) return false;
    std::vector<Vtx> va(a.size() / sizeof(Vtx)), vb(b.size() / sizeof(Vtx));
    std::memcpy(va.data(), a.data(), a.size());
    std::memcpy(vb.data(), b.data(), b.size());
    for (size_t i = 0; i < va.size(); ++i)
        if (va[i].x != vb[i].x || va[i].y != vb[i].y || va[i].z != vb[i].z || va[i].v != vb[i].v || !closeTo(static_cast<double>(va[i].u) + static_cast<double>(vb[i].u), 1.0, 1e-6)) return false;
    return true;
}

// A call to panelCurveSurfaceDraw with everything around it measured: the snapshots, the references, the recorder.
struct SurfaceCall {
    bool returned = false;
    Snapshot before, after;
    Refs refs0, refs1;
};
SurfaceCall surfaceCall(Gpu& g, float gain, int toward, bool reverseU = false, edvr::PanelCurveDrawFn draw = recordingDraw) {
    ID3D11DeviceContext* ctx = g.ctx.Get();
    SurfaceCall c;
    resetRecorders();
    c.before = takeSnapshot(ctx);
    c.refs0 = takeRefs(g);
    c.returned = panelCurveSurfaceDraw(ctx, gain, toward, reverseU, draw);
    c.refs1 = takeRefs(g);   // (the recorders' own references on the game's state are taken out there)
    c.after = takeSnapshot(ctx);
    return c;
}

void case12(Gpu& g) {
    std::printf("C12 the surface strip: nothing at curvature 0; above it the screen's own arc over the caller's gain and direction\n");
    ID3D11DeviceContext* ctx = g.ctx.Get();
    const std::string tag = "C12";
    bindGame(g, GameState::Canonical);
    const PanelCurveSurfaceInfo start = panelCurveSurfaceInfo();
    const PanelCurveInfo screen0 = panelCurveInfo();

    // ---- curvature 0: nothing is wanted, nothing is built, nothing is drawn -- and the identity test's column count does not change that.
    for (int cols : {64, 1}) {
        applyConfig("0", cols, 1, "0");
        g_note = fmt("%d columns at curvature 0", cols);
        const bool screenWants = panelCurveWants();
        const SurfaceCall c = surfaceCall(g, kSurfaceGain, 1);
        check(screenWants == (cols == 1), "C12.off-fixture", "the screen's own wants() is true for the identity test (1 column) and false at the default: the point of the two cases");
        check(!panelCurveSurfaceWanted(), "C12.off-wanted", "the surface is wanted at curvature 0 (it follows the live curvature, not panelCurveWants())");
        check(!c.returned && g_draws.empty() && g_motion.empty(), "C12.off-draw", "the surface strip drew, or said it did, at curvature 0");
        check(surfaceMoved(start, 0, 0, 0) && !panelCurveSurfaceInfo().standDown, "C12.off-nothing-built", "a strip or a state was built, or the surface stood down, at curvature 0");
        verifyRestored(tag, "off", c.before, c.after, &c.refs0, &c.refs1);
    }
    g_note.clear();

    // ---- curvature 0.3: wanted, and independent of everything the screen has to learn (no z gain, no SIZE bound).
    applyConfig("0.3", 64, 1, "0");
    bindSizeSlot(g, false);
    const Spec base = surfaceSpec(0.3f, 64, 1, kSurfaceGain);
    check(panelCurveSurfaceWanted(), "C12.wanted", "the surface is not wanted at curvature 0.3");
    {
        const bool reissueReady0 = panelCurveReissueReady();
        const SurfaceCall c = surfaceCall(g, kSurfaceGain, 1);
        check(c.returned && g_draws.size() == 1, "C12.first-draw", fmt("returned %d, %zu draws: the surface strip needs no SIZE and no gain override", c.returned, g_draws.size()));
        check(g_motion.empty(), "C12.no-motion", "the surface strip issued the screen's motion pass (the movie and the splash have none)");
        if (g_draws.size() == 1) verifySurfaceDraw(tag, "first", g_draws[0], base, g, ctx, c.before, nullptr);
        verifyRestored(tag, "first", c.before, c.after, &c.refs0, &c.refs1);
        check(surfaceMoved(start, 1, 1, 1) && !panelCurveSurfaceInfo().standDown, "C12.first-counters", "one strip, one draw, one rasterizer state");
        check(!panelCurveSurfaceInfo().reversed, "C12.first-direction", "the info says the strip in hand runs u against x, and the caller asked for u with x");
        const PanelCurveInfo si = panelCurveInfo();
        check(si.standDown == screen0.standDown && si.ready == screen0.ready && si.gain == screen0.gain && si.reissues == screen0.reissues && panelCurveReissueReady() == reissueReady0,
              "C12.screen-untouched",
              fmt("the screen's own state moved: standDown=%d ready=%d gain=%g reissues=%llu", si.standDown, si.ready, si.gain, static_cast<unsigned long long>(si.reissues)));
    }

    // ---- which way u runs is the caller's: reverseU true is u = (1 - x)/2 and nothing else of the strip; a flip rebuilds, the same value does not,
    //      the info says which way the strip in hand runs, and flipping back gives the first strip's bytes again.
    {
        const PanelCurveSurfaceInfo i0 = panelCurveSurfaceInfo();
        std::vector<uint8_t> forward;   // the strip with u running with x, as the caller first asked
        {
            const SurfaceCall c = surfaceCall(g, kSurfaceGain, 1, false);
            if (g_draws.size() == 1) forward = g_draws[0].vbBytes;
            check(c.returned && surfaceMoved(i0, 0, 1, 0) && !panelCurveSurfaceInfo().reversed, "C12.forward-kept", "the same key (u with x) rebuilt the strip, or the info reads reversed");
        }
        const SurfaceCall r1 = surfaceCall(g, kSurfaceGain, 1, true);
        check(r1.returned && g_draws.size() == 1, "C12.reverse-drawn", fmt("returned %d, %zu draws", r1.returned, g_draws.size()));
        if (g_draws.size() == 1) verifySurfaceDraw(tag, "reverse", g_draws[0], surfaceSpec(0.3f, 64, 1, kSurfaceGain, true), g, ctx, r1.before, nullptr);
        verifyRestored(tag, "reverse", r1.before, r1.after, &r1.refs0, &r1.refs1);
        check(surfaceMoved(i0, 1, 2, 0) && panelCurveSurfaceInfo().reversed, "C12.reverse-counters", "flipping reverseU did not build exactly one strip, or the info does not say the strip in hand runs u against x");
        if (g_draws.size() == 1)   // only u differs: the same positions and the same v (the index buffer is checked by verifySurfaceDraw above)
            check(onlyUDiffers(forward, g_draws[0].vbBytes), "C12.reverse-only-u", "reversing changed more than u (or u is not 1 - u): the positions and v must be the same strip's");
        const SurfaceCall r2 = surfaceCall(g, kSurfaceGain, 1, true);   // the same value again
        check(r2.returned && surfaceMoved(i0, 1, 3, 0) && panelCurveSurfaceInfo().reversed, "C12.reverse-kept", "an unchanged reverseU rebuilt the strip");
        const SurfaceCall r3 = surfaceCall(g, kSurfaceGain, 1, false);   // and flipped back
        check(r3.returned && surfaceMoved(i0, 2, 4, 0) && !panelCurveSurfaceInfo().reversed, "C12.reverse-back", "flipping reverseU back did not build exactly one strip, or the info still reads reversed");
        if (g_draws.size() == 1) check(g_draws[0].vbBytes == forward, "C12.reverse-back-bytes", "the strip after flipping back is not the first strip's, byte for byte");
    }

    // ---- the table: curvature x columns x direction x gain x which way u runs; each combination changes the key, so each builds exactly one strip.
    {
        const char* curvatures[] = {"0.1", "0.3", "0.7", "1.0"};
        const int columns[] = {8, 64, 256};
        const int directions[] = {1, -1};
        const float gains[] = {1.0f, kSurfaceGain, 100.0f};
        const bool reverses[] = {false, true};
        const PanelCurveSurfaceInfo i0 = panelCurveSurfaceInfo();
        unsigned combos = 0;
        for (const char* cv : curvatures)
            for (int n : columns)
                for (int dir : directions)
                    for (float gn : gains)
                        for (bool rev : reverses) {
                            g_note = fmt("curvature %s, %d columns, toward %+d, gain %g, reverseU %d", cv, n, dir, static_cast<double>(gn), rev);
                            applyConfig(cv, n, 1, "0");
                            const SurfaceCall c = surfaceCall(g, gn, dir, rev);
                            check(c.returned && g_draws.size() == 1, "C12.table-drawn", fmt("returned %d, %zu draws", c.returned, g_draws.size()));
                            if (g_draws.size() == 1) verifySurfaceDraw(tag, "table", g_draws[0], surfaceSpec(std::strtof(cv, nullptr), n, dir, gn, rev), g, ctx, c.before, nullptr);
                            verifyRestored(tag, "table", c.before, c.after, &c.refs0, &c.refs1);
                            check(panelCurveSurfaceInfo().reversed == rev, "C12.table-direction", "the info does not say which way the strip in hand runs u");
                            ++combos;
                        }
        g_note.clear();
        const PanelCurveSurfaceInfo i1 = panelCurveSurfaceInfo();
        check(combos == 4 * 3 * 2 * 3 * 2 && surfaceMoved(i0, combos, combos, 0), "C12.table-counters",
              fmt("%u combinations built %llu strips, drew %llu times and created %llu rasterizer states (the game's one state was derived by the first draw)", combos,
                  static_cast<unsigned long long>(i1.built - i0.built), static_cast<unsigned long long>(i1.drawn - i0.drawn), static_cast<unsigned long long>(i1.rasterStates - i0.rasterStates)));
    }

    // ---- a strip is built when curvature, columns, gain, direction or reverseU changes, and not otherwise.
    {
        applyConfig("0.3", 64, 1, "0");
        SurfaceCall c = surfaceCall(g, kSurfaceGain, 1);   // the base key (rebuilt: the table ended on another)
        const ID3D11Buffer* strip0 = g_draws.size() == 1 ? g_draws[0].snap.vb0.Get() : nullptr;
        const uint64_t b0 = panelCurveSurfaceInfo().built;
        c = surfaceCall(g, kSurfaceGain, 1);
        check(c.returned && g_draws.size() == 1 && g_draws[0].snap.vb0.Get() == strip0 && panelCurveSurfaceInfo().built == b0, "C12.kept", "an unchanged key rebuilt the strip");
        struct Change {
            const char* what;
            const char* curvature;
            int columns;
            float gain;
            int toward;
            bool reverseU;
        };
        const Change changes[] = {{"curvature", "0.5", 64, kSurfaceGain, 1, false}, {"columns", "0.3", 32, kSurfaceGain, 1, false}, {"gain", "0.3", 64, 5.0f, 1, false},
                                  {"direction", "0.3", 64, kSurfaceGain, -1, false},   {"reverse", "0.3", 64, kSurfaceGain, 1, true}};
        uint64_t built = b0;
        for (const Change& ch : changes) {
            const std::string w = std::string("C12.key-") + ch.what;
            applyConfig(ch.curvature, ch.columns, 1, "0");
            c = surfaceCall(g, ch.gain, ch.toward, ch.reverseU);
            ++built;
            check(c.returned && panelCurveSurfaceInfo().built == built, w, fmt("a changed %s did not build exactly one new strip (built %llu, want %llu)", ch.what,
                  static_cast<unsigned long long>(panelCurveSurfaceInfo().built), static_cast<unsigned long long>(built)));
            if (g_draws.size() == 1) verifySurfaceDraw(tag, ch.what, g_draws[0], surfaceSpec(std::strtof(ch.curvature, nullptr), ch.columns, ch.toward, ch.gain, ch.reverseU), g, ctx, c.before, nullptr);
            applyConfig("0.3", 64, 1, "0");
            c = surfaceCall(g, kSurfaceGain, 1);   // and back: the base strip again, one more build
            ++built;
            check(c.returned && panelCurveSurfaceInfo().built == built, w + "-back", "changing back did not build exactly one strip");
        }
    }

    // ---- the rasterizer state: the game's with the cull off, created once per distinct state of the game's, put back, no reference left.
    {
        applyConfig("0.3", 64, 1, "0");
        const uint64_t created0 = panelCurveSurfaceInfo().rasterStates;
        const struct { const char* name; ID3D11RasterizerState* rs; uint64_t newStates; } states[] = {
            {"cull-back", g.rs.Get(), 0}, {"odd", g.rsOdd.Get(), 1}, {"cull-none", g.rsNone.Get(), 0}, {"unbound", nullptr, 0}, {"cull-back-again", g.rs.Get(), 0}, {"odd-again", g.rsOdd.Get(), 0}};
        uint64_t created = created0;
        for (const auto& st : states) {
            ctx->RSSetState(st.rs);
            for (int k = 0; k < 3; ++k) {
                g_note = fmt("the game's state is %s, draw %d", st.name, k);
                const SurfaceCall c = surfaceCall(g, kSurfaceGain, 1);
                check(c.returned && g_draws.size() == 1, "C12.rs-drawn", fmt("returned %d, %zu draws", c.returned, g_draws.size()));
                if (g_draws.size() == 1) verifySurfaceDraw(tag, "rs", g_draws[0], base, g, ctx, c.before, nullptr);
                verifyRestored(tag, "rs", c.before, c.after, &c.refs0, &c.refs1);
                check(c.after.rs.Get() == st.rs, "C12.rs-back", "the game's rasterizer state is not the one bound afterwards");
            }
            created += st.newStates;
            check(panelCurveSurfaceInfo().rasterStates == created, "C12.rs-created-once",
                  fmt("%llu states created after %s, want %llu: one per distinct state, none per draw", static_cast<unsigned long long>(panelCurveSurfaceInfo().rasterStates), st.name,
                      static_cast<unsigned long long>(created)));
        }
        g_note.clear();
        ctx->RSSetState(g.rs.Get());
    }

    // ---- more distinct states of the game's than the cache holds, differing in ONE field (the depth bias) and in pairs sharing a cull mode:
    //      each is derived from its own description (never served another state's copy), each draw is right, the game's state is back after each,
    //      and the module holds no reference on any of the game's states.
    {
        std::vector<ComPtr<ID3D11RasterizerState>> many;
        for (int k = 0; k < 7; ++k) {
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = (k % 4 < 2) ? D3D11_CULL_BACK : D3D11_CULL_FRONT;
            rd.DepthClipEnable = TRUE;
            rd.DepthBias = 100 + k;
            ComPtr<ID3D11RasterizerState> s;
            g.dev->CreateRasterizerState(&rd, &s);
            check(s != nullptr, "C12.many-fixture", "a rasterizer state could not be created");
            many.push_back(s);
        }
        for (int round = 0; round < 2; ++round) {
            for (size_t k = 0; k < many.size(); ++k) {
                if (!many[k]) continue;
                g_note = fmt("distinct state %zu of %zu, round %d", k, many.size(), round);
                ctx->RSSetState(many[k].Get());
                const ULONG held0 = refCount(many[k].Get());
                {
                    const SurfaceCall c = surfaceCall(g, kSurfaceGain, 1);
                    check(c.returned && g_draws.size() == 1, "C12.many-drawn", fmt("returned %d, %zu draws", c.returned, g_draws.size()));
                    if (g_draws.size() == 1) verifySurfaceDraw(tag, "many", g_draws[0], base, g, ctx, c.before, nullptr);
                    verifyRestored(tag, "many", c.before, c.after, &c.refs0, &c.refs1);
                    check(c.after.rs.Get() == many[k].Get(), "C12.many-back", "the game's rasterizer state is not the one bound afterwards");
                }
                resetRecorders();
                check(refCount(many[k].Get()) == held0, "C12.many-refs", fmt("references on the game's state went from %lu to %lu", held0, refCount(many[k].Get())));
            }
        }
        g_note.clear();
        ctx->RSSetState(g.rs.Get());
        // The cache is bounded: fourteen draws over seven distinct states went through it, and what the module still holds is at most its four
        // slots' worth (a state it evicted is given back). The derived state is the object the runtime returns for the same description, so the
        // references beyond the rig's own are the module's.
        ULONG heldByModule = 0;
        for (const ComPtr<ID3D11RasterizerState>& st : many) {
            if (!st) continue;
            D3D11_RASTERIZER_DESC d{};
            st->GetDesc(&d);
            d.CullMode = D3D11_CULL_NONE;
            ComPtr<ID3D11RasterizerState> derived;
            if (SUCCEEDED(g.dev->CreateRasterizerState(&d, &derived)) && derived) heldByModule += refCount(derived.Get()) - 1;
        }
        check(heldByModule <= 4, "C12.many-cache-bounded", fmt("the module holds %lu references on derived states of seven distinct game states; its cache has four slots", heldByModule));
    }

    // ---- what it refuses to do: nothing is drawn or built from a call that cannot be served, and nothing is stood down by it.
    {
        applyConfig("0.3", 64, 1, "0");
        const PanelCurveSurfaceInfo i0 = panelCurveSurfaceInfo();
        resetRecorders();
        const bool refused[] = {panelCurveSurfaceDraw(nullptr, kSurfaceGain, 1, false, recordingDraw), panelCurveSurfaceDraw(ctx, kSurfaceGain, 1, false, nullptr),
                                panelCurveSurfaceDraw(ctx, 0.0f, 1, false, recordingDraw), panelCurveSurfaceDraw(ctx, -4.0f, 1, true, recordingDraw),
                                panelCurveSurfaceDraw(ctx, std::nanf(""), 1, false, recordingDraw), panelCurveSurfaceDraw(ctx, HUGE_VALF, 1, true, recordingDraw),
                                panelCurveSurfaceDraw(ctx, kSurfaceGain, 0, false, recordingDraw), panelCurveSurfaceDraw(ctx, kSurfaceGain, 2, true, recordingDraw),
                                panelCurveSurfaceDraw(ctx, kSurfaceGain, -2, false, recordingDraw)};
        bool any = false;
        for (bool b : refused) any = any || b;
        const PanelCurveSurfaceInfo i1 = panelCurveSurfaceInfo();
        check(!any && g_draws.empty(), "C12.refused", "a null context or draw function, a gain that is not a positive number, or a direction that is not +-1 was served");
        check(i1.built == i0.built && i1.drawn == i0.drawn && !i1.standDown && panelCurveSurfaceWanted(), "C12.refused-quiet", "a refused call built, counted or stood the surface down");
    }

    // ---- live: back at curvature 0 the surface is not wanted and draws nothing (a curvature the config holds to be off -- above 1, below 0 --
    //      is the same off); the shutdown releases the strip and the states.
    {
        for (const char* off : {"0", "1.5", "-0.2"}) {
            applyConfig(off, 64, 1, "0");
            g_note = fmt("curvature %s", off);
            const SurfaceCall c = surfaceCall(g, kSurfaceGain, 1);
            check(!panelCurveSurfaceWanted() && !c.returned && g_draws.empty(), "C12.live-off", "switching the curvature off live did not switch the surface off");
        }
        g_note.clear();
        applyConfig("0.3", 64, 1, "0");
        check(panelCurveSurfaceWanted(), "C12.live-on", "switching the curvature back on live did not switch the surface back on");
        // The derived state the module keeps is the object the runtime hands back for the same description, so its reference count says whether
        // the cache holds it: one reference beyond the rig's own while the strip is live, none once the shutdown has released it.
        const auto cacheRefs = [&]() -> ULONG {
            D3D11_RASTERIZER_DESC d{};
            g.rs->GetDesc(&d);
            d.CullMode = D3D11_CULL_NONE;
            ComPtr<ID3D11RasterizerState> s;
            if (FAILED(g.dev->CreateRasterizerState(&d, &s)) || !s) return 1000;
            return refCount(s.Get()) - 1;   // the rig's own is not the module's
        };
        ctx->RSSetState(g.rs.Get());
        {
            const SurfaceCall w = surfaceCall(g, kSurfaceGain, 1, true);   // the strip in hand runs u against x when the shutdown comes
            const bool drew = w.returned;
            resetRecorders();   // the recorder's snapshot of the draw holds the derived state too, and is the rig's
            check(drew && cacheRefs() == 1, "C12.cache-holds-the-state", fmt("the module holds %lu references on the derived state of the game's, want 1", cacheRefs()));
            check(panelCurveSurfaceInfo().reversed, "C12.live-direction", "the info does not say the strip in hand runs u against x");
        }
        const PanelCurveSurfaceInfo i0 = panelCurveSurfaceInfo();
        panelCurveShutdown();
        check(cacheRefs() == 0, "C12.shutdown-releases-the-states", fmt("the module still holds %lu references on the derived state after the shutdown", cacheRefs()));
        check(!panelCurveSurfaceInfo().reversed, "C12.shutdown-direction", "the info still says the strip in hand runs u against x after the shutdown released it");
        const SurfaceCall d = surfaceCall(g, kSurfaceGain, 1);
        const PanelCurveSurfaceInfo i1 = panelCurveSurfaceInfo();
        check(d.returned && g_draws.size() == 1, "C12.after-shutdown", "the surface did not draw after a shutdown");
        check(i1.built == i0.built + 1 && i1.rasterStates == i0.rasterStates + 1 && i1.drawn == i0.drawn + 1, "C12.shutdown-released",
              "the shutdown did not release the strip and the cull-off states (the next draw should rebuild both)");
        if (g_draws.size() == 1) verifySurfaceDraw(tag, "after-shutdown", g_draws[0], base, g, ctx, d.before, nullptr);
    }
    applyConfig("0", 64, 1, "0");
    bindSizeSlot(g, true);
    bindGame(g, GameState::Canonical);
}

// The screen's strip and the surface strip side by side: neither touches the other, one generator builds both, and a fault of one never stands
// the other down.
void case13(Gpu& g) {
    std::printf("C13 the surface strip beside the screen's: separate state, one generator, independent stand-downs\n");
    ID3D11DeviceContext* ctx = g.ctx.Get();
    const std::string tag = "C13";
    panelCurveShutdown();   // no strip of either kind in hand, whatever the cases before left (the surface's counters and stand-down stay: they are the session's)
    bindPipeline(g);
    bindGame(g, GameState::Canonical);
    clearStandDown();
    applyConfig("0.3", 64, 1, "35.556");
    check(beginLog(L"pcsurf"), "C13.log", "the scratch log opens");
    resetRecorders();
    check(panelCurveSubstitute(ctx, recordingDraw, true) && g_draws.size() == 1, "C13.setup", "the screen's substitution drew its strip");
    if (g_draws.size() != 1) { endLog(L"pcsurf"); return; }
    const DrawRecord screenSub = g_draws[0];
    const PanelCurveInfo screenInfo = panelCurveInfo();
    const PanelCurveSurfaceInfo surf0 = panelCurveSurfaceInfo();

    // ---- the surface draws its own strip and leaves the screen's alone.
    {
        const SurfaceCall c = surfaceCall(g, kSurfaceGain, 1);
        check(c.returned && g_draws.size() == 1, "C13.surface-drawn", fmt("returned %d, %zu draws", c.returned, g_draws.size()));
        if (g_draws.size() == 1) verifySurfaceDraw(tag, "surface", g_draws[0], surfaceSpec(0.3f, 64, 1, kSurfaceGain), g, ctx, c.before, screenSub.snap.vb0.Get());
        verifyRestored(tag, "surface", c.before, c.after, &c.refs0, &c.refs1);
        const PanelCurveInfo i = panelCurveInfo();
        check(i.standDown == screenInfo.standDown && i.ready == screenInfo.ready && i.gain == screenInfo.gain && i.reissues == screenInfo.reissues && i.wanted == screenInfo.wanted &&
                  panelCurveReissueReady(),
              "C13.screen-info-untouched", fmt("the screen's own numbers moved: ready=%d gain=%g reissues=%llu", i.ready, i.gain, static_cast<unsigned long long>(i.reissues)));
        resetRecorders();
        const bool re = panelCurveReissue(ctx, recordingDraw);
        check(re && g_draws.size() == 1 && drawDifference(g_draws[0], screenSub).empty(), "C13.screen-strip-untouched",
              "the screen's re-issue after a surface draw is not its own substitution's draw: " + (g_draws.size() == 1 ? drawDifference(g_draws[0], screenSub) : std::string("no draw")));
        check(panelCurveInfo().reissues == screenInfo.reissues + 1, "C13.screen-counter", "the surface draw moved the screen's re-issue counter (only the screen's own re-issue does)");
        check(surfaceMoved(surf0, 1, 1, 1), "C13.surface-counters", "the surface's counters did not move by exactly one strip, one draw and one rasterizer state");
    }

    // ---- one generator: the screen at sign -1 and the surface at toward +1 build the same strip, byte for byte, at the same gain.
    {
        applyConfig("0.3", 64, -1, "4.44444");
        resetRecorders();
        const bool sub = panelCurveSubstitute(ctx, recordingDraw, true);
        const std::vector<uint8_t> sv = g_draws.size() == 1 ? g_draws[0].vbBytes : std::vector<uint8_t>(), si = g_draws.size() == 1 ? g_draws[0].ibBytes : std::vector<uint8_t>();
        const SurfaceCall c = surfaceCall(g, kSurfaceGain, 1);
        check(sub && c.returned && g_draws.size() == 1 && !sv.empty() && sv == g_draws[0].vbBytes && si == g_draws[0].ibBytes, "C13.same-generator",
              "the screen's strip (sign -1, gain 4.44444) and the surface's (toward +1, gain 4.44444) are not byte for byte the same");
        // and the other way: the screen at sign +1 against the surface at toward -1
        applyConfig("0.3", 64, 1, "4.44444");
        resetRecorders();
        const bool sub2 = panelCurveSubstitute(ctx, recordingDraw, true);
        const std::vector<uint8_t> sv2 = g_draws.size() == 1 ? g_draws[0].vbBytes : std::vector<uint8_t>();
        const SurfaceCall c2 = surfaceCall(g, kSurfaceGain, -1);
        check(sub2 && c2.returned && g_draws.size() == 1 && !sv2.empty() && sv2 == g_draws[0].vbBytes && sv2 != sv, "C13.same-generator-away",
              "the screen's strip (sign +1) and the surface's (toward -1) at the same gain are not byte for byte the same, or the two directions gave the same strip");
        // and reversed: the surface with u running against x is the same strip but for u, and it is the ONE build here that says so in the log; the
        // screen, which never reverses, is rebuilt next with the surface's last strip running u against x -- and must not notice
        const SurfaceCall c3 = surfaceCall(g, kSurfaceGain, -1, true);
        check(c3.returned && g_draws.size() == 1 && onlyUDiffers(sv2, g_draws[0].vbBytes) && panelCurveSurfaceInfo().reversed, "C13.reversed-surface",
              "the surface drawn with u against x is not the same strip as with u along x but for u, or the info does not say it runs against x");
        // the screen as it was, rebuilt: the same strip as its first substitution's, byte for byte
        applyConfig("0.3", 64, 1, "35.556");
        resetRecorders();
        const bool sub3 = panelCurveSubstitute(ctx, recordingDraw, true);
        check(sub3 && g_draws.size() == 1 && g_draws[0].vbBytes == screenSub.vbBytes && g_draws[0].ibBytes == screenSub.ibBytes, "C13.screen-rebuilt",
              "the screen's strip, rebuilt after the surface's was drawn at other settings (u against x among them), is not the one it first drew");
    }

    // ---- a fault of the screen's stands the screen down and not the surface; the surface keeps drawing, with the strip it had.
    //      (The screen's fault budget is five for the whole process and C9 spent three of them: this is the fourth, the last the rig makes.)
    {
        resetRecorders();
        const PanelCurveSurfaceInfo s0 = panelCurveSurfaceInfo();
        const bool screen = panelCurveSubstitute(ctx, faultingDraw, true);
        check(!screen && panelCurveInfo().standDown && g_faultDraws == 1, "C13.screen-fault", "the screen's faulting substitution did not stand the screen down");
        check(panelCurveSurfaceWanted() && !panelCurveSurfaceInfo().standDown, "C13.screen-fault-surface-wanted", "a fault of the screen's stood the surface down");
        applyConfig("0.3", 64, 1, "35.556");   // a configure while the screen is down changes nothing of the surface's
        check(panelCurveSurfaceWanted(), "C13.screen-fault-configure", "a configure with the screen stood down switched the surface off");
        const SurfaceCall c = surfaceCall(g, kSurfaceGain, -1);
        check(c.returned && g_draws.size() == 1, "C13.screen-fault-surface-draws", fmt("the surface did not draw after a fault of the screen's (returned %d, %zu draws)", c.returned, g_draws.size()));
        if (g_draws.size() == 1) verifySurfaceDraw(tag, "after-screen-fault", g_draws[0], surfaceSpec(0.3f, 64, -1, kSurfaceGain), g, ctx, c.before, nullptr);
        verifyRestored(tag, "after-screen-fault", c.before, c.after, &c.refs0, &c.refs1);
        check(panelCurveSurfaceInfo().drawn == s0.drawn + 1 && !panelCurveSurfaceInfo().standDown, "C13.screen-fault-surface-counted", "the surface's own counters did not move by the one draw");
        clearStandDown();   // the rig's: the screen's stand-down is for the session
    }

    // ---- a fault of the surface's stands the surface down and not the screen; the game's state is back (the input assembler, and the
    //      rasterizer state the cull-off copy had replaced); the screen keeps drawing; and only this consumer's own line is logged.
    {
        resetRecorders();
        applyConfig("0.3", 64, 1, "35.556");
        check(panelCurveSubstitute(ctx, recordingDraw, true) && panelCurveReissueReady(), "C13.setup-surface-fault", "the screen's strip is not in hand and ready");
        bindGame(g, GameState::Odd);   // so a restore that did not happen is visible
        ctx->RSSetState(g.rs.Get());   // a state that culls, so the surface swaps its own in before the fault and owes the game's back
        const PanelCurveInfo screen0 = panelCurveInfo();
        const PanelCurveSurfaceInfo s0 = panelCurveSurfaceInfo();
        const SurfaceCall c = surfaceCall(g, kSurfaceGain, 1, false, faultingDraw);
        const PanelCurveSurfaceInfo s1 = panelCurveSurfaceInfo();
        check(!c.returned && g_faultDraws == 1, "C13.surface-fault", fmt("the faulting surface draw returned %d after %u call(s)", c.returned, g_faultDraws));
        check(s1.standDown && !panelCurveSurfaceWanted(), "C13.surface-standdown", "a fault in the surface's draw did not stand the surface down");
        check(s1.drawn == s0.drawn, "C13.surface-fault-uncounted", "a draw that faulted was counted as drawn");
        verifyRestored(tag, "surface-fault", c.before, c.after, &c.refs0, &c.refs1);
        check(c.after.rs.Get() == g.rs.Get(), "C13.surface-fault-rasterizer", "the game's rasterizer state is not the one bound after the fault");
        const PanelCurveInfo screen1 = panelCurveInfo();
        check(!screen1.standDown && screen1.ready == screen0.ready && screen1.gain == screen0.gain && screen1.reissues == screen0.reissues && panelCurveWants() && panelCurveReissueReady(),
              "C13.surface-fault-screen-untouched", "a fault in the surface's draw moved the screen's flags, gain or counters");
        resetRecorders();
        const bool again = panelCurveSurfaceDraw(ctx, kSurfaceGain, 1, false, recordingDraw);
        check(!again && g_draws.empty() && panelCurveSurfaceInfo().drawn == s1.drawn, "C13.surface-later", "a stood-down surface drew again");
        resetRecorders();
        const bool sub = panelCurveSubstitute(ctx, recordingDraw, true);
        const bool re = panelCurveReissue(ctx, recordingDraw);
        check(sub && re && g_draws.size() == 2, "C13.screen-draws-after-surface-fault", "the screen did not draw after a fault of the surface's");
        // the live configuration cannot bring it back: the stand-down is for the session
        applyConfig("0.3", 64, 1, "35.556");
        check(!panelCurveSurfaceWanted(), "C13.surface-stays-down", "reconfiguring brought a stood-down surface back");
        applyConfig("0.5", 32, 1, "35.556");
        resetRecorders();
        check(!panelCurveSurfaceWanted() && !panelCurveSurfaceDraw(ctx, kSurfaceGain, 1, false, recordingDraw) && g_draws.empty(), "C13.surface-stays-down-changed", "a changed configuration brought a stood-down surface back");
        // ... and neither does the shutdown: the stand-down is the session's
        panelCurveShutdown();
        check(panelCurveSurfaceInfo().standDown && !panelCurveSurfaceWanted(), "C13.surface-shutdown-keeps-standdown", "the shutdown cleared the surface's stand-down");
        bindGame(g, GameState::Canonical);
    }

    // ---- the log: the screen's fault and the surface's are each said once, in their own words.
    {
        const std::string log = endLog(L"pcsurf");
        check(count(log, "the substitution faulted, so it is off for the rest of this session") == 1, "C13.log-screen", fmt("%zu lines for the screen's fault", count(log, "the substitution faulted, so it is off for the rest of this session")));
        check(count(log, "the surface strip (the intro movie and the splash) faulted, so those two are flat for the rest of this session") == 1, "C13.log-surface",
              fmt("%zu lines for the surface's fault", count(log, "the surface strip (the intro movie and the splash) faulted, so those two are flat for the rest of this session")));
        const uint64_t builds = panelCurveSurfaceInfo().built - surf0.built;
        check(builds >= 3 && count(log, "-column SURFACE strip") == builds, "C13.log-built", fmt("%zu surface-strip lines for %llu builds", count(log, "-column SURFACE strip"), static_cast<unsigned long long>(builds)));
        // which way u runs is in each build line: against x for the one reversed build, with x for every other
        check(count(log, "u running against x: the placement's +x runs to the viewer's left") == 1 && count(log, "u running with x") == builds - 1, "C13.log-direction",
              fmt("%zu lines say u runs against x and %zu with x, for %llu builds of which one was reversed", count(log, "u running against x"), count(log, "u running with x"), static_cast<unsigned long long>(builds)));
    }
    applyConfig("0", 64, 1, "35.556");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The runner.
// ---------------------------------------------------------------------------------------------------------------------------------
struct Case {
    const char* id;
    void (*run)(Gpu&);
};
// Order matters twice: C8 runs right after C1 and C12 (neither reads a SIZE) because the module never forgets a SIZE it has read, and the
// cases after it run on the z-gain override; C13 runs last because it stands the surface strip down for the session (the screen's own
// stand-down the rig can clear, the surface's it cannot: that is what is pinned).
const Case kCases[] = {{"C1", case1},  {"C12", case12}, {"C8", case8},   {"C2", case2},   {"C3", case3},   {"C4", case4}, {"C5", case5},
                       {"C6", case6},  {"C7", case7},   {"C9", case9},   {"C10", case10}, {"C11", case11}, {"C13", case13}};

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

int run(bool keep) {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    g_dir = std::wstring(temp) + L"edvr_pctest_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(g_dir.c_str(), nullptr);
    g_runtimeProfile = RuntimeProfile::Vr;
    g_clockForTest = fakeClock;

    Gpu gpu = makeGpu();
    check(gpu.ok, "C0.gpu: a WARP device and the game's state are available");
    if (!gpu.ok) {
        std::printf("FAIL: panel curve: no WARP device\n");
        g_clockForTest = nullptr;
        if (!keep) removeScratch();
        return 1;
    }
    check(reportSystemD3D11Only("panel_curve_test"), "C0.system-d3d11: the process runs on System32's d3d11.dll only (no EDVR proxy beside the rig)");
    bindPipeline(gpu);

    unsigned ran = 0;
    for (const Case& c : kCases) {
        c.run(gpu);
        ++ran;
    }
    check(ran == sizeof(kCases) / sizeof(kCases[0]) && ran == 13, "C0.cases: all thirteen cases ran", fmt("%u cases", ran));

    panelCurveShutdown();
    resetRecorders();
    g_refSub = DrawRecord{};
    g_clockForTest = nullptr;
    gpu = Gpu{};
    if (Log::get().isOpen()) Log::get().close();
    if (keep) std::printf("the scratch directory is kept: %s\n", narrow(g_dir).c_str());   // --keep: the logs the checks read, for a look
    else removeScratch();
    return g_failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool self = false, dry = false, keep = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--self-test")) self = true;
        else if (!std::strcmp(argv[i], "--dry-run")) dry = true;
        else if (!std::strcmp(argv[i], "--keep")) keep = true;
        else if (argv[i][0] == '-') {
            std::fputs("usage: panel_curve_test --self-test [<repo root, ignored>] [--keep] | --dry-run\n", stderr);
            return 2;
        }
    }
    if (dry) {
        std::puts("panel_curve_test: --dry-run: nothing run, nothing written");
        return 0;
    }
    if (!self) {
        std::fputs("usage: panel_curve_test --self-test [<repo root, ignored>] [--keep] | --dry-run\n", stderr);
        return 2;
    }
    const int rc = run(keep);
    if (rc == 0) {
        std::printf("PASS: %u panel curve checks (%zu cases: the substitution, its re-issue, the surface strip and the faults, on WARP)\n", g_checks, sizeof(kCases) / sizeof(kCases[0]));
    } else {
        std::printf("FAIL: panel curve: %u of %u checks failed, %zu distinct\n", g_failed, g_checks, g_failCount.size());
    }
    return rc;
}

#include "engine_velocity.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "depth_probe.h"
#include "dxbc_engine_velocity.h"
#include "engine_velocity_emit.h"
#include "kinematic_eval_hook.h"
#include "kinematic_eval_probe.h"
#include "kinematic_motion.h"
#include "vscreen.h"
#include "../common/log.h"
#include "../common/timing.h"

namespace edvr {
namespace engine_velocity_detail {

template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
namespace emit = engine_velocity_emit;

std::atomic<bool> live{false};
DrawCache cache;
uint64_t familyDraws[kMaxFamilies] = {};

// --- The keyed pool families -------------------------------------------------
// Vertex-shader hash -> the pixel shaders measured with it (blur-on run 043720
// and the 09-06 dump; docs/kinematic-motion-injection-2026-09-19.md). A hash
// that is not here is not substituted: after a game update the family stands
// down by name, the way every keyed fix in EDVR does.
struct Family {
    uint64_t vs;
    const char* name;
    uint64_t ps[3];
};
constexpr Family kFamilies[] = {
    {0xEB5234DB6ADB491Dull, "vs_EB5234DB6ADB491D", {0xCB9F297EFF264251ull, 0x9ABF60B4B51F2C1Full, 0x3434972DB5336AA4ull}},
    {0x5B4D8E894EEDA8B4ull, "vs_5B4D8E894EEDA8B4", {0x4375B72964F386CDull, 0, 0}},
    {0xBBE58E40FE88EC80ull, "vs_BBE58E40FE88EC80", {0xDB3E8D20CF53FBC0ull, 0, 0}},
    {0xDE545DC8EE4FBB87ull, "vs_DE545DC8EE4FBB87", {0xE46E3E4832B2FDB0ull, 0, 0}},
    {0xAACFDCF2FB9AD809ull, "vs_AACFDCF2FB9AD809", {0xCF534B32F491561Aull, 0, 0}},
    {0x66DE2CADB1F4AE6Bull, "vs_66DE2CADB1F4AE6B", {0x864F1F949851B8DEull, 0, 0}},
    {0x61AE8EB05FDC18DDull, "vs_61AE8EB05FDC18DD", {0xFC43E42710010343ull, 0, 0}},
};
constexpr int kFamilyCount = static_cast<int>(sizeof(kFamilies) / sizeof(kFamilies[0]));
static_assert(kFamilyCount <= kMaxFamilies, "familyDraws holds every family");

int familyOfVs(uint64_t hash) {
    for (int i = 0; i < kFamilyCount; ++i) if (kFamilies[i].vs == hash) return i;
    return -1;
}
bool keyedPs(int family, uint64_t hash) {
    if (family < 0 || !hash) return false;
    for (uint64_t h : kFamilies[family].ps) if (h == hash) return true;
    return false;
}
bool anyKeyedPs(uint64_t hash) {
    for (int i = 0; i < kFamilyCount; ++i) if (keyedPs(i, hash)) return true;
    return false;
}

std::recursive_mutex g_mutex;   // shader memory, patches, eyes, the draw path's slow half

struct VsInfo {
    Ptr<ID3D11VertexShader> object;   // held: no address reuse while remembered
    int family = -1;
    std::vector<BYTE> bytes;
    bool linked = false;
};
struct PsInfo {
    Ptr<ID3D11PixelShader> object;
    uint64_t hash = 0;
    std::vector<BYTE> bytes;
    bool linked = false;
};
std::unordered_map<ID3D11VertexShader*, VsInfo> g_vs;
std::unordered_map<ID3D11PixelShader*, PsInfo> g_ps;
constexpr size_t kRememberCap = 512;   // keyed shader objects kept (a few KB of bytecode each)

struct FamilyState {
    bool derived = false, valid = false;
    EngineVelocityInputs inputs{};
    std::string reason;                            // why it stood down (empty = live)
    std::unordered_map<ID3D11VertexShader*, Ptr<ID3D11VertexShader>> patchedVs;
    std::unordered_map<ID3D11PixelShader*, Ptr<ID3D11PixelShader>> patchedPs;
    std::unordered_map<ID3D11PixelShader*, std::string> psFailed;
    uint64_t binds = 0;                            // substitutions made (this window)
    uint64_t unkeyedPsDraws = 0;                   // bind events with a pixel shader outside the keyed set
    uint64_t unkeyedPsHash = 0;
};
FamilyState g_families[kFamilyCount];

// What EDVR bound in place of the game's shaders, and at which generation of
// the game's own binding, so a later look can tell whether it is still bound.
struct Bound {
    ID3D11PixelShader* originalPs = nullptr;
    ID3D11PixelShader* patchedPs = nullptr;
    uint32_t psGen = 0;
    ID3D11VertexShader* originalVs = nullptr;
    ID3D11VertexShader* patchedVs = nullptr;
    uint32_t vsGen = 0;
    int family = -1;
};
Bound g_bound;
std::atomic<bool> g_anyBound{false};   // an EDVR shader may still be bound (owner thread restores)

// --- Per eye -------------------------------------------------------------------
struct Eye {
    Ptr<ID3D11Texture2D> depth;          // the scene depth this eye's slot target matches
    Ptr<ID3D11Texture2D> slots;
    Ptr<ID3D11RenderTargetView> slotsRtv;
    Ptr<ID3D11ShaderResourceView> slotsSrv;
    unsigned width = 0, height = 0;
    Ptr<ID3D11Buffer> pool;
    Ptr<ID3D11ShaderResourceView> poolSrv;
    UINT poolBytes = 0;
    Ptr<ID3D11Buffer> scene[2];          // the game's cb1, by present-frame parity
    uint32_t sceneFrame[2] = {~0u, ~0u};
    UINT sceneBytes = 0;
    uint32_t frame = ~0u;                // the present frame this eye's data belongs to
    uint32_t rtvGen = 0, dsvGen = 0;     // the pass binding MRT6 was added to
    const ID3D11Resource* poolSource = nullptr;   // identities only, never dereferenced
    const ID3D11Resource* sceneSource = nullptr;
    bool bound = false, written = false, invalid = false, consumed = false, passOpen = false;
};
Eye g_eyes[2];
std::atomic<const ID3D11Resource*> g_watch[4] = {};   // eye0 pool, eye0 scene, eye1 pool, eye1 scene

std::atomic<uint32_t> g_frame{0};

// --- The emit side (job threads) ---------------------------------------------
std::unique_ptr<emit::Table> g_table;
emit::Stats g_emit;
std::atomic<emit::LookupFn> g_lookup{nullptr};
std::atomic<uint64_t> g_emitSampled{0}, g_emitSampledTicks{0};
const char* g_emitStatus = "not attached";

// --- Draw-side and pixel counters (owner thread) -------------------------------
struct DrawStats {
    uint64_t slowPaths = 0, slowTicks = 0, quickPaths = 0;
    uint64_t eyeFrames = 0, eyeFramesBound = 0, invalidPool = 0, invalidScene = 0, sourcesChanged = 0;
    uint64_t targetOccupied = 0, uavBound = 0, noPool = 0, noScene = 0, createFailed = 0, depthUnsupported = 0;
    uint64_t viewsAsked = 0, viewsGiven = 0;
    // Why a view request was refused, first failing test: another depth
    // texture, another frame's data (a clock-order problem shows here), the
    // eye-frame invalidated, MRT6 never bound, last frame's scene constants
    // missing (the first frame, or a gap).
    uint64_t refusedDepth = 0, refusedFrame = 0, refusedInvalid = 0, refusedUnwritten = 0, refusedPrevious = 0;
    uint64_t restores = 0;
    uint64_t frames = 0;
    uint64_t pixelsJoined = 0, pixelsMasked = 0, pixelsCamera = 0, pixelsStale = 0, pixelReads = 0;
    uint64_t trackerMovers = 0, trackerFrames = 0;
    void clear() { *this = DrawStats{}; }
};
DrawStats g_draw;
uint64_t g_windowStartMs = 0;
constexpr uint64_t kSummaryMs = 30000;

uint32_t frameNow() { return g_frame.load(std::memory_order_acquire); }

// --- The build-keyed engine side ----------------------------------------------
// FUN_143696FA0's first 32 bytes (the dictionary lookup the bracket calls) and
// FUN_144312E00's append sequence at 0x144313185 (the node count at +0x18,
// the mask store at +0xAA0+i*8, the owner count at +0x2A4): the layout the
// bracket's reads and writes rest on, in the hash-verified exe (332841).
constexpr uintptr_t kLookupRva = 0x3696FA0u;
constexpr uint8_t kLookupPrologue[32] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83,
    0xEC, 0x40, 0x8B, 0x42, 0x18, 0x48, 0x8B, 0xDA, 0x33, 0xD2, 0x4C, 0x8B, 0xF1, 0x48, 0xF7, 0x71};
constexpr uintptr_t kAppendRva = 0x4313185u;
constexpr uint8_t kAppendBytes[23] = {
    0x48, 0x8B, 0x42, 0x18,                                // mov rax,[rdx+18h]      the node's count
    0x4C, 0x89, 0xBC, 0xC2, 0xA0, 0x0A, 0x00, 0x00,        // mov [rdx+rax*8+0AA0h],r15  the record's mask
    0x48, 0xFF, 0x42, 0x18,                                // inc qword [rdx+18h]
    0x41, 0xFF, 0x85, 0xA4, 0x02, 0x00, 0x00};             // inc dword [r13+2A4h]   the owner's count

const char* verifyEngine(uintptr_t base) noexcept {
    __try {
        uint32_t peOff = 0;
        std::memcpy(&peOff, reinterpret_cast<const void*>(base + 0x3C), 4);
        if (peOff > 0x1000) return "not build 332841 (no PE header)";
        uint32_t timestamp = 0, imageSize = 0;
        std::memcpy(&timestamp, reinterpret_cast<const void*>(base + peOff + 8), 4);
        std::memcpy(&imageSize, reinterpret_cast<const void*>(base + peOff + 0x50), 4);
        if (timestamp != KinematicEvalProbe::kExpectedTimestamp || imageSize != KinematicEvalProbe::kExpectedImageSize)
            return "not build 332841 (PE timestamp/size)";
        if (std::memcmp(reinterpret_cast<const void*>(base + kLookupRva), kLookupPrologue, sizeof(kLookupPrologue)))
            return "lookup mismatch at RVA 0x3696FA0";
        if (std::memcmp(reinterpret_cast<const void*>(base + kAppendRva), kAppendBytes, sizeof(kAppendBytes)))
            return "append sequence mismatch at RVA 0x4313185";
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "engine image unreadable";
    }
}

void observeEmit(uintptr_t record, uintptr_t owner, int32_t before, int32_t after) noexcept {
    if (!live.load(std::memory_order_acquire) || !g_table) return;
    const uint64_t n = g_emit.calls.load(std::memory_order_relaxed);
    const bool sample = (n & 63u) == 0;
    const int64_t t0 = sample ? qpcNow() : 0;
    emit::observe(record, owner, before, after, frameNow(), g_lookup.load(std::memory_order_acquire), *g_table, g_emit);
    if (sample) {
        g_emitSampled.fetch_add(1, std::memory_order_relaxed);
        g_emitSampledTicks.fetch_add(static_cast<uint64_t>(qpcNow() - t0), std::memory_order_relaxed);
    }
}

// --- Patching -------------------------------------------------------------------
thread_local bool t_creating = false;

void deriveFamily(int f) {
    FamilyState& s = g_families[f];
    if (s.derived) return;
    for (auto& [ptr, info] : g_vs) {
        if (info.family != f) continue;
        s.derived = true;
        if (info.linked) { s.reason = "vertex shader uses class linkage"; return; }
        std::string why;
        s.valid = engineVelocityDeriveInputs(info.bytes.data(), info.bytes.size(), s.inputs, why);
        if (!s.valid) s.reason = "signature: " + why;
        return;
    }
}

ID3D11VertexShader* patchedVsFor(ID3D11DeviceContext* ctx, int f, ID3D11VertexShader* vs) {
    FamilyState& s = g_families[f];
    auto found = s.patchedVs.find(vs);
    if (found != s.patchedVs.end()) return found->second.Get();
    auto info = g_vs.find(vs);
    Ptr<ID3D11VertexShader> patched;
    if (info != g_vs.end() && !info->second.linked) {
        std::vector<BYTE> out;
        std::string why;
        if (engineVelocityPatchVs(info->second.bytes.data(), info->second.bytes.size(), s.inputs, out, why)) {
            Ptr<ID3D11Device> dev;
            ctx->GetDevice(&dev);
            t_creating = true;
            const HRESULT hr = dev->CreateVertexShader(out.data(), out.size(), nullptr, &patched);
            t_creating = false;
            if (FAILED(hr)) { char t[64]; _snprintf_s(t, _TRUNCATE, "CreateVertexShader 0x%08X", unsigned(hr)); s.reason = t; patched.Reset(); }
        } else {
            s.reason = "vertex patch: " + why;
        }
    }
    s.patchedVs.emplace(vs, patched);
    return patched.Get();
}

ID3D11PixelShader* patchedPsFor(ID3D11DeviceContext* ctx, int f, ID3D11PixelShader* ps) {
    FamilyState& s = g_families[f];
    auto found = s.patchedPs.find(ps);
    if (found != s.patchedPs.end()) return found->second.Get();
    auto info = g_ps.find(ps);
    Ptr<ID3D11PixelShader> patched;
    std::string why = info == g_ps.end() ? "pixel shader bytecode not kept" : info->second.linked ? "class linkage" : "";
    if (why.empty()) {
        std::vector<BYTE> out;
        if (engineVelocityPatchPs(info->second.bytes.data(), info->second.bytes.size(), s.inputs, out, why)) {
            Ptr<ID3D11Device> dev;
            ctx->GetDevice(&dev);
            t_creating = true;
            const HRESULT hr = dev->CreatePixelShader(out.data(), out.size(), nullptr, &patched);
            t_creating = false;
            if (FAILED(hr)) { char t[64]; _snprintf_s(t, _TRUNCATE, "CreatePixelShader 0x%08X", unsigned(hr)); why = t; patched.Reset(); }
        }
    }
    if (!patched) s.psFailed[ps] = why;
    s.patchedPs.emplace(ps, patched);
    return patched.Get();
}

// Put the game's own shaders back where EDVR's are still bound (the game
// has not rebound since: the generation says so).
void restore(ID3D11DeviceContext* ctx) {
    if (g_bound.patchedPs && bindingGeneration(BindSlot::Ps) == g_bound.psGen) {
        vScreenPSSetShaderRaw(ctx, g_bound.originalPs, nullptr, 0);
        ++g_draw.restores;
    }
    if (g_bound.patchedVs && bindingGeneration(BindSlot::Vs) == g_bound.vsGen) {
        vScreenVSSetShaderRaw(ctx, g_bound.originalVs, nullptr, 0);
        ++g_draw.restores;
    }
    g_bound = Bound{};
    g_anyBound.store(false, std::memory_order_release);
    cache.family = -1;
}

// --- The eye pass --------------------------------------------------------------
bool ensureSlots(ID3D11DeviceContext* ctx, Eye& e, ID3D11Texture2D* depth) {
    D3D11_TEXTURE2D_DESC dd{};
    depth->GetDesc(&dd);
    // A single-sample, single-slice scene depth only: MRT6 must match the
    // pass's depth target exactly or the runtime drops the game's draw.
    if (dd.SampleDesc.Count != 1 || dd.ArraySize != 1) { ++g_draw.depthUnsupported; return false; }
    if (e.depth.Get() == depth && e.slots && e.width == dd.Width && e.height == dd.Height) return true;
    e.slots.Reset(); e.slotsRtv.Reset(); e.slotsSrv.Reset();
    e.depth = depth; e.width = dd.Width; e.height = dd.Height;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = dd.Width; d.Height = dd.Height; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_R32G32_FLOAT;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &e.slots)) ||
        FAILED(dev->CreateRenderTargetView(e.slots.Get(), nullptr, &e.slotsRtv)) ||
        FAILED(dev->CreateShaderResourceView(e.slots.Get(), nullptr, &e.slotsSrv))) {
        e.slots.Reset(); e.slotsRtv.Reset(); e.slotsSrv.Reset(); e.depth.Reset();
        ++g_draw.createFailed;
        return false;
    }
    return true;
}

// This eye-frame's pool and scene constants, exactly as its draws read them:
// VS t33 and b1 of the first substituted draw, copied on the GPU.
bool snapshot(ID3D11DeviceContext* ctx, Eye& e, int eye, uint32_t frame) {
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    Ptr<ID3D11ShaderResourceView> poolView;
    ctx->VSGetShaderResources(33, 1, &poolView);
    Ptr<ID3D11Resource> poolRes;
    Ptr<ID3D11Buffer> poolBuf;
    if (!poolView) { ++g_draw.noPool; return false; }
    // The slot the vertex shader indexes is relative to the view's first
    // element; the snapshot and the compose's view start at 0, so must this.
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    poolView->GetDesc(&vd);
    if (vd.ViewDimension != D3D11_SRV_DIMENSION_BUFFER || vd.Buffer.FirstElement != 0) { ++g_draw.noPool; return false; }
    poolView->GetResource(&poolRes);
    if (!poolRes || FAILED(poolRes.As(&poolBuf))) { ++g_draw.noPool; return false; }
    D3D11_BUFFER_DESC pd{};
    poolBuf->GetDesc(&pd);
    if (pd.StructureByteStride != emit::kItemBytes || !(pd.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) ||
        pd.ByteWidth < emit::kItemBytes) { ++g_draw.noPool; return false; }
    if (!e.pool || e.poolBytes != pd.ByteWidth) {
        e.pool.Reset(); e.poolSrv.Reset();
        D3D11_BUFFER_DESC d = pd;
        d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateBuffer(&d, nullptr, &e.pool)) || FAILED(dev->CreateShaderResourceView(e.pool.Get(), nullptr, &e.poolSrv))) {
            e.pool.Reset(); e.poolSrv.Reset(); e.poolBytes = 0; ++g_draw.createFailed;
            return false;
        }
        e.poolBytes = pd.ByteWidth;
    }
    Ptr<ID3D11Buffer> scene;
    ctx->VSGetConstantBuffers(1, 1, &scene);
    if (!scene) { ++g_draw.noScene; return false; }
    D3D11_BUFFER_DESC sd{};
    scene->GetDesc(&sd);
    if (sd.ByteWidth < 276u * 16u || sd.ByteWidth > 65536u) { ++g_draw.noScene; return false; }
    const unsigned slot = frame & 1u;
    if (!e.scene[slot] || e.sceneBytes != sd.ByteWidth) {
        for (auto& b : e.scene) b.Reset();
        e.sceneFrame[0] = e.sceneFrame[1] = ~0u;
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = sd.ByteWidth; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        for (auto& b : e.scene) if (FAILED(dev->CreateBuffer(&d, nullptr, &b))) { for (auto& c : e.scene) c.Reset(); e.sceneBytes = 0; ++g_draw.createFailed; return false; }
        e.sceneBytes = sd.ByteWidth;
    }
    ctx->CopyResource(e.pool.Get(), poolBuf.Get());
    ctx->CopyResource(e.scene[slot].Get(), scene.Get());
    e.sceneFrame[slot] = frame;
    e.poolSource = poolBuf.Get();
    e.sceneSource = scene.Get();
    g_watch[eye * 2].store(e.poolSource, std::memory_order_release);
    g_watch[eye * 2 + 1].store(e.sceneSource, std::memory_order_release);
    return true;
}

// Add MRT6 to the game's binding, once per pass binding. False: not here.
bool bindTarget(ID3D11DeviceContext* ctx, Eye& e, ID3D11DepthStencilView* dsv) {
    ID3D11RenderTargetView* rt[8] = {};
    ID3D11DepthStencilView* bound = nullptr;
    ctx->OMGetRenderTargets(8, rt, &bound);
    std::array<Ptr<ID3D11RenderTargetView>, 8> held;
    for (unsigned i = 0; i < 8; ++i) held[i].Attach(rt[i]);
    Ptr<ID3D11DepthStencilView> heldDsv;
    heldDsv.Attach(bound);
    if (bound != dsv) return false;
    if (rt[kEngineVelocityTarget] && rt[kEngineVelocityTarget] != e.slotsRtv.Get()) { ++g_draw.targetOccupied; return false; }
    ID3D11UnorderedAccessView* uav[8] = {};
    ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 8, uav);
    bool anyUav = false;
    for (auto* u : uav) if (u) { anyUav = true; u->Release(); }
    if (anyUav) { ++g_draw.uavBound; return false; }
    if (rt[kEngineVelocityTarget] == e.slotsRtv.Get()) return true;
    rt[kEngineVelocityTarget] = e.slotsRtv.Get();
    // All eight slots: whatever the game has at 7 stays bound.
    vScreenSetRenderTargetsRaw(ctx, 8, rt, dsv);
    return true;
}

void slowPath(ID3D11DeviceContext* ctx, bool rtv0Eye) {
    ++g_draw.slowPaths;
    auto* vs = static_cast<ID3D11VertexShader*>(bindingGet(BindSlot::Vs));
    auto* ps = static_cast<ID3D11PixelShader*>(bindingGet(BindSlot::Ps));
    auto* dsv = static_cast<ID3D11DepthStencilView*>(bindingGet(BindSlot::Dsv0));
    const uint64_t vsHash = bindingShaderHash(BindSlot::Vs);
    const uint64_t psHash = bindingShaderHash(BindSlot::Ps);
    int eye = -1, target = -1;
    const bool eyePass = rtv0Eye && dsv && depthProbeCurrentSceneEyeOf(dsv, &eye, &target) && (eye == 0 || eye == 1);
    // One eye's pass opening closes the other's: its draws are done, so a
    // later write to its sources cannot reach them. A pass stays open across
    // other targets' draws (they may interleave) until then, the temporal
    // pass's read, or the frame boundary.
    if (eyePass) g_eyes[1 - eye].passOpen = false;
    const int f = familyOfVs(vsHash);
    auto vsInfo = vs ? g_vs.find(vs) : g_vs.end();
    if (!eyePass || f < 0 || vsInfo == g_vs.end() || vsInfo->second.family != f) { restore(ctx); return; }
    deriveFamily(f);
    FamilyState& fam = g_families[f];
    if (!fam.valid) { restore(ctx); return; }
    Eye& e = g_eyes[eye];
    const uint32_t frame = frameNow();
    // The pass: this eye-frame's slot target, snapshot and MRT6.
    if (e.rtvGen != cache.rtv || e.dsvGen != cache.dsv || e.frame != frame) {
        Ptr<ID3D11Resource> depthRes;
        dsv->GetResource(&depthRes);
        Ptr<ID3D11Texture2D> depthTex;
        if (!depthRes || FAILED(depthRes.As(&depthTex)) || !ensureSlots(ctx, e, depthTex.Get())) { restore(ctx); return; }
        if (e.frame != frame) {
            ++g_draw.eyeFrames;
            e.frame = frame;
            e.bound = e.written = e.invalid = e.consumed = false;
            const float cleared[4] = {-1.0f, 0.0f, 0.0f, 0.0f};
            ctx->ClearRenderTargetView(e.slotsRtv.Get(), cleared);
            if (!snapshot(ctx, e, eye, frame)) e.invalid = true;
        } else {
            // A later pass of the same eye-frame must draw from the same pool
            // and scene constants the snapshot holds.
            Ptr<ID3D11ShaderResourceView> poolView;
            ctx->VSGetShaderResources(33, 1, &poolView);
            Ptr<ID3D11Resource> poolRes;
            if (poolView) poolView->GetResource(&poolRes);
            Ptr<ID3D11Buffer> scene;
            ctx->VSGetConstantBuffers(1, 1, &scene);
            if (poolRes.Get() != e.poolSource || scene.Get() != e.sceneSource) {
                if (!e.invalid) ++g_draw.sourcesChanged;
                e.invalid = true;
            }
        }
        e.rtvGen = cache.rtv;
        e.dsvGen = cache.dsv;
        e.bound = bindTarget(ctx, e, dsv);
        if (e.bound && !e.written) ++g_draw.eyeFramesBound;
        e.written = e.written || e.bound;
        e.passOpen = true;
    }
    if (!e.bound || e.invalid) { restore(ctx); return; }
    // The substitution.
    if (!keyedPs(f, psHash)) {
        if (!anyKeyedPs(psHash)) { ++fam.unkeyedPsDraws; fam.unkeyedPsHash = psHash; }
        restore(ctx);
        return;
    }
    ID3D11VertexShader* useVs = vs;
    if (fam.inputs.slotFromVsPatch) {
        useVs = patchedVsFor(ctx, f, vs);
        if (!useVs) { restore(ctx); return; }
    }
    ID3D11PixelShader* usePs = patchedPsFor(ctx, f, ps);
    if (!usePs) { restore(ctx); return; }
    if (useVs != vs) vScreenVSSetShaderRaw(ctx, useVs, nullptr, 0);
    vScreenPSSetShaderRaw(ctx, usePs, nullptr, 0);
    g_bound.originalPs = ps; g_bound.patchedPs = usePs; g_bound.psGen = cache.ps;
    g_bound.originalVs = useVs != vs ? vs : nullptr; g_bound.patchedVs = useVs != vs ? useVs : nullptr; g_bound.vsGen = cache.vs;
    g_bound.family = f;
    g_anyBound.store(true, std::memory_order_release);
    cache.family = f;
    ++fam.binds;
}

std::string hex64(uint64_t v) { char t[24]; _snprintf_s(t, _TRUNCATE, "%016llX", static_cast<unsigned long long>(v)); return t; }

void summaryLocked(uint64_t now) {
    const double seconds = std::max(1.0, double(now - g_windowStartMs) / 1000.0);
    const double frames = std::max<double>(1.0, double(g_draw.frames));
    const auto r = [](const std::atomic<uint64_t>& c) { return static_cast<unsigned long long>(c.load(std::memory_order_relaxed)); };
    const uint64_t sampled = g_emitSampled.exchange(0), sampledTicks = g_emitSampledTicks.exchange(0);
    const double freq = double(std::max<int64_t>(1, qpcFrequency()));
    const double emitUs = sampled ? double(sampledTicks) * 1e6 / freq / double(sampled) : 0.0;
    const double emitMsPerFrame = emitUs * double(g_emit.calls.load()) / frames / 1000.0;
    Log::get().note("engine motion: emit (%s) over %.0f s, %.0f frames: FUN_144312E00 calls %llu (%llu appended, %llu pool "
                    "records in all); pool records joined %llu (with motion %llu), masked %llu (pose changed within one "
                    "frame %llu, table full %llu); "
                    "previous pose absent: first seen %llu, gap %llu, reused pointer %llu; same-frame repeats %llu; "
                    "pose disagreements %llu (must be 0), locate failures %llu, read faults %llu, write faults %llu, "
                    "drained %llu, over 7 %llu; bracket %.2f us/call sampled, ~%.3f ms/frame on the job threads; "
                    "table %u live records.",
                    g_emitStatus, seconds, double(g_draw.frames), r(g_emit.calls), r(g_emit.callsWithItems),
                    r(g_emit.itemsAppended),
                    r(g_emit.itemsJoined), r(g_emit.itemsMoving), r(g_emit.itemsMasked), r(g_emit.sameFrameChanges),
                    r(g_emit.overflow), r(g_emit.firstSeen), r(g_emit.gaps), r(g_emit.identityResets), r(g_emit.repeats),
                    r(g_emit.disagreements), r(g_emit.locateFailures), r(g_emit.readFaults), r(g_emit.writeFaults),
                    r(g_emit.drained), r(g_emit.tooMany), emitUs, emitMsPerFrame, g_table ? g_table->live(frameNow()) : 0u);
    Log::get().note("engine motion: movers joined %.1f records/frame (moving rig records the emit wrote a previous pose for) "
                    "against the tracker's %.1f moving records/frame; eye-frames %llu, with MRT6 bound %llu, refused: "
                    "invalidated %llu (pool re-uploaded %llu, scene constants re-mapped %llu, sources changed %llu), "
                    "target 6 occupied %llu, UAV bound %llu, no pool %llu, no scene constants %llu, depth not single-sample "
                    "%llu, create failed %llu; views asked %llu, given %llu, refused: other depth %llu, other frame %llu, "
                    "invalidated %llu, unwritten %llu, no previous scene constants %llu; draw hook slow half %llu calls, "
                    "%.2f us each, ~%.3f ms/frame on the caller thread (plus %llu lock-free looks); restores %llu.",
                    double(g_emit.recordsMoving.load()) / frames,
                    g_draw.trackerFrames ? double(g_draw.trackerMovers) / double(g_draw.trackerFrames) : 0.0,
                    (unsigned long long)g_draw.eyeFrames, (unsigned long long)g_draw.eyeFramesBound,
                    (unsigned long long)(g_draw.invalidPool + g_draw.invalidScene + g_draw.sourcesChanged),
                    (unsigned long long)g_draw.invalidPool, (unsigned long long)g_draw.invalidScene,
                    (unsigned long long)g_draw.sourcesChanged, (unsigned long long)g_draw.targetOccupied,
                    (unsigned long long)g_draw.uavBound, (unsigned long long)g_draw.noPool,
                    (unsigned long long)g_draw.noScene, (unsigned long long)g_draw.depthUnsupported,
                    (unsigned long long)g_draw.createFailed,
                    (unsigned long long)g_draw.viewsAsked, (unsigned long long)g_draw.viewsGiven,
                    (unsigned long long)g_draw.refusedDepth, (unsigned long long)g_draw.refusedFrame,
                    (unsigned long long)g_draw.refusedInvalid, (unsigned long long)g_draw.refusedUnwritten,
                    (unsigned long long)g_draw.refusedPrevious,
                    (unsigned long long)g_draw.slowPaths,
                    g_draw.slowPaths ? double(g_draw.slowTicks) * 1e6 / freq / double(g_draw.slowPaths) : 0.0,
                    double(g_draw.slowTicks) * 1e3 / freq / frames, (unsigned long long)g_draw.quickPaths,
                    (unsigned long long)g_draw.restores);
    if (g_draw.pixelReads)
        Log::get().note("engine motion: pixels per eye-frame on the trained path: engine-joined %.0f, masked %.0f "
                        "(no history), pool surface not a rig record %.0f (camera term), stale slot %.0f (a later draw covered it); "
                        "%llu readbacks.",
                        double(g_draw.pixelsJoined) / double(g_draw.pixelReads), double(g_draw.pixelsMasked) / double(g_draw.pixelReads),
                        double(g_draw.pixelsCamera) / double(g_draw.pixelReads), double(g_draw.pixelsStale) / double(g_draw.pixelReads),
                        (unsigned long long)g_draw.pixelReads);
    else
        Log::get().note("engine motion: pixels: not counted this window -- the counts come from the instrumented DLSS/FSR "
                        "motion shader only (advanced.temporal_aa_diagnostics = 1, or a debug view) with the engine inputs "
                        "bound; this is not a zero count.");
    for (int f = 0; f < kFamilyCount; ++f) {
        FamilyState& s = g_families[f];
        std::string patched, failed;
        for (auto& [ptr, shader] : s.patchedPs) {
            auto info = g_ps.find(ptr);
            const std::string h = info != g_ps.end() ? "ps_" + hex64(info->second.hash) : "ps_?";
            if (shader) { if (!patched.empty()) patched += ","; patched += h; }
            else { if (!failed.empty()) failed += "; "; failed += h + " (" + s.psFailed[ptr] + ")"; }
        }
        const bool seen = std::any_of(g_vs.begin(), g_vs.end(), [&](const auto& v) { return v.second.family == f; });
        const char* state = !seen ? "not created by the game this session" : !s.derived ? "not drawn yet"
                          : s.valid ? "live" : "STOOD DOWN";
        Log::get().note("engine motion: family %s: %s%s%s; substituted %llu binds, %llu draws; patched [%s]%s%s%s%s.",
                        kFamilies[f].name, state, s.reason.empty() ? "" : " -- ", s.reason.c_str(),
                        (unsigned long long)s.binds, (unsigned long long)familyDraws[f], patched.c_str(),
                        failed.empty() ? "" : "; refused [", failed.c_str(), failed.empty() ? "" : "]",
                        s.unkeyedPsDraws ? (" ; unkeyed pixel shader ps_" + hex64(s.unkeyedPsHash) + " left stock").c_str() : "");
        s.binds = 0;
        s.unkeyedPsDraws = 0;
        familyDraws[f] = 0;
    }
    g_emit.clear();
    g_draw.clear();
    g_windowStartMs = now;
}

void clearLocked() {
    for (auto& e : g_eyes) e = Eye{};
    for (auto& w : g_watch) w.store(nullptr);
    for (auto& s : g_families) {
        s.patchedVs.clear(); s.patchedPs.clear(); s.psFailed.clear();
        s.derived = s.valid = false; s.reason.clear(); s.binds = 0; s.unkeyedPsDraws = 0;
    }
    for (auto& d : familyDraws) d = 0;
    // g_bound stays: only the owner thread may put the game's shaders back
    // (engineVelocityFrameBoundary, g_anyBound says it is owed).
    cache = DrawCache{};
    if (g_table) g_table->clear();
    g_emit.clear();
    g_draw.clear();
}

} // namespace engine_velocity_detail

using namespace engine_velocity_detail;

bool engineVelocityActive() noexcept { return live.load(std::memory_order_acquire); }

void engineVelocityConfigure(bool on) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (on == live.load(std::memory_order_acquire)) return;
    if (!on) { engineVelocityShutdown(); return; }
    // The emit bracket rides the tracker's hook set (fix.engine_motion=on
    // attaches it); the bracket itself only needs the lookup verified.
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const char* why = verifyEngine(base);
    if (!g_table) g_table = std::make_unique<emit::Table>();
    clearLocked();
    if (why) {
        g_lookup.store(nullptr, std::memory_order_release);
        g_emitStatus = why;
        kinematicEvalSetEmitObserver(nullptr);
        Log::get().note("engine motion: the emit bracket STANDS DOWN (%s): no record gets a previous pose, so every "
                        "substituted pool pixel reads as not a rig record and keeps the camera term. The pixel "
                        "substitution still runs so the log can show what it would cover.", why);
    } else {
        g_lookup.store(reinterpret_cast<emit::LookupFn>(base + kLookupRva), std::memory_order_release);
        // The bracket lives in the tracker's hook set (attached just before
        // this, by kinematicMotionConfigure): without it there are no calls.
        g_emitStatus = kinematicMotionActive() ? "live" : "hook set not attached -- see the tracker's line";
        kinematicEvalSetEmitObserver(&observeEmit);
    }
    g_windowStartMs = nowMs();
    live.store(true, std::memory_order_release);
    Log::get().note("engine motion: engine-record velocity live (fix.engine_motion=on): FUN_144312E00's records carry "
                    "their previous engine pose; the pool families' own draws write the pool slot and depth at MRT6; the "
                    "temporal pass takes exact record motion there, masks rig records it cannot follow, and keeps the "
                    "camera term elsewhere. Per-30 s lines below; every zero is printed, not omitted.");
}

void engineVelocityShutdown() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const bool was = live.exchange(false, std::memory_order_acq_rel);
    kinematicEvalSetEmitObserver(nullptr);
    g_lookup.store(nullptr, std::memory_order_release);
    clearLocked();
    if (was) Log::get().note("engine motion: engine-record velocity stood down, state cleared.");
}

void engineVelocityRememberVs(ID3D11VertexShader* shader, uint64_t hash, const void* bytecode, size_t bytes, bool linked) {
    if (t_creating || !shader || !bytecode || !bytes || bytes > 1024u * 1024u) return;
    const int f = familyOfVs(hash);
    if (f < 0) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    // One entry per shader OBJECT: the game may create a keyed hash many times.
    if (g_vs.size() >= kRememberCap || g_vs.count(shader)) return;
    VsInfo info;
    info.object = shader;
    info.family = f;
    info.linked = linked;
    info.bytes.assign(static_cast<const BYTE*>(bytecode), static_cast<const BYTE*>(bytecode) + bytes);
    g_vs.emplace(shader, std::move(info));
}

void engineVelocityRememberPs(ID3D11PixelShader* shader, uint64_t hash, const void* bytecode, size_t bytes, bool linked) {
    if (t_creating || !shader || !bytecode || !bytes || bytes > 1024u * 1024u || !anyKeyedPs(hash)) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_ps.size() >= kRememberCap || g_ps.count(shader)) return;
    PsInfo info;
    info.object = shader;
    info.hash = hash;
    info.linked = linked;
    info.bytes.assign(static_cast<const BYTE*>(bytecode), static_cast<const BYTE*>(bytecode) + bytes);
    g_ps.emplace(shader, std::move(info));
}

namespace engine_velocity_detail {
void beforeDrawSlow(ID3D11DeviceContext* ctx, bool rtv0Eye) {
    if (!ctx) return;
    const int64_t t0 = qpcNow();
    cache.vs = bindingGeneration(BindSlot::Vs);
    cache.ps = bindingGeneration(BindSlot::Ps);
    cache.rtv = bindingGeneration(BindSlot::Rtv0);
    cache.dsv = bindingGeneration(BindSlot::Dsv0);
    cache.eye = rtv0Eye;
    // The common case -- not a pool family's vertex shader, nothing of ours
    // bound -- costs no lock (terrain, the interface, every other pass).
    if (!g_anyBound.load(std::memory_order_acquire) && familyOfVs(bindingShaderHash(BindSlot::Vs)) < 0) {
        cache.family = -1;
        ++g_draw.quickPaths;
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (live.load(std::memory_order_acquire)) slowPath(ctx, rtv0Eye);
    g_draw.slowTicks += static_cast<uint64_t>(qpcNow() - t0);
}

bool watchesResource(const ID3D11Resource* resource) noexcept {
    if (!resource) return false;
    for (const auto& w : g_watch) if (w.load(std::memory_order_relaxed) == resource) return true;
    return false;
}

// A write to an eye-frame's pool or scene constants after its snapshot, while
// that eye's pass may still draw, means later draws read other contents than
// the snapshot holds: that eye-frame's engine data is dropped (counted).
void noteResourceWrite(const ID3D11Resource* resource) noexcept {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (int eye = 0; eye < 2; ++eye) {
        Eye& e = g_eyes[eye];
        if (!e.passOpen || e.invalid || e.frame != frameNow()) continue;
        if (resource == e.poolSource) { e.invalid = true; ++g_draw.invalidPool; }
        else if (resource == e.sceneSource) { e.invalid = true; ++g_draw.invalidScene; }
    }
}
} // namespace engine_velocity_detail

void engineVelocityNotePresentFrame(uint32_t presentFrame) noexcept {
    g_frame.store(presentFrame, std::memory_order_release);
}

void engineVelocityFrameBoundary(ID3D11DeviceContext* ctx) {
    // Runs while live, and once more after a stand-down that left an EDVR
    // shader bound: configure(false) may come from the config thread, which
    // must not touch the context, so the owner thread puts the game's back.
    if (!live.load(std::memory_order_acquire) && !g_anyBound.load(std::memory_order_acquire)) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    // Leave no EDVR shader bound across the frame: the binding shadow's
    // once-a-frame generation bump would otherwise hide whether it still is.
    if (ctx && (g_bound.patchedPs || g_bound.patchedVs)) {
        Ptr<ID3D11PixelShader> ps;
        ctx->PSGetShader(&ps, nullptr, nullptr);
        if (g_bound.patchedPs && ps.Get() == g_bound.patchedPs) { vScreenPSSetShaderRaw(ctx, g_bound.originalPs, nullptr, 0); ++g_draw.restores; }
        Ptr<ID3D11VertexShader> vs;
        ctx->VSGetShader(&vs, nullptr, nullptr);
        if (g_bound.patchedVs && vs.Get() == g_bound.patchedVs) { vScreenVSSetShaderRaw(ctx, g_bound.originalVs, nullptr, 0); ++g_draw.restores; }
    }
    g_bound = Bound{};
    g_anyBound.store(false, std::memory_order_release);
    if (!live.load(std::memory_order_acquire)) return;
    cache = DrawCache{};
    for (auto& e : g_eyes) e.passOpen = false;
    ++g_draw.frames;
    const KinematicMotionStats k = kinematicMotionStats();
    if (k.framesCounted) { g_draw.trackerMovers += k.moversLast; ++g_draw.trackerFrames; }
    const uint64_t now = nowMs();
    if (now - g_windowStartMs >= kSummaryMs) summaryLocked(now);
}

bool engineVelocityViews(ID3D11DeviceContext*, int eye, ID3D11Texture2D* sceneDepth, EngineVelocityViews* out) {
    if (!out) return false;
    *out = EngineVelocityViews{};
    if (!live.load(std::memory_order_acquire) || eye < 0 || eye > 1 || !sceneDepth) return false;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    ++g_draw.viewsAsked;
    Eye& e = g_eyes[eye];
    const uint32_t frame = frameNow();
    const unsigned now = frame & 1u, before = (frame - 1u) & 1u;
    if (e.depth.Get() != sceneDepth) { ++g_draw.refusedDepth; return false; }
    if (e.frame != frame) { ++g_draw.refusedFrame; return false; }
    if (e.invalid) { ++g_draw.refusedInvalid; return false; }
    if (!e.written || !e.slotsSrv || !e.poolSrv || e.sceneFrame[now] != frame || !e.scene[now]) {
        ++g_draw.refusedUnwritten;
        return false;
    }
    if (e.sceneFrame[before] != frame - 1u || !e.scene[before]) { ++g_draw.refusedPrevious; return false; }
    e.consumed = true;
    e.passOpen = false;
    out->slots = e.slotsSrv.Get(); out->slots->AddRef();
    out->pool = e.poolSrv.Get(); out->pool->AddRef();
    out->sceneNow = e.scene[now].Get(); out->sceneNow->AddRef();
    out->scenePrev = e.scene[before].Get(); out->scenePrev->AddRef();
    ++g_draw.viewsGiven;
    return true;
}

void engineVelocityNotePixels(uint32_t joined, uint32_t masked, uint32_t camera, uint32_t stale) {
    if (!live.load(std::memory_order_acquire)) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_draw.pixelsJoined += joined;
    g_draw.pixelsMasked += masked;
    g_draw.pixelsCamera += camera;
    g_draw.pixelsStale += stale;
    ++g_draw.pixelReads;
}

} // namespace edvr

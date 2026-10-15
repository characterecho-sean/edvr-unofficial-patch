// Test rig for the exposure engine path (src/d3d11/exposure_chain.h, the always-on exposure sharing).
//
// Three parts:
//   1. The decision: which dispatches of the second eye's chain are skipped, when the engine engages, and every way it declines, falls back or
//      stands down.
//   2. The equivalence, on a real (WARP) device with the debug layer: three pipelines over the same inputs -- stock with no sharing, stock with
//      the old share copy, and the engine path -- whose compute shaders have the real chain's shape (a bounds pass; an exposure pass with the one
//      history texel it reads and writes back, and five UAVs; a tonemap stand-in that reads strip[0]). The engine path must leave eye 0 exactly as
//      stock leaves it and eye 1 exactly as the old share copy left it, every frame, through engaging, a lost confirmation and re-engaging.
//   3. Source pins on the glue in exposure_fix.cpp and device_hook.cpp that no rig can run (the hook is linked into the DLL only), and on the
//      removal of the Scroll Lock toggle.
//
// No window, no files written. --self-test runs it; --dry-run says what it would do.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/common/system_d3d11.h"
#include "../../src/d3d11/exposure_chain.h"

using Microsoft::WRL::ComPtr;
namespace ec = edvr::exposurechain;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

void checkf(bool ok, const char* fmt, int a) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), fmt, a);
    check(ok, buf);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 1. The decision

constexpr uint64_t kPre = 0xB8E727E94D44D891ull;     // the bounds pass, as the census names it
constexpr uint64_t kExp = 0xF1FB2EEFB662F3AAull;     // the exposure pass
constexpr uintptr_t kU0A = 0x1000, kU0B = 0x2000;

ec::Facts pre(int eye, uint32_t mask = 0x03, uint64_t hash = kPre) {
    ec::Facts f;
    f.hash = hash;
    f.isTarget = false;
    f.u0 = eye == 0 ? kU0A : kU0B;
    f.uavMask = mask;
    return f;
}

ec::Facts target(int eye, uint32_t mask = ec::kTargetUavMask) {
    ec::Facts f;
    f.hash = kExp;
    f.isTarget = true;
    f.u0 = eye == 0 ? kU0A : kU0B;
    f.uavMask = mask;
    return f;
}

struct Frame {
    ec::Action preA = ec::Action::Run, expA = ec::Action::Run, preB = ec::Action::Run, expB = ec::Action::Run;
    bool fallbackPreB = false, fallbackExpB = false;
};

// One frame of the chain the census shows: B8 F1 B8 F1, the copy after the second F1 (only when the detection is confirmed, as in the hook),
// then the boundary.
Frame runFrame(ec::Chain& c, bool active, bool confirmed, uint32_t copiedSlots = ec::kShareSlots, uint64_t preHash = kPre) {
    Frame fr;
    ec::Decision d = c.onDispatch(pre(0, 0x03, preHash), active, confirmed);
    fr.preA = d.action;
    d = c.onDispatch(target(0), active, confirmed);
    fr.expA = d.action;
    d = c.onDispatch(pre(1, 0x03, preHash), active, confirmed);
    fr.preB = d.action;
    fr.fallbackPreB = d.fallback;
    d = c.onDispatch(target(1), active, confirmed);
    fr.expB = d.action;
    fr.fallbackExpB = d.fallback;
    if (active && confirmed) c.onShareCopied(copiedSlots, d.action == ec::Action::Skip);
    c.frameBoundary();
    return fr;
}

// Frames until the engine skips the second eye's exposure dispatch (1-based), or -1.
int framesToEngage(ec::Chain& c, bool confirmed = true) {
    for (int i = 1; i <= 40; ++i) {
        const Frame fr = runFrame(c, true, confirmed);
        if (fr.expB == ec::Action::Skip) return i;
    }
    return -1;
}

void testLearningAndSkip() {
    std::printf("decision: learning, the skip, only the second eye\n");
    ec::Chain c;
    for (int i = 1; i <= 5; ++i) {
        const Frame fr = runFrame(c, true, true);
        checkf(fr.preA == ec::Action::Run && fr.expA == ec::Action::Run && fr.preB == ec::Action::Run && fr.expB == ec::Action::Run,
               "frame %d, while learning, runs everything", i);
        checkf(!fr.fallbackPreB && !fr.fallbackExpB, "frame %d, while learning, is not a fallback", i);
    }
    check(c.skippedPre() == 0 && c.skippedTarget() == 0 && c.fallbacks() == 0, "nothing skipped or counted while learning");
    check(c.preHash() == kPre, "the predecessor is learned from the dispatch before the exposure shader, not hard-coded");
    const Frame fr = runFrame(c, true, true);
    check(fr.preA == ec::Action::Run && fr.expA == ec::Action::Run, "engaged: the FIRST eye's chain still runs");
    check(fr.preB == ec::Action::Skip && fr.expB == ec::Action::Skip, "engaged: the second eye's two dispatches are skipped");
    check(c.skippedPre() == 1 && c.skippedTarget() == 1 && c.fallbacks() == 0, "counters: one of each, no fallback");
    check(c.savedMs() > 0.149 && c.savedMs() < 0.151, "the saving is the census figure for one second-eye chain (0.113 + 0.037)");
    check(c.engagedNow(true) && !c.engagedNow(false), "engaged only while the detection is confirmed");
    for (int i = 0; i < 20; ++i) runFrame(c, true, true);
    check(c.skippedPre() == 21 && c.skippedTarget() == 21, "every later frame skips exactly one of each");
    check(c.latched() == ec::Why::None, "no latch on the happy path");
}

void testDeclines() {
    std::printf("decision: declines\n");
    {   // the flat profile, or the fix rejected itself: stock
        ec::Chain c;
        for (int i = 0; i < 30; ++i) {
            const Frame fr = runFrame(c, false, true);
            check(fr.preB == ec::Action::Run && fr.expB == ec::Action::Run, "inactive: both chains run");
        }
        check(c.skippedPre() + c.skippedTarget() + c.fallbacks() == 0, "inactive: nothing counted");
        check(c.preStreak() == 0, "inactive: the learner is not fed");
    }
    {   // detection not confirmed (menus): stock, and engages once it is
        ec::Chain c;
        for (int i = 0; i < 30; ++i) {
            const Frame fr = runFrame(c, true, false);
            check(fr.preB == ec::Action::Run && fr.expB == ec::Action::Run, "unconfirmed: both chains run");
        }
        check(c.skippedPre() + c.skippedTarget() + c.fallbacks() == 0, "unconfirmed: nothing skipped, nothing counted as a fallback");
        // The copy is not made on an unconfirmed frame, so the copy gate is unlearned: the first confirmed frame teaches it, the next engages.
        check(framesToEngage(c) == 2, "...and once confirmed, one frame teaches the copy and the next engages");
    }
    {   // the older build: four UAVs
        ec::Chain c;
        for (int i = 0; i < 30; ++i) {
            c.onDispatch(pre(0), true, true);
            c.onDispatch(target(0, 0x0F), true, true);
            c.onDispatch(pre(1), true, true);
            const ec::Decision d = c.onDispatch(target(1, 0x0F), true, true);
            c.onShareCopied(4, false);
            check(d.action == ec::Action::Run, "four UAVs: runs");
            c.frameBoundary();
        }
        check(c.learnVerdict() == ec::Why::TargetShape, "four UAVs: the learner names the shape");
        check(c.skippedPre() + c.skippedTarget() == 0, "four UAVs: never skips");
    }
    {   // a sixth UAV (a future build with another unshared output)
        ec::Chain c;
        for (int i = 0; i < 30; ++i) {
            c.onDispatch(pre(0), true, true);
            c.onDispatch(target(0, 0x3F), true, true);
            c.onDispatch(pre(1), true, true);
            const ec::Decision d = c.onDispatch(target(1, 0x3F), true, true);
            c.onShareCopied(5, false);
            check(d.action == ec::Action::Run, "six UAVs: runs");
            c.frameBoundary();
        }
        check(c.learnVerdict() == ec::Why::TargetShape && c.skippedPre() + c.skippedTarget() == 0, "six UAVs: never skips");
    }
    {   // the dispatch before the exposure shader does not share its u0
        ec::Chain c;
        for (int i = 0; i < 30; ++i) {
            ec::Facts p = pre(0);
            p.u0 = 0x9999;
            c.onDispatch(p, true, true);
            c.onDispatch(target(0), true, true);
            p = pre(1);
            p.u0 = 0x8888;
            c.onDispatch(p, true, true);
            const ec::Decision d = c.onDispatch(target(1), true, true);
            c.onShareCopied(5, false);
            check(d.action == ec::Action::Run, "unrelated predecessor: runs");
            c.frameBoundary();
        }
        check(c.learnVerdict() == ec::Why::NoPredecessor && c.skippedPre() + c.skippedTarget() == 0, "unrelated predecessor: never skips");
    }
    {   // a predecessor that does not bind both of its UAVs
        ec::Chain c;
        for (int i = 0; i < 30; ++i) {
            c.onDispatch(pre(0, 0x01), true, true);
            c.onDispatch(target(0), true, true);
            c.onDispatch(pre(1, 0x01), true, true);
            c.onDispatch(target(1), true, true);
            c.onShareCopied(5, false);
            c.frameBoundary();
        }
        check(c.skippedPre() + c.skippedTarget() == 0, "a predecessor with one UAV: never skips");
    }
    {   // both predecessors first (B8 B8 F1 F1): the second F1's predecessor is not its own
        ec::Chain c;
        for (int i = 0; i < 30; ++i) {
            c.onDispatch(pre(0), true, true);
            c.onDispatch(pre(1), true, true);
            c.onDispatch(target(0), true, true);
            const ec::Decision d = c.onDispatch(target(1), true, true);
            c.onShareCopied(5, false);
            check(d.action == ec::Action::Run, "B8 B8 F1 F1: runs");
            c.frameBoundary();
        }
        check(c.skippedPre() + c.skippedTarget() == 0, "B8 B8 F1 F1: never skips (the order is not the learned one)");
    }
    {   // one chain a frame
        ec::Chain c;
        for (int i = 0; i < 30; ++i) {
            c.onDispatch(pre(0), true, true);
            c.onDispatch(target(0), true, true);
            c.frameBoundary();
        }
        check(c.skippedPre() + c.skippedTarget() == 0 && c.fallbacks() == 0, "one chain a frame: nothing is a second eye");
    }
    {   // both eyes on one u0 (nothing to copy between)
        ec::Chain c;
        for (int i = 0; i < 30; ++i) {
            c.onDispatch(pre(0), true, true);
            c.onDispatch(target(0), true, true);
            c.onDispatch(pre(0), true, true);
            const ec::Decision d = c.onDispatch(target(0), true, true);
            c.onShareCopied(5, false);
            check(d.action == ec::Action::Run, "one shared u0: runs");
            c.frameBoundary();
        }
        check(c.skippedPre() + c.skippedTarget() == 0, "one shared u0: never skips");
    }
    {   // the copy never covers all five
        ec::Chain c;
        for (int i = 0; i < 30; ++i) runFrame(c, true, true, 3);
        check(c.skippedPre() + c.skippedTarget() == 0, "a copy that covers 3 of 5 slots: never engages");
    }
    {   // a shader that is not the exposure shader (isTarget false) with the learned predecessor's hash never starts anything
        ec::Chain c;
        for (int i = 0; i < 12; ++i) {
            c.onDispatch(pre(0), true, true);
            c.onDispatch(pre(1), true, true);
            c.frameBoundary();
        }
        check(c.skippedPre() == 0 && c.preHash() == 0, "predecessor-shaped dispatches with no exposure shader behind them teach nothing");
    }
}

void testFallbacksAndLatches() {
    std::printf("decision: fallbacks and latches\n");
    {   // the second eye's view is recreated: that frame runs whole, the next skips again on the new view
        ec::Chain c;
        framesToEngage(c);
        const uint64_t before = c.skippedPre();
        c.onDispatch(pre(0), true, true);
        c.onDispatch(target(0), true, true);
        ec::Facts p = pre(1), t = target(1);
        p.u0 = t.u0 = 0x7777;
        const ec::Decision dp = c.onDispatch(p, true, true);
        check(dp.action == ec::Action::Run && dp.fallback && dp.why == ec::Why::NoPredecessor,
              "view recreated: the predecessor runs, counted as a fallback");
        const ec::Decision dt = c.onDispatch(t, true, true);
        check(dt.action == ec::Action::Run && dt.fallback, "view recreated: so does the exposure dispatch");
        c.onShareCopied(5, false);
        c.frameBoundary();
        check(c.fallbacks() == 2 && c.skippedPre() == before, "two dispatches ran as stock and were counted");
        check(c.latched() == ec::Why::None, "a view change is not a latch");
        c.onDispatch(pre(0), true, true);
        c.onDispatch(target(0), true, true);
        const ec::Decision dp2 = c.onDispatch(p, true, true);
        const ec::Decision dt2 = c.onDispatch(t, true, true);
        check(dp2.action == ec::Action::Skip && dt2.action == ec::Action::Skip, "the next frame skips again, on the new view");
        c.onShareCopied(5, true);
        c.frameBoundary();
    }
    {   // the exposure shader's UAV set changes after its predecessor was skipped: it runs, and the engine stands down
        ec::Chain c;
        framesToEngage(c);
        c.onDispatch(pre(0), true, true);
        c.onDispatch(target(0), true, true);
        const ec::Decision dp = c.onDispatch(pre(1), true, true);
        check(dp.action == ec::Action::Skip, "setup: the predecessor is skipped");
        const ec::Decision dt = c.onDispatch(target(1, 0x0F), true, true);
        check(dt.action == ec::Action::Run && dt.fallback && dt.why == ec::Why::TargetShape, "the mismatched exposure dispatch runs");
        c.onShareCopied(4, false);
        c.frameBoundary();
        check(c.latched() == ec::Why::TargetShape, "...and the engine stands down for the session");
        const Frame fr = runFrame(c, true, true);
        check(fr.preB == ec::Action::Run && fr.expB == ec::Action::Run, "latched: stock from then on");
    }
    {   // a skipped predecessor with no exposure dispatch behind it
        ec::Chain c;
        framesToEngage(c);
        c.onDispatch(pre(0), true, true);
        c.onDispatch(target(0), true, true);
        const ec::Decision dp = c.onDispatch(pre(1), true, true);
        check(dp.action == ec::Action::Skip, "setup: the predecessor is skipped");
        c.frameBoundary();   // the second exposure dispatch never came
        check(c.latched() == ec::Why::LatchedUnfollowed, "an unfollowed skip latches");
        const Frame fr = runFrame(c, true, true);
        check(fr.preB == ec::Action::Run && fr.expB == ec::Action::Run, "latched: stock from then on");
    }
    {   // the copy comes up short on a skipped frame
        ec::Chain c;
        framesToEngage(c);
        const Frame fr = runFrame(c, true, true, 3);
        check(fr.expB == ec::Action::Skip, "setup: the frame skipped");
        check(c.latched() == ec::Why::LatchedCopy, "a short copy after a skip latches");
        const Frame next = runFrame(c, true, true);
        check(next.preB == ec::Action::Run && next.expB == ec::Action::Run, "latched: stock from then on");
    }
    {   // a third chain in a frame: only the second is skipped
        ec::Chain c;
        framesToEngage(c);
        c.onDispatch(pre(0), true, true);
        c.onDispatch(target(0), true, true);
        check(c.onDispatch(pre(1), true, true).action == ec::Action::Skip, "three chains: the second predecessor is skipped");
        check(c.onDispatch(target(1), true, true).action == ec::Action::Skip, "three chains: the second exposure dispatch is skipped");
        c.onShareCopied(5, true);
        check(c.onDispatch(pre(0), true, true).action == ec::Action::Run, "three chains: the third predecessor runs");
        check(c.onDispatch(target(0), true, true).action == ec::Action::Run, "three chains: the third exposure dispatch runs");
        c.frameBoundary();
        check(c.latched() == ec::Why::None, "three chains: no latch");
    }
    {   // the game's update renames the predecessor: relearned, stock meanwhile, then engaged on the new one
        ec::Chain c;
        framesToEngage(c);
        constexpr uint64_t kNewPre = 0x1234567890ABCDEFull;
        int stock = 0;
        int skippedAt = -1;
        for (int i = 1; i <= 20; ++i) {
            const Frame fr = runFrame(c, true, true, ec::kShareSlots, kNewPre);
            if (fr.expB == ec::Action::Skip && skippedAt < 0) skippedAt = i;
            if (skippedAt < 0 && fr.preB == ec::Action::Run && fr.expB == ec::Action::Run) ++stock;
        }
        check(c.preHash() == kNewPre, "the new predecessor is learned");
        check(stock == 5 && skippedAt == 6, "stock for the five frames of relearning, then skipping on the new hash");
        check(c.fallbacks() == 0 && c.latched() == ec::Why::None, "relearning is neither a fallback nor a latch");
    }
    {   // engaged, then the detection lapses (a menu), then returns: stock while it lapses, engaged the frame it returns
        ec::Chain c;
        framesToEngage(c);
        for (int i = 0; i < 6; ++i) {
            const Frame fr = runFrame(c, true, false);
            check(fr.preB == ec::Action::Run && fr.expB == ec::Action::Run, "lapse: stock");
        }
        const Frame back = runFrame(c, true, true);
        check(back.preB == ec::Action::Skip && back.expB == ec::Action::Skip, "...and skipping again on the first confirmed frame");
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 2. The equivalence on a device

const char* kPreHlsl =
    "Texture2D<float> src : register(t0);\n"
    "RWStructuredBuffer<float2> bounds : register(u0);\n"
    "RWTexture2D<float> map : register(u1);\n"
    "[numthreads(1,1,1)] void main() {\n"
    "  float mn = 1e9, mx = -1e9;\n"
    "  for (uint y = 0; y < 4; ++y) for (uint x = 0; x < 4; ++x) {\n"
    "    float v = src.Load(int3(x, y, 0)); map[uint2(x, y)] = v; mn = min(mn, v); mx = max(mx, v); }\n"
    "  bounds[0] = float2(mn, mx);\n"
    "}\n";

// The real pass's shape: reads the bounds and the map, reads and writes strip[0] (the one history texel), writes strip[1..5] and three
// histograms. The arithmetic is arbitrary; what matters is that every output depends on the eye's own bounds and map, and strip[0] on its history.
const char* kExpHlsl =
    "Texture2D<float> map : register(t0);\n"
    "RWStructuredBuffer<float2> bounds : register(u0);\n"
    "RWTexture2D<float> strip : register(u1);\n"
    "RWTexture2D<uint> h0 : register(u2);\n"
    "RWTexture2D<float> h1 : register(u3);\n"
    "RWTexture2D<float> h2 : register(u4);\n"
    "[numthreads(1,1,1)] void main() {\n"
    "  float2 b = bounds[0];\n"
    "  float hist = strip[uint2(0, 0)];\n"
    "  float t = b.y * 0.5 + map.Load(int3(1, 1, 0)) * 0.25;\n"
    "  float a = hist + (t - hist) * 0.25;\n"
    "  strip[uint2(0, 0)] = a;\n"
    "  for (uint i = 1; i < 6; ++i) strip[uint2(i, 0)] = a * i + b.x;\n"
    "  for (uint j = 0; j < 4; ++j) {\n"
    "    h0[uint2(j, 0)] = (uint)(b.y * 100.0) + j; h1[uint2(j, 0)] = b.x + j; h2[uint2(j, 0)] = a + j * 0.5; }\n"
    "}\n";

// The tonemap stand-in: the real tonemap's vertex shader reads strip[0] (vs_2D78DC3F: sample_l t0 at 0,0).
const char* kToneHlsl =
    "Texture2D<float> strip : register(t0);\n"
    "RWStructuredBuffer<float> outv : register(u0);\n"
    "[numthreads(1,1,1)] void main() { outv[0] = strip.Load(int3(0, 0, 0)); }\n";

ComPtr<ID3D11ComputeShader> compile(ID3D11Device* dev, const char* hlsl) {
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3DCompile(hlsl, std::strlen(hlsl), "exposure_chain_test", nullptr, nullptr, "main", "cs_5_0", 0, 0, &blob, &err);
    if (FAILED(hr)) {
        std::printf("shader compile failed: %s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return nullptr;
    }
    ComPtr<ID3D11ComputeShader> cs;
    dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs);
    return cs;
}

struct Eye {
    ComPtr<ID3D11Texture2D> src, map, strip, h0, h1, h2;
    ComPtr<ID3D11Buffer> bounds, tone;
    ComPtr<ID3D11ShaderResourceView> srcSrv, mapSrv, stripSrv;
    ComPtr<ID3D11UnorderedAccessView> boundsUav, mapUav, stripUav, h0Uav, h1Uav, h2Uav, toneUav;
};

struct Shaders {
    ComPtr<ID3D11ComputeShader> pre, exp, tone;
};

bool makeTex(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, UINT bind, const void* init, UINT pitch, ComPtr<ID3D11Texture2D>& out) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = init;
    sd.SysMemPitch = pitch;
    return SUCCEEDED(dev->CreateTexture2D(&d, init ? &sd : nullptr, &out));
}

bool makeBuf(ID3D11Device* dev, UINT bytes, UINT stride, const void* init, ComPtr<ID3D11Buffer>& out) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = bytes;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    d.StructureByteStride = stride;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = init;
    return SUCCEEDED(dev->CreateBuffer(&d, init ? &sd : nullptr, &out));
}

bool makeEye(ID3D11Device* dev, Eye& e) {
    const UINT both = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    float zeros16[16] = {};
    float stripInit[6] = {1.0f, 0, 0, 0, 0, 0};
    float zeros4[4] = {};
    float bnd[2] = {0, 0};
    float tone = 0;
    bool ok = makeTex(dev, 4, 4, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, zeros16, 16, e.src) &&
              makeTex(dev, 4, 4, DXGI_FORMAT_R32_FLOAT, both, zeros16, 16, e.map) &&
              makeTex(dev, 6, 1, DXGI_FORMAT_R32_TYPELESS, both, stripInit, 24, e.strip) &&
              makeTex(dev, 4, 1, DXGI_FORMAT_R32_TYPELESS, both, zeros4, 16, e.h0) &&
              makeTex(dev, 4, 1, DXGI_FORMAT_R32_TYPELESS, both, zeros4, 16, e.h1) &&
              makeTex(dev, 4, 1, DXGI_FORMAT_R32_TYPELESS, both, zeros4, 16, e.h2) &&
              makeBuf(dev, 8, 8, bnd, e.bounds) && makeBuf(dev, 4, 4, &tone, e.tone);
    if (!ok) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    ok = SUCCEEDED(dev->CreateShaderResourceView(e.src.Get(), &sv, &e.srcSrv)) &&
         SUCCEEDED(dev->CreateShaderResourceView(e.map.Get(), &sv, &e.mapSrv)) &&
         SUCCEEDED(dev->CreateShaderResourceView(e.strip.Get(), &sv, &e.stripSrv));
    D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    uv.Format = DXGI_FORMAT_R32_FLOAT;
    ok = ok && SUCCEEDED(dev->CreateUnorderedAccessView(e.map.Get(), &uv, &e.mapUav)) &&
         SUCCEEDED(dev->CreateUnorderedAccessView(e.strip.Get(), &uv, &e.stripUav)) &&
         SUCCEEDED(dev->CreateUnorderedAccessView(e.h1.Get(), &uv, &e.h1Uav)) &&
         SUCCEEDED(dev->CreateUnorderedAccessView(e.h2.Get(), &uv, &e.h2Uav));
    uv.Format = DXGI_FORMAT_R32_UINT;
    ok = ok && SUCCEEDED(dev->CreateUnorderedAccessView(e.h0.Get(), &uv, &e.h0Uav));
    D3D11_UNORDERED_ACCESS_VIEW_DESC bv{};
    bv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    bv.Format = DXGI_FORMAT_UNKNOWN;
    bv.Buffer.NumElements = 1;
    ok = ok && SUCCEEDED(dev->CreateUnorderedAccessView(e.bounds.Get(), &bv, &e.boundsUav)) &&
         SUCCEEDED(dev->CreateUnorderedAccessView(e.tone.Get(), &bv, &e.toneUav));
    return ok;
}

std::vector<uint8_t> dump(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Resource* res) {
    std::vector<uint8_t> out;
    D3D11_RESOURCE_DIMENSION dim{};
    res->GetType(&dim);
    ComPtr<ID3D11Resource> stage;
    UINT rows = 1, rowBytes = 0;
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d{};
        static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.MiscFlags = 0;
        d.StructureByteStride = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> b;
        if (FAILED(dev->CreateBuffer(&d, nullptr, &b))) return out;
        stage = b;
        rowBytes = d.ByteWidth;
    } else {
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> t;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &t))) return out;
        stage = t;
        rows = d.Height;
        rowBytes = d.Width * 4;
    }
    ctx->CopyResource(stage.Get(), res);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m))) return out;
    for (UINT r = 0; r < rows; ++r) {
        const uint8_t* p = static_cast<const uint8_t*>(m.pData) + (dim == D3D11_RESOURCE_DIMENSION_BUFFER ? 0 : r * m.RowPitch);
        out.insert(out.end(), p, p + rowBytes);
    }
    ctx->Unmap(stage.Get(), 0);
    return out;
}

enum class Mode { NoShare, Share, Engine };

struct Pipe {
    Mode mode;
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    const Shaders* sh;
    Eye eye[2];
    ec::Chain chain;
    uint64_t dispatches = 0;
    uint64_t csInvocations = 0;
    bool statsOk = false;
    ComPtr<ID3D11Query> stats;

    bool init() {
        D3D11_QUERY_DESC qd{};
        qd.Query = D3D11_QUERY_PIPELINE_STATISTICS;
        statsOk = SUCCEEDED(dev->CreateQuery(&qd, &stats));
        return makeEye(dev, eye[0]) && makeEye(dev, eye[1]);
    }

    void unbind() {
        ID3D11UnorderedAccessView* none[8] = {};
        ctx->CSSetUnorderedAccessViews(0, 8, none, nullptr);
        ID3D11ShaderResourceView* noneSrv[2] = {};
        ctx->CSSetShaderResources(0, 2, noneSrv);
    }

    // The hook's own sequence for one compute dispatch: the chain decides, the dispatch is forwarded unless skipped.
    ec::Decision decide(uint64_t hash, bool isTarget, int e, uint32_t mask, bool active, bool confirmed) {
        if (mode != Mode::Engine) return ec::Decision();
        ec::Facts f;
        f.hash = hash;
        f.isTarget = isTarget;
        f.u0 = reinterpret_cast<uintptr_t>(eye[e].boundsUav.Get());
        f.uavMask = mask;
        return chain.onDispatch(f, active, confirmed);
    }

    void frame(float l0, float l1, bool active, bool confirmed) {
        if (statsOk) ctx->Begin(stats.Get());
        float lum[2] = {l0, l1};
        int seenTargets = 0;
        bool lastSkipped = false;
        for (int e = 0; e < 2; ++e) {
            Eye& E = eye[e];
            float px[16];
            for (int i = 0; i < 16; ++i) px[i] = lum[e] * (0.5f + 0.1f * static_cast<float>(i));
            ctx->UpdateSubresource(E.src.Get(), 0, nullptr, px, 16, 0);

            // The bounds pass
            ec::Decision d = decide(kPre, false, e, 0x03, active, confirmed);
            if (d.action == ec::Action::Run) {
                ctx->CSSetShader(sh->pre.Get(), nullptr, 0);
                ID3D11ShaderResourceView* srv[1] = {E.srcSrv.Get()};
                ctx->CSSetShaderResources(0, 1, srv);
                ID3D11UnorderedAccessView* uavs[2] = {E.boundsUav.Get(), E.mapUav.Get()};
                ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
                ctx->Dispatch(1, 1, 1);
                ++dispatches;
                unbind();
            }
            // The exposure pass
            d = decide(kExp, true, e, ec::kTargetUavMask, active, confirmed);
            ++seenTargets;
            lastSkipped = d.action == ec::Action::Skip;
            if (!lastSkipped) {
                ctx->CSSetShader(sh->exp.Get(), nullptr, 0);
                ID3D11ShaderResourceView* srv[1] = {E.mapSrv.Get()};
                ctx->CSSetShaderResources(0, 1, srv);
                ID3D11UnorderedAccessView* uavs[5] = {E.boundsUav.Get(), E.stripUav.Get(), E.h0Uav.Get(), E.h1Uav.Get(), E.h2Uav.Get()};
                ctx->CSSetUnorderedAccessViews(0, 5, uavs, nullptr);
                ctx->Dispatch(1, 1, 1);
                ++dispatches;
                unbind();
            }
            // The share copy, where the second exposure dispatch is (run or skipped): five slots, first eye to second
            if (seenTargets == 2 && mode != Mode::NoShare && active && confirmed) {
                Eye& A = eye[0];
                ctx->CopyResource(E.bounds.Get(), A.bounds.Get());
                ctx->CopyResource(E.strip.Get(), A.strip.Get());
                ctx->CopyResource(E.h0.Get(), A.h0.Get());
                ctx->CopyResource(E.h1.Get(), A.h1.Get());
                ctx->CopyResource(E.h2.Get(), A.h2.Get());
                if (mode == Mode::Engine) chain.onShareCopied(ec::kShareSlots, lastSkipped);
            }
            // The tonemap stand-in
            ctx->CSSetShader(sh->tone.Get(), nullptr, 0);
            ID3D11ShaderResourceView* srv[1] = {E.stripSrv.Get()};
            ctx->CSSetShaderResources(0, 1, srv);
            ID3D11UnorderedAccessView* uavs[1] = {E.toneUav.Get()};
            ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
            ctx->Dispatch(1, 1, 1);
            ++dispatches;
            unbind();
        }
        if (mode == Mode::Engine) chain.frameBoundary();
        if (statsOk) {
            ctx->End(stats.Get());
            D3D11_QUERY_DATA_PIPELINE_STATISTICS s{};
            while (ctx->GetData(stats.Get(), &s, sizeof(s), 0) == S_FALSE) ctx->Flush();
            csInvocations += s.CSInvocations;
        }
    }
};

std::vector<uint8_t> all(Pipe& p, int e, bool withMap) {
    std::vector<uint8_t> v;
    auto add = [&](ID3D11Resource* r) {
        const std::vector<uint8_t> d = dump(p.dev, p.ctx, r);
        v.insert(v.end(), d.begin(), d.end());
    };
    add(p.eye[e].bounds.Get());
    add(p.eye[e].strip.Get());
    add(p.eye[e].h0.Get());
    add(p.eye[e].h1.Get());
    add(p.eye[e].h2.Get());
    if (withMap) add(p.eye[e].map.Get());
    return v;
}

float toneOf(Pipe& p, int e) {
    const std::vector<uint8_t> d = dump(p.dev, p.ctx, p.eye[e].tone.Get());
    float f = -1.0f;
    if (d.size() >= 4) std::memcpy(&f, d.data(), 4);
    return f;
}

struct Schedule {
    const char* name;
    bool (*active)(int);
    bool (*confirmed)(int);
};

void runEquivalence(ID3D11Device* dev, ID3D11DeviceContext* ctx, const Shaders& sh, const Schedule& sc) {
    std::printf("equivalence on a device: %s\n", sc.name);
    Pipe stock{Mode::NoShare, dev, ctx, &sh}, share{Mode::Share, dev, ctx, &sh}, engine{Mode::Engine, dev, ctx, &sh};
    if (!stock.init() || !share.init() || !engine.init()) {
        check(false, "the three pipelines' resources create");
        return;
    }
    int mismatchTone0 = 0, mismatchTone1 = 0, mismatchEye0 = 0, mismatchEye1 = 0, mismatchEye0VsStock = 0, divergedFromStock = 0;
    for (int f = 0; f < 70; ++f) {
        // Eye 1 meets a star from frame 8: its own metering would stop it down, which is the whole reason for sharing.
        const float l0 = 2.0f + 0.5f * static_cast<float>((f * 7) % 5);
        const float l1 = f >= 8 ? l0 + 9.0f : l0;
        const bool act = sc.active(f), conf = sc.confirmed(f);
        stock.frame(l0, l1, act, conf);
        share.frame(l0, l1, act, conf);
        engine.frame(l0, l1, act, conf);
        if (toneOf(share, 0) != toneOf(engine, 0)) ++mismatchTone0;
        if (toneOf(share, 1) != toneOf(engine, 1)) ++mismatchTone1;
        if (all(share, 0, true) != all(engine, 0, true)) ++mismatchEye0;
        if (all(share, 1, false) != all(engine, 1, false)) ++mismatchEye1;
        if (all(stock, 0, true) != all(engine, 0, true)) ++mismatchEye0VsStock;
        if (act && conf && toneOf(stock, 1) != toneOf(engine, 1)) ++divergedFromStock;
    }
    checkf(mismatchTone0 == 0, "eye 0's tonemap input matches the old share copy on every frame (%d mismatches)", mismatchTone0);
    checkf(mismatchTone1 == 0, "eye 1's tonemap input matches the old share copy on every frame (%d mismatches)", mismatchTone1);
    checkf(mismatchEye0 == 0, "eye 0's whole result (u0-u4 and the map) matches the old share copy on every frame (%d mismatches)", mismatchEye0);
    checkf(mismatchEye1 == 0, "eye 1's result (u0-u4) matches the old share copy on every frame (%d mismatches)", mismatchEye1);
    checkf(mismatchEye0VsStock == 0, "eye 0 is exactly what stock leaves it, on every frame (%d mismatches)", mismatchEye0VsStock);
    check(divergedFromStock > 0, "the sharing is doing something: eye 1 differs from the no-share eye 1 whenever the fix is on");
    check(engine.chain.skippedPre() > 0 && engine.chain.skippedTarget() > 0, "the engine path engaged in this schedule");
    check(engine.chain.latched() == ec::Why::None && engine.chain.fallbacks() == 0, "...with no fallback and no latch");
    check(engine.dispatches < share.dispatches, "the engine path issued fewer dispatches");
    check(share.dispatches - engine.dispatches == engine.chain.skippedPre() + engine.chain.skippedTarget(),
          "...fewer by exactly the skipped count");
    if (engine.statsOk && share.statsOk && engine.csInvocations != 0) {
        // The GPU's own count of compute thread invocations: one thread per dispatch here, so it is the dispatch count seen from the device.
        check(share.csInvocations - engine.csInvocations == engine.chain.skippedPre() + engine.chain.skippedTarget(),
              "the device's CS invocation count drops by exactly the skipped dispatches");
    } else {
        std::printf("  (pipeline statistics unavailable on this adapter: invocation check skipped)\n");
    }
}

bool alwaysOn(int) { return true; }
bool lapseConfirm(int f) { return !(f >= 20 && f < 26); }
bool lapseActive(int f) { return !(f >= 30 && f < 33); }

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 3. Source pins

std::string slurp(const std::string& root, const char* rel) {
    std::ifstream in(root + "/" + rel, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

size_t find(const std::string& s, const char* needle, size_t from = 0) {
    return s.find(needle, from);
}

size_t countOf(const std::string& s, const char* needle) {
    size_t n = 0, at = 0;
    while ((at = s.find(needle, at)) != std::string::npos) {
        ++n;
        at += std::strlen(needle);
    }
    return n;
}

void testSourcePins(const std::string& root) {
    std::printf("source pins\n");
    const std::string ex = slurp(root, "src/d3d11/exposure_fix.cpp");
    const std::string exh = slurp(root, "src/d3d11/exposure_fix.h");
    const std::string dh = slurp(root, "src/d3d11/device_hook.cpp");
    const std::string ini = slurp(root, "edvr.ini");
    check(!ex.empty() && !exh.empty() && !dh.empty() && !ini.empty(), "the sources are readable");

    const size_t hd = find(ex, "void STDMETHODCALLTYPE hookedDispatch(");
    const size_t census = find(ex, "drawCensusDispatch(self, x, y, z, false, nullptr, 0)", hd);
    const size_t decide = find(ex, "s->chain.onDispatch(", hd);
    const size_t fwd = find(ex, "if (!skipped) s->realDispatch(self, x, y, z);", hd);
    check(hd != std::string::npos && census != std::string::npos && decide != std::string::npos && fwd != std::string::npos,
          "hookedDispatch carries the census record, the chain decision and the guarded forward");
    check(census < decide && decide < fwd, "the census records what the game SUBMITTED, then the chain decides, then the forward is guarded");
    check(find(ex, "if (!isTarget) return;", fwd) != std::string::npos, "a skipped predecessor leaves through the same exit as any non-target");
    const size_t share = find(ex, "const uint32_t covered = shareExposure(self, s->firstEye, second);", hd);
    check(share != std::string::npos && share > fwd, "the share copy follows the (possibly skipped) second exposure dispatch");
    check(find(ex, "s->chain.onShareCopied(covered, skipped);", share) != std::string::npos, "the copy's coverage is reported to the engine");
    check(find(ex, "if (!s->pinned && s->detectStreak < kConfirmFrames) return;", hd) != std::string::npos,
          "the copy keeps its confirmation gate (a skip only happens once confirmed)");
    check(find(ex, "s->firstEye[4] = ") != std::string::npos && find(ex, "second[4] = ") != std::string::npos,
          "the fifth UAV is captured for both eyes");
    check(find(ex, "slot < exposurechain::kShareSlots") != std::string::npos, "the copy covers all five slots");
    check(find(ex, "g_state->uav[slot] = uavs[i];") != std::string::npos && find(ex, "for (void*& v : g_state->uav) v = nullptr;") != std::string::npos,
          "the UAV mirror follows the bind calls and ClearState");
    check(find(ex, "share_exposure\"") == std::string::npos, "no source reads a share_exposure setting: the sharing is always on in VR");
    check(find(ex, "g_state->enabled = !runtimeFlatProfile();") != std::string::npos,
          "enabled is the VR profile and nothing else (flat never had it: its allow-list never listed the old key)");
    check(countOf(ex, "enabled = !runtimeFlatProfile()") == 1 && countOf(ex, "enabled = true") == 0 && countOf(ex, "enabled = !") == 1,
          "...and nothing else ever turns it on or flips it (no toggle)");
    check(find(ex, "copyMask") == std::string::npos && find(ex, "copyBtoA") == std::string::npos, "the old copy configuration is gone");
    check(find(ex, "share exposure: eye-1 chain skipped %llu dispatches (saved ~%.0f ms by the census), fallbacks %llu") != std::string::npos,
          "the 30 s line has the agreed words");

    check(find(ex, "toggleExposureFix") == std::string::npos && find(exh, "toggleExposureFix") == std::string::npos &&
              find(dh, "toggleExposureFix") == std::string::npos,
          "the toggle function is gone from the sources");
    check(find(dh, "toggle_exposure") == std::string::npos && find(dh, "toggleKey") == std::string::npos, "device_hook no longer registers a toggle key");
    check(find(ini, "toggle_exposure") == std::string::npos, "the ini template no longer has the key");
    check(find(ini, "Scroll Lock hotkey") == std::string::npos, "...nor the comment that promised the hotkey");
    check(find(ini, "share_exposure") == std::string::npos && find(ini, "Both eyes share one brightness") == std::string::npos,
          "the retired setting, its comment and its menu row are gone from the template");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || (std::strcmp(argv[1], "--self-test") && std::strcmp(argv[1], "--dry-run"))) {
        std::puts("usage: exposure_chain_test --self-test [repo root] | --dry-run");
        return 2;
    }
    if (!std::strcmp(argv[1], "--dry-run")) {
        std::puts("Would run the exposure engine path's decision tests, a WARP-device equivalence of three pipelines, and source pins; writes no files.");
        return 0;
    }
    const std::string root = argc > 2 ? argv[2] : ".";

    testLearningAndSkip();
    testDeclines();
    testFallbacksAndLatches();

    const auto createDevice = edvr::systemD3D11CreateDevice();
    check(createDevice != nullptr, "System32's D3D11CreateDevice");
    if (createDevice) {
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        D3D_FEATURE_LEVEL level{};
        HRESULT hr = createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx);
        if (FAILED(hr)) {
            std::puts("(the D3D debug layer is not installed here: running without it)");
            hr = createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx);
        }
        check(SUCCEEDED(hr), "a WARP device");
        check(edvr::reportSystemD3D11Only("exposure_chain_test"), "the rig runs on System32's d3d11.dll and on no other d3d11.dll");
        if (SUCCEEDED(hr)) {
            Shaders sh;
            sh.pre = compile(dev.Get(), kPreHlsl);
            sh.exp = compile(dev.Get(), kExpHlsl);
            sh.tone = compile(dev.Get(), kToneHlsl);
            check(sh.pre && sh.exp && sh.tone, "the chain's stand-in shaders compile");
            if (sh.pre && sh.exp && sh.tone) {
                const Schedule steady = {"steady, confirmed throughout", alwaysOn, alwaysOn};
                const Schedule lapse = {"the detection lapses for six frames and returns", alwaysOn, lapseConfirm};
                const Schedule off = {"the fix stands down for three frames (rejected) and returns", lapseActive, alwaysOn};
                runEquivalence(dev.Get(), ctx.Get(), sh, steady);
                runEquivalence(dev.Get(), ctx.Get(), sh, lapse);
                runEquivalence(dev.Get(), ctx.Get(), sh, off);
            }
            ComPtr<ID3D11InfoQueue> q;
            if (SUCCEEDED(dev.As(&q))) {
                const UINT64 n = q->GetNumStoredMessagesAllowedByRetrievalFilter();
                bool bad = false;
                for (UINT64 i = 0; i < n; ++i) {
                    SIZE_T len = 0;
                    q->GetMessage(i, nullptr, &len);
                    std::vector<char> raw(len);
                    auto* m = reinterpret_cast<D3D11_MESSAGE*>(raw.data());
                    if (SUCCEEDED(q->GetMessage(i, m, &len)) &&
                        (m->Severity == D3D11_MESSAGE_SEVERITY_ERROR || m->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION)) {
                        std::printf("  debug layer: %s\n", m->pDescription);
                        bad = true;
                    }
                }
                check(!bad, "the D3D debug layer stayed clean through all three schedules");
            }
        }
    }

    testSourcePins(root);

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

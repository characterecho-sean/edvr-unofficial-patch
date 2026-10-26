// main's depth tile (src/d3d11/temporal_shader_source.h, EDVR_MAIN_DEPTH_TILE): the native-TAA pass's history fetch
// read the 3x3 of zSceneAt around every pixel (and again for the instrument's two depth candidates); it now reads a
// 10x10 group tile filled once at the top of main, and only when the constants say the 3x3 can run. The claim, on
// WARP, bit for bit:
//
//   the production main (both embedded blobs, temporal_aa_cs and temporal_aa_fast_cs) and the same text compiled
//   with EDVR_MAIN_DEPTH_TILE 0 write the same bytes to every output (O, N, UN, Stats) for the same inputs.
//
// The reference is the untiled main as of the gate's first phase (ab78bbf4, the absent-layer zSceneAt), not
// origin/main's, which predates that zSceneAt: the tile was added wholly behind the macro, and fxc gives
// EDVR_MAIN_DEPTH_TILE 0 the bytecode of ab78bbf4's main byte for byte, both variants (compared by hand on
// 2026-10-23 with the fill's gate in; docs/edvr-gpu-trims-2026-10-14.md). The inputs: a region
// offset into its texture and not a multiple of 8 either way (partial groups, the clamped edges), a depth map of
// sky, far and near surfaces with a few signed zeros, denormals and NaNs, the smoke's and the UI depth bound or
// not, the screen map, the coverage mask and the previous UI colour; the configurations: depth bound or not, the
// head's depth term and the world path each on or off, six probe.w words, no history, split.x 0 (no world path
// whatever tvCam.w says), and the diagnostic variant's candidate mask (0, 2, 8, 5, 10: which of candidates 1 and 3, the two
// that fetch with depth, are asked, and the two that cannot read the tile).
// Non-vacuity: the world path ran where asked (Stats[15]), the depth changes the outputs, WARP agrees with
// itself, a mutant tile index (every tap the pixel's own entry) is caught, and only where the 3x3 runs. The fill's
// gate, over a tile poisoned first (withPoison, so an unfilled entry reads the same on every device): the production
// gate matches the reference in every configuration, both variants, and a mutant gate (the world split left out) is
// caught exactly where the world split alone runs the 3x3. The gate's other half, that it fills nothing where the 3x3
// cannot run, is not visible in the outputs, so withFillProbe counts the groups that wrote the tile: all of them where
// the constants let the 3x3 run (per variant: the head's term, the world split with split.x > 0, and in the diagnostic
// variant candidate 1 or 3), none where they do not.
// The fill's control flow in both shipping blobs' DXBC: the barrier at depth 0, the tile's stores inside a branch.
// By hand: ui_depth_test --main-tile [--adapter nvidia] [--bench] (the bench: temporal_aa_fast_cs at 2016x2016 against the
// untiled main, and on a history reset and with no depth against the ungated fill; zscene_gate_test.h's harness).
#pragma once
#include <thread>

namespace maintile {
using namespace zgate;

constexpr UINT kTexW = 48, kTexH = 40, kRegX = 5, kRegY = 3, kW = 37, kH = 29;

struct Rig {
    ComPtr<ID3D11Buffer> cb, stats, statsStage;
    ComPtr<ID3D11Texture2D> o, n, un;
    ComPtr<ID3D11ShaderResourceView> s, h, z, zp, um, zs, zu, up, screen;
    ComPtr<ID3D11UnorderedAccessView> oU, nU, unU, statsU;
    ComPtr<ID3D11SamplerState> samp;
};
uint32_t hash32(uint32_t x) { x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16; return x; }
float unit(uint32_t x) { return float(hash32(x) & 0xFFFFFFu) / float(0xFFFFFF); }

// splitX is split.x (the world/ship split's distance, 0 = no split, so the world path is off whatever tvCam.w says) and candMask the
// instrument's candidate mask (bit c: candidate c runs; 1 and 3 fetch with depth, so bits 1 and 3, mask & 10, read the 3x3). The
// diagnostic variant reads candMask; the fast variant has no candidates and ignores it.
struct Cfg { int knobsY, tvUsedW, tvCamW, haveHistory; uint32_t probeW; int splitX = 2, candMask = 15; };
std::string cfgText(const Cfg& c) {
    char b[200];
    std::snprintf(b, sizeof(b), "knobs.y %d, tvUsed.w %d, tvCam.w %d, haveHistory %d, probe.w %u, split.x %d, candMask %d", c.knobsY, c.tvUsedW, c.tvCamW,
                  c.haveHistory, c.probeW, c.splitX, c.candMask);
    return b;
}

std::vector<char> buildParams(ID3D11ShaderReflectionConstantBuffer* P, UINT size, const Cfg& c) {
    std::vector<char> cb(size, 0);
    auto put = [&](const char* name, const void* v, UINT bytes) {
        D3D11_SHADER_VARIABLE_DESC vd{};
        check(SUCCEEDED(P->GetVariableByName(name)->GetDesc(&vd)) && bytes <= vd.Size && vd.StartOffset + bytes <= cb.size(),
              "main depth tile: cbuffer P field");
        std::memcpy(cb.data() + vd.StartOffset, v, bytes);
    };
    auto f4 = [&](const char* name, float a, float b, float cc, float d) { const float v[4] = {a, b, cc, d}; put(name, v, 16); };
    auto rows = [&](const char* r0, const char* r1, const char* r2, float yaw, float pitch) {
        f4(r0, 1, 0, -yaw, 0); f4(r1, 0, 1, -pitch, 0); f4(r2, yaw, pitch, 1, 0);
    };
    const int region[4] = {int(kRegX), int(kRegY), int(kRegX + kW), int(kRegY + kH)}, sz[2] = {int(kW), int(kH)}, tex[2] = {int(kTexW), int(kTexH)};
    put("region", region, 16); put("size", sz, 8); put("texSize", tex, 8);
    f4("tanNow", -1.1f, 0.9f, -0.8f, 1.0f); f4("tanPrev", -1.12f, 0.88f, -0.79f, 1.01f); f4("jit", .31f, -.18f, 1, .75f);
    rows("dR0", "dR1", "dR2", .012f, -.007f);
    rows("c0R0", "c0R1", "c0R2", .010f, -.006f);
    rows("c1R0", "c1R1", "c1R2", .014f, -.008f);
    rows("c2R0", "c2R1", "c2R2", .031f, .011f);
    rows("c3R0", "c3R1", "c3R2", .012f, -.007f);
    const float blend = .9f, gamma = 1.25f;
    put("blend", &blend, 4); put("gamma", &gamma, 4); put("haveHistory", &c.haveHistory, 4); put("candMask", &c.candMask, 4);
    f4("knobs", .0001f, float(c.knobsY), .025f, 1000);
    f4("tvUsed", .012f, -.007f, .02f, float(c.tvUsedW)); f4("tvCand", -.03f, .002f, .01f, 0); f4("tvCam", .35f, -.12f, .9f, float(c.tvCamW));
    f4("split", float(c.splitX), 0, 0, 0);
    const bool coverage = (c.probeW & (4u | 128u)) != 0;
    f4("probe", 1, 1, coverage ? 1.0f : 0.0f, float(c.probeW));
    f4("holoJitter", .15f, -.1f, 1, 1);
    return cb;
}

// fills: the groups whose tile the fill wrote, counted by withFillProbe's text in Stats[kFillProbeSlot] and moved out of stats here so the
// A/B compares every other word (0 for a shader without the probe).
constexpr size_t kFillProbeSlot = 120;
struct Outputs { std::vector<uint32_t> o, n, un, stats; uint32_t fills = 0; };
bool same(const Outputs& a, const Outputs& b) { return a.o == b.o && a.n == b.n && a.un == b.un && a.stats == b.stats; }
std::string firstDiff(const Outputs& a, const Outputs& b) {
    const std::pair<const char*, std::pair<const std::vector<uint32_t>*, const std::vector<uint32_t>*>> outs[] = {
        {"O", {&a.o, &b.o}}, {"N", {&a.n, &b.n}}, {"UN", {&a.un, &b.un}}, {"Stats", {&a.stats, &b.stats}}};
    for (const auto& x : outs)
        for (size_t i = 0; i < x.second.first->size(); ++i)
            if ((*x.second.first)[i] != (*x.second.second)[i]) {
                char text[160];
                const size_t texel = std::strcmp(x.first, "Stats") ? i / 4 : i;
                std::snprintf(text, sizeof(text), "%s word %zu (texel %zu, x %zu y %zu): %08X vs %08X", x.first, i, texel, texel % kW, texel / kW,
                              (*x.second.first)[i], (*x.second.second)[i]);
                return text;
            }
    return "none";
}

Rig makeRig(ID3D11Device* dev, ID3D11DeviceContext* ctx, UINT cbSize) {
    Rig r;
    const size_t texN = size_t(kTexW) * kTexH, regN = size_t(kW) * kH;
    std::vector<float> s(texN * 4), h(regN * 4), up(regN * 4), um(regN), zp(regN), screen(texN * 4, 0.0f);
    std::vector<uint32_t> z(texN), zs(texN, 0u), zu(texN);
    for (size_t i = 0; i < texN; ++i) {
        for (int c = 0; c < 4; ++c) s[i * 4 + c] = unit(uint32_t(i * 4 + c) ^ 0x51u);
        const UINT x = UINT(i % kTexW), y = UINT(i / kTexW);
        const uint32_t k = hash32(uint32_t(i) ^ 0xA5A5u);
        float d;
        if (y < kTexH / 4 || (k & 7u) == 0) d = 0.0f;                                   // sky
        else if (x > kTexW * 2 / 3 && y > kTexH / 2) d = .025f / (.3f + 1.2f * unit(k));   // the hull, near
        else d = .025f / (5.0f + 4995.0f * unit(k + 1));                                  // a surface past the split
        std::memcpy(&z[i], &d, 4);
        if ((k & 0x3FFu) == 1) z[i] = 0x80000000u;   // -0
        if ((k & 0x3FFu) == 2) z[i] = 0x00000001u;   // a denormal
        if ((k & 0x3FFu) == 3) z[i] = 0x7FC00000u;   // NaN
        if (x > 10 && x < 20 && y > 12 && y < 22) { const float smoke = .025f / 3.0f; std::memcpy(&zs[i], &smoke, 4); }
        const float panel = .025f / .6f;
        if (y > 25 && y < 30 && x > 8 && x < 40) std::memcpy(&zu[i], &panel, 4); else zu[i] = z[i];
        if (x > 30 && x < 38 && y > 4 && y < 12) {   // the screen map: tracked, and source UI history (w 2) at its corner
            screen[i * 4] = .4f; screen[i * 4 + 1] = -.3f; screen[i * 4 + 2] = .025f / .9f; screen[i * 4 + 3] = (x > 35 && y > 9) ? 2.0f : 1.0f;
        }
    }
    for (size_t i = 0; i < regN; ++i) {
        for (int c = 0; c < 4; ++c) h[i * 4 + c] = unit(uint32_t(i * 4 + c) ^ 0x1234u);
        for (int c = 0; c < 3; ++c) up[i * 4 + c] = unit(uint32_t(i * 4 + c) ^ 0x777u);
        up[i * 4 + 3] = (hash32(uint32_t(i) ^ 0x99u) & 3u) == 0 ? 1.0f : 0.0f;
        um[i] = float(hash32(uint32_t(i) ^ 0x4242u) & 0xFFu) / 255.0f;
        zp[i] = .025f / (2.0f + 50.0f * unit(uint32_t(i) ^ 0x31u));
    }
    auto f32 = [&](UINT w, UINT hh, DXGI_FORMAT fmt, const void* data, UINT bpp) {
        return srvOf(dev, tex2d(dev, w, hh, fmt, D3D11_BIND_SHADER_RESOURCE, data, w * bpp).Get());
    };
    r.s = f32(kTexW, kTexH, DXGI_FORMAT_R32G32B32A32_FLOAT, s.data(), 16);
    r.h = f32(kW, kH, DXGI_FORMAT_R32G32B32A32_FLOAT, h.data(), 16);
    r.z = f32(kTexW, kTexH, DXGI_FORMAT_R32_FLOAT, z.data(), 4);
    r.zp = f32(kW, kH, DXGI_FORMAT_R32_FLOAT, zp.data(), 4);
    r.um = f32(kW, kH, DXGI_FORMAT_R32_FLOAT, um.data(), 4);
    r.zs = f32(kTexW, kTexH, DXGI_FORMAT_R32_FLOAT, zs.data(), 4);
    r.zu = f32(kTexW, kTexH, DXGI_FORMAT_R32_FLOAT, zu.data(), 4);
    r.up = f32(kW, kH, DXGI_FORMAT_R32G32B32A32_FLOAT, up.data(), 16);
    r.screen = f32(kTexW, kTexH, DXGI_FORMAT_R32G32B32A32_FLOAT, screen.data(), 16);
    r.o = tex2d(dev, kW, kH, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    r.n = tex2d(dev, kW, kH, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    r.un = tex2d(dev, kW, kH, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    r.oU = uavOf(dev, r.o.Get()); r.nU = uavOf(dev, r.n.Get()); r.unU = uavOf(dev, r.un.Get());
    D3D11_BUFFER_DESC sb{}; sb.ByteWidth = 128 * 4; sb.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    sb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; sb.StructureByteStride = 4;
    check(SUCCEEDED(dev->CreateBuffer(&sb, nullptr, &r.stats)), "main depth tile: stats buffer");
    r.statsU = uavOf(dev, r.stats.Get());
    sb.BindFlags = 0; sb.MiscFlags = 0; sb.StructureByteStride = 0; sb.Usage = D3D11_USAGE_STAGING; sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(SUCCEEDED(dev->CreateBuffer(&sb, nullptr, &r.statsStage)), "main depth tile: stats staging");
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = cbSize; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    check(SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &r.cb)), "main depth tile: constants");
    D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    check(SUCCEEDED(dev->CreateSamplerState(&sd, &r.samp)), "main depth tile: sampler");
    (void)ctx;
    return r;
}

Outputs run(ID3D11Device* dev, ID3D11DeviceContext* ctx, Rig& r, ID3D11ComputeShader* cs, const std::vector<char>& params, bool layers) {
    ctx->UpdateSubresource(r.cb.Get(), 0, nullptr, params.data(), 0, 0);
    const UINT sentinel[4] = {0x7F7F7F7Fu, 0x7F7F7F7Fu, 0x7F7F7F7Fu, 0x7F7F7F7Fu}, zero[4] = {};
    for (auto* u : {r.oU.Get(), r.nU.Get(), r.unU.Get()}) ctx->ClearUnorderedAccessViewUint(u, sentinel);
    ctx->ClearUnorderedAccessViewUint(r.statsU.Get(), zero);
    ctx->ClearState();
    ID3D11ShaderResourceView* srvs[15] = {r.s.Get(), r.h.Get(), r.z.Get(), r.zp.Get(), r.um.Get(), nullptr,
                                          layers ? r.zs.Get() : nullptr, layers ? r.zu.Get() : nullptr, r.up.Get(),
                                          nullptr, nullptr, nullptr, nullptr, nullptr, r.screen.Get()};
    ctx->CSSetShaderResources(0, 15, srvs);
    ID3D11UnorderedAccessView* uavs[7] = {r.oU.Get(), r.nU.Get(), r.statsU.Get(), nullptr, nullptr, nullptr, r.unU.Get()};
    ctx->CSSetUnorderedAccessViews(0, 7, uavs, nullptr);
    ctx->CSSetConstantBuffers(0, 1, r.cb.GetAddressOf());
    ctx->CSSetSamplers(0, 1, r.samp.GetAddressOf());
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->Dispatch((kW + 7) / 8, (kH + 7) / 8, 1);
    ctx->ClearState();
    Outputs out;
    out.o = readU32(dev, ctx, r.o.Get());
    out.n = readU32(dev, ctx, r.n.Get());
    out.un = readU32(dev, ctx, r.un.Get());
    ctx->CopyResource(r.statsStage.Get(), r.stats.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    check(SUCCEEDED(ctx->Map(r.statsStage.Get(), 0, D3D11_MAP_READ, 0, &m)), "main depth tile: stats map");
    out.stats.assign(static_cast<const uint32_t*>(m.pData), static_cast<const uint32_t*>(m.pData) + 128);
    ctx->Unmap(r.statsStage.Get(), 0);
    out.fills = out.stats[kFillProbeSlot];
    out.stats[kFillProbeSlot] = 0;
    return out;
}

// The production text with the tile fill's gate condition replaced (the mutant, and the bench's ungated fill).
std::string withGate(const std::string& hlsl, const char* condition) {
    const char* kGate = "[branch] if (haveHistory != 0 && knobs.y != 0.0 && depthTileRead) {";
    const size_t at = hlsl.find(kGate);
    check(at != std::string::npos && hlsl.find(kGate, at + 1) == std::string::npos, "main depth tile: the fill's gate found exactly once");
    return hlsl.substr(0, at) + "[branch] if (" + condition + ") {" + hlsl.substr(at + std::strlen(kGate));
}
// The tile poisoned before the gate (rig-only text): every entry FLT_MAX, the nearest depth there is, written by the
// thread that would fill it, so an entry the gate leaves unfilled reads as that on any device. Without it an unfilled
// entry reads whatever the last group on that memory left, and on the RTX that was the same tile from the dispatch
// before (identical inputs): the wrong gate passed there (2026-10-23).
std::string withPoison(const std::string& hlsl) {
    const char* kAt = "gMainDepthTileOrigin = int2(id.xy) - int2(gi & 7u, gi >> 3u);";
    const size_t at = hlsl.find(kAt);
    check(at != std::string::npos && hlsl.find(kAt, at + 1) == std::string::npos, "main depth tile: main's tile origin found exactly once");
    const size_t after = at + std::strlen(kAt);
    return hlsl.substr(0, after) + "\n    for (uint pk = gi; pk < 100u; pk += 64u) mainDepthTile[pk] = asfloat(0x7F7FFFFFu);" + hlsl.substr(after);
}
// Whether the fill ran (rig-only text, on a poisoned text): after main's barrier, thread 0 of each group adds one to Stats[kFillProbeSlot]
// when tile entry 0 is no longer the poison (the rig's depths are never FLT_MAX; a NaN entry counts as written). The outputs cannot say it: a gate that
// fills where nothing reads the tile, or leaves a poisoned entry the 3x3 never reads, writes the same bytes. So this is what pins the
// gate's other half, that it does NOT fill where the 3x3 cannot run (the fetches it exists to skip).
std::string withFillProbe(const std::string& hlsl) {
    const char* kAt = "uint count[48] = {";
    const size_t at = hlsl.find(kAt);
    check(at != std::string::npos && hlsl.find(kAt, at + 1) == std::string::npos, "main depth tile: main's count array found exactly once");
    return hlsl.substr(0, at) + "if (gi == 0u && asuint(mainDepthTile[0]) != 0x7F7FFFFFu) InterlockedAdd(Stats[" + std::to_string(kFillProbeSlot) +
           "], 1u);\n    " + hlsl.substr(at);
}
// The fill's control flow in a shipping blob's DXBC, main's top up to its first barrier: every store to the tile
// (the groupshared array of 100) inside a branch, and the barrier (sync_g_t) outside every branch and loop, where
// every thread meets it whatever the constants say.
void tileFlow(const void* code, size_t size, const char* name) {
    ComPtr<ID3DBlob> text;
    check(SUCCEEDED(D3DDisassemble(code, size, 0, nullptr, &text)), "main depth tile: the shipping blob disassembles");
    const std::string asmText(static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize());
    std::string tile;
    int ifDepth = 0, loopDepth = 0, stores = 0, storesInBranch = 0;
    bool barrier = false, barrierOutside = false;
    size_t line = 0;
    while (line < asmText.size()) {
        size_t end = asmText.find('\n', line);
        if (end == std::string::npos) end = asmText.size();
        std::string s = asmText.substr(line, end - line);
        line = end + 1;
        const size_t first = s.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        s.erase(0, first);
        if (s.rfind("dcl_tgsm_structured g", 0) == 0 && s.find(", 4, 100") != std::string::npos) {
            tile = s.substr(20, s.find(',') - 20);   // "gN"
        } else if (s.rfind("if_", 0) == 0) ++ifDepth;
        else if (s.rfind("endif", 0) == 0) --ifDepth;
        else if (s.rfind("loop", 0) == 0) ++loopDepth;
        else if (s.rfind("endloop", 0) == 0) --loopDepth;
        else if (s.rfind("sync_g", 0) == 0) { barrier = true; barrierOutside = ifDepth == 0 && loopDepth == 0; break; }
        else if (!tile.empty() && s.rfind("store_structured " + tile + ".", 0) == 0) { ++stores; if (ifDepth > 0) ++storesInBranch; }
    }
    char label[240];
    std::snprintf(label, sizeof(label), "main depth tile: %s's DXBC fills the tile (%d stores) only inside a branch (%d) and meets its barrier outside every branch and loop",
                  name, stores, storesInBranch);
    check(!tile.empty() && stores > 0 && storesInBranch == stores && barrier && barrierOutside, label);
}

// The A/B on the given device (WARP in the build). Returns the configurations compared.
int abTest(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    static const D3D_SHADER_MACRO refFast[] = {{"EDVR_TEMPORAL_DIAGNOSTICS", "0"}, {"EDVR_MAIN_DEPTH_TILE", "0"}, {nullptr, nullptr}};
    static const D3D_SHADER_MACRO refDiag[] = {{"EDVR_TEMPORAL_DIAGNOSTICS", "1"}, {"EDVR_MAIN_DEPTH_TILE", "0"}, {nullptr, nullptr}};
    static const D3D_SHADER_MACRO fast[] = {{"EDVR_TEMPORAL_DIAGNOSTICS", "0"}, {nullptr, nullptr}};
    static const D3D_SHADER_MACRO diag[] = {{"EDVR_TEMPORAL_DIAGNOSTICS", "1"}, {nullptr, nullptr}};
    const std::string hlsl = temporalHlsl(temporalSource());
    // The mutant: every tap of the tile read is the pixel's own entry (the 3x3 collapsed; it stays inside the tile).
    const char* kAnchor = "const int2 t = int2(p) - gMainDepthTileOrigin + int2(ox, oy) + 1;";
    const size_t at = hlsl.find(kAnchor);
    check(at != std::string::npos && hlsl.find(kAnchor, at + 1) == std::string::npos, "main depth tile: the tile index found exactly once");
    std::string mutant = hlsl;
    mutant.replace(at, std::strlen(kAnchor), "const int2 t = int2(p) - gMainDepthTileOrigin + int2(0, 0) + 1;");
    // The fill's gate, with the tile poisoned first (withPoison) and the fill counted (withFillProbe): the production gate must
    // still match the reference in every configuration, both variants (every entry the 3x3 reads was filled: the head's term,
    // the world split, and the instrument's depth candidates), fill exactly the groups of the configurations whose 3x3 can run
    // (no fill where nothing reads the tile: split.x 0, a candidate mask without candidate 1 or 3, no history, no depth), and
    // the second mutant, the gate without the world split, must not match where the world split alone runs the 3x3.
    const std::string poisoned = withFillProbe(withPoison(hlsl));
    const std::string gateMutant = withFillProbe(withPoison(withGate(hlsl, "haveHistory != 0 && knobs.y != 0.0 && tvUsed.w != 0.0")));
    // The six compiles together: main alone is ~20 s in fxc (production flags: cs_5_0, flags 0).
    ComPtr<ID3DBlob> refDiagCode, refFastCode, mutantCode, gateMutantCode, poisonDiagCode, poisonFastCode;
    {
        std::thread a([&] { refDiagCode = compileEntry(hlsl, "main", refDiag, 0); });
        std::thread b([&] { refFastCode = compileEntry(hlsl, "main", refFast, 0); });
        std::thread c([&] { mutantCode = compileEntry(mutant, "main", fast, 0); });
        std::thread g([&] { gateMutantCode = compileEntry(gateMutant, "main", fast, 0); });
        std::thread e([&] { poisonDiagCode = compileEntry(poisoned, "main", diag, 0); });
        std::thread f([&] { poisonFastCode = compileEntry(poisoned, "main", fast, 0); });
        a.join(); b.join(); c.join(); g.join(); e.join(); f.join();
    }
    tileFlow(edvr::kTemporalAaBytecode, sizeof(edvr::kTemporalAaBytecode), "temporal_aa_cs");
    tileFlow(edvr::kTemporalAaFastBytecode, sizeof(edvr::kTemporalAaFastBytecode), "temporal_aa_fast_cs");
    auto refDiagCs = makeCs(dev, refDiagCode->GetBufferPointer(), refDiagCode->GetBufferSize());
    auto refFastCs = makeCs(dev, refFastCode->GetBufferPointer(), refFastCode->GetBufferSize());
    auto mutantCs = makeCs(dev, mutantCode->GetBufferPointer(), mutantCode->GetBufferSize());
    auto gateMutantCs = makeCs(dev, gateMutantCode->GetBufferPointer(), gateMutantCode->GetBufferSize());
    auto poisonDiagCs = makeCs(dev, poisonDiagCode->GetBufferPointer(), poisonDiagCode->GetBufferSize());
    auto poisonFastCs = makeCs(dev, poisonFastCode->GetBufferPointer(), poisonFastCode->GetBufferSize());
    auto newDiagCs = makeCs(dev, edvr::kTemporalAaBytecode, sizeof(edvr::kTemporalAaBytecode));
    auto newFastCs = makeCs(dev, edvr::kTemporalAaFastBytecode, sizeof(edvr::kTemporalAaFastBytecode));
    check(refDiagCode->GetBufferSize() != sizeof(edvr::kTemporalAaBytecode) ||
              std::memcmp(refDiagCode->GetBufferPointer(), edvr::kTemporalAaBytecode, sizeof(edvr::kTemporalAaBytecode)) != 0,
          "main depth tile: the embedded temporal_aa_cs is not the untiled reference (the tile is in the shipping blob)");
    ComPtr<ID3D11ShaderReflection> refl;
    check(SUCCEEDED(D3DReflect(edvr::kTemporalAaBytecode, sizeof(edvr::kTemporalAaBytecode), __uuidof(ID3D11ShaderReflection), &refl)),
          "main depth tile: reflection");
    auto* P = refl->GetConstantBufferByName("P");
    D3D11_SHADER_BUFFER_DESC pd{};
    check(SUCCEEDED(P->GetDesc(&pd)), "main depth tile: cbuffer P");
    Rig r = makeRig(dev, ctx, pd.Size);

    std::vector<Cfg> cfgs;
    for (int ky : {1, 0}) for (int tu : {1, 0}) for (int tc : {1, 0})
        for (uint32_t pw : {0u, 1u, 32u, 128u, 6u, 64u | 256u}) cfgs.push_back({ky, tu, tc, 1, pw});
    cfgs.push_back({1, 1, 1, 0, 0u});
    cfgs.push_back({1, 1, 1, 0, 32u});
    // split.x 0: no world/ship split, so the world path is off whatever tvCam.w says. The head's depth term alone reads the 3x3 (tvUsed.w 1),
    // or nothing does, and a gate that wanted the split for the head's term would under-fill, one that ignored it over-fill.
    for (int ky : {1, 0}) for (int tu : {1, 0}) for (int tc : {1, 0})
        for (uint32_t pw : {0u, 128u}) cfgs.push_back({ky, tu, tc, 1, pw, 0});
    // candMask (the diagnostic variant; the fast one ignores it): candidates 1 and 3 fetch with depth (mask & 10), 0 and 2 do not. With no
    // head term and no world path the mask alone decides, so each of the two bits, both, neither, and the two that cannot read must show;
    // once with the split on and the world term off (tvCam.w 0), once with the world term set and the split off.
    for (int cm : {0, 2, 8, 5, 10}) {
        cfgs.push_back({1, 0, 0, 1, 0u, 2, cm});
        cfgs.push_back({1, 0, 1, 1, 0u, 0, cm});
    }
    cfgs.push_back({0, 0, 0, 1, 0u, 2, 10});   // candidates 1 and 3 asked, no depth bound: nothing to fetch
    cfgs.push_back({1, 0, 0, 0, 0u, 2, 10});   // candidates 1 and 3 asked, a history reset: no fetch
    const uint32_t groups = ((kW + 7) / 8) * ((kH + 7) / 8);   // one tile each
    int compared = 0, mutantCaught = 0, depthMatters = 0, gateMutantCaught = 0, unfilled = 0, unfilledDiag = 0;
    char label[320];
    std::vector<Outputs> byDepth[2];
    for (const Cfg& c : cfgs) {
        const bool layers = (c.probeW & (64u | 256u)) == 0;   // the bits say exactly what is bound
        const auto params = buildParams(P, pd.Size, c);
        const std::string where = cfgText(c);
        const Outputs nd = run(dev, ctx, r, newDiagCs.Get(), params, layers), nd2 = run(dev, ctx, r, newDiagCs.Get(), params, layers);
        const Outputs od = run(dev, ctx, r, refDiagCs.Get(), params, layers);
        const Outputs nf = run(dev, ctx, r, newFastCs.Get(), params, layers), of = run(dev, ctx, r, refFastCs.Get(), params, layers);
        const Outputs mf = run(dev, ctx, r, mutantCs.Get(), params, layers);
        const Outputs gf = run(dev, ctx, r, gateMutantCs.Get(), params, layers);
        const Outputs poisonD = run(dev, ctx, r, poisonDiagCs.Get(), params, layers), poisonF = run(dev, ctx, r, poisonFastCs.Get(), params, layers);
        std::snprintf(label, sizeof(label), "main depth tile [%s]: WARP is deterministic (one shader, one input, one result)", where.c_str());
        check(same(nd, nd2), label);
        if (!same(od, nd)) std::printf("main depth tile [%s] diagnostic: first difference %s\n", where.c_str(), firstDiff(od, nd).c_str());
        std::snprintf(label, sizeof(label), "main depth tile [%s]: temporal_aa_cs and the untiled reference write the same bytes to O, N, UN and Stats", where.c_str());
        check(same(od, nd), label);
        if (!same(of, nf)) std::printf("main depth tile [%s] fast: first difference %s\n", where.c_str(), firstDiff(of, nf).c_str());
        std::snprintf(label, sizeof(label), "main depth tile [%s]: temporal_aa_fast_cs and the untiled reference write the same bytes to O, N, UN and Stats", where.c_str());
        check(same(of, nf), label);
        bool written = true;
        for (uint32_t v : nf.n) if (v == 0x7F7F7F7Fu) written = false;
        std::snprintf(label, sizeof(label), "main depth tile [%s]: every pixel of N written", where.c_str());
        check(written, label);
        const bool world = c.knobsY && c.tvCamW && c.splitX > 0 && c.haveHistory;
        std::snprintf(label, sizeof(label), "main depth tile [%s]: the world path ran exactly where asked (Stats[15] = %u)", where.c_str(), nd.stats[15]);
        check((nd.stats[15] > 0) == world, label);
        // Where the 3x3 can run, from the constants alone: a history to blend and a depth bound, and the head's depth term or the world
        // split (the fast blob: the used fetch only), and in the diagnostic variant a depth candidate too (1 or 3).
        const bool runs3x3 = c.knobsY && (c.tvUsedW || (c.tvCamW && c.splitX > 0)) && c.haveHistory;
        const bool runs3x3Diag = c.knobsY && c.haveHistory && (c.tvUsedW || (c.tvCamW && c.splitX > 0) || (c.candMask & 10) != 0);
        if (!same(of, mf)) ++mutantCaught;
        std::snprintf(label, sizeof(label), "main depth tile [%s]: the mutant tile index is caught exactly where the 3x3 runs", where.c_str());
        check(runs3x3 == !same(of, mf), label);
        // The production gate over a poisoned tile: every entry the 3x3 reads was filled, both variants.
        std::snprintf(label, sizeof(label), "main depth tile [%s]: the production fill gate over a poisoned tile matches the untiled reference (fast)", where.c_str());
        check(same(of, poisonF), label);
        std::snprintf(label, sizeof(label), "main depth tile [%s]: the production fill gate over a poisoned tile matches the untiled reference (diagnostic)", where.c_str());
        check(same(od, poisonD), label);
        // ...and filled the tile in every group where the 3x3 can run and in none where it cannot (the outputs of the unfilled
        // configurations are the reference's without a fill; an over-fill would write the same bytes, so the probe is what says it).
        if (!runs3x3) ++unfilled;
        if (!runs3x3Diag) ++unfilledDiag;
        std::snprintf(label, sizeof(label), "main depth tile [%s]: the production fill gate (fast) wrote the tile in %u of %u groups, exactly where the 3x3 can run (expected %u)",
                      where.c_str(), poisonF.fills, groups, runs3x3 ? groups : 0u);
        check(poisonF.fills == (runs3x3 ? groups : 0u), label);
        std::snprintf(label, sizeof(label), "main depth tile [%s]: the production fill gate (diagnostic) wrote the tile in %u of %u groups, exactly where the 3x3 can run (expected %u)",
                      where.c_str(), poisonD.fills, groups, runs3x3Diag ? groups : 0u);
        check(poisonD.fills == (runs3x3Diag ? groups : 0u), label);
        // The wrong gate (poisoned) leaves the tile unfilled where the world split alone runs the 3x3, and only there.
        const bool gateMissed = runs3x3 && !(c.knobsY && c.tvUsedW && c.haveHistory);
        if (!same(of, gf)) ++gateMutantCaught;
        std::snprintf(label, sizeof(label), "main depth tile [%s]: the mutant fill gate (world split left out) is caught exactly where it leaves the 3x3 unfilled", where.c_str());
        check(gateMissed == !same(of, gf), label);
        if (c.haveHistory && c.tvUsedW && c.tvCamW && c.probeW == 0 && c.splitX == 2 && c.candMask == 15) byDepth[c.knobsY].push_back(nf);
        compared += 7;
    }
    if (!byDepth[0].empty() && !byDepth[1].empty() && !same(byDepth[0][0], byDepth[1][0])) ++depthMatters;
    check(depthMatters > 0, "main depth tile: the scene depth changes the outputs (the A/B sees the 3x3)");
    std::snprintf(label, sizeof(label), "main depth tile: the mutant tile index (the 3x3 collapsed) is caught (%d configurations)", mutantCaught);
    check(mutantCaught > 0, label);
    std::snprintf(label, sizeof(label), "main depth tile: the mutant fill gate is caught (%d configurations) and the unfilled configurations are compared (%d fast, %d diagnostic)",
                  gateMutantCaught, unfilled, unfilledDiag);
    check(gateMutantCaught > 0 && unfilled > 0 && unfilledDiag > 0, label);
    std::printf("PASS: main depth tile: temporal_aa_cs and temporal_aa_fast_cs byte-identical to the untiled main over %zu configurations "
                "(%d dispatch pairs; %ux%u at (%u,%u) in %ux%u; split.x 0 and candidate masks 0/2/8/5/10 among them); the production gate over a "
                "poisoned tile matched in all, both variants, and wrote the tile in exactly the groups where the 3x3 can run (%d configurations fast and "
                "%d diagnostic with the fill gated off); the mutant tile index caught in %d, the mutant fill gate in %d\n",
                cfgs.size(), compared, kW, kH, kRegX, kRegY, kTexW, kTexH, unfilled, unfilledDiag, mutantCaught, gateMutantCaught);
    return int(cfgs.size());
}

// One temporal entry's inputs at the eye size, its parameters written through cbuffer P's reflected layout.
struct BenchFrame {
    UINT w = 2016, h = 2016;
    ComPtr<ID3D11Buffer> cb, stats;
    ComPtr<ID3D11ShaderResourceView> s, hist, z, zp, zs, zu;
    ComPtr<ID3D11UnorderedAccessView> o, n, statsU, mv, zc, mk;
    std::vector<char> params;
    size_t probeW = 0, haveHistoryAt = 0, knobsYAt = 0;   // byte offsets in params (the reset and no-depth bench cases)
};
BenchFrame makeFrame(ID3D11Device* dev, const void* code, size_t size) {
    BenchFrame f;
    const UINT w = f.w, h = f.h;
    ComPtr<ID3D11ShaderReflection> refl;
    check(SUCCEEDED(D3DReflect(code, size, __uuidof(ID3D11ShaderReflection), &refl)), "zscene gate: reflection");
    auto* P = refl->GetConstantBufferByName("P");
    D3D11_SHADER_BUFFER_DESC pd{};
    check(SUCCEEDED(P->GetDesc(&pd)), "zscene gate: cbuffer P");
    f.params.assign(pd.Size, 0);
    auto put = [&](const char* name, const void* v, UINT bytes) {
        D3D11_SHADER_VARIABLE_DESC vd{};
        check(SUCCEEDED(P->GetVariableByName(name)->GetDesc(&vd)) && bytes <= vd.Size && vd.StartOffset + bytes <= f.params.size(),
              "zscene gate: cbuffer P field");
        std::memcpy(f.params.data() + vd.StartOffset, v, bytes);
        if (!std::strcmp(name, "probe")) f.probeW = vd.StartOffset + 12;
        if (!std::strcmp(name, "haveHistory")) f.haveHistoryAt = vd.StartOffset;
        if (!std::strcmp(name, "knobs")) f.knobsYAt = vd.StartOffset + 4;
    };
    auto f4 = [&](const char* name, float a, float b, float c, float d) { const float v[4] = {a, b, c, d}; put(name, v, 16); };
    const int region[4] = {0, 0, int(w), int(h)}, sz[2] = {int(w), int(h)}, one = 1, none = 0;
    put("region", region, 16); put("size", sz, 8); put("texSize", sz, 8);
    f4("tanNow", -1, 1, -1, 1); f4("tanPrev", -1.002f, 0.998f, -1, 1); f4("jit", .25f, -.375f, 0, .5f);
    f4("dR0", 1, 0, -.002f, 0); f4("dR1", 0, 1, 0, 0); f4("dR2", .002f, 0, 1, 0);
    f4("c2R0", 1, 0, -.003f, 0); f4("c2R1", 0, 1, 0, 0); f4("c2R2", .003f, 0, 1, 0);
    const float blend = .9f, gamma = 1.25f;
    put("blend", &blend, 4); put("gamma", &gamma, 4); put("haveHistory", &one, 4); put("candMask", &none, 4);
    f4("knobs", 0, 1, .025f, 0); f4("tvUsed", .001f, 0, 0, 1); f4("tvCam", .01f, 0, 0, 1); f4("split", 2, 0, 0, 0);
    f4("probe", 1, 0, 0, 0); f4("holoJitter", .25f, -.375f, 1, 1);
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = pd.Size; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    check(SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &f.cb)), "zscene gate: bench constants");
    // A scene: a third sky, the rest a slanted surface 0.5-20 m away; the smoke's depth a patch; the UI depth
    // the scene with a nearer panel stamped in it (what uiDepthTemporalDepth publishes).
    std::vector<uint32_t> rgba(size_t(w) * h);
    std::vector<float> z(size_t(w) * h), smoke(size_t(w) * h, 0.0f), ui(size_t(w) * h);
    uint32_t seed = 0x9E3779B9u;
    for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) {
        const size_t i = size_t(y) * w + x;
        seed = seed * 1664525u + 1013904223u;
        rgba[i] = seed | 0xFF000000u;
        z[i] = y < h / 3 ? 0.0f : .025f / (.5f + 19.5f * float((x + y) % 997) / 997.0f);
        if (x > w / 3 && x < w / 2 && y > h / 2) smoke[i] = .025f / 3.0f;
        ui[i] = (x > w / 4 && x < 3 * w / 4 && y > 3 * h / 4) ? .025f / .6f : z[i];
    }
    auto sT = tex2d(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, rgba.data(), w * 4);
    auto hT = tex2d(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
    auto zT = tex2d(dev, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, z.data(), w * 4);
    auto zpT = tex2d(dev, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, z.data(), w * 4);
    auto zsT = tex2d(dev, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, smoke.data(), w * 4);
    auto zuT = tex2d(dev, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, ui.data(), w * 4);
    f.s = srvOf(dev, sT.Get()); f.hist = srvOf(dev, hT.Get()); f.z = srvOf(dev, zT.Get()); f.zp = srvOf(dev, zpT.Get());
    f.zs = srvOf(dev, zsT.Get()); f.zu = srvOf(dev, zuT.Get());
    f.o = uavOf(dev, tex2d(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0).Get());
    f.n = uavOf(dev, tex2d(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0).Get());
    f.mv = uavOf(dev, tex2d(dev, w, h, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0).Get());
    f.zc = uavOf(dev, tex2d(dev, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0).Get());
    f.mk = uavOf(dev, tex2d(dev, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0).Get());
    D3D11_BUFFER_DESC sb{}; sb.ByteWidth = 128 * 4; sb.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    sb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; sb.StructureByteStride = 4;
    check(SUCCEEDED(dev->CreateBuffer(&sb, nullptr, &f.stats)), "zscene gate: stats buffer");
    f.statsU = uavOf(dev, f.stats.Get());
    return f;
}
// One bench case: a shader, whether it is the mv entry (else main), which layers are bound and the probe.w bits.
struct BenchCase { const char* name; ID3D11ComputeShader* cs; bool mvEntry; bool t6, t7; uint32_t bits; };
void bindCase(ID3D11DeviceContext* ctx, BenchFrame& f, const BenchCase& c) {
    ctx->ClearState();
    float w; std::memcpy(&w, f.params.data() + f.probeW, 4);
    const float bits = static_cast<float>(c.bits);
    std::memcpy(f.params.data() + f.probeW, &bits, 4);
    ctx->UpdateSubresource(f.cb.Get(), 0, nullptr, f.params.data(), 0, 0);
    std::memcpy(f.params.data() + f.probeW, &w, 4);
    ID3D11ShaderResourceView* srvs[8] = {f.s.Get(), c.mvEntry ? nullptr : f.hist.Get(), f.z.Get(), c.mvEntry ? f.zp.Get() : nullptr,
                                         nullptr, nullptr, c.t6 ? f.zs.Get() : nullptr, c.t7 ? f.zu.Get() : nullptr};
    ctx->CSSetShaderResources(0, 8, srvs);
    ID3D11UnorderedAccessView* uavs[6] = {c.mvEntry ? nullptr : f.o.Get(), c.mvEntry ? nullptr : f.n.Get(), f.statsU.Get(),
                                          c.mvEntry ? f.mv.Get() : nullptr, c.mvEntry ? f.zc.Get() : nullptr, c.mvEntry ? f.mk.Get() : nullptr};
    ctx->CSSetUnorderedAccessViews(0, 6, uavs, nullptr);
    ctx->CSSetConstantBuffers(0, 1, f.cb.GetAddressOf());
    ctx->CSSetShader(c.cs, nullptr, 0);
}
// --bench: the native-TAA pass at the eye size, the production fast main against the untiled one; then, on a history
// reset and with no depth bound (the 3x3 never runs), against the fill ungated (42bf0a99's main) and the untiled one.
void bench(Device& d) {
    static const D3D_SHADER_MACRO refFast[] = {{"EDVR_TEMPORAL_DIAGNOSTICS", "0"}, {"EDVR_MAIN_DEPTH_TILE", "0"}, {nullptr, nullptr}};
    static const D3D_SHADER_MACRO fast[] = {{"EDVR_TEMPORAL_DIAGNOSTICS", "0"}, {nullptr, nullptr}};
    const std::string hlsl = temporalHlsl(temporalSource());
    const std::string ungated = withGate(hlsl, "true");
    ComPtr<ID3DBlob> refCode, ungatedCode;
    {
        std::thread a([&] { refCode = compileEntry(hlsl, "main", refFast, 0); });
        std::thread b([&] { ungatedCode = compileEntry(ungated, "main", fast, 0); });
        a.join(); b.join();
    }
    auto prodCs = makeCs(d.dev.Get(), edvr::kTemporalAaFastBytecode, sizeof(edvr::kTemporalAaFastBytecode));
    auto refCs = makeCs(d.dev.Get(), refCode->GetBufferPointer(), refCode->GetBufferSize());
    auto ungatedCs = makeCs(d.dev.Get(), ungatedCode->GetBufferPointer(), ungatedCode->GetBufferSize());
    // The frame's constants come from the diagnostic blob's reflection, which keeps the whole of cbuffer P.
    BenchFrame f = makeFrame(d.dev.Get(), edvr::kTemporalAaBytecode, sizeof(edvr::kTemporalAaBytecode));
    ID3D11DeviceContext* ctx = d.ctx.Get();
    auto arm = [&](const BenchCase& c) { return std::function<void()>([ctx, &f, c] { bindCase(ctx, f, c); }); };
    const UINT gx = (f.w + 7) / 8, gy = (f.h + 7) / 8;
    std::printf("== temporal_aa_fast_cs on %s, %ux%u one eye, depth bound, the head's depth term and the world path on; paired "
                "ABBA/BAAB, %u rounds of 4 blocks of %u; ms per dispatch, untiled -> tiled, median paired saving\n",
                d.name.c_str(), f.w, f.h, zgate::kPairedRounds, zgate::kPairedDispatches);
    printPaired("t6/t7 bound", pairedRounds(d, arm({"", refCs.Get(), false, true, true, 0u}), arm({"", prodCs.Get(), false, true, true, 0u}), gx, gy));
    printPaired("t6/t7 null, bits 64|256",
                pairedRounds(d, arm({"", refCs.Get(), false, false, false, 320u}), arm({"", prodCs.Get(), false, false, false, 320u}), gx, gy));
    // The cases where the 3x3 never runs (t6/t7 bound): the fill ungated -> gated, and untiled -> gated.
    const int noHistory = 0, history = 1;
    const float depthOff = 0.0f, depthOn = 1.0f;
    std::puts("  the 3x3 not run (t6/t7 bound): ungated fill or untiled -> the gated production main");
    std::memcpy(f.params.data() + f.haveHistoryAt, &noHistory, 4);
    printPaired("history reset: ungated -> gated",
                pairedRounds(d, arm({"", ungatedCs.Get(), false, true, true, 0u}), arm({"", prodCs.Get(), false, true, true, 0u}), gx, gy));
    printPaired("history reset: untiled -> gated",
                pairedRounds(d, arm({"", refCs.Get(), false, true, true, 0u}), arm({"", prodCs.Get(), false, true, true, 0u}), gx, gy));
    std::memcpy(f.params.data() + f.haveHistoryAt, &history, 4);
    std::memcpy(f.params.data() + f.knobsYAt, &depthOff, 4);
    printPaired("no depth (knobs.y 0): ungated -> gated",
                pairedRounds(d, arm({"", ungatedCs.Get(), false, true, true, 0u}), arm({"", prodCs.Get(), false, true, true, 0u}), gx, gy));
    printPaired("no depth (knobs.y 0): untiled -> gated",
                pairedRounds(d, arm({"", refCs.Get(), false, true, true, 0u}), arm({"", prodCs.Get(), false, true, true, 0u}), gx, gy));
    std::memcpy(f.params.data() + f.knobsYAt, &depthOn, 4);
    if (d.warp) std::puts("note: --bench on WARP times a CPU; use --adapter nvidia.");
}

// ui_depth_test --main-tile [--adapter nvidia] [--bench]: this alone, on the adapter asked for.
int runAlone(int argc, char** argv) {
    bool nvidia = false, wantBench = false;
    if (!aloneFlags(argc, argv, nvidia, wantBench)) { std::puts("usage: ui_depth_test --main-tile [--adapter nvidia] [--bench]"); return 2; }
    Device d = makeDevice(nvidia);
    abTest(d.dev.Get(), d.ctx.Get());
    if (wantBench) maintile::bench(d);
    return 0;
}

}  // namespace maintile

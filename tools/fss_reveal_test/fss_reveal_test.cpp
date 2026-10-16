// Test rig for the FSS reveal fix (src/d3d11/fss_reveal_core.h, fss_reveal_replay.h, fss_reveal.cpp).
//
//   1. The pure half: the decisions. The stock order replays; a hash-only match on another target is declined; there is no replay without a downsample recorded earlier in the same frame; an unresolved
//      t1 matches nothing; flat never replays; only the first composite of a frame decides; and the scanner-session verdict tells "the hook never ran", "nothing to do", "never fired" and "fired" apart.
//   2. The mechanism, on a WARP device with the debug layer: the real captureState and replayDraw. The replay draws the downsample's colour from a private copy of its constant buffer after the
//      original was rewritten, the composite's own target is untouched, and every stage the draw touches reads back equal to what it was.
//   3. Source pins on the hook glue no rig can run (it is linked into the DLL only).
//
// No window, no files written. --self-test runs it; --dry-run says what it would do.
#include <windows.h>
#include <d3d11_1.h>
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
#include "../../src/d3d11/gpu_census.h"
#include "../../src/d3d11/fss_reveal_core.h"
#include "../../src/d3d11/fss_reveal_replay.h"

using Microsoft::WRL::ComPtr;
namespace fr = edvr::fssreveal;

// The census's three entry points, counted instead of timed (the real ones are gpu_census.cpp's, which this rig does not link). The replay header opens its cost
// scope through them, so what the rig counts here is what the 30 s census would be called with.
namespace edvr {
unsigned g_censusBegins[static_cast<size_t>(GpuCensusSection::Count)] = {};
unsigned g_censusEnds[static_cast<size_t>(GpuCensusSection::Count)] = {};
unsigned g_censusIssued[static_cast<size_t>(GpuCensusSection::Count)] = {};
bool gpuCensusBegin(ID3D11DeviceContext*, GpuCensusSection section) noexcept { ++g_censusBegins[static_cast<size_t>(section)]; return false; }
void gpuCensusEnd(ID3D11DeviceContext*, GpuCensusSection section) noexcept { ++g_censusEnds[static_cast<size_t>(section)]; }
void gpuCensusNoteIssued(GpuCensusSection section) noexcept { ++g_censusIssued[static_cast<size_t>(section)]; }
}  // namespace edvr

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

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
};

void testDecisions() {
    check(fr::kCompVs == 0x953C8123AD8DC13Bull && fr::kCompPs == 0xA473A867BDDBC342ull && fr::kDownVs == 0xDEF19B035D5EDEDCull && fr::kDownPs == 0x831DF02EBA8AE814ull,
          "the reveal composite and the downsample are named by the whole 64-bit hashes of both shaders");
    int T121 = 0, other = 0, layer = 0;
    int idx = -1;
    // the stock order of flight d7769063: the first downsample (#216) is drawn before the left composite (#219), the second (#252) before the right one (#253)
    {
        fr::RevealReplay r;
        const int rec = r.record(&T121);
        check(rec == 0, "the first downsample of the frame is recorded");
        check(r.composite(false, &T121, &idx) == fr::RevealReplay::Replay && idx == 0, "the first composite replays the downsample recorded into its own t1");
        r.record(&T121);   // the second downsample, drawn by the game as it always did
        check(r.composite(false, &T121, &idx) == fr::RevealReplay::DeclDone, "the second composite of the frame decides nothing");
        r.endFrame();
        check(r.frames == 1 && r.framesWithComposite == 1 && r.framesReplayed == 1 && r.framesDeclined[fr::RevealReplay::DeclNoRecord] == 0 && r.records == 2, "the frame is tallied as replayed");
    }
    // a hash-only match on another target is declined: the downsample shaders also make generic copies
    {
        fr::RevealReplay r;
        r.record(&other);
        const fr::RevealReplay::Decision d = r.composite(false, &T121, &idx);
        r.endFrame();
        check(d == fr::RevealReplay::DeclOtherTarget && r.framesReplayed == 0 && r.framesDeclined[fr::RevealReplay::DeclOtherTarget] == 1, "a downsample into another texture than the composite's t1 is declined");
    }
    // there is no replay without a recorded downsample that frame
    {
        fr::RevealReplay r;
        check(r.composite(false, &T121, &idx) == fr::RevealReplay::DeclNoRecord, "no replay without a recorded downsample");
        r.record(&T121);   // recorded AFTER the composite: too late for this frame
        check(r.composite(false, &T121, &idx) == fr::RevealReplay::DeclDone, "a downsample after the composite is not replayed for it");
        r.endFrame();
        check(r.framesReplayed == 0 && r.framesDeclined[fr::RevealReplay::DeclNoRecord] == 1 && r.replays == 0, "the frame counts as declined, not replayed");
        // the next frame starts clean: last frame's record is gone
        check(r.composite(false, &T121, &idx) == fr::RevealReplay::DeclNoRecord, "a record does not carry into the next frame");
    }
    // a composite whose t1 did not resolve
    {
        fr::RevealReplay r;
        r.record(&T121);
        check(r.composite(false, nullptr, &idx) == fr::RevealReplay::DeclT1 && r.t1Unresolved == 1, "an unresolved t1 matches nothing");
    }
    // flat never replays
    {
        fr::RevealReplay r;
        r.record(&T121);
        check(r.composite(true, &T121, &idx) == fr::RevealReplay::DeclFlat, "flat never replays, even with a matching record");
        r.endFrame();
        check(r.framesReplayed == 0 && r.framesDeclined[fr::RevealReplay::DeclFlat] == 1, "and it is tallied");
    }
    // a record whose state could not be kept is never replayed; the table is bounded
    {
        fr::RevealReplay r;
        const int a = r.record(&T121);
        r.recordFailed(a);
        check(r.composite(false, &T121, &idx) == fr::RevealReplay::DeclOtherTarget && r.recordFailures == 1, "a record that failed is not replayed");
        r.endFrame();
        for (int i = 0; i < 4; ++i) r.record(&layer);
        check(r.record(&layer) == -1 && r.recordDrops == 1, "the frame's table is bounded");
        r.resetTotals();
        check(r.records == 0 && r.recordDrops == 0 && r.frames == 0, "the totals reset per capture");
    }
    // the first composite wins: a later composite of the frame with another t1 changes nothing
    {
        fr::RevealReplay r;
        r.record(&T121);
        r.composite(false, &T121, &idx);
        check(r.composite(false, &other, &idx) == fr::RevealReplay::DeclDone, "only the first composite decides");
    }
}

void testReplayOnWarp(Gpu& g) {
    ID3D11Device* d = g.dev.Get();
    ComPtr<ID3D11DeviceContext1> c1;
    g.ctx.As(&c1);
    const char* vsSrc = "float4 main(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2); return float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); }";
    const char* psSrc = "cbuffer C2 : register(b2) { float4 col; } Texture2D t0 : register(t0); float4 main(float4 p : SV_Position) : SV_Target { return col + t0.Load(int3(0, 0, 0)) * 0.0; }";
    ComPtr<ID3DBlob> vsb, psb, err;
    check(SUCCEEDED(D3DCompile(vsSrc, std::strlen(vsSrc), "v", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsb, &err)), "the test vertex shader compiles");
    check(SUCCEEDED(D3DCompile(psSrc, std::strlen(psSrc), "p", nullptr, nullptr, "main", "ps_5_0", 0, 0, &psb, &err)), "the test pixel shader compiles");
    if (!vsb || !psb) return;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    d->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    d->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
    auto makeTex = [&](uint32_t w, UINT bind, ComPtr<ID3D11Texture2D>& t) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = w; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.BindFlags = bind;
        return SUCCEEDED(d->CreateTexture2D(&td, nullptr, &t));
    };
    ComPtr<ID3D11Texture2D> texT, texC, texL;
    check(makeTex(8, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, texT) && makeTex(8, D3D11_BIND_RENDER_TARGET, texC) && makeTex(4, D3D11_BIND_SHADER_RESOURCE, texL), "the replay test textures");
    ComPtr<ID3D11RenderTargetView> rtvT, rtvC;
    ComPtr<ID3D11ShaderResourceView> srvT, srvL;
    d->CreateRenderTargetView(texT.Get(), nullptr, &rtvT);
    d->CreateRenderTargetView(texC.Get(), nullptr, &rtvC);
    d->CreateShaderResourceView(texT.Get(), nullptr, &srvT);
    d->CreateShaderResourceView(texL.Get(), nullptr, &srvL);
    // two constant buffers: B (the downsample's, red) and A (the composite's, blue)
    const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1}, blue[4] = {0, 0, 1, 1};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA sr{red, 0, 0}, sb{blue, 0, 0};
    ComPtr<ID3D11Buffer> bufB, bufA;
    d->CreateBuffer(&bd, &sr, &bufB);
    d->CreateBuffer(&bd, &sb, &bufA);
    // two of every state object that differs between the two draws
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> rsB, rsA;
    d->CreateRasterizerState(&rd, &rsB);
    rd.ScissorEnable = TRUE;
    d->CreateRasterizerState(&rd, &rsA);
    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    bld.RenderTarget[0].SrcBlend = bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlend = bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bld.RenderTarget[0].BlendOp = bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    ComPtr<ID3D11BlendState> bsB, bsA;
    d->CreateBlendState(&bld, &bsB);
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    d->CreateBlendState(&bld, &bsA);
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = FALSE; dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; dsd.DepthFunc = D3D11_COMPARISON_LESS; dsd.StencilEnable = FALSE;
    dsd.StencilReadMask = 0xFF; dsd.StencilWriteMask = 0xFF;
    dsd.FrontFace.StencilFailOp = dsd.FrontFace.StencilDepthFailOp = dsd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP; dsd.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    dsd.BackFace = dsd.FrontFace;
    ComPtr<ID3D11DepthStencilState> dssB, dssA;
    d->CreateDepthStencilState(&dsd, &dssB);
    dsd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL; dsd.StencilReadMask = 1;
    d->CreateDepthStencilState(&dsd, &dssA);
    D3D11_SAMPLER_DESC smd{};
    smd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; smd.ComparisonFunc = D3D11_COMPARISON_NEVER; smd.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> smpB, smpA;
    d->CreateSamplerState(&smd, &smpB);
    smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    d->CreateSamplerState(&smd, &smpA);
    const float zero[4] = {0, 0, 0, 0};
    const D3D11_VIEWPORT vp8{0, 0, 8, 8, 0, 1}, vpA{1, 1, 6, 6, 0, 1};
    const D3D11_RECT scA{1, 1, 7, 7};

    // the downsample's state (B): it draws into T from L with the red constant
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(ps.Get(), nullptr, 0);
    ID3D11Buffer* cb = bufB.Get();
    g.ctx->PSSetConstantBuffers(2, 1, &cb);
    ID3D11ShaderResourceView* sv = srvL.Get();
    g.ctx->PSSetShaderResources(0, 1, &sv);
    ID3D11RenderTargetView* rt = rtvT.Get();
    g.ctx->OMSetRenderTargets(1, &rt, nullptr);
    g.ctx->RSSetViewports(1, &vp8);
    g.ctx->RSSetState(rsB.Get());
    g.ctx->OMSetBlendState(bsB.Get(), nullptr, 0xFFFFFFFFu);
    g.ctx->OMSetDepthStencilState(dssB.Get(), 5);
    ID3D11SamplerState* ss = smpB.Get();
    g.ctx->PSSetSamplers(0, 1, &ss);
    fr::DrawState rec;
    fr::CbPool pool;
    // The GPU census sees the fix's own GPU work (review of main 9fe5b257, finding 5): the private constant-buffer copies of a recorded downsample are one entry, a replay is one.
    constexpr size_t kFss = static_cast<size_t>(edvr::GpuCensusSection::FrameFssReveal);
    {
        // a frame in which the fix has nothing to do: no downsample recorded, no composite replayed -- the hook issues no GPU work, so the census is called for nothing
        check(edvr::g_censusBegins[kFss] == 0 && edvr::g_censusEnds[kFss] == 0, "a frame with nothing recorded and nothing replayed produces no cost entry");
        // reading the state without a pool (the replay's own before/after read-backs, the rig's checks) is CPU work: no entry
        fr::DrawState peek;
        fr::captureState(d, g.ctx.Get(), c1.Get(), nullptr, peek);
        peek.release();
        check(edvr::g_censusBegins[kFss] == 0, "reading the state back without a constant-buffer pool is not timed");
    }
    const unsigned beginsBeforeCapture = edvr::g_censusBegins[kFss];
    const bool captured = fr::captureState(d, g.ctx.Get(), c1.Get(), &pool, rec);
    check(edvr::g_censusBegins[kFss] == beginsBeforeCapture + 1 && edvr::g_censusEnds[kFss] == edvr::g_censusBegins[kFss] && edvr::g_censusIssued[kFss] == 1,
          "recording a downsample with its constant buffers copied is one cost entry (opened and closed, counted as issued)");
    check(captured && rec.valid && rec.psCb[2] != nullptr && rec.psCb[2] != bufB.Get() && rec.rtv[0] == rtvT.Get() && rec.ps == ps.Get(),
          "the downsample's state is captured with its constant buffer replaced by a private copy");
    rec.perInstance = 3;
    rec.instances = 1;
    // the game moves on: T is cleared, the downsample's constant buffer is rewritten (green), and the composite's state (A) is bound, with T as its input t1
    g.ctx->ClearRenderTargetView(rtvT.Get(), zero);
    g.ctx->UpdateSubresource(bufB.Get(), 0, nullptr, green, 0, 0);
    cb = bufA.Get();
    g.ctx->PSSetConstantBuffers(2, 1, &cb);
    rt = rtvC.Get();
    g.ctx->OMSetRenderTargets(1, &rt, nullptr);   // T stops being an output first: binding it as an input while it is still a render target would unbind it
    ID3D11ShaderResourceView* svs[2] = {srvL.Get(), srvT.Get()};
    g.ctx->PSSetShaderResources(0, 2, svs);
    g.ctx->ClearRenderTargetView(rtvC.Get(), zero);
    g.ctx->RSSetViewports(1, &vpA);
    g.ctx->RSSetScissorRects(1, &scA);
    g.ctx->RSSetState(rsA.Get());
    const float bfA[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    g.ctx->OMSetBlendState(bsA.Get(), bfA, 0xFFFFFFF0u);
    g.ctx->OMSetDepthStencilState(dssA.Get(), 3);
    ss = smpA.Get();
    g.ctx->PSSetSamplers(0, 1, &ss);
    fr::DrawState a1;
    fr::captureState(d, g.ctx.Get(), c1.Get(), nullptr, a1);
    bool verified = false;
    fr::ReplayCalls noCalls;
    const unsigned beginsBeforeReplay = edvr::g_censusBegins[kFss];
    const bool done = fr::replayDraw(d, g.ctx.Get(), c1.Get(), noCalls, rec, &verified);
    check(done && edvr::g_censusBegins[kFss] == beginsBeforeReplay + 1 && edvr::g_censusEnds[kFss] == edvr::g_censusBegins[kFss] && edvr::g_censusIssued[kFss] == 2,
          "a replay is exactly one cost entry: its read-backs add none, and it is counted as issued");
    fr::DrawState a2;
    fr::captureState(d, g.ctx.Get(), c1.Get(), nullptr, a2);
    check(done && verified && fr::sameState(a1, a2), "the state after the replay equals the state before it (read back through the API, every stage the draw touches)");
    check(a2.rtv[0] == rtvC.Get() && a2.psSrv[1] == srvT.Get() && a2.psSrv[0] == srvL.Get(), "the composite's own target and its inputs (t1 is the texture the replay drew into) are back");
    check(a2.psCb[2] == bufA.Get() && a2.rs == rsA.Get() && a2.bs == bsA.Get() && a2.dss == dssA.Get() && a2.psSmp[0] == smpA.Get() && a2.ps == ps.Get(), "its constants, rasterizer, blend, depth-stencil, sampler and shader are back");
    check(a2.stencilRef == 3 && a2.sampleMask == 0xFFFFFFF0u && a2.bf[1] == 0.5f, "its stencil reference, sample mask and blend factor are back");
    check(a2.nvp >= 1 && a2.vp[0].TopLeftX == 1.0f && a2.vp[0].Width == 6.0f && a2.nsc >= 1 && a2.sc[0].right == 7, "its viewport and scissor are back");
    // what the replay drew: T holds the downsample's red (the copied constant, not the rewritten green); the composite's target is untouched
    auto readPixel = [&](ID3D11Texture2D* tex, uint8_t* out) {
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> st;
        if (FAILED(d->CreateTexture2D(&td, nullptr, &st))) return false;
        g.ctx->CopyResource(st.Get(), tex);
        g.ctx->Flush();
        D3D11_MAPPED_SUBRESOURCE m{};
        for (int tries = 0; tries < 200; ++tries) {
            if (SUCCEEDED(g.ctx->Map(st.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
                std::memcpy(out, m.pData, 4);
                g.ctx->Unmap(st.Get(), 0);
                return true;
            }
            Sleep(5);
        }
        return false;
    };
    uint8_t pt[4] = {}, pc[4] = {};
    const bool rt1 = readPixel(texT.Get(), pt), rt2 = readPixel(texC.Get(), pc);
    check(rt1 && pt[0] > 250 && pt[1] == 0 && pt[2] == 0, "the replay drew the downsample's red from its private copy of the constant buffer, not the green it was rewritten to");
    check(rt2 && pc[0] == 0 && pc[1] == 0 && pc[2] == 0 && pc[3] == 0, "the composite's render target was not drawn into");
    // a second replay from the same record is as clean
    verified = false;
    check(fr::replayDraw(d, g.ctx.Get(), c1.Get(), noCalls, rec, &verified) && verified, "a second replay from the same record also restores the state exactly");
    // an invalid record draws nothing
    fr::DrawState empty;
    const unsigned beginsBeforeEmpty = edvr::g_censusBegins[kFss];
    check(!fr::replayDraw(d, g.ctx.Get(), c1.Get(), noCalls, empty, nullptr), "a record with no state is not replayed");
    check(edvr::g_censusBegins[kFss] == beginsBeforeEmpty, "a replay that draws nothing produces no cost entry");
    a1.release(); a2.release(); rec.release(); pool.release();
    // leave the pipeline clean
    ID3D11ShaderResourceView* none[8] = {};
    g.ctx->PSSetShaderResources(0, 8, none);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11Buffer* nb[1] = {};
    g.ctx->PSSetConstantBuffers(2, 1, nb);
    g.ctx->VSSetShader(nullptr, nullptr, 0);
    g.ctx->PSSetShader(nullptr, nullptr, 0);
    g.ctx->RSSetState(nullptr);
    g.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    g.ctx->OMSetDepthStencilState(nullptr, 0);
    ID3D11SamplerState* nsmp[1] = {};
    g.ctx->PSSetSamplers(0, 1, nsmp);
}

// What a scanner session's summary says when nothing, something or everything happened.
void testVerdicts() {
    fr::RevealReplay none;
    check(std::strstr(fr::sessionVerdict(none, 0), "hook did not run") != nullptr, "no DrawInstanced call checked: the summary says the hook did not run");
    check(std::strstr(fr::sessionVerdict(none, 500), "nothing to do") != nullptr, "calls checked but no composite: the replay had nothing to do");
    int T = 0, idx = -1;
    fr::RevealReplay never;
    never.composite(false, &T, &idx);
    never.endFrame();
    check(std::strstr(fr::sessionVerdict(never, 500), "THE REPLAY NEVER FIRED") != nullptr, "composites drew and the replay never fired: said loudly, as a run that did not test the fix");
    fr::RevealReplay fired;
    fired.record(&T);
    fired.composite(false, &T, &idx);
    fired.endFrame();
    check(std::strcmp(fr::sessionVerdict(fired, 500), "the replay fired") == 0, "the replay fired");
    fired.verifiedBad = 1;
    check(std::strstr(fr::sessionVerdict(fired, 500), "DIFFERED") != nullptr, "a state read-back that differed is part of the verdict");
    check(std::strstr(fr::decisionText(fr::RevealReplay::DeclOtherTarget), "hash-only match") != nullptr, "the hash-only decline names itself");
}

void testSourcePins(const std::string& root) {
    const std::string cpp = slurp(root + "/src/d3d11/fss_reveal.cpp");
    const std::string vs = slurp(root + "/src/d3d11/vscreen.cpp");
    const std::string rp = slurp(root + "/src/d3d11/fss_reveal_replay.h");
    check(!cpp.empty() && !vs.empty() && !rp.empty(), "the sources are readable from the repo root");
    const size_t fn = vs.find("void STDMETHODCALLTYPE hookedDrawInstanced(");
    const size_t call = vs.find("fssRevealOnDrawSlow(self, rc, perInstance, instances, startVertex, startInstance);", fn);
    const size_t flat = vs.find("if (runtimeFlatProfile()) {", fn);
    check(fn != std::string::npos && call != std::string::npos && flat != std::string::npos && call < flat &&
              vs.find("if (fssRevealWatching() && !foreignContext(self) && !runtimeFlatProfile()) {", fn) < call,
          "the reveal fix is asked in DrawInstanced on the owner context, VR profile only, before the flat branch could be taken");
    check(has(vs, "rc.drawInstanced = reinterpret_cast<void*>(g_state->realDrawInstanced);") && has(vs, "rc.omSetRts = reinterpret_cast<void*>(g_state->realOMSetRenderTargets);") &&
              has(vs, "rc.psSetSrv = reinterpret_cast<void*>(g_state->realPSSetShaderResources);"),
          "the game's own un-hooked entry points are passed, so no hook sees the replay");
    check(has(vs, "fssRevealFrameBoundary(onShow, runtimeFlatProfile(), s->frameNo);") && has(vs, "deviceHookFssModeLatch() || (s->fssChromeFrame != 0 && s->frameNo - s->fssChromeFrame <= 5)") &&
              has(vs, "fssRevealConfigure(runtimeFlatProfile());") && has(vs, "fssRevealShutdown();"),
          "the scanner is on show while the FSS latch is open or its chrome matched within five frames; the fix is configured, ticked every frame and shut down");
    check(has(cpp, "g.rep.composite(g.flat, t1, &idx)") && has(cpp, "if (vs == kDownVs && ps == kDownPs) {") && has(cpp, "vs == kCompVs && ps == kCompPs") &&
              has(cpp, "replayDraw(dev, ctx, g.ctx1, calls, g.rec[idx], check ? &verified : nullptr)") && has(cpp, "captureState(dev, ctx, g.ctx1, &g.pool[idx], g.rec[idx])"),
          "a downsample is recorded with its constant buffers copied, and replayed only for the composite whose t1 is its render target");
    check(has(cpp, "const bool watch = onShow && !flat;") && has(cpp, "g.rep.endFrame();") && has(cpp, "detail::g_fssRevealWatch = watch;"), "the watch is VR only, on while the scanner is up, and every frame is tallied");
    check(has(cpp, "fss reveal fix: armed.") && has(cpp, "fss reveal fix: not armed") && has(cpp, "fss reveal fix: scanner session %u (frames %u-%u)") && has(cpp, "sessionVerdict(r, g.drawsChecked)") &&
              has(cpp, "THE CONTEXT STATE READ BACK AFTERWARDS DIFFERS FROM THE STATE BEFORE"),
          "the log says the fix is armed, then one summary per scanner session with the checked-call count, the declines, the state read-back and the verdict");
    check(has(rp, "applyState(ctx, c1, rc, rec, true);") && has(rp, "applyState(ctx, c1, rc, before, false);") && has(rp, "CopySubresourceRegion(pool.buf[stage][i]"),
          "the replay unbinds the target's inputs before it is bound, restores the targets before the inputs, and copies the constant buffers");
    // the census scopes sit at the GPU work sites in the replay header, nowhere in the watcher glue (the no-op calls per DrawInstanced are not timed)
    {
        size_t scopes = 0;
        for (size_t at = rp.find("GpuCensusScope census(ctx, GpuCensusSection::FrameFssReveal);"); at != std::string::npos;
             at = rp.find("GpuCensusScope census(ctx, GpuCensusSection::FrameFssReveal);", at + 1)) ++scopes;
        const size_t copiesScope = rp.find("GpuCensusScope census(ctx, GpuCensusSection::FrameFssReveal);", rp.find("inline bool captureState("));
        const size_t copiesCall = rp.find("snapshotCbs(dev, ctx, *pool, 0,", copiesScope);
        const size_t replayFn = rp.find("inline bool replayDraw(");
        const size_t replayScope = rp.find("GpuCensusScope census(ctx, GpuCensusSection::FrameFssReveal);", replayFn);
        const size_t replayApply = rp.find("applyState(ctx, c1, rc, rec, true);", replayFn);
        const size_t replayRestore = rp.find("applyState(ctx, c1, rc, before, false);", replayFn);
        check(scopes == 2 && copiesScope != std::string::npos && copiesCall != std::string::npos && copiesScope < copiesCall &&
                  replayScope != std::string::npos && replayScope < replayApply && replayApply < replayRestore && rp.find("}", replayRestore) < rp.find("if (verified) {", replayFn),
              "the replay header opens the cost scope around the private constant-buffer copies and around the replay draw with its two state applications, and nowhere else");
        check(!has(cpp, "GpuCensus") && !has(cpp, "gpu_census"), "the hook glue in fss_reveal.cpp opens no cost scope: the watcher's no-op calls are not timed");
    }
    // the removed things stay removed
    check(!has(vs, "fssRevealBegin") && !has(vs, "DrawVerdict::kFssReveal") && !has(vs, "fssRevealNoteMap") && !has(vs, "fssRevealWantsDraws") && !has(vs, "edvrFssHeal"), "the old composite-input freeze, its draw verdict and its tees are gone from the hook");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || (std::strcmp(argv[1], "--self-test") && std::strcmp(argv[1], "--dry-run"))) {
        std::puts("usage: fss_reveal_test --self-test [repo root] | --dry-run");
        return 2;
    }
    if (!std::strcmp(argv[1], "--dry-run")) {
        std::puts("Would run the reveal fix's decision tests, the state capture and replay on a WARP device with the debug layer, and source pins; writes no files.");
        return 0;
    }
    const std::string root = argc > 2 ? argv[2] : ".";

    testDecisions();
    testVerdicts();

    const auto createDevice = edvr::systemD3D11CreateDevice();
    check(createDevice != nullptr, "System32's D3D11CreateDevice");
    if (createDevice) {
        Gpu g;
        D3D_FEATURE_LEVEL level{};
        HRESULT hr = createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &g.dev, &level, &g.ctx);
        if (FAILED(hr)) {
            std::puts("(the D3D debug layer is not installed here: running without it)");
            hr = createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g.dev, &level, &g.ctx);
        }
        check(SUCCEEDED(hr), "a WARP device");
        check(edvr::reportSystemD3D11Only("fss_reveal_test"), "the rig runs on System32's d3d11.dll and on no other d3d11.dll");
        if (SUCCEEDED(hr)) {
            testReplayOnWarp(g);
            ComPtr<ID3D11InfoQueue> q;
            if (SUCCEEDED(g.dev.As(&q))) {
                const UINT64 cnt = q->GetNumStoredMessagesAllowedByRetrievalFilter();
                bool bad = false;
                for (UINT64 i = 0; i < cnt; ++i) {
                    SIZE_T len = 0;
                    q->GetMessage(i, nullptr, &len);
                    std::vector<char> raw(len);
                    auto* m = reinterpret_cast<D3D11_MESSAGE*>(raw.data());
                    if (SUCCEEDED(q->GetMessage(i, m, &len)) && (m->Severity == D3D11_MESSAGE_SEVERITY_ERROR || m->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION)) {
                        std::printf("  debug layer: %s\n", m->pDescription);
                        bad = true;
                    }
                }
                check(!bad, "the D3D debug layer stayed clean");
            }
        }
    }

    testSourcePins(root);

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

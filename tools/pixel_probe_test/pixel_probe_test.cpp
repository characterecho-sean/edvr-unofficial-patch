// Test rig for the pixel probe (src/d3d11/pixel_probe.*): a WARP device, a
// 256x256 eye-sized render target and a trivial VS/PS that fills a
// scissor-clipped region with a constant-buffer colour, so a "draw" can be
// placed precisely on or off a probe window. Config and Log are narrow,
// self-contained stubs -- only what pixel_probe.cpp actually calls -- so
// this rig needs nothing from vscreen.cpp or the rest of the DLL. Log's
// stub CAPTURES each formatted note() line instead of discarding it, since
// the log line is this module's entire observable output.
#include "../../src/d3d11/pixel_probe.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/common/config.h"
#include "../../src/common/log.h"

namespace edvr {
namespace {
std::string g_cfgPixelProbe;
bool        g_cfgPixelProbeSet = false;
}  // namespace

Config& Config::get() { static Config instance; return instance; }
std::string Config::getString(const char* key, const char* def) const {
    if (g_cfgPixelProbeSet && std::strcmp(key, "advanced.pixel_probe") == 0) return g_cfgPixelProbe;
    return def;
}
void Config::set(const char* key, const char* value) {
    if (std::strcmp(key, "advanced.pixel_probe") == 0) { g_cfgPixelProbe = value; g_cfgPixelProbeSet = true; }
}

namespace {
std::vector<std::string> g_log;
}  // namespace

Log& Log::get() { static Log instance; return instance; }
Log::~Log() = default;
void Log::note(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    g_log.emplace_back(buf);
}

}  // namespace edvr

using namespace edvr;
using Microsoft::WRL::ComPtr;

namespace {

int g_checks = 0, g_failures = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failures; std::printf("  FAIL  %s\n", what); }
    else std::printf("  ok    %s\n", what);
}
void hr(HRESULT v, const char* what) { check(SUCCEEDED(v), what); }

// True if any captured log line contains needle.
bool logHas(const char* needle) {
    for (const std::string& line : g_log) if (line.find(needle) != std::string::npos) return true;
    return false;
}
// The last captured line containing needle, or "".
std::string logLine(const char* needle) {
    for (auto it = g_log.rbegin(); it != g_log.rend(); ++it)
        if (it->find(needle) != std::string::npos) return *it;
    return "";
}
constexpr unsigned kFieldNotFound = 0xFFFFFFFFu;
unsigned fieldAfter(const std::string& line, const char* label) {
    const size_t p = line.find(label);
    if (p == std::string::npos) return kFieldNotFound;
    return static_cast<unsigned>(std::strtoul(line.c_str() + p + std::strlen(label), nullptr, 10));
}

constexpr UINT kSize = 256;

struct Rig {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Texture2D> rt;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vb;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11RasterizerState> rs;

    void init() {
        hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                             D3D11_SDK_VERSION, &dev, nullptr, &ctx),
           "create WARP device");

        D3D11_TEXTURE2D_DESC td{};
        td.Width = kSize; td.Height = kSize; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr(dev->CreateTexture2D(&td, nullptr, &rt), "create eye-sized target");
        hr(dev->CreateRenderTargetView(rt.Get(), nullptr, &rtv), "create RTV");

        const char* vsSrc = "float4 main(float2 pos : POSITION) : SV_Position { return float4(pos, 0, 1); }";
        const char* psSrc = "cbuffer C : register(b0) { float4 color; } "
                            "float4 main() : SV_Target { return color; }";
        ComPtr<ID3DBlob> vsBlob, psBlob, errors;
        hr(D3DCompile(vsSrc, std::strlen(vsSrc), nullptr, nullptr, nullptr, "main", "vs_5_0",
                      D3DCOMPILE_ENABLE_STRICTNESS, 0, &vsBlob, &errors), "compile trivial VS");
        hr(D3DCompile(psSrc, std::strlen(psSrc), nullptr, nullptr, nullptr, "main", "ps_5_0",
                      D3DCOMPILE_ENABLE_STRICTNESS, 0, &psBlob, &errors), "compile trivial PS");
        hr(dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs),
           "create VS");
        hr(dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps),
           "create PS");
        D3D11_INPUT_ELEMENT_DESC elem{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                      D3D11_INPUT_PER_VERTEX_DATA, 0};
        hr(dev->CreateInputLayout(&elem, 1, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout),
           "create input layout");

        // The "big triangle" trick: one triangle, no index buffer, covers the
        // whole [-1,1] clip square and more. Which pixels actually land is
        // entirely the scissor rect's decision below.
        const float verts[6] = {-1, -1, -1, 3, 3, -1};
        D3D11_BUFFER_DESC vbd{}; vbd.ByteWidth = sizeof(verts); vbd.Usage = D3D11_USAGE_IMMUTABLE;
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vsd{verts, 0, 0};
        hr(dev->CreateBuffer(&vbd, &vsd, &vb), "create vertex buffer");

        D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = 16; cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr(dev->CreateBuffer(&cbd, nullptr, &cb), "create colour constant buffer");

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.ScissorEnable = TRUE;
        hr(dev->CreateRasterizerState(&rd, &rs), "create scissor-enabled rasteriser");

        ID3D11RenderTargetView* rtvs[1] = {rtv.Get()};
        ctx->OMSetRenderTargets(1, rtvs, nullptr);
        ctx->IASetInputLayout(layout.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        UINT stride = 8, offset = 0;
        ID3D11Buffer* vbs[1] = {vb.Get()};
        ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11Buffer* cbs[1] = {cb.Get()};
        ctx->PSSetConstantBuffers(0, 1, cbs);
        ctx->RSSetState(rs.Get());
        D3D11_VIEWPORT vp{0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0, 1};
        ctx->RSSetViewports(1, &vp);
        clear(0, 0, 0, 1);
    }

    void clear(float r, float g, float b, float a) {
        const float rgba[4] = {r, g, b, a};
        ctx->ClearRenderTargetView(rtv.Get(), rgba);
    }

    // Paints [x0,y0)-[x0+w,y0+h) with a solid colour through the scissor
    // rect; nothing outside it is touched. Returns an arbitrary DrawInfo the
    // caller can hand to pixelProbeAfterDraw, so the log's reported count
    // and hashes are known values the checks below can match exactly.
    DrawInfo draw(int x0, int y0, int w, int h, float r, float g, float b, uint64_t vsHash, uint64_t psHash) {
        const D3D11_RECT scissor{x0, y0, x0 + w, y0 + h};
        ctx->RSSetScissorRects(1, &scissor);
        const float color[4] = {r, g, b, 1};
        ctx->UpdateSubresource(cb.Get(), 0, nullptr, color, 0, 0);
        ctx->Draw(3, 0);
        DrawInfo info;
        info.vsHash = vsHash; info.psHash = psHash;
        info.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        info.count = 3; info.instances = 1;
        info.blendEnable = false; info.blendSrc = 1; info.blendDest = 0;
        info.depthEnable = false; info.depthWriteMask = 0; info.depthFunc = 1;
        info.indirect = false;
        return info;
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::printf("pixel_probe_test: dry-run, 0 checks, 0 failures\n");
        return 0;
    }

    Rig rig;
    rig.init();

    // --- an unconfigured probe allocates nothing and logs nothing. ---
    {
        pixelProbeShutdown();
        check(!pixelProbeWantsDraws(), "unconfigured: wantsDraws is false");
        g_log.clear();
        pixelProbeArm();
        pixelProbeBeforeDraw(rig.ctx.Get(), rig.rt.Get(), 0);
        DrawInfo info;
        pixelProbeAfterDraw(rig.ctx.Get(), rig.rt.Get(), 0, info);
        pixelProbeFrameBoundary(rig.ctx.Get());
        check(g_log.empty(), "unconfigured: arm/before/after/boundary logged nothing");
    }

    // --- a configured but unarmed probe also allocates nothing and logs
    // nothing: arm() is a separate, explicit step. ---
    {
        const PixelProbePoint pts[1] = {{0.5f, 0.5f}};
        pixelProbeConfigure(pts, 1);
        check(pixelProbeConfigured() && !pixelProbeWantsDraws(),
              "configured but unarmed: configured, and the draw hooks stay dormant");
        g_log.clear();
        pixelProbeBeforeDraw(rig.ctx.Get(), rig.rt.Get(), 0);
        DrawInfo info;
        pixelProbeAfterDraw(rig.ctx.Get(), rig.rt.Get(), 0, info);
        pixelProbeFrameBoundary(rig.ctx.Get());
        check(g_log.empty(), "configured but unarmed: before/after/boundary logged nothing");
        pixelProbeShutdown();
    }

    // --- the core sequence: baseline, a hit, a miss, a repeat of the same
    // colour, then the closing summary. One point at (0.5,0.5) on a 256x256
    // target: window [120,136)x[120,136). ---
    {
        const PixelProbePoint pts[1] = {{0.5f, 0.5f}};
        pixelProbeConfigure(pts, 1);
        g_log.clear();
        pixelProbeArm();
        // g_frame is this module's own running count (the object_probe.cpp
        // pattern) and pixelProbeShutdown() does not reset it, matching that
        // precedent -- so later blocks arm a later frame number. Only the
        // shape of the line is checked here, not which number it names.
        check(logHas("pixel probe: armed for frame ") && logHas("0.500,0.500") && logHas("window 16x16"),
              "arm: logs the frame, the point list and the window");

        // The dump key is polled inside Present BEFORE the frame boundary, so
        // the frame in progress at arm time is ending: the probe must stay
        // dormant through it and open the whole next frame -- not close the
        // ending one empty (the order device_hook.cpp's hookedPresent runs).
        check(!pixelProbeWantsDraws(), "armed: dormant until the next boundary opens the probed frame");
        pixelProbeFrameBoundary(rig.ctx.Get());
        check(pixelProbeWantsDraws() && !logHas(" done -- eyes seen "),
              "the boundary after arming opens the probed frame and does not close it empty");

        rig.clear(0, 0, 0, 1);   // the baseline: the whole target black

        // Draw #1: paints the probe window red -- the FIRST draw into the
        // target, so it must be caught through the baseline (there is no
        // earlier draw to diff against).
        pixelProbeBeforeDraw(rig.ctx.Get(), rig.rt.Get(), 0);
        DrawInfo d1 = rig.draw(120, 120, 16, 16, 1, 0, 0, 0x1111111111111111ull, 0x2222222222222222ull);
        pixelProbeAfterDraw(rig.ctx.Get(), rig.rt.Get(), 0, d1);

        // Draw #2: paints a region far from the probe window -- must not be
        // reported at all.
        pixelProbeBeforeDraw(rig.ctx.Get(), rig.rt.Get(), 0);   // no-op: baseline already taken
        DrawInfo d2 = rig.draw(0, 0, 16, 16, 0, 1, 0, 0x3333333333333333ull, 0x4444444444444444ull);
        pixelProbeAfterDraw(rig.ctx.Get(), rig.rt.Get(), 0, d2);

        // Draw #3: repaints the probe window with the SAME colour draw #1
        // left there -- a real draw, but nothing for it to change.
        pixelProbeBeforeDraw(rig.ctx.Get(), rig.rt.Get(), 0);
        DrawInfo d3 = rig.draw(120, 120, 16, 16, 1, 0, 0, 0x5555555555555555ull, 0x6666666666666666ull);
        pixelProbeAfterDraw(rig.ctx.Get(), rig.rt.Get(), 0, d3);

        pixelProbeFrameBoundary(rig.ctx.Get());
        check(!pixelProbeWantsDraws(), "closing boundary: the draw hooks go dormant again");

        check(logHas("draw #1") && logHas("vs 1111111111111111") && logHas("ps 2222222222222222") &&
              logHas("count 3") && logHas("inst 1") && logHas("changed 256/256"),
              "draw 1 (through the baseline): reported, right point, count and hashes, fully changed");
        check(!logHas("vs 3333333333333333") && !logHas("ps 4444444444444444"),
              "draw 2 (elsewhere): not reported");
        check(!logHas("vs 5555555555555555") && !logHas("ps 6666666666666666"),
              "draw 3 (same colour repainted): not reported");

        const std::string summary = logLine(" done -- eyes seen ");
        check(!summary.empty(), "closing summary line is present");
        check(fieldAfter(summary, "draws into eye targets ") == 3, "summary: 3 draws into the eye target");
        check(fieldAfter(summary, "draws that changed a window ") == 1, "summary: exactly 1 draw changed a window");
        check(fieldAfter(summary, "dropped ") == 0, "summary: no drops under the cap");

        pixelProbeShutdown();
    }

    // --- slot exhaustion: one point, more draws than kMaxDrawSlots (2048)
    // -- the summary must report the overflow as drops, not silently lose
    // it or crash. ---
    {
        const PixelProbePoint pts[1] = {{0.5f, 0.5f}};
        pixelProbeConfigure(pts, 1);
        g_log.clear();
        pixelProbeArm();
        pixelProbeFrameBoundary(rig.ctx.Get());   // opens the probed frame, as in Present
        rig.clear(0, 0, 0, 1);
        pixelProbeBeforeDraw(rig.ctx.Get(), rig.rt.Get(), 0);
        constexpr int kOverBy = 5;
        for (int i = 0; i < 2048 + kOverBy; ++i) {
            const float shade = (i % 2) ? 1.0f : 0.5f;   // alternate, so most copies do change
            DrawInfo d = rig.draw(120, 120, 16, 16, shade, 0, 0, 0x7777, 0x8888);
            pixelProbeAfterDraw(rig.ctx.Get(), rig.rt.Get(), 0, d);
        }
        pixelProbeFrameBoundary(rig.ctx.Get());
        const std::string summary = logLine(" done -- eyes seen ");
        check(!summary.empty(), "slot exhaustion: closing summary is still present");
        check(fieldAfter(summary, "dropped ") == static_cast<unsigned>(kOverBy),
              "slot exhaustion: the summary's dropped count is exactly the overflow");
        pixelProbeShutdown();
    }

    // --- a malformed config is refused whole, not half-applied. ---
    {
        Config::get().set("advanced.pixel_probe", "0.5,0.5;not-a-point");
        g_log.clear();
        pixelProbeConfigure(Config::get());
        check(!pixelProbeConfigured(), "malformed config: refused, not partially applied");
        check(logHas("refused rather than half-applied"), "malformed config: one refusal line logged");

        // A well-formed string on the same entry point, for contrast: it
        // must actually take.
        Config::get().set("advanced.pixel_probe", "0.1,0.2;0.3,0.4");
        g_log.clear();
        pixelProbeConfigure(Config::get());
        check(pixelProbeConfigured(), "well-formed config via Config: takes");
        check(g_log.empty(), "well-formed config: silent (no log unless refused)");
        pixelProbeShutdown();
    }

    std::printf("pixel_probe_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

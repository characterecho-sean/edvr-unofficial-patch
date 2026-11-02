// Test rig for the depth writer probe (src/d3d11/depth_writer_probe.*): a WARP device, a 256x256 R32G8X24 depth texture and
// depth-only draws that fill a scissor-clipped region with a constant-buffer depth, so a "draw" can be placed precisely.
// The module is driven directly (the same Before / After / frame-boundary calls vscreen.cpp makes); Log is a narrow stub that
// captures notes, and the output directory is the rig's own temp folder.
#include "../../src/d3d11/depth_writer_probe.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/common/log.h"

namespace edvr {
namespace {
std::vector<std::string> g_log;
}  // namespace

Log& Log::get() { static Log instance; return instance; }
Log::~Log() = default;
void Log::note(const char* fmt, ...) {
    char buf[1400];
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

bool logHas(const char* needle) {
    for (const std::string& line : g_log) if (line.find(needle) != std::string::npos) return true;
    return false;
}

constexpr UINT kSize = 256;
ID3D11DepthStencilView* g_eyeDsv = nullptr;   // the one view the resolver calls eye 0

bool resolver(ID3D11DepthStencilView* dsv, int* eye) {
    if (dsv && dsv == g_eyeDsv) { *eye = 0; return true; }
    return false;
}

struct Rig {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vb, cb;
    ComPtr<ID3D11RasterizerState> rs;
    ComPtr<ID3D11DepthStencilState> writeAlways, writeNever;

    void init(UINT arraySize = 1) {
        hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx), "create WARP device");
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kSize; td.Height = kSize; td.MipLevels = 1; td.ArraySize = arraySize;
        td.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        hr(dev->CreateTexture2D(&td, nullptr, &depth), "create the eye-sized depth texture");
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        dv.ViewDimension = arraySize > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DARRAY : D3D11_DSV_DIMENSION_TEXTURE2D;
        if (arraySize > 1) { dv.Texture2DArray.ArraySize = arraySize; }
        hr(dev->CreateDepthStencilView(depth.Get(), &dv, &dsv), "create the DSV");
        g_eyeDsv = dsv.Get();

        const char* vsSrc = "cbuffer C : register(b0) { float z; float3 pad; }\n"
                            "float4 main(float2 pos : POSITION) : SV_Position { return float4(pos, z, 1); }";
        ComPtr<ID3DBlob> vsBlob, errors;
        hr(D3DCompile(vsSrc, std::strlen(vsSrc), nullptr, nullptr, nullptr, "main", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vsBlob, &errors), "compile the VS");
        hr(dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs), "create the VS");
        D3D11_INPUT_ELEMENT_DESC elem{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
        hr(dev->CreateInputLayout(&elem, 1, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout), "create the input layout");
        const float verts[6] = {-1, -1, -1, 3, 3, -1};   // one triangle past the whole clip square; the scissor decides what lands
        D3D11_BUFFER_DESC vbd{}; vbd.ByteWidth = sizeof(verts); vbd.Usage = D3D11_USAGE_IMMUTABLE; vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vsd{verts, 0, 0};
        hr(dev->CreateBuffer(&vbd, &vsd, &vb), "create the vertex buffer");
        D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = 16; cbd.Usage = D3D11_USAGE_DEFAULT; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr(dev->CreateBuffer(&cbd, nullptr, &cb), "create the depth constant buffer");
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.ScissorEnable = TRUE; rd.DepthClipEnable = TRUE;
        hr(dev->CreateRasterizerState(&rd, &rs), "create the scissor rasteriser");
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        hr(dev->CreateDepthStencilState(&dd, &writeAlways), "create the depth-write-always state");
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        hr(dev->CreateDepthStencilState(&dd, &writeNever), "create the depth-read-only state");

        ID3D11RenderTargetView* none[1] = {nullptr};
        ctx->OMSetRenderTargets(0, none, dsv.Get());
        D3D11_VIEWPORT vp{0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(rs.Get());
        ctx->IASetInputLayout(layout.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const UINT stride = 8, offset = 0;
        ID3D11Buffer* vbs[1] = {vb.Get()};
        ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ID3D11Buffer* cbs[1] = {cb.Get()};
        ctx->VSSetConstantBuffers(0, 1, cbs);
        ctx->PSSetShader(nullptr, nullptr, 0);
        ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
    }

    // One draw the way vscreen.cpp frames it: Before, the draw, After. The rect is the scissor; z the depth it writes.
    void draw(const D3D11_RECT& rect, float z, bool writeDepth, uint64_t vsHash) {
        const float c[4] = {z, 0, 0, 0};
        ctx->UpdateSubresource(cb.Get(), 0, nullptr, c, 0, 0);
        ctx->RSSetScissorRects(1, &rect);
        ctx->OMSetDepthStencilState(writeDepth ? writeAlways.Get() : writeNever.Get(), 0);
        if (depthWriterWantsDraws()) depthWriterBeforeDraw(ctx.Get());
        ctx->Draw(3, 0);
        if (depthWriterWantsDraws()) {
            DrawInfo di;
            di.vsHash = vsHash;
            di.psHash = 0;
            di.count = 3;
            di.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            depthWriterAfterDraw(ctx.Get(), di);
        }
    }
};

std::wstring tempDir() {
    wchar_t buf[MAX_PATH];
    GetTempPathW(MAX_PATH, buf);
    std::wstring dir = std::wstring(buf) + L"edvr_depth_writer_test";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::vector<uint32_t> readMap(const std::wstring& path, uint32_t* w, uint32_t* h, uint32_t* fmt) {
    std::vector<uint32_t> out;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return out;
    char magic[8];
    uint32_t header[9];
    if (std::fread(magic, 1, 8, f) == 8 && std::memcmp(magic, "EDVRTEX1", 8) == 0 && std::fread(header, sizeof(header), 1, f) == 1) {
        *w = header[1]; *h = header[2]; *fmt = header[3];
        out.resize(static_cast<size_t>(*w) * *h);
        if (std::fread(out.data(), 4, out.size(), f) != out.size()) out.clear();
    }
    std::fclose(f);
    return out;
}

std::string readText(const std::wstring& path) {
    std::string s;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

// Arms, runs one probed frame of `body`, closes. The frame counter the module keeps is its own: arm, one boundary opens the frame, the next closes it.
template <class Body>
void probedFrame(Rig& rig, const wchar_t* stamp, Body body) {
    depthWriterArm(stamp);
    depthWriterFrameBoundary(rig.ctx.Get());   // the boundary that opens the probed frame
    body();
    depthWriterFrameBoundary(rig.ctx.Get());   // the one that closes it
}

void caseMap(const std::wstring& dir) {
    Rig rig;
    rig.init();
    depthWriterSetEyeResolver(&resolver);
    depthWriterSetOutputDir(dir.c_str());
    // A compute state the module must put back: a shader-resource view of an unrelated texture stays bound in slot 0 across the frame
    // (not the depth: a resource bound as the DSV is unbound as a view by the runtime).
    ComPtr<ID3D11Texture2D> keepTex;
    ComPtr<ID3D11ShaderResourceView> keep;
    {
        D3D11_TEXTURE2D_DESC kd{};
        kd.Width = 4; kd.Height = 4; kd.MipLevels = 1; kd.ArraySize = 1; kd.Format = DXGI_FORMAT_R8_UNORM;
        kd.SampleDesc.Count = 1; kd.Usage = D3D11_USAGE_DEFAULT; kd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr(rig.dev->CreateTexture2D(&kd, nullptr, &keepTex), "create a texture for the compute-state check");
        hr(rig.dev->CreateShaderResourceView(keepTex.Get(), nullptr, &keep), "create a view for the compute-state check");
        ID3D11ShaderResourceView* v[1] = {keep.Get()};
        rig.ctx->CSSetShaderResources(0, 1, v);
    }
    check(!depthWriterWantsDraws(), "an unarmed probe wants no draws");
    g_log.clear();
    const D3D11_RECT left{0, 0, 128, 256}, mid{64, 0, 192, 256}, right{192, 0, 256, 256};
    probedFrame(rig, L"T001", [&] {
        check(depthWriterWantsDraws(), "the armed frame wants draws");
        rig.draw(left, 0.5f, true, 0xAAAA000000000001ull);    // id 1: x 0..127 at 0.5
        rig.draw(mid, 0.7f, true, 0xAAAA000000000002ull);     // id 2: x 64..191 at 0.7 (over id 1's right half)
        rig.draw(right, 0.9f, false, 0xAAAA000000000003ull);  // a read-only draw: not followed, writes nothing
        rig.draw(left, 0.55f, true, 0xAAAA000000000004ull);   // id 3: x 0..127 at 0.55, over id 1's 0.5 and id 2's 0.7 -- a change everywhere it lands
        rig.draw(left, 0.55f, true, 0xAAAA000000000005ull);   // id 4: the same value again: no change, owns nothing
        // id 5: a draw through ANOTHER view of the same depth texture, one the resolver does not know: followed by the texture's identity
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11DepthStencilView> other;
        hr(rig.dev->CreateDepthStencilView(rig.depth.Get(), &dv, &other), "create a second view of the same depth");
        ID3D11RenderTargetView* none[1] = {nullptr};
        rig.ctx->OMSetRenderTargets(0, none, other.Get());
        rig.draw(D3D11_RECT{192, 0, 256, 128}, 0.8f, true, 0xAAAA000000000006ull);
        rig.ctx->OMSetRenderTargets(0, none, rig.dsv.Get());
    });
    check(!depthWriterWantsDraws(), "the probe stands down after the frame");
    check(logHas("5 depth-writing draws followed"), "the summary counts the followed draws (4 through the resolver's view, 1 through another view of the same texture)");
    const std::wstring base = dir + L"\\depthwriter_T001";
    uint32_t w = 0, h = 0, fmt = 0;
    const std::vector<uint32_t> map = readMap(base + L"_e0.bin", &w, &h, &fmt);
    check(map.size() == kSize * kSize && w == kSize && h == kSize && fmt == 42, "the map file is an EDVRTEX1 R32_UINT of the depth's size");
    if (map.size() == kSize * kSize) {
        const auto at = [&](uint32_t x, uint32_t y) { return map[y * kSize + x]; };
        // After id 3 repaints x 0..127 the map holds id 3 there and id 2 over 128..191; id 4 changed nothing; the read-only draw nothing.
        check(at(10, 10) == 3 && at(100, 200) == 3, "a pixel repainted with a different depth names the last draw that changed it, not an earlier one");
        check(at(150, 5) == 2 && at(191, 255) == 2, "a pixel one draw wrote is named for that draw");
        check(at(200, 5) == 5 && at(255, 127) == 5, "a draw through another view of the same depth texture is followed and named");
        check(at(200, 200) == 0 && at(255, 255) == 0, "a pixel no depth-writing draw changed is 0");
    }
    const std::string json = readText(base + L".json");
    check(json.find("\"id\":1,") != std::string::npos && json.find("AAAA000000000002") != std::string::npos, "the draw table names each followed draw's shaders");
    check(json.find("AAAA000000000003") == std::string::npos, "a read-only draw is not in the table");
    check(json.find("\"id\":1,\"eye\":0") != std::string::npos && json.find("\"final_pixels\":0") != std::string::npos,
          "a draw whose pixels were all repainted, or that changed nothing, owns none of the final depth");
    check(logHas("owner #3") && logHas("owns 32768 pixels") && logHas("owner #2") && logHas("owns 16384 pixels"), "the log names the owners with their pixel counts (3: 128 x 256, 2: 64 x 256)");
    // the compute state is the game's again
    ID3D11ShaderResourceView* after[1] = {};
    rig.ctx->CSGetShaderResources(0, 1, after);
    check(after[0] == keep.Get(), "the game's compute shader-resource binding is back as it was");
    if (after[0]) after[0]->Release();
    ID3D11ComputeShader* cs = nullptr;
    rig.ctx->CSGetShader(&cs, nullptr, nullptr);
    check(cs == nullptr, "and no compute shader is left bound");
    if (cs) cs->Release();
    depthWriterShutdown();
}

void caseOther(const std::wstring& dir) {
    {   // a view the resolver does not call an eye: nothing followed, nothing written
        Rig rig;
        rig.init();
        depthWriterSetEyeResolver(&resolver);
        depthWriterSetOutputDir(dir.c_str());
        g_eyeDsv = nullptr;   // the resolver now knows no view
        g_log.clear();
        probedFrame(rig, L"T002", [&] { rig.draw(D3D11_RECT{0, 0, 256, 256}, 0.5f, true, 0xBBBB000000000001ull); });
        check(logHas("0 depth-writing draws followed") && logHas("1 not into an eye's depth"), "a draw into another depth view is counted and not followed");
        uint32_t w = 0, h = 0, fmt = 0;
        check(readMap(dir + L"\\depthwriter_T002_e0.bin", &w, &h, &fmt).empty(), "no map is written for an eye that was never seen");
        depthWriterShutdown();
    }
    {   // an array depth texture is declined, once, and the frame still reports
        Rig rig;
        rig.init(2);
        depthWriterSetEyeResolver(&resolver);
        depthWriterSetOutputDir(dir.c_str());
        g_log.clear();
        probedFrame(rig, L"T003", [&] {
            rig.draw(D3D11_RECT{0, 0, 256, 256}, 0.5f, true, 0xCCCC000000000001ull);
            rig.draw(D3D11_RECT{0, 0, 128, 256}, 0.6f, true, 0xCCCC000000000002ull);
        });
        check(logHas("0 depth-writing draws followed") && logHas("1 declined"), "an array depth is declined once and nothing is followed");
        depthWriterShutdown();
    }
    {   // never armed: the hooks are inert
        Rig rig;
        rig.init();
        depthWriterSetEyeResolver(&resolver);
        g_log.clear();
        rig.draw(D3D11_RECT{0, 0, 256, 256}, 0.5f, true, 0xDDDD000000000001ull);
        depthWriterFrameBoundary(rig.ctx.Get());
        check(g_log.empty() && !depthWriterWantsDraws(), "an unarmed probe logs nothing and wants nothing");
        depthWriterShutdown();
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::printf("depth_writer_probe_test: dry-run, 0 checks, 0 failures\n");
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--self-test") != 0) {
        std::fprintf(stderr, "usage: depth_writer_probe_test --self-test | --dry-run\n");
        return 2;
    }
    const std::wstring dir = tempDir();
    caseMap(dir);
    caseOther(dir);
    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

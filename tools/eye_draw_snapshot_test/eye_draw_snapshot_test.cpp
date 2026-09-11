#include "../../src/d3d11/eye_draw_snapshot.h"
#include <cstdlib>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;
void check(bool ok, const char* why) {
    if (!ok) { std::printf("FAIL: %s\n", why); std::exit(1); }
}
void hr(HRESULT v) { check(SUCCEEDED(v), "D3D operation"); }
int wmain(int argc, wchar_t** argv) {
    check(argc == 2, "output path required");
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                         D3D11_SDK_VERSION, &dev, nullptr, &ctx));
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 192; bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> cb; hr(dev->CreateBuffer(&bd, nullptr, &cb));
    ID3D11Buffer* b = cb.Get(); ctx->VSSetConstantBuffers(0, 1, &b); ctx->PSSetConstantBuffers(2, 1, &b);
    D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = 8;
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> rt; hr(dev->CreateTexture2D(&td, nullptr, &rt));
    ComPtr<ID3D11RenderTargetView> rtv; hr(dev->CreateRenderTargetView(rt.Get(), nullptr, &rtv));
    ID3D11RenderTargetView* r = rtv.Get(); ctx->OMSetRenderTargets(1, &r, nullptr);
    td.Width = 3; td.Height = 2; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    uint8_t rgba[24]; for (int i = 0; i < 24; ++i) rgba[i] = static_cast<uint8_t>(i);
    D3D11_SUBRESOURCE_DATA sd{rgba, 12, 0};
    ComPtr<ID3D11Texture2D> texture; hr(dev->CreateTexture2D(&td, &sd, &texture));
    ComPtr<ID3D11ShaderResourceView> srv; hr(dev->CreateShaderResourceView(texture.Get(), nullptr, &srv));
    ID3D11ShaderResourceView* s = srv.Get(); ctx->PSSetShaderResources(2, 1, &s);
    edvr::EyeDrawSnapshot snap;
    snap.capture(ctx.Get(), 0, 0, 123, 0, 'X', 6, 1, 0);
    check(snap.draws.empty(), "unwatched family captured");
    for (uint32_t i = 0; i < 3; ++i) {
        float values[48]; for (float& v : values) v = static_cast<float>(12 + i*3);
        ctx->UpdateSubresource(cb.Get(), 0, nullptr, values, 0, 0);
        snap.capture(ctx.Get(), 100+i, i*2, edvr::EyeDrawSnapshot::kHolo, 0xA2965EC2931A39C8ull, 'X', 6, 1, 0);
        // Mutating the same resources after a draw must not change its copy.
        for (float& v : values) v = 99;
        ctx->UpdateSubresource(cb.Get(), 0, nullptr, values, 0, 0);
        ctx->UpdateSubresource(texture.Get(), 0, nullptr, values, 12, 0);
    }
    ComPtr<ID3D11Buffer> bound; ctx->VSGetConstantBuffers(0, 1, &bound);
    check(bound.Get() == cb.Get(), "capture changed constant bindings");
    ComPtr<ID3D11ShaderResourceView> boundSrv; ctx->PSGetShaderResources(2, 1, &boundSrv);
    check(boundSrv.Get() == srv.Get(), "capture changed texture binding");
    ComPtr<ID3D11RenderTargetView> boundRt; ctx->OMGetRenderTargets(1, &boundRt, nullptr);
    check(boundRt.Get() == rtv.Get(), "capture changed render target");
    check(snap.surfaces.size() == 1 && snap.draws.size() == 3, "duplicate source or lost draw");
    // Only the test waits, to make WARP deterministic. Production writes
    // after the eye ledger grace period and reports unavailable copies.
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0}; ComPtr<ID3D11Query> query;
    hr(dev->CreateQuery(&qd, &query)); ctx->End(query.Get()); ctx->Flush();
    const ULONGLONG deadline = GetTickCount64()+10000;
    HRESULT ready;
    while ((ready = ctx->GetData(query.Get(), nullptr, 0, 0)) == S_FALSE && GetTickCount64()<deadline) Sleep(1);
    hr(ready); check(ready == S_OK, "GPU timeout");
    check(snap.write(ctx.Get(), argv[1]), "snapshot write");
    check(snap.failures == 0, "missing copies");
    const char shaderBytes[] = "captured-bytecode";
    edvr::EyeDrawSnapshot::rememberShader(edvr::EyeDrawSnapshot::kHolo, shaderBytes, sizeof(shaderBytes));
    std::wstring directory = argv[1]; directory.resize(directory.find_last_of(L"\\/"));
    check(snap.writeShaders(directory.c_str()) == 0, "retained shader write");
    const std::wstring shaderPath = directory + L"\\vs_81216C77F90DEDD6.dxbc";
    FILE* shader = nullptr; check(_wfopen_s(&shader, shaderPath.c_str(), L"rb") == 0, "shader file");
    char saved[sizeof(shaderBytes)] = {};
    check(fread(saved, 1, sizeof(saved), shader) == sizeof(saved), "shader payload length");
    check(std::memcmp(saved, shaderBytes, sizeof(saved)) == 0, "shader payload content");
    fclose(shader);
    snap.draws.resize(edvr::EyeDrawSnapshot::kMaxDraws);
    snap.capture(ctx.Get(), 103, 6, edvr::EyeDrawSnapshot::kHolo, 0, 'X', 6, 1, 0);
    check(snap.dropped == 1 && snap.draws.size() == edvr::EyeDrawSnapshot::kMaxDraws, "capture cap");
    snap.reset(); check(snap.draws.empty() && snap.surfaces.empty() && !snap.dropped, "reset");
    std::puts("GPU draw snapshot capture passed");
}

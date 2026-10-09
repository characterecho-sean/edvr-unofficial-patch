// Build gate for the shared UI layer across a D3D11 device change (review 2026-10-09, P2: "Reset device-owned UI caches when
// the flat D3D11 device changes"). The production module (src/d3d11/ui_layer.cpp, compiled whole by
// ui_layer_device_bridge.cpp) and the world rig's neighbour stubs (tools/ui_layer_world_test, included with its main
// renamed), on two independent WARP devices:
//   1. device A: the door, the crisp take's dependency set, a HUD frame through the production coverage pass, the
//      production composite -- read back, the expected pixel; the caller's compute and output-merger bindings restored;
//   2. the production device-change reset (uiLayerDeviceReset, what flat_ui_layer.cpp's flatUiLayerNoteDevice runs);
//   3. device B: the same, and every retained device child -- the blend cache's entry, the coverage pass's shaders and
//      deferred context, the depth seeder's deferred context, the composite shader and its parameter buffer, the
//      layers, the composite's output -- belongs to B;
//   4. a same-device resize on B (uiLayerFlatRelease, the flat profile's ResizeBuffers path) at another frame size:
//      the pixel again, and NOTHING rebuilt (the same shaders, buffer, contexts and blend object).
// Controls: before the reset the retained children are A's (the check can fail), and the resize really re-made the
// size-bound layer.
#define main ui_layer_world_test_main
#include "../ui_layer_world_test/ui_layer_world_test.cpp"
#undef main

namespace edvr {
namespace devtest {
ID3D11BlendState* rgbOnlyBlend(ID3D11DeviceContext* ctx);
bool takeReady(ID3D11DeviceContext* ctx);
bool seederReady(ID3D11Device* dev);
ID3D11DeviceContext* coverageContext();
ID3D11DeviceContext* seederContext();
ID3D11VertexShader* coverageVs();
ID3D11PixelShader* coveragePs();
ID3D11ComputeShader* compositeCs();
ID3D11Buffer* compositeCb();
ID3D11Texture2D* layerTexture();
ID3D11Texture2D* hdrLayerTexture();
uint32_t layerWidth();
bool hudFrame(ID3D11DeviceContext* ctx, uint64_t seq, const float ldr[4], const float hdr[4]);
}  // namespace devtest
}  // namespace edvr

namespace {

template <class T>
bool ownedBy(T* child, ID3D11Device* dev) {
    if (!child || !dev) return false;
    ComPtr<ID3D11Device> owner;
    child->GetDevice(&owner);
    return owner.Get() == dev;
}

struct Retained {
    void* blend = nullptr;
    void* coverageCtx = nullptr;
    void* seederCtx = nullptr;
    void* covVs = nullptr;
    void* covPs = nullptr;
    void* cs = nullptr;
    void* cb = nullptr;
    void* layer = nullptr;
};

Retained retained(ID3D11DeviceContext* ctx) {
    Retained r;
    r.blend = devtest::rgbOnlyBlend(ctx);
    r.coverageCtx = devtest::coverageContext();
    r.seederCtx = devtest::seederContext();
    r.covVs = devtest::coverageVs();
    r.covPs = devtest::coveragePs();
    r.cs = devtest::compositeCs();
    r.cb = devtest::compositeCb();
    r.layer = devtest::layerTexture();
    return r;
}

bool allOwnedBy(ID3D11DeviceContext* ctx, ID3D11Device* dev) {
    bool ok = true;
    ok = ownedBy(devtest::rgbOnlyBlend(ctx), dev) && ok;
    ok = ownedBy(devtest::coverageContext(), dev) && ok;
    ok = ownedBy(devtest::seederContext(), dev) && ok;
    ok = ownedBy(devtest::coverageVs(), dev) && ok;
    ok = ownedBy(devtest::coveragePs(), dev) && ok;
    ok = ownedBy(devtest::compositeCs(), dev) && ok;
    ok = ownedBy(devtest::compositeCb(), dev) && ok;
    ok = ownedBy(devtest::layerTexture(), dev) && ok;
    ok = ownedBy(devtest::hdrLayerTexture(), dev) && ok;
    return ok;
}

ComPtr<ID3D11Texture2D> frameTexture(ID3D11Device* dev, uint32_t w, uint32_t h, const uint8_t rgba[4]) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < px.size(); i += 4) std::memcpy(&px[i], rgba, 4);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA init{px.data(), w * 4, 0};
    ComPtr<ID3D11Texture2D> t;
    dev->CreateTexture2D(&d, &init, &t);
    return t;
}

bool readPixel(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, uint32_t x, uint32_t y, uint8_t out[4]) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging))) return false;
    ctx->CopyResource(staging.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    std::memcpy(out, static_cast<const uint8_t*>(m.pData) + y * m.RowPitch + x * 4, 4);
    ctx->Unmap(staging.Get(), 0);
    return true;
}

bool near8(uint8_t a, int b) { return std::abs(static_cast<int>(a) - b) <= 2; }

// One frame of the flat layer on `dev`: the door, the take's dependencies, the HUD through the coverage pass, the
// composite over a frame -- the pixel read back, and the caller's bindings around both passes compared.
void frameRound(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint64_t seq, uint32_t w, uint32_t h, const char* label) {
    char what[200];
    const uint8_t frameRgba[4] = {51, 102, 153, 255};  // 0.2, 0.4, 0.6
    ComPtr<ID3D11Texture2D> frame = frameTexture(dev, w, h, frameRgba);
    std::snprintf(what, sizeof(what), "%s: the frame texture", label);
    check(frame != nullptr, what);
    if (!frame) return;
    uiLayerNoteTemporal(seq, 0, frame.Get());
    uiLayerDoorSeen(seq, 0, frame.Get());
    std::snprintf(what, sizeof(what), "%s: the crisp take's dependency set is made", label);
    check(devtest::takeReady(ctx) && devtest::seederReady(dev), what);
    std::snprintf(what, sizeof(what), "%s: the layer is the door's size", label);
    check(devtest::layerWidth() == w, what);

    // The caller's output-merger state around the coverage pass (a deferred command list executed with restore).
    ComPtr<ID3D11Texture2D> sentinelTex = frameTexture(dev, 4, 4, frameRgba);
    ComPtr<ID3D11RenderTargetView> sentinelRtv;
    dev->CreateRenderTargetView(sentinelTex.Get(), nullptr, &sentinelRtv);
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> sentinelBlend;
    dev->CreateBlendState(&bd, &sentinelBlend);
    ID3D11RenderTargetView* rtvs[1] = {sentinelRtv.Get()};
    ctx->OMSetRenderTargets(1, rtvs, nullptr);
    ctx->OMSetBlendState(sentinelBlend.Get(), nullptr, 0xFFFFFFFFu);
    const float ldr[4] = {0.25f, 0.0f, 0.5f, 1.0f};
    const float hdr[4] = {0.0f, 0.0f, 0.0f, 0.5f};
    std::snprintf(what, sizeof(what), "%s: the HUD frame through the production coverage pass", label);
    check(devtest::hudFrame(ctx, seq, ldr, hdr), what);
    ComPtr<ID3D11RenderTargetView> rtvAfter;
    ctx->OMGetRenderTargets(1, &rtvAfter, nullptr);
    ComPtr<ID3D11BlendState> blendAfter;
    FLOAT factor[4] = {};
    UINT mask = 0;
    ctx->OMGetBlendState(&blendAfter, factor, &mask);
    std::snprintf(what, sizeof(what), "%s: the caller's render target and blend state are restored after the coverage pass", label);
    check(rtvAfter.Get() == sentinelRtv.Get() && blendAfter.Get() == sentinelBlend.Get(), what);

    // The caller's compute bindings around the composite (the output-merger sentinel unbound first: its texture is the
    // compute sentinel's too, and D3D11 drops a view of a resource still bound as a render target).
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    D3D11_BUFFER_DESC cbd{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    ComPtr<ID3D11Buffer> sentinelCb;
    dev->CreateBuffer(&cbd, nullptr, &sentinelCb);
    ComPtr<ID3D11ShaderResourceView> sentinelSrv;
    dev->CreateShaderResourceView(sentinelTex.Get(), nullptr, &sentinelSrv);
    ID3D11Buffer* cbs[1] = {sentinelCb.Get()};
    ID3D11ShaderResourceView* srvs[1] = {sentinelSrv.Get()};
    ctx->CSSetConstantBuffers(0, 1, cbs);
    ctx->CSSetShaderResources(0, 1, srvs);
    const uint32_t region[4] = {0, 0, w, h};
    const float uv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    ID3D11Texture2D* out = uiLayerComposite(seq, 0, frame.Get(), region, uv);
    ComPtr<ID3D11Buffer> cbAfter;
    ComPtr<ID3D11ShaderResourceView> srvAfter;
    ctx->CSGetConstantBuffers(0, 1, &cbAfter);
    ctx->CSGetShaderResources(0, 1, &srvAfter);
    std::snprintf(what, sizeof(what), "%s: the composite ran (an output texture)", label);
    check(out != nullptr, what);
    std::snprintf(what, sizeof(what), "%s: the caller's compute constant buffer and resource view are restored after the composite", label);
    check(cbAfter.Get() == sentinelCb.Get() && srvAfter.Get() == sentinelSrv.Get(), what);
    if (!out) return;
    std::snprintf(what, sizeof(what), "%s: the composite's output belongs to this device", label);
    check(ownedBy(out, dev), what);
    uint8_t px[4] = {};
    // out = L.rgb + F.rgb * L.a, L.a the HDR layer's 0.5 (128/255) through the coverage pass.
    const double a = 128.0 / 255.0;
    const bool read = readPixel(dev, ctx, out, w / 2, h / 2, px);
    std::snprintf(what, sizeof(what), "%s: the composite's pixel is the HUD over the frame (got %u %u %u)", label, px[0], px[1], px[2]);
    check(read && near8(px[0], static_cast<int>(std::lround((0.25 + 0.2 * a) * 255.0))) &&
              near8(px[1], static_cast<int>(std::lround((0.4 * a) * 255.0))) &&
              near8(px[2], static_cast<int>(std::lround((0.5 + 0.6 * a) * 255.0))),
          what);
    out->Release();
    ctx->ClearState();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("ui_layer_device_test: dry-run (no device, no files)");
        return 0;
    }
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::puts("usage: ui_layer_device_test --self-test | --dry-run");
        return 2;
    }
    const auto create = edvr::systemD3D11CreateDevice();
    ComPtr<ID3D11Device> devA, devB;
    ComPtr<ID3D11DeviceContext> ctxA, ctxB;
    D3D_FEATURE_LEVEL fl{};
    const HRESULT ha = create ? create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &devA, &fl, &ctxA) : E_FAIL;
    const HRESULT hb = create ? create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &devB, &fl, &ctxB) : E_FAIL;
    check(SUCCEEDED(ha) && SUCCEEDED(hb) && devA.Get() != devB.Get(), "two independent WARP devices");
    if (FAILED(ha) || FAILED(hb)) return 1;
    detail::g_uiLayerLive = true;
    detail::g_uiLayerCrispOn = true;

    frameRound(devA.Get(), ctxA.Get(), 10, 64, 64, "device A");
    check(allOwnedBy(ctxA.Get(), devA.Get()), "device A: every retained device child is A's");
    check(!allOwnedBy(ctxB.Get(), devB.Get()),
          "control: before the reset the retained children are not B's (the ownership check can fail)");

    uiLayerDeviceReset();  // the production device-change reset (flat_ui_layer.cpp flatUiLayerNoteDevice)
    detail::g_uiLayerLive = true;
    detail::g_uiLayerCrispOn = true;
    frameRound(devB.Get(), ctxB.Get(), 20, 64, 64, "device B");
    check(allOwnedBy(ctxB.Get(), devB.Get()), "device B: every retained device child (blend, coverage shaders and context, seeder "
                                              "context, composite shader and buffer, both layers) is B's");

    // A same-device resize: the size-bound layer is made again, nothing device-bound is.
    const Retained before = retained(ctxB.Get());
    uiLayerFlatRelease();
    frameRound(devB.Get(), ctxB.Get(), 30, 48, 32, "device B after a resize");
    const Retained after = retained(ctxB.Get());
    check(after.blend == before.blend && after.coverageCtx == before.coverageCtx && after.seederCtx == before.seederCtx &&
              after.covVs == before.covVs && after.covPs == before.covPs && after.cs == before.cs && after.cb == before.cb,
          "a same-device resize rebuilds no shader, buffer, blend or deferred context");
    check(devtest::layerWidth() == 48, "control: the resize really re-made the size-bound layer (48 wide)");
    check(allOwnedBy(ctxB.Get(), devB.Get()), "device B after a resize: every retained device child is still B's");

    uiLayerShutdown();
    std::printf("ui_layer_device_test: %u checks, %u failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

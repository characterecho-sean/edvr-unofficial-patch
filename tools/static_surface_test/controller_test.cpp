// End-to-end WARP test for the production static-surface controller. Only
// Elite's settled depth/eye classification, binding shadow and raw hook bypass
// calls are supplied by this standalone harness.
// This rig supplies its own binding shadow readers (below), so it asks the
// header for declarations rather than the inline production ones.
#define EDVR_BINDING_SHADOW_EXTERNAL 1
#include "../../src/d3d11/static_surface.cpp"
#include "../../third_party/dxbc_hash/DxilHash.cpp"

#include <d3dcompiler.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

unsigned checks = 0;
ID3D11DepthStencilView* shadowDsv = nullptr;
std::vector<std::pair<ID3D11DepthStencilView*, int>> sceneEyes;

void check(bool value, const char* why) {
    ++checks;
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", why);
        std::exit(1);
    }
}

void hr(HRESULT result, const char* why) {
    if (FAILED(result)) std::fprintf(stderr, "HRESULT %08X: %s\n", unsigned(result), why);
    check(SUCCEEDED(result), why);
}

ComPtr<ID3DBlob> compile(const char* source, const char* profile) {
    ComPtr<ID3DBlob> code, errors;
    const HRESULT result = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
                                      "main", profile, D3DCOMPILE_ENABLE_STRICTNESS |
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(result) && errors)
        std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
    hr(result, "compile shader");
    return code;
}

struct Colour {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
};

struct Depth {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11DepthStencilView> dsv;
};

Colour colour(ID3D11Device* device, UINT width, UINT height) {
    Colour result;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = width;
    d.Height = height;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    hr(device->CreateTexture2D(&d, nullptr, &result.texture), "create colour texture");
    hr(device->CreateRenderTargetView(result.texture.Get(), nullptr, &result.rtv), "create colour RTV");
    return result;
}

Depth depth(ID3D11Device* device, UINT width, UINT height, int eye) {
    Depth result;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = width;
    d.Height = height;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_D32_FLOAT;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr(device->CreateTexture2D(&d, nullptr, &result.texture), "create depth texture");
    hr(device->CreateDepthStencilView(result.texture.Get(), nullptr, &result.dsv), "create DSV");
    sceneEyes.push_back({result.dsv.Get(), eye});
    return result;
}

std::array<uint32_t, 4> pixel(ID3D11Device* device, ID3D11DeviceContext* context,
                              ID3D11ShaderResourceView* view, UINT x = 4, UINT y = 4) {
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> source;
    hr(resource.As(&source), "owner SRV is a texture");
    D3D11_TEXTURE2D_DESC d{};
    source->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = d.MiscFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    hr(device->CreateTexture2D(&d, nullptr, &staging), "create owner staging texture");
    context->CopyResource(staging.Get(), source.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "map owner staging texture");
    std::array<uint32_t, 4> result{};
    std::memcpy(result.data(), static_cast<const BYTE*>(mapped.pData) + y * mapped.RowPitch + x * 16, 16);
    context->Unmap(staging.Get(), 0);
    return result;
}

struct Model { uint32_t word[84]{}; };

struct Fixture {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vertex, instance[2], index, pool, sentinelCb;
    ComPtr<ID3D11ShaderResourceView> poolView, sentinelView;
    ComPtr<ID3D11DepthStencilState> depthState, depthDisabled;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11BlendState> blend, blended;
    Colour target[2];
    Depth scene[2];
    std::array<D3D11_INPUT_ELEMENT_DESC, 2> elements{};
    ID3DBlob* vsCode = nullptr;
    ID3DBlob* psCode = nullptr;

    Fixture() {
        D3D_FEATURE_LEVEL level{};
        hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                             D3D11_SDK_VERSION, &device, &level, &context), "create WARP device");

        static const char vsSource[] = R"HLSL(
struct I { uint model:INSTANCEANDMODELDATAINDEX; float2 xy:POSITION; };
struct O { nointerpolation uint3 id:__USER_MATERIALMODULATION_DATAID;
           float4 p:SV_Position; };
O main(I i) { O o; o.id=uint3(0,i.model,0); o.p=float4(i.xy,.5,1); return o; }
)HLSL";
        static const char psSource[] = R"HLSL(
struct I { nointerpolation uint3 id:__USER_MATERIALMODULATION_DATAID;
           float4 p:SV_Position; };
float4 main(I i):SV_Target0 { return float4((i.id.y&1)?.25:.5,i.p.z,.2,1); }
)HLSL";
        ComPtr<ID3DBlob> vsBlob = compile(vsSource, "vs_5_0");
        ComPtr<ID3DBlob> psBlob = compile(psSource, "ps_5_0");
        vsCode = vsBlob.Detach();
        psCode = psBlob.Detach();
        hr(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs),
           "create controller VS");
        hr(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps),
           "create controller PS");

        elements[0] = {"INSTANCEANDMODELDATAINDEX", 0, DXGI_FORMAT_R32_UINT, 0, 0,
                       D3D11_INPUT_PER_INSTANCE_DATA, 1};
        elements[1] = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0,
                       D3D11_INPUT_PER_VERTEX_DATA, 0};
        hr(device->CreateInputLayout(elements.data(), UINT(elements.size()), vsCode->GetBufferPointer(),
                                     vsCode->GetBufferSize(), &layout), "create input layout");

        const float vertices[6] = {-1, -1, -1, 3, 3, -1};
        const uint32_t instanceValues[2] = {0, 1};
        const uint16_t indices[6] = {0, 1, 2, 0, 1, 2};
        vertex = buffer(sizeof(vertices), D3D11_BIND_VERTEX_BUFFER, vertices);
        instance[0] = buffer(sizeof(uint32_t), D3D11_BIND_VERTEX_BUFFER, &instanceValues[0]);
        instance[1] = buffer(sizeof(uint32_t), D3D11_BIND_VERTEX_BUFFER, &instanceValues[1]);
        index = buffer(sizeof(indices), D3D11_BIND_INDEX_BUFFER, indices);

        std::array<Model, 2> models{};
        for (auto& model : models) {
            model.word[0] = 0;
            model.word[1] = 0x3f800000u;
            model.word[2] = 0x11112222u;
            model.word[3] = 0x33334444u;
            model.word[4] = 0x41200000u;
            model.word[5] = 0x41a00000u;
            model.word[6] = 0x41f00000u;
        }
        D3D11_BUFFER_DESC poolDesc{};
        poolDesc.ByteWidth = UINT(sizeof(models));
        poolDesc.Usage = D3D11_USAGE_DEFAULT;
        poolDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        poolDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        poolDesc.StructureByteStride = sizeof(Model);
        D3D11_SUBRESOURCE_DATA poolData{models.data(), 0, 0};
        hr(device->CreateBuffer(&poolDesc, &poolData, &pool), "create model pool");
        hr(device->CreateShaderResourceView(pool.Get(), nullptr, &poolView), "create model-pool SRV");

        uint32_t zero[4]{};
        sentinelCb = buffer(sizeof(zero), D3D11_BIND_CONSTANT_BUFFER, zero);
        D3D11_BUFFER_DESC sentinelDesc{};
        sentinelDesc.ByteWidth = 16;
        sentinelDesc.Usage = D3D11_USAGE_DEFAULT;
        sentinelDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sentinelDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        sentinelDesc.StructureByteStride = 4;
        D3D11_SUBRESOURCE_DATA sentinelData{zero, 0, 0};
        ComPtr<ID3D11Buffer> sentinelBuffer;
        hr(device->CreateBuffer(&sentinelDesc, &sentinelData, &sentinelBuffer), "create sentinel SRV buffer");
        hr(device->CreateShaderResourceView(sentinelBuffer.Get(), nullptr, &sentinelView), "create sentinel SRV");

        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        hr(device->CreateDepthStencilState(&dd, &depthState), "create writable depth state");
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        hr(device->CreateDepthStencilState(&dd, &depthDisabled), "create read-only depth state");

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        hr(device->CreateRasterizerState(&rd, &raster), "create rasterizer state");

        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED |
                                                   D3D11_COLOR_WRITE_ENABLE_GREEN |
                                                   D3D11_COLOR_WRITE_ENABLE_BLUE;
        hr(device->CreateBlendState(&bd, &blend), "create original blend state");
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        hr(device->CreateBlendState(&bd, &blended), "create enabled blend state");

        for (int eye = 0; eye < 2; ++eye) {
            target[eye] = colour(device.Get(), 8, 8);
            scene[eye] = depth(device.Get(), 8, 8, eye);
        }

        edvr::staticSurfaceConfigure(true);
        edvr::staticSurfaceRememberVs(vs.Get(), 0xEB5234DB6ADB491Dull,
                                      vsCode->GetBufferPointer(), vsCode->GetBufferSize(), false);
        edvr::staticSurfaceRememberPs(ps.Get(), psCode->GetBufferPointer(), psCode->GetBufferSize(), false);
        edvr::staticSurfaceRememberLayout(layout.Get(), elements.data(), UINT(elements.size()),
                                          0xEB5234DB6ADB491Dull);
    }

    ~Fixture() {
        edvr::staticSurfaceShutdown();
        if (vsCode) vsCode->Release();
        if (psCode) psCode->Release();
    }

    ComPtr<ID3D11Buffer> buffer(UINT bytes, UINT bind, const void* initial) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = bytes;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = bind;
        D3D11_SUBRESOURCE_DATA data{initial, 0, 0};
        ComPtr<ID3D11Buffer> result;
        hr(device->CreateBuffer(&d, initial ? &data : nullptr, &result), "create buffer");
        return result;
    }

    void setup(int eye, unsigned instanceBuffer, Colour* colourOverride = nullptr,
               Depth* depthOverride = nullptr) {
        Colour& c = colourOverride ? *colourOverride : target[eye];
        Depth& z = depthOverride ? *depthOverride : scene[eye];
        shadowDsv = z.dsv.Get();
        ID3D11RenderTargetView* rtv = c.rtv.Get();
        context->OMSetRenderTargets(1, &rtv, z.dsv.Get());
        const float blendFactor[4] = {.125f, .25f, .5f, 1};
        context->OMSetBlendState(blend.Get(), blendFactor, 0x5a5a5a5bu);
        context->OMSetDepthStencilState(depthState.Get(), 7);
        context->RSSetState(raster.Get());
        D3D11_TEXTURE2D_DESC zd{};
        z.texture->GetDesc(&zd);
        D3D11_VIEWPORT vp{0, 0, float(zd.Width), float(zd.Height), 0, 1};
        context->RSSetViewports(1, &vp);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetInputLayout(layout.Get());
        ID3D11Buffer* vb[2] = {instance[instanceBuffer].Get(), vertex.Get()};
        UINT stride[2] = {4, 8}, offset[2]{};
        context->IASetVertexBuffers(0, 2, vb, stride, offset);
        context->IASetIndexBuffer(index.Get(), DXGI_FORMAT_R16_UINT, 0);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11ShaderResourceView* poolRaw = poolView.Get();
        context->VSSetShaderResources(33, 1, &poolRaw);
        ID3D11Buffer* nullCb = nullptr;
        ID3D11ShaderResourceView* nullSrv = nullptr;
        context->PSSetConstantBuffers(13, 1, &nullCb);
        context->PSSetShaderResources(127, 1, &nullSrv);
    }

    bool begin(uint64_t hash = 0xEB5234DB6ADB491Dull, unsigned start = 0) {
        return edvr::staticSurfaceBegin(context.Get(), 3, 1, start, 0, 0, hash);
    }

    void draw(uint64_t hash = 0xEB5234DB6ADB491Dull, unsigned start = 0) {
        check(begin(hash, start), "eligible draw begins");
        context->DrawIndexedInstanced(3, 1, start, 0, 0);
        edvr::staticSurfaceEnd(context.Get());
    }
};

void checkRestored(Fixture& f) {
    ComPtr<ID3D11PixelShader> ps;
    f.context->PSGetShader(&ps, nullptr, nullptr);
    check(ps.Get() == f.ps.Get(), "original pixel shader restored");
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    f.context->OMGetRenderTargets(1, &rtv, &dsv);
    check(rtv.Get() == f.target[0].rtv.Get() && dsv.Get() == f.scene[0].dsv.Get(),
          "original RT and depth restored");
    ComPtr<ID3D11BlendState> blend;
    FLOAT factor[4]{};
    UINT mask = 0;
    f.context->OMGetBlendState(&blend, factor, &mask);
    check(blend.Get() == f.blend.Get() && factor[0] == .125f && factor[1] == .25f &&
          factor[2] == .5f && factor[3] == 1 && mask == 0x5a5a5a5bu,
          "original blend state, factors and mask restored");
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11ShaderResourceView> srv;
    f.context->PSGetConstantBuffers(13, 1, &cb);
    f.context->PSGetShaderResources(127, 1, &srv);
    check(!cb && !srv, "original null reserved CB and SRV restored");
}

void controllerTests() {
    Fixture f;
    ID3D11ShaderResourceView* views0[2]{};
    ID3D11ShaderResourceView* views1[2]{};
    auto releaseViews=[](ID3D11ShaderResourceView** views){for(unsigned i=0;i<2;++i){if(views[i])views[i]->Release();views[i]=nullptr;}};

    // Frame 1: each eye owns data independently, but neither can publish a
    // previous/current pair yet. Asking for a view consumes only that eye.
    f.setup(0, 0);
    f.draw();
    checkRestored(f);
    check(!edvr::staticSurfaceViews(f.context.Get(), f.scene[0].texture.Get(), views0),
          "eye 0 first frame is bootstrap only");
    check(!f.begin(), "eye 0 refuses draws after its history was consumed");
    f.setup(1, 0);
    f.draw();
    check(!edvr::staticSurfaceViews(f.context.Get(), f.scene[1].texture.Get(), views1),
          "eye 1 first frame is independently bootstrap only");
    edvr::staticSurfaceFrameBoundary(f.context.Get());

    // Frame 2: change the per-instance stream and pool index. The raw rigid
    // pose and per-vertex geometry stay exact, so both owner keys must match.
    f.setup(0, 1);
    edvr::staticSurfaceResourceWritten(f.index.Get(), 6, 12); // disjoint from [0,6)
    f.draw();
    check(edvr::staticSurfaceViews(f.context.Get(), f.scene[0].texture.Get(), views0),
          "eye 0 publishes consecutive owner history");
    const auto current0 = pixel(f.device.Get(), f.context.Get(), views0[0]);
    const auto previous0 = pixel(f.device.Get(), f.context.Get(), views0[1]);
    if (current0 != previous0)
        std::fprintf(stderr, "owner current %08x %08x %08x %08x previous %08x %08x %08x %08x\n",
                     current0[0], current0[1], current0[2], current0[3],
                     previous0[0], previous0[1], previous0[2], previous0[3]);
    check((current0[0] || current0[1] || current0[2]) && current0 == previous0,
          "instance stream/pool index and disjoint index write preserve exact owner key");
    ID3D11ShaderResourceView* frame2Current = views0[0];
    ID3D11ShaderResourceView* frame2Previous = views0[1];
    releaseViews(views0);
    f.setup(1, 1);
    f.draw();
    check(edvr::staticSurfaceViews(f.context.Get(), f.scene[1].texture.Get(), views1),
          "eye 1 publishes its own consecutive owner history");
    releaseViews(views1);
    edvr::staticSurfaceFrameBoundary(f.context.Get());

    // Frame 3: an overlapping index-buffer write changes only the used slice's
    // epoch, and the ping-pong orientation reverses.
    f.setup(0, 1);
    edvr::staticSurfaceResourceWritten(f.index.Get(), 0, 6);
    f.draw();
    check(edvr::staticSurfaceViews(f.context.Get(), f.scene[0].texture.Get(), views0),
          "third consecutive frame publishes");
    check(views0[0] == frame2Previous && views0[1] == frame2Current,
          "owner current/previous textures swap at the frame boundary");
    const auto changed = pixel(f.device.Get(), f.context.Get(), views0[0]);
    const auto old = pixel(f.device.Get(), f.context.Get(), views0[1]);
    check(changed != old, "overlapping index write changes the used geometry key");
    releaseViews(views0);
    edvr::staticSurfaceFrameBoundary(f.context.Get());

    // A frame with no eye-0 owner draw breaks continuity. The next frame is a
    // fresh bootstrap, even though the same depth identity returns.
    f.setup(1, 0);
    f.draw();
    edvr::staticSurfaceFrameBoundary(f.context.Get());
    f.setup(0, 0);
    f.draw();
    check(!edvr::staticSurfaceViews(f.context.Get(), f.scene[0].texture.Get(), views0),
          "gap invalidates consecutive history");
    edvr::staticSurfaceFrameBoundary(f.context.Get());

    // A replacement depth texture/size resets that eye's allocation/history.
    Colour resizedColour = colour(f.device.Get(), 10, 10);
    Depth resizedDepth = depth(f.device.Get(), 10, 10, 0);
    f.setup(0, 0, &resizedColour, &resizedDepth);
    f.draw();
    check(!edvr::staticSurfaceViews(f.context.Get(), resizedDepth.texture.Get(), views0),
          "resized scene depth starts a new history");
    check(edvr::static_surface_detail::eyes[0].width == 10 &&
          edvr::static_surface_detail::eyes[0].height == 10,
          "resized owner textures follow scene dimensions");
    edvr::staticSurfaceFrameBoundary(f.context.Get());

    // Fail-closed gates leave every original binding untouched.
    f.setup(0, 0, &resizedColour, &resizedDepth);
    check(!f.begin(0x123456789abcdef0ull), "unknown VS family declines before patching");

    auto badElements=f.elements;
    badElements[1].InputSlotClass=D3D11_INPUT_PER_INSTANCE_DATA;
    badElements[1].InstanceDataStepRate=1;
    ComPtr<ID3D11InputLayout> badLayout;
    hr(f.device->CreateInputLayout(badElements.data(),UINT(badElements.size()),
                                   f.vsCode->GetBufferPointer(),f.vsCode->GetBufferSize(),&badLayout),
       "create deliberately misclassified layout");
    edvr::staticSurfaceRememberLayout(badLayout.Get(),badElements.data(),UINT(badElements.size()),
                                      0xEB5234DB6ADB491Dull);
    f.context->IASetInputLayout(badLayout.Get());
    check(!f.begin(), "position input mislabeled per-instance declines");
    f.context->IASetInputLayout(f.layout.Get());

    ID3D11Buffer* cb = f.sentinelCb.Get();
    f.context->PSSetConstantBuffers(13, 1, &cb);
    check(!f.begin(), "occupied b13 declines");
    ComPtr<ID3D11Buffer> gotCb;
    f.context->PSGetConstantBuffers(13, 1, &gotCb);
    check(gotCb.Get() == f.sentinelCb.Get(), "occupied b13 is untouched");
    cb = nullptr;
    f.context->PSSetConstantBuffers(13, 1, &cb);

    ID3D11ShaderResourceView* srv = f.sentinelView.Get();
    f.context->PSSetShaderResources(127, 1, &srv);
    check(!f.begin(), "occupied t127 declines");
    ComPtr<ID3D11ShaderResourceView> gotSrv;
    f.context->PSGetShaderResources(127, 1, &gotSrv);
    check(gotSrv.Get() == f.sentinelView.Get(), "occupied t127 is untouched");
    srv = nullptr;
    f.context->PSSetShaderResources(127, 1, &srv);

    f.context->OMSetDepthStencilState(f.depthDisabled.Get(), 0);
    check(!f.begin(), "depth without writes declines");
    f.context->OMSetDepthStencilState(f.depthState.Get(), 0);
    f.context->OMSetBlendState(f.blended.Get(), nullptr, ~0u);
    check(!f.begin(), "blended target 0 declines");
    f.context->OMSetBlendState(f.blend.Get(), nullptr, ~0u);
    f.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    check(!f.begin(), "unsupported topology declines");
}

} // namespace

namespace edvr {

Log& Log::get() { static Log log; return log; }
Log::~Log() = default;
void Log::note(const char*, ...) {}

int64_t qpcNow() { LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart; }
int64_t qpcFrequency() { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return value.QuadPart; }

void* bindingGet(BindSlot slot) { return slot == BindSlot::Dsv0 ? shadowDsv : nullptr; }

bool depthProbeCurrentSceneEyeOf(ID3D11DepthStencilView* dsv, int* eye, int* target) {
    for (const auto& pair : sceneEyes) if (pair.first == dsv) {
        if (eye) *eye = pair.second;
        if (target) *target = pair.second;
        return true;
    }
    return false;
}

void vScreenSetRenderTargetsRaw(ID3D11DeviceContext* context, uint32_t count,
                                ID3D11RenderTargetView* const* rtvs,
                                ID3D11DepthStencilView* dsv) {
    context->OMSetRenderTargets(count, rtvs, dsv);
}

void vScreenPSSetShaderRaw(ID3D11DeviceContext* context, ID3D11PixelShader* ps,
                           ID3D11ClassInstance* const* classes, uint32_t count) {
    context->PSSetShader(ps, classes, count);
}

} // namespace edvr

int main() {
    controllerTests();
    std::printf("Static surface controller checks: %u passed.\n", checks);
    return 0;
}

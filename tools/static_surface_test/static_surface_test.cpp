#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../../src/d3d11/dxbc_static_surface.h"
#include "../../third_party/dxbc_hash/DxilHash.cpp"

using Microsoft::WRL::ComPtr;

namespace {
unsigned checks = 0;

void check(bool value, const char* why) {
    ++checks;
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", why);
        std::exit(1);
    }
}

void hr(HRESULT value, const char* why) { check(SUCCEEDED(value), why); }

ComPtr<ID3DBlob> compile(const char* source, const char* profile) {
    ComPtr<ID3DBlob> code, errors;
    const HRESULT result = D3DCompile(source, std::strlen(source), "static-surface-test",
        nullptr, nullptr, "main", profile, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, &code, &errors);
    if (FAILED(result) && errors)
        std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
    hr(result, "D3DCompile");
    return code;
}

std::vector<BYTE> readFile(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto size = file.tellg();
    if (size <= 0 || size > 1024 * 1024) return {};
    std::vector<BYTE> bytes(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!file) return {};
    return bytes;
}

struct Texture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
};

Texture makeTexture(ID3D11Device* device, DXGI_FORMAT format) {
    Texture result;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 8;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    hr(device->CreateTexture2D(&desc, nullptr, &result.texture), "CreateTexture2D");
    hr(device->CreateRenderTargetView(result.texture.Get(), nullptr, &result.rtv), "CreateRenderTargetView");
    return result;
}

std::vector<BYTE> readTexture(ID3D11Device* device, ID3D11DeviceContext* context,
                              ID3D11Texture2D* source) {
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    hr(device->CreateTexture2D(&desc, nullptr, &staging), "Create staging texture");
    context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map staging texture");
    const size_t pixelBytes = 16;
    std::vector<BYTE> bytes(desc.Width * desc.Height * pixelBytes);
    for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(bytes.data() + y * desc.Width * pixelBytes,
                    static_cast<const BYTE*>(mapped.pData) + y * mapped.RowPitch,
                    desc.Width * pixelBytes);
    context->Unmap(staging.Get(), 0);
    return bytes;
}

void cpuTests(ID3D11Device* device) {
    static const char materialVs[] = R"HLSL(
struct O { nointerpolation uint3 id:__USER_MATERIALMODULATION_DATAID;
           float3 n:__USER_VERTEX_M_LIGHTINGNORMAL; float3 t:__USER_VERTEX_M_LIGHTINGTANGENT;
           float2 uv:__USER_VERTEX_M_TEXCOORD; float4 p:SV_Position; };
O main(uint v:SV_VertexID){O o;o.id=uint3(0,0,0);o.n=float3(0,0,1);o.t=float3(1,0,0);o.uv=0;
o.p=float4(v==2?3:-1,v==1?3:-1,.5,1);return o;})HLSL";
    static const char faceVs[] = R"HLSL(
struct O { nointerpolation uint id:__USER_VERTEX_FACEINVARIANT;
           float3 n:__USER_VERTEX_M_LIGHTINGNORMAL; float3 t:__USER_VERTEX_M_LIGHTINGTANGENT;
           float2 uv:__USER_VERTEX_M_TEXCOORD; float4 p:SV_Position; };
O main(uint v:SV_VertexID){O o;o.id=0;o.n=float3(0,0,1);o.t=float3(1,0,0);o.uv=0;
o.p=float4(v==2?3:-1,v==1?3:-1,.5,1);return o;})HLSL";
    static const char ps[] = R"HLSL(
struct I { nointerpolation uint3 id:__USER_MATERIALMODULATION_DATAID;
           float3 n:__USER_VERTEX_M_LIGHTINGNORMAL; float3 t:__USER_VERTEX_M_LIGHTINGTANGENT;
           float2 uv:__USER_VERTEX_M_TEXCOORD; };
struct O { float4 a:SV_Target0;float4 b:SV_Target1;float4 c:SV_Target2;float4 d:SV_Target3; };
O main(I i){O o;float f=(i.id.y&1)?.25:.5;o.a=float4(f,1,2,3);o.b=2;o.c=3;o.d=4;return o;})HLSL";
    const auto material = compile(materialVs, "vs_5_0");
    const auto face = compile(faceVs, "vs_5_0");
    const auto pixel = compile(ps, "ps_5_0");
    edvr::StaticSurfaceShaderInputs inputs{};
    std::string reason;
    check(edvr::staticSurfaceDeriveShaderInputs(material->GetBufferPointer(), material->GetBufferSize(), inputs, reason), reason.c_str());
    check(inputs.identityRegister == 0 && inputs.identityComponent == 1 && inputs.positionRegister == 4,
          "material signature mapping");
    check(edvr::staticSurfaceDeriveShaderInputs(face->GetBufferPointer(), face->GetBufferSize(), inputs, reason), reason.c_str());
    check(inputs.identityRegister == 0 && inputs.identityComponent == 0 && inputs.positionRegister == 4,
          "face signature mapping");
    edvr::staticSurfaceDeriveShaderInputs(material->GetBufferPointer(), material->GetBufferSize(), inputs, reason);
    std::vector<BYTE> patched;
    check(edvr::staticSurfacePatch(pixel->GetBufferPointer(), pixel->GetBufferSize(), inputs, patched, reason), reason.c_str());
    ComPtr<ID3D11PixelShader> shader;
    hr(device->CreatePixelShader(patched.data(), patched.size(), nullptr, &shader), "Create patched pixel shader");

    auto reject = [&](const char* source, const char* expected) {
        auto code = compile(source, "ps_5_0");
        check(!edvr::staticSurfacePatch(code->GetBufferPointer(), code->GetBufferSize(), inputs, patched, reason), expected);
        check(reason.find(expected) != std::string::npos, expected);
    };
    reject("cbuffer X:register(b13){uint4 x;}struct I{nointerpolation uint3 id:__USER_MATERIALMODULATION_DATAID;};float4 main(I i):SV_Target{return i.id.y+x.x;}", "b13 occupied");
    reject("StructuredBuffer<uint> X:register(t127);struct I{nointerpolation uint3 id:__USER_MATERIALMODULATION_DATAID;};float4 main(I i):SV_Target{return i.id.y+X[0];}", "t127 occupied");
    reject("struct I{nointerpolation uint3 id:__USER_MATERIALMODULATION_DATAID;};float4 main(I i):SV_Target6{return i.id.y;}", "target 6 or 7");
    reject("struct I{nointerpolation uint3 id:__USER_MATERIALMODULATION_DATAID;};float4 main(I i,out float z:SV_Depth):SV_Target{z=.5;return i.id.y;}", "depth output");
    reject("RWStructuredBuffer<uint> U:register(u3);struct I{nointerpolation uint3 id:__USER_MATERIALMODULATION_DATAID;};float4 main(I i):SV_Target{U[0]=i.id.y;return 1;}", "UAV");
    reject("float4 main(float4 p:SV_Position):SV_Target{return p;}", "identity input absent");

    auto noIdentityVs = compile("struct O{float2 uv:TEXCOORD0;float4 p:SV_Position;};O main(uint v:SV_VertexID){O o;o.uv=0;o.p=float4(v==2?3:-1,v==1?3:-1,.5,1);return o;}", "vs_5_0");
    check(!edvr::staticSurfaceDeriveShaderInputs(noIdentityVs->GetBufferPointer(), noIdentityVs->GetBufferSize(), inputs, reason),
          "VS without rigid identity declines");
}

struct Model { uint32_t words[84]{}; };

void gpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    static const char vsSource[] = R"HLSL(
cbuffer V:register(b0){uint Identity;uint3 pad;}
struct O { nointerpolation uint3 id:__USER_MATERIALMODULATION_DATAID;
           float3 n:__USER_VERTEX_M_LIGHTINGNORMAL; float3 t:__USER_VERTEX_M_LIGHTINGTANGENT;
           float2 uv:__USER_VERTEX_M_TEXCOORD; float4 p:SV_Position; };
O main(uint v:SV_VertexID){O o;o.id=uint3(0,Identity,0);o.n=float3(0,0,1);o.t=float3(1,0,0);o.uv=0;
o.p=float4(v==2?3:-1,v==1?3:-1,.5,1);return o;})HLSL";
    static const char psSource[] = R"HLSL(
struct I { nointerpolation uint3 id:__USER_MATERIALMODULATION_DATAID;
           float3 n:__USER_VERTEX_M_LIGHTINGNORMAL; float3 t:__USER_VERTEX_M_LIGHTINGTANGENT;
           float2 uv:__USER_VERTEX_M_TEXCOORD; float4 p:SV_Position; };
struct O { float4 a:SV_Target0;float4 b:SV_Target1;float4 c:SV_Target2;float4 d:SV_Target3; };
O main(I i){if(i.p.x<4)discard;O o;float f=(i.id.y&1)?.25:.5;
o.a=float4(f,.2,.3,1);o.b=float4(.4,.5,.6,1);o.c=float4(.7,.8,.9,1);o.d=float4(1,.75,.5,.25);return o;})HLSL";
    auto vsCode = compile(vsSource, "vs_5_0");
    auto psCode = compile(psSource, "ps_5_0");
    edvr::StaticSurfaceShaderInputs inputs{};
    std::string reason;
    check(edvr::staticSurfaceDeriveShaderInputs(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), inputs, reason), reason.c_str());
    std::vector<BYTE> patched;
    check(edvr::staticSurfacePatch(psCode->GetBufferPointer(), psCode->GetBufferSize(), inputs, patched, reason), reason.c_str());
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> original, tagged;
    hr(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs), "Create VS");
    hr(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &original), "Create original PS");
    hr(device->CreatePixelShader(patched.data(), patched.size(), nullptr, &tagged), "Create tagged PS");

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = 16;
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> vsCb, geometryCb;
    hr(device->CreateBuffer(&cbDesc, nullptr, &vsCb), "Create VS CB");
    hr(device->CreateBuffer(&cbDesc, nullptr, &geometryCb), "Create geometry CB");
    ID3D11Buffer* raw = vsCb.Get();
    context->VSSetConstantBuffers(0, 1, &raw);
    raw = geometryCb.Get();
    context->PSSetConstantBuffers(13, 1, &raw);

    std::array<Model, 2> models{};
    for (auto& model : models) {
        model.words[0] = 0;
        model.words[1] = 0x3f800000u;
        model.words[2] = 0x11112222u;
        model.words[3] = 0x33334444u;
        model.words[4] = 0x41200000u;
        model.words[5] = 0x41a00000u;
        model.words[6] = 0x41f00000u;
    }
    D3D11_BUFFER_DESC poolDesc{};
    poolDesc.ByteWidth = static_cast<UINT>(sizeof(models));
    poolDesc.Usage = D3D11_USAGE_DEFAULT;
    poolDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    poolDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    poolDesc.StructureByteStride = sizeof(Model);
    D3D11_SUBRESOURCE_DATA initial{models.data(), 0, 0};
    ComPtr<ID3D11Buffer> pool;
    ComPtr<ID3D11ShaderResourceView> poolView;
    hr(device->CreateBuffer(&poolDesc, &initial, &pool), "Create pool");
    hr(device->CreateShaderResourceView(pool.Get(), nullptr, &poolView), "Create pool SRV");
    ID3D11ShaderResourceView* rawView = poolView.Get();
    context->PSSetShaderResources(127, 1, &rawView);

    std::array<Texture, 4> baseline, actual;
    for (auto& t : baseline) t = makeTexture(device, DXGI_FORMAT_R32G32B32A32_FLOAT);
    for (auto& t : actual) t = makeTexture(device, DXGI_FORMAT_R32G32B32A32_FLOAT);
    Texture owner = makeTexture(device, DXGI_FORMAT_R32G32B32A32_UINT);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT viewport{0, 0, 8, 8, 0, 1};
    context->RSSetViewports(1, &viewport);
    const float clear[4]{};
    for (auto& t : baseline) context->ClearRenderTargetView(t.rtv.Get(), clear);
    for (auto& t : actual) context->ClearRenderTargetView(t.rtv.Get(), clear);
    context->ClearRenderTargetView(owner.rtv.Get(), clear);
    ID3D11RenderTargetView* baseTargets[4];
    for (size_t i = 0; i < 4; ++i) baseTargets[i] = baseline[i].rtv.Get();
    context->OMSetRenderTargets(4, baseTargets, nullptr);
    context->PSSetShader(original.Get(), nullptr, 0);
    context->Draw(3, 0);
    ID3D11RenderTargetView* taggedTargets[8]{};
    for (size_t i = 0; i < 4; ++i) taggedTargets[i] = actual[i].rtv.Get();
    taggedTargets[7] = owner.rtv.Get();
    context->OMSetRenderTargets(8, taggedTargets, nullptr);
    context->PSSetShader(tagged.Get(), nullptr, 0);
    const uint32_t identity0[4]{};
    const uint32_t geometry[4] = {0x12345678u, 0x23456789u, 0x3456789au, 0};
    context->UpdateSubresource(vsCb.Get(), 0, nullptr, identity0, 0, 0);
    context->UpdateSubresource(geometryCb.Get(), 0, nullptr, geometry, 0, 0);
    context->Draw(3, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    for (size_t i = 0; i < 4; ++i)
        check(readTexture(device, context, baseline[i].texture.Get()) ==
              readTexture(device, context, actual[i].texture.Get()), "MRT0..3 exact parity");
    auto ownerBytes = readTexture(device, context, owner.texture.Get());
    const auto* ownerWords = reinterpret_cast<const uint32_t*>(ownerBytes.data());
    for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
        const uint32_t* p = ownerWords + 4 * (y * 8 + x);
        if (x < 4) check(p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0, "discard leaves owner clear");
        else check((p[0] || p[1] || p[2]) && p[3] == 0x3f000000u, "owner key and depth bits");
    }

    auto drawOwner = [&](uint32_t identity, const std::array<Model, 2>& poolData,
                         const uint32_t drawKey[4]) {
        context->UpdateSubresource(pool.Get(), 0, nullptr, poolData.data(), 0, 0);
        const uint32_t vsData[4] = {identity, 0, 0, 0};
        context->UpdateSubresource(vsCb.Get(), 0, nullptr, vsData, 0, 0);
        context->UpdateSubresource(geometryCb.Get(), 0, nullptr, drawKey, 0, 0);
        context->ClearRenderTargetView(owner.rtv.Get(), clear);
        context->OMSetRenderTargets(8, taggedTargets, nullptr);
        context->Draw(3, 0);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        const auto bytes = readTexture(device, context, owner.texture.Get());
        std::array<uint32_t, 4> value{};
        std::memcpy(value.data(), bytes.data() + 4 * 16, 16); // x=4,y=0: survives discard
        return value;
    };
    const auto base = drawOwner(0, models, geometry);
    const auto samePoseOtherPoolIndex = drawOwner(1, models, geometry);
    check(base == samePoseOtherPoolIndex, "pool index is not identity");
    for (uint32_t index : {1u, 2u, 3u, 4u, 5u, 6u}) {
        auto changed = models;
        changed[0].words[index] ^= 1;
        check(drawOwner(0, changed, geometry) != base, "one-bit raw rigid pose change changes key");
    }
    uint32_t changedGeometry[4] = {geometry[0] ^ 1u, geometry[1], geometry[2], 0};
    check(drawOwner(0, models, changedGeometry) != base, "geometry epoch/key changes owner");
    auto skinned = models;
    skinned[0].words[0] = 1;
    const auto skinnedOwner = drawOwner(0, skinned, geometry);
    check(skinnedOwner[0] == 0 && skinnedOwner[1] == 0 && skinnedOwner[2] == 0 &&
          skinnedOwner[3] == 0x3f000000u, "skinned record emits zero key");
}

std::wstring join(const std::wstring& root, const wchar_t* group, const wchar_t* file) {
    return root + L"\\" + group + L"\\" + file;
}

void corpusTests(ID3D11Device* device, const std::wstring& root) {
    struct VsCase { const wchar_t* name; uint32_t identity, component, position; };
    const VsCase cases[] = {
        {L"vs_EB5234DB6ADB491D.dxbc", 0, 1, 4},
        {L"vs_DE545DC8EE4FBB87.dxbc", 0, 0, 4},
        {L"vs_61AE8EB05FDC18DD.dxbc", 0, 0, 4},
        {L"vs_66DE2CADB1F4AE6B.dxbc", 0, 0, 6},
        {L"vs_AACFDCF2FB9AD809.dxbc", 0, 0, 4},
    };
    std::string reason;
    for (const auto& c : cases) {
        auto bytes = readFile(join(root, L"shaders", c.name));
        if (bytes.empty()) bytes = readFile(join(root, L"pool", c.name));
        check(!bytes.empty(), "real VS corpus present");
        edvr::StaticSurfaceShaderInputs inputs{};
        check(edvr::staticSurfaceDeriveShaderInputs(bytes.data(), bytes.size(), inputs, reason), reason.c_str());
        check(inputs.identityRegister == c.identity && inputs.identityComponent == c.component &&
              inputs.positionRegister == c.position, "real VS signature mapping");
    }
    auto eb = readFile(join(root, L"shaders", L"vs_EB5234DB6ADB491D.dxbc"));
    auto bbe = readFile(join(root, L"shaders", L"vs_BBE58E40FE88EC80.dxbc"));
    edvr::StaticSurfaceShaderInputs ebInputs{}, bbeInputs{};
    check(edvr::staticSurfaceDeriveShaderInputs(eb.data(), eb.size(), ebInputs, reason), reason.c_str());
    check(edvr::staticSurfaceDeriveShaderInputs(bbe.data(), bbe.size(), bbeInputs, reason), reason.c_str());
    struct Pair { const wchar_t* ps; const edvr::StaticSurfaceShaderInputs* inputs; };
    const Pair pairs[] = {
        {L"ps_CB9F297EFF264251.dxbc", &ebInputs},
        {L"ps_9ABF60B4B51F2C1F.dxbc", &ebInputs},
        {L"ps_DB3E8D20CF53FBC0.dxbc", &bbeInputs},
    };
    for (const auto& pair : pairs) {
        auto bytes = readFile(join(root, L"shaders", pair.ps));
        check(!bytes.empty(), "real PS corpus present");
        std::vector<BYTE> patched;
        check(edvr::staticSurfacePatch(bytes.data(), bytes.size(), *pair.inputs, patched, reason), reason.c_str());
        ComPtr<ID3D11PixelShader> shader;
        hr(device->CreatePixelShader(patched.data(), patched.size(), nullptr, &shader), "real patched PS accepted");
    }
    auto noIdentity = readFile(join(root, L"shaders", L"vs_5B4D8E894EEDA8B4.dxbc"));
    edvr::StaticSurfaceShaderInputs absent{};
    check(!edvr::staticSurfaceDeriveShaderInputs(noIdentity.data(), noIdentity.size(), absent, reason),
          "real UV-only VS declines without invented identity");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, &level, &context), "D3D11CreateDevice WARP");
    check(level >= D3D_FEATURE_LEVEL_11_0, "feature level 11");
    cpuTests(device.Get());
    gpuTests(device.Get(), context.Get());
    if (argc == 2) corpusTests(device.Get(), argv[1]);
    std::printf("Static surface patch checks: %u passed%s.\n", checks,
                argc == 2 ? " including real shader corpus" : "");
    return 0;
}

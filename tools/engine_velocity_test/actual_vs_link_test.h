#pragma once

// Drive the captured DE54 vertex shader, not a signature-matching synthetic
// stand-in, into the stock and patched PS91 pixel shaders on WARP. The scene
// and packed vertices are small controlled inputs; only the shader bytecode
// comes from the local game's edvr_logs dump.
#include "corpus_identity.h"
#include "shader_tests.h"

namespace actual_vs_link_test {
using Microsoft::WRL::ComPtr;
using namespace corpus_identity::detail;

struct Vertex {
    uint32_t a[4], b[4], c[4];
};

inline bool drawActualVs(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                 ID3D11VertexShader* vs, ID3D11PixelShader* ps,
                 const DXGI_FORMAT (&formats)[4], bool withSlot,
                 corpus_identity::detail::Frame& out) {
    ComPtr<ID3D11Texture2D> tex[7];
    ComPtr<ID3D11RenderTargetView> rt[7];
    ID3D11RenderTargetView* views[7]{};
    const float slotClear[4] = {-1, 0, 0, 0};
    for (unsigned i = 0; i < 7; ++i) {
        const DXGI_FORMAT fmt = i < 4 ? formats[i] : i == 6 && withSlot ? DXGI_FORMAT_R32G32_FLOAT : DXGI_FORMAT_UNKNOWN;
        if (fmt == DXGI_FORMAT_UNKNOWN) continue;
        tex[i] = texture(dev, fmt, D3D11_BIND_RENDER_TARGET);
        if (!tex[i] || FAILED(dev->CreateRenderTargetView(tex[i].Get(), nullptr, &rt[i]))) return false;
        ctx->ClearRenderTargetView(rt[i].Get(), i == 6 ? slotClear : clearColor(fmt));
        views[i] = rt[i].Get();
    }
    const auto depth = texture(dev, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL);
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ComPtr<ID3D11DepthStencilView> dv;
    ComPtr<ID3D11DepthStencilState> dss;
    ComPtr<ID3D11RasterizerState> rs;
    if (!depth || FAILED(dev->CreateDepthStencilView(depth.Get(), &dd, &dv)) ||
        FAILED(dev->CreateDepthStencilState(&ds, &dss)) || FAILED(dev->CreateRasterizerState(&rd, &rs))) return false;
    ctx->ClearDepthStencilView(dv.Get(), D3D11_CLEAR_DEPTH, 0, 0);
    const D3D11_VIEWPORT vp{0, 0, float(kSize), float(kSize), 0, 1};
    ctx->OMSetRenderTargets(7, views, dv.Get());
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    ctx->OMSetDepthStencilState(dss.Get(), 0);
    ctx->RSSetState(rs.Get());
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->Draw(3, 0);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    for (unsigned i = 0; i < 4; ++i) if (tex[i]) out.target[i] = readback(dev, ctx, tex[i].Get(), 16);
    out.depth = readback(dev, ctx, depth.Get(), 4);
    if (withSlot) out.slot = readback(dev, ctx, tex[6].Get(), 8);
    return !out.depth.empty() && (!withSlot || !out.slot.empty());
}

inline bool run(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                 const std::vector<BYTE>& vsCode, const std::vector<BYTE>& stockCode,
                 const std::vector<BYTE>& patchedCode, void (*check)(bool, const char*)) {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> stock, patched;
    if (FAILED(dev->CreateVertexShader(vsCode.data(), vsCode.size(), nullptr, &vs)) ||
        FAILED(dev->CreatePixelShader(stockCode.data(), stockCode.size(), nullptr, &stock)) ||
        FAILED(dev->CreatePixelShader(patchedCode.data(), patchedCode.size(), nullptr, &patched))) {
        check(false, "actual DE54 VS and PS91 shaders create on WARP"); return false;
    }
    const D3D11_INPUT_ELEMENT_DESC inputs[] = {
        {"INSTANCEANDMODELDATAINDEX", 0, DXGI_FORMAT_R32G32_UINT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"PACKEDVERTEXDATAA", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"PACKEDVERTEXDATAB", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"PACKEDVERTEXDATAC", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ComPtr<ID3D11InputLayout> layout;
    if (FAILED(dev->CreateInputLayout(inputs, 4, vsCode.data(), vsCode.size(), &layout))) {
        check(false, "actual DE54 input layout creates"); return false;
    }
    std::vector<shader_tests::Record> records(16);
    const float identity[4] = {0, 0, 0, 1};
    records[5] = shader_tests::makeRecord(0, 0, 0, 1, identity);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = UINT(records.size() * sizeof(records[0]));
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 336;
    D3D11_SUBRESOURCE_DATA init{records.data(), 0, 0};
    ComPtr<ID3D11Buffer> pool;
    ComPtr<ID3D11ShaderResourceView> poolView;
    if (FAILED(dev->CreateBuffer(&bd, &init, &pool)) ||
        FAILED(dev->CreateShaderResourceView(pool.Get(), nullptr, &poolView))) {
        check(false, "actual DE54 pool creates"); return false;
    }
    float scene[276][4]{};
    scene[270][0] = 1;
    scene[271][1] = 1;
    scene[273][2] = 0.5f;
    scene[273][3] = 1;
    bd = {};
    bd.ByteWidth = sizeof(scene);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA sceneInit{scene, 0, 0};
    ComPtr<ID3D11Buffer> sceneBuffer;
    if (FAILED(dev->CreateBuffer(&bd, &sceneInit, &sceneBuffer))) {
        check(false, "actual DE54 scene constants create"); return false;
    }
    const auto packed = [](int x, int y) {
        const uint32_t px = uint32_t((x + 1000) * 65535 / 2000);
        const uint32_t py = uint32_t((y + 1000) * 65535 / 2000);
        return px | (py << 16);
    };
    const Vertex verts[3] = {
        {{packed(-800, -800), 4096u << 16 | 32768u, 64u << 24, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
        {{packed(800, -800), 4096u << 16 | 32768u, 64u << 24, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
        {{packed(0, 800), 4096u << 16 | 32768u, 64u << 24, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    };
    const uint32_t instance[2] = {5, 0};
    bd = {};
    bd.ByteWidth = sizeof(verts);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vertexInit{verts, 0, 0};
    ComPtr<ID3D11Buffer> vertexBuffer, instanceBuffer;
    if (FAILED(dev->CreateBuffer(&bd, &vertexInit, &vertexBuffer))) {
        check(false, "actual DE54 packed vertices create"); return false;
    }
    bd.ByteWidth = sizeof(instance);
    D3D11_SUBRESOURCE_DATA instanceInit{instance, 0, 0};
    if (FAILED(dev->CreateBuffer(&bd, &instanceInit, &instanceBuffer))) {
        check(false, "actual DE54 instance index creates"); return false;
    }
    std::vector<Decl> decls;
    DXGI_FORMAT formats[4]{};
    std::string why;
    if (!declarations(stockCode, decls, why) || !outputs(stockCode, formats, why)) {
        check(false, why.c_str()); return false;
    }
    unsigned compared = 0, covered = 0, goodSlots = 0, badSlots = 0;
    corpus_identity::detail::Frame frontStock;
    for (unsigned winding = 0; winding < 2; ++winding) {
        ctx->ClearState();
        for (const auto& d : decls) if (!bind(dev, ctx, d, winding, why)) {
            check(false, why.c_str()); return false;
        }
        // The actual PS91 alpha test reads CB2[0].x. Admit both windings while
        // the patterned shader resources still exercise its other branches.
        float material[8]{};
        material[0] = -1000;
        bd = {};
        bd.ByteWidth = sizeof(material);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA materialInit{material, 0, 0};
        ComPtr<ID3D11Buffer> materialBuffer;
        if (FAILED(dev->CreateBuffer(&bd, &materialInit, &materialBuffer))) {
            check(false, "actual PS91 material constants create"); return false;
        }
        ctx->PSSetConstantBuffers(2, 1, materialBuffer.GetAddressOf());
        ctx->VSSetConstantBuffers(1, 1, sceneBuffer.GetAddressOf());
        ctx->VSSetShaderResources(33, 1, poolView.GetAddressOf());
        ctx->IASetInputLayout(layout.Get());
        ID3D11Buffer* buffers[2] = {vertexBuffer.Get(), instanceBuffer.Get()};
        const UINT strides[2] = {sizeof(Vertex), sizeof(instance)};
        const UINT offsets[2] = {0, 0};
        ctx->IASetVertexBuffers(0, 2, buffers, strides, offsets);
        // Reversing the order makes the rasterizer provide the opposite
        // SV_IsFrontFace at v4 without changing any shader or pool record.
        // draw() uses Draw(3); reorder the vertex buffer for the back face.
        // Preserve the same packed geometry while changing triangle winding.
        if (winding) {
            const Vertex reversed[3] = {verts[0], verts[2], verts[1]};
            ctx->UpdateSubresource(vertexBuffer.Get(), 0, nullptr, reversed, 0, 0);
        }
        corpus_identity::detail::Frame a, b;
        if (!drawActualVs(dev, ctx, vs.Get(), stock.Get(), formats, false, a) ||
            !drawActualVs(dev, ctx, vs.Get(), patched.Get(), formats, true, b)) {
            check(false, "actual DE54/PS91 draws read back"); return false;
        }
        for (unsigned t = 0; t < 4; ++t) if (formats[t] != DXGI_FORMAT_UNKNOWN) {
            compared += kSize * kSize;
            check(a.target[t] == b.target[t], "actual VS: stock and patched game target match byte for byte");
        }
        compared += kSize * kSize;
        check(a.depth == b.depth, "actual VS: stock and patched depth match byte for byte");
        for (unsigned p = 0; p < kSize * kSize; ++p) {
            float z, slot[2];
            std::memcpy(&z, &a.depth[p * 4], 4);
            std::memcpy(slot, &b.slot[p * 8], 8);
            if (z <= 0) continue;
            ++covered;
            uint32_t zBits, slotBits;
            std::memcpy(&zBits, &a.depth[p * 4], 4);
            std::memcpy(&slotBits, &b.slot[p * 8 + 4], 4);
            if (slot[0] == 11 && zBits == slotBits) ++goodSlots;
            else ++badSlots;
        }
        if (!winding) frontStock = std::move(a);
        else check(frontStock.target[0] != a.target[0], "actual PS91 front-face input changes the stock result");
    }
    check(covered > 0 && goodSlots == covered && badSlots == 0,
          "actual VS: covered texels have exact pool slot and depth in MRT6");
    std::printf("  actual DE54->PS91 link: %u game/depth texels compared, %u covered, MRT6 %u exact, %u bad, both front faces\n",
                compared, covered, goodSlots, badSlots);
    ctx->ClearState();
    return covered > 0 && badSlots == 0;
}
} // namespace actual_vs_link_test

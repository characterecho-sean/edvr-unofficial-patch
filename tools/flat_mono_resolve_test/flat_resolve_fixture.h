// The pieces of a resolver call that the VR world route's cases share (flat_upscaler_slot_gpu_tests.h and
// flat_first_person_gpu_tests.h): the engine inputs the prep kernel reads -- no engine slot anywhere, so the camera term moves
// every pixel; one empty pool record; scene constants for a still camera -- and a game pipeline the resolver must hand back
// untouched, including the two compute shader-resource slots (t9 and t10) the first-person inputs are bound to.
#pragma once

struct ResolveFixture {
    static constexpr UINT w = 16, h = 16;
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ComPtr<ID3D11Texture2D> slotTexture, sampled, other;
    ComPtr<ID3D11Buffer> pool, now, old;
    ComPtr<ID3D11ShaderResourceView> slotView, poolView, gameSrv;
    ComPtr<ID3D11RenderTargetView> gameRtv;
    D3D11_VIEWPORT viewport{3, 4, 19, 21, .2f, .8f};

    ResolveFixture(ID3D11Device* d, ID3D11DeviceContext* c) : device(d), context(c) {
        std::vector<float> slots(w * h * 2);
        for (size_t i = 0; i < slots.size(); i += 2) { slots[i] = -1; slots[i + 1] = .01f; }
        slotTexture = texture(d, w, h, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE, slots.data(), w * 8);
        slotView = view(d, slotTexture.Get());
        uint32_t record[84]{};
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(record); bd.StructureByteStride = 336;
        bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem = record;
        check(SUCCEEDED(d->CreateBuffer(&bd, &initial, pool.GetAddressOf())), "world route fixture: pool buffer");
        poolView = view(d, pool.Get());
        constexpr uint32_t kStamp = 77;   // the freshness stamp EN[276].x; no record joins it here
        float scene[277][4]{}; float cam[6][4]; camera(cam); std::memcpy(scene + 270, cam, sizeof(cam));
        { const uint32_t stamp = kStamp; std::memcpy(&scene[276][0], &stamp, 4); }
        bd = {}; bd.ByteWidth = sizeof(scene); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        initial.pSysMem = scene;
        check(SUCCEEDED(d->CreateBuffer(&bd, &initial, now.GetAddressOf())) &&
              SUCCEEDED(d->CreateBuffer(&bd, &initial, old.GetAddressOf())), "world route fixture: engine camera buffers");
        // The game's pipeline: a render target, a shader resource over ANOTHER texture (one resource bound as both would have
        // the runtime unbind one of them), a compute constant buffer and a viewport.
        other = texture(d, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
        sampled = texture(d, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        gameSrv = view(d, sampled.Get());
        check(SUCCEEDED(d->CreateRenderTargetView(other.Get(), nullptr, gameRtv.GetAddressOf())), "world route fixture: original RTV");
    }
    void engine(edvr::FlatMonoResolveFrame& f) const { f.engine = {slotView.Get(), poolView.Get(), now.Get(), old.Get()}; }
    void bindOriginal() {
        ID3D11RenderTargetView* rt = gameRtv.Get(); context->OMSetRenderTargets(1, &rt, nullptr);
        ID3D11ShaderResourceView* srv = gameSrv.Get();
        context->PSSetShaderResources(0, 1, &srv); context->VSSetShaderResources(3, 1, &srv);
        // The game's own compute views at t9 and t10: where the resolver's prep binds the first-person pair.
        context->CSSetShaderResources(9, 1, &srv); context->CSSetShaderResources(10, 1, &srv);
        ID3D11Buffer* cb = old.Get(); context->CSSetConstantBuffers(4, 1, &cb); context->RSSetViewports(1, &viewport);
    }
    bool restored() {
        ComPtr<ID3D11RenderTargetView> rt; ComPtr<ID3D11ShaderResourceView> ps, vs, cs[2]; ComPtr<ID3D11Buffer> cb;
        context->OMGetRenderTargets(1, rt.GetAddressOf(), nullptr); context->PSGetShaderResources(0, 1, ps.GetAddressOf());
        context->VSGetShaderResources(3, 1, vs.GetAddressOf());
        ID3D11ShaderResourceView* both[2]{}; context->CSGetShaderResources(9, 2, both); cs[0].Attach(both[0]); cs[1].Attach(both[1]);
        context->CSGetConstantBuffers(4, 1, cb.GetAddressOf());
        UINT count = 1; D3D11_VIEWPORT current{}; context->RSGetViewports(&count, &current);
        return rt.Get() == gameRtv.Get() && ps.Get() == gameSrv.Get() && vs.Get() == gameSrv.Get() &&
               cs[0].Get() == gameSrv.Get() && cs[1].Get() == gameSrv.Get() && cb.Get() == old.Get() &&
               count == 1 && !std::memcmp(&current, &viewport, sizeof(viewport));
    }
};

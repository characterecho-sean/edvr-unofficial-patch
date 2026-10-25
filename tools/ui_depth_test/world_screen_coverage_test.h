// F105: the in-world screens' coverage (src\d3d11\world_screen_coverage.h), on WARP with the production stand-in
// (kWorldScreenCoverageBytecode), the production mask states (maskDepthState, maskBlend) and the production pass
// (WorldScreenCoverage). Included by ui_depth_test.cpp after its helpers.
//
// What it holds: the classifier names the exact pair and nothing else; the line carries its counts; the stand-in's
// input signature is the game pixel shader's, element for element (PS EBA95E15B0A66102 as the 2026-10-03 shader dump
// disassembles it), so the vertex stage that feeds the game's shader links to it (the debug layer, when installed,
// fails the rig on a linkage warning); the second issue marks the masks exactly where the screen won the depth test,
// with the values the UI resolve reads, writes no game target and no depth, and puts every binding back; and a draw
// whose state could not be put back exactly is refused without a change.
void testWorldScreenCoverage(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    // ---- the classifier and the line ----
    check(worldScreenDraw(kWorldScreenVs, kWorldScreenPs, true, true, true), "F105: the pair, a composite in scene depth, trained");
    check(!worldScreenDraw(kWorldScreenVs, kWorldScreenPs, false, true, true), "F105: not without a learned surface bound");
    check(!worldScreenDraw(kWorldScreenVs, kWorldScreenPs, true, false, true), "F105: not outside the scene's depth");
    check(!worldScreenDraw(kWorldScreenVs, kWorldScreenPs, true, true, false), "F105: not without an external upscaler");
    check(!worldScreenDraw(kWorldScreenVs, 0xB46E52A1E0B2F39Cull, true, true, true), "F105: not the station's own pixel shader");
    check(!worldScreenDraw(0x025B4B9FF54622EDull, kWorldScreenPs, true, true, true), "F105: not another vertex family");
    {
        WorldScreenWindow w;
        w.frames = 1800; w.seen = 7200; w.marked = 7190; w.eye[0] = 3595; w.eye[1] = 3595;
        w.refuse(WorldScreenRefusal::kNoEye); w.refuse(WorldScreenRefusal::kState); w.refuse(WorldScreenRefusal::kState);
        char line[600] = {};
        const int n = worldScreenFormatWindow(line, sizeof(line), w);
        check(n > 0 && n < static_cast<int>(sizeof(line)), "F105: the line fits its buffer");
        check(std::strstr(line, "ui depth world screens (F105): 1800 frames, seen 7200, marked 7190 (eye 0 3595, eye 1 3595); "
                                "refused: no eye 1, no mask 0, no shader 0, state 2.") == line,
              "F105: the line's counts, in the order the reader parses them");
        w.reset();
        check(w.frames == 0 && w.seen == 0 && w.marked == 0 && w.refused[3] == 0, "F105: the window resets");
        check(std::strcmp(worldScreenRefusalName(WorldScreenRefusal::kShader), "no shader") == 0, "F105: refusal names");
    }

    // ---- the stand-in's signature is the game shader's ----
    {
        ComPtr<ID3D11ShaderReflection> refl;
        hr(D3DReflect(kWorldScreenCoverageBytecode, sizeof(kWorldScreenCoverageBytecode), __uuidof(ID3D11ShaderReflection),
                      reinterpret_cast<void**>(refl.GetAddressOf())));
        D3D11_SHADER_DESC desc{};
        hr(refl->GetDesc(&desc));
        struct Want { const char* name; UINT reg; BYTE mask; D3D_REGISTER_COMPONENT_TYPE type; };
        // PS EBA95E15B0A66102's input signature (edvr_logs\shaders\ps_EBA95E15B0A66102.dxbc, 3616 bytes, disassembled).
        const Want game[4] = {
            {"__USER_VERTEX_FACEINVARIANT", 0, 0x3, D3D_REGISTER_COMPONENT_UINT32},
            {"__USER_VERTEX_M_LIGHTINGNORMAL", 1, 0x7, D3D_REGISTER_COMPONENT_FLOAT32},
            {"__USER_VERTEX_M_LIGHTINGTANGENT", 2, 0x7, D3D_REGISTER_COMPONENT_FLOAT32},
            {"__USER_VERTEX_M_TEXCOORD", 3, 0x3, D3D_REGISTER_COMPONENT_FLOAT32},
        };
        check(desc.InputParameters == 4, "F105: the stand-in takes the game shader's four inputs");
        for (UINT i = 0; i < 4 && i < desc.InputParameters; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC p{};
            hr(refl->GetInputParameterDesc(i, &p));
            check(std::strcmp(p.SemanticName, game[i].name) == 0 && p.SemanticIndex == 0 && p.Register == game[i].reg &&
                      p.Mask == game[i].mask && p.ComponentType == game[i].type && p.SystemValueType == D3D_NAME_UNDEFINED,
                  "F105: each input element is the game shader's (name, index, register, mask, type)");
        }
        check(desc.OutputParameters == 2, "F105: the stand-in writes two targets");
        for (UINT i = 0; i < 2 && i < desc.OutputParameters; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC p{};
            hr(refl->GetOutputParameterDesc(i, &p));
            check(std::strcmp(p.SemanticName, "SV_Target") == 0 && p.SemanticIndex == (i ? 2u : 0u) && p.Register == (i ? 2u : 0u),
                  "F105: the coverage mask at target 0, the edit mask at target 2");
        }
        check(desc.BoundResources == 0, "F105: the stand-in reads no resource");
    }

    // ---- the pass on WARP ----
    ctx->ClearState();
    // The game's vertex stage, with VS 889A5279E68F0672's output signature: the four elements, then SV_POSITION.
    auto vsCode = compile(R"HLSL(
struct O {
    nointerpolation uint2 f : __USER_VERTEX_FACEINVARIANT;
    float3 n : __USER_VERTEX_M_LIGHTINGNORMAL;
    float3 t : __USER_VERTEX_M_LIGHTINGTANGENT;
    float2 uv : __USER_VERTEX_M_TEXCOORD;
    float4 p : SV_POSITION;
};
O main(float3 p : POSITION) { O o = (O)0; o.uv = p.xy; o.n = float3(0, 0, 1); o.t = float3(1, 0, 0); o.p = float4(p, 1); return o; }
)HLSL", "vs_5_0");
    // The game's G-buffer pixel stage: the same inputs, four targets.
    auto gamePsCode = compile(R"HLSL(
struct I {
    nointerpolation uint2 f : __USER_VERTEX_FACEINVARIANT;
    float3 n : __USER_VERTEX_M_LIGHTINGNORMAL;
    float3 t : __USER_VERTEX_M_LIGHTINGTANGENT;
    float2 uv : __USER_VERTEX_M_TEXCOORD;
};
struct O { float4 a : SV_Target0; float4 b : SV_Target1; float4 c : SV_Target2; float4 d : SV_Target3; };
O main(I i) { O o; o.a = float4(.1, .2, .3, 1); o.b = float4(.4, .5, .6, 1); o.c = float4(.7, .8, .9, 1); o.d = float4(.25, .5, .75, 1); return o; }
)HLSL", "ps_5_0");
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> gamePs, standIn;
    hr(dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
    hr(dev->CreatePixelShader(gamePsCode->GetBufferPointer(), gamePsCode->GetBufferSize(), nullptr, &gamePs));
    hr(dev->CreatePixelShader(kWorldScreenCoverageBytecode, sizeof(kWorldScreenCoverageBytecode), nullptr, &standIn));
    D3D11_INPUT_ELEMENT_DESC input{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    ComPtr<ID3D11InputLayout> layout;
    hr(dev->CreateInputLayout(&input, 1, vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &layout));
    // Two quads in NDC: an occluder over pixels 2..3 at depth .9 (reverse Z: nearer), and the screen over pixels 0..3 at .5.
    const float verts[] = {
        -.5f, -1, .9f,   0, -1, .9f,   -.5f, 1, .9f,   -.5f, 1, .9f,   0, -1, .9f,   0, 1, .9f,
        -1, -1, .5f,     0, -1, .5f,   -1, 1, .5f,     -1, 1, .5f,     0, -1, .5f,   0, 1, .5f,
    };
    D3D11_BUFFER_DESC vbd{}; vbd.ByteWidth = sizeof(verts); vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vinit{verts, 0, 0};
    ComPtr<ID3D11Buffer> vb; hr(dev->CreateBuffer(&vbd, &vinit, &vb));
    auto colourTarget = [&](DXGI_FORMAT format, ComPtr<ID3D11Texture2D>& tex, ComPtr<ID3D11RenderTargetView>& rtv) {
        D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = 8; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = format; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        hr(dev->CreateTexture2D(&td, nullptr, &tex)); hr(dev->CreateRenderTargetView(tex.Get(), nullptr, &rtv));
    };
    ComPtr<ID3D11Texture2D> gbuf[4], maskTex, editTex; ComPtr<ID3D11RenderTargetView> gbufRtv[4], maskRtv, editRtv;
    for (int i = 0; i < 4; ++i) colourTarget(DXGI_FORMAT_R8G8B8A8_UNORM, gbuf[i], gbufRtv[i]);
    colourTarget(DXGI_FORMAT_R8_UNORM, maskTex, maskRtv);
    colourTarget(DXGI_FORMAT_R8_UNORM, editTex, editRtv);
    auto scene = depth(dev, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT);
    D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    ComPtr<ID3D11DepthStencilState> gameDepth; hr(dev->CreateDepthStencilState(&dd, &gameDepth));
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> raster; hr(dev->CreateRasterizerState(&rd, &raster));
    D3D11_BLEND_DESC bd{}; bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> gameBlend; hr(dev->CreateBlendState(&bd, &gameBlend));
    const float gameFactors[4] = {.1f, .2f, .3f, .4f};
    const UINT gameSampleMask = 0xA5A5A5A5u;   // bit 0 set: the one sample of these targets is written

    ID3D11DepthStencilState* test = maskDepthState(ctx);
    ID3D11BlendState* blend = maskBlend(ctx);
    check(test && blend, "F105: the production mask states");
    for (int occluded = 0; occluded < 2; ++occluded) {
        ctx->ClearState();
        UINT stride = 12, offset = 0;
        ctx->IASetInputLayout(layout.Get()); ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
        ctx->VSSetShader(vs.Get(), nullptr, 0); ctx->PSSetShader(gamePs.Get(), nullptr, 0);
        ctx->RSSetState(raster.Get()); D3D11_VIEWPORT vp{0, 0, 8, 8, 0, 1}; ctx->RSSetViewports(1, &vp);
        ID3D11RenderTargetView* gameTargets[4] = {gbufRtv[0].Get(), gbufRtv[1].Get(), gbufRtv[2].Get(), gbufRtv[3].Get()};
        ctx->OMSetRenderTargets(4, gameTargets, scene.dsv.Get());
        ctx->OMSetDepthStencilState(gameDepth.Get(), 3);
        ctx->OMSetBlendState(gameBlend.Get(), gameFactors, gameSampleMask);
        const float zero[4] = {};
        for (auto& r : gbufRtv) ctx->ClearRenderTargetView(r.Get(), zero);
        ctx->ClearRenderTargetView(maskRtv.Get(), zero); ctx->ClearRenderTargetView(editRtv.Get(), zero);
        ctx->ClearDepthStencilView(scene.dsv.Get(), D3D11_CLEAR_DEPTH, 0, 0);
        if (occluded) ctx->Draw(6, 0);   // the occluder, drawn first
        ctx->Draw(6, 6);                 // the screen: the game's draw
        std::vector<float> before[4];
        for (int i = 0; i < 4; ++i) before[i] = read(dev, ctx, gbuf[i].Get());
        const auto depthBefore = read(dev, ctx, scene.tex.Get());
        bool premise = true;   // the game's own draws wrote what the case says: the screen where it won, the occluder nearer
        for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
            const float want = x < 4 ? (occluded && x >= 2 ? .9f : .5f) : 0.0f;
            premise = premise && std::fabs(depthBefore[y * 8 + x] - want) < 1e-6f;
        }
        check(premise, "F105: the rig's game draws wrote the screen's and the occluder's depth");

        WorldScreenCoverage pass;
        check(pass.begin(ctx, standIn.Get(), maskRtv.Get(), editRtv.Get(), test, blend), "F105: the pass begins for the game's draw");
        ctx->Draw(6, 6);                 // the second issue
        pass.end(ctx);
        check(!pass.active(), "F105: the pass ends");

        for (int i = 0; i < 4; ++i) check(before[i] == read(dev, ctx, gbuf[i].Get()), "F105: no game target is written");
        check(depthBefore == read(dev, ctx, scene.tex.Get()), "F105: the game's depth is not written");
        const auto mask = read(dev, ctx, maskTex.Get()), edit = read(dev, ctx, editTex.Get());
        bool exact = true;
        for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
            const bool screen = x < (occluded ? 2u : 4u);
            exact = exact && std::fabs(mask[y * 8 + x] * 255.0f - (screen ? float(kWorldScreenMaskValue) : 0.0f)) < .5f &&
                    std::fabs(edit[y * 8 + x] * 255.0f - (screen ? float(kWorldScreenEditValue) : 0.0f)) < .5f;
        }
        if (!exact) {   // the grids, so a failure says what was marked (mask / edit, row by row)
            std::printf("F105 rig, occluded %d: the game's depth, row 0:", occluded);
            for (unsigned x = 0; x < 8; ++x) std::printf(" %.3f", depthBefore[x]);
            std::printf("\nF105 rig, occluded %d: mask / edit as written\n", occluded);
            for (unsigned y = 0; y < 8; ++y) {
                for (unsigned x = 0; x < 8; ++x) std::printf("%4.0f", mask[y * 8 + x] * 255.0f);
                std::printf("   /");
                for (unsigned x = 0; x < 8; ++x) std::printf("%4.0f", edit[y * 8 + x] * 255.0f);
                std::printf("\n");
            }
            ComPtr<ID3D11InfoQueue> q;
            if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&q)))) for (UINT64 i = 0; i < q->GetNumStoredMessages(); ++i) {
                SIZE_T size = 0; q->GetMessage(i, nullptr, &size); std::vector<unsigned char> storage(size);
                auto* m = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
                if (SUCCEEDED(q->GetMessage(i, m, &size))) std::printf("debug layer: %s\n", m->pDescription);
            }
        }
        for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
            const bool screen = x < (occluded ? 2u : 4u);
            const float m = mask[y * 8 + x] * 255.0f, e = edit[y * 8 + x] * 255.0f;
            check(std::fabs(m - (screen ? float(kWorldScreenMaskValue) : 0.0f)) < .5f,
                  "F105: the coverage mask holds the attached value exactly where the screen won the depth test");
            check(std::fabs(e - (screen ? float(kWorldScreenEditValue) : 0.0f)) < .5f,
                  "F105: the edit mask is marked exactly where the coverage mask is");
        }
        // The game's bindings, back.
        ComPtr<ID3D11PixelShader> ps; ctx->PSGetShader(&ps, nullptr, nullptr);
        ID3D11RenderTargetView* rtvs[8] = {}; ComPtr<ID3D11DepthStencilView> dsv;
        ctx->OMGetRenderTargets(8, rtvs, &dsv);
        bool targetsBack = dsv == scene.dsv;
        for (int i = 0; i < 8; ++i) {
            targetsBack = targetsBack && rtvs[i] == (i < 4 ? gameTargets[i] : nullptr);
            if (rtvs[i]) rtvs[i]->Release();
        }
        ComPtr<ID3D11DepthStencilState> ds; UINT ref = 0; ctx->OMGetDepthStencilState(&ds, &ref);
        ComPtr<ID3D11BlendState> bs; float factors[4] = {}; UINT sampleMask = 0; ctx->OMGetBlendState(&bs, factors, &sampleMask);
        check(ps == gamePs && targetsBack, "F105: the game's pixel shader and all four targets with its depth, back");
        check(ds == gameDepth && ref == 3 && bs == gameBlend && sampleMask == gameSampleMask &&
                  std::memcmp(factors, gameFactors, sizeof(factors)) == 0,
              "F105: the game's depth state, stencil reference, blend, factors and sample mask, back");
    }

    // ---- refused, changing nothing: an unordered-access view beside the targets ----
    {
        ctx->ClearState();
        D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = 8; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R32_FLOAT; td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> uavTex; ComPtr<ID3D11UnorderedAccessView> uav;
        hr(dev->CreateTexture2D(&td, nullptr, &uavTex)); hr(dev->CreateUnorderedAccessView(uavTex.Get(), nullptr, &uav));
        ID3D11RenderTargetView* one = gbufRtv[0].Get();
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, &one, scene.dsv.Get(), 1, 1, uav.GetAddressOf(), nullptr);
        ctx->PSSetShader(gamePs.Get(), nullptr, 0);
        WorldScreenCoverage pass;
        check(!pass.begin(ctx, standIn.Get(), maskRtv.Get(), editRtv.Get(), test, blend),
              "F105: refused with an unordered-access view bound beside the targets");
        ComPtr<ID3D11PixelShader> ps; ctx->PSGetShader(&ps, nullptr, nullptr);
        ComPtr<ID3D11UnorderedAccessView> still;
        ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1, 1, &still);
        check(ps == gamePs && still == uav && !pass.active(), "F105: a refusal changes nothing");
        // ...and without a depth target.
        ctx->ClearState();
        ctx->OMSetRenderTargets(1, &one, nullptr);
        check(!pass.begin(ctx, standIn.Get(), maskRtv.Get(), editRtv.Get(), test, blend), "F105: refused without a depth target");
        check(!pass.begin(ctx, nullptr, maskRtv.Get(), editRtv.Get(), test, blend), "F105: refused without the stand-in");
    }
    ctx->ClearState();
}

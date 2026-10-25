// F105 and F108: the in-world screens' coverage (src\d3d11\world_screen_coverage.h), on WARP with the production stand-ins
// (kWorldScreenCoverageBytecode, the held one; kWorldScreenTrackedBytecode, F108's), the production mask states (maskDepthState,
// maskBlend), the production pass (WorldScreenCoverage), UiContent's array path, the census (WorldScreenCensusPass and
// WorldScreenCensus) and the motion pass (kTemporalMvFastBytecode). Included by ui_depth_test.cpp after zscene_gate_test.h,
// whose texture helpers it uses.
//
// What it holds. F105: the classifier names the exact pair and nothing else; the line carries its counts; each stand-in's
// input signature is the game pixel shader's, element for element (PS EBA95E15B0A66102 as the 2026-10-03 shader dump
// disassembles it), so the vertex stage that feeds the game's shader links to it (the debug layer, when installed, fails the
// rig on a linkage warning); the second issue marks the masks exactly where the screen won the depth test, writes no game
// target and no depth, and puts every binding back; a draw whose state could not be put back exactly is refused without a
// change. F108: the classification's refusals are named in order; a static slice gives no edit after the expiry; a changed
// region is edited only there, in its own slice; a solid-colour instance gives no edit; the fresh-edit bit (6) appears only
// on an edit's first frame; the census counts the states and classifies rest and change; the motion pass writes the
// history-invalidate value only where the coverage carries the fresh bit. F108b: a block is held exactly kWorldScreenHoldFrames
// frames from its last change and at rest after, while UiContent's age still lasts; a block that changes again is held again, and a
// change within the hold restarts it; the census counts a burst of changes once; a 2D interface's ages reach the stand-in as a
// one-slice array view. F116: the census counts each eye's screen pixels apart (the terminal hide's footprint).
namespace wstest {
// Every slice of a 2D or array texture of 1-byte texels (R8_UNORM), as floats in 0..1, slice by slice.
std::vector<std::vector<float>> readSlices8(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* t) {
    D3D11_TEXTURE2D_DESC td{}; t->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> stage; hr(dev->CreateTexture2D(&td, nullptr, &stage));
    ctx->CopyResource(stage.Get(), t);
    std::vector<std::vector<float>> out(td.ArraySize);
    for (UINT s = 0; s < td.ArraySize; ++s) {
        D3D11_MAPPED_SUBRESOURCE m{};
        const UINT sub = D3D11CalcSubresource(0, s, td.MipLevels);
        hr(ctx->Map(stage.Get(), sub, D3D11_MAP_READ, 0, &m));
        out[s].resize(size_t(td.Width) * td.Height);
        for (UINT y = 0; y < td.Height; ++y) for (UINT x = 0; x < td.Width; ++x)
            out[s][size_t(y) * td.Width + x] = static_cast<const unsigned char*>(m.pData)[size_t(y) * m.RowPitch + x] / 255.0f;
        ctx->Unmap(stage.Get(), sub);
    }
    return out;
}
std::vector<std::vector<float>> agesOf(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* v) {
    check(v != nullptr, "F108: UiContent returned the array's ages");
    ComPtr<ID3D11Resource> r; v->GetResource(&r);
    ComPtr<ID3D11Texture2D> t; hr(r.As(&t));
    return readSlices8(dev, ctx, t.Get());
}
}  // namespace wstest

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
    // F108: the classification's refusals, the first failed test named.
    check(worldScreenClassRefusal(true, true, true) == WorldScreenClassRefusal::kNone, "F108: a screen F105 takes is no refusal");
    check(worldScreenClassRefusal(false, false, false) == WorldScreenClassRefusal::kNoSurface, "F108: no learned surface is named first");
    check(worldScreenClassRefusal(true, false, false) == WorldScreenClassRefusal::kNotSceneDepth, "F108: then not the scene's depth");
    check(worldScreenClassRefusal(true, true, false) == WorldScreenClassRefusal::kNotTrained, "F108: then no external upscaler");
    check(std::strcmp(worldScreenClassRefusalName(WorldScreenClassRefusal::kNotSceneDepth), "not the scene's depth") == 0,
          "F108: classification refusal names");
    // F108: the census, frame by frame. Before any change every frame is at rest; a change's first frame and the N - 1 after it are
    // during it (N = kWorldScreenHoldFrames, F108b); the Nth after it is at rest again; a frame with no screen is not counted; the
    // window boundary keeps the change.
    {
        constexpr uint64_t N = kWorldScreenHoldFrames;
        WorldScreenCensus c;
        c.add(100, 1000, 0, 0);
        c.add(101, 1000, 300, 300);
        c.add(101 + N - 2, 1000, 300, 0);
        c.add(101 + N - 1, 1000, 300, 0);
        c.add(101 + N, 1000, 10, 0);
        c.add(102 + N, 1000, 0, 0);
        c.add(103 + N, 0, 0, 0);
        check(c.frames == 6 && c.editFrames == 4 && c.changeFrames == 1 && c.freshPx == 300 && c.changes == 1,
              "F108 census: frames, edits, changes, restarts");
        check(c.restFrames == 3 && c.restScreen == 3000 && c.restEdited == 10, "F108b census: before the change and from the hold's end, at rest");
        check(c.duringFrames == 3 && c.duringScreen == 3000 && c.duringEdited == 900, "F108b census: a change's first frame and N - 1 after, during");
        c.resetWindow();
        check(c.frames == 0 && c.restFrames == 0 && c.freshPx == 0 && c.changes == 0 && c.haveChange && c.lastChange == 101,
              "F108 census: a window keeps the last change");
        c.add(101 + N - 1, 500, 50, 0);
        check(c.duringFrames == 1 && c.restFrames == 0, "F108 census: a frame after the boundary within the hold of the change is during it");
        // F108b: changes from rest. A keystroke whose interface animates three frames in a row is one change; a change within the hold
        // of the last is the same change; one that begins N or more frames after the last begins another.
        WorldScreenCensus k;
        k.add(300, 1000, 100, 100); k.add(301, 1000, 100, 100); k.add(302, 1000, 100, 100);
        k.add(302 + N - 1, 1000, 100, 100);
        k.add(302 + N - 1 + N, 1000, 100, 100);
        check(k.changeFrames == 5 && k.changes == 2, "F108b census: changes from rest count a burst once, and a change after the hold again");
        WorldScreenCensus held;   // F105's rule: every screen pixel edited every frame, no change ever begins
        for (uint64_t f = 200; f < 240; ++f) held.add(f, 1000, 1000, 0);
        check(held.restFrames == 40 && held.restEdited == held.restScreen && held.changeFrames == 0,
              "F108 census: F105's rule reads as every pixel edited at rest");
        WorldScreenWindow w;
        w.frames = 1800; w.tracked = 900; w.held = 2;
        w.classRefused[static_cast<size_t>(WorldScreenClassRefusal::kNotSceneDepth)] = 7;
        char line[900] = {};
        const int n = worldScreenFormatF108(line, sizeof(line), w, held, "format 2");
        check(n > 0 && n < static_cast<int>(sizeof(line)), "F108: the line fits its buffer");
        char want[900] = {};
        std::snprintf(want, sizeof(want), "ui depth world screens (F108): 1800 frames, a change held %u frames; draws tracked 900, held without "
                                          "change tracking 2 (format 2); not taken at the classification: no learned surface 0, not the scene's depth 7, "
                                          "no external upscaler 0; census 40 frames with a screen (0 skipped): frames with an edit 40, a change began 0, "
                                          "changes from rest 0; at rest 40 frames, edited 40000 of 40000 px; during a change 0 frames, edited 0 of 0 px; "
                                          "history restarts 0 px in 0 frames.", kWorldScreenHoldFrames);
        check(std::strcmp(line, want) == 0 &&
              std::strstr(line, "ui depth world screens (F108): 1800 frames, a change held ") == line,
              "F108: the line's counts, in the order the reader parses them");
    }

    // ---- each stand-in's signature is the game shader's ----
    struct Want { const char* name; UINT reg; BYTE mask; D3D_REGISTER_COMPONENT_TYPE type; };
    // PS EBA95E15B0A66102's input signature (edvr_logs\shaders\ps_EBA95E15B0A66102.dxbc, 3616 bytes, disassembled).
    const Want game[4] = {
        {"__USER_VERTEX_FACEINVARIANT", 0, 0x3, D3D_REGISTER_COMPONENT_UINT32},
        {"__USER_VERTEX_M_LIGHTINGNORMAL", 1, 0x7, D3D_REGISTER_COMPONENT_FLOAT32},
        {"__USER_VERTEX_M_LIGHTINGTANGENT", 2, 0x7, D3D_REGISTER_COMPONENT_FLOAT32},
        {"__USER_VERTEX_M_TEXCOORD", 3, 0x3, D3D_REGISTER_COMPONENT_FLOAT32},
    };
    const std::pair<const void*, size_t> standIns[2] = {{kWorldScreenCoverageBytecode, sizeof(kWorldScreenCoverageBytecode)},
                                                        {kWorldScreenTrackedBytecode, sizeof(kWorldScreenTrackedBytecode)}};
    for (int s = 0; s < 2; ++s) {
        ComPtr<ID3D11ShaderReflection> refl;
        hr(D3DReflect(standIns[s].first, standIns[s].second, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(refl.GetAddressOf())));
        D3D11_SHADER_DESC desc{};
        hr(refl->GetDesc(&desc));
        check(desc.InputParameters == 4, "F105/F108: the stand-in takes the game shader's four inputs");
        for (UINT i = 0; i < 4 && i < desc.InputParameters; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC p{};
            hr(refl->GetInputParameterDesc(i, &p));
            check(std::strcmp(p.SemanticName, game[i].name) == 0 && p.SemanticIndex == 0 && p.Register == game[i].reg &&
                      p.Mask == game[i].mask && p.ComponentType == game[i].type && p.SystemValueType == D3D_NAME_UNDEFINED,
                  "F105/F108: each input element is the game shader's (name, index, register, mask, type)");
        }
        check(desc.OutputParameters == 3, "F105/F108: the stand-in writes three targets");
        for (UINT i = 0; i < 3 && i < desc.OutputParameters; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC p{};
            hr(refl->GetOutputParameterDesc(i, &p));
            check(std::strcmp(p.SemanticName, "SV_Target") == 0 && p.SemanticIndex == i && p.Register == i,
                  "F105/F108: the coverage mask at target 0, the census state at 1, the edit mask at 2");
        }
        if (s == 0) check(desc.BoundResources == 0, "F105: the held stand-in reads no resource");
        if (s == 1) {
            D3D11_SHADER_INPUT_BIND_DESC b{};
            check(SUCCEEDED(refl->GetResourceBindingDescByName("Instances", &b)) && b.BindPoint == 1 && b.Type == D3D_SIT_STRUCTURED,
                  "F108: the tracked stand-in reads the game's instance records at t1");
            check(SUCCEEDED(refl->GetResourceBindingDescByName("Material", &b)) && b.BindPoint == 2, "F108: ...the game's cb2 at b2");
            check(SUCCEEDED(refl->GetResourceBindingDescByName("UiEditAges", &b)) && b.BindPoint == WorldScreenCoverage::kAgesSlot &&
                      b.Dimension == D3D_SRV_DIMENSION_TEXTURE2DARRAY,
                  "F108: ...and the edit ages, an array, at t14");
        }
    }

    // ---- F105: the held pass on WARP ----
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
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> gamePs, standIn, tracked;
    hr(dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
    hr(dev->CreatePixelShader(gamePsCode->GetBufferPointer(), gamePsCode->GetBufferSize(), nullptr, &gamePs));
    hr(dev->CreatePixelShader(kWorldScreenCoverageBytecode, sizeof(kWorldScreenCoverageBytecode), nullptr, &standIn));
    hr(dev->CreatePixelShader(kWorldScreenTrackedBytecode, sizeof(kWorldScreenTrackedBytecode), nullptr, &tracked));
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
    auto colourTarget = [&](DXGI_FORMAT format, ComPtr<ID3D11Texture2D>& tex, ComPtr<ID3D11RenderTargetView>& rtv, UINT w = 8) {
        D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = 8; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = format; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        hr(dev->CreateTexture2D(&td, nullptr, &tex)); hr(dev->CreateRenderTargetView(tex.Get(), nullptr, &rtv));
    };
    ComPtr<ID3D11Texture2D> gbuf[4], maskTex, editTex, stateTex; ComPtr<ID3D11RenderTargetView> gbufRtv[4], maskRtv, editRtv, stateRtv;
    for (int i = 0; i < 4; ++i) colourTarget(DXGI_FORMAT_R8G8B8A8_UNORM, gbuf[i], gbufRtv[i]);
    colourTarget(DXGI_FORMAT_R8_UNORM, maskTex, maskRtv);
    colourTarget(DXGI_FORMAT_R8_UNORM, editTex, editRtv);
    colourTarget(DXGI_FORMAT_R8_UNORM, stateTex, stateRtv);
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
        ctx->ClearRenderTargetView(stateRtv.Get(), zero);
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
        check(pass.begin(ctx, standIn.Get(), maskRtv.Get(), stateRtv.Get(), editRtv.Get(), nullptr, test, blend),
              "F105: the pass begins for the game's draw");
        ctx->Draw(6, 6);                 // the second issue
        pass.end(ctx);
        check(!pass.active(), "F105: the pass ends");

        for (int i = 0; i < 4; ++i) check(before[i] == read(dev, ctx, gbuf[i].Get()), "F105: no game target is written");
        check(depthBefore == read(dev, ctx, scene.tex.Get()), "F105: the game's depth is not written");
        const auto mask = read(dev, ctx, maskTex.Get()), edit = read(dev, ctx, editTex.Get()), state = read(dev, ctx, stateTex.Get());
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
            const float m = mask[y * 8 + x] * 255.0f, e = edit[y * 8 + x] * 255.0f, s = state[y * 8 + x] * 255.0f;
            check(std::fabs(m - (screen ? float(kWorldScreenMaskValue) : 0.0f)) < .5f,
                  "F105: the coverage mask holds the attached value exactly where the screen won the depth test");
            check(std::fabs(e - (screen ? float(kWorldScreenEditValue) : 0.0f)) < .5f,
                  "F105: the edit mask is marked exactly where the coverage mask is");
            check(std::fabs(s - (screen ? float(kWorldScreenStateEdited) : 0.0f)) < .5f,
                  "F108: the held stand-in's census state says edited wherever it marks (the old behaviour, counted as such)");
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
        check(!pass.begin(ctx, standIn.Get(), maskRtv.Get(), stateRtv.Get(), editRtv.Get(), nullptr, test, blend),
              "F105: refused with an unordered-access view bound beside the targets");
        ComPtr<ID3D11PixelShader> ps; ctx->PSGetShader(&ps, nullptr, nullptr);
        ComPtr<ID3D11UnorderedAccessView> still;
        ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1, 1, &still);
        check(ps == gamePs && still == uav && !pass.active(), "F105: a refusal changes nothing");
        // ...and without a depth target, a stand-in or a state target.
        ctx->ClearState();
        ctx->OMSetRenderTargets(1, &one, nullptr);
        check(!pass.begin(ctx, standIn.Get(), maskRtv.Get(), stateRtv.Get(), editRtv.Get(), nullptr, test, blend),
              "F105: refused without a depth target");
        check(!pass.begin(ctx, nullptr, maskRtv.Get(), stateRtv.Get(), editRtv.Get(), nullptr, test, blend), "F105: refused without the stand-in");
        check(!pass.begin(ctx, standIn.Get(), maskRtv.Get(), nullptr, editRtv.Get(), nullptr, test, blend), "F108: refused without the state target");
    }

    // ---- F108: UiContent's array path ----
    auto makeArray = [&](UINT w, UINT h, UINT slices) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = slices; d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> t; hr(dev->CreateTexture2D(&d, nullptr, &t)); return t;
    };
    auto arrayView = [&](ID3D11Texture2D* t, UINT first, UINT count) {
        D3D11_SHADER_RESOURCE_VIEW_DESC v{}; v.Format = DXGI_FORMAT_R8G8B8A8_UNORM; v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        v.Texture2DArray.MipLevels = 1; v.Texture2DArray.FirstArraySlice = first; v.Texture2DArray.ArraySize = count;
        ComPtr<ID3D11ShaderResourceView> s; hr(dev->CreateShaderResourceView(t, &v, &s)); return s;
    };
    // One texel of one slice set to a value (mip 0).
    auto poke = [&](ID3D11Texture2D* t, UINT slice, UINT x, UINT y, unsigned char v) {
        const unsigned char px[4] = {v, v, v, 255};
        D3D11_BOX box{x, y, 0, x + 1, y + 1, 1};
        ctx->UpdateSubresource(t, D3D11CalcSubresource(0, slice, 1), &box, px, 4, 0);
    };
    {
        ctx->ClearState();
        TestUiContent tracker;
        auto t = makeArray(16, 8, 3);   // 4x2 blocks a slice
        auto v = arrayView(t.Get(), 0, 3);
        auto* a0 = tracker.prepare(ctx, v.Get(), 10);
        check(tracker.lastDecision == UiContent::Decision::kReset, "F108: a new array is seeded");
        D3D11_SHADER_RESOURCE_VIEW_DESC ad{}; a0->GetDesc(&ad);
        check(ad.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY && ad.Texture2DArray.ArraySize == 3, "F108: the ages are an array of the view's slices");
        for (auto& s : wstest::agesOf(dev, ctx, a0)) for (float a : s) check(a == 0, "F108: a seeded array has no edit");
        for (auto& s : wstest::agesOf(dev, ctx, tracker.prepare(ctx, v.Get(), 11))) for (float a : s) check(a == 0, "F108: an unchanged array has no edit");
        check(tracker.lastDecision == UiContent::Decision::kUpdated, "F108: an array's next frame is compared");
        poke(t.Get(), 1, 5, 2, 200);   // slice 1, block (1, 0)
        auto ages = wstest::agesOf(dev, ctx, tracker.prepare(ctx, v.Get(), 12));
        check(ages.size() == 3 && ages[1].size() == 8, "F108: three slices of 4x2 blocks");
        for (UINT s = 0; s < 3; ++s) for (UINT b = 0; b < 8; ++b)
            check(ages[s][b] == ((s == 1 && b == 1) ? 1.f : 0.f), "F108: a changed region is edited only there, in its own slice");
        for (uint32_t f = 13; f <= 44; ++f) ages = wstest::agesOf(dev, ctx, tracker.prepare(ctx, v.Get(), f));
        for (auto& s : ages) for (float a : s) check(a == 0, "F108: a static slice gives no edit after the 32-frame expiry");
        // A view on slices 1..2 is its own entry, and its slice 1 is the texture's slice 2.
        auto v2 = arrayView(t.Get(), 1, 2);
        tracker.prepare(ctx, v2.Get(), 44);
        check(tracker.lastDecision == UiContent::Decision::kReset && tracker.inUse() == 2, "F108: a sub-range view is its own entry");
        poke(t.Get(), 2, 0, 0, 90);
        ages = wstest::agesOf(dev, ctx, tracker.prepare(ctx, v2.Get(), 45));
        check(ages.size() == 2 && ages[0][0] == 0 && ages[1][0] == 1, "F108: the view's slice index is the texture's slice less the view's first");
        // A 2D surface is not an array entry, and the 2D path still declines an array texture viewed as one slice of 2D... no such
        // view exists in D3D11, so the declines that remain are format and binding.
        D3D11_TEXTURE2D_DESC d{}; d.Width = 8; d.Height = 8; d.MipLevels = 1; d.ArraySize = 2; d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> noRt; hr(dev->CreateTexture2D(&d, nullptr, &noRt));
        check(tracker.prepare(ctx, arrayView(noRt.Get(), 0, 2).Get(), 46) == nullptr &&
                  tracker.lastDecision == UiContent::Decision::kDeclinedNoRenderTarget,
              "F108: an array that is not a render target is declined (a static atlas, not a rendered interface)");
    }

    // ---- F108: the tracked stand-in on WARP: two instances, the interface's slice 1 and a solid colour ----
    {
        ctx->ClearState();
        auto trackVsCode = compile(R"HLSL(
struct O {
    nointerpolation uint2 f : __USER_VERTEX_FACEINVARIANT;
    float3 n : __USER_VERTEX_M_LIGHTINGNORMAL;
    float3 t : __USER_VERTEX_M_LIGHTINGTANGENT;
    float2 uv : __USER_VERTEX_M_TEXCOORD;
    float4 p : SV_POSITION;
};
O main(float2 q : POSITION, uint instance : SV_InstanceID) {
    O o = (O)0; o.f = uint2(0, instance); o.uv = q; o.n = float3(0, 0, 1); o.t = float3(1, 0, 0);
    o.p = float4(-1 + q.x + instance, 1 - 2 * q.y, .5, 1); return o;
}
)HLSL", "vs_5_0");
        ComPtr<ID3D11VertexShader> tvs; hr(dev->CreateVertexShader(trackVsCode->GetBufferPointer(), trackVsCode->GetBufferSize(), nullptr, &tvs));
        D3D11_INPUT_ELEMENT_DESC in2{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
        ComPtr<ID3D11InputLayout> layout2; hr(dev->CreateInputLayout(&in2, 1, trackVsCode->GetBufferPointer(), trackVsCode->GetBufferSize(), &layout2));
        const float quad[] = {0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1};
        D3D11_BUFFER_DESC qb{}; qb.ByteWidth = sizeof(quad); qb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA qi{quad, 0, 0}; ComPtr<ID3D11Buffer> quadVb; hr(dev->CreateBuffer(&qb, &qi, &quadVb));
        // The game's instance records (t1, stride 96): word 4 the interface's slice, bit 31 a solid colour.
        uint32_t records[2][24] = {};
        records[0][4] = 1u;
        records[1][4] = 0x80000000u | 1u;
        D3D11_BUFFER_DESC sb{}; sb.ByteWidth = sizeof(records); sb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; sb.StructureByteStride = 96;
        D3D11_SUBRESOURCE_DATA si{records, 0, 0}; ComPtr<ID3D11Buffer> inst; hr(dev->CreateBuffer(&sb, &si, &inst));
        ComPtr<ID3D11ShaderResourceView> instSrv; hr(dev->CreateShaderResourceView(inst.Get(), nullptr, &instSrv));
        // The game's cb2: [1] the uv's scale and offset (the identity here).
        const float material[24] = {0, 0, 0, 0, 1, 1, 0, 0};
        D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = sizeof(material); cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA ci{material, 0, 0}; ComPtr<ID3D11Buffer> cb2; hr(dev->CreateBuffer(&cbd, &ci, &cb2));
        ComPtr<ID3D11Texture2D> tMask, tEdit, tState, tGb; ComPtr<ID3D11RenderTargetView> tMaskRtv, tEditRtv, tStateRtv, tGbRtv;
        colourTarget(DXGI_FORMAT_R8_UNORM, tMask, tMaskRtv, 16);
        colourTarget(DXGI_FORMAT_R8_UNORM, tEdit, tEditRtv, 16);
        colourTarget(DXGI_FORMAT_R8_UNORM, tState, tStateRtv, 16);
        colourTarget(DXGI_FORMAT_R8G8B8A8_UNORM, tGb, tGbRtv, 16);
        auto tDepth = depth(dev, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, 16);
        // One frame: UiContent's ages for the interface, as ui_depth.cpp hands them to the stand-in (an array view, a 2D interface's
        // ages given as a one-slice array), the game's draw, then the tracked pass, the masks read back.
        struct Read { std::vector<float> mask, state, edit; };
        auto frame = [&](TestUiContent& tracker, ID3D11ShaderResourceView* guiView, uint32_t f) {
            ID3D11ShaderResourceView* ages = tracker.prepare(ctx, guiView, f);
            check(ages != nullptr, "F108: the interface is tracked");
            ID3D11ShaderResourceView* asArray = tracker.arrayView(ctx, ages);
            D3D11_SHADER_RESOURCE_VIEW_DESC ad{};
            if (asArray) asArray->GetDesc(&ad);
            check(asArray && ad.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY, "F108b: the stand-in's ages are an array view");
            Read r;
            ctx->ClearState();
            UINT stride = 8, offset = 0;
            ctx->IASetInputLayout(layout2.Get()); ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->IASetVertexBuffers(0, 1, quadVb.GetAddressOf(), &stride, &offset);
            ctx->VSSetShader(tvs.Get(), nullptr, 0); ctx->PSSetShader(gamePs.Get(), nullptr, 0);
            ctx->PSSetShaderResources(1, 1, instSrv.GetAddressOf()); ctx->PSSetShaderResources(2, 1, &guiView);
            ctx->PSSetConstantBuffers(2, 1, cb2.GetAddressOf());
            ctx->RSSetState(raster.Get()); D3D11_VIEWPORT vp{0, 0, 16, 8, 0, 1}; ctx->RSSetViewports(1, &vp);
            ID3D11RenderTargetView* gt[4] = {tGbRtv.Get(), nullptr, nullptr, nullptr};
            ctx->OMSetRenderTargets(4, gt, tDepth.dsv.Get());
            ctx->OMSetDepthStencilState(gameDepth.Get(), 0);
            const float zero[4] = {};
            for (auto* rtv : {tGbRtv.Get(), tMaskRtv.Get(), tEditRtv.Get(), tStateRtv.Get()}) ctx->ClearRenderTargetView(rtv, zero);
            ctx->ClearDepthStencilView(tDepth.dsv.Get(), D3D11_CLEAR_DEPTH, 0, 0);
            ctx->DrawInstanced(6, 2, 0, 0);   // the game's draw
            WorldScreenCoverage pass;
            check(pass.begin(ctx, tracked.Get(), tMaskRtv.Get(), tStateRtv.Get(), tEditRtv.Get(), asArray, test, blend),
                  "F108: the tracked pass begins");
            ctx->DrawInstanced(6, 2, 0, 0);   // the second issue
            pass.end(ctx);
            ComPtr<ID3D11ShaderResourceView> t14; ctx->PSGetShaderResources(WorldScreenCoverage::kAgesSlot, 1, &t14);
            check(!t14, "F108: the game's t14 (empty here) is put back");
            r.mask = read(dev, ctx, tMask.Get()); r.state = read(dev, ctx, tState.Get()); r.edit = read(dev, ctx, tEdit.Get());
            return r;
        };
        auto at = [](const std::vector<float>& v, unsigned x, unsigned y) { return v[y * 16 + x] * 255.0f; };
        // The pixels whose 2x2 blocks about the uv include block (0, 0): x and y 0..2 of the interface's instance (left half).
        auto inChange = [](unsigned x, unsigned y) { return x < 3 && y < 3; };
        // A frame k frames after the block's last change: on k = 0 the fresh bit (6), the first-frame state and the full edit; for
        // k = 1..N-1 (N = kWorldScreenHoldFrames) the edited state and UiContent's age (255 - 8k); from k = N the screen is at rest,
        // although UiContent's age is still above zero (it lasts 32 frames). Outside the change, and on the solid colour, rest.
        auto expect = [&](const Read& r, int k, const char* what) {
            bool ok = true;
            for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 16; ++x) {
                const bool changed = k >= 0 && x < 8 && inChange(x, y);
                const bool fresh = changed && k == 0;
                const bool held = changed && k < static_cast<int>(kWorldScreenHoldFrames);
                const float wantMask = float(fresh ? kWorldScreenFreshMaskValue : kWorldScreenMaskValue);
                const float wantState = float(fresh ? kWorldScreenStateFresh : (held ? kWorldScreenStateEdited : kWorldScreenStateRest));
                const float wantEdit = held ? float(255 - 8 * k) : 0.0f;
                ok = ok && std::fabs(at(r.mask, x, y) - wantMask) < .5f && std::fabs(at(r.state, x, y) - wantState) < .5f &&
                     std::fabs(at(r.edit, x, y) - wantEdit) < 1.5f;
            }
            check(ok, what);
        };
        // run: one interface (gui, viewed by guiView; the instance samples slice 1, clamped to the last), poked at `slice`.
        auto run = [&](ID3D11Texture2D* gui, ID3D11ShaderResourceView* guiView, UINT slice, const char* kind) {
            TestUiContent tracker;
            char what[200];
            auto say = [&](const char* s) { std::snprintf(what, sizeof(what), "%s (%s)", s, kind); return what; };
            uint32_t f = 20;
            expect(frame(tracker, guiView, f++), -1, say("F108: seeded, the screen is marked (2) at rest with no edit"));
            unsigned char value = 77;
            auto change = [&]() { for (unsigned ty = 0; ty < 4; ++ty) for (unsigned tx = 0; tx < 4; ++tx) poke(gui, slice, tx, ty, value); ++value; };
            // A change, then the hold: held exactly N frames counting the change's own, at rest on the Nth after it.
            change();
            const uint32_t first = f;
            for (int k = 0; k <= static_cast<int>(kWorldScreenHoldFrames); ++k)
                expect(frame(tracker, guiView, f++), k, say(k == 0 ? "F108: the fresh bit, the first-frame state and the edit exactly where the interface changed"
                                                         : k < static_cast<int>(kWorldScreenHoldFrames) ? "F108b: held on each frame of the hold after the change"
                                                         : "F108b: at rest on the hold's end"));
            auto ages = wstest::agesOf(dev, ctx, tracker.prepare(ctx, guiView, f - 1));
            const UINT agedSlice = UINT(ages.size()) > slice ? slice : 0;
            check(!ages.empty() && std::fabs(ages[agedSlice][0] * 255.0f - float(255 - 8 * kWorldScreenHoldFrames)) < 1.5f,
                  say("F108b: the hold ends while UiContent's own age still lasts (its 32-frame expiry is unchanged)"));
            for (int k = int(kWorldScreenHoldFrames) + 1; k < 32; ++k)
                expect(frame(tracker, guiView, f++), k, say("F108b: at rest for the rest of UiContent's expiry"));
            // The block changes again: held again for N frames from the new change.
            change();
            for (int k = 0; k <= static_cast<int>(kWorldScreenHoldFrames); ++k)
                expect(frame(tracker, guiView, f++), k, say("F108b: a block that changes again is held again for the hold"));
            // A change inside the hold restarts it: the block is held N frames from its LAST change.
            change();
            expect(frame(tracker, guiView, f++), 0, say("F108b: a change begins"));
            expect(frame(tracker, guiView, f++), 1, say("F108b: ...held"));
            expect(frame(tracker, guiView, f++), 2, say("F108b: ...held"));
            change();
            for (int k = 0; k <= static_cast<int>(kWorldScreenHoldFrames); ++k)
                expect(frame(tracker, guiView, f++), k, say("F108b: a change within the hold holds the block N frames from its last change"));
            check(f > first, "F108b: the timeline ran");
        };
        // The interface as an array: 16x16, two slices, 4x4 blocks a slice; the instance samples slice 1 over 8x8 pixels.
        auto guiArray = makeArray(16, 16, 2);
        run(guiArray.Get(), arrayView(guiArray.Get(), 0, 2).Get(), 1, "a Texture2DArray interface");
        // The interface as a 2D texture with a Texture2D view, as the 2026-10-25 flight's terminals were (3760x2115, one slice, a
        // Texture2D view): its ages are 2D, given to the stand-in as a one-slice array; the instance's slice 1 clamps to slice 0.
        auto gui2d = makeArray(16, 16, 1);
        D3D11_SHADER_RESOURCE_VIEW_DESC v2d{}; v2d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; v2d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        v2d.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> gui2dView; hr(dev->CreateShaderResourceView(gui2d.Get(), &v2d, &gui2dView));
        run(gui2d.Get(), gui2dView.Get(), 0, "a Texture2D interface");
        {
            TestUiContent tracker;
            ID3D11ShaderResourceView* ages2d = tracker.prepare(ctx, gui2dView.Get(), 5);
            D3D11_SHADER_RESOURCE_VIEW_DESC ad{};
            if (ages2d) ages2d->GetDesc(&ad);
            check(ages2d && ad.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D, "F108b: a 2D interface's ages are 2D");
            ID3D11ShaderResourceView* asArray = tracker.arrayView(ctx, ages2d);
            ID3D11ShaderResourceView* again = tracker.arrayView(ctx, ages2d);
            D3D11_SHADER_RESOURCE_VIEW_DESC arr{};
            if (asArray) asArray->GetDesc(&arr);
            check(asArray && asArray == again && arr.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY && arr.Texture2DArray.ArraySize == 1,
                  "F108b: ...given as one one-slice array view, made once");
            ID3D11ShaderResourceView* arrayAges = tracker.prepare(ctx, arrayView(guiArray.Get(), 0, 2).Get(), 5);
            check(arrayAges && tracker.arrayView(ctx, arrayAges) == arrayAges, "F108b: an array interface's ages are given as they are");
            check(tracker.arrayView(ctx, gui2dView.Get()) == nullptr, "F108b: a view that is not this cache's ages is refused");
        }
    }

    // ---- F108: the census pass on WARP ----
    {
        ctx->ClearState();
        std::vector<unsigned char> states(64, 0);
        for (int i = 0; i < 10; ++i) states[i] = 1;
        for (int i = 10; i < 15; ++i) states[i] = 2;
        for (int i = 15; i < 18; ++i) states[i] = 3;
        auto st = zgate::tex2d(dev, 8, 8, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE, states.data(), 8);
        auto sv = zgate::srvOf(dev, st.Get());
        WorldScreenCensusPass census;
        ID3D11ShaderResourceView* both[2] = {sv.Get(), sv.Get()};
        check(census.count(ctx, both, 2, 7), "F108: the census counts");
        WorldScreenCensusRow got; bool drained = false;
        const auto deadline = GetTickCount64() + 2000;
        while (!drained && GetTickCount64() < deadline) {
            ctx->Flush();
            census.drain(ctx, [&](const WorldScreenCensusRow& r) { got = r; drained = true; });
            if (!drained) Sleep(1);
        }
        check(drained && got.ok && got.frame == 7 && got.screen == 36 && got.edited == 16 && got.fresh == 6,
              "F108: the census counts both eyes' screen, edited and first-frame pixels");
        check(got.eye[0] == 18 && got.eye[1] == 18, "F116: ...and each eye's screen pixels apart");
        {   // F116: one eye only (the other not marked): its pixels in its own range, none in the other's
            ID3D11ShaderResourceView* oneEye[2] = {nullptr, sv.Get()};
            check(census.count(ctx, oneEye, 2, 8), "F116: the census counts one eye");
            WorldScreenCensusRow one; bool done = false;
            const auto until = GetTickCount64() + 2000;
            while (!done && GetTickCount64() < until) {
                ctx->Flush();
                census.drain(ctx, [&](const WorldScreenCensusRow& r) { one = r; done = true; });
                if (!done) Sleep(1);
            }
            check(done && one.ok && one.frame == 8 && one.eye[0] == 0 && one.eye[1] == 18 && one.screen == 18,
                  "F116: an eye with no screen counts 0, the other its own 18");
        }
        for (int i = 0; i < 4; ++i) check(census.count(ctx, both, 1, 9 + i), "F108: the ring takes four frames");
        check(!census.count(ctx, both, 1, 13), "F108: a fifth frame in flight is skipped, not waited for");
        census.reset();
    }
    ctx->ClearState();
}

// F108: the motion pass (the production temporal_mv_fast_cs) on WARP, with the coverage mask bound at t4 and probe.z set: the
// history-invalidate value (size*2) and the reactive mask 1 exactly where the coverage carries the fresh bit (6); a screen at rest
// (2) and an unmarked pixel get the motion they get with no mask at all.
void testWorldScreenHistoryRestart(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    const UINT w = 64, h = 64;
    ComPtr<ID3D11ShaderReflection> refl;
    hr(D3DReflect(edvr::kTemporalMvBytecode, sizeof(edvr::kTemporalMvBytecode), __uuidof(ID3D11ShaderReflection),
                  reinterpret_cast<void**>(refl.GetAddressOf())));
    auto* P = refl->GetConstantBufferByName("P");
    D3D11_SHADER_BUFFER_DESC pd{};
    hr(P->GetDesc(&pd));
    std::vector<char> params(pd.Size, 0);
    auto put = [&](const char* name, const void* v, UINT bytes) {
        D3D11_SHADER_VARIABLE_DESC vd{};
        check(SUCCEEDED(P->GetVariableByName(name)->GetDesc(&vd)) && bytes <= vd.Size && vd.StartOffset + bytes <= params.size(),
              "F108 mv: cbuffer field");
        std::memcpy(params.data() + vd.StartOffset, v, bytes);
    };
    auto f4 = [&](const char* name, float a, float b, float c, float d) { const float v[4] = {a, b, c, d}; put(name, v, 16); };
    const int region[4] = {0, 0, int(w), int(h)}, sz[2] = {int(w), int(h)};
    put("region", region, 16); put("size", sz, 8); put("texSize", sz, 8);
    f4("tanNow", -1, 1, -1, 1); f4("tanPrev", -1, 1, -1, 1);
    f4("dR0", 1, 0, 0, 0); f4("dR1", 0, 1, 0, 0); f4("dR2", 0, 0, 1, 0);
    f4("knobs", 0, 1, .025f, 0); f4("jit", .25f, -.375f, 0, .5f); f4("holoJitter", .25f, -.375f, 1, 0);
    f4("probe", 1, 0, 1, 0);   // probe.z: the coverage mask is bound; probe.w 0: no fixed bias, no adaptive UI
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = pd.Size; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init{params.data(), 0, 0};
    ComPtr<ID3D11Buffer> cb; hr(dev->CreateBuffer(&bd, &init, &cb));
    std::vector<float> sceneZ(size_t(w) * h);
    for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) sceneZ[size_t(y) * w + x] = .01f + float((x * 7 + y * 3) % 29) / 1000.0f;
    std::vector<unsigned char> mark(size_t(w) * h, 0), none(size_t(w) * h, 0);
    for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) {
        if (y < 16) mark[size_t(y) * w + x] = static_cast<unsigned char>(kWorldScreenMaskValue);            // a screen at rest
        else if (y < 32) mark[size_t(y) * w + x] = static_cast<unsigned char>(kWorldScreenFreshMaskValue);  // an edit's first frame
    }
    auto scene = zgate::srvOf(dev, zgate::tex2d(dev, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, sceneZ.data(), w * 4).Get());
    auto um = zgate::srvOf(dev, zgate::tex2d(dev, w, h, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE, mark.data(), w).Get());
    auto umNone = zgate::srvOf(dev, zgate::tex2d(dev, w, h, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE, none.data(), w).Get());
    auto mv = zgate::tex2d(dev, w, h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    auto zc = zgate::tex2d(dev, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    auto mk = zgate::tex2d(dev, w, h, DXGI_FORMAT_R8_UNORM, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    auto mvU = zgate::uavOf(dev, mv.Get()), zcU = zgate::uavOf(dev, zc.Get()), mkU = zgate::uavOf(dev, mk.Get());
    auto cs = zgate::makeCs(dev, edvr::kTemporalMvFastBytecode, sizeof(edvr::kTemporalMvFastBytecode));
    D3D11_SAMPLER_DESC smp{}; smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ComPtr<ID3D11SamplerState> sampler; hr(dev->CreateSamplerState(&smp, &sampler));
    auto run = [&](ID3D11ShaderResourceView* mask, std::vector<uint32_t>& motion, std::vector<float>& reactive) {
        ctx->ClearState();
        const float zero[4] = {};
        for (auto* u : {mvU.Get(), zcU.Get(), mkU.Get()}) ctx->ClearUnorderedAccessViewFloat(u, zero);
        ctx->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
        ctx->CSSetSamplers(0, 1, sampler.GetAddressOf());
        ID3D11ShaderResourceView* srvs[5] = {nullptr, nullptr, scene.Get(), nullptr, mask};
        ctx->CSSetShaderResources(0, 5, srvs);
        ID3D11UnorderedAccessView* uavs[3] = {mvU.Get(), zcU.Get(), mkU.Get()};
        ctx->CSSetUnorderedAccessViews(3, 3, uavs, nullptr);
        ctx->CSSetShader(cs.Get(), nullptr, 0);
        ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        ctx->ClearState();
        motion = zgate::readU32(dev, ctx, mv.Get());
        reactive = wstest::readSlices8(dev, ctx, mk.Get())[0];
    };
    std::vector<uint32_t> base, marked; std::vector<float> baseMk, markedMk;
    run(umNone.Get(), base, baseMk);
    run(um.Get(), marked, markedMk);
    // size*2 = (128, 128) in R16G16_FLOAT: 0x5800 each half.
    const uint32_t invalidate = 0x58005800u;
    uint32_t restarts = 0;
    for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) {
        const size_t i = size_t(y) * w + x;
        const bool fresh = y >= 16 && y < 32;
        if (fresh) {
            check(marked[i] == invalidate && markedMk[i] == 1.0f, "F108 mv: the invalidate value and the reactive mask 1 where an edit begins");
            ++restarts;
        } else {
            check(marked[i] == base[i] && markedMk[i] == baseMk[i], "F108 mv: a screen at rest and an unmarked pixel keep their motion");
        }
        check(base[i] != invalidate, "F108 mv: the control writes no invalidate value");
    }
    check(restarts == 16 * w, "F108 mv: every fresh pixel restarted");
    std::printf("PASS: F108 history restart: the motion pass writes the invalidate value at %u fresh pixels and nowhere else.\n", restarts);
}

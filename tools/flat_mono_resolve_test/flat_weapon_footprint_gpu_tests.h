#pragma once
#define EDVR_WEAPON_FOOTPRINT_TEST 1
#include "../../src/d3d11/flat_weapon_footprint.h"
#undef EDVR_WEAPON_FOOTPRINT_TEST
#include <filesystem>
#include <fstream>
#include <iterator>

inline int flatWeaponFootprintGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    namespace fs = std::filesystem;
    using Microsoft::WRL::ComPtr;
    int failures = 0;
    auto check = [&](bool good, const char* why) {
        if (!good) { std::printf("FAIL: flat weapon GPU %s\n", why); ++failures; }
    };
    constexpr UINT width = 64, height = 64;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width; td.Height = height; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R11G11B10_FLOAT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> color;
    ComPtr<ID3D11RenderTargetView> rtv;
    check(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &color)) &&
          SUCCEEDED(device->CreateRenderTargetView(color.Get(), nullptr, &rtv)), "R11G11B10 render target");
    td.Format = DXGI_FORMAT_R32G8X24_TYPELESS; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11DepthStencilView> dsv, aliasDsv;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    check(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &depth)) &&
          SUCCEEDED(device->CreateDepthStencilView(depth.Get(), &dsvDesc, &dsv)) &&
          SUCCEEDED(device->CreateDepthStencilView(depth.Get(), &dsvDesc, &aliasDsv)),
          "D32S8 depth target and alias view");
    if (!color || !rtv || !depth || !dsv || !aliasDsv) return failures;

    auto compile = [&](const char* text, const char* profile) {
        ComPtr<ID3DBlob> code, errors;
        check(SUCCEEDED(D3DCompile(text, std::strlen(text), nullptr, nullptr, nullptr,
            "main", profile, 0, 0, &code, &errors)), "fixture shader compiles");
        return code;
    };
    const char* vsText =
        "float4 main(uint id:SV_VertexID):SV_Position {"
        "float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};"
        "return float4(p[id],0.5,1);}";
    const char* lateVsText =
        "float4 main(uint id:SV_VertexID):SV_Position {"
        "float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};"
        "return float4(p[id],0.3,1);}";
    auto vsCode = compile(vsText, "vs_5_0"), lateVsCode = compile(lateVsText, "vs_5_0");
    auto psCode = compile("float4 main():SV_Target{return float4(0,1,0,1);}", "ps_5_0");
    auto csCode = compile("[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID){}", "cs_5_0");
    if (!vsCode || !lateVsCode || !psCode || !csCode) return failures;
    ComPtr<ID3D11VertexShader> vs, lateVs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ComputeShader> cs;
    check(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)) &&
          SUCCEEDED(device->CreateVertexShader(lateVsCode->GetBufferPointer(), lateVsCode->GetBufferSize(), nullptr, &lateVs)) &&
          SUCCEEDED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps)) &&
          SUCCEEDED(device->CreateComputeShader(csCode->GetBufferPointer(), csCode->GetBufferSize(), nullptr, &cs)),
          "fixture graphics and sentinel compute shaders");
    if (!vs || !lateVs || !ps || !cs) return failures;

    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D11_FILL_SOLID; rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE; rasterDesc.ScissorEnable = TRUE;
    ComPtr<ID3D11RasterizerState> raster;
    check(SUCCEEDED(device->CreateRasterizerState(&rasterDesc, &raster)), "scissor raster state");
    D3D11_DEPTH_STENCIL_DESC markDesc{};
    markDesc.DepthEnable = TRUE; markDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    markDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    markDesc.StencilEnable = TRUE; markDesc.StencilReadMask = 0xff; markDesc.StencilWriteMask = 4;
    markDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    markDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    markDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    markDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    markDesc.BackFace = markDesc.FrontFace;
    ComPtr<ID3D11DepthStencilState> markState, lateDepthState;
    check(SUCCEEDED(device->CreateDepthStencilState(&markDesc, &markState)), "bit-4 mark state");
    auto lateDesc = markDesc;
    lateDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    lateDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    lateDesc.StencilEnable = FALSE;
    check(SUCCEEDED(device->CreateDepthStencilState(&lateDesc, &lateDepthState)), "unrelated late depth state");
    D3D11_BLEND_DESC noColorDesc{};
    noColorDesc.RenderTarget[0].RenderTargetWriteMask = 0;
    ComPtr<ID3D11BlendState> noColor;
    check(SUCCEEDED(device->CreateBlendState(&noColorDesc, &noColor)), "stencil-only blend state");
    if (!raster || !markState || !lateDepthState || !noColor) return failures;

    // Keep nontrivial CS bindings live across all four production snapshots.
    D3D11_TEXTURE2D_DESC csTexture{};
    csTexture.Width = csTexture.Height = 1;
    csTexture.MipLevels = csTexture.ArraySize = csTexture.SampleDesc.Count = 1;
    csTexture.Format = DXGI_FORMAT_R32_FLOAT; csTexture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> csInput;
    ComPtr<ID3D11ShaderResourceView> csSrv;
    check(SUCCEEDED(device->CreateTexture2D(&csTexture, nullptr, &csInput)) &&
          SUCCEEDED(device->CreateShaderResourceView(csInput.Get(), nullptr, &csSrv)), "sentinel compute SRV");
    csTexture.Format = DXGI_FORMAT_R32_UINT; csTexture.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> csOutputs[2];
    ComPtr<ID3D11UnorderedAccessView> csUavs[2];
    for (unsigned i = 0; i < 2; ++i)
        check(SUCCEEDED(device->CreateTexture2D(&csTexture, nullptr, &csOutputs[i])) &&
              SUCCEEDED(device->CreateUnorderedAccessView(csOutputs[i].Get(), nullptr, &csUavs[i])),
              "sentinel compute UAV");
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = 16; cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> csCb;
    check(SUCCEEDED(device->CreateBuffer(&cbDesc, nullptr, &csCb)), "sentinel compute CB");
    if (!csSrv || !csUavs[0] || !csUavs[1] || !csCb) return failures;

    context->ClearState();
    const float clearColor[4] = {0.1f, 0.2f, 0.3f, 1};
    context->ClearRenderTargetView(rtv.Get(), clearColor);
    context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.8f, 0);
    ID3D11RenderTargetView* rt = rtv.Get();
    context->OMSetRenderTargets(1, &rt, dsv.Get());
    context->RSSetState(raster.Get());
    D3D11_VIEWPORT viewport{}; viewport.Width = float(width); viewport.Height = float(height); viewport.MaxDepth = 1;
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(), nullptr, 0); context->PSSetShader(ps.Get(), nullptr, 0);
    context->CSSetShader(cs.Get(), nullptr, 0);
    ID3D11ShaderResourceView* savedSrvs[2] = {csSrv.Get(), csSrv.Get()};
    ID3D11UnorderedAccessView* savedUavs[2] = {csUavs[0].Get(), csUavs[1].Get()};
    ID3D11Buffer* savedCb = csCb.Get();
    context->CSSetShaderResources(0, 2, savedSrvs);
    context->CSSetUnorderedAccessViews(0, 2, savedUavs, nullptr);
    context->CSSetConstantBuffers(0, 1, &savedCb);
    auto scissor = [&](LONG left, LONG top, LONG right, LONG bottom) {
        D3D11_RECT rect{left, top, right, bottom}; context->RSSetScissorRects(1, &rect);
    };
    auto statePreserved = [&] {
        ID3D11RenderTargetView* currentRt = nullptr;
        ID3D11DepthStencilView* currentDs = nullptr;
        context->OMGetRenderTargets(1, &currentRt, &currentDs);
        ID3D11ComputeShader* currentCs = nullptr;
        context->CSGetShader(&currentCs, nullptr, nullptr);
        ID3D11ShaderResourceView* currentSrvs[2]{};
        ID3D11UnorderedAccessView* currentUavs[2]{};
        ID3D11Buffer* currentCb = nullptr;
        context->CSGetShaderResources(0, 2, currentSrvs);
        context->CSGetUnorderedAccessViews(0, 2, currentUavs);
        context->CSGetConstantBuffers(0, 1, &currentCb);
        const bool good = currentRt == rtv.Get() && currentDs == dsv.Get() &&
            currentCs == cs.Get() && currentSrvs[0] == csSrv.Get() && currentSrvs[1] == csSrv.Get() &&
            currentUavs[0] == csUavs[0].Get() && currentUavs[1] == csUavs[1].Get() &&
            currentCb == csCb.Get();
        if (currentRt) currentRt->Release(); if (currentDs) currentDs->Release();
        if (currentCs) currentCs->Release(); if (currentCb) currentCb->Release();
        for (auto* s : currentSrvs) if (s) s->Release();
        for (auto* u : currentUavs) if (u) u->Release();
        return good;
    };

    // A preexisting mark outside the weapon-colored area catches an inference
    // that bit 0x04 uniquely belongs to the selected draw.
    context->OMSetBlendState(noColor.Get(), nullptr, 0xffffffffu);
    context->OMSetDepthStencilState(markState.Get(), 4);
    scissor(1, 1, 3, 3); context->Draw(3, 0);
    check(statePreserved(), "preexisting-mark draw retains compute state");
    wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const fs::path fixtureRoot = fs::path(exe).parent_path() / L"flat-weapon-fixture";
    std::error_code fileError; fs::create_directories(fixtureRoot, fileError);
    check(!fileError, "fixture output directory");
    for (const auto& item : fs::directory_iterator(fixtureRoot))
        if (item.is_regular_file() && item.path().filename().wstring().find(L"frame_101") == 0)
            fs::remove(item.path(), fileError);
    edvr::FlatWeaponFootprint capture;
    float camera[6][4]{}; camera[0][0] = camera[1][1] = camera[2][2] = camera[3][3] = 1;
    constexpr uint64_t weaponVs = 0x025B4B9FF54622EDull, weaponPs = 0x46F92DC71BF8DFA5ull;
    check(!capture.before(context, 101, 9, weaponVs, weaponPs, true, 0x1234,
          reinterpret_cast<const unsigned char*>(camera)), "off-arm draw is ignored");
    capture.armForTest(100, fixtureRoot.wstring());
    check(!capture.before(context, 101, 9, weaponVs ^ 1, weaponPs, true, 0x1234,
          reinterpret_cast<const unsigned char*>(camera)), "wrong shader pair is ignored");
    check(statePreserved(), "negative admission preserves game state");
    check(capture.before(context, 101, 10, weaponVs, weaponPs, true, 0x1234,
          reinterpret_cast<const unsigned char*>(camera)), "known exact-pair pre-draw snapshot");
    check(statePreserved(), "before snapshot preserves render and compute state");

    // The observed weapon pass writes color and stencil bit 4, but not depth.
    context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    scissor(2, 2, 6, 6);
    capture.beginActualDraw(context,101,10,'D',3,0,0,1,0);
    context->Draw(3, 0);
    capture.endActualDraw(context,101,10);
    capture.after(context, 101, 10);
    check(statePreserved(), "after snapshot preserves render and compute state");
    // A later pass overwrites part of the mark and changes depth elsewhere.
    context->OMSetBlendState(noColor.Get(), nullptr, 0xffffffffu);
    context->OMSetDepthStencilState(markState.Get(), 0);
    scissor(2, 2, 4, 4); context->Draw(3, 0);
    context->OMSetDepthStencilState(lateDepthState.Get(), 0);
    context->VSSetShader(lateVs.Get(), nullptr, 0);
    scissor(8, 8, 10, 10); context->Draw(3, 0);
    ComPtr<ID3D11Resource> colorResource;
    rtv->GetResource(&colorResource);
    capture.consumer(context, 101, 30, colorResource.Get());
    check(statePreserved(), "consumer snapshot preserves render and compute state");
    capture.clear(aliasDsv.Get(), D3D11_CLEAR_STENCIL, 0, 39);
    context->ClearDepthStencilView(aliasDsv.Get(), D3D11_CLEAR_STENCIL, 0, 0);
    capture.beforePresent(context, 101, 40);
    check(statePreserved(), "pre-Present snapshot preserves render and compute state");
    capture.clear(aliasDsv.Get(), D3D11_CLEAR_STENCIL, 0, 41);
    capture.present(context, 101, 40);
    context->Flush();
    const auto manifest = fixtureRoot / L"frame_101.json";
    for (unsigned attempt = 0; attempt < 200 && !fs::exists(manifest); ++attempt) {
        capture.present(context, 102 + attempt, 40); Sleep(1);
    }
    check(fs::exists(manifest), "four-stage manifest committed");
    { std::ifstream stream(manifest, std::ios::binary);
      const std::string body((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
      check(body.find("\"draw_seq\":39") != std::string::npos,
            "stencil clear through an alias DSV is attributed to the retained depth resource"); }
    { std::ifstream stream(manifest, std::ios::binary);
      const std::string body((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
      check(body.find("\"draw_seq\":41") == std::string::npos,
            "clear after frame end is excluded from selected frame chronology"); }
    auto read = [&](const wchar_t* filename) {
        std::ifstream stream(fixtureRoot / filename, std::ios::binary);
        return std::vector<unsigned char>((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    };
    const auto beforeDepth = read(L"frame_101_before_depth.bin");
    const auto afterDepth = read(L"frame_101_after_depth.bin");
    const auto consumerDepth = read(L"frame_101_hdr_consumer_depth.bin");
    const auto beforeStencil = read(L"frame_101_before_stencil.bin");
    const auto afterStencil = read(L"frame_101_after_stencil.bin");
    const auto consumerStencil = read(L"frame_101_hdr_consumer_stencil.bin");
    const auto endStencil = read(L"frame_101_end_frame_stencil.bin");
    auto sampleDepth = [&](const std::vector<unsigned char>& bytes, unsigned x, unsigned y) {
        float value = 0; if (bytes.size() == width * height * 4)
            std::memcpy(&value, bytes.data() + (y * width + x) * 4, 4);
        return value;
    };
    auto bit = [&](const std::vector<unsigned char>& bytes, unsigned x, unsigned y) {
        return bytes.size() == width * height && (bytes[y * width + x] & 4) != 0;
    };
    check(beforeDepth.size() == width * height * 4 && afterDepth.size() == beforeDepth.size() &&
          consumerDepth.size() == beforeDepth.size(), "packed depth planes");
    check(std::abs(sampleDepth(beforeDepth, 5, 5) - 0.8f) < 1e-6f &&
          std::abs(sampleDepth(afterDepth, 5, 5) - 0.8f) < 1e-6f &&
          std::abs(sampleDepth(consumerDepth, 8, 8) - 0.3f) < 1e-6f,
          "depth extraction sees stable weapon depth and unrelated later write");
    check(beforeStencil.size() == width * height && afterStencil.size() == beforeStencil.size() &&
          consumerStencil.size() == beforeStencil.size() && endStencil.size() == beforeStencil.size(),
          "packed one-byte stencil planes");
    check(bit(beforeStencil, 1, 1) && !bit(beforeStencil, 5, 5) &&
          bit(afterStencil, 1, 1) && bit(afterStencil, 5, 5) &&
          !bit(consumerStencil, 2, 2) && bit(consumerStencil, 5, 5) && !bit(endStencil, 5, 5),
          "preexisting mark, weapon mark, later overwrite, and end clear extracted");

    // Missing HDR consumer is a real producer-side partial capture, with
    // before/after/end files but no invented consumer pixels.
    const fs::path partialRoot = fs::path(exe).parent_path() / L"flat-weapon-partial-fixture";
    fs::create_directories(partialRoot, fileError);
    check(!fileError, "partial fixture output directory");
    for (const auto& item : fs::directory_iterator(partialRoot))
        if (item.is_regular_file() && item.path().filename().wstring().find(L"frame_201") == 0)
            fs::remove(item.path(), fileError);
    context->ClearRenderTargetView(rtv.Get(), clearColor);
    context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.8f, 0);
    context->OMSetRenderTargets(1, &rt, dsv.Get());
    context->OMSetDepthStencilState(markState.Get(), 4);
    context->OMSetBlendState(noColor.Get(), nullptr, 0xffffffffu);
    context->VSSetShader(vs.Get(), nullptr, 0);
    scissor(1, 1, 3, 3); context->Draw(3, 0);
    edvr::FlatWeaponFootprint partial;
    partial.armForTest(200, partialRoot.wstring());
    check(partial.before(context, 201, 50, weaponVs, weaponPs, true, 0x5678,
          reinterpret_cast<const unsigned char*>(camera)), "missing-consumer pre-draw captured");
    context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    scissor(2, 2, 6, 6);
    partial.beginActualDraw(context,201,50,'D',3,0,0,1,0);
    context->Draw(3, 0);
    partial.endActualDraw(context,201,50);
    partial.after(context, 201, 50);
    partial.beforePresent(context, 201, 60);
    partial.present(context, 201, 60);
    context->Flush();
    const auto partialManifest = partialRoot / L"frame_201.json";
    for (unsigned attempt = 0; attempt < 200 && !fs::exists(partialManifest); ++attempt) {
        partial.present(context, 202 + attempt, 60); Sleep(1);
    }
    check(fs::exists(partialManifest), "missing-consumer partial manifest committed");
    { std::ifstream stream(partialManifest, std::ios::binary);
      const std::string body((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
      check(body.find("\"status\":\"partial\"") != std::string::npos &&
            body.find("\"reason\":\"consumer-not-observed\"") != std::string::npos,
            "missing-consumer failure is explicit in producer manifest"); }

    // An armed capture with no qualifying pair expires without a fabricated
    // frame manifest. The summary itself is the observable no-match result.
    const fs::path noMatchRoot = fs::path(exe).parent_path() / L"flat-weapon-no-match-fixture";
    fs::create_directories(noMatchRoot, fileError);
    check(!fileError, "no-match fixture output directory");
    weaponFootprintLines.clear();
    edvr::FlatWeaponFootprint noMatch;
    noMatch.armForTest(300, noMatchRoot.wstring());
    noMatch.present(context, 1201, 0);
    check(!noMatch.active(), "no-match arm expires at bounded frame count");
    check(std::any_of(weaponFootprintLines.begin(), weaponFootprintLines.end(),
          [](const std::string& line) { return line.find("summary status=arm-expired-no-match") != std::string::npos; }),
          "no-match expiry writes a distinct diagnostic summary");
    check(!fs::exists(noMatchRoot / L"frame_300.json"), "no-match expiry does not fabricate a capture frame");

    // A right-edge draw must be inside the new full-width lower ROI. The old
    // centered 2048-wide ROI on this 2304-wide source ended at x2175 and
    // would miss the known selected pixels at x2240..2243 entirely.
    constexpr UINT wideWidth = 2304, wideHeight = 64;
    D3D11_TEXTURE2D_DESC wideDesc{};
    wideDesc.Width = wideWidth; wideDesc.Height = wideHeight;
    wideDesc.MipLevels = wideDesc.ArraySize = wideDesc.SampleDesc.Count = 1;
    wideDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    wideDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> wideColor;
    ComPtr<ID3D11RenderTargetView> wideRtv;
    check(SUCCEEDED(device->CreateTexture2D(&wideDesc, nullptr, &wideColor)) &&
          SUCCEEDED(device->CreateRenderTargetView(wideColor.Get(), nullptr, &wideRtv)),
          "wide HDR source target");
    wideDesc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    wideDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> wideDepth;
    ComPtr<ID3D11DepthStencilView> wideDsv;
    check(SUCCEEDED(device->CreateTexture2D(&wideDesc, nullptr, &wideDepth)) &&
          SUCCEEDED(device->CreateDepthStencilView(wideDepth.Get(), &dsvDesc, &wideDsv)),
          "wide depth-stencil target");
    auto rejectDesc = markDesc;
    rejectDesc.DepthFunc = D3D11_COMPARISON_GREATER;
    ComPtr<ID3D11DepthStencilState> rejectState;
    check(SUCCEEDED(device->CreateDepthStencilState(&rejectDesc, &rejectState)),
          "depth-rejected stencil state");
    if (!wideColor || !wideRtv || !wideDepth || !wideDsv || !rejectState) return failures;
    ComPtr<ID3D11Resource> wideColorResource;
    wideRtv->GetResource(&wideColorResource);
    auto wideStatePreserved = [&](ID3D11DepthStencilState* expectedDepth,
                                  ID3D11BlendState* expectedBlend) {
        ID3D11RenderTargetView* gotRt=nullptr;
        ID3D11DepthStencilView* gotDsv=nullptr;
        ID3D11DepthStencilState* gotDepth=nullptr;
        ID3D11BlendState* gotBlend=nullptr;
        ID3D11RasterizerState* gotRaster=nullptr;
        ID3D11ComputeShader* gotCs=nullptr;
        ID3D11VertexShader* gotVs=nullptr;
        ID3D11PixelShader* gotPs=nullptr;
        ID3D11ShaderResourceView* gotSrvs[2]{};
        ID3D11UnorderedAccessView* gotUavs[2]{};
        ID3D11Buffer* gotCb=nullptr;
        UINT ref=0, mask=0; FLOAT factors[4]{};
        context->OMGetRenderTargets(1,&gotRt,&gotDsv);
        context->OMGetDepthStencilState(&gotDepth,&ref);
        context->OMGetBlendState(&gotBlend,factors,&mask);
        context->RSGetState(&gotRaster);
        context->VSGetShader(&gotVs,nullptr,nullptr);
        context->PSGetShader(&gotPs,nullptr,nullptr);
        context->CSGetShader(&gotCs,nullptr,nullptr);
        context->CSGetShaderResources(0,2,gotSrvs);
        context->CSGetUnorderedAccessViews(0,2,gotUavs);
        context->CSGetConstantBuffers(0,1,&gotCb);
        UINT nv=1, ns=1; D3D11_VIEWPORT gotViewport{}; D3D11_RECT gotScissor{};
        context->RSGetViewports(&nv,&gotViewport);
        context->RSGetScissorRects(&ns,&gotScissor);
        const bool good=gotRt==wideRtv.Get() && gotDsv==wideDsv.Get() &&
            gotDepth==expectedDepth && ref==4 && gotBlend==expectedBlend && mask==0xffffffffu &&
            gotRaster==raster.Get() && gotVs==vs.Get() && gotPs==ps.Get() && gotCs==cs.Get() &&
            gotSrvs[0]==csSrv.Get() && gotSrvs[1]==csSrv.Get() &&
            gotUavs[0]==csUavs[0].Get() && gotUavs[1]==csUavs[1].Get() && gotCb==csCb.Get() &&
            nv==1 && gotViewport.Width==float(wideWidth) && gotViewport.Height==float(wideHeight) &&
            ns==1 && gotScissor.left==2240 && gotScissor.right==2244 &&
            gotScissor.top==2 && gotScissor.bottom==6;
        for (auto* p : {static_cast<IUnknown*>(gotRt),static_cast<IUnknown*>(gotDsv),
                        static_cast<IUnknown*>(gotDepth),static_cast<IUnknown*>(gotBlend),
                        static_cast<IUnknown*>(gotRaster),static_cast<IUnknown*>(gotVs),
                        static_cast<IUnknown*>(gotPs),static_cast<IUnknown*>(gotCs),
                        static_cast<IUnknown*>(gotSrvs[0]),static_cast<IUnknown*>(gotSrvs[1]),
                        static_cast<IUnknown*>(gotUavs[0]),static_cast<IUnknown*>(gotUavs[1]),
                        static_cast<IUnknown*>(gotCb)}) if(p)p->Release();
        return good;
    };
    D3D11_QUERY_DESC outerDesc{};outerDesc.Query=D3D11_QUERY_OCCLUSION;
    ComPtr<ID3D11Query> outerQuery;
    check(SUCCEEDED(device->CreateQuery(&outerDesc,&outerQuery)),"game-like outer occlusion query");
    if(!outerQuery)return failures;
    struct WideCase {const char* name; UINT frame; ID3D11DepthStencilState* depth; UINT drawCount;
                     bool colorWrites; bool changedColor; bool changedStencil; UINT passingSamples;
                     bool queryUnavailable; bool queryPending;};
    const WideCase wideCases[] = {
        {"visible",501,markState.Get(),3,true,true,true,16,false,false},
        {"depth_rejected",601,rejectState.Get(),3,true,false,false,0,false,false},
        {"color_disabled",701,markState.Get(),3,false,false,true,16,false,false},
        {"zero_count",801,markState.Get(),0,true,false,false,0,false,false},
        {"query_unavailable",901,markState.Get(),3,true,true,true,16,true,false},
        {"query_timeout",1001,markState.Get(),3,true,true,true,16,false,true},
    };
    for (const auto& scenario : wideCases) {
        const fs::path caseRoot=fs::path(exe).parent_path() /
            (std::wstring(L"flat-weapon-wide-")+std::wstring(scenario.name,scenario.name+std::strlen(scenario.name))+L"-fixture");
        fs::create_directories(caseRoot,fileError);
        check(!fileError,"wide fixture output directory");
        const std::wstring stem=L"frame_"+std::to_wstring(scenario.frame);
        for(const auto& item:fs::directory_iterator(caseRoot))
            if(item.is_regular_file() && item.path().filename().wstring().find(stem)==0)
                fs::remove(item.path(),fileError);
        context->ClearRenderTargetView(wideRtv.Get(),clearColor);
        context->ClearDepthStencilView(wideDsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.8f,0);
        ID3D11RenderTargetView* wideTarget=wideRtv.Get();
        context->OMSetRenderTargets(1,&wideTarget,wideDsv.Get());
        context->RSSetState(raster.Get());
        D3D11_VIEWPORT wideViewport{};
        wideViewport.Width=float(wideWidth);wideViewport.Height=float(wideHeight);wideViewport.MaxDepth=1;
        context->RSSetViewports(1,&wideViewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
        context->CSSetShader(cs.Get(),nullptr,0);
        context->CSSetShaderResources(0,2,savedSrvs);
        context->CSSetUnorderedAccessViews(0,2,savedUavs,nullptr);
        context->CSSetConstantBuffers(0,1,&savedCb);
        context->OMSetDepthStencilState(markState.Get(),4);
        context->OMSetBlendState(noColor.Get(),nullptr,0xffffffffu);
        scissor(1,1,3,3);context->Draw(3,0);
        context->OMSetDepthStencilState(scenario.depth,4);
        context->OMSetBlendState(scenario.colorWrites?nullptr:noColor.Get(),nullptr,0xffffffffu);
        scissor(2240,2,2244,6);
        edvr::FlatWeaponFootprint wide;
        wide.armForTest(scenario.frame-1,caseRoot.wstring(),scenario.name);
        wide.setQueryTestMode(scenario.queryUnavailable,scenario.queryPending);
        context->Begin(outerQuery.Get());
        check(wide.before(context,scenario.frame,10,weaponVs,weaponPs,true,0x9999,
              reinterpret_cast<const unsigned char*>(camera)),"wide selected pre-draw snapshot");
        check(wideStatePreserved(scenario.depth,scenario.colorWrites?nullptr:noColor.Get()),
              "wide pre-draw capture restores game pipeline state");
        wide.beginActualDraw(context,scenario.frame,10,'D',scenario.drawCount,0,0,1,0);
        context->Draw(scenario.drawCount,0);
        wide.endActualDraw(context,scenario.frame,10);
        wide.after(context,scenario.frame,10);
        context->End(outerQuery.Get());
        check(wideStatePreserved(scenario.depth,scenario.colorWrites?nullptr:noColor.Get()),
              "wide query and post-draw capture restore game pipeline state");
        wide.consumer(context,scenario.frame,20,wideColorResource.Get());
        check(wideStatePreserved(scenario.depth,scenario.colorWrites?nullptr:noColor.Get()),
              "wide consumer snapshot restores game pipeline state");
        wide.beforePresent(context,scenario.frame,30);
        check(wideStatePreserved(scenario.depth,scenario.colorWrites?nullptr:noColor.Get()),
              "wide end-frame snapshot restores game pipeline state");
        wide.present(context,scenario.frame,30);
        context->Flush();
        const auto wideManifest=caseRoot/(stem+L".json");
        for(unsigned attempt=0;attempt<1000&&!fs::exists(wideManifest);++attempt){
            wide.present(context,scenario.frame+1+attempt,30);Sleep(1);
        }
        check(fs::exists(wideManifest),"wide manifest and query results committed");
        if(fs::exists(wideManifest) && (scenario.queryUnavailable || scenario.queryPending)){
            std::ifstream manifestStream(wideManifest,std::ios::binary);
            const std::string manifest((std::istreambuf_iterator<char>(manifestStream)),std::istreambuf_iterator<char>());
            const char* status=scenario.queryUnavailable?"unavailable":"timeout";
            check(manifest.find(std::string("\"occlusion\":{\"status\":\"")+status+"\"")!=std::string::npos &&
                  manifest.find(std::string("\"pipeline_statistics\":{\"status\":\"")+status+"\"")!=std::string::npos,
                  "forced query failure remains explicitly unavailable or timed out");
        }
        UINT64 outerSamples=~UINT64(0);HRESULT outerStatus=S_FALSE;
        for(unsigned attempt=0;attempt<1000&&outerStatus==S_FALSE;++attempt){
            outerStatus=context->GetData(outerQuery.Get(),&outerSamples,sizeof(outerSamples),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(outerStatus==S_FALSE)Sleep(1);
        }
        check(outerStatus==S_OK && outerSamples==scenario.passingSamples,
              "nested game occlusion query retains its exact passed-sample count");
        auto readWide=[&](const wchar_t* stage,const wchar_t* plane){
            const auto filename=stem+L"_"+stage+L"_"+plane+L".bin";
            std::ifstream stream(caseRoot/filename,std::ios::binary);
            return std::vector<unsigned char>((std::istreambuf_iterator<char>(stream)),std::istreambuf_iterator<char>());
        };
        const auto colorBefore=readWide(L"before",L"color"),colorAfter=readWide(L"after",L"color");
        const auto stencilBefore=readWide(L"before",L"stencil"),stencilAfter=readWide(L"after",L"stencil");
        const size_t pixel=size_t(3)*wideWidth+2241;
        check(colorBefore.size()==size_t(wideWidth)*wideHeight*4 && colorAfter.size()==colorBefore.size() &&
              stencilBefore.size()==size_t(wideWidth)*wideHeight && stencilAfter.size()==stencilBefore.size(),
              "full-width lower ROI has packed native planes");
        if(colorBefore.size()==size_t(wideWidth)*wideHeight*4 && colorAfter.size()==colorBefore.size())
            check((std::memcmp(colorBefore.data()+pixel*4,colorAfter.data()+pixel*4,4)!=0)==scenario.changedColor,
                  "right-edge color footprint matches draw case");
        if(stencilBefore.size()==size_t(wideWidth)*wideHeight && stencilAfter.size()==stencilBefore.size())
            check((bool(stencilAfter[pixel]&4)&&!bool(stencilBefore[pixel]&4))==scenario.changedStencil,
                  "right-edge stencil footprint matches draw case");
    }
    context->ClearState();
    return failures;
}

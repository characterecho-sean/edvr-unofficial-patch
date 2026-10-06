// Drives the shipped graphics proxy through real D3D11 hooks on WARP.
// One process owns one case: the proxy's frame and shader registries are global.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <sstream>
#include <iomanip>
#include <vector>
#include "../../src/d3d11/flat_sdk_bench_probe.h"
#include "../openxr_native_test/present_device.h"

using Microsoft::WRL::ComPtr;

namespace {
using Snapshot = EdvrFlatSdkBenchSnapshot;
using SnapshotFn = unsigned int (__cdecl*)(Snapshot*, unsigned int);

bool absolute(const wchar_t* p) {
    return p && ((p[0] && p[1] == L':') || (p[0] == L'\\' && p[1] == L'\\'));
}
struct BenchCase { const wchar_t* argument; const char* sceneName; };
constexpr BenchCase benchCases[] = {
    {L"smoke","smoke"}, {L"scene","scene"}, {L"unsupported_host","unsupported_host"},
    {L"inert_no_write","inert_no_write"}, {L"inert_depth_write","inert_depth_write"},
    {L"inert_color_write","inert_color_write"}, {L"state_partial_mask","state_partial_mask"},
    {L"state_blended","state_blended"}
};
const BenchCase* commandCase(int argc, const wchar_t* const* argv) {
    if (argc!=9 || std::wcscmp(argv[1],L"--case")!=0 ||
        std::wcscmp(argv[3],L"--proxy")!=0 || !absolute(argv[4]) ||
        std::wcscmp(argv[5],L"--fixtures")!=0 || !absolute(argv[6]) ||
        std::wcscmp(argv[7],L"--adapter")!=0 ||
        (std::wcscmp(argv[8],L"warp")!=0 && std::wcscmp(argv[8],L"hardware")!=0))return nullptr;
    for (const auto& entry:benchCases)
        if (std::wcscmp(argv[2],entry.argument)==0)return &entry;
    return nullptr;
}
bool commandSelfTest() {
    const wchar_t* args[]={L"bench",L"--case",L"scene",L"--proxy",L"C:\\proxy.dll",
        L"--fixtures",L"C:\\fixtures",L"--adapter",L"warp"};
    for (const auto& entry:benchCases) {
        args[2]=entry.argument;
        for (const auto* adapter:{L"warp",L"hardware"}) {
            args[8]=adapter;
            // The admitted entry is also the dispatched scene name: no second whitelist.
            if (commandCase(9,args)!=&entry)return false;
        }
    }
    args[2]=L"state_partial_mask";
    if (!commandCase(9,args) || std::strcmp(commandCase(9,args)->sceneName,"state_partial_mask")!=0)return false;
    args[2]=L"state_blended";
    if (!commandCase(9,args) || std::strcmp(commandCase(9,args)->sceneName,"state_blended")!=0)return false;
    args[2]=L"unknown";
    if (commandCase(9,args))return false;
    args[2]=L"scene";args[4]=L"proxy.dll";
    if (commandCase(9,args))return false;
    args[4]=L"C:\\proxy.dll";args[8]=L"unknown";
    return !commandCase(9,args) && !commandCase(8,args);
}
bool ok(HRESULT hr, const char* operation) {
    if (SUCCEEDED(hr)) return true;
    std::fprintf(stderr, "flat SDK bench: %s failed 0x%08X\n", operation, unsigned(hr));
    return false;
}
void printResult(const char* name, const char* verdict, const Snapshot& before,
                 const Snapshot& after, const char* detail) {
    std::printf("EDVR_BENCH_RESULT {\"schema\":\"edvr-flat-sdk-bench\",\"version\":1,"
                 "\"case\":\"%s\",\"mode\":\"%s\",\"purpose\":\"entry\",\"verdict\":\"%s\","
                "\"cause\":\"%s\",\"observed\":{\"profileFlat\":%u,"
                "\"scope\":{\"frame\":%llu,\"drawBefore\":%u,\"drawAfter\":%u,\"work\":%u},"
                "\"namedWorld\":%u,\"candidates\":%u,"
                "\"firstFailure\":{\"frame\":%llu,\"q\":%u,"
                "\"vs\":\"%016llX\",\"ps\":\"%016llX\",\"stage\":\"%s\","
                "\"reason\":\"%s\",\"selectedH\":%u},"
                "\"lastH\":{\"attempts\":%llu,\"qualified\":%llu,\"reason\":\"%s\"},"
                "\"owner\":{\"foreignSeen\":%llu,\"captures\":%llu,\"captureAttempts\":%llu,"
                "\"gpuIdentitySubmitted\":%llu,\"worldMarkers\":%llu},"
                "\"resolverCalls\":%llu,\"hdrPrepped\":%llu,\"backendCalls\":%llu},"
                 "\"limitations\":[\"Camera, geometry, target and OM state reconstructed in WARP; no exact scene replay\","
                "\"Backend count is completed HDR SDK calls; WARP does not evaluate vendor features\"]}\n",
                 name, std::strcmp(after.mode,"on")==0?"taa":after.mode, verdict, detail, after.flatProfile,
                static_cast<unsigned long long>(after.frame), before.drawSequence, after.drawSequence,
                after.work, after.namedWorld, after.candidates,
                static_cast<unsigned long long>(after.firstFailureFrame), after.firstFailureSequence,
                static_cast<unsigned long long>(after.firstFailureVs),
                static_cast<unsigned long long>(after.firstFailurePs),
                after.firstFailureStage, after.firstFailureReason, after.firstFailureSelectedH,
                static_cast<unsigned long long>(after.hAttempts),
                static_cast<unsigned long long>(after.hQualified), after.hRefusal,
                static_cast<unsigned long long>(after.foreignSeen),
                static_cast<unsigned long long>(after.captured),
                static_cast<unsigned long long>(after.captureAttempts),
                static_cast<unsigned long long>(after.gpuIdentitySubmitted),
                static_cast<unsigned long long>(after.worldMarkers),
                static_cast<unsigned long long>(after.resolverCalls),
                static_cast<unsigned long long>(after.hdrPrepped),
                static_cast<unsigned long long>(after.hdrBackendCompleted));
}

int smoke(const wchar_t* proxyPath, D3D_DRIVER_TYPE driver) {
    HMODULE proxy=LoadLibraryExW(proxyPath,nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!proxy) { std::fprintf(stderr,"flat SDK bench: proxy load failed %lu\n",GetLastError()); return 2; }
    const auto snapshot=reinterpret_cast<SnapshotFn>(GetProcAddress(proxy,"edvr_selftest_flat_sdk_snapshot"));
    if (!snapshot) { std::fputs("flat SDK bench: snapshot export absent\n",stderr); return 2; }
    edvr::openxr::PresentDevice present;
    if (!ok(present.initialize(proxy,driver),"hidden proxy device")) return 2;
    for (unsigned i=0;i<3;++i)
        if (!ok(present.present(),"arm owned Present")) return 2;
    Snapshot before{};
    const unsigned read=snapshot(&before,sizeof(before));
    if (!read || !before.flatProfile || !before.live || !before.owner) {
        ComPtr<ID3D11Texture2D> backbuffer;
        const HRESULT bufferResult=present.swapchain()->GetBuffer(0,IID_PPV_ARGS(&backbuffer));
        ComPtr<ID3D11Device> chainDevice;
        const HRESULT deviceResult=present.swapchain()->GetDevice(IID_PPV_ARGS(&chainDevice));
        ComPtr<ID3D11DeviceContext> chainContext;
        if (chainDevice) chainDevice->GetImmediateContext(&chainContext);
        std::fprintf(stderr,"flat SDK bench: flat frame did not arm snapshot=%u profile=%u live=%u owner=%u device=%u context=%u output=%u frame=%llu work=%u\n",
            read,before.flatProfile,before.live,before.owner,before.deviceReady,before.contextReady,before.outputReady,
            static_cast<unsigned long long>(before.frame),before.work);
        std::fprintf(stderr,"flat SDK bench: swapchain GetBuffer=0x%08X GetDevice=0x%08X device=%p context=%p\n",
            unsigned(bufferResult),unsigned(deviceResult),chainDevice.Get(),chainContext.Get());
        return 2;
    }
    auto* device=present.device();auto* context=present.context();
    D3D11_TEXTURE2D_DESC colorDesc{};
    colorDesc.Width=colorDesc.Height=64;
    colorDesc.MipLevels=colorDesc.ArraySize=colorDesc.SampleDesc.Count=1;
    colorDesc.Format=static_cast<DXGI_FORMAT>(23);
    colorDesc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> color;
    ComPtr<ID3D11RenderTargetView> rtv;
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format=DXGI_FORMAT_R10G10B10A2_UNORM;
    rtvDesc.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    if (!ok(device->CreateTexture2D(&colorDesc,nullptr,&color),"scene color") ||
        !ok(device->CreateRenderTargetView(color.Get(),&rtvDesc,&rtv),"scene RTV")) return 2;
    D3D11_TEXTURE2D_DESC depthDesc=colorDesc;
    depthDesc.Format=DXGI_FORMAT_R32G8X24_TYPELESS;
    depthDesc.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11DepthStencilView> dsv;
    if (!ok(device->CreateTexture2D(&depthDesc,nullptr,&depth),"scene depth")) return 2;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dsvDesc.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    if (!ok(device->CreateDepthStencilView(depth.Get(),&dsvDesc,&dsv),"scene DSV")) return 2;
    constexpr char vsText[]="float4 main(uint id:SV_VertexID):SV_Position {float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};return float4(p[id],.5,1);}";
    constexpr char psText[]="float4 main():SV_Target0{return float4(0,0,0,0);}";
    ComPtr<ID3DBlob> vsCode,psCode,errors;
    if (!ok(D3DCompile(vsText,sizeof(vsText)-1,nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&vsCode,&errors),"bench VS compile") ||
        !ok(D3DCompile(psText,sizeof(psText)-1,nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&psCode,&errors),"bench PS compile")) return 2;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    if (!ok(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs),"bench VS create") ||
        !ok(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps),"bench PS create")) return 2;
    // B1[270..275] is the actual camera table location. This synthetic camera
    // and all OM state are reconstructed, never represented as Epic capture.
    std::vector<float> constants(276*4);
    const float rows[6][4]={{1,0,0,0},{0,1,0,0},{0,0,0,1},{0,0,.025f,0},{0,0,1,0},{0,0,0,0}};
    std::memcpy(constants.data()+270*4,rows,sizeof(rows));
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth=static_cast<UINT>(constants.size()*sizeof(float));
    cbDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.Usage=D3D11_USAGE_DYNAMIC;
    cbDesc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> cb;
    if (!ok(device->CreateBuffer(&cbDesc,nullptr,&cb),"camera B1")) return 2;
    ID3D11Buffer* camera=cb.Get();context->VSSetConstantBuffers(1,1,&camera);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!ok(context->Map(cb.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"camera B1 map")) return 2;
    std::memcpy(mapped.pData,constants.data(),cbDesc.ByteWidth);
    context->Unmap(cb.Get(),0);
    D3D11_VIEWPORT viewport{};
    viewport.Width=viewport.Height=64;
    viewport.MaxDepth=1;
    context->RSSetViewports(1,&viewport);
    ID3D11RenderTargetView* target=rtv.Get();
    context->OMSetRenderTargets(1,&target,dsv.Get());
    context->VSSetShader(vs.Get(),nullptr,0);
    context->PSSetShader(ps.Get(),nullptr,0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->Draw(3,0);
    Snapshot after{};
    if (!snapshot(&after,sizeof(after))) return 2;
    const bool reached=after.drawSequence>before.drawSequence && after.frame==before.frame;
    const bool taa=std::strcmp(after.mode,"taa")==0;
    const char* verdict=!reached?"FAIL":taa?"PASS":"UNSUPPORTED";
    printResult("smoke",verdict,before,after,
                !reached?"draw-scope-not-observed":taa?"actual-proxy-draw-scope-entered":
                    "actual-proxy-draw-scope-entered; SDK H trigger absent from smoke scene");
    return !reached?1:taa?0:2;
}
#include "scene.h"
} // namespace

int wmain(int argc,wchar_t** argv) {
    if (argc==2 && std::wcscmp(argv[1],L"--dry-run")==0) {
        std::puts("Would load a staged flat proxy, create a hidden WARP swapchain and issue original draws; writes nothing.");
        return 0;
    }
    if (argc==2 && std::wcscmp(argv[1],L"--self-test")==0) {
        const bool good=absolute(L"C:\\proxy.dll") && !absolute(L"proxy.dll") &&
            sizeof(Snapshot)==sizeof(EdvrFlatSdkBenchSnapshot) && commandSelfTest();
        std::printf("flat_sdk_integration_test: %s (CLI and snapshot contract)\n",good?"PASS":"FAIL");
        return good?0:1;
    }
    if (const auto* entry=commandCase(argc,argv))
        return std::strcmp(entry->sceneName,"smoke")==0?
            smoke(argv[4],std::wcscmp(argv[8],L"warp")==0?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE):
            scene(argv[4],argv[6],std::wcscmp(argv[8],L"warp")==0?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE,
                  entry->sceneName);
    std::fputs("usage: flat_sdk_integration_test --dry-run|--self-test|--case ",stderr);
    for (size_t i=0;i<std::size(benchCases);++i)
        std::fprintf(stderr,"%s%s",i?"|":"",benchCases[i].sceneName);
    std::fputs(" --proxy ABS_DLL --fixtures ABS_DIR --adapter warp|hardware\n",stderr);
    return 2;
}

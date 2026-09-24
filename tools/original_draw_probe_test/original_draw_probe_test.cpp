#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "../../src/common/system_d3d11.h"
#include "../../src/d3d11/gpu_timing.h"
#include "../../src/d3d11/game_query_probe.h"
#include "../../src/d3d11/original_draw_probe.h"
#include "identity_capture_test.h"

using Microsoft::WRL::ComPtr;
using namespace edvr;

namespace {
unsigned checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void hr(HRESULT value, const char* message) { check(value == S_OK, message); }

struct Device {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Device() {
        auto create = systemD3D11CreateDevice();
        check(create != nullptr, "load system D3D11");
        D3D_FEATURE_LEVEL level{};
        hr(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                  D3D11_SDK_VERSION, &device, &level, &context), "create WARP device");
    }
};

enum class ReadMode { Native, Pending, StatsPending, Error, DelayFirstOcclusion };
ReadMode g_readMode = ReadMode::Native;
bool g_delayedFirstOcclusion = false;
std::atomic<unsigned> g_begins{0}, g_ends{0}, g_reads{0}, g_badFlags{0};
std::atomic<unsigned> g_timingDisjointBegins{0}, g_timingDisjointEnds{0};
std::atomic<unsigned> g_timingTimestampEnds{0};
void STDMETHODCALLTYPE rawBegin(ID3D11DeviceContext* ctx, ID3D11Asynchronous* query) {
    ++g_begins;
    ctx->Begin(query);
}
void STDMETHODCALLTYPE rawEnd(ID3D11DeviceContext* ctx, ID3D11Asynchronous* query) {
    ++g_ends;
    ctx->End(query);
}
HRESULT STDMETHODCALLTYPE rawGetData(ID3D11DeviceContext* ctx, ID3D11Asynchronous* query,
                                     void* data, UINT bytes, UINT flags) {
    ++g_reads;
    if (flags != D3D11_ASYNC_GETDATA_DONOTFLUSH) ++g_badFlags;
    if (g_readMode == ReadMode::DelayFirstOcclusion && !g_delayedFirstOcclusion) {
        D3D11_QUERY_DESC desc{};
        static_cast<ID3D11Query*>(query)->GetDesc(&desc);
        if (desc.Query == D3D11_QUERY_OCCLUSION) {
            g_delayedFirstOcclusion = true;
            return S_FALSE;
        }
    }
    if (g_readMode == ReadMode::Pending) return S_FALSE;
    if (g_readMode == ReadMode::StatsPending) {
        ComPtr<ID3D11Query> typed;
        if (SUCCEEDED(query->QueryInterface(IID_PPV_ARGS(&typed)))) {
            D3D11_QUERY_DESC desc{};
            typed->GetDesc(&desc);
            if (desc.Query == D3D11_QUERY_PIPELINE_STATISTICS) return S_FALSE;
        }
    }
    if (g_readMode == ReadMode::Error) return E_FAIL;
    return ctx->GetData(query, data, bytes, flags);
}
OriginalDrawProbeQueryOps queryOps() { return {rawBegin, rawEnd, rawGetData}; }

HRESULT timingCreate(void*, ID3D11Device* device, const D3D11_QUERY_DESC* desc,
                     ID3D11Query** out) {
    return device->CreateQuery(desc, out);
}
HRESULT timingBegin(void*, ID3D11DeviceContext* ctx, ID3D11Asynchronous* query) {
    D3D11_QUERY_DESC desc{};
    static_cast<ID3D11Query*>(query)->GetDesc(&desc);
    if (desc.Query == D3D11_QUERY_TIMESTAMP_DISJOINT) ++g_timingDisjointBegins;
    ctx->Begin(query);
    return S_OK;
}
HRESULT timingEnd(void*, ID3D11DeviceContext* ctx, ID3D11Asynchronous* query) {
    D3D11_QUERY_DESC desc{};
    static_cast<ID3D11Query*>(query)->GetDesc(&desc);
    if (desc.Query == D3D11_QUERY_TIMESTAMP_DISJOINT) ++g_timingDisjointEnds;
    if (desc.Query == D3D11_QUERY_TIMESTAMP) ++g_timingTimestampEnds;
    ctx->End(query);
    return S_OK;
}
HRESULT timingGetData(void*, ID3D11DeviceContext* ctx, ID3D11Asynchronous* query,
                      void* data, UINT bytes, UINT flags) {
    return ctx->GetData(query, data, bytes, flags);
}
void timingRelease(void*, ID3D11Query* query) noexcept { query->Release(); }
GpuSpanD3D11Ops timingOps() {
    return {nullptr, timingCreate, timingBegin, timingEnd, timingGetData, timingRelease};
}

ComPtr<ID3DBlob> compile(const char* source, const char* target) {
    ComPtr<ID3DBlob> code, errors;
    const HRESULT result = D3DCompile(source, std::strlen(source), "original-draw-probe-test",
        nullptr, nullptr, "main", target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(result) && errors)
        std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    hr(result, "compile shader");
    return code;
}

struct Scene {
    ComPtr<ID3D11Texture2D> color, depth;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11DepthStencilState> visible, occluded;

    explicit Scene(Device& d) {
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = texture.Height = 16;
        texture.MipLevels = texture.ArraySize = 1;
        texture.SampleDesc.Count = 1;
        texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr(d.device->CreateTexture2D(&texture, nullptr, &color), "create color");
        hr(d.device->CreateRenderTargetView(color.Get(), nullptr, &rtv), "create RTV");
        texture.Format = DXGI_FORMAT_D32_FLOAT;
        texture.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        hr(d.device->CreateTexture2D(&texture, nullptr, &depth), "create depth");
        hr(d.device->CreateDepthStencilView(depth.Get(), nullptr, &dsv), "create DSV");
        const auto vsCode = compile(
            "float4 main(uint id:SV_VertexID):SV_Position {"
            "float2 p=id==0?float2(-1,-1):id==1?float2(-1,3):float2(3,-1);"
            "return float4(p,0.5,1);}", "vs_5_0");
        const auto psCode = compile("float4 main():SV_Target{return float4(1,0,0,1);}", "ps_5_0");
        hr(d.device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs),
           "create VS");
        hr(d.device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps),
           "create PS");
        D3D11_DEPTH_STENCIL_DESC state{};
        state.DepthEnable = TRUE;
        state.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        state.DepthFunc = D3D11_COMPARISON_ALWAYS;
        hr(d.device->CreateDepthStencilState(&state, &visible), "create visible depth state");
        state.DepthFunc = D3D11_COMPARISON_NEVER;
        hr(d.device->CreateDepthStencilState(&state, &occluded), "create occluded depth state");
    }

    void bind(Device& d, bool pass) {
        ID3D11RenderTargetView* target = rtv.Get();
        d.context->OMSetRenderTargets(1, &target, dsv.Get());
        d.context->OMSetDepthStencilState(pass ? visible.Get() : occluded.Get(), 0);
        D3D11_VIEWPORT viewport{0, 0, 16, 16, 0, 1};
        d.context->RSSetViewports(1, &viewport);
        d.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d.context->VSSetShader(vs.Get(), nullptr, 0);
        d.context->PSSetShader(ps.Get(), nullptr, 0);
        const float clear[4] = {0, 0, 1, 1};
        d.context->ClearRenderTargetView(rtv.Get(), clear);
        d.context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1, 0);
    }

    void verify(Device& d, bool red) {
        D3D11_TEXTURE2D_DESC desc{};
        color->GetDesc(&desc);
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        hr(d.device->CreateTexture2D(&desc, nullptr, &staging), "create staging");
        d.context->CopyResource(staging.Get(), color.Get());
        d.context->Flush(); // Test readback only; the probe never flushes.
        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr(d.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "map staging");
        bool exact = true;
        for (UINT y = 0; y < desc.Height; ++y) {
            const auto* row = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch;
            for (UINT x = 0; x < desc.Width; ++x) {
                const auto* pixel = row + x * 4;
                exact = exact && pixel[0] == (red ? 255 : 0) && pixel[1] == 0 &&
                    pixel[2] == (red ? 0 : 255) && pixel[3] == 255;
            }
        }
        check(exact, "query bracket preserves every visible/occluded RGBA pixel");
        d.context->Unmap(staging.Get(), 0);
    }
};

OriginalDrawProbeInput input() {
    OriginalDrawProbeInput value{};
    value.kind = OriginalDrawKind::Draw;
    value.count = 3;
    value.instances = 1;
    value.originalVsHash = 0x1122334455667788ull;
    value.originalPsHash = 0x8877665544332211ull;
    value.verdict = 7;
    value.eye = 0;
    value.pass = OriginalDrawPass::EyeColor;
    value.eyeSized = true;
    return value;
}

void boundary(Device& d, uint64_t frame) {
    originalDrawProbeFrame(d.context.Get(), {frame, nullptr, 16, 16});
}

OriginalDrawProbeInput targetedInput(unsigned family, bool modified = false) {
    static const uint64_t vs[] = {0xEB5234DB6ADB491Dull, 0x5B4D8E894EEDA8B4ull,
                                  0xBBE58E40FE88EC80ull};
    static const uint64_t ps[] = {0xCB9F297EFF264251ull, 0x4375B72964F386CDull,
                                  0xDB3E8D20CF53FBC0ull};
    auto value = input();
    value.originalVsHash = vs[family]; value.originalPsHash = ps[family];
    value.modified = modified;
    return value;
}

void targetedSelectionAndScale(Device& d) {
    check(originalDrawProbeBind(d.device.Get(), d.context.Get(), queryOps()),
          "bind targeted selection controller");
    originalDrawProbeConfigure(true, true);
    boundary(d, 1);
    std::array<unsigned, 3> population{{7, 5, 3}};
    auto issuePopulation = [&](const std::array<unsigned, 3>& counts,
                               std::array<int, 3>* chosen) {
        unsigned selected = 0;
        auto unknown = input();
        for (unsigned i = 0; i < 11; ++i)
            check(!originalDrawProbeSelect(d.context.Get(), &unknown),
                  "targeted mode excludes unknown material pairs");
        for (unsigned family = 0; family < 3; ++family) {
            auto value = targetedInput(family);
            for (unsigned i = 0; i < counts[family]; ++i) if (originalDrawProbeSelect(d.context.Get(), &value)) {
                ++selected; if (chosen) (*chosen)[family] = static_cast<int>(i);
            }
            auto modified = targetedInput(family, true);
            check(!originalDrawProbeSelect(d.context.Get(), &modified),
                  "targeted mode excludes modified draws");
        }
        check(selected <= 3, "targeted controller selects at most one draw per family per frame");
        return selected;
    };
    check(issuePopulation(population, nullptr) == 0,
          "first targeted frame establishes family-local populations");
    boundary(d, 2);
    std::array<int, 3> first{{-1, -1, -1}}, held{{-1, -1, -1}};
    check(issuePopulation(population, &first) == 3, "one candidate selected from each known family");
    boundary(d, 3);
    check(issuePopulation(population, &held) == 3 && held == first,
          "cohort holds each family-local ordinal across frames");
    boundary(d, 4);
    const std::array<unsigned, 3> lower{{1, 1, 1}};
    for (unsigned frame = 0; frame < 26; ++frame) {
        issuePopulation(lower, nullptr);
        boundary(d, 5 + frame);
    }
    const auto snapshot = originalDrawProbeSnapshot();
    check(snapshot.targetedCandidates != 0 && snapshot.cohortReseeds >= 2 &&
          snapshot.populationDrift >= 3 && snapshot.targetMisses != 0,
          "lower populations record drift/misses and periodically reseed cohorts");

    auto unknown = input();
    constexpr unsigned iterations = 230000;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < iterations; ++i) originalDrawProbeSelect(d.context.Get(), &unknown);
    const auto targetedEnd = std::chrono::steady_clock::now();
    originalDrawProbeConfigure(true, false);
    for (unsigned i = 0; i < iterations; ++i) originalDrawProbeSelect(d.context.Get());
    const auto broadEnd = std::chrono::steady_clock::now();
    const auto targetedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(targetedEnd - start).count();
    const auto broadNs = std::chrono::duration_cast<std::chrono::nanoseconds>(broadEnd - targetedEnd).count();
    std::printf("original_draw_probe_test local Select scale: targeted-unknown %.1f ns/call, broad %.1f ns/call (%u iterations; informational)\n",
        double(targetedNs) / iterations, double(broadNs) / iterations, iterations);
    originalDrawProbeShutdown(d.context.Get());
}

struct RecurrenceScene {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> ids, geometryA, geometryB, pool;
    ComPtr<ID3D11ShaderResourceView> poolView;
    ComPtr<ID3D11DepthStencilState> depth;
    std::array<unsigned char, 336> model{};
    explicit RecurrenceScene(Device& d) {
        const auto code = compile(
            "float4 main(uint2 instance:INSTANCEANDMODELDATAINDEX,uint id:SV_VertexID):SV_Position{"
            "float2 p=id==0?float2(-1,-1):id==1?float2(-1,3):float2(3,-1);return float4(p,0.5,1);}",
            "vs_5_0");
        hr(d.device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs),
           "create recurrence VS");
        D3D11_INPUT_ELEMENT_DESC element{"INSTANCEANDMODELDATAINDEX",0,DXGI_FORMAT_R32G32_UINT,
            0,0,D3D11_INPUT_PER_INSTANCE_DATA,1};
        hr(d.device->CreateInputLayout(&element,1,code->GetBufferPointer(),code->GetBufferSize(),&layout),
           "create recurrence layout");
        originalDrawProbeRememberLayout(layout.Get(), &element, 1, 0);
        const uint32_t pair[2]{};
        D3D11_BUFFER_DESC b{}; b.ByteWidth=8;b.Usage=D3D11_USAGE_DEFAULT;b.BindFlags=D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init{pair};hr(d.device->CreateBuffer(&b,&init,&ids),"create recurrence IDs");
        b.ByteWidth=16;hr(d.device->CreateBuffer(&b,nullptr,&geometryA),"create recurrence geometry A");
        hr(d.device->CreateBuffer(&b,nullptr,&geometryB),"create recurrence geometry B");
        b={};b.ByteWidth=336;b.Usage=D3D11_USAGE_DEFAULT;b.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;b.StructureByteStride=336;init={model.data()};
        hr(d.device->CreateBuffer(&b,&init,&pool),"create recurrence pool");
        hr(d.device->CreateShaderResourceView(pool.Get(),nullptr,&poolView),"create recurrence pool view");
        D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
        ds.DepthFunc=D3D11_COMPARISON_LESS;hr(d.device->CreateDepthStencilState(&ds,&depth),"create recurrence depth");
    }
    void bind(Device& d, Scene& scene, bool zero, bool alternateGeometry=false) {
        scene.bind(d, true);d.context->OMSetDepthStencilState(depth.Get(),0);
        d.context->ClearDepthStencilView(scene.dsv.Get(),D3D11_CLEAR_DEPTH,zero?0.0f:1.0f,0);
        d.context->VSSetShader(vs.Get(),nullptr,0);d.context->IASetInputLayout(layout.Get());
        ID3D11Buffer* buffers[]={ids.Get(),alternateGeometry?geometryB.Get():geometryA.Get()};
        UINT strides[]={8,4},offsets[]={0,0};d.context->IASetVertexBuffers(0,2,buffers,strides,offsets);
        ID3D11ShaderResourceView* view=poolView.Get();d.context->VSSetShaderResources(33,1,&view);
    }
    void mutate(Device& d) { model[20]^=0x5a;d.context->UpdateSubresource(pool.Get(),0,nullptr,model.data(),0,0); }
};

void targetedRecurrence(Device& d, Scene& scene) {
    RecurrenceScene recurrence(d);
    uint64_t frame=1;
    auto start = [&] {
        check(originalDrawProbeBind(d.device.Get(),d.context.Get(),queryOps()),"bind recurrence probe");
        originalDrawProbeConfigure(true,true);boundary(d,frame++);
        recurrence.bind(d,scene,true);auto value=targetedInput(0);value.kind=OriginalDrawKind::DrawInstanced;
        check(!originalDrawProbeSelect(d.context.Get(),&value),"recurrence first frame establishes family");
        d.context->DrawInstanced(3,1,0,0);boundary(d,frame++);
    };
    auto sample = [&](bool zero, bool alternate, uint32_t count=3) {
        recurrence.bind(d,scene,zero,alternate);auto value=targetedInput(0);
        value.kind=OriginalDrawKind::DrawInstanced;value.count=count;value.instances=1;
        OriginalDrawProbeTicket ticket{};if(originalDrawProbeSelect(d.context.Get(),&value))ticket=originalDrawProbeBegin(d.context.Get(),value);
        d.context->DrawInstanced(3,1,0,0);originalDrawProbeEnd(d.context.Get(),ticket,true);boundary(d,frame++);
    };
    auto settle = [&] {
        d.context->Flush();
        for(unsigned i=0;i<200&&originalDrawProbeSnapshot().pending;++i) {
            Sleep(1); boundary(d,frame++);
        }
        check(originalDrawProbeSnapshot().pending==0,"bounded WARP settle retires targeted queries");
    };

    start();sample(true,false);sample(false,false);settle();
    auto s=originalDrawProbeSnapshot();
    if (!(s.recurrenceZeroToVisible==1&&s.recurrenceSamePayload>=1))
        std::fprintf(stderr,"recurrence failure: submitted=%llu ready=%llu pending=%llu zero=%llu visible=%llu same=%llu changed=%llu binding=%llu gaps=%llu order=%llu unsupported=%llu invalid=%llu readback=%llu expired=%llu skinned=%llu\n",
            s.submitted,s.ready,s.pending,s.zeroSamples,s.nonzeroSamples,s.recurrenceSamePayload,
            s.recurrencePayloadChanged,s.recurrenceBindingChanged,s.recurrenceGaps,s.recurrenceOutOfOrder,
            s.identityUnsupported,s.identityInvalidRecord,s.identityReadbackFailed,s.identityExpired,s.identitySkinned);
    check(s.recurrenceZeroToVisible==1&&s.recurrenceSamePayload>=1,
          "same binding and full payload count zero-to-visible recurrence");
    originalDrawProbeShutdown(d.context.Get());

    // Make every GPU result ready, then force the older sample's first poll
    // pending. The rotating cursor encounters the newer sample first next time.
    start();sample(true,false);sample(false,false);
    D3D11_QUERY_DESC eventDesc{D3D11_QUERY_EVENT,0};
    ComPtr<ID3D11Query> completed;
    hr(d.device->CreateQuery(&eventDesc,&completed),"create recurrence completion event");
    d.context->End(completed.Get());d.context->Flush();
    HRESULT completedResult=S_FALSE;
    const auto completionDeadline=GetTickCount64()+5000;
    while (completedResult==S_FALSE && GetTickCount64()<completionDeadline) {
        completedResult=d.context->GetData(completed.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if(completedResult==S_FALSE)Sleep(1);
    }
    hr(completedResult,"recurrence GPU work completes before forced query delay");
    g_delayedFirstOcclusion=false;g_readMode=ReadMode::DelayFirstOcclusion;
    settle();g_readMode=ReadMode::Native;s=originalDrawProbeSnapshot();
    check(g_delayedFirstOcclusion&&s.recurrenceZeroToVisible==1&&s.recurrenceOutOfOrder==0,
          "rotating polls retire ready family samples in submission order");
    originalDrawProbeShutdown(d.context.Get());

    start();sample(true,false);recurrence.mutate(d);sample(false,false);settle();s=originalDrawProbeSnapshot();
    check(s.recurrencePayloadChanged>=1&&s.recurrenceZeroToVisible==0,
          "changed full payload prevents a visibility transition");
    originalDrawProbeShutdown(d.context.Get());

    start();sample(true,false);sample(false,true);settle();s=originalDrawProbeSnapshot();
    check(s.recurrenceBindingChanged>=1&&s.recurrenceZeroToVisible==0,
          "changed geometry binding prevents a visibility transition");
    originalDrawProbeShutdown(d.context.Get());

    start();sample(true,false);boundary(d,frame++);sample(false,false);settle();s=originalDrawProbeSnapshot();
    check(s.recurrenceGaps>=1&&s.recurrenceZeroToVisible==0,
          "missing sampled frame resets recurrence without manufacturing a transition");
    originalDrawProbeShutdown(d.context.Get());

    start();for(uint32_t variant=1;variant<=140;++variant) {
        sample(false,false,variant);
        if ((variant%16)==0) settle();
    } settle();
    s=originalDrawProbeSnapshot();
    check(s.targetedFamilySamples>=129&&s.bucketOverflow!=0,
          "fixed family aggregates survive more than 128 exact count variants");
    originalDrawProbeShutdown(d.context.Get());

    // A timed-out payload gather must not leave the per-slot helper permanently
    // pending when that slot is reused after disable/re-enable.
    start();sample(false,false);g_readMode=ReadMode::Pending;originalDrawProbeConfigure(false);
    for(unsigned i=0;i<125;++i)boundary(d,frame++);
    s=originalDrawProbeSnapshot();
    check(s.identityExpired>=1&&s.pending==0,"targeted timeout abandons pending payload capture");
    g_readMode=ReadMode::Native;originalDrawProbeConfigure(true,true);
    recurrence.bind(d,scene,false);auto value=targetedInput(0);value.kind=OriginalDrawKind::DrawInstanced;
    check(!originalDrawProbeSelect(d.context.Get(),&value),"restart establishes fresh targeted population");
    d.context->DrawInstanced(3,1,0,0);boundary(d,frame++);sample(false,false);settle();
    s=originalDrawProbeSnapshot();
    check(s.targetedFamilySamples==1&&s.identityReadbackFailed==0,
          "slot captures successfully after timeout and targeted restart");
    originalDrawProbeShutdown(d.context.Get());
}

unsigned drawFrame(Device& d, Scene& scene, bool pass, OriginalDrawProbeInput value = input()) {
    scene.bind(d, pass);
    unsigned selected = 0;
    for (unsigned i = 0; i < 9; ++i) {
        OriginalDrawProbeTicket ticket{};
        if (originalDrawProbeSelect(d.context.Get())) {
            ++selected;
            ticket = originalDrawProbeBegin(d.context.Get(), value);
        }
        d.context->Draw(3, 0);
        originalDrawProbeEnd(d.context.Get(), ticket, true);
    }
    return selected;
}

void renderingAndCadence(Device& d, Scene& scene) {
    g_readMode = ReadMode::Native;
    g_begins = g_ends = g_reads = g_badFlags = 0;
    const auto disjointBegins = g_timingDisjointBegins.load();
    const auto timestampEnds = g_timingTimestampEnds.load();
    check(originalDrawProbeBind(d.device.Get(), d.context.Get(), queryOps()), "bind probe");
    originalDrawProbeConfigure(true);
    boundary(d, 1);
    check(drawFrame(d, scene, true) == 0, "first frame only establishes population");
    boundary(d, 2);
    check(drawFrame(d, scene, true) == 3, "three unique targets selected from prior population");
    scene.verify(d, true);
    boundary(d, 3);
    check(drawFrame(d, scene, false) == 3, "cadence repeats three targets");
    scene.verify(d, false);
    d.context->Flush();
    for (uint64_t frame = 4; frame != 14; ++frame) boundary(d, frame);
    const auto snapshot = originalDrawProbeSnapshot();
    check(snapshot.submitted == 6 && snapshot.ready == 6, "six sparse WARP brackets complete");
    check(snapshot.zeroSamples == 3 && snapshot.nonzeroSamples == 3,
          "WARP separates zero and nonzero passed-sample outcomes");
    check(snapshot.pipelineReady == 6 && snapshot.timingUnavailable == 6 &&
          snapshot.timedReady == 0 && snapshot.timingInvalid == 0,
          "visibility and pipeline statistics survive without a shared frame clock");
    check(g_timingDisjointBegins.load() == disjointBegins &&
          g_timingTimestampEnds.load() == timestampEnds,
          "parentless visibility emits no timestamp or disjoint markers");
    check(g_reads.load() != 0 && g_badFlags.load() == 0, "all raw reads use DONOTFLUSH");
    check(g_begins.load() == g_ends.load() && g_begins.load() >= 12,
          "every admitted counter Begin has one raw End");
    check(!originalDrawProbeInternalQuery(), "internal query marker is scoped");
    const auto before = snapshot.originalCalls;
    std::thread wrong([&] { boundary(d, 99); });
    wrong.join();
    check(originalDrawProbeSnapshot().originalCalls == before,
          "wrong-thread frame call cannot poll or mutate owner state");
    ComPtr<ID3D11DeviceContext> deferred;
    hr(d.device->CreateDeferredContext(0, &deferred), "create wrong context");
    originalDrawProbeFrame(deferred.Get(), {100, nullptr, 16, 16});
    check(originalDrawProbeSnapshot().originalCalls == before,
          "wrong context cannot poll or mutate owner state");
    boundary(d, 20);
    drawFrame(d, scene, true);
    originalDrawProbeConfigure(false);
    originalDrawProbeConfigure(true);
    boundary(d, 21);
    check(drawFrame(d, scene, true) == 0,
          "off/on restart discards pending work and stale target schedule");
    originalDrawProbeShutdown(d.context.Get());
}

void guardCase(Device& d, Scene& scene, unsigned which) {
    g_readMode = ReadMode::Native;
    g_begins = g_ends = 0;
    check(originalDrawProbeBind(d.device.Get(), d.context.Get(), queryOps()), "rebind guard case");
    originalDrawProbeConfigure(true);
    boundary(d, 1);
    drawFrame(d, scene, true);
    boundary(d, 2);
    auto value = input();
    if (which == 0) value.gameCountingActive = true;
    if (which == 2) value.gameQueryOverflow = true;
    drawFrame(d, scene, true, value);
    const auto snapshot = originalDrawProbeSnapshot();
    check(snapshot.submitted == 0, "game query guard rejects every selected bracket before Begin");
    check((which == 0 && snapshot.gameCounting == 3) ||
          (which == 2 && snapshot.queryOverflow == 3), "guard reason is explicit");
    check(g_begins.load() == 0 && g_ends.load() == 0,
          "game query conflict invokes no diagnostic Begin or End");
    originalDrawProbeShutdown(d.context.Get());
}

void predicationCase(Device& d, Scene& scene) {
    g_begins = g_ends = 0;
    check(originalDrawProbeBind(d.device.Get(), d.context.Get(), queryOps()), "rebind predication case");
    originalDrawProbeConfigure(true);
    boundary(d, 1);
    drawFrame(d, scene, true);
    boundary(d, 2);
    D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
    ComPtr<ID3D11Predicate> predicate;
    hr(d.device->CreatePredicate(&desc, &predicate), "create predicate");
    d.context->SetPredication(predicate.Get(), TRUE);
    drawFrame(d, scene, true);
    d.context->SetPredication(nullptr, FALSE);
    const auto snapshot = originalDrawProbeSnapshot();
    check(snapshot.submitted == 0 && snapshot.predicated == 3,
          "predication guard runs before every diagnostic Begin");
    check(g_begins.load() == 0 && g_ends.load() == 0,
          "predication invokes no diagnostic Begin or End");
    originalDrawProbeShutdown(d.context.Get());
}

void externalDisjointCase(Device& d, Scene& scene, bool pass) {
    check(originalDrawProbeBind(d.device.Get(), d.context.Get(), queryOps()), "rebind external disjoint case");
    originalDrawProbeConfigure(true);
    boundary(d, 1);
    drawFrame(d, scene, true);
    boundary(d, 2);
    D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ComPtr<ID3D11Query> external;
    hr(d.device->CreateQuery(&desc, &external), "create external disjoint");
    GameQueryProbe gameQueries;
    const auto disjointBegins = g_timingDisjointBegins.load();
    const auto disjointEnds = g_timingDisjointEnds.load();
    const auto timestampEnds = g_timingTimestampEnds.load();
    d.context->Begin(external.Get());
    gameQueries.bracketBegin(external.Get());
    GpuTimingFrameDriver frameClock;
    check(frameClock.bind(d.device.Get(), d.context.Get()) && frameClock.create(0) &&
          frameClock.begin(0) && frameClock.timestamp(0, 0),
          "shared application frame clock opens inside external disjoint context");
    auto value = input();
    const auto guard = gameQueries.guard();
    value.gameDisjointActive = guard.disjoint;
    drawFrame(d, scene, pass, value);
    check(frameClock.timestamp(0, 1) && frameClock.end(0),
          "shared application frame clock closes after sampled draws");
    gameQueries.bracketEnd(external.Get());
    d.context->End(external.Get());
    scene.verify(d, pass);
    d.context->Flush();
    for (uint64_t frame = 3; frame != 13; ++frame) boundary(d, frame);
    const auto snapshot = originalDrawProbeSnapshot();
    check(snapshot.submitted == 3 && snapshot.ready == 3 &&
          snapshot.zeroSamples == (pass ? 0u : 3u) &&
          snapshot.nonzeroSamples == (pass ? 3u : 0u),
          "external full-frame disjoint preserves zero/nonzero visibility result");
    check(snapshot.gameDisjoint == 3 && snapshot.pipelineReady == 3 &&
          snapshot.timedReady == 3 && snapshot.timingUnavailable == 0 &&
          snapshot.timingInvalid == 0,
          "borrowed shared frame timestamps and pipeline statistics remain valid");
    check(g_timingDisjointBegins.load() == disjointBegins + 1 &&
          g_timingDisjointEnds.load() == disjointEnds + 1 &&
          g_timingTimestampEnds.load() == timestampEnds + 8,
          "three sampled draws borrow one frame disjoint and add only timestamp pairs");
    GpuSpanRawSample raw{};
    GpuSpanPoll status = GpuSpanPoll::Pending;
    const auto deadline = GetTickCount64() + 1500;
    do {
        status = frameClock.poll(0, raw);
        if (status == GpuSpanPoll::Pending) Sleep(1);
    } while (status == GpuSpanPoll::Pending && GetTickCount64() < deadline);
    check(status == GpuSpanPoll::Ready && raw.frequency != 0,
          "shared parent frame frequency is healthy");
    frameClock.destroy(0);
    frameClock.reset(d.context.Get());
    originalDrawProbeShutdown(d.context.Get());
}

void readFailureCase(Device& d, Scene& scene, ReadMode mode) {
    g_readMode = mode;
    g_reads = g_badFlags = 0;
    check(originalDrawProbeBind(d.device.Get(), d.context.Get(), queryOps()), "rebind read failure case");
    originalDrawProbeConfigure(true);
    boundary(d, 1);
    drawFrame(d, scene, true);
    boundary(d, 2);
    drawFrame(d, scene, true);
    originalDrawProbeConfigure(false);
    d.context->Flush();
    const uint64_t limit = 140;
    for (uint64_t frame = 3; frame < limit; ++frame) boundary(d, frame);
    const auto snapshot = originalDrawProbeSnapshot();
    if (mode == ReadMode::Pending)
        check(snapshot.expired == 3 && snapshot.pending == 0, "permanent pending reads expire after bounded drain");
    else if (mode == ReadMode::StatsPending)
        check(snapshot.expired == 3 && snapshot.ready == 3 && snapshot.pipelineInvalid == 3 &&
              snapshot.invalid == 0 && snapshot.pending == 0,
              "timeout preserves ready visibility while retiring pending statistics");
    else
        check(snapshot.invalid == 3 && snapshot.pending == 0, "query read errors retire as invalid");
    check(g_reads.load() != 0 && g_badFlags.load() == 0, "failure polling stays non-flushing");
    originalDrawProbeShutdown(d.context.Get());
    g_readMode = ReadMode::Native;
}

void terminalRestartCase(Device& d) {
    check(originalDrawProbeBind(d.device.Get(), d.context.Get(), queryOps()), "rebind terminal case");
    originalDrawProbeConfigure(true);
    for (uint64_t frame = 1; frame <= 601; ++frame) boundary(d, frame);
    const auto snapshot = originalDrawProbeSnapshot();
    check(snapshot.window == 2 && snapshot.collecting && snapshot.frames == 0 &&
          snapshot.submitted == 0 && snapshot.planned == 0,
          "600-frame no-data window reports before a clean repeating window starts");
    originalDrawProbeShutdown(d.context.Get());
}

void timingDomainLossCase(Device& d, Scene& scene) {
    check(originalDrawProbeBind(d.device.Get(), d.context.Get(), queryOps()),
          "rebind timing-domain-loss case");
    originalDrawProbeConfigure(true);
    boundary(d, 1);
    drawFrame(d, scene, true);
    boundary(d, 2);
    check(gpuTimingShutdown(d.context.Get()), "remove shared timing domain while probe is active");
    scene.bind(d, true);
    unsigned selected = 0;
    for (unsigned i = 0; i < 9; ++i) {
        if (originalDrawProbeSelect(d.context.Get())) {
            ++selected;
            const auto ticket = originalDrawProbeBegin(d.context.Get(), input());
            check(!ticket, "selected Begin rejects a lost shared timing domain");
        }
        d.context->Draw(3, 0);
    }
    const auto beforeFrame = originalDrawProbeSnapshot();
    boundary(d, 3);
    const auto afterFrame = originalDrawProbeSnapshot();
    check(selected == 3 && beforeFrame.submitted == 0 &&
          afterFrame.frames == beforeFrame.frames &&
          afterFrame.originalCalls == beforeFrame.originalCalls &&
          afterFrame.pending == beforeFrame.pending,
          "lost timing domain admits no GPU work and Frame does not mutate or poll");
    scene.verify(d, true);
    originalDrawProbeShutdown(d.context.Get());
    check(gpuTimingBind(d.device.Get(), d.context.Get(), timingOps()),
          "shared timing domain cleanly rebinds after loss");
}

size_t occurrences(const std::string& text, const char* needle) {
    size_t count = 0, at = 0;
    while ((at = text.find(needle, at)) != std::string::npos) { ++count; at += std::strlen(needle); }
    return count;
}

void sourceContract() {
    std::ifstream file("src/d3d11/vscreen.cpp", std::ios::binary);
    check(bool(file), "open vscreen source contract");
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    check(occurrences(text, "originalDrawNativeBegin(") == 8,
          "one helper declaration plus seven native Begin call sites");
    check(occurrences(text, "originalDrawNativeEnd(") == 8,
          "one helper declaration plus seven native End call sites");
    check(text.find("originalDrawProbeInternalQuery()") != std::string::npos,
          "hook guard excludes only probe-owned shared timer traffic");
    check(text.find("originalDrawNativeBegin(self);\n"
                    "        g_state->realDrawIndexedInstanced") != std::string::npos,
          "indexed-instanced original draw remains immediately after probe Begin");
    check(text.find("baseVertex, startInstance);\n"
                    "        originalDrawNativeEnd(self, sample);\n"
                    "        // The weapon's temporal-AA motion vectors") != std::string::npos,
          "indexed-instanced probe End remains adjacent to the original draw");
    check(text.find("!originalDrawProbeSelect(self, &input)") != std::string::npos,
          "selected-only resource capture remains gated by cheap metadata Select");
    check(text.find("constexpr bool kControlledBaselineOriginalDrawDiagnostics = false;") != std::string::npos &&
          text.find("if constexpr (!kControlledBaselineOriginalDrawDiagnostics) return {};") != std::string::npos,
          "controlled baseline exits before original-draw metadata and query bookkeeping");
    check(text.find("Original draw diagnostic: controlled baseline OFF; coarse application GPU timing %s.") != std::string::npos,
          "controlled baseline has an explicit probe-off/coarse-timer startup marker");
}

void run() {
    sourceContract();
    Device d;
    check(gpuTimingBind(d.device.Get(), d.context.Get(), timingOps()),
          "bind shared GPU timer owner");
    Scene scene(d);
    identity_capture_test::run(d.device.Get(), d.context.Get(), checks);
    targetedSelectionAndScale(d);
    targetedRecurrence(d, scene);
    renderingAndCadence(d, scene);
    guardCase(d, scene, 0);
    guardCase(d, scene, 2);
    predicationCase(d, scene);
    externalDisjointCase(d, scene, true);
    externalDisjointCase(d, scene, false);
    readFailureCase(d, scene, ReadMode::Error);
    readFailureCase(d, scene, ReadMode::Pending);
    readFailureCase(d, scene, ReadMode::StatsPending);
    terminalRestartCase(d);
    timingDomainLossCase(d, scene);
    check(gpuTimingShutdown(d.context.Get()), "shutdown shared GPU timer");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2 || (std::strcmp(argv[1], "--self-test") && std::strcmp(argv[1], "--dry-run"))) {
        std::fprintf(stderr, "usage: original_draw_probe_test --self-test|--dry-run\n");
        return 2;
    }
    if (!std::strcmp(argv[1], "--dry-run")) {
        std::puts("original_draw_probe_test dry-run: no files written");
        return 0;
    }
    try {
        run();
        std::printf("original_draw_probe_test: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        originalDrawProbeShutdown();
        std::fprintf(stderr, "original_draw_probe_test: FAIL after %u checks: %s\n", checks, e.what());
        return 1;
    }
}

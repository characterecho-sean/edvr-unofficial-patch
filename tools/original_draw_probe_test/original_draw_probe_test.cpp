#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>
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

enum class ReadMode { Native, Pending, StatsPending, Error };
ReadMode g_readMode = ReadMode::Native;
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
    check(text.find("originalDrawNativeBegin(self, separate);\n        g_state->realDrawIndexedInstanced") <
          text.find("originalDrawNativeEnd(self, sample);\n        // The weapon's temporal-AA motion vectors"),
          "indexed-instanced original call ends before weapon motion capture");
    check(text.find("!originalDrawProbeSelect(self)") != std::string::npos,
          "selected-only descriptor path remains gated by Select");
}

void run() {
    sourceContract();
    Device d;
    check(gpuTimingBind(d.device.Get(), d.context.Get(), timingOps()),
          "bind shared GPU timer owner");
    Scene scene(d);
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

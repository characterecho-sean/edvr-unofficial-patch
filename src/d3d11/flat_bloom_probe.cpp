#include "flat_bloom_probe.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <windows.h>
#include <wrl/client.h>

#include "../common/config.h"
#include "../common/hotkey.h"
#include "../common/log.h"
#include "elite_binds.h"
#include "flat_compute_readback.h"
#include "menu.h"
#include "temporal_shader_bytecode.h"

using Microsoft::WRL::ComPtr;

namespace edvr {
namespace {

constexpr uint64_t kStripShader = 0xF1FB2EEFB662F3AAull;
constexpr uint64_t kBloomVs = 0xDFED8E1C9E191BECull;
constexpr uint64_t kBloomPs = 0x143AAE0597E2F7BFull;
constexpr uint64_t kToneVs = 0xF9CFC798F21E9AEAull;
constexpr uint64_t kTonePs = 0xE8948389387DA083ull;
constexpr uint32_t kGroup = 16;
constexpr uint32_t kMaxGroups = 32768;
constexpr int kSlots = 6;
constexpr const char* kDefaultKey = "F6";
constexpr const char* kThresholds = "0.85,0.95,0.99,0.999,1,4,16,64";

enum class State : uint8_t { Both = 0, JitterOnly, AaOnly, Neither };

const char* stateName(State s) {
    switch (s) {
    case State::Both: return "jitter=on aa=on";
    case State::JitterOnly: return "jitter=on aa=off";
    case State::AaOnly: return "jitter=off aa=on";
    case State::Neither: return "jitter=off aa=off";
    }
    return "jitter=? aa=?";
}

struct Bin {
    uint32_t n;
    float sum;
    float maxv;
    uint32_t c0, c1, c2, c3, c4, c5, c6, c7;
    uint32_t pad;
};
static_assert(sizeof(Bin) == 48, "bloom probe bin stride must match the shader");

struct Limits {
    uint32_t sizeX, sizeY;
    uint32_t originX, originY;
    uint32_t extentX, extentY;
    uint32_t groupsX;
    uint32_t groupCount;
    uint32_t pad0, pad1, pad2, pad3;
};
static_assert(sizeof(Limits) == 48, "bloom probe constant buffer must match the shader");

struct Slot {
    bool pending = false;
    BloomProbePoint point = BloomProbePoint::HBefore;
    uint64_t frame = 0;
    ComPtr<ID3D11Buffer> staging;
};

ComPtr<ID3D11ComputeShader> g_reduce, g_finish;
ComPtr<ID3D11Buffer> g_limits, g_groups, g_out;
ComPtr<ID3D11UnorderedAccessView> g_groupsUav, g_outUav, g_outUavCentre;
ComPtr<ID3D11ShaderResourceView> g_groupsSrv;
uint32_t g_groupCap = 0;
Slot g_slots[kSlots];
int g_slot = 0;
bool g_shaderFailed = false;

ComPtr<ID3D11Texture2D> g_stripStage;
bool g_stripPending = false;
uint64_t g_stripFrame = 0;

State g_state = State::Both;
bool g_armed = false;
bool g_routeHdr = false;
bool g_routeKnown = false;
uint64_t g_frame = 0;
uint64_t g_windowMs = 0;
uint32_t g_captured = 0;
bool g_stripCaptured = false;
bool g_historyReset = false;
bool g_loggedBusy = false;
Hotkey g_key;
std::string g_keyText;
uint32_t g_generation = ~0u;

const char* pointName(BloomProbePoint p) {
    switch (p) {
    case BloomProbePoint::HBefore: return "p0";
    case BloomProbePoint::HAfter: return "p1";
    case BloomProbePoint::Bloom: return "p2";
    case BloomProbePoint::Tone: return "p3";
    case BloomProbePoint::Final: return "p3f";
    default: return "p?";
    }
}

const char* routeName() {
    return g_routeKnown ? (g_routeHdr ? "hdr" : "copy") : "unknown";
}

bool due(BloomProbePoint point) {
    return (g_captured & (1u << static_cast<unsigned>(point))) == 0;
}

void mark(BloomProbePoint point) { g_captured |= 1u << static_cast<unsigned>(point); }

bool ensure(ID3D11Device* device, uint32_t groups) {
    if (g_shaderFailed) return false;
    if (!g_reduce) {
        if (FAILED(device->CreateComputeShader(kFlatBloomProbeReduceBytecode, sizeof(kFlatBloomProbeReduceBytecode), nullptr, g_reduce.GetAddressOf())) ||
            FAILED(device->CreateComputeShader(kFlatBloomProbeFinishBytecode, sizeof(kFlatBloomProbeFinishBytecode), nullptr, g_finish.GetAddressOf()))) {
            g_shaderFailed = true;
            Log::get().note("bloom-probe: compute shaders failed to create (no point can be measured)");
            return false;
        }
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(Limits);
        cb.Usage = D3D11_USAGE_DEFAULT;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&cb, nullptr, g_limits.GetAddressOf()))) return false;
        D3D11_BUFFER_DESC out{};
        out.ByteWidth = sizeof(Bin) * 2;
        out.Usage = D3D11_USAGE_DEFAULT;
        out.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        out.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        out.StructureByteStride = sizeof(Bin);
        if (FAILED(device->CreateBuffer(&out, nullptr, g_out.GetAddressOf()))) return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 1;
        if (FAILED(device->CreateUnorderedAccessView(g_out.Get(), &ud, g_outUav.GetAddressOf()))) return false;
        ud.Buffer.FirstElement = 1;
        if (FAILED(device->CreateUnorderedAccessView(g_out.Get(), &ud, g_outUavCentre.GetAddressOf()))) return false;
    }
    if (groups > g_groupCap) {
        g_groups.Reset();
        g_groupsUav.Reset();
        g_groupsSrv.Reset();
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(Bin) * groups;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = sizeof(Bin);
        if (FAILED(device->CreateBuffer(&bd, nullptr, g_groups.GetAddressOf()))) return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = groups;
        if (FAILED(device->CreateUnorderedAccessView(g_groups.Get(), &ud, g_groupsUav.GetAddressOf()))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = groups;
        if (FAILED(device->CreateShaderResourceView(g_groups.Get(), &sd, g_groupsSrv.GetAddressOf()))) return false;
        g_groupCap = groups;
    }
    return g_reduce && g_groups;
}

void unbind(ID3D11DeviceContext* context) {
    ID3D11ShaderResourceView* noneS[2] = {};
    ID3D11UnorderedAccessView* noneU[2] = {};
    context->CSSetShaderResources(0, 2, noneS);
    context->CSSetUnorderedAccessViews(0, 2, noneU, nullptr);
}

bool reduce(ID3D11DeviceContext* context, ID3D11ShaderResourceView* src, const Limits& full, const Limits& centre) {
    ID3D11ComputeShader* oldShader = nullptr;
    ID3D11ShaderResourceView* oldSrv[2] = {};
    ID3D11UnorderedAccessView* oldUav[2] = {};
    ID3D11Buffer* oldCb = nullptr;
    context->CSGetShader(&oldShader, nullptr, nullptr);
    context->CSGetShaderResources(0, 2, oldSrv);
    context->CSGetUnorderedAccessViews(0, 2, oldUav);
    context->CSGetConstantBuffers(0, 1, &oldCb);
    auto run = [&](const Limits& lim, ID3D11UnorderedAccessView* dest) {
        context->UpdateSubresource(g_limits.Get(), 0, nullptr, &lim, 0, 0);
        ID3D11Buffer* cb = g_limits.Get();
        context->CSSetConstantBuffers(0, 1, &cb);
        unbind(context);
        if (lim.groupCount) {
            context->CSSetShader(g_reduce.Get(), nullptr, 0);
            ID3D11ShaderResourceView* in = src;
            ID3D11UnorderedAccessView* groups = g_groupsUav.Get();
            context->CSSetShaderResources(0, 1, &in);
            context->CSSetUnorderedAccessViews(0, 1, &groups, nullptr);
            context->Dispatch((lim.extentX + kGroup - 1) / kGroup, (lim.extentY + kGroup - 1) / kGroup, 1);
            unbind(context);
        }
        context->CSSetShader(g_finish.Get(), nullptr, 0);
        ID3D11ShaderResourceView* bins = g_groupsSrv.Get();
        context->CSSetShaderResources(0, 1, &bins);
        context->CSSetUnorderedAccessViews(0, 1, &dest, nullptr);
        context->Dispatch(1, 1, 1);
        unbind(context);
    };
    run(full, g_outUav.Get());
    run(centre, g_outUavCentre.Get());
    context->CSSetShader(oldShader, nullptr, 0);
    context->CSSetShaderResources(0, 2, oldSrv);
    context->CSSetUnorderedAccessViews(0, 2, oldUav, nullptr);
    context->CSSetConstantBuffers(0, 1, &oldCb);
    if (oldShader) oldShader->Release();
    for (auto* p : oldSrv) if (p) p->Release();
    for (auto* p : oldUav) if (p) p->Release();
    if (oldCb) oldCb->Release();
    return true;
}

void logBin(BloomProbePoint point, int region, uint64_t frame, const Bin& b) {
    const double mean = b.n ? static_cast<double>(b.sum) / static_cast<double>(b.n) : 0.0;
    const double n = b.n ? static_cast<double>(b.n) : 1.0;
    Log::get().note("bloom-probe: %s frame=%llu region=%s samples=%u mean=%.4g max=%.4g above=%.4g,%.4g,%.4g,%.4g,%.4g,%.4g,%.4g,%.4g thr=%s",
        pointName(point), static_cast<unsigned long long>(frame), region ? "centre" : "full", b.n, mean,
        static_cast<double>(b.maxv), b.c0 / n, b.c1 / n, b.c2 / n, b.c3 / n, b.c4 / n, b.c5 / n, b.c6 / n, b.c7 / n, kThresholds);
}

void poll(ID3D11DeviceContext* context) {
    if (!context) return;
    FlatComputeInternalScope guard;
    for (auto& slot : g_slots) {
        if (!slot.pending || !slot.staging) continue;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = context->Map(slot.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        if (FAILED(hr) || !mapped.pData) {
            Log::get().note("bloom-probe: %s frame=%llu readback failed hr=0x%08X (the sample was taken; these are not zeros)",
                pointName(slot.point), static_cast<unsigned long long>(slot.frame), static_cast<unsigned>(hr));
            slot.pending = false;
            continue;
        }
        Bin bins[2]{};
        std::memcpy(bins, mapped.pData, sizeof(bins));
        context->Unmap(slot.staging.Get(), 0);
        logBin(slot.point, 0, slot.frame, bins[0]);
        logBin(slot.point, 1, slot.frame, bins[1]);
        slot.pending = false;
    }
    if (!g_stripPending || !g_stripStage) return;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = context->Map(g_stripStage.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
    if (FAILED(hr) || !mapped.pData) {
        Log::get().note("bloom-probe: strip frame=%llu readback failed hr=0x%08X (the copy was taken; these are not zeros)",
            static_cast<unsigned long long>(g_stripFrame), static_cast<unsigned>(hr));
        g_stripPending = false;
        return;
    }
    float texel[6]{};
    std::memcpy(texel, mapped.pData, sizeof(texel));
    context->Unmap(g_stripStage.Get(), 0);
    Log::get().note("bloom-probe: strip frame=%llu samples=6 %.4g %.4g %.4g %.4g %.4g %.4g",
        static_cast<unsigned long long>(g_stripFrame), texel[0], texel[1], texel[2], texel[3], texel[4], texel[5]);
    g_stripPending = false;
}

void tickWindow(uint64_t frame) {
    const uint64_t now = GetTickCount64();
    if (!g_windowMs) g_windowMs = now;
    if (now - g_windowMs < 1000) return;
    g_windowMs = now;
    char missed[96] = {};
    auto add = [&](const char* name, bool hit) {
        if (hit) return;
        if (missed[0]) std::strcat(missed, ",");
        std::strcat(missed, name);
    };
    add("p0", g_captured & 1u);
    add("p1", g_captured & 2u);
    add("p2", g_captured & 4u);
    add("p3", g_captured & 8u);
    add("p3f", g_captured & 16u);
    add("strip", g_stripCaptured);
    Log::get().note("bloom-probe: tick frame=%llu state=%s route=%s missed=%s",
        static_cast<unsigned long long>(frame), stateName(g_state), routeName(), missed[0] ? missed : "none");
    g_captured = 0;
    g_stripCaptured = false;
    g_loggedBusy = false;
}

void noteClash(const std::string& text) {
    uint32_t mods = 0;
    const int vk = virtualKeyFromName(text.c_str(), &mods);
    if (!vk) return;
    static EliteKeyboardUse uses[512];
    char file[64] = {};
    const int n = eliteBindsKeyboardUses(uses, 512, file, sizeof(file));
    if (n < 0) return;
    for (int i = 0; i < n; ++i) {
        uint32_t m2 = 0;
        const int vk2 = virtualKeyFromName(uses[i].binding, &m2);
        if (vk != vk2 || mods != m2) continue;
        Log::get().note("WARNING hotkey: bloom probe key %s CLASHES with Elite binding %s (%s). One press does both.",
            text.c_str(), uses[i].element, uses[i].binding);
        return;
    }
}

void rebind(Config& cfg) {
    const uint32_t generation = cfg.generation();
    if (g_generation == generation && g_armed) return;
    g_generation = generation;
    const std::string value = cfg.getString("hotkey.bloom_probe", kDefaultKey);
    if (g_armed && value == g_keyText) return;
    const bool first = !g_armed;
    g_keyText = value;
    g_key.setBinding(value.c_str());
    if (g_key.bound()) {
        char text[64] = "";
        hotkeyFormatBinding(g_key.binding(), text, sizeof(text));
        Log::get().note("hotkey: bloom probe key %s as %s (vk=%d mods=0x%X). Press it to cycle jitter x the AA pass. It does not write the ini.",
            first ? "bound" : "changed", text, g_key.key(), g_key.mods());
        noteClash(text);
    } else if (!value.empty()) {
        Log::get().note("hotkey: hotkey.bloom_probe = \"%s\" bound nothing; the bloom probe key is off and the state stays jitter=on aa=on.", value.c_str());
    } else {
        Log::get().note("hotkey: bloom probe key off (hotkey.bloom_probe is empty). The state stays jitter=on aa=on.");
    }
}

void sampleSrv(ID3D11DeviceContext* context, ID3D11ShaderResourceView* srv, BloomProbePoint point, uint64_t frame) {
    if (!context || !srv || !due(point)) return;
    ComPtr<ID3D11Resource> resource;
    srv->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource.As(&texture))) return;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (!desc.Width || !desc.Height) return;
    const uint32_t groupsX = (desc.Width + kGroup - 1) / kGroup;
    const uint32_t groupsY = (desc.Height + kGroup - 1) / kGroup;
    const uint32_t groups = groupsX * groupsY;
    if (!groups || groups > kMaxGroups) {
        Log::get().note("bloom-probe: %s frame=%llu not taken (groups=%u)", pointName(point), static_cast<unsigned long long>(frame), groups);
        return;
    }
    ComPtr<ID3D11Device> device;
    context->GetDevice(device.GetAddressOf());
    if (!device || !ensure(device.Get(), groups)) {
        if (!g_shaderFailed)
            Log::get().note("bloom-probe: %s frame=%llu gpu resources failed", pointName(point), static_cast<unsigned long long>(frame));
        return;
    }
    Slot* slot = nullptr;
    for (int i = 0; i < kSlots; ++i) {
        Slot& candidate = g_slots[(g_slot + i) % kSlots];
        if (!candidate.pending) { slot = &candidate; g_slot = (g_slot + i + 1) % kSlots; break; }
    }
    if (!slot) {
        if (!g_loggedBusy) {
            g_loggedBusy = true;
            Log::get().note("bloom-probe: %s frame=%llu not taken (readback slots full)", pointName(point), static_cast<unsigned long long>(frame));
        }
        return;
    }
    if (!slot->staging) {
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth = sizeof(Bin) * 2;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        sd.StructureByteStride = sizeof(Bin);
        if (FAILED(device->CreateBuffer(&sd, nullptr, slot->staging.GetAddressOf()))) {
            Log::get().note("bloom-probe: %s frame=%llu staging failed", pointName(point), static_cast<unsigned long long>(frame));
            return;
        }
    }
    const uint32_t centreW = desc.Width / 2;
    const uint32_t centreH = desc.Height / 2;
    const uint32_t centreGX = centreW ? (centreW + kGroup - 1) / kGroup : 0;
    const uint32_t centreGY = centreH ? (centreH + kGroup - 1) / kGroup : 0;
    Limits full{desc.Width, desc.Height, 0, 0, desc.Width, desc.Height, groupsX, groups, 0, 0, 0, 0};
    Limits centre{desc.Width, desc.Height, desc.Width / 4, desc.Height / 4, centreW, centreH, centreGX, centreGX * centreGY, 0, 0, 0, 0};
    FlatComputeInternalScope guard;
    if (!reduce(context, srv, full, centre)) return;
    context->CopyResource(slot->staging.Get(), g_out.Get());
    slot->pending = true;
    slot->point = point;
    slot->frame = frame;
    mark(point);
}

}  // namespace

bool bloomProbeJitter() noexcept { return g_state == State::Both || g_state == State::JitterOnly; }
bool bloomProbeAaPass() noexcept { return g_state == State::Both || g_state == State::AaOnly; }
bool bloomProbeDue(BloomProbePoint point) noexcept { return due(point); }
bool bloomProbeStripDue() noexcept { return !g_stripCaptured && !g_stripPending; }
bool bloomProbeTakeHistoryReset() noexcept {
    const bool armed = g_historyReset;
    g_historyReset = false;
    return armed;
}
uint64_t bloomProbeFrame() noexcept { return g_frame; }

void bloomProbeNoteFrame(uint64_t frame) noexcept {
    if (g_frame == frame) return;
    g_frame = frame;
    g_routeKnown = false;
}

void bloomProbeNoteRoute(bool hdrRoute, uint64_t frame) {
    g_routeHdr = hdrRoute;
    g_routeKnown = true;
    g_frame = frame;
}

void bloomProbeTick(ID3D11DeviceContext* context, uint64_t frame) {
    if (frame) g_frame = frame;
    Config& cfg = Config::get();
    rebind(cfg);
    if (!g_armed) {
        g_armed = true;
        Log::get().note("bloom-probe: armed state=%s key=%s. No p-line yet means that point has not been sampled.",
            stateName(g_state), g_keyText.empty() ? kDefaultKey : g_keyText.c_str());
    }
    if (g_key.pressed()) {
        const bool aaWas = bloomProbeAaPass();
        g_state = static_cast<State>((static_cast<uint8_t>(g_state) + 1) % 4);
        if (aaWas && !bloomProbeAaPass()) g_historyReset = true;
        g_windowMs = GetTickCount64();
        g_captured = 0;
        g_stripCaptured = false;
        g_loggedBusy = false;
        Log::get().note("bloom-probe: state %s route=%s frame=%llu",
            stateName(g_state), routeName(), static_cast<unsigned long long>(frame));
        menuShowNotice(stateName(g_state));
    }
    poll(context);
    tickWindow(frame);
}

void bloomProbeSample(ID3D11DeviceContext* context, ID3D11ShaderResourceView* srv, BloomProbePoint point, uint64_t frame) {
    sampleSrv(context, srv, point, frame);
}

void bloomProbeSampleTarget(ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv, BloomProbePoint point, uint64_t frame) {
    if (!context || !rtv || !due(point)) return;
    ComPtr<ID3D11Resource> resource;
    rtv->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource.As(&texture))) return;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
    rtv->GetDesc(&viewDesc);
    const bool typeless = desc.Format == DXGI_FORMAT_R32G32B32A32_TYPELESS || desc.Format == DXGI_FORMAT_R32G32B32_TYPELESS ||
        desc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS || desc.Format == DXGI_FORMAT_R32G32_TYPELESS ||
        desc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS || desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
        desc.Format == DXGI_FORMAT_R16G16_TYPELESS || desc.Format == DXGI_FORMAT_R32_TYPELESS ||
        desc.Format == DXGI_FORMAT_R8G8_TYPELESS || desc.Format == DXGI_FORMAT_R16_TYPELESS ||
        desc.Format == DXGI_FORMAT_R8_TYPELESS || desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    ComPtr<ID3D11Device> device;
    context->GetDevice(device.GetAddressOf());
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = typeless ? viewDesc.Format : desc.Format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (!device || FAILED(device->CreateShaderResourceView(texture.Get(), &sd, srv.GetAddressOf()))) {
        Log::get().note("bloom-probe: %s frame=%llu target srv failed", pointName(point), static_cast<unsigned long long>(frame));
        return;
    }
    // The draw just wrote this target. It cannot stay bound as a render target while the reduction reads it.
    FlatComputeInternalScope guard;
    ID3D11RenderTargetView* oldRt = nullptr;
    ID3D11DepthStencilView* oldDs = nullptr;
    context->OMGetRenderTargets(1, &oldRt, &oldDs);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    sampleSrv(context, srv.Get(), point, frame);
    context->OMSetRenderTargets(1, &oldRt, oldDs);
    if (oldRt) oldRt->Release();
    if (oldDs) oldDs->Release();
}

void bloomProbeNoteStrip(ID3D11DeviceContext* context, ID3D11UnorderedAccessView* uav, uint64_t frame) {
    if (!context || !uav || g_stripCaptured || g_stripPending) return;
    ComPtr<ID3D11Resource> resource;
    uav->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource.As(&texture))) return;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Width < 6 || desc.Height != 1 || desc.Format != DXGI_FORMAT_R32_FLOAT) return;
    ComPtr<ID3D11Device> device;
    context->GetDevice(device.GetAddressOf());
    if (!g_stripStage) {
        D3D11_TEXTURE2D_DESC sd = desc;
        sd.Width = 6;
        sd.Height = 1;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        if (!device || FAILED(device->CreateTexture2D(&sd, nullptr, g_stripStage.GetAddressOf()))) {
            Log::get().note("bloom-probe: strip frame=%llu staging failed", static_cast<unsigned long long>(frame));
            return;
        }
    }
    FlatComputeInternalScope guard;
    ID3D11UnorderedAccessView* oldUav[2] = {};
    context->CSGetUnorderedAccessViews(0, 2, oldUav);
    ID3D11UnorderedAccessView* noneUav[2] = {};
    context->CSSetUnorderedAccessViews(0, 2, noneUav, nullptr);
    D3D11_BOX box{0, 0, 0, 6, 1, 1};
    context->CopySubresourceRegion(g_stripStage.Get(), 0, 0, 0, 0, texture.Get(), 0, &box);
    context->CSSetUnorderedAccessViews(0, 2, oldUav, nullptr);
    for (auto* p : oldUav) if (p) p->Release();
    g_stripPending = true;
    g_stripFrame = frame;
    g_stripCaptured = true;
}

bool bloomProbeIsBloomDraw(uint64_t vs, uint64_t ps) noexcept { return vs == kBloomVs && ps == kBloomPs; }
bool bloomProbeIsToneDraw(uint64_t vs, uint64_t ps) noexcept { return vs == kToneVs && ps == kTonePs; }
bool bloomProbeIsStripShader(uint64_t hash) noexcept { return hash == kStripShader; }

}  // namespace edvr

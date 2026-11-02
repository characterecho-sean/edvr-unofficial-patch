#include "depth_writer_probe.h"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../common/log.h"

namespace edvr {

namespace detail {
bool g_depthWriterActive = false;
}  // namespace detail

namespace {

constexpr uint32_t kMaxDraws = 6000;        // draws followed in the frame, both eyes
constexpr uint32_t kMaxLoggedOwners = 24;   // owners named in the log, most pixels first

// Stamps the draw's id where the depth differs from the copy taken before it. asuint, so a NaN or a -0 is a change too.
const char kWriterHlsl[] =
    "Texture2D<float> Cur : register(t0);\n"
    "Texture2D<float> Prev : register(t1);\n"
    "RWTexture2D<uint> Writer : register(u0);\n"
    "cbuffer C : register(b0) { uint2 size; uint id; uint pad; };\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint3 t : SV_DispatchThreadID) {\n"
    "    if (t.x >= size.x || t.y >= size.y) return;\n"
    "    if (asuint(Cur[t.xy]) != asuint(Prev[t.xy])) Writer[t.xy] = id;\n"
    "}\n";

struct DrawRow {
    int eye = -1;
    DrawInfo info;
    bool stencilEnable = false;
    uint32_t stencilRef = 0;
    uint32_t stencilFunc = 0;
    uint32_t colourMask = 0;      // RT0's write mask; 0 with no blend state's say (the default is all)
    bool rtv0Bound = false;
    uint32_t rtv0Width = 0, rtv0Height = 0, rtv0Format = 0;
    bool rasterKnown = false;
    int32_t depthBias = 0;
    float biasClamp = 0.0f, slopeBias = 0.0f;
    bool depthClip = true;
    uint32_t cull = 0, fill = 0;
    uint64_t finalPixels = 0;     // filled at the close: the pixels whose last depth change was this draw's
};

struct EyeState {
    ID3D11Texture2D* depth = nullptr;            // identity of the depth texture the baseline was taken from (not a reference)
    ID3D11Texture2D* scratch[2] = {};
    ID3D11ShaderResourceView* srv[2] = {};
    ID3D11Texture2D* map = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    uint32_t width = 0, height = 0;
    int cur = 0;
    bool baseline = false;
    bool declined = false;   // counted once: the eye's depth is a format or kind the probe does not follow
    uint32_t followed = 0;
};

DepthWriterEyeResolver g_resolver = nullptr;
bool g_armed = false;
uint32_t g_frame = 0;
uint32_t g_targetFrame = 0;
wchar_t g_stamp[16] = L"";
std::wstring g_outputDir;   // empty: Log's directory + \pool

ID3D11Device* g_device = nullptr;            // the device the objects below were made on (a reference)
ID3D11ComputeShader* g_cs = nullptr;
ID3D11Buffer* g_cb = nullptr;
bool g_compileFailed = false;
EyeState g_eye[2];
std::vector<DrawRow> g_rows;
uint32_t g_declined = 0;      // the eye's depth was a format, size or kind the probe does not follow
uint32_t g_dropped = 0;       // draws past kMaxDraws
uint32_t g_skipped = 0;       // depth-writing draws that were not into an eye's scene depth (not counted by the resolver)

typedef HRESULT(WINAPI* PfnD3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

// d3dcompiler_47.dll is loaded once and never freed (the eye-dump crash of 2026-10-23: a blob's vtable lives in it).
bool compileShader(ID3DBlob** code) {
    static PfnD3DCompile compile = nullptr;
    if (!compile) {
        HMODULE mod = LoadLibraryW(L"d3dcompiler_47.dll");
        if (mod) compile = reinterpret_cast<PfnD3DCompile>(GetProcAddress(mod, "D3DCompile"));
        if (!compile) return false;
    }
    ID3DBlob* errors = nullptr;
    const HRESULT hr = compile(kWriterHlsl, sizeof(kWriterHlsl) - 1, "depth_writer", nullptr, nullptr, "main", "cs_5_0", 0, 0, code, &errors);
    if (errors) errors->Release();
    return SUCCEEDED(hr) && *code;
}

void releaseEye(EyeState& e) {
    for (auto& s : e.srv) if (s) { s->Release(); s = nullptr; }
    for (auto& t : e.scratch) if (t) { t->Release(); t = nullptr; }
    if (e.uav) { e.uav->Release(); e.uav = nullptr; }
    if (e.map) { e.map->Release(); e.map = nullptr; }
    e = EyeState{};
}

void releaseAll() {
    for (auto& e : g_eye) releaseEye(e);
    if (g_cs) { g_cs->Release(); g_cs = nullptr; }
    if (g_cb) { g_cb->Release(); g_cb = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
    g_compileFailed = false;
}

void resetRun() {
    for (auto& e : g_eye) releaseEye(e);
    g_rows.clear();
    g_declined = g_dropped = g_skipped = 0;
}

// The view format that reads the depth channel of a depth texture of this (typeless) format, UNKNOWN if the probe does not follow it.
DXGI_FORMAT depthViewFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// The shader, the constant buffer and the device they belong to, made once per device.
bool ensureShared(ID3D11DeviceContext* ctx) {
    if (g_compileFailed) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    if (g_device && g_device != dev) releaseAll();
    if (!g_device) g_device = dev;
    else dev->Release();
    if (g_cs && g_cb) return true;
    if (!g_cs) {
        ID3DBlob* code = nullptr;
        if (!compileShader(&code)) {
            g_compileFailed = true;
            Log::get().note("depth writer probe: the writer shader did not compile (d3dcompiler_47.dll missing or refused it); no map this run.");
            return false;
        }
        const HRESULT hr = g_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_cs);
        code->Release();
        if (FAILED(hr) || !g_cs) {
            g_compileFailed = true;
            Log::get().note("depth writer probe: the writer shader compiled but did not create (0x%08X); no map this run.", static_cast<unsigned>(hr));
            return false;
        }
    }
    if (!g_cb) {
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = 16;
        cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(g_device->CreateBuffer(&cbd, nullptr, &g_cb)) || !g_cb) return false;
    }
    return true;
}

// The eye's scratch pair, map and UAV, sized to its depth texture. False declines the eye for the run.
bool ensureEye(ID3D11DeviceContext* ctx, EyeState& e, ID3D11Texture2D* depth, const D3D11_TEXTURE2D_DESC& td) {
    if (e.depth == depth && e.map) return true;
    if (e.depth && e.depth != depth) return false;   // the eye's depth changed mid-frame: follow neither
    const DXGI_FORMAT viewFmt = depthViewFormat(td.Format);
    if (viewFmt == DXGI_FORMAT_UNKNOWN || td.ArraySize != 1 || td.SampleDesc.Count != 1 || td.MipLevels != 1) return false;
    if (!ensureShared(ctx)) return false;
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_DEFAULT;
    sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.CPUAccessFlags = 0;
    sd.MiscFlags = 0;
    for (int i = 0; i < 2; ++i) {
        if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &e.scratch[i])) || !e.scratch[i]) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = viewFmt;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
        if (FAILED(g_device->CreateShaderResourceView(e.scratch[i], &vd, &e.srv[i])) || !e.srv[i]) return false;
    }
    D3D11_TEXTURE2D_DESC md{};
    md.Width = td.Width; md.Height = td.Height; md.MipLevels = 1; md.ArraySize = 1;
    md.Format = DXGI_FORMAT_R32_UINT;
    md.SampleDesc.Count = 1;
    md.Usage = D3D11_USAGE_DEFAULT;
    md.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(g_device->CreateTexture2D(&md, nullptr, &e.map)) || !e.map) return false;
    if (FAILED(g_device->CreateUnorderedAccessView(e.map, nullptr, &e.uav)) || !e.uav) return false;
    e.depth = depth;
    e.width = td.Width;
    e.height = td.Height;
    return true;
}

// The eye this context's bound depth view is, and its texture, or false (with *tex null) when it is no eye's scene depth.
// The caller releases *dsv and *tex.
bool boundEyeDepth(ID3D11DeviceContext* ctx, int* eye, ID3D11DepthStencilView** dsv, ID3D11Texture2D** tex, ID3D11RenderTargetView** rtv0) {
    *dsv = nullptr; *tex = nullptr; *rtv0 = nullptr;
    ctx->OMGetRenderTargets(1, rtv0, dsv);
    if (!*dsv) return false;
    ID3D11Resource* res = nullptr;
    (*dsv)->GetResource(&res);
    if (!res) return false;
    res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(tex));
    res->Release();
    if (!*tex) return false;
    // The resolver names the depth probe's own view; the game may also bind another view of the same texture (a different format
    // or flags), whose draws write the same depth. Once an eye's baseline is taken its texture is the identity.
    *eye = -1;
    if (g_resolver && g_resolver(*dsv, eye) && (*eye == 0 || *eye == 1)) return true;
    for (int i = 0; i < 2; ++i)
        if (g_eye[i].baseline && g_eye[i].depth == *tex) { *eye = i; return true; }
    return false;
}

void takeBaseline(ID3D11DeviceContext* ctx, EyeState& e, ID3D11Texture2D* depth) {
    ctx->CopyResource(e.scratch[0], depth);
    const UINT zero[4] = {0, 0, 0, 0};
    ctx->ClearUnorderedAccessViewUint(e.uav, zero);
    e.cur = 0;
    e.baseline = true;
}

// Follows one depth-writing draw: copy the depth, compare with the copy before it, stamp the id. The context's compute state is put
// back as it was; nothing else of the game's state is touched.
void follow(ID3D11DeviceContext* ctx, EyeState& e, ID3D11Texture2D* depth, uint32_t id) {
    const int next = 1 - e.cur;
    ctx->CopyResource(e.scratch[next], depth);
    ID3D11ComputeShader* oldCs = nullptr;
    ctx->CSGetShader(&oldCs, nullptr, nullptr);
    ID3D11ShaderResourceView* oldSrv[2] = {};
    ctx->CSGetShaderResources(0, 2, oldSrv);
    ID3D11UnorderedAccessView* oldUav[1] = {};
    ctx->CSGetUnorderedAccessViews(0, 1, oldUav);
    ID3D11Buffer* oldCb = nullptr;
    ctx->CSGetConstantBuffers(0, 1, &oldCb);
    const UINT constants[4] = {e.width, e.height, id, 0};
    ctx->UpdateSubresource(g_cb, 0, nullptr, constants, 0, 0);
    ID3D11ShaderResourceView* srvs[2] = {e.srv[next], e.srv[e.cur]};
    const UINT keep[1] = {0xFFFFFFFFu};
    ID3D11UnorderedAccessView* uavs[1] = {e.uav};
    ctx->CSSetShader(g_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 2, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, keep);
    ctx->CSSetConstantBuffers(0, 1, &g_cb);
    ctx->Dispatch((e.width + 7u) / 8u, (e.height + 7u) / 8u, 1);
    ctx->CSSetShader(oldCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 2, oldSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, oldUav, keep);
    ctx->CSSetConstantBuffers(0, 1, &oldCb);
    if (oldCs) oldCs->Release();
    for (auto* s : oldSrv) if (s) s->Release();
    if (oldUav[0]) oldUav[0]->Release();
    if (oldCb) oldCb->Release();
    e.cur = next;
    ++e.followed;
}

void writeJson(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return;
    std::fprintf(f, "{\"version\":1,\"draws\":[");
    for (size_t i = 0; i < g_rows.size(); ++i) {
        const DrawRow& r = g_rows[i];
        std::fprintf(f,
            "%s{\"id\":%u,\"eye\":%d,\"vs\":\"%016llX\",\"ps\":\"%016llX\",\"topology\":%u,\"count\":%u,\"instances\":%u,\"indirect\":%d,"
            "\"depth\":{\"enable\":%d,\"func\":%u,\"write\":%u},\"stencil\":{\"enable\":%d,\"ref\":%u,\"func\":%u},"
            "\"blend\":{\"enable\":%d,\"src\":%u,\"dest\":%u,\"rt0_mask\":%u},"
            "\"rtv0\":{\"bound\":%d,\"width\":%u,\"height\":%u,\"format\":%u},"
            "\"raster\":{\"known\":%d,\"depth_bias\":%d,\"bias_clamp\":%.9g,\"slope_bias\":%.9g,\"depth_clip\":%d,\"cull\":%u,\"fill\":%u},"
            "\"final_pixels\":%llu}",
            i ? "," : "", static_cast<unsigned>(i + 1), r.eye, static_cast<unsigned long long>(r.info.vsHash), static_cast<unsigned long long>(r.info.psHash),
            r.info.topology, r.info.count, r.info.instances, r.info.indirect ? 1 : 0,
            r.info.depthEnable ? 1 : 0, r.info.depthFunc, r.info.depthWriteMask,
            r.stencilEnable ? 1 : 0, r.stencilRef, r.stencilFunc,
            r.info.blendEnable ? 1 : 0, r.info.blendSrc, r.info.blendDest, r.colourMask,
            r.rtv0Bound ? 1 : 0, r.rtv0Width, r.rtv0Height, r.rtv0Format,
            r.rasterKnown ? 1 : 0, r.depthBias, static_cast<double>(r.biasClamp), static_cast<double>(r.slopeBias), r.depthClip ? 1 : 0, r.cull, r.fill,
            static_cast<unsigned long long>(r.finalPixels));
    }
    std::fprintf(f, "]}\n");
    std::fclose(f);
}

bool writeMap(const std::wstring& path, uint32_t w, uint32_t h, uint32_t eye, const std::vector<uint32_t>& pixels) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return false;
    // EDVRTEX1: the magic, then version, width, height, DXGI format (42 R32_UINT), row bytes, frame, eye, UI-bound, UI-flags (tools/eye_inputs.py).
    const uint32_t header[9] = {1u, w, h, 42u, w * 4u, g_targetFrame, eye, 0u, 0u};
    bool ok = std::fwrite("EDVRTEX1", 1, 8, f) == 8 && std::fwrite(header, sizeof(header), 1, f) == 1 &&
              std::fwrite(pixels.data(), 4, pixels.size(), f) == pixels.size();
    ok = (std::fclose(f) == 0) && ok;
    return ok;
}

void closeProbedFrame(ID3D11DeviceContext* ctx) {
    const std::wstring dir = g_outputDir.empty() ? Log::get().dir() + L"\\pool" : g_outputDir;
    CreateDirectoryW(dir.c_str(), nullptr);
    uint32_t written = 0;
    struct Owner { uint32_t id; uint64_t pixels; };
    std::vector<Owner> owners;
    for (int eye = 0; eye < 2; ++eye) {
        EyeState& e = g_eye[eye];
        if (!e.baseline || !e.map || !ctx || !g_device) continue;
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = e.width; sd.Height = e.height; sd.MipLevels = 1; sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_R32_UINT;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* staging = nullptr;
        if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &staging)) || !staging) continue;
        ctx->CopyResource(staging, e.map);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            std::vector<uint32_t> pixels(static_cast<size_t>(e.width) * e.height);
            for (uint32_t y = 0; y < e.height; ++y)
                std::memcpy(pixels.data() + static_cast<size_t>(y) * e.width, static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                            static_cast<size_t>(e.width) * 4u);
            ctx->Unmap(staging, 0);
            for (uint32_t v : pixels)
                if (v && v <= g_rows.size()) ++g_rows[v - 1].finalPixels;
            wchar_t name[MAX_PATH];
            _snwprintf_s(name, MAX_PATH, _TRUNCATE, L"%s\\depthwriter_%s_e%d.bin", dir.c_str(), g_stamp, eye);
            if (writeMap(name, e.width, e.height, static_cast<uint32_t>(eye), pixels)) ++written;
        }
        staging->Release();
    }
    wchar_t jname[MAX_PATH];
    _snwprintf_s(jname, MAX_PATH, _TRUNCATE, L"%s\\depthwriter_%s.json", dir.c_str(), g_stamp);
    writeJson(jname);
    for (size_t i = 0; i < g_rows.size(); ++i)
        if (g_rows[i].finalPixels) owners.push_back({static_cast<uint32_t>(i + 1), g_rows[i].finalPixels});
    std::sort(owners.begin(), owners.end(), [](const Owner& a, const Owner& b) { return a.pixels > b.pixels; });
    Log::get().note("depth writer probe: run %ls frame %u done -- %u depth-writing draws followed (eye 0 %u, eye 1 %u), %u not into an eye's depth, "
                    "%u declined (format or size), %u dropped past the cap; %u map(s) and the draw table written to edvr_logs\\pool\\depthwriter_%ls*.",
                    g_stamp, g_targetFrame, static_cast<unsigned>(g_rows.size()), g_eye[0].followed, g_eye[1].followed, g_skipped, g_declined, g_dropped,
                    written, g_stamp);
    for (size_t i = 0; i < owners.size() && i < kMaxLoggedOwners; ++i) {
        const DrawRow& r = g_rows[owners[i].id - 1];
        Log::get().note("depth writer probe: owner #%u eye %d vs %016llX ps %016llX count %u inst %u depth func %u write %u stencil %s ref %u "
                        "raster bias %d slope %.3g -- owns %llu pixels of the final depth",
                        owners[i].id, r.eye, static_cast<unsigned long long>(r.info.vsHash), static_cast<unsigned long long>(r.info.psHash),
                        r.info.count, r.info.instances, r.info.depthFunc, r.info.depthWriteMask, r.stencilEnable ? "on" : "off", r.stencilRef,
                        r.depthBias, static_cast<double>(r.slopeBias), static_cast<unsigned long long>(owners[i].pixels));
    }
}

}  // namespace

void depthWriterSetEyeResolver(DepthWriterEyeResolver resolver) { g_resolver = resolver; }

void depthWriterSetOutputDir(const wchar_t* dir) { g_outputDir = dir ? dir : L""; }

void depthWriterArm(const wchar_t* stamp) {
    if (g_armed) return;
    g_armed = true;
    g_targetFrame = g_frame + 2;   // the whole next frame, as the pixel probe
    resetRun();
    wcsncpy_s(g_stamp, stamp ? stamp : L"", _TRUNCATE);
    Log::get().note("depth writer probe: armed for frame %u (run %ls): every depth-writing draw into an eye's scene depth will be followed.",
                    g_targetFrame, g_stamp);
}

bool depthWriterArmed() { return g_armed; }

void depthWriterFrameBoundary(ID3D11DeviceContext* ctx) {
    if (g_armed && g_frame + 1 >= g_targetFrame) {
        if (ctx) closeProbedFrame(ctx);
        resetRun();
        g_armed = false;
    }
    ++g_frame;
    detail::g_depthWriterActive = g_armed && g_frame + 1 == g_targetFrame;
}

void depthWriterBeforeDraw(ID3D11DeviceContext* ctx) {
    if (!g_armed || !ctx || g_frame + 1 != g_targetFrame) return;
    int eye = -1;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11Texture2D* depth = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    const bool isEye = boundEyeDepth(ctx, &eye, &dsv, &depth, &rtv);
    if (isEye && !g_eye[eye].baseline && !g_eye[eye].declined) {
        D3D11_TEXTURE2D_DESC td{};
        depth->GetDesc(&td);
        if (ensureEye(ctx, g_eye[eye], depth, td)) takeBaseline(ctx, g_eye[eye], depth);
        else { g_eye[eye].declined = true; ++g_declined; }
    }
    if (depth) depth->Release();
    if (dsv) dsv->Release();
    if (rtv) rtv->Release();
}

void depthWriterAfterDraw(ID3D11DeviceContext* ctx, const DrawInfo& info) {
    if (!g_armed || !ctx || g_frame + 1 != g_targetFrame) return;
    int eye = -1;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11Texture2D* depth = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    const bool isEye = boundEyeDepth(ctx, &eye, &dsv, &depth, &rtv);
    do {
        if (!isEye) { if (dsv) ++g_skipped; break; }
        EyeState& e = g_eye[eye];
        if (!e.baseline || e.depth != depth) break;               // declined, or not this eye's depth
        // A draw that cannot write depth cannot be its writer: no copy.
        ID3D11DepthStencilState* ds = nullptr;
        UINT stencilRef = 0;
        ctx->OMGetDepthStencilState(&ds, &stencilRef);
        D3D11_DEPTH_STENCIL_DESC dd{};
        bool haveDs = false;
        if (ds) { ds->GetDesc(&dd); ds->Release(); haveDs = true; }
        if (haveDs && (!dd.DepthEnable || dd.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO)) break;
        if (g_rows.size() >= kMaxDraws) { ++g_dropped; break; }
        DrawRow row;
        row.eye = eye;
        row.info = info;
        if (haveDs) {
            row.info.depthEnable = dd.DepthEnable != 0;
            row.info.depthWriteMask = static_cast<uint32_t>(dd.DepthWriteMask);
            row.info.depthFunc = static_cast<uint32_t>(dd.DepthFunc);
            row.stencilEnable = dd.StencilEnable != 0;
            row.stencilFunc = static_cast<uint32_t>(dd.FrontFace.StencilFunc);
        } else {
            row.info.depthEnable = true;   // the API's default state: depth on, write all, LESS
            row.info.depthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            row.info.depthFunc = D3D11_COMPARISON_LESS;
        }
        row.stencilRef = stencilRef;
        ID3D11BlendState* bs = nullptr;
        FLOAT bf[4]{};
        UINT sm = 0;
        ctx->OMGetBlendState(&bs, bf, &sm);
        row.colourMask = 0xF;
        if (bs) {
            D3D11_BLEND_DESC bd{};
            bs->GetDesc(&bd);
            row.colourMask = bd.RenderTarget[0].RenderTargetWriteMask;
            bs->Release();
        }
        ID3D11RasterizerState* rs = nullptr;
        ctx->RSGetState(&rs);
        if (rs) {
            D3D11_RASTERIZER_DESC rd{};
            rs->GetDesc(&rd);
            row.rasterKnown = true;
            row.depthBias = rd.DepthBias;
            row.biasClamp = rd.DepthBiasClamp;
            row.slopeBias = rd.SlopeScaledDepthBias;
            row.depthClip = rd.DepthClipEnable != 0;
            row.cull = static_cast<uint32_t>(rd.CullMode);
            row.fill = static_cast<uint32_t>(rd.FillMode);
            rs->Release();
        }
        if (rtv) {
            row.rtv0Bound = true;
            ID3D11Resource* rres = nullptr;
            rtv->GetResource(&rres);
            if (rres) {
                ID3D11Texture2D* rtex = nullptr;
                if (SUCCEEDED(rres->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&rtex))) && rtex) {
                    D3D11_TEXTURE2D_DESC rtd{};
                    rtex->GetDesc(&rtd);
                    row.rtv0Width = rtd.Width; row.rtv0Height = rtd.Height; row.rtv0Format = static_cast<uint32_t>(rtd.Format);
                    rtex->Release();
                }
                rres->Release();
            }
        }
        g_rows.push_back(row);
        follow(ctx, e, depth, static_cast<uint32_t>(g_rows.size()));
    } while (false);
    if (depth) depth->Release();
    if (dsv) dsv->Release();
    if (rtv) rtv->Release();
}

void depthWriterShutdown() {
    releaseAll();
    g_rows.clear();
    g_armed = false;
    detail::g_depthWriterActive = false;
}

}  // namespace edvr

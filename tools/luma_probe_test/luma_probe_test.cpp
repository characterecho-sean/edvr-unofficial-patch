// WARP exercise of the production probe: exact format bytes and old grid
// statistics, observed transfer footprint, timeout and throttle semantics.
#include "../../src/d3d11/luma_probe.cpp"
#include <vector>
#include <string>
#include <algorithm>
#include <cstdarg>
#include <cstdlib>

namespace edvr {
std::vector<std::string> testLog;
Log& Log::get() { static Log log; return log; }
Log::~Log() = default;
// periodic_work.h's production clock (luma_probe.cpp times its rounds with it).
int64_t qpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
int64_t qpcFrequency() { LARGE_INTEGER t; QueryPerformanceFrequency(&t); return t.QuadPart; }
void Log::note(const char* fmt, ...) {
    char line[1024]; va_list args; va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args); va_end(args);
    testLog.emplace_back(line);
}
}
using namespace edvr;
unsigned checks = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { printf("FAIL: %s\n", what); exit(1); }
}
void hr(HRESULT result) { check(SUCCEEDED(result), "D3D operation"); }

// Forward real commands, observing only production calls. Reference
// readbacks and benchmark waits run outside the spy's lifetime.
struct CommandSpy {
    using CopyFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
    using MapFn = HRESULT (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
    static CommandSpy* active;
    ID3D11DeviceContext* ctx;
    void** original;
    void* slots[115];
    UINT sourceW, sourceH;
    DXGI_FORMAT format;
    size_t bpp;
    unsigned copies = 0, maps = 0;
    uint64_t copiedBytes = 0;
    bool forcePending = false;
    static void copy(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, UINT x, UINT y, UINT z,
                     ID3D11Resource* src, UINT ss, const D3D11_BOX* box) {
        auto& s = *active;
        D3D11_TEXTURE2D_DESC td{}; static_cast<ID3D11Texture2D*>(dst)->GetDesc(&td);
        check(td.Width == s.sourceW && td.Height == 16 && td.Format == s.format &&
              td.Usage == D3D11_USAGE_STAGING && td.MipLevels == 1 && td.ArraySize == 1,
              "production staging: source format/width, sixteen rows");
        check(box && ds == 0 && ss == 0 && x == 0 && z == 0 && y == s.copies,
              "production copy: exact destination row, subresource zero");
        const UINT referenceY = std::min(s.sourceH - 1, UINT((float(s.copies) + .5f) / 16.0f * float(s.sourceH)));
        check(box->left == 0 && box->right == s.sourceW && box->top == referenceY &&
              box->bottom == referenceY + 1 && box->front == 0 && box->back == 1,
              "production copy: historical sampled source row only");
        s.copiedBytes += uint64_t(box->right - box->left) * (box->bottom - box->top) * s.bpp;
        ++s.copies;
        reinterpret_cast<CopyFn>(s.original[46])(c, dst, ds, x, y, z, src, ss, box);
    }
    static HRESULT map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub,
                       D3D11_MAP kind, UINT flags, D3D11_MAPPED_SUBRESOURCE* out) {
        ++active->maps;
        check(kind == D3D11_MAP_READ && flags == D3D11_MAP_FLAG_DO_NOT_WAIT,
              "production reads always nonblocking");
        if (active->forcePending) return DXGI_ERROR_WAS_STILL_DRAWING;
        return reinterpret_cast<MapFn>(active->original[14])(c, r, sub, kind, flags, out);
    }
    void setTable(void** table) {
        DWORD old = 0, ignored = 0;
        check(VirtualProtect(ctx, sizeof(void*), PAGE_READWRITE, &old) != 0, "spy vptr writable");
        *reinterpret_cast<void***>(ctx) = table;
        check(VirtualProtect(ctx, sizeof(void*), old, &ignored) != 0, "spy protection restored");
    }
    CommandSpy(ID3D11DeviceContext* c, UINT w, UINT h, DXGI_FORMAT f, size_t bytes)
        : ctx(c), original(*reinterpret_cast<void***>(c)), sourceW(w), sourceH(h), format(f), bpp(bytes) {
        check(!active, "one spy owner");
        memcpy(slots, original, sizeof(slots));
        slots[46] = reinterpret_cast<void*>(&copy); slots[14] = reinterpret_cast<void*>(&map);
        active = this; setTable(slots);
    }
    ~CommandSpy() { setTable(original); active = nullptr; }
};
CommandSpy* CommandSpy::active = nullptr;

void fillPixels(std::vector<uint8_t>& bytes, FormatKind kind, UINT w, UINT h) {
    const size_t bpp = bytesPerPixel(kind);
    bytes.resize(size_t(w) * h * bpp);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        uint8_t* p = bytes.data() + i * bpp;
        const unsigned v = unsigned((i * 37 + i / w * 19) % 257);
        if (kind == FormatKind::Rgba16F) {
            const uint16_t pixel[4] = {uint16_t(v % 7 ? 0x3800 + v : 0), uint16_t(0x3000 + v), 0x3C00, 0x3C00};
            memcpy(p, pixel, sizeof(pixel));
        } else if (kind == FormatKind::Rgba32F) {
            const float pixel[4] = {float(v) / 256, float(v % 17) / 16, v % 7 ? .75f : 0, 1};
            memcpy(p, pixel, sizeof(pixel));
        } else if (kind == FormatKind::R11G11B10F) {
            const uint32_t pixel = (0x380u + (v & 127)) | ((0x300u + (v & 127)) << 11) | ((0x1C0u + (v & 31)) << 22);
            memcpy(p, &pixel, sizeof(pixel));
        } else for (size_t k = 0; k < bpp; ++k) p[k] = uint8_t((v + k * 29) & 255);
    }
}

Ptr<ID3D11Texture2D> texture(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT f,
                           const std::vector<uint8_t>& bytes, size_t bpp, bool mipArray = false) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = td.ArraySize = mipArray ? 2 : 1; td.Format = f;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA data[4];
    for (auto& s : data) s = {bytes.data(), UINT(w * bpp), 0};
    Ptr<ID3D11Texture2D> result;
    hr(dev->CreateTexture2D(&td, data, &result));
    return result;
}

void equivalence(ID3D11Device* dev, ID3D11DeviceContext* ctx, DXGI_FORMAT f, UINT w, UINT h, bool mipArray = false) {
    const auto kind = classifyFormat(f); const size_t bpp = bytesPerPixel(kind);
    std::vector<uint8_t> bytes; fillPixels(bytes, kind, w, h);
    auto src = texture(dev, w, h, f, bytes, bpp, mipArray);
    g_eyes[0] = EyeState{}; g_eyes[0].armed = true;
    const uint64_t timed0 = g_workRound.runs();
    {
        CommandSpy spy(ctx, w, h, f, bpp);
        lumaProbeSample(ctx, src.Get(), 0, 0);
        check(spy.copies == 16 && spy.copiedBytes == uint64_t(w) * 16 * bpp,
              "observed footprint: sixteen complete rows, no full-eye copy");
        lumaProbeSample(ctx, src.Get(), 0, 0);
        check(spy.copies == 16, "stage sampled once per round");
    }
    check(g_workRound.runs() == timed0 + 1,
          "phase-0 timing: the stage copy is one luma_round run, the repeat sample none");
    auto& slot = g_eyes[0].stages[0];
    // A blocking reference read is a desk-test wait, outside production.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr(ctx->Map(slot.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    double sum = 0; float maximum = 0; int black = 0;
    for (int j = 0; j < 16; ++j) {
        const UINT py = std::min(h - 1, UINT((float(j) + .5f) / 16.0f * float(h)));
        const auto* copied = static_cast<const uint8_t*>(mapped.pData) + size_t(j) * mapped.RowPitch;
        check(memcmp(copied, bytes.data() + size_t(py) * w * bpp, size_t(w) * bpp) == 0,
              "every staging byte equals original sampled row (including sRGB bytes)");
        for (int i = 0; i < 16; ++i) {
            const UINT px = std::min(w - 1, UINT((float(i) + .5f) / 16.0f * float(w)));
            float r, g, b; decodePixel(bytes.data() + (size_t(py) * w + px) * bpp, kind, r, g, b);
            const float y = .2126f * r + .7152f * g + .0722f * b;
            sum += y; maximum = std::max(maximum, y); black += y < 1.0f / 255;
        }
    }
    ctx->Unmap(slot.staging.Get(), 0);
    {
        CommandSpy spy(ctx, w, h, f, bpp);
        lumaProbeEnd(ctx, 0);
        check(spy.maps == 1 && spy.copies == 0, "ready stage polled once, no extra transfer");
    }
    check(g_workRound.runs() == timed0 + 2, "phase-0 timing: the readback poll is one more luma_round run");
    check(slot.mean == float(sum / 256) && slot.maxLuma == maximum && slot.blackPct == float(black) * 100 / 256,
          "mean/max/near-black percentage exactly match historical full-texture grid");
    check(!g_eyes[0].armed && g_eyes[0].haveLastReport, "report starts two-second throttle");
    {
        CommandSpy spy(ctx, w, h, f, bpp);
        lumaProbeSample(ctx, src.Get(), 0, 0); lumaProbeEnd(ctx, 0);
        check(spy.copies == 0 && spy.maps == 0 && !g_eyes[0].armed, "throttle performs no diagnostic commands");
    }
    check(g_workRound.runs() == timed0 + 2, "phase-0 timing: a throttled pass times nothing");
}

void submissionBenchmark(ID3D11Device* dev, ID3D11DeviceContext* ctx, UINT w, UINT h) {
    D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h;
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    Ptr<ID3D11Texture2D> src, full, rows;
    hr(dev->CreateTexture2D(&td, nullptr, &src));
    td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr(dev->CreateTexture2D(&td, nullptr, &full));
    td.Height = 16; hr(dev->CreateTexture2D(&td, nullptr, &rows));
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    std::vector<double> ms[2];
    // Alternate paths after warmup, draining each outside its timed region.
    // Software-driver submission cost is evidence about command overhead,
    // not a hardware-GPU/Frontier frametime prediction or a timing gate.
    for (int trial = 0; trial < 36; ++trial) for (int mode = 0; mode < 2; ++mode) {
        LARGE_INTEGER begin{}, end{}; QueryPerformanceCounter(&begin);
        if (mode == 0) ctx->CopySubresourceRegion(full.Get(), 0, 0, 0, 0, src.Get(), 0, nullptr);
        else for (int j = 0; j < 16; ++j) {
            const UINT y = UINT(gridCoordinate(j, h)); const D3D11_BOX box{0, y, 0, w, y + 1, 1};
            ctx->CopySubresourceRegion(rows.Get(), 0, 0, UINT(j), 0, src.Get(), 0, &box);
        }
        QueryPerformanceCounter(&end);
        if (trial >= 4) ms[mode].push_back(double(end.QuadPart - begin.QuadPart) * 1000 / double(frequency.QuadPart));
        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr(ctx->Map(mode ? rows.Get() : full.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        ctx->Unmap(mode ? rows.Get() : full.Get(), 0);
    }
    for (auto& samples : ms) std::sort(samples.begin(), samples.end());
    printf("luma probe WARP submission %ux%u, 32 rounds, ms median/p95: full %.4f/%.4f; rows %.4f/%.4f (desk comparison, no hardware claim)\n",
           w, h, ms[0][15], ms[0][30], ms[1][15], ms[1][30]);
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--dry-run") == 0) { puts("luma_probe_test: dry-run writes nothing"); return 0; }
    if (argc > 1 && strcmp(argv[1], "--self-test") != 0) { puts("usage: luma_probe_test [--self-test|--dry-run]"); return 2; }
    Ptr<ID3D11Device> dev; Ptr<ID3D11DeviceContext> ctx;
    hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                         D3D11_SDK_VERSION, &dev, nullptr, &ctx));
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_TYPELESS,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_TYPELESS,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_TYPELESS,
        DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R16G16B16A16_UNORM};
    for (auto f : formats) { equivalence(dev.Get(), ctx.Get(), f, 37, 29); equivalence(dev.Get(), ctx.Get(), f, 3, 1); }
    equivalence(dev.Get(), ctx.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 2037, 1969);
    equivalence(dev.Get(), ctx.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 4074, 3938);
    equivalence(dev.Get(), ctx.Get(), DXGI_FORMAT_R8G8B8A8_TYPELESS, 37, 29, true);

    // An unavailable sample never masquerades as a black pixel and never
    // gets a blocking fallback, even on the thirty-first pass.
    std::vector<uint8_t> bytes; fillPixels(bytes, FormatKind::Rgba8, 37, 29);
    auto src = texture(dev.Get(), 37, 29, DXGI_FORMAT_R8G8B8A8_UNORM, bytes, 4);
    g_eyes[0] = EyeState{}; g_eyes[0].armed = true;
    lumaProbeSample(ctx.Get(), src.Get(), 0, 0);
    auto oldStaging = g_eyes[0].stages[0].staging;
    {
        CommandSpy spy(ctx.Get(), 37, 29, DXGI_FORMAT_R8G8B8A8_UNORM, 4); spy.forcePending = true;
        for (int i = 0; i < 30; ++i) lumaProbeEnd(ctx.Get(), 0);
        check(g_eyes[0].armed && g_eyes[0].stages[0].status == SlotStatus::Pending, "thirty passes retain pending sample");
        lumaProbeEnd(ctx.Get(), 0);
        check(!g_eyes[0].armed && !g_eyes[0].stages[0].staging && spy.maps == 31,
              "deadline drops staging after nonblocking attempt and completes round");
    }
    check(std::any_of(testLog.begin(), testLog.end(), [](const std::string& s) { return s.find("game=unavailable(timeout)") != std::string::npos; }),
          "timeout explicitly reported, not zero luminance");
    g_eyes[0].haveLastReport = false; lumaProbeEnd(ctx.Get(), 0);
    lumaProbeSample(ctx.Get(), src.Get(), 0, 0);
    check(g_eyes[0].stages[0].staging.Get() != oldStaging.Get(), "retry gets fresh target instead of overwriting timed-out pending copy");

    g_eyes[0] = EyeState{}; g_eyes[0].armed = true;
    lumaProbeSample(ctx.Get(), nullptr, 0, 0);
    check(g_eyes[0].stages[0].status == SlotStatus::Absent, "null stage remains absent");
    D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = 4; td.ArraySize = td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT; td.SampleDesc.Count = 1;
    Ptr<ID3D11Texture2D> unsupported; hr(dev->CreateTexture2D(&td, nullptr, &unsupported));
    {
        CommandSpy spy(ctx.Get(), 4, 4, td.Format, 4);
        lumaProbeSample(ctx.Get(), unsupported.Get(), 0, 1);
        check(g_eyes[0].stages[1].status == SlotStatus::Unsupported && spy.copies == 0,
              "unsupported format unchanged, no staging/copy");
    }
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 4; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    Ptr<ID3D11Texture2D> msaa; hr(dev->CreateTexture2D(&td, nullptr, &msaa));
    {
        CommandSpy spy(ctx.Get(), 4, 4, td.Format, 4);
        lumaProbeSample(ctx.Get(), msaa.Get(), 0, 2);
        check(g_eyes[0].stages[2].status == SlotStatus::Unsupported && spy.copies == 0,
              "MSAA remains unsupported, no resolve or staging/copy");
    }
    submissionBenchmark(dev.Get(), ctx.Get(), 2037, 1969);
    submissionBenchmark(dev.Get(), ctx.Get(), 4074, 3938);
    const uint64_t oldBytes = 2ull * (2037ull * 1969 + 2ull * 4074 * 3938) * 4;
    const uint64_t newBytes = 2ull * (2037ull + 2ull * 4074) * 16 * 4;
    printf("luma probe Frontier footprint: %llu -> %llu logical bytes/round; 6 -> 96 row-copy commands\n", oldBytes, newBytes);
    printf("luma_probe_test: %u checks passed\n", checks);
    return 0;
}

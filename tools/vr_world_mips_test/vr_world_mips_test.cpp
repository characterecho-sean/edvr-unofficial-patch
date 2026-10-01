// The VR world route's mipped screen (src\d3d11\vr_world_mips.cpp) on a real D3D11 WARP device: the production module is
// linked as a source (build.bat :rig_vr_world_mips_test), and this file supplies what the DLL would: the log, the GPU census,
// the two internal-scope flags. Nothing is faked on the GPU side: the copy, the GenerateMips, the views and the samplers are
// WARP's own, read back through staging textures.
//
// WHAT IS PINNED, by rule (a check's label starts with its rule's id, and tools\vr_world_mips_test\mutants.py names the rule
// each of its edits of the production source must trip):
//   R1  mip 0 is a byte copy of the screen, for a TYPELESS and for a UNORM source
//   R2  the mips come out of the sRGB view: linear light, a per-texel 0/255 checkerboard lands on 188 (not 128), and a noise
//       image matches an exact sRGB decode / mean / encode reference; a control run through the UNORM view lands on 128, so
//       the check can tell the two apart
//   R3  the returned view is the UNORM one over every mip: it samples back the stored bytes at LOD 0 and the generated mips
//   R4  the full chain for any size: 13 levels for 5040x2835, 1 + floor(log2(longer side)) for odd and tiny sizes, and the
//       last level of a solid colour is that colour
//   R5  once a frame per screen (the second call does no work, a new frame does, a new screen redoes it); a new size or
//       source format recreates; the same size from another screen only re-copies
//   R6  refusals: an sRGB-typed, multisampled, array, already-mipped or non-R8G8B8A8 texture, and a null argument, return
//       null with the named reason counted and no GPU work; logged once each; only the first 8 distinct are logged
//   R7  the sampler: the game's addressing and border colour, the route's trilinear filter and LOD range, cached per
//       distinct game sampler, the oldest evicted past 8 without leaking a reference
//   R8  the hooks: every device and context call runs inside a VrWorldInternalScope, and the GPU census scope brackets the
//       copy and GenerateMips and nothing else
//   R9  lifetime: a size change, a device change and vrWorldMipsReset release every object (proved by reference counts)
//   R10 no heap allocation on the warm paths (a counting operator new)
//   R11 the log: one creation line per creation, in its exact words, capped at 8
//
// The D3D calls are watched the way tools\hologram_depth_test does it: for the length of one production call (Armed) the WARP
// context's and device's vtable pointers are swapped for private copies, taken at that moment, with the few methods of
// interest replaced by thunks that record the call and forward it. Two things WARP does make the copy's length and its
// freshness matter: its context rewrites its own table (a slot patched in place stops firing after the first Flush), and its
// device object implements more methods than ID3D11Device declares (a 43-slot copy crashes it; the copies here are 128). The
// rig's own readbacks happen outside an Armed scope, so they are not counted.
//
// Usage: --self-test [--only R1,R5,...] [--verbose] | --dry-run (no device, no GPU work, no files). --only runs the named
// cases (tools\vr_world_mips_test\mutants.py runs each mutant on its own rule's case); --verbose echoes the module's log lines
// (a failure prints the last few either way). --self-test relaunches itself as a child with a watchdog, like
// tools\gpu_census_test: a hung WARP must fail the rig, not the build's timeout. Nothing is written to disk.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../src/common/log.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/d3d11/gpu_census.h"
#include "../../src/d3d11/vr_world_mips.h"
#include "../../src/d3d11/vr_world_route.h"

using Microsoft::WRL::ComPtr;

// ---- R10's instrument: every C++ allocation this process makes while g_countHeap is set. The D3D runtime has its own
// allocator in its own module, so this counts the production module's, the rig's stubs' and the spies'.
namespace {
std::atomic<bool> g_countHeap{false};
std::atomic<unsigned> g_heapCalls{0};
volatile char g_sink = 0;   // keeps a deliberate allocation from being optimised away
}  // namespace
void* operator new(size_t size) {
    if (g_countHeap.load(std::memory_order_relaxed)) g_heapCalls.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

// ---- what the DLL provides around the module: the log, the census, the two internal-scope flags ----------------------
namespace edvr {
thread_local bool g_flatComputeInternal = false;
thread_local bool g_vrWorldInternal = false;

std::vector<std::string> g_lines;   // every log line the production module wrote, in order
bool g_verbose = false;             // --verbose: echo each of them as it is written (a failure always prints the last few)
Log& Log::get() { static Log instance; return instance; }
Log::~Log() = default;
void Log::note(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_lines.emplace_back(buf);
    if (g_verbose) std::printf("    log: %s\n", buf);
}

// What the census scope saw: how often it opened and closed, on which section and context, what the two internal flags read
// then, and how many copies and generates the module had issued at each edge (so the scope can be shown to bracket them).
struct CensusProbe {
    unsigned begins = 0, ends = 0;
    int depth = 0;
    GpuCensusSection beginSection = GpuCensusSection::Count, endSection = GpuCensusSection::Count;
    ID3D11DeviceContext* beginCtx = nullptr;
    ID3D11DeviceContext* endCtx = nullptr;
    bool internalAtBegin = false, internalAtEnd = false;
    uint64_t copiesAtBegin = 0, generatesAtBegin = 0, copiesAtEnd = 0, generatesAtEnd = 0;
};
CensusProbe g_census;

bool gpuCensusBegin(ID3D11DeviceContext* ctx, GpuCensusSection section) noexcept {
    CensusProbe& c = g_census;
    ++c.begins;
    ++c.depth;
    c.beginSection = section;
    c.beginCtx = ctx;
    c.internalAtBegin = g_vrWorldInternal && g_flatComputeInternal;
    const VrWorldMipsStats s = vrWorldMipsStats();
    c.copiesAtBegin = s.copies;
    c.generatesAtBegin = s.generates;
    return true;
}
void gpuCensusEnd(ID3D11DeviceContext* ctx, GpuCensusSection section) noexcept {
    CensusProbe& c = g_census;
    ++c.ends;
    --c.depth;
    c.endSection = section;
    c.endCtx = ctx;
    c.internalAtEnd = g_vrWorldInternal && g_flatComputeInternal;
    const VrWorldMipsStats s = vrWorldMipsStats();
    c.copiesAtEnd = s.copies;
    c.generatesAtEnd = s.generates;
}
}  // namespace edvr

namespace {
using namespace edvr;

// ---- the harness ---------------------------------------------------------------------------------------------------
unsigned g_checks = 0;
void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) throw std::runtime_error(label);
}
void checkf(bool ok, const char* fmt, ...) {
    ++g_checks;
    if (ok) return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    throw std::runtime_error(buf);
}
void hr(HRESULT h) { checkf(SUCCEEDED(h), "D3D operation failed, hr=0x%08lX", static_cast<unsigned long>(h)); }
constexpr int idx(VrWorldMipsRefusal why) { return static_cast<int>(why); }
UINT maxu(UINT a, UINT b) { return a > b ? a : b; }

// ---- the spy: the D3D calls the production module makes, recorded only while armed ----------------------------------------
constexpr unsigned kRecords = 8;
struct Flags {
    bool world, compute;
    int census;   // the depth of the GPU census scope at the call: 1 means inside the module's own
};
Flags flagsNow() { return Flags{g_vrWorldInternal, g_flatComputeInternal, g_census.depth}; }
struct CopyCall {
    ID3D11Resource* dst;
    UINT dstSub, dx, dy, dz;
    ID3D11Resource* src;
    UINT srcSub;
    bool boxNull;
    Flags f;
    unsigned order;
};
struct GenerateCall {
    ID3D11ShaderResourceView* srv;
    Flags f;
    unsigned order;
};
struct TextureCall {
    D3D11_TEXTURE2D_DESC desc;
    bool hadInitialData;
    Flags f;
};
struct ViewCall {
    ID3D11Resource* resource;
    D3D11_SHADER_RESOURCE_VIEW_DESC desc;
    bool hadDesc;
    Flags f;
};
struct SamplerCall {
    D3D11_SAMPLER_DESC desc;
    Flags f;
};
struct SpyLog {
    bool armed = false;
    unsigned order = 0;
    CopyCall copies[kRecords];
    GenerateCall generates[kRecords];
    TextureCall textures[kRecords];
    ViewCall views[kRecords];
    SamplerCall samplers[kRecords];
    unsigned nCopies = 0, nGenerates = 0, nTextures = 0, nViews = 0, nSamplers = 0;
    // Failure injection for the NEXT armed call (Armed's destructor clears it): the device call answers with an error instead
    // of making anything, for the texture, for the Nth CreateShaderResourceView (1 = the sRGB view, 2 = the UNORM view) or
    // for the sampler. With holdCreated, every texture and view the device really makes is also AddRef'd into held[], so a
    // case can ask afterwards whether the module let its own references go.
    HRESULT failTexture = S_OK, failSampler = S_OK;
    unsigned failViewAt = 0;
    HRESULT failViewWith = S_OK;
    bool holdCreated = false;
    IUnknown* held[4] = {};
    unsigned nHeld = 0;
    void clear() { nCopies = nGenerates = nTextures = nViews = nSamplers = 0; order = 0; }
    void clearInjection() {
        failTexture = failSampler = failViewWith = S_OK;
        failViewAt = 0;
        holdCreated = false;
    }
    unsigned calls() const { return nCopies + nGenerates + nTextures + nViews + nSamplers; }
    void hold(IUnknown* created) {
        if (holdCreated && created && nHeld < 4) {
            created->AddRef();
            held[nHeld++] = created;
        }
    }
};
SpyLog g_spy;

// One object's vtable, swapped for a private copy with some slots replaced, and swapped back. The copy is taken when the swap
// begins, because WARP's context rewrites its own table (a slot patched in place stops firing after the first Flush), and it is
// kVtableSlots long, because the device's object implements more methods than ID3D11Device declares (a 43-slot copy crashes).
// This is tools\hologram_depth_test's CensusCommandSpy technique.
constexpr unsigned kVtableSlots = 128;
struct VtableSwap {
    void* object = nullptr;
    void** live = nullptr;                 // the table the object pointed at when the swap began
    void* original[kVtableSlots] = {};     // its entries then: what a thunk forwards to
    void* patched[kVtableSlots] = {};      // the copy the object points at while swapped
    void begin(void* obj) {
        object = obj;
        live = *reinterpret_cast<void***>(obj);
        std::memcpy(original, live, sizeof original);
        std::memcpy(patched, live, sizeof patched);
    }
    void patch(unsigned index, void* thunk) { patched[index] = thunk; }
    void install() { write(patched); }
    void end() {
        if (!object) return;
        write(live);
        object = nullptr;
    }
    void write(void** table) {
        DWORD old = 0, ignored = 0;
        if (!VirtualProtect(object, sizeof(void*), PAGE_READWRITE, &old)) throw std::runtime_error("spy: the object's vptr is not writable");
        *reinterpret_cast<void***>(object) = table;
        VirtualProtect(object, sizeof(void*), old, &ignored);
    }
};
// ID3D11DeviceContext: CopySubresourceRegion is slot 46, GenerateMips 54. ID3D11Device: CreateTexture2D 5,
// CreateShaderResourceView 7, CreateSamplerState 23. The rig proves the indices itself: a spy that never fires fails R8.
struct Swaps {
    VtableSwap ctx, dev;
};
Swaps g_swap;

using CopyFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT,
                                        const D3D11_BOX*);
using GenerateFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11ShaderResourceView*);
using TextureFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*,
                                              ID3D11Texture2D**);
using ViewFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*,
                                           ID3D11ShaderResourceView**);
using SamplerFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_SAMPLER_DESC*, ID3D11SamplerState**);

void STDMETHODCALLTYPE copyThunk(ID3D11DeviceContext* self, ID3D11Resource* dst, UINT dstSub, UINT dx, UINT dy, UINT dz,
                                 ID3D11Resource* src, UINT srcSub, const D3D11_BOX* box) {
    if (g_spy.armed && g_spy.nCopies < kRecords)
        g_spy.copies[g_spy.nCopies++] = CopyCall{dst, dstSub, dx, dy, dz, src, srcSub, box == nullptr, flagsNow(), g_spy.order++};
    reinterpret_cast<CopyFn>(g_swap.ctx.original[46])(self, dst, dstSub, dx, dy, dz, src, srcSub, box);
}
void STDMETHODCALLTYPE generateThunk(ID3D11DeviceContext* self, ID3D11ShaderResourceView* srv) {
    if (g_spy.armed && g_spy.nGenerates < kRecords)
        g_spy.generates[g_spy.nGenerates++] = GenerateCall{srv, flagsNow(), g_spy.order++};
    reinterpret_cast<GenerateFn>(g_swap.ctx.original[54])(self, srv);
}
HRESULT STDMETHODCALLTYPE textureThunk(ID3D11Device* self, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init,
                                       ID3D11Texture2D** out) {
    if (g_spy.armed) {
        if (g_spy.nTextures < kRecords && desc) g_spy.textures[g_spy.nTextures++] = TextureCall{*desc, init != nullptr, flagsNow()};
        if (FAILED(g_spy.failTexture)) {
            if (out) *out = nullptr;
            return g_spy.failTexture;
        }
    }
    const HRESULT made = reinterpret_cast<TextureFn>(g_swap.dev.original[5])(self, desc, init, out);
    if (g_spy.armed && SUCCEEDED(made) && out) g_spy.hold(*out);
    return made;
}
HRESULT STDMETHODCALLTYPE viewThunk(ID3D11Device* self, ID3D11Resource* resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,
                                    ID3D11ShaderResourceView** out) {
    if (g_spy.armed && g_spy.nViews < kRecords) {
        ViewCall c{};
        c.resource = resource;
        c.hadDesc = desc != nullptr;
        if (desc) c.desc = *desc;
        c.f = flagsNow();
        g_spy.views[g_spy.nViews++] = c;
        if (g_spy.failViewAt && g_spy.nViews == g_spy.failViewAt) {
            if (out) *out = nullptr;
            return g_spy.failViewWith;
        }
    }
    const HRESULT made = reinterpret_cast<ViewFn>(g_swap.dev.original[7])(self, resource, desc, out);
    if (g_spy.armed && SUCCEEDED(made) && out) g_spy.hold(*out);
    return made;
}
HRESULT STDMETHODCALLTYPE samplerThunk(ID3D11Device* self, const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** out) {
    if (g_spy.armed) {
        if (g_spy.nSamplers < kRecords && desc) g_spy.samplers[g_spy.nSamplers++] = SamplerCall{*desc, flagsNow()};
        if (FAILED(g_spy.failSampler)) {
            if (out) *out = nullptr;
            return g_spy.failSampler;
        }
    }
    return reinterpret_cast<SamplerFn>(g_swap.dev.original[23])(self, desc, out);
}

// ---- the device, and what the cases make on it ----------------------------------------------------------------------------
struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11InfoQueue> info;   // only with the D3D11 debug layer, which this machine may not have installed
    bool debugLayer = false;
};
Gpu makeGpu() {
    PFN_D3D11_CREATE_DEVICE create = systemD3D11CreateDevice();
    check(create != nullptr, "System32 D3D11CreateDevice is available");
    Gpu g;
    D3D_FEATURE_LEVEL level{};
    HRESULT made = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION,
                          g.dev.ReleaseAndGetAddressOf(), &level, g.ctx.ReleaseAndGetAddressOf());
    g.debugLayer = SUCCEEDED(made);
    if (!g.debugLayer)
        made = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, g.dev.ReleaseAndGetAddressOf(),
                      &level, g.ctx.ReleaseAndGetAddressOf());
    hr(made);
    if (g.debugLayer) g.dev.As(&g.info);
    return g;
}

ComPtr<ID3D11Texture2D> makeTexture(Gpu& g, UINT w, UINT h, DXGI_FORMAT format, const std::vector<uint8_t>* bytes,
                                    UINT bind = D3D11_BIND_SHADER_RESOURCE, UINT mips = 1, UINT slices = 1, UINT samples = 1) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = mips;
    d.ArraySize = slices;
    d.Format = format;
    d.SampleDesc.Count = samples;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init{};
    if (bytes) {
        init.pSysMem = bytes->data();
        init.SysMemPitch = w * 4;
    }
    ComPtr<ID3D11Texture2D> t;
    hr(g.dev->CreateTexture2D(&d, bytes ? &init : nullptr, t.GetAddressOf()));
    return t;
}
ComPtr<ID3D11Texture2D> screenOf(Gpu& g, UINT w, UINT h, const std::vector<uint8_t>& bytes,
                                 DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_TYPELESS) {
    return makeTexture(g, w, h, format, &bytes);
}
ComPtr<ID3D11Texture2D> textureOf(ID3D11ShaderResourceView* view) {
    ComPtr<ID3D11Resource> res;
    view->GetResource(res.GetAddressOf());
    ComPtr<ID3D11Texture2D> tex;
    hr(res.As(&tex));
    return tex;
}
ComPtr<ID3D11Device> deviceOf(ID3D11DeviceChild* child) {
    ComPtr<ID3D11Device> dev;
    child->GetDevice(dev.GetAddressOf());
    return dev;
}

// One subresource of an R8G8B8A8 texture (any format of the family), tightly packed.
std::vector<uint8_t> readSubresource(Gpu& g, ID3D11Texture2D* tex, UINT sub) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    const UINT mip = sub % d.MipLevels;
    const UINT w = maxu(1, d.Width >> mip), h = maxu(1, d.Height >> mip);
    D3D11_TEXTURE2D_DESC s{};
    s.Width = w;
    s.Height = h;
    s.MipLevels = 1;
    s.ArraySize = 1;
    s.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    s.SampleDesc.Count = 1;
    s.Usage = D3D11_USAGE_STAGING;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage;
    hr(g.dev->CreateTexture2D(&s, nullptr, stage.GetAddressOf()));
    g.ctx->CopySubresourceRegion(stage.Get(), 0, 0, 0, 0, tex, sub, nullptr);
    D3D11_MAPPED_SUBRESOURCE m{};
    hr(g.ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m));
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
    for (UINT y = 0; y < h; ++y) std::memcpy(&out[static_cast<size_t>(y) * w * 4], static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, static_cast<size_t>(w) * 4);
    g.ctx->Unmap(stage.Get(), 0);
    return out;
}

// ---- the images the cases use ---------------------------------------------------------------------------------------------
uint32_t lcg(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return s;
}
std::vector<uint8_t> noiseBytes(UINT w, UINT h, uint32_t seed) {
    std::vector<uint8_t> v(static_cast<size_t>(w) * h * 4);
    for (uint8_t& b : v) b = static_cast<uint8_t>(lcg(seed) >> 24);
    return v;
}
std::vector<uint8_t> solidBytes(UINT w, UINT h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    std::vector<uint8_t> v(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < v.size(); i += 4) {
        v[i] = r;
        v[i + 1] = g;
        v[i + 2] = b;
        v[i + 3] = a;
    }
    return v;
}
// A 0/255 checkerboard of single texels (alpha 255): every 2x2 footprint is half black and half white.
std::vector<uint8_t> checkerBytes(UINT w, UINT h) {
    std::vector<uint8_t> v(static_cast<size_t>(w) * h * 4);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x) {
            const uint8_t c = ((x + y) & 1) ? 255 : 0;
            uint8_t* p = &v[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = p[1] = p[2] = c;
            p[3] = 255;
        }
    return v;
}
// 1 + floor(log2(longer side)): the chain a full set of mips has.
UINT chainOf(UINT w, UINT h) {
    UINT n = 1;
    for (UINT side = maxu(w, h); side > 1; side >>= 1) ++n;
    return n;
}

// The reference the mips are held to: decode the four gamma-space texels to linear light, take the mean, encode again.
double decodeSrgb(int byte) {
    const double c = byte / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
int encodeSrgb(double linear) {
    const double c = linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    return static_cast<int>(std::lround(c * 255.0));
}
// The linear-light mip-1 texel (x, y) of an image `bytes` of width w: RGB through the sRGB reference, alpha a plain mean.
void referenceMip1(const std::vector<uint8_t>& bytes, UINT w, UINT x, UINT y, int out[4]) {
    for (int ch = 0; ch < 4; ++ch) {
        double sum = 0;
        int isum = 0;
        for (UINT dy = 0; dy < 2; ++dy)
            for (UINT dx = 0; dx < 2; ++dx) {
                const int v = bytes[((static_cast<size_t>(2 * y + dy)) * w + 2 * x + dx) * 4 + ch];
                sum += decodeSrgb(v);
                isum += v;
            }
        out[ch] = ch < 3 ? encodeSrgb(sum / 4.0) : static_cast<int>(std::lround(isum / 4.0));
    }
}

// The same generation the module does, on a texture of the rig's own and through a view format of the rig's choosing, so a
// control can be made with the sRGB view (must equal the module's mips) and the UNORM view (must not).
std::vector<uint8_t> controlMip(Gpu& g, const std::vector<uint8_t>& bytes, UINT w, UINT h, DXGI_FORMAT viewFormat, UINT mip) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 0;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ComPtr<ID3D11Texture2D> tex;
    hr(g.dev->CreateTexture2D(&d, nullptr, tex.GetAddressOf()));
    tex->GetDesc(&d);
    g.ctx->UpdateSubresource(tex.Get(), 0, nullptr, bytes.data(), w * 4, 0);
    D3D11_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = viewFormat;
    v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    v.Texture2D.MipLevels = d.MipLevels;
    ComPtr<ID3D11ShaderResourceView> srv;
    hr(g.dev->CreateShaderResourceView(tex.Get(), &v, srv.GetAddressOf()));
    g.ctx->GenerateMips(srv.Get());
    return readSubresource(g, tex.Get(), mip);
}

// ---- the sampling pass: a full-screen triangle into an R8G8B8A8_UNORM target, Load at a mip or SampleLevel at a LOD ---------
const char kHlsl[] =
    "Texture2D<float4> src : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "cbuffer P : register(b0) { float4 p; };\n"   // p.x = Load mip, p.y = SampleLevel lod, p.zw = uv
    "float4 vsMain(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n"
    "float4 loadMain(float4 pos : SV_Position) : SV_Target { return src.Load(int3(int2(pos.xy), (int)p.x)); }\n"
    "float4 sampleMain(float4 pos : SV_Position) : SV_Target { return src.SampleLevel(smp, p.zw, p.y); }\n";

struct Sampling {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> loadPs, samplePs;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11RasterizerState> raster;

    static ComPtr<ID3DBlob> compile(const char* entry, const char* profile) {
        ComPtr<ID3DBlob> blob, errors;
        const HRESULT h = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "vr_world_mips_test.hlsl", nullptr, nullptr, entry, profile,
                                     D3DCOMPILE_ENABLE_STRICTNESS, 0, blob.GetAddressOf(), errors.GetAddressOf());
        if (FAILED(h)) {
            if (errors) std::puts(static_cast<const char*>(errors->GetBufferPointer()));
            throw std::runtime_error("the rig's sampling shader does not compile");
        }
        return blob;
    }
    void init(Gpu& g) {
        ComPtr<ID3DBlob> v = compile("vsMain", "vs_5_0"), l = compile("loadMain", "ps_5_0"), s = compile("sampleMain", "ps_5_0");
        hr(g.dev->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, vs.GetAddressOf()));
        hr(g.dev->CreatePixelShader(l->GetBufferPointer(), l->GetBufferSize(), nullptr, loadPs.GetAddressOf()));
        hr(g.dev->CreatePixelShader(s->GetBufferPointer(), s->GetBufferSize(), nullptr, samplePs.GetAddressOf()));
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr(g.dev->CreateBuffer(&bd, nullptr, cb.GetAddressOf()));
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        hr(g.dev->CreateRasterizerState(&rd, raster.GetAddressOf()));
    }
    std::vector<uint8_t> draw(Gpu& g, ID3D11PixelShader* ps, UINT w, UINT h, ID3D11ShaderResourceView* srv, ID3D11SamplerState* smp,
                              float p0, float p1, float p2, float p3) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target;
        hr(g.dev->CreateTexture2D(&td, nullptr, target.GetAddressOf()));
        ComPtr<ID3D11RenderTargetView> rtv;
        hr(g.dev->CreateRenderTargetView(target.Get(), nullptr, rtv.GetAddressOf()));
        const float params[4] = {p0, p1, p2, p3};
        g.ctx->UpdateSubresource(cb.Get(), 0, nullptr, params, 0, 0);
        const float clear[4] = {0, 0, 0, 0};
        g.ctx->ClearRenderTargetView(rtv.Get(), clear);
        ID3D11RenderTargetView* rtvs[1] = {rtv.Get()};
        g.ctx->OMSetRenderTargets(1, rtvs, nullptr);
        const D3D11_VIEWPORT vp = {0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
        g.ctx->RSSetViewports(1, &vp);
        g.ctx->RSSetState(raster.Get());
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.ctx->VSSetShader(vs.Get(), nullptr, 0);
        g.ctx->PSSetShader(ps, nullptr, 0);
        g.ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
        ID3D11ShaderResourceView* srvs[1] = {srv};
        g.ctx->PSSetShaderResources(0, 1, srvs);
        ID3D11SamplerState* smps[1] = {smp};
        g.ctx->PSSetSamplers(0, 1, smps);
        g.ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none[1] = {nullptr};
        g.ctx->PSSetShaderResources(0, 1, none);
        ID3D11SamplerState* noSampler[1] = {nullptr};
        g.ctx->PSSetSamplers(0, 1, noSampler);
        g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        return readSubresource(g, target.Get(), 0);
    }
    // The texels of mip `mip` (w x h of that level) as the UNORM view hands them to a shader.
    std::vector<uint8_t> loadMip(Gpu& g, ID3D11ShaderResourceView* srv, UINT mip, UINT w, UINT h) {
        return draw(g, loadPs.Get(), w, h, srv, nullptr, static_cast<float>(mip), 0, 0, 0);
    }
    // One sample at (u, v), LOD `lod`, through `smp`.
    std::vector<uint8_t> sampleAt(Gpu& g, ID3D11ShaderResourceView* srv, ID3D11SamplerState* smp, float lod, float u, float v) {
        return draw(g, samplePs.Get(), 1, 1, srv, smp, 0, lod, u, v);
    }
};

struct Rig {
    Gpu gpu;
    Sampling sampling;
};

// A reference a case holds on an object of the module's, to ask afterwards how many others remain: the probe's own reference
// is dropped last, and 0 back means nothing else held the object, i.e. the module let go of everything it held.
struct RefProbe {
    IUnknown* obj = nullptr;
    void hold(IUnknown* o) {
        obj = o;
        obj->AddRef();
    }
    ULONG drop() {
        const ULONG left = obj->Release();
        obj = nullptr;
        return left;
    }
};

// ---- a clean slate for each case, and the production call as the DLL would make it ------------------------------------------
void freshState(Gpu& g) {
    g.ctx->ClearState();
    g.ctx->Flush();
    vrWorldMipsReset();
    vrWorldMipsStatsClear();
    g_lines.clear();
    g_spy.clear();
    g_census = CensusProbe{};
}

// The spy, for the length of one production call: the context's and the device's tables swapped for patched copies, the records
// and the census probe cleared. Nothing the rig itself does runs inside one, so nothing it does is counted.
struct Armed {
    explicit Armed(Gpu& g) {
        g_spy.clear();
        g_spy.nHeld = 0;
        g_census = CensusProbe{};
        g_swap.ctx.begin(g.ctx.Get());
        g_swap.ctx.patch(46, reinterpret_cast<void*>(&copyThunk));
        g_swap.ctx.patch(54, reinterpret_cast<void*>(&generateThunk));
        g_swap.dev.begin(g.dev.Get());
        g_swap.dev.patch(5, reinterpret_cast<void*>(&textureThunk));
        g_swap.dev.patch(7, reinterpret_cast<void*>(&viewThunk));
        g_swap.dev.patch(23, reinterpret_cast<void*>(&samplerThunk));
        g_swap.ctx.install();
        g_swap.dev.install();
        g_spy.armed = true;
    }
    ~Armed() {
        g_spy.armed = false;
        g_spy.clearInjection();
        g_swap.ctx.end();
        g_swap.dev.end();
    }
    Armed(const Armed&) = delete;
    Armed& operator=(const Armed&) = delete;
};
ID3D11ShaderResourceView* screenCall(Gpu& g, ID3D11Texture2D* screen, uint64_t frame) {
    Armed armed(g);
    return vrWorldMipsScreen(g.ctx.Get(), screen, frame);
}
ID3D11SamplerState* samplerCall(Gpu& g, const D3D11_SAMPLER_DESC& game) {
    Armed armed(g);
    return vrWorldMipsSampler(g.dev.Get(), game);
}

bool hasLine(const std::string& needle) {
    for (const std::string& l : g_lines)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

// ================================================================================================================
// R1: mip 0 is a byte copy
void caseR1(Rig& r) {
    Gpu& g = r.gpu;
    freshState(g);
    const UINT w = 64, h = 48;
    const std::vector<uint8_t> bytes = noiseBytes(w, h, 1);
    ComPtr<ID3D11Texture2D> screen = screenOf(g, w, h, bytes);
    ID3D11ShaderResourceView* view = screenCall(g, screen.Get(), 1);
    check(view != nullptr, "R1a: a TYPELESS screen is accepted and answered with a view");
    ComPtr<ID3D11Texture2D> mips = textureOf(view);
    check(mips.Get() != screen.Get(), "R1b: the view is over the route's own texture, not the game's screen");
    check(readSubresource(g, mips.Get(), 0) == bytes, "R1c: mip 0 equals the screen byte for byte (TYPELESS source, alpha included)");

    const std::vector<uint8_t> other = noiseBytes(w, h, 2);
    ComPtr<ID3D11Texture2D> unorm = screenOf(g, w, h, other, DXGI_FORMAT_R8G8B8A8_UNORM);
    ID3D11ShaderResourceView* view2 = screenCall(g, unorm.Get(), 2);
    check(view2 != nullptr, "R1d: a UNORM screen is accepted and answered with a view");
    ComPtr<ID3D11Texture2D> mips2 = textureOf(view2);
    check(readSubresource(g, mips2.Get(), 0) == other, "R1e: mip 0 equals the screen byte for byte (UNORM source)");
    std::printf("  R1: mip 0 byte-exact for a TYPELESS and a UNORM %ux%u noise screen\n", w, h);
}

// R2: the mips are made in linear light
void caseR2(Rig& r) {
    Gpu& g = r.gpu;
    freshState(g);
    {
        const UINT w = 64, h = 48;
        const std::vector<uint8_t> bytes = checkerBytes(w, h);
        ComPtr<ID3D11Texture2D> screen = screenOf(g, w, h, bytes);
        ID3D11ShaderResourceView* view = screenCall(g, screen.Get(), 1);
        check(view != nullptr, "R2a: the checkerboard screen is accepted");
        ComPtr<ID3D11Texture2D> tex = textureOf(view);
        const std::vector<uint8_t> mip1 = readSubresource(g, tex.Get(), 1);
        int lo = 255, hi = 0;
        for (size_t i = 0; i < mip1.size(); i += 4)
            for (int c = 0; c < 3; ++c) {
                lo = mip1[i + c] < lo ? mip1[i + c] : lo;
                hi = mip1[i + c] > hi ? mip1[i + c] : hi;
            }
        checkf(lo >= 186 && hi <= 190, "R2b: mip 1 of a per-texel 0/255 checkerboard is 188 +-2 (linear light), read %d..%d", lo, hi);
        checkf(lo > 128 + 40, "R2c: mip 1 is not near 128 (the gamma-space average), read %d", lo);
        bool alphaOk = true;
        for (size_t i = 3; i < mip1.size(); i += 4) alphaOk = alphaOk && mip1[i] == 255;
        check(alphaOk, "R2d: alpha stays 255 through the mips");
        // the chain continues in linear light: every level below is the same grey
        bool deepOk = true;
        int worst = 0;
        for (UINT m = 2; m < chainOf(w, h); ++m) {
            const std::vector<uint8_t> px = readSubresource(g, tex.Get(), m);
            for (size_t i = 0; i < px.size(); i += 4)
                for (int c = 0; c < 3; ++c) {
                    const int d = std::abs(static_cast<int>(px[i + c]) - 188);
                    worst = d > worst ? d : worst;
                    deepOk = deepOk && d <= 3;
                }
        }
        checkf(deepOk, "R2e: every level below mip 1 stays 188 +-3, worst off by %d", worst);

        // the control: the same texture generated through the UNORM view lands on the gamma-space mean, which the checks above
        // reject, and through the sRGB view lands where the module's did
        const std::vector<uint8_t> viaUnorm = controlMip(g, bytes, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, 1);
        const std::vector<uint8_t> viaSrgb = controlMip(g, bytes, w, h, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1);
        checkf(viaUnorm[0] >= 127 && viaUnorm[0] <= 128, "R2f: the control through the UNORM view lands on 127/128, read %d", viaUnorm[0]);
        checkf(std::abs(static_cast<int>(viaUnorm[0]) - static_cast<int>(mip1[0])) > 40, "R2g: the control and the module's mips differ by more than 40, so the check can tell");
        check(viaSrgb == mip1, "R2h: the control through the sRGB view equals the module's mip 1 exactly");
        std::printf("  R2: checkerboard mip 1 = %d (module), %d (UNORM-view control), %d (sRGB-view control)\n", mip1[0], viaUnorm[0], viaSrgb[0]);
    }
    {
        // noise against an exact reference: decode to linear, mean of the four, encode; alpha a plain mean
        const UINT w = 64, h = 48;
        const std::vector<uint8_t> bytes = noiseBytes(w, h, 7);
        ComPtr<ID3D11Texture2D> screen = screenOf(g, w, h, bytes);
        ID3D11ShaderResourceView* view = screenCall(g, screen.Get(), 2);
        check(view != nullptr, "R2i: the noise screen is accepted");
        const std::vector<uint8_t> mip1 = readSubresource(g, textureOf(view).Get(), 1);
        const std::vector<uint8_t> viaUnorm = controlMip(g, bytes, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, 1);
        int worstRgb = 0, worstAlpha = 0, worstControl = 0;
        for (UINT y = 0; y < h / 2; ++y)
            for (UINT x = 0; x < w / 2; ++x) {
                int ref[4];
                referenceMip1(bytes, w, x, y, ref);
                const uint8_t* got = &mip1[(static_cast<size_t>(y) * (w / 2) + x) * 4];
                const uint8_t* ctl = &viaUnorm[(static_cast<size_t>(y) * (w / 2) + x) * 4];
                for (int c = 0; c < 3; ++c) {
                    worstRgb = std::abs(got[c] - ref[c]) > worstRgb ? std::abs(got[c] - ref[c]) : worstRgb;
                    worstControl = std::abs(ctl[c] - ref[c]) > worstControl ? std::abs(ctl[c] - ref[c]) : worstControl;
                }
                worstAlpha = std::abs(got[3] - ref[3]) > worstAlpha ? std::abs(got[3] - ref[3]) : worstAlpha;
            }
        checkf(worstRgb <= 2, "R2j: noise mip 1 matches the linear-light reference within 2, worst %d", worstRgb);
        checkf(worstAlpha <= 1, "R2k: noise mip 1's alpha is the plain mean within 1, worst %d", worstAlpha);
        checkf(worstControl > 10, "R2l: the UNORM-view control misses the linear-light reference by more than 10, worst %d", worstControl);
        std::printf("  R2: noise mip 1 vs the exact reference: worst RGB %d, alpha %d (UNORM-view control: %d)\n", worstRgb, worstAlpha, worstControl);
    }
}

// R3: the returned view is the UNORM one, over every mip
void caseR3(Rig& r) {
    Gpu& g = r.gpu;
    freshState(g);
    const UINT w = 40, h = 30;
    const std::vector<uint8_t> bytes = noiseBytes(w, h, 3);
    ComPtr<ID3D11Texture2D> screen = screenOf(g, w, h, bytes);
    ID3D11ShaderResourceView* view = screenCall(g, screen.Get(), 1);
    check(view != nullptr, "R3a: the screen is accepted");
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    view->GetDesc(&vd);
    check(vd.Format == DXGI_FORMAT_R8G8B8A8_UNORM, "R3b: the returned view is R8G8B8A8_UNORM, not sRGB");
    check(vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && vd.Texture2D.MostDetailedMip == 0, "R3c: a 2D view from the most detailed mip");
    ComPtr<ID3D11Texture2D> tex = textureOf(view);
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    checkf(vd.Texture2D.MipLevels == td.MipLevels || vd.Texture2D.MipLevels == UINT_MAX,
           "R3d: the view covers every mip (%u of %u)", vd.Texture2D.MipLevels, td.MipLevels);
    // a shader Load through the view gives the stored bytes back at mip 0, and mip 1 as generated: the view exposes the chain
    const std::vector<uint8_t> loaded0 = r.sampling.loadMip(g, view, 0, w, h);
    check(loaded0 == bytes, "R3e: a shader Load of mip 0 through the returned view gives the screen's bytes back (UNORM, no decode)");
    const UINT w1 = maxu(1, w >> 1), h1 = maxu(1, h >> 1);
    const std::vector<uint8_t> direct1 = readSubresource(g, tex.Get(), 1);
    const std::vector<uint8_t> loaded1 = r.sampling.loadMip(g, view, 1, w1, h1);
    check(loaded1 == direct1, "R3f: a shader Load of mip 1 through the returned view equals the generated mip 1's bytes");
    std::printf("  R3: Load(mip 0) and Load(mip 1) through the returned view match the stored bytes\n");
}

// R4: the full chain for any size
void caseR4(Rig& r) {
    Gpu& g = r.gpu;
    freshState(g);
    // 5040x2835, the real screen: 13 levels, mip 0 still byte-exact, and the creation line in its exact words (R11)
    {
        const UINT w = 5040, h = 2835;
        const std::vector<uint8_t> bytes = noiseBytes(w, h, 11);
        ComPtr<ID3D11Texture2D> screen = screenOf(g, w, h, bytes);
        ID3D11ShaderResourceView* view = screenCall(g, screen.Get(), 1);
        check(view != nullptr, "R4a: a 5040x2835 screen is accepted");
        ComPtr<ID3D11Texture2D> tex = textureOf(view);
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        checkf(td.MipLevels == 13, "R4b: the chain for 5040x2835 is 13 levels, got %u", td.MipLevels);
        const VrWorldMipsStats s = vrWorldMipsStats();
        check(s.width == w && s.height == h && s.levels == 13, "R4c: the module reports 5040x2835 and 13 levels");
        check(td.Width == w && td.Height == h, "R4d: the mipped texture is the screen's size");
        check(readSubresource(g, tex.Get(), 0) == bytes, "R4e: mip 0 of the full-size screen is byte-exact");
        const std::vector<uint8_t> last = readSubresource(g, tex.Get(), 12);
        check(last.size() == 4, "R4f: the last level of 5040x2835 is 1x1");
        check(g_lines.size() == 1 &&
                  g_lines[0] == "vr world mips: mipped screen 5040x2835, 13 levels, 76.2 MB (linear-light mips through an sRGB view, "
                                "sampled through the UNORM view)",
              "R11a: the creation line for the real screen reads exactly: 5040x2835, 13 levels, 76.2 MB, the colour-space note");
    }
    // the chain for other sizes, odd and tiny ones included, and a solid colour surviving to the last level
    struct Size {
        UINT w, h, levels;
    };
    const Size sizes[] = {{4032, 3898, 12}, {64, 48, 7}, {30, 18, 5}, {37, 21, 6}, {3, 5, 3}, {2, 2, 2}, {1, 1, 1}, {1000, 700, 10}};
    for (const Size& z : sizes) {
        freshState(g);
        const std::vector<uint8_t> bytes = z.w * z.h > 1000000 ? std::vector<uint8_t>(static_cast<size_t>(z.w) * z.h * 4, 0) : solidBytes(z.w, z.h, 200, 100, 50, 255);
        ComPtr<ID3D11Texture2D> screen = screenOf(g, z.w, z.h, bytes);
        ID3D11ShaderResourceView* view = screenCall(g, screen.Get(), 1);
        checkf(view != nullptr, "R4g: a %ux%u screen is accepted", z.w, z.h);
        ComPtr<ID3D11Texture2D> tex = textureOf(view);
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        checkf(td.MipLevels == z.levels && chainOf(z.w, z.h) == z.levels, "R4h: the chain for %ux%u is %u levels, got %u", z.w, z.h, z.levels, td.MipLevels);
        checkf(vrWorldMipsStats().levels == z.levels, "R4i: the module reports %u levels for %ux%u", z.levels, z.w, z.h);
        if (z.w * z.h <= 1000000) {
            const std::vector<uint8_t> last = readSubresource(g, tex.Get(), z.levels - 1);
            const bool ok = last.size() == 4 && std::abs(last[0] - 200) <= 1 && std::abs(last[1] - 100) <= 1 && std::abs(last[2] - 50) <= 1 && last[3] == 255;
            checkf(ok, "R4j: the last level of a solid (200,100,50) %ux%u screen is that colour, read (%d,%d,%d,%d)", z.w, z.h, last[0], last[1], last[2], last[3]);
        }
    }
    std::printf("  R4: 5040x2835 -> 13 levels; %zu other sizes checked, last level of solid colours intact\n", sizeof(sizes) / sizeof(sizes[0]));
}

// R5: once per frame per screen; a new size or source format recreates
void caseR5(Rig& r) {
    Gpu& g = r.gpu;
    freshState(g);
    const UINT w = 32, h = 24;
    const std::vector<uint8_t> bytesA = noiseBytes(w, h, 21), bytesB = noiseBytes(w, h, 22);
    ComPtr<ID3D11Texture2D> a = screenOf(g, w, h, bytesA), b = screenOf(g, w, h, bytesB);

    ID3D11ShaderResourceView* v1 = screenCall(g, a.Get(), 10);
    check(v1 != nullptr, "R5a: the first call answers");
    VrWorldMipsStats s = vrWorldMipsStats();
    check(s.copies == 1 && s.generates == 1 && s.creations == 1 && s.frameHits == 0, "R5b: the first call made the texture, copied once and generated once");
    checkf(g_spy.nCopies == 1 && g_spy.nGenerates == 1, "R5c: the first call issued one CopySubresourceRegion and one GenerateMips (saw %u and %u)", g_spy.nCopies, g_spy.nGenerates);

    ID3D11ShaderResourceView* v2 = screenCall(g, a.Get(), 10);
    s = vrWorldMipsStats();
    check(v2 == v1, "R5d: a second call for the same frame and screen returns the same view");
    check(s.copies == 1 && s.generates == 1 && s.frameHits == 1, "R5e: the second call was a frame hit: no copy, no generate");
    check(g_spy.calls() == 0 && g_census.begins == 0, "R5f: the second call issued no D3D call at all and opened no census scope");

    ID3D11ShaderResourceView* v3 = screenCall(g, a.Get(), 11);
    s = vrWorldMipsStats();
    check(v3 == v1, "R5g: a new frame answers with the same view (the same texture)");
    check(s.copies == 2 && s.generates == 2 && s.frameHits == 1 && s.creations == 1, "R5h: a new frame copies and generates again and does not recreate");
    check(g_spy.nCopies == 1 && g_spy.nGenerates == 1 && g_spy.nTextures == 0, "R5i: the new frame's work was one copy and one generate, no allocation of a texture");

    // another screen of the same size, same frame: its bytes must arrive (the cache is keyed on the screen too), and it must
    // not cost a recreation (a game that alternates two screen textures would otherwise allocate 76 MB a frame)
    ID3D11ShaderResourceView* v4 = screenCall(g, b.Get(), 11);
    s = vrWorldMipsStats();
    check(v4 == v1, "R5j: another screen of the same size is answered through the same view");
    check(s.copies == 3 && s.generates == 3 && s.creations == 1 && s.releases == 0, "R5k: another screen redoes the frame's work without recreating the texture");
    ComPtr<ID3D11Texture2D> tex = textureOf(v4);
    check(readSubresource(g, tex.Get(), 0) == bytesB, "R5l: mip 0 now holds the other screen's bytes");
    ID3D11ShaderResourceView* v5 = screenCall(g, a.Get(), 11);
    check(v5 == v1 && vrWorldMipsStats().copies == 4, "R5m: asking for the first screen again in the same frame redoes the work");
    check(readSubresource(g, tex.Get(), 0) == bytesA, "R5n: mip 0 holds the first screen's bytes again");

    // a size change: the width alone, the height alone, the source format alone: each recreates
    struct Change {
        const char* what;
        UINT w, h;
        DXGI_FORMAT format;
    };
    const Change changes[] = {{"width", w + 8, h, DXGI_FORMAT_R8G8B8A8_TYPELESS},
                              {"height", w + 8, h + 6, DXGI_FORMAT_R8G8B8A8_TYPELESS},
                              {"source format", w + 8, h + 6, DXGI_FORMAT_R8G8B8A8_UNORM}};
    uint64_t frame = 20;
    VrWorldMipsStats before = vrWorldMipsStats();
    for (const Change& c : changes) {
        const std::vector<uint8_t> bytes = noiseBytes(c.w, c.h, 30 + c.w + c.h);
        ComPtr<ID3D11Texture2D> screen = screenOf(g, c.w, c.h, bytes, c.format);
        ID3D11ShaderResourceView* v = screenCall(g, screen.Get(), ++frame);
        checkf(v != nullptr, "R5o: the screen after a %s change is accepted", c.what);
        const VrWorldMipsStats after = vrWorldMipsStats();
        checkf(after.creations == before.creations + 1 && after.releases == before.releases + 1,
               "R5p: a %s change recreates the texture (creations %llu -> %llu, releases %llu -> %llu)", c.what,
               static_cast<unsigned long long>(before.creations), static_cast<unsigned long long>(after.creations),
               static_cast<unsigned long long>(before.releases), static_cast<unsigned long long>(after.releases));
        checkf(after.width == c.w && after.height == c.h, "R5q: after the %s change the texture is %ux%u, got %ux%u", c.what, c.w, c.h, after.width, after.height);
        ComPtr<ID3D11Texture2D> t = textureOf(v);
        D3D11_TEXTURE2D_DESC td{};
        t->GetDesc(&td);
        checkf(td.Width == c.w && td.Height == c.h && td.MipLevels == chainOf(c.w, c.h), "R5r: after the %s change the texture really is %ux%u with its own chain", c.what, c.w, c.h);
        checkf(readSubresource(g, t.Get(), 0) == bytes, "R5s: after the %s change mip 0 holds the new screen", c.what);
        before = after;
    }
    std::printf("  R5: frame hit = no work; new frame = 1 copy + 1 generate; new screen re-copies; width/height/format change recreates\n");
}

// R6: refusals
const char* const kRefusalWords[] = {"none", "null-argument", "multisampled", "texture-array", "already-mipped", "not-rgba8", "srgb-typed", "create-failed"};

void expectRefused(Rig& r, ID3D11Texture2D* tex, VrWorldMipsRefusal why, const char* what) {
    Gpu& g = r.gpu;
    const VrWorldMipsStats before = vrWorldMipsStats();
    const size_t lines = g_lines.size();
    ID3D11ShaderResourceView* v = screenCall(g, tex, 500);
    const VrWorldMipsStats after = vrWorldMipsStats();
    checkf(v == nullptr, "R6a: %s is refused (null)", what);
    checkf(after.refusals[idx(why)] == before.refusals[idx(why)] + 1, "R6b: %s is counted as %s", what, vrWorldMipsRefusalName(why));
    for (int k = 1; k < idx(VrWorldMipsRefusal::Count); ++k)
        if (k != idx(why)) checkf(after.refusals[k] == before.refusals[k], "R6c: %s is not counted as %s", what, kRefusalWords[k]);
    checkf(after.copies == before.copies && after.generates == before.generates && after.creations == before.creations,
           "R6d: %s caused no copy, no generate and no creation", what);
    checkf(g_spy.calls() == 0 && g_census.begins == 0, "R6e: %s reached no device or context call and opened no census scope", what);
    checkf(g_lines.size() == lines + 1 && g_lines.back().find(vrWorldMipsRefusalName(why)) != std::string::npos &&
               g_lines.back().find("vr world mips: refused the screen texture") == 0,
           "R6f: %s is logged once, naming %s", what, vrWorldMipsRefusalName(why));
    // again: counted, not logged again, nothing done
    for (int i = 0; i < 3; ++i) check(screenCall(g, tex, 501 + i) == nullptr, "R6g: the same refusal again is refused again");
    const VrWorldMipsStats again = vrWorldMipsStats();
    checkf(again.refusals[idx(why)] == before.refusals[idx(why)] + 4 && g_lines.size() == lines + 1,
           "R6h: %s repeated three times is counted three more and logged no more", what);
}

struct NullCall {
    ID3D11DeviceContext* ctx;
    ID3D11Texture2D* tex;
    ID3D11ShaderResourceView* result;
};
bool callSurvives(NullCall* c) {
    __try {
        c->result = vrWorldMipsScreen(c->ctx, c->tex, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void caseR6(Rig& r) {
    Gpu& g = r.gpu;
    freshState(g);
    // an accepted screen first, so the refusals can be shown to leave the module's state alone
    const UINT w = 16, h = 16;
    ComPtr<ID3D11Texture2D> good = screenOf(g, w, h, noiseBytes(w, h, 40));
    ID3D11ShaderResourceView* kept = screenCall(g, good.Get(), 499);
    check(kept != nullptr, "R6i: a good screen is accepted before the refusals");
    const VrWorldMipsStats base = vrWorldMipsStats();

    // null arguments: no context, no texture
    {
        NullCall a{nullptr, good.Get(), nullptr}, b{g.ctx.Get(), nullptr, nullptr};
        check(callSurvives(&a) && a.result == nullptr, "R6j: a null context is refused without touching anything");
        check(callSurvives(&b) && b.result == nullptr, "R6k: a null texture is refused without touching anything");
        const VrWorldMipsStats s = vrWorldMipsStats();
        check(s.refusals[idx(VrWorldMipsRefusal::NullArgument)] == 2, "R6l: both null calls are counted as null-argument");
    }
    // the five kinds of texture the route cannot take
    expectRefused(r, makeTexture(g, w, h, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, nullptr, D3D11_BIND_SHADER_RESOURCE).Get(), VrWorldMipsRefusal::SrgbTyped, "an R8G8B8A8_UNORM_SRGB texture");
    {
        UINT samples = 0;
        for (UINT n : {4u, 2u, 8u}) {
            UINT q = 0;
            if (SUCCEEDED(g.dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, n, &q)) && q > 0) {
                samples = n;
                break;
            }
        }
        check(samples != 0, "R6m: this device offers a multisample count for R8G8B8A8_UNORM to test with");
        expectRefused(r, makeTexture(g, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 1, 1, samples).Get(), VrWorldMipsRefusal::Multisampled, "a multisampled texture");
    }
    expectRefused(r, makeTexture(g, w, h, DXGI_FORMAT_R8G8B8A8_TYPELESS, nullptr, D3D11_BIND_SHADER_RESOURCE, 1, 2).Get(), VrWorldMipsRefusal::TextureArray, "a two-slice array texture");
    expectRefused(r, makeTexture(g, w, h, DXGI_FORMAT_R8G8B8A8_TYPELESS, nullptr, D3D11_BIND_SHADER_RESOURCE, 3).Get(), VrWorldMipsRefusal::AlreadyMipped, "an already-mipped texture");
    expectRefused(r, makeTexture(g, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, nullptr).Get(), VrWorldMipsRefusal::NotRgba8, "a B8G8R8A8_UNORM texture");
    expectRefused(r, makeTexture(g, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, nullptr).Get(), VrWorldMipsRefusal::NotRgba8, "an R16G16B16A16_FLOAT texture");
    expectRefused(r, makeTexture(g, w, h, DXGI_FORMAT_R8G8B8A8_UINT, nullptr).Get(), VrWorldMipsRefusal::NotRgba8, "an R8G8B8A8_UINT texture");

    // the module's state is as it was: the same texture, and this frame's answer still stands
    const VrWorldMipsStats s = vrWorldMipsStats();
    check(s.width == base.width && s.height == base.height && s.levels == base.levels && s.creations == base.creations && s.releases == base.releases,
          "R6n: the refusals left the mipped texture as it was");
    check(screenCall(g, good.Get(), 499) == kept && vrWorldMipsStats().frameHits == base.frameHits + 1,
          "R6o: after the refusals the accepted screen's frame answer is still the cached one");

    // only the first 8 DISTINCT refusals are logged: twelve sRGB textures of different sizes, then one of them again
    vrWorldMipsStatsClear();
    g_lines.clear();
    for (UINT i = 1; i <= 12; ++i) {
        ComPtr<ID3D11Texture2D> t = makeTexture(g, i, 4, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, nullptr);
        check(screenCall(g, t.Get(), 600 + i) == nullptr, "R6p: each of the twelve is refused");
    }
    {
        ComPtr<ID3D11Texture2D> t = makeTexture(g, 1, 4, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, nullptr);
        check(screenCall(g, t.Get(), 700) == nullptr, "R6q: a thirteenth, equal to the first, is refused");
    }
    const VrWorldMipsStats cap = vrWorldMipsStats();
    size_t refusalLines = 0;
    for (const std::string& l : g_lines) refusalLines += l.find("vr world mips: refused the screen texture") == 0;
    checkf(cap.refusals[idx(VrWorldMipsRefusal::SrgbTyped)] == 13, "R6r: all thirteen are counted, got %llu", static_cast<unsigned long long>(cap.refusals[idx(VrWorldMipsRefusal::SrgbTyped)]));
    checkf(refusalLines == 8 && cap.refusalLines == 8, "R6s: only the first 8 distinct refusals are logged, got %zu lines (stat %u)", refusalLines, cap.refusalLines);

    // create-failed: the device refuses the texture, then the first view, then the second; the refusal is named and counted,
    // and every object the module had made by then is let go of (the probes hold what the device made, and ask afterwards)
    struct Injection {
        const char* what;
        HRESULT texture;
        unsigned viewAt;
        unsigned made;   // how many objects the device made before it refused
    };
    const Injection injections[] = {{"the texture", E_OUTOFMEMORY, 0, 0}, {"the sRGB view", S_OK, 1, 1}, {"the UNORM view", S_OK, 2, 2}};
    for (const Injection& in : injections) {
        freshState(g);
        ComPtr<ID3D11Texture2D> screen = screenOf(g, 32, 24, noiseBytes(32, 24, 90));
        g_spy.failTexture = in.texture;
        g_spy.failViewAt = in.viewAt;
        g_spy.failViewWith = E_OUTOFMEMORY;
        g_spy.holdCreated = true;
        check(screenCall(g, screen.Get(), 1) == nullptr, "R6t: a screen is refused when the device cannot make the mipped texture or a view");
        const VrWorldMipsStats st = vrWorldMipsStats();
        checkf(st.refusals[idx(VrWorldMipsRefusal::CreateFailed)] == 1 && st.creations == 0 && st.copies == 0 && st.generates == 0 && st.width == 0,
               "R6u: a failed creation of %s is counted as create-failed, and made, copied and generated nothing", in.what);
        checkf(g_spy.nHeld == in.made, "R6v: the device had made %u object(s) before it refused %s, saw %u", in.made, in.what, g_spy.nHeld);
        check(g_spy.nCopies == 0 && g_spy.nGenerates == 0 && g_census.begins == 0, "R6w: no copy, no GenerateMips and no census scope after a failed creation");
        checkf(g_lines.size() == 1 && g_lines[0].find("create-failed") != std::string::npos && g_lines[0].find("hr=0x8007000E") != std::string::npos,
               "R6x: the failed creation of %s is logged once, with its HRESULT", in.what);
        bool released = true;
        for (unsigned i = in.made; i-- > 0;) released = g_spy.held[i]->Release() == 0 && released;
        checkf(released, "R6y: the module released everything it had made when the device refused %s", in.what);
        check(screenCall(g, screen.Get(), 2) != nullptr && vrWorldMipsStats().creations == 1, "R6z: after the failure the next call makes the texture and answers");
    }
    std::printf("  R6: srgb, msaa, array, mipped, 3 non-RGBA8 and null calls refused and named; 13 refusals -> 8 log lines; 3 failed creations released\n");
}

// R7: the sampler
D3D11_SAMPLER_DESC gameSampler() {
    D3D11_SAMPLER_DESC d{};
    d.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    d.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
    d.AddressV = D3D11_TEXTURE_ADDRESS_MIRROR;
    d.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    d.MipLODBias = 0.75f;
    d.MaxAnisotropy = 16;
    d.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    d.BorderColor[0] = 0.25f;
    d.BorderColor[1] = 0.5f;
    d.BorderColor[2] = 0.75f;
    d.BorderColor[3] = 1.0f;
    d.MinLOD = 1.0f;
    d.MaxLOD = 2.0f;
    return d;
}

void caseR7(Rig& r) {
    Gpu& g = r.gpu;
    freshState(g);
    const D3D11_SAMPLER_DESC game = gameSampler();
    ID3D11SamplerState* s = samplerCall(g, game);
    check(s != nullptr, "R7a: a sampler is made for the game's desc");
    // what the module asked the device for (the runtime canonicalises a desc it reads back, e.g. MaxAnisotropy 0 for a
    // non-anisotropic filter, so the request itself is the evidence of what was meant)
    check(g_spy.nSamplers == 1, "R7b: one CreateSamplerState call was made");
    const D3D11_SAMPLER_DESC& asked = g_spy.samplers[0].desc;
    check(asked.Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR, "R7c: the filter asked for is trilinear (MIN_MAG_MIP_LINEAR), not the game's point");
    check(asked.AddressU == game.AddressU && asked.AddressV == game.AddressV && asked.AddressW == game.AddressW, "R7d: the three address modes are the game's");
    check(asked.BorderColor[0] == 0.25f && asked.BorderColor[1] == 0.5f && asked.BorderColor[2] == 0.75f && asked.BorderColor[3] == 1.0f, "R7e: the border colour is the game's");
    check(asked.MipLODBias == 0.0f, "R7f: the LOD bias is 0, not the game's 0.75");
    check(asked.MinLOD == 0.0f, "R7g: MinLOD is 0, not the game's 1");
    check(asked.MaxLOD == D3D11_FLOAT32_MAX, "R7h: MaxLOD is the maximum, not the game's 2");
    check(asked.MaxAnisotropy == 1, "R7i: MaxAnisotropy is 1, not the game's 16");
    check(asked.ComparisonFunc == D3D11_COMPARISON_NEVER, "R7j: the comparison function is NEVER, not the game's ALWAYS");
    // and what the runtime made of it
    D3D11_SAMPLER_DESC got{};
    s->GetDesc(&got);
    check(got.Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR && got.MinLOD == 0.0f && got.MaxLOD == D3D11_FLOAT32_MAX && got.MipLODBias == 0.0f,
          "R7k: the sampler the device made is trilinear with LOD 0..max and no bias");
    check(got.AddressU == D3D11_TEXTURE_ADDRESS_BORDER && got.AddressV == D3D11_TEXTURE_ADDRESS_MIRROR && got.AddressW == D3D11_TEXTURE_ADDRESS_WRAP,
          "R7l: the sampler the device made has the game's addressing");

    // behaviour: a texture whose mip 0 is black and mip 1 white, sampled at LOD 0.5, is grey through the module's sampler; a
    // point/MaxLOD 0 sampler (a game's) gives black, so the check can tell
    const UINT w = 16, h = 16;
    ComPtr<ID3D11Texture2D> screen = screenOf(g, w, h, solidBytes(w, h, 10, 10, 10, 255));
    ID3D11ShaderResourceView* view = screenCall(g, screen.Get(), 1);
    check(view != nullptr, "R7m: a screen for the sampling checks is accepted");
    ComPtr<ID3D11Texture2D> tex = textureOf(view);
    const std::vector<uint8_t> black = solidBytes(w, h, 0, 0, 0, 255), white = solidBytes(w / 2, h / 2, 255, 255, 255, 255);
    g.ctx->UpdateSubresource(tex.Get(), 0, nullptr, black.data(), w * 4, 0);
    g.ctx->UpdateSubresource(tex.Get(), 1, nullptr, white.data(), (w / 2) * 4, 0);
    const std::vector<uint8_t> grey = r.sampling.sampleAt(g, view, s, 0.5f, 0.5f, 0.5f);
    checkf(grey[0] >= 126 && grey[0] <= 129, "R7n: LOD 0.5 through the module's sampler blends mip 0 and mip 1 to 127/128, read %d", grey[0]);
    D3D11_SAMPLER_DESC naive{};
    naive.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    naive.AddressU = naive.AddressV = naive.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    naive.MaxLOD = 0.0f;
    naive.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ComPtr<ID3D11SamplerState> gameSampler0;
    hr(g.dev->CreateSamplerState(&naive, gameSampler0.GetAddressOf()));
    const std::vector<uint8_t> flat = r.sampling.sampleAt(g, view, gameSampler0.Get(), 0.5f, 0.5f, 0.5f);
    checkf(flat[0] == 0, "R7o: a point, MaxLOD 0 sampler at the same LOD stays on mip 0 (black), read %d: the check can tell", flat[0]);
    // the border colour and the addressing: outside the texture in u the BORDER mode returns the game's border colour
    const std::vector<uint8_t> border = r.sampling.sampleAt(g, view, s, 0.0f, -0.25f, 0.5f);
    checkf(std::abs(border[0] - 64) <= 1 && std::abs(border[1] - 128) <= 1 && std::abs(border[2] - 191) <= 1 && border[3] == 255,
           "R7p: sampling outside the texture in u returns the game's border colour (64,128,191,255), read (%d,%d,%d,%d)", border[0], border[1], border[2], border[3]);

    // the cache: the same game desc again is the table's, not a new sampler; a different one is a new entry
    const VrWorldMipsStats before = vrWorldMipsStats();
    ID3D11SamplerState* again = samplerCall(g, game);
    const VrWorldMipsStats after = vrWorldMipsStats();
    check(again == s, "R7q: the same game desc returns the same sampler pointer");
    check(after.samplerHits == before.samplerHits + 1 && after.samplerCreates == before.samplerCreates && g_spy.nSamplers == 0,
          "R7r: the repeat was a table hit: no CreateSamplerState call");
    D3D11_SAMPLER_DESC other = game;
    other.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    ID3D11SamplerState* second = samplerCall(g, other);
    check(second != nullptr && second != s, "R7s: a game desc with another address mode is a different sampler");
    // descs that differ only in what the route overrides are one sampler
    D3D11_SAMPLER_DESC same = game;
    same.Filter = D3D11_FILTER_ANISOTROPIC;
    same.MaxAnisotropy = 4;
    same.MipLODBias = -1.0f;
    same.MinLOD = 3.0f;
    same.MaxLOD = 5.0f;
    same.ComparisonFunc = D3D11_COMPARISON_LESS;
    check(samplerCall(g, same) == s && g_spy.nSamplers == 0, "R7t: a game desc that differs only in what the route overrides is the same table entry");
    check(vrWorldMipsSampler(nullptr, game) == nullptr, "R7u: no device, no sampler");

    // eviction: 8 live entries at most, the oldest goes first, and its reference is let go of (probed through the runtime)
    freshState(g);
    std::vector<ID3D11SamplerState*> made;
    RefProbe probe;
    for (int i = 0; i < 8; ++i) {
        D3D11_SAMPLER_DESC d = game;
        d.BorderColor[0] = 0.1f * static_cast<float>(i + 1);
        ID3D11SamplerState* p = samplerCall(g, d);
        checkf(p != nullptr, "R7v: sampler %d of 8 is made", i);
        made.push_back(p);
        if (i == 0) probe.hold(p);
    }
    VrWorldMipsStats eight = vrWorldMipsStats();
    check(eight.samplersLive == 8 && eight.samplerCreates == 8 && eight.samplerEvictions == 0, "R7w: eight distinct descs fill the table, nothing evicted");
    RefProbe second1;
    second1.hold(made[1]);
    D3D11_SAMPLER_DESC ninth = game;
    ninth.BorderColor[0] = 0.95f;
    ID3D11SamplerState* p9 = samplerCall(g, ninth);
    check(p9 != nullptr, "R7x: a ninth distinct desc is still answered");
    const VrWorldMipsStats nine = vrWorldMipsStats();
    check(nine.samplersLive == 8 && nine.samplerEvictions == 1 && nine.samplerCreates == 9, "R7y: the ninth evicts one entry and the table stays at 8");
    const ULONG leftOldest = probe.drop();
    checkf(leftOldest == 0, "R7z: the oldest sampler's reference was released on eviction (the probe's own was the last, left %lu)", leftOldest);
    // the second-oldest is still the table's: it is the next asked for, it is a hit, and it is the next to go
    const VrWorldMipsStats pre = vrWorldMipsStats();
    check(samplerCall(g, [&] { D3D11_SAMPLER_DESC d = game; d.BorderColor[0] = 0.2f; return d; }()) == made[1], "R7aa: the second-oldest is still cached (a hit, the same pointer)");
    check(vrWorldMipsStats().samplerHits == pre.samplerHits + 1, "R7ab: and counted as a hit");
    D3D11_SAMPLER_DESC tenth = game;
    tenth.BorderColor[0] = 0.96f;
    check(samplerCall(g, tenth) != nullptr, "R7ac: a tenth distinct desc is answered");
    const ULONG leftSecond = second1.drop();
    checkf(leftSecond == 0, "R7ad: the second sampler was the next evicted and released (left %lu)", leftSecond);
    check(vrWorldMipsStats().samplersLive == 8 && vrWorldMipsStats().samplerEvictions == 2, "R7ae: still 8 live, two evicted");
    // the evicted desc asked for again is a miss and a new sampler
    const uint64_t createsBefore = vrWorldMipsStats().samplerCreates;
    check(samplerCall(g, [&] { D3D11_SAMPLER_DESC d = game; d.BorderColor[0] = 0.1f; return d; }()) != nullptr, "R7af: the evicted desc asked for again is made again");
    check(vrWorldMipsStats().samplerCreates == createsBefore + 1, "R7ag: that was a miss (a creation)");

    // a device that refuses the sampler: null, counted, nothing added to the table, and the table still works after
    freshState(g);
    g_spy.failSampler = E_OUTOFMEMORY;
    check(samplerCall(g, game) == nullptr, "R7ah: a sampler the device cannot make is answered with null");
    const VrWorldMipsStats failed = vrWorldMipsStats();
    check(failed.samplerFailures == 1 && failed.samplerCreates == 0 && failed.samplersLive == 0 && failed.samplerEvictions == 0,
          "R7ai: the failure is counted and nothing entered the table");
    check(samplerCall(g, game) != nullptr && vrWorldMipsStats().samplersLive == 1, "R7aj: the next request makes the sampler");
    std::printf("  R7: game addressing + border kept, trilinear 0..max LOD; hits cached; 8 live max, oldest evicted and released\n");
}

// R8: the hooks
void caseR8(Rig& r) {
    Gpu& g = r.gpu;
    freshState(g);
    check(!g_vrWorldInternal && !g_flatComputeInternal, "R8a: the internal flags start clear");
    const UINT w = 40, h = 24;
    ComPtr<ID3D11Texture2D> screen = screenOf(g, w, h, noiseBytes(w, h, 50));
    // the first call makes the texture and its views, then does the work
    ID3D11ShaderResourceView* view = screenCall(g, screen.Get(), 1);
    check(view != nullptr, "R8b: the first call answers");
    check(!g_vrWorldInternal && !g_flatComputeInternal, "R8c: the internal flags are clear again when the call returns");
    check(g_spy.nTextures == 1 && g_spy.nViews == 2 && g_spy.nCopies == 1 && g_spy.nGenerates == 1,
          "R8d: the spies saw one CreateTexture2D, two CreateShaderResourceView, one copy and one GenerateMips (so the vtable slots are right)");
    ComPtr<ID3D11Texture2D> tex = textureOf(view);
    // the texture asked for
    const D3D11_TEXTURE2D_DESC& td = g_spy.textures[0].desc;
    check(td.Width == w && td.Height == h && td.ArraySize == 1 && td.SampleDesc.Count == 1, "R8e: the texture asked for is the screen's size, one slice, one sample");
    check(td.MipLevels == 0, "R8f: the texture asks for the full chain (MipLevels 0)");
    check(td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS, "R8g: the texture is R8G8B8A8_TYPELESS");
    check(td.Usage == D3D11_USAGE_DEFAULT && !g_spy.textures[0].hadInitialData, "R8h: DEFAULT usage, no initial data");
    check(td.BindFlags == (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET), "R8i: bound as shader resource and render target (GenerateMips renders the levels)");
    check((td.MiscFlags & D3D11_RESOURCE_MISC_GENERATE_MIPS) != 0 && td.CPUAccessFlags == 0, "R8j: GENERATE_MIPS, no CPU access");
    // the two views
    const ViewCall& v0 = g_spy.views[0];
    const ViewCall& v1 = g_spy.views[1];
    const UINT levels = chainOf(w, h);
    check(v0.hadDesc && v0.desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && v1.hadDesc && v1.desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM,
          "R8k: the views are made sRGB first, then UNORM");
    check(v0.desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && v1.desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D &&
              v0.desc.Texture2D.MostDetailedMip == 0 && v1.desc.Texture2D.MostDetailedMip == 0,
          "R8l: both views are 2D from mip 0");
    check((v0.desc.Texture2D.MipLevels == levels || v0.desc.Texture2D.MipLevels == UINT_MAX) &&
              (v1.desc.Texture2D.MipLevels == levels || v1.desc.Texture2D.MipLevels == UINT_MAX),
          "R8m: both views cover every mip");
    check(v0.resource == tex.Get() && v1.resource == tex.Get(), "R8n: both views are over the mipped texture");
    // the copy and the generate
    const CopyCall& cp = g_spy.copies[0];
    check(cp.dst == tex.Get() && cp.src == screen.Get(), "R8o: the copy goes from the game's screen into the mipped texture");
    check(cp.dstSub == 0 && cp.srcSub == 0 && cp.dx == 0 && cp.dy == 0 && cp.dz == 0 && cp.boxNull, "R8p: the copy is mip 0 to mip 0, whole, from the origin");
    const GenerateCall& gm = g_spy.generates[0];
    D3D11_SHADER_RESOURCE_VIEW_DESC gd{};
    gm.srv->GetDesc(&gd);
    check(gd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "R8q: GenerateMips is called with the sRGB view");
    check(textureOf(gm.srv).Get() == tex.Get(), "R8r: GenerateMips is called on a view of the mipped texture");
    check(cp.order < gm.order, "R8s: the copy comes before GenerateMips");
    // every device and context call ran inside the internal scope, and the copy and GenerateMips inside the census scope
    const Flags all[] = {g_spy.textures[0].f, g_spy.views[0].f, g_spy.views[1].f, cp.f, gm.f};
    bool internalOk = true;
    for (const Flags& f : all) internalOk = internalOk && f.world && f.compute;
    check(internalOk, "R8t: CreateTexture2D, both CreateShaderResourceView, the copy and GenerateMips all ran inside a VrWorldInternalScope (both flags set)");
    check(cp.f.census == 1 && gm.f.census == 1, "R8u: the copy and GenerateMips ran inside the GPU census scope");
    check(g_spy.textures[0].f.census == 0 && g_spy.views[0].f.census == 0, "R8v: the creations ran outside the census scope (they are not the timed work)");
    // the census scope itself
    const CensusProbe& c = g_census;
    check(c.begins == 1 && c.ends == 1 && c.depth == 0, "R8w: one census scope opened and closed");
    check(c.beginSection == GpuCensusSection::FrameWorldMips && c.endSection == GpuCensusSection::FrameWorldMips, "R8x: the census scope is FrameWorldMips");
    check(c.beginCtx == g.ctx.Get() && c.endCtx == g.ctx.Get(), "R8y: the census scope is on the context the caller gave");
    check(c.internalAtBegin && c.internalAtEnd, "R8z: the internal flags were set when the census scope opened and when it closed");
    check(c.copiesAtBegin == 0 && c.generatesAtBegin == 0 && c.copiesAtEnd == 1 && c.generatesAtEnd == 1,
          "R8aa: the census scope brackets both the copy and GenerateMips: neither had happened when it opened, both had when it closed");
    // a frame hit: nothing at all
    check(screenCall(g, screen.Get(), 1) == view && g_spy.calls() == 0 && g_census.begins == 0, "R8ab: a frame hit makes no D3D call and opens no census scope");
    // a new frame: no creation, the work inside both scopes again
    check(screenCall(g, screen.Get(), 2) == view, "R8ac: a new frame answers with the same view");
    check(g_spy.nTextures == 0 && g_spy.nViews == 0 && g_spy.nCopies == 1 && g_spy.nGenerates == 1, "R8ad: a new frame is one copy and one generate and no creation");
    check(g_spy.copies[0].f.world && g_spy.copies[0].f.compute && g_spy.generates[0].f.world && g_spy.generates[0].f.compute &&
              g_spy.copies[0].f.census == 1 && g_spy.generates[0].f.census == 1 && g_census.begins == 1 && g_census.ends == 1,
          "R8ae: the new frame's work is inside both the internal scope and the census scope again");
    // nesting: a caller already inside the scope stays inside it
    {
        VrWorldInternalScope outer;
        check(screenCall(g, screen.Get(), 3) == view, "R8af: a call made inside an outer internal scope answers");
        check(g_vrWorldInternal && g_flatComputeInternal, "R8ag: and leaves that outer scope's flags set");
    }
    check(!g_vrWorldInternal && !g_flatComputeInternal, "R8ah: the flags clear when the outer scope ends");
    // the sampler's creation is inside the scope too
    ID3D11SamplerState* s = samplerCall(g, gameSampler());
    check(s != nullptr && g_spy.nSamplers == 1, "R8ai: the sampler's CreateSamplerState was seen");
    check(g_spy.samplers[0].f.world && g_spy.samplers[0].f.compute, "R8aj: CreateSamplerState ran inside a VrWorldInternalScope");
    check(!g_vrWorldInternal && !g_flatComputeInternal, "R8ak: the flags are clear again after the sampler call");
    std::printf("  R8: every device/context call inside the internal scope; census FrameWorldMips brackets copy + GenerateMips only\n");
}

// R9: lifetime
// What the device made for one accepted screen, held by the rig (the device thunks AddRef each object as it is made) so a case
// can ask afterwards whether the module let go of every one: the texture, the sRGB view and the UNORM view, in that order. A
// texture's own reference count does not include its views, so each is probed on its own.
struct Made {
    IUnknown* object[3] = {};
};
ID3D11ShaderResourceView* screenCallHolding(Gpu& g, ID3D11Texture2D* screen, uint64_t frame, Made* made) {
    g_spy.holdCreated = true;
    ID3D11ShaderResourceView* v = screenCall(g, screen, frame);
    for (unsigned i = 0; i < 3; ++i) made->object[i] = i < g_spy.nHeld ? g_spy.held[i] : nullptr;
    return v;
}
// The probes' own references dropped: what is left of each (all 0 when the module held nothing more of them).
void dropMade(const Made& made, ULONG left[3]) {
    for (int i = 2; i >= 0; --i) left[i] = made.object[i] ? made.object[i]->Release() : 0;
}

void caseR9(Rig& r) {
    Gpu& g = r.gpu;
    freshState(g);
    const UINT w = 32, h = 24;
    // a size change lets the old texture and both its views go
    {
        ComPtr<ID3D11Texture2D> a = screenOf(g, w, h, noiseBytes(w, h, 60));
        Made made;
        check(screenCallHolding(g, a.Get(), 1, &made) != nullptr && g_spy.nHeld == 3, "R9a: a screen is accepted and the device made a texture and two views for it");
        ComPtr<ID3D11Texture2D> b = screenOf(g, w + 4, h, noiseBytes(w + 4, h, 61));
        check(screenCall(g, b.Get(), 2) != nullptr, "R9b: a screen of another size is accepted");
        g.ctx->ClearState();
        g.ctx->Flush();
        ULONG left[3];
        dropMade(made, left);
        checkf(left[2] == 0, "R9c: after a size change the old UNORM view was released (left %lu)", left[2]);
        checkf(left[1] == 0, "R9d: after a size change the old sRGB view was released (left %lu)", left[1]);
        checkf(left[0] == 0, "R9e: after a size change the old texture was released (left %lu)", left[0]);
    }
    // vrWorldMipsReset lets everything go: the texture, both views, every sampler
    {
        freshState(g);
        ComPtr<ID3D11Texture2D> a = screenOf(g, w, h, noiseBytes(w, h, 62));
        Made made;
        check(screenCallHolding(g, a.Get(), 1, &made) != nullptr && g_spy.nHeld == 3, "R9f: a screen is accepted and the device made a texture and two views for it");
        RefProbe sampler, sampler2;
        ID3D11SamplerState* s = samplerCall(g, gameSampler());
        D3D11_SAMPLER_DESC d2 = gameSampler();
        d2.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        ID3D11SamplerState* s2 = samplerCall(g, d2);
        check(s != nullptr && s2 != nullptr && s != s2, "R9g: two different samplers are made");
        sampler.hold(s);
        sampler2.hold(s2);
        g.ctx->ClearState();
        g.ctx->Flush();
        vrWorldMipsReset();
        const VrWorldMipsStats after = vrWorldMipsStats();
        check(after.width == 0 && after.height == 0 && after.levels == 0 && after.samplersLive == 0, "R9h: after a reset the module holds no texture and no sampler");
        ULONG left[3];
        dropMade(made, left);
        checkf(left[2] == 0, "R9i: a reset released the UNORM view (left %lu)", left[2]);
        checkf(left[1] == 0, "R9j: a reset released the sRGB view (left %lu)", left[1]);
        checkf(left[0] == 0, "R9k: a reset released the texture (left %lu)", left[0]);
        const ULONG s1Left = sampler.drop(), s2Left = sampler2.drop();
        checkf(s1Left == 0 && s2Left == 0, "R9l: a reset released both samplers (left %lu, %lu)", s1Left, s2Left);
        // and the module starts again from nothing: a creation, and its line
        const size_t lines = g_lines.size();
        check(screenCall(g, a.Get(), 2) != nullptr, "R9m: after a reset the next screen is accepted");
        check(vrWorldMipsStats().creations == 2 && g_lines.size() == lines + 1 && hasLine("vr world mips: mipped screen"),
              "R9n: and makes a new texture (the second creation of this case), with a new creation line");
        vrWorldMipsReset();
        vrWorldMipsReset();   // twice in a row is harmless
        check(vrWorldMipsStats().width == 0, "R9o: a second reset is harmless");
    }
    // a device change: a screen on another device releases the first device's objects and gets its own
    {
        freshState(g);
        ComPtr<ID3D11Texture2D> a = screenOf(g, w, h, noiseBytes(w, h, 63));
        Made madeA;
        check(screenCallHolding(g, a.Get(), 1, &madeA) != nullptr && g_spy.nHeld == 3, "R9p: a screen on the first device is accepted and made a texture and two views");
        RefProbe samplerA;
        samplerA.hold(samplerCall(g, gameSampler()));

        Gpu b = makeGpu();
        ComPtr<ID3D11Texture2D> screenB = screenOf(b, w, h, noiseBytes(w, h, 64));
        const VrWorldMipsStats before = vrWorldMipsStats();
        ID3D11ShaderResourceView* vb = screenCall(b, screenB.Get(), 2);
        check(vb != nullptr, "R9q: a screen on a second device is accepted");
        check(deviceOf(vb).Get() == b.dev.Get(), "R9r: the view it answers with belongs to the second device");
        const VrWorldMipsStats after = vrWorldMipsStats();
        check(after.creations == before.creations + 1 && after.releases == before.releases + 1, "R9s: a device change releases the old texture and makes a new one");
        g.ctx->ClearState();
        g.ctx->Flush();
        ULONG left[3];
        dropMade(madeA, left);
        checkf(left[2] == 0, "R9t: the first device's UNORM view was released on the device change (left %lu)", left[2]);
        checkf(left[1] == 0, "R9u: the first device's sRGB view was released on the device change (left %lu)", left[1]);
        checkf(left[0] == 0, "R9v: the first device's texture was released on the device change (left %lu)", left[0]);
        check(readSubresource(b, textureOf(vb).Get(), 0) == noiseBytes(w, h, 64), "R9w: mip 0 on the second device holds its screen");
        // a sampler for the second device replaces the table made for the first
        ID3D11SamplerState* sb = vrWorldMipsSampler(b.dev.Get(), gameSampler());
        check(sb != nullptr, "R9x: a sampler for the second device is made");
        ComPtr<ID3D11Device> sd;
        sb->GetDevice(sd.GetAddressOf());
        check(sd.Get() == b.dev.Get(), "R9y: it belongs to the second device");
        const ULONG leftS = samplerA.drop();
        checkf(leftS == 0, "R9z: the first device's sampler was released when the table moved to the second device (left %lu)", leftS);
        check(vrWorldMipsStats().samplersLive == 1, "R9za: the table holds only the second device's sampler");
        b.ctx->ClearState();
        b.ctx->Flush();
        vrWorldMipsReset();
    }
    std::printf("  R9: a size change, a reset and a device change each released every object (texture, both views, samplers: reference counts back to the probe's own)\n");
}

// R10: no heap allocation on the warm paths
void caseR10(Rig& r) {
    Gpu& g = r.gpu;
    freshState(g);
    const UINT w = 32, h = 24;
    ComPtr<ID3D11Texture2D> screen = screenOf(g, w, h, noiseBytes(w, h, 70));
    ComPtr<ID3D11Texture2D> bad = makeTexture(g, w, h, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, nullptr);
    const D3D11_SAMPLER_DESC game = gameSampler();
    // warm every path once (the creation, a refusal's first, logged, occurrence, a sampler's creation)
    check(screenCall(g, screen.Get(), 1) != nullptr, "R10a: the warm-up call answers");
    check(screenCall(g, bad.Get(), 1) == nullptr, "R10b: the warm-up refusal is refused");
    check(samplerCall(g, game) != nullptr, "R10c: the warm-up sampler is made");

    auto allocations = [&g](auto&& body) {
        Armed armed(g);
        g_heapCalls.store(0);
        g_countHeap.store(true);
        body();
        g_countHeap.store(false);
        return g_heapCalls.load();
    };
    ID3D11ShaderResourceView* v = nullptr;
    const unsigned hit = allocations([&] { v = vrWorldMipsScreen(g.ctx.Get(), screen.Get(), 1); });
    checkf(v != nullptr && hit == 0, "R10d: a frame hit allocates nothing (%u allocations)", hit);
    const unsigned work = allocations([&] { v = vrWorldMipsScreen(g.ctx.Get(), screen.Get(), 2); });
    checkf(v != nullptr && work == 0, "R10e: a new frame's copy and GenerateMips allocate nothing (%u allocations)", work);
    bool refusedAgain = false;
    const unsigned refusal = allocations([&] { refusedAgain = vrWorldMipsScreen(g.ctx.Get(), bad.Get(), 3) == nullptr; });
    checkf(refusedAgain && refusal == 0, "R10f: a repeated, already-logged refusal allocates nothing (%u allocations)", refusal);
    ID3D11SamplerState* s = nullptr;
    const unsigned hitSampler = allocations([&] { s = vrWorldMipsSampler(g.dev.Get(), game); });
    checkf(s != nullptr && hitSampler == 0, "R10g: a sampler table hit allocates nothing (%u allocations)", hitSampler);
    // the instrument itself: a deliberate allocation inside the same window is seen
    const unsigned deliberate = allocations([&] {
        std::vector<char> scratch(64);
        scratch[0] = 1;
        g_sink = scratch[0];
    });
    checkf(deliberate >= 1, "R10h: the counting instrument sees a deliberate allocation (%u)", deliberate);
    std::printf("  R10: frame hit, new-frame work, repeated refusal and sampler hit: 0 heap allocations each (the counter sees a deliberate one)\n");
}

// R11: the log
void caseR11(Rig& r) {
    Gpu& g = r.gpu;
    freshState(g);
    const UINT w = 1000, h = 500;
    ComPtr<ID3D11Texture2D> screen = screenOf(g, w, h, solidBytes(w, h, 1, 2, 3, 255));
    check(screenCall(g, screen.Get(), 1) != nullptr, "R11b: a screen is accepted");
    check(g_lines.size() == 1, "R11c: one creation line for one creation");
    // 1000x500: 500000 + 125000 + 31248 + ... the chain is 2.67 MB
    check(g_lines[0] == "vr world mips: mipped screen 1000x500, 10 levels, 2.7 MB (linear-light mips through an sRGB view, sampled through the UNORM view)",
          "R11d: the creation line's words, size, level count and megabytes (10^6 bytes) are as specified");
    check(screenCall(g, screen.Get(), 1) != nullptr && screenCall(g, screen.Get(), 2) != nullptr && g_lines.size() == 1,
          "R11e: a frame hit and a new frame write no line");
    // creations are logged up to 8 and counted beyond: twelve different sizes
    freshState(g);
    for (UINT i = 0; i < 12; ++i) {
        const UINT sw = 8 + 2 * i, sh = 8;
        ComPtr<ID3D11Texture2D> t = screenOf(g, sw, sh, solidBytes(sw, sh, 9, 9, 9, 255));
        check(screenCall(g, t.Get(), 100 + i) != nullptr, "R11f: each of twelve sizes is accepted");
    }
    size_t creationLines = 0;
    for (const std::string& l : g_lines) creationLines += l.find("vr world mips: mipped screen") == 0;
    const VrWorldMipsStats s = vrWorldMipsStats();
    checkf(s.creations == 12, "R11g: twelve creations are counted, got %llu", static_cast<unsigned long long>(s.creations));
    checkf(creationLines == 8 && s.creationLines == 8, "R11h: only the first 8 creations are logged, got %zu lines (stat %u)", creationLines, s.creationLines);
    std::printf("  R11: one creation line per creation in its exact words; capped at 8\n");
}

// ---- the cases, by rule ---------------------------------------------------------------------------------------------------
struct Case {
    const char* id;
    void (*run)(Rig&);
};
const Case kCases[] = {{"R1", caseR1}, {"R2", caseR2}, {"R3", caseR3}, {"R4", caseR4}, {"R5", caseR5},  {"R6", caseR6},
                       {"R7", caseR7}, {"R8", caseR8}, {"R9", caseR9}, {"R10", caseR10}, {"R11", caseR11}};

bool selected(const std::string& only, const char* id) {
    if (only.empty()) return true;
    const std::string list = "," + only + ",";
    return list.find(std::string(",") + id + ",") != std::string::npos;
}

// The D3D11 debug layer's own verdict, where the layer is installed: any ERROR or CORRUPTION message means the module used
// the API wrongly in a way WARP tolerated.
void reportDebugLayer(Gpu& g) {
    if (!g.debugLayer || !g.info) {
        std::puts("  D3D11 debug layer: not installed on this machine, so no runtime validation messages were checked");
        return;
    }
    const UINT64 n = g.info->GetNumStoredMessages();
    unsigned bad = 0;
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        g.info->GetMessage(i, nullptr, &len);
        std::vector<char> buf(len);
        auto* m = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
        if (FAILED(g.info->GetMessage(i, m, &len))) continue;
        if (m->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION || m->Severity == D3D11_MESSAGE_SEVERITY_ERROR) {
            std::printf("  D3D11 debug layer: %s\n", m->pDescription);
            ++bad;
        }
    }
    checkf(bad == 0, "D3D11 debug layer reported %u error message(s) for the production calls", bad);
    std::printf("  D3D11 debug layer: active, %llu message(s) stored, none of them errors\n", static_cast<unsigned long long>(n));
}

void run(const std::string& only) {
    if (!only.empty()) {
        for (const char* id = only.c_str(); *id;) {
            const char* comma = std::strchr(id, ',');
            const std::string one = comma ? std::string(id, comma) : std::string(id);
            bool known = false;
            for (const Case& c : kCases) known = known || one == c.id;
            if (!known) throw std::runtime_error("--only names a case that does not exist: " + one);
            id = comma ? comma + 1 : id + one.size();
        }
    }
    Rig rig;
    rig.gpu = makeGpu();
    std::printf("vr_world_mips_test: WARP device, debug layer %s\n", rig.gpu.debugLayer ? "on" : "off");
    check(reportSystemD3D11Only("vr_world_mips_test"), "the process runs on System32's d3d11.dll only");
    rig.sampling.init(rig.gpu);

    unsigned ran = 0;
    try {
        for (const Case& c : kCases) {
            if (!selected(only, c.id)) continue;
            std::printf("case %s\n", c.id);
            c.run(rig);
            ++ran;
        }
        checkf(ran > 0, "--only selected no case");
        reportDebugLayer(rig.gpu);
    } catch (...) {
        // what the module had logged lately, for whoever reads the failure; then its objects go before the device they belong
        // to, on a failure too
        const size_t from = g_lines.size() > 6 ? g_lines.size() - 6 : 0;
        for (size_t i = from; i < g_lines.size(); ++i) std::printf("    log: %s\n", g_lines[i].c_str());
        vrWorldMipsReset();
        throw;
    }
    vrWorldMipsReset();
    rig.gpu.ctx->ClearState();
    rig.gpu.ctx->Flush();
    std::printf("PASS: %u VR world mips checks (%u cases)\n", g_checks, ran);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2 && !wcscmp(argv[1], L"--dry-run")) {
        std::puts("dry-run: no device, no GPU work, no files");
        return 0;
    }
    std::wstring onlyW;
    bool child = false, selfTest = false, verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--child")) child = true;
        else if (!wcscmp(argv[i], L"--self-test")) selfTest = true;
        else if (!wcscmp(argv[i], L"--verbose")) verbose = true;
        else if (!wcscmp(argv[i], L"--only") && i + 1 < argc) onlyW = argv[++i];
        else {
            std::fputs("usage: --self-test [--only R1,R5,...] [--verbose] | --dry-run\n", stderr);
            return 2;
        }
    }
    if (!child && !selfTest) {
        std::fputs("usage: --self-test [--only R1,R5,...] [--verbose] | --dry-run\n", stderr);
        return 2;
    }
    edvr::g_verbose = verbose;
    std::string only;
    for (wchar_t c : onlyW) only += static_cast<char>(c);
    if (child) {
        try {
            run(only);
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL: %s\n", error.what());
            return 1;
        }
    }
    // Relaunched as a child with a watchdog (tools\gpu_census_test's convention): a hang in WARP or in the module under test
    // has to fail this rig, not sit under the runner's timeout; and the child's exit code says which way it ended.
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
    if (!length || length >= 32768) return 2;
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --child";
    if (!onlyW.empty()) command += L" --only " + onlyW;
    if (verbose) command += L" --verbose";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable, &command[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return 2;
    const DWORD waited = WaitForSingleObject(process.hProcess, 120000);
    DWORD code = 1;
    if (waited == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &code);
    } else {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 1000);
        std::fputs("FAIL: owned test child timed out\n", stderr);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (code != 0 && code != 1) std::fprintf(stderr, "FAIL: owned D3D11 child exited 0x%08lX\n", code);
    return code == 0 ? 0 : 1;
}

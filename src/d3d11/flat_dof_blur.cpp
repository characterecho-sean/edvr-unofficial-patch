// The depth-of-field blur's average: flat_dof_blur.h says what and why; the rules are flat_dof_blur_math.h's.
#include "flat_dof_blur.h"

#include "flat_compute_readback.h"  // FlatComputeInternalScope
#include "flat_dof_blur_math.h"
#include "flat_mono_frame.h"        // kToneDofCompositePs
#include "temporal_shader_bytecode.h"
#include "vscreen.h"                // the raw PS entry point: the shadow keeps describing the game's bindings

#include "../common/guard.h"
#include "../common/log.h"

#include <wrl/client.h>

#include <cstring>

namespace edvr {

namespace {

template <class T>
using Ptr = Microsoft::WRL::ComPtr<T>;

static_assert(kFlatDofBlurTonePs == flat_mono_detail::kToneDofCompositePs, "the tone that blends the blur is the selector's own");
static_assert(DXGI_FORMAT_R16G16B16A16_FLOAT == 10 && DXGI_FORMAT_R11G11B10_FLOAT == 26, "flatDofBlurShapeOk names its formats by number");

struct Images {
    Ptr<ID3D11Texture2D> ring[kFlatDofBlurMaxFrames];
    Ptr<ID3D11ShaderResourceView> ringSrv[kFlatDofBlurMaxFrames];
    Ptr<ID3D11Texture2D> averaged;
    Ptr<ID3D11ShaderResourceView> averagedSrv;
    Ptr<ID3D11UnorderedAccessView> averagedUav;
    uint32_t width = 0, height = 0, viewFormat = 0;
    DXGI_FORMAT textureFormat = DXGI_FORMAT_UNKNOWN;
};
Images g_images;
Ptr<ID3D11ComputeShader> g_cs;
Ptr<ID3D11Buffer> g_weights[kFlatDofBlurMaxFrames];  // [n - 1] holds 1/n
Ptr<ID3D11Device> g_device;
bool g_shaderFailed = false;
FlatDofBlurRetry g_imageRetry;

FlatDofBlurRing g_ring;
uint64_t g_lastComposite = 0;
bool g_lastCompositeValid = false;
bool g_averagedLast = false;

float g_rows[2][3] = {};
uint64_t g_noteFrame = 0;
bool g_noteValid = false, g_moved = false, g_movedValid = false;

struct Window {
    uint64_t composites = 0, averaged = 0, startedRun = 0, refusedShape = 0, failed = 0;
    uint64_t steps[static_cast<int>(FlatDofBlurStep::Count)] = {};
    uint32_t lastFrames = 0, lastW = 0, lastH = 0;
};
Window g_win;
bool g_firstNoted = false, g_shapeNoted = false;
FaultBudget g_budget("flatDofBlur.pass", 4);

void releaseImages() {
    g_images = Images{};
    g_ring.restart();
    g_averagedLast = false;
}

void releaseObjects() {
    releaseImages();
    g_cs.Reset();
    for (auto& b : g_weights) b.Reset();
    g_shaderFailed = false;
    flatDofBlurRetryClear(g_imageRetry);
}

bool readShape(ID3D11ShaderResourceView* view, FlatDofBlurShape* shape, D3D11_TEXTURE2D_DESC* desc, Ptr<ID3D11Texture2D>* texture) {
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    view->GetDesc(&vd);
    shape->tex2dView = vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
    shape->viewFormat = static_cast<uint32_t>(vd.Format);
    if (shape->tex2dView) shape->mostDetailedMip = vd.Texture2D.MostDetailedMip;
    Ptr<ID3D11Resource> resource;
    view->GetResource(&resource);
    if (!resource || FAILED(resource.As(texture)) || !*texture) return false;
    (*texture)->GetDesc(desc);
    shape->mipLevels = desc->MipLevels;
    shape->arraySize = desc->ArraySize;
    shape->sampleCount = desc->SampleDesc.Count;
    shape->width = desc->Width;
    shape->height = desc->Height;
    return true;
}

bool ensureShader(ID3D11Device* dev) {
    if (g_cs) return true;
    if (g_shaderFailed) return false;
    bool ok = SUCCEEDED(dev->CreateComputeShader(kFlatDofBlurBytecode, sizeof(kFlatDofBlurBytecode), nullptr, &g_cs));
    for (uint32_t i = 0; ok && i < kFlatDofBlurMaxFrames; ++i) {
        const float weight[4] = {1.0f / static_cast<float>(i + 1), 0.0f, 0.0f, 0.0f};
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(weight);
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = weight;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, &init, &g_weights[i]));
    }
    if (!ok) {
        g_shaderFailed = true;
        g_cs.Reset();
        for (auto& b : g_weights) b.Reset();
        Log::get().note("flat dof blur: the averaging shader or its constants could not be created; the game's blur stays as it is this session");
    }
    return ok;
}

// On false, what names the object that could not be made and hr its result.
bool ensureImages(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& blur, uint32_t viewFormat, const char** what, HRESULT* hr) {
    if (g_images.averaged && g_images.width == blur.Width && g_images.height == blur.Height && g_images.textureFormat == blur.Format && g_images.viewFormat == viewFormat)
        return true;
    releaseImages();
    Images made;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = blur.Width;
    d.Height = blur.Height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = blur.Format;  // CopyResource needs the blur's own format family
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = static_cast<DXGI_FORMAT>(viewFormat);
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    for (uint32_t i = 0; i < kFlatDofBlurMaxFrames; ++i) {
        *hr = dev->CreateTexture2D(&d, nullptr, &made.ring[i]);
        if (FAILED(*hr)) { *what = "a ring texture"; return false; }
        *hr = dev->CreateShaderResourceView(made.ring[i].Get(), &sv, &made.ringSrv[i]);
        if (FAILED(*hr)) { *what = "a ring view"; return false; }
    }
    // The mean is always half-float: it holds both formats, and a compute shader can write it.
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    *hr = dev->CreateTexture2D(&d, nullptr, &made.averaged);
    if (FAILED(*hr)) { *what = "the averaged texture"; return false; }
    *hr = dev->CreateShaderResourceView(made.averaged.Get(), nullptr, &made.averagedSrv);
    if (FAILED(*hr)) { *what = "the averaged view"; return false; }
    *hr = dev->CreateUnorderedAccessView(made.averaged.Get(), nullptr, &made.averagedUav);
    if (FAILED(*hr)) { *what = "the averaged unordered view"; return false; }
    made.width = blur.Width;
    made.height = blur.Height;
    made.viewFormat = viewFormat;
    made.textureFormat = blur.Format;
    g_images = made;
    return true;
}

// The mean of the ring into the averaged image, every compute slot it touches put back.
void average(ID3D11DeviceContext* ctx) {
    ID3D11ComputeShader* savedCs = nullptr;
    ID3D11ShaderResourceView* savedSrv[kFlatDofBlurMaxFrames] = {};
    ID3D11UnorderedAccessView* savedUav = nullptr;
    ID3D11Buffer* savedCb = nullptr;
    ctx->CSGetShader(&savedCs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, kFlatDofBlurMaxFrames, savedSrv);
    ctx->CSGetUnorderedAccessViews(0, 1, &savedUav);
    ctx->CSGetConstantBuffers(0, 1, &savedCb);

    ID3D11ShaderResourceView* frames[kFlatDofBlurMaxFrames] = {};
    for (uint32_t i = 0; i < g_ring.count; ++i) frames[i] = g_images.ringSrv[i].Get();
    ID3D11UnorderedAccessView* uav = g_images.averagedUav.Get();
    ID3D11Buffer* weight = g_weights[g_ring.count - 1].Get();
    const UINT keep = ~0u;
    ctx->CSSetShader(g_cs.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, kFlatDofBlurMaxFrames, frames);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, &keep);
    ctx->CSSetConstantBuffers(0, 1, &weight);
    ctx->Dispatch((g_images.width + 7) / 8, (g_images.height + 7) / 8, 1);

    ID3D11UnorderedAccessView* noUav = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &noUav, &keep);
    ctx->CSSetShader(savedCs, nullptr, 0);
    ctx->CSSetShaderResources(0, kFlatDofBlurMaxFrames, savedSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &savedUav, &keep);
    ctx->CSSetConstantBuffers(0, 1, &savedCb);
    if (savedCs) savedCs->Release();
    for (auto* s : savedSrv)
        if (s) s->Release();
    if (savedUav) savedUav->Release();
    if (savedCb) savedCb->Release();
}

}  // namespace

bool flatDofBlurBegin(ID3D11DeviceContext* ctx, uint64_t frame, bool jittered, bool hdrRouteTreated, uint32_t phaseCount, uint64_t ps, ID3D11ShaderResourceView** original) {
    if (!ctx || !original || *original) return false;
    ++g_win.composites;
    const bool continues = g_lastCompositeValid && frame == g_lastComposite + 1 && g_averagedLast;
    g_lastComposite = frame;
    g_lastCompositeValid = true;
    g_averagedLast = false;

    FlatDofBlurAsk ask;
    ask.jittered = jittered;
    ask.hdrRouteTreated = hdrRouteTreated;
    ask.cameraKnown = g_noteValid && g_movedValid && g_noteFrame + 1 == frame;
    ask.cameraMoved = g_moved;
    const FlatDofBlurStep step = flatDofBlurDecide(ask);
    ++g_win.steps[static_cast<int>(step)];
    if (step != FlatDofBlurStep::Average) {
        g_ring.restart();
        return false;
    }

    FlatComputeInternalScope internal;
    ID3D11ShaderResourceView* gameRaw = nullptr;
    ctx->PSGetShaderResources(kFlatDofBlurSlot, 1, &gameRaw);
    Ptr<ID3D11ShaderResourceView> game;
    game.Attach(gameRaw);
    if (!game) return false;
    FlatDofBlurShape shape;
    D3D11_TEXTURE2D_DESC desc{};
    Ptr<ID3D11Texture2D> texture;
    if (!readShape(game.Get(), &shape, &desc, &texture) || !flatDofBlurShapeOk(shape)) {
        ++g_win.refusedShape;
        if (!g_shapeNoted) {
            g_shapeNoted = true;
            Log::get().note("flat dof blur: composite ps %016llX has a blur this cannot average (view format %u, %ux%u, %u mips, %u array, %u samples); the game's blur stays bound",
                            static_cast<unsigned long long>(ps), shape.viewFormat, shape.width, shape.height, shape.mipLevels, shape.arraySize, shape.sampleCount);
        }
        return false;
    }
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev || !ensureShader(dev.Get())) {
        ++g_win.failed;
        return false;
    }
    FlatDofBlurShapeKey key;
    key.width = desc.Width;
    key.height = desc.Height;
    key.textureFormat = static_cast<uint32_t>(desc.Format);
    key.viewFormat = shape.viewFormat;
    if (!flatDofBlurRetryAllowed(g_imageRetry, key, frame)) {
        ++g_win.failed;   // failed on this shape a short while ago: the game's blur stays bound until the wait is over
        return false;
    }
    const char* what = "";
    HRESULT hr = S_OK;
    if (!ensureImages(dev.Get(), desc, shape.viewFormat, &what, &hr)) {
        ++g_win.failed;
        if (flatDofBlurRetryFailed(g_imageRetry, key, frame))
            Log::get().note("flat dof blur: %s could not be made for a blur of %ux%u (texture format %u, view format %u), hr=0x%08lX; the game's blur stays bound, and another try waits %llu frames unless the shape changes",
                            what, desc.Width, desc.Height, key.textureFormat, key.viewFormat, static_cast<unsigned long>(hr), static_cast<unsigned long long>(kFlatDofBlurIdleFrames));
        return false;
    }
    if (g_imageRetry.failed) flatDofBlurRetryClear(g_imageRetry);
    if (!continues) {
        g_ring.restart();
        ++g_win.startedRun;
    }
    const uint32_t slot = g_ring.push(phaseCount);
    bool ran = guardedBudget(g_budget, [&] {
        ctx->CopyResource(g_images.ring[slot].Get(), texture.Get());
        // The first frame of a run is its own mean, and the game's view already says it.
        if (g_ring.count > 1) average(ctx);
    });
    if (!ran) {
        ++g_win.failed;
        g_ring.restart();
        return false;
    }
    g_averagedLast = true;
    if (g_ring.count < 2) return false;
    ID3D11ShaderResourceView* bound = g_images.averagedSrv.Get();
    vScreenPSSetShaderResourcesRaw(ctx, kFlatDofBlurSlot, 1, &bound);
    *original = game.Detach();
    ++g_win.averaged;
    g_win.lastFrames = g_ring.count;
    g_win.lastW = desc.Width;
    g_win.lastH = desc.Height;
    if (!g_firstNoted) {
        g_firstNoted = true;
        Log::get().note("flat dof blur: composite ps %016llX reads the mean of the last frames' blur from frame %llu on (window %u, blur %ux%u, view format %u); a window line follows while composites run",
                        static_cast<unsigned long long>(ps), static_cast<unsigned long long>(frame), g_ring.window, desc.Width, desc.Height, shape.viewFormat);
    }
    return true;
}

void flatDofBlurEnd(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* original) {
    if (!original) return;
    if (ctx) {
        FlatComputeInternalScope internal;
        ID3D11ShaderResourceView* game = original;
        vScreenPSSetShaderResourcesRaw(ctx, kFlatDofBlurSlot, 1, &game);
    }
    original->Release();
}

void flatDofBlurNoteCamera(uint64_t frame, const float (*camera)[4]) {
    if (!camera) return;
    const bool adjacent = g_noteValid && g_noteFrame + 1 == frame;
    g_moved = adjacent && flatDofBlurCameraMoved(camera[4], g_rows[0], camera[5], g_rows[1]);
    g_movedValid = adjacent;
    std::memcpy(g_rows[0], camera[4], sizeof(g_rows[0]));
    std::memcpy(g_rows[1], camera[5], sizeof(g_rows[1]));
    g_noteFrame = frame;
    g_noteValid = true;
}

void flatDofBlurFrame(uint64_t frame) {
    if (g_images.averaged && (!g_lastCompositeValid || frame > g_lastComposite + kFlatDofBlurIdleFrames)) releaseImages();
}

void flatDofBlurLogWindow() {
    if (!g_win.composites) return;
    using S = FlatDofBlurStep;
    Log::get().note("flat dof blur 5s: composites=%llu averaged=%llu runs-started=%llu window-now=%u blur=%ux%u; left to the game: not-jittered=%llu hdr-route=%llu camera-unknown=%llu camera-moved=%llu "
                    "shape=%llu failed=%llu",
                    static_cast<unsigned long long>(g_win.composites), static_cast<unsigned long long>(g_win.averaged), static_cast<unsigned long long>(g_win.startedRun), g_win.lastFrames,
                    g_win.lastW, g_win.lastH, static_cast<unsigned long long>(g_win.steps[static_cast<int>(S::NotJittered)]),
                    static_cast<unsigned long long>(g_win.steps[static_cast<int>(S::HdrRoute)]), static_cast<unsigned long long>(g_win.steps[static_cast<int>(S::CameraUnknown)]),
                    static_cast<unsigned long long>(g_win.steps[static_cast<int>(S::CameraMoved)]), static_cast<unsigned long long>(g_win.refusedShape), static_cast<unsigned long long>(g_win.failed));
    g_win = Window{};
}

void flatDofBlurRelease() {
    releaseImages();
    flatDofBlurRetryClear(g_imageRetry);
    g_noteValid = g_movedValid = false;
    g_lastCompositeValid = false;
}

void flatDofBlurNoteDevice(ID3D11Device* device) {
    if (!device || device == g_device.Get()) return;
    if (g_device) releaseObjects();
    g_device = device;
}

}  // namespace edvr

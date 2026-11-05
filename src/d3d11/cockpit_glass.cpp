// visual.cockpit_glass: cockpit_glass.h says what and why; the decisions are cockpit_glass_core.h's, the dirt texture is cockpit_glass_streaks.h's.
#include "cockpit_glass.h"

#include "binding_shadow.h"            // bindingShaderHash: the game's own shader hashes, which the swap and the overlay never disturb
#include "cockpit_glass_core.h"
#include "cockpit_glass_streaks.h"
#include "shader_swap.h"
#include "temporal_shader_bytecode.h"   // kCockpitGlass*PsBytecode
#include "vscreen.h"                    // the raw calls: past the binding shadow's hooks, so the shadow keeps describing the game's bindings

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"

#include <windows.h>

#include <string>
#include <thread>
#include <vector>

namespace edvr {

namespace detail {
std::atomic<bool> g_cockpitGlassLegacy{false};
}  // namespace detail

namespace {

using namespace cockpit_glass;

constexpr ULONGLONG kWindowMs = 30000;
// The streak texture lives at t12 for every variant (cockpit_glass_shader.h).
constexpr UINT kStreakSlot = 12;

bool g_configRead = false;
uint32_t g_configGeneration = 0;
bool g_on = false;
int g_dirtPercent = kDirtPercentDefault;   // visual.cockpit_glass_dirt, read with the look at the frame boundary
std::string g_unrecognisedSaid;   // the last unrecognised value the log has named, so a typo is said once and not every config change

// Everything made on one device is kept as raw pointers on purpose, never released at exit (a static destructor that releases a D3D object during shutdown is the kind of
// thing the exit-crash arcs were about). They are released only when the device they were made on is replaced.
void* g_device = nullptr;
ID3D11PixelShader* g_ps = nullptr;                    // the intact replacement
ID3D11PixelShader* g_overlayPs[2] = {nullptr, nullptr};   // ice, condensation
ID3D11BlendState* g_overlayBlend = nullptr;           // colour ONE + ONE, RGB written, alpha untouched
ID3D11ShaderResourceView* g_streakSrv = nullptr;
ID3D11Buffer* g_levelCb = nullptr;                    // the dirt level, one register (16 bytes) at b13
float g_levelUploaded = -1.0f;                        // the level g_levelCb holds; a different one is uploaded at the next bind
bool g_levelRefused = false;
Replacement g_state = Replacement::NotTried;
bool g_overlayRefused[2] = {false, false};
bool g_blendRefused = false;
bool g_streakRefused = false;
bool g_streakLogged = false;

// The streak texture is generated off the render thread (about 50 ms on the build machine): started when the key turns on, picked up when ready. Until then the
// replacement draws without dirt (an unbound t12 reads 0), never stock. Leaked, so a thread that outlives everything finds its job.
struct StreakJob {
    std::atomic<int> state{0};   // 0 not started, 1 running, 2 ready, 3 failed
    std::vector<std::vector<uint8_t>> mips;
    double ms = 0;
    double mean = 0;   // of the stored value (1 = the legacy max)
};
StreakJob* g_job = nullptr;

ULONGLONG g_windowStart = 0;
uint32_t g_binds = 0;      // canopy binds handed the replacement, this window
uint32_t g_seen = 0;       // canopy binds the game made, this window
uint32_t g_overlays = 0;   // ice or condensation draws drawn over with the legacy dirt, this window

// What was at t12 and b13 before the intact replacement's bind put the streaks and the dirt level there; given back at the next pixel-shader bind of another shader, or at the
// frame boundary.
ID3D11ShaderResourceView* g_t12Prev = nullptr;
ID3D11Buffer* g_b13Prev = nullptr;
bool g_t12Bound = false;

// The overlay's own save of the game's state across the one reissued draw.
ID3D11PixelShader* g_savedPs = nullptr;
ID3D11BlendState* g_savedBlend = nullptr;
float g_savedFactor[4] = {1, 1, 1, 1};
UINT g_savedMask = 0xFFFFFFFFu;
ID3D11ShaderResourceView* g_savedT12 = nullptr;
ID3D11Buffer* g_savedB13 = nullptr;

template <class T>
void releaseGuarded(T*& p, const char* site) {
    T* old = p;
    p = nullptr;
    if (old) guarded(site, [&] { old->Release(); });
}

// The device a context belongs to, as a plain pointer for comparison (the reference taken is released at once).
void* deviceOf(ID3D11DeviceContext* ctx) {
    ID3D11Device* dev = nullptr;
    if (!guarded("cockpitGlass.device", [&] { ctx->GetDevice(&dev); }) || !dev) return nullptr;
    void* key = dev;
    guarded("cockpitGlass.device.release", [&] { dev->Release(); });
    return key;
}

void startStreaks() {
    if (g_job) return;
    g_job = new StreakJob;
    g_job->state.store(1);
    StreakJob* job = g_job;
    try {
        std::thread([job] {
            try {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                LARGE_INTEGER f, a, b;
                QueryPerformanceFrequency(&f);
                QueryPerformanceCounter(&a);
                std::vector<uint8_t> base(static_cast<size_t>(glass_streaks::kSize) * glass_streaks::kSize);
                glass_streaks::generate(base.data());
                glass_streaks::buildMips(base.data(), glass_streaks::kSize, job->mips);
                QueryPerformanceCounter(&b);
                double sum = 0;
                for (uint8_t v : base) sum += v;
                job->mean = sum / static_cast<double>(base.size()) / 255.0;
                job->ms = static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
                job->state.store(2);
            } catch (...) {
                job->state.store(3);
            }
        }).detach();
    } catch (...) {
        job->state.store(3);
    }
}

// The streak texture's view for this device, made from the finished job the first time it is wanted. Null until the worker is done, and for good if it failed.
ID3D11ShaderResourceView* streakSrv(ID3D11DeviceContext* ctx) {
    if (g_streakSrv) return g_streakSrv;
    if (g_streakRefused) return nullptr;
    if (!g_job) startStreaks();
    const int state = g_job->state.load();
    if (state == 3) {
        g_streakRefused = true;
        Log::get().note("cockpit glass: the dirt streak texture could not be generated; the legacy glass draws without dirt");
        return nullptr;
    }
    if (state != 2) return nullptr;
    if (!g_streakLogged) {
        g_streakLogged = true;
        Log::get().note("cockpit glass: dirt streak texture %dx%d generated in %.1f ms on a worker thread (stored mean %.4f of max, the legacy dirt texture's 0.024 / 0.09 = %.4f)",
                        glass_streaks::kSize, glass_streaks::kSize, g_job->ms, g_job->mean, static_cast<double>(glass_streaks::kTargetMean));
    }
    ID3D11Device* dev = nullptr;
    bool ok = false;
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    guarded("cockpitGlass.streaks.create", [&] {
        ctx->GetDevice(&dev);
        if (!dev) return;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = glass_streaks::kSize;
        td.MipLevels = static_cast<UINT>(g_job->mips.size());
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<D3D11_SUBRESOURCE_DATA> init(g_job->mips.size());
        int size = glass_streaks::kSize;
        for (size_t m = 0; m < g_job->mips.size(); ++m, size /= 2) {
            init[m].pSysMem = g_job->mips[m].data();
            init[m].SysMemPitch = static_cast<UINT>(size);
        }
        if (SUCCEEDED(dev->CreateTexture2D(&td, init.data(), &tex)) && tex) ok = SUCCEEDED(dev->CreateShaderResourceView(tex, nullptr, &view)) && view;
    });
    if (dev) guarded("cockpitGlass.streaks.device.release", [&] { dev->Release(); });
    releaseGuarded(tex, "cockpitGlass.streaks.tex.release");   // the view keeps the texture alive
    if (!ok) {
        releaseGuarded(view, "cockpitGlass.streaks.view.release");
        g_streakRefused = true;
        Log::get().note("cockpit glass: the dirt streak texture was refused by the device; the legacy glass draws without dirt");
        return nullptr;
    }
    g_streakSrv = view;
    Log::get().note("cockpit glass: dirt streak texture created (R8_UNORM %dx%d, %zu mips) and bound at t12", glass_streaks::kSize, glass_streaks::kSize, g_job->mips.size());
    return g_streakSrv;
}

void dropDeviceObjects() {
    releaseGuarded(g_ps, "cockpitGlass.ps.release");
    for (auto& p : g_overlayPs) releaseGuarded(p, "cockpitGlass.overlay.release");
    releaseGuarded(g_overlayBlend, "cockpitGlass.blend.release");
    releaseGuarded(g_streakSrv, "cockpitGlass.streaks.release");
    releaseGuarded(g_levelCb, "cockpitGlass.level.release");
    releaseGuarded(g_t12Prev, "cockpitGlass.t12.release");
    releaseGuarded(g_b13Prev, "cockpitGlass.b13.release");
    g_t12Bound = false;
    g_state = Replacement::NotTried;
    g_overlayRefused[0] = g_overlayRefused[1] = false;
    g_blendRefused = false;
    g_streakRefused = false;
    g_streakLogged = false;
    g_levelRefused = false;
    g_levelUploaded = -1.0f;
}

// The device the objects were made on is this context's, or they are made again.
void* ensureDevice(ID3D11DeviceContext* ctx) {
    void* dev = deviceOf(ctx);
    if (!dev) return nullptr;
    if (g_device && g_device != dev) {
        Log::get().note("cockpit glass: the device changed; the replacement shaders and the streak texture are made again");
        dropDeviceObjects();
    }
    g_device = dev;
    return dev;
}

// The intact replacement for this context's device, made the first time it is wanted. Null means draw stock; a refusal stands until the key is turned off and on.
ID3D11PixelShader* replacementFor(ID3D11DeviceContext* ctx) {
    if (g_state == Replacement::Refused) return nullptr;
    if (!ensureDevice(ctx)) return nullptr;
    if (g_ps) return g_ps;
    g_ps = shaderSwapCreatePs(ctx, kCockpitGlassLegacyPsBytecode, sizeof(kCockpitGlassLegacyPsBytecode), "cockpit_glass_legacy_ps", "cockpit glass");
    if (!g_ps) {
        g_state = Replacement::Refused;
        Log::get().note("cockpit glass: replacement pixel shader refused (the build's bytecode did not create); the canopy is drawn stock");
        return nullptr;
    }
    g_state = Replacement::Ready;
    Log::get().note("cockpit glass: replacement pixel shader ready (%zu bytes of build bytecode); it takes the place of the game's intact quality-3 "
                    "canopy shader %016llX, linked with the canopy vertex shader %016llX",
                    sizeof(kCockpitGlassLegacyPsBytecode), static_cast<unsigned long long>(kReplacedPsHash),
                    static_cast<unsigned long long>(kCanopyVsHash));
    return g_ps;
}

// The dirt level's block for this device, made the first time it is wanted and brought up to date with visual.cockpit_glass_dirt when the player has moved it (an upload
// only on a change). Null when the device refused it; the shaders then read an unbound b13, which is no dirt, never the game's own glass.
ID3D11Buffer* levelBuffer(ID3D11DeviceContext* ctx) {
    const float level = dirtLevelOf(g_dirtPercent);
    if (g_levelCb) {
        if (level != g_levelUploaded) {
            const float v[4] = {level, 0, 0, 0};
            guarded("cockpitGlass.level.update", [&] { vScreenUpdateSubresourceRaw(ctx, g_levelCb, 0, nullptr, v, 0, 0); });
            g_levelUploaded = level;
        }
        return g_levelCb;
    }
    if (g_levelRefused) return nullptr;
    ID3D11Device* dev = nullptr;
    guarded("cockpitGlass.level.create", [&] {
        ctx->GetDevice(&dev);
        if (!dev) return;
        const float v[4] = {level, 0, 0, 0};
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof v;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const D3D11_SUBRESOURCE_DATA init{v, 0, 0};
        dev->CreateBuffer(&bd, &init, &g_levelCb);
    });
    if (dev) guarded("cockpitGlass.level.device.release", [&] { dev->Release(); });
    if (!g_levelCb) {
        g_levelRefused = true;
        Log::get().note("cockpit glass: the dirt level block was refused by the device; the legacy glass draws without dirt");
        return nullptr;
    }
    g_levelUploaded = level;
    return g_levelCb;
}

// The shaders' two inputs of EDVR's own: the streak texture at t12 and the dirt level at b13. Both are put back by restoreGlassInputs.
void bindGlassInputs(ID3D11DeviceContext* ctx) {
    ID3D11ShaderResourceView* srv = streakSrv(ctx);   // null while the worker runs: an unbound slot reads 0, no dirt
    ID3D11Buffer* level = levelBuffer(ctx);           // null if refused: bound as such, so b13 never holds a block of the game's
    guarded("cockpitGlass.inputs.bind", [&] {
        if (!g_t12Bound) {
            ctx->PSGetShaderResources(kStreakSlot, 1, &g_t12Prev);
            ctx->PSGetConstantBuffers(kLevelSlot, 1, &g_b13Prev);
            g_t12Bound = true;
        }
        vScreenPSSetShaderResourcesRaw(ctx, kStreakSlot, 1, &srv);
        vScreenPSSetConstantBuffersRaw(ctx, kLevelSlot, 1, &level);
    });
}

void restoreGlassInputs(ID3D11DeviceContext* ctx) {
    if (!g_t12Bound) return;
    g_t12Bound = false;
    ID3D11ShaderResourceView* prev = g_t12Prev;
    ID3D11Buffer* prevB13 = g_b13Prev;
    g_t12Prev = nullptr;
    g_b13Prev = nullptr;
    guarded("cockpitGlass.inputs.restore", [&] {
        vScreenPSSetShaderResourcesRaw(ctx, kStreakSlot, 1, &prev);
        vScreenPSSetConstantBuffersRaw(ctx, kLevelSlot, 1, &prevB13);
    });
    if (prev) guarded("cockpitGlass.streaks.prev.release", [&] { prev->Release(); });
    if (prevB13) guarded("cockpitGlass.b13.prev.release", [&] { prevB13->Release(); });
}

ID3D11PixelShader* overlayShaderFor(ID3D11DeviceContext* ctx, Overlay which) {
    const int i = static_cast<int>(which);
    if (g_overlayPs[i]) return g_overlayPs[i];
    if (g_overlayRefused[i]) return nullptr;
    const bool ice = which == Overlay::Ice;
    const void* code = ice ? static_cast<const void*>(kCockpitGlassIceOverlayPsBytecode) : static_cast<const void*>(kCockpitGlassCondensationOverlayPsBytecode);
    const size_t bytes = ice ? sizeof(kCockpitGlassIceOverlayPsBytecode) : sizeof(kCockpitGlassCondensationOverlayPsBytecode);
    g_overlayPs[i] = shaderSwapCreatePs(ctx, code, bytes, ice ? "cockpit_glass_ice_overlay_ps" : "cockpit_glass_condensation_overlay_ps", "cockpit glass");
    if (!g_overlayPs[i]) {
        g_overlayRefused[i] = true;
        Log::get().note("cockpit glass: the %s overlay pixel shader was refused; the %s is drawn as the game draws it, without the legacy dirt", ice ? "ice" : "condensation",
                        ice ? "ice" : "condensation");
        return nullptr;
    }
    Log::get().note("cockpit glass: %s overlay pixel shader ready (%zu bytes); it is drawn over the game's %s canopy draw, quality 3 (%016llX), with colour ONE + ONE",
                    ice ? "ice" : "condensation", bytes, ice ? "ice" : "condensation",
                    static_cast<unsigned long long>(ice ? kIcePsQ3Hash : kCondensationPsQ3Hash));
    return g_overlayPs[i];
}

ID3D11BlendState* overlayBlend(ID3D11DeviceContext* ctx) {
    if (g_overlayBlend) return g_overlayBlend;
    if (g_blendRefused) return nullptr;
    ID3D11Device* dev = nullptr;
    guarded("cockpitGlass.blend.create", [&] {
        ctx->GetDevice(&dev);
        if (!dev) return;
        D3D11_BLEND_DESC bd{};
        D3D11_RENDER_TARGET_BLEND_DESC& rt = bd.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_ONE;
        rt.DestBlend = D3D11_BLEND_ONE;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_ZERO;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;   // alpha untouched
        dev->CreateBlendState(&bd, &g_overlayBlend);
    });
    if (dev) guarded("cockpitGlass.blend.device.release", [&] { dev->Release(); });
    if (!g_overlayBlend) {
        g_blendRefused = true;
        Log::get().note("cockpit glass: the overlay blend state was refused; ice and condensation are drawn as the game draws them");
    }
    return g_overlayBlend;
}

void resetWindow(ULONGLONG now) {
    g_windowStart = now;
    g_binds = 0;
    g_seen = 0;
    g_overlays = 0;
}

void logWindow(ULONGLONG now) {
    char line[240];
    formatWindow(line, sizeof line, (now - g_windowStart) / 1000.0, g_binds, g_seen, g_overlays, replacementName(g_state));
    Log::get().note("%s", line);
}

}  // namespace

ID3D11PixelShader* cockpitGlassPsFor(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, uint64_t hash) {
    if (hash != kReplacedPsHash) {   // every other bind: this compare, the streaks' and the level's hand-back if they are still bound, and out
        if (g_t12Bound) restoreGlassInputs(ctx);
        return ps;
    }
    ++g_seen;
    ID3D11PixelShader* replacement = replacementFor(ctx);
    if (!swapWanted(Look::Legacy, replacement != nullptr, hash)) return ps;
    ++g_binds;
    bindGlassInputs(ctx);
    return replacement;
}

uint64_t cockpitGlassReplacedHash(ID3D11PixelShader* actual) { return actual && actual == g_ps ? kReplacedPsHash : 0; }

bool cockpitGlassOverlayBegin(ID3D11DeviceContext* ctx) {
    const Overlay which = overlayFor(bindingShaderHash(BindSlot::Ps));   // the game's own pixel shader: the shadow records it, never the replacement
    if (which == Overlay::None) return false;
    if (!ensureDevice(ctx)) return false;
    ID3D11PixelShader* ps = overlayShaderFor(ctx, which);
    ID3D11BlendState* blend = ps ? overlayBlend(ctx) : nullptr;
    if (!ps || !blend) return false;
    ID3D11ShaderResourceView* srv = streakSrv(ctx);
    ID3D11Buffer* level = levelBuffer(ctx);
    bool saved = false;
    const bool ran = guarded("cockpitGlass.overlay.begin", [&] {
        ctx->PSGetShader(&g_savedPs, nullptr, nullptr);
        ctx->OMGetBlendState(&g_savedBlend, g_savedFactor, &g_savedMask);
        ctx->PSGetShaderResources(kStreakSlot, 1, &g_savedT12);
        ctx->PSGetConstantBuffers(kLevelSlot, 1, &g_savedB13);
        saved = true;
        const float one[4] = {1, 1, 1, 1};
        vScreenPSSetShaderRaw(ctx, ps, nullptr, 0);
        vScreenOMSetBlendStateRaw(ctx, blend, one, 0xFFFFFFFFu);
        vScreenPSSetShaderResourcesRaw(ctx, kStreakSlot, 1, &srv);
        vScreenPSSetConstantBuffersRaw(ctx, kLevelSlot, 1, &level);
    });
    if (!ran) {
        if (saved) cockpitGlassOverlayEnd(ctx);
        return false;
    }
    ++g_overlays;
    return true;
}

void cockpitGlassOverlayEnd(ID3D11DeviceContext* ctx) {
    guarded("cockpitGlass.overlay.end", [&] {
        vScreenPSSetShaderRaw(ctx, g_savedPs, nullptr, 0);
        vScreenOMSetBlendStateRaw(ctx, g_savedBlend, g_savedFactor, g_savedMask);
        vScreenPSSetShaderResourcesRaw(ctx, kStreakSlot, 1, &g_savedT12);
        vScreenPSSetConstantBuffersRaw(ctx, kLevelSlot, 1, &g_savedB13);
    });
    releaseGuarded(g_savedPs, "cockpitGlass.overlay.ps.release");
    releaseGuarded(g_savedBlend, "cockpitGlass.overlay.blend.release");
    releaseGuarded(g_savedT12, "cockpitGlass.overlay.t12.release");
    releaseGuarded(g_savedB13, "cockpitGlass.overlay.b13.release");
}

void cockpitGlassFrameBoundary(ID3D11DeviceContext* ctx) {
    Config& cfg = Config::get();
    const uint32_t generation = cfg.generation();   // read before the value (config.h): a swap between costs one more read, never a stale one
    if (!g_configRead || generation != g_configGeneration) {
        g_configRead = true;
        g_configGeneration = generation;
        const std::string v = cfg.getString("visual.cockpit_glass", "odyssey");
        bool recognised = true;
        const bool on = parseLook(v.c_str(), &recognised) == Look::Legacy;
        if (!recognised && v != g_unrecognisedSaid) {
            g_unrecognisedSaid = v;
            Log::get().note("cockpit glass: visual.cockpit_glass = \"%s\" is not odyssey or legacy; it is read as odyssey (the game as it ships)", v.c_str());
        }
        // The dirt level (visual.cockpit_glass_dirt), read whatever the look is: it takes effect at the next bind, and is said when it moves while `legacy` is on.
        const int dirtPercent = cfg.getIntInRange("visual.cockpit_glass_dirt", kDirtPercentDefault, kDirtPercentMin, kDirtPercentMax);
        if (dirtPercent != g_dirtPercent) {
            g_dirtPercent = dirtPercent;
            if (on && g_on)
                Log::get().note("cockpit glass: dirt level %d%% (visual.cockpit_glass_dirt), from the next canopy bind", dirtPercent);
        }
        if (on != g_on) {
            const ULONGLONG now = GetTickCount64();
            if (on) {
                resetWindow(now);
                if (g_state == Replacement::Refused) g_state = Replacement::NotTried;   // a new try for a new switch-on
                startStreaks();
                Log::get().note("cockpit glass: legacy on (visual.cockpit_glass = legacy): the game's intact quality-3 canopy pixel shader %016llX is "
                                "replaced at bind time, and the ice and condensation draws get the legacy dirt drawn over them, at a dirt level of %d%% "
                                "(visual.cockpit_glass_dirt); a window line every 30 s "
                                "counts the binds, and binds=0 with canopy-seen above 0 means the replacement did not take",
                                static_cast<unsigned long long>(kReplacedPsHash), g_dirtPercent);
            } else {
                logWindow(now);
                if (ctx) restoreGlassInputs(ctx);
                Log::get().note("cockpit glass: legacy off (visual.cockpit_glass = %s): the canopy is drawn by the game's own shader from its next bind",
                                v.c_str());
            }
            g_on = on;
            detail::g_cockpitGlassLegacy.store(on, std::memory_order_relaxed);
        }
    }
    if (!g_on) return;
    if (ctx) restoreGlassInputs(ctx);   // the frame is over: whatever was at t12 and b13 is back
    const ULONGLONG now = GetTickCount64();
    if (windowDue(now, g_windowStart, kWindowMs)) {
        logWindow(now);
        resetWindow(now);
    }
}

}  // namespace edvr

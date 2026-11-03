// visual.cockpit_glass: cockpit_glass.h says what and why; the decisions are cockpit_glass_core.h's.
#include "cockpit_glass.h"

#include "cockpit_glass_core.h"
#include "shader_swap.h"
#include "temporal_shader_bytecode.h"   // kCockpitGlassLegacyPsBytecode

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"

#include <windows.h>

#include <string>

namespace edvr {

namespace detail {
std::atomic<bool> g_cockpitGlassLegacy{false};
}  // namespace detail

namespace {

using namespace cockpit_glass;

constexpr ULONGLONG kWindowMs = 30000;

bool g_configRead = false;
uint32_t g_configGeneration = 0;
bool g_on = false;
std::string g_unrecognisedSaid;   // the last unrecognised value the log has named, so a typo is said once and not every config change

// The replacement is kept for the process: a raw pointer on purpose, never released at exit (a static destructor that releases a D3D object
// during shutdown is the kind of thing the exit-crash arcs were about). It is released only when the device it was made on is replaced.
ID3D11PixelShader* g_ps = nullptr;
void* g_psDevice = nullptr;
Replacement g_state = Replacement::NotTried;

ULONGLONG g_windowStart = 0;
uint32_t g_binds = 0;   // canopy binds handed the replacement, this window
uint32_t g_seen = 0;    // canopy binds the game made, this window

// The device a context belongs to, as a plain pointer for comparison (the reference taken is released at once).
void* deviceOf(ID3D11DeviceContext* ctx) {
    ID3D11Device* dev = nullptr;
    if (!guarded("cockpitGlass.device", [&] { ctx->GetDevice(&dev); }) || !dev) return nullptr;
    void* key = dev;
    guarded("cockpitGlass.device.release", [&] { dev->Release(); });
    return key;
}

void dropReplacement() {
    ID3D11PixelShader* old = g_ps;
    g_ps = nullptr;
    g_psDevice = nullptr;
    if (old) guarded("cockpitGlass.ps.release", [&] { old->Release(); });
}

// The replacement for this context's device, made the first time it is wanted. Null means draw stock; a refusal stands until the key is turned off and on.
ID3D11PixelShader* replacementFor(ID3D11DeviceContext* ctx) {
    if (g_state == Replacement::Refused) return nullptr;
    void* dev = deviceOf(ctx);
    if (g_ps && dev == g_psDevice) return g_ps;
    if (g_ps) {
        Log::get().note("cockpit glass: the device changed; the replacement pixel shader is made again");
        dropReplacement();
    }
    if (!dev) return nullptr;
    g_ps = shaderSwapCreatePs(ctx, kCockpitGlassLegacyPsBytecode, sizeof(kCockpitGlassLegacyPsBytecode), "cockpit_glass_legacy_ps", "cockpit glass");
    if (!g_ps) {
        g_state = Replacement::Refused;
        Log::get().note("cockpit glass: replacement pixel shader refused (the build's bytecode did not create); the canopy is drawn stock");
        return nullptr;
    }
    g_psDevice = dev;
    g_state = Replacement::Ready;
    Log::get().note("cockpit glass: replacement pixel shader ready (%zu bytes of build bytecode); it takes the place of the game's intact quality-3 "
                    "canopy shader %016llX, linked with the canopy vertex shader %016llX",
                    sizeof(kCockpitGlassLegacyPsBytecode), static_cast<unsigned long long>(kReplacedPsHash),
                    static_cast<unsigned long long>(kCanopyVsHash));
    return g_ps;
}

void resetWindow(ULONGLONG now) {
    g_windowStart = now;
    g_binds = 0;
    g_seen = 0;
}

void logWindow(ULONGLONG now) {
    char line[200];
    formatWindow(line, sizeof line, (now - g_windowStart) / 1000.0, g_binds, g_seen, replacementName(g_state));
    Log::get().note("%s", line);
}

}  // namespace

ID3D11PixelShader* cockpitGlassPsFor(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, uint64_t hash) {
    if (hash != kReplacedPsHash) return ps;   // every other bind: this compare and out
    ++g_seen;
    ID3D11PixelShader* replacement = replacementFor(ctx);
    if (!swapWanted(Look::Legacy, replacement != nullptr, hash)) return ps;
    ++g_binds;
    return replacement;
}

void cockpitGlassFrameBoundary() {
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
        if (on != g_on) {
            const ULONGLONG now = GetTickCount64();
            if (on) {
                resetWindow(now);
                if (g_state == Replacement::Refused) g_state = Replacement::NotTried;   // a new try for a new switch-on
                Log::get().note("cockpit glass: legacy on (visual.cockpit_glass = legacy): the game's intact quality-3 canopy pixel shader %016llX is "
                                "replaced at bind time; a window line every 30 s counts the binds, and binds=0 with canopy-seen above 0 means the "
                                "replacement did not take",
                                static_cast<unsigned long long>(kReplacedPsHash));
            } else {
                logWindow(now);
                Log::get().note("cockpit glass: legacy off (visual.cockpit_glass = %s): the canopy is drawn by the game's own shader from its next bind",
                                v.c_str());
            }
            g_on = on;
            detail::g_cockpitGlassLegacy.store(on, std::memory_order_relaxed);
        }
    }
    if (!g_on) return;
    const ULONGLONG now = GetTickCount64();
    if (windowDue(now, g_windowStart, kWindowMs)) {
        logWindow(now);
        resetWindow(now);
    }
}

}  // namespace edvr

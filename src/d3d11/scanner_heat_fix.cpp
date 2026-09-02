#include "scanner_heat_fix.h"

#include <cstdio>    // _snprintf_s: HEAT_GAIN's macro value for the compile

#include <windows.h>

#include <cstdint>
#include <string>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "exposure_fix.h"   // lookupShaderHash: the shared shader registry
#include "shader_swap.h"

namespace edvr {
namespace {

// The filter's two pixel shaders (2026-09-01), measured from the census's
// ph= column in the fumaroles capture and confirmed absent from every
// unfiltered and normal-space capture. Only the fill is ever swapped --
// the marker draws a different, simpler shape (bio/geo dots) from a
// different vertex signature and was never the one failing its own
// discards, so it is recognised here only to keep it counted.
constexpr uint64_t kHeatFill   = 0x3B47A4BCE1891CC8ULL;  // vs A1E566372990C88B
constexpr uint64_t kHeatMarker = 0x5FC9FC1E3B008DF1ULL;  // vs 7C5DA553

// EDVR's transcription of the fill's disassembly (game ps
// 3B47A4BCE1891CC8, dumped via fxc /dumpbin and cross-checked instruction
// for instruction). Register semantics, swizzles and the saturate
// placements reproduce the DXBC exactly; the #if ladders are the only
// thing this fix adds. Desk-compiled by tools/compile_variants.py before
// it ships -- the game is never the compiler's first audience.
constexpr char kFillHlsl[] = R"HLSL(
// EDVR's transcription of the DSS signal-filter fill shader (game ps 3B47A4BCE1891CC8).
cbuffer CB1 : register(b1) { float4 c[333]; }
SamplerState s0 : register(s0);
SamplerState s1 : register(s1);
Texture2D t0 : register(t0);   // the eye's linear scene depth, metres, 1e17 = sky
Texture2D t1 : register(t1);   // altitude lookup A (density)
Texture2D t2 : register(t2);   // altitude lookup B (heat)

struct PSIn {
    float4 tc2  : TEXCOORD2;    // xyz: vertex world position; w: dot(normal, c[151].xyz)
    float  tc11 : TEXCOORD11;   // the fragment's own linear depth (clip w)
    float4 pos  : SV_POSITION;
};

float4 main(PSIn i) : SV_TARGET {
    float3 p    = i.tc2.xyz + c[275].xyz - c[142].yzw;
    float  r    = length(p);
    float  rIn  = c[137].x;
    float  rOut = c[137].y;
    float  alt  = r - rIn;
    float  tRaw = alt / (rOut - rIn);
    float  t    = saturate(tRaw);
    float  dens = t1.Sample(s1, float2(t, 0.0)).x;
    dens = saturate(dens * 200.0);
    dens = dens * dens * (3.0 - 2.0 * dens);

#if MODE_UV
    float w, h; t0.GetDimensions(w, h);
    float2 uv = i.pos.xy / float2(w, h);
#else
    float2 uv = i.pos.xy * c[332].zw;
#endif
    float scene = t0.Sample(s0, uv).x;

    bool depthFail  = (scene - i.tc11) < 0.0;
    bool radiusFail = ((rOut - r) < 0.0) || (alt < 0.0);
    bool gateFail   = !(c[149].w < i.tc2.w);

#if PROBE_WHY
    float3 why = scene < 10.0 ? float3(1, 0, 1)
               : depthFail    ? float3(1, 0, 0)
               : radiusFail   ? float3(0, 1, 0)
               : gateFail     ? float3(0, 0, 1)
                              : float3(1, 1, 1);
    return float4(why * 0.3, 1.0);
#elif PROBE_DEPTH
    float g = saturate(log2(max(scene, 1.0)) / 60.0);
    return float4(g.xxx * 0.3, 1.0);
#endif

#if !MODE_NODEPTH
    if (depthFail) discard;
#endif
#if !MODE_NORADIUS
    if (radiusFail) discard;
#endif
#if !MODE_NOGATE
    if (gateFail) discard;
#endif

    float heat = t2.Sample(s1, float2(t, 0.0)).x;
    heat = dens * heat * c[150].y;
    float f = saturate((i.tc2.w - c[149].w) * (1.0 / c[150].x));
    float k = (c[150].z - (f * f * (3.0 - 2.0 * f))) + 1.0;
    float2 col = float2(heat, 1.0) * k;

    float  d2   = dens * c[150].y;
    float3 base = d2 * float3(0.0195, 0.1375, 0.09);
    float  e    = max((col.x - 0.131563) * 20.5, 0.0);
    float3 alt2 = float3(0.1315, 0.431, 0.5) - d2 * float3(0.0195, 0.1375, 0.09);
    float3 rgb  = col.y * (e * alt2 + base);

    float fade = saturate(min((tRaw - 0.95) * -16.0 + 1.0, tRaw * 16.0 + 0.2));
    rgb *= fade;
#if MODE_GAIN
    rgb *= HEAT_GAIN;
#else
    rgb *= c[90].y;
#endif
    return float4(rgb * 65.0, 1.0);
}
)HLSL";

// advanced.scanner_heat_mode: which of the fill's own three discards the
// compiled shader skips. One word, not a bitset -- the field is narrowing
// down a single failing term, not composing an arbitrary combination, and
// `all` exists precisely so the composed case doesn't need its own name.
enum class Mode : uint32_t {
    kStock, kNoDepth, kNoRadius, kNoGate, kUv, kGain, kAll
};

// advanced.scanner_heat_probe: a diagnostic paint that overrides the mode
// above while set. Same shape as advanced.resolve_probe's shader modes --
// the replacement can answer a question the picture alone cannot.
enum class Probe : uint32_t { kOff, kWhy, kDepth };

Mode parseMode(const std::string& w) {
    if (w == "stock")    return Mode::kStock;
    if (w == "nodepth")  return Mode::kNoDepth;
    if (w == "noradius") return Mode::kNoRadius;
    if (w == "nogate")   return Mode::kNoGate;
    if (w == "uv")       return Mode::kUv;
    if (w == "gain")     return Mode::kGain;
    // "all" and anything unrecognised: the same two-choice shape
    // advanced.cull_guard_submit uses, read as the default rather than
    // refused, because a stray edit should still draw something sensible.
    return Mode::kAll;
}

Probe parseProbe(const std::string& w) {
    if (w == "why")   return Probe::kWhy;
    if (w == "depth") return Probe::kDepth;
    return Probe::kOff;
}

const char* modeName(Mode m) {
    switch (m) {
        case Mode::kStock:    return "stock";
        case Mode::kNoDepth:  return "nodepth";
        case Mode::kNoRadius: return "noradius";
        case Mode::kNoGate:   return "nogate";
        case Mode::kUv:       return "uv";
        case Mode::kGain:     return "gain";
        case Mode::kAll:      default: return "all";
    }
}

const char* probeName(Probe p) {
    switch (p) {
        case Probe::kWhy:   return "why";
        case Probe::kDepth: return "depth";
        case Probe::kOff:   default: return "off";
    }
}

// The startup line's user-facing half: what advanced.scanner_heat_mode
// actually changes about the fill, in the same words a player would read
// in the log without needing the source. Takes the gain value too, since
// modes gain/all bake it into the sentence rather than making the reader
// cross-reference a second key.
std::string modeNeutralizes(Mode m, float gain) {
    char gbuf[32];
    _snprintf_s(gbuf, sizeof(gbuf), _TRUNCATE, "%.4f", gain);
    switch (m) {
        case Mode::kStock:
            return "nothing changed -- it should still look exactly like "
                   "the game";
        case Mode::kNoDepth:
            return "its manual depth test skipped";
        case Mode::kNoRadius:
            return "its radius window skipped";
        case Mode::kNoGate:
            return "its hemisphere gate skipped (the gated maths still "
                   "runs)";
        case Mode::kUv:
            return "its depth sample's uv taken from the depth texture's "
                   "own size instead of the game's constant";
        case Mode::kGain:
            return std::string("its brightness constant replaced by ") +
                   gbuf;
        case Mode::kAll:
        default:
            return std::string(
                       "its manual depth test, radius window and "
                       "hemisphere gate all skipped, its depth sample's uv "
                       "taken from the depth texture's own size, and its "
                       "brightness constant replaced by ") +
                   gbuf;
    }
}

FaultBudget g_budget("scannerHeat", 8);

bool     g_on = false;
Mode     g_mode = Mode::kAll;      // matches edvr.ini's shipped default
Probe    g_probe = Probe::kOff;
float    g_gain = 0.003f;
uint32_t g_passes = 0;

// Which shader the most recent scannerHeatOnDraw matched: true for the
// fill, false for the marker. Set by the recognizer, read once by
// scannerHeatBegin and again by scannerHeatNoteApplied, cleared by
// scannerHeatEnd -- fss_panel.cpp's shape for handing a two-way match from
// its recognizer to the calls that act on it.
bool g_matchedFill = false;

// The compiled fill shader and the flag that a compile was already tried
// for the current mode/probe/gain -- so a failed compile is not retried
// every single draw. A config change that touches any of the three drops
// this via releaseShader(), and the next matched fill draw builds fresh.
ID3D11PixelShader* g_fillPs = nullptr;
bool                g_fillTried = false;
bool                g_engagedNoted = false;

ID3D11PixelShader* g_savedPs = nullptr;
bool                g_engaged = false;

uint32_t g_fillSwapped = 0;   // fill draws actually drawn through g_fillPs
uint32_t g_markerSeen  = 0;   // marker draws recognised, always left stock

void releaseShader() {
    if (g_fillPs) {
        g_fillPs->Release();
        g_fillPs = nullptr;
    }
    g_fillTried = false;
    // The variant changed, so the next successful bind is a NEW shader as
    // far as the log is concerned and earns its own "engaged" line.
    g_engagedNoted = false;
}

// Compile the fill's replacement with the current mode/probe/gain baked in
// as macros. Every macro is always defined, as "0" or "1" (HEAT_GAIN as a
// float literal), so none of the HLSL's #if ladders ever see an undefined
// name regardless of which word is active.
void ensureCompiled(ID3D11DeviceContext* ctx) {
    if (g_fillTried) return;
    g_fillTried = true;

    const bool noDepth  = g_mode == Mode::kNoDepth  || g_mode == Mode::kAll;
    const bool noRadius = g_mode == Mode::kNoRadius || g_mode == Mode::kAll;
    const bool noGate   = g_mode == Mode::kNoGate   || g_mode == Mode::kAll;
    const bool uvMode   = g_mode == Mode::kUv       || g_mode == Mode::kAll;
    const bool gainMode = g_mode == Mode::kGain     || g_mode == Mode::kAll;

    char gainStr[32];
    _snprintf_s(gainStr, sizeof(gainStr), _TRUNCATE, "%.6f", g_gain);

    const SwapMacro macros[] = {
        {"MODE_NODEPTH",  noDepth  ? "1" : "0"},
        {"MODE_NORADIUS", noRadius ? "1" : "0"},
        {"MODE_NOGATE",   noGate   ? "1" : "0"},
        {"MODE_UV",       uvMode   ? "1" : "0"},
        {"MODE_GAIN",     gainMode ? "1" : "0"},
        {"HEAT_GAIN",     gainStr},
        {"PROBE_WHY",     g_probe == Probe::kWhy   ? "1" : "0"},
        {"PROBE_DEPTH",   g_probe == Probe::kDepth ? "1" : "0"},
        {nullptr, nullptr},
    };
    g_fillPs = shaderSwapCompilePs(ctx, kFillHlsl, sizeof(kFillHlsl) - 1,
                                   "main", "scanner_heat_fill_ps", macros,
                                   "scanner heat");
    if (!g_fillPs) {
        // shader_swap.h already logged which step failed and why; this
        // just says what that means here.
        Log::get().note(
            "scanner heat fix: the fill's replacement shader did not "
            "compile, so the fill draws stock this session.");
    }
}

}  // namespace

void scannerHeatConfigure(Config& cfg) {
    const std::string v = cfg.getString("fix.scanner_heat", "off");
    const bool want = (v == "on" || v == "1");

    const std::string modeWord =
        cfg.getString("advanced.scanner_heat_mode", "all");
    const Mode mode = parseMode(modeWord);

    const std::string probeWord =
        cfg.getString("advanced.scanner_heat_probe", "off");
    const Probe probe = parseProbe(probeWord);

    const float gain = cfg.getFloat("advanced.scanner_heat_gain", 0.003f);

    // 0..6: zero is the shipped default -- the overlay is expected to
    // reach its own stock strength once the failing discard is neutralized,
    // and re-issuing it further is a knob for if the field finds that
    // isn't enough, not a number anybody should need going in.
    const uint32_t passes = static_cast<uint32_t>(
        cfg.getIntInRange("advanced.scanner_heat_passes", 0, 0, 6));

    // A session that STARTS with the fix off says so, once -- the same
    // lesson resolve_bind_fix learned the same day this fix was born: it
    // defaults off, so without this line a stock session's bundle is
    // silent about a state the next report will need.
    static bool announced = false;
    const bool specChanged =
        (mode != g_mode) || (probe != g_probe) || (gain != g_gain);
    const bool changed =
        (want != g_on) || (want && (passes != g_passes || specChanged));
    g_on = want;
    g_passes = passes;
    if (specChanged) {
        g_mode = mode;
        g_probe = probe;
        g_gain = gain;
        // All three are baked into the compiled shader as macros, so any
        // of them changing invalidates it; the next matched fill draw
        // compiles the new variant fresh.
        releaseShader();
    }
    if (!changed) {
        if (!announced && !g_on) {
            Log::get().note("scanner heat fix off: the filter overlay is "
                            "left at the game's own strength.");
        }
        announced = true;
        return;
    }
    announced = true;

    if (g_on) {
        Log::get().note(
            "scanner heat fix ON: the DSS signal filter's fill is drawn "
            "through EDVR's own copy of its shader with %s; probe %s; "
            "extra passes %u.",
            modeNeutralizes(g_mode, g_gain).c_str(), probeName(g_probe),
            g_passes);
    } else {
        Log::get().note("scanner heat fix off: the filter overlay is left "
                        "at the game's own strength.");
        releaseShader();
    }
}

bool scannerHeatWants() { return g_on; }

bool scannerHeatOnDraw(ID3D11DeviceContext* ctx, char kind) {
    if (!g_on || !ctx || kind != 'X') return false;
    bool matchFill = false, matchMarker = false;
    guardedBudget(g_budget, [&] {
        ID3D11PixelShader* ps = nullptr;
        ctx->PSGetShader(&ps, nullptr, nullptr);
        if (ps) {
            const uint64_t h = lookupShaderHash(ps);
            matchFill = (h == kHeatFill);
            matchMarker = (h == kHeatMarker);
            ps->Release();
        }
    });
    if (matchFill) g_matchedFill = true;
    else if (matchMarker) g_matchedFill = false;
    return matchFill || matchMarker;
}

void scannerHeatBegin(ID3D11DeviceContext* ctx) {
    if (!ctx || !g_matchedFill) return;   // the marker draws stock; nothing to do
    guardedBudget(g_budget, [&] {
        ensureCompiled(ctx);
        if (!g_fillPs) return;   // shader_swap already said why; draw stock
        ctx->PSGetShader(&g_savedPs, nullptr, nullptr);
        ctx->PSSetShader(g_fillPs, nullptr, 0);
        g_engaged = true;
        if (!g_engagedNoted) {
            g_engagedNoted = true;
            Log::get().note(
                "scanner heat fix: engaged -- compiled %s for the fill; "
                "the markers stay the game's own. Counting silently from "
                "here.",
                g_probe != Probe::kOff ? probeName(g_probe)
                                        : modeName(g_mode));
        }
    });
}

void scannerHeatEnd(ID3D11DeviceContext* ctx) {
    g_matchedFill = false;
    if (!g_engaged || !ctx) return;
    g_engaged = false;
    guardedBudget(g_budget, [&] {
        ctx->PSSetShader(g_savedPs, nullptr, 0);
        if (g_savedPs) {
            g_savedPs->Release();
            g_savedPs = nullptr;
        }
    });
}

uint32_t scannerHeatExtraPasses() { return g_passes; }

// Called once per original draw the kScannerHeat verdict carried, after
// the passes loop and before scannerHeatEnd -- so g_matchedFill and
// g_engaged still describe the draw being counted here. A fill draw only
// counts as "swapped" when the bind actually happened; a marker draw
// always counts as "seen", since it never engages anything and this is
// the only place its presence is recorded at all.
void scannerHeatNoteApplied() {
    if (g_matchedFill) {
        if (g_engaged) ++g_fillSwapped;
    } else {
        ++g_markerSeen;
    }
}

void scannerHeatShutdown() {
    if (g_fillSwapped || g_markerSeen) {
        Log::get().note(
            "scanner heat fix: %u fill draw(s) swapped this session (%u "
            "extra pass(es) each), %u marker draw(s) seen. A zero count on "
            "both means no signal filter was up while the fix was on.",
            g_fillSwapped, g_passes, g_markerSeen);
    }
    g_fillSwapped = 0;
    g_markerSeen = 0;
    g_matchedFill = false;
    g_engaged = false;
    releaseShader();
    if (g_savedPs) {
        g_savedPs->Release();
        g_savedPs = nullptr;
    }
}

}  // namespace edvr

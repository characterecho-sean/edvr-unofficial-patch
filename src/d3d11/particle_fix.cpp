#include "temporal_shader_bytecode.h"
#include "particle_fix.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../common/config.h"
#include "../common/runtime_profile.h"
#include "../common/guard.h"
#include "../common/log.h"
#include <string>

#include "../common/timing.h"
#include "binding_shadow.h"   // bindingShaderHash: the bound vertex shader's hash, set with the shader
#include "exposure_fix.h"   // lookupShaderHash
// flare_vs.h is compiled by the build shader generator.
// particle_vs.h is compiled by the build shader generator.
#include "shader_swap.h"

namespace edvr {

// particleSteady reads this from the header with no call: asked per
// draw, and the build has no /GL to fold a cross-TU getter. ParticleMode
// itself is declared in the header, so the inline function there can
// name its kSteady value.
namespace detail {
ParticleMode g_particleMode = ParticleMode::kStock;
// Published for the draw path's inline first tests (particle_fix.h); the
// file-local names below are references to these.
bool g_particleHideStars = false;
}  // namespace detail

namespace {
using Mode = detail::ParticleMode;

// The particle billboard vertex shader, by content hash. Found 2026-08-23
// by skipping shaders one at a time at a geyser field (census_skip's
// vs:HASH term) after both offline methods failed: the differential census
// had nothing to subtract, because a geyser field is never quiet in every
// frame, and every size-level signature collided with terrain and props.
constexpr uint64_t kPlumeVs = 0xEB787F983BC1F5A3ull;

// The SOLAR FLARE billboard, found the same way 2026-08-29: prominences
// erupting off star surfaces, riding the head. Same construction, same
// constants, different shader.
//
// WHY A TABLE AND NOT MORE HASHES. The family has at least seven members
// and they do NOT share a signature: the plume takes thirteen inputs and
// writes twelve outputs, the flare takes ten and writes five. A shader
// whose signature does not match the input layout and the pixel shader
// behind it cannot be substituted at all, so each signature group needs
// its own transcription and its own entry here.
constexpr uint64_t kFlareVs = 0x6041FD2D3D0164E1ull;

// ONE HASH PER TRANSCRIPTION. NEVER A SECOND ONE ON A LIKENESS.
//
// 0.12.3 shipped 9AEC596A2B036EA6 alongside kFlareVs as a "twin", on the
// claim that its instruction body was byte-identical and it would get the
// flare's replacement free. It is not a twin, and the claim came from a
// comparison that could not have detected the difference: it read only
// indented lines, so it skipped every dcl_output, and semantic INDICES do
// not appear in the instruction stream at all -- they live in the
// signature block, which no instruction diff reads.
//
// The two shaders write the same values to DIFFERENT output registers
// (max o2.y against max o1.y, utof o1.w against utof o0.w), declare
// different masks, and carry different TEXCOORD indices: the flare emits
// TEXCOORD 2/4/7/9 and 9AEC596A2B036EA6 emits 3/6/8/9. Substituting the
// flare's replacement handed its pixel shader the wrong registers.
//
// What it draws is the WITCHSPACE STARFIELD, and in 0.12.3 that starfield
// disappeared. Field-confirmed 2026-08-30 by census_skip on this hash.
//
// So: a hash earns a replacement only by having its own disassembly read
// and its own transcription written. Likeness is not evidence, and the
// cheap comparison that suggests it is the exact trap this paragraph is
// here to stop.

// The witchspace starfield, by the hash the twin mistake identified. It is
// NOT replaced -- it has no transcription -- but it is offered as something
// a player can turn off, because a user asked for the empty tunnel 0.12.3
// produced by accident and it is a reasonable thing to want.
//
// Skipping the draw is not how 0.12.3 removed it. That fed the draw a
// mismatched replacement and let the pixel shader read whatever the wrong
// registers held, which happened to come out empty. Not drawing it at all
// is the version that means what it says.
//
// Scope, measured: this hash appears ZERO times across three censuses at a
// star where the flare's own hash appears 24 times, so it is not the
// general background starfield. That is evidence it is specific to the
// jump tunnel, not proof it draws nowhere else.
constexpr uint64_t kWitchspaceStarsVs = 0x9AEC596A2B036EA6ull;
// Bound to the published flag witchspaceStarsHidden() reads (particle_fix.h).
bool& g_hideWitchspaceStars = detail::g_particleHideStars;
struct BillboardVariant {
    uint64_t    hash;
    const void* bytecode;
    size_t      bytecodeLen;
    const char* name;      // names the compile in the log
};

constexpr int kVariantCount = 2;
const BillboardVariant kVariants[kVariantCount] = {
    {kPlumeVs, kParticleWorldBytecode, sizeof(kParticleWorldBytecode),
     "particle_vs"},
    {kFlareVs, kFlareWorldBytecode, sizeof(kFlareWorldBytecode),
     "flare_vs"},
};
// The draw path's inline prefilter (particleOnDrawMayMatch, particle_fix.h)
// compares against its own copy of these hashes; a variant added here and
// not there would be silently never offered, so the two lists must agree.
static_assert(kVariantCount == 2 && detail::kParticleVariantVs[0] == kPlumeVs &&
                  detail::kParticleVariantVs[1] == kFlareVs,
              "particle_fix.h's kParticleVariantVs must list kVariants' hashes in order");

const char* variantLabel(int v) {
    return (v == 1) ? "solar flare" : "smoke plume";
}

// cb1 is 280 registers. The basis vectors live at 278 and 279 -- floats
// 1112..1119 -- and the neighbours are logged with them because "which
// register is the up" is exactly what this exists to measure rather than
// assume.
constexpr uint32_t kFirstReg = 276;
constexpr uint32_t kRegs = 4;

// The fix. steady replaces the basis's up vector with WORLD UP for the
// matched draw, turning a fully camera-locked billboard into a
// cylindrical one: still facing you, but with the smoke column held
// vertical in the world. Measured 2026-08-23: with the view level the
// game's own up reads (0.056, 0.981, 0.185) -- already world up to within
// the view's pitch -- so at a level view this substitution is a no-op,
// and everything it changes is roll and pitch coupling.
//
// Why a constant substitution is safe here when it was not for the sun
// glare: that buffer's rows were a VIEW MATRIX, and the elements' screen
// positions flowed through them, so every rewrite displaced them per eye.
// These are pure direction vectors used for nothing but the billboard
// basis -- position arrives separately through cb0[9..11] -- and cb1[277]
// holds a camera right the shader never even reads. [278] is the whole of
// the change.
// Which billboard variant is this draw, or -1 for none? By shader hash and
// nothing else: the geyser hunt established that kind, count, stride and
// every sampler size are shared with the terrain and prop pipelines.
// The bound vertex shader's hash: the binding shadow's, set with the
// shader (2026-09-09; a VSGetShader per draw was a millisecond a frame
// here), and the Get only when the shadow has seen no set.
uint64_t boundVsHashFast(ID3D11DeviceContext* ctx) {
    if (bindingGet(BindSlot::Vs)) {
        // A held zero is a shader the registry had not met at its set
        // (2026-09-09), and the context is asked instead.
        const uint64_t held = bindingShaderHash(BindSlot::Vs);
        if (held) return held;
    }
    ID3D11VertexShader* vs = nullptr;
    ctx->VSGetShader(&vs, nullptr, nullptr);
    if (!vs) return 0;
    const uint64_t h = lookupShaderHash(vs);
    vs->Release();
    return h;
}

// Armed-trace counterpart. It follows the exact consumed-input path above
// and reports whether the hash came from the binding shadow or the existing
// VSGetShader fallback; callers must not issue a second query for tracing.
uint64_t boundVsHashFastObserved(ID3D11DeviceContext* ctx,
                                WitchspaceStarsObservation& observation) {
    if (bindingGet(BindSlot::Vs)) {
        const uint64_t held = bindingShaderHash(BindSlot::Vs);
        if (held) {
            observation.hashKnown = true;
            observation.hashSource = WitchspaceStarsHashSource::kBindingShadow;
            observation.vsHash = held;
            return held;
        }
    }
    ID3D11VertexShader* vs = nullptr;
    ctx->VSGetShader(&vs, nullptr, nullptr);
    if (!vs) {
        observation.hashKnown = true;
        observation.hashSource = WitchspaceStarsHashSource::kFallbackNoShader;
        observation.vsHash = 0;
        return 0;
    }
    const uint64_t h = lookupShaderHash(vs);
    vs->Release();
    observation.hashKnown = true;
    observation.hashSource = WitchspaceStarsHashSource::kFallbackShaderLookup;
    observation.vsHash = h;
    return h;
}

int billboardVariantFor(ID3D11DeviceContext* ctx) {
    const uint64_t h = boundVsHashFast(ctx);
    if (!h) return -1;
    for (int i = 0; i < kVariantCount; ++i) {
        if (h == kVariants[i].hash) return i;
    }
    return -1;
}

// The shadow of the game's cb1, kept by the Map/Unmap tee, and our own
// buffer built from it. The contents cannot be read at the draw -- a
// dynamic buffer the GPU owns is write-only from here -- so the only way
// to substitute is to watch the game write it, which is the mechanism the
// panel distance fix has used since 0.3.
constexpr uint32_t kMaxShadow = 8192;
void*    g_target = nullptr;
uint8_t  g_shadow[kMaxShadow];
uint32_t g_shadowBytes = 0;
bool     g_shadowValid = false;
uint64_t g_applied = 0;
uint64_t g_appliedAtNote = 0;
uint64_t g_noteMs = 0;
bool     g_learnNoted = false;

// Why the quads do not face their own emitter. Elite renders in
// CAMERA-RELATIVE world space -- the shader's own near-fade takes
// dot(forward, position) with no camera term, which is only a depth if
// positions are already relative to the eye -- so cb0[9..11]'s
// translation column is the vector from the viewer to the plume, and
// normalising it is the direction the quads should face. Measured at a
// geyser field: two emitters at (312.1, 37.2, -294.3) and (43.1, -47.0,
// 19.5), each steady while the view moved around them.
// The per-emitter facing is OFF, and it is a dead end rather than a
// pending one. MEASURED 2026-08-23: cb0 here is 208 bytes bound at
// register 0 -- no ring, no offset -- and 208 bytes is the
// engine-standard CAMERA block this project decoded during the sun-glare
// arc. So cb0[9..11] is not an emitter's model matrix; it is that
// block's world-frame rows, whose w components the glare arc already
// convicted as an accumulator rather than a position (its "distance"
// grew linearly forever). Aiming down that vector pointed every quad
// somewhere meaningless: the field saw the smoke go edge-on in stacks.
//
// There is no per-emitter position in these constants, so there is no
// per-draw facing to be had from them. Exact facing is per-PARTICLE and
// lives in the vertex stream, which only a replacement shader can read
// -- the same ceiling the glare's constant substitution hit before it
// became a shader swap. The key that repeated the measurement
// (advanced.particle_face_emitter) and the emitter-buffer shadow it needed
// retired 2026-09-23.
uint64_t g_facingUsed = 0;
float    g_lastFacing[3] = {};

// THE SWAP. The constant substitution that came before this could remove
// the roll -- world up in place of the camera's -- but never the
// foreshortening, because facing the viewer is a per-PARTICLE direction
// and a constant is per-draw. So the draw gets a different vertex shader
// instead: a transcription of the game's own, with the basis rebuilt per
// vertex. See particle_vs.h.
//
// The buffer copy the substitution needed every draw -- 5376 bytes,
// mapped and filled -- is gone with it. What remains per draw is a shader
// swap and a 32-byte constant write.
// One compiled replacement per signature group, compiled on first sight of
// a draw that needs it. A group nobody draws costs nothing.
ID3D11VertexShader* g_ourVs[kVariantCount] = {};
bool                g_vsTried[kVariantCount] = {};
uint64_t            g_appliedBy[kVariantCount] = {};
// Which variant the draw currently being matched belongs to. Set by
// particleOnDraw, read by particleBegin, -1 between them.
int                 g_activeVariant = -1;
ID3D11VertexShader* g_savedVs = nullptr;
ID3D11Buffer*       g_ourCb = nullptr;     // b3: viewer position, world up
ID3D11Buffer*       g_savedCb3 = nullptr;
bool                g_engaged = false;

// The viewer's position in the space the particles are transformed into,
// solved from the game's own clip rows. For any projective transform the
// camera annihilates the x, y and w rows -- dot(row, cam) = -row.w -- so
// three rows and a 3x3 solve give it, exactly the identity the sun-glare
// arc used to find the camera behind the glare train.
float g_cam[3] = {};
bool  g_camOk = false;

void solveCamera() {
    g_camOk = false;
    if (!g_shadowValid || g_shadowBytes < 274 * 16) return;
    const float* f = reinterpret_cast<const float*>(g_shadow);
    const float* c0 = f + 270 * 4;   // the x contribution
    const float* c1 = f + 271 * 4;   // y
    const float* c2 = f + 272 * 4;   // z
    const float* t  = f + 273 * 4;   // translation
    // Rows of the 3x3, one per clip component that the camera kills.
    const float a[3][3] = {{c0[0], c1[0], c2[0]},
                           {c0[1], c1[1], c2[1]},
                           {c0[3], c1[3], c2[3]}};
    const float b[3] = {-t[0], -t[1], -t[3]};
    const float det =
        a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
        a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
        a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (fabsf(det) < 1e-12f) return;
    const float inv = 1.0f / det;
    g_cam[0] = inv * (b[0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                      a[0][1] * (b[1] * a[2][2] - a[1][2] * b[2]) +
                      a[0][2] * (b[1] * a[2][1] - a[1][1] * b[2]));
    g_cam[1] = inv * (a[0][0] * (b[1] * a[2][2] - a[1][2] * b[2]) -
                      b[0] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                      a[0][2] * (a[1][0] * b[2] - b[1] * a[2][0]));
    g_cam[2] = inv * (a[0][0] * (a[1][1] * b[2] - b[1] * a[2][1]) -
                      a[0][1] * (a[1][0] * b[2] - b[1] * a[2][0]) +
                      b[0] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]));
    g_camOk = true;
}

FaultBudget g_subBudget("particle.substitute", 5);

// Is the shadow shaped like the camera block this fix understands? Both
// basis vectors must be unit length; anything else is a buffer that is
// not what we think it is, and substituting into it would be writing a
// guess into the game's pipeline.
bool shapeOk(const float* f, uint32_t floats) {
    if (floats < 274 * 4) return false;
    if (floats < (kFirstReg + kRegs) * 4) return false;
    const float* up = f + 278 * 4;
    const float* fwd = f + 279 * 4;
    const float lu = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    const float lf = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    if (lu < 0.9f || lu > 1.1f) return false;
    if (lf < 0.9f || lf > 1.1f) return false;
    return true;
}

}  // namespace

uint64_t g_starsSkipped = 0;
uint64_t g_starsNoteMs = 0;

bool witchspaceStarsSkip(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                         uint32_t instances) {
    if (!g_hideWitchspaceStars || !ctx) return false;
    if (kind != 'X' && kind != 'N') return false;
    if (instances == 0 || count < 6) return false;
    const uint64_t h = boundVsHashFast(ctx);
    if (h != kWitchspaceStarsVs) return false;
    ++g_starsSkipped;
    const uint64_t now = nowMs();
    if (now - g_starsNoteMs >= 30000) {
        g_starsNoteMs = now;
        Log::get().note(
            "witchspace stars: OFF -- %llu draw(s) of vs %016llX withheld so "
            "far. The tunnel is empty by choice; set witchspace_stars = on "
            "to have them back.",
            static_cast<unsigned long long>(g_starsSkipped),
            static_cast<unsigned long long>(kWitchspaceStarsVs));
    }
    return true;
}

bool witchspaceStarsSkipTraced(ID3D11DeviceContext* ctx, char kind,
                               uint32_t count, uint32_t instances,
                               WitchspaceStarsObservation* output) {
    WitchspaceStarsObservation local{};
    WitchspaceStarsObservation& observation = output ? *output : local;
    observation = {};
    observation.hidden = g_hideWitchspaceStars;
    const uint64_t skippedBefore = g_starsSkipped;
    if (!observation.hidden) {
        observation.skippedDeltaKnown = true;
        observation.skippedDelta = 0;
        return false;
    }
    observation.contextKnown = true;
    observation.contextValid = ctx != nullptr;
    if (!ctx) {
        observation.skippedDeltaKnown = true;
        observation.skippedDelta = 0;
        return false;
    }
    observation.shapeReached = true;
    observation.shapeMatched = (kind == 'X' || kind == 'N') &&
                               instances != 0 && count >= 6;
    if (!observation.shapeMatched) {
        observation.skippedDeltaKnown = true;
        observation.skippedDelta = 0;
        return false;
    }
    const uint64_t h = boundVsHashFastObserved(ctx, observation);
    if (h != kWitchspaceStarsVs) {
        observation.skippedDeltaKnown = true;
        const uint64_t delta = g_starsSkipped - skippedBefore;
        observation.skippedDelta = delta <= 1 ? static_cast<uint32_t>(delta) : 2u;
        return false;
    }
    ++g_starsSkipped;
    const uint64_t now = nowMs();
    if (now - g_starsNoteMs >= 30000) {
        g_starsNoteMs = now;
        Log::get().note(
            "witchspace stars: OFF -- %llu draw(s) of vs %016llX withheld so "
            "far. The tunnel is empty by choice; set witchspace_stars = on "
            "to have them back.",
            static_cast<unsigned long long>(g_starsSkipped),
            static_cast<unsigned long long>(kWitchspaceStarsVs));
    }
    observation.skippedDeltaKnown = true;
    const uint64_t delta = g_starsSkipped - skippedBefore;
    observation.skippedDelta = delta <= 1 ? static_cast<uint32_t>(delta) : 2u;
    return true;
}

void* particleTarget() {
    return detail::g_particleMode == Mode::kSteady ? g_target : nullptr;
}

void particleCapture(const void* data, uint32_t bytes) {
    if (detail::g_particleMode != Mode::kSteady || !data || bytes < 64 || bytes > kMaxShadow) {
        g_shadowValid = false;
        return;
    }
    memcpy(g_shadow, data, bytes);
    g_shadowBytes = bytes;
    g_shadowValid = true;
}

bool particleOnDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                    uint32_t instances) {
    if (detail::g_particleMode != Mode::kSteady || !ctx) return false;
    if (kind != 'X' && kind != 'N') return false;
    if (instances == 0 || count < 6) return false;
    const int variant = billboardVariantFor(ctx);
    if (variant < 0) return false;
    g_activeVariant = variant;

    // Follow the buffer the game binds for these draws. Learning it here
    // rather than assuming means a build that moves the block simply never
    // matches, instead of substituting into the wrong buffer.
    ID3D11Buffer* cb = nullptr;
    ctx->VSGetConstantBuffers(1, 1, &cb);
    if (!cb) return false;
    if (cb != g_target) {
        g_target = cb;
        g_shadowValid = false;
        if (!g_learnNoted) {
            g_learnNoted = true;
            Log::get().note(
                "particle billboard: watching the plume shader's constants "
                "at slot 1. The next write the game makes there is what the "
                "substitution is built from; until then these draws go "
                "through untouched.");
        }
        cb->Release();
        return false;
    }
    cb->Release();

    if (!g_shadowValid) return false;
    return shapeOk(reinterpret_cast<const float*>(g_shadow), g_shadowBytes / 4);
}

void particleBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    if (!ctx || !g_shadowValid) return;
    const int variant = g_activeVariant;
    if (variant < 0 || variant >= kVariantCount) return;
    guardedBudget(g_subBudget, [&] {
        if (!g_vsTried[variant]) {
            g_vsTried[variant] = true;
            const BillboardVariant& d = kVariants[variant];
            g_ourVs[variant] = shaderSwapCreateVs(
                ctx, d.bytecode, d.bytecodeLen, d.name,
                "particle billboard");
            if (g_ourVs[variant]) {
                Log::get().note(
                    "particle billboard: replacement compiled for the %s "
                    "(vs %016llX). Each signature group needs its own -- "
                    "this family's shaders do not share one.",
                    variantLabel(variant),
                    static_cast<unsigned long long>(d.hash));
            }
        }
        if (!g_ourVs[variant]) return;   // compile failed: the game draws stock

        if (!g_ourCb) {
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (!dev) return;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 32;
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            dev->CreateBuffer(&bd, nullptr, &g_ourCb);
            dev->Release();
            if (!g_ourCb) return;
        }

        solveCamera();
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(g_ourCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) ||
            !m.pData) {
            return;
        }
        float* f = static_cast<float*>(m.pData);
        f[0] = g_cam[0];
        f[1] = g_cam[1];
        f[2] = g_cam[2];
        // The flag the shader reads: without a solved viewer position it
        // keeps the game's own camera-plane basis rather than aiming at
        // a point nobody measured.
        f[3] = g_camOk ? 1.0f : 0.0f;
        f[4] = 0.0f;
        f[5] = 1.0f;
        f[6] = 0.0f;
        f[7] = 0.0f;
        ctx->Unmap(g_ourCb, 0);

        ctx->VSGetShader(&g_savedVs, nullptr, nullptr);
        ctx->VSGetConstantBuffers(3, 1, &g_savedCb3);
        ID3D11Buffer* ours = g_ourCb;
        ctx->VSSetConstantBuffers(3, 1, &ours);
        ctx->VSSetShader(g_ourVs[variant], nullptr, 0);
        g_engaged = true;
        ++g_applied;
        ++g_appliedBy[variant];
        if (g_camOk) ++g_facingUsed;
        g_lastFacing[0] = g_cam[0];
        g_lastFacing[1] = g_cam[1];
        g_lastFacing[2] = g_cam[2];
    });
}

void particleEnd(ID3D11DeviceContext* ctx) {
    if (!g_engaged || !ctx) return;
    g_engaged = false;
    ctx->VSSetShader(g_savedVs, nullptr, 0);
    if (g_savedVs) {
        g_savedVs->Release();
        g_savedVs = nullptr;
    }
    ctx->VSSetConstantBuffers(3, 1, &g_savedCb3);
    if (g_savedCb3) {
        g_savedCb3->Release();
        g_savedCb3 = nullptr;
    }
    const uint64_t now = nowMs();
    if (now - g_noteMs >= 10000) {
        Log::get().note(
            "particle billboard: steady -- %llu draw(s) in the last ten "
            "seconds through the replacement shader (%llu smoke plume, "
            "%llu solar flare), %llu of them with a solved viewer at "
            "(%.1f %.1f %.1f). Each quad now faces the viewer instead of "
            "the view axis. A zero in one of the two is not a fault -- it "
            "means you were nowhere near that effect.",
            static_cast<unsigned long long>(g_applied - g_appliedAtNote),
            static_cast<unsigned long long>(g_appliedBy[0]),
            static_cast<unsigned long long>(g_appliedBy[1]),
            static_cast<unsigned long long>(g_facingUsed),
            g_lastFacing[0], g_lastFacing[1], g_lastFacing[2]);
        g_noteMs = now;
        g_appliedAtNote = g_applied;
        g_facingUsed = 0;
        for (int i = 0; i < kVariantCount; ++i) g_appliedBy[i] = 0;
    }
}

void particleConfigure(Config& cfg) {
    // The witchspace starfield switch. "on" is the game's own behaviour and
    // the default; "off" empties the jump tunnel, which a player asked for
    // after 0.12.3 did it by accident.
    const std::string ws = runtimeVrProfile() ?
        cfg.getString("fix.witchspace_stars", "on") : "on";
    const bool wasHidden = g_hideWitchspaceStars;
    if (ws == "off") {
        g_hideWitchspaceStars = true;
    } else {
        if (ws != "on") {
            Log::get().note("witchspace stars: that is not on or off; "
                            "leaving them on.");
        }
        g_hideWitchspaceStars = false;
    }
    if (g_hideWitchspaceStars != wasHidden) {
        g_starsSkipped = 0;
        g_starsNoteMs = 0;
        Log::get().note(
            g_hideWitchspaceStars
                ? "witchspace stars: OFF. The streaking starfield inside the "
                  "hyperspace tunnel is not drawn -- the draw is withheld, "
                  "nothing else about the jump changes."
                : "witchspace stars: ON. The hyperspace tunnel keeps its "
                  "starfield, which is the game's own behaviour.");
    }

    const Mode wasMode = detail::g_particleMode;
    const std::string m = runtimeVrProfile() ?
        cfg.getString("fix.particle_billboard", "steady") : "stock";
    if (m == "steady") {
        detail::g_particleMode = Mode::kSteady;
    } else {
        if (m != "stock") {
            Log::get().note("particle billboard: that is not stock or "
                            "steady; running stock.");
        }
        detail::g_particleMode = Mode::kStock;
    }
    if (detail::g_particleMode != wasMode) {
        if (detail::g_particleMode == Mode::kSteady) {
            g_learnNoted = false;
            Log::get().note(
                "particle billboard: STEADY -- smoke and steam quads are "
                "built against world up instead of the view's up, so they "
                "face you without rolling when you tilt your head or swing "
                "the camera. Read from the game's own shader: "
                "docs/particle-billboards.md.");
        } else {
            g_target = nullptr;
            g_shadowValid = false;
            Log::get().note("particle billboard: stock.");
        }
    }
}

bool particleSubstituteDrawInterestConfigured() noexcept {
    return detail::g_particleMode == Mode::kSteady;
}

std::size_t particleSubstituteDrawInterestFilters(
    draw_interest::ShaderFilter* out, std::size_t capacity) noexcept {
    if (!particleSubstituteDrawInterestConfigured()) return 0;
    constexpr uint64_t hashes[] = {detail::kParticleVariantVs[0],
                                   detail::kParticleVariantVs[1]};
    for (std::size_t i = 0; out && i < 2 && i < capacity; ++i) {
        out[i] = {draw_interest::InterestId::ParticleSubstitute,
                  draw_interest::HashFilter::Vertex, hashes[i], 0};
    }
    return 2;
}

bool witchspaceStarsDrawInterestConfigured() noexcept {
    return g_hideWitchspaceStars;
}

std::size_t witchspaceStarsDrawInterestFilters(
    draw_interest::ShaderFilter* out, std::size_t capacity) noexcept {
    if (!g_hideWitchspaceStars) return 0;
    if (out && capacity) {
        out[0] = {draw_interest::InterestId::WitchspaceStars,
                  draw_interest::HashFilter::Vertex, kWitchspaceStarsVs, 0};
    }
    return 1;
}

bool particleWantsDraws() {
    return detail::g_particleMode == Mode::kSteady;
}

void particleShutdown() {
    for (int i = 0; i < kVariantCount; ++i) {
        if (g_ourVs[i]) {
            g_ourVs[i]->Release();
            g_ourVs[i] = nullptr;
        }
        g_vsTried[i] = false;
        g_appliedBy[i] = 0;
    }
    g_activeVariant = -1;
    if (g_ourCb) {
        g_ourCb->Release();
        g_ourCb = nullptr;
    }
    if (g_savedVs) {
        g_savedVs->Release();
        g_savedVs = nullptr;
    }
    if (g_savedCb3) {
        g_savedCb3->Release();
        g_savedCb3 = nullptr;
    }
    g_engaged = false;
    g_target = nullptr;
    g_shadowValid = false;
}

}  // namespace edvr

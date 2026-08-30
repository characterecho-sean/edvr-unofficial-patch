#include "particle_fix.h"

#include <windows.h>

#include <d3d11.h>
#include <d3d11_1.h>   // VSGetConstantBuffers1: the per-draw bind offset

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include <string>

#include "../common/timing.h"
#include "exposure_fix.h"   // lookupShaderHash
#include "flare_vs.h"
#include "particle_vs.h"
#include "shader_swap.h"

namespace edvr {
namespace {

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

// A second hash whose instruction body is BYTE-IDENTICAL to kFlareVs --
// the same program compiled twice. It gets the same replacement free.
// Verified by diffing the two disassemblies, 2026-08-29.
constexpr uint64_t kFlareVsTwin = 0x9AEC596A2B036EA6ull;

struct BillboardVariant {
    uint64_t    hash;
    uint64_t    twin;      // a second hash with an identical body, or 0
    const char* hlsl;
    size_t      hlslLen;
    const char* name;      // names the compile in the log
};

constexpr int kVariantCount = 2;
const BillboardVariant kVariants[kVariantCount] = {
    {kPlumeVs, 0, kParticleWorldVS, sizeof(kParticleWorldVS) - 1,
     "particle_vs"},
    {kFlareVs, kFlareVsTwin, kFlareWorldVS, sizeof(kFlareWorldVS) - 1,
     "flare_vs"},
};

const char* variantLabel(int v) {
    return (v == 1) ? "solar flare" : "smoke plume";
}

// cb1 is 280 registers. The basis vectors live at 278 and 279 -- floats
// 1112..1119 -- and the neighbours are logged with them because "which
// register is the up" is exactly what this exists to measure rather than
// assume.
constexpr uint32_t kFirstReg = 276;
constexpr uint32_t kRegs = 4;
constexpr uint32_t kNeedBytes = (kFirstReg + kRegs) * 16;

// A few frames, so the map does not block the render thread waiting for
// the copy it just queued -- the trade panel_quad and panel_curve already
// make and name.
constexpr uint64_t kReadbackLagMs = 50;
constexpr uint64_t kSampleMs = 1000;

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
enum class Mode { kStock, kSteady };
Mode     g_mode = Mode::kStock;
bool     g_probe = false;
bool     g_pending = false;
uint64_t g_copyMs = 0;
uint64_t g_lastMs = 0;
uint64_t g_seen = 0;
bool     g_noted = false;

ID3D11Buffer* g_staging = nullptr;
uint32_t      g_stagingBytes = 0;

FaultBudget g_budget("particle.probe", 5);

// The offset-aware view of a constant-buffer bind. D3D11.1 lets a game
// bind ONE buffer to many draws, each reading a different slice via a
// first-constant offset, and the plain VSGetConstantBuffers cannot see
// that offset -- it returns the buffer and nothing else. Reading the
// wrong slice is the leading suspect for the field result where aiming
// each draw at its emitter made the smoke disappear: every draw would
// have been aimed with whichever emitter happened to sit at offset zero.
ID3D11DeviceContext1* g_ctx1 = nullptr;
bool                  g_ctx1Tried = false;

ID3D11DeviceContext1* context1(ID3D11DeviceContext* ctx) {
    if (!g_ctx1Tried) {
        g_ctx1Tried = true;
        ctx->QueryInterface(__uuidof(ID3D11DeviceContext1),
                            reinterpret_cast<void**>(&g_ctx1));
    }
    return g_ctx1;
}

// The first constant (in 16-byte registers) this draw reads slot `slot`
// from. Zero when the runtime has no offset to report, which is also the
// right answer for a plainly bound buffer.
uint32_t bindOffsetRegs(ID3D11DeviceContext* ctx, UINT slot) {
    ID3D11DeviceContext1* c1 = context1(ctx);
    if (!c1) return 0;
    ID3D11Buffer* b = nullptr;
    UINT first = 0, num = 0;
    c1->VSGetConstantBuffers1(slot, 1, &b, &first, &num);
    if (b) b->Release();
    return static_cast<uint32_t>(first);
}

// Which billboard variant is this draw, or -1 for none? By shader hash and
// nothing else: the geyser hunt established that kind, count, stride and
// every sampler size are shared with the terrain and prop pipelines.
int billboardVariantFor(ID3D11DeviceContext* ctx) {
    ID3D11VertexShader* vs = nullptr;
    ctx->VSGetShader(&vs, nullptr, nullptr);
    if (!vs) return -1;
    const uint64_t h = lookupShaderHash(vs);
    vs->Release();
    for (int i = 0; i < kVariantCount; ++i) {
        if (h == kVariants[i].hash) return i;
        if (kVariants[i].twin && h == kVariants[i].twin) return i;
    }
    return -1;
}

void readBack(ID3D11DeviceContext* ctx) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_staging, 0, D3D11_MAP_READ, 0, &m)) || !m.pData) {
        g_pending = false;
        return;
    }
    const float* f = static_cast<const float*>(m.pData);
    char line[420];
    int o = 0;
    for (uint32_t r = 0; r < kRegs && o < 380; ++r) {
        const float* v = f + (kFirstReg + r) * 4;
        const float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        o += snprintf(line + o, sizeof(line) - o,
                      "%scb1[%u]=(%.3f %.3f %.3f %.3f)|%.3f|",
                      r ? "  " : "", kFirstReg + r, v[0], v[1], v[2], v[3],
                      len);
    }
    ctx->Unmap(g_staging, 0);
    Log::get().note(
        "particle probe: %s  -- the vector that TURNS as you look around is "
        "the view direction, and the one that ROLLS as you tilt your head is "
        "the up a fix replaces with world up. A unit length is the tell for "
        "both; anything else here is not a basis vector.",
        line);
    g_pending = false;
}

ID3D11Buffer* g_cb0Staging = nullptr;
uint32_t      g_cb0StagingBytes = 0;
bool          g_cb0Pending = false;

// cb0[9..11] is the emitter's model matrix: the shader sends every
// particle position through it, so its translation column -- the .w of
// each row -- is where the emitter itself sits in the world. With the
// camera position from cb1[276] beside it, that is everything a
// per-emitter facing direction needs, and both are constants EDVR can
// already substitute. Logged to confirm before anything is built on it.
void readBackCb0(ID3D11DeviceContext* ctx) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_cb0Staging, 0, D3D11_MAP_READ, 0, &m)) || !m.pData) {
        g_cb0Pending = false;
        return;
    }
    const float* f = static_cast<const float*>(m.pData);
    const float ex = f[9 * 4 + 3], ey = f[10 * 4 + 3], ez = f[11 * 4 + 3];
    ctx->Unmap(g_cb0Staging, 0);
    Log::get().note(
        "particle probe: emitter at (%.1f %.1f %.1f) from cb0[9..11].w -- if "
        "this differs between the geysers in view and holds still while you "
        "look around, it is the per-emitter position a facing direction is "
        "built from.",
        ex, ey, ez);
    g_cb0Pending = false;
}

void queueCopyCb0(ID3D11DeviceContext* ctx) {
    ID3D11Buffer* cb = nullptr;
    ctx->VSGetConstantBuffers(0, 1, &cb);
    if (!cb) return;
    D3D11_BUFFER_DESC bd{};
    cb->GetDesc(&bd);
    if (bd.ByteWidth < 12 * 16) { cb->Release(); return; }
    if (g_cb0Staging && g_cb0StagingBytes != bd.ByteWidth) {
        g_cb0Staging->Release();
        g_cb0Staging = nullptr;
        g_cb0StagingBytes = 0;
    }
    if (!g_cb0Staging) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            D3D11_BUFFER_DESC sd{};
            sd.ByteWidth = bd.ByteWidth;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            dev->CreateBuffer(&sd, nullptr, &g_cb0Staging);
            dev->Release();
            if (g_cb0Staging) g_cb0StagingBytes = bd.ByteWidth;
        }
    }
    if (g_cb0Staging) {
        ctx->CopyResource(g_cb0Staging, cb);
        g_cb0Pending = true;
    }
    cb->Release();
}

void queueCopy(ID3D11DeviceContext* ctx) {
    ID3D11Buffer* cb = nullptr;
    ctx->VSGetConstantBuffers(1, 1, &cb);
    if (!cb) {
        if (!g_noted) {
            g_noted = true;
            Log::get().note(
                "particle probe: the plume draw has no constant buffer at "
                "slot 1, so the basis this reads is not there. The shader "
                "declares cb1, so this means the recognition matched a draw "
                "it should not have.");
        }
        return;
    }
    D3D11_BUFFER_DESC bd{};
    cb->GetDesc(&bd);
    if (bd.ByteWidth < kNeedBytes) {
        if (!g_noted) {
            g_noted = true;
            Log::get().note(
                "particle probe: cb1 is %u bytes, and the basis registers "
                "sit at %u. Either this build lays the buffer out "
                "differently or the match is wrong; nothing is read.",
                bd.ByteWidth, kNeedBytes);
        }
        cb->Release();
        return;
    }
    if (g_staging && g_stagingBytes != bd.ByteWidth) {
        g_staging->Release();
        g_staging = nullptr;
        g_stagingBytes = 0;
    }
    if (!g_staging) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            D3D11_BUFFER_DESC sd{};
            sd.ByteWidth = bd.ByteWidth;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            dev->CreateBuffer(&sd, nullptr, &g_staging);
            dev->Release();
            if (g_staging) g_stagingBytes = bd.ByteWidth;
        }
    }
    if (g_staging) {
        ctx->CopyResource(g_staging, cb);
        g_copyMs = nowMs();
        g_pending = true;
        if (!g_noted) {
            g_noted = true;
            Log::get().note(
                "particle probe: watching the plume shader %016llX -- cb1 is "
                "%u bytes. Sweep your head: roll it, then turn it, in the "
                "stereo view and in the flat on-foot view, and the lines "
                "below say which vector follows which motion in each.",
                static_cast<unsigned long long>(kPlumeVs), bd.ByteWidth);
        }
    }
    cb->Release();
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

// The emitter's constants, shadowed per draw. Elite renders in
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
// became a shader swap. Kept behind the key so the measurement can be
// repeated, never as something to switch on.
bool     g_faceEmitter = false;
void*    g_target0 = nullptr;
// Sized for a RING buffer, not one object's constants: if the
// emitter's matrix is reached by per-draw offset, the whole ring
// has to be in hand to index into it.
uint8_t  g_shadow0[65536];
uint32_t g_shadow0Bytes = 0;
bool     g_shadow0Valid = false;
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

bool particleSteady() { return g_mode == Mode::kSteady; }

void* particleTargetCb0() {
    return g_mode == Mode::kSteady ? g_target0 : nullptr;
}

void particleCaptureCb0(const void* data, uint32_t bytes) {
    if (g_mode != Mode::kSteady || !data || bytes < 12 * 16 ||
        bytes > sizeof(g_shadow0)) {
        g_shadow0Valid = false;
        return;
    }
    memcpy(g_shadow0, data, bytes);
    g_shadow0Bytes = bytes;
    g_shadow0Valid = true;
}

void* particleTarget() {
    return g_mode == Mode::kSteady ? g_target : nullptr;
}

void particleCapture(const void* data, uint32_t bytes) {
    if (g_mode != Mode::kSteady || !data || bytes < 64 || bytes > kMaxShadow) {
        g_shadowValid = false;
        return;
    }
    memcpy(g_shadow, data, bytes);
    g_shadowBytes = bytes;
    g_shadowValid = true;
}

bool particleOnDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                    uint32_t instances) {
    if (g_mode != Mode::kSteady || !ctx) return false;
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

    // The emitter buffer, learned the same way. Its absence is not a
    // refusal: without it the substitution still removes the roll, which
    // is most of the artifact -- it only loses the per-emitter facing.
    ID3D11Buffer* cb0 = nullptr;
    ctx->VSGetConstantBuffers(0, 1, &cb0);
    if (cb0) {
        if (cb0 != g_target0) {
            g_target0 = cb0;
            g_shadow0Valid = false;
            D3D11_BUFFER_DESC bd{};
            cb0->GetDesc(&bd);
            Log::get().note(
                "particle billboard: the emitter's constants live in a %u-byte "
                "buffer, and this draw reads it from register %u. A large "
                "buffer with a moving offset is a ring the draws share -- "
                "which is why aiming from its start pointed every plume with "
                "one emitter's direction.",
                bd.ByteWidth, bindOffsetRegs(ctx, 0));
        }
        cb0->Release();
    }

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
            g_ourVs[variant] = shaderSwapCompileVs(
                ctx, d.hlsl, d.hlslLen, "main", d.name, nullptr,
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
    const Mode wasMode = g_mode;
    const std::string m = cfg.getString("fix.particle_billboard", "steady");
    if (m == "steady") {
        g_mode = Mode::kSteady;
    } else {
        if (m != "stock") {
            Log::get().note("particle billboard: that is not stock or "
                            "steady; running stock.");
        }
        g_mode = Mode::kStock;
    }
    if (g_mode != wasMode) {
        if (g_mode == Mode::kSteady) {
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
            g_target0 = nullptr;
            g_shadow0Valid = false;
            Log::get().note("particle billboard: stock.");
        }
    }
    g_faceEmitter = cfg.getBool("advanced.particle_face_emitter", false);
    const bool was = g_probe;
    g_probe = cfg.getBool("advanced.particle_probe", false);
    if (g_probe != was) {
        if (g_probe) {
            g_noted = false;
            g_seen = 0;
            Log::get().note(
                "particle probe: ON. The particle billboards' basis "
                "constants are logged once a second while a plume draws. "
                "Nothing is changed -- this only reads.");
        } else {
            Log::get().note("particle probe: off.");
        }
    }
}

bool particleWantsDraws() {
    return g_probe || g_mode == Mode::kSteady;
}

void particleOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                       uint32_t instances) {
    if (!g_probe || !ctx) return;
    // Cheap tests first: the plume is instanced and never a bare Draw, so
    // the shader lookup -- which costs a COM call -- is spared most draws.
    if (kind != 'X' && kind != 'N') return;
    if (instances == 0 || count < 6) return;

    guardedBudget(g_budget, [&] {
        if (g_pending) {
            if (g_staging && nowMs() - g_copyMs >= kReadbackLagMs) {
                readBack(ctx);
                if (g_cb0Pending && g_cb0Staging) readBackCb0(ctx);
            }
            return;
        }
        const uint64_t now = nowMs();
        if (now - g_lastMs < kSampleMs) return;
        // Any family member will do for the probe: they share cb1 and the
        // basis registers it samples, which is the whole point of it.
        if (billboardVariantFor(ctx) < 0) return;
        ++g_seen;
        g_lastMs = now;
        queueCopy(ctx);
        queueCopyCb0(ctx);
    });
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
    g_target0 = nullptr;
    g_shadow0Valid = false;
    if (g_staging) {
        g_staging->Release();
        g_staging = nullptr;
    }
    if (g_cb0Staging) {
        g_cb0Staging->Release();
        g_cb0Staging = nullptr;
    }
    g_cb0StagingBytes = 0;
    g_cb0Pending = false;
    g_stagingBytes = 0;
    g_pending = false;
}

}  // namespace edvr

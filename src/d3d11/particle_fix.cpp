#include "particle_fix.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include <string>

#include "../common/timing.h"
#include "exposure_fix.h"   // lookupShaderHash

namespace edvr {
namespace {

// The particle billboard vertex shader, by content hash. Found 2026-08-23
// by skipping shaders one at a time at a geyser field (census_skip's
// vs:HASH term) after both offline methods failed: the differential census
// had nothing to subtract, because a geyser field is never quiet in every
// frame, and every size-level signature collided with terrain and props.
constexpr uint64_t kPlumeVs = 0xEB787F983BC1F5A3ull;

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

// Is this draw the particle billboard? By shader hash and nothing else:
// the geyser hunt established that kind, count, stride and every sampler
// size are shared with the terrain and prop pipelines.
bool isPlumeDraw(ID3D11DeviceContext* ctx) {
    ID3D11VertexShader* vs = nullptr;
    ctx->VSGetShader(&vs, nullptr, nullptr);
    if (!vs) return false;
    const uint64_t h = lookupShaderHash(vs);
    vs->Release();
    return h == kPlumeVs;
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
// The per-emitter facing is OFF by default, and that is a field result
// rather than caution: aiming each draw at its own emitter made the smoke
// disappear entirely (2026-08-23) while the roll fix beside it was
// verified good. The direction computed sensibly -- about forty degrees
// off the view axis, right for a plume below and to the side -- so the
// arithmetic is not what failed. The open suspect is freshness: if Elite
// binds one constant buffer at per-draw OFFSETS rather than remapping it,
// the shadow holds some other emitter's matrix and most quads end up
// aimed edge-on. Confirming that needs the offset-aware bind hook the
// tee does not have yet.
bool     g_faceEmitter = false;
void*    g_target0 = nullptr;
uint8_t  g_shadow0[1024];
uint32_t g_shadow0Bytes = 0;
bool     g_shadow0Valid = false;
uint64_t g_facingUsed = 0;
float    g_lastFacing[3] = {};

ID3D11Buffer* g_ourCb = nullptr;
uint32_t      g_ourBytes = 0;
ID3D11Buffer* g_savedCb = nullptr;
bool          g_engaged = false;

FaultBudget g_subBudget("particle.substitute", 5);

// Is the shadow shaped like the camera block this fix understands? Both
// basis vectors must be unit length; anything else is a buffer that is
// not what we think it is, and substituting into it would be writing a
// guess into the game's pipeline.
bool shapeOk(const float* f, uint32_t floats) {
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
    if (!isPlumeDraw(ctx)) return false;

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
        }
        cb0->Release();
    }

    if (!g_shadowValid) return false;
    return shapeOk(reinterpret_cast<const float*>(g_shadow), g_shadowBytes / 4);
}

void particleBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    if (!ctx || !g_shadowValid) return;
    guardedBudget(g_subBudget, [&] {
        if (g_ourCb && g_ourBytes != g_shadowBytes) {
            g_ourCb->Release();
            g_ourCb = nullptr;
            g_ourBytes = 0;
        }
        if (!g_ourCb) {
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (!dev) return;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = g_shadowBytes;
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            dev->CreateBuffer(&bd, nullptr, &g_ourCb);
            dev->Release();
            if (!g_ourCb) return;
            g_ourBytes = g_shadowBytes;
        }
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(g_ourCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) ||
            !m.pData) {
            return;
        }
        memcpy(m.pData, g_shadow, g_shadowBytes);
        // The whole substitution: the basis's up becomes world up. The
        // shader then builds right = cross(worldUp, forward) -- horizontal
        // in the world -- and up = cross(forward, right), so the quad still
        // faces the camera while its roll stops following the view. The w
        // is left as the game wrote it: only the direction is ours.
        float* f = static_cast<float*>(m.pData);
        f[278 * 4 + 0] = 0.0f;
        f[278 * 4 + 1] = 1.0f;
        f[278 * 4 + 2] = 0.0f;

        // And the facing, when the emitter's own constants are in hand:
        // point the quads AT the viewer rather than along the view axis.
        // Sharing one view-aligned normal across a whole draw is what
        // made a plume off to the side look foreshortened, and made the
        // foreshortening change -- read as the sprite rotating -- as the
        // view yawed. Per emitter, the residual error is only the plume's
        // own angular width instead of its angular distance off centre.
        if (g_faceEmitter && g_shadow0Valid) {
            const float* e = reinterpret_cast<const float*>(g_shadow0);
            const float ex = e[9 * 4 + 3], ey = e[10 * 4 + 3],
                        ez = e[11 * 4 + 3];
            const float len = sqrtf(ex * ex + ey * ey + ez * ez);
            if (len > 1.0f) {
                f[279 * 4 + 0] = ex / len;
                f[279 * 4 + 1] = ey / len;
                f[279 * 4 + 2] = ez / len;
                g_lastFacing[0] = ex / len;
                g_lastFacing[1] = ey / len;
                g_lastFacing[2] = ez / len;
                ++g_facingUsed;
            }
        }
        ctx->Unmap(g_ourCb, 0);

        ctx->VSGetConstantBuffers(1, 1, &g_savedCb);
        ID3D11Buffer* ours = g_ourCb;
        ctx->VSSetConstantBuffers(1, 1, &ours);
        g_engaged = true;
        ++g_applied;
    });
}

void particleEnd(ID3D11DeviceContext* ctx) {
    if (!g_engaged || !ctx) return;
    g_engaged = false;
    ctx->VSSetConstantBuffers(1, 1, &g_savedCb);
    if (g_savedCb) {
        g_savedCb->Release();
        g_savedCb = nullptr;
    }
    const uint64_t now = nowMs();
    if (now - g_noteMs >= 10000) {
        Log::get().note(
            "particle billboard: steady -- %llu substitution(s) in the last "
            "ten seconds, %llu of them aimed at their own emitter, the last "
            "at (%.3f %.3f %.3f). Quads are held upright in the world; the "
            "aiming is off unless particle_face_emitter is set.",
            static_cast<unsigned long long>(g_applied - g_appliedAtNote),
            static_cast<unsigned long long>(g_facingUsed),
            g_lastFacing[0], g_lastFacing[1], g_lastFacing[2]);
        g_facingUsed = 0;
        g_noteMs = now;
        g_appliedAtNote = g_applied;
    }
}

void particleConfigure(Config& cfg) {
    const Mode wasMode = g_mode;
    const std::string m = cfg.getString("fix.particle_billboard", "stock");
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
        if (!isPlumeDraw(ctx)) return;
        ++g_seen;
        g_lastMs = now;
        queueCopy(ctx);
        queueCopyCb0(ctx);
    });
}

void particleShutdown() {
    if (g_ourCb) {
        g_ourCb->Release();
        g_ourCb = nullptr;
    }
    g_ourBytes = 0;
    if (g_savedCb) {
        g_savedCb->Release();
        g_savedCb = nullptr;
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

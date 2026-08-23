#include "particle_fix.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
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

}  // namespace

void particleConfigure(Config& cfg) {
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

bool particleWantsDraws() { return g_probe; }

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
            }
            return;
        }
        const uint64_t now = nowMs();
        if (now - g_lastMs < kSampleMs) return;
        if (!isPlumeDraw(ctx)) return;
        ++g_seen;
        g_lastMs = now;
        queueCopy(ctx);
    });
}

void particleShutdown() {
    if (g_staging) {
        g_staging->Release();
        g_staging = nullptr;
    }
    g_stagingBytes = 0;
    g_pending = false;
}

}  // namespace edvr

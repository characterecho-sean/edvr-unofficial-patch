#include "billboard_fix.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstring>

#include "../common/log.h"
#include "../common/timing.h"
#include "binding_shadow.h"

namespace edvr {

namespace {

// The measured layout (cb_peek, 2026-08-20, 208-byte buffer). Offsets in
// floats. The shape check below is what keeps these from being trusted
// blindly: a write where these offsets do not LOOK like the measured
// structure is not offered.
constexpr uint32_t kDrawnRight = 16;   // [16..18], magnitude = sprite scale
constexpr uint32_t kDrawnUp = 20;      // [20..22]
constexpr uint32_t kWorldRight = 36;   // [36..38]
constexpr uint32_t kWorldUp = 40;      // [40..42], (0,1,0) in every capture
constexpr uint32_t kMinFloats = 47;    // the layout's last consulted float
constexpr uint32_t kShadowBytes = 512;

// The buffer being watched and its last write. The game multiplexes several
// sprites through one buffer, write-draw, write-draw -- so the shadow is
// per-write and consumed by the next matched draw.
void*    g_target = nullptr;
uint8_t  g_shadow[kShadowBytes];
uint32_t g_shadowBytes = 0;
bool     g_shadowValid = false;
uint64_t g_shadowMs = 0;    // when the shadow content last updated -- a
                            // perfectly fresh-LOOKING shadow of the wrong
                            // buffer is the failure mode this exposes

bool     g_glareWatch = false;   // fix.sun_glare's world shader or probe
bool     g_learnNoted = false;

float len3(const float* v) {
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

float dot3(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Does this write look like the measured billboard constants? Two
// equal-magnitude orthogonal vectors in the drawn slots, unit rows in the
// world slots. Everything the multiplexed buffer carries that is NOT a
// billboard -- and every layout a game update might move -- fails here.
bool shapeOk(const float* f, uint32_t floats) {
    if (floats <= kMinFloats) return false;
    const float* r = f + kDrawnRight;
    const float* u = f + kDrawnUp;
    const float lr = len3(r), lu = len3(u);
    if (!(lr > 0.001f) || !(lr < 1000.0f)) return false;
    if (!(fabsf(lr - lu) <= 0.2f * lr)) return false;
    if (!(fabsf(dot3(r, u)) <= 0.15f * lr * lu)) return false;
    const float* wr = f + kWorldRight;
    const float* wu = f + kWorldUp;
    const float lwr = len3(wr), lwu = len3(wu);
    if (!(fabsf(lwr - 1.0f) <= 0.1f) || !(fabsf(lwu - 1.0f) <= 0.1f)) return false;
    if (!(fabsf(dot3(wr, wu)) <= 0.15f)) return false;
    return true;
}

}  // namespace

void billboardGlareWatch(bool on) {
    if (g_glareWatch != on) {
        g_glareWatch = on;
        if (!on) {
            g_target = nullptr;
            g_shadowValid = false;
        }
    }
}

bool billboardOnGlareDraw(uint32_t count, uint32_t /*instances*/) {
    if (!g_glareWatch) return false;
    void* cb = bindingGet(BindSlot::VsCb0);
    if (!cb) return false;
    if (cb != g_target) {
        g_target = cb;
        g_shadowValid = false;
        if (!g_learnNoted) {
            g_learnNoted = true;
            Log::get().note("billboard: watching the glare train's constants "
                            "(first matched draw n=%u).", count);
        }
        return false;   // nothing captured for this buffer yet
    }
    if (!g_shadowValid) return false;
    const float* f = reinterpret_cast<const float*>(g_shadow);
    return shapeOk(f, g_shadowBytes / 4);
}

const float* billboardShadowFloats(uint32_t* count) {
    if (!g_shadowValid) return nullptr;
    if (count) *count = g_shadowBytes / 4;
    return reinterpret_cast<const float*>(g_shadow);
}

void* billboardTarget() {
    return g_glareWatch ? g_target : nullptr;
}

void billboardCapture(const void* data, uint32_t bytes) {
    if (!g_glareWatch || !data || bytes < (kMinFloats + 1) * 4) {
        g_shadowValid = false;
        return;
    }
    if (bytes > kShadowBytes) bytes = kShadowBytes;
    memcpy(g_shadow, data, bytes);
    g_shadowBytes = bytes;
    g_shadowValid = true;
    g_shadowMs = nowMs();
}

uint64_t billboardShadowAgeMs() {
    if (!g_shadowValid || g_shadowMs == 0) return ~0ull;
    return nowMs() - g_shadowMs;
}

void billboardShutdown() {
    g_target = nullptr;
    g_shadowValid = false;
}

}  // namespace edvr

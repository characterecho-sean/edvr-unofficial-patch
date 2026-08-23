#include "billboard_fix.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstring>

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/log.h"
#include "../common/timing.h"
#include "binding_shadow.h"

namespace edvr {
namespace {

// The family, matcher v2 (see cb_peek.cpp for the sweep that corrected v1):
// DrawIndexedInstanced, the eye-sized depth resolve in PS slot 0, a texture
// in slot 1, a large-ish index count.
constexpr char     kKind = 'X';
constexpr uint32_t kMinIndices = 64;

// The measured layout (cb_peek, 2026-08-20, 208-byte buffer). Offsets in
// floats. The shape check below is what keeps these from being trusted
// blindly: a write where these offsets do not LOOK like the measured
// structure draws untouched.
constexpr uint32_t kDrawnRight = 16;   // [16..18], magnitude = sprite scale
constexpr uint32_t kDrawnUp = 20;      // [20..22]
constexpr uint32_t kDrawnFwd = 28;     // [28..30], unit, tracks the view
constexpr uint32_t kWorldRight = 36;   // [36..38]
constexpr uint32_t kWorldUp = 40;      // [40..42], (0,1,0) in every capture
constexpr uint32_t kMinFloats = 47;    // the layout's last consulted float
constexpr uint32_t kShadowBytes = 512;

// stock: untouched. steady: the fix. probe: a DIAGNOSTIC that shrinks the
// drawn basis to a tenth instead of reorienting it -- the discriminating
// experiment for a substitution that engages without visible effect. A
// sprite that shrinks proves the shader consumes these floats (and the
// wrongness is which draw is matched); a sprite that ignores even this
// proves it does not (and the basis lives in a second constant slot or the
// shader's own math).
enum class Mode : uint32_t { kStock, kSteady, kProbe };
Mode g_mode = Mode::kStock;

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

// Our substitute, panel-distance style: created at the game's buffer size,
// filled per substitution.
ID3D11Buffer* g_ourCb = nullptr;
uint32_t      g_ourCbBytes = 0;

bool                g_engaged = false;
bool                g_glareWatch = false;   // sun_glare_steady borrows the
                                            // machinery; see below
uint64_t            g_applied = 0;
uint64_t            g_rejected = 0;
uint64_t            g_retargets = 0;
bool                g_learnNoted = false;
uint64_t            g_lastNoteMs = 0;
uint64_t            g_appliedAtNote = 0;

float len3(const float* v) {
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

float dot3(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Does this write look like the measured billboard constants? Two
// equal-magnitude orthogonal vectors in the drawn slots, unit rows in the
// world slots. Everything the multiplexed buffer carries that is NOT a
// billboard -- and every layout a game update might move -- fails here and
// draws stock.
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

void billboardConfigure(Config& /*cfg*/) {
    // RETIRED as a user fix 2026-08-23: the sprite family this targeted
    // turned out not to read the constants it substitutes (the probe of
    // 2026-08-20 -- 900 subs/s, nothing shrank), and the suns it was
    // reaching for are handled by fix.sun_glare's shader swap. What
    // remains in service is the shadow-and-substitute machinery below,
    // loaned to the glare fix as its constants tee (billboardGlareWatch).
    // No keys are read.
    g_mode = Mode::kStock;
}

bool billboardWantsDraws() { return g_mode != Mode::kStock; }

// The glare-steady loan: sun_glare_steady borrows this module's whole
// shadow-and-substitute machinery -- the glare train writes the SAME
// 208-byte camera-standard layout the family matcher was built for, and
// the sweep of 2026-08-21 showed its rows pass shapeOk exactly. Only the
// MATCHING differs, and that already happened in sunglare_fix; this entry
// point is the family path minus the family test.
void billboardGlareWatch(bool on) {
    if (g_glareWatch != on) {
        g_glareWatch = on;
        if (!on && g_mode == Mode::kStock) {
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
        ++g_retargets;
        if (!g_learnNoted) {
            g_learnNoted = true;
            Log::get().note("billboard: watching the glare train's constants "
                            "(first matched draw n=%u).", count);
        }
        return false;   // nothing captured for this buffer yet
    }
    if (!g_shadowValid) return false;
    const float* f = reinterpret_cast<const float*>(g_shadow);
    if (!shapeOk(f, g_shadowBytes / 4)) {
        ++g_rejected;
        return false;
    }
    return true;
}

bool billboardOnEyeDraw(char kind, uint32_t count, uint32_t /*instances*/) {
    if (g_mode == Mode::kStock || kind != kKind || count < kMinIndices) {
        return false;
    }

    ResourceInfo atlas;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv1), &atlas) ||
        !atlas.isTexture2D) {
        return false;
    }
    uint32_t eyeW = 0, eyeH = 0;
    if (!eyeTextureSize(&eyeW, &eyeH)) return false;
    ResourceInfo depth;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv0), &depth) ||
        !depth.isTexture2D || depth.a != eyeW || depth.b != eyeH) {
        return false;
    }

    void* cb = bindingGet(BindSlot::VsCb0);
    if (!cb) return false;
    if (cb != g_target) {
        // Follow the family's buffer wherever the game keeps it; the shadow
        // resets because contents captured from another buffer are not this
        // one's. Counted, because a family whose draws alternate between
        // SEVERAL buffers thrashes this and substitutes almost nothing --
        // the telemetry line below is where that becomes visible.
        g_target = cb;
        g_shadowValid = false;
        ++g_retargets;
        if (!g_learnNoted) {
            g_learnNoted = true;
            Log::get().note("billboard: watching the sprite constants "
                            "(first matched draw n=%u).", count);
        }
        return false;   // nothing captured for this buffer yet
    }
    if (!g_shadowValid) return false;

    const float* f = reinterpret_cast<const float*>(g_shadow);
    if (!shapeOk(f, g_shadowBytes / 4)) {
        ++g_rejected;
        return false;
    }
    return true;
}

const float* billboardShadowFloats(uint32_t* count) {
    if (!g_shadowValid) return nullptr;
    if (count) *count = g_shadowBytes / 4;
    return reinterpret_cast<const float*>(g_shadow);
}

void* billboardTarget() {
    return g_mode != Mode::kStock || g_glareWatch ? g_target : nullptr;
}

void billboardCapture(const void* data, uint32_t bytes) {
    if ((g_mode == Mode::kStock && !g_glareWatch) || !data ||
        bytes < (kMinFloats + 1) * 4) {
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

void billboardBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;

    const uint32_t bytes = g_shadowBytes;
    if (!g_ourCb || g_ourCbBytes != bytes) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return;
        if (g_ourCb) {
            g_ourCb->Release();
            g_ourCb = nullptr;
        }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = bytes;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        const HRESULT hr = dev->CreateBuffer(&bd, nullptr, &g_ourCb);
        dev->Release();
        if (FAILED(hr) || !g_ourCb) {
            g_ourCb = nullptr;
            return;
        }
        g_ourCbBytes = bytes;
    }

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_ourCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) ||
        !m.pData) {
        return;
    }
    memcpy(m.pData, g_shadow, bytes);
    float* f = static_cast<float*>(m.pData);
    const float s = len3(f + kDrawnRight);
    if (g_mode == Mode::kProbe) {
        // The discriminating experiment: shrink instead of reorient. A
        // sprite that visibly shrinks is consuming these floats; one that
        // does not has its basis somewhere this buffer is not.
        for (int i = 0; i < 3; ++i) {
            f[kDrawnRight + i] *= 0.1f;
            f[kDrawnUp + i] *= 0.1f;
        }
    } else {
        // In-plane roll removal, the third steady formula and the first
        // derived from field failures rather than theory. Full replacement
        // taught two lessons: anchoring to the world rows builds an
        // edge-on plane (elements vanish), and even a camera-facing
        // rebuild displaces the elements PER EYE -- the drawn rows encode
        // each eye's projection mapping, not orientation alone, and rows
        // built from scratch lose it. So the steer must TRANSFORM the
        // write's own rows, never replace them: rotate right and up
        // within their own plane until up aligns with the world-up's
        // projection into that plane. A linear mix of the original rows
        // preserves their span, their magnitudes, and whatever projective
        // role they carry per eye; the only thing removed is the spin.
        // A level head yields a near-identity mix; under roll, the mix is
        // exactly the counter-rotation.
        const float* r0 = f + kDrawnRight;
        const float* u0 = f + kDrawnUp;
        const float* wu = f + kWorldUp;
        const float lr0 = len3(r0), lu0 = len3(u0), lwu = len3(wu);
        float rn[3], un[3], wn[3];
        for (int i = 0; i < 3; ++i) {
            rn[i] = r0[i] / lr0;
            un[i] = u0[i] / lu0;
            wn[i] = wu[i] / lwu;
        }
        const float a = dot3(wn, rn);
        const float b = dot3(wn, un);
        const float n = sqrtf(a * a + b * b);
        if (n > 0.05f) {   // world-up nearly out of the plane (looking
                           // straight up or down): draw untouched rather
                           // than divide by nothing
            for (int i = 0; i < 3; ++i) {
                const float upT = (a * rn[i] + b * un[i]) / n;
                const float rtT = (b * rn[i] - a * un[i]) / n;
                f[kDrawnRight + i] = rtT * lr0;
                f[kDrawnUp + i] = upT * lu0;
            }
        }
    }
    ctx->Unmap(g_ourCb, 0);

    ID3D11Buffer* ours = g_ourCb;
    ctx->VSSetConstantBuffers(0, 1, &ours);
    g_engaged = true;

    // Telemetry every two seconds while applying: how many draws are being
    // substituted, how many writes the shape gate refused, and how often
    // the target buffer CHANGED under the matcher -- the number that says
    // whether the family draws through one buffer or thrashes several,
    // which decides whether the substitutions are landing on the sprite
    // being looked at.
    ++g_applied;
    const uint64_t now = nowMs();
    if (g_applied == 1 || now - g_lastNoteMs >= 2000) {
        Log::get().note("billboard: %s -- %llu substitution(s) since last "
                        "note (scale %.3f), %llu shape-refusals, %llu "
                        "target changes total.",
                        g_mode == Mode::kProbe ? "PROBE shrinking" : "steady",
                        static_cast<unsigned long long>(g_applied -
                                                        g_appliedAtNote),
                        s, static_cast<unsigned long long>(g_rejected),
                        static_cast<unsigned long long>(g_retargets));
        g_lastNoteMs = now;
        g_appliedAtNote = g_applied;
    }
}

void billboardEnd(ID3D11DeviceContext* ctx) {
    if (!g_engaged) return;
    g_engaged = false;
    ID3D11Buffer* orig = static_cast<ID3D11Buffer*>(g_target);
    ctx->VSSetConstantBuffers(0, 1, &orig);
}

void billboardShutdown() {
    if (g_ourCb) {
        g_ourCb->Release();
        g_ourCb = nullptr;
    }
    g_target = nullptr;
    g_shadowValid = false;
}

}  // namespace edvr

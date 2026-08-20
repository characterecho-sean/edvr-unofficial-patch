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

// Our substitute, panel-distance style: created at the game's buffer size,
// filled per substitution.
ID3D11Buffer* g_ourCb = nullptr;
uint32_t      g_ourCbBytes = 0;

bool                g_engaged = false;
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

void billboardConfigure(Config& cfg) {
    const Mode was = g_mode;
    const std::string m = cfg.getString("fix.billboard", "stock");
    if (m == "stock") {
        g_mode = Mode::kStock;
    } else if (m == "steady") {
        g_mode = Mode::kSteady;
    } else if (m == "probe") {
        g_mode = Mode::kProbe;
    } else {
        g_mode = Mode::kStock;
        Log::get().note("billboard \"%s\" is not stock, steady or probe; "
                        "running stock.", m.c_str());
    }
    if (was != g_mode) {
        const char* names[] = {"stock", "steady", "probe"};
        Log::get().note("billboard: %s.%s",
                        names[static_cast<uint32_t>(g_mode)],
                        g_mode == Mode::kProbe
                            ? " DIAGNOSTIC: matched sprites draw at a tenth "
                              "of their size. Whatever shrinks is consuming "
                              "the substituted constants; whatever ignores "
                              "this is not."
                            : "");
        if (g_mode == Mode::kStock) {
            g_target = nullptr;
            g_shadowValid = false;
        }
    }
}

bool billboardWantsDraws() { return g_mode != Mode::kStock; }

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

void* billboardTarget() {
    return g_mode != Mode::kStock ? g_target : nullptr;
}

void billboardCapture(const void* data, uint32_t bytes) {
    if (g_mode == Mode::kStock || !data || bytes < (kMinFloats + 1) * 4) {
        g_shadowValid = false;
        return;
    }
    if (bytes > kShadowBytes) bytes = kShadowBytes;
    memcpy(g_shadow, data, bytes);
    g_shadowBytes = bytes;
    g_shadowValid = true;
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
        // Orientation from the world frame the write itself carries, scale
        // from the drawn basis it came with. The starburst then rotates
        // only when its OWN frame does -- the sun moving relative to the
        // ship -- and never with the head.
        const float* wr = f + kWorldRight;
        const float* wu = f + kWorldUp;
        const float lwr = len3(wr), lwu = len3(wu);
        for (int i = 0; i < 3; ++i) {
            f[kDrawnRight + i] = wr[i] / lwr * s;
            f[kDrawnUp + i] = wu[i] / lwu * s;
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

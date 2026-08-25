#include "fss_reveal.h"

#include <cstring>

#include <windows.h>

#include <d3d11.h>

#include <string>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "exposure_fix.h"   // lookupShaderHash

namespace edvr {
namespace {

constexpr uint64_t kCompositeHash = 0x953C8123AD8DC13Bull;

// The scene block the composite's pixel stage reads. The shader DECLARES
// 333 vectors (5,328 bytes) -- but D3D binds any buffer at least that
// big, and the game's is 5,376 (the 336-vector buffer the transition
// flash detector has watched since it was built). The first field flight
// stood down on an equality test against the declaration and said
// nothing, which cost the whole look: accept the range, shadow the real
// size, and SAY every refusal.
constexpr uint32_t kSceneBlockMin = 333 * 16;
constexpr uint32_t kSceneBlockMax = 8192;

bool g_steady = false;

// The PS b1 buffer object, learned at the first matched draw, and the
// live shadow of its contents fed by the Map/Unmap and UpdateSubresource
// tees. mapped is the pData between Map and Unmap, the census tee's
// bargain exactly.
void*    g_sceneCb = nullptr;
uint32_t g_sceneCbBytes = 0;
void*    g_mapped = nullptr;
uint8_t  g_shadow[kSceneBlockMax];
bool     g_shadowValid = false;

// Occurrence 1's snapshot, applied at occurrence 2.
uint8_t  g_snapshot[kSceneBlockMax];
bool     g_snapshotValid = false;
uint8_t  g_occurrence = 0;

// The receipts a silent stand-down owes: refusals said once, and a note
// when composites keep matching while no write has ever been seen.
bool     g_sizeRefusedNoted = false;
uint32_t g_matchedNoShadow = 0;
bool     g_noShadowNoted = false;

// EDVR's own buffer for the substitution, sized to the game's.
ID3D11Buffer* g_ourCb = nullptr;
uint32_t      g_ourCbBytes = 0;
bool          g_createFailedNoted = false;

bool          g_engaged = false;
ID3D11Buffer* g_displaced = nullptr;

uint64_t g_applied = 0;
bool     g_engagedNoted = false;

FaultBudget g_budget("fssReveal", 8);

}  // namespace

void fssRevealConfigure(Config& cfg) {
    const bool was = g_steady;
    const std::string m = cfg.getString("fix.fss_reveal_sync", "stock");
    if (m == "stock") {
        g_steady = false;
    } else if (m == "steady") {
        g_steady = true;
    } else {
        g_steady = false;
        Log::get().note("fss_reveal_sync \"%s\" is not stock or steady; "
                        "running stock.", m.c_str());
    }
    if (was != g_steady) {
        Log::get().note(
            g_steady
                ? "fss reveal: steady. Both eyes' composites are drawn with "
                  "ONE snapshot of the scene constants, so the dissolve is "
                  "evaluated at the same moment for both -- the per-eye "
                  "square split cannot survive it if the stepping progress "
                  "is its cause."
                : "fss reveal: stock; each eye's composite reads the scene "
                  "constants as the game last wrote them.");
        // A mode flip invalidates learned state; relearn from scratch.
        g_sceneCb = nullptr;
        g_shadowValid = false;
        g_snapshotValid = false;
    }
}

bool fssRevealWantsDraws() { return g_steady; }

void fssRevealNoteMap(void* resource, void* data) {
    if (!g_steady || resource != g_sceneCb) return;
    g_mapped = data;
}

void fssRevealNoteUnmap(void* resource) {
    if (!g_steady || resource != g_sceneCb || !g_mapped) return;
    void* src = g_mapped;
    g_mapped = nullptr;
    if (!g_sceneCbBytes) return;
    guardedBudget(g_budget, [&] {
        memcpy(g_shadow, src, g_sceneCbBytes);
        g_shadowValid = true;
    });
}

void fssRevealNoteUpdate(void* resource, const void* data) {
    if (!g_steady || resource != g_sceneCb || !data || !g_sceneCbBytes) return;
    guardedBudget(g_budget, [&] {
        memcpy(g_shadow, data, g_sceneCbBytes);
        g_shadowValid = true;
    });
}

bool fssRevealOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                        uint32_t instances) {
    if (!g_steady || kind != 'N' || count != 6 || instances != 1 || !ctx) {
        return false;
    }
    uint64_t h = 0;
    guardedBudget(g_budget, [&] {
        ID3D11VertexShader* vs = nullptr;
        ctx->VSGetShader(&vs, nullptr, nullptr);
        h = lookupShaderHash(vs);
        if (vs) vs->Release();
    });
    return h == kCompositeHash;
}

void fssRevealBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    if (!ctx || !g_steady) return;
    guardedBudget(g_budget, [&] {
        ++g_occurrence;

        if (g_occurrence == 1) {
            // Learn (or confirm) the scene block: the buffer at PS b1 of
            // exactly this draw, accepted at any size covering the
            // shader's declaration. A relearn after the game recreates it
            // costs one frame of stock, the ourCb lifecycle's bargain.
            ID3D11Buffer* b1 = nullptr;
            ctx->PSGetConstantBuffers(1, 1, &b1);
            if (b1 != g_sceneCb) {
                g_sceneCb = b1;
                g_sceneCbBytes = 0;
                g_shadowValid = false;
                if (b1) {
                    D3D11_BUFFER_DESC d{};
                    b1->GetDesc(&d);
                    if (d.ByteWidth >= kSceneBlockMin &&
                        d.ByteWidth <= kSceneBlockMax) {
                        g_sceneCbBytes = d.ByteWidth;
                    } else {
                        g_sceneCb = nullptr;
                        if (!g_sizeRefusedNoted) {
                            g_sizeRefusedNoted = true;
                            Log::get().note(
                                "fss reveal: the buffer at the composite's "
                                "PS b1 is %u bytes, outside the %u-%u this "
                                "fix accepts; standing down. Said once.",
                                d.ByteWidth, kSceneBlockMin, kSceneBlockMax);
                        }
                    }
                }
            }
            if (b1) b1->Release();
            // The snapshot eye B will read: the bytes eye A is reading
            // NOW, as the tees last saw them written.
            if (g_shadowValid) {
                memcpy(g_snapshot, g_shadow, g_sceneCbBytes);
                g_snapshotValid = true;
                g_matchedNoShadow = 0;
            } else {
                g_snapshotValid = false;
                // A fix that keeps matching while its shadow never fills
                // is being starved by an unhooked write path, and the log
                // is where that has to be visible.
                if (g_sceneCb && ++g_matchedNoShadow == 300 &&
                    !g_noShadowNoted) {
                    g_noShadowNoted = true;
                    Log::get().note(
                        "fss reveal: 300 composites matched but no write "
                        "to the scene block has been seen -- it is being "
                        "written by a path the tees do not cover, and the "
                        "sync cannot engage. Said once.");
                }
            }
            return;   // eye A draws stock, always
        }

        if (g_occurrence != 2 || !g_snapshotValid || !g_sceneCbBytes) return;

        if (!g_ourCb || g_ourCbBytes != g_sceneCbBytes) {
            if (g_ourCb) {
                g_ourCb->Release();
                g_ourCb = nullptr;
            }
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (!dev) return;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = g_sceneCbBytes;
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            dev->CreateBuffer(&bd, nullptr, &g_ourCb);
            dev->Release();
            if (!g_ourCb) {
                if (!g_createFailedNoted) {
                    g_createFailedNoted = true;
                    Log::get().note("fss reveal: could not create the %u-byte "
                                    "buffer; the composite draws stock.",
                                    g_sceneCbBytes);
                }
                return;
            }
            g_ourCbBytes = g_sceneCbBytes;
        }

        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(g_ourCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) ||
            !m.pData) {
            return;
        }
        memcpy(m.pData, g_snapshot, g_sceneCbBytes);
        ctx->Unmap(g_ourCb, 0);

        ctx->PSGetConstantBuffers(1, 1, &g_displaced);
        ID3D11Buffer* ours = g_ourCb;
        ctx->PSSetConstantBuffers(1, 1, &ours);
        g_engaged = true;
        ++g_applied;
        if (!g_engagedNoted) {
            g_engagedNoted = true;
            Log::get().note(
                "fss reveal: steady engaged -- the second eye's composite "
                "reads the first eye's scene constants, restored after "
                "every draw. Both eyes now dissolve at one moment.");
        }
    });
}

void fssRevealEnd(ID3D11DeviceContext* ctx) {
    if (!g_engaged || !ctx) return;
    g_engaged = false;
    ctx->PSSetConstantBuffers(1, 1, &g_displaced);
    if (g_displaced) {
        g_displaced->Release();
        g_displaced = nullptr;
    }
}

void fssRevealFrameBoundary() { g_occurrence = 0; }

void fssRevealShutdown() {
    if (g_ourCb) {
        g_ourCb->Release();
        g_ourCb = nullptr;
    }
}

}  // namespace edvr

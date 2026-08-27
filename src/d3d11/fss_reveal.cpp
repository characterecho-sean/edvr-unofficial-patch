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
bool g_lockstep = false;

// ---- The lockstep copies: occurrence 1's four content textures, frozen
// at its draw so occurrence 2 reads the same bytes. Recreated whenever a
// source's description changes (fss_res remakes the body layer per
// zoom). A slot the copy machinery cannot serve stands the whole
// substitution down for the frame -- half a lockstep is a new split.
ID3D11Texture2D*          g_lsTex[4] = {};
ID3D11ShaderResourceView* g_lsView[4] = {};
D3D11_TEXTURE2D_DESC      g_lsDesc[4] = {};
bool                      g_lsHave[4] = {};
ID3D11ShaderResourceView* g_lsDisplaced[4] = {};
bool     g_lsBound = false;
uint64_t g_lsApplied = 0;
bool     g_lsNoted = false;
bool     g_lsFailNoted = false;

void lsRelease() {
    for (int i = 0; i < 4; ++i) {
        if (g_lsView[i]) { g_lsView[i]->Release(); g_lsView[i] = nullptr; }
        if (g_lsTex[i]) { g_lsTex[i]->Release(); g_lsTex[i] = nullptr; }
        g_lsHave[i] = false;
    }
}

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
    const bool wasSteady = g_steady;
    // High-level values: on | off | steady (developer instrument). The
    // mechanism's campaign names -- "lockstep" and "stock" -- survive as
    // silent aliases for inis written while they were the words.
    const std::string m = cfg.getString("fix.fss_reveal_sync", "off");
    const bool on = m == "on" || m == "lockstep";
    if (m == "off" || m == "stock") {
        g_steady = false;
    } else if (m == "steady") {
        g_steady = true;
    } else if (on) {
        g_steady = false;
        g_lockstep = true;
    } else {
        g_steady = false;
        Log::get().note("fss_reveal_sync \"%s\" is not on, off or steady; "
                        "running off.", m.c_str());
    }
    if (!on) g_lockstep = false;
    static bool s_wasLockstep = false;
    const bool lockFlip = s_wasLockstep != g_lockstep;
    s_wasLockstep = g_lockstep;
    if (wasSteady != g_steady || lockFlip) {
        Log::get().note(
            g_lockstep
                ? "fss reveal: ON (the lockstep mechanism). The second "
                  "eye's composite reads "
                  "byte-identical copies of the first eye's textures and "
                  "scene constants -- the two panels cannot differ, and "
                  "both show the resolve animation the flat screen shows, "
                  "binocularly fused."
            : g_steady
                ? "fss reveal: steady. Both eyes' composites are drawn with "
                  "ONE snapshot of the scene constants, so the dissolve is "
                  "evaluated at the same moment for both -- the per-eye "
                  "square split cannot survive it if the stepping progress "
                  "is its cause."
                : "fss reveal: off; each eye's composite reads the scene "
                  "constants as the game last wrote them.");
        // A mode flip invalidates learned state; relearn from scratch.
        g_sceneCb = nullptr;
        g_shadowValid = false;
        g_snapshotValid = false;
        lsRelease();
    }
}

bool fssRevealWantsDraws() { return g_steady || g_lockstep; }

void fssRevealNoteMap(void* resource, void* data) {
    if ((!g_steady && !g_lockstep) || resource != g_sceneCb) return;
    g_mapped = data;
}

void fssRevealNoteUnmap(void* resource) {
    if ((!g_steady && !g_lockstep) || resource != g_sceneCb || !g_mapped) {
        return;
    }
    void* src = g_mapped;
    g_mapped = nullptr;
    if (!g_sceneCbBytes) return;
    guardedBudget(g_budget, [&] {
        memcpy(g_shadow, src, g_sceneCbBytes);
        g_shadowValid = true;
    });
}

void fssRevealNoteUpdate(void* resource, const void* data) {
    if ((!g_steady && !g_lockstep) || resource != g_sceneCb || !data ||
        !g_sceneCbBytes) {
        return;
    }
    guardedBudget(g_budget, [&] {
        memcpy(g_shadow, data, g_sceneCbBytes);
        g_shadowValid = true;
    });
}

bool fssRevealOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                        uint32_t instances) {
    if ((!g_steady && !g_lockstep) || kind != 'N' ||
        count != 6 || instances != 1 || !ctx) {
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
    g_lsBound = false;
    if (!ctx || (!g_steady && !g_lockstep)) return;
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
            if (g_lockstep) {
                // Freeze THIS draw's four content textures. All four or
                // none: a partial freeze is a new per-eye split.
                ID3D11ShaderResourceView* srv[4] = {};
                ctx->PSGetShaderResources(0, 4, srv);
                ID3D11Device* dev = nullptr;
                ctx->GetDevice(&dev);
                bool all = dev != nullptr;
                for (int i = 0; i < 4 && all; ++i) {
                    g_lsHave[i] = false;
                    if (!srv[i]) continue;   // an unbound slot stays unbound
                    ID3D11Resource* res = nullptr;
                    srv[i]->GetResource(&res);
                    ID3D11Texture2D* tex = nullptr;
                    if (res) {
                        res->QueryInterface(__uuidof(ID3D11Texture2D),
                                            reinterpret_cast<void**>(&tex));
                        res->Release();
                    }
                    if (!tex) { all = false; break; }
                    D3D11_TEXTURE2D_DESC d{};
                    tex->GetDesc(&d);
                    if (d.SampleDesc.Count != 1) {
                        tex->Release();
                        all = false;
                        break;
                    }
                    if (!g_lsTex[i] ||
                        memcmp(&d, &g_lsDesc[i], sizeof(d)) != 0) {
                        if (g_lsView[i]) {
                            g_lsView[i]->Release();
                            g_lsView[i] = nullptr;
                        }
                        if (g_lsTex[i]) {
                            g_lsTex[i]->Release();
                            g_lsTex[i] = nullptr;
                        }
                        D3D11_TEXTURE2D_DESC cd = d;
                        cd.Usage = D3D11_USAGE_DEFAULT;
                        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                        cd.CPUAccessFlags = 0;
                        cd.MiscFlags = 0;
                        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                        srv[i]->GetDesc(&vd);
                        if (FAILED(dev->CreateTexture2D(&cd, nullptr,
                                                        &g_lsTex[i])) ||
                            FAILED(dev->CreateShaderResourceView(
                                g_lsTex[i], &vd, &g_lsView[i]))) {
                            if (g_lsTex[i]) {
                                g_lsTex[i]->Release();
                                g_lsTex[i] = nullptr;
                            }
                            g_lsView[i] = nullptr;
                            tex->Release();
                            all = false;
                            break;
                        }
                        g_lsDesc[i] = d;
                    }
                    ctx->CopyResource(g_lsTex[i], tex);
                    tex->Release();
                    g_lsHave[i] = true;
                }
                if (dev) dev->Release();
                for (int i = 0; i < 4; ++i) {
                    if (srv[i]) srv[i]->Release();
                }
                if (!all) {
                    for (int i = 0; i < 4; ++i) g_lsHave[i] = false;
                    if (!g_lsFailNoted) {
                        g_lsFailNoted = true;
                        Log::get().note(
                            "fss reveal: a lockstep input could not be "
                            "copied (unbindable or multisampled); the "
                            "second eye draws stock. Said once.");
                    }
                }
            }
            return;   // eye A draws stock, always
        }

        if (g_occurrence != 2) return;

        if (g_lockstep) {
            // The frozen inputs, if occurrence 1 produced a full set. The
            // b1 substitution below completes the byte-identical read.
            bool any = false;
            for (int i = 0; i < 4; ++i) {
                if (g_lsHave[i]) { any = true; break; }
            }
            if (any) {
                ctx->PSGetShaderResources(0, 4, g_lsDisplaced);
                ID3D11ShaderResourceView* set[4] = {
                    g_lsHave[0] ? g_lsView[0] : g_lsDisplaced[0],
                    g_lsHave[1] ? g_lsView[1] : g_lsDisplaced[1],
                    g_lsHave[2] ? g_lsView[2] : g_lsDisplaced[2],
                    g_lsHave[3] ? g_lsView[3] : g_lsDisplaced[3],
                };
                ctx->PSSetShaderResources(0, 4, set);
                g_lsBound = true;
                ++g_lsApplied;
                if (!g_lsNoted) {
                    g_lsNoted = true;
                    Log::get().note(
                        "fss reveal: lockstep engaged -- the second eye's "
                        "composite reads byte-identical copies of the "
                        "first eye's four inputs and its scene constants. "
                        "The two panels cannot differ.");
                }
            }
        }

        if (!g_snapshotValid || !g_sceneCbBytes) return;

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
    if (g_lsBound && ctx) {
        g_lsBound = false;
        ctx->PSSetShaderResources(0, 4, g_lsDisplaced);
        for (int i = 0; i < 4; ++i) {
            if (g_lsDisplaced[i]) {
                g_lsDisplaced[i]->Release();
                g_lsDisplaced[i] = nullptr;
            }
        }
    }
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
    lsRelease();
}

}  // namespace edvr

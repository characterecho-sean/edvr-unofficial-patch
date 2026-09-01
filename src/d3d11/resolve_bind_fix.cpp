#include "resolve_bind_fix.h"

#include <windows.h>

#include <string>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "exposure_fix.h"   // lookupShaderHash: the shared shader registry

namespace edvr {
namespace {

// The deferred resolve's PIXEL shader -- the same content hash
// resolve_probe.cpp matches on, measured 2026-08-30 from a field shader
// dump and confirmed by disassembly. Duplicated rather than shared because
// the two modules stand alone: one is a probe somebody arms for an
// evening, this is a fix that ships on.
constexpr uint64_t kResolvePs = 0x7CECABDE34FFBE9EULL;

FaultBudget g_budget("resolveBind", 8);

bool g_on = false;

// The remembered quad. A reference is held: the game reuses one buffer for
// both eyes and every frame (the healthy captures show the same token all
// round), and holding it means a lend can never name a dead object even if
// the game releases its own reference between sightings. Refreshed on
// every resolve draw that carries a buffer.
ID3D11Buffer* g_vb = nullptr;
UINT g_stride = 0;
UINT g_offset = 0;

bool g_lent = false;

uint32_t g_healed = 0;        // lends this session
uint32_t g_seenEmpty = 0;     // empty sightings, healed or not
bool g_healNoted = false;
bool g_dryNoted = false;

void releaseCache() {
    if (g_vb) {
        g_vb->Release();
        g_vb = nullptr;
    }
    g_stride = 0;
    g_offset = 0;
}

}  // namespace

void resolveBindConfigure(Config& cfg) {
    const std::string v = cfg.getString("fix.scanner_body", "on");
    const bool want = !(v == "off" || v == "0");
    // A session that STARTS with the fix off used to say nothing at all --
    // the 2026-09-01 fix-off capture only revealed its own state through
    // the ini in the zip. One line at first sight, whatever the state, so
    // every bundle names it.
    static bool announced = false;
    if (want == g_on) {
        if (!announced && !g_on) {
            announced = true;
            Log::get().note("scanner body fix off: resolve draws are left "
                            "as the game issues them.");
        }
        announced = true;
        return;
    }
    announced = true;
    g_on = want;
    if (g_on) {
        Log::get().note(
            "scanner body fix ON: a lighting-resolve draw that arrives with "
            "no vertex buffer is drawn with the buffer the other eye's "
            "resolve used, put back empty afterwards. On a machine that "
            "never drops the binding this never engages.");
    } else {
        Log::get().note("scanner body fix off: resolve draws are left as "
                        "the game issues them.");
        releaseCache();
    }
}

bool resolveBindWants() { return g_on; }

bool resolveBindOnEyeDraw(ID3D11DeviceContext* ctx) {
    if (!g_on || !ctx) return false;
    bool match = false;
    guardedBudget(g_budget, [&] {
        ID3D11PixelShader* ps = nullptr;
        ctx->PSGetShader(&ps, nullptr, nullptr);
        if (ps) {
            match = lookupShaderHash(ps) == kResolvePs;
            ps->Release();
        }
    });
    return match;
}

void resolveBindBegin(ID3D11DeviceContext* ctx) {
    if (!g_on || !ctx) return;
    guardedBudget(g_budget, [&] {
        ID3D11Buffer* vb = nullptr;
        UINT stride = 0, offset = 0;
        ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
        if (vb) {
            // A healthy sighting refreshes the cache. The Get's reference
            // transfers straight into it.
            if (g_vb) g_vb->Release();
            g_vb = vb;
            g_stride = stride;
            g_offset = offset;
            return;
        }
        ++g_seenEmpty;
        if (!g_vb) {
            // Nothing to lend yet -- a resolve with no buffer before any
            // resolve with one. Recorded, because a session where this is
            // all that happens is a session the fix could not help.
            if (!g_dryNoted) {
                g_dryNoted = true;
                Log::get().note(
                    "scanner body fix: a resolve draw arrived with no "
                    "vertex buffer before any resolve had shown one -- "
                    "nothing to lend it. Counting; the tally is in the "
                    "shutdown line.");
            }
            return;
        }
        ctx->IASetVertexBuffers(0, 1, &g_vb, &g_stride, &g_offset);
        g_lent = true;
        ++g_healed;
        if (!g_healNoted) {
            g_healNoted = true;
            Log::get().note(
                "scanner body fix: ENGAGED -- a lighting-resolve draw "
                "arrived with no vertex buffer and was drawn with the other "
                "eye's (stride %u, offset %u). This is the black-body "
                "mechanism; if the body still looks wrong in one eye, say "
                "so, because from here it should not.",
                g_stride, g_offset);
        }
    });
}

void resolveBindEnd(ID3D11DeviceContext* ctx) {
    if (!g_lent || !ctx) return;
    g_lent = false;
    guardedBudget(g_budget, [&] {
        // The game left the slot empty; leave it exactly as found. Later
        // draws that need a buffer bind their own, but a draw relying on
        // "still empty" must find it empty.
        ID3D11Buffer* none = nullptr;
        UINT zero = 0;
        ctx->IASetVertexBuffers(0, 1, &none, &zero, &zero);
    });
}

void resolveBindShutdown() {
    if (g_seenEmpty) {
        Log::get().note(
            "scanner body fix: %u resolve draw(s) arrived with no vertex "
            "buffer this session; %u were drawn with the lent one. A gap "
            "between the numbers is draws seen before the first healthy "
            "sighting.",
            g_seenEmpty, g_healed);
    }
    releaseCache();
    g_lent = false;
}

}  // namespace edvr

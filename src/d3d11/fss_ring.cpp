#include "fss_ring.h"

#include <windows.h>

#include <d3d11.h>

#include <string>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "exposure_fix.h"   // lookupShaderHash

namespace edvr {
namespace {

// The three ring-draw families, by vertex-shader content hash, with how many
// times each runs PER EYE per frame (measured 30/30 in the round-16/17
// captures: the n=4 quad and the n=14 mesh once per eye, the f60 writer
// twice per eye). Occurrences 1..k are the first eye, k+1..2k the second;
// anything past 2k passes through untouched.
struct Family {
    uint64_t vh;
    uint8_t  perEye;
};
constexpr Family kFamilies[] = {
    {0x7E38A6AA1269C901ull, 1},
    {0x0357BBB2DEE43C1Full, 1},
    {0x53211E8C072CD02Eull, 2},
};
constexpr uint32_t kFamilyCount =
    static_cast<uint32_t>(sizeof(kFamilies) / sizeof(kFamilies[0]));
constexpr uint32_t kMaxPerEye = 2;
constexpr uint32_t kSlots = 8;

// 0 = stock, 1 = "second" (first eye receives, second lends),
// 2 = "first" (second eye receives, first lends).
uint8_t g_mode = 0;

// The learned SRV sets, AddRef-held across frames: [family][j][slot], where
// j is the occurrence index within the lending eye.
ID3D11ShaderResourceView* g_learned[kFamilyCount][kMaxPerEye][kSlots] = {};
bool g_learnedValid[kFamilyCount][kMaxPerEye] = {};

// Per-frame occurrence counters, and the pending apply between OnEyeDraw
// and Begin (the draw path is single-threaded; the pattern every fss module
// here uses).
uint8_t  g_occ[kFamilyCount] = {};
uint32_t g_pendingFam = 0;
uint32_t g_pendingJ = 0;

// Begin/End state: the displaced live views, to restore.
bool                      g_engaged = false;
ID3D11ShaderResourceView* g_displaced[kSlots] = {};

uint64_t g_applied = 0;
bool     g_engagedNoted = false;
bool     g_learnNoted = false;

FaultBudget g_budget("fssRing", 8);

void releaseLearned() {
    for (uint32_t f = 0; f < kFamilyCount; ++f) {
        for (uint32_t j = 0; j < kMaxPerEye; ++j) {
            for (uint32_t s = 0; s < kSlots; ++s) {
                if (g_learned[f][j][s]) {
                    g_learned[f][j][s]->Release();
                    g_learned[f][j][s] = nullptr;
                }
            }
            g_learnedValid[f][j] = false;
        }
    }
}

}  // namespace

void fssRingConfigure(Config& cfg) {
    const std::string m = cfg.getString("experimental.fss_ring_feed", "stock");
    uint8_t mode = 0;
    if (m == "stock") {
        mode = 0;
    } else if (m == "second") {
        mode = 1;
    } else if (m == "first") {
        mode = 2;
    } else {
        Log::get().note("fss_ring_feed \"%s\" is not stock, second or first; "
                        "running stock.", m.c_str());
    }
    if (mode == g_mode) return;
    g_mode = mode;
    g_engagedNoted = false;
    g_learnNoted = false;
    releaseLearned();
    if (g_mode) {
        Log::get().note(
            "fss ring ARMED: the %s eye's ring draws run with the %s eye's "
            "sampled inputs, restored after every draw. The zoomed body sits "
            "at optical infinity, so one eye's imagery is correct for both "
            "-- if the black squares die, the lagging per-eye input set is "
            "measured and this is the fix's shape. Clear to restore.",
            g_mode == 1 ? "FIRST" : "SECOND", g_mode == 1 ? "SECOND" : "FIRST");
    } else {
        Log::get().note("fss ring: stock; each eye's ring draws read their "
                        "own inputs (%llu draws were fed while armed).",
                        static_cast<unsigned long long>(g_applied));
    }
}

bool fssRingWantsDraws() { return g_mode != 0; }

bool fssRingOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                      uint32_t instances) {
    if (!g_mode || !ctx) return false;
    // The cheap gate first: every family is a tiny fixed-geometry draw.
    if (!((kind == 'N' && instances == 1 && (count == 3 || count == 4)) ||
          (kind == 'X' && count == 14))) {
        return false;
    }
    uint64_t h = 0;
    guardedBudget(g_budget, [&] {
        ID3D11VertexShader* vs = nullptr;
        ctx->VSGetShader(&vs, nullptr, nullptr);
        h = lookupShaderHash(vs);
        if (vs) vs->Release();
    });
    for (uint32_t f = 0; f < kFamilyCount; ++f) {
        if (kFamilies[f].vh != h) continue;
        const uint8_t k = kFamilies[f].perEye;
        const uint8_t occ = ++g_occ[f];
        if (occ > 2 * k) return false;   // unexpected extra: leave alone
        const uint8_t eye = static_cast<uint8_t>((occ - 1) / k);
        const uint8_t j = static_cast<uint8_t>((occ - 1) % k);
        const uint8_t lender = (g_mode == 1) ? 1 : 0;
        if (eye == lender) {
            // Learn, inline: a read costs nothing the draw can notice.
            guardedBudget(g_budget, [&] {
                ID3D11ShaderResourceView* live[kSlots] = {};
                ctx->PSGetShaderResources(0, kSlots, live);
                for (uint32_t s = 0; s < kSlots; ++s) {
                    if (g_learned[f][j][s]) g_learned[f][j][s]->Release();
                    g_learned[f][j][s] = live[s];   // keep the Get's ref
                }
                g_learnedValid[f][j] = true;
                if (!g_learnNoted) {
                    g_learnNoted = true;
                    Log::get().note("fss ring: lender inputs learned; the "
                                    "receiving eye is fed from the next "
                                    "matching draw on.");
                }
            });
            return false;
        }
        if (!g_learnedValid[f][j]) return false;
        g_pendingFam = f;
        g_pendingJ = j;
        return true;
    }
    return false;
}

void fssRingBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    if (!ctx || !g_mode) return;
    guardedBudget(g_budget, [&] {
        ctx->PSGetShaderResources(0, kSlots, g_displaced);
        ID3D11ShaderResourceView* feed[kSlots];
        for (uint32_t s = 0; s < kSlots; ++s) {
            feed[s] = g_learned[g_pendingFam][g_pendingJ][s];
        }
        ctx->PSSetShaderResources(0, kSlots, feed);
        g_engaged = true;
        ++g_applied;
        if (!g_engagedNoted) {
            g_engagedNoted = true;
            Log::get().note(
                "fss ring: engaged -- the receiving eye's ring draws are "
                "running on the lending eye's inputs, restored after each.");
        }
    });
}

void fssRingEnd(ID3D11DeviceContext* ctx) {
    if (!g_engaged || !ctx) return;
    g_engaged = false;
    ctx->PSSetShaderResources(0, kSlots, g_displaced);
    for (uint32_t s = 0; s < kSlots; ++s) {
        if (g_displaced[s]) {
            g_displaced[s]->Release();
            g_displaced[s] = nullptr;
        }
    }
}

void fssRingFrameBoundary() {
    for (uint32_t f = 0; f < kFamilyCount; ++f) g_occ[f] = 0;
}

void fssRingShutdown() { releaseLearned(); }

}  // namespace edvr

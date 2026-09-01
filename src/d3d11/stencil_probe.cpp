#include "stencil_probe.h"

#include <windows.h>

#include <cstdlib>
#include <string>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "exposure_fix.h"   // lookupShaderHash: the shared shader registry

namespace edvr {
namespace {

FaultBudget g_budget("stencilProbe", 8);

uint64_t g_vsHash = 0;      // zero is off
uint32_t g_ref = 0;

ID3D11DepthStencilState* g_savedDs = nullptr;
UINT g_savedRef = 0;
bool g_engaged = false;

bool g_engagedNoted = false;
uint32_t g_applied = 0;

// What the game's own reference was on the matched draw, counted by value.
// This is the measurement the census could only make with a volunteer
// pressing a key at the right moment: the split between the good number and
// the bad one, over a whole session, from the log alone.
uint32_t g_sameCount = 0;    // the game already had our reference
uint32_t g_changedCount = 0; // it had a different one and we replaced it
uint32_t g_firstOther = 0;   // the first different value seen, for the log
bool g_haveOther = false;

void releaseSaved() {
    if (g_savedDs) {
        g_savedDs->Release();
        g_savedDs = nullptr;
    }
}

}  // namespace

// "vs:HASH:REF" -- the vertex shader the census prints as vh=, and the
// stencil reference to issue its draws with. Parsed whole or refused whole:
// a half-applied spec would run a probe nobody asked for and read as a
// result.
void stencilProbeConfigure(Config& cfg) {
    const std::string spec = cfg.getString("advanced.stencil_probe", "");

    uint64_t wantHash = 0;
    uint32_t wantRef = 0;
    bool ok = true;

    if (!spec.empty() && spec != "off") {
        const char* p = spec.c_str();
        while (*p == ' ' || *p == '\t') ++p;
        if ((p[0] == 'v' || p[0] == 'V') && (p[1] == 's' || p[1] == 'S') &&
            p[2] == ':') {
            char* end = nullptr;
            const unsigned long long h = _strtoui64(p + 3, &end, 16);
            if (end == p + 3 || h == 0 || *end != ':') {
                ok = false;
            } else {
                const char* q = end + 1;
                char* rend = nullptr;
                const unsigned long r = strtoul(q, &rend, 10);
                while (rend && (*rend == ' ' || *rend == '\t')) ++rend;
                if (rend == q || *rend || r > 255) {
                    ok = false;
                } else {
                    wantHash = static_cast<uint64_t>(h);
                    wantRef = static_cast<uint32_t>(r);
                }
            }
        } else {
            ok = false;
        }
    }

    if (!ok) {
        Log::get().note(
            "stencil probe: \"%s\" is not understood. One term, "
            "vs:HASH:REF -- the 16-digit hex the census logs as vh=, then a "
            "colon, then the stencil reference as a plain number 0 to 255. "
            "\"vs:9BFC7FD232328391:8\" is the shape. off is off. Refused; "
            "the game's own reference stands.",
            spec.c_str());
        wantHash = 0;
        wantRef = 0;
    }

    if (wantHash == g_vsHash && wantRef == g_ref) return;
    g_vsHash = wantHash;
    g_ref = wantRef;
    g_engagedNoted = false;
    g_applied = 0;
    g_sameCount = 0;
    g_changedCount = 0;
    g_haveOther = false;
    g_firstOther = 0;

    if (g_vsHash) {
        Log::get().note(
            "stencil probe ARMED: every eye draw running vertex shader "
            "%016llX is issued with stencil reference %u. The game's own "
            "depth-stencil state object is re-bound UNCHANGED -- its "
            "comparisons, masks and pass-ops are exactly as the game set "
            "them -- and only the reference beside it differs, so anything "
            "that changes can be attributed to that number alone. Both are "
            "put back after every draw.",
            static_cast<unsigned long long>(g_vsHash), g_ref);
    } else {
        Log::get().note("stencil probe: off, the game's own reference.");
    }
}

bool stencilProbeWantsDraws() { return g_vsHash != 0; }

bool stencilProbeOnEyeDraw(ID3D11DeviceContext* ctx) {
    if (!g_vsHash || !ctx) return false;
    bool match = false;
    guardedBudget(g_budget, [&] {
        ID3D11VertexShader* vs = nullptr;
        ctx->VSGetShader(&vs, nullptr, nullptr);
        if (vs) {
            match = lookupShaderHash(vs) == g_vsHash;
            vs->Release();
        }
    });
    return match;
}

void stencilProbeBegin(ID3D11DeviceContext* ctx) {
    if (!g_vsHash || !ctx) return;
    guardedBudget(g_budget, [&] {
        ctx->OMGetDepthStencilState(&g_savedDs, &g_savedRef);

        // The count is the point of this line as much as the override is.
        // The failing state is intermittent -- roughly half the recorded
        // passes -- so a session that never once reports the other value was
        // a session that never reproduced the fault, and a result from it
        // means nothing. That is worth knowing from the log rather than from
        // asking someone what they thought they saw.
        if (g_savedRef == g_ref) {
            ++g_sameCount;
        } else {
            ++g_changedCount;
            if (!g_haveOther) {
                g_haveOther = true;
                g_firstOther = static_cast<uint32_t>(g_savedRef);
            }
        }

        // Re-bind the game's OWN state object. Null is a state the game can
        // legitimately be in -- it means the D3D11 default, whose stencil
        // test is off -- and passing it back through is correct rather than
        // something to guard against.
        ctx->OMSetDepthStencilState(g_savedDs, g_ref);
        g_engaged = true;
        ++g_applied;

        if (!g_engagedNoted) {
            g_engagedNoted = true;
            Log::get().note(
                "stencil probe: engaged -- THE GAME'S reference on this draw "
                "was %u, OURS is %u. Counting silently from here; the tally "
                "of each is reported when this is cleared or the session "
                "ends.",
                static_cast<unsigned>(g_savedRef), g_ref);
        }
    });
}

void stencilProbeEnd(ID3D11DeviceContext* ctx) {
    if (!g_engaged || !ctx) return;
    g_engaged = false;
    guardedBudget(g_budget, [&] {
        // Restore even where the saved pointer is null: leaving ours bound
        // would apply this reference to every later draw in the frame, which
        // is a second experiment nobody asked for.
        ctx->OMSetDepthStencilState(g_savedDs, g_savedRef);
        releaseSaved();
    });
    releaseSaved();
}

void stencilProbeShutdown() {
    if (g_applied) {
        Log::get().note(
            "stencil probe: %u draw(s) issued with reference %u. The game "
            "had already chosen %u on %u of them and something else on %u%s. "
            "A run whose 'something else' count is zero never reproduced the "
            "state this is testing, whatever the picture looked like.",
            g_applied, g_ref, g_ref, g_sameCount, g_changedCount,
            g_haveOther ? "" : " (none seen)");
        if (g_haveOther) {
            Log::get().note(
                "stencil probe: the first other reference seen was %u.",
                g_firstOther);
        }
    }
    releaseSaved();
    g_engaged = false;
    g_applied = 0;
    g_sameCount = 0;
    g_changedCount = 0;
    g_haveOther = false;
}

}  // namespace edvr

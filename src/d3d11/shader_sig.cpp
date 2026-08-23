#include "shader_sig.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "../common/guard.h"

namespace edvr {
namespace {

// Vertex shaders the game creates run to a few thousand over a session, but
// only the handful bound to draws anybody recognises are ever asked about.
// 512 entries is far past what has been needed and the table simply stops
// growing past it -- a shader that misses out answers "unknown", which is
// the same answer it would give if this module did not exist.
constexpr uint32_t kMaxEntries = 512;
constexpr uint32_t kSigChars = 192;

struct Entry {
    void* shader = nullptr;
    char  sig[kSigChars] = {};
};

Entry            g_tab[kMaxEntries];
uint32_t         g_count = 0;
CRITICAL_SECTION g_lock;
bool             g_lockReady = false;

FaultBudget g_budget("shaderSig.parse", 8);

struct Init {
    Init() {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }
} g_init;

uint32_t rd32(const unsigned char* p) {
    uint32_t v = 0;
    memcpy(&v, p, sizeof(v));
    return v;
}

// "xyzw" for the bits set in a component mask, "-" for none.
void maskText(unsigned char mask, char* out, size_t n) {
    char buf[5] = {};
    uint32_t w = 0;
    if (mask & 1) buf[w++] = 'x';
    if (mask & 2) buf[w++] = 'y';
    if (mask & 4) buf[w++] = 'z';
    if (mask & 8) buf[w++] = 'w';
    _snprintf_s(out, n, _TRUNCATE, "%s", w ? buf : "-");
}

// Walk a DXBC container to its ISGN chunk and render the input signature.
//
// Every offset is checked against len before it is read. This parses the
// game's own bytecode, which is not hostile, but it is parsed on the asset
// streaming thread while the game is running and a malformed read here would
// be indistinguishable from any other crash in the process.
bool parseIsgn(const unsigned char* b, size_t len, char* out, size_t outN) {
    if (len < 32 || memcmp(b, "DXBC", 4) != 0) return false;
    const uint32_t chunks = rd32(b + 28);
    if (chunks == 0 || chunks > 32) return false;
    if (len < 32u + chunks * 4u) return false;

    for (uint32_t c = 0; c < chunks; ++c) {
        const uint32_t off = rd32(b + 32 + c * 4);
        if (off + 8 > len) continue;
        // ISGN is the input signature; ISG1 is its later form, same layout
        // for the fields this reads.
        const bool isgn = memcmp(b + off, "ISGN", 4) == 0 ||
                          memcmp(b + off, "ISG1", 4) == 0;
        if (!isgn) continue;

        const uint32_t size = rd32(b + off + 4);
        const unsigned char* d = b + off + 8;         // chunk data
        if (off + 8 + size > len || size < 8) return false;

        const uint32_t elems = rd32(d);
        if (elems == 0 || elems > 32) return false;
        if (8u + elems * 24u > size) return false;

        uint32_t w = 0;
        out[0] = '\0';
        for (uint32_t e = 0; e < elems; ++e) {
            const unsigned char* el = d + 8 + e * 24;
            const uint32_t nameOff = rd32(el);
            const uint32_t semIdx = rd32(el + 4);
            const uint32_t reg = rd32(el + 16);
            const unsigned char has = el[20];
            const unsigned char used = el[21];

            // The name is an offset from the start of the chunk DATA, and it
            // has to be NUL-terminated inside the chunk to be a name at all.
            if (nameOff >= size) return false;
            const char* name = reinterpret_cast<const char*>(d + nameOff);
            uint32_t nameLen = 0;
            while (nameOff + nameLen < size && name[nameLen] != '\0') ++nameLen;
            if (nameOff + nameLen >= size) return false;

            char hasT[8], usedT[8];
            maskText(has, hasT, sizeof(hasT));
            maskText(used, usedT, sizeof(usedT));
            const int n = _snprintf_s(out + w, outN - w, _TRUNCATE,
                                      "%s%.*s%u r%u has=%s used=%s",
                                      w ? "; " : "", static_cast<int>(nameLen),
                                      name, semIdx, reg, hasT, usedT);
            if (n < 0) break;   // truncated: what fits is still worth having
            w += static_cast<uint32_t>(n);
        }
        return w > 0;
    }
    return false;
}

}  // namespace

void shaderSigRegister(void* shader, const void* bytecode, size_t len) {
    if (!shader || !bytecode || !len || !g_lockReady) return;

    char sig[kSigChars];
    bool ok = false;
    guardedBudget(g_budget, [&] {
        ok = parseIsgn(static_cast<const unsigned char*>(bytecode), len, sig,
                       sizeof(sig));
    });
    if (!ok) return;

    EnterCriticalSection(&g_lock);
    if (g_count < kMaxEntries) {
        // Shader pointers can be reused after a release, so an existing entry
        // is overwritten rather than kept: the newest creation is the truth
        // about what that address is now.
        uint32_t slot = g_count;
        for (uint32_t i = 0; i < g_count; ++i) {
            if (g_tab[i].shader == shader) { slot = i; break; }
        }
        g_tab[slot].shader = shader;
        _snprintf_s(g_tab[slot].sig, kSigChars, _TRUNCATE, "%s", sig);
        if (slot == g_count) ++g_count;
    }
    LeaveCriticalSection(&g_lock);
}

const char* shaderSigOf(void* shader) {
    if (!shader || !g_lockReady) return nullptr;
    const char* out = nullptr;
    EnterCriticalSection(&g_lock);
    for (uint32_t i = 0; i < g_count; ++i) {
        if (g_tab[i].shader == shader) { out = g_tab[i].sig; break; }
    }
    LeaveCriticalSection(&g_lock);
    return out;
}

}  // namespace edvr

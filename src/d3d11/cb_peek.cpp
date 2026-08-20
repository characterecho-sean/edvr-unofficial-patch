#include "cb_peek.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/log.h"
#include "../common/timing.h"
#include "binding_shadow.h"

namespace edvr {
namespace {

// The family, matcher v2 (cb_peek's first sweep corrected v1; see the
// commit history). v3's change is WHAT is peeked, not what is matched: the
// IA tails proved the flare draws carry NO vertex constant buffer at all --
// 8-byte corner vertices, per-sprite data in a STRUCTURED BUFFER read by
// the vertex shader. The peek now asks the context directly which VS
// resources the matched draw has bound (the census reads IA state the same
// way), takes the first that resolves to a buffer, and shadows THAT.
constexpr char     kKind = 'X';
constexpr uint32_t kMinIndices = 64;

// Sprite arrays run to hundreds of kilobytes; the shadow must hold a whole
// one or the diff lies about what changed.
constexpr uint32_t kMaxBytes = 262144;
constexpr uint32_t kMaxFloats = kMaxBytes / 4;
constexpr uint64_t kSampleMs = 1000;
constexpr uint32_t kMaxLines = 400;
constexpr float    kChangeEps = 1e-4f;

// How much of a big buffer the first capture prints, and how many changed
// values a delta line names. A record dump names the fields; the sweep's
// job is only to say WHICH move with the head.
constexpr uint32_t kFirstDumpFloats = 64;
constexpr uint32_t kDeltaPerLine = 16;

bool     g_enabled = false;
uint32_t g_wantN = 0;              // cb_peek_n: learn only this index count;
                                   // 0 keeps first-match-wins
void*    g_target = nullptr;       // the buffer RESOURCE, raw pointer,
                                   // compared at Map and never dereferenced
uint32_t g_targetBytes = 0;
uint32_t g_targetStride = 0;       // StructureByteStride, 0 when none
bool     g_dumpedFull = false;
uint64_t g_lastSampleMs = 0;
uint32_t g_lines = 0;

// The family roster: every DISTINCT data shape that matched, logged once.
// The first roster keyed on index count -- but the corona fans
// RE-TESSELLATE, so counts vary per frame. The second keyed on the bound
// buffer pointers -- but the engine rotates its buffers, so pointers churn
// too, and all 24 slots filled with copies of two members inside two
// seconds. What the churn itself proved stable is the SHAPE of the data:
// sizes and strides held constant across every churned pointer. The
// roster keys on shape, logs when a NEW one appears, and hard-caps its
// lines besides.
struct RosterKey {
    uint32_t vb0Bytes;
    uint32_t stride0;
    uint32_t vb1Bytes;
    uint32_t stride1;
    uint32_t srvBytes;
    uint32_t srvStride;
};
RosterKey g_roster[24];
uint32_t  g_rosterCount = 0;
uint32_t  g_rosterLines = 0;
constexpr uint32_t kRosterMaxLines = 32;

// The aim: learn the first matched draw whose candidate data buffer is at
// least this big. The 768-byte marker row is excluded by any real
// threshold; a flare's array is not.
uint32_t g_wantMinBytes = 0;

static float g_last[kMaxFloats];

}  // namespace

void cbPeekConfigure(Config& cfg) {
    const bool was = g_enabled;
    g_enabled = cfg.getBool("advanced.cb_peek", false);
    const int wantN = cfg.getIntInRange("advanced.cb_peek_n", 0, 0, 10000000);
    g_wantN = static_cast<uint32_t>(wantN);
    const int minB = cfg.getIntInRange("advanced.cb_peek_min_bytes", 0, 0,
                                       100000000);
    g_wantMinBytes = static_cast<uint32_t>(minB);
    if (g_enabled && !was) {
        g_target = nullptr;
        g_dumpedFull = false;
        g_lines = 0;
        g_rosterCount = 0;
        g_rosterLines = 0;
        if (g_wantN || g_wantMinBytes) {
            Log::get().note("cb peek: ON, aimed -- n=%u, min data bytes %u "
                            "(zero means unconstrained). Park at the star "
                            "and run the sweep: straight, roll, straight, "
                            "yaw off-centre, straight, pitch, straight.",
                            g_wantN, g_wantMinBytes);
        } else {
            Log::get().note("cb peek: ON, roster mode -- every distinct "
                            "matched binding signature logs once, including "
                            "the IA vertex streams. Read the roster, set "
                            "cb_peek_min_bytes (or cb_peek_n) to aim, "
                            "toggle off and on, then run the sweep.");
        }
    }
    if (!g_enabled && was) g_target = nullptr;
}

bool cbPeekEnabled() { return g_enabled; }

void* cbPeekTarget() { return g_target; }

void cbPeekOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                     uint32_t instances) {
    if (!g_enabled || g_target || !ctx || kind != kKind ||
        count < kMinIndices) {
        return;
    }

    uint32_t eyeW = 0, eyeH = 0;
    if (!eyeTextureSize(&eyeW, &eyeH)) return;
    ResourceInfo depth;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv0), &depth) ||
        !depth.isTexture2D || depth.a != eyeW || depth.b != eyeH) {
        return;
    }

    // The IA vertex streams, asked of the context the same way. Slot 1 is
    // where an instanced sprite pipeline keeps its per-sprite records --
    // the stream the census tail (slot 0) and the VS-resource query both
    // structurally cannot see, and the only channel left that can be
    // feeding sprites whose draws bind no constants and no VS resources.
    ID3D11Buffer* vbs[2] = {};
    UINT strides[2] = {};
    UINT offsets[2] = {};
    ctx->IAGetVertexBuffers(0, 2, vbs, strides, offsets);

    const bool roster = !g_wantN && !g_wantMinBytes;
    if (roster) {
        ID3D11ShaderResourceView* srv0 = nullptr;
        ctx->VSGetShaderResources(0, 1, &srv0);
        RosterKey key{};
        key.stride0 = strides[0];
        key.stride1 = strides[1];
        ResourceInfo info;
        if (vbs[0] && bindingResolveResource(vbs[0], &info)) {
            key.vb0Bytes = info.a;
        }
        if (vbs[1] && bindingResolveResource(vbs[1], &info)) {
            key.vb1Bytes = info.a;
        }
        bool srvIsBuf = false;
        if (srv0 && bindingResolve(srv0, &info)) {
            key.srvBytes = info.a;
            key.srvStride = info.b;
            srvIsBuf = info.isBuffer;
        }
        bool seen = false;
        for (uint32_t i = 0; i < g_rosterCount && !seen; ++i) {
            seen = memcmp(&g_roster[i], &key, sizeof(key)) == 0;
        }
        if (!seen && g_rosterCount < 24 && g_rosterLines < kRosterMaxLines) {
            g_roster[g_rosterCount++] = key;
            ++g_rosterLines;
            char desc[200];
            int at = _snprintf_s(desc, sizeof(desc), _TRUNCATE, "n=%u i=%u",
                                 count, instances);
            if (key.vb0Bytes) {
                at += _snprintf_s(desc + at, sizeof(desc) - at, _TRUNCATE,
                                  " vb0=%uB/sd%u", key.vb0Bytes, key.stride0);
            }
            if (key.vb1Bytes) {
                at += _snprintf_s(desc + at, sizeof(desc) - at, _TRUNCATE,
                                  " vb1=%uB/sd%u", key.vb1Bytes, key.stride1);
            }
            if (key.srvBytes) {
                at += _snprintf_s(desc + at, sizeof(desc) - at, _TRUNCATE,
                                  " vssrv0=%s%u/%u", srvIsBuf ? "buf" : "tex",
                                  key.srvBytes, key.srvStride);
            }
            Log::get().note("cb peek roster: %s", desc);
        }
        if (srv0) srv0->Release();
        for (int i = 0; i < 2; ++i) {
            if (vbs[i]) vbs[i]->Release();
        }
        return;
    }

    // Aimed: n filter first when set.
    if (g_wantN && count != g_wantN) {
        for (int i = 0; i < 2; ++i) {
            if (vbs[i]) vbs[i]->Release();
        }
        return;
    }

    // The candidate data source, most-likely first: the second vertex
    // stream, then a VS-SRV buffer, then the FIRST stream (the roster
    // showed members whose records ride stream 0 per-vertex, wide strides
    // and no other data bound), then the constant buffer. The min-bytes
    // aim applies to whichever is chosen.
    void*    resource = nullptr;
    uint32_t rbytes = 0;
    uint32_t rstride = 0;
    void*    vb0raw = nullptr;
    uint32_t vb0bytes = 0;
    if (vbs[1]) {
        ResourceInfo info;
        if (bindingResolveResource(vbs[1], &info) && info.isBuffer) {
            resource = vbs[1];
            rbytes = info.a;
            rstride = strides[1];
        }
    }
    if (vbs[0]) {
        ResourceInfo info;
        if (bindingResolveResource(vbs[0], &info) && info.isBuffer) {
            vb0raw = vbs[0];
            vb0bytes = info.a;
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (vbs[i]) vbs[i]->Release();   // raw pointer kept, reference not
    }

    const char* source = "IA-stream-1";

    // No second vertex stream: the VS-SRV path (the marker-row class),
    // then the constant buffer -- the peek serves every sprite pipeline
    // the family turns out to contain.
    if (!resource) {
        ID3D11ShaderResourceView* srvs[8] = {};
        ctx->VSGetShaderResources(0, 8, srvs);
        for (int i = 0; i < 8; ++i) {
            if (!srvs[i]) continue;
            ResourceInfo info;
            if (!resource && bindingResolve(srvs[i], &info) && info.isBuffer) {
                ID3D11Resource* res = nullptr;
                srvs[i]->GetResource(&res);
                if (res) {
                    resource = res;   // raw pointer kept; reference dropped.
                                      // The Map hook GetTypes before GetDesc,
                                      // the compositeCb discipline.
                    rbytes = info.a;
                    rstride = info.b;
                    source = "VS-SRV";
                    res->Release();
                }
            }
            srvs[i]->Release();
        }
    }
    if (!resource && vb0raw) {
        resource = vb0raw;
        rbytes = vb0bytes;
        rstride = strides[0];
        source = "IA-stream-0";
    }
    if (!resource) {
        void* cb = bindingGet(BindSlot::VsCb0);
        ResourceInfo info;
        if (cb && bindingResolveResource(cb, &info) && info.isBuffer) {
            resource = cb;
            rbytes = info.a;
            rstride = info.b;
            source = "VS-CB";
        }
    }
    if (!resource) return;
    if (g_wantMinBytes && rbytes < g_wantMinBytes) return;

    g_target = resource;
    g_targetBytes = rbytes;
    g_targetStride = rstride;
    g_dumpedFull = false;
    Log::get().note("cb peek: learned the sprite data of a matched draw "
                    "(n=%u) -- %s, %u bytes, stride %u. The next write "
                    "dumps its head; after that, only what changes.",
                    count, source, g_targetBytes, g_targetStride);
}

void cbPeekCapture(const void* data, uint32_t bytes) {
    if (!g_enabled || !data || g_lines >= kMaxLines) return;
    if (bytes > kMaxBytes) bytes = kMaxBytes;
    const uint32_t n = bytes / 4;
    if (n == 0) return;
    const float* f = static_cast<const float*>(data);
    const uint64_t now = nowMs();

    if (!g_dumpedFull) {
        g_dumpedFull = true;
        g_lastSampleMs = now;
        memcpy(g_last, f, n * 4);
        const uint32_t head = n < kFirstDumpFloats ? n : kFirstDumpFloats;
        for (uint32_t i = 0; i < head && g_lines < kMaxLines; i += 8) {
            char line[360];
            int at = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%u]", i);
            for (uint32_t k = i; k < i + 8 && k < head; ++k) {
                at += _snprintf_s(line + at, sizeof(line) - at, _TRUNCATE,
                                  " %.5g", static_cast<double>(f[k]));
            }
            ++g_lines;
            Log::get().note("CBP full %s", line);
        }
        return;
    }

    if (now - g_lastSampleMs < kSampleMs) return;
    g_lastSampleMs = now;

    // The delta pass over the WHOLE buffer: count everything, name the
    // first few. For a sprite array the indices divided by the structure
    // stride say which RECORD moved, which is how the head-tracking fields
    // are told from the crowd.
    char line[440];
    int at = 0;
    uint32_t changed = 0;
    uint32_t named = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const float d = f[i] - g_last[i];
        const float a = d < 0 ? -d : d;
        if (a > kChangeEps && !std::isnan(d)) {
            ++changed;
            if (named < kDeltaPerLine && at < 380) {
                ++named;
                at += _snprintf_s(line + at, sizeof(line) - at, _TRUNCATE,
                                  "%s[%u]=%.5g", at ? " " : "", i,
                                  static_cast<double>(f[i]));
            }
        }
        g_last[i] = f[i];
    }
    if (changed && g_lines < kMaxLines) {
        ++g_lines;
        Log::get().note("CBP d %u: %s", changed, line);
        if (g_lines == kMaxLines) {
            Log::get().note("cb peek: line budget spent; capture over. "
                            "Set cb_peek = 0 and back to 1 to run another.");
        }
    }
}

}  // namespace edvr

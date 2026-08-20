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
void*    g_target = nullptr;       // the buffer RESOURCE, raw pointer,
                                   // compared at Map and never dereferenced
uint32_t g_targetBytes = 0;
uint32_t g_targetStride = 0;       // StructureByteStride, 0 when none
bool     g_dumpedFull = false;
uint64_t g_lastSampleMs = 0;
uint32_t g_lines = 0;

static float g_last[kMaxFloats];

}  // namespace

void cbPeekConfigure(Config& cfg) {
    const bool was = g_enabled;
    g_enabled = cfg.getBool("advanced.cb_peek", false);
    if (g_enabled && !was) {
        g_target = nullptr;
        g_dumpedFull = false;
        g_lines = 0;
        Log::get().note("cb peek: ON -- the matched sprite draw's VERTEX-"
                        "STAGE data source (constant buffer, or the "
                        "structured buffer behind an 8-byte-vertex sprite "
                        "pipeline) will be learned and its writes dumped. "
                        "Park at a star; hold each of: straight, roll left, "
                        "straight, yaw the sun off-centre, straight, pitch "
                        "up, straight -- a few seconds each.");
    }
    if (!g_enabled && was) g_target = nullptr;
}

bool cbPeekEnabled() { return g_enabled; }

void* cbPeekTarget() { return g_target; }

void cbPeekOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                     uint32_t /*instances*/) {
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

    // Ask the context what the vertex stage is actually reading -- the
    // census's own pattern for IA state. Straight Get calls on unhooked
    // slots; every returned view carries a reference to release.
    ID3D11ShaderResourceView* srvs[8] = {};
    ctx->VSGetShaderResources(0, 8, srvs);
    int   chosen = -1;
    void* resource = nullptr;
    ResourceInfo chosenInfo;
    char  slotDesc[160];
    int   at = 0;
    for (int i = 0; i < 8; ++i) {
        if (!srvs[i]) continue;
        ResourceInfo info;
        if (bindingResolve(srvs[i], &info) && at < 130) {
            at += _snprintf_s(slotDesc + at, sizeof(slotDesc) - at, _TRUNCATE,
                              "%s[%d]=%s%u%s", at ? " " : "", i,
                              info.isBuffer ? "buf" : "tex", info.a,
                              info.isBuffer && info.b ? "s" : "");
            if (chosen < 0 && info.isBuffer) {
                chosen = i;
                chosenInfo = info;
                ID3D11Resource* res = nullptr;
                srvs[i]->GetResource(&res);
                if (res) {
                    resource = res;   // raw pointer kept; reference dropped.
                                      // The Map hook GetTypes before GetDesc,
                                      // the compositeCb discipline.
                    res->Release();
                }
            }
        }
        srvs[i]->Release();
    }
    if (at == 0) _snprintf_s(slotDesc, sizeof(slotDesc), _TRUNCATE, "(none)");

    // Fall back to the constant-buffer path for draws that have one -- the
    // v2 behaviour, kept because OTHER sprite systems may still be
    // CB-driven and the peek serves them all.
    if (!resource) {
        resource = bindingGet(BindSlot::VsCb0);
        if (resource) {
            ResourceInfo info;
            if (bindingResolveResource(resource, &info) && info.isBuffer) {
                chosenInfo = info;
            } else {
                resource = nullptr;
            }
        }
    }
    if (!resource) return;

    g_target = resource;
    g_targetBytes = chosenInfo.a;
    g_targetStride = chosenInfo.b;
    g_dumpedFull = false;
    Log::get().note("cb peek: learned the vertex-stage data of a matched "
                    "draw (n=%u). VS resources: %s. Watching %s slot %d -- "
                    "%u bytes, structure stride %u. The next write dumps "
                    "its head; after that, only what changes.",
                    count, slotDesc, chosen >= 0 ? "VS-SRV" : "VS-CB",
                    chosen, g_targetBytes, g_targetStride);
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

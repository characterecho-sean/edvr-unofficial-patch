#include "cb_peek.h"

#include <windows.h>

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

// The sprite family, as the SUN-scene census resolved it (2026-08-20; the
// first matcher required the witchspace corona's 1024x1024 fmt-99 atlas in
// slot 1, and a whole sweep captured nothing because the sun flare samples
// OTHER textures -- a 1024x512 streak atlas, 32x32 sparkles): kind X, the
// eye-sized depth resolve in PS slot 0, any TEXTURE in slot 1 (excluding
// the buffer-fed text meshes that share the pass), and a large-ish index
// count (the flare candidates run 576 and 600; HUD widgets run 6-36).
constexpr char     kKind = 'X';
constexpr uint32_t kMinIndices = 64;

constexpr uint32_t kMaxBytes = 8192;
constexpr uint32_t kMaxFloats = kMaxBytes / 4;
constexpr uint64_t kSampleMs = 1000;
constexpr uint32_t kMaxLines = 400;
constexpr float    kChangeEps = 1e-4f;

bool     g_enabled = false;
void*    g_target = nullptr;
uint32_t g_targetBytes = 0;
bool     g_dumpedFull = false;
uint64_t g_lastSampleMs = 0;
uint32_t g_lines = 0;
float    g_last[kMaxFloats];

}  // namespace

void cbPeekConfigure(Config& cfg) {
    const bool was = g_enabled;
    g_enabled = cfg.getBool("advanced.cb_peek", false);
    if (g_enabled && !was) {
        g_target = nullptr;
        g_dumpedFull = false;
        g_lines = 0;
        Log::get().note("cb peek: ON -- the sprite family's vertex constants "
                        "will be learned at the next matched draw and dumped "
                        "while they change. Park at a star; hold each of: "
                        "straight, roll left, straight, yaw so the sun sits "
                        "off-centre, straight -- a few seconds each.");
    }
    if (!g_enabled && was) g_target = nullptr;
}

bool cbPeekEnabled() { return g_enabled; }

void* cbPeekTarget() { return g_target; }

void cbPeekOnEyeDraw(char kind, uint32_t count, uint32_t /*instances*/) {
    if (!g_enabled || g_target || kind != kKind || count < kMinIndices) {
        // Learned ONCE per enable-cycle: two flare candidates share the sun
        // frame, and relearning on every different pointer interleaved two
        // buffers' dumps. Toggle cb_peek off and on to hunt the next one.
        return;
    }

    ResourceInfo atlas;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv1), &atlas) ||
        !atlas.isTexture2D) {
        return;
    }
    uint32_t eyeW = 0, eyeH = 0;
    if (!eyeTextureSize(&eyeW, &eyeH)) return;
    ResourceInfo depth;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv0), &depth) ||
        !depth.isTexture2D || depth.a != eyeW || depth.b != eyeH) {
        return;
    }

    void* cb = bindingGet(BindSlot::VsCb0);
    if (!cb) return;
    g_target = cb;
    g_dumpedFull = false;
    ResourceInfo info;
    const bool resolved = bindingResolveResource(cb, &info) && info.isBuffer;
    g_targetBytes = resolved ? info.a : 0;
    Log::get().note("cb peek: learned the vertex constants of a matched draw "
                    "-- n=%u, slot1 tex %ux%u fmt=%u, buffer %u bytes. The "
                    "next write dumps it in full; after that, only the floats "
                    "that change.",
                    count, atlas.a, atlas.b, atlas.fmt, g_targetBytes);
}

void cbPeekCapture(const void* data, uint32_t bytes) {
    if (!g_enabled || !data || g_lines >= kMaxLines) return;
    if (bytes > kMaxBytes) bytes = kMaxBytes;
    const uint32_t n = bytes / 4;
    const float* f = static_cast<const float*>(data);
    const uint64_t now = nowMs();

    if (!g_dumpedFull) {
        g_dumpedFull = true;
        g_lastSampleMs = now;
        memcpy(g_last, f, n * 4);
        // Eight floats a line: a 5376-byte buffer is 168 lines, spent once
        // per learned target and charged against the same cap as the deltas.
        for (uint32_t i = 0; i < n && g_lines < kMaxLines; i += 8) {
            char line[360];
            int at = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%u]", i);
            for (uint32_t k = i; k < i + 8 && k < n; ++k) {
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

    char line[440];
    int at = 0;
    uint32_t changed = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const float d = f[i] - g_last[i];
        const float a = d < 0 ? -d : d;
        if (a > kChangeEps && !std::isnan(d)) {
            ++changed;
            if (at < 360) {
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

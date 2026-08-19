#include "fov_probe.h"

#include <windows.h>

#include <cmath>
#include <cstring>

#include "../common/config.h"
#include "../common/log.h"
#include "../common/timing.h"
#include "journal_watch.h"

namespace edvr {
namespace {

constexpr uint32_t kMaxBytes = 5376;
constexpr uint32_t kMaxFloats = kMaxBytes / 4;
constexpr uint64_t kSampleMs = 500;

// A float is RAMPING when it moves the same direction for this many
// consecutive samples, each step big enough to be real and small enough to
// be an animation rather than a teleport. An animated FOV sweeps smoothly;
// camera positions leap thousands of units; constants do not move.
constexpr uint8_t kRampStreak = 3;
constexpr float   kMinStep = 1e-5f;
constexpr float   kMaxStep = 0.5f;

// How many ramping floats one report line names, and how many report lines
// a session may spend. A jump is ~40 samples; the cap keeps a pathological
// buffer from papering the log.
constexpr uint32_t kTopPerLine = 12;
constexpr uint32_t kMaxLines = 200;

bool     g_enabled = false;
bool     g_inWindow = false;
uint64_t g_lastSampleMs = 0;
uint32_t g_lines = 0;

float  g_last[kMaxFloats];
int8_t g_streak[kMaxFloats];   // signed run length: +n rising, -n falling
bool   g_haveLast = false;

}  // namespace

void fovProbeConfigure(Config& cfg) {
    const bool was = g_enabled;
    g_enabled = cfg.getBool("advanced.fov_probe", false);
    if (g_enabled && !was) {
        Log::get().note("fov probe: ON -- during jump tunnels, the scene "
                        "camera buffer's floats are sampled twice a second "
                        "and the ones that RAMP are named. Looking for the "
                        "game's animated tunnel projection.");
    }
}

void fovProbeObserve(const void* data, uint32_t bytes) {
    if (!g_enabled || !data || bytes < 64) return;
    if (bytes > kMaxBytes) bytes = kMaxBytes;

    const bool inWindow = journalInJumpTunnel();
    if (!inWindow) {
        if (g_inWindow) {
            g_inWindow = false;
            Log::get().note("fov probe: jump window closed.");
        }
        g_haveLast = false;
        return;
    }
    if (!g_inWindow) {
        g_inWindow = true;
        g_haveLast = false;
        Log::get().note("fov probe: jump window open, sampling.");
    }

    const uint64_t now = nowMs();
    if (g_haveLast && now - g_lastSampleMs < kSampleMs) return;
    g_lastSampleMs = now;

    const float* f = static_cast<const float*>(data);
    const uint32_t n = bytes / 4;

    if (!g_haveLast) {
        memcpy(g_last, f, n * 4);
        memset(g_streak, 0, sizeof(g_streak));
        g_haveLast = true;
        return;
    }

    // Update streaks, collect the longest-running ramps.
    uint32_t ramping = 0;
    uint32_t top[kTopPerLine];
    uint32_t topCount = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const float d = f[i] - g_last[i];
        const float a = d < 0 ? -d : d;
        if (a >= kMinStep && a <= kMaxStep && !std::isnan(d)) {
            if (d > 0) {
                g_streak[i] = g_streak[i] > 0 ? static_cast<int8_t>(
                    g_streak[i] < 100 ? g_streak[i] + 1 : 100) : 1;
            } else {
                g_streak[i] = g_streak[i] < 0 ? static_cast<int8_t>(
                    g_streak[i] > -100 ? g_streak[i] - 1 : -100) : -1;
            }
        } else {
            g_streak[i] = 0;
        }
        const uint8_t run = static_cast<uint8_t>(
            g_streak[i] < 0 ? -g_streak[i] : g_streak[i]);
        if (run >= kRampStreak) {
            ++ramping;
            if (topCount < kTopPerLine) top[topCount++] = i;
        }
        g_last[i] = f[i];
    }

    if (ramping && g_lines < kMaxLines) {
        ++g_lines;
        char list[512];
        int at = 0;
        for (uint32_t k = 0; k < topCount && at < 440; ++k) {
            const uint32_t i = top[k];
            at += _snprintf_s(list + at, sizeof(list) - at, _TRUNCATE,
                              "%s[%u]=%.4f", k ? " " : "", i,
                              static_cast<double>(f[i]));
        }
        Log::get().note("fov probe: %u ramping float(s); longest runs: %s",
                        ramping, list);
    }
}

}  // namespace edvr

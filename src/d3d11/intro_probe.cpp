#include "intro_probe.h"

#include <cstdio>

#include "../common/config.h"
#include "../common/log.h"
#include "../common/timing.h"

namespace edvr {
namespace {

// Distinct target sizes recorded in one frame. The startup frames this was
// built for hold two or three; a rendered scene holds more than this, and the
// count of what was dropped goes on the line so a truncated frame says so.
constexpr uint32_t kMaxBuckets = 10;

// How long the probe records. The intro runs about forty seconds from attach
// to the menu on the measured rig, and a slow install is slower; three
// minutes covers both and still ends the session's logging by itself.
constexpr uint64_t kWindowMs = 180000;

// Lines one session may write. The interesting ones come FIRST -- the cap
// protects the startup by arriving after it, once the menu's rendered scene
// starts changing composition every frame.
constexpr uint32_t kMaxLines = 96;

// A frame this long is the thing the report calls a freeze. 200 ms is well
// clear of an ordinary hitch and well under the reported one to two seconds.
constexpr uint64_t kStallMs = 200;
constexpr uint32_t kMaxStallLines = 16;

// Distinct target-size-and-colour clears named. Eight would do for the
// startup; sixteen leaves room for the menu behind it.
constexpr uint32_t kMaxClears = 16;

bool     g_on = false;
bool     g_configured = false;
bool     g_closed = false;

uint64_t g_startMs = 0;        // the first frame boundary
uint64_t g_prevMs = 0;
uint32_t g_lines = 0;
uint32_t g_stallLines = 0;
uint32_t g_frames = 0;
uint32_t g_changes = 0;
uint64_t g_longestMs = 0;
uint32_t g_longestFrame = 0;
uint32_t g_firstEyeFrame = 0;
uint64_t g_firstEyeMs = 0;
uint64_t g_lastSig = 0;

struct Bucket {
    uint32_t w = 0, h = 0, draws = 0;
    bool     eye = false;
};
Bucket   g_buckets[kMaxBuckets];
uint32_t g_bucketCount = 0;
uint32_t g_bucketsDropped = 0;
uint32_t g_drawsThisFrame = 0;

struct ClearSeen {
    uint32_t w = 0, h = 0;
    float    c[4] = {0, 0, 0, 0};
};
ClearSeen g_clears[kMaxClears];
uint32_t  g_clearCount = 0;
uint32_t  g_clearsDropped = 0;

double elapsedSec(uint64_t now) {
    if (!g_startMs || now < g_startMs) return 0.0;
    return static_cast<double>(now - g_startMs) / 1000.0;
}

// The power-of-two class of a draw count. The signature has to survive the
// game drawing 9 quads one frame and 11 the next -- that is a progress bar
// re-tessellating, not a new phase -- while still separating "a handful" from
// "hundreds", which is exactly what tells a menu from a rendered scene.
uint32_t countClass(uint32_t n) {
    uint32_t c = 0;
    while (n) {
        ++c;
        n >>= 1;
    }
    return c;
}

void mix(uint64_t& h, uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
}

// Area descending, so the line reads big-target-first however the game
// ordered its draws, and so the signature does not move when it reorders
// them. Insertion sort over at most ten entries.
void sortBuckets() {
    for (uint32_t i = 1; i < g_bucketCount; ++i) {
        const Bucket key = g_buckets[i];
        const uint64_t keyArea = static_cast<uint64_t>(key.w) * key.h;
        uint32_t j = i;
        while (j > 0) {
            const uint64_t area =
                static_cast<uint64_t>(g_buckets[j - 1].w) * g_buckets[j - 1].h;
            if (area > keyArea ||
                (area == keyArea && g_buckets[j - 1].w >= key.w)) {
                break;
            }
            g_buckets[j] = g_buckets[j - 1];
            --j;
        }
        g_buckets[j] = key;
    }
}

void closeProbe(uint32_t frameNo, const char* why) {
    g_closed = true;
    char tail[224];
    if (g_firstEyeFrame) {
        snprintf(tail, sizeof(tail),
                 "the first draw into an eye texture was frame %u (%.2f s), "
                 "which is where VR took over",
                 g_firstEyeFrame,
                 (g_startMs && g_firstEyeMs >= g_startMs)
                     ? static_cast<double>(g_firstEyeMs - g_startMs) / 1000.0
                     : 0.0);
    } else {
        snprintf(tail, sizeof(tail),
                 "no draw ever landed in an eye texture, so either VR never "
                 "started or one eye's size was never published to this half");
    }
    Log::get().note(
        "intro probe: closed at frame %u, %.1f s in (%s) -- %u frames, %u "
        "composition change(s), %u distinct clear(s)%s, longest frame %llu ms "
        "at frame %u; %s.",
        frameNo, elapsedSec(nowMs()), why, g_frames, g_changes, g_clearCount,
        g_clearsDropped ? " (more were dropped)" : "",
        static_cast<unsigned long long>(g_longestMs), g_longestFrame, tail);
}

}  // namespace

void introProbeConfigure(Config& cfg) {
    const bool on = cfg.getBool("advanced.intro_probe", false);
    const bool first = !g_configured;
    g_configured = true;
    if (on == g_on && !first) return;
    g_on = on;
    if (!on) {
        if (!first) Log::get().note("intro probe: off.");
        return;
    }
    if (g_closed) {
        Log::get().note(
            "intro probe: on, but its window has already closed this session. "
            "Restart the game to record another startup.");
        return;
    }
    Log::get().note(
        "intro probe: on. Every frame's draws are counted by the target they "
        "land in, and a line is written whenever that shape changes, whenever "
        "a frame takes %llu ms or more, and for each new clear colour. An "
        "eye-sized target is marked with a star. Nothing is changed. "
        "docs\\intro-video.md",
        static_cast<unsigned long long>(kStallMs));
}

bool introProbeWants() { return g_on && !g_closed; }

void introProbeOnDraw(uint32_t targetW, uint32_t targetH, bool eyeSized) {
    if (!g_on || g_closed) return;
    ++g_drawsThisFrame;
    for (uint32_t i = 0; i < g_bucketCount; ++i) {
        if (g_buckets[i].w == targetW && g_buckets[i].h == targetH) {
            ++g_buckets[i].draws;
            g_buckets[i].eye = g_buckets[i].eye || eyeSized;
            return;
        }
    }
    if (g_bucketCount >= kMaxBuckets) {
        ++g_bucketsDropped;
        return;
    }
    Bucket& b = g_buckets[g_bucketCount++];
    b.w = targetW;
    b.h = targetH;
    b.draws = 1;
    b.eye = eyeSized;
}

void introProbeOnClear(uint32_t targetW, uint32_t targetH, const float rgba[4]) {
    if (!g_on || g_closed || !rgba) return;
    for (uint32_t i = 0; i < g_clearCount; ++i) {
        const ClearSeen& s = g_clears[i];
        if (s.w != targetW || s.h != targetH) continue;
        bool same = true;
        for (int k = 0; k < 4; ++k) {
            const float d = s.c[k] - rgba[k];
            if (d > 0.001f || d < -0.001f) same = false;
        }
        if (same) return;
    }
    if (g_clearCount >= kMaxClears) {
        ++g_clearsDropped;
        return;
    }
    ClearSeen& s = g_clears[g_clearCount++];
    s.w = targetW;
    s.h = targetH;
    for (int k = 0; k < 4; ++k) s.c[k] = rgba[k];
    ++g_lines;
    Log::get().note(
        "intro probe: at %.2f s a %ux%u target was cleared to r=%.3f g=%.3f "
        "b=%.3f a=%.3f -- what the GAME asked for, before any EDVR "
        "substitution (%u of %u colours named).",
        elapsedSec(nowMs()), targetW, targetH, rgba[0], rgba[1], rgba[2],
        rgba[3], g_clearCount, kMaxClears);
}

void introProbeFrameBoundary(uint32_t frameNo) {
    if (!g_on || g_closed) return;
    const uint64_t now = nowMs();
    if (g_startMs == 0) {
        g_startMs = stampMs();
        g_prevMs = g_startMs;
    }
    const uint64_t dt = now >= g_prevMs ? now - g_prevMs : 0;
    g_prevMs = now;
    ++g_frames;

    sortBuckets();

    uint64_t sig = 1469598103934665603ull;
    mix(sig, g_bucketCount);
    bool anyEye = false;
    for (uint32_t i = 0; i < g_bucketCount; ++i) {
        mix(sig, (static_cast<uint64_t>(g_buckets[i].w) << 32) | g_buckets[i].h);
        mix(sig, (static_cast<uint64_t>(g_buckets[i].eye ? 1 : 0) << 32) |
                     countClass(g_buckets[i].draws));
        anyEye = anyEye || g_buckets[i].eye;
    }

    // The VR handover, said once and on its own line. It is the single fact
    // the whole flight is for: everything before it is the flat phase the
    // player sees at mirror resolution, everything after it is the phase a
    // fix can reach through the eye textures.
    if (anyEye && !g_firstEyeFrame) {
        g_firstEyeFrame = frameNo;
        g_firstEyeMs = now;
        ++g_lines;
        Log::get().note(
            "intro probe: frame %u at %.2f s is the FIRST frame with a draw "
            "into an eye texture -- VR took over here. Every line above this "
            "is the flat phase.",
            frameNo, elapsedSec(now));
    }

    if (sig != g_lastSig && g_lines < kMaxLines) {
        g_lastSig = sig;
        ++g_changes;
        char list[400];
        list[0] = '\0';
        int n = 0;
        for (uint32_t i = 0; i < g_bucketCount; ++i) {
            const int room = static_cast<int>(sizeof(list)) - n;
            if (room < 40) {
                snprintf(list + n, static_cast<size_t>(room), ", ...");
                break;
            }
            const int wrote =
                snprintf(list + n, static_cast<size_t>(room), "%s%ux%u%s x%u",
                         i ? ", " : "", g_buckets[i].w, g_buckets[i].h,
                         g_buckets[i].eye ? "*" : "", g_buckets[i].draws);
            if (wrote < 0) break;
            n += wrote;
        }
        ++g_lines;
        Log::get().note(
            "intro probe: frame %u at %.2f s, %llu ms -- %u draw(s) into %u "
            "target(s): %s%s",
            frameNo, elapsedSec(now), static_cast<unsigned long long>(dt),
            g_drawsThisFrame, g_bucketCount, g_bucketCount ? list : "none",
            g_bucketsDropped ? " (further sizes not recorded)" : "");
    }

    if (dt > g_longestMs) {
        g_longestMs = dt;
        g_longestFrame = frameNo;
    }
    if (dt >= kStallMs && g_stallLines < kMaxStallLines) {
        ++g_stallLines;
        ++g_lines;
        Log::get().note(
            "intro probe: frame %u at %.2f s took %llu ms -- the render thread "
            "stalled here (stall %u of %u recorded).",
            frameNo, elapsedSec(now), static_cast<unsigned long long>(dt),
            g_stallLines, kMaxStallLines);
    }

    g_bucketCount = 0;
    g_bucketsDropped = 0;
    g_drawsThisFrame = 0;
    for (Bucket& b : g_buckets) b = Bucket();

    if (g_lines >= kMaxLines) {
        closeProbe(frameNo, "the line budget is spent");
    } else if (elapsedMs(g_startMs, kWindowMs)) {
        closeProbe(frameNo, "the recording window is over");
    }
}

}  // namespace edvr

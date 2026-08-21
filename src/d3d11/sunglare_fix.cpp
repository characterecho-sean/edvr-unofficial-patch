#include "sunglare_fix.h"

#include <windows.h>

#include <cstring>

#include "../common/config.h"
#include "../common/log.h"
#include "billboard_fix.h"
#include "binding_shadow.h"

namespace edvr {
namespace {

// The glare train, as the sun census resolved it: DrawInstanced, 6
// vertices per instance, more than one instance, and BOTH of PS slots 0
// and 1 the 2048x1024 fmt-98 art sheets the elements are stamped from.
// The size-and-format pair is the identity; nothing else in the frame
// samples those sheets. Counts, positions and buffer pointers all proved
// unstable over this hunt -- what a pass READS is what it is.
constexpr char     kKind = 'N';
constexpr uint32_t kVerts = 6;
constexpr uint32_t kSheetW = 2048;
constexpr uint32_t kSheetH = 1024;
constexpr uint32_t kSheetFmt = 98;

enum class Mode { kStock, kOff, kFirst };

Mode     g_mode = Mode::kStock;
uint32_t g_keep = 0;
bool     g_steady = false;
uint64_t g_skipped = 0;
uint64_t g_clamped = 0;

}  // namespace

bool sunglareIsGlareTrain(char kind, uint32_t count, uint32_t instances) {
    if (kind != kKind || count != kVerts || instances < 2) return false;
    ResourceInfo s0, s1;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv0), &s0) ||
        !s0.isTexture2D || s0.a != kSheetW || s0.b != kSheetH ||
        s0.fmt != kSheetFmt) {
        return false;
    }
    if (!bindingResolve(bindingGet(BindSlot::PsSrv1), &s1) ||
        !s1.isTexture2D || s1.a != kSheetW || s1.b != kSheetH ||
        s1.fmt != kSheetFmt) {
        return false;
    }
    return true;
}

void sunglareConfigure(Config& cfg) {
    const Mode was = g_mode;
    const uint32_t wasKeep = g_keep;
    const std::string v = cfg.getString("fix.sun_glare", "stock");
    if (v == "stock") {
        g_mode = Mode::kStock;
    } else if (v == "off") {
        g_mode = Mode::kOff;
    } else if (v.compare(0, 6, "first:") == 0) {
        char* end = nullptr;
        const unsigned long k = strtoul(v.c_str() + 6, &end, 10);
        if (end == v.c_str() + 6 || *end || k < 1 || k > 32) {
            Log::get().note("sun glare: \"%s\" is not stock, off or first:K "
                            "with K 1..32; staying stock.", v.c_str());
            g_mode = Mode::kStock;
        } else {
            g_mode = Mode::kFirst;
            g_keep = static_cast<uint32_t>(k);
        }
    } else {
        Log::get().note("sun glare: \"%s\" is not stock, off or first:K; "
                        "staying stock.", v.c_str());
        g_mode = Mode::kStock;
    }
    const bool wasSteady = g_steady;
    g_steady = cfg.getBool("fix.sun_glare_steady", false);
    billboardGlareWatch(g_steady);
    if (g_steady != wasSteady) {
        Log::get().note("sun glare steady: %s -- the train's drawn basis is "
                        "%s per write from the world rows its own constants "
                        "carry. Orientation stops following the head; scale, "
                        "position and everything else stay the game's.",
                        g_steady ? "ON" : "off",
                        g_steady ? "rebuilt" : "no longer rebuilt");
    }
    if (g_mode != was || (g_mode == Mode::kFirst && g_keep != wasKeep)) {
        if (g_mode == Mode::kOff) {
            Log::get().note("sun glare: OFF -- the screen-space glare "
                            "element train (both beams, corona flare, "
                            "smudge, rays) is not drawn. The star's own "
                            "disc is a different draw and is untouched.");
        } else if (g_mode == Mode::kFirst) {
            Log::get().note("sun glare: drawing only the FIRST %u "
                            "instance(s) of the glare element train -- the "
                            "mapping walk. Step K and note what appears.",
                            g_keep);
        } else {
            Log::get().note("sun glare: stock.");
        }
    }
}

bool sunglareWantsDraws() { return g_mode != Mode::kStock || g_steady; }

bool sunglareSteady() { return g_steady; }

SunglareAction sunglareOnEyeDraw(char kind, uint32_t count,
                                 uint32_t instances) {
    if (!sunglareWantsDraws() ||
        !sunglareIsGlareTrain(kind, count, instances)) {
        return SunglareAction::kStock;
    }
    if (g_mode == Mode::kOff) {
        ++g_skipped;
        return SunglareAction::kSkip;
    }
    if (g_mode == Mode::kFirst && instances > g_keep) {
        ++g_clamped;
        return SunglareAction::kClamp;
    }
    // Matched, not skipped, not clamped -- kMatch tells the caller a train
    // draw is happening, which is all the steady path needs to know.
    return SunglareAction::kMatch;
}

uint32_t sunglareKeep() { return g_keep; }

}  // namespace edvr

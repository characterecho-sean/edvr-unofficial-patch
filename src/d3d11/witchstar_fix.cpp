#include "witchstar_fix.h"

#include <windows.h>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/log.h"
#include "binding_shadow.h"

namespace edvr {
namespace {

// The family, as the witchspace census resolved it: DrawIndexedInstanced,
// PS slot 0 the eye-sized depth resolve, slot 1 the 1024x1024 fmt-99 atlas.
// The star's members are the ones with large index counts; the small quads
// of the same family scattered through the HUD are ordinary widgets and
// must not pin, so a run only ARMS on a large member.
constexpr char     kKind = 'X';
constexpr uint32_t kAtlasW = 1024;
constexpr uint32_t kAtlasH = 1024;
constexpr uint32_t kAtlasFmt = 99;
constexpr uint32_t kArmIndexCount = 100;

bool g_pinned = false;
bool g_flipX = false;
bool g_flipY = false;

// The run state: inside a star cluster, and how many clusters this frame
// has ended (0 while the first -- left eye -- is open).
bool     g_inStarRun = false;
uint32_t g_clustersEnded = 0;

bool                   g_engaged = false;
UINT                   g_savedVpCount = 0;
D3D11_VIEWPORT         g_savedVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};

uint64_t g_applied = 0;

bool isFamily(char kind, ResourceInfo* atlasOut) {
    if (kind != kKind) return false;
    ResourceInfo atlas;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv1), &atlas) ||
        !atlas.isTexture2D || atlas.a != kAtlasW || atlas.b != kAtlasH ||
        atlas.fmt != kAtlasFmt) {
        return false;
    }
    uint32_t eyeW = 0, eyeH = 0;
    if (!eyeTextureSize(&eyeW, &eyeH)) return false;
    ResourceInfo depth;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv0), &depth) ||
        !depth.isTexture2D || depth.a != eyeW || depth.b != eyeH) {
        return false;
    }
    if (atlasOut) *atlasOut = atlas;
    return true;
}

}  // namespace

void witchstarConfigure(Config& cfg) {
    const bool was = g_pinned;
    const std::string m = cfg.getString("fix.witchstar", "stock");
    if (m == "stock") {
        g_pinned = false;
    } else if (m == "pinned") {
        g_pinned = true;
    } else {
        g_pinned = false;
        Log::get().note("witchstar \"%s\" is not stock or pinned; running "
                        "stock.", m.c_str());
    }
    g_flipX = cfg.getBool("advanced.witchstar_flip_x", false);
    g_flipY = cfg.getBool("advanced.witchstar_flip_y", false);

    if (was != g_pinned) {
        Log::get().note("witchstar: %s. The jump tunnel's destination star is "
                        "%s.",
                        g_pinned ? "pinned" : "stock",
                        g_pinned ? "held to the ship's forward axis while the "
                                   "head turns"
                                 : "the game's own (head-locked in VR)");
    }
}

bool witchstarWantsDraws() { return g_pinned; }

bool witchstarOnEyeDraw(char kind, uint32_t count, uint32_t /*instances*/) {
    if (!g_pinned) return false;

    if (!isFamily(kind, nullptr)) {
        // Anything else breaks a run. Counting the ends is what tells the
        // left cluster from the right, should the shift ever need per-eye
        // terms; today both eyes use the same delta.
        if (g_inStarRun) {
            g_inStarRun = false;
            ++g_clustersEnded;
        }
        return false;
    }
    if (g_inStarRun) return true;              // interleaved flare quads ride along
    if (count >= kArmIndexCount) {
        g_inStarRun = true;                    // the star's body arms the run
        return true;
    }
    return false;                              // a lone HUD widget of the family
}

void witchstarBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;

    float tx = 0.0f, ty = 0.0f;
    if (!headForward(&tx, &ty)) return;        // openvr half absent: stock
    float outer = 0.0f, inner = 0.0f;
    if (!eyeTangents(&outer, &inner)) return;
    const float span = outer + inner;
    if (span < 1e-3f) return;

    UINT vpCount = 1;
    D3D11_VIEWPORT vp{};
    ctx->RSGetViewports(&vpCount, &vp);
    if (vpCount == 0 || vp.Width <= 0.0f) return;

    // Pixels per unit tangent, horizontal. The vertical span is not
    // published; on every headset measured the two spans agree within two
    // percent, which is beneath notice for a glow sprite, so one number
    // serves both axes.
    const float pxPerTan = vp.Width / span;
    float sx = tx * pxPerTan;
    float sy = -ty * pxPerTan;   // +y is up in the head frame, down on screen
    if (g_flipX) sx = -sx;
    if (g_flipY) sy = -sy;
    // A star more than a viewport off-axis is out of view; the clamp only
    // keeps a pathological pose from flinging the viewport into numeric
    // nonsense.
    const float lim = vp.Width * 0.9f;
    if (sx > lim) sx = lim;
    if (sx < -lim) sx = -lim;
    if (sy > lim) sy = lim;
    if (sy < -lim) sy = -lim;

    g_savedVpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&g_savedVpCount, g_savedVps);
    D3D11_VIEWPORT shifted = vp;
    shifted.TopLeftX = vp.TopLeftX + sx;
    shifted.TopLeftY = vp.TopLeftY + sy;
    ctx->RSSetViewports(1, &shifted);
    g_engaged = true;

    if (++g_applied == 1) {
        Log::get().note("witchstar: pinned engaged -- the destination star "
                        "now holds the ship's forward axis. First shift "
                        "%+.0f,%+.0f px (head-forward tangents %+.3f,%+.3f). "
                        "If the star moves WITH your head twice as fast "
                        "instead of holding still, set witchstar_flip_x=1 "
                        "(and _y for vertical) under [advanced].",
                        sx, sy, tx, ty);
    }
}

void witchstarEnd(ID3D11DeviceContext* ctx) {
    if (!g_engaged) return;
    g_engaged = false;
    ctx->RSSetViewports(g_savedVpCount, g_savedVpCount ? g_savedVps : nullptr);
}

void witchstarFrameBoundary() {
    g_inStarRun = false;
    g_clustersEnded = 0;
}

}  // namespace edvr

#include "witchstar_fix.h"

#include <windows.h>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/log.h"
#include "../common/timing.h"
#include "binding_shadow.h"
#include "journal_watch.h"
#include "vscreen.h"  // vScreenIsEyeSized

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
// Compensation gain. 1.0 is the geometric answer -- pixels-per-tangent
// times the head-forward tangents SHOULD hold a direction exactly -- and
// the first field flight said the star still tracked the gaze under both
// signs, which is the signature of a magnitude or timing error rather
// than a sign error. Live-tuned to find the truth: the gain that pins by
// eye measures the real relationship, and if NO gain pins, the problem is
// the pose's timing, not its scale.
float g_gain = 1.0f;

// The run state: inside a star cluster, and how many clusters this frame
// has ended (0 while the first -- left eye -- is open).
bool     g_inStarRun = false;
uint32_t g_clustersEnded = 0;

bool                   g_engaged = false;
UINT                   g_savedVpCount = 0;
D3D11_VIEWPORT         g_savedVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};

uint64_t g_applied = 0;
uint64_t g_lastDiagMs = 0;

bool isFamily(char kind, ResourceInfo* atlasOut) {
    if (kind != kKind) return false;
    ResourceInfo atlas;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv1), &atlas) ||
        !atlas.isTexture2D || atlas.a != kAtlasW || atlas.b != kAtlasH ||
        atlas.fmt != kAtlasFmt) {
        return false;
    }
    // Eye-sized by vScreen's answer, not by an equality against the
    // published size. On a rig with a render scale the world -- and so
    // this depth resolve -- is a fraction of the size the headset is
    // handed, and an equality there matched nothing at all: this fix was
    // silently off for the whole of that session (2026-08-19).
    ResourceInfo depth;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv0), &depth) ||
        !depth.isTexture2D || !vScreenIsEyeSized(depth.a, depth.b)) {
        return false;
    }
    if (atlasOut) *atlasOut = atlas;
    return true;
}

}  // namespace

void witchstarConfigure(Config& /*cfg*/) {
    // RETIRED 2026-08-22, superseded by fix.sun_glare: the jump
    // tunnel's destination corona rides the same glare element train
    // as ordinary suns (field-verified 2026-08-21), so the sun-glare
    // modes cover witchspace and the viewport-shift mechanism never
    // engages. No keys are read; the machinery stays for reference.
    g_pinned = false;
}

bool witchstarWantsDraws() { return g_pinned; }

bool witchstarOnEyeDraw(char kind, uint32_t count, uint32_t /*instances*/) {
    if (!g_pinned) return false;
    // Only while the journal says a jump tunnel is plausibly on screen. The
    // family this matcher recognises also draws a sun's flare in ordinary
    // space -- where the game positions it correctly and the pin is pure
    // error, measured as the flare counter-moving while parked at a star.
    // Outside the window the run state idles empty, so there is nothing to
    // break.
    if (!journalInJumpTunnel()) {
        g_inStarRun = false;
        return false;
    }

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
    float sx = tx * pxPerTan * g_gain;
    float sy = -ty * pxPerTan * g_gain;   // +y up in the head frame, down on screen
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

    // Telemetry while engaged, throttled to one line every two seconds: the
    // tangents are what the pose said, the shift is what was done with
    // them, and a reader with the headset on says what actually happened.
    // The three theories -- wrong sign, wrong scale, stale pose -- each
    // print a different relationship here.
    ++g_applied;
    const uint64_t now = nowMs();
    if (g_applied == 1 || now - g_lastDiagMs >= 2000) {
        g_lastDiagMs = now;
        Log::get().note("witchstar: shift %+.0f,%+.0f px from tangents "
                        "%+.3f,%+.3f (gain %.2f, flips %d/%d, %.0f px/tan). "
                        "Tune witchstar_gain live until the star holds "
                        "still; if no gain does, the pose's timing is the "
                        "suspect, not its scale.",
                        sx, sy, tx, ty, g_gain, g_flipX ? 1 : 0,
                        g_flipY ? 1 : 0, pxPerTan * g_gain);
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

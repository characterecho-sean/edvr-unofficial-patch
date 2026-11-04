// F198: a game target's eye by the textures the game SUBMITTED, for the native on-foot marker fix and the weapon readout pin with Anti-aliasing off.
//
// Those features name a marker composite draw's eye by its target (native_on_foot.cpp targetEye): the UI layer's redirect mapping ('L'), else ui depth's frame
// table ('T'). Both are written only while the temporal path runs (the layer is live only with a temporal mode, ui depth classifies draws only under
// uiDepthWantsDraws), so with Anti-aliasing off both answer -1, fixDecide returns at once and the marker lock, the weapon readout pin, the labels and the
// reticle NDC are dead (the pilot's flight edvr_gfx_20261031_161247: "no marker composite draw of the eye reached the fix" with 330 of them a second at the tee).
//
// The game's Submit names an eye and its texture (ui_layer.h calls it "the one authority"), and native_temporal.cpp's treat reports it BEFORE its
// Anti-aliasing-off return. The layer's own eye check (uiLayerComposite) relies on the same fact: the target the UI left is the texture submitted for the eye,
// or was copied into it. So a draw whose target IS an eye's submitted texture is that eye's draw: source 'S'.
//
// Timing. The marker composite draws precede the frame's treat, so S answers from the frames before it. That is safe because the submitted textures are
// stable (the same pair every frame, as the layer's "this frame's or last: stable textures" already assumes); it is made safe by waiting for two submits of
// the same texture at the same size for each eye. A recreate answers -1 (no eye, as without S) until two stable submits of the new pair; an on-foot or
// render-mode edge clears the lot (the pair may be new and the eyes' order is the setter's).
//
// The order is L, then T, then S only while the temporal path is NOT running, then the caller's own cache ('C'). With the temporal path running S is never
// asked, so the temporal path's answers are exactly what they were.
#pragma once

#include <cstdint>

namespace edvr {
namespace submittedeye {

constexpr uint32_t kStableSubmits = 2;   // submits of the same texture at the same size, per eye, before S names anything

struct Tracker {
    const void* tex[2] = {};
    uint32_t w[2] = {}, h[2] = {};
    uint32_t stable[2] = {};   // consecutive submits of this texture at this size
    uint64_t edge = 0;
    bool haveEdge = false;

    void clear() {
        for (int e = 0; e < 2; ++e) {
            tex[e] = nullptr;
            w[e] = h[e] = 0;
            stable[e] = 0;
        }
    }
    // One submit: the eye the game named, its texture (an identity, never dereferenced), the texture's size and the edge token (any change of it is an
    // on-foot or render-mode edge). Called from treat, once per eye a frame, before the frame's draws for the next.
    void note(int eye, const void* texture, uint32_t width, uint32_t height, uint64_t edgeToken) {
        if (eye < 0 || eye > 1) return;
        if (haveEdge && edgeToken != edge) clear();
        haveEdge = true;
        edge = edgeToken;
        if (!texture) {
            tex[eye] = nullptr;
            w[eye] = h[eye] = 0;
            stable[eye] = 0;
            return;
        }
        if (tex[eye] == texture && w[eye] == width && h[eye] == height) {
            if (stable[eye] < UINT32_MAX) ++stable[eye];
        } else {   // a recreate, or a size change: the eye starts over
            tex[eye] = texture;
            w[eye] = width;
            h[eye] = height;
            stable[eye] = 1;
        }
    }
    // The eye whose submitted texture `res` is, or -1: both eyes known, distinct, each stable for kStableSubmits.
    int eyeOf(const void* res) const {
        if (!res || !tex[0] || !tex[1] || tex[0] == tex[1]) return -1;
        if (stable[0] < kStableSubmits || stable[1] < kStableSubmits) return -1;
        return res == tex[0] ? 0 : res == tex[1] ? 1 : -1;
    }
};

struct Named {
    int eye;    // 0, 1, or -1
    char src;   // 'L', 'T', 'S', or 0 for none (the caller's cache is next)
};

// targetEye's order, with the three askers as callables so the rig counts which were asked. `temporalPathRuns` is the temporal mode itself
// (temporalPassWantsFssChrome(): the fix.temporal_aa reading), not a feature's stand-down: S is asked only when it is false.
template <class Layer, class Depth, class Submitted>
inline Named nameTargetEye(Layer layer, Depth depth, bool temporalPathRuns, Submitted submitted) {
    int eye = layer();
    if (eye == 0 || eye == 1) return {eye, 'L'};
    eye = depth();
    if (eye == 0 || eye == 1) return {eye, 'T'};
    if (!temporalPathRuns) {
        eye = submitted();
        if (eye == 0 || eye == 1) return {eye, 'S'};
    }
    return {-1, 0};
}

}  // namespace submittedeye
}  // namespace edvr

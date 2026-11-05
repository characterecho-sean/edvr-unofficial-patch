// The VR Galaxy Map's run of sprites (docs/galaxy-map-vr-2026-11-03.md, "After the fifth flight"): the decision of when the real context's render targets change, as a state
// machine with no device, no game memory and no log (tools/map_run_test drives it; ui_layer.cpp holds the D3D side).
//
// A RUN is a sequence of owner-context map sprite, canvas and mark issues into the HDR layer with the eye's private D16 bound. Binding the layer and the D16, drawing, and binding
// the game's R11G11B10 and D32S8 back after every issue alternated two depth-stencil surfaces 2,600 to 3,500 times a frame (11.1 ms of GPU at 432 sprites an eye on an RTX 5090,
// against 1.0 ms for one bind a run; restoring once a sprite instead of twice was still 6.2 ms). While a run is HELD the real context has the layer and the D16 bound and the game
// believes its own targets are bound. The game's targets are kept by the caller; this class holds their identities, to compare a game's own set of targets against them.
#pragma once

#include <cstdint>

namespace edvr {
namespace maprun {

constexpr int kTargets = 8;

enum class Step : uint8_t {
    kBind,         // not held: the issue binds its targets
    kKeep,         // held with the issue's own targets: nothing is bound
    kEndThenBind,  // held with other targets, or the issue is of another kind: the caller binds the game's targets back (the run is over), then the issue binds its own
};

enum class GameSet : uint8_t {
    kNotHeld,  // no run: the game's call is made
    kKeep,     // the game sets exactly the targets the run saved: its shadow is updated and the real call is not made
    kDrop,     // the game sets others: the hold is dropped with no bind (the game's call replaces ours) and the call is made
};

struct Core {
    bool held = false;
    int eye = -1;
    const void* layerRtv = nullptr;
    const void* layerDsv = nullptr;
    const void* gameRtv[kTargets] = {};
    const void* gameDsv = nullptr;
    // Since the last take(): real target binds made by the run path (an issue's bind of a run kind, and every end), runs started, runs ended (by a bind or dropped), and the game's
    // own sets of targets seen inside a run: of the saved targets, of others, and of the layer's own.
    uint64_t binds = 0, started = 0, ended = 0, setsKept = 0, setsDropped = 0, setsOwn = 0;

    // An issue is about to bind its targets. runIssue: it is a map sprite, canvas or mark issue (colour or depth) with the eye's D16 in hand.
    Step begin(bool runIssue, int issueEye, const void* rtv, const void* dsv) {
        if (held) {
            if (runIssue && issueEye == eye && rtv == layerRtv && dsv == layerDsv) return Step::kKeep;
            end();
            if (runIssue) ++binds;
            return Step::kEndThenBind;
        }
        if (runIssue) ++binds;
        return Step::kBind;
    }

    // The issue has been drawn and its restore finds it bound the run's targets: the run is held with them, the game's targets in the caller's keeping. True when this started the run
    // (the caller takes the references then).
    bool handOver(int issueEye, const void* rtv, const void* dsv, const void* const (&game)[kTargets], const void* gameDepth) {
        if (held) return false;
        held = true;
        eye = issueEye;
        layerRtv = rtv;
        layerDsv = dsv;
        for (int i = 0; i < kTargets; ++i) gameRtv[i] = game[i];
        gameDsv = gameDepth;
        ++started;
        return true;
    }

    // The game sets the layer's own targets, with no UAVs. Nothing hooks OMGetRenderTargets, so a read inside a run returns the layer's and a read followed by a set hands them back. They
    // are the ones bound and the game's are still the saved ones, so nothing changes. True when this call is that.
    bool onOwnSet(unsigned n, const void* const* rtvs, const void* dsv) {
        if (!held || !rtvs || n < 1 || n > static_cast<unsigned>(kTargets) || rtvs[0] != layerRtv || dsv != layerDsv) return false;
        for (unsigned i = 1; i < n; ++i) {
            if (rtvs[i]) return false;
        }
        ++setsOwn;
        return true;
    }

    // The game sets render targets (n of them, with or without UAVs).
    GameSet onGameSet(unsigned n, const void* const* rtvs, const void* dsv, bool hasUav) {
        if (!held) return GameSet::kNotHeld;
        bool same = !hasUav && dsv == gameDsv && n <= static_cast<unsigned>(kTargets);
        for (int i = 0; same && i < kTargets; ++i) same = (static_cast<unsigned>(i) < n && rtvs ? rtvs[i] : nullptr) == gameRtv[i];
        if (same) {
            ++setsKept;
            return GameSet::kKeep;
        }
        ++setsDropped;
        drop();
        return GameSet::kDrop;
    }

    // The run ends with a bind of the game's targets (the caller makes it). False when no run is held.
    bool end() {
        if (!held) return false;
        held = false;
        ++ended;
        ++binds;
        return true;
    }

    // The run is over with no bind: the game, a clear of the state or a released view replaced the targets.
    void drop() {
        if (!held) return;
        held = false;
        ++ended;
    }

    // The counters, cleared.
    struct Counts {
        uint64_t binds, started, ended, setsKept, setsDropped, setsOwn;
    };
    Counts take() {
        const Counts c{binds, started, ended, setsKept, setsDropped, setsOwn};
        binds = started = ended = setsKept = setsDropped = setsOwn = 0;
        return c;
    }
};

} // namespace maprun
} // namespace edvr

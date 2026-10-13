// Whether the game's mode request is rewritten (vr_window_trampolines.cpp's requestReplacement). Pure and free of any Windows API, so
// tools\runtime_profile_test checks every combination here.
#pragma once

#include "../common/install_scope.h"

namespace edvr {

// The rewrite runs in the VR profile. In Flat and Passthrough it runs only when the install scope includes VR and the runtime's VR
// session is live. At a 3D-on the game asks for its windowed mode before Settings.xml is written, so the profile is still flat then, and
// flight D (2026-10-12) left that request unrewritten: the game came up 1280x768 windowed and saved so. The live check keeps a flat
// launch with no VR session out of it. Invalid never rewrites.
inline bool vrWindowRewriteAllowed(RuntimeProfile current, bool scopeIncludesVr, bool vrLive) {
    switch (current) {
    case RuntimeProfile::Vr:
    case RuntimeProfile::LegacyVr: return true;
    case RuntimeProfile::Flat:
    case RuntimeProfile::Passthrough: return scopeIncludesVr && vrLive;
    default: return false;
    }
}

}  // namespace edvr

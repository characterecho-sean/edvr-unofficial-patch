// fix.fss_eye_sync -- ONE key for the scanner's black-squares fix.
//
// The fix has two mechanisms because the squares live in two pipeline
// layers: a submitted-image heal for the zoom-arrival frames (where the
// body composite never draws, so no draw-level fix can reach), and an
// input freeze for the composite-drawn resolve that follows. They shipped
// in 0.11.0 as two keys -- fss_eye_heal and fss_reveal_sync -- purely
// because they were built as two campaign artifacts; no user ever wants
// half the fix, so the key surface is now the function, not the
// mechanism:
//
//   on      both halves (the fix; the default)
//   off     stock
//
// and, documented as developer instruments, one mechanism each:
//
//   heal    the arrival fill alone (mode 1)
//   mirror  the arrival mirror alone (mode 2: stamp the left's blacks
//           into the right -- squares in both eyes, deliberately)
//   sync    the composite input freeze alone (lockstep)
//   steady  the composite constants snapshot alone
//
// Legacy spellings from the two retired keys parse silently -- 1 -> on,
// 0/stock -> off, 2 -> mirror, lockstep -> sync -- so an old ini, or a
// value the installer's merge carried across the key rename, still means
// what its author meant. An unrecognised value is the DEFAULT (on) with a
// note, per getBool's philosophy: a typo must not switch the fix off.
//
// Header-only and dependency-free because four modules across both DLLs
// parse the same value; a shared translation unit would drag link-order
// into what is one switch statement.
#pragma once

#include <string>

namespace edvr {

struct EyeSync {
    int  healMode = 0;   // 0 off | 1 fill | 2 mirror
    bool lockstep = false;
    bool steady = false;
    bool recognised = true;   // false: caller notes once, runs the default

    bool any() const { return healMode != 0 || lockstep || steady; }
};

inline EyeSync eyeSyncParse(std::string v) {
    for (char& c : v) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    EyeSync s;
    if (v == "off" || v == "0" || v == "stock" || v == "false" || v == "no") {
        return s;
    }
    if (v == "heal" || v == "1") {
        s.healMode = 1;
        // "1" was fss_eye_heal's fix value; alone it now means the whole
        // fix -- the halves were never meant to run separately by users.
        if (v == "1") s.lockstep = true;
        return s;
    }
    if (v == "mirror" || v == "2") {
        s.healMode = 2;
        return s;
    }
    if (v == "sync" || v == "lockstep") {
        s.lockstep = true;
        return s;
    }
    if (v == "steady") {
        s.steady = true;
        return s;
    }
    if (!(v == "on" || v == "true" || v == "yes" || v.empty())) {
        s.recognised = false;   // fall through to the default: on
    }
    s.healMode = 1;
    s.lockstep = true;
    return s;
}

// The one read everybody shares. The compiled default is ON -- matching
// the shipped ini, per the fss_panel_distance lesson: a missing line and
// the shipped default must mean the same thing.
template <typename ConfigT>
inline EyeSync eyeSyncFromConfig(ConfigT& cfg) {
    return eyeSyncParse(cfg.getString("fix.fss_eye_sync", "on"));
}

}  // namespace edvr

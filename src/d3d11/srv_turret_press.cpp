#include "srv_turret_press.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "../common/config.h"
#include "../common/hotkey.h"
#include "../common/log.h"
#include "elite_binds.h"

namespace edvr {
namespace {

constexpr uint64_t kCheckMs = 3000;   // how often the bindings' files are stamped (two equal changed stamps in a row re-read, like hmd_reset_press.cpp)

struct Watch {
    EliteResetBind bind{};
    bool read = false;
    bool readOk = false;
    bool readingOff = false;
    bool watched[2] = {false, false};
    Hotkey hk[2];
    uint32_t epoch = 0;
    uint64_t checkAt = 0;
    unsigned long long fingerprint = 0, pending = 0;
};
Watch* g_watch = nullptr;

const char* kindName(HotkeyKind k) {
    return k == HotkeyKind::Key ? "keyboard or mouse button" : k == HotkeyKind::Pad ? "xinput pad" : k == HotkeyKind::Joy ? "joystick, watched through the game's own DirectInput reads" : "none";
}

const char* whyNot(const Watch& w) {
    if (w.readingOff) return srv_turret::pressmsg::kReadingOff;
    if (!w.readOk) return srv_turret::pressmsg::kUnreadable;
    if (!w.bind.present || w.bind.count == 0) return srv_turret::pressmsg::kUnbound;
    for (int i = 0; i < w.bind.count && i < 2; ++i)
        if (!w.bind.slot[i].observable || !w.watched[i]) return srv_turret::pressmsg::kUnwatchable;
    return "";
}

void readBindings(Watch& w) {
    w.read = true;
    ++w.epoch;
    memset(&w.bind, 0, sizeof(w.bind));
    memset(w.watched, 0, sizeof(w.watched));
    w.hk[0].setBinding("");
    w.hk[1].setBinding("");
    w.readingOff = !Config::get().getBool("hotkey.read_game_bindings", true);
    w.readOk = false;
    if (!w.readingOff) {
        w.readOk = eliteBindsActionSrv("ToggleBuggyTurretButton", &w.bind) || w.bind.filesSeen > 0;
        for (int i = 0; i < w.bind.count && i < 2; ++i) {
            if (!w.bind.slot[i].observable) continue;
            char text[96];
            if (hotkeyFormatBinding(w.bind.slot[i].binding, text, sizeof(text)) == 0) continue;
            w.hk[i].setBinding(text);
            w.hk[i].setGameMirrored(true);      // Elite takes input through DirectInput while unfocused; it is the game's key, not EDVR's
            w.watched[i] = w.hk[i].bound();
        }
    }
    // One line each time the answer changes: which slots are watched and which cannot be seen.
    char slots[300] = "";
    size_t at = 0;
    int seen = 0, blind = 0;
    for (int i = 0; i < w.bind.count && i < 2; ++i) {
        const bool ok = w.bind.slot[i].observable && w.watched[i];
        (ok ? seen : blind)++;
        at += snprintf(slots + at, sizeof(slots) - at, "%s[%d] %s (%s)", i ? "; " : "", i, w.bind.slot[i].label, ok ? kindName(w.bind.slot[i].binding.kind) : "CANNOT be seen: a device or key nothing here watches");
        if (at >= sizeof(slots)) break;
    }
    const char* why = whyNot(w);
    Log::get().note("srv turret press: Elite's ToggleBuggyTurretButton in %s: %d bound slot(s)%s%s; %d watched, %d cannot be seen. %s", w.bind.file[0] ? w.bind.file : "(no file)", w.bind.count,
                    w.bind.count ? ": " : "", slots, seen, blind, why[0] ? "The press cannot be relied on: the turret view keeps the gunner-sheet watch (the hold starts at the sheet)." : "The press opens the window in which the gunner sheet raises the turret view.");
}

}  // namespace

SrvTurretPressState srvTurretPressPoll() {
    if (!g_watch) g_watch = new Watch();
    Watch& w = *g_watch;
    const uint64_t now = GetTickCount64();
    if (!w.read) {
        readBindings(w);
        w.fingerprint = eliteBindsFingerprint();
        w.checkAt = now + kCheckMs;
    } else if (now >= w.checkAt) {
        w.checkAt = now + kCheckMs;
        const unsigned long long fp = eliteBindsFingerprint();
        if (fp == w.fingerprint) {
            w.pending = 0;
        } else if (fp == w.pending) {      // the change held across two looks: an Apply writes several files
            w.fingerprint = fp;
            w.pending = 0;
            readBindings(w);
        } else {
            w.pending = fp;
        }
    }
    SrvTurretPressState s;
    s.epoch = w.epoch;
    s.why = whyNot(w);
    s.observable = s.why[0] == '\0';
    for (int i = 0; i < 2; ++i) {
        if (w.watched[i] && w.hk[i].pressed()) s.pressed = true;
    }
    return s;
}

}  // namespace edvr

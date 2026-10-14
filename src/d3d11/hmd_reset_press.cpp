#include "hmd_reset_press.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/hotkey.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "../common/seat_context.h"
#include "elite_binds.h"

namespace edvr {
namespace {

constexpr uint64_t kCheckMs = 3000;   // how often the bindings' files are stamped (two equal changed stamps in a row re-read, like tickEliteBinds)

struct Watch {
    EliteResetBind bind{};
    bool read = false;
    bool readOk = false;
    bool watched[2] = {false, false};
    bool proven[2] = {false, false};
    Hotkey hk[2];
    uint32_t seq = 0;
    uint64_t lastPressTick = 0;
    HmdPressStatus status = HmdPressStatus::Unpublished;
    uint64_t checkAt = 0;
    unsigned long long fingerprint = 0, pending = 0;
};
Watch* g_watch = nullptr;

const char* kindName(HotkeyKind k) {
    return k == HotkeyKind::Key ? "keyboard or mouse button" : k == HotkeyKind::Pad ? "xinput pad" : k == HotkeyKind::Joy ? "joystick, watched through the game's own DirectInput reads" : "none";
}

HmdPressStatus statusOf(const Watch& w) {
    if (!w.readOk) return HmdPressStatus::Unreadable;
    if (!w.bind.present || w.bind.count == 0) return HmdPressStatus::NoBind;
    bool allObservable = true, allProven = true;
    for (int i = 0; i < w.bind.count; ++i) {
        if (!w.bind.slot[i].observable) { allObservable = false; continue; }
        if (w.bind.slot[i].needsProof && !w.proven[i]) allProven = false;
    }
    if (!allObservable) return HmdPressStatus::UnobservableDevice;     // a press on the unwatched slot would be invisible
    return allProven ? HmdPressStatus::Observable : HmdPressStatus::Unproven;
}

void publishLabels(const Watch& w) {
    char labels[2][16] = {};
    for (int i = 0; i < w.bind.count && i < 2; ++i) {
        const char* l = w.bind.slot[i].label;
        if (_strnicmp(l, "Keyboard:", 9) == 0) l += 9;      // the key alone says it is the keyboard
        snprintf(labels[i], sizeof(labels[i]), "%s", l);    // 15 characters and a NUL: a long joystick spelling is cut, in the runtime's log only
    }
    publishHmdResetLabels(labels);
}

void readBindings(Watch& w) {
    w.read = true;
    memset(&w.bind, 0, sizeof(w.bind));
    memset(w.watched, 0, sizeof(w.watched));
    memset(w.proven, 0, sizeof(w.proven));
    w.hk[0].setBinding("");
    w.hk[1].setBinding("");
    if (!Config::get().getBool("hotkey.read_game_bindings", true)) {
        w.readOk = true;      // not an unreadable file: reading the game's bindings is off, so nothing is known about the key
        w.status = HmdPressStatus::NoBind;
        Log::get().note("hmd reset key: hotkey.read_game_bindings is off, so Elite's HMDReset binding is not read; the seat memory cannot tell "
                        "your press from the game's reset and keeps the timing rules (window + hold).");
        publishLabels(w);
        return;
    }
    w.readOk = eliteBindsHmdReset(&w.bind) || w.bind.filesSeen > 0;
    for (int i = 0; i < w.bind.count && i < 2; ++i) {
        if (!w.bind.slot[i].observable) continue;
        char text[96];
        if (hotkeyFormatBinding(w.bind.slot[i].binding, text, sizeof(text)) == 0) continue;
        w.hk[i].setBinding(text);
        w.hk[i].setGameMirrored(true);      // Elite takes input through DirectInput while unfocused; it is the game's key, not EDVR's
        w.watched[i] = w.hk[i].bound();
    }
    w.status = statusOf(w);
    publishLabels(w);
    // One line each time the answer changes: which bind was found, on which device, and whether it can be observed.
    char slots[300] = "";
    size_t at = 0;
    for (int i = 0; i < w.bind.count && i < 2; ++i) {
        at += snprintf(slots + at, sizeof(slots) - at, "%s[%d] %s (%s)", i ? "; " : "", i, w.bind.slot[i].label,
                       w.bind.slot[i].observable ? kindName(w.bind.slot[i].binding.kind) : "NOT observable: a device or key nothing here watches");
        if (at >= sizeof(slots)) break;
    }
    Log::get().note("hmd reset key: Elite's HMDReset in %s: %d bound slot(s)%s%s. Seat memory press status: %s%s", w.bind.file[0] ? w.bind.file : "(no file)",
                    w.bind.count, w.bind.count ? ": " : "", slots, hmdPressStatusName(w.status),
                    w.status == HmdPressStatus::Observable ? " (a recentre without this press is the game's own)"
                    : w.status == HmdPressStatus::Unproven ? " (a pad or joystick slot is trusted after its first press seen; until then the timing rules stand)"
                                                           : " (the timing rules stand: window + hold)");
}

}  // namespace

void hmdResetPressFrame() {
    if (runtimeFlatProfile()) return;      // the flat profile has no runtime to tell
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
    for (int i = 0; i < 2; ++i) {
        if (!w.watched[i]) continue;
        if (w.hk[i].pressed()) {
            w.lastPressTick = now;
            w.seq = (w.seq + 1) & kHmdPressSeqMask;
            if (!w.proven[i]) {
                w.proven[i] = true;
                const HmdPressStatus next = statusOf(w);
                if (next != w.status) {
                    w.status = next;
                    Log::get().note("hmd reset key: slot %d (%s) saw its first press; seat memory press status: %s", i, w.bind.slot[i].label,
                                    hmdPressStatusName(w.status));
                }
            }
        }
    }
    HmdResetPress p;
    p.status = w.status;
    p.heldNow = false;      // reserved: the edge is what counts, at frame granularity (the runtime logs the gap, and a press seen just after a reset is corrected)
    p.seq = w.seq;
    publishHmdResetPress(encodeHmdPress(p), w.lastPressTick);
}

}  // namespace edvr

#include "hotkey_capture.h"

#include <cstdio>
#include <cstring>

#include <windows.h>

namespace edvr {

// ---------------------------------------------------------------------------
// Which keys can be captured

bool hotkeyCaptureKeyEligible(int vk) {
    if (vk == VK_BACK || vk == VK_TAB || vk == VK_CLEAR || vk == VK_RETURN || vk == VK_PAUSE ||
        vk == VK_CAPITAL || vk == VK_ESCAPE || vk == VK_APPS || vk == VK_NUMLOCK || vk == VK_SCROLL ||
        vk == VK_OEM_102) {
        return true;
    }
    if (vk >= VK_SPACE && vk <= VK_DOWN) return true;        // space, page up/down, end, home, arrows
    if (vk >= VK_SNAPSHOT && vk <= VK_HELP) return true;     // print screen, insert, delete, help
    if (vk >= '0' && vk <= '9') return true;
    if (vk >= 'A' && vk <= 'Z') return true;
    if (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) return true;    // the numpad and its operators
    if (vk >= VK_F1 && vk <= VK_F24) return true;
    if (vk >= 0xBA && vk <= 0xC0) return true;               // ; = , - . / `
    if (vk >= 0xDB && vk <= 0xDF) return true;               // [ \ ] ' and the layout's own
    return false;
}

// ---------------------------------------------------------------------------
// The state machine

void HotkeyCapture::begin(const CaptureSnapshot& now) {
    m_prev = now;   // everything held now is "already down": it counts after a release
    m_active = true;
}

CaptureStep HotkeyCapture::step(const CaptureSnapshot& now, bool canClear) {
    CaptureStep out;
    if (!m_active) return out;
    const CaptureSnapshot prev = m_prev;
    m_prev = now;

    // The keyboard first: a chord's modifiers are whatever is held at the moment
    // its key goes down.
    for (int vk = 1; vk < 256; ++vk) {
        if (!hotkeyCaptureKeyEligible(vk) || !now.keyDown[vk] || prev.keyDown[vk]) continue;
        if (now.mods == 0) {
            // A bare Esc, Delete or Backspace is a command. With a modifier held
            // it is a chord like any other (Shift+Delete can be a hotkey).
            if (vk == VK_ESCAPE) {
                out.kind = CaptureKind::Cancelled;
                return out;
            }
            if (vk == VK_DELETE || vk == VK_BACK) {
                out.kind = canClear ? CaptureKind::Cleared : CaptureKind::ClearRefused;
                return out;
            }
        }
        out.kind = CaptureKind::Captured;
        out.binding.kind = HotkeyKind::Key;
        out.binding.vk = vk;
        out.binding.mods = now.mods;
        return out;
    }

    // A pad: the first button that went down, else a trigger. One button; a pad
    // chord is not captured.
    const uint16_t newButtons = static_cast<uint16_t>(now.padButtons & ~prev.padButtons);
    if (newButtons) {
        uint16_t bit = 1;
        while (!(newButtons & bit)) bit = static_cast<uint16_t>(bit << 1);
        out.kind = CaptureKind::Captured;
        out.binding.kind = HotkeyKind::Pad;
        out.binding.padButtons = bit;
        return out;
    }
    const uint8_t newTriggers = static_cast<uint8_t>(now.padTriggers & ~prev.padTriggers);
    if (newTriggers) {
        out.kind = CaptureKind::Captured;
        out.binding.kind = HotkeyKind::Pad;
        out.binding.padTrigger = (newTriggers & 1) ? 1 : 2;
        return out;
    }

    // A joystick or HOTAS button, or a hat direction, on any device.
    uint32_t device = 0;
    uint16_t input = 0;
    if (joyFirstNew(prev.joy, now.joy, &device, &input)) {
        out.kind = CaptureKind::Captured;
        out.binding.kind = HotkeyKind::Joy;
        out.binding.joyDevice = device;
        out.binding.joyInput = input;
        return out;
    }
    return out;
}

// ---------------------------------------------------------------------------
// The checks

bool hotkeyClashes(const HotkeyBinding& a, const HotkeyBinding& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case HotkeyKind::Key:
            if (a.vk != b.vk || !a.vk) return false;
            // SHIFT+F5 and CTRL+F5 are two presses; F5 is part of both of them.
            return !((a.mods & ~b.mods) != 0 && (b.mods & ~a.mods) != 0);
        case HotkeyKind::Pad:
            return a.padButtons == b.padButtons && a.padTrigger == b.padTrigger &&
                   (a.padButtons != 0 || a.padTrigger != 0);
        case HotkeyKind::Joy:
            return a.joyDevice == b.joyDevice && a.joyInput == b.joyInput;
        default:
            return false;
    }
}

bool eliteElementIsOnFoot(const char* element) {
    if (!element || !*element) return false;
    if (strstr(element, "Humanoid")) return true;      // the on-foot controls, and the *_Humanoid twins
    if (strncmp(element, "UI_", 3) == 0) return true;  // the panel keys work wherever a panel is up
    if (strncmp(element, "Photo", 5) == 0) return true;
    if (strstr(element, "FreeCam")) return true;
    return false;
}

bool hotkeyReservedByMenu(const HotkeyBinding& b) {
    if (b.kind != HotkeyKind::Key) return false;
    // WHATEVER THE MODIFIERS (review 2026-10-08, finding 2). The menu reads these keys raw
    // (GetAsyncKeyState) and never asks what else is held, so CTRL+ENTER pressed in the
    // menu is also its Enter, and CTRL+ALT+ESCAPE is also its Escape: a chord does not move
    // the key out of the menu's reach. An exemption for Ctrl and Alt here would be a rule the
    // dispatch does not keep.
    switch (b.vk) {
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
        case VK_RETURN: case VK_SPACE: case VK_TAB:
        case VK_PRIOR: case VK_NEXT: case VK_HOME: case VK_END:
        case VK_ESCAPE: case 'R':
            return true;
        default:
            return false;
    }
}

BindCheck hotkeyCheckBinding(const HotkeyBinding& b, const HotkeyOther* others, int nOthers,
                             const EliteBindUse* uses, int nUses, ClashScope scope) {
    BindCheck r;
    if (b.kind == HotkeyKind::None) return r;
    if (hotkeyReservedByMenu(b)) {
        r.verdict = BindVerdict::Reserved;
        return r;
    }
    for (int i = 0; i < nOthers; ++i) {
        if (others[i].binding.kind != HotkeyKind::None && hotkeyBindingsEqual(b, others[i].binding)) {
            r.verdict = BindVerdict::Duplicate;
            r.duplicateOf = others[i].dotted;
            return r;
        }
    }
    int shown = 0;
    for (int i = 0; i < nUses; ++i) {
        const EliteBindUse& u = uses[i];
        if (scope == ClashScope::OnFoot && !eliteElementIsOnFoot(u.element)) continue;
        if (!hotkeyClashes(b, u.binding)) continue;
        // One element can sit on the key twice (both slots); name it once.
        bool seen = false;
        for (int k = 0; k < i; ++k) {
            if (strcmp(uses[k].element, u.element) == 0 && hotkeyClashes(b, uses[k].binding) &&
                (scope == ClashScope::AnyContext || eliteElementIsOnFoot(uses[k].element))) {
                seen = true;
                break;
            }
        }
        if (seen) continue;
        ++r.clashCount;
        if (shown < 3) {
            const size_t len = strlen(r.clashList);
            snprintf(r.clashList + len, sizeof(r.clashList) - len, "%s%s", shown ? ", " : "", u.element);
            ++shown;
        }
    }
    if (r.clashCount > shown) {
        const size_t len = strlen(r.clashList);
        snprintf(r.clashList + len, sizeof(r.clashList) - len, " (+%d more)", r.clashCount - shown);
    }
    return r;
}

bool hotkeyRowLocked(const char* rowKey, bool explorerCamSession) {
    return explorerCamSession && rowKey && strcmp(rowKey, "explorer_cam") == 0;
}

size_t hotkeyLockedText(const char* keyText, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    snprintf(out, cap, "Leave Explorer Cam (%s) to change its key", keyText && keyText[0] ? keyText : "its key");
    return strlen(out);
}

bool hotkeyCaptureMayBegin(const HotkeyRowContext& ctx) {
    return !hotkeyRowLocked(ctx.rowKey, ctx.explorerCamSession);
}

CaptureDecision hotkeyDecide(const CaptureStep& step, const HotkeyRowContext& ctx) {
    CaptureDecision d;
    switch (step.kind) {
        case CaptureKind::Waiting:
            return d;
        case CaptureKind::Cancelled:
            d.outcome = CaptureOutcome::Cancelled;
            return d;
        default:
            break;
    }
    // Everything below would change the row, or says why it will not.
    if (hotkeyRowLocked(ctx.rowKey, ctx.explorerCamSession)) {
        d.outcome = CaptureOutcome::Locked;
        return d;
    }
    if (step.kind == CaptureKind::ClearRefused ||
        (step.kind == CaptureKind::Cleared && ctx.rowKey && strcmp(ctx.rowKey, "menu") == 0)) {
        d.outcome = CaptureOutcome::ClearRefusedMenu;
        return d;
    }
    if (step.kind == CaptureKind::Cleared) {
        HotkeyBinding now;
        hotkeyParseBinding(ctx.currentText, &now, /*quiet=*/true);
        d.outcome = now.kind == HotkeyKind::None ? CaptureOutcome::ClearAlready : CaptureOutcome::Clear;
        return d;
    }
    // Captured.
    d.binding = step.binding;
    d.check = hotkeyCheckBinding(step.binding, ctx.others, ctx.nOthers, ctx.uses, ctx.nUses, ctx.scope);
    if (d.check.verdict == BindVerdict::Reserved) {
        d.outcome = CaptureOutcome::Reserved;
        return d;
    }
    if (d.check.verdict == BindVerdict::Duplicate) {
        d.outcome = CaptureOutcome::Duplicate;
        return d;
    }
    HotkeyBinding now;
    hotkeyParseBinding(ctx.currentText, &now, /*quiet=*/true);
    d.outcome = hotkeyBindingsEqual(now, step.binding) ? CaptureOutcome::Unchanged : CaptureOutcome::Bind;
    return d;
}

int hotkeyPageRows(const MenuRowDef* rows, int n, bool developer, int* out, int max) {
    int count = 0;
    for (int i = 0; i < n && count < max; ++i) {
        const MenuRowDef& d = rows[i];
        if (d.kind != MenuKind::Hotkey || strcmp(d.page, "hotkeys") != 0) continue;
        if (d.tier != MenuTier::Fix && !developer) continue;
        out[count++] = i;
    }
    return count;
}

}  // namespace edvr

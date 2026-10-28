// F162 (native on-foot VR): the ADS sight eye, fix.native_on_foot_ads_sight_eye = both | right | left. The pure part: the key's text, and which eye the sight lock lines up.
// Design and journal: the arc doc, F162.
//
// Header only, no game or D3D11 state, so native_frame_test runs the very functions the frame thread uses.
//
//   both   the right eye is lined up on the sights and the left eye shows the right eye's view (F106): the share, the lock on the right eye.
//   right  the right eye is lined up on the sights, each eye keeps its own view (the normal stereo view the F106 key's off gave).
//   left   the left eye is lined up on the sights, each eye keeps its own view; F106's share never runs.
//
// The key replaced fix.native_on_foot_ads_sight_both_eyes (on | off). edvr.ini carries a saved value over through its `# moved-from:` line (on becomes both, off becomes right); a
// file that reaches the reader with the old spelling still in the new key (a hand-copied DLL reads an old-layout file through the audit's fallback, which copies the text) is read
// the same way, so on and off are accepted here as that carried text and never written back.
#pragma once

#include <cstring>

namespace edvr {
namespace nofoot {
namespace f162 {

enum SightEye : int { kBoth = 0, kRight = 1, kLeft = 2, kModes = 3 };

inline const char* name(int mode) noexcept { return mode == kRight ? "right" : mode == kLeft ? "left" : "both"; }

// The key's text (case-insensitive, either side trimmed of spaces). *known is false for text that is none of the three or the carried on and off; the answer is then both, the
// recommended value. on is the old key's default (both); off is the normal stereo view the old key gave (right).
inline SightEye parse(const char* text, bool* known) noexcept {
    if (known) *known = true;
    if (!text) {
        if (known) *known = false;
        return kBoth;
    }
    while (*text == ' ' || *text == '\t') ++text;
    size_t n = std::strlen(text);
    while (n && (text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\r' || text[n - 1] == '\n')) --n;
    auto is = [&](const char* word) noexcept {
        const size_t w = std::strlen(word);
        if (n != w) return false;
        for (size_t i = 0; i < w; ++i) {
            char c = text[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != word[i]) return false;
        }
        return true;
    };
    if (is("both") || is("on")) return kBoth;
    if (is("right") || is("off")) return kRight;
    if (is("left")) return kLeft;
    if (known) *known = false;
    return kBoth;
}

// The eye the sight lock lines up on the sights: 0 the left eye, 1 the right eye. Both and right are the right eye, bit for bit what the lock did before the key had three values.
inline int aimingEye(int mode) noexcept { return mode == kLeft ? 0 : 1; }
inline const char* aimingName(int aim) noexcept { return aim == 0 ? "left" : "right"; }

// F106's share (the left eye shows the right eye's view) runs in the both mode only: the right eye is the eye shown, so the left mode has no eye to copy and the right mode keeps stereo.
inline bool sharesView(int mode) noexcept { return mode == kBoth; }

}  // namespace f162
}  // namespace nofoot
}  // namespace edvr

// The game's 3D mode, read from Settings.xml's text. Pure and free of any Windows API, so the launch decision (install_scope.h), the
// Supersampling hold (src\d3d11\vr_ssaa_hold_math.h) and the rigs that drive them share one parser.
#pragma once

#include <cstring>

namespace edvr {

// The value of <StereoscopicMode> in Settings.xml's text. True with *mode = N when the tag holds a whole number from 0 to 6 (the
// game clamps it to that range), with whitespace allowed around it and the complete </StereoscopicMode> closing tag after it; false
// when the tag is absent or holds anything else, a truncated or mismatched closing tag included.
inline bool parseStereoscopicMode(const char* xml, int* mode) {
    static const char kTag[] = "<StereoscopicMode>";
    const char* at = xml ? std::strstr(xml, kTag) : nullptr;
    if (!at) return false;
    const char* p = at + (sizeof(kTag) - 1);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p < '0' || *p > '9') return false;
    int v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        if (v > 6) return false;
        ++p;
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    // The closing tag must follow in full. Settings.xml is read while the game may be rewriting it, so a cut-off "0<" or "0</Stereo" is
    // not a value; strncmp stops at the end of the text, so a short read cannot run past it.
    static const char kClose[] = "</StereoscopicMode>";
    if (std::strncmp(p, kClose, sizeof(kClose) - 1) != 0) return false;
    *mode = v;
    return true;
}

}  // namespace edvr

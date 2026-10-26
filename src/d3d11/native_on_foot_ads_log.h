#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "native_on_foot_logwin.h"

namespace edvr { namespace nofoot { namespace adslog {
// Frame-thread diagnostics only. Poll before the current frame changes the
// snapshot: an edge closes the old context, then opens the new one. Each normal
// second folds once; a partial edge contributes one final sampled second.
struct Window {
    uint64_t last = 0, context = 0;
    uint32_t seconds = 0;
    bool open = false, detailed = false;
    template<class Fold, class Emit>
    void poll(uint64_t now, bool live, uint64_t key, Fold fold, Emit emit) noexcept {
        const bool d = logwin::detail();
        const bool edge = open && (!live || key != context || d != detailed || now < last);
        if (edge) {
            if (now != last || !seconds) { fold(); ++seconds; }
            emit(seconds);
            open = false; seconds = 0;
        }
        if (!live) return;
        if (!open) { open = true; last = now; context = key; detailed = d; return; }
        if (now - last < 1000) return;
        last = now; fold(); ++seconds;
        if (seconds >= logwin::windowSeconds(detailed)) { emit(seconds); seconds = 0; }
    }
};
inline uint64_t mix(uint64_t h, uint64_t v) noexcept { return (h ^ v) * 1099511628211ull; }
inline uint64_t number(uint64_t h, double v) noexcept {
    uint64_t bits = 0; std::memcpy(&bits, &v, sizeof(bits)); return mix(h, bits);
}
inline uint64_t text(uint64_t h, const char* v) noexcept {
    while (*v) h = mix(h, static_cast<unsigned char>(*v++)); return mix(h, 0xff);
}
struct HoldStates {
    uint32_t n[6] = {};
    static int state(const char* s) noexcept {
        return !std::strncmp(s,"holding",7) ? 0 : !std::strncmp(s,"frozen",6) ? 1 :
            !std::strncmp(s,"no pose",7) ? 2 : !std::strncmp(s,"ramping",7) ? 3 :
            !std::strncmp(s,"out of scope",12) ? 4 : 5;
    }
    void fold(const char* s) noexcept { ++n[state(s)]; }
    void reset() noexcept { *this = HoldStates{}; }
};
struct OffsetStates {
    uint32_t armed = 0, ads = 0, high = 0, moved = 0;
    double weights[10] = {}, lo = 1.0, hi = 0.0;
    void fold(bool a, bool ad, double w, bool move) noexcept {
        if (ad) ++ads;
        if (!a) return;
        if (armed < 10) weights[armed] = w;
        ++armed; lo = std::min(lo,w); hi = std::max(hi,w);
        if (w >= 0.9) ++high;
        if (move) ++moved;
    }
    // These are the individual second weights, not a median of window medians.
    void weightText(char* out, size_t cap) const noexcept {
        size_t used = 0; if (cap) out[0] = 0;
        for (uint32_t i=0;i<armed && i<10;++i) {
            const int n = std::snprintf(out+used, cap-used, "%s%.2f", i ? "," : "", weights[i]);
            if (n < 0 || static_cast<size_t>(n) >= cap-used) break;
            used += static_cast<size_t>(n);
        }
    }
    void reset() noexcept { *this = OffsetStates{}; }
};
}}} // namespace edvr::nofoot::adslog

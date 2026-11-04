// TEMPORARY (VR draw path CPU, build 6, item 4; docs\vr-draw-path-cpu-2026-11-01.md): the counts that judge build 6, and the window line that prints them beside
// "draw mask:" in perf_monitor.cpp. It replaces build 5's "build 5 counts:" line. Remove with the arc.
//
// eye_misses, holo_misses: the ui_depth eye draws' memo and the hologram pass's memo, counted in the real miss, after the memo-hit test. S2 (the eye key ignores a
// same-pointer PS SRV 0..3 re-set) is judged on eye_misses per eye draw against build 5's.
//
// drift_checks, drift_mismatches, mask_bits: the interest mask's drift check (draw_interest.h). bindingAudit's 1-in-1024 owner draw recomputes every module's bits from the
// real expressions and compares them with State::interest. drift_checks above 0 is how the log tells "the check ran and found nothing" from "the check never ran";
// drift_mismatches must stay 0; mask_bits is how many of the mirrored bits were set at the last check (the others were clear, so their cache lines were not touched).
//
// Plain counters, as build 4's and 5's were: written by the draw thread (a deferred context's thread can race a count, which a temporary count tolerates). The line prints
// the change since the last window as key=value pairs (mask_bits is a level, not a change); tools\edvr_log.py --perf-windows parses it, and its self-test holds the fragments
// to this file.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {

struct UiDepthB6Counts {
    uint64_t eyeMisses = 0, holoMisses = 0;
};

struct DrawInterestDrift {
    uint64_t checks = 0, mismatches = 0;
    uint32_t bitsSet = 0;   // the mask's popcount at the last check
};

struct B6Counts {
    UiDepthB6Counts ui;
    DrawInterestDrift drift;
};

inline B6Counts b6Minus(const B6Counts& a, const B6Counts& b) {
    B6Counts d;
    d.ui.eyeMisses = a.ui.eyeMisses - b.ui.eyeMisses;
    d.ui.holoMisses = a.ui.holoMisses - b.ui.holoMisses;
    d.drift.checks = a.drift.checks - b.drift.checks;
    d.drift.mismatches = a.drift.mismatches - b.drift.mismatches;
    d.drift.bitsSet = a.drift.bitsSet;   // a level
    return d;
}

// The change since the last window, formatted: about 200 characters.
inline void formatB6Window(char* out, size_t cap, unsigned frame, const B6Counts& c) {
    std::snprintf(out, cap,
                  "build 6 counts: 1800-frame window ending %u; eye_misses=%llu holo_misses=%llu; drift_checks=%llu drift_mismatches=%llu mask_bits=%u.",
                  frame, static_cast<unsigned long long>(c.ui.eyeMisses), static_cast<unsigned long long>(c.ui.holoMisses),
                  static_cast<unsigned long long>(c.drift.checks), static_cast<unsigned long long>(c.drift.mismatches), static_cast<unsigned>(c.drift.bitsSet));
}

}  // namespace edvr

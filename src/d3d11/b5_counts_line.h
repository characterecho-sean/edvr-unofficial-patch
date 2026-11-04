// TEMPORARY (VR draw path CPU, build 5, item 6; docs\vr-draw-path-cpu-2026-11-01.md): the counts that decide build 6, and the window line that prints them beside
// "draw mask:" in perf_monitor.cpp. It replaces build 4's "binding re-sets:" line. Remove with the arc.
//
// #184: the three per-draw depth flags (vscreen.cpp t_uiDepthThisDraw, t_holoDepthThisDraw, t_compositeThisDraw) are set by beginPanelOverride and consumed by
// forwardWithVerdict's UiDepthScope, but cleared only after beginPanelOverride's early returns, so a flag an owner draw left at the blocked return (uiLayerIssueBlocked)
// can reach a later draw. Each flag records a thread_local draw stamp (bumped first thing in beginPanelOverride) where it is set; a scope that consumes a flag set
// under another stamp is a stale consume. The totals (blocked returns, scope constructions) print beside the counts: zero blocked returns reads UNDETERMINED, not "never".
//
// S2's size (ui_depth.cpp): shadow memos for the eye draws' and the hologram pass's memos, keyed on the pointer-only eye key (binding_shadow.h
// bindingEyeKeyPtrGeneration), stored and checked only in the real miss, so shadow lookups equal real misses. ptr_would_hit: the shadow hits and the classifier then
// makes a plain decline (what S2 would save). ptr_would_disagree: the shadow hits and the classifier does not (S2 would have changed an answer).
//
// Also: viewIsSurface calls, item 3's front-cache lookups and hits, and engine velocity's slow-half entries (beforeDrawSlow; an equivalence check for item 4, not a
// never-ran signature: build 4 never logged it).
//
// Plain counters, as build 4's were: written by the draw thread (a deferred context's thread can race a count, which a temporary count tolerates). The line prints
// the change since the last window as key=value pairs; tools\edvr_log.py --perf-windows parses it, and its self-test holds the fragments to this file.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {

struct UiDepthB5Counts {
    uint64_t viewIsSurfaceCalls = 0, frontLookups = 0, frontHits = 0;
    uint64_t eyeMisses = 0, eyeShadowLookups = 0, eyePtrWouldHit = 0, eyePtrWouldDisagree = 0;
    uint64_t holoMisses = 0, holoShadowLookups = 0, holoPtrWouldHit = 0, holoPtrWouldDisagree = 0;
};

struct VScreenB5Counts {
    uint64_t blockedReturns = 0, blockedUi = 0, blockedHolo = 0, blockedComposite = 0;   // forwardWithVerdict's owner return on uiLayerIssueBlocked, and with each flag set
    uint64_t scopes = 0, staleUi = 0, staleHolo = 0, staleComposite = 0;                  // UiDepthScope constructions, and those consuming a flag set under another draw's stamp
    uint64_t evSlowEntries = 0;                                                           // engine velocity's beforeDrawSlow
};

struct B5Counts {
    UiDepthB5Counts ui;
    VScreenB5Counts vs;
};

inline B5Counts b5Minus(const B5Counts& a, const B5Counts& b) {
    B5Counts d;
    d.ui.viewIsSurfaceCalls = a.ui.viewIsSurfaceCalls - b.ui.viewIsSurfaceCalls;
    d.ui.frontLookups = a.ui.frontLookups - b.ui.frontLookups;
    d.ui.frontHits = a.ui.frontHits - b.ui.frontHits;
    d.ui.eyeMisses = a.ui.eyeMisses - b.ui.eyeMisses;
    d.ui.eyeShadowLookups = a.ui.eyeShadowLookups - b.ui.eyeShadowLookups;
    d.ui.eyePtrWouldHit = a.ui.eyePtrWouldHit - b.ui.eyePtrWouldHit;
    d.ui.eyePtrWouldDisagree = a.ui.eyePtrWouldDisagree - b.ui.eyePtrWouldDisagree;
    d.ui.holoMisses = a.ui.holoMisses - b.ui.holoMisses;
    d.ui.holoShadowLookups = a.ui.holoShadowLookups - b.ui.holoShadowLookups;
    d.ui.holoPtrWouldHit = a.ui.holoPtrWouldHit - b.ui.holoPtrWouldHit;
    d.ui.holoPtrWouldDisagree = a.ui.holoPtrWouldDisagree - b.ui.holoPtrWouldDisagree;
    d.vs.blockedReturns = a.vs.blockedReturns - b.vs.blockedReturns;
    d.vs.blockedUi = a.vs.blockedUi - b.vs.blockedUi;
    d.vs.blockedHolo = a.vs.blockedHolo - b.vs.blockedHolo;
    d.vs.blockedComposite = a.vs.blockedComposite - b.vs.blockedComposite;
    d.vs.scopes = a.vs.scopes - b.vs.scopes;
    d.vs.staleUi = a.vs.staleUi - b.vs.staleUi;
    d.vs.staleHolo = a.vs.staleHolo - b.vs.staleHolo;
    d.vs.staleComposite = a.vs.staleComposite - b.vs.staleComposite;
    d.vs.evSlowEntries = a.vs.evSlowEntries - b.vs.evSlowEntries;
    return d;
}

// The change since the last window, formatted: about 700 characters.
inline void formatB5Window(char* out, size_t cap, unsigned frame, const B5Counts& c) {
    std::snprintf(out, cap,
                  "build 5 counts: 1800-frame window ending %u; "
                  "blocked_returns=%llu blocked_ui=%llu blocked_holo=%llu blocked_composite=%llu scopes=%llu stale_ui=%llu stale_holo=%llu stale_composite=%llu; "
                  "eye_misses=%llu eye_shadow_lookups=%llu eye_ptr_would_hit=%llu eye_ptr_would_disagree=%llu; "
                  "holo_misses=%llu holo_shadow_lookups=%llu holo_ptr_would_hit=%llu holo_ptr_would_disagree=%llu; "
                  "view_is_surface_calls=%llu front_lookups=%llu front_hits=%llu; ev_slow_entries=%llu.",
                  frame, static_cast<unsigned long long>(c.vs.blockedReturns), static_cast<unsigned long long>(c.vs.blockedUi),
                  static_cast<unsigned long long>(c.vs.blockedHolo), static_cast<unsigned long long>(c.vs.blockedComposite), static_cast<unsigned long long>(c.vs.scopes),
                  static_cast<unsigned long long>(c.vs.staleUi), static_cast<unsigned long long>(c.vs.staleHolo), static_cast<unsigned long long>(c.vs.staleComposite),
                  static_cast<unsigned long long>(c.ui.eyeMisses), static_cast<unsigned long long>(c.ui.eyeShadowLookups),
                  static_cast<unsigned long long>(c.ui.eyePtrWouldHit), static_cast<unsigned long long>(c.ui.eyePtrWouldDisagree),
                  static_cast<unsigned long long>(c.ui.holoMisses), static_cast<unsigned long long>(c.ui.holoShadowLookups),
                  static_cast<unsigned long long>(c.ui.holoPtrWouldHit), static_cast<unsigned long long>(c.ui.holoPtrWouldDisagree),
                  static_cast<unsigned long long>(c.ui.viewIsSurfaceCalls), static_cast<unsigned long long>(c.ui.frontLookups),
                  static_cast<unsigned long long>(c.ui.frontHits), static_cast<unsigned long long>(c.vs.evSlowEntries));
}

}  // namespace edvr

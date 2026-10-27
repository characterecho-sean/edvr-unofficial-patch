// F121c (native on-foot VR): is the eye the filing names for a visor draw the eye it is? The pure parts: header only, no game or D3D11 state, so the rig
// (tools\visor_axis_test) and remlok_fix.cpp run the very decision, counters and format strings the headset does. Journal: the arc doc, 2026-10-25, F121c.
//
// WHY. F121 shifts the visor's frame by +a/2 for the left eye and -a/2 for the right, and takes the eye from the eye filing (ui_depth.cpp: the first target of a
// shape is the left eye's, the second the right's). In the flight of 2026-10-25 15:11 (edvr_gfx_20261025_151004, v0.21.1-281) the game drew ONE visor draw a frame
// (2491 in 30 s, against 5355 in v269 and 5036 in the second session of the same build), every one on the RIGHT eye's target, which the filing called the left eye:
// the left shift went onto the right eye's frame, the left layer's UI check failed 4919 times ("holds UI from the other eye's target", target = the right eye's
// submitted texture), and the marker pairs existed for eye 0 only. Why one eye's post-scene draws went missing in that session is not known (journal: the open
// question). The shift must not guess: it is bound only when the draws prove the filing.
//
// THE TRUST. A visor draw (eye e, target T) is trusted when
//   * the previous visor draw was the OTHER eye on ANOTHER target (the draws arrive as pairs, one a frame per eye, in either order), and
//   * the eye's target has not moved since it was last filed (a resize or a swapchain rebuild costs one untrusted draw), and
//   * where the eyes' submitted targets are known (the UI layer's uiLayerNoteSubmitted) and T is one of them, the eye of that submitted target is e.
// The first draw of a session, or after a run of untrusted ones, is untrusted (nothing to compare with). Untrusted draws run the game's own visor shader and are
// counted (Refusal::kOneEye in visor_axis_math.h).
//
// THE INSTRUMENT. Cheap by design (the F125 log diet): a line when the state changes (paired, one eye only, disagrees), after kTrustRun draws in the new state, and
// a 10 s summary only for a window with an anomaly or the session's first window; nothing at all otherwise.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace visoraxis {

constexpr unsigned kTrustRun = 12;   // consecutive draws in a new state before it is logged (about six frames)

// Why a draw was or was not trusted.
enum class Why : unsigned { kOk = 0, kFirst, kOneEye, kMoved, kSubmitted, kCount };
// The filing's state, as logged.
enum class FilingState : unsigned { kUnknown = 0, kPaired, kOneEye, kDisagrees };

inline const char* filingText(FilingState s) noexcept {
    switch (s) {
        case FilingState::kPaired: return "paired (one visor draw per eye, each on its own target)";
        case FilingState::kOneEye: return "ONE EYE ONLY (consecutive visor draws came to one eye or to one target)";
        case FilingState::kDisagrees: return "DISAGREES with the submitted eye targets (a draw filed to one eye is on the other eye's target)";
        default: return "unknown";
    }
}

inline unsigned long long ptrBits(const void* p) noexcept { return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p)); }

struct Tally {
    const void* target = nullptr;
    int eye = -1;
    unsigned long long draws = 0;
};

// One 10 s window of visor draws.
struct EyeWindow {
    unsigned long long draws = 0;
    unsigned long long why[static_cast<unsigned>(Why::kCount)] = {0, 0, 0, 0, 0};
    Tally tally[4];
    unsigned tallies = 0;
    unsigned events = 0;
    void add(Why w, int eye, const void* target) noexcept {
        ++draws;
        ++why[static_cast<unsigned>(w)];
        for (unsigned i = 0; i < tallies; ++i)
            if (tally[i].target == target && tally[i].eye == eye) { ++tally[i].draws; return; }
        if (tallies < 4) {
            tally[tallies].target = target;
            tally[tallies].eye = eye;
            tally[tallies].draws = 1;
            ++tallies;
        }
    }
    // Worth a line: a draw that came to one eye or one target, or disagreed with the submitted targets, or the filing moved more than the once a resize costs.
    bool anomalous() const noexcept {
        return why[static_cast<unsigned>(Why::kOneEye)] || why[static_cast<unsigned>(Why::kSubmitted)] || why[static_cast<unsigned>(Why::kMoved)] > 1;
    }
};

class EyeTrust {
public:
    struct Verdict {
        bool trusted = false;
        Why why = Why::kFirst;
        bool event = false;   // the state reached kTrustRun draws and differs from the last logged one: the caller logs formatEvent
    };

    // One visor draw: its eye from the filing (0 or 1), its target, and the eye of that target among the eyes' submitted targets (-1: not one of them, or not known).
    Verdict note(int eye, const void* target, int submittedEye) noexcept {
        Verdict v;
        if (eye != 0 && eye != 1) {   // the caller refuses a draw with no eye before it gets here; if one comes, it proves nothing
            v.why = Why::kOneEye;
            win_.add(v.why, eye, target);
            return v;
        }
        ++total_;
        Why w;
        if (submittedEye >= 0 && submittedEye != eye) w = Why::kSubmitted;
        else if (lastEye_ < 0) w = Why::kFirst;
        else if (lastEye_ == eye || lastTarget_ == target) w = Why::kOneEye;
        else if (slot_[eye] && slot_[eye] != target) w = Why::kMoved;
        else w = Why::kOk;
        slot_[eye] = target;
        lastEye_ = eye;
        lastTarget_ = target;
        v.why = w;
        v.trusted = (w == Why::kOk);
        win_.add(w, eye, target);
        if (w != Why::kFirst && w != Why::kMoved) {   // those two say nothing about the state
            const FilingState cand = w == Why::kOk ? FilingState::kPaired : (w == Why::kOneEye ? FilingState::kOneEye : FilingState::kDisagrees);
            if (cand == cand_) ++run_;
            else { cand_ = cand; run_ = 1; }
            if (run_ >= kTrustRun && cand_ != logged_) {
                logged_ = cand_;
                ++win_.events;
                v.event = true;
            }
        }
        return v;
    }

    FilingState state() const noexcept { return logged_; }
    const void* slot(int eye) const noexcept { return (eye == 0 || eye == 1) ? slot_[eye] : nullptr; }
    unsigned long long total() const noexcept { return total_; }
    const EyeWindow& window() const noexcept { return win_; }
    // The window's summary is worth a line (an anomaly, an event, or the session's first window); takes the window either way.
    bool takeWindow(EyeWindow* out) noexcept {
        const bool worth = win_.draws && (win_.anomalous() || win_.events || !firstDone_);
        if (win_.draws) firstDone_ = true;
        if (out) *out = win_;
        win_ = EyeWindow();
        return worth;
    }

private:
    const void* slot_[2] = {nullptr, nullptr};   // the target last filed under each eye
    int lastEye_ = -1;
    const void* lastTarget_ = nullptr;
    FilingState cand_ = FilingState::kUnknown, logged_ = FilingState::kUnknown;
    unsigned run_ = 0;
    unsigned long long total_ = 0;
    bool firstDone_ = false;
    EyeWindow win_;
};

// The eye of a target among the eyes' submitted targets: 0 or 1, or -1 when it is neither (or both are the same, or not known).
inline int submittedEyeOf(const void* target, const void* submittedLeft, const void* submittedRight) noexcept {
    if (!target) return -1;
    const bool l = (target == submittedLeft), r = (target == submittedRight);
    if (l == r) return -1;
    return l ? 0 : 1;
}

// ---- the lines (the health rule reads their words; the self-test renders them from these literals) ----
inline int formatEvent(char* out, size_t size, FilingState s, const void* filedLeft, const void* filedRight, int eye, const void* target, const void* submittedLeft,
                       const void* submittedRight, unsigned long long draws) noexcept {
    return std::snprintf(out, size,
        "visor eyes (F121c): %s -- the filing names target %016llX for the left eye and %016llX for the right; this draw is eye %d on target %016llX; the eyes' submitted "
        "targets: left %016llX, right %016llX; %llu visor draws so far.",
        filingText(s), ptrBits(filedLeft), ptrBits(filedRight), eye, ptrBits(target), ptrBits(submittedLeft), ptrBits(submittedRight), draws);
}
inline int formatTargets(char* out, size_t size, const EyeWindow& w) noexcept {
    size_t n = 0;
    if (size) out[0] = '\0';
    for (unsigned i = 0; i < w.tallies && n + 1 < size; ++i) {
        const int k = std::snprintf(out + n, size - n, "%s%016llX eye %d x %llu", i ? ", " : "", ptrBits(w.tally[i].target), w.tally[i].eye, w.tally[i].draws);
        if (k < 0) break;
        n += static_cast<size_t>(k);
        if (n >= size) { n = size - 1; break; }
    }
    if (!w.tallies && size) std::snprintf(out, size, "none");
    return static_cast<int>(n);
}
inline int formatWindow(char* out, size_t size, const EyeWindow& w) noexcept {
    char targets[200];
    formatTargets(targets, sizeof(targets), w);
    return std::snprintf(out, size,
        "visor eyes (F121c) 10 s: visor draws %llu: paired %llu, first %llu, one eye only %llu, target moved %llu, submitted-eye mismatch %llu; targets: %s",
        w.draws, w.why[static_cast<unsigned>(Why::kOk)], w.why[static_cast<unsigned>(Why::kFirst)], w.why[static_cast<unsigned>(Why::kOneEye)],
        w.why[static_cast<unsigned>(Why::kMoved)], w.why[static_cast<unsigned>(Why::kSubmitted)], targets);
}

}  // namespace visoraxis
}  // namespace edvr

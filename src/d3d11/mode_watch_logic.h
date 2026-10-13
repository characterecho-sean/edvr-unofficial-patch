// The mode watch's decisions: when a read of Settings.xml posts a 3D mode change, and when a live runtime posts Vr. Pure and free of any
// Windows API, so tools\mode_watch_test drives every rule here; mode_watch.cpp owns the files, the threads and the log. The decisions
// are what the commit switches on (docs\design-unified-install-2026-10-12.md section 3.2). The one-read-later rule follows the Bindings
// poll's two-check rule (device_hook.cpp).
#pragma once

#include <cstdint>

#include "../common/install_scope.h"

namespace edvr::modewatch {

// ignored: the read did not name a mode (absent, half-written, out of range), or the file is stale (staleFile on the Step); an unknown
// confirm also moves the pending change's deadline by a retry interval. same: nothing changed, or a pending mode is still waiting.
// pending: a change to mode was seen and waits for its confirming read. posted: the committed mode changed. dropped: the pending mode did
// not survive its confirming read, or its unknown confirms ran out. nothing: a live rise that changes nothing.
enum class Outcome { Nothing, Ignored, Same, Pending, Posted, Dropped };

// mode is the pending or posted class for Pending and Posted; for the other outcomes it is not meaningful. staleFile is set for an
// Ignored read that the stale-file rule refused (the log says "ignored (stale file)").
struct Step {
    Outcome outcome;
    GameMode mode;
    bool staleFile = false;
};

// Three classes are kept. fileMode_ is what Settings.xml is taken to hold: a read is compared with it, so a read posts only when the file
// changes. committed_ is the class EDVR runs. fileStale_ says fileMode_ was not set by the file: a /vr or /novr launch whose file class
// differs from the class it runs, or a live rise posting Vr over a flat file. A stale file is not evidence (the flight D case: a /vr
// launch over a file at 0 whose unchanged 0 must not post Flat, and a write at 3D off that must). Epic's "Launch in VR" is the live rise
// case: it goes live with Settings.xml still saying flat.
class Watch {
public:
    // fileMode is the launch file's class; effectiveMode is the class the launch runs, which a /vr or /novr override makes different from
    // the file (design section 3.1). EDVR runs effectiveMode from the start. The file is stale when the two differ, and then no read counts
    // until a write has been seen. An unknown launch file already reads as Vr (design Q2).
    Watch(GameMode fileMode, GameMode effectiveMode)
        : committed_(effectiveMode), fileMode_(effectiveMode), fileStale_(fileMode != effectiveMode) {}

    // afterWrite is true for a read that follows a write of Settings.xml (a folder notification or the stat), false for a hint, a live
    // rise or a timer read. While the file is stale, a known read without a write is ignored, and a known read after a write clears the
    // staleness and follows the normal rules. A confirm of a pending change (the timer) is unaffected.
    Step onRead(uint64_t nowMs, const ModeRead& r, bool afterWrite) {
        if (!r.known) {
            // Before the confirm is due an unknown read is ignored. At or after it the file cannot confirm the change yet (absent, cut
            // off), so the confirm is put off by a bounded retry. After kMaxUnknownRetries of those the change is dropped, and the watch
            // waits for the next write: no clock keeps reading a file that stays invalid.
            if (!pending_ || nowMs < due_) return Step{Outcome::Ignored, r.mode};
            if (unknownRetries_ >= kMaxUnknownRetries) {
                pending_ = false;
                unknownRetries_ = 0;
                return Step{Outcome::Dropped, r.mode};
            }
            ++unknownRetries_;
            due_ = nowMs + kUnknownRetryMs;
            return Step{Outcome::Ignored, r.mode};
        }
        if (!pending_) {
            if (fileStale_) {
                if (!afterWrite) return Step{Outcome::Ignored, r.mode, true};
                fileStale_ = false;
            }
            if (r.mode == fileMode_) return Step{Outcome::Same, committed_};
            pending_ = true;
            pendingMode_ = r.mode;
            due_ = nowMs + kConfirmMs;
            unknownRetries_ = 0;
            return Step{Outcome::Pending, pendingMode_};
        }
        if (nowMs < due_) {
            if (r.mode == pendingMode_) return Step{Outcome::Same, pendingMode_};
            pending_ = false;
            return Step{Outcome::Dropped, r.mode};
        }
        const bool confirmed = r.mode == pendingMode_;
        const GameMode mode = pendingMode_;
        pending_ = false;
        if (!confirmed) return Step{Outcome::Dropped, r.mode};
        fileMode_ = mode;
        // A live rise may already have posted this class; then the confirmed file changes nothing EDVR runs.
        if (mode == committed_) return Step{Outcome::Same, committed_};
        committed_ = mode;
        return Step{Outcome::Posted, committed_};
    }

    // Touches committed_ only, except that a Vr post over a flat fileMode_ makes the file stale: the file still says flat. A pending file
    // change stays pending, and its confirm is unaffected.
    Step onLiveRise(uint64_t /*nowMs*/) {
        if (committed_ != GameMode::Flat) return Step{Outcome::Nothing, committed_};
        committed_ = GameMode::Vr;
        if (fileMode_ == GameMode::Flat) {
            fileMode_ = GameMode::Vr;
            fileStale_ = true;
        }
        return Step{Outcome::Posted, committed_};
    }

    // The time the pending mode is confirmed, or 0 when nothing is pending.
    uint64_t confirmDueMs() const { return pending_ ? due_ : 0; }
    bool pending() const { return pending_; }
    GameMode committed() const { return committed_; }
    bool stale() const { return fileStale_; }
    // The unknown confirms the pending change has retried so far, for the log.
    unsigned unknownRetries() const { return pending_ ? unknownRetries_ : 0; }

    static constexpr uint64_t kConfirmMs = 250;
    static constexpr uint64_t kUnknownRetryMs = 100;
    static constexpr unsigned kMaxUnknownRetries = 20;

private:
    GameMode committed_;
    GameMode fileMode_;
    bool fileStale_;
    bool pending_ = false;
    GameMode pendingMode_ = GameMode::Vr;
    uint64_t due_ = 0;
    unsigned unknownRetries_ = 0;
};

}  // namespace edvr::modewatch

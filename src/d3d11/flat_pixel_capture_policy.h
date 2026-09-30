#pragma once
#include <cstdint>

namespace edvr {
// A frame is a LIVE sample when its history stood and it ran at a raster phase
// other than (0,0): the frames a running image is made of. The frames right after
// an F10 arm are not (2026-09-29: the resolver reset and the phase was 0, so the
// draw capture's two frames held constants no live frame has -- camera rows with
// no phase in them -- and the pixel capture's first sample was a reset frame).
// `phaseExpected` is false when the jitter is switched off on purpose
// (experimental.temporal_aa_jitter = off): phase 0 is then what a live frame is.
constexpr bool flatCaptureFrameLive(bool reset, bool phaseNonzero, bool phaseExpected = true) {
    return !reset && (phaseNonzero || !phaseExpected);
}

// One manually armed burst. Bytes count attempted GPU copies, not successful
// files, so failed readbacks cannot bypass the memory/disk-work budget.
struct FlatPixelCapturePolicy {
    static constexpr uint64_t maxBytes = 384ull * 1024 * 1024;
    static constexpr unsigned maxSamples = 4;
    // Frames between samples. The second follows the first as soon as the first's
    // readback is done, so the stability report can compare two live frames of the
    // same still scene; later samples keep the old spacing. A FAILED first sample
    // keeps the old spacing too (its retry is not a second sample).
    static constexpr unsigned secondSpacing = 1, laterSpacing = 15;
    bool active = false, pending = false;
    unsigned copied = 0, completed = 0, failed = 0;
    uint64_t bytes = 0, armedFrame = 0, armedMs = 0, copyFrame = 0, copyMs = 0;
    void arm(uint64_t frame, uint64_t ms) { *this={};active=true;armedFrame=frame;armedMs=ms; }
    bool expired(uint64_t frame,uint64_t ms) const {
        return frame<armedFrame || ms<armedMs || frame-armedFrame>=900 || ms-armedMs>=30000;
    }
    bool pendingExpired(uint64_t frame,uint64_t ms) const {
        return pending && (frame<copyFrame || ms<copyMs || frame-copyFrame>=120 || ms-copyMs>=5000);
    }
    unsigned spacingAfterLast() const { return completed == 1 && copied == 1 ? secondSpacing : laterSpacing; }
    // `live` false (the frame reset) is never a sample and changes nothing: the
    // next frame is asked again.
    bool due(uint64_t frame, bool live = true) const {
        return active && !pending && copied<maxSamples && live &&
            (!copied || (frame>=copyFrame && frame-copyFrame>=spacingAfterLast()));
    }
    bool fits(uint64_t count) const { return count && count<=maxBytes && bytes<=maxBytes-count; }
    bool reserve(uint64_t frame,uint64_t ms,uint64_t count,bool live = true) {
        if(!due(frame,live) || expired(frame,ms) || !fits(count))return false;
        ++copied;bytes+=count;copyFrame=frame;copyMs=ms;pending=true;return true;
    }
    void finish(bool success) { pending=false;if(success)++completed;else ++failed; }
};
} // namespace edvr

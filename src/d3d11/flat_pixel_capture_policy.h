#pragma once
#include <cstdint>

namespace edvr {
// One manually armed burst. Bytes count attempted GPU copies, not successful
// files, so failed readbacks cannot bypass the memory/disk-work budget.
struct FlatPixelCapturePolicy {
    static constexpr uint64_t maxBytes = 384ull * 1024 * 1024;
    static constexpr unsigned maxSamples = 4;
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
    bool due(uint64_t frame) const {
        return active && !pending && copied<maxSamples &&
            (!copied || (frame>=copyFrame && frame-copyFrame>=15));
    }
    bool fits(uint64_t count) const { return count && count<=maxBytes && bytes<=maxBytes-count; }
    bool reserve(uint64_t frame,uint64_t ms,uint64_t count) {
        if(!due(frame) || expired(frame,ms) || !fits(count))return false;
        ++copied;bytes+=count;copyFrame=frame;copyMs=ms;pending=true;return true;
    }
    void finish(bool success) { pending=false;if(success)++completed;else ++failed; }
};
} // namespace edvr

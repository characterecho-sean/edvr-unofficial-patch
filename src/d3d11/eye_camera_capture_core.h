#pragma once
#include <cstdint>

namespace edvr { namespace eyecamera {
// A manual run samples one complete ledger frame, independently of screen naming.
struct Window {
    uint32_t selected = 0;
    bool armed = false, sampled = false, closed = false;
    void arm(uint32_t frame) noexcept { selected=frame; armed=true; sampled=false; closed=false; }
    bool boundary(uint32_t frame) noexcept {
        if (!armed || closed) return false;
        if (frame > selected) { closed=true; return false; }
        if (frame == selected && !sampled) { sampled=true; return true; }
        return false;
    }
    void end() noexcept { armed=false; closed=true; }
};
constexpr unsigned kSources=16, kRefresh=640, kStagesWritten=192, kActivitiesWritten=1024, kPlacedWritten=64;
} }

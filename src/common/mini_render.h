#pragma once
// P3, the third view (reviews\vr-view-caches-2026-10-13.md, P3; docs\sky-flicker-2026-10-13.md): a record of the frames in which Elite renders a
// small square deferred view of its own -- a 256x256 G-buffer of four colour targets with a depth of the same size, about 400 draws, a full lighting
// chain -- and the question the 2026-10-13 review asked of it: is this the reflection probe whose forward cube-face camera the chooser sometimes takes?
//
// LOG ONLY. The hooks (vscreen.cpp's OM set and draws, through temporal_pass.cpp) note a pass when four or more colour targets are bound with a square
// depth view that is not an eye's, count the draws made while it is bound, and keep the first game-side return address of the stack that bound it.
// This holds those notes by frame, for the Pause dump to print and to join with the camera-row events (camera_row_events.h): for each event frame, was
// there a mini-render that frame, or the frame before?
//
// Pure: no Windows, no globals. The rig is tools\sky_flicker_test.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace edvr::minirender {

constexpr uint32_t kKeepFrames = 2048;
constexpr uint32_t kMaxSquare = 1024;       // a square depth view up to this wide, with 4 or more colour targets, is a mini view (an eye is about 2000)
constexpr uint32_t kMinColours = 4;

// Is this bind a mini view: four or more colour targets, a depth view that is square and no wider than kMaxSquare. The eye's size is excluded by the
// width cap alone; a depth view of zero size is no depth view.
inline bool isMiniBind(uint32_t colours, uint32_t depthW, uint32_t depthH) {
    return colours >= kMinColours && depthW != 0 && depthW == depthH && depthW <= kMaxSquare;
}

struct Frame {
    uint32_t frame = 0;
    bool valid = false;
    uint16_t w = 0, h = 0;
    uint8_t colours = 0;
    uint16_t passes = 0;         // binds of a mini view this frame
    uint32_t draws = 0;          // draws made while one was bound
    uint32_t rva[2] = {};        // the first two distinct game-side return addresses (RVAs) of the stack at a bind; 0 for none
    uint8_t rvaN = 0;
    uint16_t writes = 0;         // camera-block writes whose first draw went into a mini view
    bool haveRows = false;
    float rows[12] = {};         // the first such write's rows
};

class Ring {
public:
    // The frame's record, reset when the slot held another frame.
    Frame& at(uint32_t frame) {
        Frame& f = slots_[frame % kKeepFrames];
        if (!f.valid || f.frame != frame) {
            f = Frame{};
            f.frame = frame;
            f.valid = true;
        }
        return f;
    }
    // A mini bind: the first one of the frame fixes the size.
    void notePass(uint32_t frame, uint32_t w, uint32_t h, uint32_t colours, uint32_t rva) {
        Frame& f = at(frame);
        if (!f.passes) { f.w = static_cast<uint16_t>(w); f.h = static_cast<uint16_t>(h); f.colours = static_cast<uint8_t>(colours); }
        if (f.passes < 0xFFFFu) ++f.passes;
        if (rva) {
            bool seen = false;
            for (uint8_t i = 0; i < f.rvaN; ++i) seen = seen || f.rva[i] == rva;
            if (!seen && f.rvaN < 2) f.rva[f.rvaN++] = rva;
        }
        ++minis_;
    }
    void noteDraw(uint32_t frame) { ++at(frame).draws; }
    void noteWrite(uint32_t frame, const float rows[12]) {
        Frame& f = at(frame);
        if (f.writes < 0xFFFFu) ++f.writes;
        if (!f.haveRows) { std::memcpy(f.rows, rows, sizeof(f.rows)); f.haveRows = true; }
    }
    // The mini-render record of a frame, or null when that frame had none (or is no longer kept).
    const Frame* find(uint32_t frame) const {
        const Frame& f = slots_[frame % kKeepFrames];
        return (f.valid && f.frame == frame && f.passes) ? &f : nullptr;
    }
    // The kept frames that had a mini-render, in frame order.
    std::vector<uint32_t> frames() const {
        std::vector<uint32_t> out;
        for (const Frame& f : slots_) if (f.valid && f.passes) out.push_back(f.frame);
        std::sort(out.begin(), out.end());
        return out;
    }
    uint64_t totalPasses() const { return minis_; }
private:
    Frame slots_[kKeepFrames];
    uint64_t minis_ = 0;
};

// ---- the join with the camera-row events ----------------------------------------------------------------------------------------------------------------------

struct Join {
    uint32_t events = 0;
    uint32_t sameFrame = 0;       // events with a mini-render in the event's first frame
    uint32_t previousFrame = 0;   // ...in the frame before it
    uint32_t either = 0;
    uint32_t neither = 0;
    uint32_t outOfRange = 0;      // events whose frame the ring no longer (or not yet) keeps, counted in neither as well
};

// eventFrames: the first frame of each event. oldestKept..newestKept: the frames the ring has been written over, so a frame outside them reads
// "no record" and not "no mini-render".
inline Join joinEvents(const Ring& ring, const std::vector<uint32_t>& eventFrames, uint32_t oldestKept, uint32_t newestKept) {
    Join j;
    for (uint32_t f : eventFrames) {
        ++j.events;
        if (f < oldestKept + 1 || f > newestKept) { ++j.outOfRange; ++j.neither; continue; }
        const bool a = ring.find(f) != nullptr;
        const bool b = ring.find(f - 1) != nullptr;
        if (a) ++j.sameFrame;
        if (b) ++j.previousFrame;
        if (a || b) ++j.either; else ++j.neither;
    }
    return j;
}

// "P3 join: events=N mini_same_frame=a mini_previous_frame=b either=c neither=d no_record=e". Returns the length written, or 0 when too small.
inline int formatJoin(char* buf, size_t n, const Join& j) {
    const int len = std::snprintf(buf, n,
        "P3 join: events=%u mini_same_frame=%u mini_previous_frame=%u either=%u neither=%u no_record=%u",
        j.events, j.sameFrame, j.previousFrame, j.either, j.neither, j.outOfRange);
    return (len <= 0 || static_cast<size_t>(len) >= n) ? 0 : len;
}

// "P3 f36358 mini=256x256 colours=4 passes=8 draws=285 rva=0x1234,0x5678 writes=2 first_write_t=(-0.054,-0.094,0.045)". One line per mini-render frame.
inline int formatFrame(char* buf, size_t n, const Frame& f) {
    int len = std::snprintf(buf, n, "P3 f%u mini=%ux%u colours=%u passes=%u draws=%u rva=", f.frame, f.w, f.h, f.colours, f.passes, f.draws);
    if (len <= 0 || static_cast<size_t>(len) >= n) return 0;
    auto add = [&](const char* fmt, auto... args) {
        if (len <= 0 || static_cast<size_t>(len) >= n) return;
        const int w = std::snprintf(buf + len, n - static_cast<size_t>(len), fmt, args...);
        if (w < 0 || static_cast<size_t>(len) + static_cast<size_t>(w) >= n) len = 0; else len += w;
    };
    if (!f.rvaN) add("-");
    for (uint8_t i = 0; i < f.rvaN; ++i) add("%s0x%X", i ? "," : "", f.rva[i]);
    add(" writes=%u", f.writes);
    if (f.haveRows) add(" first_write_t=(%+.3f,%+.3f,%+.3f)", static_cast<double>(f.rows[3]), static_cast<double>(f.rows[7]), static_cast<double>(f.rows[11]));
    else add(" first_write_t=-");
    return len;
}

// "P3 mini-render frames: N in the kept M frames (X%), size 256x256, cadence median G frames; pass binds T since start". seen: frames the ring covers.
inline int formatSummary(char* buf, size_t n, const Ring& ring, uint32_t seen) {
    const std::vector<uint32_t> fr = ring.frames();
    std::vector<uint32_t> gaps;
    for (size_t i = 1; i < fr.size(); ++i) gaps.push_back(fr[i] - fr[i - 1]);
    std::sort(gaps.begin(), gaps.end());
    uint32_t med = gaps.empty() ? 0 : gaps[gaps.size() / 2];
    uint32_t w = 0, h = 0;
    if (!fr.empty()) { const Frame* f = ring.find(fr.front()); if (f) { w = f->w; h = f->h; } }
    const double pct = seen ? 100.0 * static_cast<double>(fr.size()) / static_cast<double>(seen) : 0.0;
    int len;
    if (fr.empty())
        len = std::snprintf(buf, n, "P3 mini-render frames: 0 in the kept %u frames (none seen%s); pass binds since start: %llu",
                            seen, ring.totalPasses() ? " in the kept frames" : "", static_cast<unsigned long long>(ring.totalPasses()));
    else
        len = std::snprintf(buf, n, "P3 mini-render frames: %zu in the kept %u frames (%.1f%%), first size %ux%u, cadence median %u frames; pass binds since start: %llu",
                            fr.size(), seen, pct, w, h, med, static_cast<unsigned long long>(ring.totalPasses()));
    return (len <= 0 || static_cast<size_t>(len) >= n) ? 0 : len;
}

}  // namespace edvr::minirender

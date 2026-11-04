// The VR map camera's publisher, pure (docs\galaxy-map-vr-2026-11-03.md, Fix 2): the renderer hook (flat_stationary_camera_capture.cpp) reads the
// camera source's stage 0 on the game's threads; this holds what it found for the frame thread, and the frame thread's "is a map open" hold. No
// device, no Config, no Log, no game memory: tools\ui_quality_test pins it.
//
// Stage 0 of the Galaxy Map's camera source is named "GalaxyMapView" and its pose is head-free: identical to the last digit in three eye dumps
// while the head swung 45 degrees. So a publish that differs from the last of its view by even one bit is the map moving, and a frame with
// no such publish is the map at rest. "SystemMapView" is the string that sits beside it in the game's read-only data; that it is the System
// Map's stage name is unproven, the first flight with the System Map open shows it.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

namespace edvr { namespace mapcam {

constexpr uint32_t kHoldFrames = 8;   // frames "open" outlasts the last publish (a frame may skip it): flat's kFlatUiMapCanvasHoldFrames
constexpr unsigned kNameBytes = 48;   // stationcam::kNameBytes
constexpr unsigned kMaxNames = 32;    // distinct stage 0 names the log says in a session

enum class View : uint8_t { kNone = 0, kGalaxy, kSystem };

inline View viewOfName(const char* name) {
    if (!name) return View::kNone;
    if (std::strcmp(name, "GalaxyMapView") == 0) return View::kGalaxy;
    if (std::strcmp(name, "SystemMapView") == 0) return View::kSystem;
    return View::kNone;
}

// Is a view open on this frame: its publish counter advanced this frame, or within the last kHoldFrames. One per view; the frame thread only.
struct Hold {
    bool seen = false;
    uint64_t lastCount = 0, lastFrame = 0;
    bool step(uint64_t frame, uint64_t count) {
        if (count != lastCount) { lastCount = count; lastFrame = frame; seen = true; }
        return seen && frame - lastFrame <= kHoldFrames;
    }
};

struct Pose {
    float rows[3][3];
    float position[3];
};

// Written by any game thread behind a try-lock (a busy publisher is skipped, the next call carries the same pose), read by the frame thread and the draw
// threads behind a seqlock. The counters are bumped after the pose is whole.
class Publisher {
public:
    // One stage 0 observation: its name (printable ASCII, NUL-terminated) and its 16-float pose (rows at floats 0..2, 4..6, 8..10, position at 12..14).
    // Records the name the first time it is seen (kMaxNames at most), and publishes the pose when the name is a map view. False when busy.
    bool observe(const char* name, const float pose[16]) noexcept {
        if (busy_.exchange(true, std::memory_order_acquire)) return false;
        noteName(name, pose);
        const View v = viewOfName(name);
        if (v != View::kNone) publish(v, pose);
        busy_.store(false, std::memory_order_release);
        return true;
    }
    uint64_t galaxyPublishes() const noexcept { return galaxy_.load(std::memory_order_acquire); }
    uint64_t systemPublishes() const noexcept { return system_.load(std::memory_order_acquire); }
    // Publishes that moved the map: a different view from the last map publish, or a different bit anywhere in the rows or position of the same view's last.
    uint64_t changes() const noexcept { return changes_.load(std::memory_order_acquire); }
    // The latest map publish. False before the first, or when the seqlock could not settle.
    bool read(View& view, Pose& out) const noexcept {
        for (int tries = 0; tries < 8; ++tries) {
            const uint32_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1u) continue;
            uint32_t w[kWords];
            for (unsigned i = 0; i < kWords; ++i) w[i] = words_[i].load(std::memory_order_relaxed);
            const uint8_t v = view_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) != s1) continue;
            if (v == 0) return false;
            view = static_cast<View>(v);
            std::memcpy(&out, w, sizeof(out));
            return true;
        }
        return false;
    }
    // The names seen so far, in order (immutable once counted). The frame thread logs from its own index to nameCount().
    unsigned nameCount() const noexcept { return names_.load(std::memory_order_acquire); }
    const char* nameAt(unsigned i) const noexcept { return i < kMaxNames ? nameText_[i] : ""; }
    const float* namePosition(unsigned i) const noexcept { return i < kMaxNames ? namePos_[i] : nullptr; }

private:
    static constexpr unsigned kWords = 12;
    static_assert(sizeof(Pose) == kWords * sizeof(float), "Pose is twelve floats");

    static void gather(const float pose[16], uint32_t out[kWords]) noexcept {
        static const unsigned from[kWords] = {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14};
        for (unsigned i = 0; i < kWords; ++i) std::memcpy(&out[i], &pose[from[i]], sizeof(uint32_t));
    }
    void noteName(const char* name, const float pose[16]) noexcept {
        const unsigned n = names_.load(std::memory_order_relaxed);
        if (n >= kMaxNames) return;
        for (unsigned i = 0; i < n; ++i)
            if (std::strcmp(nameText_[i], name) == 0) return;
        unsigned k = 0;
        for (; k < kNameBytes - 1 && name[k]; ++k) nameText_[n][k] = name[k];
        nameText_[n][k] = 0;
        namePos_[n][0] = pose[12]; namePos_[n][1] = pose[13]; namePos_[n][2] = pose[14];
        names_.store(n + 1, std::memory_order_release);
    }
    void publish(View v, const float pose[16]) noexcept {
        uint32_t w[kWords];
        gather(pose, w);
        const unsigned slot = v == View::kGalaxy ? 0u : 1u;
        const bool moved = lastView_ != v || !lastKnown_[slot] || std::memcmp(last_[slot], w, sizeof(w)) != 0;
        std::memcpy(last_[slot], w, sizeof(w));
        lastKnown_[slot] = true;
        lastView_ = v;
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (unsigned i = 0; i < kWords; ++i) words_[i].store(w[i], std::memory_order_relaxed);
        view_.store(static_cast<uint8_t>(v), std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);
        if (moved) changes_.fetch_add(1, std::memory_order_release);
        (v == View::kGalaxy ? galaxy_ : system_).fetch_add(1, std::memory_order_release);
    }

    std::atomic<bool> busy_{false};
    std::atomic<uint32_t> seq_{0};
    std::atomic<uint32_t> words_[kWords]{};
    std::atomic<uint8_t> view_{0};
    std::atomic<uint64_t> galaxy_{0}, system_{0}, changes_{0};
    // Under the try-lock only.
    uint32_t last_[2][kWords]{};
    bool lastKnown_[2]{};
    View lastView_ = View::kNone;
    // Written once under the try-lock, then counted by names_.
    std::atomic<unsigned> names_{0};
    char nameText_[kMaxNames][kNameBytes]{};
    float namePos_[kMaxNames][3]{};
};

} }  // namespace edvr::mapcam

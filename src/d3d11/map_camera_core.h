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
    // A closed hold that takes the publish counter as it stands now for the last one seen: a reset while the counter is not 0 (a map was opened earlier in the session) reads as
    // closed until the counter advances, not as a map opened by the counter differing from 0.
    static Hold closedAt(uint64_t count) {
        Hold h;
        h.lastCount = count;
        return h;
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

// The two eyes' offsets from stage 0's position (docs\galaxy-map-vr-2026-11-03.md, "The wobble"). The game keeps its eye position in the map frame in float32 at about 50000 units,
// where one step is 0.0039 and the eye moves in several-step jumps; the UI frame's two cameras hold each eye's offset near zero (camera+0x50, the refresh hook's
// stored_negative_origin), where float32 is exact to 1e-8. The renderer hook's refresh callback offers every camera's origin here; this keeps the two most recent distinct ones that are
// short enough to be an eye's offset. Written by any game thread behind a try-lock (a busy writer is skipped, the next call carries the same origin), read behind a seqlock.
class Eyes {
public:
    // An origin shorter than this is a UI-frame eye's offset; a scene camera's origin is tens of thousands of units.
    static constexpr double kMaxLength = 100.0;

    // One offered origin. A new origin equal (bit for bit) to either slot changes nothing; a different one replaces the older slot. False when busy.
    bool observe(const float o[3]) noexcept {
        const double l2 = double(o[0]) * o[0] + double(o[1]) * o[1] + double(o[2]) * o[2];
        if (!(l2 < kMaxLength * kMaxLength)) return true;   // too long, or not a number
        uint32_t w[3];
        std::memcpy(w, o, sizeof(w));
        if (busy_.exchange(true, std::memory_order_acquire)) return false;
        const bool same0 = known_ >= 1 && std::memcmp(slot_[0], w, sizeof(w)) == 0;
        const bool same1 = known_ >= 2 && std::memcmp(slot_[1], w, sizeof(w)) == 0;
        if (!same0 && !same1) {
            const unsigned at = known_ < 2 ? known_ : older_;
            std::memcpy(slot_[at], w, sizeof(w));
            if (known_ < 2) ++known_;
            older_ = 1u - at;
            const uint32_t s = seq_.load(std::memory_order_relaxed);
            seq_.store(s + 1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            for (unsigned i = 0; i < 3; ++i) words_[at][i].store(w[i], std::memory_order_relaxed);
            valid_.store(known_ == 2, std::memory_order_relaxed);
            gen_.store(gen_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            seq_.store(s + 2, std::memory_order_release);
        }
        busy_.store(false, std::memory_order_release);
        return true;
    }
    // Slot writes so far (each one counts): the generation the readers compare.
    uint64_t generation() const noexcept { return gen_.load(std::memory_order_acquire); }
    // Both slots known: the two offsets and the generation they belong to. False before the second distinct origin, or when the seqlock could not settle.
    bool read(float out[2][3], uint64_t* generation) const noexcept {
        for (int tries = 0; tries < 8; ++tries) {
            const uint32_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1u) continue;
            uint32_t w[2][3];
            for (unsigned k = 0; k < 2; ++k)
                for (unsigned i = 0; i < 3; ++i) w[k][i] = words_[k][i].load(std::memory_order_relaxed);
            const bool valid = valid_.load(std::memory_order_relaxed);
            const uint64_t g = gen_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) != s1) continue;
            if (!valid) return false;
            std::memcpy(out, w, sizeof(w));
            if (generation) *generation = g;
            return true;
        }
        return false;
    }

private:
    std::atomic<bool> busy_{false};
    std::atomic<uint32_t> seq_{0};
    std::atomic<uint32_t> words_[2][3]{};
    std::atomic<bool> valid_{false};
    std::atomic<uint64_t> gen_{0};
    // Under the try-lock only.
    uint32_t slot_[2][3]{};
    unsigned known_ = 0, older_ = 0;
};

} }  // namespace edvr::mapcam

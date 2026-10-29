// F171 (native on foot VR): the raise's carriage, measured. Header only, no game or D3D11 state, so native_frame_test runs the very functions the pin hook and the frame thread use.
// Journal: the arc doc, F171.
//
// The flight edvr_gfx_20261029_043951 (build d64b85ac), the unarmed look-up at 04:44:25 to 04:44:35 with the mouse, aim +52 to +85 deg: the raise EDVR carries (F94a's, F138's sink in it)
// ran to its clamp, -50.0 cm on 90 of 90 ticks, while the game's camera over the neck read +14 to +40 cm: camera over the neck plus the raise came to -26, -13, -18, -23, -10 cm, the camera
// moving against the raise almost one to one. In the flight before, at the same pose with the raise held at +7 cm, the camera read -17.4. Hypothesis, not proven: while pitched, the camera the
// pin hook measures (E, with the raise the pose carried taken out by its alignment serial) does not carry the raise the serial says, so F138's G moves against the raise and the loop runs to a
// clamp in the direction of its first value.
//
// The instrument: per pin call with nothing in the sights, the carried raise (what f94NeckForPin takes out of the pin's camera), the game's camera as the hook read it BEFORE any removal
// (the job camera, in the model's axes less the animated neck: up and forward), G as F138 forms it (the camera after the removal, less the neck, less the headset's own height), and the
// eye as drawn (the same camera) up from the neck as finally written. Over each second, per look mode and band of |aim pitch| (under 10 deg, 10 to 40, 40 and over), the least-squares
// slope through the origin of consecutive calls' changes against the change of the carried raise, over the pairs whose carried raise changed 2 mm or more: of the raw camera's up (k_up)
// and forward (k_fwd), of G (c) and of the drawn eye's up (k_eye), with the pair counts and the carried raise's range.
//   Predicted if the hook's camera carries the raise and G is clear of it: k_up about 1, c about 0.
//   Predicted if the camera does not carry it but it is removed anyway: k_up about 0, c about -1.
// The level band (under 10 deg) is the control: its raise steps come from the settles and the releases. The slopes are printed, never folded into a word here.
#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f171c {

constexpr double kMinStepM = 0.002;           // a pair counts when the carried raise changed this much (m) or more between the two calls
constexpr double kBandLoDeg = 10.0, kBandHiDeg = 40.0;   // the bands of |aim pitch|: under kBandLoDeg, kBandLoDeg to kBandHiDeg, kBandHiDeg and over
constexpr uint32_t kMaxP1Gap = 2;             // two calls are consecutive when the game-frame index moved by 1..this (a dropped call between is tolerated)
constexpr int kModes = 2, kBands = 3;         // look modes (0 mouse look, 1 look to aim), bands
constexpr uint32_t kFlagPairs = 20;           // the health rule: the 40 deg and over band with at least this many pairs...
constexpr double kFlagSlope = 0.3;            // ... and k_up under this says the raise is not in the hook's camera

// The band of an aim pitch: 0 under 10 deg, 1 from 10 to 40, 2 from 40; -1 for a pitch that is not a number.
inline int bandOf(double aimDeg) noexcept {
    if (!std::isfinite(aimDeg)) return -1;
    const double a = std::fabs(aimDeg);
    return a < kBandLoDeg ? 0 : a < kBandHiDeg ? 1 : 2;
}
inline const char* modeName(int mode) noexcept { return mode ? "look to aim" : "mouse look"; }
inline const char* bandName(int band) noexcept { return band == 0 ? "under 10" : band == 1 ? "10-40" : "40+"; }

// One pin call's figures (m): the carried raise, the raw camera up and forward from the animated neck, G, and the drawn eye up from the neck as written; the call's game-frame index, the
// LOCAL pose's generation (G contains the headset's own height, which another generation measures in another frame), the look mode and the band of the aim's pitch.
struct Sample {
    bool have = false;
    uint32_t p1 = 0, generation = 0;
    int mode = 0, band = -1;
    double raiseM = 0.0, camUpM = 0.0, camFwdM = 0.0, gM = 0.0, eyeUpM = 0.0;
    double f137UpM = 0.0, f137FwdM = 0.0;   // the virtual neck the pose carried (F137), up and forward: counted per sample, not part of a pair
    bool valid() const noexcept {
        return have && (mode == 0 || mode == 1) && band >= 0 && band < kBands && std::isfinite(raiseM) && std::isfinite(camUpM) && std::isfinite(camFwdM) && std::isfinite(gM) &&
               std::isfinite(eyeUpM);
    }
};

// One cell's accumulators: the pairs, the sum of the squares of the raise's changes and the sums of its products with the others' changes, and the raise's range over the pairs' calls.
struct Cell {
    uint32_t n = 0;
    double sxx = 0.0, sxyUp = 0.0, sxyFwd = 0.0, sxyG = 0.0, sxyEye = 0.0;
    double raiseMin = 1e9, raiseMax = -1e9;
    void add(const Sample& a, const Sample& b) noexcept {
        const double dx = b.raiseM - a.raiseM;
        ++n;
        sxx += dx * dx;
        sxyUp += dx * (b.camUpM - a.camUpM);
        sxyFwd += dx * (b.camFwdM - a.camFwdM);
        sxyG += dx * (b.gM - a.gM);
        sxyEye += dx * (b.eyeUpM - a.eyeUpM);
        raiseMin = std::fmin(raiseMin, std::fmin(a.raiseM, b.raiseM));
        raiseMax = std::fmax(raiseMax, std::fmax(a.raiseM, b.raiseM));
    }
    void merge(const Cell& o) noexcept {
        n += o.n;
        sxx += o.sxx; sxyUp += o.sxyUp; sxyFwd += o.sxyFwd; sxyG += o.sxyG; sxyEye += o.sxyEye;
        raiseMin = std::fmin(raiseMin, o.raiseMin);
        raiseMax = std::fmax(raiseMax, o.raiseMax);
    }
    // The least-squares slope through the origin of the others' change against the raise's: sum(dx dy) / sum(dx dx). 0 with no pair (the line does not print such a cell).
    static double slope(double sxy, double sxx) noexcept { return sxx > 0.0 ? sxy / sxx : 0.0; }
    double kUp() const noexcept { return slope(sxyUp, sxx); }
    double kFwd() const noexcept { return slope(sxyFwd, sxx); }
    double c() const noexcept { return slope(sxyG, sxx); }
    double kEye() const noexcept { return slope(sxyEye, sxx); }
};
struct Window {
    Cell cell[kModes][kBands];
    uint32_t samples = 0;                            // the second's samples (calls that passed the gate), pairs or not: a second with samples and no pair is not a second the code never ran
    double f137UpSum = 0.0, f137FwdSum = 0.0;        // the virtual neck the poses carried (F137), over the samples (m)
    void addSample(const Sample& s) noexcept {
        ++samples;
        f137UpSum += s.f137UpM;
        f137FwdSum += s.f137FwdM;
    }
    uint32_t pairs() const noexcept {
        uint32_t total = 0;
        for (const auto& row : cell)
            for (const Cell& c : row) total += c.n;
        return total;
    }
    bool any() const noexcept { return pairs() > 0; }
    void clear() noexcept { *this = Window{}; }
};

// The hook's pairing: the previous call's sample, and the pair a new call makes with it. A pair needs the same look mode, the same band, the same LOCAL generation, a game-frame index 1 to
// kMaxP1Gap later and a change of the carried raise of kMinStepM or more. A call that is not valid, or from a context the previous one is not, only starts over.
struct Pairing {
    Sample prev;
    void reset() noexcept { prev = Sample{}; }
    // True when the call made a pair (added to w).
    bool step(const Sample& s, Window& w) noexcept {
        const Sample was = prev;
        prev = s.valid() ? s : Sample{};
        if (!s.valid() || !was.valid()) return false;
        if (s.mode != was.mode || s.band != was.band || s.generation != was.generation) return false;
        const uint32_t gap = s.p1 - was.p1;
        if (gap < 1u || gap > kMaxP1Gap) return false;
        if (!(std::fabs(s.raiseM - was.raiseM) >= kMinStepM)) return false;
        w.cell[s.mode][s.band].add(was, s);
        return true;
    }
};

// The window the hook adds to (one writer) and the frame thread takes once a second, with F170's rule for the F138 window: taking a window asks the writer to clear it before its next
// write; a second in which the writer wrote nothing leaves the window as it was taken, and the next take would read it again, so a take that finds the clear still pending reads an empty
// window. A write that lands between a take's copy and its request is dropped with the next clear (a call or two of a diagnostic).
template <class W>
struct Mailbox {
    W win;
    std::atomic<uint32_t> seq{0};
    std::atomic<bool> clearPending{false};
    template <class Fn>
    void write(Fn&& fn) noexcept {
        seq.fetch_add(1, std::memory_order_acq_rel);
        if (clearPending.exchange(false, std::memory_order_relaxed)) win.clear();
        fn(win);
        seq.fetch_add(1, std::memory_order_release);
    }
    bool take(W* out) noexcept {
        for (int tries = 0; tries < 2; ++tries) {
            const uint32_t s1 = seq.load(std::memory_order_acquire);
            if (s1 & 1u) continue;
            std::memcpy(static_cast<void*>(out), static_cast<const void*>(&win), sizeof(W));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_relaxed) != s1) continue;
            if (clearPending.load(std::memory_order_acquire)) *out = W{};   // nothing written since the last take: that window was printed
            else clearPending.store(true, std::memory_order_relaxed);
            return true;
        }
        return false;
    }
};

// ---- the line -----------------------------------------------------------------------------------------------------------------------------------------------------------------

// The cells of a window that have pairs, one per look mode and band, in the order mouse look then look to aim, each band from the level one to the steepest:
// "mouse look 40+ deg: 37 pairs, raise -50.0..-12.3 cm, k_up +0.02, k_fwd -0.05, c -0.98, k_eye +0.01". Stops before cap; returns the length (0: no cell has a pair).
inline size_t cellsText(const Window& w, char* buf, size_t cap) noexcept {
    size_t n = 0;
    if (cap) buf[0] = '\0';
    for (int m = 0; m < kModes; ++m) {
        for (int b = 0; b < kBands; ++b) {
            const Cell& c = w.cell[m][b];
            if (!c.n) continue;
            const int k = std::snprintf(buf + n, cap - n, "%s%s %s deg: %u pairs, raise %+.1f..%+.1f cm, k_up %+.2f, k_fwd %+.2f, c %+.2f, k_eye %+.2f", n ? "; " : "", modeName(m), bandName(b), c.n,
                                        c.raiseMin * 100.0, c.raiseMax * 100.0, c.kUp(), c.kFwd(), c.c(), c.kEye());
            if (k < 0 || static_cast<size_t>(k) >= cap - n) {
                buf[n] = '\0';
                break;
            }
            n += static_cast<size_t>(k);
        }
    }
    return n;
}

// The window's line, in any second with a pair: the cells, the key to them and the second's samples with the virtual neck (F137) they carried. The log's lines end at 1200 bytes (1166 of
// them the message); the six cells and the key to them run to about 1000.
inline int formatSummary(char* buf, size_t n, uint32_t seconds, const Window& w) noexcept {
    char cells[640];
    cellsText(w, cells, sizeof(cells));
    const double sn = w.samples ? static_cast<double>(w.samples) : 1.0;
    return std::snprintf(buf, n,
                         "native on foot raise carriage (F171) %u s: consecutive pin calls whose carried raise changed 2 mm or more, per look mode and band of |aim pitch|: %s | slope of the change "
                         "against the carried raise's change (1: carries it, 0: does not): k_up the game's camera as read before any removal, up from the animated neck; k_fwd the same forward; "
                         "c G as F138 forms it; k_eye the eye as drawn, up from the neck as written; samples %u, F137 carried up %+.1f forward %+.1f cm (mean per sample)",
                         seconds, cells, w.samples, w.f137UpSum / sn * 100.0, w.f137FwdSum / sn * 100.0);
}

}  // namespace f171c
}  // namespace nofoot
}  // namespace edvr

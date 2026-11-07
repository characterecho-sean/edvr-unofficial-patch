// F216 (native on-foot VR): the view's running bob. Header only, no game or D3D11 state, so native_frame_test runs the very functions the P1 hook and alignTick use.
//
// What the flights established. Recordings on a hill, native on-foot VR, unarmed. E below is the game's job camera with EDVR's own terms removed, in model space; N0 is the animated neck.
//  - The view's bob, in the 2.5 to 6 Hz band, on level ground: jogging 0.2 mm rms (nothing), sprinting 7.3 mm rms at about 3.4 Hz; side to side about 6.5 mm rms at half that rate. It does
//    not correlate with the animated neck, so it has to be filtered from the camera.
//  - The physical head reaches the camera with a gain of about 1 and no lag while running (flight 20261106_042552: e[0] on head_local[3] +0.89, e[2] on head_local[11] -0.97). A filter on the camera alone would
//    damp the pilot's own head moves at 1 Hz and up. So the pose's LOCAL head position is taken out of the camera before the filter sees it (In::head), and the move it wants leaves the head in the view.
//  - The first filter (the camera's height less a moving mean over a stride, carried forward by half a stride) was exact only on a steady ramp. A change of slope is an acceleration, and its error
//    filled the term: on five runs of recording 20261106_090823 the up term was 22.0 mm rms, 20.4 of it under 1.5 Hz, 6.1 at 1.5 to 2.5 Hz and 5.1 at 2.5 to 6 Hz, and it sat at the 5 cm clamp on 28 percent of
//    the downhill sprint rows. The view then moved against the HUD (view motion 1.62 mm rms a frame, 0.28 without the filter).
//
// So the filter is a band-pass on each axis, centred on the stride: a state-variable filter (BandPass, Q 1) at 4.2 Hz on the height after a first-order high-pass at 1 Hz (the slope's acceleration
// is slow and the high-pass takes it out), at 2.0 Hz on the side sway, at 4.2 Hz along the facing. Replayed offline on that recording with the term applied two frames late: level sprint height
// 6.9 mm rms to 1.6 with a 6.5 mm term and no clamp (4.42 flown with the moving mean); jog level 1.8 to 0.8 with a 2.8 mm term; side sway 6.8 to 0.9 (2.63 flown); along 2.4 to 1.0. The band's largest
// term on level ground was 16 mm; a landing can ask for 16 cm, so each axis is held to kClampM (2.5 cm), which a term of that size would only turn into a move of the eye against the HUD for nothing.
//
// The filter runs in the P1 hook (one writer, once per game frame) on the camera after EDVR's terms are removed (camU) and the 3P AMC's rows (right rows 0, up rows 1, forward rows 2, the
// origin m3p[12..14]). wAcc is the camera's height along the body's up, accumulated from increments (a far origin or a slowly turning up cannot matter); ex and ez are the camera's offset from the AMC
// origin along the body's right and forward, less the head, taken from their first values after a restart. The view's move wanted, in body axes, is (-side, -up, -along), each held to kClampM, times a
// weight w that ramps in and out over kRampS and is 1 only at a run with the filter wanted. At w = 0 every term is an exact 0.0. The filters run the whole time, whatever w is.
//
// The move goes to the frame thread as a seated alignment term (bodyToSeated), is taken out of the camera again at the P1 hook by the alignment serial it travelled under (SeatedHistory,
// the unturned vector), so E and the filter's own input never carry it and nothing feeds back.
#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f216 {

constexpr double kUpHz = 4.2;           // the height's band-pass: the sprint's stride rate
constexpr double kUpHighPassHz = 1.0;   // the height's first-order high-pass in front of it (a change of slope is slow)
constexpr double kSideHz = 2.0;         // the side sway: half the stride rate
constexpr double kAlongHz = 4.2;
constexpr double kQ = 1.0;
constexpr double kClampM = 0.025;       // each axis of the move is held to this (the band's largest on level ground was 16 mm)
constexpr double kRampS = 0.25;         // w ramps in and out over this
constexpr double kWarmS = 0.3;          // the filters have run this long since a restart before the move may be made
constexpr double kMinMps = 2.0;         // the plane speed over the last kSpeedS under which the filter is not wanted (an in-place turn swings the root at about 1.5 m/s)
constexpr double kSpeedS = 0.5;
constexpr double kStepResetM = 1.0;     // a step of the camera or the origin over this restarts the filter
constexpr double kGapS = 0.25;          // a gap between calls over this restarts the filter
constexpr double kMaxDtS = 0.1;
constexpr int kCap = 256;               // samples held per series (kSpeedS at 400 Hz would still fit)

// ---- a time-windowed series -------------------------------------------------------------------------------------------------------------------------------------------------------------------------

// Samples (t, v) of the last keepS seconds and the one before them. The mean over a window is the integral of the piecewise-linear signal over [last - w, last] (w no more than keepS) divided
// by w, so the window is exact at any frame rate and a sine of period w is nulled.
struct Series {
    double t[kCap] = {}, v[kCap] = {};
    int head = 0, n = 0;
    void clear() noexcept {
        head = 0;
        n = 0;
    }
    double tAt(int i) const noexcept { return t[(head + i) % kCap]; }
    double vAt(int i) const noexcept { return v[(head + i) % kCap]; }
    double lastT() const noexcept { return n ? tAt(n - 1) : 0.0; }
    void add(double tS, double value, double keepS) noexcept {
        if (!std::isfinite(tS) || !std::isfinite(value)) return;
        if (n > 0 && tS <= lastT()) clear();
        if (n == kCap) {
            head = (head + 1) % kCap;
            --n;
        }
        const int at = (head + n) % kCap;
        t[at] = tS;
        v[at] = value;
        ++n;
        while (n >= 3 && tAt(1) <= tS - keepS) {
            head = (head + 1) % kCap;
            --n;
        }
    }
    // The window w is held: a sample at or before last - w exists.
    bool covers(double w) const noexcept { return n >= 2 && tAt(0) <= lastT() - w + 1e-9; }
    bool valueAt(double tq, double* out) const noexcept {
        if (n < 2 || tq < tAt(0) - 1e-9 || tq > lastT() + 1e-9) return false;
        for (int i = 0; i + 1 < n; ++i) {
            if (tAt(i + 1) < tq) continue;
            const double t0 = tAt(i), t1 = tAt(i + 1);
            *out = t1 > t0 ? vAt(i) + (vAt(i + 1) - vAt(i)) * (tq - t0) / (t1 - t0) : vAt(i);
            return true;
        }
        *out = vAt(n - 1);
        return true;
    }
    double mean(double w) const noexcept {
        if (!covers(w) || !(w > 0.0)) return std::nan("");
        const double a = lastT() - w;
        double integral = 0.0;
        for (int i = 0; i + 1 < n; ++i) {
            double t0 = tAt(i), t1 = tAt(i + 1), v0 = vAt(i);
            const double v1 = vAt(i + 1);
            if (t1 <= a) continue;
            if (t0 < a) {
                v0 = v0 + (v1 - v0) * (a - t0) / (t1 - t0);
                t0 = a;
            }
            integral += 0.5 * (v0 + v1) * (t1 - t0);
        }
        return integral / w;
    }
};

// ---- the band-pass and the high-pass --------------------------------------------------------------------------------------------------------------------------------------------------------------

// A state-variable filter (topology preserving, the trapezoidal form): stable at any step, so a frame time that varies is safe. The output is the band-pass at unity gain at f0. A step of 0 outputs
// the last value and changes nothing; a step over kMaxDtS is held to it.
struct BandPass {
    double f0 = 1.0, q = 1.0;
    double ic1 = 0.0, ic2 = 0.0, last = 0.0;
    void init(double hz, double qFactor) noexcept {
        f0 = hz;
        q = qFactor;
        clear();
    }
    void clear() noexcept { ic1 = ic2 = last = 0.0; }
    double step(double u, double dtS) noexcept {
        if (!(dtS > 0.0) || !std::isfinite(u)) return last;
        if (dtS > kMaxDtS) dtS = kMaxDtS;
        const double g = std::tan(3.14159265358979323846 * f0 * dtS), k = 1.0 / q;
        const double a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1, a3 = g * a2;
        const double v3 = u - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        last = k * v1;
        return last;
    }
};

// A first-order high-pass, y = a * (y + x - xLast) with a = 1 / (1 + 2 pi fc dt). The first sample after a clear only sets xLast.
struct HighPass {
    double fc = 1.0;
    double y = 0.0, xLast = 0.0;
    bool have = false;
    void init(double hz) noexcept {
        fc = hz;
        clear();
    }
    void clear() noexcept {
        y = xLast = 0.0;
        have = false;
    }
    double step(double x, double dtS) noexcept {
        if (!std::isfinite(x)) return y;
        if (!have) {
            xLast = x;
            have = true;
            return y;
        }
        if (!(dtS > 0.0)) return y;
        if (dtS > kMaxDtS) dtS = kMaxDtS;
        const double a = 1.0 / (1.0 + 2.0 * 3.14159265358979323846 * fc * dtS);
        y = a * (y + x - xLast);
        xLast = x;
        return y;
    }
};

// ---- the filter ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

struct In {
    double tS = 0.0;
    double camU[3] = {};       // the camera after EDVR's terms are removed
    float m3p[16] = {};        // the 3P AMC's matrix: rows 0, 1, 2 right, up, forward; origin 12..14
    bool remove = false;       // the filter is wanted (the key is off)
    bool ads = false;
    double head[3] = {};       // the pose's LOCAL head position (head_local[3], [7], [11]); in body axes its move is (right, up, forward) = (x, y, -z)
    bool headOk = false;       // the pose carried a valid LOCAL head; without it the last good value stands
};
struct Out {
    double move[3] = {};       // the view's move in body axes (right, up, forward), m; exact zeros at w = 0
    double w = 0.0;
    bool full = false;         // the filters have run kWarmS since the restart and the speed window is held
    bool running = false;      // full and the plane speed over kSpeedS is at least kMinMps
    bool clamped = false;      // an axis was held to kClampM
    bool restarted = false;    // this call restarted the filter (a gap, a step over kStepResetM, a matrix that is not a number)
    double speedMps = 0.0;
};

inline double dot3(const double a[3], const double b[3]) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

struct Filter {
    Series Cx, Cy, Cz;
    HighPass highPass;
    BandPass bandUp, bandSide, bandAlong;
    double lastT = 0.0, lastCam[3] = {}, lastOrigin[3] = {}, lastHead[3] = {}, goodHead[3] = {}, wAcc = 0.0, cum[3] = {}, w = 0.0;
    double ex0 = 0.0, ez0 = 0.0, warmS = 0.0;
    bool have = false;
    Filter() noexcept {
        highPass.init(kUpHighPassHz);
        bandUp.init(kUpHz, kQ);
        bandSide.init(kSideHz, kQ);
        bandAlong.init(kAlongHz, kQ);
    }
    void reset() noexcept {
        Cx.clear();
        Cy.clear();
        Cz.clear();
        highPass.clear();
        bandUp.clear();
        bandSide.clear();
        bandAlong.clear();
        wAcc = 0.0;
        cum[0] = cum[1] = cum[2] = 0.0;
        ex0 = ez0 = warmS = 0.0;
        w = 0.0;
        have = false;
    }
    Out step(const In& in) noexcept {
        Out o;
        double right[3], up[3], fwd[3], origin[3];
        bool ok = std::isfinite(in.tS) && std::isfinite(in.camU[0]) && std::isfinite(in.camU[1]) && std::isfinite(in.camU[2]);
        for (int i = 0; i < 3; ++i) {
            right[i] = static_cast<double>(in.m3p[i]);
            up[i] = static_cast<double>(in.m3p[4 + i]);
            fwd[i] = static_cast<double>(in.m3p[8 + i]);
            origin[i] = static_cast<double>(in.m3p[12 + i]);
            ok = ok && std::isfinite(right[i]) && std::isfinite(up[i]) && std::isfinite(fwd[i]) && std::isfinite(origin[i]);
        }
        if (!ok) {
            reset();
            o.restarted = true;
            return o;
        }
        if (in.headOk && std::isfinite(in.head[0]) && std::isfinite(in.head[1]) && std::isfinite(in.head[2]))
            for (int i = 0; i < 3; ++i) goodHead[i] = in.head[i];
        const double* hd = goodHead;
        double dt = 0.0;
        if (have) {
            dt = in.tS - lastT;
            const double dc[3] = {in.camU[0] - lastCam[0], in.camU[1] - lastCam[1], in.camU[2] - lastCam[2]};
            const double dorg[3] = {origin[0] - lastOrigin[0], origin[1] - lastOrigin[1], origin[2] - lastOrigin[2]};
            if (!(dt > 0.0) || dt > kGapS || std::sqrt(dot3(dc, dc)) > kStepResetM || std::sqrt(dot3(dorg, dorg)) > kStepResetM) {
                reset();
                o.restarted = true;
            }
        }
        const double rel[3] = {in.camU[0] - origin[0], in.camU[1] - origin[1], in.camU[2] - origin[2]};
        const double ex = dot3(rel, right) - hd[0], ez = dot3(rel, fwd) + hd[2];   // body forward is -z of the LOCAL head
        if (!have) {
            lastT = in.tS;
            for (int i = 0; i < 3; ++i) {
                lastCam[i] = in.camU[i];
                lastOrigin[i] = origin[i];
                lastHead[i] = hd[i];
            }
            ex0 = ex;
            ez0 = ez;
            have = true;
            dt = 0.0;
        } else {
            const double dc[3] = {in.camU[0] - lastCam[0], in.camU[1] - lastCam[1], in.camU[2] - lastCam[2]};
            const double dorg[3] = {origin[0] - lastOrigin[0], origin[1] - lastOrigin[1], origin[2] - lastOrigin[2]};
            wAcc += dot3(dc, up) - (hd[1] - lastHead[1]);   // the head's own rise is not bob
            const double upPart = dot3(dorg, up);   // the plane: the origin's increment less its part along the body's up
            for (int i = 0; i < 3; ++i) cum[i] += dorg[i] - upPart * up[i];
            lastT = in.tS;
            for (int i = 0; i < 3; ++i) {
                lastCam[i] = in.camU[i];
                lastOrigin[i] = origin[i];
                lastHead[i] = hd[i];
            }
        }
        if (dt > kMaxDtS) dt = kMaxDtS;
        double upBand = bandUp.last, sideBand = bandSide.last, alongBand = bandAlong.last;
        if (dt > 0.0) {
            warmS += dt;
            upBand = bandUp.step(highPass.step(wAcc, dt), dt);
            sideBand = bandSide.step(ex - ex0, dt);
            alongBand = bandAlong.step(ez - ez0, dt);
        } else {
            highPass.step(wAcc, dt);   // the first call after a restart sets the high-pass's reference
        }
        Cx.add(in.tS, cum[0], kSpeedS);
        Cy.add(in.tS, cum[1], kSpeedS);
        Cz.add(in.tS, cum[2], kSpeedS);
        double c0[3] = {};
        o.full = warmS >= kWarmS && Cx.covers(kSpeedS) && Cx.valueAt(in.tS - kSpeedS, &c0[0]) && Cy.valueAt(in.tS - kSpeedS, &c0[1]) && Cz.valueAt(in.tS - kSpeedS, &c0[2]);
        if (o.full) {
            const double d[3] = {cum[0] - c0[0], cum[1] - c0[1], cum[2] - c0[2]};
            o.speedMps = std::sqrt(dot3(d, d)) / kSpeedS;
        }
        o.running = o.full && o.speedMps >= kMinMps;
        const bool want = in.remove && o.running && !in.ads;
        if (!o.full) {   // the filters are not warm and the speed window is not held yet: nothing is wanted and nothing is made
            w = 0.0;
            return o;
        }
        w = want ? std::fmin(1.0, w + dt / kRampS) : std::fmax(0.0, w - dt / kRampS);
        o.w = w;
        if (!(w > 0.0)) return o;   // the move stays exact zeros
        const double raw[3] = {-sideBand, -upBand, -alongBand};   // right, up, forward
        for (int i = 0; i < 3; ++i) {
            double r = std::isfinite(raw[i]) ? raw[i] : 0.0;
            if (r > kClampM) {
                r = kClampM;
                o.clamped = true;
            } else if (r < -kClampM) {
                r = -kClampM;
                o.clamped = true;
            }
            o.move[i] = r * w;
        }
        return o;
    }
};

// The seated alignment term a view move needs. The camera moves in body axes (right, up, forward) by (-S0, -S1, +S2) for a seated shift S (nofoot::adsCamShiftBody; the view moves by minus the
// published alignment, F94's seatedY = -appliedM for an upward move), so S = (-right, -up, +forward). 0.0 - x keeps an exact +0.0 for a zero.
inline void bodyToSeated(const double move[3], double seated[3]) noexcept {
    seated[0] = 0.0 - move[0];
    seated[1] = 0.0 - move[1];
    seated[2] = 0.0 + move[2];
}

// The inverse: the eye's move in body axes (right, up, forward) a seated term makes, (-S0, -S1, +S2). An exact zero stays an exact +0.0.
inline void seatedToBody(const double seated[3], double body[3]) noexcept {
    body[0] = 0.0 - seated[0];
    body[1] = 0.0 - seated[1];
    body[2] = 0.0 + seated[2];
}

// F220: a placement matrix (row vectors; rows 0 to 2 the basis, row 3 the translation at 12 to 14) moved by the eye's move in body axes (right, up, forward) so the HUD goes with the view. Only the
// translation changes, and a component that is exactly 0 is not touched, so a zero shift leaves the matrix's bytes as they were.
inline void addHudShift(float* m, const float shift[3]) noexcept {
    for (int i = 0; i < 3; ++i)
        if (shift[i] != 0.0f) m[12 + i] += shift[i];
}

// What the P1 hook hands the frame thread (a seqlock in the cpp): the move in body axes and the weight, with the time it was made (the frame thread takes nothing older than kPubMaxAgeMs).
constexpr uint64_t kPubMaxAgeMs = 250;
struct Pub {
    double move[3] = {};
    double w = 0.0;
    uint64_t ms = 0;
    bool valid = false;
};

// ---- the term's part of every alignment, by serial -----------------------------------------------------------------------------------------------------------------------------------------

// The frame thread records the unturned seated vector of every alignment it publishes under the alignment's serial (record); the P1 hook finds the one the pose it reads carried (find).
// A slot is the serial and three float words; a reader that sees the serial change under it has a miss, never a mixed value. Serial 0 is the zero alignment before any publication.
struct SeatedHistory {
    static constexpr uint32_t kN = 256;
    struct Slot {
        std::atomic<uint32_t> serial{~0u};
        std::atomic<uint32_t> bits[3];
        Slot() noexcept {
            for (auto& b : bits) b.store(0u, std::memory_order_relaxed);
        }
    };
    Slot slot[kN];
    std::atomic<uint32_t> newestSeq{0};
    std::atomic<uint32_t> newestBits[3];
    SeatedHistory() noexcept {
        for (auto& b : newestBits) b.store(0u, std::memory_order_relaxed);
    }
    static uint32_t toBits(double x) noexcept {
        const float f = static_cast<float>(x);
        uint32_t b = 0;
        std::memcpy(&b, &f, sizeof(b));
        return b;
    }
    static double fromBits(uint32_t b) noexcept {
        float f = 0.0f;
        std::memcpy(&f, &b, sizeof(f));
        return static_cast<double>(f);
    }
    void record(uint32_t serial, const double s[3]) noexcept {
        Slot& sl = slot[serial % kN];
        sl.serial.store(~0u, std::memory_order_release);   // invalid while the words change
        for (int i = 0; i < 3; ++i) sl.bits[i].store(toBits(s[i]), std::memory_order_release);
        sl.serial.store(serial, std::memory_order_release);
        newestSeq.fetch_add(1, std::memory_order_acq_rel);
        for (int i = 0; i < 3; ++i) newestBits[i].store(toBits(s[i]), std::memory_order_relaxed);
        newestSeq.fetch_add(1, std::memory_order_release);
    }
    struct Found {
        bool reported = false;   // the runtime reported a serial (a word above 0)
        bool matched = false;    // ... and the history still holds it
        double s[3] = {};
    };
    // word: the frame input's applied-alignment word (0: not reported, else the serial plus 1).
    Found find(uint32_t word) const noexcept {
        Found f;
        if (word == 0) return f;
        f.reported = true;
        const uint32_t serial = word - 1u;
        if (serial == 0) {
            f.matched = true;
            return f;
        }
        const Slot& sl = slot[serial % kN];
        if (sl.serial.load(std::memory_order_acquire) != serial) return f;
        uint32_t b[3];
        for (int i = 0; i < 3; ++i) b[i] = sl.bits[i].load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (sl.serial.load(std::memory_order_relaxed) != serial) return f;
        double s[3];
        for (int i = 0; i < 3; ++i) s[i] = fromBits(b[i]);
        f.matched = std::isfinite(s[0]) && std::isfinite(s[1]) && std::isfinite(s[2]);
        if (f.matched)
            for (int i = 0; i < 3; ++i) f.s[i] = s[i];
        return f;
    }
    void newest(double s[3]) const noexcept {
        for (int tries = 0; tries < 3; ++tries) {
            const uint32_t s1 = newestSeq.load(std::memory_order_acquire);
            if (s1 & 1u) continue;
            uint32_t b[3];
            for (int i = 0; i < 3; ++i) b[i] = newestBits[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (newestSeq.load(std::memory_order_relaxed) != s1) continue;
            for (int i = 0; i < 3; ++i) s[i] = fromBits(b[i]);
            return;
        }
        s[0] = s[1] = s[2] = 0.0;
    }
};

// ---- the log window ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

struct Stats {
    uint32_t frames = 0, running = 0, removing = 0, atClamp = 0;
    double sumSqMm[3] = {}, maxMm[3] = {};   // the move's axes (right, up, forward) over the removing frames, mm
    bool keepsGameBob = false;               // the key says on: the view has the game's bob
    void add(const Out& o, bool keepsBob) noexcept {
        ++frames;
        keepsGameBob = keepsBob;
        if (o.running) ++running;
        if (!(o.w > 0.0)) return;
        ++removing;
        if (o.clamped) ++atClamp;
        for (int i = 0; i < 3; ++i) {
            const double mm = o.move[i] * 1000.0;
            sumSqMm[i] += mm * mm;
            if (std::fabs(mm) > maxMm[i]) maxMm[i] = std::fabs(mm);
        }
    }
    bool any() const noexcept { return frames > 0; }
    void clear() noexcept { *this = Stats{}; }
    double rmsMm(int axis) const noexcept { return removing ? std::sqrt(sumSqMm[axis] / removing) : 0.0; }
};

// The window's line. A reader tells the key on (the game's bob kept: idle), the key off with no running (nothing to remove), and the key off with running frames and none removing (the filter
// never took hold). tools\edvr_log.py reads it.
inline int formatLine(char* buf, size_t n, uint32_t seconds, const Stats& s) noexcept {
    return std::snprintf(buf, n,
                         "native on foot view bob (F216) %u s: fix.native_on_foot_view_bob is %s; game frames %u, running %u, removing %u; up rms %.1f max %.1f mm, right rms %.1f max %.1f mm, "
                         "along rms %.1f max %.1f mm; at the clamp %u",
                         seconds, s.keepsGameBob ? "on" : "off", s.frames, s.running, s.removing, s.rmsMm(1), s.maxMm[1], s.rmsMm(0), s.maxMm[0], s.rmsMm(2), s.maxMm[2], s.atClamp);
}

// ---- F220: the HUD goes with the view ------------------------------------------------------------------------------------------------------------------------------------------------------------

// The window of placement calls on foot (the game thread writes, the frame thread takes). A call counts whatever the key says; "shifted" is a call that moved the placement by a non-zero shift.
struct HudStats {
    uint32_t calls = 0, shifted = 0;
    double sumSqMm[2] = {}, maxMm[2] = {};   // the shift's up and side, over the shifted calls, mm
    bool keyOn = false;
    void add(bool key, const float shift[3]) noexcept {
        ++calls;
        keyOn = key;
        if (shift[0] == 0.0f && shift[1] == 0.0f && shift[2] == 0.0f) return;
        ++shifted;
        const double mm[2] = {static_cast<double>(shift[1]) * 1000.0, static_cast<double>(shift[0]) * 1000.0};
        for (int i = 0; i < 2; ++i) {
            sumSqMm[i] += mm[i] * mm[i];
            if (std::fabs(mm[i]) > maxMm[i]) maxMm[i] = std::fabs(mm[i]);
        }
    }
    bool any() const noexcept { return calls > 0; }
    void clear() noexcept { *this = HudStats{}; }
    double rmsMm(int axis) const noexcept { return shifted ? std::sqrt(sumSqMm[axis] / shifted) : 0.0; }
};

// tools\edvr_log.py reads it: the key on with the F216 line removing and no call shifted is the shift that never ran.
inline int formatHudLine(char* buf, size_t n, uint32_t seconds, const HudStats& s) noexcept {
    return std::snprintf(buf, n,
                         "native on foot hud with the view (F220) %u s: experimental.native_on_foot_hud_stays_with_view is %s; placement calls %u, with a shift %u; up rms %.1f max %.1f mm, "
                         "side rms %.1f max %.1f mm",
                         seconds, s.keyOn ? "on" : "off", s.calls, s.shifted, s.rmsMm(0), s.maxMm[0], s.rmsMm(1), s.maxMm[1]);
}

}  // namespace f216
}  // namespace nofoot
}  // namespace edvr

// F216 (native on-foot VR): the view's running bob. Header only, no game or D3D11 state, so native_frame_test runs the very functions the P1 hook and alignTick use.
//
// What the flights established. Recordings on a hill, native on-foot VR, unarmed. E below is the game's job camera with EDVR's own terms removed, in model space; N0 is the animated neck.
//  - The view's bob, in the 2.5 to 6 Hz band, on level ground: jogging 0.2 mm rms (nothing), sprinting 7.3 mm rms at about 3.4 Hz; side to side about 6.5 mm rms at half that rate. It does
//    not correlate with the animated neck, so it has to be filtered from the camera. A replay of the filter below, applied one frame late: level sprint 7.3 to 2.0 mm, hill sprint 8.6 to
//    4.4 mm.
//  - The physical head reaches the camera with a gain of about 1 and no lag while running (flight 20261106_042552: e[0] on head_local[3] +0.89, e[2] on head_local[11] -0.97). A filter on the camera alone would
//    damp the pilot's own head moves at 1 Hz and up. So the pose's LOCAL head position is taken out of the camera before the filter sees it (In::head), and the move it wants leaves the head in the view.
//
// The filter runs in the P1 hook (one writer, once per game frame) on the camera after EDVR's terms are removed (camU) and the 3P AMC's rows (right rows 0, up rows 1, forward rows 2, the
// origin m3p[12..14]). W is the camera's height along the body's up, accumulated from increments (a far origin or a slowly turning up cannot matter). M is the mean of W over one stride
// (kStrideS); V = M + (M - M(t - kStrideS)) / 2 is that mean carried forward by half a stride, so the stride is nulled in both parts and a steady slope has no lag. Mx and Mz are the means of
// the camera's offset from the AMC origin, along the body's right and forward, over two strides. The view's move wanted, in body axes, is (V - W, Mx - ex, Mz - ez), each held to kClampM,
// times a weight w that ramps in and out over kRampS and is 1 only at a run with the filter wanted. At w = 0 every term is an exact 0.0.
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

constexpr double kStrideS = 0.294;      // one stride at the sprint's 3.4 Hz
constexpr double kSwayS = 0.588;        // two strides: the side to side sway's period
constexpr double kClampM = 0.05;        // each axis of the move is held to this
constexpr double kRampS = 0.25;         // w ramps in and out over this
constexpr double kMinMps = 2.0;         // the plane speed over the last kSpeedS under which the filter is not wanted (an in-place turn swings the root at about 1.5 m/s)
constexpr double kSpeedS = 0.5;
constexpr double kStepResetM = 1.0;     // a step of the camera or the origin over this restarts the filter
constexpr double kGapS = 0.25;          // a gap between calls over this restarts the filter
constexpr double kMaxDtS = 0.1;
constexpr int kCap = 256;               // samples held per series (kSwayS at 400 Hz would still fit)

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
    bool full = false;         // every window holds its span
    bool running = false;      // full and the plane speed over kSpeedS is at least kMinMps
    bool clamped = false;      // an axis was held to kClampM
    bool restarted = false;    // this call restarted the filter (a gap, a step over kStepResetM, a matrix that is not a number)
    double speedMps = 0.0;
};

inline double dot3(const double a[3], const double b[3]) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

struct Filter {
    Series W, M, Ex, Ez, Cx, Cy, Cz;
    double lastT = 0.0, lastCam[3] = {}, lastOrigin[3] = {}, lastHead[3] = {}, goodHead[3] = {}, wAcc = 0.0, cum[3] = {}, w = 0.0;
    bool have = false;
    void reset() noexcept {
        W.clear();
        M.clear();
        Ex.clear();
        Ez.clear();
        Cx.clear();
        Cy.clear();
        Cz.clear();
        wAcc = 0.0;
        cum[0] = cum[1] = cum[2] = 0.0;
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
        if (!have) {
            lastT = in.tS;
            for (int i = 0; i < 3; ++i) {
                lastCam[i] = in.camU[i];
                lastOrigin[i] = origin[i];
                lastHead[i] = hd[i];
            }
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
        const double rel[3] = {in.camU[0] - origin[0], in.camU[1] - origin[1], in.camU[2] - origin[2]};
        const double ex = dot3(rel, right) - hd[0], ez = dot3(rel, fwd) + hd[2];   // body forward is -z of the LOCAL head
        W.add(in.tS, wAcc, kStrideS);
        const double mNow = W.mean(kStrideS);
        if (std::isfinite(mNow)) M.add(in.tS, mNow, kStrideS);
        Ex.add(in.tS, ex, kSwayS);
        Ez.add(in.tS, ez, kSwayS);
        Cx.add(in.tS, cum[0], kSpeedS);
        Cy.add(in.tS, cum[1], kSpeedS);
        Cz.add(in.tS, cum[2], kSpeedS);
        double mAgo = 0.0, c0[3] = {};
        o.full = std::isfinite(mNow) && M.covers(kStrideS) && Ex.covers(kSwayS) && Ez.covers(kSwayS) && Cx.covers(kSpeedS) && M.valueAt(in.tS - kStrideS, &mAgo) &&
                 Cx.valueAt(in.tS - kSpeedS, &c0[0]) && Cy.valueAt(in.tS - kSpeedS, &c0[1]) && Cz.valueAt(in.tS - kSpeedS, &c0[2]);
        if (o.full) {
            const double d[3] = {cum[0] - c0[0], cum[1] - c0[1], cum[2] - c0[2]};
            o.speedMps = std::sqrt(dot3(d, d)) / kSpeedS;
        }
        o.running = o.full && o.speedMps >= kMinMps;
        const bool want = in.remove && o.running && !in.ads;
        if (dt > kMaxDtS) dt = kMaxDtS;
        if (!o.full) {   // the windows do not hold their span yet: nothing is wanted and nothing is made
            w = 0.0;
            return o;
        }
        w = want ? std::fmin(1.0, w + dt / kRampS) : std::fmax(0.0, w - dt / kRampS);
        o.w = w;
        if (!(w > 0.0)) return o;   // the move stays exact zeros
        const double mx = Ex.mean(kSwayS), mz = Ez.mean(kSwayS);
        const double v = mNow + (mNow - mAgo) * 0.5;
        const double raw[3] = {mx - ex, v - wAcc, mz - ez};   // right, up, forward
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

}  // namespace f216
}  // namespace nofoot
}  // namespace edvr

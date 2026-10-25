// F119 (native on-foot VR, measure only): head containment. On foot the game's collision keeps the commander's head out of walls, and EDVR adds the player's own
// head translation on top of the game's camera: the eye is the camera plus the head's offset from its rest point, the seated origin the last recentre set. Leaning
// can therefore put the eye through a wall. A leash (a soft radius that fades toward black, a hard one that recentres) and a wall contact rule are the designed
// answer; this build only measures what sets their numbers, and changes nothing that is drawn. Journal: the arc doc, 2026-10-25, F119. Header only, no game or
// D3D11 state, so native_frame_test runs the very functions the render thread uses.
//
// 1. The head's offset from the rest point. The pose the runtime hands the d3d11 half is the head in the seated space with EDVR's own eye alignment applied (the
//    standing-height hold, the neck reference's raise, the ADS locks, the weapon eye and view offsets); the physical head is that pose plus the alignment it carried
//    (nofoot::alignHeadTrue, F38; the alignment named by the serial the runtime reported with the pose, F89). The seated space is kept on the body's heading
//    (F40's map, psi 0): right is +x, up +y, forward -z. The horizontal offset is what pushes an eye through a wall; the vertical one is the stance.
//    Reported by class: the stance (standing, crouched, or not known), the item (nothing, a weapon, a tool) and ADS.
// 2. How close world geometry gets to each eye: an 8 x 8 grid over the eye's scene depth (head_containment_shader.h), read back two frames later. A cell's depth is
//    the nearest of its 8 x 8 taps. Taps carrying the stencil bit 0x10 (the player's weapon and arms, and the game's characters, the commander's own body among
//    them) are kept apart: "world" is the taps without it, "own" is all of them. The coverage is the number of cells (of 64) whose depth is within 5, 10 and 20 cm
//    of the camera, and within 3 cm of the near plane (a wall the near plane cuts reads at the near plane, not at 0).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f119 {

// The humanoid's collision radius in metres; 0: not read. Build 334925: not an exe constant (the movement and motion-graph physics components take their shapes from
// packed game data by numeric type id; analysis\decomp\verify_20261025_humanoid_collision_radius.txt).
constexpr double kCollisionRadiusM = 0.0;
constexpr double kBaselineCm = 3.0;       // a head this far or less from the rest point is where the game's own camera is: the coverage there must read about 0
constexpr double kNearPlaneSlackM = 0.03; // the coverage against the near plane
constexpr uint32_t kGrid = 8, kCells = 64, kTaps = 8;
constexpr uint32_t kStencilMask = 0x10u;
constexpr uint32_t kMaxSamples = 512;     // a second's pose samples (a frame each)
constexpr int kLineLimit = 1090;          // the log's note() holds 1200 bytes with its stamp and a truncation mark

// ---- the classes ------------------------------------------------------------------------------------------------------------------------------------------
enum Stance : int { kStanding = 0, kCrouched = 1, kStanceUnknown = 2, kStances = 3 };
enum Item : int { kUnarmed = 0, kWeapon = 1, kTool = 2, kItems = 3 };
constexpr int kClasses = kStances * kItems * 2;
inline int classIndex(int stance, int item, bool ads) noexcept { return (stance * kItems + item) * 2 + (ads ? 1 : 0); }
inline const char* stanceName(int s) noexcept { return s == kStanding ? "standing" : s == kCrouched ? "crouched" : "unknown"; }
inline const char* itemName(int i) noexcept { return i == kUnarmed ? "unarmed" : i == kWeapon ? "weapon" : "tool"; }
inline void className(int c, char* out, size_t cap) noexcept {
    const int ads = c & 1, item = (c >> 1) % kItems, stance = (c >> 1) / kItems;
    std::snprintf(out, cap, "%s/%s/%s", stance >= kStances ? "?" : stanceName(stance), itemName(item), ads ? "ADS" : "hip");
}
// The stance the pin published (-1/2: not known; 0 standing; 1 crouched), unknown when older than staleMs.
inline int stanceOf(int published, uint64_t publishedMs, uint64_t nowMs, uint64_t staleMs) noexcept {
    if (publishedMs == 0 || nowMs < publishedMs || nowMs - publishedMs > staleMs) return kStanceUnknown;
    return published == 0 ? kStanding : published == 1 ? kCrouched : kStanceUnknown;
}
// The item: a weapon when Status.json names one (or F83's native primary), a tool when something else is drawn, nothing otherwise.
inline int itemOf(bool weapon, bool armed) noexcept { return weapon ? kWeapon : armed ? kTool : kUnarmed; }

// ---- the physical head --------------------------------------------------------------------------------------------------------------------------------
struct Offset {
    double right = 0.0, up = 0.0, forward = 0.0;   // metres in the seated space: +x, +y, -z
    double horizontal() const noexcept { return std::sqrt(right * right + forward * forward); }
};
// render: the pose's 3x4 (translation at 3, 7, 11); applied: the eye alignment that pose carried (seated metres).
inline Offset physicalOffset(const float render[12], const double applied[3]) noexcept {
    Offset o;
    o.right = static_cast<double>(render[3]) + applied[0];
    o.up = static_cast<double>(render[7]) + applied[1];
    o.forward = -(static_cast<double>(render[11]) + applied[2]);
    return o;
}

// The eye alignment published under each serial, all three axes (F89's history keeps the y alone), so the pose's applied alignment is known whole.
constexpr uint32_t kHistN = 256;
struct AlignHistory3 {
    struct Slot {
        uint32_t serial = ~0u;
        double v[3] = {};
    };
    Slot s[kHistN];
    uint32_t newest = 0;
    void record(uint32_t serial, const double v[3]) noexcept {
        Slot& d = s[serial % kHistN];
        d.serial = serial;
        for (int i = 0; i < 3; ++i) d.v[i] = v[i];
        newest = serial;
    }
    // word: EdvrNativeNowPose::eyeAlignApplied (0: the runtime reports none; else the serial + 1). reported/matched as nofoot::f89::Applied.
    struct Found {
        bool reported = false, matched = false;
        double v[3] = {};
    };
    Found find(uint32_t word) const noexcept {
        Found f;
        if (word == 0) return f;
        f.reported = true;
        const uint32_t serial = word - 1u;
        if (serial == 0) {
            f.matched = true;
            return f;
        }
        const Slot& d = s[serial % kHistN];
        if (d.serial != serial) return f;
        f.matched = true;
        for (int i = 0; i < 3; ++i) f.v[i] = d.v[i];
        return f;
    }
};

// ---- a second of offsets ------------------------------------------------------------------------------------------------------------------------------
constexpr int kBins = 17;
constexpr double kEdgesCm[kBins - 1] = {1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 25, 30, 40, 60, 100};   // upper edges of the horizontal distance's bins; the last is open
inline int binOf(double cm) noexcept {
    for (int i = 0; i < kBins - 1; ++i)
        if (cm < kEdgesCm[i]) return i;
    return kBins - 1;
}
// The value at nearest rank q of n values (the ceil(q n)-th smallest); the values are rearranged.
inline double nearestRank(float* v, uint32_t n, double q) noexcept {
    if (!n) return 0.0;
    uint32_t rank = static_cast<uint32_t>(std::ceil(q * static_cast<double>(n)));
    rank = std::min(std::max(rank, 1u), n);
    std::nth_element(v, v + (rank - 1), v + n);
    return static_cast<double>(v[rank - 1]);
}
struct ClassOut {
    int cls = 0;
    uint32_t n = 0;
    double p50 = 0.0, p95 = 0.0, max = 0.0;                  // the horizontal offset, cm
    double rMin = 0.0, rMax = 0.0, fMin = 0.0, fMax = 0.0, uMin = 0.0, uMax = 0.0;   // right, forward, up, cm
    uint32_t hist[kBins] = {};
};
struct OffsetSecond {
    struct Sample {
        float h, r, f, u;
        uint8_t cls;
    };
    Sample s[kMaxSamples];
    uint32_t n = 0, dropped = 0;
    uint32_t frames = 0, noPose = 0, alignMissed = 0;   // boundary ticks on foot; ticks with no usable pose; ticks whose reported alignment the history no longer held
    uint32_t classN[kClasses] = {};
    void add(int cls, const Offset& o) noexcept {
        if (cls < 0 || cls >= kClasses) return;
        if (n >= kMaxSamples) {
            ++dropped;
            return;
        }
        Sample& d = s[n++];
        d.h = static_cast<float>(o.horizontal() * 100.0);
        d.r = static_cast<float>(o.right * 100.0);
        d.f = static_cast<float>(o.forward * 100.0);
        d.u = static_cast<float>(o.up * 100.0);
        d.cls = static_cast<uint8_t>(cls);
        ++classN[cls];
    }
    // The class's second. False when it has no samples.
    bool summarize(int cls, ClassOut* out) const noexcept {
        if (cls < 0 || cls >= kClasses || !classN[cls]) return false;
        float h[kMaxSamples];
        uint32_t m = 0;
        ClassOut c;
        c.cls = cls;
        bool first = true;
        for (uint32_t i = 0; i < n; ++i) {
            const Sample& x = s[i];
            if (x.cls != cls) continue;
            h[m++] = x.h;
            ++c.hist[binOf(static_cast<double>(x.h))];
            if (first) {
                c.rMin = c.rMax = x.r;
                c.fMin = c.fMax = x.f;
                c.uMin = c.uMax = x.u;
                first = false;
            } else {
                c.rMin = std::min(c.rMin, static_cast<double>(x.r));
                c.rMax = std::max(c.rMax, static_cast<double>(x.r));
                c.fMin = std::min(c.fMin, static_cast<double>(x.f));
                c.fMax = std::max(c.fMax, static_cast<double>(x.f));
                c.uMin = std::min(c.uMin, static_cast<double>(x.u));
                c.uMax = std::max(c.uMax, static_cast<double>(x.u));
            }
            c.max = std::max(c.max, static_cast<double>(x.h));
        }
        c.n = m;
        c.p95 = nearestRank(h, m, 0.95);
        c.p50 = nearestRank(h, m, 0.50);
        *out = c;
        return true;
    }
    // The classes with samples, most samples first (at most k).
    int top(int k, int out[]) const noexcept {
        int idx[kClasses], m = 0;
        for (int c = 0; c < kClasses; ++c)
            if (classN[c]) idx[m++] = c;
        std::sort(idx, idx + m, [this](int a, int b) { return classN[a] != classN[b] ? classN[a] > classN[b] : a < b; });
        const int t = std::min(k, m);
        for (int i = 0; i < t; ++i) out[i] = idx[i];
        return m;
    }
    double maxHorizontalCm() const noexcept {
        double m = 0.0;
        for (uint32_t i = 0; i < n; ++i) m = std::max(m, static_cast<double>(s[i].h));
        return m;
    }
    void reset() noexcept {
        n = dropped = frames = noPose = alignMissed = 0;
        std::memset(classN, 0, sizeof(classN));
    }
};

// EDVR's own eye offsets, kept apart from the head's: the eye alignment as published (the total) and the terms it is made of.
struct OwnSecond {
    uint32_t n = 0;
    double hMax = 0.0, upMin = 0.0, upMax = 0.0;
    double lockH = 0.0, weaponH = 0.0, viewH = 0.0;   // largest horizontal magnitude of the ADS lock and sight, the weapon eye offset, the view offset (metres)
    double neckUp = 0.0, holdUp = 0.0;                // the neck reference's raise and the standing-height hold, the value of largest magnitude (metres)
    static double hor(const double v[3]) noexcept { return std::sqrt(v[0] * v[0] + v[2] * v[2]); }
    void add(const double total[3], const double lock[3], const double weapon[3], const double view[3], double neck, double hold) noexcept {
        const double h = hor(total);
        if (!n) {
            upMin = upMax = total[1];
        } else {
            upMin = std::min(upMin, total[1]);
            upMax = std::max(upMax, total[1]);
        }
        ++n;
        hMax = std::max(hMax, h);
        lockH = std::max(lockH, hor(lock));
        weaponH = std::max(weaponH, hor(weapon));
        viewH = std::max(viewH, hor(view));
        if (std::fabs(neck) > std::fabs(neckUp)) neckUp = neck;
        if (std::fabs(hold) > std::fabs(holdUp)) holdUp = hold;
    }
    void reset() noexcept { *this = OwnSecond{}; }
};

// ---- the depth grid -----------------------------------------------------------------------------------------------------------------------------------
// One tap of the shader's procedure on the CPU (the rigs compare the shader with it): the pixel a cell's tap reads, and the view depth the buffer value stands for.
inline void tapPixel(uint32_t cx, uint32_t cy, uint32_t tx, uint32_t ty, uint32_t W, uint32_t H, uint32_t* px, uint32_t* py) noexcept {
    const uint32_t span = kGrid * kTaps;
    const uint32_t sx = cx * kTaps + tx, sy = cy * kTaps + ty;
    *px = std::min(((2u * sx + 1u) * W) / (2u * span), W - 1u);
    *py = std::min(((2u * sy + 1u) * H) / (2u * span), H - 1u);
}
inline float viewDepth(float depth, float A, float B) noexcept {
    const float dd = depth - A;
    const float w = std::fabs(dd) > 1e-30f ? B / dd : 1e30f;
    return w > 0.0f ? w : 1e30f;
}
struct Cell {
    float world = 1e30f, all = 1e30f;
    uint32_t marked = 0;
};
// depth: W x H floats; stencil: W x H bytes, or null (no stencil view: no tap is marked).
inline Cell cellCpu(const float* depth, const uint8_t* stencil, uint32_t W, uint32_t H, double A, double B, uint32_t cx, uint32_t cy) noexcept {
    Cell c;
    for (uint32_t ty = 0; ty < kTaps; ++ty)
        for (uint32_t tx = 0; tx < kTaps; ++tx) {
            uint32_t px, py;
            tapPixel(cx, cy, tx, ty, W, H, &px, &py);
            const float w = viewDepth(depth[py * W + px], static_cast<float>(A), static_cast<float>(B));
            const bool fp = stencil && (stencil[py * W + px] & kStencilMask) != 0;
            c.all = std::min(c.all, w);
            if (fp) ++c.marked;
            else c.world = std::min(c.world, w);
        }
    return c;
}

// One eye's output words (head_containment_shader.h): the header (sequence, width, height, mask), then three words a cell.
constexpr uint32_t kEyeWords = 4u + 3u * kCells;
struct EyeGrid {
    bool ok = false;
    uint32_t seq = 0, width = 0, height = 0, mask = 0;
    float world[kCells] = {}, all[kCells] = {};
    uint8_t marked[kCells] = {};
};
inline float bitsToFloat(uint32_t b) noexcept {
    float f;
    std::memcpy(&f, &b, sizeof(f));
    return f;
}
// Decodes one eye's words. False (the words refused) when the sequence is not the dispatch's or the size is not the texture's the slot recorded.
inline bool decodeEye(const uint32_t* words, uint32_t seq, uint32_t width, uint32_t height, EyeGrid* out) noexcept {
    EyeGrid g;
    g.seq = words[0];
    g.width = words[1];
    g.height = words[2];
    g.mask = words[3];
    if (g.seq != seq || g.width != width || g.height != height) return false;
    for (uint32_t k = 0; k < kCells; ++k) {
        g.world[k] = bitsToFloat(words[4 + 3 * k]);
        g.all[k] = bitsToFloat(words[5 + 3 * k]);
        g.marked[k] = static_cast<uint8_t>(std::min(words[6 + 3 * k], 255u));
    }
    g.ok = true;
    *out = g;
    return true;
}
// A grid's coverage. thresholds: within 5, 10, 20 cm of the camera (the view depth), and within kNearPlaneSlackM of the near plane (nearM).
constexpr int kThresholds = 4;
struct Coverage {
    double nearestWorld = 1e30, nearestAll = 1e30;   // metres
    uint32_t within[kThresholds] = {};               // the world's cells
    uint32_t all20 = 0;                              // all taps' cells within 20 cm
    uint32_t markedCells = 0;                        // cells with any tap carrying the mask
};
inline Coverage coverage(const EyeGrid& g, double nearM) noexcept {
    Coverage c;
    const double t[kThresholds] = {0.05, 0.10, 0.20, nearM + kNearPlaneSlackM};
    for (uint32_t k = 0; k < kCells; ++k) {
        const double w = static_cast<double>(g.world[k]), a = static_cast<double>(g.all[k]);
        c.nearestWorld = std::min(c.nearestWorld, w);
        c.nearestAll = std::min(c.nearestAll, a);
        for (int i = 0; i < kThresholds; ++i)
            if (w < t[i]) ++c.within[i];
        if (a < 0.20) ++c.all20;
        if (g.marked[k]) ++c.markedCells;
    }
    return c;
}

// One eye's second.
struct EyeSecond {
    uint32_t read = 0, within[kThresholds] = {}, all20 = 0, markedCells = 0;   // the largest of the second
    double nearestWorld = 1e30, nearestAll = 1e30, nearM = 0.0;
    void add(const Coverage& c, double nearPlaneM) noexcept {
        ++read;
        nearM = nearPlaneM;
        nearestWorld = std::min(nearestWorld, c.nearestWorld);
        nearestAll = std::min(nearestAll, c.nearestAll);
        for (int i = 0; i < kThresholds; ++i) within[i] = std::max(within[i], c.within[i]);
        all20 = std::max(all20, c.all20);
        markedCells = std::max(markedCells, c.markedCells);
    }
};
enum Skip : int { kSkipRows = 0, kSkipDepth, kSkipGpu, kSkipSrv, kSkipRing, kSkipThread, kSkipMap, kSkipLate, kSkipDropped, kSkipSeq, kSkipZw, kSkips };
inline const char* skipName(int s) noexcept {
    static const char* const k[kSkips] = {"no scene rows", "no scene depth", "no GPU", "no view", "ring full", "other thread", "map failed", "late", "dropped", "wrong sequence", "not a depth fit"};
    return s >= 0 && s < kSkips ? k[s] : "?";
}
struct DepthSecond {
    EyeSecond eye[2];
    uint32_t skipped[kSkips] = {};
    // frames read at a head offset under kBaselineCm: the game's own camera, where the coverage must be about 0
    uint32_t baseFrames = 0, baseCells20 = 0, baseOwn20 = 0;
    // the smallest horizontal offset (cm) of a frame that had at least 1 / 8 world cells within kNearPlaneSlackM of the near plane (1e9: none)
    double contactMin1 = 1e9, contactMin8 = 1e9;
    void addFrame(const Coverage c[2], const bool have[2], bool offsetKnown, double offsetCm) noexcept {
        uint32_t w20 = 0, o20 = 0, np = 0;
        bool any = false;
        for (int e = 0; e < 2; ++e) {
            if (!have[e]) continue;
            any = true;
            w20 = std::max(w20, c[e].within[2]);
            o20 = std::max(o20, c[e].all20);
            np = std::max(np, c[e].within[3]);
        }
        if (!any || !offsetKnown) return;
        if (offsetCm < kBaselineCm) {
            ++baseFrames;
            baseCells20 = std::max(baseCells20, w20);
            baseOwn20 = std::max(baseOwn20, o20);
        }
        if (np >= 1) contactMin1 = std::min(contactMin1, offsetCm);
        if (np >= 8) contactMin8 = std::min(contactMin8, offsetCm);
    }
    void reset() noexcept { *this = DepthSecond{}; }
};

// ---- the lines ----------------------------------------------------------------------------------------------------------------------------------------
inline int formatHist(char* out, size_t cap, const uint32_t h[kBins]) noexcept {
    size_t len = 0;
    out[0] = '\0';
    for (int i = 0; i < kBins; ++i) {
        const int n = std::snprintf(out + len, cap - len, "%s%u", i ? "," : "", static_cast<unsigned>(h[i]));
        if (n < 0 || static_cast<size_t>(n) >= cap - len) return static_cast<int>(len);
        len += static_cast<size_t>(n);
    }
    return static_cast<int>(len);
}
// "[standing/unarmed/hip n 88 h p50 ...]"; withHist false leaves the histogram out (a long line).
inline int formatClass(char* out, size_t cap, const ClassOut& c, bool withHist) noexcept {
    char name[48], hist[160];
    className(c.cls, name, sizeof(name));
    formatHist(hist, sizeof(hist), c.hist);
    if (withHist)
        return std::snprintf(out, cap, "[%s n %u h p50 %.1f p95 %.1f max %.1f, right %+.1f..%+.1f, forward %+.1f..%+.1f, up %+.1f..%+.1f, hist %s]", name,
                             static_cast<unsigned>(c.n), c.p50, c.p95, c.max, c.rMin, c.rMax, c.fMin, c.fMax, c.uMin, c.uMax, hist);
    return std::snprintf(out, cap, "[%s n %u h p50 %.1f p95 %.1f max %.1f, right %+.1f..%+.1f, forward %+.1f..%+.1f, up %+.1f..%+.1f, hist -]", name,
                         static_cast<unsigned>(c.n), c.p50, c.p95, c.max, c.rMin, c.rMax, c.fMin, c.fMax, c.uMin, c.uMax);
}
inline void formatDepthValue(char* out, size_t cap, double metres) noexcept {
    if (metres < 1e29) std::snprintf(out, cap, "%.1f", metres * 100.0);
    else std::snprintf(out, cap, "none");
}
// One eye: the near plane, the nearest depth (world, and with the player's body: the weapon, arms and the game's characters), the cells within 5/10/20 cm and 3 cm of the
// near plane, the player's body among those within 20 cm, and the frames read.
inline int formatEye(char* out, size_t cap, const char* name, const EyeSecond& e) noexcept {
    char nw[24], na[24];
    formatDepthValue(nw, sizeof(nw), e.nearestWorld);
    formatDepthValue(na, sizeof(na), e.nearestAll);
    return std::snprintf(out, cap, "%s near %.1f, nearest %s (body %s), cells %u/%u/%u/%u (body 20 cm: %u), read %u", name, e.nearM * 100.0, nw, na,
                         static_cast<unsigned>(e.within[0]), static_cast<unsigned>(e.within[1]), static_cast<unsigned>(e.within[2]), static_cast<unsigned>(e.within[3]),
                         static_cast<unsigned>(e.all20), static_cast<unsigned>(e.read));
}

// What the 1 s line holds.
struct LineIn {
    uint32_t frames = 0, noPose = 0, dropped = 0, generation = 0, recentres = 0;
    double restAgeS = 0.0;
    const OffsetSecond* off = nullptr;
    const OwnSecond* own = nullptr;
    const DepthSecond* depth = nullptr;
    double costUs = 0.0;
};
// The line, written whole into out (cap bytes); the histograms of the second class and then of the first are dropped, then the second class itself, until it
// fits kLineLimit. Returns the length.
inline int formatLine(char* out, size_t cap, const LineIn& in) noexcept {
    int cls[2] = {0, 0};
    const int classes = in.off ? in.off->top(2, cls) : 0;
    ClassOut c[2];
    int have = 0;
    for (int i = 0; i < 2 && i < classes; ++i)
        if (in.off->summarize(cls[i], &c[have])) ++have;
    const OwnSecond own = in.own ? *in.own : OwnSecond{};
    const DepthSecond dep = in.depth ? *in.depth : DepthSecond{};
    char eyes[2][400], depthBlock[900], skipped[200];
    size_t sk = 0;
    skipped[0] = '\0';
    for (int s = 0; s < kSkips; ++s) {
        if (!dep.skipped[s]) continue;
        const int n = std::snprintf(skipped + sk, sizeof(skipped) - sk, "%s%s %u", sk ? ", " : "", skipName(s), static_cast<unsigned>(dep.skipped[s]));
        if (n > 0 && static_cast<size_t>(n) < sizeof(skipped) - sk) sk += static_cast<size_t>(n);
    }
    formatEye(eyes[0], sizeof(eyes[0]), "L", dep.eye[0]);
    formatEye(eyes[1], sizeof(eyes[1]), "R", dep.eye[1]);
    if (dep.eye[0].read && dep.eye[1].read) std::snprintf(depthBlock, sizeof(depthBlock), "%s; %s", eyes[0], eyes[1]);
    else if (dep.eye[0].read) std::snprintf(depthBlock, sizeof(depthBlock), "%s", eyes[0]);
    else if (dep.eye[1].read) std::snprintf(depthBlock, sizeof(depthBlock), "%s", eyes[1]);
    else std::snprintf(depthBlock, sizeof(depthBlock), "none read");
    char contact1[24], contact8[24];
    if (dep.contactMin1 < 1e8) std::snprintf(contact1, sizeof(contact1), "%.1f", dep.contactMin1);
    else std::snprintf(contact1, sizeof(contact1), "none");
    if (dep.contactMin8 < 1e8) std::snprintf(contact8, sizeof(contact8), "%.1f", dep.contactMin8);
    else std::snprintf(contact8, sizeof(contact8), "none");
    char offs[900];
    int n = 0;
    for (int variant = 0; variant < 4; ++variant) {
        // variant 0: both classes with histograms; 1: the second without; 2: neither with; 3: the first class alone, without
        offs[0] = '\0';
        size_t len = 0;
        const int shown = variant == 3 ? std::min(have, 1) : have;
        for (int i = 0; i < shown; ++i) {
            char one[420];
            formatClass(one, sizeof(one), c[i], variant == 0 || (variant == 1 && i == 0));
            const int m = std::snprintf(offs + len, sizeof(offs) - len, "%s%s", i ? " " : "", one);
            if (m > 0 && static_cast<size_t>(m) < sizeof(offs) - len) len += static_cast<size_t>(m);
        }
        if (!shown) std::snprintf(offs, sizeof(offs), "none (no pose this second)");
        else if (classes > shown) {
            uint32_t rest = 0;
            for (int k = 0; k < kClasses; ++k) rest += in.off->classN[k];
            for (int i = 0; i < shown; ++i) rest -= c[i].n;
            std::snprintf(offs + len, sizeof(offs) - len, " +%d classes (n %u)", classes - shown, static_cast<unsigned>(rest));
        }
        n = std::snprintf(out, cap,
                          "native on foot head containment (F119) 1 s: frames %u (no pose %u, over limit %u); rest point: generation %u, %.1f s old, recentres %u; "
                          "head offset cm: %s; EDVR eye offsets cm (not in the head offset): h max %.1f, up %+.1f..%+.1f [ADS lock+sight %.1f, weapon eye %.1f, view %.1f "
                          "horizontal; neck %+.1f, hold %+.1f up]; depth cm (cells of 64 within 5/10/20 cm of the camera and 3 cm of the near plane, the second's max): %s; "
                          "skipped %s; contact at the near plane, smallest head offset: 1 cell %s, 8 cells %s; at head offset under 3 cm: frames %u, cells within 20 cm "
                          "max %u (body %u); cost %.0f us a frame",
                          static_cast<unsigned>(in.frames), static_cast<unsigned>(in.noPose), static_cast<unsigned>(in.dropped), static_cast<unsigned>(in.generation),
                          in.restAgeS, static_cast<unsigned>(in.recentres), offs, own.hMax * 100.0, own.upMin * 100.0, own.upMax * 100.0, own.lockH * 100.0, own.weaponH * 100.0,
                          own.viewH * 100.0, own.neckUp * 100.0, own.holdUp * 100.0, depthBlock, sk ? skipped : "none", contact1, contact8,
                          static_cast<unsigned>(dep.baseFrames), static_cast<unsigned>(dep.baseCells20), static_cast<unsigned>(dep.baseOwn20), in.costUs);
        if (n >= 0 && n <= kLineLimit) break;
    }
    return n;
}

// The once-per-session line.
inline int formatSession(char* out, size_t cap, double radiusM = kCollisionRadiusM) noexcept {
    char radius[32];
    if (radiusM > 0.0) std::snprintf(radius, sizeof(radius), "%.3f m", radiusM);
    else std::snprintf(radius, sizeof(radius), "not read");
    return std::snprintf(out, cap,
                         "native on foot head containment (F119): measure only, nothing drawn changes; the commander's collision radius: %s; the head offset is the physical head's "
                         "from the rest point (the seated origin) with EDVR's own eye offsets taken out, in the seated space the runtime keeps on the body's heading (right +x, "
                         "up +y, forward -z); the depth grid is %u x %u cells of %u x %u taps over each eye's scene depth, and stencil bit 0x%02X marks the body (the player's "
                         "weapon and arms, the game's characters) apart from the world",
                         radius, static_cast<unsigned>(kGrid), static_cast<unsigned>(kGrid), static_cast<unsigned>(kTaps), static_cast<unsigned>(kTaps),
                         static_cast<unsigned>(kStencilMask));
}
// A move of the rest point on foot.
inline int formatRecentre(char* out, size_t cap, uint32_t from, uint32_t to, uint32_t onFoot, bool have, const Offset& was, double sinceS, int stance, int item, bool ads) noexcept {
    if (!have)
        return std::snprintf(out, cap,
                             "native on foot head containment (F119): the rest point moved (reference generation %u to %u; %u on foot this session): no head offset was "
                             "sampled before it, %.1f s after the last move; %s, %s, %s",
                             static_cast<unsigned>(from), static_cast<unsigned>(to), static_cast<unsigned>(onFoot), sinceS, stanceName(stance), itemName(item), ads ? "ADS" : "hip");
    return std::snprintf(out, cap,
                         "native on foot head containment (F119): the rest point moved (reference generation %u to %u; %u on foot this session): the head was %.1f cm "
                         "horizontal and %+.1f cm up from the old rest point (right %+.1f, forward %+.1f), %.1f s after the last move; %s, %s, %s",
                         static_cast<unsigned>(from), static_cast<unsigned>(to), static_cast<unsigned>(onFoot), was.horizontal() * 100.0, was.up * 100.0, was.right * 100.0,
                         was.forward * 100.0, sinceS, stanceName(stance), itemName(item), ads ? "ADS" : "hip");
}

}  // namespace f119
}  // namespace nofoot
}  // namespace edvr

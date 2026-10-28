// F144 (native on-foot VR): the pure parts of three changes. Header only, no game or D3D11 state, so native_frame_test runs the very functions the frame thread, the avatar hooks
// and the weapon UI's placement use. Journal: the arc doc, 2026-10-27, "the F143 flight" and F144.
//
// 1. A settled sight line per (item, scope). F143 took the ADS eye's shift from a live filter of the game's line E -> S, and the game's raise animation still moves E toward the
//    sight for about a second after the press: the eye followed the line through the moving E and overshot 1 cm left for 0.4 s (the F89 trace, kinetic rifle, 18:44:49). Now a
//    line is kept per (item, scope) for the session, fed only by samples that are settled (the game's ADS fraction 1.0, this press's fraction first at 1.0 at least 0.6 s earlier,
//    E and S each moved under 2 mm from the previous usable sample of the press) and averaged (an exponential, tau 1 s); the sight lock uses it from the press's first frame. Until
//    one exists the table stands in, and when one first becomes available mid-press the shift blends from the table's to the line's over 0.25 s. The entry path is measured from
//    the F89 trace's own eye less F_root samples: the eye's overshoot beyond its settled value in the first 1.5 s.
//
// 2. The coat. Under fix.native_on_foot_body_full_outfit, EVASuit_Gear_Back is promoted (and unclipped) only for an outer garment: the third-person avatar has no EVASuit_Arms
//    instance and has an EVASuit_Gear_Arms instance (the coat); the other suit's Gear_Back is its backpack and stays the game's shadows-only part. The first-person avatar's
//    census (the parts with an instance, their masks before EDVR's zeroing, which EDVR zeroed) is written on a change.
//
// 3. The weapon readout placed per role. The readout parts have roles (F91: the hip readout, the rifle's ADS readout, the pistol's ADS readout; the tool's is never touched).
//    Before F144 the pistol class placed its hip and ADS readouts by one set of keys (the ADS part took the hip's target), the rifle class placed only its ADS readout, and the
//    rifle's hip readout was "matched only": the four live trims reached the parts that were placed but never the rifle's hip readout. Now every weapon has a placement per role
//    (forward, left, up, toward the eye, cm), keyed by (item, scope, role) and seeded from today's values (the F140 table is the ADS and pistol roles' seed; the rifle hip role's seed
//    is 0, which draws it exactly where it is drawn now). The four trims are added to the role drawn now (the ADS role while the ADS weight is at least 0.5, else the hip role) and to
//    no other, so a trim never carries to another weapon, scope or role. The proof measures what reaches the draw.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "native_on_foot_f139.h"
#include "native_on_foot_f140.h"
#include "native_on_foot_f142.h"
#include "native_on_foot_f143.h"

namespace edvr {
namespace nofoot {
namespace f144 {

// ---- 1. the settled sight line --------------------------------------------------------------------------------------------------------------------------------------
constexpr double kFullHoldS = 0.6;     // the press's ADS fraction first reached 1.0 at least this long ago
constexpr double kStillM = 0.002;      // E and S each moved under this from the previous usable sample of the press
constexpr double kTauS = 1.0;          // the settled average's time constant
constexpr double kBlendS = 0.25;       // table -> settled line, when a line first becomes available mid-press

namespace detail {
inline double dist3(const double a[3], const double b[3]) noexcept {
    const double d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}
}  // namespace detail

// One press's gate. settled() is asked of every usable sample paired with the FP snapshot of its own render frame, in order; reset() at the press's start.
struct SettleGate {
    bool fullSeen = false;
    uint64_t fullMs = 0;
    bool havePrev = false;
    double pe[3] = {}, ps[3] = {};
    void reset() noexcept { *this = SettleGate{}; }
    bool settled(bool wOk, float w, uint64_t nowMs, const double e[3], const double s[3]) noexcept {
        const bool full = wOk && std::isfinite(w) && w >= 1.0f;
        if (full && !fullSeen) {
            fullSeen = true;
            fullMs = nowMs;
        }
        const bool still = havePrev && detail::dist3(e, pe) < kStillM && detail::dist3(s, ps) < kStillM;
        std::memcpy(pe, e, sizeof(pe));
        std::memcpy(ps, s, sizeof(ps));
        havePrev = true;
        return full && fullSeen && nowMs - fullMs >= static_cast<uint64_t>(kFullHoldS * 1000.0) && still;
    }
};
// The settled average: a tick's settled samples are one step (their mean), applied as an exponential with time constant kTauS.
struct Line {
    bool have = false;
    uint64_t ms = 0;
    double e[3] = {}, s[3] = {};
    uint32_t samples = 0;
    double pe[3] = {}, ps[3] = {};
    uint32_t pn = 0;
    void note(const double eRoot[3], const double sRoot[3]) noexcept {
        for (int i = 0; i < 3; ++i) {
            pe[i] += eRoot[i];
            ps[i] += sRoot[i];
        }
        ++pn;
    }
    bool flush(uint64_t nowMs, double tauS = kTauS) noexcept {
        if (!pn) return false;
        double me[3], ms3[3];
        for (int i = 0; i < 3; ++i) {
            me[i] = pe[i] / pn;
            ms3[i] = ps[i] / pn;
        }
        if (!have) {
            for (int i = 0; i < 3; ++i) {
                e[i] = me[i];
                s[i] = ms3[i];
            }
            have = true;
        } else {
            const double dt = nowMs > ms ? static_cast<double>(nowMs - ms) / 1000.0 : 0.0;
            const double a = tauS > 0.0 ? 1.0 - std::exp(-dt / tauS) : 1.0;
            for (int i = 0; i < 3; ++i) {
                e[i] += a * (me[i] - e[i]);
                s[i] += a * (ms3[i] - s[i]);
            }
        }
        ms = nowMs;
        samples += pn;
        pn = 0;
        for (int i = 0; i < 3; ++i) pe[i] = ps[i] = 0.0;
        return true;
    }
};
// The session's lines, by (item name, scope state): the exact Status.json name, so a weapon drawn again later finds its line.
struct Entry {
    bool used = false;
    char name[64] = {};
    int scope = 0;
    uint64_t lastUse = 0;
    Line line;
    uint64_t gameFrames = 0;   // frames the sight lock took its shift from this line (this session)
};
struct Cache {
    static constexpr int kN = 16;
    Entry e[kN];
    Entry* find(const char* name, int scope) noexcept {
        if (!name) return nullptr;
        for (Entry& x : e)
            if (x.used && x.scope == scope && std::strcmp(x.name, name) == 0) return &x;
        return nullptr;
    }
    // The line for (name, scope), made when there is none (the least recently used entry is replaced). nullptr for no name.
    Entry* get(const char* name, int scope, uint64_t nowMs) noexcept {
        if (!name || !name[0]) return nullptr;
        if (Entry* f = find(name, scope)) {
            f->lastUse = nowMs;
            return f;
        }
        Entry* victim = &e[0];
        for (Entry& x : e) {
            if (!x.used) {
                victim = &x;
                break;
            }
            if (x.lastUse < victim->lastUse) victim = &x;
        }
        *victim = Entry{};
        victim->used = true;
        std::snprintf(victim->name, sizeof(victim->name), "%s", name);
        victim->scope = scope;
        victim->lastUse = nowMs;
        return victim;
    }
};
// The choice for one frame: the settled line of the held (item, scope), else the table (the reason counted). settled may be null.
inline f143::Choice choose(bool keyOn, bool itemOk, bool weapon, const Entry* settled, const double p14w[3], const double up[3], const double right[3], const double fwd[3],
                           const double origin[3], double k, double trimUpCm, double trimRightCm) noexcept {
    f143::Choice c;
    if (!keyOn) { c.why = f143::kKeyOff; return c; }
    if (!itemOk) { c.why = f143::kNoItem; return c; }
    if (!weapon) { c.why = f143::kOtherItem; return c; }
    if (!settled || !settled->line.have) { c.why = f143::kNoLine; return c; }
    return f143::lineShift(settled->line.e, settled->line.s, p14w, up, right, fwd, origin, k, trimUpCm, trimRightCm);
}
// The blend from the table's shift to the settled line's. step() returns the settled line's weight for this frame (1: the line alone, 0: the table alone). A press whose first
// frame already has a line uses it at once; a line that becomes usable after a frame of the table (or after a frame it was not usable) blends in over kBlendS.
struct Blend {
    bool inPress = false, prevTable = false, blending = false;
    uint64_t startMs = 0;
    void reset() noexcept { *this = Blend{}; }
    double step(bool usable, uint64_t nowMs) noexcept {
        double wt = 0.0;
        if (usable) {
            wt = 1.0;
            if (!blending && inPress && prevTable) {
                blending = true;
                startMs = nowMs;
            }
            if (blending) {
                wt = nowMs > startMs ? static_cast<double>(nowMs - startMs) / (kBlendS * 1000.0) : 0.0;
                if (wt >= 1.0) {
                    wt = 1.0;
                    blending = false;
                }
            }
        } else {
            blending = false;
        }
        prevTable = wt < 1.0;
        inPress = true;
        return wt;
    }
};
inline void blendShift(double wSettled, const double table[3], const double line[3], double out[3]) noexcept {
    for (int i = 0; i < 3; ++i) out[i] = (1.0 - wSettled) * table[i] + wSettled * line[i];
}

// ---- the entry path: the eye's excursion beyond its settled value after a press ---------------------------------------------------------------------------------------------
// The F89 trace's own samples (the eye less F_root, right and up, cm, at t seconds since the press) while ADS is engaged. Settled value: the mean of the last second of the
// engaged samples, taken only when the press was held past kPathMinHoldS. The excursion: beyond the settled value, in the direction the eye was travelling (from its first
// sample), in the first kPathWindowS: its maximum, the time of it, and the signed deviation there. An eye that never passes its settled value has 0.
constexpr double kPathWindowS = 1.5, kPathSettleS = 1.0, kPathMinHoldS = 2.5, kPathTravelMinCm = 0.05;
struct PathSample {
    float t, r, u;
};
struct PathAxis {
    double settled = 0.0, first = 0.0;
    double over = 0.0, overT = 0.0, dev = 0.0;   // the largest excursion beyond the settled value, when, and the signed deviation then
};
struct PathResult {
    bool ok = false;
    const char* why = "";
    int n = 0;
    double endT = 0.0;
    PathAxis lateral, vertical;
};
struct EntryPath {
    static constexpr int kMax = 640;
    PathSample s[kMax];
    int n = 0;
    uint32_t dropped = 0;
    void reset() noexcept {
        n = 0;
        dropped = 0;
    }
    void add(double t, double rCm, double uCm) noexcept {
        if (n >= kMax) {
            ++dropped;
            return;
        }
        s[n++] = PathSample{static_cast<float>(t), static_cast<float>(rCm), static_cast<float>(uCm)};
    }
    static void axis(const PathSample* a, int n, bool lateral, double endT, PathAxis* out) noexcept {
        double sum = 0.0;
        int m = 0;
        for (int i = 0; i < n; ++i)
            if (a[i].t >= endT - kPathSettleS) {
                sum += lateral ? a[i].r : a[i].u;
                ++m;
            }
        out->settled = m ? sum / m : 0.0;
        out->first = lateral ? a[0].r : a[0].u;
        const double travel = out->settled - out->first;
        const double sign = travel > kPathTravelMinCm ? 1.0 : travel < -kPathTravelMinCm ? -1.0 : 0.0;
        for (int i = 0; i < n; ++i) {
            if (a[i].t > kPathWindowS) break;
            const double dev = (lateral ? a[i].r : a[i].u) - out->settled;
            const double over = sign != 0.0 ? sign * dev : std::fabs(dev);
            if (over > out->over) {
                out->over = over;
                out->overT = a[i].t;
                out->dev = dev;
            }
        }
    }
    PathResult result() const noexcept {
        PathResult r;
        r.n = n;
        if (n < 20) { r.why = "too few eye samples"; return r; }
        r.endT = s[n - 1].t;
        if (r.endT < kPathMinHoldS) { r.why = "the press ended before 2.5 s: no settled value"; return r; }
        axis(s, n, true, r.endT, &r.lateral);
        axis(s, n, false, r.endT, &r.vertical);
        r.ok = true;
        return r;
    }
};

// ---- 2. the coat: Gear_Back only for an outer garment ---------------------------------------------------------------------------------------------------------------------
enum GearBack : int { kGearUnknown = 0, kGearOuter = 1, kGearSuitArms = 2, kGearNoArms = 3 };
inline int gearBackDecision(bool suitArms, bool gearArms) noexcept {
    if (suitArms) return kGearSuitArms;
    return gearArms ? kGearOuter : kGearNoArms;
}
inline bool gearBackPromoted(int d) noexcept { return d == kGearOuter; }
inline const char* gearBackText(int d) noexcept {
    switch (d) {
        case kGearOuter: return "Gear_Back promoted: outer garment";
        case kGearSuitArms: return "Gear_Back kept shadows only: suit arms present";
        case kGearNoArms: return "Gear_Back kept shadows only: no outer-garment arms (EVASuit_Gear_Arms absent)";
        default: return "Gear_Back: not decided";
    }
}
// The first-person avatar's census: the parts with an instance (f142::Census: instances, class, flags, items, masks as read BEFORE EDVR's zeroing), the parts EDVR's list
// zeroes, and of those the ones that had a mask bit set (so the zero changed something). A change is the census's signature (never the masks) or either set.
struct FpCensus {
    f142::Census c;
    uint64_t listMask = 0, zeroedMask = 0;
};
inline uint64_t fpSig(const FpCensus& f) noexcept {
    return f142::sigMix(f142::sigMix(f142::censusSig(f.c), f.listMask), f.zeroedMask);
}
// The parts (bit i = part i) among the list that had a view mask bit set in the census: what the zero took away.
inline uint64_t zeroedOf(const f142::Census& c, uint64_t listMask) noexcept {
    uint64_t m = 0;
    for (uint32_t i = 0; i < c.n && i < f142::kPartCount; ++i) {
        const f142::PartRow& r = c.row[i];
        if (((listMask >> r.idx) & 1ull) && (r.masks[0] | r.masks[1] | r.masks[2] | r.masks[3])) m |= 1ull << r.idx;
    }
    return m;
}

// ---- 3. the readout placed per role ---------------------------------------------------------------------------------------------------------------------------------------
enum Role : int { kHip = 0, kAds = 1, kRoles = 2 };
inline const char* roleName(int r) noexcept { return r == kHip ? "hip" : "ADS"; }
constexpr double kAdsDrawnW = 0.5;   // the ADS weight from which the ADS role is the one drawn now
inline int drawnRole(double adsW) noexcept { return adsW >= kAdsDrawnW ? kAds : kHip; }
struct Row {
    const char* item;   // the exact item name (Status.json SelectedWeapon)
    int scope;          // loadout::kScopeNo or kScopeYes
    int role;
    double v[4];        // forward, left, up, toward eye (cm), in the weapon frame the class's placement uses (native_on_foot_f90.h target())
};
// F144 seeded this from the values then in use (the F140 table for the ADS roles and the pistols, 0 for a rifle-class weapon's hip role). F145 bakes the pilot's dialled totals
// (the seed plus trim each role was last settled at for 3 s or more in the F144 flight) into the scope-no rows of the eight weapons he dialled: the pistols' hip rows are as they
// were (13, 3.5, -3, 3). The scope-yes rows, the fists, the plasma pistol and the plasma sniper stay as F144 left them: the pistol and the sniper were never placed in that flight.
constexpr Row kTable[] = {
    {"$wpn_m_assaultrifle_laser_fauto_name;", loadout::kScopeNo, kHip, {0.0, 0.0, 0.0, 0.0}},
    {"$wpn_m_assaultrifle_laser_fauto_name;", loadout::kScopeNo, kAds, {0.0, 3.5, 0.0, 0.0}},
    {"$wpn_m_assaultrifle_laser_fauto_name;", loadout::kScopeYes, kHip, {0.0, 0.0, 0.0, 0.0}},
    {"$wpn_m_assaultrifle_laser_fauto_name;", loadout::kScopeYes, kAds, {0.0, 3.5, 0.0, 0.0}},
    {"$wpn_m_assaultrifle_plasma_fauto_name;", loadout::kScopeNo, kHip, {7.0, 2.5, 0.0, 0.0}},
    {"$wpn_m_assaultrifle_plasma_fauto_name;", loadout::kScopeNo, kAds, {7.0, 6.0, 0.0, 0.0}},
    {"$wpn_m_assaultrifle_plasma_fauto_name;", loadout::kScopeYes, kHip, {0.0, 0.0, 0.0, 0.0}},
    {"$wpn_m_assaultrifle_plasma_fauto_name;", loadout::kScopeYes, kAds, {0.0, 3.5, 0.0, 0.0}},
    {"$wpn_m_assaultrifle_kinetic_fauto_name;", loadout::kScopeNo, kHip, {5.0, 4.5, 0.0, 0.0}},
    {"$wpn_m_assaultrifle_kinetic_fauto_name;", loadout::kScopeNo, kAds, {3.5, 5.5, 0.0, 0.0}},
    {"$wpn_m_assaultrifle_kinetic_fauto_name;", loadout::kScopeYes, kHip, {0.0, 0.0, 0.0, 0.0}},
    {"$wpn_m_assaultrifle_kinetic_fauto_name;", loadout::kScopeYes, kAds, {0.0, 3.5, 0.0, 0.0}},
    {"$wpn_m_launcher_rocket_sauto_name;", loadout::kScopeNo, kHip, {5.5, 0.0, 0.0, 0.0}},
    {"$wpn_m_launcher_rocket_sauto_name;", loadout::kScopeNo, kAds, {5.5, 3.5, 0.0, 0.0}},
    {"$wpn_m_launcher_rocket_sauto_name;", loadout::kScopeYes, kHip, {0.0, 0.0, 0.0, 0.0}},
    {"$wpn_m_launcher_rocket_sauto_name;", loadout::kScopeYes, kAds, {0.0, 3.5, 0.0, 0.0}},
    {"$wpn_m_sniper_plasma_charged_name;", loadout::kScopeNo, kHip, {0.0, 0.0, 0.0, 0.0}},
    {"$wpn_m_sniper_plasma_charged_name;", loadout::kScopeNo, kAds, {0.0, 3.5, 0.0, 0.0}},
    {"$wpn_m_sniper_plasma_charged_name;", loadout::kScopeYes, kHip, {0.0, 0.0, 0.0, 0.0}},
    {"$wpn_m_sniper_plasma_charged_name;", loadout::kScopeYes, kAds, {0.0, 3.5, 0.0, 0.0}},
    {"$wpn_m_submachinegun_kinetic_fauto_name;", loadout::kScopeNo, kHip, {6.5, 0.0, 0.0, 0.0}},
    {"$wpn_m_submachinegun_kinetic_fauto_name;", loadout::kScopeNo, kAds, {6.5, 3.5, 0.0, 0.0}},
    {"$wpn_m_submachinegun_kinetic_fauto_name;", loadout::kScopeYes, kHip, {0.0, 0.0, 0.0, 0.0}},
    {"$wpn_m_submachinegun_kinetic_fauto_name;", loadout::kScopeYes, kAds, {0.0, 3.5, 0.0, 0.0}},
    {"$wpn_m_submachinegun_laser_fauto_name;", loadout::kScopeNo, kHip, {7.0, 1.5, 0.5, 0.0}},
    {"$wpn_m_submachinegun_laser_fauto_name;", loadout::kScopeNo, kAds, {10.0, 4.5, 1.0, 0.0}},
    {"$wpn_m_submachinegun_laser_fauto_name;", loadout::kScopeYes, kHip, {0.0, 0.0, 0.0, 0.0}},
    {"$wpn_m_submachinegun_laser_fauto_name;", loadout::kScopeYes, kAds, {0.0, 3.5, 0.0, 0.0}},
    {"$wpn_m_shotgun_plasma_doublebarrel_name;", loadout::kScopeNo, kHip, {10.0, -0.5, 1.0, 0.0}},
    {"$wpn_m_shotgun_plasma_doublebarrel_name;", loadout::kScopeNo, kAds, {10.0, 3.0, 1.0, 0.0}},
    {"$wpn_m_shotgun_plasma_doublebarrel_name;", loadout::kScopeYes, kHip, {0.0, 0.0, 0.0, 0.0}},
    {"$wpn_m_shotgun_plasma_doublebarrel_name;", loadout::kScopeYes, kAds, {0.0, 3.5, 0.0, 0.0}},
    {"$wpn_s_pistol_kinetic_sauto_name;", loadout::kScopeNo, kHip, {13.0, 3.5, -3.0, 3.0}},
    {"$wpn_s_pistol_kinetic_sauto_name;", loadout::kScopeNo, kAds, {16.0, 5.0, -3.0, 3.0}},
    {"$wpn_s_pistol_kinetic_sauto_name;", loadout::kScopeYes, kHip, {13.0, 3.5, -3.0, 3.0}},
    {"$wpn_s_pistol_kinetic_sauto_name;", loadout::kScopeYes, kAds, {13.0, 3.5, -3.0, 3.0}},
    {"$wpn_s_pistol_laser_sauto_name;", loadout::kScopeNo, kHip, {13.0, 3.5, -3.0, 3.0}},
    {"$wpn_s_pistol_laser_sauto_name;", loadout::kScopeNo, kAds, {18.0, 6.5, -3.0, 3.0}},
    {"$wpn_s_pistol_laser_sauto_name;", loadout::kScopeYes, kHip, {13.0, 3.5, -3.0, 3.0}},
    {"$wpn_s_pistol_laser_sauto_name;", loadout::kScopeYes, kAds, {13.0, 3.5, -3.0, 3.0}},
    {"$wpn_s_pistol_plasma_charged_name;", loadout::kScopeNo, kHip, {13.0, 3.5, -3.0, 3.0}},
    {"$wpn_s_pistol_plasma_charged_name;", loadout::kScopeNo, kAds, {13.0, 3.5, -3.0, 3.0}},
    {"$wpn_s_pistol_plasma_charged_name;", loadout::kScopeYes, kHip, {13.0, 3.5, -3.0, 3.0}},
    {"$wpn_s_pistol_plasma_charged_name;", loadout::kScopeYes, kAds, {13.0, 3.5, -3.0, 3.0}},
};
constexpr int kTableCount = static_cast<int>(sizeof(kTable) / sizeof(kTable[0]));
inline int entryScopeOf(int scopeState) noexcept { return scopeState == loadout::kScopeYes ? loadout::kScopeYes : loadout::kScopeNo; }
inline const Row* find(const char* name, int scopeState, int role) noexcept {
    if (!name) return nullptr;
    const int scope = entryScopeOf(scopeState);
    for (const Row& r : kTable)
        if (r.scope == scope && r.role == role && std::strcmp(name, r.item) == 0) return &r;
    return nullptr;
}
struct RolePlacement {
    bool coherent = false;       // the item snapshot was usable
    bool weapon = false;         // a $wpn_ item: the trims apply
    bool table = false;          // the item has a row (under the scope row used)
    bool drawnNow = false;       // this role is the one drawn now: the trims are added to it
    bool scopeUnknown = false;
    int cls = f140::kClassRifle; // the class whose built-in values stood in for a miss
    int role = kHip;
    int scope = loadout::kScopeUnknown, entryScope = loadout::kScopeNo;
    double seed[4] = {}, trim[4] = {}, total[4] = {};
    f90::Keys keys() const noexcept {
        f90::Keys k;
        k.forwardCm = total[0];
        k.leftCm = total[1];
        k.upCm = total[2];
        k.towardCm = total[3];
        return k;
    }
};
// The placement of one role of the held item. cls picks the built-in values a miss falls back to (the pistol class's for both its roles; the rifle class's for its ADS role and 0
// for its hip role). trim: the four live keys, added only when this role is the one drawn now and the item is a coherent $wpn_ item.
inline RolePlacement place(bool readOk, uint32_t snapshotSerial, uint32_t currentSerial, const char* name, int scopeState, int cls, int role, bool drawnNow,
                           const double trim[4]) noexcept {
    RolePlacement p;
    p.cls = cls;
    p.role = role;
    p.drawnNow = drawnNow;
    p.scope = scopeState;
    p.entryScope = entryScopeOf(scopeState);
    p.scopeUnknown = scopeState != loadout::kScopeNo && scopeState != loadout::kScopeYes;
    if (cls == f140::kClassPistol) {
        for (int i = 0; i < 4; ++i) p.seed[i] = f140::kPistolClass[i];
    } else if (role == kAds) {
        for (int i = 0; i < 4; ++i) p.seed[i] = f140::kRifleClass[i];
    }   // the rifle class's hip role: 0
    p.coherent = f140::itemCoherent(readOk, snapshotSerial, currentSerial, name);
    if (p.coherent) {
        p.weapon = f140::isWeapon(name);
        if (const Row* r = find(name, scopeState, role)) {
            p.table = true;
            for (int i = 0; i < 4; ++i) p.seed[i] = r->v[i];
        }
        if (p.weapon && drawnNow && trim)
            for (int i = 0; i < 4; ++i) p.trim[i] = f140::clampTrim(i, trim[i]);
    }
    for (int i = 0; i < 4; ++i) p.total[i] = f140::clampTotal(i, p.seed[i] + p.trim[i]);
    return p;
}

// ---- the proof: what reaches the draw ------------------------------------------------------------------------------------------------------------------------------------
// The drawn centre of a placed part (the world position the patched rows put its content centre at, found by inverting them: the camera the patched rows draw with) against the
// same frame without the placement (the getter's U frame, before F90's map), along the weapon's axes; the expected move is the placement's own (T - C, metres).
struct Move {
    double cm[3] = {};          // the drawn move along the weapon's barrel, left and up (cm)
    double absCm = 0.0;         // |drawn move| (cm)
    double expAbsCm = 0.0;      // |expected move| (cm): the total's
    double diffMm = 0.0;        // |drawn move - expected move| (mm)
};
inline Move moveOf(const double drawn[3], const double game[3], const double expected[3], const double b[3], const double l[3], const double u[3]) noexcept {
    Move m;
    const double d[3] = {drawn[0] - game[0], drawn[1] - game[1], drawn[2] - game[2]};
    m.cm[0] = 100.0 * (d[0] * b[0] + d[1] * b[1] + d[2] * b[2]);
    m.cm[1] = 100.0 * (d[0] * l[0] + d[1] * l[1] + d[2] * l[2]);
    m.cm[2] = 100.0 * (d[0] * u[0] + d[1] * u[1] + d[2] * u[2]);
    m.absCm = 100.0 * std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    m.expAbsCm = 100.0 * std::sqrt(expected[0] * expected[0] + expected[1] * expected[1] + expected[2] * expected[2]);
    const double e[3] = {d[0] - expected[0], d[1] - expected[1], d[2] - expected[2]};
    m.diffMm = 1000.0 * std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    return m;
}
// A role's window: the frames its placement was applied, the frames measured (the patched rows solved to a camera, and the seed-only reference too) and bound at the draw's issue,
// and the bound frames' sums. F145: the proof is the drawn move against the same frame placed at the seed alone (the sums without a prefix: what the trims asked and what the draw
// did with it), the move against the game's own draw is kept as information (the game* sums): a seed moves the part from where the game draws it on purpose, and a size factor
// makes the inverted camera no rotation, so that move reads tens of cm in the ADS roles whatever the pilot dialled.
struct RoleAcc {
    bool have = false;
    char item[64] = {};
    int scope = loadout::kScopeUnknown;
    double seed[4] = {}, trim[4] = {}, total[4] = {};
    uint64_t placed = 0, measured = 0, bound = 0;
    double moveSum[3] = {}, absSum = 0.0, expSum = 0.0, diffMaxMm = 0.0;
    double gameMoveSum[3] = {}, gameAbsSum = 0.0, gameExpSum = 0.0, gameDiffMaxMm = 0.0;
    void fold(const Move& vsSeed, const Move& vsGame) noexcept {
        ++bound;
        for (int i = 0; i < 3; ++i) {
            moveSum[i] += vsSeed.cm[i];
            gameMoveSum[i] += vsGame.cm[i];
        }
        absSum += vsSeed.absCm;
        expSum += vsSeed.expAbsCm;
        diffMaxMm = std::max(diffMaxMm, vsSeed.diffMm);
        gameAbsSum += vsGame.absCm;
        gameExpSum += vsGame.expAbsCm;
        gameDiffMaxMm = std::max(gameDiffMaxMm, vsGame.diffMm);
    }
};
enum Verdict : int { kVNotMeasured = 0, kVNotReaching, kVNoMoveAsked, kVDiffers, kVReaches };
constexpr double kReachMm = 1.0;       // the drawn move must equal the trims' within this
constexpr double kNoMoveCm = 0.05;     // a move under this (0.5 mm) is no move
constexpr double kAskedCm = 0.1;       // a trim move of at least this (1 mm) is a move asked
inline int verdictOf(const RoleAcc& a) noexcept {
    if (!a.measured) return kVNotMeasured;
    if (!a.bound) return kVNotReaching;
    const double n = static_cast<double>(a.bound);
    const double meanAbs = a.absSum / n, meanExp = a.expSum / n;
    if (meanExp < kAskedCm) return a.diffMaxMm <= kReachMm ? kVNoMoveAsked : kVDiffers;
    if (meanAbs < kNoMoveCm) return kVNotReaching;
    return a.diffMaxMm <= kReachMm ? kVReaches : kVDiffers;
}
inline const char* verdictText(int v) noexcept {
    switch (v) {
        case kVNotMeasured: return "not measured (the draw's camera could not be solved)";
        case kVNotReaching: return "NOT REACHING THE DRAW";
        case kVNoMoveAsked: return "no trim asked: drawn at its seed";
        case kVDiffers: return "the drawn move differs from the trims'";
        default: return "reaches the draw";
    }
}

}  // namespace f144
}  // namespace nofoot
}  // namespace edvr

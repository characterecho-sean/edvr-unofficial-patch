#pragma once
// The optimized station's pure half (optimized_station.cpp). No Windows and no engine memory: the record rules of the sealed hangar, the own pad's
// cell (its centre, its shaft to the hangar's BlastShieldBase, its oriented box), the cell held in the pad's own frame, the release rules, the
// rig verdicts of the early veto, and the fault-safe cull. tools\optimized_station_test compiles it alone.
//
// The rule (named mode): a record is kept when its centre lies in the sealed cell (the box along the shaft, or the pad's sphere before a shaft is
// found), when it is an explicit part of the hangar nearest the shaft, when it is an own-ship part, or when its centre cannot be read. Everything
// else is culled from the views. A record whose centre cannot be read is kept, never judged (rawCentreUsable, optimized_station.cpp).
#include <cctype>
#include <cmath>
#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace edvr::sealed {

// The builder's transformed centre: out = R(q) * c + t (the same expression as decomp_42B4420.txt:196-237). The identity quaternion gives c + t.
inline void builderCentre(const float q[4], const float t[4], const float c[4], float out[4]) noexcept {
    const float w2 = q[3] + q[3];
    const float dot = q[0] * c[0] + q[1] * c[1] + q[2] * c[2];
    const float ww = q[3] * q[3] + q[3] * q[3];
    out[0] = (c[2] * q[1] - c[1] * q[2]) * w2 + ww * c[0] + (-c[0]) + dot * (q[0] + q[0]) + t[0];
    out[1] = (c[0] * q[2] - c[2] * q[0]) * w2 + ww * c[1] + (-c[1]) + dot * (q[1] + q[1]) + t[1];
    out[2] = (c[1] * q[0] - c[0] * q[1]) * w2 + ww * c[2] + (-c[2]) + dot * (q[2] + q[2]) + t[2];
    out[3] = (c[3] * q[3] - c[3] * q[3]) * w2 + ww * c[3] + (-c[3]) + dot * w2 + t[3];
}

// Where a record's centre came from: the pose (the builder's), the raw rec+0x240 (the pose was unreadable: a counted fallback), or none.
enum class CentreSource : uint8_t { kPose = 0, kRaw = 1, kNone = 2 };

inline bool finite3(const float v[3]) noexcept { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

// The record's centre from what was read: the pose centre when it is readable and finite; else rec+0x240 when that is; else none.
inline CentreSource chooseCentre(bool poseOk, const float pose[3], bool rawOk, const float raw[3], float out[3]) noexcept {
    if (poseOk && pose && finite3(pose)) {
        out[0] = pose[0];
        out[1] = pose[1];
        out[2] = pose[2];
        return CentreSource::kPose;
    }
    if (rawOk && raw && finite3(raw)) {
        out[0] = raw[0];
        out[1] = raw[1];
        out[2] = raw[2];
        return CentreSource::kRaw;
    }
    out[0] = out[1] = out[2] = 0.0f;
    return CentreSource::kNone;
}

// rec+0x240 is exactly the origin: the value the flight showed for dozens of distinct records.
inline bool isZeroCentre(const float c[3]) noexcept { return c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f; }

// Named mode's cell (Built v8.3b; Built v11 capsule). The radii are GUESSES until measured on the flight: 30 m for a small pad (Built v11: the
// capsule's radius, the addendum's guess; was 60), 90 m medium, 130 m large. The ladder is what hotkey.sealed_cell_larger and
// hotkey.sealed_cell_smaller (PAGEUP and PAGEDOWN) step the CURRENT size's radius through, ascending (Built v11: 15 to 60 m).
constexpr float kCellDefaultM[3] = {30.0f, 90.0f, 130.0f};

constexpr float kPadFallbackM = 150.0f;      // no pad sphere contains the camera: the nearest pad centre within this

constexpr float kExplicitM = 200.0f;         // an explicit part's nearest instance to the cell centre must be within this (no capsule)

constexpr float kExplicitCapsuleM = 15.0f;   // Built v11: with a capsule, an explicit part's nearest instance must be within this of the axis

constexpr size_t kPadSlots = 32;             // pad records a frame keeps for the pick

constexpr size_t kExplicitSlots = 128;       // explicit-part records a frame keeps for the nearest-instance pick

constexpr size_t kExplicitNodes = 64;        // distinct part nodes the nearest-instance pick tracks

inline float distance(const float a[3], const float b[3]) noexcept {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The record's sphere contains the camera.
inline bool containsCamera(const float centre[3], float radius, const float camera[3]) noexcept {
    return distance(centre, camera) <= radius;
}

// The record's sphere intersects the sphere of radius R around the camera.
inline bool intersectsCameraSphere(const float centre[3], float radius, const float camera[3], float R) noexcept {
    return distance(centre, camera) <= radius + R;
}

// The sphere rule: keep a record that contains the camera or intersects the camera's sphere.
inline bool keepRecord(const float centre[3], float radius, const float camera[3], float R) noexcept {
    return containsCamera(centre, radius, camera) || intersectsCameraSphere(centre, radius, camera, R);
}

// Case-insensitive prefix test on ASCII text.
inline bool hasPrefixNoCase(const char* text, const char* word) noexcept {
    for (size_t k = 0; word[k]; ++k) {
        if (!text[k] || std::tolower(static_cast<unsigned char>(text[k])) != std::tolower(static_cast<unsigned char>(word[k]))) return false;
    }
    return true;
}

// Case-insensitive substring test on ASCII text.
inline bool containsNoCase(const char* text, const char* word) noexcept {
    const size_t n = std::strlen(word);
    for (size_t i = 0; text[i]; ++i) {
        size_t k = 0;
        while (k < n && text[i + k] && std::tolower(static_cast<unsigned char>(text[i + k])) ==
                                           std::tolower(static_cast<unsigned char>(word[k])))
            ++k;
        if (k == n) return true;
    }
    return false;
}

// The class of a record's name, as the lock-free cache stores it. kPad: a LandingPad* record (GuideMarkers excluded). kExplicit: a part whose
// centre is judged against the cell by its nearest instance (Hanger, Hangar, BlastShield, SmallHangerSkirt, MediumHangerSkirt, LargeHangerSkirt).
// kOther: any other readable name. kUnnamed: no readable name. kNone is the empty cache slot, never a verdict.
enum class NameClass : uint8_t { kNone = 0, kPad = 1, kExplicit = 2, kOther = 3, kUnnamed = 4 };

inline bool isPadName(const char* name) noexcept {
    return name && hasPrefixNoCase(name, "LandingPad") && !containsNoCase(name, "GuideMarkers");
}

inline bool isExplicitName(const char* name) noexcept {
    if (!name) return false;
    static const char* const kPrefixes[] = {"Hanger", "Hangar", "BlastShield", "SmallHangerSkirt", "MediumHangerSkirt", "LargeHangerSkirt"};
    for (const char* p : kPrefixes)
        if (hasPrefixNoCase(name, p)) return true;
    return false;
}

inline NameClass classifyName(bool readable, const char* name) noexcept {
    if (!readable || !name || !*name) return NameClass::kUnnamed;
    if (isPadName(name)) return NameClass::kPad;
    if (isExplicitName(name)) return NameClass::kExplicit;
    return NameClass::kOther;
}

// The own pad's size, from its name: LandingPadSmall*, LandingPadMedium*, LandingPadLarge* (GuideMarkers and other names: none).
enum class PadSize : uint8_t { kNone = 0, kSmall = 1, kMedium = 2, kLarge = 3 };

inline PadSize padSizeOf(const char* name) noexcept {
    if (!isPadName(name)) return PadSize::kNone;
    const char* rest = name + 10;   // past "LandingPad"
    if (hasPrefixNoCase(rest, "Small")) return PadSize::kSmall;
    if (hasPrefixNoCase(rest, "Medium")) return PadSize::kMedium;
    if (hasPrefixNoCase(rest, "Large")) return PadSize::kLarge;
    return PadSize::kNone;
}

inline char padSizeLetter(PadSize s) noexcept {
    switch (s) {
        case PadSize::kSmall: return 'S';
        case PadSize::kMedium: return 'M';
        case PadSize::kLarge: return 'L';
        default: return 'n';
    }
}

// One pad record of a frame, for the pick.
struct PadObs {
    uintptr_t addr = 0;
    float centre[3] = {};
    float radius = 0.0f;       // the pad's own sphere (rec+0x280)
    PadSize size = PadSize::kNone;
    char name[64] = {};
    // Built v14 (fix 1): the pad's up axis (its builder pose, axes[2]); upOk false when the pose is unreadable.
    float up[3] = {};
    bool upOk = false;
};

// The own cell of a frame. has false: no own pad this frame (named mode is the sphere rule for it) and reason says why.
struct OwnCell {
    bool has = false;
    PadSize size = PadSize::kNone;
    float centre[3] = {};      // the pad's centre
    float radius = 0.0f;       // the cell radius for the pad's size
    uintptr_t pad = 0;
    float padDist = -1.0f;     // the camera to the pad's centre
    bool padContains = false;  // the pad's own sphere contains the camera (else it was the fallback within kPadFallbackM)
    char padName[64] = {};
    char reason[80] = {};
    // Built v11: the capsule. When capsule is true the cell test is the distance to the segment capA..capB (radius kept) instead of the sphere
    // about centre; capLen is the pad-to-target distance and capTarget the target's name.
    bool capsule = false;
    float capA[3] = {};
    float capB[3] = {};
    float capLen = 0.0f;
    char capTarget[48] = {};
    // Built v11 (second addendum): the oriented box along the shaft. When box is true the named rule's cell is the box: boxCentre, the length
    // axis boxU, the width axis boxV, the shaft axis boxW (unit vectors), half-extents boxHalf (length, width, height). boxInfo says how it was
    // built (the pad's axes, extents, the choice), or why it was not (then the capsule or the pad sphere is the cell).
    bool box = false;
    float boxCentre[3] = {};
    float boxU[3] = {};
    float boxV[3] = {};
    float boxW[3] = {};
    float boxHalf[3] = {};
    float boxMargin = 0.0f;
    float boxLateral = 0.0f;         // Built v12: the BlastShieldBase (the target) off the pad's up axis, metres (the box stands on the up axis)
    char boxInfo[320] = {};
};

// The index into radii[] (small, medium, large) for a size; -1 for none.
inline int sizeIndex(PadSize s) noexcept { return s == PadSize::kNone ? -1 : static_cast<int>(s) - 1; }

// The own pad: among the frame's pad records with a size, the one whose own sphere contains the camera, nearest centre first; else the nearest
// centre within kPadFallbackM. The cell's radius is radii[size]. Built v14 (fix 1): a pad within kPadTieM of the nearest is a near-tie; the tie goes
// to the pad whose centre is nearest the camera laterally to its own up axis, then to the platform named A (LandingPadMediumA over ...B), then to the
// lower record address (so the order of the records never decides). With no candidate, has is false and reason says why.
constexpr float kPadTieM = 5.0f;

// The distance of the camera from the pad's up axis through its centre (the height along the axis is not counted). Without an up axis the total
// distance stands in.
inline float padLateralM(const PadObs& p, const float camera[3]) noexcept {
    const float d[3] = {camera[0] - p.centre[0], camera[1] - p.centre[1], camera[2] - p.centre[2]};
    if (!p.upOk) return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const float a = d[0] * p.up[0] + d[1] * p.up[1] + d[2] * p.up[2];
    const float l[3] = {d[0] - a * p.up[0], d[1] - a * p.up[1], d[2] - a * p.up[2]};
    return std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
}

// True when the pad's platform name ends in A.
inline bool padIsPlatformA(const PadObs& p) noexcept {
    const size_t len = std::strlen(p.name);
    return len > 0 && p.name[len - 1] == 'A';
}

// Built v14.8: a platform named B (LandingPadMediumB) is the lift platform in a medium bay, ~40 m lateral to A; the A platform of the same size is
// the bay's pad. A B pad is never picked while an A pad of the same size is within kPlatformAM metres of the camera.
constexpr float kPlatformAM = 60.0f;
// Built v14.8: the box is a seal only with every half-extent at least this many metres (a zero box culls the whole bay).
constexpr float kBoxMinHalfM = 10.0f;
// Built v14.8: a ship has many parts; a boarding volume or a GUI record does not. A prefix names a ship only with this many records within
// kShipRecordsM of the camera.
constexpr uint32_t kShipRecordsMin = 10;
constexpr float kShipRecordsM = 40.0f;

inline bool padIsPlatformB(const PadObs& p) noexcept {
    const size_t len = std::strlen(p.name);
    return len > 0 && p.name[len - 1] == 'B';
}

// True when an A-platform pad of this size is within kPlatformAM of the camera.
inline bool aPadNear(const PadObs* obs, size_t n, const float camera[3], PadSize size) noexcept {
    for (size_t i = 0; i < n; ++i) {
        if (obs[i].size != size || !padIsPlatformA(obs[i])) continue;
        if (distance(obs[i].centre, camera) <= kPlatformAM) return true;
    }
    return false;
}

// Why the pad's cell cannot seal, or nullptr when it can: a pad, a shaft target, a box with every half-extent at least kBoxMinHalfM (the floor is
// the box's height half-extent, which includes the below distance).
inline const char* cellInvalidWhy(const OwnCell& c) noexcept {
    if (!c.has) return "no own pad";
    if (!c.capsule) return "no shaft target (no instance of the pad's target name on the shaft)";
    if (!c.box) return "no box (the pad's pose or the target axis is unreadable)";
    for (int k = 0; k < 3; ++k) {
        if (!(c.boxHalf[k] >= kBoxMinHalfM)) return "box under 10 m on an axis (no valid bay volume)";
    }
    return nullptr;
}

inline void fillOwnFromPad(const PadObs& p, const float radii[3], float d, OwnCell* out) noexcept {
    out->has = true;
    out->size = p.size;
    for (int k = 0; k < 3; ++k) out->centre[k] = p.centre[k];
    out->radius = radii[sizeIndex(p.size)];
    out->pad = p.addr;
    out->padDist = d;
    out->padContains = d <= p.radius;
    std::snprintf(out->padName, sizeof(out->padName), "%s", p.name);
}

inline void pickOwnPad(const PadObs* obs, size_t n, const float camera[3], const float radii[3], OwnCell* out) noexcept {
    *out = OwnCell{};
    if (n == 0) {
        std::snprintf(out->reason, sizeof(out->reason), "no LandingPad record this frame");
        return;
    }
    // Built v14.8: a B platform is not eligible while an A platform of its size is within kPlatformAM.
    auto eligible = [&](size_t i) noexcept {
        if (obs[i].size == PadSize::kNone) return false;
        return !(padIsPlatformB(obs[i]) && aPadNear(obs, n, camera, obs[i].size));
    };
    bool containing = false;
    for (size_t i = 0; i < n && !containing; ++i) {
        if (eligible(i) && distance(obs[i].centre, camera) <= obs[i].radius) containing = true;
    }
    float nearest = -1.0f;
    for (size_t i = 0; i < n; ++i) {
        if (!eligible(i)) continue;
        const float d = distance(obs[i].centre, camera);
        if (!(containing ? d <= obs[i].radius : d <= kPadFallbackM)) continue;
        if (nearest < 0.0f || d < nearest) nearest = d;
    }
    if (nearest < 0.0f) {
        std::snprintf(out->reason, sizeof(out->reason), "no pad sphere contains the camera and none is within %.0f m", static_cast<double>(kPadFallbackM));
        return;
    }
    int best = -1;
    float bestD = 0.0f, bestLat = 0.0f;
    bool bestA = false;
    for (size_t i = 0; i < n; ++i) {
        if (!eligible(i)) continue;
        const float d = distance(obs[i].centre, camera);
        if (!(containing ? d <= obs[i].radius : d <= kPadFallbackM)) continue;
        if (d > nearest + kPadTieM) continue;   // not a near-tie with the nearest
        const float lat = padLateralM(obs[i], camera);
        const bool isA = padIsPlatformA(obs[i]);
        bool better = best < 0;
        if (!better) {
            if (lat < bestLat - 1e-3f) better = true;
            else if (lat <= bestLat + 1e-3f) {
                if (isA != bestA) better = isA;
                else better = obs[i].addr < obs[best].addr;
            }
        }
        if (better) {
            best = static_cast<int>(i);
            bestD = d;
            bestLat = lat;
            bestA = isA;
        }
    }
    fillOwnFromPad(obs[best], radii, bestD, out);
}

// One explicit-part record of a frame: its node identifies the part name (records of one part share it).
struct ExplicitObs {
    uintptr_t addr = 0;
    uintptr_t node = 0;
    float centre[3] = {};
    char name[48] = {};        // Built v11: the part's name (the capsule's target is picked by it)
};

// For each part node, the nearest instance to the cell centre within kExplicitM. Writes their addresses to out (at most outCap); returns the count.
inline size_t pickExplicit(const ExplicitObs* obs, size_t n, const float cellCentre[3], uintptr_t* out, size_t outCap) noexcept {
    struct Best {
        uintptr_t node;
        uintptr_t addr;
        float d;
    };
    Best best[kExplicitNodes];
    size_t nb = 0;
    for (size_t i = 0; i < n; ++i) {
        const float d = distance(obs[i].centre, cellCentre);
        if (d > kExplicitM) continue;
        size_t k = 0;
        while (k < nb && best[k].node != obs[i].node) ++k;
        if (k < nb) {
            if (d < best[k].d) {
                best[k].addr = obs[i].addr;
                best[k].d = d;
            }
        } else if (nb < kExplicitNodes) {
            best[nb++] = Best{obs[i].node, obs[i].addr, d};
        }
    }
    size_t written = 0;
    for (size_t k = 0; k < nb && written < outCap; ++k) out[written++] = best[k].addr;
    return written;
}

// Built v11: the cell capsule. Its target is the nearest BlastShieldBase* explicit instance within kCapsuleMaxM of the pad (else the nearest own
// HangerSmall, HangerMedium or HangerLarge within the same reach). The axis runs from the pad's centre to the target and on kCapsuleExtendM past
// it, so a 90 m shaft that a sphere on the pad cannot hold is inside the cell. The cell test is the distance to that segment.
constexpr float kCapsuleMaxM = 400.0f;

constexpr float kCapsuleExtendM = 20.0f;

inline bool hasPrefix(const char* s, const char* p) noexcept { return std::strncmp(s, p, std::strlen(p)) == 0; }

inline bool isBlastBase(const char* name) noexcept { return hasPrefix(name, "BlastShieldBase"); }

inline bool isOwnHanger(const char* name) noexcept {
    return hasPrefix(name, "HangerSmall") || hasPrefix(name, "HangerMedium") || hasPrefix(name, "HangerLarge");
}

// The distance from p to the segment ab (its end points included).
inline float distToSegment(const float p[3], const float a[3], const float b[3]) noexcept {
    const float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float ap[3] = {p[0] - a[0], p[1] - a[1], p[2] - a[2]};
    const float len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float t = len2 > 0.0f ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / len2 : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float q[3] = {a[0] + t * ab[0], a[1] + t * ab[1], a[2] + t * ab[2]};
    return distance(p, q);
}

// The capsule from the pad centre p to the target q, extended past q by extend along the axis. False when the target is not a distance away.
inline bool buildCapsule(const float p[3], const float q[3], float extend, float a[3], float b[3], float* length) noexcept {
    const float d[3] = {q[0] - p[0], q[1] - p[1], q[2] - p[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!std::isfinite(len) || !(len >= 1.0f)) return false;
    for (int k = 0; k < 3; ++k) {
        a[k] = p[k];
        b[k] = q[k] + d[k] / len * extend;
    }
    if (length) *length = len;
    return finite3(a) && finite3(b);
}

// Built v13.1 (b): the own hangar's parts are kept in named mode. The bay volume is the shaft from the pad's centre o along its unit axis u:
// lateral distance from the shaft within kBayLateralM, along it from -below (under the pad; diag.sealed_cell_below_m, default kBelowDefaultM) to
// the target's distance plus kBayPastM. Built v13.2: the below distance is the box's (one key), 60 m.
constexpr float kBayLateralM = 60.0f;

constexpr float kBayPastM = 20.0f;

// Built v13.2 (a): the below distance: the box bottom and the bay's lower bound, measured from the pad record's centre along the shaft.
// Built v13.4 (a): one default per pad size (small, medium, large), from the measured lift doors (small -72, medium -104, large -126 m) plus a
// margin: 85, 120, 145 m. Index is sizeIndex(size). diag.sealed_cell_below_<size>_m; diag.sealed_cell_below_m, when set, overrides all three.
constexpr float kBelowDefaultsM[3] = {85.0f, 120.0f, 145.0f};

// Built v13.4 (a): the below distance for a size: the override when one is set, else that size's table entry (small for no size).
inline float belowForSize(PadSize size, const float table[3], bool hasOverride, float overrideM) noexcept {
    if (hasOverride) return overrideM;
    const int i = sizeIndex(size);
    return table[i < 0 ? 0 : i];
}

// Built v13.3 (c): a raw centre is usable only when it is finite, not the origin (rec+0x240 zero is an unreadable centre, not a place) and its
// radius is positive and finite. A zero raw centre with a radius would be a fake sphere at the origin, judged as far away and culled.
inline bool rawCentreUsable(const float raw[3], float radius) noexcept {
    return finite3(raw) && !isZeroCentre(raw) && std::isfinite(radius) && radius > 0.0f;
}

// Built v13.3 (b): the own pad candidates: a pad record containing the camera, or with its centre within kOwnPadNearM of it, of any size.
constexpr float kOwnPadNearM = 40.0f;

inline bool ownPadCandidate(float centreDist, float radius) noexcept {
    return centreDist <= radius || centreDist <= kOwnPadNearM;
}

// Built v13.3 (b): the held cell is dropped when its pad record is absent for limit consecutive frames, or another own pad candidate is the
// nearest one for limit consecutive frames.
constexpr uint32_t kHeldLostFrames = 30;

inline bool heldCellDrops(uint32_t absentFrames, uint32_t otherFrames, uint32_t limit) noexcept {
    return absentFrames >= limit || otherFrames >= limit;
}

// Built v13.7 (a): the held cell in the held PAD's own frame. A point is written as its coordinates along the pad's three unit axes from the
// pad's centre; a direction the same way without the centre. The frame the records are expressed in can shift (the v13.6 flight: a static pad
// moved 34 m when the player went on foot, the same offset for the box), so the cell is frozen in the pad's frame at engage and rebuilt from the
// pad's live pose before every judgement. axes[k] is the k-th unit axis in world coordinates (axes[2] is up).
inline void pointToLocal(const float p[3], const float centre[3], const float axes[3][3], float out[3]) noexcept {
    const float d[3] = {p[0] - centre[0], p[1] - centre[1], p[2] - centre[2]};
    for (int k = 0; k < 3; ++k) out[k] = axes[k][0] * d[0] + axes[k][1] * d[1] + axes[k][2] * d[2];
}

inline void pointToWorld(const float local[3], const float centre[3], const float axes[3][3], float out[3]) noexcept {
    for (int j = 0; j < 3; ++j) out[j] = centre[j] + local[0] * axes[0][j] + local[1] * axes[1][j] + local[2] * axes[2][j];
}

inline void dirToLocal(const float v[3], const float axes[3][3], float out[3]) noexcept {
    for (int k = 0; k < 3; ++k) out[k] = axes[k][0] * v[0] + axes[k][1] * v[1] + axes[k][2] * v[2];
}

inline void dirToWorld(const float local[3], const float axes[3][3], float out[3]) noexcept {
    for (int j = 0; j < 3; ++j) out[j] = local[0] * axes[0][j] + local[1] * axes[1][j] + local[2] * axes[2][j];
}

// The cell's points (centre, capA, capB, boxCentre) and directions (boxU, boxV, boxW) written in the pad's frame. Every other field is copied.
inline void localizeCell(const OwnCell& world, const float centre[3], const float axes[3][3], OwnCell* local) noexcept {
    *local = world;
    pointToLocal(world.centre, centre, axes, local->centre);
    pointToLocal(world.capA, centre, axes, local->capA);
    pointToLocal(world.capB, centre, axes, local->capB);
    pointToLocal(world.boxCentre, centre, axes, local->boxCentre);
    dirToLocal(world.boxU, axes, local->boxU);
    dirToLocal(world.boxV, axes, local->boxV);
    dirToLocal(world.boxW, axes, local->boxW);
}

// The inverse: the local cell placed in the pad's CURRENT frame (its live centre and axes). Only the points and directions of *world are written;
// its other fields (the names, the reasons, the sizes) keep their values.
inline void worldizeCell(const OwnCell& local, const float centre[3], const float axes[3][3], OwnCell* world) noexcept {
    pointToWorld(local.centre, centre, axes, world->centre);
    pointToWorld(local.capA, centre, axes, world->capA);
    pointToWorld(local.capB, centre, axes, world->capB);
    pointToWorld(local.boxCentre, centre, axes, world->boxCentre);
    dirToWorld(local.boxU, axes, world->boxU);
    dirToWorld(local.boxV, axes, world->boxV);
    dirToWorld(local.boxW, axes, world->boxW);
}

// Built v13.7 (b): the pad's height along the shaft (up) relative to a static reference, the hangar shell record: the pad minus the shell, dotted
// with up. A frame shift moves both alike and cancels; only the pad's own motion changes it.
constexpr float kPadMoveReleaseM = 2.0f;   // the station releases when the pad moves more than this along the shaft against the shell

inline float shellRelative(const float pad[3], const float shell[3], const float up[3]) noexcept {
    return (pad[0] - shell[0]) * up[0] + (pad[1] - shell[1]) * up[1] + (pad[2] - shell[2]) * up[2];
}

// True when the pad has moved more than limitM along the shaft since the engage (the two relative heights differ).
inline bool padMovedAway(float engageRel, float nowRel, float limitM = kPadMoveReleaseM) noexcept {
    return std::isfinite(engageRel) && std::isfinite(nowRel) && std::fabs(nowRel - engageRel) > limitM;
}

// The name classes the bay keeps: substrings of the game's record names (the own hangar's parts at any size).
inline bool bayPartName(const char* name) noexcept {
    static const char* const kParts[] = {"Hanger", "Hangar", "Lobby", "BlastShield", "Skirt", "LandingPad", "Lift", "Gate"};
    if (!name || !name[0]) return false;
    for (const char* part : kParts) {
        if (std::strstr(name, part)) return true;
    }
    return false;
}

// The distance from p to the infinite line through o along the unit u; along (optional) receives the position of p's foot along u.
inline float lateralToAxis(const float p[3], const float o[3], const float u[3], float* along = nullptr) noexcept {
    const float d[3] = {p[0] - o[0], p[1] - o[1], p[2] - o[2]};
    const float a = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
    const float l[3] = {d[0] - a * u[0], d[1] - a * u[1], d[2] - a * u[2]};
    if (along) *along = a;
    return std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
}

// True when p lies in the bay volume of the shaft from o along u, the target at targetA metres from o, and below metres under o.
inline bool inBayVolume(const float p[3], const float o[3], const float u[3], float targetA, float below) noexcept {
    float a = 0.0f;
    const float lat = lateralToAxis(p, o, u, &a);
    return std::isfinite(lat) && std::isfinite(a) && lat <= kBayLateralM && a >= -below && a <= targetA + kBayPastM;
}

// Built v13.2 (a): diag.sealed_cell_below_m, a number of metres in [0, 200]; empty is the default. False for text that is not one.
inline bool parseBelowM(const char* text, float defaultM, float* out) noexcept {
    *out = defaultM;
    if (!text || !*text) return true;
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    if (end == text || !std::isfinite(v) || v < 0.0 || v > 200.0) return false;
    while (*end == ' ' || *end == '\t') ++end;
    if (*end != '\0') return false;
    *out = static_cast<float>(v);
    return true;
}

// Built v13.2 (b): the own ship's record name is the type prefix without its trailing underscore ("CobraMkV_" -> "CobraMkV").
inline bool strippedPrefix(const char* prefix, char* out, size_t cap) noexcept {
    out[0] = '\0';
    if (!prefix || !cap) return false;
    const size_t n = std::strlen(prefix);
    if (n < 2 || prefix[n - 1] != '_' || n >= cap) return false;
    std::memcpy(out, prefix, n - 1);
    out[n - 1] = '\0';
    return true;
}

// True when the record's name is exactly the stripped prefix (both non-empty).
inline bool ownRecordMatch(const char* name, const char* stripped) noexcept {
    return name && stripped && stripped[0] && std::strcmp(name, stripped) == 0;
}

// Built v13.2 (b): the own ship's per-frame keep: a record whose centre lies within the own record's CURRENT sphere plus marginM.
inline bool ownSphereKeeps(const float centre[3], const float ownCentre[3], float ownRadius, float marginM) noexcept {
    if (!std::isfinite(ownRadius) || !(ownRadius > 0.0f)) return false;
    return distance(centre, ownCentre) <= ownRadius + marginM;
}

// The unit vector from a to b (false when they are closer than a metre, or not finite).
inline bool unitBetween(const float a[3], const float b[3], float u[3]) noexcept {
    const float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!std::isfinite(len) || !(len >= 1.0f)) return false;
    for (int k = 0; k < 3; ++k) u[k] = d[k] / len;
    return true;
}

// The nearest BlastShieldBase* instance within kCapsuleMaxM of p (else the nearest own Hanger*Small/Medium/Large). Writes its centre and name.
inline bool pickCapsuleTarget(const ExplicitObs* obs, size_t n, const float p[3], float q[3], char* name, size_t cap) noexcept {
    for (int pass = 0; pass < 2; ++pass) {
        int best = -1;
        float bestD = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            const bool match = pass == 0 ? isBlastBase(obs[i].name) : isOwnHanger(obs[i].name);
            if (!match) continue;
            const float d = distance(obs[i].centre, p);
            if (d > kCapsuleMaxM) continue;
            if (best < 0 || d < bestD) {
                best = static_cast<int>(i);
                bestD = d;
            }
        }
        if (best >= 0) {
            for (int k = 0; k < 3; ++k) q[k] = obs[best].centre[k];
            std::snprintf(name, cap, "%s", obs[best].name);
            return true;
        }
    }
    return false;
}

// Built v11 (second addendum): the oriented box. Elite's pad sizes (the wiki), length x width in metres, small, medium, large. The hangar is the
// pad doubled along one axis (diag.sealed_cell_double), so each half-extent is half the pad's size times 2 on that axis and 1 on the other, plus
// the margin (diag.sealed_cell_margin_m). Vertically the box runs from the below distance (diag.sealed_cell_below_m) under the pad's centre up the shaft to kBoxBeyondM past the
// target (the nearest BlastShieldBase). A round cell cannot hold a rectangular bay without the neighbours: this is the bay's footprint.
constexpr float kPadWikiM[3][2] = {{66.0f, 55.0f}, {150.0f, 90.0f}, {213.0f, 128.0f}};

constexpr float kBoxBeyondM = 20.0f;

constexpr float kMarginDefaultM = 10.0f;   // v12.1: 10 m (was 5; a guess)

// The local axes of a rotation given as the builder's quaternion (R(q)): axes[k] is R(q) e_k, from builderCentre.
inline void quatAxes(const float q[4], float axes[3][3]) noexcept {
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float o0[4] = {};
    builderCentre(q, zero, zero, o0);
    for (int k = 0; k < 3; ++k) {
        float e[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        e[k] = 1.0f;
        float ok[4] = {};
        builderCentre(q, zero, e, ok);
        for (int j = 0; j < 3; ++j) axes[k][j] = ok[j] - o0[j];
    }
}

// The local axes of a store-style pose (the node block through rec+0x190; decomp_4312040.txt): with A the pose block (3x3 at A[4k+j], translation
// A[12..14]) and B the matrix at A+0x50 (B[4i+j], translation B[12..14]), the world image of local e_k is axes[k][j] = sum_i B[4i+j] * A[4k+i].
inline void storeAxes(const float A[15], const float B[16], float axes[3][3]) noexcept {
    for (int k = 0; k < 3; ++k)
        for (int j = 0; j < 3; ++j) {
            float s = 0.0f;
            for (int i = 0; i < 3; ++i) s += B[4 * i + j] * A[4 * k + i];
            axes[k][j] = s;
        }
}

// The footprint of the pad's box: of its three local axes, the one most along the shaft u is the up axis; of the other two, the one with the
// larger builder extent is the length axis (lenIdx), the other the width (widIdx). False with no extent: the box cannot tell length from width.
inline bool footprintAxes(const float axes[3][3], const float u[3], const float* ext, int* lenIdx, int* widIdx, int* upIdx) noexcept {
    if (!ext) return false;
    int up = 0;
    float best = -1.0f;
    for (int k = 0; k < 3; ++k) {
        const float d = std::fabs(axes[k][0] * u[0] + axes[k][1] * u[1] + axes[k][2] * u[2]);
        if (d > best) {
            best = d;
            up = k;
        }
    }
    if (upIdx) *upIdx = up;
    int a = -1, b = -1;
    for (int k = 0; k < 3; ++k) {
        if (k == up) continue;
        if (a < 0) a = k;
        else b = k;
    }
    if (ext[a] >= ext[b]) {
        *lenIdx = a;
        *widIdx = b;
    } else {
        *lenIdx = b;
        *widIdx = a;
    }
    return true;
}

// v12.1 (flight): the box's axis W is the unit vector from the pad's centre to the target (BlastShieldBase): the shaft is the direction the pad
// travels, not the pad's own e2 (the v12 flight: the pad rose along the pad-to-BlastShieldBase line). The pad is on the axis, so the box is
// centred laterally on it: from the below distance (diag.sealed_cell_below_m) under the pad's centre to kBoxBeyondM past the target along W. The footprint's length axis lenAxis is
// made orthogonal to W (Gram-Schmidt, length first); the width is W x length. False when the target is closer than 1 m or the axes are degenerate.
inline bool buildBox(const float pad[3], const float target[3], const float lenAxis[3], float halfLen, float halfWid, float margin,
                     float below, OwnCell* cell) noexcept {
    const float d[3] = {target[0] - pad[0], target[1] - pad[1], target[2] - pad[2]};
    const float proj = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!std::isfinite(proj) || !(proj >= 1.0f)) return false;
    float W[3] = {d[0] / proj, d[1] / proj, d[2] / proj};
    const float lateral = 0.0f;
    const float dp = lenAxis[0] * W[0] + lenAxis[1] * W[1] + lenAxis[2] * W[2];
    float U[3] = {lenAxis[0] - dp * W[0], lenAxis[1] - dp * W[1], lenAxis[2] - dp * W[2]};
    const float ul = std::sqrt(U[0] * U[0] + U[1] * U[1] + U[2] * U[2]);
    if (!std::isfinite(ul) || !(ul > 1e-3f)) return false;
    for (float& v : U) v /= ul;
    const float w[3] = {W[0], W[1], W[2]};
    const float V[3] = {w[1] * U[2] - w[2] * U[1], w[2] * U[0] - w[0] * U[2], w[0] * U[1] - w[1] * U[0]};
    const float lo = -below, hi = proj + kBoxBeyondM;
    const float along = (lo + hi) * 0.5f;
    const float half = (hi - lo) * 0.5f;
    float hx = halfLen + margin, hy = halfWid + margin;
    if (hx < 1.0f) hx = 1.0f;
    if (hy < 1.0f) hy = 1.0f;
    cell->box = true;
    cell->boxMargin = margin;
    cell->boxLateral = lateral;
    for (int k = 0; k < 3; ++k) {
        cell->boxCentre[k] = pad[k] + w[k] * along;
        cell->boxU[k] = U[k];
        cell->boxV[k] = V[k];
        cell->boxW[k] = w[k];
    }
    cell->boxHalf[0] = hx;
    cell->boxHalf[1] = hy;
    cell->boxHalf[2] = half;
    return true;
}

// A record's position in the box: across the length (U), across the width (V), up the shaft (W), metres from the box's centre.
inline void boxCoords(const float c[3], const OwnCell& cell, float out[3]) noexcept {
    const float d[3] = {c[0] - cell.boxCentre[0], c[1] - cell.boxCentre[1], c[2] - cell.boxCentre[2]};
    out[0] = d[0] * cell.boxU[0] + d[1] * cell.boxU[1] + d[2] * cell.boxU[2];
    out[1] = d[0] * cell.boxV[0] + d[1] * cell.boxV[1] + d[2] * cell.boxV[2];
    out[2] = d[0] * cell.boxW[0] + d[1] * cell.boxW[1] + d[2] * cell.boxW[2];
}

// Whether a centre is inside the cell's box (its centre, the three half-extents).
inline bool inBox(const float c[3], const OwnCell& cell) noexcept {
    float o[3];
    boxCoords(c, cell, o);
    return std::fabs(o[0]) <= cell.boxHalf[0] && std::fabs(o[1]) <= cell.boxHalf[1] && std::fabs(o[2]) <= cell.boxHalf[2];
}

// Built v12: the off-screen emulation's steps, in Elite's order (decomp_431AFE0.txt lines 99 to 102; bytes 0x431B221..0x431B240). Kind 1: the byte
// store [coll+0x629] = 0. Kind 2: the call of 0x434DD50 with rcx = coll+0x300 (the owner count reset). Kind 3: the call of 0x42B5760 with rcx = the
// pipe, only when the pipe is non-null (it adds 0x78 and jumps to 0x434DD50: pipe+0x31C). Returns the number of steps written to kinds and args.
inline int offScreenSteps(uintptr_t coll, uintptr_t pipe, int* kinds, uintptr_t* args) noexcept {
    int n = 0;
    kinds[n] = 1;
    args[n] = coll + 0x629;
    ++n;
    kinds[n] = 2;
    args[n] = coll + 0x300;
    ++n;
    if (pipe) {
        kinds[n] = 3;
        args[n] = pipe;
        ++n;
    }
    return n;
}

// Built v12: the early rig veto's rule (pure). A rig's bound is the box of its records' last-frame world spheres; its bounding sphere has the box
// centre and the half-diagonal as radius (conservative). The cell in force is the named box, the capsule or the pad's sphere; mode all has no cell
// and vetoes every bounded rig. A rig is OUTSIDE when its bounding sphere lies wholly outside the cell. A rig that is outside is vetoed, except every
// kRigRefreshN-th time, when it is let through so its records are re-judged (vetoedRun counts the vetoes since that).
constexpr uint32_t kRigRefreshN = 30;

enum class RigVerdict : uint8_t { kUnknown = 0, kMixed = 1, kOutside = 2, kRefresh = 3 };

// The distance from a point to the cell (0 inside it): the box, the capsule's segment less its radius, or the sphere less its radius.
inline float distanceToCell(const float c[3], const OwnCell& cell) noexcept {
    if (cell.box) {
        float o[3];
        boxCoords(c, cell, o);
        const float dx = std::max(std::fabs(o[0]) - cell.boxHalf[0], 0.0f);
        const float dy = std::max(std::fabs(o[1]) - cell.boxHalf[1], 0.0f);
        const float dz = std::max(std::fabs(o[2]) - cell.boxHalf[2], 0.0f);
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    if (cell.capsule) return std::max(distToSegment(c, cell.capA, cell.capB) - cell.radius, 0.0f);
    return std::max(distance(c, cell.centre) - cell.radius, 0.0f);
}

// The verdict for one rig. bounded: the rig has a bound from its records. c and r: the bounding sphere's centre and radius. hasCell: a named cell is
// in force (mode all does not need one). vetoedRun: the vetoes since the rig was last let through.
inline RigVerdict rigVerdict(bool bounded, const float c[3], float r, bool hasCell, const OwnCell& cell, bool allMode, uint32_t vetoedRun) noexcept {
    if (!bounded) return RigVerdict::kUnknown;
    bool outside;
    if (allMode) {
        outside = true;
    } else {
        if (!hasCell) return RigVerdict::kUnknown;
        outside = distanceToCell(c, cell) > r;
    }
    if (!outside) return RigVerdict::kMixed;
    if (vetoedRun >= kRigRefreshN) return RigVerdict::kRefresh;
    return RigVerdict::kOutside;
}

// Built v13 (fix 1): the own ship is never culled in sphere or named mode (only all culls it). Exempt: a record or rig whose sphere contains the
// camera with a radius under kOwnShipRadiusM, or whose name starts with the own ship's type prefix (taken at engage).
constexpr float kOwnShipRadiusM = 40.0f;

// The own ship's type prefix from its name: everything up to and including the first underscore ("CobraMkV_01" -> "CobraMkV_"). Empty when the
// name has no underscore.
inline void ownPrefixOf(const char* name, char* out, size_t cap) noexcept {
    out[0] = '\0';
    if (!name || !cap) return;
    const char* u = std::strchr(name, '_');
    if (!u) return;
    const size_t n = static_cast<size_t>(u - name) + 1;
    if (n >= cap) return;
    std::memcpy(out, name, n);
    out[n] = '\0';
}

// Whether a record (or rig) is the own ship: the camera inside a small sphere, or the type prefix matches the name.
inline bool ownShipExempt(bool containsCamera, float radius, const char* name, const char* prefix) noexcept {
    if (containsCamera && radius < kOwnShipRadiusM) return true;
    if (prefix && prefix[0] && name && std::strncmp(name, prefix, std::strlen(prefix)) == 0) return true;
    return false;
}

// Built v13 (fix 3): the shaft's target is the instance of the given name class nearest the LINE through the pad's centre along the pad's up
// axis e2 (perpendicular distance; ties by the distance along the axis). Only instances above the pad (along > 0) and within kCapsuleMaxM of it.
// Writes the target's centre and name and its perpendicular and along distances. False when there is none.
inline bool pickTargetOnLine(const ExplicitObs* obs, size_t n, const float pad[3], const float e2[3], int passClass, float q[3], char* name,
                             size_t cap, float* perp, float* along) noexcept {
    int best = -1;
    float bestPerp = 0.0f, bestAlong = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const bool match = passClass == 0 ? isBlastBase(obs[i].name) : isOwnHanger(obs[i].name);
        if (!match) continue;
        const float v[3] = {obs[i].centre[0] - pad[0], obs[i].centre[1] - pad[1], obs[i].centre[2] - pad[2]};
        const float a = v[0] * e2[0] + v[1] * e2[1] + v[2] * e2[2];
        if (!(a > 0.0f)) continue;
        const float d = distance(obs[i].centre, pad);
        if (d > kCapsuleMaxM) continue;
        const float pd = std::sqrt(std::fmax(d * d - a * a, 0.0f));
        if (best < 0 || pd < bestPerp || (pd == bestPerp && a < bestAlong)) {
            best = static_cast<int>(i);
            bestPerp = pd;
            bestAlong = a;
        }
    }
    if (best < 0) return false;
    for (int k = 0; k < 3; ++k) q[k] = obs[best].centre[k];
    std::snprintf(name, cap, "%s", obs[best].name);
    *perp = bestPerp;
    *along = bestAlong;
    return true;
}


// Built v12.1: the precomputed verdict's code, one byte per rig (the frame tick writes it, the decision reads it): 2 outside, 1 mixed, 0 unknown or
// a refresh (the decision decides the refresh from its own run count).
inline uint8_t verdictCode(RigVerdict v) noexcept {
    return v == RigVerdict::kOutside ? uint8_t{2} : (v == RigVerdict::kMixed ? uint8_t{1} : uint8_t{0});
}

// Built v11 (addendum): with a capsule, the explicit picks are the nearest instance of each part node to the capsule's AXIS, and only within
// kExplicitCapsuleM of it. They backstop the own hangar's parts and nothing further: a neighbouring pad's part is farther than that from this
// axis, so it is not picked. Same shape as pickExplicit.
inline size_t pickExplicitSegment(const ExplicitObs* obs, size_t n, const float a[3], const float b[3], uintptr_t* out, size_t outCap) noexcept {
    struct Best {
        uintptr_t node;
        uintptr_t addr;
        float d;
    };
    Best best[kExplicitNodes];
    size_t nb = 0;
    for (size_t i = 0; i < n; ++i) {
        const float d = distToSegment(obs[i].centre, a, b);
        if (d > kExplicitCapsuleM) continue;
        size_t k = 0;
        while (k < nb && best[k].node != obs[i].node) ++k;
        if (k < nb) {
            if (d < best[k].d) {
                best[k].addr = obs[i].addr;
                best[k].d = d;
            }
        } else if (nb < kExplicitNodes) {
            best[nb++] = Best{obs[i].node, obs[i].addr, d};
        }
    }
    size_t written = 0;
    for (size_t k = 0; k < nb && written < outCap; ++k) out[written++] = best[k].addr;
    return written;
}

// Why a record is kept in named mode.
enum class KeepWhy : uint8_t { kNone = 0, kCell = 1, kExplicit = 2, kSphere = 3 };

struct NamedKeep {
    bool keep = false;
    KeepWhy why = KeepWhy::kNone;
    bool containing = false;      // the record's sphere contains the camera
    bool sphereJudged = false;    // the sphere rule alone would keep it (so a cell keep is "by cell" only when this is false)
};

inline bool inAddrSet(const uintptr_t* set, size_t n, uintptr_t addr) noexcept {
    for (size_t i = 0; i < n; ++i)
        if (set[i] == addr) return true;
    return false;
}

// The named rule for one record (Built v8.3b). No own cell: the sphere rule for the frame. With one: kept when the record's centre lies within
// the cell radius of the cell centre (why cell), or when it is an explicit part's nearest instance (why explicit). Otherwise culled, including a
// record whose sphere contains the camera.
inline NamedKeep keepNamedCell(const float centre[3], float radius, const float camera[3], float R, const OwnCell& cell,
                               const uintptr_t* explicitSet, size_t explicitN, uintptr_t addr) noexcept {
    NamedKeep v;
    v.containing = containsCamera(centre, radius, camera);
    v.sphereJudged = keepRecord(centre, radius, camera, R);
    if (!cell.has) {
        v.keep = v.sphereJudged;
        v.why = v.keep ? KeepWhy::kSphere : KeepWhy::kNone;
        return v;
    }
    // The box when one is in force (Built v11, second addendum), else the capsule, else the pad's sphere.
    const bool inCell = cell.box ? inBox(centre, cell)
                        : cell.capsule ? distToSegment(centre, cell.capA, cell.capB) <= cell.radius
                                       : distance(centre, cell.centre) <= cell.radius;
    if (inCell) {
        v.keep = true;
        v.why = KeepWhy::kCell;
        return v;
    }
    if (inAddrSet(explicitSet, explicitN, addr)) {
        v.keep = true;
        v.why = KeepWhy::kExplicit;
    }
    return v;
}

// The class cache's slot (lock-free): node>>3 in bits 0..43, the class in bits 44..46 (never 0 for a stored slot), the frame's stamp in bits
// 47..63. A zero word is an empty slot. A node that is not 8-aligned or is out of the 47-bit user range is not cached.
// Built v14.29: the slot also carries the node's flags (what its name is good for) in bits 47..49, and the key in bits 50..63 (14 bits). Built v14.31:
// the key is the lifecycle generation alone: a node's name does not change while the node lives, so it is read once per node per generation, and a
// lifecycle point (undock, hangar change, ship change, setting toggle) starts every node afresh.
constexpr uint64_t kSlotNodeMask = (uint64_t(1) << 44) - 1;

constexpr uint32_t kSlotKeyMask = (1u << 14) - 1;

constexpr uint32_t kNodeMechanism = 1;    // the name is a bay mechanism record (Lift, BlastShield, HangarGate ...): a latch candidate
constexpr uint32_t kNodeHangarLike = 2;   // the name could name the hangar (Hanger, Pad, Lift, Bay ...): the names note wants it
constexpr uint32_t kNodeShipPrefix = 4;   // the name has a type prefix up to an underscore (the ship tally counts it)

inline bool slotFits(uintptr_t node) noexcept { return (node & 7u) == 0 && (uint64_t(node) >> 3) <= kSlotNodeMask; }

// The key a lifecycle generation gives.
inline uint32_t classKey(uint32_t generation) noexcept { return generation & kSlotKeyMask; }

inline uint64_t packSlot(uintptr_t node, NameClass cls, uint32_t flags, uint32_t key) noexcept {
    return (uint64_t(node) >> 3) | (uint64_t(static_cast<uint8_t>(cls)) << 44) | (uint64_t(flags & 7u) << 47) | (uint64_t(key & kSlotKeyMask) << 50);
}

// True when the slot holds this node's class and flags under this key; they are written to *cls and *flags.
inline bool unpackSlot(uint64_t v, uintptr_t node, uint32_t key, NameClass* cls, uint32_t* flags) noexcept {
    if (v == 0) return false;
    if ((v & kSlotNodeMask) != (uint64_t(node) >> 3)) return false;
    if (((v >> 50) & kSlotKeyMask) != (key & kSlotKeyMask)) return false;
    const uint32_t c = static_cast<uint32_t>((v >> 44) & 7u);
    if (c == 0) return false;
    *cls = static_cast<NameClass>(c);
    *flags = static_cast<uint32_t>((v >> 47) & 7u);
    return true;
}

// Built v10: a record's world sphere centre the way the store computes it for the records producer B appends (decomp_4312040.txt, lines
// 212-262; the derivation is in docs\concourse-culling-2026-10-11.md, Built v10). L is the node's bounds centre (*(node+8)+0x20, xyz), A the
// record's pose block (*(rec+0x190): 15 floats), B the matrix at *(A+0x50) (16 floats). With u_j = sum_k A[4k+j] L_k + A[12+j] and
// v_j = sum_i B[4i+j] u_i + B[12+j], the centre is v. The radius is the node block's +0x2C, unscaled. False when the result is not finite.
inline bool storeWorldSphere(const float L[3], const float A[15], const float B[16], float out[3]) noexcept {
    float u[3];
    for (int j = 0; j < 3; ++j) {
        u[j] = A[12 + j];
        for (int k = 0; k < 3; ++k) u[j] += A[4 * k + j] * L[k];
    }
    for (int j = 0; j < 3; ++j) {
        out[j] = B[12 + j];
        for (int i = 0; i < 3; ++i) out[j] += B[4 * i + j] * u[i];
    }
    return finite3(out);
}

// The cull of one record, with its fault handling (sealed_trial.cpp; the rig tests it against a fake memory). The record's view mask and its
// admitted flag are read first and written only after both are read: a failed read touches nothing, a failed mask write touches nothing,
// and a failed admitted write puts the mask back, so a fault always leaves the record as the store wrote it (kept). A failed put-back is
// reported as torn: the one case where the record may be left half culled, counted and named.
enum class Cull : uint8_t { kCulled = 0, kUntouched = 1, kRestored = 2, kTorn = 3 };

enum class CullField : uint8_t { kMaskRead = 0, kAdmittedRead = 1, kMaskWrite = 2, kAdmittedWrite = 3, kRestoreWrite = 4 };

template <typename Mem>
inline Cull cullRecordFaultSafe(Mem& m, CullField* failed, uint64_t* value) noexcept {
    uint64_t oldMask = 0;
    uint8_t oldAdmitted = 0;
    if (!m.readMask(&oldMask)) {
        *failed = CullField::kMaskRead;
        *value = 0;
        return Cull::kUntouched;
    }
    if (!m.readAdmitted(&oldAdmitted)) {
        *failed = CullField::kAdmittedRead;
        *value = oldMask;
        return Cull::kUntouched;
    }
    if (!m.writeMask(0)) {
        *failed = CullField::kMaskWrite;
        *value = oldMask;
        return Cull::kUntouched;
    }
    if (!m.writeAdmitted(0)) {
        *failed = CullField::kAdmittedWrite;
        *value = oldAdmitted;
        if (m.writeMask(oldMask)) return Cull::kRestored;
        *failed = CullField::kRestoreWrite;
        *value = oldMask;
        return Cull::kTorn;
    }
    return Cull::kCulled;
}

// ---- The seal's trigger: the game's pad state (built v14.23, 2026-10-13). Pure: the engine feeds the inputs each tick and acts on the result. -------
// The station seals when the game says the own pad is in the hangar: r1 = 1 on the own pad object, read fresh. The own object is the one the
// docking reader r0 was last called on this dock (GameTrack). No geometry decides the seal: no height, no stillness clock, no descent observation,
// no fallback. The shell-relative height (kLoweredM) is kept only for the sun's "pad raised" test.
constexpr float kLoweredM = -10.0f;
constexpr float kBayWatchMinM = 5.0f;
constexpr float kBayPosM = 0.03f;     // the latch's motion limits (poseMoved)
constexpr float kBayRotDeg = 0.5f;
constexpr uint64_t kBayStillMs = 2000;   // the latch's movers window (the records that moved lately): not a seal clock any more
// The camera has left the held box by more than this: the seal is released (riding the elevator down, an external camera flying out).
constexpr float kCameraLeaveM = 2.0f;
// The camera counts in both directions over this many consecutive frames: it must be outside the held box for this long to release, and inside the
// bay for this long to re-seal. Explorer Cam is no condition.
constexpr uint32_t kCameraFrames = 10;
// Built v14.24: a mechanism release (the Lift, a hatch) is not a launch unless the bay stays away. The latched records must be back within these of
// their sealed poses for kPoseBackFrames consecutive frames (with r1 1) before the seal may come back without the r1 0 to 1 edge.
constexpr float kPoseTolM = 0.25f;
constexpr float kPoseTolDeg = 1.0f;
constexpr uint32_t kPoseBackFrames = 30;
// The game's pad state is fresh when its last call on the own object is within this many milliseconds.
constexpr uint64_t kPadStateFreshMs = 1000;
// The reader codes (r0: 0 Ready for landing, 1 Inside_Hangar, 2 Outside_Hangar, 3 other; r1: 1 Inside_Hangar and settled).
constexpr uint8_t kReaderInHangar = 1;
constexpr uint8_t kReaderMoving = 1;    // r0 1: moving in or locked in
constexpr uint8_t kReaderDescent = 3;   // r0 3: the descent has started

enum class SealAction : uint8_t { kNone = 0, kRequestEngage = 1, kRelease = 2 };
enum class ReleaseWhy : uint8_t { kNone = 0, kSettingOff, kUndocked, kCameraLeft, kPadMoved, kHangarChanged, kLaunch, kMechanism, kInHangarLeft };

// The own pad's game state this tick (the caller fills it from the probe and GameTrack): freshness is decided by the caller in milliseconds.
struct SealInputs {
    bool setting = false;        // fix.optimized_station is on
    bool docked = false;         // Status.json Flags bit 0
    bool ownKnown = false;       // the game has named the own pad this dock (r0 was called on it)
    bool r1Fresh = false;        // the own pad's r1 was called within kPadStateFreshMs
    bool inHangar = false;       // the own pad's r1 is 1 (meaningful only with r1Fresh)
    bool pickOk = false;         // the pad pick and its cell are valid this frame
    bool padHeld = false;        // the own pad is absent this frame, held for fewer than kHeldLostFrames: the trigger keeps its state
    bool cameraInBox = false;    // the camera is inside the candidate box (the seal needs the camera in the bay)
    bool held = false;           // sealed now
    bool cameraOutside = false;  // the camera is outside the held box by more than kCameraLeaveM (only meaningful while held)
    uintptr_t pad = 0;           // the own pad's record (a change withdraws the request)
    bool mechanismMoved = false; // a latched bay mechanism record moved on two frames in a row (the launch)
    bool poseBack = false;       // built v14.24: every latched record is within kPoseTolM and kPoseTolDeg of its pose at the seal this frame
    bool padMoved = false;       // the pad moved more than kPadMoveReleaseM along the shaft relative to the hangar shell (ShellMotion)
    bool hangarChanged = false;  // the held pad is gone from the store, or another own pad is the nearest for kHeldLostFrames
    bool launched = false;       // not used (no reader for the launch): always false
    uint64_t nowMs = 0;
};

struct SealState {
    bool requested = false;      // one engage per seal (cleared by a release or the trigger lapsing)
    uintptr_t padAddr = 0;       // the pad the request is for (a change withdraws it)
    // Consecutive frames the camera has been outside the held box (camOutRun) or inside the bay (camInRun; starts satisfied).
    uint32_t camOutRun = 0;
    uint32_t camInRun = kCameraFrames;
    // The re-seal after a release by the mechanism, the pad moving, or the pad leaving the bay: 0 free, 1 wait for the pad to leave the bay
    // (r1 0), 2 wait for it to come back (r1 1 after a 0). Camera, ship and setting releases leave it at 0.
    uint8_t reseal = 0;
    // Built v14.24: the release was by the mechanism (the latch), and the frames in a row with every latched record back at its sealed pose.
    // Those frames re-seal without the r1 edge (kPoseBackFrames), when r1 is 1 and fresh and the camera rule holds.
    bool mechanism = false;
    uint32_t poseRun = 0;
};

// A release, or the trigger lapsing: the request starts again.
inline void clearTrigger(SealState* st) noexcept {
    st->requested = false;
}

// The own pad's game state is fresh for the trigger when its last call was within kPadStateFreshMs.
inline bool gameFresh(uint64_t ageMs) noexcept { return ageMs <= kPadStateFreshMs; }

// The own pad, from the game (built v14.23): the object r0 was called on most recently this dock. Reset at undock and at a hangar change: only an r0
// call after the reset (its QPC time is later) names an own pad. A different object on r0 switches the own pad. The caller logs the event once.
enum class GameTrackEvent : uint8_t { kNone = 0, kFirst, kSwitched };
struct GameTrack {
    bool have = false;
    uintptr_t own = 0;
    uint64_t resetQpc = 0;
    GameTrackEvent observe(bool haveR0, uintptr_t obj, uint64_t r0Qpc) noexcept {
        if (!haveR0 || obj == 0 || r0Qpc <= resetQpc) return GameTrackEvent::kNone;
        if (have && obj == own) return GameTrackEvent::kNone;
        const GameTrackEvent e = have ? GameTrackEvent::kSwitched : GameTrackEvent::kFirst;
        have = true;
        own = obj;
        return e;
    }
    void reset(uint64_t qpc) noexcept {
        have = false;
        own = 0;
        resetQpc = qpc;
    }
};

// Built v14.17 (instrument): the pad in the hangar shell's frame. The shaft is the pad's up axis, the one shellRelative measures along, so z is the
// rel reading. x and y lie across the shaft: x along the shell's forward axis (its pose axes[0] with the shaft removed), y across both. lateral is the
// distance from the shell's shaft line. yaw is the pad's forward axis against the shell's, about the shaft, in degrees.
struct ShellFrame {
    bool ok = false;      // position fields valid
    bool yawOk = false;   // yawDeg valid (the pad's forward axis is not along the shaft)
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float lateral = 0.0f;
    float yawDeg = 0.0f;
};

inline ShellFrame shellFrameOf(const float pad[3], const float shell[3], const float up[3], const float shellFwd[3], const float padFwd[3]) noexcept {
    ShellFrame f;
    const float z = shellRelative(pad, shell, up);
    const float d[3] = {pad[0] - shell[0], pad[1] - shell[1], pad[2] - shell[2]};
    const float h[3] = {d[0] - z * up[0], d[1] - z * up[1], d[2] - z * up[2]};   // across the shaft
    const float sd = shellFwd[0] * up[0] + shellFwd[1] * up[1] + shellFwd[2] * up[2];
    float xa[3] = {shellFwd[0] - sd * up[0], shellFwd[1] - sd * up[1], shellFwd[2] - sd * up[2]};
    const float xl = std::sqrt(xa[0] * xa[0] + xa[1] * xa[1] + xa[2] * xa[2]);
    if (!(xl > 1e-4f)) return f;   // the shell's forward axis lies along the shaft: no horizontal frame
    for (int k = 0; k < 3; ++k) xa[k] /= xl;
    const float ya[3] = {up[1] * xa[2] - up[2] * xa[1], up[2] * xa[0] - up[0] * xa[2], up[0] * xa[1] - up[1] * xa[0]};   // up x xa
    f.ok = true;
    f.z = z;
    f.x = h[0] * xa[0] + h[1] * xa[1] + h[2] * xa[2];
    f.y = h[0] * ya[0] + h[1] * ya[1] + h[2] * ya[2];
    f.lateral = std::sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
    const float pd = padFwd[0] * up[0] + padFwd[1] * up[1] + padFwd[2] * up[2];
    float pa[3] = {padFwd[0] - pd * up[0], padFwd[1] - pd * up[1], padFwd[2] - pd * up[2]};
    const float pl = std::sqrt(pa[0] * pa[0] + pa[1] * pa[1] + pa[2] * pa[2]);
    if (pl > 1e-4f) {
        for (int k = 0; k < 3; ++k) pa[k] /= pl;
        const float c = xa[0] * pa[0] + xa[1] * pa[1] + xa[2] * pa[2];
        const float cr[3] = {xa[1] * pa[2] - xa[2] * pa[1], xa[2] * pa[0] - xa[0] * pa[2], xa[0] * pa[1] - xa[1] * pa[0]};   // xa x pa
        const float s = cr[0] * up[0] + cr[1] * up[1] + cr[2] * up[2];
        f.yawDeg = std::atan2(s, c) * 57.29577951f;
        f.yawOk = true;
    }
    return f;
}

// ---- Built v14.20 (2026-10-13, the review of 670864ca): the pure parts of the P2 fixes ----------------------------------------------------

// Review 5: a bounded probe. Walks at most maxProbes slots from home. A matching slot is a hit; a stale or free slot is the insertion point and the end
// of a lookup. Exhausting the bound is a miss for a lookup and a skipped insert for a put: never a walk of the whole table.
constexpr uint32_t kTableProbeMax = 32;
struct ProbeResult {
    size_t index = 0;
    uint32_t probes = 0;     // slots examined
    bool found = false;
    bool empty = false;      // stopped at a stale or free slot
    bool exhausted = false;  // the bound was reached first
};
template <typename Matches, typename Stale>
inline ProbeResult boundedProbe(size_t home, size_t cap, uint32_t maxProbes, Matches matches, Stale stale) noexcept {
    ProbeResult r;
    if (cap == 0) {
        r.exhausted = true;
        return r;
    }
    const uint32_t limit = maxProbes < cap ? maxProbes : static_cast<uint32_t>(cap);
    size_t i = home % cap;
    for (uint32_t n = 1; n <= limit; ++n) {
        r.probes = n;
        r.index = i;
        if (matches(i)) {
            r.found = true;
            return r;
        }
        if (stale(i)) {
            r.empty = true;
            return r;
        }
        i = (i + 1) % cap;
    }
    r.exhausted = true;
    return r;
}

// Review 1: a pending engage is bound to the pad and the shell it was requested for. A zero pad is never bound.
struct EngageRequest {
    bool pending = false;
    uintptr_t pad = 0;
    uintptr_t shell = 0;
};

// The trigger withdrew its request (the pad rose, the camera left, the pad changed, the trigger lapsed): the pending engage is cancelled.
inline void engageTrigger(EngageRequest* r, const SealState& st) noexcept {
    if (!st.requested) r->pending = false;
}

// A pending engage may freeze a cell only while the trigger still asks for the seal (the request in force), for the same pad and shell, with a valid cell
// for that identity and the camera in the bay (built v14.23: the game's pad state decided the request; no height is read here).
inline bool engageAdmitted(const EngageRequest& r, const SealState& st, uintptr_t pad, uintptr_t shell, bool cellValid) noexcept {
    return r.pending && st.requested && pad != 0 && r.pad == pad && r.shell == shell && st.camInRun >= kCameraFrames && cellValid;
}

// Review 2: the mechanism latch's identity, the own pad and its shell. step() reports a genuine change (another pad record, or another shell record
// while the shell is read) so the latch is cleared and regathered. A sample that misses the pad keeps the identity: a brief miss is not a change.
struct LatchIdentity {
    bool have = false;
    uintptr_t pad = 0;
    uintptr_t shell = 0;
    void bind(uintptr_t p, uintptr_t s) noexcept {
        have = true;
        pad = p;
        shell = s;
    }
    bool step(bool padOk, uintptr_t padAddr, bool shellOk, uintptr_t shellAddr) noexcept {
        if (!padOk) return false;
        if (!have) {
            bind(padAddr, shellOk ? shellAddr : 0);
            return false;
        }
        if (padAddr != pad) {
            bind(padAddr, shellOk ? shellAddr : shell);
            return true;
        }
        if (shellOk && shellAddr != shell) {
            shell = shellAddr;
            return true;
        }
        return false;
    }
};

// Review 4: a rig's cached verdict may skip its body only when it belongs to the current lifecycle epoch and a bound was read in that epoch. Anything
// else forwards: a reused address, a new rig, a changed collection, an absence.
inline bool rigMayVeto(uint32_t slotEpoch, uint32_t epoch, bool bounded, uint8_t verdict) noexcept {
    return slotEpoch == epoch && bounded && verdict == static_cast<uint8_t>(RigVerdict::kOutside);
}

// Review 7c: the view's centre is ready when the origin the station prefers is read, or the explorer cam's rows are (the only case the rows are
// preferred). A missing eye pose alone is not a reason to resolve the view again.
inline bool viewCentreReady(bool originOk, bool poseOk, bool explorerCam) noexcept {
    return originOk || (poseOk && explorerCam);
}

// ---- Built v14.24 (2026-10-13, the review of the 013c6122 flight): the rig association per rig, the mechanism pose, the cost summary, the log gate ----

// A change of one rig's collection forgets that rig's bounds and verdict, and no other rig's (review 4, per rig). The global epoch is for the
// lifecycle events only (undock, hangar change, ship change, setting toggle).
inline void rigForgetBounds(bool* curValid, bool* lastValid, uint8_t* verdict, uint32_t* vetoedRun) noexcept {
    *curValid = false;
    *lastValid = false;
    *verdict = 0;
    *vetoedRun = 0;
}

// A table entry names a rig whose collection is still the one it was filed under (a stale entry names a rig whose collection changed).
inline bool collAssociationIs(uintptr_t rigColl, uintptr_t key) noexcept { return rigColl != 0 && rigColl == key; }

// The angle in degrees between two unit axes (clamped; a zero axis reads 0).
inline float axisAngleBetweenDeg(const float a[3], const float b[3]) noexcept {
    const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    const float c = d > 1.0f ? 1.0f : (d < -1.0f ? -1.0f : d);
    return std::acos(c) * 57.29577951f;
}

// A latched record's drift against its sealed pose: the centre's offset (m) and the larger of its two axis angles (deg).
inline void latchPoseDelta(const float relA[3], const float relB[3], const float a0A[3], const float a0B[3], const float a2A[3], const float a2B[3],
                           float* dM, float* dDeg) noexcept {
    const float dx = relA[0] - relB[0];
    const float dy = relA[1] - relB[1];
    const float dz = relA[2] - relB[2];
    *dM = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float g0 = axisAngleBetweenDeg(a0A, a0B);
    const float g2 = axisAngleBetweenDeg(a2A, a2B);
    *dDeg = g0 > g2 ? g0 : g2;
}

// Within the pose tolerance (kPoseTolM, kPoseTolDeg).
inline bool latchPoseWithin(float dM, float dDeg) noexcept { return dM <= kPoseTolM && dDeg <= kPoseTolDeg; }

// The p50, the p95 and the max of n samples (sorted in place). None: zeros.
inline void samplePercentiles(double* v, size_t n, double* p50, double* p95, double* maxV) noexcept {
    if (n == 0) {
        *p50 = 0.0;
        *p95 = 0.0;
        *maxV = 0.0;
        return;
    }
    std::sort(v, v + n);
    *p50 = v[(n - 1) * 50 / 100];
    *p95 = v[(n - 1) * 95 / 100];
    *maxV = v[n - 1];
}

// A log gate: a line is written only when its key changes. Texts that carry a counter that changes every frame must not be logged per frame.
struct ChangeGate {
    bool have = false;
    uint64_t key = 0;
    bool changed(uint64_t k) noexcept {
        if (have && k == key) return false;
        have = true;
        key = k;
        return true;
    }
    void reset() noexcept { have = false; }
};

// FNV-1a of a text: a key for ChangeGate.
inline uint64_t textKey(const char* s) noexcept {
    uint64_t h = 1469598103934665603ull;
    for (const char* p = s; p != nullptr && *p != '\0'; ++p) {
        h ^= static_cast<uint8_t>(*p);
        h *= 1099511628211ull;
    }
    return h;
}

// Built v14.18 (2026-10-13, the store hook's identity gate): the admission store's sixteen prologue bytes in build 332841. store_hook.cpp detours
// FUN_144312040 by them, and the identity check comes first.
inline constexpr uint8_t kStorePrologue[16] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x68, 0xFE, 0xFF};

// The store hook's install decision (store_hook.cpp installOnce). The identity (the PE TimeDateStamp and SizeOfImage the other build-keyed hooks
// check) is read first; a mismatch refuses whatever the prologue is. identityRead false: the PE header was unreadable. got null: the prologue was
// unreadable. The caller passes the expected pair from the build's constants (explorer_cam_core.h's ecm::kExpectedTimestamp and ecm::kExpectedImageSize).
enum class StoreInstall : uint8_t { kInstall = 0, kUnreadable, kWrongIdentity, kWrongPrologue };
inline StoreInstall storeInstallDecision(bool identityRead, uint32_t stamp, uint32_t size, uint32_t wantStamp, uint32_t wantSize,
                                         const uint8_t* got, const uint8_t* pin, size_t n) noexcept {
    if (!identityRead || got == nullptr || pin == nullptr) return StoreInstall::kUnreadable;
    if (stamp != wantStamp || size != wantSize) return StoreInstall::kWrongIdentity;
    if (std::memcmp(got, pin, n) != 0) return StoreInstall::kWrongPrologue;
    return StoreInstall::kInstall;
}

// Built v14.8: a prefix names a ship only with kShipRecordsMin records within kShipRecordsM of the camera, and never GUI_ or BoardingVolume prefixes.
inline bool shipPrefixNamesShip(const char* prefix, uint32_t records) noexcept {
    if (prefix == nullptr || prefix[0] == '\0') return false;
    if (records < kShipRecordsMin) return false;
    if (hasPrefixNoCase(prefix, "GUI_")) return false;
    if (containsNoCase(prefix, "BoardingVolume")) return false;
    return true;
}

// Built v14.6: the own ship's type prefix, read from the candidate record each tick. A change of prefix is a ship change, but only after
// kShipChangeTicks readings in a row of the new prefix: a flicker between two near records is not a swap. The first reading only sets the prefix.
constexpr uint32_t kShipChangeTicks = 10;

struct ShipWatch {
    char prefix[32] = {};
    char pending[32] = {};
    uint32_t run = 0;
    // Returns true when the ship changed (the prefix is now the new one). A missing reading (empty) is no reading.
    bool step(const char* cur) noexcept {
        if (cur == nullptr || cur[0] == '\0') return false;
        if (prefix[0] == '\0') {
            std::snprintf(prefix, sizeof(prefix), "%s", cur);
            run = 0;
            return false;
        }
        if (std::strcmp(cur, prefix) == 0) {
            run = 0;
            pending[0] = '\0';
            return false;
        }
        if (std::strcmp(cur, pending) != 0) {
            std::snprintf(pending, sizeof(pending), "%s", cur);
            run = 0;
        }
        if (++run < kShipChangeTicks) return false;
        std::snprintf(prefix, sizeof(prefix), "%s", cur);
        run = 0;
        pending[0] = '\0';
        return true;
    }
};

// Built v14.10: the angle between two unit axes, degrees.
inline float axisAngleDeg(const float a[3], const float b[3]) noexcept {
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    if (d > 1.0f) d = 1.0f;
    if (d < -1.0f) d = -1.0f;
    return std::acos(d) * 57.29578f;
}

// Built v14.10: a watched record moved between two frames: its centre relative to the shell moved more than kBayPosM, or an axis rotated more than
// kBayRotDeg. Relative centres, so a frame shift of the record and the shell together is not motion.
inline bool poseMoved(const float prevRel[3], const float rel[3], const float prevA0[3], const float a0[3], const float prevA2[3],
                      const float a2[3]) noexcept {
    const float dx = rel[0] - prevRel[0];
    const float dy = rel[1] - prevRel[1];
    const float dz = rel[2] - prevRel[2];
    if (std::sqrt(dx * dx + dy * dy + dz * dz) > kBayPosM) return true;
    return axisAngleDeg(prevA0, a0) > kBayRotDeg || axisAngleDeg(prevA2, a2) > kBayRotDeg;
}

// Built v14.28: a latched record in the hangar SHELL's frame. The game's coordinate origin shifts (on foot to cockpit, a rebase), moving every record by
// one vector and, if the frame turns, rotating every axis; the v14.14 latch compared world positions and read that as the mechanism moving (flight
// 2d4ba6db: 50.85 m on every record, a false launch release). The record's centre is written along the shell's unit axes from the shell's centre, and its
// two axes in those axes, so a shift or a turn that the shell shares changes nothing. sAx[k] is the k-th unit axis of the shell in world coordinates.
inline void shellLocalSample(const float p[3], const float a0[3], const float a2[3], const float shellPos[3], const float sAx[3][3], float rel[3], float l0[3],
                             float l2[3]) noexcept {
    const float d[3] = {p[0] - shellPos[0], p[1] - shellPos[1], p[2] - shellPos[2]};
    for (int k = 0; k < 3; ++k) {
        rel[k] = sAx[k][0] * d[0] + sAx[k][1] * d[1] + sAx[k][2] * d[2];
        l0[k] = sAx[k][0] * a0[0] + sAx[k][1] * a0[1] + sAx[k][2] * a0[2];
        l2[k] = sAx[k][0] * a2[0] + sAx[k][1] * a2[1] + sAx[k][2] * a2[2];
    }
}

// Built v14.28 (the backstop): between two samples the shell moved by shellDelta (at least minShellM) and every one of the n latched records moved by
// that vector within tolM: the whole frame shifted (an origin rebase), nothing in the bay moved. The caller re-bases its references and counts no motion.
inline bool commonFrameShift(const float (*recDelta)[3], size_t n, const float shellDelta[3], float minShellM, float tolM) noexcept {
    if (n == 0) return false;
    const float sl = std::sqrt(shellDelta[0] * shellDelta[0] + shellDelta[1] * shellDelta[1] + shellDelta[2] * shellDelta[2]);
    if (!(sl >= minShellM)) return false;
    for (size_t i = 0; i < n; ++i) {
        const float dx = recDelta[i][0] - shellDelta[0];
        const float dy = recDelta[i][1] - shellDelta[1];
        const float dz = recDelta[i][2] - shellDelta[2];
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > tolM) return false;
    }
    return true;
}

// Built v14.12: the bay's mechanism, latched once per descent. The own pad; the other pads of its size within kLatchLateralM of the shaft; and the
// mechanism records (Lift*, TurboLift*, BlastShield*, HangarGate*) within kLatchLateralM of the shaft and along it within the box's span. The own pad
// is first; at most kLatchCap records. Each is read by address each frame (LatchRec::step); nothing else is watched.
constexpr size_t kLatchCap = 16;
constexpr size_t kLatchCandCap = 128;
constexpr float kLatchLateralM = 60.0f;

struct LatchCand {
    uintptr_t addr = 0;
    char name[48] = {};
    float centre[3] = {};
};

// The latch's class of a candidate name (built v14.16): 1 the pads of the own size (checked as pads), 2 BlastShield* and Lift / Lift_*, 3 HangarGate*.
// -1: not a mechanism. TurboLift* (the elevator doors, open whenever the lift is used), doors and decals are never mechanism records.
inline int latchClass(const char* name) noexcept {
    if (containsNoCase(name, "Door") || containsNoCase(name, "Decal") || hasPrefixNoCase(name, "TurboLift")) return -1;
    if (isPadName(name)) return 1;
    if (hasPrefixNoCase(name, "BlastShield")) return 2;
    if (std::strcmp(name, "Lift") == 0 || hasPrefixNoCase(name, "Lift_")) return 2;
    if (hasPrefixNoCase(name, "HangarGate")) return 3;
    return -1;
}

inline bool mechanismName(const char* name) noexcept {
    const int cls = latchClass(name);
    return cls == 2 || cls == 3;
}

// The distance of c from the shaft line through o along the unit up axis, and the position of c along that axis.
inline float shaftLateralM(const float c[3], const float o[3], const float up[3]) noexcept {
    const float d[3] = {c[0] - o[0], c[1] - o[1], c[2] - o[2]};
    const float a = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
    const float l[3] = {d[0] - a * up[0], d[1] - a * up[1], d[2] - a * up[2]};
    return std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
}

inline float shaftAlongM(const float c[3], const float o[3], const float up[3]) noexcept {
    return (c[0] - o[0]) * up[0] + (c[1] - o[1]) * up[1] + (c[2] - o[2]) * up[2];
}

// One latched record: its address, name, and its centre relative to the shell and its axes at its last reading.
struct LatchRec {
    uintptr_t addr = 0;
    char name[48] = {};
    uint32_t stamp = 0;
    float rel[3] = {};
    float ax0[3] = {};
    float ax2[3] = {};
    uint8_t ps = 4;
    uint8_t ss = 4;
    uint64_t lastMovedMs = 0;
    bool haveMotion = false;
    uint32_t movedRun = 0;   // consecutive compared frames the record moved (built v14.14)
    // Reads the record for this frame: a reading one frame after the last, from the same sources, is compared (poseMoved); a missing frame or another
    // source re-bases without counting. Returns true when the record moved.
    bool step(uint32_t frame, const float r[3], const float a0[3], const float a2[3], uint8_t p, uint8_t s, uint64_t nowMs) noexcept {
        bool moved = false;
        if (stamp != 0 && stamp + 1 == frame && ps == p && ss == s) {
            moved = poseMoved(rel, r, ax0, a0, ax2, a2);
            if (moved) {
                lastMovedMs = nowMs;
                haveMotion = true;
            }
        }
        movedRun = moved ? movedRun + 1 : 0;   // a re-base or a missing frame resets the run: never a move from a source change
        stamp = frame;
        std::memcpy(rel, r, sizeof(rel));
        std::memcpy(ax0, a0, sizeof(ax0));
        std::memcpy(ax2, a2, sizeof(ax2));
        ps = p;
        ss = s;
        return moved;
    }
};

// The latched set: the own pad (index 0) and the mechanism records. lastMotionMs is the last motion of a record other than the pad.
struct LatchSet {
    LatchRec recs[kLatchCap];
    size_t n = 0;
    bool latched = false;
    uint64_t lastMotionMs = 0;

    // Built v14.20 (review 2): a reset clears every record's motion references too, so a rebuilt latch never inherits an old run or an old clock.
    void reset() noexcept {
        for (LatchRec& r : recs) {
            r.stamp = 0;
            r.haveMotion = false;
            r.lastMovedMs = 0;
            r.movedRun = 0;
        }
        n = 0;
        latched = false;
        lastMotionMs = 0;
    }
    void recomputeMotion() noexcept {
        uint64_t m = 0;
        for (size_t i = 1; i < n; ++i) {
            if (recs[i].haveMotion && recs[i].lastMovedMs > m) m = recs[i].lastMovedMs;
        }
        lastMotionMs = m;
    }
    // The latched record (index 0 is the own pad) that moved on two compared frames in a row, or -1 (built v14.14; the pad too, built v14.15).
    int movedIndex() const noexcept {
        for (size_t i = 0; i < n; ++i) {
            if (recs[i].movedRun >= 2) return static_cast<int>(i);
        }
        return -1;
    }
    uint64_t padLastMotionMs() const noexcept { return n > 0 ? recs[0].lastMovedMs : 0; }
    // The names of the latched records that moved within kBayStillMs, up to maxNames, joined with ", ".
    void movers(uint64_t now, char* out, size_t cap, size_t maxNames) const noexcept {
        out[0] = '\0';
        size_t used = 0;
        size_t named = 0;
        for (size_t i = 0; i < n && named < maxNames; ++i) {
            const LatchRec& r = recs[i];
            if (!r.haveMotion || now > r.lastMovedMs + kBayStillMs) continue;
            if (used + 2 >= cap) break;
            const int w = std::snprintf(out + used, cap - used, "%s%s", named > 0 ? ", " : "", r.name);
            if (w < 0) break;
            used += static_cast<size_t>(w);
            ++named;
        }
    }
    // The latched record that moved last (empty when none has moved).
    void lastMover(char* out, size_t cap) const noexcept {
        out[0] = '\0';
        uint64_t latest = 0;
        bool any = false;
        for (size_t i = 0; i < n; ++i) {
            if (!recs[i].haveMotion) continue;
            if (!any || recs[i].lastMovedMs >= latest) {
                any = true;
                latest = recs[i].lastMovedMs;
                std::snprintf(out, cap, "%s", recs[i].name);
            }
        }
    }
};

// The latch: the own pad first (ownAddr, ownName, ownSize, at origin along up), then the candidates that qualify. A pad qualifies with the own
// size within kLatchLateralM laterally; a mechanism record within kLatchLateralM laterally and with its along position in [alongMin, alongMax].
// Returns the count written to out (at most kLatchCap).
// The bay's span from the pad along the shaft, in metres: the target's distance (with a margin), and never less than kLatchSpanM (a bay's
// mechanism reaches ~270 m along the shaft; the target is not always built yet, at load-in).
constexpr float kLatchSpanM = 300.0f;

// A candidate the latch refused, and why (the first few are logged at the latch).
struct LatchReject {
    char name[48] = {};
    char why[80] = {};
};

// The latch: the own pad first (ownAddr, ownName, ownSize, at origin along up), then the candidates that qualify. A pad qualifies with the own size
// within kLatchLateralM laterally; a mechanism record within kLatchLateralM laterally and with its along position in [alongMin, alongMax]. Returns
// the count written to out (at most kLatchCap). Refused candidates are written to rej (up to rejCap), with their reason, when rej is given.
// The latch: the own pad first (ownAddr, ownName, ownSize, at origin along up), then, in priority order, the pads of the own size within kLatchLateralM
// laterally, then BlastShield* and Lift records, then HangarGate* records; the mechanism records within kLatchLateralM laterally and with their along
// position in [alongMin, alongMax]. At most kLatchCap records: a candidate that qualifies beyond the cap is refused with "over the latch cap" (built
// v14.16). Refused candidates are written to rej (up to rejCap), with their reason, when rej is given.
inline size_t selectMechanism(const LatchCand* cands, size_t n, uintptr_t ownAddr, const char* ownName, PadSize ownSize, const float origin[3],
                              const float up[3], float alongMin, float alongMax, LatchRec* out, LatchReject* rej = nullptr, size_t rejCap = 0,
                              size_t* rejN = nullptr) noexcept {
    size_t k = 0;
    auto refuse = [&](const LatchCand& cand, const char* why) noexcept {
        if (rej == nullptr || rejN == nullptr || *rejN >= rejCap) return;
        std::snprintf(rej[*rejN].name, sizeof(rej[*rejN].name), "%s", cand.name);
        std::snprintf(rej[*rejN].why, sizeof(rej[*rejN].why), "%s", why);
        ++*rejN;
    };
    if (ownAddr != 0) {
        out[k] = LatchRec{};
        out[k].addr = ownAddr;
        std::snprintf(out[k].name, sizeof(out[k].name), "%s", ownName);
        ++k;
    }
    for (int pass = 1; pass <= 3; ++pass) {
        for (size_t i = 0; i < n; ++i) {
            const LatchCand& cand = cands[i];
            if (cand.addr == 0 || cand.addr == ownAddr) continue;
            const int cls = latchClass(cand.name);
            if (cls < 0) {
                if (pass == 1) refuse(cand, "not a mechanism record (or excluded: a door, a decal, a turbo lift)");
                continue;
            }
            const int want = cls == 1 ? 1 : (cls == 2 ? 2 : 3);
            if (want != pass) continue;
            const float lat = shaftLateralM(cand.centre, origin, up);
            const float along = shaftAlongM(cand.centre, origin, up);
            bool ok = false;
            if (cls == 1) {
                if (ownSize == PadSize::kNone || padSizeOf(cand.name) != ownSize) {
                    refuse(cand, "a pad of another size");
                } else if (lat > kLatchLateralM) {
                    char why[80];
                    std::snprintf(why, sizeof(why), "%.0f m lateral (limit %.0f)", static_cast<double>(lat), static_cast<double>(kLatchLateralM));
                    refuse(cand, why);
                } else {
                    ok = true;
                }
            } else if (lat > kLatchLateralM) {
                char why[80];
                std::snprintf(why, sizeof(why), "%.0f m lateral (limit %.0f)", static_cast<double>(lat), static_cast<double>(kLatchLateralM));
                refuse(cand, why);
            } else if (along < alongMin || along > alongMax) {
                char why[80];
                std::snprintf(why, sizeof(why), "%.0f m along, outside [%.0f, %.0f]", static_cast<double>(along), static_cast<double>(alongMin),
                              static_cast<double>(alongMax));
                refuse(cand, why);
            } else {
                ok = true;
            }
            if (!ok) continue;
            if (k >= kLatchCap) {
                refuse(cand, "over the latch cap (16): dropped");
                continue;
            }
            out[k] = LatchRec{};
            out[k].addr = cand.addr;
            std::snprintf(out[k].name, sizeof(out[k].name), "%s", cand.name);
            ++k;
        }
    }
    return k;
}

// Built v14.23: the sun inside the hangar, from the game's pad state. The skip is off while the own pad's latest r0 is 1 (moving in) or 3 (the descent),
// read fresh. It comes back on when the pad is out (not r1 1 fresh), r0 is neither 1 nor 3 (or stale), and the pad is raised (the geometric test),
// or on undock, setting off, or a hangar change. An outpost surface (r0 0) never turns it off.
struct SunInputs {
    bool setting = false;
    bool docked = false;
    bool ownKnown = false;       // the game has named the own pad this dock
    bool r0Fresh = false;        // the own pad's r0 was called within kPadStateFreshMs
    uint8_t r0 = 0;              // the own pad's latest r0 code (meaningful with r0Fresh)
    bool r1Fresh = false;        // the own pad's r1 was called within kPadStateFreshMs
    bool inHangar = false;       // the own pad's r1 is 1 (meaningful with r1Fresh)
    bool relOk = false;          // the pad's height above the shell is readable this frame (the raised test)
    float rel = 0.0f;
    bool hangarChanged = false;
};

struct SunState {
    bool off = false;
};

// Returns true when the skip changed. *why names the change for the state line.
inline bool sunStep(const SunInputs& in, SunState* st, const char** why) noexcept {
    *why = "";
    const bool moving = in.ownKnown && in.r0Fresh && (in.r0 == kReaderMoving || in.r0 == kReaderDescent);
    if (!st->off) {
        if (in.setting && in.docked && moving) {
            st->off = true;
            *why = "pad moving in or locked in (r0)";
            return true;
        }
        return false;
    }
    if (!in.setting) {
        st->off = false;
        *why = "setting off";
        return true;
    }
    if (!in.docked) {
        st->off = false;
        *why = "undocked";
        return true;
    }
    if (in.hangarChanged) {
        st->off = false;
        *why = "hangar changed";
        return true;
    }
    if (moving) return false;                              // still moving in, or the descent
    if (in.r1Fresh && in.inHangar) return false;           // the game still says the pad is in the bay
    if (!in.relOk) return false;                           // unreadable this frame: hold
    if (in.rel >= kLoweredM) {
        st->off = false;
        *why = "pad raised (launch)";
        return true;
    }
    return false;
}

// Built v14.13: a docked pad with no sized shell. The no-hangar note is due once per dock (after kHangarNoteFrames frames without a shell). After
// kNoShellGiveUpFrames the gather of the shell parts stops until undock or a new pad. A shell found resets the count.
constexpr uint32_t kHangarNoteFrames = 60;
constexpr uint32_t kNoShellGiveUpFrames = 300;

struct HangarWatch {
    uint32_t missing = 0;
    bool giveUp = false;
    bool logged = false;
    // One frame. Returns true on the frame the once-per-dock note is due.
    bool step(bool shellFound, bool docked, bool padChanged) noexcept {
        if (!docked) {
            *this = HangarWatch{};
            return false;
        }
        if (padChanged) {
            missing = 0;
            giveUp = false;
        }
        if (shellFound) {
            missing = 0;
            return false;
        }
        if (missing < kNoShellGiveUpFrames) ++missing;
        if (missing >= kNoShellGiveUpFrames) giveUp = true;
        if (!logged && missing >= kHangarNoteFrames) {
            logged = true;
            return true;
        }
        return false;
    }
};

// Built v14.13: the names that may mark a hangar (or a pad or a bay door) near a pad with no sized shell: logged once per dock.
inline bool hangarLikeName(const char* name) noexcept {
    return containsNoCase(name, "Hanger") || containsNoCase(name, "Hangar") || containsNoCase(name, "Pad") || containsNoCase(name, "Lift") ||
           containsNoCase(name, "BlastShield") || containsNoCase(name, "Bay") || containsNoCase(name, "Elevator");
}

// Built v14.29: what a node's name is good for, once per node (the class cache stores it): a latch candidate, a hangar-name candidate, a ship tally
// prefix. A pad or an explicit part is a class, not a flag. A name that is empty or unreadable has none.
inline uint32_t nodeFlagsOf(const char* name) noexcept {
    if (!name || !*name) return 0;
    uint32_t f = 0;
    if (mechanismName(name)) f |= kNodeMechanism;
    if (hangarLikeName(name)) f |= kNodeHangarLike;
    if (std::strchr(name, '_') != nullptr) f |= kNodeShipPrefix;
    return f;
}

// Built v14.29: what the store observer gathers for a record in an unsealed pass, from its node alone (no centre read). A pass that samples the near
// list, the own candidate and the tally gathers every record's parts; any other pass reads the centre only of the nodes that can matter.
inline bool nodeWantsCentre(NameClass cls, uint32_t flags, bool giveUp, bool latchPending, bool namesPending, bool fullPass) noexcept {
    if (fullPass) return true;
    if (cls == NameClass::kPad) return true;
    if (cls == NameClass::kExplicit && !giveUp) return true;
    if (latchPending && (flags & kNodeMechanism) != 0) return true;
    if (namesPending && (flags & kNodeHangarLike) != 0) return true;
    return false;
}

// Built v14.29, v14.31: whether a pass is a full one. The pass of stamp S samples the near list, the own candidate and the ship tally every
// kSamplePass-th stamp unsealed and every kSamplePassSealed-th while sealed, and every stamp while an engage is pending.
constexpr uint32_t kSamplePass = 8;
constexpr uint32_t kSamplePassSealed = 4;
inline bool fullGatherPass(uint32_t stamp, bool engagePending, bool sealed) noexcept {
    return engagePending || (stamp % (sealed ? kSamplePassSealed : kSamplePass)) == 0;
}

// Advances the state and returns the action (built v14.23: the game's pad state decides). Seal when the own pad is in the hangar by the game (r1 1,
// fresh), the setting is on, Status says docked, the pad pick and cell are valid, and the camera is in the bay for kCameraFrames. Seal at once: no
// stillness clock, no load-in or arrival rule, no fallback. Releases: setting off, undocked, the camera left, the mechanism moved, the pad moved,
// the hangar changed, and the own pad leaving the hangar (the r1 1 to 0 edge). A release by the mechanism, the pad moving or the pad leaving the
// hangar needs a fresh in-hangar edge (0 then 1 of r1) before the next seal; a camera or setting release does not.
inline SealAction sealStep(const SealInputs& in, SealState* st, ReleaseWhy* why) noexcept {
    *why = ReleaseWhy::kNone;
    if (!in.setting) {
        *st = SealState{};
        if (in.held) {
            *why = ReleaseWhy::kSettingOff;
            return SealAction::kRelease;
        }
        return SealAction::kNone;
    }
    // The camera's runs: the camera rule is the only camera condition, and it needs kCameraFrames frames either way.
    if (in.cameraOutside) ++st->camOutRun;
    else st->camOutRun = 0;
    if (in.cameraInBox) {
        if (st->camInRun < kCameraFrames) ++st->camInRun;
    } else {
        st->camInRun = 0;
    }
    if (in.held) {
        ReleaseWhy w = ReleaseWhy::kNone;
        if (!in.docked) w = ReleaseWhy::kUndocked;
        else if (st->camOutRun >= kCameraFrames) w = ReleaseWhy::kCameraLeft;
        else if (in.mechanismMoved) w = ReleaseWhy::kMechanism;   // the hatch or the lift moved: the launch
        else if (in.padMoved) w = ReleaseWhy::kPadMoved;
        else if (in.hangarChanged) w = ReleaseWhy::kHangarChanged;
        else if (in.launched) w = ReleaseWhy::kLaunch;
        else if (in.r1Fresh && !in.inHangar) w = ReleaseWhy::kInHangarLeft;   // the game's r1 1 to 0: the pad left the bay
        if (w != ReleaseWhy::kNone) {
            if (w == ReleaseWhy::kMechanism || w == ReleaseWhy::kPadMoved || w == ReleaseWhy::kInHangarLeft || w == ReleaseWhy::kLaunch) {
                st->reseal = 1;   // a fresh in-hangar edge is needed before the next seal
            }
            st->mechanism = (w == ReleaseWhy::kMechanism);   // built v14.24: a mechanism release may re-seal on the poses instead of the edge
            st->poseRun = 0;
            clearTrigger(st);
            st->camOutRun = 0;
            if (w == ReleaseWhy::kCameraLeft) st->camInRun = 0;   // the re-seal waits kCameraFrames inside the bay
            *why = w;
            return SealAction::kRelease;
        }
        return SealAction::kNone;
    }
    if (!in.docked) {
        clearTrigger(st);
        st->reseal = 0;
        st->mechanism = false;
        st->poseRun = 0;
        return SealAction::kNone;
    }
    if (in.padHeld) return SealAction::kNone;   // a short absence: the trigger keeps its state
    if (!in.pickOk) {
        clearTrigger(st);
        return SealAction::kNone;
    }
    if (st->padAddr != in.pad) {   // another pad: a request made for the old one is withdrawn
        clearTrigger(st);
        st->padAddr = in.pad;
    }
    // The game's pad state. Unknown (no r0 on this dock) or stale r1: no seal.
    if (!in.ownKnown || !in.r1Fresh) {
        clearTrigger(st);
        return SealAction::kNone;
    }
    if (!in.inHangar) {   // the pad is out of the bay: the re-seal edge advances (and the pose count restarts: the edge rule stands)
        clearTrigger(st);
        if (st->reseal == 1) st->reseal = 2;
        st->mechanism = false;
        st->poseRun = 0;
        return SealAction::kNone;
    }
    if (st->reseal == 1 && st->mechanism) {   // built v14.24: r1 1 and fresh, with the latched poses back, for kPoseBackFrames frames in a row
        st->poseRun = in.poseBack ? st->poseRun + 1 : 0;
        if (st->poseRun >= kPoseBackFrames) {
            st->reseal = 0;
            st->mechanism = false;
            st->poseRun = 0;
        }
    }
    if (st->reseal == 1) return SealAction::kNone;   // in the bay but not yet out since the release: wait for the edge
    if (st->reseal == 2) st->reseal = 0;             // out, then in: the fresh edge
    if (st->camInRun < kCameraFrames) {
        clearTrigger(st);
        return SealAction::kNone;
    }
    if (st->requested) return SealAction::kNone;
    st->requested = true;
    return SealAction::kRequestEngage;
}

// Built v14.4 (the flicker fix): the pad's motion against its shell, measured along the shaft. The reference is captured ONCE, at the first sample after
// the seal is held, by the same function and inputs as every later sample (the v14.3 flight took it from the gather snapshot, a different centre by a
// constant: the pad read 2.4 m off its shell at once). A sample whose centre sources differ from the reference's is another centre: it is not
// compared, and the run is kept. A release needs the offset beyond kPadMoveReleaseM on kPadMoveFrames consecutive samples: motion is continuous.
constexpr uint32_t kPadMoveFrames = 10;

struct ShellMotion {
    bool captured = false;
    float ref = 0.0f;        // the pad's height along the shaft above the shell, at the capture
    uint8_t padSrc = 4;      // the centre sources the reference was read from
    uint8_t shellSrc = 4;
    uint32_t run = 0;        // consecutive samples beyond the limit
    float delta = 0.0f;      // the latest sample minus the reference
    // Returns true when the pad has been beyond the limit for kPadMoveFrames samples in a row.
    bool sample(const float pad[3], uint8_t ps, const float shell[3], uint8_t ss, const float up[3]) noexcept {
        if (!captured) {
            ref = shellRelative(pad, shell, up);
            padSrc = ps;
            shellSrc = ss;
            captured = true;
            run = 0;
            delta = 0.0f;
            return false;
        }
        if (ps != padSrc || ss != shellSrc) return false;
        const float now = shellRelative(pad, shell, up);
        delta = now - ref;
        run = padMovedAway(ref, now) ? run + 1 : 0;
        return run >= kPadMoveFrames;
    }
};

// Built v14.4 (the safety net): more than kSealRateMax seals in kSealRateWindowMs, in one hangar, stops the sealing until a hangar change or a
// setting toggle. A user must never see the sun flicker from this feature again.
constexpr uint32_t kSealRateMax = 6;
constexpr uint64_t kSealRateWindowMs = 60000;

struct SealRateGuard {
    uint64_t times[kSealRateMax + 1] = {};
    uint32_t n = 0;
    uint32_t head = 0;
    bool tripped = false;
    // Records a seal request at now. Returns true when it trips the guard (the first time, and every time after while tripped).
    bool note(uint64_t now) noexcept {
        if (tripped) return true;
        times[head] = now;
        head = (head + 1) % (kSealRateMax + 1);
        if (n < kSealRateMax + 1) ++n;
        uint32_t inWindow = 0;
        for (uint32_t i = 0; i < n; ++i) {
            if (now >= times[i] && now - times[i] <= kSealRateWindowMs) ++inWindow;
        }
        if (inWindow > kSealRateMax) tripped = true;
        return tripped;
    }
};

// Built v14 (fix 1): the own pad is sticky. Once chosen it stays the own pad while its record is in the store; another pad can replace it only after
// the held one has been unseen for kHeldLostFrames frames (the hangar-change rule). While it is unseen and not yet replaced there is no pick.
struct OwnPadSticky {
    uintptr_t pad = 0;      // the sticky own pad's record (0: none chosen yet)
    uint32_t unseen = 0;    // frames the sticky pad has been absent
};

inline void pickOwnPadSticky(const PadObs* obs, size_t n, const float camera[3], const float radii[3], OwnPadSticky* st, OwnCell* out) noexcept {
    if (st->pad != 0) {
        const PadObs* held = nullptr;
        for (size_t i = 0; i < n; ++i) {
            if (obs[i].addr == st->pad && obs[i].size != PadSize::kNone) {
                held = &obs[i];
                break;
            }
        }
        // Built v14.8: a held B platform gives way to the A platform of its size within kPlatformAM (a fresh pick).
        if (held != nullptr && !(padIsPlatformB(*held) && aPadNear(obs, n, camera, held->size))) {
            st->unseen = 0;
            *out = OwnCell{};
            fillOwnFromPad(*held, radii, distance(held->centre, camera), out);
            return;
        }
        if (held == nullptr && ++st->unseen < kHeldLostFrames) {
            *out = OwnCell{};
            // Built v14.24: no frame counter in the text (it changed every frame and made the waiting line repeat each frame).
            std::snprintf(out->reason, sizeof(out->reason), "the own pad is not in the store this frame (held for up to %u frames)",
                          static_cast<unsigned>(kHeldLostFrames));
            return;
        }
        st->pad = 0;
        st->unseen = 0;
    }
    pickOwnPad(obs, n, camera, radii, out);
    if (out->has) st->pad = out->pad;
}

// Built v14 (fix 2): the shell the pad's motion is checked against, chosen once at the freeze. A usable shell is a Hanger or Hangar part that is not a
// gate, a ramp or a skirt. The part named for the pad's size (HangerSmall, HangerMedium, HangerLarge) comes first, then the other sized hangers, then
// the rest; within each rank the nearest to the pad. Returns n when none qualifies.
inline bool hangerWord(const char* n) noexcept {
    return std::strstr(n, "Hanger") != nullptr || std::strstr(n, "Hangar") != nullptr;
}

inline bool shellNameUsable(const char* n) noexcept {
    if (!hangerWord(n)) return false;
    return std::strstr(n, "Gate") == nullptr && std::strstr(n, "Ramp") == nullptr && std::strstr(n, "Skirt") == nullptr;
}

inline int shellRank(const char* n, PadSize size) noexcept {
    const char* word = size == PadSize::kSmall ? "Small" : size == PadSize::kMedium ? "Medium" : size == PadSize::kLarge ? "Large" : nullptr;
    if (word != nullptr && std::strstr(n, word) != nullptr) return 0;
    if (std::strstr(n, "Small") != nullptr || std::strstr(n, "Medium") != nullptr || std::strstr(n, "Large") != nullptr) return 1;
    return 2;
}

inline size_t pickShell(const ExplicitObs* obs, size_t n, const float centre[3], PadSize size, float maxM) noexcept {
    size_t best = n;
    int bestRank = 9;
    float bestD = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        if (!shellNameUsable(obs[i].name)) continue;
        const float d = distance(obs[i].centre, centre);
        if (d > maxM) continue;
        const int r = shellRank(obs[i].name, size);
        if (r == 2) continue;   // built v14.13: a shell needs a size word (HangerSmall, HangerMedium, HangerLarge)
        if (best == n || r < bestRank || (r == bestRank && d < bestD)) {
            best = i;
            bestRank = r;
            bestD = d;
        }
    }
    return best;
}

// Built v14.1: what the store observer does with a record. Judging (the keep and the cull) is sealed-only. Gathering (the camera, the frame's
// pads, the explicit parts and so the own-pad pick the seal trigger needs) runs while docked and unsealed, so a pad is in hand before the seal.
// Setting off: nothing. The v14 flight never sealed because the gathering was inside the sealed-only judge (no pad, so no seal, ever).
enum class StoreMode : uint8_t { kNone = 0, kGather = 1, kJudge = 2 };

inline StoreMode storeMode(bool setting, bool isSealed, bool docked) noexcept {
    if (!setting) return StoreMode::kNone;
    if (isSealed) return StoreMode::kJudge;
    return docked ? StoreMode::kGather : StoreMode::kNone;
}

// Built v14.23: the waiting line's inputs, as they stand this frame: the Status docked flag, the own pad from the game, its codes and freshness, the
// camera, and the cell. The line changes when a key changes (not per frame) and at most every 10 s while docked.
struct GameView {
    bool dockedKnown = false;   // Status.json has a Flags sample
    bool docked = false;        // Flags bit 0
    bool ownKnown = false;      // the game has named the own pad this dock
    uintptr_t own = 0;          // its record (the probe's key is its low six hex digits)
    int r0 = -1;                // its latest r0 code (-1: never)
    int r1 = -1;                // its latest r1 code (-1: never)
    bool r0Fresh = false;
    bool r1Fresh = false;
    bool cameraKnown = false;   // the frame has a camera
    bool cameraIn = false;      // the camera is inside the candidate box
    bool cellBuilt = false;     // the pad's cell can be built
    char cellReason[96] = {};   // when not built: why
};

// True when a key of the line changed: docked, the own pad, its codes or freshness, the camera, the cell.
inline bool gameKeyChanged(const GameView& a, const GameView& b) noexcept {
    return a.dockedKnown != b.dockedKnown || a.docked != b.docked || a.ownKnown != b.ownKnown || a.own != b.own || a.r0 != b.r0 || a.r1 != b.r1 ||
           a.r0Fresh != b.r0Fresh || a.r1Fresh != b.r1Fresh || a.cameraIn != b.cameraIn || a.cellBuilt != b.cellBuilt ||
           std::strcmp(a.cellReason, b.cellReason) != 0;
}

// The waiting line, with the unmet condition in its words:
//   optimized station: waiting (docked=yes, own pad @0abc40, r0=1 r1=1 in hangar=yes, camera=yes, cell=built)
//   optimized station: waiting (docked=yes, game pad state unknown (no r0 on this dock), cell=built)
inline void gameWaitingLine(const GameView& w, char* out, size_t cap) noexcept {
    const char* docked = !w.dockedKnown ? "unknown" : (w.docked ? "yes" : "no");
    char cell[128];
    if (w.cellBuilt) std::snprintf(cell, sizeof(cell), "built");
    else std::snprintf(cell, sizeof(cell), "failed:%s", w.cellReason[0] != '\0' ? w.cellReason : "no cell");
    const char* camera = !w.cameraKnown ? "unknown" : (w.cameraIn ? "yes" : "no");
    if (!w.ownKnown) {
        std::snprintf(out, cap, "optimized station: waiting (docked=%s, game pad state unknown (no r0 on this dock), camera=%s, cell=%s)", docked,
                      camera, cell);
        return;
    }
    char r0[24];
    char r1[24];
    if (w.r0 < 0) std::snprintf(r0, sizeof(r0), "-");
    else std::snprintf(r0, sizeof(r0), "%d%s", w.r0, w.r0Fresh ? "" : " (stale)");
    if (w.r1 < 0) std::snprintf(r1, sizeof(r1), "-");
    else std::snprintf(r1, sizeof(r1), "%d%s", w.r1, w.r1Fresh ? "" : " (stale)");
    const char* inHangar = !w.r1Fresh ? "unknown" : (w.r1 == kReaderInHangar ? "yes" : "no");
    std::snprintf(out, cap, "optimized station: waiting (docked=%s, own pad @%06llx, r0=%s r1=%s, in hangar=%s, camera=%s, cell=%s)", docked,
                  static_cast<unsigned long long>(static_cast<uint64_t>(w.own) & 0xFFFFFFull), r0, r1, inHangar, camera, cell);
}

// Built v14.2: promoteFrame runs once per frame, at the frame's first record. The frame stamp advances at each tick; it never did (the v14.1
// flight: the stamp stayed 0 and the cell stamp started at 0, so no promote ever ran, and no pad was ever picked).
inline bool framePromoteDue(uint32_t cellStamp, uint32_t frameStamp) noexcept { return cellStamp != frameStamp; }

// The position of p along the cell box's own axes (u, v, w), when the cell is a box. False otherwise.
inline bool boxCoordsOf(const OwnCell& c, const float p[3], float coords[3]) noexcept {
    if (!c.box) return false;
    const float d[3] = {p[0] - c.boxCentre[0], p[1] - c.boxCentre[1], p[2] - c.boxCentre[2]};
    coords[0] = d[0] * c.boxU[0] + d[1] * c.boxU[1] + d[2] * c.boxU[2];
    coords[1] = d[0] * c.boxV[0] + d[1] * c.boxV[1] + d[2] * c.boxV[2];
    coords[2] = d[0] * c.boxW[0] + d[1] * c.boxW[1] + d[2] * c.boxW[2];
    return true;
}

// ---- Names: the text a record's name is read from (the census's accept rules, asset_census_core.h) ----
constexpr size_t kNameBytes = 96;                    // bytes read at a pointer when looking for text

constexpr size_t kNameMax = 48;                      // longest text kept

// ---------------------------------------------------------------------------------------------------------------------
// Names: text reached through a pointer in an object's first 0x100 bytes. Accepted: 4 to 48 characters from the allowed set,
// as ASCII or as UTF-16LE with ASCII code units, NUL-terminated within kNameBytes. Anything else is not a name.
inline bool nameChar(uint8_t c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '/' ||
           c == ' ' || c == ':' || c == '(' || c == ')' || c == '#' || c == '+' || c == '-';
}

// Writes the text found in bytes (ascii or utf16) to out. False when the bytes are not a name.
inline bool textName(const uint8_t* bytes, size_t n, bool wide, std::string* out) {
    std::string text;
    const size_t step = wide ? 2 : 1;
    for (size_t i = 0; i + step <= n; i += step) {
        const uint8_t c = bytes[i];
        if (wide && bytes[i + 1] != 0) return false;
        if (c == 0) {
            if (text.size() < 4 || text.size() > kNameMax) return false;
            *out = text;
            return true;
        }
        if (!nameChar(c) || text.size() >= kNameMax) return false;
        text.push_back(static_cast<char>(c));
    }
    return false;
}

}  // namespace edvr::sealed

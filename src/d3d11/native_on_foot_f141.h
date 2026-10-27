// F141 (native on-foot VR, MEASURE ONLY): the game's ADS sight point, and the weapon readout's anchor. Header only, no game or D3D11 state, so native_frame_test
// runs the very functions the hooks and the frame thread use. Journal: the arc doc, 2026-10-27, F141. RE: analysis\decomp\verify_20261027_ads_sight_line.txt and
// verify_20261027_ads_readout_anchor.txt (build 334925).
//
// The sight point. Flat ADS does not move the camera onto the sight: HumanoidWeaponAiming's per-frame 0x1908760 (rcx = the activity, "act") bends the SHOT. Once
// the ADS fraction w = [act+0xC8]->vt+0x100() reaches 1.0 and the sight handle at act+0x198 is valid (0x3175A80), it takes the sight point
// S = [act+0x68]->vt+0x38(out, 0x3176930(act+0x198, tmp)) in the world and sets the eye's aim dir (eye+0x300, in the eye's own axes) to normalize(S - E.pos) when
// that points forward of the eye matrix E's forward row. In VR the shots follow P14's forward, so the bend is not taking effect; the candidates are (a) w never
// reaches 1, (b) the handle is invalid, (c) 0x1908760 is not called for the local player, (d) another writer of the aim dir before the shot, (e) E is not P14.
// A hook on 0x1908760 reads after the game's own call (and the aim dir before it); an entry hook on the aim 0x19079E0 reads the aim dir at the shot. Nothing is
// written into the game.
//
// The readout's anchor. The 16 floats EDVR's weapon UI getter observation already reads at 0x1AB1960 are a locator's world matrix U. Per part key, over a held
// item and its scope, the spread of U against the held weapon's world W (the FP attach joint 51 through F_root) and against P14's world says whether the part is
// weapon- or optics-anchored (constant against W) or avatar-anchored (constant against P14).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f141 {

// ---- build 334925 (EliteDangerous64-334925.exe, read with capstone 2026-10-27) ----------------------------------------------------------------------------
// The bend: mov r11,rsp (4C 8B DC); push rsi (56); sub rsp,0xA0 (48 81 EC A0 00 00 00): 11 bytes, no relative operand. The verified run goes on through
// mov rax,[rcx+0x10]; mov rsi,rcx; cmp byte [rax+0x10],0 (the act's data and its ADS handling byte).
constexpr uintptr_t kBendRva = 0x1908760;
constexpr size_t kBendStolen = 11;
constexpr uint8_t kBendPrologue[22] = {0x4C, 0x8B, 0xDC, 0x56, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00,
                                       0x48, 0x8B, 0x41, 0x10, 0x48, 0x8B, 0xF1, 0x80, 0x78, 0x10, 0x00};
// The aim: mov r11,rsp (4C 8B DC); mov [r11+0x18],rbx (49 89 5B 18): 7 bytes, no relative operand; then push rbp; lea rbp,[r11-0x108].
constexpr uintptr_t kAimRva = 0x19079E0;
constexpr size_t kAimStolen = 7;
constexpr uint8_t kAimPrologue[15] = {0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x18, 0x55, 0x49, 0x8D, 0xAB, 0xF8, 0xFE, 0xFF, 0xFF};
// The game functions the bend calls, called again after it with the same arguments (each one's disassembly was read; see the arc doc):
constexpr uintptr_t kHandleValidRva = 0x3175A80;   // char (handle*): [h] resolved (0x647E90), then [h+0x48]->vt+0x8() and vt+0x28(); no write
constexpr uintptr_t kHandleLocalRva = 0x3176930;   // float4* (handle*, float4* out): [h+0x48]->vt+0x38(out, 0); returns out
// The eye interface ([act+0x98] = HumanoidEyeComponent + 0x58): its vtable, and two slots read here instead of called.
constexpr uintptr_t kEyeIfaceVtRva = 0x5200038;
constexpr uint32_t kEyeMatrixSlot = 0x20;          // -> 0xF893D0: lea rax,[rcx+0x268]; ret (the eye matrix E is at iface+0x268 = eye+0x2C0)
constexpr uintptr_t kEyeMatrixFnRva = 0xF893D0;    // (the aim dir is read from memory too: GetAimDir, slot +0x60, is movups xmm0,[rcx+0x2A8])
// Offsets, each from its base.
constexpr uint32_t kActData = 0x10;          // act+0x10: the activity's data
constexpr uint32_t kDataSightName = 0x08;    // data+0x08: char*, the sight locator's name
constexpr uint32_t kDataAdsOn = 0x10;        // data+0x10: byte, the ADS handling enabled
constexpr uint32_t kActXform = 0x68;         // act+0x68: the point transform's interface (vt+0x38(this, out, in))
constexpr uint32_t kXformSlot = 0x38;
constexpr uint32_t kActEyeIface = 0x98;      // act+0x98: the eye interface (eye+0x58)
constexpr uint32_t kActAdsCtl = 0xC8;        // act+0xC8: the weapon/ADS controller (vt+0x100() -> the ADS fraction)
constexpr uint32_t kAdsFractionSlot = 0x100;
constexpr uint32_t kActAimRec = 0x140;       // act+0x140: the weapon's aim record; record+0x20 = the shot's eye aim dir (world)
constexpr uint32_t kAimRecDir = 0x20;
constexpr uint32_t kActSight = 0x198;        // act+0x198: the sight transform handle (0x50 bytes; +0x48 the locator object)
constexpr uint32_t kHandleObj = 0x48;
constexpr uint32_t kIfaceToEye = 0x58;       // eye = iface - 0x58
constexpr uint32_t kIfaceMatrix = 0x268;     // iface+0x268 = eye+0x2C0: E, rows x, y, z (forward), position
constexpr uint32_t kIfaceAim = 0x2A8;        // iface+0x2A8 = eye+0x300: the aim dir in the eye's own axes (float4)
constexpr uint32_t kEyeData = 0x50;          // eye+0x50: the eye's data; data+0x40 = char*, the eye locator's name
constexpr uint32_t kEyeDataLocName = 0x40;
constexpr uint32_t kEyeLocator = 0x198;      // eye+0x198: the eye locator's handle (valid only on the local humanoid's FP avatar: the local filter)
// The readout's part record (record = hash slot + 0x10; the getter observation's `part`): the flags verify_20261027_ads_readout_anchor.txt section 2 names.
constexpr uint32_t kRecWeaponAnchored = 0x18, kRecWorldScale = 0x19, kRecFacing = 0x28, kRecDistanceScale = 0x29, kRecSpace = 0xD0, kRecHandle = 0x100;

// ---- vectors ---------------------------------------------------------------------------------------------------------------------------------------------
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = 180.0 / kPi;
inline double dot3(const double a[3], const double b[3]) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline double len3(const double a[3]) noexcept { return std::sqrt(dot3(a, a)); }
inline bool unit3(const double a[3], double out[3]) noexcept {
    const double n = len3(a);
    if (!(n > 1e-9) || !std::isfinite(n)) return false;
    for (int i = 0; i < 3; ++i) out[i] = a[i] / n;
    return true;
}
// The angle between two directions in degrees; -1 when either has no length.
inline double angleDeg(const double a[3], const double b[3]) noexcept {
    double ua[3], ub[3];
    if (!unit3(a, ua) || !unit3(b, ub)) return -1.0;
    const double c = dot3(ua, ub);
    return std::acos(c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c) * kDeg;
}
inline bool finiteAll(const float* v, size_t n) noexcept {
    for (size_t i = 0; i < n; ++i)
        if (!std::isfinite(v[i])) return false;
    return true;
}
inline void row3(const float m[16], int r, double out[3]) noexcept {
    for (int i = 0; i < 3; ++i) out[i] = static_cast<double>(m[4 * r + i]);
}
// F142. The 4x4 a game matrix is read as: rows 0..2 are the axes (x, y, z) and row 3 the position, so the lanes used are x, y, z of each row (0, 1, 2, 4, 5, 6, 8, 9, 10 and
// 12, 13, 14); lanes 3, 7, 11 and 15 are padding the readers never use. F141's first build tested all 16 floats and rejected every sample of the 2026-10-27 flight, S, E and U
// all read 0: the hypothesis is a non-finite padding lane. finiteUsed tests the lanes used; finitePad the others. A matrix with a non-finite padding lane and finite used
// lanes is counted (padNonFinite) and used.
inline bool finiteUsed(const float m[16]) noexcept {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c)
            if (!std::isfinite(m[4 * r + c])) return false;
    return true;
}
inline bool finitePad(const float m[16]) noexcept {
    return std::isfinite(m[3]) && std::isfinite(m[7]) && std::isfinite(m[11]) && std::isfinite(m[15]);
}
// One float for a log line: %.6g, a NaN as "nan" and an infinity as "inf" or "-inf" whatever the C runtime prints for them. Returns the length written.
inline size_t formatFloat(float v, char* out, size_t cap) noexcept {
    if (!out || cap < 2) return 0;
    const int n = std::isnan(v)   ? std::snprintf(out, cap, "nan")
                  : std::isinf(v) ? std::snprintf(out, cap, "%s", v < 0 ? "-inf" : "inf")
                                  : std::snprintf(out, cap, "%.6g", static_cast<double>(v));
    if (n <= 0) return 0;
    return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}
// A 4x4 for a log line, row by row: "[a b c d | e f g h | i j k l | m n o p]". The reader can tell a position in row 3 (m, n, o: lanes 12..14) from one in column 3 (d, h, l:
// lanes 3, 7, 11), and which lane is not a number.
inline void formatMat16(const float m[16], char* out, size_t cap) noexcept {
    if (!out || !cap) return;
    size_t at = 0;
    out[at++] = '[';
    for (int i = 0; i < 16 && at + 20 < cap; ++i) {
        if (i) {
            out[at++] = ' ';
            if (i % 4 == 0) {
                out[at++] = '|';
                out[at++] = ' ';
            }
        }
        at += formatFloat(m[i], out + at, cap - at);
    }
    if (at + 2 <= cap) out[at++] = ']';
    out[at < cap ? at : cap - 1] = 0;
}
// |E.pos - P14w| of the position read as row 3 (E[12..14], the RE note's reading) and as column 3 (E[3], E[7], E[11]); -1 when that candidate has a lane that is not a number.
inline double positionDist(const float m[16], bool column, const double p14w[3]) noexcept {
    const float p[3] = {column ? m[3] : m[12], column ? m[7] : m[13], column ? m[11] : m[14]};
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return -1.0;
    const double d[3] = {p[0] - p14w[0], p[1] - p14w[1], p[2] - p14w[2]};
    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

// ---- the bend, one call --------------------------------------------------------------------------------------------------------------------------------------
// What the hook read for one local call. E: the eye matrix (rows x, y, z = forward, position; world). S: the sight point (world). aimIn/aimOut: eye+0x300 before
// and after the game's call (the eye's own axes; (0, 0, 1) = straight down E's forward).
struct BendIn {
    bool eOk = false, sOk = false, wOk = false, okRead = false, ok = false, aimInOk = false, aimOutOk = false;
    float w = 0.0f;
    float E[16] = {};
    float S[3] = {};
    float sRaw[16] = {};   // F142: the transform's whole 16-float output (S is its first three), for the raw-float line; valid when sOk
    float aimIn[4] = {}, aimOut[4] = {};
};
// F142: why a sample gave no geometry (BendOut::why), counted per window so a run in which every sample is rejected reads as the instrument's, never as a finding about the
// game. kWhyHandle is not a rejection: the game's sight handle was not valid (candidate b), so S was never asked for.
enum Why : uint8_t { kWhyOk = 0, kWhyENotRead, kWhyENonFinite, kWhyHandle, kWhySNotRead, kWhySNonFinite, kWhyRow, kWhySOnE, kWhyCount };
constexpr double kSOnEMetres = 1e-6;   // |S - E| under this has no ray
inline const char* whyText(unsigned w) noexcept {
    static const char* const kText[kWhyCount] = {"usable", "E not read", "E non-finite in a used lane", "sight handle not valid (the game's)", "S not read",
                                                 "S non-finite", "a degenerate row of E", "|S - E| under 1 um"};
    return w < kWhyCount ? kText[w] : "?";
}
struct BendOut {
    uint8_t why = kWhyOk;          // F142: kWhyOk when geo; else the first reason geo is not set (E before S, reads before values)
    bool ePad = false;             // F142: E was read and its used lanes are finite, but a padding lane (3, 7, 11, 15) is not a number
    bool geo = false;              // E and S read, S off E's position
    double dist = 0.0;             // |S - E.pos| (m)
    double dotZ = 0.0;             // dot(unit E.z, normalize(S - E.pos)): the game's test is > 0
    double eyeRight = 0.0, eyeUp = 0.0, eyeAlong = 0.0;   // S - E.pos in E's unit rows (m)
    double rayDeg = -1.0;          // the angle between E.z and S - E.pos: the bend the sight asks
    bool applied = false;          // the game's own test: ok, w >= 1.0 and dotZ > 0
    double aimInDeg = -1.0, aimOutDeg = -1.0;   // the aim dir's angle from the eye's forward (0, 0, 1), on entry and on exit
    double aimOutVsRayDeg = -1.0;  // the exit aim dir in the world (through E's rows) against S - E.pos
    double ray[3] = {};            // normalize(S - E.pos) (world), when geo
};
inline BendOut bend(const BendIn& in) noexcept {
    BendOut o;
    const double fwd[3] = {0.0, 0.0, 1.0};
    if (in.aimInOk && finiteAll(in.aimIn, 3)) {
        const double a[3] = {in.aimIn[0], in.aimIn[1], in.aimIn[2]};
        o.aimInDeg = angleDeg(a, fwd);
    }
    if (in.aimOutOk && finiteAll(in.aimOut, 3)) {
        const double a[3] = {in.aimOut[0], in.aimOut[1], in.aimOut[2]};
        o.aimOutDeg = angleDeg(a, fwd);
    }
    // F142: E's used lanes only (finiteUsed), then S; the first reason is kept (E before S, a read that failed before a value that is not a number)
    if (!in.eOk) { o.why = kWhyENotRead; return o; }
    if (!finiteUsed(in.E)) { o.why = kWhyENonFinite; return o; }
    o.ePad = !finitePad(in.E);
    if (!in.sOk) { o.why = (in.okRead && !in.ok) ? kWhyHandle : kWhySNotRead; return o; }
    if (!finiteAll(in.S, 3)) { o.why = kWhySNonFinite; return o; }
    double ex[3], ey[3], ez[3], p[3];
    row3(in.E, 0, ex);
    row3(in.E, 1, ey);
    row3(in.E, 2, ez);
    row3(in.E, 3, p);
    double ux[3], uy[3], uz[3];
    if (!unit3(ex, ux) || !unit3(ey, uy) || !unit3(ez, uz)) { o.why = kWhyRow; return o; }
    const double d[3] = {in.S[0] - p[0], in.S[1] - p[1], in.S[2] - p[2]};
    o.dist = len3(d);
    if (!(o.dist >= kSOnEMetres) || !unit3(d, o.ray)) { o.why = kWhySOnE; return o; }
    o.geo = true;
    o.dotZ = dot3(uz, o.ray);
    o.eyeRight = dot3(d, ux);
    o.eyeUp = dot3(d, uy);
    o.eyeAlong = dot3(d, uz);
    o.rayDeg = angleDeg(uz, o.ray);
    o.applied = in.okRead && in.ok && in.wOk && in.w >= 1.0f && o.dotZ > 0.0;
    if (in.aimOutOk && finiteAll(in.aimOut, 3)) {
        // the game's SetAimDir takes (E.x . dir, E.y . dir, E.z . dir) with E's raw rows: the world dir is the rows weighted by the local one
        double wd[3];
        for (int i = 0; i < 3; ++i) wd[i] = in.aimOut[0] * ex[i] + in.aimOut[1] * ey[i] + in.aimOut[2] * ez[i];
        o.aimOutVsRayDeg = angleDeg(wd, o.ray);
    }
    return o;
}

// S's offset from P14's forward line, and E less P14, in the FP root's axes (row 0 right, row 1 up, row 2 forward, each made unit), metres.
struct P14Off {
    bool ok = false;
    double right = 0.0, up = 0.0, along = 0.0;     // S - P14w
    double eRel[3] = {};                           // E.pos - P14w: right, up, forward
    double eDist = 0.0;                            // |E.pos - P14w|
    double rowDist = -1.0, colDist = -1.0;         // F142: |E.pos - P14w| with the position read as row 3 (E[12..14], used) and as column 3 (E[3], E[7], E[11]); -1 not a number
};
inline P14Off p14Offset(const float S[3], const float E[16], const double p14w[3], const float froot[16]) noexcept {
    P14Off o;
    double r[3], u[3], f[3], rr[3], uu[3], ff[3];
    row3(froot, 0, r);
    row3(froot, 1, u);
    row3(froot, 2, f);
    if (!unit3(r, rr) || !unit3(u, uu) || !unit3(f, ff) || !finiteAll(S, 3) || !finiteUsed(E)) return o;
    o.rowDist = positionDist(E, false, p14w);
    o.colDist = positionDist(E, true, p14w);
    const double d[3] = {S[0] - p14w[0], S[1] - p14w[1], S[2] - p14w[2]};
    const double e[3] = {E[12] - p14w[0], E[13] - p14w[1], E[14] - p14w[2]};
    o.right = dot3(d, rr);
    o.up = dot3(d, uu);
    o.along = dot3(d, ff);
    o.eRel[0] = dot3(e, rr);
    o.eRel[1] = dot3(e, uu);
    o.eRel[2] = dot3(e, ff);
    o.eDist = len3(e);
    o.ok = std::isfinite(o.right) && std::isfinite(o.up) && std::isfinite(o.eDist);
    return o;
}

// A locator name from game memory, made printable: up to 63 bytes, stopped at the first NUL; a byte outside 0x20..0x7E becomes '?'. Returns the length.
inline size_t printableName(const char* raw, size_t rawBytes, char out[64]) noexcept {
    size_t n = 0;
    for (; n < rawBytes && n < 63 && raw[n]; ++n) {
        const unsigned char c = static_cast<unsigned char>(raw[n]);
        out[n] = (c >= 0x20 && c <= 0x7E && c != '"') ? static_cast<char>(c) : '?';
    }
    out[n] = 0;
    return n;
}

// One activation: the act or its sight handle changed. The handle's identity is its first word and its locator object (+0x48).
struct ActKey {
    uintptr_t act = 0, h0 = 0, h48 = 0;
    bool operator==(const ActKey& o) const noexcept { return act == o.act && h0 == o.h0 && h48 == o.h48; }
    bool operator!=(const ActKey& o) const noexcept { return !(*this == o); }
};

// ---- the sight point's window (frame thread) -------------------------------------------------------------------------------------------------------------
struct Acc {
    uint32_t samples = 0, adsOff = 0;      // local calls drained; of them, with the act's ADS handling off (data+0x10 == 0)
    uint32_t wN = 0, wFull = 0;            // w read; w >= 1.0
    double wMin = 0.0, wMax = 0.0;
    uint32_t okN = 0, okReadN = 0, applied = 0, geoN = 0;
    double distSum = 0.0, eyeRightSum = 0.0, eyeUpSum = 0.0, rayDegSum = 0.0;
    uint32_t p14N = 0, p14Exact = 0, noFp = 0;
    double p14Right = 0.0, p14Up = 0.0, p14Along = 0.0, frameGapSum = 0.0;
    double eRel[3] = {}, eDistSum = 0.0, eDistMax = 0.0;
    uint32_t aimInN = 0, aimOutN = 0, rayVsN = 0;
    double aimInSum = 0.0, aimOutSum = 0.0, aimOutMax = 0.0, rayVsSum = 0.0;
    // F142: the ADS-on samples by reason (why[kWhyOk] = usable), those whose E has a non-finite padding lane (counted whether or not the sample was usable), and, per paired
    // sample, which candidate position of E lies within 50 cm of P14w (row 3, the one used, and column 3).
    uint32_t why[kWhyCount] = {};
    uint32_t ePadN = 0, posPairs = 0, posRowNear = 0, posColNear = 0;
    static constexpr double kNearM = 0.5;
    void reset() noexcept { *this = Acc{}; }
    // The samples the instrument rejected (every reason but usable and the game's own invalid handle) of the ADS-on ones.
    uint32_t rejected() const noexcept {
        uint32_t n = 0;
        for (unsigned i = 1; i < kWhyCount; ++i)
            if (i != kWhyHandle) n += why[i];
        return n;
    }
    // One local call: adsOn (data+0x10), the bend's geometry, and S against P14 when an FP snapshot was paired (gap: |sample frame - snapshot frame|).
    void add(bool adsOn, const BendIn& in, const BendOut& b, bool havePair, const P14Off& p, uint32_t gap) noexcept {
        ++samples;
        if (!adsOn) {
            ++adsOff;
            return;
        }
        ++why[b.why < kWhyCount ? b.why : kWhySNotRead];
        if (b.ePad) ++ePadN;
        if (havePair && p.ok) {
            ++posPairs;
            if (p.rowDist >= 0.0 && p.rowDist < kNearM) ++posRowNear;
            if (p.colDist >= 0.0 && p.colDist < kNearM) ++posColNear;
        }
        if (in.wOk && std::isfinite(in.w)) {
            const double w = static_cast<double>(in.w);
            wMin = wN ? std::min(wMin, w) : w;
            wMax = wN ? std::max(wMax, w) : w;
            ++wN;
            if (in.w >= 1.0f) ++wFull;
        }
        if (in.okRead) ++okReadN;
        if (in.okRead && in.ok) ++okN;
        if (b.applied) ++applied;
        if (b.geo) {
            ++geoN;
            distSum += b.dist;
            eyeRightSum += b.eyeRight;
            eyeUpSum += b.eyeUp;
            rayDegSum += b.rayDeg;
            if (havePair && p.ok) {
                ++p14N;
                if (gap == 0) ++p14Exact;
                frameGapSum += gap;
                p14Right += p.right;
                p14Up += p.up;
                p14Along += p.along;
                for (int i = 0; i < 3; ++i) eRel[i] += p.eRel[i];
                eDistSum += p.eDist;
                eDistMax = std::max(eDistMax, p.eDist);
            } else if (!havePair) {
                ++noFp;
            }
        }
        if (b.aimInDeg >= 0.0) { ++aimInN; aimInSum += b.aimInDeg; }
        if (b.aimOutDeg >= 0.0) { ++aimOutN; aimOutSum += b.aimOutDeg; aimOutMax = std::max(aimOutMax, b.aimOutDeg); }
        if (b.applied && b.aimOutVsRayDeg >= 0.0) { ++rayVsN; rayVsSum += b.aimOutVsRayDeg; }
    }
    static double mean(double sum, uint32_t n) noexcept { return n ? sum / n : 0.0; }
};

// ---- the readout's anchor (frame thread) -----------------------------------------------------------------------------------------------------------------
// 4x4 row-vector product a x b (a point p goes to p a b).
inline void mul4(const float a[16], const float b[16], double out[16]) noexcept {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) s += static_cast<double>(a[4 * i + k]) * static_cast<double>(b[4 * k + j]);
            out[4 * i + j] = s;
        }
}
// A rotation (rows, row-vector) as a unit quaternion (w, x, y, z); false for a degenerate one.
inline bool quatOf(const double R[9], double q[4]) noexcept {
    const double tr = R[0] + R[4] + R[8];
    double w, x, y, z;
    if (tr > 0.0) {
        const double s = std::sqrt(tr + 1.0) * 2.0;
        w = 0.25 * s; x = (R[5] - R[7]) / s; y = (R[6] - R[2]) / s; z = (R[1] - R[3]) / s;
    } else if (R[0] > R[4] && R[0] > R[8]) {
        const double s = std::sqrt(1.0 + R[0] - R[4] - R[8]) * 2.0;
        w = (R[5] - R[7]) / s; x = 0.25 * s; y = (R[1] + R[3]) / s; z = (R[2] + R[6]) / s;
    } else if (R[4] > R[8]) {
        const double s = std::sqrt(1.0 + R[4] - R[0] - R[8]) * 2.0;
        w = (R[6] - R[2]) / s; x = (R[1] + R[3]) / s; y = 0.25 * s; z = (R[5] + R[7]) / s;
    } else {
        const double s = std::sqrt(1.0 + R[8] - R[0] - R[4]) * 2.0;
        w = (R[1] - R[3]) / s; x = (R[2] + R[6]) / s; y = (R[5] + R[7]) / s; z = 0.25 * s;
    }
    const double n = std::sqrt(w * w + x * x + y * y + z * z);
    if (!(n > 1e-9) || !std::isfinite(n)) return false;
    q[0] = w / n; q[1] = x / n; q[2] = y / n; q[3] = z / n;
    return true;
}
// The angle between two unit quaternions' rotations, degrees.
inline double quatAngleDeg(const double a[4], const double b[4]) noexcept {
    double c = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    if (c > 1.0) c = 1.0;
    return 2.0 * std::acos(c) * kDeg;
}
// U against a frame F (row-vector, world): rel = U x F^-1 with F's rows made unit (a rigid frame). t: U's position less F's, in F's unit rows (m); q: U's rows
// (made unit) in F's unit rows, as a quaternion. False when either matrix is degenerate.
// F142: why a rel was not made: U's used lanes not numbers, a row of U with no length, or a row of the frame F with none (F is the weapon's or P14's world, not U).
enum RelWhy : uint8_t { kRelOk = 0, kRelUNonFinite, kRelUDegenerate, kRelFDegenerate, kRelWhyCount };
struct Rel {
    bool ok = false;
    uint8_t why = kRelOk;       // F142
    bool padNonFinite = false;  // F142: U's used lanes are numbers but a padding lane (3, 7, 11, 15) is not
    double t[3] = {};
    double q[4] = {};
};
inline Rel relTo(const float U[16], const double F[16]) noexcept {
    Rel r;
    r.padNonFinite = finiteUsed(U) && !finitePad(U);
    if (!finiteUsed(U)) {
        r.why = kRelUNonFinite;
        return r;
    }
    double fr[3][3], ur[3][3];
    for (int i = 0; i < 3; ++i) {
        const double a[3] = {F[4 * i], F[4 * i + 1], F[4 * i + 2]};
        if (!unit3(a, fr[i])) {
            r.why = kRelFDegenerate;
            return r;
        }
    }
    for (int i = 0; i < 3; ++i) {
        const double b[3] = {U[4 * i], U[4 * i + 1], U[4 * i + 2]};
        if (!unit3(b, ur[i])) {
            r.why = kRelUDegenerate;
            return r;
        }
    }
    const double d[3] = {U[12] - F[12], U[13] - F[13], U[14] - F[14]};
    for (int i = 0; i < 3; ++i) r.t[i] = dot3(d, fr[i]);
    double R[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) R[3 * i + j] = dot3(ur[i], fr[j]);
    if (!quatOf(R, r.q)) {
        r.why = kRelUDegenerate;
        return r;
    }
    r.ok = std::isfinite(r.t[0]) && std::isfinite(r.t[1]) && std::isfinite(r.t[2]);
    if (!r.ok) r.why = kRelFDegenerate;   // U's position is a number (used lanes), so F's position is not
    return r;
}
// F142: a window's rels by reason, for the readout's instrument line. U's reasons are the same for the weapon's rel and P14's (the same U), so they are counted once per row
// (from the weapon's); each frame's own failures are counted apart.
struct RelCounts {
    uint32_t rows = 0;                 // rows that reached the rel (a held item's, attachment read)
    uint32_t uNotRead = 0;             // rows whose attachment was not read (counted before the rel)
    uint32_t usable = 0;               // both rels made
    uint32_t uNonFinite = 0, uDegenerate = 0, padNonFinite = 0;
    uint32_t weaponFrame = 0, p14Frame = 0;   // the frame F had a degenerate row, for the weapon's rel and for P14's
    void reset() noexcept { *this = RelCounts{}; }
    void note(const Rel& rw, const Rel& rp) noexcept {
        ++rows;
        if (rw.padNonFinite) ++padNonFinite;
        if (rw.ok && rp.ok) {
            ++usable;
            return;
        }
        if (rw.why == kRelUNonFinite) ++uNonFinite;
        else if (rw.why == kRelUDegenerate || rp.why == kRelUDegenerate) ++uDegenerate;
        if (rw.why == kRelFDegenerate) ++weaponFrame;
        if (rp.why == kRelFDegenerate) ++p14Frame;
    }
    uint32_t rejected() const noexcept { return rows - usable; }
};
// A window's samples of one rel for one part: the mean translation, the largest distance from it, and the largest rotation from the mean rotation (the mean of
// the quaternions taken on the first one's side, made unit).
struct Spread {
    uint32_t n = 0;
    double mean[3] = {};
    double devMaxM = 0.0, rotDevMaxDeg = 0.0;
};
struct RelSample {
    float t[3];
    float q[4];
};
inline Spread spreadOf(const RelSample* s, uint32_t n) noexcept {
    Spread o;
    if (!s || !n) return o;
    o.n = n;
    double qm[4] = {};
    for (uint32_t k = 0; k < n; ++k) {
        for (int i = 0; i < 3; ++i) o.mean[i] += s[k].t[i];
        const double sgn = (s[k].q[0] * s[0].q[0] + s[k].q[1] * s[0].q[1] + s[k].q[2] * s[0].q[2] + s[k].q[3] * s[0].q[3]) < 0.0 ? -1.0 : 1.0;
        for (int i = 0; i < 4; ++i) qm[i] += sgn * s[k].q[i];
    }
    for (int i = 0; i < 3; ++i) o.mean[i] /= n;
    const double qn = std::sqrt(qm[0] * qm[0] + qm[1] * qm[1] + qm[2] * qm[2] + qm[3] * qm[3]);
    for (int i = 0; i < 4; ++i) qm[i] = qn > 0.0 ? qm[i] / qn : (i == 0 ? 1.0 : 0.0);
    for (uint32_t k = 0; k < n; ++k) {
        const double d[3] = {s[k].t[0] - o.mean[0], s[k].t[1] - o.mean[1], s[k].t[2] - o.mean[2]};
        o.devMaxM = std::max(o.devMaxM, len3(d));
        const double q[4] = {s[k].q[0], s[k].q[1], s[k].q[2], s[k].q[3]};
        o.rotDevMaxDeg = std::max(o.rotDevMaxDeg, quatAngleDeg(q, qm));
    }
    return o;
}

// One part's window: its samples against the weapon (W) and against P14, kept in two sets by the FP root's frame against the joints' (the same render frame, or
// one apart: F79's rule), its record flags (kFlagNotRead: not read; a value that changed within the window: kFlagMixed), and the rows it could not pair.
constexpr uint32_t kSampleCap = 1024;   // per set: 11 s at 90 Hz; a longer window's later frames count in `frames` and `paired` only
constexpr uint32_t kExactMin = 30;      // the same-frame set is the stats' basis alone from this many samples; below it, both sets
constexpr uint8_t kFlagNotRead = 0xFF, kFlagMixed = 0xFE;
constexpr int kFlags = 6;               // record +0x18, +0x19, +0x28, +0x29, +0xD0, +0x100 (the handle's first word set: 1, zero: 0)
struct PartSets {
    RelSample w[2][kSampleCap];         // [0] the root of the same render frame, [1] one frame apart
    RelSample p[2][kSampleCap];
};
struct Part {
    bool used = false;
    uint16_t key = 0;
    uint32_t lastFrame = ~0u;           // one sample per render frame
    uint32_t frames = 0, unpaired = 0, paired[2] = {};
    uint32_t n[2] = {};                 // samples stored per set (the same count for W and P14)
    uint8_t flag[kFlags] = {kFlagNotRead, kFlagNotRead, kFlagNotRead, kFlagNotRead, kFlagNotRead, kFlagNotRead};
    bool flagSeen = false;
    void noteFlags(const uint8_t f[kFlags]) noexcept {
        if (!flagSeen) {
            std::memcpy(flag, f, sizeof(flag));
            flagSeen = true;
            return;
        }
        for (int i = 0; i < kFlags; ++i)
            if (flag[i] != f[i]) flag[i] = kFlagMixed;
    }
    // One paired frame: set 0 (same frame) or 1 (one apart), both rels read. Stored while the set has room; counted either way.
    void add(PartSets& sets, int set, const Rel& rw, const Rel& rp) noexcept {
        if (set < 0 || set > 1 || !rw.ok || !rp.ok) return;
        ++paired[set];
        if (n[set] >= kSampleCap) return;
        RelSample& a = sets.w[set][n[set]];
        RelSample& b = sets.p[set][n[set]];
        for (int i = 0; i < 3; ++i) { a.t[i] = static_cast<float>(rw.t[i]); b.t[i] = static_cast<float>(rp.t[i]); }
        for (int i = 0; i < 4; ++i) { a.q[i] = static_cast<float>(rw.q[i]); b.q[i] = static_cast<float>(rp.q[i]); }
        ++n[set];
    }
    bool exact() const noexcept { return n[0] >= kExactMin; }
};
// A flag for the line: its value in decimal, "mixed", or "not read".
inline const char* flagText(uint8_t v, char buf[8]) noexcept {
    if (v == kFlagNotRead) return "not read";
    if (v == kFlagMixed) return "mixed";
    const unsigned n = v;
    int k = 0;
    if (n >= 100) buf[k++] = static_cast<char>('0' + n / 100);
    if (n >= 10) buf[k++] = static_cast<char>('0' + (n / 10) % 10);
    buf[k++] = static_cast<char>('0' + n % 10);
    buf[k] = 0;
    return buf;
}
// The spread of the part's rel to the weapon (weapon) or to P14 over its basis: the same-frame set alone when exact(), else both sets, copied into scratch
// (2 x kSampleCap).
inline Spread partSpread(const Part& part, const PartSets& sets, bool weapon, RelSample* scratch) noexcept {
    const RelSample* s0 = weapon ? sets.w[0] : sets.p[0];
    const RelSample* s1 = weapon ? sets.w[1] : sets.p[1];
    if (part.exact()) return spreadOf(s0, part.n[0]);
    uint32_t m = 0;
    for (uint32_t k = 0; k < part.n[0]; ++k) scratch[m++] = s0[k];
    for (uint32_t k = 0; k < part.n[1]; ++k) scratch[m++] = s1[k];
    return spreadOf(scratch, m);
}

}  // namespace f141
}  // namespace nofoot
}  // namespace edvr

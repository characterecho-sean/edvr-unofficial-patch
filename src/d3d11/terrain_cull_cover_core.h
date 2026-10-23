#pragma once
// The terrain cull cover (docs\terrain-culling.md, 2026-10-10; always on in VR, no key). Pure: numbers, bytes and formats, no Windows, so a rig
// drives every case (tools\terrain_cover_test). terrain_cull_cover_hook.cpp is the glue that finds the function, patches it and calls this.
//
// WHAT IS READ (build 334473, EliteDangerous64.exe; analysis\decomp\verify_20261010_tiles_*; READ = a disassembled instruction):
//   The planet draw-list builder 0x4462E80 calls 0x44587C0 (at 0x4463A83), which writes one record per render view. For a main (non-ortho) view it loads the view's eye camera
//   aspect and vertical fov and builds the four cull planes:
//     4458D5D  movss xmm0,[r14+0x70]   aspect  (the recommended W/H, the fov getter's out[0])
//     4458D6A  movss xmm1,[r14+0x90]   vFOV    (atan|top| + atan|bottom|, out[1])
//     4458D73  movss [rsp+0x28],xmm0 ; 4458D79  movss [rsp+0x20],xmm1 ; 4458D87  call 0x144441580   (rcx,rdx,r8,r9 = four output/scratch pointers)
//   so at the callee's entry arg 5 = vFOV at [rsp+0x28] and arg 6 = aspect at [rsp+0x30]. 0x4441580 (prologue below) halves the vFOV (mulss by the 0.5 at 0x4DE28B8), takes
//   a = vFOV/2, b = atan(aspect * tan a), and writes four planes through the eye: +-a about the camera's Z axis vertically, +-b horizontally. A CENTRED frustum, no margin. It is
//   the ONLY direct caller (a .text scan finds exactly 0x4458D87). The per-(patch, view) test (0x445BA40 -> 0x445CF40) culls a patch box against these with no epsilon. The
//   rendered image uses the explicit kind-5 projection matrix, not these values.
//
// SO an eye whose frustum is asymmetric is under-covered on its wide sides: the cull reaches +-a vertically and +-b horizontally about the camera's axis, symmetric, while the
//   image reaches atan(outer tangent) on the outer side and, on a headset whose up and down differ, further down than the half vertical angle (cropped Crystal Super: cull
//   +-45.83 v / +-46.80 h against an outer edge of 51.44; Quest 3 via VD: +-49.50 / +-47.78 against 54.00 outward and 55.00 down). A patch is dropped only when its whole box
//   lies in the uncovered band -- rare at rest, common as patches slide through the band on a head turn; flat ground has the largest patches.
//
// THE RULE (widen): with T = the largest vertical tangent and H = the largest horizontal tangent over both eyes of the frustum EDVR hands the game,
//   a_in = vFOV/2, b_in = atan(aspect * tan a_in); a_out = max(a_in, atan T); b_out = max(b_in, atan H); vFOV_out = 2 a_out; aspect_out = tan(b_out) / tan(a_out).
//   The callee recovers a = a_out and b = b_out. It never narrows (up to float rounding, 1e-7). Anything that is not a finite, in-range input, or no tangents known (flat, or before
//   the first VR frame), passes the arguments through unchanged.
//
// FLOWN 2026-10-10 (Quest 3 via Virtual Desktop, 90 Hz): 20 flashes in 5.7 minutes with the cover off, none in 3.5 minutes with it on; every call took the eye's own aspect 0.9412 and vFOV
//   99.00 degrees and left with 0.9638 and 110.00; no measurable cost. About 19 to 25 calls a frame: one per planet patch group per eye.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace tcover {

// ---- identity: build 334473 (the pair every build-keyed hook here checks) -----------------------------------------------------------------------------------------
constexpr uint32_t kExpectedTimestamp = 1791840573u;
constexpr uint32_t kExpectedImageSize = 104906752u;
constexpr uint32_t kTargetRva = 0x4441580u;   // 0x144441580: the cull-plane builder
constexpr uint32_t kCallRva = 0x4458D87u;     // the one call to it
constexpr uint32_t kSiteRva = 0x4458D5Du;     // the first byte of the call-site signature below (fragment A)

// The callee's first 32 bytes: mov rax,rsp / mov [rax+10h],rbx / mov [rax+18h],rbp / mov [rax+20h],rsi / push rdi / sub rsp,90h / movss xmm1,[rsp+0C0h] (= arg 5, the vFOV).
// CodeHook steals the first two instructions (7 bytes, nothing rip-relative). The mulss that follows (F3 0F 59 0D rel32, by 0.5) is found in the next 16 bytes and its operand
// read: the function that halves arg 5.
constexpr size_t kHeadBytes = 32;
constexpr uint8_t kHead[kHeadBytes] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20, 0x57,
                                       0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x8C, 0x24, 0xC0, 0x00, 0x00, 0x00};
constexpr size_t kHeadWindow = 48;            // bytes read at the callee: the head, then room for the mulss
constexpr uint8_t kMulss[4] = {0xF3, 0x0F, 0x59, 0x0D};

// The call site, as five fragments with the instructions the scheduler interleaves between them left free (at most kMaxGap bytes each time):
//   A movss xmm0,[r14+70h]    B movss xmm1,[r14+90h]    C movss [rsp+28h],xmm0    D movss [rsp+20h],xmm1    E call rel32
constexpr uint8_t kFragA[] = {0xF3, 0x41, 0x0F, 0x10, 0x46, 0x70};
constexpr uint8_t kFragB[] = {0xF3, 0x41, 0x0F, 0x10, 0x8E, 0x90, 0x00, 0x00, 0x00};
constexpr uint8_t kFragC[] = {0xF3, 0x0F, 0x11, 0x44, 0x24, 0x28};
constexpr uint8_t kFragD[] = {0xF3, 0x0F, 0x11, 0x4C, 0x24, 0x20};
constexpr uint32_t kMaxGap = 16;

inline uint32_t load32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline int32_t load32s(const uint8_t* p) { int32_t v; std::memcpy(&v, p, 4); return v; }
inline uint16_t load16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

// A mapped image by RVA. Every read is checked against the size first: a read past it is a refusal, not a fault.
struct Image {
    const uint8_t* base = nullptr;
    size_t size = 0;
    bool has(uint64_t rva, size_t n) const { return base && rva <= size && n <= size - rva; }
    const uint8_t* at(uint64_t rva) const { return base + rva; }
};

// The callee at `rva`: its head, then a mulss in the next 16 bytes whose operand is exactly 0.5f.
inline bool calleeMatches(const Image& img, uint32_t rva) {
    if (!img.has(rva, kHeadWindow)) return false;
    const uint8_t* p = img.at(rva);
    if (std::memcmp(p, kHead, kHeadBytes) != 0) return false;
    for (size_t k = kHeadBytes; k + 8 <= kHeadWindow; ++k) {
        if (std::memcmp(p + k, kMulss, sizeof(kMulss)) != 0) continue;
        const int64_t operand = int64_t(rva) + int64_t(k) + 8 + int64_t(load32s(p + k + 4));
        if (operand < 0 || !img.has(uint64_t(operand), 4)) return false;
        return load32(img.at(uint64_t(operand))) == 0x3F000000u;   // 0.5f
    }
    return false;
}

// The call-site chain starting at `rvaA`. On success *callRva is the E8 and *targetRva where it goes.
inline bool matchSite(const Image& img, uint32_t rvaA, uint32_t* callRva, uint32_t* targetRva) {
    if (!img.has(rvaA, sizeof(kFragA)) || std::memcmp(img.at(rvaA), kFragA, sizeof(kFragA)) != 0) return false;
    uint64_t pos = uint64_t(rvaA) + sizeof(kFragA);
    struct Fragment { const uint8_t* bytes; size_t n; };
    const Fragment rest[] = {{kFragB, sizeof(kFragB)}, {kFragC, sizeof(kFragC)}, {kFragD, sizeof(kFragD)}};
    for (const Fragment& f : rest) {
        bool found = false;
        for (uint32_t gap = 0; gap <= kMaxGap && !found; ++gap) {
            if (img.has(pos + gap, f.n) && std::memcmp(img.at(pos + gap), f.bytes, f.n) == 0) {
                pos += gap + f.n;
                found = true;
            }
        }
        if (!found) return false;
    }
    for (uint32_t gap = 0; gap <= kMaxGap; ++gap) {
        if (!img.has(pos + gap, 5) || img.at(pos + gap)[0] != 0xE8) continue;
        const int64_t target = int64_t(pos + gap) + 5 + int64_t(load32s(img.at(pos + gap) + 1));
        if (target < 0 || uint64_t(target) > img.size) return false;
        *callRva = uint32_t(pos + gap);
        *targetRva = uint32_t(target);
        return true;
    }
    return false;
}

struct SiteMatch {
    uint32_t siteRva = 0, callRva = 0, targetRva = 0;
};
// Every call site in [textRva, textRva + textSize), counted; the first `cap` are listed. The scan walks to each F3 and compares fragment A there.
inline size_t scanSites(const Image& img, uint32_t textRva, uint32_t textSize, SiteMatch* out, size_t cap) {
    size_t count = 0;
    if (!img.has(textRva, textSize) || textSize < sizeof(kFragA)) return 0;
    uint64_t r = textRva;
    const uint64_t end = uint64_t(textRva) + textSize;
    while (r + sizeof(kFragA) <= end) {
        const uint8_t* p = img.at(r);
        const void* hit = std::memchr(p, 0xF3, size_t(end - r - sizeof(kFragA) + 1));
        if (!hit) break;
        r += size_t(static_cast<const uint8_t*>(hit) - p);
        uint32_t call = 0, target = 0;
        if (matchSite(img, uint32_t(r), &call, &target)) {
            if (count < cap) out[count] = SiteMatch{uint32_t(r), call, target};
            ++count;
        }
        ++r;
    }
    return count;
}

// ---- finding it -------------------------------------------------------------------------------------------------------------------------------------------------
struct Located {
    uint32_t targetRva = 0, callRva = 0;
    bool byStamp = false;   // found at the build's known address (the PE stamp matched), not by scanning
    uint32_t stamp = 0;
    char how[400] = {};     // when found: the log's "(<how found>)"
    char why[400] = {};     // when not: the log's reason
};

// Parse the PE header at `base` (at least 0x400 bytes readable) and find the cull-plane builder. `knownSize` bounds every read (0: the header's SizeOfImage). Reads only inside the
// image; a read the caller's SEH has to absorb is the caller's (the glue guards the call).
inline bool locate(const uint8_t* base, size_t knownSize, Located* out) {
    *out = Located{};
    if (!base) { std::snprintf(out->why, sizeof(out->why), "the game's module could not be resolved"); return false; }
    const uint32_t peOff = load32(base + 0x3C);
    if (peOff < 0x40 || peOff > 0x300) { std::snprintf(out->why, sizeof(out->why), "the PE header offset is implausible"); return false; }
    if (std::memcmp(base + peOff, "PE\0\0", 4) != 0 || load16(base + peOff + 24) != 0x20B) {
        std::snprintf(out->why, sizeof(out->why), "the module is not a PE32+ image");
        return false;
    }
    const uint16_t sections = load16(base + peOff + 6);
    const uint16_t optionalSize = load16(base + peOff + 20);
    out->stamp = load32(base + peOff + 8);
    const uint32_t sizeOfImage = load32(base + peOff + 0x50);
    Image img;
    img.base = base;
    img.size = knownSize ? knownSize : sizeOfImage;
    if (sizeOfImage < 0x1000 || img.size < 0x1000) { std::snprintf(out->why, sizeof(out->why), "the image size is implausible"); return false; }

    if (out->stamp == kExpectedTimestamp && sizeOfImage == kExpectedImageSize) {
        uint32_t call = 0, target = 0;
        if (!calleeMatches(img, kTargetRva)) {
            std::snprintf(out->why, sizeof(out->why), "the PE stamp %u and image size %u are build 334473's, but the head at exe+0x%X is not the cull-plane builder's", out->stamp, sizeOfImage, kTargetRva);
            return false;
        }
        if (!matchSite(img, kSiteRva, &call, &target) || call != kCallRva || target != kTargetRva) {
            std::snprintf(out->why, sizeof(out->why), "the PE stamp %u and image size %u are build 334473's, but the call site at exe+0x%X is not the one read", out->stamp, sizeOfImage, kSiteRva);
            return false;
        }
        out->targetRva = kTargetRva;
        out->callRva = call;
        out->byStamp = true;
        std::snprintf(out->how, sizeof(out->how), "PE stamp %u and image size %u are build 334473's; the callee's head and the call site at exe+0x%X verified", out->stamp, sizeOfImage, kSiteRva);
        return true;
    }

    // Another build: scan the code section for the call-site signature, and accept one match whose callee has the same head.
    uint32_t textRva = 0, textSize = 0;
    const size_t tableAt = size_t(peOff) + 24 + optionalSize;
    bool named = false;   // a section called .text wins; otherwise the largest section that is code and executable
    for (uint16_t i = 0; i < sections && i < 96; ++i) {
        const size_t s = tableAt + size_t(i) * 40;
        if (s + 40 > 0x400 || s + 40 > img.size) break;
        const uint8_t* h = base + s;
        const uint32_t virtualSize = load32(h + 8), virtualAddress = load32(h + 12), characteristics = load32(h + 36);
        const bool isText = std::memcmp(h, ".text\0\0\0", 8) == 0;
        const bool isCode = (characteristics & 0x20000020u) == 0x20000020u;
        if ((isText && !named) || (!named && isCode && virtualSize > textSize)) {
            textRva = virtualAddress;
            textSize = virtualSize;
            named = isText;
        }
    }
    if (!textSize || !img.has(textRva, textSize)) {
        std::snprintf(out->why, sizeof(out->why), "the PE stamp %u is not build 334473's and no code section could be read for the signature scan", out->stamp);
        return false;
    }
    SiteMatch found[4];
    const size_t count = scanSites(img, textRva, textSize, found, 4);
    if (count != 1) {
        std::snprintf(out->why, sizeof(out->why), "the PE stamp %u is not build 334473's and the signature scan of the %u KB code section found %s", out->stamp, textSize / 1024u,
                      count == 0 ? "no call site" : "more than one call site (ambiguous)");
        return false;
    }
    if (!calleeMatches(img, found[0].targetRva)) {
        std::snprintf(out->why, sizeof(out->why), "the PE stamp %u is not build 334473's; the one call site at exe+0x%X reaches exe+0x%X, whose head is not the cull-plane builder's", out->stamp,
                      found[0].callRva, found[0].targetRva);
        return false;
    }
    out->targetRva = found[0].targetRva;
    out->callRva = found[0].callRva;
    std::snprintf(out->how, sizeof(out->how), "found by signature scan: the one call site at exe+0x%X reaches it; PE stamp %u is not build 334473's", found[0].callRva, out->stamp);
    return true;
}

// ---- the rule ---------------------------------------------------------------------------------------------------------------------------------------------------
struct Cover {
    float vfov, aspect;   // radians, as the callee's arg 5 and arg 6
    bool widened;
};
constexpr double kWidenEpsilon = 1e-5;   // radians

// Pure, thread-safe. `tanV` / `tanH`: the largest vertical / horizontal tangent over both eyes of the frustum handed to the game, 0 when unknown.
inline Cover widen(float vfov, float aspect, float tanV, float tanH) {
    Cover r{vfov, aspect, false};
    if (!(tanV > 0.0f) || !(tanH > 0.0f) || !std::isfinite(tanV) || !std::isfinite(tanH)) return r;
    if (!std::isfinite(vfov) || !std::isfinite(aspect) || !(aspect > 0.0f)) return r;
    const double halfPi = 1.57079632679489661923;
    const double aIn = double(vfov) * 0.5;
    if (!(aIn > 0.0 && aIn < halfPi)) return r;
    const double bIn = std::atan(double(aspect) * std::tan(aIn));
    // An axis is widened only when the tangents reach past it by more than a float's rounding of the game's own sums (1e-5 rad, 0.0006 degree: well under a pixel), so a
    // headset whose frustum the cull already covers passes through bit for bit and the census' `widened` means widened.
    const double aT = std::atan(double(tanV)), bT = std::atan(double(tanH));
    const bool widenA = aT > aIn + kWidenEpsilon, widenB = bT > bIn + kWidenEpsilon;
    if (!widenA && !widenB) return r;
    const double aOut = widenA ? aT : aIn;
    const double bOut = widenB ? bT : bIn;
    const double vOut = 2.0 * aOut;
    const double sOut = std::tan(bOut) / std::tan(aOut);
    const float vf = float(vOut), sf = float(sOut);
    if (!std::isfinite(vf) || !std::isfinite(sf) || !(sf > 0.0f) || vf < vfov) return r;
    r.vfov = vf;
    r.aspect = sf;
    r.widened = true;
    return r;
}

// T and H from the frusta handed to the game ({left, right, top, bottom} tangents per eye, signs as the runtime has them: only the magnitudes matter). False when any is not a
// finite non-zero number.
inline bool frustaTangents(const float frusta[2][4], float* tanV, float* tanH) {
    float t = 0.0f, h = 0.0f;
    for (int e = 0; e < 2; ++e) {
        for (int i = 0; i < 4; ++i) {
            if (!std::isfinite(frusta[e][i]) || frusta[e][i] == 0.0f) return false;
        }
        t = std::max(t, std::max(std::fabs(frusta[e][2]), std::fabs(frusta[e][3])));
        h = std::max(h, std::max(std::fabs(frusta[e][0]), std::fabs(frusta[e][1])));
    }
    *tanV = t;
    *tanH = h;
    return true;
}

// T and H in one 64-bit word for an atomic (0: unknown).
inline uint64_t packTangents(float tanV, float tanH) {
    uint32_t a, b;
    std::memcpy(&a, &tanV, 4);
    std::memcpy(&b, &tanH, 4);
    return (uint64_t(a) << 32) | b;
}
inline void unpackTangents(uint64_t word, float* tanV, float* tanH) {
    const uint32_t a = uint32_t(word >> 32), b = uint32_t(word);
    std::memcpy(tanV, &a, 4);
    std::memcpy(tanH, &b, 4);
}

// ---- the lines --------------------------------------------------------------------------------------------------------------------------------------------------
constexpr uint64_t kHeartbeatMs = 600000;   // ten minutes, the vertex resync's cadence
constexpr double kRadToDeg = 57.29577951308232;

inline void formatLive(char* out, size_t cap, uint32_t rva, const char* how) {
    std::snprintf(out, cap, "terrain cull cover: hook live at exe+0x%X (%s)", rva, how);
}
inline void formatInert(char* out, size_t cap, const char* why) {
    std::snprintf(out, cap, "terrain cull cover: inert: %s", why);
}
// The first call made with the frusta known (a call before the first VR frame has no tangents and is not the one worth a line).
inline void formatFirstCall(char* out, size_t cap, float inAspect, float inVfov, float outAspect, float outVfov, bool widened, float tanV, float tanH) {
    std::snprintf(out, cap, "terrain cull cover: first call: in aspect %.4f vfov %.2f deg -> out aspect %.4f vfov %.2f deg (%s); T=%.4f H=%.4f", double(inAspect), double(inVfov) * kRadToDeg,
                  double(outAspect), double(outVfov) * kRadToDeg, widened ? "widened" : "unchanged", double(tanV), double(tanH));
}
struct Last {
    bool have = false;
    float inAspect = 0, inVfov = 0, outAspect = 0, outVfov = 0;
};
// Every ten minutes while the hook is live, zero counts included: the line that tells a hook that was reached and had nothing to widen from one that was never reached.
inline void formatHeartbeat(char* out, size_t cap, uint64_t widened, uint64_t calls, const Last& last) {
    char seen[160];
    if (last.have)
        std::snprintf(seen, sizeof(seen), "last in aspect %.4f vfov %.2f deg -> out aspect %.4f vfov %.2f deg", double(last.inAspect), double(last.inVfov) * kRadToDeg, double(last.outAspect),
                      double(last.outVfov) * kRadToDeg);
    else
        std::snprintf(seen, sizeof(seen), "no call yet");
    std::snprintf(out, cap, "terrain cull cover: armed; %llu of %llu calls widened in the last 10 min (%s)", static_cast<unsigned long long>(widened), static_cast<unsigned long long>(calls),
                  seen);
}

}  // namespace tcover
}  // namespace edvr
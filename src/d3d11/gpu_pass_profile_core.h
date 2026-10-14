#pragma once
// Pure logic for the GPU pass profiler (gpu_pass_profile.h): what a pass IS
// (classification), how cube-face runs group, the stamp budget, the per-pass
// shader tally, the summary arithmetic and the CSV text. Nothing here touches
// D3D11 or Win32, so tools\gpu_pass_profile_test compiles it alone. The rules
// are stated in docs\gpu-pass-profile-2026-10-12.md; change them there too.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace edvr::gpupass {

constexpr uint32_t kDefaultCaptureFrames = 30; // whole frames per press when the hotkey's value names no count
constexpr uint32_t kMaxCaptureFrames = 120;    // the cap on a requested count; the pool is sized to what is captured
constexpr uint32_t kMaxStampsPerFrame = 4096;  // timestamp queries per frame; a boundary past the cap is counted, never issued
constexpr uint32_t kHashSlots = 8;             // distinct VS/PS pairs (or CS hashes) tracked per pass; the rest fold into one count
constexpr uint32_t kTopHashes = 3;             // shader pairs listed per pass
constexpr uint32_t kTopPasses = 20;            // passes in the summary's top list, by median
constexpr uint32_t kCubeRun = 6;               // consecutive same-size square passes read as one cube map
// GPU pass profile v3: the scope slots the profiler's frames take (one per captured frame, slot = frame index). Must equal gpu_timing.h's
// kPassScopeSlots; gpu_pass_profile.cpp static_asserts it. v2 had 4 here and every frame from the fifth was refused.
constexpr uint32_t kPassScopeSlotCount = 128;
// A boundary that finds the shared clock held by another scope waits for it, up to this many boundaries (about 2.7 s at 90 Hz), then the
// batch ends with a note. A holder that ends in time is not a failure: the frame starts at the boundary after it ends.
constexpr uint32_t kMaxStartWaits = 240;
// probe_cube (GPU pass profile v3): a colour target that is square and at most kProbeMaxSide on a side, or a cube array of kCubeArraySlices or
// more slices. Squareness alone is not a probe: the eye render is square too (2016x2016 in the v2 flight).
constexpr uint32_t kProbeMinSide = 256;     // v4: a probe's square side is at least this (the 252x252 exposure passes are below it)
constexpr uint32_t kProbeMaxSide = 1024;
constexpr uint32_t kCubeArraySlices = 6;
// v4: the exposure reduction chain's target (Elite's, measured: three 252x252 post passes per eye, with two compute shaders; audit
// reviews\vr-view-caches-2026-10-13.md candidate 1). Not a probe, whatever its shape rule says.
constexpr uint32_t kExposureSide = 252;
// v5 (delay): the front-loaded delay's default and clamp, in milliseconds; a gap above kLargeGapMs is "large"; a pass is suspect when it
// takes more than kSuspectFactor times its own median and is followed by a large gap.
constexpr uint32_t kDefaultDelayMs = 6;
constexpr uint32_t kMinDelayMs = 2;
constexpr uint32_t kMaxDelayMs = 12;
constexpr double kLargeGapMs = 0.25;
constexpr double kSuspectFactor = 3.0;

// v5 (delay): the hotkey's value is "KEY", "KEY COUNT" or "KEY COUNT DELAY_MS". Up to two whole numbers may trail the key, separated by
// whitespace: the first is the frame count (1..kMaxCaptureFrames, default kDefaultCaptureFrames) and the second the front-loaded delay
// (kMinDelayMs..kMaxDelayMs, default kDefaultDelayMs). A number is not taken when no key precedes it, so a key named by a digit alone
// still binds. key is the value without the numbers (what the hotkey binds).
struct CaptureSpec {
    std::string key;
    uint32_t frames = kDefaultCaptureFrames;
    uint32_t delayMs = kDefaultDelayMs;
};
inline uint32_t clampFrames(unsigned long n) {
    if (n < 1) return 1;
    return n > kMaxCaptureFrames ? kMaxCaptureFrames : static_cast<uint32_t>(n);
}
inline uint32_t clampDelayMs(unsigned long n) {
    if (n < kMinDelayMs) return kMinDelayMs;
    return n > kMaxDelayMs ? kMaxDelayMs : static_cast<uint32_t>(n);
}
inline CaptureSpec parseCaptureSpec(const std::string& value) {
    std::vector<std::string> tok;
    std::string cur;
    for (char c : value) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) tok.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) tok.push_back(cur);
    auto isNumber = [](const std::string& s) {
        if (s.empty() || s.size() > 6) return false;
        for (char c : s)
            if (c < '0' || c > '9') return false;
        return true;
    };
    size_t end = tok.size();
    int trailing = 0;
    while (trailing < 2 && end > 1 && isNumber(tok[end - 1])) {
        --end;
        ++trailing;
    }
    CaptureSpec spec;
    for (size_t i = 0; i < end; ++i) {
        if (i) spec.key += ' ';
        spec.key += tok[i];
    }
    if (trailing >= 1) spec.frames = clampFrames(std::stoul(tok[end]));
    if (trailing == 2) spec.delayMs = clampDelayMs(std::stoul(tok[end + 1]));
    return spec;
}
// The frame count and key only (the v2 interface, kept for its callers and rig).
inline uint32_t splitCaptureSpec(const std::string& value, std::string& keyOut) {
    const CaptureSpec spec = parseCaptureSpec(value);
    keyOut = spec.key;
    return spec.frames;
}
// v5: the loop count whose dispatch takes about delayMs, from one measured iteration cost (ms per iteration). 0 when the cost is unknown.
inline uint32_t delayIterations(double msPerIteration, uint32_t delayMs) {
    if (!(msPerIteration > 0.0)) return 0;
    const double n = static_cast<double>(delayMs) / msPerIteration;
    if (n < 1.0) return 1;
    return n > 4.0e9 ? 4000000000u : static_cast<uint32_t>(n + 0.5);
}

// The timestamp pool holds one stamp array per captured frame. Pool frames to create for a capture of `frames`: the capture's own count,
// clamped the same way. A pool already at least that large is reused.
inline uint32_t poolFramesFor(uint32_t frames) {
    if (frames < 1) return 1;
    return frames > kMaxCaptureFrames ? kMaxCaptureFrames : frames;
}
inline bool poolNeedsGrowth(uint32_t have, uint32_t frames) { return have < poolFramesFor(frames); }

enum class PassClass : uint8_t {
    Edvr = 0,         // inside an EDVR census section: EDVR's own work, excluded from Elite's total
    Compute,          // dispatches and no draws
    Clear,            // clears and no draws
    Shadow,           // depth only, square, >= 512 or an array of slices
    ProbeCube,        // v3: a colour target that is square and at most 1024, or a cube array (6+ slices), not at the eye size; or one of kCubeRun such face passes in a row
    SceneGBuffer,     // two or more colour targets at the eye size, or a depth-only pass at the eye size
    FullscreenAtEye,  // one colour target at the eye size with one or two draws (a full-screen triangle or quad)
    ForwardAtEye,     // one colour target at the eye size with three or more draws
    PostSmaller,      // draws into a colour target smaller than the eye (downsampled post chains)
    HudOverlay,       // Built v13.7: one to kHudMaxDraws draws whose most-drawn VS is a known HUD, holo or UI family (the caller's families)
    LightingVolume,   // instanced light volumes drawn into the eye-size target (the caller's lighting family, most-drawn VS)
    Exposure,         // v4: Elite's exposure reduction chain: the 252x252 single-target passes, and the dispatches whose top compute shader is the caller's exposure family
    Other,
    Count
};
inline constexpr const char* kPassClassNames[] = {
    "edvr", "compute", "clear", "shadow", "probe_cube", "scene_gbuffer",
    "fullscreen_at_eye", "forward_at_eye", "post_smaller", "hud_overlay", "lighting_volume", "exposure", "other"};
static_assert(sizeof(kPassClassNames) / sizeof(kPassClassNames[0]) == static_cast<size_t>(PassClass::Count),
              "one name for each pass class");

inline const char* passClassName(PassClass c) {
    const size_t i = static_cast<size_t>(c);
    return i < static_cast<size_t>(PassClass::Count) ? kPassClassNames[i] : "other";
}

// One bound target, as the classifier and the CSV need it. w and h are the
// view's own mip size; slices is the array size the view addresses (1 for a
// plain 2D view); cube is whether the underlying resource is a cube map.
struct TargetShape {
    uint32_t w = 0, h = 0;
    uint32_t format = 0;   // DXGI_FORMAT
    uint32_t slices = 1;
    bool cube = false;
    bool square() const { return w != 0 && w == h; }
};

// Everything a pass segment has accumulated, before classification.
struct PassFacts {
    bool edvr = false;          // inside an EDVR census section
    uint32_t colours = 0;       // non-null colour targets bound
    TargetShape colour;         // the first of them
    bool hasDepth = false;
    TargetShape depth;
    uint32_t draws = 0, dispatches = 0, clears = 0;
    uint64_t topVs = 0;         // Built v13.7: the segment's most-drawn vertex shader (its content hash); 0 when it drew nothing
    uint64_t topCs = 0;         // v4: the segment's most-dispatched compute shader (its content hash); 0 when it dispatched nothing
};

// A set of vertex shader hashes a class is known by, taken by the caller from the game headers (eye_draw_snapshot.h, holo_material.h,
// holo_families.h, and the lighting shapes of reviews\sealed-remaining-costs-2026-10-13.md) so this core stays free of them.
constexpr uint32_t kFamilySlots = 4;
constexpr uint32_t kHudMaxDraws = 4;        // a HUD overlay is a few draws: more than this is a forward or post pass
struct VsFamily {
    uint64_t vs[kFamilySlots] = {};
    uint32_t n = 0;
};
inline bool isVsFamily(uint64_t vs, const VsFamily& fam) noexcept {
    if (vs == 0) return false;
    for (uint32_t i = 0; i < fam.n && i < kFamilySlots; ++i)
        if (fam.vs[i] == vs) return true;
    return false;
}

// Built v13.7 (1): a known compute shader, by its content hash, from the repo's own catalogues (skin_ledger.h's kChainHash and kClearHash, given
// by the caller).
struct ComputeName {
    uint64_t hash = 0;
    const char* name = "";
};

struct EyeSize {
    uint32_t w = 0, h = 0;
};

// The eye size is the headset's eye texture (vscreen's eyeW and eyeH, from openvr_api.dll when it is there). A target at that size is never
// a probe, whatever its shape.
inline bool atEye(const TargetShape& t, EyeSize eye) { return eye.w != 0 && t.w == eye.w && t.h == eye.h; }
// v4: a square of kProbeMinSide..kProbeMaxSide on a side, or a cube array (kCubeArraySlices or more slices).
inline bool isProbeShape(const TargetShape& t, EyeSize eye) {
    if (!t.square() || atEye(t, eye)) return false;
    return (t.w >= kProbeMinSide && t.w <= kProbeMaxSide) || t.slices >= kCubeArraySlices;
}
// v4: the exposure chain's single-target pass (the 252x252 post passes).
inline bool isExposureShape(const TargetShape& t) { return t.square() && t.w == kExposureSide && t.h == kExposureSide; }

// The largest colour target whose depth is the same size, over one frame's segments: the eye render in the usual frame. {0,0} when none.
// Reported beside the eye size, so a flight can see when they disagree.
inline EyeSize matchedEye(const std::vector<PassFacts>& facts) {
    EyeSize best{};
    for (const PassFacts& f : facts) {
        if (!f.colours || !f.hasDepth || f.colour.w != f.depth.w || f.colour.h != f.depth.h) continue;
        if (static_cast<uint64_t>(f.colour.w) * f.colour.h > static_cast<uint64_t>(best.w) * best.h) best = EyeSize{f.colour.w, f.colour.h};
    }
    return best;
}

inline std::string targetText(const TargetShape& t) {
    if (!t.w) return "-";
    return std::to_string(t.w) + "x" + std::to_string(t.h);
}

// The class of one segment. Order matters: the first rule that holds wins.
// exposure: the compute shaders of the exposure chain (a VsFamily, read as a set of content hashes; PassFacts::topCs is checked against it).
inline PassClass classifyPass(const PassFacts& f, EyeSize eye, const VsFamily& hud = VsFamily{},
                              const VsFamily& lighting = VsFamily{}, const VsFamily& exposure = VsFamily{}) {
    if (f.edvr) return PassClass::Edvr;
    if (f.draws == 0 && f.dispatches > 0)
        return isVsFamily(f.topCs, exposure) ? PassClass::Exposure : PassClass::Compute;
    if (f.draws == 0) return f.clears > 0 ? PassClass::Clear : PassClass::Other;
    // v4: a single 252x252 target is the exposure chain's post pass, not a probe (the 2026-10-13 flights: 24 per capture).
    if (f.colours == 1 && isExposureShape(f.colour)) return PassClass::Exposure;
    const bool eyeColour = eye.w != 0 && f.colour.w == eye.w && f.colour.h == eye.h;
    if (f.colours == 0 && f.hasDepth) {
        // Built v13.7 (2): a depth-only map that is not at the eye size is a shadow map, square or not, large or small (the 2048x1024 D16 pair and
        // the 256x256 D16 map were other). With no eye size known, only a square map of 512 or more, or an array of slices, is one.
        if (eye.w != 0 && f.depth.w == eye.w && f.depth.h == eye.h) return PassClass::SceneGBuffer;
        if (eye.w != 0) return PassClass::Shadow;
        if (f.depth.square() && (f.depth.w >= 512 || f.depth.slices > 1)) return PassClass::Shadow;
        return PassClass::Other;
    }
    // Built v13.7 (3): a few draws whose most-drawn vertex shader is a known HUD, holo or UI family: its own class, so the HUD and holo cost is
    // not counted as fullscreen_at_eye.
    if (f.colours >= 1 && f.draws <= kHudMaxDraws && isVsFamily(f.topVs, hud)) return PassClass::HudOverlay;
    if (f.colours >= 1 && isProbeShape(f.colour, eye)) return PassClass::ProbeCube;
    // The light volumes are instanced draws into the eye-size target, so the lighting family is tested before the
    // two-target rule that would otherwise name the lighting pass scene_gbuffer.
    if (eyeColour && isVsFamily(f.topVs, lighting)) return PassClass::LightingVolume;
    if (f.colours >= 2 && eyeColour) return PassClass::SceneGBuffer;
    if (eyeColour) return f.draws <= 2 ? PassClass::FullscreenAtEye : PassClass::ForwardAtEye;
    if (eye.w != 0 && f.colours >= 1 &&
        static_cast<uint64_t>(f.colour.w) * f.colour.h < static_cast<uint64_t>(eye.w) * eye.h)
        return PassClass::PostSmaller;
    return PassClass::Other;
}

// A cube map drawn face by face is kCubeRun or more consecutive passes, each
// one colour target, square, drawn into, all the same size. Relabels those
// runs ProbeCube in place. cls[i] is the class of facts[i].
// v3: a candidate must be a probe shape (not at the eye size, square and small or a cube array). The v2 rule took any square single-target
// pass, so six eye passes in a row (2016x2016, square) were relabelled probe_cube.
inline bool cubeCandidate(const PassFacts& f, EyeSize eye) {
    return !f.edvr && f.colours == 1 && f.draws > 0 && isProbeShape(f.colour, eye);
}
inline void markCubeRuns(const std::vector<PassFacts>& facts, std::vector<PassClass>& cls, EyeSize eye = EyeSize{}) {
    size_t i = 0;
    while (i < facts.size()) {
        if (!cubeCandidate(facts[i], eye)) { ++i; continue; }
        size_t j = i + 1;
        while (j < facts.size() && cubeCandidate(facts[j], eye) && facts[j].colour.w == facts[i].colour.w) ++j;
        if (j - i >= kCubeRun)
            for (size_t k = i; k < j; ++k) cls[k] = PassClass::ProbeCube;
        i = j;
    }
}

// One stamp budget per frame. take(reserve) issues a stamp only while at
// least reserve stamps stay free after it, so a boundary (reserve 1) can never
// take the slot the frame's end needs (reserve 0). A refused stamp is counted.
struct StampBudget {
    uint32_t cap = kMaxStampsPerFrame;
    uint32_t used = 0, overflow = 0;
    bool take(uint32_t reserve) {
        if (used + reserve >= cap) { ++overflow; return false; }
        ++used;
        return true;
    }
};

struct HashTally {
    uint64_t vs = 0, ps = 0;
    uint32_t draws = 0;
};

// The shader pairs a pass drew with, by draw count. Eight slots; a pair past
// them is folded into one count, so a pass that uses many shaders still
// reports its top three correctly for the heavy ones.
class PassHashes {
public:
    void note(uint64_t vs, uint64_t ps) {
        for (uint32_t i = 0; i < used_; ++i)
            if (slots_[i].vs == vs && slots_[i].ps == ps) { ++slots_[i].draws; return; }
        if (used_ < kHashSlots) { slots_[used_++] = HashTally{vs, ps, 1}; return; }
        ++folded_;
    }
    uint32_t folded() const { return folded_; }
    // Every tracked pair (not only the top kTopHashes), for the summary's totals.
    std::vector<HashTally> all() const { return std::vector<HashTally>(slots_, slots_ + used_); }
    std::vector<HashTally> top() const {
        std::vector<HashTally> out(slots_, slots_ + used_);
        std::stable_sort(out.begin(), out.end(),
                         [](const HashTally& a, const HashTally& b) { return a.draws > b.draws; });
        if (out.size() > kTopHashes) out.resize(kTopHashes);
        return out;
    }
private:
    HashTally slots_[kHashSlots]{};
    uint32_t used_ = 0, folded_ = 0;
};

// One row of the CSV: one pass segment of one captured frame.
struct Row {
    uint32_t frame = 0, index = 0;
    PassClass cls = PassClass::Other;
    bool edvr = false;
    double ms = 0;
    uint32_t draws = 0, dispatches = 0, clears = 0;
    uint64_t tris = 0;          // estimate: count / 3 x instances for the triangle draws; 0 otherwise
    uint32_t colours = 0;
    std::string rt = "-", dsv = "-", top = "-";
    std::string topCs = "-";   // Built v13.7 (1): the segment's most-dispatched compute shaders, "hash(NAME):count;..."; "-" when none
    // The pass's shader identity without counts, so the same pass in another frame groups with it: "vs:ps" of the most-drawn pair, or
    // "cs:hash(NAME)" of the most-dispatched compute shader for a dispatch-only pass; "-" when neither.
    std::string ident = "-";
    std::vector<HashTally> cs;   // every compute shader this pass dispatched with, by dispatch count (the summary's top_cs_dispatches totals)
    double gapAfterMs = 0;       // v5: the no-work time (empty boundary segments) directly after this pass, before the next pass
    bool suspect = false;        // v5: markSuspects: over kSuspectFactor times the pass's own median, and followed by a large gap
    uint32_t rtFormat = 0, dsvFormat = 0, slices = 1;
    bool cube = false;
    // The grouping key: class, target signature and shader identity, not the frame or the index.
    std::string key() const {
        return std::string(passClassName(cls)) + " rt=" + rt + " colours=" + std::to_string(colours) +
               " slices=" + std::to_string(slices) + (cube ? " cube" : "") + " dsv=" + dsv + " shader=" + ident;
    }
};

inline std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

inline const char* csvHeader() {
    return "frame,pass,class,edvr,ms,draws,dispatches,clears,tris,colours,rt,rt_fmt,slices,cube,dsv,dsv_fmt,top_vs_ps_draws,top_cs_dispatches,gap_after_ms,suspect";
}

inline std::string csvRow(const Row& r) {
    char ms[32];
    std::snprintf(ms, sizeof ms, "%.4f", r.ms);
    std::string out;
    out += std::to_string(r.frame) + "," + std::to_string(r.index) + "," + passClassName(r.cls) + "," +
           (r.edvr ? "1" : "0") + "," + ms + "," + std::to_string(r.draws) + "," +
           std::to_string(r.dispatches) + "," + std::to_string(r.clears) + "," + std::to_string(r.tris) + "," +
           std::to_string(r.colours) + "," + csvField(r.rt) + "," + std::to_string(r.rtFormat) + "," +
           std::to_string(r.slices) + "," + (r.cube ? "1" : "0") + "," + csvField(r.dsv) + "," +
           std::to_string(r.dsvFormat) + "," + csvField(r.top) + "," + csvField(r.topCs);
    char gap[32];
    std::snprintf(gap, sizeof gap, "%.4f", r.gapAfterMs);
    out += std::string(",") + gap + (r.suspect ? ",1" : ",0");
    return out;
}

// "vs:ps:draws;..." with the hashes in hex. "-" when the pass drew nothing.
inline std::string topText(const std::vector<HashTally>& top) {
    if (top.empty()) return "-";
    std::string out;
    char buf[64];
    for (size_t i = 0; i < top.size(); ++i) {
        if (i) out += ';';
        std::snprintf(buf, sizeof buf, "%016llx:%016llx:%u", static_cast<unsigned long long>(top[i].vs),
                      static_cast<unsigned long long>(top[i].ps), top[i].draws);
        out += buf;
    }
    return out;
}

// Built v13.7 (1): "hash(NAME):count;..." for the compute shaders a pass dispatched with, most dispatches first. The name is in parentheses when the
// caller's catalogue knows the hash; the hash is the bound compute shader's content hash in hex.
inline std::string csText(const std::vector<HashTally>& top, const ComputeName* names, size_t nNames) {
    if (top.empty()) return "-";
    std::string out;
    for (size_t i = 0; i < top.size(); ++i) {
        if (i) out += ';';
        const char* name = "";
        for (size_t k = 0; k < nNames; ++k)
            if (names[k].hash == top[i].vs) {
                name = names[k].name;
                break;
            }
        char buf[96];
        if (*name)
            std::snprintf(buf, sizeof buf, "%016llx(%s):%u", static_cast<unsigned long long>(top[i].vs), name, top[i].draws);
        else
            std::snprintf(buf, sizeof buf, "%016llx:%u", static_cast<unsigned long long>(top[i].vs), top[i].draws);
        out += buf;
    }
    return out;
}

// GPU pass profile v2: the shader identity a pass groups by (Row::ident). A draw pass names its most-drawn VS:PS pair; a dispatch-only pass
// (no draws) names its most-dispatched compute shader, with the name when the catalogue knows it. A zero hash is never named: it is "-".
inline std::string identText(const std::vector<HashTally>& pairs, const std::vector<HashTally>& cs, const ComputeName* names,
                             size_t nNames) {
    char buf[96];
    if (!pairs.empty() && pairs[0].vs) {
        std::snprintf(buf, sizeof buf, "%016llx:%016llx", static_cast<unsigned long long>(pairs[0].vs),
                      static_cast<unsigned long long>(pairs[0].ps));
        return buf;
    }
    if (!cs.empty() && cs[0].vs) {
        const char* name = "";
        for (size_t k = 0; k < nNames; ++k)
            if (names[k].hash == cs[0].vs) {
                name = names[k].name;
                break;
            }
        if (*name)
            std::snprintf(buf, sizeof buf, "cs:%016llx(%s)", static_cast<unsigned long long>(cs[0].vs), name);
        else
            std::snprintf(buf, sizeof buf, "cs:%016llx", static_cast<unsigned long long>(cs[0].vs));
        return buf;
    }
    return "-";
}

struct ClassMean {
    double ms = 0, passes = 0;   // per frame
    double median = 0;           // Built v13.7 (4): the median of one pass's ms over the frames read (a single spike does not move it)
};
constexpr uint32_t kTopCs = 5;             // compute shaders listed in the summary's top_cs_dispatches
struct CsTotal {
    uint64_t hash = 0;
    double perFrame = 0;     // dispatches per captured frame, mean
};
struct Summary {
    struct Group {
        double ms = 0, passes = 0, draws = 0;
        double median = 0;       // Built v13.7 (4): the median of one pass's ms in this group over the frames read
        PassClass cls = PassClass::Other;
        std::string sig;
    };
    uint32_t frames = 0;                   // captured frames the means are over
    double frameMs = 0, eliteMs = 0, edvrMs = 0;   // per frame, mean
    ClassMean cls[static_cast<size_t>(PassClass::Count)]{};
    std::vector<Group> top;                // the passes by median, most first, at most kTopPasses (ms and the per-frame means are per group)
    std::vector<CsTotal> cs;               // GPU pass profile v2: the compute shaders by dispatches per frame, most first, at most kTopCs
};

// Built v13.7 (4): the median of a list (the mean of the two middle values for an even count); 0 for an empty list.
inline double medianOf(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t m = v.size() / 2;
    return v.size() % 2 ? v[m] : 0.5 * (v[m - 1] + v[m]);
}

// frameMs[f] is frame f's span; frameOk[f] is false for a frame whose result
// was not read (its rows must be absent). Every mean is over the frames that
// were read, so a missing frame is said, not averaged in as zero.
inline Summary summarise(const std::vector<Row>& rows, const std::vector<double>& frameMs,
                         const std::vector<bool>& frameOk) {
    Summary s;
    double frameSum = 0;
    for (size_t f = 0; f < frameOk.size(); ++f)
        if (frameOk[f]) {
            ++s.frames;
            frameSum += f < frameMs.size() ? frameMs[f] : 0;
        }
    if (!s.frames) return s;
    const double n = static_cast<double>(s.frames);
    std::map<std::string, Summary::Group> groups;
    std::map<std::string, std::vector<double>> groupSamples;              // Built v13.7 (4): one pass's ms per row, for the medians
    std::vector<double> clsSamples[static_cast<size_t>(PassClass::Count)];
    double elite = 0, edvr = 0;
    std::map<uint64_t, double> csCount;
    for (const Row& r : rows) {
        if (r.frame >= frameOk.size() || !frameOk[r.frame]) continue;
        for (const HashTally& h : r.cs) csCount[h.vs] += h.draws;
        ClassMean& c = s.cls[static_cast<size_t>(r.cls)];
        c.ms += r.ms;
        c.passes += 1;
        clsSamples[static_cast<size_t>(r.cls)].push_back(r.ms);
        groupSamples[r.key()].push_back(r.ms);
        (r.edvr ? edvr : elite) += r.ms;
        Summary::Group& g = groups[r.key()];
        g.ms += r.ms;
        g.passes += 1;
        g.draws += r.draws;
        g.cls = r.cls;
        g.sig = "rt " + r.rt + " colours " + std::to_string(r.colours) + " slices " +
                std::to_string(r.slices) + (r.cube ? " cube" : "") + " depth " + r.dsv + " shader " + r.ident;
    }
    s.frameMs = frameSum / n;
    s.eliteMs = elite / n;
    s.edvrMs = edvr / n;
    for (size_t c = 0; c < static_cast<size_t>(PassClass::Count); ++c) {
        s.cls[c].ms /= n;
        s.cls[c].passes /= n;
        s.cls[c].median = medianOf(clsSamples[c]);
    }
    for (auto& kv : groups) {
        Summary::Group g = kv.second;
        g.ms /= n;
        g.passes /= n;
        g.draws /= n;
        g.median = medianOf(groupSamples[kv.first]);
        s.top.push_back(g);
    }
    // GPU pass profile v2: the top list is by median pass time, so one spiked frame does not place a pass in it.
    std::stable_sort(s.top.begin(), s.top.end(),
                     [](const Summary::Group& a, const Summary::Group& b) { return a.median > b.median; });
    if (s.top.size() > kTopPasses) s.top.resize(kTopPasses);
    for (const auto& kv : csCount) s.cs.push_back(CsTotal{kv.first, kv.second / n});
    std::stable_sort(s.cs.begin(), s.cs.end(), [](const CsTotal& a, const CsTotal& b) { return a.perFrame > b.perFrame; });
    if (s.cs.size() > kTopCs) s.cs.resize(kTopCs);
    return s;
}

// v5 (delay): one frame's segment times split by kind. kind 0 = work (a draw, dispatch or clear), 1 = the delay, 2 = empty (no work
// between two boundaries: the no-work gap). busy is the frame time without the delay (work plus gaps, the sum of the pass durations as the
// rows carry them); gap is the empty time alone.
enum class SegKind : uint8_t { Work = 0, Delay = 1, Empty = 2 };
struct SegTotals {
    double busy = 0, delay = 0, gap = 0;
};
inline SegTotals segmentTotals(const std::vector<double>& ms, const std::vector<SegKind>& kind) {
    SegTotals t;
    for (size_t i = 0; i < ms.size() && i < kind.size(); ++i) {
        if (kind[i] == SegKind::Delay) {
            t.delay += ms[i];
            continue;
        }
        t.busy += ms[i];
        if (kind[i] == SegKind::Empty) t.gap += ms[i];
    }
    return t;
}
// v5: the suspect passes. A pass is suspect when its duration is more than kSuspectFactor times the median of its own group (the same
// key) AND the next gap after it is over kLargeGapMs. Sets Row::suspect; returns how many were flagged.
inline size_t markSuspects(std::vector<Row>& rows) {
    std::map<std::string, std::vector<double>> byKey;
    for (const Row& r : rows) byKey[r.key()].push_back(r.ms);
    std::map<std::string, double> median;
    for (auto& kv : byKey) median[kv.first] = medianOf(kv.second);
    size_t n = 0;
    for (Row& r : rows) {
        const double m = median[r.key()];
        r.suspect = m > 0 && r.ms > kSuspectFactor * m && r.gapAfterMs > kLargeGapMs;
        if (r.suspect) ++n;
    }
    return n;
}

// Built v13.6: the shared clock is yielded to the profiler from the press until the batch's last frame is closed: wanted while a capture is armed
// (requested) or has a batch open. The yield flag (gpuTimingSetYield) follows this state; the frame driver and the timers test the flag.
inline bool yieldWanted(bool requested, bool batch) noexcept { return requested || batch; }

// GPU pass profile v3: what a frame boundary does while a batch is starting frames. started frames have begun; waits is the count of
// consecutive boundaries that found the shared clock held by another scope (the caller counts them: Wait increments it, Begin resets it).
// Begin: start the next frame now. Wait: another scope holds the clock (a census timer begun before the press, say); retry at the next
// boundary. Abort: no slot for the frame, or the holder did not end within kMaxStartWaits; the batch ends with a note. Done: all frames
// have begun.
enum class StartAction : uint8_t { Begin, Wait, Abort, Done };
inline StartAction startAction(uint32_t started, uint32_t total, uint32_t slots, bool scopeHeld, uint32_t waits) noexcept {
    if (started >= total) return StartAction::Done;
    if (scopeHeld) return waits >= kMaxStartWaits ? StartAction::Abort : StartAction::Wait;
    if (started >= slots) return StartAction::Abort;
    return StartAction::Begin;
}

}  // namespace edvr::gpupass

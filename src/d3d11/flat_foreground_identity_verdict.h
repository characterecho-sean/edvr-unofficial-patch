#pragma once
// The identity test of the flat foreground map, in plain C++ (design doc section 104, the pistol's identity-differs window).
//
// A draw the history gave candidates is matched to one of them on the GPU, by the identity words of its pool record (the map's vertex
// shader, flat_foreground_motion_shader.h): identity.x equal, and identity.y equal but for byte 30. When none matches the draw's pixels
// are refused with reason 5 (identity-differs), and the census says only how many pixels. classifyIdentity() makes the shader's two tests
// on words read back from the GPU (flat_foreground_identity_sample.h), so a log can say which word moved. No device here: the classifier
// and its names are what tools/flat_temporal_test runs.
#include <cstdint>

namespace edvr {

struct IdentityWords { uint32_t x = 0, y = 0, z = 0, w = 0; };
inline constexpr uint32_t kIdentityParameterMaskCpu = 0xFF00FFFFu;   // the shader's kIdentityParameterMask
inline bool sameIdentityWords(const IdentityWords& prior, const IdentityWords& current) {
    return prior.x == current.x && ((prior.y ^ current.y) & kIdentityParameterMaskCpu) == 0;
}

enum class IdentityVerdict : uint8_t {
    Match,               // some candidate has the draw's identity: the map takes its history
    CurrentUnauthentic,  // the draw's own identity words are unwritten (reason 2)
    PriorsUnreadable,    // no candidate's identity words were ever written (reason 6)
    XDiffers,            // no candidate shares the first word (the record's bone base) and one shares the signature
    ParameterDiffers,    // a candidate shares the first word, and none shares the signature (reason 5)
    BothDiffer,          // no candidate shares either (reason 5)
    Count
};
inline constexpr unsigned kIdentityVerdictCount = static_cast<unsigned>(IdentityVerdict::Count);
inline const char* identityVerdictName(IdentityVerdict v) {
    static const char* const names[kIdentityVerdictCount] = {"match", "current-unauthentic", "priors-unreadable", "x-differs", "parameter-differs", "both-differ"};
    const unsigned i = static_cast<unsigned>(v);
    return i < kIdentityVerdictCount ? names[i] : "unknown";
}
// The shader's decision for one draw, from the words alone. `n` candidates, as the map was handed them.
inline IdentityVerdict classifyIdentity(const IdentityWords& current, const IdentityWords* priors, unsigned n) {
    if (current.z == 0) return IdentityVerdict::CurrentUnauthentic;
    unsigned readable = 0;
    bool xEqual = false, signatureEqual = false;
    for (unsigned i = 0; i < n; ++i) {
        if (priors[i].z == 0) continue;
        ++readable;
        if (sameIdentityWords(priors[i], current)) return IdentityVerdict::Match;
        if (priors[i].x == current.x) xEqual = true;
        if (((priors[i].y ^ current.y) & kIdentityParameterMaskCpu) == 0) signatureEqual = true;
    }
    if (!readable) return IdentityVerdict::PriorsUnreadable;
    if (xEqual) return IdentityVerdict::ParameterDiffers;
    return signatureEqual ? IdentityVerdict::XDiffers : IdentityVerdict::BothDiffer;
}

}  // namespace edvr

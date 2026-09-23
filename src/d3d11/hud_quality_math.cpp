#include "hud_quality_math.h"

#include <cstring>

namespace edvr {

float hudQualityParseTarget(const char* text, bool* recognized) {
    if (recognized) *recognized = true;
    if (!text) { if (recognized) *recognized = false; return 0.0f; }
    if (std::strcmp(text, "off") == 0) return 0.0f;
    if (std::strcmp(text, "1.0") == 0) return 1.0f;
    if (std::strcmp(text, "1.25") == 0) return 1.25f;
    if (recognized) *recognized = false;
    return 0.0f;
}

bool hudQualityFactor(float target, float hmdMultiplier, float* factorOut) {
    if (!(target > 0.0f) || !(hmdMultiplier > 0.0f)) return false;
    float factor = target / hmdMultiplier;
    if (!(factor > 1.01f)) return false;
    if (factor > 4.0f) factor = 4.0f;
    if (factorOut) *factorOut = factor;
    return true;
}

uint32_t hudQualityRoundDim(uint32_t v, float factor) {
    return static_cast<uint32_t>(static_cast<float>(v) * factor + 0.5f);
}

int hudQualityMatchLearned(uint32_t w, uint32_t h, const uint32_t* learnedW,
                           const uint32_t* learnedH, uint32_t n) {
    if (!learnedW || !learnedH) return -1;
    for (uint32_t i = 0; i < n; ++i) {
        if (learnedW[i] == w && learnedH[i] == h) return static_cast<int>(i);
    }
    return -1;
}

uint32_t hudQualityRatioX10000(uint32_t dim, uint32_t internalDim) {
    if (!internalDim) return 0;
    const double r = static_cast<double>(dim) * 10000.0 / static_cast<double>(internalDim);
    return static_cast<uint32_t>(r + 0.5);
}

bool hudQualityRatioNear(uint32_t candidateX10000, uint32_t knownX10000,
                         uint32_t toleranceX10000) {
    const uint32_t d = candidateX10000 > knownX10000 ? candidateX10000 - knownX10000
                                                       : knownX10000 - candidateX10000;
    return d <= toleranceX10000;
}

HudQualityRatioVerdict hudQualityRatioObserve(HudQualityRatioSlot* slots, uint32_t* count,
                                              uint32_t capacity, uint32_t rw, uint32_t rh,
                                              uint32_t internalW, uint32_t toleranceX10000) {
    if (!slots || !count) return HudQualityRatioVerdict::kNoSlot;
    for (uint32_t i = 0; i < *count; ++i) {
        HudQualityRatioSlot& s = slots[i];
        if (!hudQualityRatioNear(rw, s.ratioWx10000, toleranceX10000) ||
            !hudQualityRatioNear(rh, s.ratioHx10000, toleranceX10000)) {
            continue;
        }
        if (s.confirmed) {
            s.lastInternalW = internalW;
            return HudQualityRatioVerdict::kConfirmed;
        }
        // A tolerance on internalW itself: two sessions asked for the same
        // width by coincidence (a headset's own resolution list repeating)
        // must still count as "the same resolution", not a fresh proof.
        const uint32_t widthTolerance = s.lastInternalW / 100 + 1;  // ~1%
        if (hudQualityRatioNear(internalW, s.lastInternalW, widthTolerance)) {
            s.lastInternalW = internalW;
            return HudQualityRatioVerdict::kSameSession;
        }
        s.lastInternalW = internalW;
        s.confirmed = true;
        return HudQualityRatioVerdict::kConfirmed;
    }
    if (*count >= capacity) return HudQualityRatioVerdict::kNoSlot;
    HudQualityRatioSlot& s = slots[*count];
    s.ratioWx10000 = rw;
    s.ratioHx10000 = rh;
    s.lastInternalW = internalW;
    s.confirmed = false;
    ++*count;
    return HudQualityRatioVerdict::kNewCandidate;
}

}  // namespace edvr

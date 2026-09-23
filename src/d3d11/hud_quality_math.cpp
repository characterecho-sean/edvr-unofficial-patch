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

}  // namespace edvr

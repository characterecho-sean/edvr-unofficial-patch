// F101 checks: the frame ABI against the released one (v0.21.1). Header only, run by native_frame_test (run101). (Its second part, the reticle's game placement
// in the render path, went with that setting in F127.)
// The released provider requires a version 1 input of its struct's sizeof (88 on x64); from F31 this branch sent and accepted the words through valid
// alone (84), so neither mixed pair negotiated.
#pragma once

#include "../../src/common/native_frame.h"
#include "../../src/d3d11/native_on_foot_f85.h"

#include <cstddef>
#include <cstdint>

namespace f101test {

// The v0.21.1 structs, frozen (git show v0.21.1:src/common/native_frame.h): what a released d3d11.dll or openvr_api.dll declares.
struct ReleasedInput {
    uint32_t size, version;
    uint64_t generation, referenceGeneration, sequence;
    float physicalHead[12];
    uint32_t valid;
};
struct ReleasedOutput {
    uint32_t size, version;
    float reservedOffset[3];
    float reservedYaw;
    uint32_t reservedOffsetEnabled, reservedOffsetGamePoses;
    uint32_t reservedTerrainGuard[21];
    uint32_t sceneReady;
    uint32_t transitionEnabled;
    uint32_t resubmitEnabled;
    float trimOuterDeg, trimNasalDeg, trimVerticalDeg;
    uint32_t deferredPacing;
    uint32_t reservedProjectionChannel;
    float fadeAlpha;
};

static_assert(sizeof(ReleasedInput) == 88 && offsetof(ReleasedInput, valid) == 80, "the released version 1 input is 88 bytes on x64, valid at 80");
static_assert(sizeof(ReleasedOutput) == 152, "the released version 5 output is 152 bytes");
// The input: version 1's size is the released sizeof, and every released word sits where it did.
static_assert(EDVR_NATIVE_FRAME_INPUT_SIZE_1 == sizeof(ReleasedInput), "the version 1 input size is the released struct's sizeof");
static_assert(EDVR_NATIVE_FRAME_INPUT_WORDS_1 == 84, "the unpadded version 1 words (F31 to F100's size)");
static_assert(offsetof(EdvrNativeFrameInput, generation) == offsetof(ReleasedInput, generation) &&
                  offsetof(EdvrNativeFrameInput, referenceGeneration) == offsetof(ReleasedInput, referenceGeneration) &&
                  offsetof(EdvrNativeFrameInput, sequence) == offsetof(ReleasedInput, sequence) &&
                  offsetof(EdvrNativeFrameInput, physicalHead) == offsetof(ReleasedInput, physicalHead) &&
                  offsetof(EdvrNativeFrameInput, valid) == offsetof(ReleasedInput, valid),
              "the version 1 input words are where the released struct has them");
static_assert(EDVR_NATIVE_FRAME_INPUT_SIZE_2 == 664 && EDVR_NATIVE_FRAME_INPUT_SIZE_2 == offsetof(EdvrNativeFrameInput, localHead) &&
                  EDVR_NATIVE_FRAME_INPUT_SIZE_2 % alignof(EdvrNativeFrameInput) == 0,
              "the version 2 input is the words through ringCount, already on an 8-byte boundary");
static_assert(sizeof(EdvrNativeFrameInput) == 720, "the version 3 input is the whole struct");
static_assert(sizeof(EdvrNativeFrameOutput) == 184, "the whole output is 184 bytes, as it was before world scale took the last of the retired guard's words");
// The output: every member is four bytes, so each prefix is its version's sizeof; versions 1 to 5 are the released offsets.
static_assert(EDVR_NATIVE_FRAME_OUTPUT_SIZE_1 == offsetof(ReleasedOutput, trimOuterDeg) &&
                  EDVR_NATIVE_FRAME_OUTPUT_SIZE_2 == offsetof(ReleasedOutput, deferredPacing) &&
                  EDVR_NATIVE_FRAME_OUTPUT_SIZE_3 == offsetof(ReleasedOutput, reservedProjectionChannel) &&
                  EDVR_NATIVE_FRAME_OUTPUT_SIZE_4 == offsetof(ReleasedOutput, fadeAlpha) && EDVR_NATIVE_FRAME_OUTPUT_SIZE_5 == sizeof(ReleasedOutput),
              "output versions 1 to 5 are the released prefixes");
static_assert(offsetof(EdvrNativeFrameOutput, sceneReady) == offsetof(ReleasedOutput, sceneReady) &&
                  offsetof(EdvrNativeFrameOutput, resubmitEnabled) == offsetof(ReleasedOutput, resubmitEnabled) &&
                  offsetof(EdvrNativeFrameOutput, trimOuterDeg) == offsetof(ReleasedOutput, trimOuterDeg) &&
                  offsetof(EdvrNativeFrameOutput, deferredPacing) == offsetof(ReleasedOutput, deferredPacing) &&
                  offsetof(EdvrNativeFrameOutput, leftEyeShare) == offsetof(ReleasedOutput, reservedProjectionChannel) &&   // F106: renamed, same word
                  sizeof(EdvrNativeFrameOutput::leftEyeShare) == sizeof(ReleasedOutput::reservedProjectionChannel) &&
                  offsetof(EdvrNativeFrameOutput, fadeAlpha) == offsetof(ReleasedOutput, fadeAlpha) &&
                  alignof(EdvrNativeFrameOutput) == 4,
              "the released output words are where they were, with no padding in play");

// The released provider's shape checks (v0.21.1 src/d3d11/native_frame.cpp, beginFrame), on the released sizes: true when it takes the pair.
inline bool releasedAccepts(const EdvrNativeFrameInput& in, const EdvrNativeFrameOutput& out) {
    const bool wantsFade = out.version == EDVR_NATIVE_FRAME_VERSION_5 && out.size == sizeof(ReleasedOutput);
    const bool wantsChannel = !wantsFade && out.version == EDVR_NATIVE_FRAME_VERSION_4 && out.size == offsetof(ReleasedOutput, fadeAlpha);
    const bool wantsPacing = !wantsFade && !wantsChannel && out.version == EDVR_NATIVE_FRAME_VERSION_3 &&
                             out.size == offsetof(ReleasedOutput, reservedProjectionChannel);
    const bool wantsTrim = !wantsFade && !wantsChannel && !wantsPacing && out.version == EDVR_NATIVE_FRAME_VERSION_2 &&
                           out.size == offsetof(ReleasedOutput, deferredPacing);
    const bool legacy = !wantsFade && !wantsChannel && !wantsPacing && !wantsTrim && out.version == EDVR_NATIVE_FRAME_VERSION_1 &&
                        out.size == offsetof(ReleasedOutput, trimOuterDeg);
    return in.size == sizeof(ReleasedInput) && in.version == EDVR_NATIVE_FRAME_VERSION_1 && (wantsFade || wantsChannel || wantsPacing || wantsTrim || legacy);
}
// The released runtime's ask for a version (v0.21.1 src/openxr/native_frame_client.h, ask): 5 down to 1.
inline EdvrNativeFrameOutput releasedAsk(uint32_t version) {
    if (version >= EDVR_NATIVE_FRAME_VERSION_5) return EdvrNativeFrameOutput{static_cast<uint32_t>(sizeof(ReleasedOutput)), EDVR_NATIVE_FRAME_VERSION_5};
    if (version == EDVR_NATIVE_FRAME_VERSION_4) return EdvrNativeFrameOutput{static_cast<uint32_t>(offsetof(ReleasedOutput, fadeAlpha)), EDVR_NATIVE_FRAME_VERSION_4};
    if (version == EDVR_NATIVE_FRAME_VERSION_3)
        return EdvrNativeFrameOutput{static_cast<uint32_t>(offsetof(ReleasedOutput, reservedProjectionChannel)), EDVR_NATIVE_FRAME_VERSION_3};
    if (version == EDVR_NATIVE_FRAME_VERSION_2) return EdvrNativeFrameOutput{static_cast<uint32_t>(offsetof(ReleasedOutput, deferredPacing)), EDVR_NATIVE_FRAME_VERSION_2};
    return EdvrNativeFrameOutput{static_cast<uint32_t>(offsetof(ReleasedOutput, trimOuterDeg)), EDVR_NATIVE_FRAME_VERSION_1};
}

// The runtime's ladder (native_frame_client.h beginLadder: ask, and one step down on each refusal) against a provider's accept, with the shapes
// native_frame.h gives (nativeFrameAsk, nativeFrameInputShape). wordsOnly sends version 1 as F31 to F100 did (the unpadded words). The version the
// ladder settles on, 0 when every step was refused.
template <class Accept>
inline uint32_t ladder(Accept&& accept, bool wordsOnly = false) {
    for (uint32_t v = EDVR_NATIVE_FRAME_VERSION_11; v >= EDVR_NATIVE_FRAME_VERSION_1; --v) {
        EdvrNativeFrameInput in{};
        edvr::nativeFrameInputShape(v, in);
        if (wordsOnly && in.version == EDVR_NATIVE_FRAME_INPUT_VERSION_1) in.size = EDVR_NATIVE_FRAME_INPUT_WORDS_1;
        const EdvrNativeFrameOutput out = edvr::nativeFrameAsk(v);
        if (accept(in, out)) return v;
    }
    return 0;
}

template <class Check>
inline void run101(Check&& check) {
    {   // this runtime's ladder against a released d3d11.dll: refused at 10 to 6, taken at 5 (input version 1 of 88 bytes, output version 5 of 152)
        const uint32_t settled = ladder([](const EdvrNativeFrameInput& in, const EdvrNativeFrameOutput& out) { return releasedAccepts(in, out); });
        bool everyOld = true;
        for (uint32_t v = EDVR_NATIVE_FRAME_VERSION_1; v <= EDVR_NATIVE_FRAME_VERSION_5; ++v) {
            EdvrNativeFrameInput in{};
            edvr::nativeFrameInputShape(v, in);
            everyOld = everyOld && releasedAccepts(in, edvr::nativeFrameAsk(v)) && in.size == 88u;
        }
        check(settled == EDVR_NATIVE_FRAME_VERSION_5 && everyOld,
              "F101: this runtime's ladder settles at version 5 with a released (v0.21.1) provider (refused from 11 to 6), and every step below 7 is a shape it takes (input 88 bytes)");
        const uint32_t before = ladder([](const EdvrNativeFrameInput& in, const EdvrNativeFrameOutput& out) { return releasedAccepts(in, out); }, true);
        check(before == 0, "F101 (control): with F31 to F100's version 1 size (84, the words alone) a released provider refuses every step");
    }
    {   // the released runtime's shapes are the ones this build's shapes name (sizes and versions, 1 to 5)
        bool same = true;
        for (uint32_t v = EDVR_NATIVE_FRAME_VERSION_1; v <= EDVR_NATIVE_FRAME_VERSION_5; ++v) {
            const EdvrNativeFrameOutput a = releasedAsk(v), b = edvr::nativeFrameAsk(v);
            same = same && a.size == b.size && a.version == b.version;
        }
        check(same, "F101: output versions 1 to 5 are asked with the released runtime's sizes");
    }
}

}  // namespace f101test

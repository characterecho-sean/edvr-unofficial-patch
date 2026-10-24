#pragma once

#include <cstdint>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace edvr::nofoot::optic_capture {

constexpr uint32_t kCount = 16;

enum class SourceState : uint8_t { Unsampled, Matched, Inactive, Missing, Stale, Future, Invalid, Unmatched };
enum class PoseState : uint8_t { Unrequested, Matched, Missing, Stale, Future, IdentityMismatch, FrameMismatch, NoWrite, Invalid };
enum class SceneEyeState : uint8_t { Unrequested, Matched, NotOwn, Missing, Stale, Future, Invalid };
constexpr uint32_t kUiRowsPerEye = 16;
inline bool uiTraceMatches(uint32_t rowRenderFrame, uint32_t rowItemSerial, bool itemRead,
                           uint32_t expectedRenderFrame, uint32_t expectedItemSerial) noexcept {
    return itemRead && expectedRenderFrame != 0 && expectedItemSerial != 0 && rowRenderFrame == expectedRenderFrame && rowItemSerial == expectedItemSerial;
}

struct PoseKey {
    uint32_t renderFrame = ~uint32_t(0), hookRenderFrame = ~uint32_t(0), rootReadFrame = ~uint32_t(0), fpSequence = 0, gGameFrame = ~uint32_t(0), itemSerial = 0, rigGeneration = 0;
    uint64_t rig = 0, qpc = 0;
};

inline PoseState poseJoinState(bool found, const PoseKey& pose, const PoseKey& source, uint32_t expectedRenderFrame,
                               uint64_t sampleQpc, uint64_t maxAgeQpc, bool wrote, bool finite) noexcept {
    if (!found) return PoseState::Missing;
    if (pose.renderFrame != expectedRenderFrame || pose.hookRenderFrame != expectedRenderFrame || pose.rootReadFrame != expectedRenderFrame || source.rootReadFrame != expectedRenderFrame ||
        pose.fpSequence != source.fpSequence || pose.gGameFrame != source.gGameFrame)
        return PoseState::FrameMismatch;
    if (pose.itemSerial != source.itemSerial || pose.rig != source.rig || pose.rigGeneration != source.rigGeneration)
        return PoseState::IdentityMismatch;
    if (!pose.qpc || !sampleQpc) return PoseState::Invalid;
    if (pose.qpc > sampleQpc) return PoseState::Future;
    if (sampleQpc - pose.qpc > maxAgeQpc) return PoseState::Stale;
    if (!wrote) return PoseState::NoWrite;
    return finite ? PoseState::Matched : PoseState::Invalid;
}

struct RowDescriptor {
    bool scheduled = false;
    uint32_t sceneFrame = 0, renderFrame = 0, epoch = 0;
    uint64_t sequence = 0;
    char imageStatus[32] = "unscheduled";
};

inline bool formatJsonNumber(char* out, size_t capacity, float value) noexcept {
    if (!std::isfinite(value)) {
        if (capacity) std::snprintf(out, capacity, "null");
        return false;
    }
    if (capacity) std::snprintf(out, capacity, "%.9g", static_cast<double>(value));
    return true;
}

struct Record {
    bool sampled = false;
    uint32_t index = 0, sceneFrame = 0, renderFrame = 0, epoch = 0;
    uint64_t sequence = 0;
    char imageStatus[32] = "unscheduled";
    SourceState state = SourceState::Unsampled;
    bool p14BasisValid = false, p14PositionValid = false;
    uint32_t adsFrame = ~0u, fpFrame = ~0u, gGameFrame = ~0u, rootReadFrame = ~0u, eyeFrame = ~0u;
    uint32_t fpGeneration = 0;
    uint64_t sampleQpc = 0, adsQpc = 0, fpQpc = 0, gQpc = 0, rootReadQpc = 0, eyeQpc = 0;
    bool adsActive = false, fpRead = false, gRead = false, eyeRead = false;
    bool itemRead = false;
    uint32_t itemSerial = 0;
    char itemName[64] = "";
    float p14Model[3] = {}, p14World[3] = {}, p14Basis[9] = {}, rootForward[3] = {};
    float eyeRows[16] = {}, eyeOrigin[4] = {};
    SceneEyeState sceneEyeState = SceneEyeState::Unrequested;
    bool sceneEyeRowsRead = false, sceneEyeRowsOk = false, sceneEyeRowsOwn = false, sceneEyeRowsBound = false;
    uint32_t sceneEyeFrame = ~0u;
    int32_t sceneEyeFollow = -1;
    uint64_t sceneEyeQpc = 0;
    float sceneEyeRows[12] = {};
    PoseState poseState = PoseState::Unrequested;
    PoseKey poseKey{}, poseSourceKey{};
    uint32_t posePlanGeneration = 0, poseWriteMask = 0;
    uint64_t poseQpc = 0, poseFpQpc = 0, poseGQpc = 0, poseNativeEpoch = 0;
    bool posePinValid = false, poseEyeCalHave = false, poseEyeCalAxesValid = false, poseMatricesValid = false;
    bool poseRawMatricesValid = false, posePinEyeCalValid = false, poseWriteFresh = false;
    bool poseM3pAffineValid = false, poseM3pPaddingFinite = false, poseM3pPaddingCanonicalized = false;
    float poseArm[2][4][3] = {}; // actual shoulder, elbow, hand and target; model coordinates, metres.
    // F86a: the wrists in world as the submit that drew this pose put them, after the pose carry (and without it), joined by the pose's game frame.
    bool poseArmDrawnValid = false, poseArmDrawnHave[2] = {};
    int poseArmDrawnWhy = 0; // nofoot::f86::CarryWhy of that submit
    uint32_t poseArmDrawnAge = 0;
    float poseArmDrawn[2][3] = {}, poseArmDrawnBefore[2][3] = {}, poseCarryDelta[3] = {};
    bool poseRifleQualityValid = false, poseRifleQualityAccepted = false;
    float poseRifleQualityWeight = 0.0f, poseRifleDetachWeight = 0.0f;
    float poseRifleBaselineYawDeg = 0.0f, poseRifleSelectedYawDeg = 0.0f, poseRifleBaselineCost = 0.0f, poseRifleSelectedCost = 0.0f;
    float poseRiflePreAssist[2][4][3] = {}; // clavicle, shoulder, elbow, hand before candidate assist; model metres.
    float poseRifleTarget[2][3] = {}, poseRifleSolveWeight[2] = {}, poseRifleAssistDeg[2] = {};
    float poseRifleLengths[2][2] = {}, poseRifleFpPole[2][3] = {}, poseRiflePoleSeed[2][3][3] = {};
    bool poseRifleStableEligible[2] = {}, poseRifleFpPoleValid[2] = {}, poseRiflePoleSeedValid[2] = {};
    float poseRifleElbowOutM = 0.0f, poseRifleTrimRad = 0.0f;
    float poseRifleNeck[3] = {}, poseRifleChestPivot[3] = {}, poseRifleAdsPush[3] = {}, poseRifleLegVertical = 0.0f;
    float poseRifleLeg[2][3][3] = {}; // hip, knee, held foot for exact candidate feasibility replay.
    float poseRawTorso[3][16] = {}; // input AMC joints 24 chest, 34 neck and 44 head.
    float poseFinalTorso[4][16] = {}; // after-write joints 24 chest, 34 neck, 35/36 clavicles.
    float poseRoot[16] = {}, poseM3p[16] = {}, poseFroot[16] = {}, poseBodyOffset[3] = {};
    float posePinEyeTarget[3] = {}, posePinTarget[3] = {}, posePinNeckBefore[3] = {}, posePinEyeLocal[3] = {};
    float poseNeckRestTarget[3] = {}, poseNeckRestBaseline[3] = {}, poseLocalPitchDeg = 0.0f, poseLocalPitchAgeUs = 0.0f;
    uint64_t poseLocalPoseQpc = 0, poseLocalPoseFrame = 0;
    uint32_t poseLocalPoseGeneration = 0;
    uint8_t poseNeckRestFlags = 0; // requested, candidate valid, applied, refused, local pitch source valid, validated
    uint8_t poseNeckGenerationState = 0; // 0 unavailable (validated sample policy), 1 current reference matches, 2 known mismatch/refused
    uint8_t poseNeckEyeSource = 2; // 0 current scene-eye rows, 1 cached scene-eye rows, 2 unavailable
    uint32_t poseNeckEyeAgeMs = 0, poseNeckCurrentGeneration = 0;
    float poseEyeCalibration[3] = {};
    float poseEyeAxes[4] = {}; // f axis/sign, up axis/sign from the standing PinEyeCal.
    float poseCrouchWeight = 0.0f, posePinDetach = 0.0f, poseReachCandidate[3] = {}, poseReachApplied[3] = {};
    float poseClearance[6][3] = {};
    uint16_t poseArmJoints[2][3] = {}; // U/F/H per side, copied from the exact after-write ArmPlan.
    uint8_t poseClearanceHave = 0, poseClearanceInside = 0, poseReachFlags = 0;
    uint32_t poseRigGeneration = 0;
    uint64_t poseRig = 0, posePlanX = 0;
    uint8_t uiCount = 0;
    uint32_t uiDropped = 0;
    struct UiSnapshot {
        bool valid = false, itemRead = false, fresh = false, fieldsValid = false, instanceValid = false, distanceScale = false;
        bool cameraAvailable = false, cameraValid = false, attachmentAvailable = false, attachmentValid = false, joinedGetter = false;
        uint32_t renderFrame = 0, itemSerial = 0;
        uint64_t qpc = 0;
        uint64_t rendererIdentity = 0, partIdentity = 0, instanceIdentity = 0, cameraPointer = 0, getterQpc = 0;
        uint16_t nameId = 0;
        uint8_t facing = 0;
        int32_t space = 0;
        float width = 0.0f, height = 0.0f, pixelOffset[2] = {}, anchor[2] = {}, baseScale = 0.0f, instanceScale = 0.0f;
        float instance[16] = {}, camera[16] = {}, attachment[16] = {};
    } ui[kUiRowsPerEye] = {};
};

inline bool sourceFramesMatchRender(uint32_t renderFrame, uint32_t fpFrame, uint32_t rootReadFrame, uint32_t eyeFrame, uint32_t adsFrame) noexcept {
    return renderFrame != 0 && renderFrame != ~uint32_t(0) && renderFrame == fpFrame && renderFrame == rootReadFrame && renderFrame == eyeFrame && renderFrame == adsFrame;
}

struct Ledger {
    bool armed = false;
    uint32_t duplicates = 0;
    uint64_t stamp = 0;
    Record rows[kCount] = {};
    RowDescriptor requested[kCount] = {};

    void begin(uint64_t runStamp) noexcept {
        armed = true;
        duplicates = 0;
        stamp = runStamp;
        for (uint32_t i = 0; i < kCount; ++i) { rows[i] = Record{}; requested[i] = RowDescriptor{}; }
    }

    bool sample(uint32_t index, const Record& row) noexcept {
        if (!armed || index >= kCount) return false;
        if (rows[index].sampled) { ++duplicates; return false; }
        rows[index] = row;
        rows[index].sampled = true;
        rows[index].index = index;
        return true;
    }

    void end(const RowDescriptor* descriptors, uint32_t count) noexcept {
        for (uint32_t i = 0; i < kCount; ++i) requested[i] = (descriptors && i < count) ? descriptors[i] : RowDescriptor{};
        armed = false;
    }
};

} // namespace edvr::nofoot::optic_capture

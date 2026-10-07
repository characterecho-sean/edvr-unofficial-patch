#pragma once
#include <cstdint>
#include "flat_foreground_receipt.h"

// Read-only, owner-thread snapshot for the offline proxy integration bench.
// The caller supplies the exact structure size so an old bench cannot silently
// interpret a new DLL's layout. No pointer or COM object crosses this boundary.
struct EdvrFlatSdkBenchSnapshot {
    uint32_t size = sizeof(EdvrFlatSdkBenchSnapshot);
    uint32_t version = 3;
    uint32_t flatProfile = 0;
    uint32_t live = 0;
    uint32_t owner = 0;
    uint32_t deviceReady = 0;
    uint32_t contextReady = 0;
    uint32_t outputReady = 0;
    uint64_t frame = 0;
    uint32_t drawSequence = 0;
    uint32_t work = 0;
    uint32_t namedWorld = 0;
    uint32_t candidates = 0;
    uint64_t foreignSeen = 0;
    uint64_t captured = 0;
    uint64_t captureAttempts = 0;
    uint64_t gpuIdentitySubmitted = 0;
    uint64_t worldMarkers = 0;
    uint64_t hAttempts = 0;
    uint64_t hQualified = 0;
    uint32_t hdrTriggered = 0;
    uint32_t hdrSelected = 0;
    uint64_t resolverCalls = 0;
    uint64_t backendFailures = 0;
    uint64_t hdrResolves = 0;
    uint64_t hdrSpatial = 0;
    uint64_t hdrCaptured = 0;
    uint64_t hdrCopied = 0;
    uint64_t hdrPrepped = 0;
    uint64_t hdrBackendCompleted = 0;
    uint64_t hdrFinished = 0;
    uint64_t hdrRestored = 0;
    uint64_t firstFailureFrame = 0;
    uint32_t firstFailureSequence = 0;
    uint32_t firstFailureFormat = 0;
    uint64_t firstFailureVs = 0;
    uint64_t firstFailurePs = 0;
    uint32_t firstFailureSelectedH = 0;
    char mode[16]{};
    char firstFailureStage[48]{};
    char firstFailureReason[96]{};
    char hRefusal[96]{};
    char hdrVerdict[96]{};
    EdvrFlatForegroundStateReceipt firstFailureState{};
    EdvrFlatForegroundBudgetReceipt firstFailureBudget{};
    // Version 3: provisional world before naming, world draws left unmarked,
    // draws that cannot write depth forwarded unchanged, and the distinct
    // refusals of the last failed H.
    uint64_t predictedWorld = 0;
    uint64_t surfacePreserving = 0;
    uint64_t surfacePreservingForeign = 0;
    uint64_t worldUnmarked = 0;
    uint32_t failureKinds = 0;
    uint32_t failureKindsDropped = 0;
};

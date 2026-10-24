#pragma once
#include "flat_mono_frame.h"
#include "flat_deferred_camera.h"
#include "flat_forward_light.h"
#include "flat_inert_pairs.h"
#include <optional>

namespace edvr {
inline uint32_t flatRuntimeDepthReadFormat(uint32_t format) {
    return format == 19 ? 21u : format == 39 ? 41u : format == 44 ? 46u : 0u;
}
// Known, actual-shader-verified projection recipes may use the selector's HDR
// viewport contract only on a scene HDR target with its owned scene depth.
// This does not admit depth-clamped draws as motion sources or image intermediates.
inline bool flatRuntimeProjectionHdr(const FlatContractObservation& k, const void* output,
                                     const void* ownedDepth) {
    return k.format == 26 && k.color && k.color != output && k.rtv &&
        ownedDepth && k.depth == ownedDepth && k.dsv && k.width && k.height &&
        k.depthWidth == k.width && k.depthHeight == k.height;
}
inline bool flatRuntimeProjectionViewport(uint32_t count, const float* viewport,
                                          uint32_t width, uint32_t height, bool sceneHdr) {
    return sceneHdr ? flat_mono_detail::hdrViewport(count, viewport, width, height)
                    : flat_mono_detail::fullViewport(count, viewport, width, height);
}
// Online prefix contract. No resource ownership and no previous-frame admission.
struct FlatRuntimeDraw {
    FlatContractObservation key{};
    unsigned char camera[kFlatCameraBytes]{};
    bool supported = false;
    // Set only after the bridge verifies the actual HDR image-copy shaders,
    // RT0/DSV/SRV0 views, dimensions, sample count and viewport.
    bool hdrCopyVerified = false;
    // Exact menu HDR-to-HDR copy, with live views and typed texture layout verified.
    bool menuHdrCopyVerified = false;
    // The depth-of-field composite (kDofCompositeVs/Ps): the bridge verified its actual shaders, its scene-HDR input at t0, its blurred
    // image at t1, its depthless HDR output and the viewport. It makes a new HDR image of the scene with the blur laid in, which the
    // tone then reads in the scene's place (the 3D main menu at Depth of Field 2).
    bool dofCompositeVerified = false;
    // The bridge verified the actual camera-independent format 9 image shaders
    // and their RT/DSV views. The model also checks the exact shader pair.
    bool imageSourceCameraIndependentVerified = false;
    // Verified actual deferred shader/range/original CB2 relation; FTR keeps
    // this distinct from HasCamera. camera[] then holds the canonical donor.
    bool forwardHdrVerified = false;
    bool forwardUpstreamPhase = false; // independent used-view phase certified
    bool deferredHdrVerified = false;
    bool deferredInverseRepaired = false; // executed clone certified and bound, original metadata unchanged
    // Set only by the runtime, whose key.cameraHash is the camera table's flatCameraHash of these very
    // camera bytes (flat_camera_table.h), so cameraCurrent's hash check on this draw's record is true by
    // construction and is not recomputed. A replay leaves it false and keeps the check.
    bool cameraHashTrusted = false;
    // A provisional late HDR overlay: the bridge must finish its private
    // coverage draw or make this target bad before the HDR consumer selects it.
    bool overlayProtected = false;
    bool effectiveDepthWrite = false;
    bool effectiveStencilWrite = false;
    uint32_t instances = 1;
    // The weapon's two passes on the COPY route (design section 104; flatWeaponRoute in flat_copy_structure.h says where a frame is
    // judged). Both are set only by the runtime, only for DLSS or FSR, and only where the HDR route will not treat the frame, and
    // both ride the trace (kFlatTraceFirstPersonCohort, kFlatTraceAlternateHdr), so a replay decides as the live run did. A draw
    // without them is judged exactly as it always was.
    //   firstPersonCohort: a supported pool draw of the first-person weapon family under an alternate camera, into the scene's
    //     depth (the cohort that draws before the world, near 0.0675 against 0.025). It is no world motion source, so it joins no
    //     source record, and the frame it is in is mixed-camera (FlatMonoFrame::mixedCamera): the foreground contract qualifies it.
    //   alternateHdr: an HDR draw from a second camera that is the world's pose, with no depth write (the weapon's glow pass). No
    //     overlay is planned for it, and on a scene H that is still sound it neither sets nor vetoes the scene camera, as the laser
    //     pair does by its shader hashes.
    bool firstPersonCohort = false;
    bool alternateHdr = false;
    // A pool family's vertex shader with a pixel shader the motion producer does not substitute, drawn into the scene's depth at the
    // scene's extent (design section 104). Set only by the runtime (engineVelocityPoolFamilyVs, flatContractKind) and carried by the
    // trace (kFlatTracePoolFamilyVs). It moves with no motion source tracking it, so the frame holding one is never source-free.
    bool poolFamilyVs = false;
};
// Exact bytecode-qualified image filters, with a position/UV passthrough VS.
// Live shader verification and resource/frame provenance remain mandatory.
inline bool flatRuntimeCameraIndependentImageSourcePair(uint64_t vs, uint64_t ps) {
    return vs == 0xCFA91824129ECBBCull &&
        (ps == 0xFCFAD73924BF45B9ull || ps == 0x07B3F82100F29401ull);
}
enum class FlatRuntimeConflict : uint32_t {
    None, Viewport, MissingDepth, DepthMismatch, CameraChange,
    CameraProvenance, ExplicitWrite, ImageCopySource, MenuCopySource, SelectorLayout,
    SelectorCamera, SelectorCameraProvenance, OverlaySuffix, DofCompositeSource, Count
};
inline const char* flatRuntimeConflictName(FlatRuntimeConflict c) {
    switch (c) {
    case FlatRuntimeConflict::Viewport: return "viewport";
    case FlatRuntimeConflict::MissingDepth: return "missing-depth-or-dsv";
    case FlatRuntimeConflict::DepthMismatch: return "depth-or-dsv-changed";
    case FlatRuntimeConflict::CameraChange: return "hdr-camera-changed";
    case FlatRuntimeConflict::CameraProvenance: return "hdr-camera-provenance";
    case FlatRuntimeConflict::ExplicitWrite: return "explicit-resource-write";
    case FlatRuntimeConflict::ImageCopySource: return "image-copy-source";
    case FlatRuntimeConflict::MenuCopySource: return "menu-copy-source";
    case FlatRuntimeConflict::SelectorLayout: return "selector-hdr-layout";
    case FlatRuntimeConflict::SelectorCamera: return "selector-hdr-camera-conflict";
    case FlatRuntimeConflict::SelectorCameraProvenance: return "selector-hdr-camera-provenance";
    case FlatRuntimeConflict::OverlaySuffix: return "unprotected-late-hdr-overlay-or-depth-write";
    case FlatRuntimeConflict::DofCompositeSource: return "dof-composite-source";
    default: return "none";
    }
}
struct FlatRuntimeWitnessDraw {
    const void* rtv = nullptr, *depth = nullptr, *dsv = nullptr, *b1 = nullptr;
    uint64_t vs = 0, ps = 0, cameraHash = 0, writeEpoch = 0;
    uint32_t first = 0, writeSeq = 0, width = 0, height = 0, format = 0;
    uint32_t depthWidth = 0, depthHeight = 0, depthFormat = 0, viewportCount = 0;
    float viewport[6]{};
    bool hasCamera = false;
    unsigned char camera[kFlatCameraBytes]{};
};
inline FlatRuntimeWitnessDraw flatRuntimeWitnessDraw(const FlatContractRecord& r) {
    FlatRuntimeWitnessDraw d{}; const auto& k = r.key;
    d.rtv = k.rtv; d.depth = k.depth; d.dsv = k.dsv; d.b1 = k.b1;
    d.vs = k.vs; d.ps = k.ps; d.cameraHash = k.cameraHash;
    d.writeEpoch = r.firstWriteEpoch; d.first = r.first; d.writeSeq = r.firstWriteSeq;
    d.width = k.width; d.height = k.height; d.format = k.format;
    d.depthWidth = k.depthWidth; d.depthHeight = k.depthHeight; d.depthFormat = k.depthFormat;
    d.viewportCount = k.viewportCount;
    std::memcpy(d.viewport, k.viewport, sizeof(d.viewport));
    d.hasCamera = k.camera != nullptr;
    if (d.hasCamera) std::memcpy(d.camera, r.camera, sizeof(d.camera));
    return d;
}
struct FlatRuntimeWitness {
    FlatRuntimeConflict cause = FlatRuntimeConflict::None;
    const void* hdr = nullptr;
    uint32_t sequence = 0;
    FlatRuntimeWitnessDraw reference{}, current{};
};
static_assert(sizeof(FlatRuntimeWitness) <= 512, "keep per-target refusal witnesses bounded");
struct FlatRuntimeTarget {
    const void* resource = nullptr;
    // tone stores the first known camera for HDR, or for format 9 image geometry.
    FlatContractRecord writes{}, tone{};
    bool hdrBad = false, hdrCamera = false, hdrLayoutChanged = false, imageSourceBad = false;
    bool menuInherited = false;
    bool dofInherited = false;   // the depth-of-field composite's output, taken as the scene HDR's alias (not the menu's static-scene policy)
    bool imageHasCamera = false;
    // The target's first write was a verified HDR image copy (the glow layer of a screen with no scene, drawn before anything else
    // into the HDR). That copy is not recorded as a write (it carries no depth and no camera, so as the HDR's first record it could
    // only make the HDR unselectable), and while it stands alone the HDR is a 2D image. Any later write into the HDR by a pair that
    // reads b1 turns the copy into the image-copy-source conflict it was before this flag existed.
    bool imageCopyFirst = false;
    bool overlayOpen = false;
    const void* overlayDepth = nullptr;
    uint32_t tones = 0;
    FlatRuntimeWitness firstBad{};
    // Every cause that made the target bad, one bit each (1u << FlatRuntimeConflict), where firstBad keeps only the first. A bad mark
    // that names no cause sets bit 0 (None), so a target is camera-only bad (flatRuntimeCameraConflictSpatialOnly) only when
    // CameraChange is the single bit and nothing else ever marked it.
    uint32_t badCauses = 0;
};
enum class FlatRuntimeUncertainCause : uint8_t {
    None, MissingTarget, TargetCapacity, ToneUavWrite, SourceUavWrite,
    ForwardIdentity, DeferredIdentity, FirstPersonDepth, SourceCapacity,
    DeferredPrepare, DeferredExecution, CameraCapacity, UnknownContext,
    DeferredBinding, ForeignWork, ForwardRefusal, MultipleCopies, Unspecified, Count
};
inline const char* flatRuntimeUncertainName(FlatRuntimeUncertainCause cause) {
    switch (cause) {
    case FlatRuntimeUncertainCause::MissingTarget: return "missing-target";
    case FlatRuntimeUncertainCause::TargetCapacity: return "target-capacity";
    case FlatRuntimeUncertainCause::ToneUavWrite: return "tone-uav-write";
    case FlatRuntimeUncertainCause::SourceUavWrite: return "source-uav-write";
    case FlatRuntimeUncertainCause::ForwardIdentity: return "forward-identity";
    case FlatRuntimeUncertainCause::DeferredIdentity: return "deferred-identity";
    case FlatRuntimeUncertainCause::FirstPersonDepth: return "first-person-depth";
    case FlatRuntimeUncertainCause::SourceCapacity: return "source-capacity";
    case FlatRuntimeUncertainCause::DeferredPrepare: return "deferred-prepare";
    case FlatRuntimeUncertainCause::DeferredExecution: return "deferred-execution";
    case FlatRuntimeUncertainCause::CameraCapacity: return "camera-capacity";
    case FlatRuntimeUncertainCause::UnknownContext: return "unknown-context";
    case FlatRuntimeUncertainCause::DeferredBinding: return "deferred-binding";
    case FlatRuntimeUncertainCause::ForeignWork: return "foreign-work";
    case FlatRuntimeUncertainCause::ForwardRefusal: return "forward-refusal";
    case FlatRuntimeUncertainCause::MultipleCopies: return "multiple-output-copies";
    case FlatRuntimeUncertainCause::Unspecified: return "unspecified-missing-witness";
    default: return "none";
    }
}
struct FlatRuntimeUncertainWitness {
    FlatRuntimeUncertainCause cause = FlatRuntimeUncertainCause::None;
    uint32_t q = 0;
    const void* resource = nullptr;
    const void* related = nullptr;
    uint64_t vs = 0, ps = 0;
    uint32_t detail = 0;
};
// The camera the writers of a colour target bound on a depth, kept for a scene with no motion source on that depth. A scene whose every
// G-buffer writer is a pair the motion producer does not substitute (a character preview on the loadout screen) has no record in
// FlatRuntimePrefix::sources, yet its writers still bound the scene constants. One entry per (depth, colour) written by a Screen-kind
// draw that is not a supported pair, noted only while no source record exists on that depth. flatRuntimeDeferredDonor reads it where
// the pool rule has no record at all on the depth, and nowhere else.
enum class FlatWitnessConflict : uint8_t { None, Layout, Viewport, CameraProvenance, Camera, Written, Count };
inline const char* flatWitnessConflictName(FlatWitnessConflict c) {
    switch (c) {
    case FlatWitnessConflict::Layout: return "layout";
    case FlatWitnessConflict::Viewport: return "viewport";
    case FlatWitnessConflict::CameraProvenance: return "camera-not-current";
    case FlatWitnessConflict::Camera: return "second-camera";
    case FlatWitnessConflict::Written: return "explicit-write";
    default: return "none";
    }
}
// One writer as the witness saw it, for the witness line: the pair, the b1 it bound, its q, and one bit for each term flatRuntimeCameraCurrent
// asks of its camera. A line that says conflict=camera-not-current then says which writer raised it and which term it failed.
enum : uint8_t {
    kWitnessTermCamera = 1,    // key.camera is not null: it bound readable camera rows
    kWitnessTermEpoch = 2,     // the camera was written this frame (writeEpoch == frame)
    kWitnessTermOrdered = 4,   // and before the draw (writeSeq nonzero, writeSeq <= q)
    kWitnessTermTrusted = 8,   // the hash was recorded with the rows (cameraHashTrusted), not recomputed
    kWitnessTermHash = 16      // the hash is the rows' own: trusted, or equal to flatCameraHash of them
};
struct FlatWitnessWriter {
    uint64_t vs = 0, ps = 0;
    const void* b1 = nullptr;
    uint32_t q = 0;            // 0: no such writer
    uint64_t cameraHash = 0, writeEpoch = 0;   // the writer's own key (the second-camera donor reads them)
    uint32_t writeSeq = 0;
    uint8_t terms = 0;
    bool has(uint8_t term) const { return (terms & term) != 0; }
};
struct FlatRuntimeCameraWitness {
    const void* depth = nullptr, *color = nullptr, *rtv = nullptr, *dsv = nullptr, *b1 = nullptr;
    uint64_t cameraHash = 0, writeEpoch = 0, lastWriteEpoch = 0;
    uint64_t firstVs = 0, firstPs = 0;   // the first writer's pair (its q is `first`, its b1 `b1`, its camera `hasCamera`)
    uint32_t writeSeq = 0, lastWriteSeq = 0, first = 0, last = 0, writers = 0;
    uint32_t width = 0, height = 0, depthWidth = 0, depthHeight = 0, format = 0, depthFormat = 0;
    bool fullViewport = false;    // the first writer's viewport; a later writer's partial one is a conflict
    bool hasCamera = false;       // the first writer bound readable camera rows
    bool cameraCurrent = false;   // flatRuntimeCameraCurrent of the first writer; a later writer's failing it is a conflict
    FlatWitnessConflict conflict = FlatWitnessConflict::None;
    FlatWitnessWriter offender;   // the writer that first raised `conflict` (q 0 for none, or for an explicit write)
    bool offenderHasCamera = false;   // conflict == Camera and the offender bound readable camera rows: offenderCamera holds them
    // conflict == Camera: the entry goes on counting its writers by camera, so the verifier can name the camera the pass's CB2 belongs to
    // (flatRuntimeDeferredDonor's `select`). pairClean holds while every later writer was current, in the entry's layout, full viewport,
    // and bound exactly one of the two cameras; a third camera or any other fault ends it for the rest of the frame.
    bool pairClean = false;
    uint32_t firstWriters = 0, offenderWriters = 0;
    uint64_t offenderLastWriteEpoch = 0; uint32_t offenderLastWriteSeq = 0;
    unsigned char camera[kFlatCameraBytes]{};
    unsigned char offenderCamera[kFlatCameraBytes]{};
};
static_assert(sizeof(FlatRuntimeCameraWitness) <= 448, "keep the camera witness table small: the prefix is rebuilt every frame");
constexpr uint32_t kFlatRuntimeWitnesses = 16;
inline bool flatRuntimeWitnessSameCamera(const FlatRuntimeCameraWitness& a, const FlatRuntimeCameraWitness& b) {
    return a.hasCamera && b.hasCamera && a.b1 == b.b1 && a.cameraHash == b.cameraHash &&
        std::memcmp(a.camera, b.camera, kFlatCameraBytes) == 0;
}
// What reached a resource since its last clear, kept for one question: did the deferred lighting pass's inputs receive anything this frame
// (flatRuntimeEmptyGBuffer). An entry is made only by a clear, so a resource nothing cleared has no entry and that answer is "unknown";
// every later draw, copy, update, resolve or compute write into a resource with an entry is counted against it, and a second clear
// erases the count (it erased the content). A write to a resource with no entry costs a lookup and nothing else, so the buffers the game
// maps by the hundred never fill the table. A table that overflows fails closed: unknown for the rest of the frame.
struct FlatRuntimeReached {
    const void* resource = nullptr;
    uint32_t draws = 0;    // draws that bound it as RTV0 or as the depth, the pairs flatProjectionB1Unread names apart
    uint32_t writes = 0;   // writes into it that are no clear
};
constexpr uint32_t kFlatRuntimeReached = 64;
struct FlatRuntimePrefix {
    FlatRuntimeTarget targets[128]{};
    FlatContractRecord sources[32]{};
    // The source-free camera witness (FlatRuntimeCameraWitness). A table that overflows fails closed: no witness that frame.
    FlatRuntimeCameraWitness witnesses[kFlatRuntimeWitnesses]{};
    uint32_t witnessesUsed = 0;
    bool witnessOverflow = false;
    // The cleared resources and what reached them since (FlatRuntimeReached).
    FlatRuntimeReached reached[kFlatRuntimeReached]{};
    uint32_t reachedUsed = 0;
    bool reachedOverflow = false;
    uint32_t targetsUsed = 0, sourcesUsed = 0, sequence = 0, copies = 0;
    uint32_t imageCopiesAccepted = 0, imageCopiesRefused = 0;
    // Verified image copies that were the first write into their HDR and so conflicted with nothing (FlatRuntimeTarget::imageCopyFirst).
    uint32_t imageCopiesFirstWrite = 0;
    uint32_t menuCopiesAccepted = 0, menuCopiesRefused = 0;
    // The depth-of-field composite's own counts, the failed conditions of its refusals (kDof* bits below, ORed over the frame) and the
    // first conflict its source HDR already carried: the log names them, so one flight reads every cause on the chain.
    uint32_t dofCompositesAccepted = 0, dofCompositesRefused = 0, dofRefusalMask = 0;
    FlatRuntimeConflict dofSourceBad = FlatRuntimeConflict::None;
    uint64_t frame = 0;
    const void* output = nullptr;
    uint32_t width = 0, height = 0, format = 0;
    bool uncertain = false;
    FlatRuntimeUncertainWitness firstUncertain{};
    FlatRuntimeWitness selectedConflict{};
    // The first-person cohort seen so far (FlatRuntimeDraw::firstPersonCohort): the depth it draws into and how many draws. Never
    // part of the contract hash; the copy branch reads it to call the selected frame mixed-camera.
    const void* firstPersonDepth = nullptr;
    uint32_t firstPersonDraws = 0;
    // The draws of a pool family left stock (FlatRuntimeDraw::poolFamilyVs): the selector's FlatMonoFrameInput::unsupportedFamilyDraws.
    uint32_t unsupportedFamilyDraws = 0;
    // Temporal AA is off this frame (flat_ui_layer_math.h flatUiMapTemporalOff: a map or the Full System Scanner is open): no temporal
    // resolve, no jitter, no history. The runtime sets it at the frame boundary from the frame's one GuiFocus read; it is false in every
    // recorded trace, so a replay judges a frame as it always did.
    bool temporalOff = false;
    // The copy branch's outcomes for a target whose only fault is a camera disagreement between its writers
    // (flatRuntimeCameraConflictSpatialOnly). Waived: the frame went on to the selector instead of being refused. Kept: a temporal-off
    // frame whose first fault was the camera disagreement but which other faults also marked, so it was refused as before.
    bool cameraConflictWaived = false, cameraConflictKept = false;
};
// Only a current reducer attestation makes forward-camera refusal global.
// Retained phase-planning H identity is not current authority. Keep the tone
// proof after revocation clears hdrCamera/sets hdrBad, so cancellation still
// invalidates a frame that already depended on the donated camera.
inline bool flatRuntimeOwnsForwardCamera(const FlatRuntimePrefix& p, const void* hdr) {
    if (!hdr || !p.frame) return false;
    for (uint32_t i = 0; i < p.targetsUsed; ++i) {
        const auto& t = p.targets[i];
        if (t.resource == hdr && t.tone.forwardLightCamera && t.tone.key.writeEpoch == p.frame)
            return true;
    }
    return false;
}
inline void flatRuntimeMarkUncertain(FlatRuntimePrefix& p, FlatRuntimeUncertainCause cause,
                                     const void* resource = nullptr, const void* related = nullptr,
                                     uint64_t vs = 0, uint64_t ps = 0, uint32_t detail = 0) {
    if (!p.uncertain) p.firstUncertain = {cause, p.sequence, resource, related, vs, ps, detail};
    p.uncertain = true;
}
struct FlatFailureReceiptWindow {
    static constexpr uint32_t kDetailLimit = 3;
    uint32_t seen = 0, emitted = 0;
    bool admit() { ++seen; if (emitted == kDetailLimit) return false; ++emitted; return true; }
    uint32_t suppressed() const { return seen - emitted; }
    void reset() { seen = emitted = 0; }
};
// Whether the cohort drew into `depth`, the scene depth a selection names: the selected frame is then mixed-camera.
inline bool flatRuntimeFirstPerson(const FlatRuntimePrefix& p, const void* depth) {
    return depth && p.firstPersonDraws && p.firstPersonDepth == depth;
}
// The one place a cause joins a target's record of why it is bad (FlatRuntimeTarget::badCauses).
inline void flatRuntimeNoteBad(FlatRuntimeTarget& t, FlatRuntimeConflict cause) {
    t.badCauses |= 1u << static_cast<uint32_t>(cause);
    t.hdrBad = true;
}
inline void flatRuntimeBad(FlatRuntimeTarget& t, FlatRuntimeConflict cause,
                          uint32_t sequence, const FlatContractRecord& reference,
                          const FlatContractRecord& current) {
    if (t.firstBad.cause == FlatRuntimeConflict::None) {
        t.firstBad.cause = cause; t.firstBad.hdr = t.resource;
        t.firstBad.sequence = sequence; t.firstBad.reference = flatRuntimeWitnessDraw(reference);
        t.firstBad.current = flatRuntimeWitnessDraw(current);
    }
    flatRuntimeNoteBad(t, cause);
}
// A camera disagreement between the writers of an HDR target (the second writer's camera differs from the first camera the target
// saw, FlatRuntimeConflict::CameraChange) can only cost a frame reprojection, and a frame with no temporal resolve, no jitter and no
// history reprojects nothing: the spatial recovery reads the picture and the frame's size. So on such a frame (the prefix's
// temporalOff) a target whose only fault is that disagreement stays selectable. The target must still carry its first camera
// (hdrCamera: the reference the selector reads), keep its layout, and never have been marked bad for any other cause (badCauses
// holds CameraChange alone); every other fault, and every frame with temporal AA on, refuses as before.
inline bool flatRuntimeCameraConflictSpatialOnly(const FlatRuntimePrefix& p, const FlatRuntimeTarget& t) {
    return p.temporalOff && t.hdrBad && t.hdrCamera && !t.hdrLayoutChanged &&
        t.badCauses == (1u << static_cast<uint32_t>(FlatRuntimeConflict::CameraChange));
}
// The question every point that refuses a bad HDR target asks (the copy branch below, and the admission by structure's selection,
// flatSelectHdrRouteAt with its waiver asked): true when the target refuses the frame. The outcome is noted on the prefix for the
// runtime's counters: waived when the camera disagreement alone is let through, kept when a temporal-off frame whose first fault was
// the camera disagreement is refused all the same because other faults also marked the target.
inline bool flatRuntimeBadTargetRefuses(FlatRuntimePrefix& p, const FlatRuntimeTarget& t) {
    if (flatRuntimeCameraConflictSpatialOnly(p, t)) { p.cameraConflictWaived = true; return false; }
    if (p.temporalOff && t.firstBad.cause == FlatRuntimeConflict::CameraChange) p.cameraConflictKept = true;
    return true;
}
inline FlatContractRecord flatRuntimeRecord(const FlatRuntimeDraw& d, uint32_t q, uint64_t frame) {
    FlatContractRecord r{}; r.key = d.key; r.draws = 1; r.first = r.last = q;
    r.firstInstances = r.lastInstances = d.instances;
    (void)frame;
    r.firstWriteEpoch = r.lastWriteEpoch = d.key.writeEpoch;
    r.firstWriteSeq = r.lastWriteSeq = d.key.writeSeq;
    std::memcpy(r.camera, d.camera, sizeof(r.camera)); return r;
}
// sameCamera(a, flatRuntimeRecord(d, q, frame)) without making the record.
inline bool flatRuntimeSameCamera(const FlatContractRecord& a, const FlatRuntimeDraw& d) {
    return flat_mono_detail::cameraIdentity(a) == d.key.b1 && a.key.camera && d.key.camera &&
        a.key.cameraHash == d.key.cameraHash &&
        std::memcmp(a.camera, d.camera, kFlatCameraBytes) == 0;
}
// cameraCurrent(flatRuntimeRecord(d, q, frame), frame) without making the record: one draw at q >= 1 is
// ordered, and its first and last write epoch and sequence are the draw's own.
inline bool flatRuntimeCameraCurrent(const FlatRuntimeDraw& d, uint32_t q, uint64_t frame) {
    const auto& k = d.key;
    return q && k.b1 && k.camera && k.writeEpoch == frame && k.writeSeq && k.writeSeq <= q &&
        (d.cameraHashTrusted || k.cameraHash == flatCameraHash(d.camera));
}
// The witness line's record of a writer: flatRuntimeCameraCurrent's terms, each kept apart.
inline FlatWitnessWriter flatRuntimeWitnessWriter(const FlatRuntimeDraw& d, uint32_t q, uint64_t frame) {
    const auto& k = d.key;
    FlatWitnessWriter w; w.vs = k.vs; w.ps = k.ps; w.b1 = k.b1; w.q = q;
    w.cameraHash = k.cameraHash; w.writeEpoch = k.writeEpoch; w.writeSeq = k.writeSeq;
    unsigned terms = 0;
    if (k.camera) terms |= kWitnessTermCamera;
    if (k.writeEpoch == frame) terms |= kWitnessTermEpoch;
    if (k.writeSeq && k.writeSeq <= q) terms |= kWitnessTermOrdered;
    if (d.cameraHashTrusted) terms |= kWitnessTermTrusted;
    if (k.camera && (d.cameraHashTrusted || k.cameraHash == flatCameraHash(d.camera))) terms |= kWitnessTermHash;
    w.terms = static_cast<uint8_t>(terms);
    return w;
}
// A writer after a Camera conflict: counted against the camera it bound. Anything but a current, full-viewport writer in the entry's layout
// with exactly one of the two cameras ends pairClean for the rest of the frame.
inline void flatRuntimeWitnessPairWriter(FlatRuntimeCameraWitness& e, const FlatRuntimeDraw& d, uint32_t q, uint64_t frame) {
    const auto& k = d.key;
    const bool layout = k.dsv == e.dsv && k.width == e.width && k.height == e.height && k.depthWidth == e.depthWidth &&
        k.depthHeight == e.depthHeight && k.format == e.format && k.depthFormat == e.depthFormat;
    if (!layout || !k.camera || !flat_mono_detail::fullViewport(k, k.width, k.height) || !flatRuntimeCameraCurrent(d, q, frame)) {
        e.pairClean = false; return;
    }
    if (e.hasCamera && e.b1 == k.b1 && e.cameraHash == k.cameraHash && std::memcmp(e.camera, d.camera, kFlatCameraBytes) == 0) {
        ++e.firstWriters; e.lastWriteEpoch = k.writeEpoch; e.lastWriteSeq = k.writeSeq; return;
    }
    if (e.offenderHasCamera && e.offender.b1 == k.b1 && e.offender.cameraHash == k.cameraHash &&
        std::memcmp(e.offenderCamera, d.camera, kFlatCameraBytes) == 0) {
        ++e.offenderWriters; e.offenderLastWriteEpoch = k.writeEpoch; e.offenderLastWriteSeq = k.writeSeq; return;
    }
    e.pairClean = false;
}
// One Screen-kind writer that is not a supported pair, noted while no source record exists on its depth (a source record there means the
// pool rule alone decides, now and for the rest of the frame: records are never removed). The first writer of a (depth, colour) makes the
// entry; every later one either agrees with it or raises the entry's conflict, the first reason kept. The draw's camera rows are already in
// d.camera (the runtime's camera table answers every draw), so nothing is copied here but the first writer's 96 bytes.
inline void flatRuntimeNoteWitness(FlatRuntimePrefix& p, const FlatRuntimeDraw& d, uint32_t q) {
    const auto& k = d.key;
    if (flatProjectionB1Unread(k.vs, k.ps)) return;   // its bound b1 is not a camera it drew with
    for (uint32_t i = 0; i < p.sourcesUsed; ++i) if (p.sources[i].key.depth == k.depth) return;
    FlatRuntimeCameraWitness* e = nullptr;
    for (uint32_t i = 0; i < p.witnessesUsed; ++i)
        if (p.witnesses[i].depth == k.depth && p.witnesses[i].color == k.color) { e = &p.witnesses[i]; break; }
    if (e && e->conflict != FlatWitnessConflict::None) {
        ++e->writers; e->last = q;
        if (e->conflict == FlatWitnessConflict::Camera && e->pairClean) flatRuntimeWitnessPairWriter(*e, d, q, p.frame);
        return;
    }
    const bool full = flat_mono_detail::fullViewport(k, k.width, k.height);
    const bool current = flatRuntimeCameraCurrent(d, q, p.frame);
    if (!e || !e->writers) {   // a new entry, or one a colour write restarted (flatRuntimeWitnessWritten)
        if (!e) {
            if (p.witnessesUsed == kFlatRuntimeWitnesses) { p.witnessOverflow = true; return; }
            e = &p.witnesses[p.witnessesUsed++];
        }
        e->depth = k.depth; e->color = k.color; e->rtv = k.rtv; e->dsv = k.dsv; e->b1 = k.b1;
        e->cameraHash = k.cameraHash; e->writeEpoch = e->lastWriteEpoch = k.writeEpoch;
        e->writeSeq = e->lastWriteSeq = k.writeSeq; e->first = e->last = q; e->writers = 1;
        e->width = k.width; e->height = k.height; e->depthWidth = k.depthWidth; e->depthHeight = k.depthHeight;
        e->format = k.format; e->depthFormat = k.depthFormat;
        e->fullViewport = full; e->hasCamera = k.camera != nullptr; e->cameraCurrent = current;
        e->firstVs = k.vs; e->firstPs = k.ps;
        if (k.camera) std::memcpy(e->camera, d.camera, kFlatCameraBytes);
        return;
    }
    ++e->writers; e->last = q;
    const bool sameCamera = e->hasCamera && k.camera && e->b1 == k.b1 && e->cameraHash == k.cameraHash &&
        std::memcmp(e->camera, d.camera, kFlatCameraBytes) == 0;
    if (k.dsv != e->dsv || k.width != e->width || k.height != e->height || k.depthWidth != e->depthWidth ||
        k.depthHeight != e->depthHeight || k.format != e->format || k.depthFormat != e->depthFormat)
        e->conflict = FlatWitnessConflict::Layout;
    else if (!full) e->conflict = FlatWitnessConflict::Viewport;
    else if (!current) e->conflict = FlatWitnessConflict::CameraProvenance;
    else if (!sameCamera) e->conflict = FlatWitnessConflict::Camera;
    else { e->lastWriteEpoch = k.writeEpoch; e->lastWriteSeq = k.writeSeq; return; }
    e->offender = flatRuntimeWitnessWriter(d, q, p.frame);   // this writer raised the conflict; the first reason is kept above
    if (e->conflict == FlatWitnessConflict::Camera && k.camera) {   // kept so the log can say which camera the pass's CB2 carries
        std::memcpy(e->offenderCamera, d.camera, kFlatCameraBytes); e->offenderHasCamera = true;
        // every writer before the offender bound the first camera (an unconflicted entry's writers agree); the offender is the first to bind the second
        e->pairClean = e->hasCamera && e->cameraCurrent && e->fullViewport;
        e->firstWriters = e->writers - 1; e->offenderWriters = 1;
        e->offenderLastWriteEpoch = k.writeEpoch; e->offenderLastWriteSeq = k.writeSeq;
    }
}
enum class FlatDeferredDonorRole : uint8_t { None, Pool, Witness };
inline const char* flatDeferredDonorRoleName(FlatDeferredDonorRole r) {
    return r == FlatDeferredDonorRole::Pool ? "pool" : r == FlatDeferredDonorRole::Witness ? "witness" : "none";
}
// The failure ownership line's older wording for the same three answers.
inline const char* flatDeferredDonorOwnershipName(FlatDeferredDonorRole r) {
    return r == FlatDeferredDonorRole::Pool ? "qualified-pool" : r == FlatDeferredDonorRole::Witness ? "witness" : "unavailable";
}
// Why no donor, for the witness line. Uncertain: the prefix is. Shape: the draw is not the deferred lighting pass the resolver can judge
// (its pair, targets, views or viewport). PoolRule: a source record exists on the depth, so the pool rule alone decided and said no.
// EmptyGBuffer is no donor and no refusal: nothing a camera placed reached the pass's inputs this frame (flatRuntimeEmptyGBuffer), so there
// is no camera for its rows to be verified against and none for them to misplace. GBufferReached: no writer on the depth, yet something
// the witness cannot see (a draw with no depth, a copy, a compute write) reached the inputs, so the pass stays unverified and refused.
enum class FlatDeferredDonorWhy : uint8_t {
    None, Uncertain, Shape, PoolRule, NoEntry, Overflow, Conflict, SameColor, Layout, Viewport, Camera, Late, OtherCamera,
    EmptyGBuffer, GBufferReached, Count
};
inline const char* flatDeferredDonorWhyName(FlatDeferredDonorWhy w) {
    switch (w) {
    case FlatDeferredDonorWhy::Uncertain: return "prefix-uncertain";
    case FlatDeferredDonorWhy::Shape: return "not-the-deferred-pass-shape";
    case FlatDeferredDonorWhy::PoolRule: return "pool-rule-refused";
    case FlatDeferredDonorWhy::NoEntry: return "no-witness-entry";
    case FlatDeferredDonorWhy::Overflow: return "witness-table-overflow";
    case FlatDeferredDonorWhy::Conflict: return "witness-conflict";
    case FlatDeferredDonorWhy::SameColor: return "witness-is-own-target";
    case FlatDeferredDonorWhy::Layout: return "witness-layout";
    case FlatDeferredDonorWhy::Viewport: return "witness-viewport";
    case FlatDeferredDonorWhy::Camera: return "witness-camera-not-current";
    case FlatDeferredDonorWhy::Late: return "witness-not-before-draw";
    case FlatDeferredDonorWhy::OtherCamera: return "second-camera-on-depth";
    case FlatDeferredDonorWhy::EmptyGBuffer: return "empty-gbuffer";
    case FlatDeferredDonorWhy::GBufferReached: return "gbuffer-reached";
    default: return "accepted";
    }
}
// ---- what reached the deferred lighting pass's inputs ----------------------------------------------------------
inline FlatRuntimeReached* flatRuntimeReachedFind(FlatRuntimePrefix& p, const void* resource) {
    if (!resource) return nullptr;
    for (uint32_t i = 0; i < p.reachedUsed; ++i) if (p.reached[i].resource == resource) return &p.reached[i];
    return nullptr;
}
inline const FlatRuntimeReached* flatRuntimeReachedFind(const FlatRuntimePrefix& p, const void* resource) {
    if (!resource) return nullptr;
    for (uint32_t i = 0; i < p.reachedUsed; ++i) if (p.reached[i].resource == resource) return &p.reached[i];
    return nullptr;
}
// A write into `resource` that is not a draw: a clear (constantFill) makes or resets its entry, anything else is counted on an entry that
// exists and ignored where none does. The clear is the only way in, which is the point: the rule asks for an input the game cleared this
// frame and nothing wrote since, and what was written before a clear is gone.
inline void flatRuntimeReachedWrite(FlatRuntimePrefix& p, const void* resource, bool constantFill) {
    if (!resource) return;
    FlatRuntimeReached* e = flatRuntimeReachedFind(p, resource);
    if (!constantFill) { if (e) ++e->writes; return; }
    if (!e) {
        if (p.reachedUsed == kFlatRuntimeReached) { p.reachedOverflow = true; return; }
        e = &p.reached[p.reachedUsed++];
        e->resource = resource;
    }
    e->draws = e->writes = 0;
}
// A draw's two bindings. The pairs that never read the b1 they happen to have bound (flatProjectionB1Unread: the inert filters, the
// sun-shadow resolve, the star's depth-only pre-pass) are not counted: they are what the witness already looks past. The first two write
// nothing a camera placed. The star pre-pass is different: it writes depth with the jittered b0 matrices, so a Detailed Surface Scanner
// frame whose only draw on the depth is the star pre-pass is judged empty here although its depth holds star fragments. Whether that
// shows on screen is not flown yet.
inline void flatRuntimeReachedDraw(FlatRuntimePrefix& p, const FlatContractObservation& k) {
    if (!p.reachedUsed || flatProjectionB1Unread(k.vs, k.ps)) return;
    for (uint32_t i = 0; i < p.reachedUsed; ++i) {
        auto& e = p.reached[i];
        if ((k.depth && e.resource == k.depth) || (k.color && e.resource == k.color)) ++e.draws;
    }
}
// Whether the deferred lighting pass reads inputs a camera placed nothing in this frame. `gbuffer` is the pass's SRV0..SRV2 resources.
// Empty needs all of: no pool source and no witness entry on the depth (nothing with a camera drew there); the depth and the three inputs
// cleared this frame, the depth and SRV0 with no draw bound to them since (a draw is counted unless its pair never reads b1) and none of
// the four with a copy, update, resolve or compute write since; the HDR cleared this frame, not written, and not bad. SRV1 and SRV2 are
// not charged for a draw of their own: the game's last G-buffer pass draws the alpha of SRV2 with no depth bound in every frame (a
// full-screen triangle, mask 8), a draw that places nothing; geometry that placed something bound the depth and is a source or a writer
// above. Reached: the writer tables are empty and something else reached an input, which is what the answer is to say. Unknown: anything
// less than the whole, the prefix being uncertain among it.
enum class FlatEmptyGBuffer : uint8_t { Unknown, Reached, Empty };
inline FlatEmptyGBuffer flatRuntimeEmptyGBuffer(const FlatRuntimePrefix& p, const FlatContractObservation& k, const void* const* gbuffer) {
    if (p.uncertain || p.reachedOverflow || p.witnessOverflow || !gbuffer || !k.depth || !k.color) return FlatEmptyGBuffer::Unknown;
    if (gbuffer[0] != k.srvResource[0]) return FlatEmptyGBuffer::Unknown;
    for (uint32_t i = 0; i < 3; ++i)
        if (!gbuffer[i] || gbuffer[i] == k.color || gbuffer[i] == k.depth) return FlatEmptyGBuffer::Unknown;
    for (uint32_t i = 0; i < p.sourcesUsed; ++i) if (p.sources[i].key.depth == k.depth) return FlatEmptyGBuffer::Unknown;
    for (uint32_t i = 0; i < p.witnessesUsed; ++i) if (p.witnesses[i].depth == k.depth) return FlatEmptyGBuffer::Unknown;
    for (uint32_t i = 0; i < p.targetsUsed; ++i) {
        const auto& t = p.targets[i];
        if (t.resource == k.color && (t.writes.draws || t.hdrBad || t.hdrCamera || t.hdrLayoutChanged || t.imageCopyFirst))
            return FlatEmptyGBuffer::Unknown;
    }
    const auto* hdr = flatRuntimeReachedFind(p, k.color);
    if (!hdr || hdr->draws || hdr->writes) return FlatEmptyGBuffer::Unknown;
    const auto* depth = flatRuntimeReachedFind(p, k.depth);
    const auto* in0 = flatRuntimeReachedFind(p, gbuffer[0]);
    const auto* in1 = flatRuntimeReachedFind(p, gbuffer[1]);
    const auto* in2 = flatRuntimeReachedFind(p, gbuffer[2]);
    if (!depth || !in0 || !in1 || !in2) return FlatEmptyGBuffer::Unknown;
    if (depth->draws || depth->writes || in0->draws || in0->writes || in1->writes || in2->writes) return FlatEmptyGBuffer::Reached;
    return FlatEmptyGBuffer::Empty;
}

// What the deferred lighting pass is verified against: one camera, from one of two proofs, or none. The pool fields are a source record's;
// the witness fields an entry's. camera is the 96 rows (null with no donor) and lives in the prefix. The diagnostic block is filled
// whether or not a donor comes back.
struct FlatDeferredDonor {
    FlatDeferredDonorRole role = FlatDeferredDonorRole::None;
    FlatDeferredDonorWhy why = FlatDeferredDonorWhy::Uncertain;
    const void* color = nullptr, *b1 = nullptr;
    const unsigned char* camera = nullptr;
    uint64_t cameraHash = 0, writeEpoch = 0, lastWriteEpoch = 0;
    uint32_t writeSeq = 0, lastWriteSeq = 0, first = 0, last = 0;
    uint32_t poolRecords = 0;      // source records of any kind on the depth
    uint32_t witnessEntries = 0;   // witness entries on the depth
    uint32_t witnessCameras = 0;   // distinct cameras among them
    uint32_t writers = 0;          // writers into the (depth, SRV0) entry
    bool entry = false, overflow = false;
    FlatWitnessConflict conflict = FlatWitnessConflict::None;
    uint64_t entryCameraHash = 0;
    const void* entryB1 = nullptr;
    // The entry's first writer (q 0 with no entry) and the writer that raised its conflict (q 0 for none), as the entry kept them.
    uint32_t entryFirst = 0;
    uint64_t entryFirstVs = 0, entryFirstPs = 0;
    bool entryHasCamera = false;
    FlatWitnessWriter offender;
    // The entry's first camera and, for conflict == Camera, the offender's (null when absent). Log only: neither is ever a donor.
    const unsigned char* entryCamera = nullptr, *offenderCamera = nullptr;
    // conflict == Camera: the entry's writers by camera, and whether the verifier may name one of the two (pairSelectable: the entry is the
    // depth's only one, clean, current, in this draw's layout and written before q). selected: this donor is the camera the caller named.
    uint32_t firstWriters = 0, offenderWriters = 0;
    bool pairClean = false, pairSelectable = false, selected = false;
    // The first other entry on the depth that carried another camera (why == OtherCamera): its colour, b1 and camera hash.
    const void* vetoColor = nullptr, *vetoB1 = nullptr;
    uint64_t vetoHash = 0;
    explicit operator bool() const { return role != FlatDeferredDonorRole::None; }
};
// A laser-family H draw is normally an alternate-camera weapon draw. It may
// establish the scene camera only when the already-qualified pool source on
// this exact depth proves that its current b1 rows are the scene's own camera
// from before H's first write, and the pending camera-independent image source
// names the same scene depth. The image copy's bound b1 is never evidence.

// The one donor resolver: the verifier, its binding-failure path and the model's qualification all ask it, so they cannot disagree.
// (a) The pool rule is the single qualified source record on the depth, writing the draw's SRV0. (b) Only where no source record of any
// kind exists on the depth does the witness stand in. (c) It must be an unconflicted entry for (depth, SRV0) of another target, of the
// draw's dsv, depth format and extent, with a full viewport and a current camera, written before q; (d) and no other entry on the
// depth may carry a different camera, so a second camera on the depth fails closed. (e) Where no source record and no entry stand on the
// depth at all, the caller that names the pass's three inputs (`gbuffer`, SRV0..SRV2) can have the answer "empty": no donor, and
// why == EmptyGBuffer rather than NoEntry (flatRuntimeEmptyGBuffer). It is never a donor: role stays None, so nothing that asks a donor
// for a camera gets one.
inline FlatDeferredDonor flatRuntimeDeferredDonor(const FlatRuntimePrefix& p,
        const FlatContractObservation& k,uint32_t q,const void* const* gbuffer=nullptr,const unsigned char* select=nullptr) {
    using namespace flat_mono_detail;
    FlatDeferredDonor out;
    if(k.depth)for(uint32_t i=0;i<p.sourcesUsed;++i)if(p.sources[i].key.depth==k.depth)++out.poolRecords;
    out.overflow=p.witnessOverflow;
    if(p.uncertain)return out;
    out.why=FlatDeferredDonorWhy::Shape;
    if(k.format!=kFlatDeferredHdrFormat || k.vs!=kFlatDeferredHdrVs || k.ps!=kFlatDeferredHdrPs ||
       !k.color || !k.rtv || !k.depth || !k.dsv || k.depthWidth!=k.width || k.depthHeight!=k.height ||
       !k.srvView[0] || !k.srvResource[0] || !fullViewport(k,k.width,k.height))return out;
    if(out.poolRecords) {
        out.why=FlatDeferredDonorWhy::PoolRule;
        const FlatContractRecord* donor=nullptr;
        for(uint32_t i=0;i<p.sourcesUsed;++i) {
            const auto& r=p.sources[i];if(r.key.depth!=k.depth)continue;
            if(donor || r.key.kind!=kFlatContractPool || !r.key.color || !r.key.rtv ||
               r.key.color!=k.srvResource[0] || r.key.color==k.color || r.key.dsv!=k.dsv ||
               r.key.depthFormat!=k.depthFormat || r.key.width!=k.width || r.key.height!=k.height ||
               r.key.depthWidth!=k.width || r.key.depthHeight!=k.height ||
               !fullViewport(r.key,k.width,k.height) || !cameraCurrent(r,p.frame) || r.last>=q)return out;
            donor=&r;
        }
        if(!donor)return out;
        out.role=FlatDeferredDonorRole::Pool;out.why=FlatDeferredDonorWhy::None;
        out.color=donor->key.color;out.b1=donor->key.b1;out.camera=donor->key.camera?donor->camera:nullptr;
        out.cameraHash=donor->key.cameraHash;out.writeEpoch=donor->key.writeEpoch;out.writeSeq=donor->key.writeSeq;
        out.lastWriteEpoch=donor->lastWriteEpoch;out.lastWriteSeq=donor->lastWriteSeq;out.first=donor->first;out.last=donor->last;
        return out;
    }
    const FlatRuntimeCameraWitness* e=nullptr;
    for(uint32_t i=0;i<p.witnessesUsed;++i) {
        const auto& w=p.witnesses[i];if(w.depth!=k.depth)continue;
        ++out.witnessEntries;if(w.color==k.srvResource[0])e=&w;
        bool seen=false;
        if(w.hasCamera)for(uint32_t j=0;j<i&&!seen;++j)
            seen=p.witnesses[j].depth==k.depth && flatRuntimeWitnessSameCamera(p.witnesses[j],w);
        if(w.hasCamera && !seen)++out.witnessCameras;
    }
    if(e){out.entry=true;out.writers=e->writers;out.conflict=e->conflict;out.entryCameraHash=e->hasCamera?e->cameraHash:0;out.entryB1=e->b1;
        out.entryFirst=e->first;out.entryFirstVs=e->firstVs;out.entryFirstPs=e->firstPs;out.entryHasCamera=e->hasCamera;out.offender=e->offender;
        out.entryCamera=e->hasCamera?e->camera:nullptr;out.offenderCamera=e->offenderHasCamera?e->offenderCamera:nullptr;}
    if(p.witnessOverflow){out.why=FlatDeferredDonorWhy::Overflow;return out;}
    if(!e){
        out.why=FlatDeferredDonorWhy::NoEntry;
        if(!out.witnessEntries) {
            const auto empty=flatRuntimeEmptyGBuffer(p,k,gbuffer);
            if(empty==FlatEmptyGBuffer::Empty)out.why=FlatDeferredDonorWhy::EmptyGBuffer;
            else if(empty==FlatEmptyGBuffer::Reached)out.why=FlatDeferredDonorWhy::GBufferReached;
        }
        return out;
    }
    if(e->conflict!=FlatWitnessConflict::None) {
        out.why=FlatDeferredDonorWhy::Conflict;
        if(e->conflict!=FlatWitnessConflict::Camera)return out;
        // Two cameras drew into the entry. Neither is a donor on the entry's say-so, but where the entry is the depth's only one, kept
        // every writer's camera (pairClean), and stands in this draw's layout, the caller that holds the pass's CB2 may name the one the
        // CB2 belongs to (`select`: that camera's 96 bytes). Nothing is selected unless the bytes name exactly one of the two.
        out.firstWriters=e->firstWriters;out.offenderWriters=e->offenderWriters;out.pairClean=e->pairClean;
        out.pairSelectable=out.witnessEntries==1 && e->pairClean && e->hasCamera && e->cameraCurrent && e->offenderHasCamera &&
            e->writers && e->color!=k.color && e->rtv && e->dsv==k.dsv && e->depthFormat==k.depthFormat && e->width==k.width &&
            e->height==k.height && e->depthWidth==k.width && e->depthHeight==k.height && e->fullViewport && e->last<q;
        if(!out.pairSelectable || !select)return out;
        const bool isFirst=std::memcmp(select,e->camera,kFlatCameraBytes)==0,isOffender=std::memcmp(select,e->offenderCamera,kFlatCameraBytes)==0;
        if(isFirst==isOffender)return out;
        out.role=FlatDeferredDonorRole::Witness;out.why=FlatDeferredDonorWhy::None;out.selected=true;
        out.color=e->color;out.last=e->last;
        if(isFirst) {
            out.b1=e->b1;out.camera=e->camera;out.cameraHash=e->cameraHash;out.writeEpoch=e->writeEpoch;out.writeSeq=e->writeSeq;
            out.lastWriteEpoch=e->lastWriteEpoch;out.lastWriteSeq=e->lastWriteSeq;out.first=e->first;
        } else {
            out.b1=e->offender.b1;out.camera=e->offenderCamera;out.cameraHash=e->offender.cameraHash;out.writeEpoch=e->offender.writeEpoch;
            out.writeSeq=e->offender.writeSeq;out.lastWriteEpoch=e->offenderLastWriteEpoch;out.lastWriteSeq=e->offenderLastWriteSeq;
            out.first=e->offender.q;
        }
        return out;
    }
    if(!e->writers){out.why=FlatDeferredDonorWhy::NoEntry;return out;}   // a colour write restarted it, no writer since
    if(e->color==k.color){out.why=FlatDeferredDonorWhy::SameColor;return out;}
    if(!e->rtv || e->dsv!=k.dsv || e->depthFormat!=k.depthFormat || e->width!=k.width || e->height!=k.height ||
       e->depthWidth!=k.width || e->depthHeight!=k.height){out.why=FlatDeferredDonorWhy::Layout;return out;}
    if(!e->fullViewport){out.why=FlatDeferredDonorWhy::Viewport;return out;}
    if(!e->hasCamera || !e->cameraCurrent){out.why=FlatDeferredDonorWhy::Camera;return out;}
    if(e->last>=q){out.why=FlatDeferredDonorWhy::Late;return out;}
    for(uint32_t i=0;i<p.witnessesUsed;++i) {
        const auto& w=p.witnesses[i];if(&w==e || w.depth!=k.depth)continue;
        if(w.conflict==FlatWitnessConflict::Camera || w.conflict==FlatWitnessConflict::CameraProvenance ||
           (w.hasCamera && !flatRuntimeWitnessSameCamera(w,*e))){
            out.why=FlatDeferredDonorWhy::OtherCamera;
            out.vetoColor=w.color;out.vetoB1=w.b1;out.vetoHash=w.hasCamera?w.cameraHash:0;
            return out;
        }
    }
    out.role=FlatDeferredDonorRole::Witness;out.why=FlatDeferredDonorWhy::None;
    out.color=e->color;out.b1=e->b1;out.camera=e->camera;out.cameraHash=e->cameraHash;
    out.writeEpoch=e->writeEpoch;out.writeSeq=e->writeSeq;out.lastWriteEpoch=e->lastWriteEpoch;out.lastWriteSeq=e->lastWriteSeq;
    out.first=e->first;out.last=e->last;
    return out;
}
inline bool flatRuntimeQualifiedDeferredCamera(const FlatRuntimePrefix& p,const FlatRuntimeTarget& t,
        const FlatRuntimeDraw& d,uint32_t q) {
    if(!d.deferredHdrVerified || d.key.camera || t.writes.draws || t.hdrBad || t.hdrCamera || t.hdrLayoutChanged)return false;
    // d.camera is the camera the verifier took (the donor's); only a Camera-conflict entry reads it, as the camera the verifier named.
    const auto donor=flatRuntimeDeferredDonor(p,d.key,q,nullptr,d.camera);const auto& projection=d.key.projection[1];
    return donor && d.key.cameraHash==donor.cameraHash &&
        std::memcmp(d.camera,donor.camera,kFlatCameraBytes)==0 && projection.resource &&
        projection.status==FlatProjectionStatus::Available && projection.width>=kFlatDeferredCameraMinWidth &&
        projection.copied==kFlatDeferredCameraBytes && projection.hash && projection.writeEpoch==p.frame &&
        projection.writeSeq && projection.writeSeq<=q;
}
// Runtime's exact-resource/used-transform attestation has its own type. An
// inherited scene identity never pretends the light actually bound VS b1.
inline const FlatContractRecord* flatRuntimeForwardDonor(const FlatRuntimePrefix& p,
        const FlatContractObservation& k,uint32_t q) {
    if(p.uncertain||k.vs!=forward_light::kVs||k.ps!=forward_light::kPs||k.format!=26||
       !k.depth||!k.dsv||!k.color||!k.rtv||!flat_mono_detail::fullViewport(k,k.width,k.height))return nullptr;
    const FlatContractRecord* donor=nullptr;
    for(unsigned i=0;i<p.sourcesUsed;++i){const auto& r=p.sources[i];if(r.key.depth!=k.depth)continue;
        if(r.key.kind!=kFlatContractPool||!r.key.color||!r.key.rtv||r.key.dsv!=k.dsv||
           r.key.width!=k.width||r.key.height!=k.height||r.key.depthWidth!=k.width||r.key.depthHeight!=k.height||
           r.key.depthFormat!=k.depthFormat||!flat_mono_detail::fullViewport(r.key,k.width,k.height)||
           !flat_mono_detail::cameraCurrent(r,p.frame)||r.last>=q)return nullptr;
        if(donor&&(donor->key.b1!=r.key.b1||donor->key.cameraHash!=r.key.cameraHash||
                   std::memcmp(donor->camera,r.camera,kFlatCameraBytes)))return nullptr;
        donor=&r;
    }return donor;
}
inline bool flatRuntimeQualifiedForwardCamera(const FlatRuntimePrefix& p,const FlatRuntimeTarget& t,
        const FlatRuntimeDraw& d,uint32_t q) {
    if(!d.forwardHdrVerified||!d.forwardUpstreamPhase||d.key.camera||t.writes.draws||t.hdrBad||t.hdrCamera||t.hdrLayoutChanged)return false;
    const auto* donor=flatRuntimeForwardDonor(p,d.key,q);const auto& cb=d.key.projection[1];
    return donor&&d.key.cameraHash==donor->key.cameraHash&&!std::memcmp(d.camera,donor->camera,kFlatCameraBytes)&&
        cb.resource&&cb.width>=forward_light::kBytes&&cb.copied==forward_light::kBytes&&cb.hash&&
        cb.status==FlatProjectionStatus::Available&&cb.writeEpoch==p.frame&&cb.writeSeq&&cb.writeSeq<q;
}
inline bool flatRuntimeQualifiedLaserSceneCamera(const FlatRuntimePrefix& p,
                                                 const FlatRuntimeTarget& t,
                                                 const FlatRuntimeDraw& d, uint32_t q) {
    using namespace flat_mono_detail;
    const auto& k = d.key;
    if (p.uncertain || t.hdrBad || t.hdrLayoutChanged || !t.writes.draws ||
        t.writes.key.format != 26 || t.writes.key.width != k.width ||
        t.writes.key.height != k.height || t.writes.key.depth != k.depth ||
        t.writes.key.dsv != k.dsv || t.writes.key.depthFormat != k.depthFormat ||
        !flatRuntimeCameraCurrent(d, q, p.frame)) return false;
    uint32_t sameDepthSources = 0;
    for (uint32_t i = 0; i < p.sourcesUsed; ++i) {
        const auto& source = p.sources[i];
        if (source.key.depth != k.depth) continue;
        ++sameDepthSources;
        if (source.key.kind != kFlatContractPool || !source.key.color || !source.key.rtv ||
            source.key.color == k.color || source.key.dsv != k.dsv ||
            source.key.depthFormat != k.depthFormat || source.key.width != k.width ||
            source.key.height != k.height || source.key.depthWidth != k.width ||
            source.key.depthHeight != k.height || !fullViewport(source.key, k.width, k.height) ||
            !cameraCurrent(source, p.frame) || !flatRuntimeSameCamera(source, d) ||
            source.last >= t.writes.first) return false;
    }
    if (sameDepthSources != 1) return false;
    uint32_t imageSources = 0;
    for (uint32_t i = 0; i < p.targetsUsed; ++i) {
        const auto& image = p.targets[i];
        const auto& r = image.writes;
        if (!r.draws || r.key.format != 9 || r.key.depth != k.depth) continue;
        ++imageSources;
        if (image.hdrBad || image.imageSourceBad || image.imageHasCamera || r.key.camera ||
            !flatRuntimeCameraIndependentImageSourcePair(r.key.vs, r.key.ps) ||
            !r.key.rtv || r.key.dsv != k.dsv || r.key.depthFormat != k.depthFormat ||
            r.key.width != k.width || r.key.height != k.height ||
            r.key.depthWidth != k.width || r.key.depthHeight != k.height ||
            !fullViewport(r.key, k.width, k.height) ||
            r.first <= t.writes.first || r.last >= q) return false;
    }
    return imageSources == 1;
}
inline FlatRuntimeTarget* flatRuntimeTarget(FlatRuntimePrefix& p, const void* resource) {
    for (uint32_t i = 0; i < p.targetsUsed; ++i) if (p.targets[i].resource == resource) return &p.targets[i];
    if (!resource || p.targetsUsed == 128) {
        flatRuntimeMarkUncertain(p, !resource ? FlatRuntimeUncertainCause::MissingTarget : FlatRuntimeUncertainCause::TargetCapacity,
                                 resource);
        return nullptr;
    }
    auto& t = p.targets[p.targetsUsed++]; t.resource = resource; return &t;
}
// A write to a witnessed colour replaces what its writers drew there (a deferred renderer clears the
// G-buffer after a depth prepass that bound it with colour writes off): the entry restarts empty, and
// only writers after the write speak for the colour. A write to the witnessed depth is content no
// writer drew, and a later colour write does not undo it: that conflict stays.
inline void flatRuntimeWitnessWritten(FlatRuntimePrefix& p, const void* resource) {
    for (uint32_t i = 0; i < p.witnessesUsed; ++i) {
        auto& w = p.witnesses[i];
        if (w.color == resource) {
            const void* const depth = w.depth;
            const bool depthWritten = w.conflict == FlatWitnessConflict::Written;
            w = FlatRuntimeCameraWitness{};
            w.depth = depth; w.color = resource;
            if (depthWritten) w.conflict = FlatWitnessConflict::Written;
        } else if (w.depth == resource && w.conflict == FlatWitnessConflict::None) {
            w.conflict = FlatWitnessConflict::Written;
        }
    }
}
inline void flatRuntimeWritten(FlatRuntimePrefix& p, const void* resource, bool constantFill = false) {
    for (uint32_t i = 0; i < p.targetsUsed; ++i) {
        auto& t = p.targets[i];
        if (t.overlayOpen && resource == t.overlayDepth) {
            flatRuntimeBad(t, FlatRuntimeConflict::OverlaySuffix, p.sequence,
                           t.tone, FlatContractRecord{});
        }
        if (t.resource != resource || !t.writes.draws) continue;
        flatRuntimeBad(t, FlatRuntimeConflict::ExplicitWrite, p.sequence,
                       t.writes, FlatContractRecord{});
        t.tones = 0;
        if (t.writes.key.format == 9) t.imageSourceBad = true;
    }
    for (uint32_t i = 0; i < p.sourcesUsed; ++i) if (p.sources[i].key.depth == resource) p.sources[i].key.camera = nullptr;
    flatRuntimeWitnessWritten(p, resource);
    flatRuntimeReachedWrite(p, resource, constantFill);   // constantFill: a clear, which carries no content
}
inline void flatRuntimeOverlayFailed(FlatRuntimePrefix& p, const void* resource = nullptr) {
    for (uint32_t i = 0; i < p.targetsUsed; ++i) {
        auto& t = p.targets[i];
        if (t.overlayOpen && (!resource || t.resource == resource))
            flatRuntimeBad(t, FlatRuntimeConflict::OverlaySuffix, p.sequence,
                           t.tone, FlatContractRecord{});
    }
}
inline bool flatRuntimeOverlaySeal(FlatRuntimePrefix& p, const void* resource) {
    for(uint32_t i=0;i<p.targetsUsed;++i) {
        auto& t=p.targets[i];
        if(t.resource==resource && t.overlayOpen && !t.hdrBad) {
            t.overlayOpen=false;
            return true;
        }
    }
    return false;
}
inline void flatRuntimeComputeWritten(FlatRuntimePrefix& p, const void* resource) {
    // Lighting may write the original scene HDR before the menu copy. An
    // inherited destination has no such exemption: its contents are a copy.
    if (!p.menuCopiesAccepted && !p.dofCompositesAccepted) return;
    for (uint32_t i = 0; i < p.targetsUsed; ++i)
        if (p.targets[i].resource == resource && (p.targets[i].menuInherited || p.targets[i].dofInherited)) {
            flatRuntimeWritten(p, resource);
            return;
        }
}
// The dispatch path's full UAV-write guard (flat_runtime.cpp's dispatch
// scope), shared with the trace replay: the completed handoff and its HDR
// input refuse GPU writes after lighting's legitimate pre-tone window.
inline void flatRuntimeDispatchObserveWritten(FlatRuntimePrefix& p, const void* resource) {
    for (uint32_t i = 0; i < p.targetsUsed; ++i) {
        const auto& t = p.targets[i];
        if (t.overlayOpen && (t.resource == resource || t.overlayDepth == resource)) {
            flatRuntimeOverlayFailed(p, t.resource);
            break;
        }
    }
    flatRuntimeComputeWritten(p, resource);
    for (uint32_t i = 0; i < p.targetsUsed; ++i) {
        auto& target = p.targets[i];
        const void* const hdrInput = flat_mono_detail::toneHdrInput(target.tone.key);
        if (target.tones && (target.resource == resource || (hdrInput && hdrInput == resource)))
            flatRuntimeMarkUncertain(p, FlatRuntimeUncertainCause::ToneUavWrite, resource, target.resource);
    }
    for (uint32_t i = 0; i < p.sourcesUsed; ++i) if (p.sources[i].key.depth == resource)
        flatRuntimeMarkUncertain(p, FlatRuntimeUncertainCause::SourceUavWrite, resource, p.sources[i].key.color);
    flatRuntimeWitnessWritten(p, resource);
    flatRuntimeReachedWrite(p, resource, false);
}
// Whether the selected frame's contract came through the verified menu HDR copy: the HDR the
// tone pass reads (`hdr`, the selection's) is the copy's inherited destination. That is the 3D
// main menu -- a ship on its pedestal -- and only that: an ordinary frame's HDR is written by
// scene draws and never inherited, so this is false for every flight, station and on-foot frame
// the corpus holds. FlatMonoResolveFrame::staticScene is exactly this (the menu-scoped
// stale-slot policy), so the answer has one definition, here, and the trace replay asks the same
// function the runtime does.
inline bool flatFrameThroughMenuCopy(const FlatRuntimePrefix& p, const void* hdr) {
    if (!hdr || !p.menuCopiesAccepted) return false;
    for (uint32_t i = 0; i < p.targetsUsed; ++i)
        if (p.targets[i].resource == hdr) return p.targets[i].menuInherited;
    return false;
}
// A depthless HDR image made from a verified scene HDR (the menu copy; the depth-of-field composite): it comes into existence at the draw,
// and its scene and camera provenance is the source's, never the draw's own b1. `cur` is the draw's record.
inline void flatRuntimeInheritSceneHdr(FlatRuntimeTarget& t, const FlatRuntimeTarget& source, const FlatContractRecord& cur) {
    t.writes = cur;
    t.writes.key.depth = source.writes.key.depth;
    t.writes.key.dsv = source.writes.key.dsv;
    t.writes.key.depthWidth = source.writes.key.depthWidth;
    t.writes.key.depthHeight = source.writes.key.depthHeight;
    t.writes.key.depthFormat = source.writes.key.depthFormat;
    t.writes.key.camera = nullptr;
    t.tone = cur;
    t.tone.key.depth = t.writes.key.depth;
    t.tone.key.dsv = t.writes.key.dsv;
    t.tone.key.depthWidth = t.writes.key.depthWidth;
    t.tone.key.depthHeight = t.writes.key.depthHeight;
    t.tone.key.depthFormat = t.writes.key.depthFormat;
    t.tone.key.b1 = source.tone.key.b1;
    t.tone.deferredCamera = source.tone.deferredCamera;
    t.tone.forwardLightCamera = source.tone.forwardLightCamera;
    t.tone.deferredInverseRepaired = source.tone.deferredInverseRepaired;
    t.tone.sceneCameraB1 = source.tone.sceneCameraB1;
    t.tone.sceneCameraWriteEpoch = source.tone.sceneCameraWriteEpoch;
    t.tone.sceneCameraWriteSeq = source.tone.sceneCameraWriteSeq;
    if (t.tone.deferredCamera) t.tone.key.projection[1] = source.tone.key.projection[1];
    t.tone.key.cameraHash = source.tone.key.cameraHash;
    t.tone.firstWriteEpoch = t.tone.lastWriteEpoch = source.tone.firstWriteEpoch;
    t.tone.firstWriteSeq = t.tone.lastWriteSeq = source.tone.firstWriteSeq;
    std::memcpy(t.tone.camera, source.tone.camera, sizeof(t.tone.camera));
    t.tone.key.camera = t.tone.camera;
    t.hdrCamera = true;
}
// The depth-of-field composite's failed conditions, one bit each (FlatRuntimePrefix::dofRefusalMask, the log's last-refusal-mask).
constexpr uint32_t kDofNotVerified = 1u << 0, kDofNoInput = 1u << 1, kDofNoSource = 1u << 2, kDofSourceNoDraws = 1u << 3,
                   kDofSourceBad = 1u << 4, kDofSourceLayout = 1u << 5, kDofSourceNoCamera = 1u << 6, kDofSourceFormat = 1u << 7,
                   kDofSourceSize = 1u << 8, kDofSourceDepth = 1u << 9, kDofSourceOrder = 1u << 10, kDofSourceStale = 1u << 11,
                   kDofSourceCamera = 1u << 12, kDofDestination = 1u << 13, kDofOutputDepth = 1u << 14, kDofViewport = 1u << 15;
// Optional sink for the copy branch's assembled fixture records: the frame
// contract's carrier (flat_frame_contract.h). count is set only when the
// copy branch ran, so count != 0 means the reducer produced a contract.
struct FlatRuntimeContractSink {
    FlatContractRecord* records = nullptr;
    uint32_t capacity = 0, count = 0;
};
inline FlatMonoFrame flatRuntimeObserve(FlatRuntimePrefix& p, const FlatRuntimeDraw& d,
                                        FlatRuntimeContractSink* sink) {
    using namespace flat_mono_detail;
    FlatMonoFrame out{}; out.frame = out.epoch = p.frame;
    const uint32_t q = ++p.sequence;
    // A typed deferred witness cannot simultaneously claim an actual VS b1 camera.
    if((d.forwardHdrVerified&&(d.key.camera||!d.forwardUpstreamPhase))||
       (d.forwardUpstreamPhase&&!d.forwardHdrVerified)){
        flatRuntimeMarkUncertain(p, FlatRuntimeUncertainCause::ForwardIdentity,d.key.color,d.key.depth,d.key.vs,d.key.ps);return out;}
    if((d.deferredHdrVerified && d.key.camera) || (d.deferredInverseRepaired && !d.deferredHdrVerified)){
        flatRuntimeMarkUncertain(p, FlatRuntimeUncertainCause::DeferredIdentity,d.key.color,d.key.depth,d.key.vs,d.key.ps);return out;}
    // The draw's record (about 1.2 KB) is made only where it is stored or a witness takes it: most draws
    // only extend their target's record. The camera tests ask the draw directly (flatRuntimeSameCamera,
    // flatRuntimeCameraCurrent), with the record's own answers.
    std::optional<FlatContractRecord> made;
    const auto current = [&]() -> const FlatContractRecord& {
        if (!made) made.emplace(flatRuntimeRecord(d, q, p.frame));
        return *made;
    };
    // flatRuntimeBad with the record made only when it becomes the target's first witness.
    const auto bad = [&](FlatRuntimeTarget& target, FlatRuntimeConflict cause, const FlatContractRecord& reference) {
        if (target.firstBad.cause == FlatRuntimeConflict::None) flatRuntimeBad(target, cause, q, reference, current());
        else flatRuntimeNoteBad(target, cause);
    };
    const auto& k = d.key;
    const bool copy = k.vs == kCopyVs && k.ps == kCopyPs && k.color == p.output;
    if (copy) {
        p.selectedConflict = FlatRuntimeWitness{};
        ++p.copies;
        if (p.uncertain || p.copies != 1) { out.reason = FlatMonoReason::Truncated; return out; }
        FlatRuntimeTarget* tone = nullptr;
        for (uint32_t i = 0; i < p.targetsUsed; ++i) if (p.targets[i].resource == k.srvResource[0]) tone = &p.targets[i];
        if (!tone || tone->tones != 1 || tone->tone.last != tone->writes.last) { out.reason = FlatMonoReason::NoTonePass; return out; }
        const void* const hdrInput = toneHdrInput(tone->tone.key);
        FlatContractRecord records[36]{}; uint32_t n = 0;
        // Reuse the proven completed-frame selector on this exact prefix. Only
        // HDR aggregates, supported sources and tone/copy enter the fixture.
        for (uint32_t i = 0; i < p.targetsUsed; ++i) {
            const auto& t = p.targets[i];
            if (hdrInput && t.resource == hdrInput && t.writes.key.format == 26 && t.writes.draws) {
                // A camera-only fault on a temporal-off frame goes on to the selector below, which judges the target by its first camera, as it does a clean one.
                if (t.hdrBad && flatRuntimeBadTargetRefuses(p, t)) { p.selectedConflict = t.firstBad; out.reason = FlatMonoReason::ConflictingHdr; return out; }
                records[n] = t.writes; records[n].key.camera = nullptr; records[n++].key.kind = kFlatContractScreen;
                if (t.hdrCamera) { records[n] = t.tone; records[n++].key.kind = kFlatContractScreen; }
            }
        }
        records[n++] = tone->tone;
        for (uint32_t i = 0; i < p.sourcesUsed; ++i) records[n++] = p.sources[i];
        records[n++] = current();
        // The frame contract's record fixture: the exact records the selector
        // ran on, delivered to the sink before selection.
        if (sink && sink->records && n <= sink->capacity) {
            std::memcpy(sink->records, records, n * sizeof(FlatContractRecord));
            sink->count = n;
        }
        FlatMonoFrameInput in{}; in.world = records; in.worldCount = n;
        in.output = p.output; in.outputWidth = p.width; in.outputHeight = p.height; in.outputFormat = p.format;
        in.frame = in.epoch = p.frame; in.supportedPair = [](uint64_t, uint64_t) { return true; };
        in.unsupportedFamilyDraws = p.unsupportedFamilyDraws;
        out = flatSelectMonoFrame(in);
        // The first-person cohort left out of the sources above makes the frame mixed-camera (the foreground contract's
        // trigger). Not part of the contract hash, and false for every frame whose draws carry no cohort flag.
        if (out.selected() && flatRuntimeFirstPerson(p, out.depth)) out.mixedCamera = true;
        if (out.reason == FlatMonoReason::ConflictingHdr) {
            for (uint32_t i = 0; i < p.targetsUsed; ++i) {
                const auto& t = p.targets[i];
                if (!hdrInput || t.resource != hdrInput || !t.writes.draws) continue;
                auto& w = p.selectedConflict;
                w.hdr = t.resource; w.sequence = t.hdrCamera ? t.tone.first : t.writes.first;
                w.reference = flatRuntimeWitnessDraw(t.writes);
                w.current = flatRuntimeWitnessDraw(t.hdrCamera ? t.tone : t.writes);
                w.cause = t.hdrCamera && !cameraCurrent(t.tone, p.frame)
                    ? FlatRuntimeConflict::SelectorCameraProvenance
                    : t.hdrCamera && t.writes.key.camera && !sameCamera(t.tone, t.writes)
                    ? FlatRuntimeConflict::SelectorCamera : FlatRuntimeConflict::SelectorLayout;
                break;
            }
        }
        return out;
    }
    // A late scene-depth draw can invalidate the clean HDR snapshot even when
    // it renders into a different colour target (or has no colour target).
    // The runtime supplies the effective write state after the draw is bound.
    for (uint32_t i = 0; i < p.targetsUsed; ++i) {
        auto& suffix = p.targets[i];
        if (suffix.overlayOpen && d.supported)
            bad(suffix, FlatRuntimeConflict::OverlaySuffix, suffix.tone);
        if (suffix.overlayOpen && (d.effectiveDepthWrite ||
            (d.effectiveStencilWrite && !d.overlayProtected)) && k.depth == suffix.overlayDepth)
            bad(suffix, FlatRuntimeConflict::OverlaySuffix, suffix.tone);
    }
    flatRuntimeReachedDraw(p, k);   // a draw reaches its depth and its colour, and a depth-only draw its depth
    if (d.poolFamilyVs) ++p.unsupportedFamilyDraws;
    if (!k.color) return out;
    auto* t = flatRuntimeTarget(p, k.color); if (!t) return out;
    if (t->overlayOpen && !d.overlayProtected)
        bad(*t, FlatRuntimeConflict::OverlaySuffix, t->tone);
    constexpr uint64_t kHdrImageCopyVs = 0xCFA91824129ECBBCull;
    constexpr uint64_t kHdrImageCopyPs = 0xDFCBA0EC70B03C9Bull;
    constexpr uint64_t kMenuCopyVs = 0xDEF19B035D5EDEDCull;
    constexpr uint64_t kMenuCopyPs = 0xDED8796049C7BB4Aull;
    constexpr uint64_t kDofCompositeVs = 0x20F383BBAC05C031ull;   // the shared full-screen VS (also the output copy's)
    constexpr uint64_t kDofCompositePs = 0xBF2302BCC7B434DBull;
    if (k.format == 26 && k.vs == kMenuCopyVs && k.ps == kMenuCopyPs) {
        FlatRuntimeTarget* source = nullptr;
        for (uint32_t i = 0; i < p.targetsUsed; ++i)
            if (p.targets[i].resource == k.srvResource[0]) source = &p.targets[i];
        const bool valid = d.menuHdrCopyVerified && k.srvView[0] && k.srvResource[0] &&
            k.srvResource[0] != k.color && source && source != t &&
            source->writes.draws && !source->hdrBad && !source->hdrLayoutChanged && source->hdrCamera &&
            source->writes.key.format == 26 && source->writes.key.rtv &&
            source->writes.key.width == k.width && source->writes.key.height == k.height &&
            source->writes.key.depth && source->writes.key.dsv && source->writes.key.depthFormat &&
            source->writes.key.depthWidth == k.width && source->writes.key.depthHeight == k.height &&
            source->writes.last < q && cameraCurrent(source->tone, p.frame) &&
            (!source->writes.key.camera || sameCamera(source->writes, source->tone)) &&
            !t->writes.draws && !t->hdrBad && !k.depth && !k.dsv &&
            fullViewport(k, k.width, k.height);
        if (!valid) {
            ++p.menuCopiesRefused;
            const bool firstConflict = t->firstBad.cause == FlatRuntimeConflict::None;
            bad(*t, FlatRuntimeConflict::MenuCopySource, source ? source->writes : t->writes);
            if (firstConflict && source && source->firstBad.cause != FlatRuntimeConflict::None)
                t->firstBad = source->firstBad;
            // A rejected first copy is still a real write into this HDR target;
            // retain it so the final selector reports the original conflict.
            if (!t->writes.draws) t->writes = current();
            return out;
        }
        ++p.menuCopiesAccepted;
        // The destination comes into existence at this copy. Its scene and camera
        // provenance is inherited from the verified source, never from copy b1.
        flatRuntimeInheritSceneHdr(*t, *source, current());
        t->menuInherited = true;
        return out;
    }
    // The depth-of-field composite (2026-10-22, the 3D main menu at render below output with Depth of Field 2): the game's blur chain reads
    // the scene HDR, and this full-screen pass lays the blurred image over it, writing a NEW depthless HDR image that the exposure chain and
    // the tone then read in the scene's place. Left to the ordinary HDR rules it is a writer with no depth (MissingDepth) into the tone's
    // input. Taken, like the menu copy, as the scene HDR's alias: the verified draw reads the scene HDR at t0 and the destination inherits
    // its depth and camera. Not menuInherited: the menu's static-scene policy keys on the plain menu copy, and this frame is an ordinary
    // scene frame for the resolver. A refusal keeps the source's own first conflict where it has one, so the frame names the original cause.
    if (k.format == 26 && k.vs == kDofCompositeVs && k.ps == kDofCompositePs) {
        FlatRuntimeTarget* source = nullptr;
        for (uint32_t i = 0; i < p.targetsUsed; ++i)
            if (p.targets[i].resource == k.srvResource[0]) source = &p.targets[i];
        uint32_t why = 0;
        const auto need = [&](bool ok, uint32_t bit) { if (!ok) why |= bit; };
        need(d.dofCompositeVerified, kDofNotVerified);
        need(k.srvView[0] && k.srvResource[0] && k.srvResource[0] != k.color, kDofNoInput);
        need(source && source != t, kDofNoSource);
        if (source) {
            const auto& w = source->writes;
            need(w.draws != 0, kDofSourceNoDraws);
            need(!source->hdrBad, kDofSourceBad);
            need(!source->hdrLayoutChanged, kDofSourceLayout);
            need(source->hdrCamera, kDofSourceNoCamera);
            need(w.key.format == 26 && w.key.rtv, kDofSourceFormat);
            need(w.key.width == k.width && w.key.height == k.height, kDofSourceSize);
            need(w.key.depth && w.key.dsv && w.key.depthFormat && w.key.depthWidth == k.width && w.key.depthHeight == k.height,
                 kDofSourceDepth);
            need(w.last < q, kDofSourceOrder);
            need(cameraCurrent(source->tone, p.frame), kDofSourceStale);
            need(!w.key.camera || sameCamera(w, source->tone), kDofSourceCamera);
        }
        need(!t->writes.draws && !t->hdrBad, kDofDestination);
        need(!k.depth && !k.dsv, kDofOutputDepth);
        need(fullViewport(k, k.width, k.height), kDofViewport);
        if (why) {
            ++p.dofCompositesRefused;
            p.dofRefusalMask |= why;
            if (source && source->firstBad.cause != FlatRuntimeConflict::None) p.dofSourceBad = source->firstBad.cause;
            const bool firstConflict = t->firstBad.cause == FlatRuntimeConflict::None;
            bad(*t, FlatRuntimeConflict::DofCompositeSource, source ? source->writes : t->writes);
            if (firstConflict && source && source->firstBad.cause != FlatRuntimeConflict::None) t->firstBad = source->firstBad;
            if (!t->writes.draws) t->writes = current();
            return out;
        }
        ++p.dofCompositesAccepted;
        flatRuntimeInheritSceneHdr(*t, *source, current());
        t->dofInherited = true;
        return out;
    }
    const bool imageCameraIndependent = k.format == 9 &&
        flatRuntimeCameraIndependentImageSourcePair(k.vs, k.ps) &&
        d.imageSourceCameraIndependentVerified;
    const bool imageCopy = k.format == 26 && k.vs == kHdrImageCopyVs && k.ps == kHdrImageCopyPs;
    if (imageCopy) {
        FlatRuntimeTarget* source = nullptr;
        for (uint32_t i = 0; i < p.targetsUsed; ++i)
            if (p.targets[i].resource == k.srvResource[0]) source = &p.targets[i];
        const bool valid = d.hdrCopyVerified && k.srvView[0] && k.srvResource[0] &&
            k.srvResource[0] != k.color && source && source->writes.draws &&
            !source->imageSourceBad && !source->hdrBad &&
            source->writes.key.format == 9 && source->writes.key.rtv &&
            source->writes.key.width == k.width && source->writes.key.height == k.height &&
            source->writes.key.depth && source->writes.key.dsv &&
            source->writes.key.depthWidth == k.width && source->writes.key.depthHeight == k.height &&
            source->writes.last < q &&
            (!source->imageHasCamera ||
             (cameraCurrent(source->tone, p.frame) && sameCamera(t->tone, source->tone))) &&
            t->writes.draws && !t->hdrBad && t->hdrCamera &&
            t->writes.key.format == 26 && t->writes.key.width == k.width &&
            t->writes.key.height == k.height &&
            t->writes.key.depth == source->writes.key.depth &&
            t->writes.key.dsv == source->writes.key.dsv &&
            t->writes.key.depthFormat == source->writes.key.depthFormat &&
            cameraCurrent(t->tone, p.frame) &&
            fullViewport(k, k.width, k.height) && !k.depth && !k.dsv;
        // The copy is the first write into this HDR: nothing drew into it before, so there is no scene camera for the copy to
        // continue, and nothing it could contradict. It is the glow layer of a screen with no scene (a loading screen's hologram,
        // drawn into a cleared HDR before anything else). Not recorded and not a conflict; the HDR is whatever its later writes
        // make it. Every other refused copy (a prior write that carries no camera, a bad HDR, an unverified or misshapen copy) is
        // refused exactly as before.
        if (!valid && !t->writes.draws && !t->hdrBad && d.hdrCopyVerified && k.srvView[0] && k.srvResource[0] &&
            k.srvResource[0] != k.color && fullViewport(k, k.width, k.height) && !k.depth && !k.dsv) {
            ++p.imageCopiesFirstWrite;
            t->imageCopyFirst = true;
            return out;
        }
        if (!valid) {
            ++p.imageCopiesRefused;
            const bool firstConflict = t->firstBad.cause == FlatRuntimeConflict::None;
            bad(*t, FlatRuntimeConflict::ImageCopySource, source ? source->writes : t->writes);
            if (firstConflict &&
                source && source->firstBad.cause == FlatRuntimeConflict::ImageCopySource) {
                t->firstBad.reference = source->firstBad.reference;
                t->firstBad.current = source->firstBad.current;
            }
            return out;
        }
        ++p.imageCopiesAccepted;
        ++t->writes.draws; t->writes.last = q; t->writes.lastInstances = d.instances;
        return out;
    }
    if (k.format == 26) {
        if (t->imageCopyFirst && !flatProjectionB1Unread(k.vs, k.ps))
            bad(*t, FlatRuntimeConflict::ImageCopySource, t->writes);
        if (!hdrViewport(k, k.width, k.height)) bad(*t, FlatRuntimeConflict::Viewport, t->writes);
        if (!k.depth || !k.dsv || k.depthWidth != k.width || k.depthHeight != k.height)
            bad(*t, FlatRuntimeConflict::MissingDepth, t->writes);
        if (t->writes.draws && (t->writes.key.depth != k.depth || t->writes.key.dsv != k.dsv))
            bad(*t, FlatRuntimeConflict::DepthMismatch, t->writes);
        if(flatRuntimeQualifiedForwardCamera(p,*t,d,q)) {
            const auto& donor=*flatRuntimeForwardDonor(p,k,q);
            t->tone=current();t->tone.deferredCamera=true;t->tone.forwardLightCamera=true;
            t->tone.sceneCameraB1=donor.key.b1;t->tone.sceneCameraWriteEpoch=donor.lastWriteEpoch;
            t->tone.sceneCameraWriteSeq=donor.lastWriteSeq;t->tone.key.camera=donor.key.camera;
            t->tone.key.writeEpoch=t->tone.firstWriteEpoch=t->tone.lastWriteEpoch=k.projection[1].writeEpoch;
            t->tone.key.writeSeq=t->tone.firstWriteSeq=t->tone.lastWriteSeq=k.projection[1].writeSeq;
            t->hdrCamera=true;
        }
        if(flatRuntimeQualifiedDeferredCamera(p,*t,d,q)) {
            const auto donor=flatRuntimeDeferredDonor(p,k,q,nullptr,d.camera);
            t->tone=current();t->tone.deferredCamera=true;t->tone.deferredInverseRepaired=d.deferredInverseRepaired;t->tone.sceneCameraB1=donor.b1;
            t->tone.sceneCameraWriteEpoch=donor.writeEpoch;t->tone.sceneCameraWriteSeq=donor.writeSeq;
            t->tone.key.camera=donor.camera;
            t->tone.key.writeEpoch=t->tone.firstWriteEpoch=t->tone.lastWriteEpoch=k.projection[1].writeEpoch;
            t->tone.key.writeSeq=t->tone.firstWriteSeq=t->tone.lastWriteSeq=k.projection[1].writeSeq;
            t->hdrCamera=true;
        }
        if (k.camera) {
            // Epic 20260925_122208: the first-person weapon pass (laser rifle
            // family) draws into the scene HDR/depth with its own qualified
            // second projection -- same position and orientation, narrower
            // FOV, near 0.0675 versus 0.025; its b0[4..7] transposes its own
            // b1[270..273]. A differing weapon projection neither sets nor
            // vetoes the scene camera; its pixels fail strict depth ownership
            // at resolve and fall back to current colour. This exact pair may
            // set H's scene camera only when an earlier supported pool source
            // on the same depth proves identical current camera bytes.
            const bool laserPair =
                k.vs == 0x88DCF1164C640EC3ull && k.ps == 0x494506A63091DF8Cull;
            const bool otherCamera = t->hdrCamera && !flatRuntimeSameCamera(t->tone, d);
            // The weapon's glow pass where the copy route judges it (FlatRuntimeDraw::alternateHdr): the laser pair's exemption by flag,
            // for a second camera and only for a draw that writes no depth (a flag on a draw that writes depth is no flag: it conflicts
            // as any second camera does). Nothing else needs saying here, because the checks around this one already judge the rest and
            // the flag changes none of them: a draw into another depth is a DepthMismatch above, an H a plain second camera already made
            // bad stays bad with its first witness, and a draw planned as a protected overlay is the overlay's own case just below.
            const bool alternate = d.alternateHdr && otherCamera && !d.effectiveDepthWrite;
            const bool secondCamera = laserPair || alternate;
            const bool sceneLaserCamera = laserPair && !t->hdrCamera &&
                flatRuntimeQualifiedLaserSceneCamera(p, *t, d, q);
            const bool overlay = d.overlayProtected && otherCamera && !t->hdrBad &&
                t->writes.draws && t->writes.key.format == 26 &&
                k.depth == t->writes.key.depth && k.dsv == t->writes.key.dsv;
            if (d.overlayProtected && !overlay)
                bad(*t, FlatRuntimeConflict::OverlaySuffix, t->tone);
            if (otherCamera && !secondCamera && !overlay)
                bad(*t, FlatRuntimeConflict::CameraChange, t->tone);
            if (!flatRuntimeCameraCurrent(d, q, p.frame))
                bad(*t, FlatRuntimeConflict::CameraProvenance, t->tone);
            // Keep the first known camera draw separate from earlier HDR writes
            // without b1. Assigning its later write to those draws invents provenance.
            if (!t->hdrCamera && (!secondCamera || sceneLaserCamera)) { t->tone = current(); t->hdrCamera = true; }
            if (overlay) { t->overlayOpen = true; t->overlayDepth = k.depth; }
        }
    }
    if (t->writes.draws && t->writes.key.format == 26 &&
        (k.format != 26 || k.width != t->writes.key.width ||
         k.height != t->writes.key.height ||
         k.depth != t->writes.key.depth || k.dsv != t->writes.key.dsv ||
         k.depthWidth != t->writes.key.depthWidth ||
         k.depthHeight != t->writes.key.depthHeight ||
         k.depthFormat != t->writes.key.depthFormat))
        t->hdrLayoutChanged = true;
    if (t->writes.draws && t->writes.key.format == 9 && k.format != 9) {
        t->imageSourceBad = true;
        bad(*t, FlatRuntimeConflict::ImageCopySource, t->writes);
    }
    if (k.format == 9) {
        const auto& first = t->writes;
        if (!k.depth || !k.dsv || !k.depthFormat ||
            k.depthWidth != k.width || k.depthHeight != k.height ||
            !fullViewport(k, k.width, k.height) ||
            (!imageCameraIndependent && !flatRuntimeCameraCurrent(d, q, p.frame)) ||
            (first.draws && (first.key.format != k.format || first.key.width != k.width ||
                             first.key.height != k.height || first.key.depth != k.depth ||
                             first.key.dsv != k.dsv || first.key.depthFormat != k.depthFormat ||
                             first.key.depthWidth != k.depthWidth ||
                             first.key.depthHeight != k.depthHeight ||
                             (!imageCameraIndependent && t->imageHasCamera &&
                              !flatRuntimeSameCamera(t->tone, d))))) {
            t->imageSourceBad = true;
            bad(*t, FlatRuntimeConflict::ImageCopySource, t->imageHasCamera ? t->tone : first);
        }
        if (!imageCameraIndependent && k.camera && !t->imageHasCamera) {
            t->tone = current(); t->imageHasCamera = true;
        }
    }
    if (!t->writes.draws) t->writes = current();
    else { ++t->writes.draws; t->writes.last = q; t->writes.lastInstances = d.instances;
        if (k.camera) { t->writes.lastWriteEpoch = k.writeEpoch; t->writes.lastWriteSeq = k.writeSeq; } }
    if (k.format != 9 && toneHdrSlot(k.vs, k.ps) != ~0u) { ++t->tones; t->tone = current(); }
    if (d.firstPersonCohort && d.supported && k.depth && k.kind == kFlatContractPool) {
        // Not a world source (FlatRuntimeDraw::firstPersonCohort): counted, never a source record. A cohort in two depths is a frame
        // this cannot name the scene of, and refuses as every uncertain frame does.
        if (!p.firstPersonDraws) p.firstPersonDepth = k.depth;
        else if (p.firstPersonDepth != k.depth)
            flatRuntimeMarkUncertain(p, FlatRuntimeUncertainCause::FirstPersonDepth,k.depth,p.firstPersonDepth,k.vs,k.ps);
        ++p.firstPersonDraws;
    } else if (d.supported && k.depth && k.kind == kFlatContractPool) {
        FlatContractRecord* source = nullptr;
        for (uint32_t i = 0; i < p.sourcesUsed; ++i) if (p.sources[i].key.depth == k.depth && flatRuntimeSameCamera(p.sources[i], d)) { source = &p.sources[i]; break; }
        if (!source) {
            if (p.sourcesUsed == 32)
                flatRuntimeMarkUncertain(p, FlatRuntimeUncertainCause::SourceCapacity,k.depth,k.color,k.vs,k.ps);
            else p.sources[p.sourcesUsed++] = current();
        } else {
            if (!fullViewport(k, k.width, k.height) || source->key.dsv != k.dsv ||
                source->key.width != k.width || source->key.height != k.height ||
                source->key.depthWidth != k.depthWidth || source->key.depthHeight != k.depthHeight)
                source->key.camera = nullptr;
            ++source->draws; source->last = q; source->lastWriteEpoch = k.writeEpoch; source->lastWriteSeq = k.writeSeq; source->lastInstances = d.instances;
        }
    } else if (!d.supported && k.kind == kFlatContractScreen && k.depth && k.dsv && k.rtv) {
        // A Screen-kind writer no source record tracks: the witness of a scene with no motion source on this depth.
        flatRuntimeNoteWitness(p, d, q);
    }
    return out;
}
// The two-argument form keeps every existing caller; the sink form is the
// frame-contract producer (flat_frame_contract.h).
inline FlatMonoFrame flatRuntimeObserve(FlatRuntimePrefix& p, const FlatRuntimeDraw& d) {
    return flatRuntimeObserve(p, d, nullptr);
}
} // namespace edvr

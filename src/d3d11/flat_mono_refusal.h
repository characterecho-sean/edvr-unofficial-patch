#pragma once
// The resolver prep kernel's pixel classes and the refusal census (design doc section 82, stage 2 experiment build). Pure: no D3D, no
// I/O, so the route's rig and the resolver's rig read the same numbers the shader writes.
//
// WHAT THE CLASS IS. Every render pixel the prep treats gets one byte, written to a private texture only on a frame that needs
// it (a census sample, or the route's refusal view): the low four bits say what the pixel IS (0..15), bits 4-6 say WHY a refused
// first-person pixel has no history (0..7, set only with kFlatMonoClassWeaponRefused), bit 7 says the prep REFUSED its
// history (rejection 1: the finish shows the raw input, which under a jittered world is a different sample every frame, so a
// fine pattern on a refused pixel shimmers). Every reader of the byte masks what it reads: the class with kFlatMonoClassMask, the
// reason with kFlatMonoClassReasonMask. Classes 1..6 are the eye path's source kinds (screen_motion's sourceEngine and the
// temporal pass's motion_source view), with the same numbers and the same colours, so the two views read alike; 7 and up are what
// only the resolver's prep can say. The HLSL repeats the numbers as `static const uint kClass...` (flat_mono_shader_source.h) and
// tools\flat_mono_resolve_test holds the two to each other.
#include <cstdint>

namespace edvr {

constexpr uint32_t kFlatMonoClassNone = 0;            // no engine slot: not a keyed pool surface, the camera term
constexpr uint32_t kFlatMonoClassJoined = 1;          // a rig record Elite's records followed from last frame: its exact motion
constexpr uint32_t kFlatMonoClassMasked = 2;          // a rig record with no usable history this frame (first seen, after a gap): REFUSED
constexpr uint32_t kFlatMonoClassNotRig = 3;          // a pool surface that is not a rig record: the camera term
constexpr uint32_t kFlatMonoClassStale = 4;           // the slot's depth is not the pixel's, a later draw changed it: REFUSED; with the steady detail on (both routes always ask for it), the camera term where last frame's depth confirms it, REFUSED where it does not
constexpr uint32_t kFlatMonoClassCorrupt = 5;         // a slot code or a record number that did not survive intact: REFUSED
constexpr uint32_t kFlatMonoClassStaleStamp = 6;      // a joined marker from an older frame: the camera term
constexpr uint32_t kFlatMonoClassSentinel = 7;        // a slot with no depth under it (sky), or the out-of-range marker: REFUSED
constexpr uint32_t kFlatMonoClassUnreprojectable = 8; // a moved record whose reprojection failed: REFUSED
constexpr uint32_t kFlatMonoClassCamera = 9;          // the camera term could not be formed (singular, behind the camera): REFUSED
constexpr uint32_t kFlatMonoClassRange = 10;          // the reprojection leaves the screen or is not finite: REFUSED
constexpr uint32_t kFlatMonoClassDepth = 11;          // the pixel's own depth is not finite or outside [0,1]: REFUSED
constexpr uint32_t kFlatMonoClassWeapon = 12;         // an attached first-person pixel with a valid map: the map's motion
constexpr uint32_t kFlatMonoClassWeaponRefused = 13;  // an attached first-person pixel the map cannot place: REFUSED
constexpr uint32_t kFlatMonoClassReset = 14;          // a reset frame, every pixel refused: never sampled, never painted
constexpr uint32_t kFlatMonoClassSkinned = 15;        // F2 on foot (VR world route only): a skinned character's pixel whose target-7 texel is valid: its exact motion. ACCEPTED, so it
                                                       // is counted in its own census slot (kFlatMonoRefusalSkinned), never among the refused; a skinned pixel with no valid texel is kFlatMonoClassMasked
constexpr uint32_t kFlatMonoClassRefusedBit = 0x80u;  // the prep refused this pixel's history
constexpr uint32_t kFlatMonoClassMask = 0x0Fu;        // the class, bits 0-3: the highest is 15, so the next bits are free
constexpr uint32_t kFlatMonoClassReasonShift = 4;
constexpr uint32_t kFlatMonoClassReasonMask = 0x70u;  // bits 4-6: kFlatMonoWeaponReason*, only ever with kFlatMonoClassWeaponRefused

// WHY A REFUSED FIRST-PERSON PIXEL HAS NO HISTORY (the SDK foreground map's samples, flat_foreground_motion_shader.h, which says what each
// number means). The map's vertex shader names the cause of a rejected draw in the x channel of its class-2 samples; the prep keeps it in
// bits 4-6 of the class byte of a refused first-person pixel; the census counts it. The HLSL repeats the numbers as kReason*, and
// tools\flat_mono_resolve_test holds the two to each other. 0 is no reason given: no sample at the pixel, or a valid sample the prep refused
// (a reset frame, a previous position off the raster).
constexpr uint32_t kFlatMonoWeaponReasonNone = 0;
constexpr uint32_t kFlatMonoWeaponReasonInvalidCurrent = 1;       // the current draw is not valid, or its slot is out of range
constexpr uint32_t kFlatMonoWeaponReasonNotAuthentic = 2;         // the instance index has flag bits, or the pool has no row for it
constexpr uint32_t kFlatMonoWeaponReasonNoPrior = 3;              // the previous frame has no draw of this geometry under this pool and near
constexpr uint32_t kFlatMonoWeaponReasonPriorPositions = 4;       // the matching prior's vertices are not finite or are behind the camera
constexpr uint32_t kFlatMonoWeaponReasonIdentityDiffers = 5;      // priors were read and none has this identity
constexpr uint32_t kFlatMonoWeaponReasonPriorIdentityInvalid = 6; // priors were supplied and none could be read
constexpr uint32_t kFlatMonoWeaponReasonAmbiguous = 7;            // several priors matched and their previous positions or phases differ
constexpr uint32_t kFlatMonoWeaponReasons = 8;

// The census counts REFUSED pixels by class (slot = class, 0..14) and, in slot 15, the stale pixels that were not refused because
// the steady-detail rule kept them ("stale-kept": the camera term, confirmed by last frame's depth, or the menu's blanket policy).
// The stale pixels that are refused stay in the stale slot, which with the rule on is "stale-refused": the depth check turned them
// away. Accepted pixels of any other class are not counted (the contended counters they would need cost more than the answer is
// worth); the total examined is the sampled frames' render size, known on the host.
constexpr uint32_t kFlatMonoRefusalSlots = 16;
constexpr uint32_t kFlatMonoRefusalStaleKept = 15;
// One sample every this many resolves that ask for the census; the 5 s line names it.
constexpr uint32_t kFlatMonoRefusalEvery = 4;
constexpr uint32_t kFlatMonoRefusalStripes = 16;     // the counter buffer's stripes (spreads the atomics); the host sums them
// Each stripe holds kFlatMonoRefusalSlots class counters, then kFlatMonoWeaponReasons counters of the refused first-person pixels by reason.
// After them one more, the only ACCEPTED pixels the census counts: the skinned pixels that took their exact motion from target 7 (class kFlatMonoClassSkinned),
// which is how a log shows the world route read E. Always 0 in the flat profile (it has no target 7).
constexpr uint32_t kFlatMonoRefusalSkinned = kFlatMonoRefusalSlots + kFlatMonoWeaponReasons;
constexpr uint32_t kFlatMonoRefusalCounters = kFlatMonoRefusalSlots + kFlatMonoWeaponReasons + 1;

// The steady-detail depth check's tolerance (flat_mono_shader_source.h, kStaleDepthRel and kStaleDepthFloor, which the resolver's rig
// holds to these): a stale pixel keeps its camera-term history only where last frame's depth, in the best of the four texels around the
// position the camera term sends the pixel to, is within max(kFlatMonoStaleDepthFloor, expected * kFlatMonoStaleDepthRelative) of
// the depth the camera term expects there. Reversed-Z float32 depth (d = near / z): a relative error in d is the same relative
// error in z at any range. 1% and 1e-6 are the numbers the resolver's own TAA applies in taa() before it trusts history.
constexpr double kFlatMonoStaleDepthRelative = 0.01;
constexpr double kFlatMonoStaleDepthFloor = 1e-6;

inline const char* flatMonoClassName(uint32_t cls) {
    switch (cls) {
        case kFlatMonoClassNone: return "none";
        case kFlatMonoClassJoined: return "joined";
        case kFlatMonoClassMasked: return "masked";
        case kFlatMonoClassNotRig: return "not-rig";
        case kFlatMonoClassStale: return "stale";
        case kFlatMonoClassCorrupt: return "corrupt";
        case kFlatMonoClassStaleStamp: return "stale-stamp";
        case kFlatMonoClassSentinel: return "sentinel";
        case kFlatMonoClassUnreprojectable: return "unreprojectable";
        case kFlatMonoClassCamera: return "camera";
        case kFlatMonoClassRange: return "range";
        case kFlatMonoClassDepth: return "depth";
        case kFlatMonoClassWeapon: return "weapon";
        case kFlatMonoClassWeaponRefused: return "weapon-refused";
        case kFlatMonoClassReset: return "reset";
        case kFlatMonoClassSkinned: return "skinned";
    }
    return "?";
}

inline const char* flatMonoWeaponReasonName(uint32_t reason) {
    switch (reason) {
        case kFlatMonoWeaponReasonNone: return "unspecified";
        case kFlatMonoWeaponReasonInvalidCurrent: return "invalid-current";
        case kFlatMonoWeaponReasonNotAuthentic: return "not-authentic";
        case kFlatMonoWeaponReasonNoPrior: return "no-prior";
        case kFlatMonoWeaponReasonPriorPositions: return "prior-positions";
        case kFlatMonoWeaponReasonIdentityDiffers: return "identity-differs";
        case kFlatMonoWeaponReasonPriorIdentityInvalid: return "prior-identity-invalid";
        case kFlatMonoWeaponReasonAmbiguous: return "ambiguous";
    }
    return "?";
}

// What flatMonoResolveTakeRefusalCensus() hands back: the samples read back since the last take.
struct FlatMonoRefusalCensus {
    uint64_t asked = 0;        // resolves that asked for the census (the key on, not a reset frame)
    uint64_t sampled = 0;      // samples dispatched
    uint64_t dropped = 0;      // samples skipped because every readback slot was still pending
    uint64_t frames = 0;       // samples whose counts were read back
    uint64_t pixels = 0;       // pixels those samples examined (render width x height each)
    uint64_t counts[kFlatMonoRefusalSlots] = {};   // refused pixels by class; [kFlatMonoRefusalStaleKept] stale pixels the steady-detail rule kept
    uint64_t weaponReasons[kFlatMonoWeaponReasons] = {};   // the refused first-person pixels (counts[kFlatMonoClassWeaponRefused]) by reason
    uint64_t skinned = 0;      // ACCEPTED skinned pixels that took their exact motion from target 7 (F2 on foot); not part of refused()
    // The steady-detail rule's own frames since the last take, whatever the census asked: resolves with the key on whose prep ran the
    // depth check (last frame's depth was there to check against) and resolves with the key on that could not (a reset frame is neither:
    // it refuses every pixel anyway). "Key on and checked=0" is the check never having run.
    uint64_t checked = 0, skipped = 0;
    uint32_t width = 0, height = 0;                // the render size of the last sample read back
    uint32_t every = kFlatMonoRefusalEvery;
    uint64_t refused() const {
        uint64_t n = 0;
        for (uint32_t i = 0; i < kFlatMonoRefusalStaleKept; ++i) n += counts[i];
        return n;
    }
};

// The refusal view's palette (the eye path's, temporal_shader_source.h motion_source, painted by the HDR finish before the game's
// tone pass: it scales each colour by the pixel's own level, so the hue survives the tone but the absolute colour does not).
//   green  1 joined      red  2 masked      blue 3 not a rig record      yellow 4 stale slot kept   pink 4 stale slot refused
//   magenta 5 corrupt    orange 6 stale stamp   cyan 12 weapon           white  any other refusal (7..11, 13)
//   dimmed to a quarter  no engine slot (0)
// The names below are for the log line.
inline const char* flatMonoViewLegend() {
    return "green exact record, red masked record, blue pool surface (camera term), yellow stale slot (kept), pink stale slot refused "
           "(shown raw), magenta corrupt slot, orange stale stamp, cyan first-person, white any other refusal, dimmed no engine slot";
}

}  // namespace edvr
